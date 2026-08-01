// SPDX-License-Identifier: MIT

#include "Core.h"
#include "MemAccess.h"
#include "PluginManager.h"
#include "VTableInterpose.h"

#include "modules/DFSDL.h"

#include "df/enabler.h"
#include "df/graphic.h"
#include "df/graphic_viewportst.h"
#include "df/renderer_2d_base.h"
#include "df/texture_fullid.h"

#include "visual_animation.h"

#include <SDL_render.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <set>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace DFHack;

DFHACK_PLUGIN("smooth-movement");
DFHACK_PLUGIN_IS_ENABLED(is_enabled);

REQUIRE_GLOBAL(enabler);
REQUIRE_GLOBAL(gps);
REQUIRE_GLOBAL(window_x);
REQUIRE_GLOBAL(window_y);
REQUIRE_GLOBAL(window_z);

namespace {

constexpr const char *plugin_version="0.3.0";
#ifdef WIN32
constexpr const char *sdl_library="SDL2.dll";
#else
constexpr const char *sdl_library="libSDL2-2.0.so.0";
#endif

// Runtime harness for the engine-owned visual state; gameplay data is never read.
DFLibrary *sdl_handle=nullptr;
decltype(&SDL_RenderCopyF) render_copy_f=nullptr;
decltype(&SDL_RenderFillRect) render_fill_rect=nullptr;
decltype(&SDL_RenderSetClipRect) render_set_clip_rect=nullptr;
decltype(&SDL_GetRenderDrawColor) get_render_draw_color=nullptr;
decltype(&SDL_SetRenderDrawColor) set_render_draw_color=nullptr;

visual_animation_managerst animation_manager;
std::set<std::pair<int32_t,int32_t>> previous_coverage;
uint64_t visual_context_revision=0;
const void *previous_viewport=nullptr;
std::array<int32_t,12> previous_view_signature{};
bool has_view_signature=false;
// Map scroll (window_x/window_y) is tracked separately from the reset signature: a pure pan is
// followed (movements are translated) instead of triggering a full reset, so it must NOT bump the
// context revision. It only invalidates the viewport-space blackout coverage from the prior frame.
int32_t previous_pan_x=0;
int32_t previous_pan_y=0;
bool has_pan_context=false;

// --- free camera -------------------------------------------------------------------------------
// The camera is visually unbound from the tile grid. Two layered offsets:
//   rest      -- a PERSISTENT sub-tile offset in tiles: the free camera. Set by pixel-perfect
//                middle-mouse drag panning (the view rests wherever released, mid-tile or not)
//                and by the `smooth-movement camera <fx> <fy>` console command. Survives zoom
//                and z-level changes. Kept in [-0.5,0.5] by normalization: whole-tile parts are
//                folded into window_x/window_y (a plain UI scroll write -- NEVER the viewport
//                dims, which crash DF; the sub-tile strip this leaves at one screen edge has no
//                buffer data and stays black).
//   transient -- the decaying scroll glide from before, in pixels, layered on top.
// Render offset = transient + rest*tile. window_x/window_y remain the game's own tile camera.
bool camera_enabled=false;                    // OFF by default: plain `enable smooth-movement`
                                              // keeps upstream behavior (creature interpolation
                                              // only); `smooth-movement camera on` opts in.
constexpr int32_t camera_max_glide_tiles=3;   // per-jump: farther than this snaps instantly
constexpr double camera_tau_ms=35.0;          // transient catch-up (~95% done after 100ms)
double transient_x=0.0;                       // decaying glide offset, pixels
double transient_y=0.0;
double rest_x=0.0;                            // persistent free-camera offset, tiles
double rest_y=0.0;                            // (positive = view sits WEST/NORTH of window)
int32_t camera_pending_dx=0;                  // scroll delta announced but not yet in the buffers
int32_t camera_pending_dy=0;
int32_t camera_pending_frames=0;
int32_t self_scroll_x=0;                      // window deltas WE wrote: visual no-ops when landing
int32_t self_scroll_y=0;
bool drag_active=false;
double drag_anchor_vx=0.0;                    // visual camera at drag start, tiles
double drag_anchor_vy=0.0;
int32_t drag_anchor_mx=0;                     // precise mouse at drag start, pixels
int32_t drag_anchor_my=0;
bool camera_was_offset=false;                 // edge-detects offset->0 for one cleanup redraw
int32_t camera_prev_wx=0;                     // window-scroll observation baseline
int32_t camera_prev_wy=0;
bool camera_has_prev=false;

double tile_px(const df::renderer_2d_base *renderer)
{
	const int32_t zoom=renderer->viewport_zoom_factor;
	return double(zoom==128?32:std::max(1,zoom*32/128));
}

// Match ratio of "buffers shifted by (dwx,dwy)" on the background layer: 0..1, or -1 when there
// is nothing to compare (empty background).
double background_match_ratio(const df::graphic_viewportst *vp,int32_t dwx,int32_t dwy)
{
	int32_t considered=0;
	int32_t matches=0;
	for(int32_t x=0;x<vp->dim_x;++x)
		{
		const int32_t sx=x+dwx;
		if(sx<0||sx>=vp->dim_x)continue;
		for(int32_t y=0;y<vp->dim_y;++y)
			{
			const int32_t sy=y+dwy;
			if(sy<0||sy>=vp->dim_y)continue;
			const int32_t cur=vp->screentexpos_background[x*vp->dim_y+y];
			if(cur==0)continue;
			++considered;
			if(vp->screentexpos_background_old[sx*vp->dim_y+sy]==cur)++matches;
			}
		}
	if(considered==0)return -1.0;
	return double(matches)/double(considered);
}

// Cancel everything except the persistent rest offset (the camera keeps its sub-tile position
// across zoom/z/resize; only the in-flight animation state is unfollowable).
void clear_camera_pending()
{
	camera_pending_dx=0;
	camera_pending_dy=0;
	camera_pending_frames=0;
	self_scroll_x=0;
	self_scroll_y=0;
}

void cancel_camera_transients()
{
	transient_x=0.0;
	transient_y=0.0;
	clear_camera_pending();
	drag_active=false;
}

void set_camera_enabled(bool enable)
{
	if(camera_enabled==enable)return;
	camera_enabled=enable;
	cancel_camera_transients();
	rest_x=0.0;
	rest_y=0.0;
	camera_has_prev=false;   // fresh observation baseline; no phantom scroll on re-enable
	// camera_was_offset stays: the render path issues one cleanup redraw if we were mid-offset.
}

// Fold whole tiles of rest into window_x/window_y so |rest| <= 0.5 (minimal edge strip). The
// visual position is unchanged: the window write is attributed via self_scroll when it lands.
void normalize_rest()
{
	const int32_t kx=int32_t(-std::llround(rest_x));
	const int32_t ky=int32_t(-std::llround(rest_y));
	if(kx!=0&&window_x!=nullptr&&*window_x+kx>=0)
		{
		*window_x+=kx;
		self_scroll_x+=kx;
		}
	if(ky!=0&&window_y!=nullptr&&*window_y+ky>=0)
		{
		*window_y+=ky;
		self_scroll_y+=ky;
		}
}

// A scroll of (ax,ay) tiles has landed in the buffers: our own normalization writes are visual
// no-ops (they move into rest); the remainder is a real scroll and glides -- unless a drag is
// driving the position directly, in which case it folds into rest wholesale.
void attribute_landed(int32_t ax,int32_t ay,double tile)
{
	int32_t sx=0;
	if(self_scroll_x!=0&&(self_scroll_x>0)==(ax>0)&&ax!=0)
		sx=(std::abs(self_scroll_x)<=std::abs(ax))?self_scroll_x:ax;
	int32_t sy=0;
	if(self_scroll_y!=0&&(self_scroll_y>0)==(ay>0)&&ay!=0)
		sy=(std::abs(self_scroll_y)<=std::abs(ay))?self_scroll_y:ay;
	self_scroll_x-=sx;
	self_scroll_y-=sy;
	rest_x+=sx;
	rest_y+=sy;
	const int32_t gx=ax-sx;
	const int32_t gy=ay-sy;
	if(drag_active)
		{
		rest_x+=gx;
		rest_y+=gy;
		}
	else
		{
		transient_x+=gx*tile;
		transient_y+=gy*tile;
		const double cap=tile*(camera_max_glide_tiles+0.5);
		transient_x=std::clamp(transient_x,-cap,cap);
		transient_y=std::clamp(transient_y,-cap,cap);
		}
}

// Per-frame camera bookkeeping: observe window scrolls, attribute them when the buffers apply
// them (glide vs our own normalization writes), drive the drag, decay the transient.
void update_camera(
	df::renderer_2d_base *renderer,
	const df::graphic_viewportst *vp,
	uint32_t delta_ms)
{
	if(!camera_enabled)return;
	const double tile=tile_px(renderer);
	const int32_t wx=window_x?*window_x:0;
	const int32_t wy=window_y?*window_y:0;
	if(camera_has_prev&&(wx!=camera_prev_wx||wy!=camera_prev_wy))
		{
		const int32_t dx=wx-camera_prev_wx;
		const int32_t dy=wy-camera_prev_wy;
		if((std::abs(dx)>camera_max_glide_tiles||std::abs(dy)>camera_max_glide_tiles)&&
			!drag_active)
			cancel_camera_transients();   // teleport-like jump (recenter/minimap): snap
		else
			{
			camera_pending_dx+=dx;
			camera_pending_dy+=dy;
			camera_pending_frames=0;
			}
		}
	camera_prev_wx=wx;
	camera_prev_wy=wy;
	camera_has_prev=true;

	if(camera_pending_dx!=0||camera_pending_dy!=0)
		{
		if(std::abs(camera_pending_dx)>6||std::abs(camera_pending_dy)>6)
			{
			// Scrolling far outran detection: snap (keep rest, drop the animation debt).
			transient_x=0.0;
			transient_y=0.0;
			clear_camera_pending();
			}
		else
			{
			// Fast scrolling applies the pending delta PIECEMEAL: the buffers may hold +1 of a
			// pending +3 this frame. Testing only the total made landings miss, time out, and
			// snap -- the fast-scroll jitter. Instead, find the LARGEST applied prefix of the
			// pending scroll and attribute just that; the rest keeps pending. Ties between
			// qualifying shifts only happen on uniform terrain, where mistiming is invisible.
			const int32_t stepx=(camera_pending_dx>0)-(camera_pending_dx<0);
			const int32_t stepy=(camera_pending_dy>0)-(camera_pending_dy<0);
			int32_t best_ax=0,best_ay=0,best_mag=-1;
			double best_score=-1.0;
			bool no_data=false;
			for(int32_t ix=0;ix<=std::abs(camera_pending_dx);++ix)
				{
				for(int32_t iy=0;iy<=std::abs(camera_pending_dy);++iy)
					{
					const double score=background_match_ratio(vp,ix*stepx,iy*stepy);
					if(score<0.0){no_data=true;break;}
					const int32_t mag=ix+iy;
					if(score>=0.6&&(mag>best_mag||(mag==best_mag&&score>best_score)))
						{
						best_mag=mag;
						best_score=score;
						best_ax=ix*stepx;
						best_ay=iy*stepy;
						}
					}
				if(no_data)break;
				}
			if(no_data)
				{
				// Nothing to compare against (empty background): give up on attribution.
				clear_camera_pending();
				}
			else if(best_mag>0)
				{
				attribute_landed(best_ax,best_ay,tile);
				camera_pending_dx-=best_ax;
				camera_pending_dy-=best_ay;
				camera_pending_frames=0;
				}
			else if(best_mag==0)
				{
				// Content demonstrably hasn't moved yet: keep waiting, no timeout pressure.
				camera_pending_frames=0;
				}
			else if(++camera_pending_frames>4)
				{
				// Neither static nor any prefix recognizable (heavy simultaneous change):
				// drop the debt without touching the in-flight glide.
				clear_camera_pending();
				}
			}
		}

	// --- pixel-perfect middle-mouse drag: the view follows the mouse 1:1 and rests where
	// released. DF's own drag still moves window in tile steps; rest carries the remainder.
	// Positions are tracked against the CONTENT window (window minus unlanded jumps) so the
	// buffer lag never causes a visible stutter.
	const bool mbut=enabler!=nullptr&&enabler->mouse_mbut;
	const double content_wx=double(wx-camera_pending_dx);
	const double content_wy=double(wy-camera_pending_dy);
	if(mbut&&!drag_active&&gps!=nullptr)
		{
		drag_active=true;
		drag_anchor_vx=content_wx-rest_x-transient_x/tile;
		drag_anchor_vy=content_wy-rest_y-transient_y/tile;
		drag_anchor_mx=gps->precise_mouse_x;
		drag_anchor_my=gps->precise_mouse_y;
		transient_x=0.0;
		transient_y=0.0;
		}
	if(drag_active)
		{
		if(!mbut)
			{
			drag_active=false;
			normalize_rest();
			}
		else if(gps!=nullptr)
			{
			double vx=drag_anchor_vx-double(gps->precise_mouse_x-drag_anchor_mx)/tile;
			double vy=drag_anchor_vy-double(gps->precise_mouse_y-drag_anchor_my)/tile;
			rest_x=content_wx-vx;
			rest_y=content_wy-vy;
			// If DF's own drag disagrees by more than a tile and a half, rebase on its view.
			const double lim=1.5;
			if(rest_x<-lim||rest_x>lim||rest_y<-lim||rest_y>lim)
				{
				rest_x=std::clamp(rest_x,-lim,lim);
				rest_y=std::clamp(rest_y,-lim,lim);
				drag_anchor_vx=content_wx-rest_x+
					double(gps->precise_mouse_x-drag_anchor_mx)/tile;
				drag_anchor_vy=content_wy-rest_y+
					double(gps->precise_mouse_y-drag_anchor_my)/tile;
				}
			}
		}

	if(transient_x!=0.0||transient_y!=0.0)
		{
		const double k=std::exp(-double(delta_ms)/camera_tau_ms);
		transient_x*=k;
		transient_y*=k;
		if(std::abs(transient_x)<0.5&&std::abs(transient_y)<0.5)
			{
			transient_x=0.0;
			transient_y=0.0;
			}
		}
}

constexpr uint32_t fire_bits=0x70000000U;

void update_visual_context(
	const df::renderer_2d_base *renderer,
	const df::graphic_viewportst *vp)
{
	// window_x/window_y are deliberately excluded: a horizontal/vertical scroll is followed, not
	// reset. window_z (z-level) stays, since a z change is not followable.
	const std::array<int32_t,12> signature=
		{
		window_z?*window_z:0,
		vp->dim_x,
		vp->dim_y,
		vp->clipx[0],
		vp->clipx[1],
		vp->clipy[0],
		vp->clipy[1],
		renderer->viewport_zoom_factor,
		renderer->origin_x,
		renderer->origin_y,
		gps->dimx,
		gps->dimy
		};
	const bool changed=!has_view_signature||previous_viewport!=vp||
		previous_view_signature!=signature;
	if(changed)
		{
		++visual_context_revision;
		previous_coverage.clear();
		cancel_camera_transients();
		}
	previous_viewport=vp;
	previous_view_signature=signature;
	has_view_signature=true;

	// On a pure pan the reset signature is unchanged, but last frame's blackout coverage is in the
	// old viewport frame, so discard it (the engine repaints the whole scrolled viewport anyway).
	const int32_t pan_x=window_x?*window_x:0;
	const int32_t pan_y=window_y?*window_y:0;
	if(!has_pan_context||previous_pan_x!=pan_x||previous_pan_y!=pan_y)
		previous_coverage.clear();
	previous_pan_x=pan_x;
	previous_pan_y=pan_y;
	has_pan_context=true;
}

using viewport_layer_memberst=int32_t *df::graphic_viewportst::*;

struct visual_layer_bufferst
{
	viewport_layer_memberst current;
	viewport_layer_memberst previous;
};

constexpr size_t visual_layer_count=static_cast<size_t>(viewport_visual_layer::count);
constexpr std::array visual_layer_buffers=
	{
	visual_layer_bufferst{&df::graphic_viewportst::screentexpos_right_creature,
		&df::graphic_viewportst::screentexpos_right_creature_old},
	visual_layer_bufferst{&df::graphic_viewportst::screentexpos,
		&df::graphic_viewportst::screentexpos_old},
	visual_layer_bufferst{&df::graphic_viewportst::screentexpos_left_creature,
		&df::graphic_viewportst::screentexpos_left_creature_old},
	visual_layer_bufferst{&df::graphic_viewportst::screentexpos_upright_creature,
		&df::graphic_viewportst::screentexpos_upright_creature_old},
	visual_layer_bufferst{&df::graphic_viewportst::screentexpos_up_creature,
		&df::graphic_viewportst::screentexpos_up_creature_old},
	visual_layer_bufferst{&df::graphic_viewportst::screentexpos_upleft_creature,
		&df::graphic_viewportst::screentexpos_upleft_creature_old},
	visual_layer_bufferst{&df::graphic_viewportst::screentexpos_vehicle,
		&df::graphic_viewportst::screentexpos_vehicle_old},
	visual_layer_bufferst{&df::graphic_viewportst::screentexpos_item,
		&df::graphic_viewportst::screentexpos_item_old},
	visual_layer_bufferst{&df::graphic_viewportst::screentexpos_designation,
		&df::graphic_viewportst::screentexpos_designation_old}
	};
static_assert(visual_layer_buffers.size()==visual_layer_count);

template<typename Viewport>
auto visual_layers(Viewport *vp,bool previous=false)
{
	using layer_pointer=std::conditional_t<
		std::is_const_v<Viewport>,const int32_t *,int32_t *>;
	std::array<layer_pointer,visual_layer_count> layers{};
	for(size_t i=0;i<layers.size();++i)
		layers[i]=vp->*(previous?
			visual_layer_buffers[i].previous:
			visual_layer_buffers[i].current);
	return layers;
}

viewport_visual_animation_inputst animation_input(df::graphic_viewportst *vp)
{
	const df::graphic_viewportst *const_viewport=vp;
	return {
		vp,
		vp->dim_x,
		vp->dim_y,
		visual_context_revision,
		visual_layers(const_viewport),
		visual_layers(const_viewport,true),
		window_x?*window_x:0,
		window_y?*window_y:0
		};
}

int32_t tile_pixel(int32_t tile,int32_t origin,int32_t zoom)
{
	return zoom==128?32*tile+origin:(zoom*32*tile)/128+origin;
}

bool inside_clip(const df::graphic_viewportst *vp,int32_t x,int32_t y)
{
	return x>=vp->clipx[0]&&x<=vp->clipx[1]&&
		y>=vp->clipy[0]&&y<=vp->clipy[1];
}

bool has_fire(const df::graphic_viewportst *vp,int32_t x,int32_t y)
{
	return vp->screentexpos_spatter_flag[x*vp->dim_y+y]&fire_bits;
}

enum class visual_render_groupst
{
	item,
	vehicle,
	main,
	upper,
	designation
};

constexpr visual_render_groupst visual_render_group(viewport_visual_layer layer)
{
	if(layer==viewport_visual_layer::item)return visual_render_groupst::item;
	if(layer==viewport_visual_layer::vehicle)return visual_render_groupst::vehicle;
	if(layer==viewport_visual_layer::designation)
		return visual_render_groupst::designation;
	if(layer==viewport_visual_layer::right||
		layer==viewport_visual_layer::center||
		layer==viewport_visual_layer::left)return visual_render_groupst::main;
	return visual_render_groupst::upper;
}

template<typename T,T default_value=T{}>
class scoped_zerost
{
	T *value_ptr;
	T saved{};

	public:
		explicit scoped_zerost(T &value):value_ptr(&value)
			{
			saved=std::move(value);
			value=default_value;
			}

		~scoped_zerost()
			{
			*value_ptr=std::move(saved);
			}

		scoped_zerost(const scoped_zerost &)=delete;
		scoped_zerost &operator=(const scoped_zerost &)=delete;
};

struct render_proxyst
{
	viewport_visual_layer layer;
	float source_x;
	float source_y;
	int32_t target_x;
	int32_t target_y;
	int32_t texpos;
	float progress;
	SDL_Texture *texture;
	std::set<std::pair<int32_t,int32_t>> coverage;
};

constexpr uint16_t visual_layer_bit(viewport_visual_layer layer)
{
	return uint16_t(1U<<static_cast<uint8_t>(layer));
}

uint16_t selected_mask(
	const std::unordered_map<int32_t,uint16_t> &selected,
	int32_t index)
{
	const auto found=selected.find(index);
	return found==selected.end()?0:found->second;
}

template<size_t Layer=0,typename Callback>
void with_suppressed_visual_layers(
	const std::array<int32_t *,visual_layer_count> &layers,
	int32_t index,
	uint16_t mask,
	const Callback &callback)
{
	if constexpr(Layer==visual_layer_count)
		callback();
	else if(mask&(1U<<Layer))
		{
		scoped_zerost<int32_t> zero(layers[Layer][index]);
		with_suppressed_visual_layers<Layer+1>(layers,index,mask,callback);
		}
	else
		with_suppressed_visual_layers<Layer+1>(layers,index,mask,callback);
}

template<typename Callback>
void with_base_suppressed(
	df::graphic_viewportst *vp,
	int32_t index,
	const Callback &callback)
{
	scoped_zerost<int32_t> background(vp->screentexpos_background[index]);
	scoped_zerost<uint64_t> floor_flag(vp->screentexpos_floor_flag[index]);
	scoped_zerost<int32_t> background_two(vp->screentexpos_background_two[index]);
	scoped_zerost<uint32_t> liquid_flag(vp->screentexpos_liquid_flag[index]);
	scoped_zerost<uint32_t> spatter_flag(vp->screentexpos_spatter_flag[index]);
	scoped_zerost<int32_t> spatter(vp->screentexpos_spatter[index]);
	scoped_zerost<uint64_t> ramp_flag(vp->screentexpos_ramp_flag[index]);
	scoped_zerost<uint32_t> shadow_flag(vp->screentexpos_shadow_flag[index]);
	scoped_zerost<int32_t> building_one(vp->screentexpos_building_one[index]);
	callback();
}

template<typename Callback>
void with_main_suppressed(
	df::graphic_viewportst *vp,
	int32_t index,
	const Callback &callback)
{
	with_base_suppressed(vp,index,[&]
		{
		scoped_zerost<int32_t> vermin(vp->screentexpos_vermin[index]);
		callback();
		});
}

template<typename Callback>
void with_upper_suppressed(
	df::graphic_viewportst *vp,
	int32_t index,
	const Callback &callback)
{
	with_main_suppressed(vp,index,[&]
		{
		scoped_zerost<int32_t> building_two(vp->screentexpos_building_two[index]);
		scoped_zerost<int32_t> projectile(vp->screentexpos_projectile[index]);
		scoped_zerost<int32_t> high_flow(vp->screentexpos_high_flow[index]);
		scoped_zerost<int32_t> top_shadow(vp->screentexpos_top_shadow[index]);
		scoped_zerost<int32_t> signpost(vp->screentexpos_signpost[index]);
		callback();
		});
}

void redraw_world_tile(
	df::renderer_2d_base *renderer,
	df::graphic_viewportst *vp,
	int32_t x,
	int32_t y,
	const std::unordered_map<int32_t,uint16_t> &selected)
{
	const int32_t index=x*vp->dim_y+y;
	const auto redraw=[&]
		{
		for(int32_t lower=7;lower>=0;--lower)
			{
			df::graphic_viewportst *lower_vp=gps->lower_viewport[lower];
			if(lower_vp!=nullptr&&lower_vp->flag.bits.active)
				renderer->update_viewport_tile(lower_vp,x,y);
			}
		renderer->update_viewport_tile(vp,x,y);
		};
	with_suppressed_visual_layers(
		visual_layers(vp),index,selected_mask(selected,index),redraw);
}

enum class redraw_stagest
{
	item,
	vehicle,
	main,
	upper
};

void redraw_above(
	df::renderer_2d_base *renderer,
	df::graphic_viewportst *vp,
	int32_t x,
	int32_t y,
	redraw_stagest stage,
	const std::unordered_map<int32_t,uint16_t> &selected)
{
	const int32_t index=x*vp->dim_y+y;
	constexpr uint16_t item_mask=visual_layer_bit(viewport_visual_layer::item);
	constexpr uint16_t vehicle_mask=
		item_mask|visual_layer_bit(viewport_visual_layer::vehicle);
	constexpr uint16_t main_mask=vehicle_mask|
		visual_layer_bit(viewport_visual_layer::right)|
		visual_layer_bit(viewport_visual_layer::center)|
		visual_layer_bit(viewport_visual_layer::left);
	constexpr uint16_t upper_mask=main_mask|
		visual_layer_bit(viewport_visual_layer::upright)|
		visual_layer_bit(viewport_visual_layer::up)|
		visual_layer_bit(viewport_visual_layer::upleft);
	const std::array masks={item_mask,vehicle_mask,main_mask,upper_mask};
	const auto redraw=[&]{renderer->update_viewport_tile(vp,x,y);};
	const auto suppress_visuals=[&]
		{
		with_suppressed_visual_layers(
			visual_layers(vp),
			index,
			selected_mask(selected,index)|masks[static_cast<size_t>(stage)],
			redraw);
		};
	if(stage==redraw_stagest::item||stage==redraw_stagest::vehicle)
		with_base_suppressed(vp,index,suppress_visuals);
	else if(stage==redraw_stagest::main)
		with_main_suppressed(vp,index,suppress_visuals);
	else
		with_upper_suppressed(vp,index,suppress_visuals);
}

void draw_proxy(df::renderer_2d_base *renderer,const render_proxyst &proxy)
{
	const int32_t zoom=renderer->viewport_zoom_factor;
	const int32_t target_x=tile_pixel(proxy.target_x,renderer->origin_x,zoom);
	const int32_t target_y=tile_pixel(proxy.target_y,renderer->origin_y,zoom);
	const float tile_size=float(zoom==128?32:std::max(1,zoom*32/128));
	const float source_x=target_x+(proxy.source_x-proxy.target_x)*tile_size;
	const float source_y=target_y+(proxy.source_y-proxy.target_y)*tile_size;
	const SDL_FRect destination=
		{
		source_x+(target_x-source_x)*proxy.progress,
		source_y+(target_y-source_y)*proxy.progress,
		tile_size,
		tile_size
		};
	render_copy_f(
		static_cast<SDL_Renderer *>(renderer->sdl_renderer),
		proxy.texture,
		nullptr,
		&destination);
}

std::vector<render_proxyst> collect_proxies(
	df::renderer_2d_base *renderer,
	df::graphic_viewportst *vp)
{
	std::vector<render_proxyst> proxies;
	auto layers=visual_layers(vp);
	const std::array order={
		viewport_visual_layer::center,
		viewport_visual_layer::item,
		viewport_visual_layer::vehicle,
		viewport_visual_layer::right,
		viewport_visual_layer::left,
		viewport_visual_layer::upright,
		viewport_visual_layer::up,
		viewport_visual_layer::upleft,
		viewport_visual_layer::designation
		};
	for(viewport_visual_layer visual_layer:order)
		{
		const size_t layer=static_cast<size_t>(visual_layer);
		for(int32_t y=0;y<vp->dim_y;++y)
			{
			for(int32_t x=0;x<vp->dim_x;++x)
				{
				const int32_t index=x*vp->dim_y+y;
				const int32_t texpos=layers[layer][index];
				if(texpos==0)continue;
				const auto movement=animation_manager.get_movement(
					vp,static_cast<viewport_visual_layer>(layer),x,y);
				if(!movement.active)continue;
				if(!visual_layer_moves_independently(visual_layer))
					{
					bool anchored=false;
					for(const render_proxyst &anchor:proxies)
						{
						if(anchor.layer==viewport_visual_layer::center&&
							std::abs(anchor.target_x-x)<=1&&
							std::abs(anchor.target_y-y)<=1&&
							anchor.source_x-anchor.target_x==movement.source_x-x&&
							anchor.source_y-anchor.target_y==movement.source_y-y&&
							anchor.progress==movement.progress)anchored=true;
						}
					if(!anchored)continue;
					}
				if((visual_layer==viewport_visual_layer::item||
					visual_layer==viewport_visual_layer::designation)&&
					movement.inherited)
					{
					const int32_t source_x=x-
						((x>movement.source_x)-(x<movement.source_x));
					const int32_t source_y=y-
						((y>movement.source_y)-(y<movement.source_y));
					if(source_x<0||source_x>=vp->dim_x||
						source_y<0||source_y>=vp->dim_y)continue;
					const int32_t source=
						source_x*vp->dim_y+source_y;
					if(!visual_moved_between_tiles(
						visual_layer,
						layers[layer],
						visual_layer==viewport_visual_layer::item?
							vp->screentexpos_item_old:
							vp->screentexpos_designation_old,
						source,
						index))continue;
					}

				render_proxyst proxy=
					{
					static_cast<viewport_visual_layer>(layer),
					movement.source_x,
					movement.source_y,
					x,
					y,
					texpos,
					movement.progress,
					nullptr,
					{}
					};
				bool blocked=false;
				for(int32_t coverage_x=int32_t(std::floor(
						std::min(proxy.source_x,float(x))));
					coverage_x<=int32_t(std::ceil(
						std::max(proxy.source_x,float(x))));++coverage_x)
					{
					for(int32_t coverage_y=int32_t(std::floor(
							std::min(proxy.source_y,float(y))));
						coverage_y<=int32_t(std::ceil(
							std::max(proxy.source_y,float(y))));++coverage_y)
						{
						if(!inside_clip(vp,coverage_x,coverage_y))
							{
							blocked=true;
							break;
							}
						if(visual_render_group(proxy.layer)==visual_render_groupst::main&&
							has_fire(vp,coverage_x,coverage_y))
							{
							blocked=true;
							break;
							}
						proxy.coverage.emplace(coverage_x,coverage_y);
						}
					if(blocked)break;
					}
				if(blocked)continue;

				df::texture_fullid texture_id;
				texture_id.texpos=texpos;
				texture_id.r=texture_id.g=texture_id.b=1.0f;
				texture_id.br=texture_id.bg=texture_id.bb=0.0f;
				texture_id.flag=
					df::texture_fullid_flag::mask_transparent_background;
				const auto texture=renderer->tile_cache.tile_cache.find(texture_id);
				if(texture==renderer->tile_cache.tile_cache.end())
					{
					continue;
					}
				proxy.texture=static_cast<SDL_Texture *>(texture->second);
				proxies.push_back(std::move(proxy));
				}
			}
		}
	return proxies;
}

using tile_coveragest=std::set<std::pair<int32_t,int32_t>>;

struct render_coveragest
{
	tile_coveragest all;
	tile_coveragest item;
	tile_coveragest vehicle;
	tile_coveragest main;
	tile_coveragest upper;
	std::unordered_map<int32_t,uint16_t> selected;
};

render_coveragest collect_coverage(
	const std::vector<render_proxyst> &proxies,
	int32_t dim_y)
{
	render_coveragest coverage;
	for(const render_proxyst &proxy:proxies)
		{
		coverage.all.insert(proxy.coverage.begin(),proxy.coverage.end());
		coverage.selected[proxy.target_x*dim_y+proxy.target_y]|=
			visual_layer_bit(proxy.layer);
		tile_coveragest *layer_coverage=nullptr;
		switch(visual_render_group(proxy.layer))
			{
			case visual_render_groupst::item:
			layer_coverage=&coverage.item;
			break;
			case visual_render_groupst::vehicle:
			layer_coverage=&coverage.vehicle;
			break;
			case visual_render_groupst::main:
			layer_coverage=&coverage.main;
			break;
			case visual_render_groupst::upper:
			layer_coverage=&coverage.upper;
			break;
			case visual_render_groupst::designation:
			break;
			}
		if(layer_coverage!=nullptr)
			layer_coverage->insert(proxy.coverage.begin(),proxy.coverage.end());
		}
	return coverage;
}

void draw_interpolation_stages(
	df::renderer_2d_base *renderer,
	df::graphic_viewportst *vp,
	const std::vector<render_proxyst> &proxies,
	const render_coveragest &coverage)
{
	const auto draw_matching=[&](const auto &predicate)
		{
		for(const render_proxyst &proxy:proxies)
			if(predicate(proxy.layer))draw_proxy(renderer,proxy);
		};
	const auto redraw=[&](const tile_coveragest &tiles,redraw_stagest stage)
		{
		for(const auto &[x,y]:tiles)
			redraw_above(renderer,vp,x,y,stage,coverage.selected);
		};

	draw_matching([](viewport_visual_layer layer)
		{return visual_render_group(layer)==visual_render_groupst::item;});
	redraw(coverage.item,redraw_stagest::item);
	draw_matching([](viewport_visual_layer layer)
		{return visual_render_group(layer)==visual_render_groupst::vehicle;});
	redraw(coverage.vehicle,redraw_stagest::vehicle);
	draw_matching([](viewport_visual_layer layer)
		{return visual_render_group(layer)==visual_render_groupst::main;});
	redraw(coverage.main,redraw_stagest::main);
	draw_matching([](viewport_visual_layer layer)
		{
		return visual_render_group(layer)==visual_render_groupst::upper;
		});
	redraw(coverage.upper,redraw_stagest::upper);
	draw_matching([](viewport_visual_layer layer)
		{return visual_render_group(layer)==visual_render_groupst::designation;});
}

void render_interpolated_world(df::renderer_2d_base *renderer)
{
	df::graphic_viewportst *vp=gps?gps->main_viewport:nullptr;

	if(vp!=nullptr)update_visual_context(renderer,vp);
	animation_manager.begin_frame(Core::getInstance().p->getTickCount());
	if(vp!=nullptr)
		{
		const auto input=animation_input(vp);
		animation_manager.synchronize_viewport(
			input,
			vp->flag.bits.active);
		}
	animation_manager.end_frame();

	if(vp==nullptr||!vp->flag.bits.active||renderer->sdl_renderer==nullptr)
		return;
	update_camera(renderer,vp,animation_manager.get_frame_delta_ms());
	const double cam_tile=tile_px(renderer);
	const int32_t glide_x=int32_t(std::lround(transient_x+rest_x*cam_tile));
	const int32_t glide_y=int32_t(std::lround(transient_y+rest_y*cam_tile));
	const bool glide=glide_x!=0||glide_y!=0;
	if(!glide&&camera_was_offset)
		{
		// The camera just re-joined the grid: one engine redraw replaces the last shifted frame.
		camera_was_offset=false;
		if(gps!=nullptr)++gps->force_full_display_count;
		}
	if(glide)camera_was_offset=true;
	if(!glide&&!animation_manager.requires_full_redraw())
		return;

	std::vector<render_proxyst> proxies=collect_proxies(renderer,vp);
	render_coveragest coverage=collect_coverage(proxies,vp->dim_y);

	SDL_Renderer *sdl_renderer=static_cast<SDL_Renderer *>(renderer->sdl_renderer);
	const int32_t zoom=renderer->viewport_zoom_factor;
	const int32_t tile_size=zoom==128?32:std::max(1,zoom*32/128);

	if(glide)
		{
		// Camera mid-glide: repaint the WHOLE map rect at the shifted origin so the world (and
		// the creature proxies, which read origin at draw time) renders between tiles. The engine
		// already drew this frame at the snapped position; everything here overdraws it, clipped
		// to the map rect so shifted tiles never spill over the UI. The uncovered strip on the
		// trailing edge stays black until the glide lands.
		const SDL_Rect map_rect=
			{
			tile_pixel(vp->clipx[0],renderer->origin_x,zoom),
			tile_pixel(vp->clipy[0],renderer->origin_y,zoom),
			tile_pixel(vp->clipx[1]+1,renderer->origin_x,zoom)-
				tile_pixel(vp->clipx[0],renderer->origin_x,zoom),
			tile_pixel(vp->clipy[1]+1,renderer->origin_y,zoom)-
				tile_pixel(vp->clipy[0],renderer->origin_y,zoom)
			};
		render_set_clip_rect(sdl_renderer,&map_rect);
		Uint8 old_r=0,old_g=0,old_b=0,old_a=255;
		get_render_draw_color(sdl_renderer,&old_r,&old_g,&old_b,&old_a);
		set_render_draw_color(sdl_renderer,0,0,0,255);
		render_fill_rect(sdl_renderer,&map_rect);
		set_render_draw_color(sdl_renderer,old_r,old_g,old_b,old_a);

		const int32_t saved_origin_x=renderer->origin_x;
		const int32_t saved_origin_y=renderer->origin_y;
		renderer->origin_x+=glide_x;
		renderer->origin_y+=glide_y;
		for(int32_t x=vp->clipx[0];x<=vp->clipx[1];++x)
			{
			for(int32_t y=vp->clipy[0];y<=vp->clipy[1];++y)
				redraw_world_tile(renderer,vp,x,y,coverage.selected);
			}
		draw_interpolation_stages(renderer,vp,proxies,coverage);
		renderer->origin_x=saved_origin_x;
		renderer->origin_y=saved_origin_y;
		render_set_clip_rect(sdl_renderer,nullptr);

		// Everything was repainted; per-tile coverage bookkeeping restarts after the glide.
		previous_coverage.clear();
		return;
		}

	std::set<std::pair<int32_t,int32_t>> redraw_coverage=coverage.all;
	redraw_coverage.insert(previous_coverage.begin(),previous_coverage.end());
	Uint8 old_r=0,old_g=0,old_b=0,old_a=255;
	get_render_draw_color(sdl_renderer,&old_r,&old_g,&old_b,&old_a);
	set_render_draw_color(sdl_renderer,0,0,0,255);
	for(const auto &[x,y]:redraw_coverage)
		{
		if(!inside_clip(vp,x,y))continue;
		const SDL_Rect tile_rect=
			{
			tile_pixel(x,renderer->origin_x,zoom),
			tile_pixel(y,renderer->origin_y,zoom),
			tile_size,
			tile_size
			};
		render_fill_rect(sdl_renderer,&tile_rect);
		}
	set_render_draw_color(sdl_renderer,old_r,old_g,old_b,old_a);

	for(const auto &[x,y]:redraw_coverage)
		{
		if(inside_clip(vp,x,y))redraw_world_tile(renderer,vp,x,y,coverage.selected);
		}
	draw_interpolation_stages(renderer,vp,proxies,coverage);

	previous_coverage=std::move(coverage.all);
}

struct renderer_hook : df::renderer_2d_base
{
	typedef df::renderer_2d_base interpose_base;
	DEFINE_VMETHOD_INTERPOSE(void,update_all,());
};

IMPLEMENT_VMETHOD_INTERPOSE(renderer_hook,update_all);

void renderer_hook::interpose_fn_update_all()
{
	// update_all is the existing UI stage, so world correction must run first.
	render_interpolated_world(this);
	INTERPOSE_NEXT(update_all)();
}

void clear_sdl_bindings()
{
	if(sdl_handle!=nullptr)ClosePlugin(sdl_handle);
	sdl_handle=nullptr;
	render_copy_f=nullptr;
	render_fill_rect=nullptr;
	render_set_clip_rect=nullptr;
	get_render_draw_color=nullptr;
	set_render_draw_color=nullptr;
}

bool load_sdl(color_ostream &out)
{
	clear_sdl_bindings();
	sdl_handle=OpenPlugin(sdl_library);
	if(sdl_handle==nullptr)
		{
		out.printerr("smooth-movement: could not load SDL2\n");
		return false;
		}
	#define bind(name,target) \
		target=reinterpret_cast<decltype(target)>(LookupPlugin(sdl_handle,#name)); \
		if(target==nullptr) { \
			out.printerr("smooth-movement: SDL2 function unavailable: " #name "\n"); \
			clear_sdl_bindings(); \
			return false; \
		}
	bind(SDL_RenderCopyF,render_copy_f);
	bind(SDL_RenderFillRect,render_fill_rect);
	bind(SDL_RenderSetClipRect,render_set_clip_rect);
	bind(SDL_GetRenderDrawColor,get_render_draw_color);
	bind(SDL_SetRenderDrawColor,set_render_draw_color);
	#undef bind
	return true;
}

void reset_state()
{
	animation_manager=visual_animation_managerst();
	previous_coverage.clear();
	visual_context_revision=0;
	previous_viewport=nullptr;
	previous_view_signature={};
	has_view_signature=false;
	previous_pan_x=0;
	previous_pan_y=0;
	has_pan_context=false;
	cancel_camera_transients();
	rest_x=0.0;
	rest_y=0.0;
	camera_enabled=false;
	camera_has_prev=false;
	camera_was_offset=false;
}

command_result status_command(
	color_ostream &out,
	std::vector<std::string> &parameters)
{
	if(parameters.empty())
		{
		out.print(
			"smooth-movement {}: {}\n",
			plugin_version,
			is_enabled?"enabled":"disabled");
		out.print("free camera: {}, offset {:.3f} {:.3f} (tiles east/south of the grid)\n",
			camera_enabled?"on":"off",-rest_x,-rest_y);
		return CR_OK;
		}
	if(parameters[0]=="camera")
		{
		if(parameters.size()==1)
			{
			out.print("free camera: {}, offset {:.3f} {:.3f}\n",
				camera_enabled?"on":"off",-rest_x,-rest_y);
			return CR_OK;
			}
		if(parameters.size()==2&&parameters[1]=="on")
			{
			set_camera_enabled(true);
			return CR_OK;
			}
		if(parameters.size()==2&&parameters[1]=="off")
			{
			set_camera_enabled(false);
			return CR_OK;
			}
		if(parameters.size()==2&&parameters[1]=="reset")
			{
			rest_x=0.0;
			rest_y=0.0;
			return CR_OK;
			}
		if(parameters.size()==3)
			{
			try
				{
				const double fx=std::stod(parameters[1]);
				const double fy=std::stod(parameters[2]);
				if(fx<-0.99||fx>0.99||fy<-0.99||fy>0.99)
					{
					out.printerr("offsets must be within -0.99..0.99 tiles\n");
					return CR_FAILURE;
					}
				// User-facing: positive = view sits east/south of the grid position.
				set_camera_enabled(true);
				rest_x=-fx;
				rest_y=-fy;
				normalize_rest();
				return CR_OK;
				}
			catch(...)
				{
				return CR_WRONG_USAGE;
				}
			}
		return CR_WRONG_USAGE;
		}
	return CR_WRONG_USAGE;
}

} // namespace

DFhackCExport command_result
plugin_init(color_ostream &,std::vector<PluginCommand> &commands)
{
	commands.emplace_back(
		"smooth-movement",
		"Smooth movement status; free camera: camera on|off|reset|<fx> <fy>.",
		status_command);
	return CR_OK;
}

DFhackCExport command_result plugin_enable(color_ostream &out,bool enable)
{
	if(is_enabled==enable)return CR_OK;
	if(enable)
		{
		reset_state();
		if(!load_sdl(out))return CR_FAILURE;
		if(!INTERPOSE_HOOK(renderer_hook,update_all).apply())
			{
			out.printerr("smooth-movement: could not hook the 2D renderer\n");
			clear_sdl_bindings();
			return CR_FAILURE;
			}
		}
	else
		{
		INTERPOSE_HOOK(renderer_hook,update_all).remove();
		reset_state();
		clear_sdl_bindings();
		if(gps!=nullptr)++gps->force_full_display_count;
		}
	is_enabled=enable;
	out.print("smooth-movement: {}\n",enable?"enabled":"disabled");
	return CR_OK;
}

DFhackCExport command_result plugin_shutdown(color_ostream &out)
{
	return plugin_enable(out,false);
}
