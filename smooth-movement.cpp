// SPDX-License-Identifier: MIT

#include "Core.h"
#include "MemAccess.h"
#include "PluginManager.h"
#include "VTableInterpose.h"

#include "modules/DFSDL.h"
#include "modules/Materials.h"
#include "modules/Units.h"

#include "df/enabler.h"
#include "df/graphic.h"
#include "df/graphic_viewportst.h"
#include "df/item.h"
#include "df/item_type.h"
#include "df/material.h"
#include "df/plotinfost.h"
#include "df/renderer_2d_base.h"
#include "df/texture_fullid.h"
#include "df/unit.h"
#include "df/unit_inventory_item.h"
#include "df/viewport_spatter_flag.h"

#include "visual_animation.h"

#include <SDL_render.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
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
REQUIRE_GLOBAL(pause_state);
REQUIRE_GLOBAL(plotinfo);
REQUIRE_GLOBAL(window_x);
REQUIRE_GLOBAL(window_y);
REQUIRE_GLOBAL(window_z);

namespace {

constexpr const char *plugin_version="0.5.0";

// Frame timing for `smooth-movement stats`. Counting is always on: one increment per frame,
// one per painted frame, one per engine repaint. The clock is read three times per frame while
// enabled. Sync covers movement detection across every viewport; render covers everything
// after it, including the camera update and carried-item lookup that feed the draw decision,
// then proxy collection, tile blanking, engine repaints and sprites. The render thread
// writes, the console thread reads and clears, so the fields are relaxed atomics; a clear
// that lands mid-frame skews that one frame and nothing else.
struct frame_statsst
{
	std::atomic<bool> enabled{false};
	std::atomic<uint64_t> frames{0};
	std::atomic<uint64_t> painted{0};
	std::atomic<uint64_t> repaints{0};
	std::atomic<uint64_t> timed{0};
	std::atomic<uint64_t> sync_us{0};
	std::atomic<uint64_t> sync_max_us{0};
	std::atomic<uint64_t> render_us{0};
	std::atomic<uint64_t> render_max_us{0};

	static uint64_t now_us()
		{
		return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count());
		}

	void clear()
		{
		for(std::atomic<uint64_t> *counter:{&frames,&painted,&repaints,&timed,
			&sync_us,&sync_max_us,&render_us,&render_max_us})
			counter->store(0,std::memory_order_relaxed);
		}

	static void add(std::atomic<uint64_t> &total,std::atomic<uint64_t> &max,uint64_t us)
		{
		total.fetch_add(us,std::memory_order_relaxed);
		uint64_t seen=max.load(std::memory_order_relaxed);
		while(seen<us&&!max.compare_exchange_weak(seen,us,std::memory_order_relaxed)){}
		}

	void add_sync(uint64_t us){add(sync_us,sync_max_us,us);}
	void add_render(uint64_t us){add(render_us,render_max_us,us);}
};

frame_statsst frame_stats;

// Runs a callback when the scope ends, whichever return path is taken.
template<typename Callback>
struct scope_guardst
{
	Callback callback;
	explicit scope_guardst(Callback c):callback(std::move(c)){}
	~scope_guardst(){callback();}
	scope_guardst(const scope_guardst &)=delete;
	scope_guardst &operator=(const scope_guardst &)=delete;
};

// Runtime harness for the engine-owned visual state; gameplay data is never read.
decltype(&SDL_RenderCopyF) render_copy_f=nullptr;
decltype(&SDL_RenderCopyExF) render_copy_ex_f=nullptr;
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
bool flip_enabled=false;
bool hauled_enabled=false;

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
int32_t self_scroll_x=0;                      // window deltas WE wrote: visual no-ops when landing
int32_t self_scroll_y=0;
visual_movement_idst camera_follow_id=no_visual_movement;
const void *camera_follow_viewport=nullptr;
double camera_follow_x=0.0;
double camera_follow_y=0.0;
bool camera_ignore_pending=false;
bool drag_active=false;
double drag_anchor_vx=0.0;                    // visual camera at drag start, tiles
double drag_anchor_vy=0.0;
int32_t drag_anchor_mx=0;                     // precise mouse at drag start, pixels
int32_t drag_anchor_my=0;
bool camera_was_offset=false;                 // edge-detects offset->0 for one cleanup redraw
int32_t camera_prev_wx=0;                     // window-scroll observation baseline
int32_t camera_prev_wy=0;
bool camera_has_prev=false;
int32_t native_follow_id=-1;

double tile_px(const df::renderer_2d_base *renderer)
{
	const int32_t zoom=renderer->viewport_zoom_factor;
	return double(zoom==128?32:std::max(1,zoom*32/128));
}

// Match ratio of "buffers shifted by (dwx,dwy)" on the background layer: 0..1, or -1 when there
// is nothing to compare (empty background).
// Cancel everything except the persistent rest offset (the camera keeps its sub-tile position
// across zoom/z/resize; only the in-flight animation state is unfollowable).
void clear_camera_tracking()
{
	self_scroll_x=0;
	self_scroll_y=0;
	camera_follow_id=no_visual_movement;
	camera_follow_viewport=nullptr;
	camera_follow_x=0.0;
	camera_follow_y=0.0;
	camera_ignore_pending=false;
}

void cancel_camera_transients()
{
	transient_x=0.0;
	transient_y=0.0;
	clear_camera_tracking();
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

// Per-frame camera bookkeeping: observe window scrolls, attribute them when the buffers apply
// them (glide vs our own normalization writes), drive the drag, decay the transient.
void update_camera(
	df::renderer_2d_base *renderer,
	const df::graphic_viewportst *vp,
	uint32_t delta_ms,
	bool native_follow_active)
{
	if(!camera_glide_enabled(camera_enabled,native_follow_active))return;
	const double tile=tile_px(renderer);
	const double k=std::exp(-double(delta_ms)/camera_tau_ms);
	transient_x*=k;
	transient_y*=k;
	const int32_t wx=window_x?*window_x:0;
	const int32_t wy=window_y?*window_y:0;
	if(camera_has_prev&&(wx!=camera_prev_wx||wy!=camera_prev_wy))
		{
		const int32_t dx=wx-camera_prev_wx;
		const int32_t dy=wy-camera_prev_wy;
		if((std::abs(dx)>camera_max_glide_tiles||std::abs(dy)>camera_max_glide_tiles)&&
			!drag_active)
			{
			transient_x=0.0;
			transient_y=0.0;
			camera_follow_id=no_visual_movement;
			camera_follow_x=0.0;
			camera_follow_y=0.0;
			camera_ignore_pending=true;
			}
		}
	camera_prev_wx=wx;
	camera_prev_wy=wy;
	camera_has_prev=true;

	const visual_scroll_renderst scroll=animation_manager.get_scroll(vp);
	if(scroll.abandoned)
		{
		clear_camera_tracking();
		}
	if(scroll.landed)
		{
		int32_t sx=0;
		if(self_scroll_x!=0&&scroll.landed_x!=0&&
			(self_scroll_x>0)==(scroll.landed_x>0))
			sx=std::abs(self_scroll_x)<=std::abs(scroll.landed_x)?
				self_scroll_x:scroll.landed_x;
		int32_t sy=0;
		if(self_scroll_y!=0&&scroll.landed_y!=0&&
			(self_scroll_y>0)==(scroll.landed_y>0))
			sy=std::abs(self_scroll_y)<=std::abs(scroll.landed_y)?
				self_scroll_y:scroll.landed_y;
		self_scroll_x-=sx;
		self_scroll_y-=sy;
		rest_x+=sx;
		rest_y+=sy;
		const int32_t gx=scroll.landed_x-sx;
		const int32_t gy=scroll.landed_y-sy;
		const bool ignore=camera_ignore_pending;
		if(!scroll.pending)camera_ignore_pending=false;
		if(drag_active)
			{
			rest_x+=gx;
			rest_y+=gy;
			}
		else if((gx!=0||gy!=0)&&!ignore&&native_follow_active&&sx==0&&sy==0&&
			scroll.follow_candidate!=no_visual_movement)
			{
			camera_follow_id=scroll.follow_candidate;
			camera_follow_viewport=vp;
			transient_x=0.0;
			transient_y=0.0;
			}
		else if((gx!=0||gy!=0)&&!ignore)
			{
			camera_follow_id=no_visual_movement;
			camera_follow_viewport=nullptr;
			camera_follow_x=0.0;
			camera_follow_y=0.0;
			const double cap=tile*(camera_max_glide_tiles+0.5);
			transient_x=std::clamp(transient_x+gx*tile,-cap,cap);
			transient_y=std::clamp(transient_y+gy*tile,-cap,cap);
			}
		}
	if(camera_follow_id!=no_visual_movement)
		{
		const auto follow=animation_manager.get_follow(
			camera_follow_viewport,camera_follow_id);
		if(follow.active)
			{
			camera_follow_x=follow.offset_x*tile;
			camera_follow_y=follow.offset_y*tile;
			}
		else
			{
			camera_follow_id=no_visual_movement;
			camera_follow_viewport=nullptr;
			camera_follow_x=0.0;
			camera_follow_y=0.0;
			}
		}

	// --- pixel-perfect middle-mouse drag: the view follows the mouse 1:1 and rests where
	// released. DF's own drag still moves window in tile steps; rest carries the remainder.
	// Positions are tracked against the CONTENT window (window minus unlanded jumps) so the
	// buffer lag never causes a visible stutter.
	const bool mbut=camera_enabled&&enabler!=nullptr&&enabler->mouse_mbut;
	const double content_wx=double(wx-scroll.pending_x);
	const double content_wy=double(wy-scroll.pending_y);
	if(mbut&&!drag_active&&gps!=nullptr)
		{
		drag_active=true;
		drag_anchor_vx=content_wx-rest_x-(transient_x+camera_follow_x)/tile;
		drag_anchor_vy=content_wy-rest_y-(transient_y+camera_follow_y)/tile;
		drag_anchor_mx=gps->precise_mouse_x;
		drag_anchor_my=gps->precise_mouse_y;
		transient_x=0.0;
		transient_y=0.0;
		camera_follow_id=no_visual_movement;
		camera_follow_viewport=nullptr;
		camera_follow_x=0.0;
		camera_follow_y=0.0;
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

	if(std::abs(transient_x)<0.5&&std::abs(transient_y)<0.5)
		{
		transient_x=0.0;
		transient_y=0.0;
		}
}

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
	viewport_visual_layer layer;
	viewport_layer_memberst current;
	viewport_layer_memberst previous;
};

constexpr size_t visual_layer_count=static_cast<size_t>(viewport_visual_layer::count);
constexpr std::array visual_layer_buffers=
	{
	visual_layer_bufferst{viewport_visual_layer::right,
		&df::graphic_viewportst::screentexpos_right_creature,
		&df::graphic_viewportst::screentexpos_right_creature_old},
	visual_layer_bufferst{viewport_visual_layer::center,
		&df::graphic_viewportst::screentexpos,
		&df::graphic_viewportst::screentexpos_old},
	visual_layer_bufferst{viewport_visual_layer::left,
		&df::graphic_viewportst::screentexpos_left_creature,
		&df::graphic_viewportst::screentexpos_left_creature_old},
	visual_layer_bufferst{viewport_visual_layer::upright,
		&df::graphic_viewportst::screentexpos_upright_creature,
		&df::graphic_viewportst::screentexpos_upright_creature_old},
	visual_layer_bufferst{viewport_visual_layer::up,
		&df::graphic_viewportst::screentexpos_up_creature,
		&df::graphic_viewportst::screentexpos_up_creature_old},
	visual_layer_bufferst{viewport_visual_layer::upleft,
		&df::graphic_viewportst::screentexpos_upleft_creature,
		&df::graphic_viewportst::screentexpos_upleft_creature_old},
	visual_layer_bufferst{viewport_visual_layer::vehicle,
		&df::graphic_viewportst::screentexpos_vehicle,
		&df::graphic_viewportst::screentexpos_vehicle_old},
	visual_layer_bufferst{viewport_visual_layer::item,
		&df::graphic_viewportst::screentexpos_item,
		&df::graphic_viewportst::screentexpos_item_old},
	visual_layer_bufferst{viewport_visual_layer::designation,
		&df::graphic_viewportst::screentexpos_designation,
		&df::graphic_viewportst::screentexpos_designation_old}
	};

constexpr bool valid_visual_layer_buffers()
{
	uint16_t layers=0;
	for(const auto &buffer:visual_layer_buffers)
		{
		const uint16_t layer=uint16_t(1U<<static_cast<uint8_t>(buffer.layer));
		if(layers&layer)return false;
		layers|=layer;
		}
	return layers==uint16_t((1U<<visual_layer_count)-1);
}

static_assert(valid_visual_layer_buffers());

template<typename Viewport>
auto visual_layers(Viewport *vp,bool previous=false)
{
	using layer_pointer=std::conditional_t<
		std::is_const_v<Viewport>,const int32_t *,int32_t *>;
	std::array<layer_pointer,visual_layer_count> layers{};
	for(const auto &buffer:visual_layer_buffers)
		layers[static_cast<size_t>(buffer.layer)]=vp->*(previous?
			buffer.previous:buffer.current);
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
		vp->screentexpos_background,
		vp->screentexpos_background_old,
		window_x?*window_x:0,
		window_y?*window_y:0
		};
}

// The layer buffers are freed and nulled without clearing the active flag.
bool viewport_readable(df::graphic_viewportst *vp)
{
	return vp!=nullptr&&vp->flag.bits.active&&animation_input(vp).valid();
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

template<typename Flag>
bool fire_frame(const Flag &flag)
{
	if constexpr(std::is_same_v<std::remove_cv_t<Flag>,uint32_t>)
		return (flag&0x70000000U)!=0;
	else
		return flag.bits.fire_frame_type!=0;
}

bool has_fire(const df::graphic_viewportst *vp,int32_t x,int32_t y)
{
	return vp->screentexpos_spatter_flag!=nullptr&&
		fire_frame(vp->screentexpos_spatter_flag[x*vp->dim_y+y]);
}

template<typename T>
class scoped_value_restorest
{
	T &value;
	T saved;

	public:
		explicit scoped_value_restorest(T &value,T replacement=T{}):
			value(value),
			saved(std::exchange(value,std::move(replacement)))
			{
			static_assert(std::is_nothrow_move_assignable_v<T>);
			}

		~scoped_value_restorest() noexcept
			{
			value=std::move(saved);
			}

		scoped_value_restorest(const scoped_value_restorest &)=delete;
		scoped_value_restorest &operator=(const scoped_value_restorest &)=delete;
		scoped_value_restorest(scoped_value_restorest &&)=delete;
		scoped_value_restorest &operator=(scoped_value_restorest &&)=delete;
};

template<typename Callback>
void with_zeroed_values(const Callback &callback)
{
	callback();
}

template<typename Callback,typename T,typename... Values>
void with_zeroed_values(const Callback &callback,T &value,Values &...values)
{
	scoped_value_restorest<T> zero(value);
	with_zeroed_values(callback,values...);
}

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
	bool mirrored=false;
	int32_t mirror_shift=0;
	std::set<std::pair<int32_t,int32_t>> coverage;
};

struct carried_item_proxyst
{
	float source_x;
	float source_y;
	int32_t target_x;
	int32_t target_y;
	float progress;
	SDL_Texture *texture;
	std::set<std::pair<int32_t,int32_t>> coverage;
};

using tile_coveragest=std::set<std::pair<int32_t,int32_t>>;

struct render_coveragest
{
	tile_coveragest all;
	std::array<tile_coveragest,static_cast<size_t>(visual_render_groupst::count)> groups;
	std::unordered_map<int32_t,uint16_t> selected;
};

struct viewport_renderst
{
	df::graphic_viewportst *viewport;
	std::vector<render_proxyst> proxies;
	render_coveragest coverage;
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
		scoped_value_restorest<int32_t> zero(layers[Layer][index]);
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
	with_zeroed_values(
		callback,
		vp->screentexpos_background[index],
		vp->screentexpos_floor_flag[index],
		vp->screentexpos_background_two[index],
		vp->screentexpos_liquid_flag[index],
		vp->screentexpos_spatter_flag[index],
		vp->screentexpos_spatter[index],
		vp->screentexpos_ramp_flag[index],
		vp->screentexpos_shadow_flag[index],
		vp->screentexpos_building_one[index]);
}

template<typename Callback>
void with_main_suppressed(
	df::graphic_viewportst *vp,
	int32_t index,
	const Callback &callback)
{
	with_base_suppressed(vp,index,[&]
		{
		with_zeroed_values(callback,vp->screentexpos_vermin[index]);
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
		with_zeroed_values(
			callback,
			vp->screentexpos_building_two[index],
			vp->screentexpos_projectile[index],
			vp->screentexpos_high_flow[index],
			vp->screentexpos_top_shadow[index],
			vp->screentexpos_signpost[index]);
		});
}

// Every engine repaint the plugin asks for goes through here so `stats` can count them.
void engine_repaint(df::renderer_2d_base *renderer,df::graphic_viewportst *vp,int32_t x,int32_t y)
{
	frame_stats.repaints.fetch_add(1,std::memory_order_relaxed);
	renderer->update_viewport_tile(vp,x,y);
}

void redraw_viewport_tile(
	df::renderer_2d_base *renderer,
	const viewport_renderst &viewport,
	int32_t x,
	int32_t y,
	bool defer_interface)
{
	df::graphic_viewportst *vp=viewport.viewport;
	const int32_t index=x*vp->dim_y+y;
	const auto redraw=[&]{engine_repaint(renderer,vp,x,y);};
	const auto stage=[&]
		{
		with_suppressed_visual_layers(
			visual_layers(vp),index,
			selected_mask(viewport.coverage.selected,index),redraw);
		};
	// The interface layer is the shading for levels below the camera.
	// A staged tile has a sprite drawn over it afterwards, so draw_interface_only places it instead.
	if(!defer_interface||vp->screentexpos_interface==nullptr)stage();
	else with_zeroed_values(stage,vp->screentexpos_interface[index]);
}

// Every buffer the interface-only pass zeroes has to exist before it can be zeroed.
bool interface_pass_readable(const df::graphic_viewportst *vp)
{
	return vp!=nullptr&&
		vp->screentexpos_interface!=nullptr&&
		vp->screentexpos_background!=nullptr&&
		vp->screentexpos_floor_flag!=nullptr&&
		vp->screentexpos_background_two!=nullptr&&
		vp->screentexpos_liquid_flag!=nullptr&&
		vp->screentexpos_spatter_flag!=nullptr&&
		vp->screentexpos_spatter!=nullptr&&
		vp->screentexpos_ramp_flag!=nullptr&&
		vp->screentexpos_shadow_flag!=nullptr&&
		vp->screentexpos_building_one!=nullptr&&
		vp->screentexpos_vermin!=nullptr&&
		vp->screentexpos_building_two!=nullptr&&
		vp->screentexpos_projectile!=nullptr&&
		vp->screentexpos_high_flow!=nullptr&&
		vp->screentexpos_signpost!=nullptr;
}

// Runs after the proxies so the shading covers them rather than sitting underneath.
void draw_interface_only(
	df::renderer_2d_base *renderer,
	df::graphic_viewportst *vp,
	int32_t x,
	int32_t y)
{
	if(!interface_pass_readable(vp))return;
	const int32_t index=x*vp->dim_y+y;
	const auto redraw=[&]{engine_repaint(renderer,vp,x,y);};
	const auto without_visuals=[&]
		{
		with_suppressed_visual_layers(
			visual_layers(vp),
			index,
			uint16_t((1U<<visual_layer_count)-1),
			redraw);
		};
	with_zeroed_values(
		without_visuals,
		vp->screentexpos_background[index],
		vp->screentexpos_floor_flag[index],
		vp->screentexpos_background_two[index],
		vp->screentexpos_liquid_flag[index],
		vp->screentexpos_spatter_flag[index],
		vp->screentexpos_spatter[index],
		vp->screentexpos_ramp_flag[index],
		vp->screentexpos_shadow_flag[index],
		vp->screentexpos_building_one[index],
		vp->screentexpos_vermin[index],
		vp->screentexpos_building_two[index],
		vp->screentexpos_projectile[index],
		vp->screentexpos_high_flow[index],
		vp->screentexpos_signpost[index]);
}

void redraw_world_tile(
	df::renderer_2d_base *renderer,
	const std::vector<viewport_renderst> &viewports,
	const tile_coveragest &staged,
	int32_t x,
	int32_t y)
{
	// The stage pass repaints everything above the lowest across the staged tiles, after the proxies.
	const bool staged_tile=staged.count({x,y})!=0;
	for(const viewport_renderst &viewport:viewports)
		{
		if(inside_clip(viewport.viewport,x,y))
			redraw_viewport_tile(renderer,viewport,x,y,staged_tile);
		if(staged_tile)break;
		}
}

constexpr uint16_t visual_layers_through_group(visual_render_groupst group)
{
	uint16_t mask=0;
	for(const auto &descriptor:visual_layer_descriptors)
		if(descriptor.render_group!=visual_render_groupst::designation&&
			static_cast<uint8_t>(descriptor.render_group)<=static_cast<uint8_t>(group))
			mask|=visual_layer_bit(descriptor.layer);
	return mask;
}

void redraw_above(
	df::renderer_2d_base *renderer,
	df::graphic_viewportst *vp,
	int32_t x,
	int32_t y,
	visual_render_groupst group,
	const std::unordered_map<int32_t,uint16_t> &selected)
{
	const int32_t index=x*vp->dim_y+y;
	const auto redraw=[&]{engine_repaint(renderer,vp,x,y);};
	const auto suppress_visuals=[&]
		{
		const auto stage=[&]
			{
			with_suppressed_visual_layers(
				visual_layers(vp),
				index,
				selected_mask(selected,index)|visual_layers_through_group(group),
				redraw);
			};
		// The interface layer sits above every group, so each group's redraw would paint it again.
		// draw_interface_only places it once, after the sprites.
		if(vp->screentexpos_interface==nullptr)stage();
		else with_zeroed_values(stage,vp->screentexpos_interface[index]);
		};
	if(group==visual_render_groupst::item||group==visual_render_groupst::vehicle)
		with_base_suppressed(vp,index,suppress_visuals);
	else if(group==visual_render_groupst::main)
		with_main_suppressed(vp,index,suppress_visuals);
	else
		with_upper_suppressed(vp,index,suppress_visuals);
}

SDL_Texture *cached_texture(
	df::renderer_2d_base *renderer,
	int32_t texpos,
	bool transparent_background=true)
{
	if(texpos==0)return nullptr;
	df::texture_fullid texture_id;
	texture_id.texpos=texpos;
	texture_id.r=texture_id.g=texture_id.b=1.0f;
	texture_id.br=texture_id.bg=texture_id.bb=0.0f;
	texture_id.flag=transparent_background?
		df::texture_fullid_flag::mask_transparent_background:
		0;
	const auto texture=renderer->tile_cache.tile_cache.find(texture_id);
	return texture==renderer->tile_cache.tile_cache.end()?
		nullptr:
		static_cast<SDL_Texture *>(texture->second);
}

// The render_copy_ex_f null check is defensive only, not a graceful-degradation path.
// `bind` aborts load_sdl on any missing symbol and plugin_enable then refuses the render hook.
void render_copy_maybe_mirrored(
	SDL_Renderer *renderer,
	SDL_Texture *texture,
	const SDL_FRect &destination,
	bool mirrored)
{
	if(mirrored&&render_copy_ex_f!=nullptr)
		{
		render_copy_ex_f(
			renderer,texture,nullptr,&destination,
			0.0,nullptr,SDL_FLIP_HORIZONTAL);
		return;
		}
	render_copy_f(renderer,texture,nullptr,&destination);
}

void draw_proxy(df::renderer_2d_base *renderer,const render_proxyst &proxy)
{
	const int32_t zoom=renderer->viewport_zoom_factor;
	const int32_t target_x=tile_pixel(proxy.target_x,renderer->origin_x,zoom);
	const int32_t target_y=tile_pixel(proxy.target_y,renderer->origin_y,zoom);
	const float tile_size=float(zoom==128?32:std::max(1,zoom*32/128));
	const float source_x=target_x+(proxy.source_x-proxy.target_x)*tile_size;
	const float source_y=target_y+(proxy.source_y-proxy.target_y)*tile_size;
	const float mirror_offset=float(proxy.mirror_shift)*tile_size;
	const SDL_FRect destination=
		{
		source_x+(target_x-source_x)*proxy.progress+mirror_offset,
		source_y+(target_y-source_y)*proxy.progress,
		tile_size,
		tile_size
		};
	render_copy_maybe_mirrored(
		static_cast<SDL_Renderer *>(renderer->sdl_renderer),
		proxy.texture,
		destination,
		proxy.mirrored);
}

void draw_carried_item_proxy(
	df::renderer_2d_base *renderer,
	const carried_item_proxyst &proxy)
{
	const int32_t zoom=renderer->viewport_zoom_factor;
	const float tile_size=float(zoom==128?32:std::max(1,zoom*32/128));
	const float target_x=tile_pixel(proxy.target_x,renderer->origin_x,zoom);
	const float target_y=tile_pixel(proxy.target_y,renderer->origin_y,zoom);
	const float source_x=target_x+(proxy.source_x-proxy.target_x)*tile_size;
	const float source_y=target_y+(proxy.source_y-proxy.target_y)*tile_size;
	const auto icon=carried_item_icon_rect(
		source_x+(target_x-source_x)*proxy.progress,
		source_y+(target_y-source_y)*proxy.progress,
		tile_size);
	const SDL_FRect destination={icon.x,icon.y,icon.width,icon.height};
	render_copy_f(
		static_cast<SDL_Renderer *>(renderer->sdl_renderer),
		proxy.texture,nullptr,&destination);
}

df::item *hauled_item(const df::unit *unit)
{
	if(unit==nullptr)return nullptr;
	for(const df::unit_inventory_item *inventory_item:unit->inventory)
		if(inventory_item!=nullptr&&inventory_item->item!=nullptr&&
			inventory_item->mode==df::inv_item_role_type::Hauled)
			return inventory_item->item;
	return nullptr;
}

SDL_Texture *cached_viewport_texture(
	df::renderer_2d_base *renderer,
	df::graphic_viewportst *vp,
	int32_t index,
	int32_t texpos)
{
	if(texpos==0)return nullptr;
	SDL_Texture *texture=cached_texture(renderer,texpos);
	if(texture!=nullptr||vp->screentexpos_background_two==nullptr)return texture;
	// Hauled items are not normally drawn, so stage one tile to populate the renderer cache.
	scoped_value_restorest<int32_t> staged(vp->screentexpos_background_two[index]);
	vp->screentexpos_background_two[index]=texpos;
	engine_repaint(renderer,vp,index/vp->dim_y,index%vp->dim_y);
	return cached_texture(renderer,texpos);
}

int32_t item_texpos(df::item *item)
{
	if(item==nullptr)return 0;
	const MaterialInfo material(item);
	if(!material.isValid())return 0;
	switch(item->getType())
		{
		case df::item_type::BOULDER:
			return material.material->boulder_texpos1!=0?
				material.material->boulder_texpos1:
				material.material->boulder_texpos2;
		case df::item_type::BAR:
			return material.material->bar_texpos;
		case df::item_type::WOOD:
			return material.material->wood_texpos;
		default:
			return 0;
		}
}

std::vector<carried_item_proxyst> collect_carried_item_proxies(
	df::renderer_2d_base *renderer,
	df::graphic_viewportst *vp)
{
	std::vector<carried_item_proxyst> proxies;
	if(window_x==nullptr||window_y==nullptr||window_z==nullptr)return proxies;

	std::vector<df::unit *> units;
	Units::getUnitsInBox(
		units,
		*window_x,*window_y,*window_z,
		*window_x+vp->dim_x-1,*window_y+vp->dim_y-1,*window_z,
		[](df::unit *unit){return !Units::isHidden(unit);});
	for(const df::unit *unit:units)
		{
		const int32_t x=unit->pos.x-*window_x;
		const int32_t y=unit->pos.y-*window_y;
		if(!inside_clip(vp,x,y))continue;
		const int32_t index=x*vp->dim_y+y;
		if(vp->screentexpos[index]==0)continue;
		const int32_t texpos=item_texpos(hauled_item(unit));
		SDL_Texture *texture=cached_viewport_texture(renderer,vp,index,texpos);
		if(texture==nullptr)continue;
		const auto movement=animation_manager.get_movement(
			vp,viewport_visual_layer::center,x,y);
		const float source_x=movement.active?movement.source_x:float(x);
		const float source_y=movement.active?movement.source_y:float(y);
		carried_item_proxyst proxy={
			source_x,source_y,x,y,movement.active?movement.progress:1.0f,texture,{}};
		for(int32_t coverage_x=int32_t(std::floor(std::min(source_x,float(x))));
			coverage_x<=int32_t(std::ceil(std::max(source_x,float(x))));++coverage_x)
			for(int32_t coverage_y=int32_t(std::floor(std::min(source_y,float(y))));
				coverage_y<=int32_t(std::ceil(std::max(source_y,float(y))));++coverage_y)
				if(inside_clip(vp,coverage_x,coverage_y))
					proxy.coverage.emplace(coverage_x,coverage_y);
		proxies.push_back(std::move(proxy));
		}
	return proxies;
}

std::vector<render_proxyst> collect_proxies(
	df::renderer_2d_base *renderer,
	df::graphic_viewportst *vp)
{
	std::vector<render_proxyst> proxies;
	auto layers=visual_layers(vp);
	auto previous_layers=visual_layers(vp,true);
	for(uint8_t draw_order=0;draw_order<visual_layer_count;++draw_order)
		{
		const viewport_visual_layer visual_layer=visual_layer_at_draw_order(draw_order);
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
				const int32_t inherited_source_x=inherited_visual_source_tile(
					x,movement.source_x,x);
				const int32_t inherited_source_y=inherited_visual_source_tile(
					y,movement.source_y,y);
				const bool inherited_source_in_bounds=
					inherited_source_x>=0&&inherited_source_x<vp->dim_x&&
					inherited_source_y>=0&&inherited_source_y<vp->dim_y;
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
					if(visual_layer==viewport_visual_layer::item&&
						vp->screentexpos_old[index]!=0)continue;
					if(!inherited_source_in_bounds)continue;
					const int32_t source=
						inherited_source_x*vp->dim_y+inherited_source_y;
					if(!visual_moved_between_tiles(
						visual_layer,
							layers[layer],
							previous_layers[layer],
							source,
							index))continue;
					}
				if(!visual_layer_moves_independently(visual_layer)&&
					visual_layer!=viewport_visual_layer::designation&&movement.inherited)
					{
					const bool fragment_moved=inherited_source_in_bounds&&
						visual_moved_between_tiles(
							visual_layer,layers[layer],previous_layers[layer],
							inherited_source_x*vp->dim_y+inherited_source_y,index);
					if(!fragment_moved)
						{
					const auto &descriptor=visual_layer_descriptor(visual_layer);
					bool owns_fragment=false;
					for(const render_proxyst &anchor:proxies)
						if(anchor.layer==viewport_visual_layer::center&&
							anchor.target_x==x+descriptor.center_x&&
							anchor.target_y==y+descriptor.center_y&&
							anchor.source_x-anchor.target_x==movement.source_x-x&&
							anchor.source_y-anchor.target_y==movement.source_y-y&&
							anchor.progress==movement.progress)owns_fragment=true;
					if(!owns_fragment)continue;
						}
					}

				// Items, vehicles and designations keep their vanilla orientation.
				const auto &mirror_descriptor=
					visual_layer_descriptor(visual_layer);
				const visual_render_groupst group=
					visual_render_group(visual_layer);
				const bool mirror_eligible=flip_enabled&&
					(group==visual_render_groupst::main||
					group==visual_render_groupst::upper);
				// Facing is read from the anchor tile so every fragment of one creature agrees.
				const bool mirrored=mirror_eligible&&
					animation_manager.get_facing(
						vp,
						x+mirror_descriptor.center_x,
						y+mirror_descriptor.center_y)!=native_sprite_facing;
				// The anchor's own layer has center_x 0, so it flips in place.
				const int32_t mirror_shift=
					mirrored?
					mirrored_tile_x(x,x+mirror_descriptor.center_x)-x:
					0;
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
					mirrored,
					mirror_shift,
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
				if(proxy.mirror_shift!=0)
					{
					std::set<std::pair<int32_t,int32_t>> mirrored_coverage;
					for(const auto &tile:proxy.coverage)
						mirrored_coverage.emplace(
							tile.first+proxy.mirror_shift,tile.second);
					for(const auto &tile:mirrored_coverage)
						{
						if(!inside_clip(vp,tile.first,tile.second))
							{
							blocked=true;
							break;
							}
						if(visual_render_group(proxy.layer)==visual_render_groupst::main&&
							has_fire(vp,tile.first,tile.second))
							{
							blocked=true;
							break;
							}
						proxy.coverage.insert(tile);
						}
					if(blocked)continue;
					}

				proxy.texture=cached_texture(renderer,texpos);
				if(proxy.texture==nullptr)continue;
				proxies.push_back(std::move(proxy));
				}
			}
		}

	// A creature that has stopped still needs its mirrored sprite painted each frame.
	// Otherwise the engine repaints it natively and the two orientations alternate between steps.
	// A fragment's tile is its anchor minus the layer's centre offset, inverting the moving path.
	if(flip_enabled)
		{
		for(int32_t anchor_x=0;anchor_x<vp->dim_x;++anchor_x)
			{
			for(int32_t anchor_y=0;anchor_y<vp->dim_y;++anchor_y)
				{
				if(animation_manager.get_facing(vp,anchor_x,anchor_y)==
					native_sprite_facing)continue;
				for(uint8_t draw_order=0;draw_order<visual_layer_count;++draw_order)
					{
					const viewport_visual_layer visual_layer=
						visual_layer_at_draw_order(draw_order);
					const visual_render_groupst group=
						visual_render_group(visual_layer);
					if(group!=visual_render_groupst::main&&
						group!=visual_render_groupst::upper)continue;
					const auto &descriptor=visual_layer_descriptor(visual_layer);
					const int32_t x=anchor_x-descriptor.center_x;
					const int32_t y=anchor_y-descriptor.center_y;
					if(x<0||x>=vp->dim_x||y<0||y>=vp->dim_y)continue;
					const size_t layer=static_cast<size_t>(visual_layer);
					const int32_t texpos=layers[layer][x*vp->dim_y+y];
					if(texpos==0)continue;
					bool already_drawn=false;
					for(const render_proxyst &existing:proxies)
						if(existing.layer==visual_layer&&
							existing.target_x==x&&existing.target_y==y)
							already_drawn=true;
					if(already_drawn)continue;

					// source == target at progress 1.0 draws in place, moved only by mirror_shift.
					render_proxyst proxy=
						{
						visual_layer,
						float(x),
						float(y),
						x,
						y,
						texpos,
						1.0f,
						nullptr,
						true,
						mirrored_tile_x(x,anchor_x)-x,
						{}
						};
					// The sprite lands on x+mirror_shift, so that interval must be repaintable.
					// The shift has either sign, so order the interval ends first.
					const int32_t coverage_first=std::min(x,x+proxy.mirror_shift);
					const int32_t coverage_last=std::max(x,x+proxy.mirror_shift);
					bool blocked=false;
					for(int32_t coverage_x=coverage_first;
						coverage_x<=coverage_last;++coverage_x)
						{
						if(!inside_clip(vp,coverage_x,y)||
							(group==visual_render_groupst::main&&
							has_fire(vp,coverage_x,y)))
							{
							blocked=true;
							break;
							}
						proxy.coverage.emplace(coverage_x,y);
						}
					if(blocked)continue;

					proxy.texture=cached_texture(renderer,texpos);
					if(proxy.texture==nullptr)continue;
					proxies.push_back(std::move(proxy));
					}
				}
			}
		}
	return proxies;
}

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
		auto &group=coverage.groups[static_cast<size_t>(visual_render_group(proxy.layer))];
		group.insert(proxy.coverage.begin(),proxy.coverage.end());
		}
	return coverage;
}

std::vector<df::graphic_viewportst *> active_viewports()
{
	std::vector<df::graphic_viewportst *> viewports;
	if(gps==nullptr)return viewports;
	for(int32_t lower=7;lower>=0;--lower)
		{
		df::graphic_viewportst *vp=gps->lower_viewport[lower];
		if(viewport_readable(vp))viewports.push_back(vp);
		}
	if(viewport_readable(gps->main_viewport))
		viewports.push_back(gps->main_viewport);
	return viewports;
}

std::vector<viewport_renderst> collect_viewport_renders(
	df::renderer_2d_base *renderer,
	const std::vector<df::graphic_viewportst *> &viewports)
{
	std::vector<viewport_renderst> renders;
	renders.reserve(viewports.size());
	for(df::graphic_viewportst *vp:viewports)
		{
		viewport_renderst render={vp,collect_proxies(renderer,vp),{}};
		render.coverage=collect_coverage(render.proxies,vp->dim_y);
		renders.push_back(std::move(render));
		}
	return renders;
}

tile_coveragest collect_viewport_coverage(
	const std::vector<viewport_renderst> &viewports)
{
	tile_coveragest coverage;
	for(const viewport_renderst &viewport:viewports)
		coverage.insert(
			viewport.coverage.all.begin(),viewport.coverage.all.end());
	return coverage;
}

void draw_interpolation_stages(
	df::renderer_2d_base *renderer,
	df::graphic_viewportst *vp,
	const std::vector<render_proxyst> &proxies,
	const render_coveragest &coverage)
{
	for(size_t index=0;index<coverage.groups.size();++index)
		{
		const auto group=static_cast<visual_render_groupst>(index);
		for(const render_proxyst &proxy:proxies)
			if(visual_render_group(proxy.layer)==group)draw_proxy(renderer,proxy);
		if(group==visual_render_groupst::designation)continue;
		for(const auto &[x,y]:coverage.groups[index])
			redraw_above(renderer,vp,x,y,group,coverage.selected);
		}
}

void redraw_viewport_tiles(
	df::renderer_2d_base *renderer,
	const viewport_renderst &viewport,
	const tile_coveragest &coverage)
{
	df::graphic_viewportst *vp=viewport.viewport;
	for(const auto &[x,y]:coverage)
		{
		if(!inside_clip(vp,x,y))continue;
		redraw_viewport_tile(renderer,viewport,x,y,true);
		}
}

void draw_viewport_interpolation_stages(
	df::renderer_2d_base *renderer,
	const std::vector<viewport_renderst> &viewports,
	const tile_coveragest &coverage,
	const std::vector<carried_item_proxyst> &carried_items)
{
	for(size_t index=0;index<viewports.size();++index)
		{
		// A lower z-level's proxy must be covered by the next viewport's fog and terrain.
		// Reapply that viewport before its own proxies, matching DF's lower-to-main draw order.
		if(index>0)redraw_viewport_tiles(renderer,viewports[index],coverage);
		const viewport_renderst &viewport=viewports[index];
		draw_interpolation_stages(
			renderer,viewport.viewport,viewport.proxies,viewport.coverage);
		if(index+1==viewports.size())
			for(const carried_item_proxyst &proxy:carried_items)
				draw_carried_item_proxy(renderer,proxy);
		// A viewport shades everything drawn beneath it, so this covers every staged tile.
		// Restricting it to the tiles this viewport has sprites on would not deepen with distance.
		for(const auto &[x,y]:coverage)
			{
			if(inside_clip(viewport.viewport,x,y))
				draw_interface_only(renderer,viewport.viewport,x,y);
			}
		}
}

bool has_mirrored_viewport_facing(
	const std::vector<df::graphic_viewportst *> &viewports)
{
	for(const df::graphic_viewportst *vp:viewports)
		if(animation_manager.has_mirrored_facing(vp))return true;
	return false;
}

void render_interpolated_world(df::renderer_2d_base *renderer)
{
	frame_stats.frames.fetch_add(1,std::memory_order_relaxed);
	// Read once: if the console flipped the flag on mid-frame, the guard would subtract a
	// start time of zero.
	const bool timing_enabled=frame_stats.enabled.load(std::memory_order_relaxed);
	const uint64_t frame_start_us=timing_enabled?frame_statsst::now_us():0;
	uint64_t sync_end_us=frame_start_us;
	// Runs on every exit, including the early return for frames with nothing to draw.
	const scope_guardst timing([&]
		{
		if(!timing_enabled)return;
		const uint64_t end_us=frame_statsst::now_us();
		frame_stats.timed.fetch_add(1,std::memory_order_relaxed);
		frame_stats.add_sync(sync_end_us-frame_start_us);
		frame_stats.add_render(end_us-sync_end_us);
		});
	df::graphic_viewportst *vp=gps?gps->main_viewport:nullptr;
	const std::vector<df::graphic_viewportst *> viewports=active_viewports();

	if(vp!=nullptr)update_visual_context(renderer,vp);
	const int32_t follow_id=plotinfo?plotinfo->follow_unit:-1;
	if(native_follow_changed(native_follow_id,follow_id))
		{
		native_follow_id=follow_id;
		++visual_context_revision;
		previous_coverage.clear();
		cancel_camera_transients();
		camera_has_prev=false;
		}
	const uint32_t now_ms=Core::getInstance().p->getTickCount();
	animation_manager.begin_frame(now_ms);
	for(df::graphic_viewportst *viewport:viewports)
		animation_manager.synchronize_viewport(animation_input(viewport));
	animation_manager.end_frame();
	if(timing_enabled)sync_end_us=frame_statsst::now_us();

	if(!viewport_readable(vp)||renderer->sdl_renderer==nullptr)
		return;
	const bool paused=pause_state&&*pause_state;
	if(paused)
		{
		cancel_camera_transients();
		camera_has_prev=false;
		}
	const bool native_follow_active=follow_id>=0;
	if(!paused)
		update_camera(renderer,vp,animation_manager.get_frame_delta_ms(),native_follow_active);
	const double cam_tile=tile_px(renderer);
	const int32_t glide_x=int32_t(std::lround(
		transient_x+camera_follow_x+rest_x*cam_tile));
	const int32_t glide_y=int32_t(std::lround(
		transient_y+camera_follow_y+rest_y*cam_tile));
	const bool glide=glide_x!=0||glide_y!=0;
	if(!glide&&camera_was_offset)
		{
		// The camera just re-joined the grid: one engine redraw replaces the last shifted frame.
		camera_was_offset=false;
		if(gps!=nullptr)++gps->force_full_display_count;
		}
	if(glide)camera_was_offset=true;
	std::vector<carried_item_proxyst> carried_items=
		hauled_enabled?collect_carried_item_proxies(renderer,vp):
		std::vector<carried_item_proxyst>{};
	if(!glide&&!animation_manager.requires_full_redraw()&&
		(!flip_enabled||!has_mirrored_viewport_facing(viewports))&&
		carried_items.empty()&&previous_coverage.empty())
		return;
	frame_stats.painted.fetch_add(1,std::memory_order_relaxed);

	std::vector<viewport_renderst> viewport_renders=
		collect_viewport_renders(renderer,viewports);
	tile_coveragest coverage=collect_viewport_coverage(viewport_renders);
	for(const carried_item_proxyst &proxy:carried_items)
		coverage.insert(proxy.coverage.begin(),proxy.coverage.end());

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
				redraw_world_tile(renderer,viewport_renders,coverage,x,y);
			}
		draw_viewport_interpolation_stages(
			renderer,viewport_renders,coverage,carried_items);
		renderer->origin_x=saved_origin_x;
		renderer->origin_y=saved_origin_y;
		render_set_clip_rect(sdl_renderer,nullptr);

		// Everything was repainted; per-tile coverage bookkeeping restarts after the glide.
		previous_coverage.clear();
		return;
		}

	tile_coveragest redraw_coverage=coverage;
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
		if(inside_clip(vp,x,y))
			redraw_world_tile(renderer,viewport_renders,coverage,x,y);
		}
	draw_viewport_interpolation_stages(
		renderer,viewport_renders,coverage,carried_items);

	previous_coverage=std::move(coverage);
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
	render_copy_f=nullptr;
	render_copy_ex_f=nullptr;
	render_fill_rect=nullptr;
	render_set_clip_rect=nullptr;
	get_render_draw_color=nullptr;
	set_render_draw_color=nullptr;
}

bool load_sdl(color_ostream &out)
{
	clear_sdl_bindings();
	DFLibrary *sdl_handle=DFSDL::obtain_library_handle();
	#define bind(name,target) \
		target=reinterpret_cast<decltype(target)>(LookupPlugin(sdl_handle,#name)); \
		if(target==nullptr) { \
			out.printerr("smooth-movement: SDL2 function unavailable: " #name "\n"); \
			clear_sdl_bindings(); \
			return false; \
		}
	bind(SDL_RenderCopyF,render_copy_f);
	bind(SDL_RenderCopyExF,render_copy_ex_f);
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
	native_follow_id=-1;
	flip_enabled=false;
	hauled_enabled=false;
	frame_stats.enabled=false;
	frame_stats.clear();
}

void print_frame_stats(color_ostream &out)
{
	const auto get=[](const std::atomic<uint64_t> &v){return v.load(std::memory_order_relaxed);};
	const uint64_t frames=get(frame_stats.frames),painted=get(frame_stats.painted),
		repaints=get(frame_stats.repaints),timed=get(frame_stats.timed),
		sync_us=get(frame_stats.sync_us),render_us=get(frame_stats.render_us);
	out.print("frame stats: {}\n",frame_stats.enabled?"on":"off");
	if(frames==0)
		{
		out.print("no frames counted\n");
		return;
		}
	const auto mean=[](uint64_t total,uint64_t count)
		{
		return count==0?0.0:double(total)/double(count);
		};
	out.print("frames: {} ({} painted, {:.0f}%)\n",
		frames,painted,100.0*mean(painted,frames));
	out.print("engine tile repaints: {} ({:.1f} per frame, {:.1f} per painted frame)\n",
		repaints,mean(repaints,frames),mean(repaints,painted));
	if(timed==0)return;
	out.print("timed frames: {}\n",timed);
	out.print("sync: mean {:.0f} us, max {} us\n",
		mean(sync_us,timed),get(frame_stats.sync_max_us));
	out.print("render: mean {:.0f} us, max {} us\n",
		mean(render_us,timed),get(frame_stats.render_max_us));
	out.print("total: mean {:.0f} us per timed frame\n",
		mean(sync_us+render_us,timed));
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
		out.print("sprite flipping: {}\n",
			flip_enabled?"on":"off");
		out.print("linear movement: {}\n",
			animation_manager.is_linear()?"on":"off");
		out.print("hauled item icons: {}\n",
			hauled_enabled?"on":"off");
		out.print("frame stats: {}\n",
			frame_stats.enabled?"on":"off");
		return CR_OK;
		}
	if(parameters[0]=="stats")
		{
		if(parameters.size()==1)
			{
			print_frame_stats(out);
			return CR_OK;
			}
		if(parameters.size()==2&&
			(parameters[1]=="on"||parameters[1]=="off"))
			{
			const bool on=parameters[1]=="on";
			if(on)frame_stats.clear();
			frame_stats.enabled=on;
			out.print("smooth-movement: frame stats {}\n",parameters[1]);
			return CR_OK;
			}
		if(parameters.size()==2&&parameters[1]=="reset")
			{
			frame_stats.clear();
			out.print("smooth-movement: frame stats reset\n");
			return CR_OK;
			}
		return CR_WRONG_USAGE;
		}
	if(parameters[0]=="all")
		{
		if(parameters.size()!=2||
			(parameters[1]!="on"&&parameters[1]!="off"))return CR_WRONG_USAGE;
		const bool enabled=parameters[1]=="on";
		flip_enabled=enabled;
		animation_manager.set_linear(enabled);
		hauled_enabled=enabled;
		if(gps!=nullptr)++gps->force_full_display_count;
		out.print("smooth-movement: flip, linear and hauled {}\n",parameters[1]);
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
	if(parameters[0]=="flip")
		{
		if(parameters.size()==1)
			{
			out.print("sprite flipping: {}\n",
				flip_enabled?"on":"off");
			return CR_OK;
			}
		// A toggle changes the screen without changing anything DF knows, so DF will not repaint.
		// OFF matters most: the render path stops touching tiles it painted every frame.
		// The last mirrored frame would persist.
		// Same flush plugin_enable(false) uses.
		if(parameters.size()==2&&parameters[1]=="on")
			{
			flip_enabled=true;
			if(gps!=nullptr)++gps->force_full_display_count;
			out.print("smooth-movement: sprite flipping enabled\n");
			return CR_OK;
			}
		if(parameters.size()==2&&parameters[1]=="off")
			{
			flip_enabled=false;
			if(gps!=nullptr)++gps->force_full_display_count;
			out.print("smooth-movement: sprite flipping disabled\n");
			return CR_OK;
			}
		return CR_WRONG_USAGE;
		}
	if(parameters[0]=="linear")
		{
		if(parameters.size()==1)
			{
			out.print("linear movement: {}\n",
				animation_manager.is_linear()?"on":"off");
			return CR_OK;
			}
		if(parameters.size()==2&&
			(parameters[1]=="on"||parameters[1]=="off"))
			{
			animation_manager.set_linear(parameters[1]=="on");
			out.print("smooth-movement: linear movement {}\n",parameters[1]);
			return CR_OK;
			}
		return CR_WRONG_USAGE;
		}
	if(parameters[0]=="hauled")
		{
		if(parameters.size()==1)
			{
			out.print("hauled item icons: {}\n",hauled_enabled?"on":"off");
			return CR_OK;
			}
		if(parameters.size()==2&&
			(parameters[1]=="on"||parameters[1]=="off"))
			{
			hauled_enabled=parameters[1]=="on";
			if(gps!=nullptr)++gps->force_full_display_count;
			out.print("smooth-movement: hauled item icons {}\n",parameters[1]);
			return CR_OK;
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
		"Smooth movement status; free camera: camera on|off|reset|<fx> <fy>; "
		"flip, linear and hauled together: all on|off; "
		"sprite flipping: flip on|off; linear movement: linear on|off; "
		"hauled item icons: hauled on|off; "
		"frame timing: stats [on|off|reset].",
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
