// SPDX-License-Identifier: MIT

#include "Core.h"
#include "MemAccess.h"
#include "PluginManager.h"
#include "VTableInterpose.h"

#include "modules/DFSDL.h"
#include "modules/Maps.h"

#include "df/enabler.h"
#include "df/game_mode.h"
#include "df/graphic.h"
#include "df/graphic_viewportst.h"
#include "df/interface_key.h"
#include "df/renderer_2d_base.h"
#include "df/texture_fullid.h"
#include "df/viewscreen_dungeonmodest.h"
#include "df/viewscreen_dwarfmodest.h"

#include "TileTypes.h"
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
REQUIRE_GLOBAL(gamemode);
REQUIRE_GLOBAL(gps);
REQUIRE_GLOBAL(window_x);
REQUIRE_GLOBAL(window_y);
REQUIRE_GLOBAL(window_z);

namespace {

constexpr const char *plugin_version="0.4.0-dev";
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
decltype(&SDL_GetTextureAlphaMod) get_texture_alpha_mod=nullptr;
decltype(&SDL_SetTextureAlphaMod) set_texture_alpha_mod=nullptr;
decltype(&SDL_GetTextureBlendMode) get_texture_blend_mode=nullptr;
decltype(&SDL_SetTextureBlendMode) set_texture_blend_mode=nullptr;

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
bool construction_transitions_enabled=false;

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

bool adventure_mode()
{
	return gamemode!=nullptr&&*gamemode==df::game_mode::ADVENTURE;
}

// --- world-tile slide ---------------------------------------------------------------------------
// Adventure mode's smoothing: the camera stays locked to the character and the WORLD TILES glide.
// When a scroll lands, the content jumped by -shift tiles on screen; the slide draws the world at
// a decaying pixel offset (same 100 ms smoothstep as creature movement, so world-anchored sprites
// and terrain stay in lockstep) while camera-carried sprites -- the player, screen-static across
// the landing, reported by the manager as "pinned" -- draw WITHOUT the offset. Active whenever the
// fortress free camera is not (the camera owns fort-mode smoothing when enabled).
constexpr double slide_max_tiles=4.0;         // beyond this the view just steps (fast scrolling)
uint32_t slide_duration_ms=100;               // debug-tunable: `smooth-movement slidems <n>`
double slide_from_x=0.0;                      // pixel offset at slide start
double slide_from_y=0.0;
uint32_t slide_start_ms=0;
bool slide_active=false;
uint64_t slide_resets_seen=0;                 // manager resets cancel the slide
uint64_t slide_revision_seen=0;               // context changes cancel the slide
int32_t slide_drag_cooldown=0;                // frames after a drag whose landings don't slide

uint64_t stat_glide_frames=0;   // frames the world was drawn at a sub-tile offset
uint64_t stat_mouse_shifts=0;   // input dispatches corrected for the visual offset
uint64_t stat_visual_jumps=0;   // frames the drawn world moved > 0.6 tile at once
double prev_visual_x=0.0;
double prev_visual_y=0.0;
bool has_prev_visual=false;
uint64_t prev_visual_revision=0;
float last_jump_x=0.0f;
float last_jump_y=0.0f;
uint32_t last_sig_diff=0;       // bitmask: context-signature fields changed this frame

double slide_offset_now(uint32_t now_ms,double from)
{
	if(!slide_active)return 0.0;
	const double t=std::min(1.0,double(now_ms-slide_start_ms)/double(slide_duration_ms));
	const double ease=1.0-t*t*(3.0-2.0*t);
	return from*ease;
}

// Per-frame diagnostic trace (dump with `smooth-movement trace`).
struct trace_framest
{
	uint32_t frame;
	int32_t wx,wy,wz;
	uint32_t sig_diff;
	uint64_t revision;
	uint32_t landings,static_f,identical,unrecognized,absorbed,resets,movements,suppressed,pans;
	int32_t pending_dx,pending_dy;
	float slide_px_x,slide_px_y;
	float jump_x,jump_y;
};
constexpr size_t trace_capacity=2700;
std::array<trace_framest,trace_capacity> trace_ring{};
uint32_t trace_frames=0;

int32_t tile_pixel(int32_t tile,int32_t origin,int32_t zoom);

// --- outgoing-tile cache -------------------------------------------------------------------------
// While the world (or the fort camera) glides, the trailing screen edge shows tiles that already
// scrolled OUT of the viewport buffers; without help they render black until the glide lands. At
// each landing the pre-scroll frame still sits in the engine's *_old buffers, so a padded cache in
// the CURRENT window's coordinate frame is rebuilt from them, chaining the previous cache so
// stacked landings keep up to `strip_pad` tiles of departed world alive.
constexpr int32_t strip_pad=int32_t(slide_max_tiles);
constexpr size_t strip_layer_count=10;
struct strip_cachest
{
	int32_t dim_x=0;
	int32_t dim_y=0;
	bool valid=false;
	std::array<std::vector<int32_t>,strip_layer_count> layers;
};
strip_cachest strip_caches[2];
int strip_cache_front=0;
uint64_t stat_strip_draws=0;
uint64_t stat_strip_misses=0;
// The engine's tile cache is keyed by (texpos, tint colors, flags) and terrain tints vary per
// tile, so an exact-recipe lookup misses. Departed tiles were just on screen, so SOME recipe for
// their texpos is cached: remember which full key worked per texpos and re-find it fresh each
// draw (never holding the SDL texture across frames). Flushed on context changes.
std::unordered_map<int32_t,df::texture_fullid> strip_recipe_memo;

int32_t strip_index(const strip_cachest &cache,int32_t x,int32_t y)
{
	return (x+strip_pad)*(cache.dim_y+2*strip_pad)+(y+strip_pad);
}

std::array<const int32_t *,strip_layer_count> strip_old_layers(df::graphic_viewportst *vp)
{
	return {
		vp->screentexpos_background_old,
		vp->screentexpos_background_two_old,
		vp->screentexpos_building_one_old,
		vp->screentexpos_item_old,
		vp->screentexpos_vehicle_old,
		vp->screentexpos_vermin_old,
		vp->screentexpos_right_creature_old,
		vp->screentexpos_old,
		vp->screentexpos_left_creature_old,
		vp->screentexpos_building_two_old
		};
}

void strip_cache_invalidate()
{
	strip_caches[0].valid=false;
	strip_caches[1].valid=false;
}

void strip_cache_update(df::graphic_viewportst *vp,int32_t shift_x,int32_t shift_y)
{
	strip_cachest &prev=strip_caches[strip_cache_front];
	strip_cachest &next=strip_caches[1-strip_cache_front];
	next.dim_x=vp->dim_x;
	next.dim_y=vp->dim_y;
	const size_t cells=size_t(vp->dim_x+2*strip_pad)*size_t(vp->dim_y+2*strip_pad);
	const bool chain=prev.valid&&prev.dim_x==vp->dim_x&&prev.dim_y==vp->dim_y;
	const auto old_layers=strip_old_layers(vp);
	for(size_t layer=0;layer<strip_layer_count;++layer)
		{
		next.layers[layer].assign(cells,0);
		for(int32_t x=-strip_pad;x<vp->dim_x+strip_pad;++x)
			{
			for(int32_t y=-strip_pad;y<vp->dim_y+strip_pad;++y)
				{
				const int32_t sx=x+shift_x;
				const int32_t sy=y+shift_y;
				int32_t value=0;
				if(sx>=0&&sx<vp->dim_x&&sy>=0&&sy<vp->dim_y)
					value=old_layers[layer][sx*vp->dim_y+sy];
				else if(chain&&sx>=-strip_pad&&sx<vp->dim_x+strip_pad&&
					sy>=-strip_pad&&sy<vp->dim_y+strip_pad)
					value=prev.layers[layer][strip_index(prev,sx,sy)];
				next.layers[layer][strip_index(next,x,y)]=value;
				}
			}
		}
	next.valid=true;
	strip_cache_front=1-strip_cache_front;
}

void draw_strip_tile(
	df::renderer_2d_base *renderer,
	SDL_Renderer *sdl_renderer,
	const strip_cachest &cache,
	int32_t x,
	int32_t y,
	int32_t zoom,
	int32_t tile_size)
{
	const int32_t index=strip_index(cache,x,y);
	for(size_t layer=0;layer<strip_layer_count;++layer)
		{
		const int32_t texpos=cache.layers[layer][index];
		if(texpos==0)continue;
		auto &tiles=renderer->tile_cache.tile_cache;
		auto texture=tiles.end();
		const auto memo=strip_recipe_memo.find(texpos);
		if(memo!=strip_recipe_memo.end())
			{
			texture=tiles.find(memo->second);
			if(texture==tiles.end())strip_recipe_memo.erase(texpos);
			}
		if(texture==tiles.end())
			{
			for(auto it=tiles.begin();it!=tiles.end();++it)
				{
				if(it->first.texpos==texpos)
					{
					texture=it;
					strip_recipe_memo[texpos]=it->first;
					break;
					}
				}
			}
		if(texture==tiles.end())
			{
			++stat_strip_misses;
			continue;
			}
		const SDL_FRect destination=
			{
			float(tile_pixel(x,renderer->origin_x,zoom)),
			float(tile_pixel(y,renderer->origin_y,zoom)),
			float(tile_size),
			float(tile_size)
			};
		render_copy_f(
			sdl_renderer,
			static_cast<SDL_Texture *>(texture->second),
			nullptr,
			&destination);
		++stat_strip_draws;
		}
}

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
	if(!camera_enabled||adventure_mode())return;   // adventure smoothing = the world slide
	const double tile=tile_px(renderer);
	const int32_t wx=window_x?*window_x:0;
	const int32_t wy=window_y?*window_y:0;
	if(camera_has_prev&&(wx!=camera_prev_wx||wy!=camera_prev_wy))
		{
		const int32_t dx=wx-camera_prev_wx;
		const int32_t dy=wy-camera_prev_wy;
		if(std::abs(dx)>6||std::abs(dy)>6)
			{
			// Almost certainly a one-frame window excursion (combat/announcement camera
			// flick) whose reversal arrives a frame or two later: park it for the wait
			// below instead of cancelling.
			camera_pending_dx+=dx;
			camera_pending_dy+=dy;
			camera_pending_frames=0;
			}
		else if((std::abs(dx)>camera_max_glide_tiles||std::abs(dy)>camera_max_glide_tiles)&&
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
			// One-frame window excursion: the reversal usually collapses the delta on
			// its own -- attribute nothing meanwhile so the glide keeps decaying
			// untouched. A delta that never collapses is a genuine teleport: snap
			// (keep rest, drop the animation debt).
			if(++camera_pending_frames>4)
				{
				transient_x=0.0;
				transient_y=0.0;
				clear_camera_pending();
				}
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
				// Content demonstrably hasn't moved yet -- but a scroll masked by an
				// excursion frame never diffs, and a phantom delta never renders at all.
				// Absorb either after a grace period instead of poisoning attribution.
				if(++camera_pending_frames>6)clear_camera_pending();
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
	last_sig_diff=0;
	if(has_view_signature&&previous_viewport==vp)
		{
		for(size_t i=0;i<signature.size();++i)
			{
			if(previous_view_signature[i]!=signature[i])last_sig_diff|=1U<<i;
			}
		}
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
		window_x?*window_x:0,
		window_y?*window_y:0,
		// Scroll-landing oracle: lets movements that coincide with a camera scroll be
		// attributed (adventure mode -- the camera follows the player, EVERY step is one).
		vp->screentexpos_background,
		vp->screentexpos_background_old
		};
}

struct tile_transitionst
{
	tile_transition_layer layer;
	int32_t previous_texpos;
	int32_t current_texpos;
	uint32_t start_time_ms;
};

bool is_tile_construction_or_destruction(
	df::tiletype previous,
	df::tiletype current)
{
	if(previous==current)return false;
	if(tileMaterial(previous)==df::tiletype_material::CONSTRUCTION||
		tileMaterial(current)==df::tiletype_material::CONSTRUCTION)
		return true;
	if(!isGroundMaterial(previous)||
		tileShape(previous)==tileShape(current))return false;
	return isWallTerrain(previous)||
		isFloorTerrain(previous)||
		isRampTerrain(previous)||
		isStairTerrain(previous);
}

class tile_transition_managerst
{
	const void *viewport=nullptr;
	int32_t dim_x=0;
	int32_t dim_y=0;
	uint64_t context_revision=0;
	int32_t pan_x=0;
	int32_t pan_y=0;
	bool has_context=false;
	std::unordered_map<int32_t,tile_transitionst> transitions;
	std::vector<df::tiletype> previous_tiletypes;
	std::vector<uint8_t> has_previous_tiletype;

	static constexpr uint32_t transition_duration_ms=120;

	void snapshot_tiletypes(df::graphic_viewportst *vp)
		{
		const int32_t tile_count=vp->dim_x*vp->dim_y;
		previous_tiletypes.resize(tile_count);
		has_previous_tiletype.assign(tile_count,0);
		const int32_t map_x=window_x?*window_x:0;
		const int32_t map_y=window_y?*window_y:0;
		const int32_t map_z=window_z?*window_z:0;
		for(int32_t x=0;x<vp->dim_x;++x)
			{
			for(int32_t y=0;y<vp->dim_y;++y)
				{
				const int32_t index=x*vp->dim_y+y;
				const df::tiletype *tiletype=
					Maps::getTileType(map_x+x,map_y+y,map_z);
				if(tiletype==nullptr)continue;
				previous_tiletypes[index]=*tiletype;
				has_previous_tiletype[index]=1;
				}
			}
		}

	public:
		void clear()
			{
			transitions.clear();
			previous_tiletypes.clear();
			has_previous_tiletype.clear();
			has_context=false;
			}

		void reset()
			{
			clear();
			}

		void synchronize(df::graphic_viewportst *vp,uint32_t now_ms)
			{
			const int32_t current_pan_x=window_x?*window_x:0;
			const int32_t current_pan_y=window_y?*window_y:0;
			const bool context_changed=!has_context||viewport!=vp||
				dim_x!=vp->dim_x||dim_y!=vp->dim_y||
				context_revision!=visual_context_revision||
				pan_x!=current_pan_x||pan_y!=current_pan_y;
			viewport=vp;
			dim_x=vp->dim_x;
			dim_y=vp->dim_y;
			context_revision=visual_context_revision;
			pan_x=current_pan_x;
			pan_y=current_pan_y;
			has_context=true;
			if(context_changed)
				{
				transitions.clear();
				snapshot_tiletypes(vp);
				return;
				}

			const int32_t map_x=window_x?*window_x:0;
			const int32_t map_y=window_y?*window_y:0;
			const int32_t map_z=window_z?*window_z:0;
			for(int32_t x=0;x<dim_x;++x)
				{
				for(int32_t y=0;y<dim_y;++y)
					{
					const int32_t index=x*dim_y+y;
					const df::tiletype *tiletype=
						Maps::getTileType(map_x+x,map_y+y,map_z);
					const bool gameplay_change=tiletype!=nullptr&&
						has_previous_tiletype[index]&&
						is_tile_construction_or_destruction(
							previous_tiletypes[index],*tiletype);
					if(tiletype!=nullptr)
						{
						previous_tiletypes[index]=*tiletype;
						has_previous_tiletype[index]=1;
						}
					else
						has_previous_tiletype[index]=0;
					if(!gameplay_change)continue;
					const auto candidate=select_tile_transition(
						vp->screentexpos_background[index],
						vp->screentexpos_background_old[index],
						vp->screentexpos_building_one[index],
						vp->screentexpos_building_one_old[index]);
					if(candidate.layer!=tile_transition_layer::none)
						{
						const auto existing=transitions.find(index);
						if(existing==transitions.end()||
							existing->second.layer!=candidate.layer||
							existing->second.previous_texpos!=candidate.previous_texpos||
							existing->second.current_texpos!=candidate.current_texpos)
							{
							transitions[index]={
								candidate.layer,
								candidate.previous_texpos,
								candidate.current_texpos,
								now_ms
								};
							}
						}
					}
				}
			for(auto it=transitions.begin();it!=transitions.end();)
				{
				if(now_ms-it->second.start_time_ms>=transition_duration_ms)
					it=transitions.erase(it);
				else
					++it;
				}
			}

		bool active() const
			{
			return !transitions.empty();
			}

		const tile_transitionst *get(int32_t index) const
			{
			const auto found=transitions.find(index);
			return found==transitions.end()?nullptr:&found->second;
			}

		float progress(const tile_transitionst &transition,uint32_t now_ms) const
			{
			return animation_progress(
				now_ms,transition.start_time_ms,transition_duration_ms);
			}

		const std::unordered_map<int32_t,tile_transitionst> &entries() const
			{
			return transitions;
			}

};

tile_transition_managerst tile_transition_manager;

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

template<typename Callback>
void with_incoming_tile_suppressed(
	df::graphic_viewportst *vp,
	int32_t index,
	const Callback &callback)
{
	const tile_transitionst *transition=tile_transition_manager.get(index);
	if(transition==nullptr||transition->current_texpos==0)
		{
		callback();
		return;
		}
	if(transition->layer==tile_transition_layer::background)
		with_zeroed_values(callback,vp->screentexpos_background[index]);
	else
		with_zeroed_values(callback,vp->screentexpos_building_one[index]);
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
	with_incoming_tile_suppressed(vp,index,[&]
		{
		with_suppressed_visual_layers(
			visual_layers(vp),index,selected_mask(selected,index),redraw);
		});
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
	const auto redraw=[&]{renderer->update_viewport_tile(vp,x,y);};
	const auto suppress_visuals=[&]
		{
		with_suppressed_visual_layers(
			visual_layers(vp),
			index,
			selected_mask(selected,index)|visual_layers_through_group(group),
			redraw);
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

SDL_Texture *cached_tile_texture(
	df::renderer_2d_base *renderer,
	int32_t texpos)
{
	SDL_Texture *texture=cached_texture(renderer,texpos,false);
	return texture!=nullptr?texture:cached_texture(renderer,texpos);
}

void draw_texture_with_alpha(
	SDL_Renderer *renderer,
	SDL_Texture *texture,
	const SDL_FRect &destination,
	float opacity)
{
	if(opacity<=0.0f)return;
	Uint8 saved_alpha=255;
	SDL_BlendMode saved_blend=SDL_BLENDMODE_NONE;
	if(get_texture_alpha_mod(texture,&saved_alpha)!=0||
		get_texture_blend_mode(texture,&saved_blend)!=0)
		{
		render_copy_f(renderer,texture,nullptr,&destination);
		return;
		}
	set_texture_blend_mode(texture,SDL_BLENDMODE_BLEND);
	set_texture_alpha_mod(texture,Uint8(std::lround(saved_alpha*opacity)));
	render_copy_f(renderer,texture,nullptr,&destination);
	set_texture_alpha_mod(texture,saved_alpha);
	set_texture_blend_mode(texture,saved_blend);
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

				proxy.texture=cached_texture(renderer,texpos);
				if(proxy.texture==nullptr)continue;
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
	std::array<tile_coveragest,static_cast<size_t>(visual_render_groupst::count)> groups;
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
		auto &group=coverage.groups[static_cast<size_t>(visual_render_group(proxy.layer))];
		group.insert(proxy.coverage.begin(),proxy.coverage.end());
		}
	return coverage;
}

void add_tile_transition_coverage(
	render_coveragest &coverage,
	int32_t dim_y)
{
	for(const auto &entry:tile_transition_manager.entries())
		coverage.all.emplace(entry.first/dim_y,entry.first%dim_y);
}

void draw_tile_transitions(
	df::renderer_2d_base *renderer,
	df::graphic_viewportst *vp)
{
	SDL_Renderer *sdl_renderer=static_cast<SDL_Renderer *>(renderer->sdl_renderer);
	const int32_t zoom=renderer->viewport_zoom_factor;
	const int32_t tile_size=zoom==128?32:std::max(1,zoom*32/128);
	const uint32_t now_ms=animation_manager.get_frame_time_ms();
	for(const auto &[index,transition]:tile_transition_manager.entries())
		{
		const int32_t x=index/vp->dim_y;
		const int32_t y=index%vp->dim_y;
		if(!inside_clip(vp,x,y))continue;
		const int32_t target_x=tile_pixel(x,renderer->origin_x,zoom);
		const int32_t target_y=tile_pixel(y,renderer->origin_y,zoom);
		const float progress=tile_transition_manager.progress(transition,now_ms);
		const SDL_FRect destination=
			{
			float(target_x),
			float(target_y),
			float(tile_size),
			float(tile_size)
			};
		SDL_Texture *previous=cached_tile_texture(renderer,transition.previous_texpos);
		SDL_Texture *current=cached_tile_texture(renderer,transition.current_texpos);
		if(previous==nullptr&&current==nullptr)
			{
			continue;
			}
		if(previous!=nullptr)
			draw_texture_with_alpha(sdl_renderer,previous,destination,1.0f-progress);
		if(current!=nullptr)
			draw_texture_with_alpha(sdl_renderer,current,destination,progress);
		}
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

void render_interpolated_world(df::renderer_2d_base *renderer)
{
	df::graphic_viewportst *vp=gps?gps->main_viewport:nullptr;

	if(vp!=nullptr)update_visual_context(renderer,vp);
	const uint32_t now_ms=Core::getInstance().p->getTickCount();
	animation_manager.begin_frame(now_ms);
	if(vp!=nullptr)
		{
		const auto input=animation_input(vp);
		animation_manager.synchronize_viewport(
			input,
			vp->flag.bits.active);
		if(vp->flag.bits.active&&construction_transitions_enabled)
			tile_transition_manager.synchronize(vp,now_ms);
		else
			tile_transition_manager.clear();
		}
	else
		tile_transition_manager.clear();
	animation_manager.end_frame();

	if(vp==nullptr||!vp->flag.bits.active||renderer->sdl_renderer==nullptr)
		return;
	update_camera(renderer,vp,animation_manager.get_frame_delta_ms());
	const double cam_tile=tile_px(renderer);
	const bool camera_active=camera_enabled&&!adventure_mode();

	// --- world-tile slide (owns smoothing whenever the fort camera does not) ---
	if(animation_manager.stats.resets!=slide_resets_seen||
		visual_context_revision!=slide_revision_seen)
		{
		slide_active=false;   // unfollowable change: the world snaps to the grid
		strip_cache_invalidate();
		strip_recipe_memo.clear();
		}
	slide_resets_seen=animation_manager.stats.resets;
	slide_revision_seen=visual_context_revision;
	// A middle-mouse drag is direct manipulation: the slide sits out while the button is
	// held, plus a few frames after release (the drag's final window steps land late).
	const bool dragging=enabler!=nullptr&&enabler->mouse_mbut;
	if(dragging)
		{
		slide_active=false;
		slide_drag_cooldown=3;
		}
	else if(slide_drag_cooldown>0)--slide_drag_cooldown;
	double slide_now_x=slide_offset_now(now_ms,slide_from_x);
	double slide_now_y=slide_offset_now(now_ms,slide_from_y);
	if(slide_active&&now_ms-slide_start_ms>=slide_duration_ms)
		{
		slide_active=false;
		slide_now_x=0.0;
		slide_now_y=0.0;
		}
	if(animation_manager.stats.last_shift_x!=0||animation_manager.stats.last_shift_y!=0)
		{
		strip_cache_update(
			vp,
			animation_manager.stats.last_shift_x,
			animation_manager.stats.last_shift_y);
		if(!camera_active&&!dragging&&slide_drag_cooldown==0)
			{
			// A scroll landed: the content jumped by -shift tiles on screen. Start (or
			// retarget from the current fractional offset) a slide back to the grid.
			const double cap=slide_max_tiles*cam_tile;
			slide_from_x=std::clamp(
				slide_now_x+double(animation_manager.stats.last_shift_x)*cam_tile,
				-cap,cap);
			slide_from_y=std::clamp(
				slide_now_y+double(animation_manager.stats.last_shift_y)*cam_tile,
				-cap,cap);
			slide_start_ms=now_ms;
			slide_active=true;
			slide_now_x=slide_from_x;
			slide_now_y=slide_from_y;
			}
		}
	if(camera_active&&slide_active)
		{
		slide_active=false;
		slide_now_x=0.0;
		slide_now_y=0.0;
		}

	const int32_t glide_x=camera_active?
		int32_t(std::lround(transient_x+rest_x*cam_tile)):
		int32_t(std::lround(slide_now_x));
	const int32_t glide_y=camera_active?
		int32_t(std::lround(transient_y+rest_y*cam_tile)):
		int32_t(std::lround(slide_now_y));
	const bool glide=glide_x!=0||glide_y!=0;

	// Visual continuity check: the drawn world offset is -window*tile + glide. Any
	// one-frame move above 0.6 tile is a visible jump -- an intended snap or a bug.
	{
	const double visual_x=-double(window_x?*window_x:0)*cam_tile+double(glide_x);
	const double visual_y=-double(window_y?*window_y:0)*cam_tile+double(glide_y);
	last_jump_x=0.0f;
	last_jump_y=0.0f;
	if(has_prev_visual&&prev_visual_revision==visual_context_revision&&
		!dragging&&slide_drag_cooldown==0)
		{
		const double dvx=visual_x-prev_visual_x;
		const double dvy=visual_y-prev_visual_y;
		if(std::abs(dvx)>0.6*cam_tile||std::abs(dvy)>0.6*cam_tile)
			{
			++stat_visual_jumps;
			last_jump_x=float(dvx);
			last_jump_y=float(dvy);
			}
		}
	prev_visual_x=visual_x;
	prev_visual_y=visual_y;
	prev_visual_revision=visual_context_revision;
	has_prev_visual=true;
	}
	if(!glide&&camera_was_offset)
		{
		// The camera just re-joined the grid: one engine redraw replaces the last shifted frame.
		camera_was_offset=false;
		if(gps!=nullptr)++gps->force_full_display_count;
		}
	if(glide)camera_was_offset=true;
	if(!glide&&!animation_manager.requires_full_redraw()&&
		!tile_transition_manager.active())
		return;

	std::vector<render_proxyst> proxies=collect_proxies(renderer,vp);
	render_coveragest coverage=collect_coverage(proxies,vp->dim_y);
	add_tile_transition_coverage(coverage,vp->dim_y);

	SDL_Renderer *sdl_renderer=static_cast<SDL_Renderer *>(renderer->sdl_renderer);
	const int32_t zoom=renderer->viewport_zoom_factor;
	const int32_t tile_size=zoom==128?32:std::max(1,zoom*32/128);

	if(glide)
		{
		++stat_glide_frames;
		// Camera-carried sprites (screen-static across a landing: the adventure player and
		// its overlays) are masked out of the sliding repaint and drawn pinned afterwards.
		const auto &pinned=camera_active?
			visual_animation_managerst::empty_pinned:
			animation_manager.get_pinned(vp);
		for(const auto &pin:pinned)
			coverage.selected[pin.x*vp->dim_y+pin.y]|=visual_layer_bit(pin.layer);
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
		// Trailing edge: the band uncovered by the glide has no viewport data any more --
		// draw it from the outgoing-tile cache so departed world stays visible (the clip
		// rect confines any overdraw to the map).
		{
		const strip_cachest &strip=strip_caches[strip_cache_front];
		if(strip.valid&&strip.dim_x==vp->dim_x&&strip.dim_y==vp->dim_y)
			{
			const int32_t band_w=std::min(strip_pad,glide_x>0?(glide_x+tile_size-1)/tile_size:0);
			const int32_t band_e=std::min(strip_pad,glide_x<0?(-glide_x+tile_size-1)/tile_size:0);
			const int32_t band_n=std::min(strip_pad,glide_y>0?(glide_y+tile_size-1)/tile_size:0);
			const int32_t band_s=std::min(strip_pad,glide_y<0?(-glide_y+tile_size-1)/tile_size:0);
			for(int32_t x=vp->clipx[0]-band_w;x<=vp->clipx[1]+band_e;++x)
				{
				for(int32_t y=vp->clipy[0]-band_n;y<=vp->clipy[1]+band_s;++y)
					{
					if(x>=vp->clipx[0]&&x<=vp->clipx[1]&&
						y>=vp->clipy[0]&&y<=vp->clipy[1])continue;
					if(x<-strip_pad||x>=vp->dim_x+strip_pad||
						y<-strip_pad||y>=vp->dim_y+strip_pad)continue;
					draw_strip_tile(renderer,sdl_renderer,strip,x,y,zoom,tile_size);
					}
				}
			}
		}
		draw_tile_transitions(renderer,vp);
		draw_interpolation_stages(renderer,vp,proxies,coverage);
		renderer->origin_x=saved_origin_x;
		renderer->origin_y=saved_origin_y;
		// The pinned sprites render at their true screen tiles, WITHOUT the slide: the
		// world visibly moves beneath them.
		for(const auto &pin:pinned)
			{
			df::texture_fullid texture_id;
			texture_id.texpos=pin.texpos;
			texture_id.r=texture_id.g=texture_id.b=1.0f;
			texture_id.br=texture_id.bg=texture_id.bb=0.0f;
			texture_id.flag=df::texture_fullid_flag::mask_transparent_background;
			const auto texture=renderer->tile_cache.tile_cache.find(texture_id);
			if(texture==renderer->tile_cache.tile_cache.end())continue;
			const SDL_FRect destination=
				{
				float(tile_pixel(pin.x,renderer->origin_x,zoom)),
				float(tile_pixel(pin.y,renderer->origin_y,zoom)),
				float(tile_size),
				float(tile_size)
				};
			render_copy_f(
				sdl_renderer,
				static_cast<SDL_Texture *>(texture->second),
				nullptr,
				&destination);
			}
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
	draw_tile_transitions(renderer,vp);
	draw_interpolation_stages(renderer,vp,proxies,coverage);

	previous_coverage=std::move(coverage.all);
}

// --- mouse dispatch under a visual offset ------------------------------------------------------
// While the world is drawn at a visual offset (fort camera glide/rest, or the adventure world
// slide), DF's mouse->map-tile math still resolves precise_mouse against the snapped grid, so
// clicks dispatch to the tile under the grid instead of the tile the user sees. The map screens'
// input (and the engine's hover-highlight stage) run with precise_mouse shifted into the
// displayed frame, scoped and restored; the interface-grid mouse is untouched.
bool visual_mouse_shift(int32_t &shift_px_x,int32_t &shift_px_y)
{
	if(gps==nullptr)return false;
	const int32_t zoom=gps->viewport_zoom_factor;
	const double tile=double(zoom==128?32:std::max(1,zoom*32/128));
	if(camera_enabled&&!adventure_mode())
		{
		shift_px_x=int32_t(std::lround(transient_x+rest_x*tile));
		shift_px_y=int32_t(std::lround(transient_y+rest_y*tile));
		}
	else if(slide_active)
		{
		const uint32_t now_ms=Core::getInstance().p->getTickCount();
		shift_px_x=int32_t(std::lround(slide_offset_now(now_ms,slide_from_x)));
		shift_px_y=int32_t(std::lround(slide_offset_now(now_ms,slide_from_y)));
		}
	else
		return false;
	return shift_px_x!=0||shift_px_y!=0;
}

class scoped_mouse_shiftst
{
	int32_t saved_x=0;
	int32_t saved_y=0;
	bool active=false;

	public:
		scoped_mouse_shiftst()
			{
			int32_t shift_x=0;
			int32_t shift_y=0;
			if(!visual_mouse_shift(shift_x,shift_y))return;
			if(gps->precise_mouse_x<0||gps->precise_mouse_y<0)return;
			const df::graphic_viewportst *vp=gps->main_viewport;
			if(vp==nullptr)return;
			const int32_t zoom=gps->viewport_zoom_factor;
			const int32_t tile=zoom==128?32:std::max(1,zoom*32/128);
			saved_x=gps->precise_mouse_x;
			saved_y=gps->precise_mouse_y;
			gps->precise_mouse_x=std::clamp(saved_x-shift_x,0,vp->dim_x*tile-1);
			gps->precise_mouse_y=std::clamp(saved_y-shift_y,0,vp->dim_y*tile-1);
			active=true;
			}

		~scoped_mouse_shiftst()
			{
			if(!active)return;
			gps->precise_mouse_x=saved_x;
			gps->precise_mouse_y=saved_y;
			}

		bool applied() const
			{
			return active;
			}

		scoped_mouse_shiftst(const scoped_mouse_shiftst &)=delete;
		scoped_mouse_shiftst &operator=(const scoped_mouse_shiftst &)=delete;
};

struct adventure_input_hook : df::viewscreen_dungeonmodest
{
	typedef df::viewscreen_dungeonmodest interpose_base;
	DEFINE_VMETHOD_INTERPOSE(void,feed,(std::set<df::interface_key> *events));
	DEFINE_VMETHOD_INTERPOSE(void,logic,());
};

IMPLEMENT_VMETHOD_INTERPOSE(adventure_input_hook,feed);
IMPLEMENT_VMETHOD_INTERPOSE(adventure_input_hook,logic);

void adventure_input_hook::interpose_fn_feed(std::set<df::interface_key> *events)
{
	scoped_mouse_shiftst shifted;
	if(shifted.applied())++stat_mouse_shifts;
	INTERPOSE_NEXT(feed)(events);
}

void adventure_input_hook::interpose_fn_logic()
{
	scoped_mouse_shiftst shifted;
	INTERPOSE_NEXT(logic)();
}

struct fort_input_hook : df::viewscreen_dwarfmodest
{
	typedef df::viewscreen_dwarfmodest interpose_base;
	DEFINE_VMETHOD_INTERPOSE(void,feed,(std::set<df::interface_key> *events));
	DEFINE_VMETHOD_INTERPOSE(void,logic,());
};

IMPLEMENT_VMETHOD_INTERPOSE(fort_input_hook,feed);
IMPLEMENT_VMETHOD_INTERPOSE(fort_input_hook,logic);

void fort_input_hook::interpose_fn_feed(std::set<df::interface_key> *events)
{
	scoped_mouse_shiftst shifted;
	if(shifted.applied())++stat_mouse_shifts;
	INTERPOSE_NEXT(feed)(events);
}

void fort_input_hook::interpose_fn_logic()
{
	scoped_mouse_shiftst shifted;
	INTERPOSE_NEXT(logic)();
}

void record_trace_frame()
{
	trace_framest &rec=trace_ring[trace_frames%trace_capacity];
	++trace_frames;
	rec.frame=trace_frames;
	rec.wx=window_x?*window_x:0;
	rec.wy=window_y?*window_y:0;
	rec.wz=window_z?*window_z:0;
	rec.sig_diff=last_sig_diff;
	rec.revision=visual_context_revision;
	const auto &st=animation_manager.stats;
	rec.landings=uint32_t(st.landings);
	rec.static_f=uint32_t(st.static_frames);
	rec.identical=uint32_t(st.identical_frames);
	rec.unrecognized=uint32_t(st.unrecognized_frames);
	rec.absorbed=uint32_t(st.absorbed);
	rec.resets=uint32_t(st.resets);
	rec.movements=uint32_t(st.movements);
	rec.suppressed=uint32_t(st.suppressed);
	rec.pans=uint32_t(st.pan_frames);
	rec.pending_dx=st.last_pending_dx;
	rec.pending_dy=st.last_pending_dy;
	const uint32_t now_ms=Core::getInstance().p->getTickCount();
	rec.slide_px_x=float(slide_offset_now(now_ms,slide_from_x));
	rec.slide_px_y=float(slide_offset_now(now_ms,slide_from_y));
	rec.jump_x=last_jump_x;
	rec.jump_y=last_jump_y;
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
	record_trace_frame();
	// The UI stage computes the hover highlight from the mouse; shift it into the
	// displayed frame too, so the highlighted tile is the one a click dispatches to.
	scoped_mouse_shiftst shifted;
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
	get_texture_alpha_mod=nullptr;
	set_texture_alpha_mod=nullptr;
	get_texture_blend_mode=nullptr;
	set_texture_blend_mode=nullptr;
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
	bind(SDL_GetTextureAlphaMod,get_texture_alpha_mod);
	bind(SDL_SetTextureAlphaMod,set_texture_alpha_mod);
	bind(SDL_GetTextureBlendMode,get_texture_blend_mode);
	bind(SDL_SetTextureBlendMode,set_texture_blend_mode);
	#undef bind
	return true;
}

void reset_state()
{
	slide_from_x=0.0;
	slide_from_y=0.0;
	slide_start_ms=0;
	slide_active=false;
	slide_resets_seen=0;
	slide_revision_seen=0;
	slide_drag_cooldown=0;
	strip_cache_invalidate();
	strip_recipe_memo.clear();
	stat_glide_frames=0;
	stat_mouse_shifts=0;
	stat_visual_jumps=0;
	stat_strip_draws=0;
	stat_strip_misses=0;
	has_prev_visual=false;
	trace_frames=0;
	last_sig_diff=0;
	animation_manager=visual_animation_managerst();
	tile_transition_manager.reset();
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
	construction_transitions_enabled=false;
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
		out.print("free camera (fortress mode): {}, offset {:.3f} {:.3f}"
			" (tiles east/south of the grid)\n",
			camera_enabled?"on":"off",-rest_x,-rest_y);
		{
		const uint32_t now_ms=Core::getInstance().p->getTickCount();
		out.print("world slide (adventure mode): {}, offset {:.1f} {:.1f} px\n",
			slide_active?"active":"idle",
			slide_offset_now(now_ms,slide_from_x),
			slide_offset_now(now_ms,slide_from_y));
		}
		out.print("construction transitions: {}\n",
			construction_transitions_enabled?"on":"off");
		out.print(
			"stats: movements {} landings {} suppressed {} resets {}\n",
			animation_manager.stats.movements,
			animation_manager.stats.landings,
			animation_manager.stats.suppressed,
			animation_manager.stats.resets);
		out.print("glide frames: {}, shifted input dispatches: {}, visual jumps: {}\n",
			stat_glide_frames,stat_mouse_shifts,stat_visual_jumps);
		out.print("strip: draws {} cache-misses {}\n",stat_strip_draws,stat_strip_misses);
		out.print(
			"scroll: pans {} static {} identical {} unrecognized {} absorbed {}"
			" pending {} {}\n",
			animation_manager.stats.pan_frames,
			animation_manager.stats.static_frames,
			animation_manager.stats.identical_frames,
			animation_manager.stats.unrecognized_frames,
			animation_manager.stats.absorbed,
			animation_manager.stats.last_pending_dx,
			animation_manager.stats.last_pending_dy);
		return CR_OK;
		}
	if(parameters[0]=="slidems"&&parameters.size()==2)
		{
		// Debug: stretch the slide to make its rendering inspectable by eye/screenshot.
		try
			{
			slide_duration_ms=uint32_t(std::stoul(parameters[1]));
			return CR_OK;
			}
		catch(...)
			{
			return CR_WRONG_USAGE;
			}
		}
	if(parameters[0]=="trace")
		{
		static const char *sig_names[12]=
			{"z","dimx","dimy","cx0","cx1","cy0","cy1","zoom","ox","oy","gdimx","gdimy"};
		const uint32_t have=trace_frames<trace_capacity?trace_frames:uint32_t(trace_capacity);
		const trace_framest *prev=nullptr;
		uint32_t printed=0;
		for(uint32_t i=0;i<have;++i)
			{
			const trace_framest &rec=
				trace_ring[(trace_frames-have+i)%trace_capacity];
			bool interesting=rec.jump_x!=0.0f||rec.jump_y!=0.0f||rec.sig_diff!=0||
				rec.pending_dx!=0||rec.pending_dy!=0||
				std::abs(rec.slide_px_x)>0.5f||std::abs(rec.slide_px_y)>0.5f;
			std::string delta;
			if(prev!=nullptr)
				{
				if(rec.wx!=prev->wx||rec.wy!=prev->wy||rec.wz!=prev->wz)interesting=true;
				const auto d=[&](uint32_t now,uint32_t before,const char *tag)
					{
					if(now!=before)
						delta+=std::string(" ")+tag+"+"+std::to_string(now-before);
					};
				d(rec.pans,prev->pans,"pan");
				d(rec.landings,prev->landings,"land");
				d(rec.static_f,prev->static_f,"stat");
				d(rec.identical,prev->identical,"ident");
				d(rec.unrecognized,prev->unrecognized,"unrec");
				d(rec.absorbed,prev->absorbed,"absorb");
				d(rec.resets,prev->resets,"reset");
				d(rec.movements,prev->movements,"mov");
				d(rec.suppressed,prev->suppressed,"sup");
				if(rec.revision!=prev->revision)
					delta+=" REV+"+std::to_string(rec.revision-prev->revision);
				if(!delta.empty())interesting=true;
				}
			if(interesting)
				{
				std::string sig;
				for(uint32_t b=0;b<12;++b)
					{
					if(rec.sig_diff&(1U<<b))
						sig+=std::string(sig.empty()?"":"|")+sig_names[b];
					}
				std::string jump;
				if(rec.jump_x!=0.0f||rec.jump_y!=0.0f)
					jump=" JUMP="+std::to_string(int(rec.jump_x))+","+
						std::to_string(int(rec.jump_y));
				out.print(
					"f={} w={},{},{}{}{}{} pend={},{} slide={:.1f},{:.1f}\n",
					rec.frame,rec.wx,rec.wy,rec.wz,
					sig.empty()?"":(" sig="+sig),
					delta,
					jump,
					rec.pending_dx,rec.pending_dy,
					rec.slide_px_x,rec.slide_px_y);
				++printed;
				}
			prev=&rec;
			}
		out.print("({} of {} frames interesting)\n",printed,have);
		return CR_OK;
		}
	if(parameters[0]=="construction")
		{
		if(parameters.size()==1)
			{
			out.print("construction transitions: {}\n",
				construction_transitions_enabled?"on":"off");
			return CR_OK;
			}
		if(parameters.size()==2&&parameters[1]=="on")
			{
			construction_transitions_enabled=true;
			return CR_OK;
			}
		if(parameters.size()==2&&parameters[1]=="off")
			{
			construction_transitions_enabled=false;
			tile_transition_manager.clear();
			return CR_OK;
			}
		return CR_WRONG_USAGE;
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
			if(adventure_mode())
				out.print("note: adventure mode uses the world slide; the free camera"
					" applies in fortress mode.\n");
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
		"Smooth movement status; camera on|off|reset|<fx> <fy>; construction on|off; trace; slidems <n>.",
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
		if(!INTERPOSE_HOOK(renderer_hook,update_all).apply()||
			!INTERPOSE_HOOK(adventure_input_hook,feed).apply()||
			!INTERPOSE_HOOK(adventure_input_hook,logic).apply()||
			!INTERPOSE_HOOK(fort_input_hook,feed).apply()||
			!INTERPOSE_HOOK(fort_input_hook,logic).apply())
			{
			out.printerr("smooth-movement: could not hook the renderer/input\n");
			INTERPOSE_HOOK(renderer_hook,update_all).remove();
			INTERPOSE_HOOK(adventure_input_hook,feed).remove();
			INTERPOSE_HOOK(adventure_input_hook,logic).remove();
			INTERPOSE_HOOK(fort_input_hook,feed).remove();
			INTERPOSE_HOOK(fort_input_hook,logic).remove();
			clear_sdl_bindings();
			return CR_FAILURE;
			}
		}
	else
		{
		INTERPOSE_HOOK(renderer_hook,update_all).remove();
		INTERPOSE_HOOK(adventure_input_hook,feed).remove();
		INTERPOSE_HOOK(adventure_input_hook,logic).remove();
		INTERPOSE_HOOK(fort_input_hook,feed).remove();
		INTERPOSE_HOOK(fort_input_hook,logic).remove();
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
