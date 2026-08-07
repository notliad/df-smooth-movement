// SPDX-License-Identifier: MIT

#include "Core.h"
#include "MemAccess.h"
#include "PluginManager.h"
#include "VTableInterpose.h"

#include "modules/DFSDL.h"

#include "df/enabler.h"
#include "df/game_mode.h"
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
REQUIRE_GLOBAL(gamemode);
REQUIRE_GLOBAL(gps);
REQUIRE_GLOBAL(window_x);
REQUIRE_GLOBAL(window_y);
REQUIRE_GLOBAL(window_z);

namespace {

constexpr const char *plugin_version="0.4.0-dev";

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

// --- free camera -------------------------------------------------------------------------------
// The camera is visually unbound from the tile grid. Two layered offsets:
//   rest      -- a PERSISTENT sub-tile offset in tiles: the free camera. Set only by the
//                `smooth-movement camera <fx> <fy>` console command. Survives zoom
//                and z-level changes. Kept in [-0.5,0.5] by normalization: whole-tile parts are
//                folded into window_x/window_y (a plain UI scroll write -- NEVER the viewport
//                dims, which crash DF; the sub-tile strip this leaves at one screen edge has no
//                buffer data and stays black).
//   transient -- the decaying scroll glide from before, in pixels, layered on top.
// Render offset = transient + rest*tile. window_x/window_y remain the game's own tile camera.
bool camera_enabled=false;                    // OFF by default: plain `enable smooth-movement`
                                              // keeps upstream behavior (creature interpolation
                                              // only); `smooth-movement camera on` opts in.
constexpr double camera_tau_ms=90.0;          // transient catch-up (~95% done after ~270ms) --
                                              // double the prior 180 (halving tau doubles the
                                              // catch-up rate for exponential decay)
constexpr double camera_glide_knee_tiles=1.0; // debt past this decays proportionally faster --
                                              // see glide_tau_ms. Applies ONLY during sustained
                                              // scrolling (camera_glide_sustained): the knee
                                              // exists to keep the equilibrium debt low while
                                              // landings keep arriving, where the fast catch-up
                                              // blends into the motion already on screen. An
                                              // ISOLATED landing has no such cover -- the screen
                                              // is otherwise still, so knee-rate decay reads as a
                                              // snap -- and multi-tile isolated landings are the
                                              // NORM while paused, where DF recomputes the
                                              // buffers lazily and scrolls coalesce.
constexpr double camera_tau_min_ms=20.0;      // floor on the shortened tau: below this the
                                              // catch-up stops reading as motion and becomes a snap
constexpr double camera_glide_clamp_tiles=8.0;    // runaway backstop ONLY, not an operating limit.
                                              // A hard cap is a bad tool for bounding this: when it
                                              // bites it discards a tile of compensation, so the
                                              // content moves a tile with nothing cancelling it --
                                              // it manufactures exactly the jump the glide exists
                                              // to hide, once per landing, which at speed is every
                                              // frame. A 1-tile cap was tried and did that
                                              // (smoothing visibly gave up as soon as the view
                                              // moved quickly). The knee above bounds the debt
                                              // instead, by decaying it faster rather than
                                              // throwing it away.
constexpr uint32_t camera_sustained_gap_ms=150;  // landings closer together than this are one
                                              // continuous scroll (a drag lands every frame or
                                              // two); further apart they are isolated actions
double transient_x=0.0;                       // decaying glide offset, pixels
double transient_y=0.0;
uint32_t camera_last_landing_ms=0;            // when the previous landing was attributed
bool camera_glide_sustained=false;            // last landing followed another within the gap:
                                              // the knee (fast catch-up) may engage
double rest_x=0.0;                            // persistent free-camera offset, tiles
double rest_y=0.0;                            // (positive = view sits WEST/NORTH of window)
int32_t self_scroll_x=0;                      // window deltas WE wrote: visual no-ops when landing
int32_t self_scroll_y=0;
bool camera_was_offset=false;                 // edge-detects offset->0 for one cleanup redraw

bool adventure_mode()
{
	return gamemode!=nullptr&&*gamemode==df::game_mode::ADVENTURE;
}

double tile_px(const df::renderer_2d_base *renderer)
{
	const int32_t zoom=renderer->viewport_zoom_factor;
	return double(zoom==128?32:std::max(1,zoom*32/128));
}

// Cancel everything except the persistent rest offset (the camera keeps its sub-tile position
// across zoom/z/resize; only the in-flight animation state is unfollowable).
void cancel_camera_transients()
{
	transient_x=0.0;
	transient_y=0.0;
	self_scroll_x=0;
	self_scroll_y=0;
	// The scroll context is gone with the debt; without this a post-cancel landing could decay
	// one frame at the knee tau off a stale flag before resampling it.
	camera_glide_sustained=false;
}

void set_camera_enabled(bool enable)
{
	if(camera_enabled==enable)return;
	camera_enabled=enable;
	cancel_camera_transients();
	rest_x=0.0;
	rest_y=0.0;
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

// Exponential lerp toward zero: shrinks `value` by a FIXED FRACTION of its remaining distance to
// zero every tau_ms, independent of frame rate -- the standard framerate-independent damped
// approach. This is what makes the glide catch-up provably overshoot-proof: every call multiplies
// `value` by a factor in (0,1), so it can shrink and change magnitude but can never cross zero or
// reverse sign in a single step -- there is no way for the result to land on the far side of the
// target. (Contrast with clamping a value to a fixed cap right after an unrelated jump, which IS a
// discontinuous snap -- that was the actual source of the visible "over-correction" under fast
// dragging, not this decay.)
double decay_toward_zero(double value,double tau_ms,uint32_t delta_ms)
{
	return value*std::exp(-double(delta_ms)/tau_ms);
}

// Tau for the debt currently outstanding. Up to the knee the glide eases out at the base rate --
// the ordinary case, a single landing, is one tile and never exceeds it, so isolated scrolls
// animate exactly as before. Past the knee tau shortens in proportion, so every extra tile of debt
// buys a faster catch-up instead of being discarded.
//
// This is what lets the glide keep animating when the view moves fast. Compensate-then-decay under
// SUSTAINED scrolling settles where new debt and decay balance; at a fixed tau that is
// rate*tau/frame, about six tiles at a tile per frame -- far enough behind that the view visibly
// trails and lurches. Shortening tau in proportion to the debt makes the balance point
// sqrt(rate*knee*tau/frame) instead: ~1.6 tiles at half a tile per frame, ~2.3 at one, ~3.3 at two.
// Sub-linear in scroll rate, so it stays bounded at any speed without ever dropping a tile of
// compensation, which is the part a hard cap cannot do.
double glide_tau_ms(double debt_px,double tile)
{
	if(tile<=0.0)return camera_tau_ms;
	// The knee is an equilibrium tool, not a presentation upgrade: only sustained scrolling has
	// an equilibrium to bound. For an isolated landing the shortened tau -- floored at 20ms,
	// about one render frame -- discards more than half the debt per frame, which on an
	// otherwise-still screen is a visible snap, not a glide. Isolated multi-tile landings are
	// exactly the paused case (coalesced scrolls, recenters), so they ease out at the base tau.
	if(!camera_glide_sustained)return camera_tau_ms;
	const double tiles=std::abs(debt_px)/tile;
	if(tiles<=camera_glide_knee_tiles)return camera_tau_ms;
	return std::max(camera_tau_min_ms,camera_tau_ms*camera_glide_knee_tiles/tiles);
}

// A scroll of (ax,ay) tiles has landed in the buffers: our own normalization writes are visual
// no-ops (they move into rest); the remainder is a real scroll and adds to the glide debt.
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
	transient_x+=gx*tile;
	transient_y+=gy*tile;
	// Safety backstop only -- see camera_glide_clamp_tiles. A well-behaved lerp does not need a
	// magnitude clamp to avoid overshoot; this exists purely to bound a genuinely runaway state.
	const double cap=tile*camera_glide_clamp_tiles;
	transient_x=std::clamp(transient_x,-cap,cap);
	transient_y=std::clamp(transient_y,-cap,cap);
}

// Per-frame camera bookkeeping: compensate scrolls on the frame their content actually lands in
// the render buffers, and decay that compensation away.
//
// ATTRIBUTION IS KEYED TO BUFFER LANDINGS, NEVER TO window_x/window_y DELTAS. The distinction is
// the whole correctness argument here, and getting it wrong is what every previous version of this
// function did:
//
//   The drawn image is the viewport BUFFER content (redraw_world_tile -> update_viewport_tile,
//   which reads vp->screentexpos_*) painted at renderer->origin plus our glide offset. window_x
//   is not a term in it. window_x changes at INPUT time; DF recomputes the buffers on some LATER
//   frame -- "far less often than this hook runs, and while paused hardly at all" (upstream
//   c116b2d, the same observation that fixed the sprite jiggle). So window_x is an announcement of
//   intent, and the buffers are the thing on screen.
//
//   Compensating on the window delta therefore shifts the world by a tile while the screen is
//   still showing pre-scroll content -- a visible jump AGAINST the direction of travel -- and then
//   jumps again when the buffer finally lands. Unpaused this is invisible: the buffers recompute
//   nearly every frame, so window delta and buffer shift coincide and the wrong model happens to
//   give the right answer. Paused, they come apart, and that gap IS the "camera jitters while
//   paused" bug. (It is also the original "camera jumps the opposite direction as motion starts"
//   report, which was misdiagnosed as a since-removed 2-frame hold buffer. A fixed frame delay was
//   never the fix either: the correct delay is "until the buffers advance", which is not a number.)
//
// animation_manager.get_last_shift(vp) is exactly that landing: the tile shift the manager
// confirmed against buffer content THIS frame, zero on frames nothing landed. It is gated on the
// buffer fingerprint actually changing, resolves fast scrolls piecewise (the buffers may apply
// only +1 of a pending +3), handles view crossings, and ages by wall clock -- machinery this
// function used to duplicate, worse, in a private background_match_ratio search with a fistful of
// magic frame counts. synchronize_viewport runs earlier in the same frame, so the value is fresh.
void update_camera(
	df::renderer_2d_base *renderer,
	uint32_t now_ms,
	uint32_t delta_ms,
	int32_t shift_x,
	int32_t shift_y)
{
	// Adventure mode is excluded: the view follows the player there, so EVERY step is a scroll
	// and the whole-map glide repaint would run near-constantly -- too heavy to be worth it.
	if(!camera_enabled||adventure_mode())return;
	const double tile=tile_px(renderer);

	// Decay BEFORE attributing, so the landing frame renders its compensation IN FULL. The
	// landing frame is the one frame the glide exists for: the content just jumped -shift on
	// screen and the offset must cancel exactly that. Decaying after attributing leaked
	// (1-exp(-dt/tau)) of the landing before it was ever drawn once -- ~18% of a tile at the
	// base tau, and at the knee-shortened tau MOST of it (a five-tile landing rendered barely a
	// third of its compensation; the missing two-thirds was the paused-scroll jump). The sprite
	// interpolation path already renders full compensation on its landing frame (a smoothstep
	// at t=0 is the whole offset); this makes the camera match it.
	if(transient_x!=0.0||transient_y!=0.0)
		{
		transient_x=decay_toward_zero(
			transient_x,glide_tau_ms(transient_x,tile),delta_ms);
		transient_y=decay_toward_zero(
			transient_y,glide_tau_ms(transient_y,tile),delta_ms);
		if(std::abs(transient_x)<0.5&&std::abs(transient_y)<0.5)
			{
			transient_x=0.0;
			transient_y=0.0;
			}
		}

	// EVERY landing is animated. There is no size above which the glide stands down: a landing
	// is content that has already moved on screen, so declining to compensate it is not "taking
	// the step cleanly", it is choosing to show the jump. The old threshold could only ever
	// convert a smooth catch-up into a visible one, and judging when that trade was worth making
	// from the landing size alone proved unreliable in play -- it fired on ordinary fast
	// scrolling, which is exactly when the smoothing matters most.
	//
	// Nothing needs the threshold any more. A genuinely huge landing (recenter, minimap warp) is
	// bounded by camera_glide_clamp_tiles and eased out at the base tau (isolated landings never
	// engage the knee), a fast quarter-second slide rather than a chase across the map.
	//
	// Our own normalization writes are filtered out inside attribute_landed -- they move into
	// rest, not into the glide.
	if(shift_x!=0||shift_y!=0)
		{
		attribute_landed(shift_x,shift_y,tile);
		// Landings arriving back-to-back are one continuous scroll: the knee may engage to keep
		// the trailing debt bounded (see glide_tau_ms). The flag is sampled here, on landings
		// only, so the decay of an isolated landing stays at the base tau for its whole tail --
		// it cannot speed up mid-glide.
		camera_glide_sustained=
			now_ms-camera_last_landing_ms<=camera_sustained_gap_ms;
		camera_last_landing_ms=now_ms;
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
		// Scroll-landing oracle: terrain is world-anchored, so landings attribute even when
		// the sprite layers are sparse or scrolled in lockstep with the view.
		vp->screentexpos_background,
		vp->screentexpos_background_old
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

bool has_fire(const df::graphic_viewportst *vp,int32_t x,int32_t y)
{
	return vp->screentexpos_spatter_flag!=nullptr&&
		(vp->screentexpos_spatter_flag[x*vp->dim_y+y]&fire_bits)!=0;
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

void redraw_viewport_tile(
	df::renderer_2d_base *renderer,
	const viewport_renderst &viewport,
	int32_t x,
	int32_t y,
	bool defer_interface)
{
	df::graphic_viewportst *vp=viewport.viewport;
	const int32_t index=x*vp->dim_y+y;
	const auto redraw=[&]{renderer->update_viewport_tile(vp,x,y);};
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
	const auto redraw=[&]{renderer->update_viewport_tile(vp,x,y);};
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
	const auto redraw=[&]{renderer->update_viewport_tile(vp,x,y);};
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
	const tile_coveragest &coverage)
{
	for(size_t index=0;index<viewports.size();++index)
		{
		// A lower z-level's proxy must be covered by the next viewport's fog and terrain.
		// Reapply that viewport before its own proxies, matching DF's lower-to-main draw order.
		if(index>0)redraw_viewport_tiles(renderer,viewports[index],coverage);
		const viewport_renderst &viewport=viewports[index];
		draw_interpolation_stages(
			renderer,viewport.viewport,viewport.proxies,viewport.coverage);
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
	df::graphic_viewportst *vp=gps?gps->main_viewport:nullptr;
	const std::vector<df::graphic_viewportst *> viewports=active_viewports();

	if(vp!=nullptr)update_visual_context(renderer,vp);
	const uint32_t now_ms=Core::getInstance().p->getTickCount();
	animation_manager.begin_frame(now_ms);
	for(df::graphic_viewportst *viewport:viewports)
		animation_manager.synchronize_viewport(animation_input(viewport));
	animation_manager.end_frame();

	if(!viewport_readable(vp)||renderer->sdl_renderer==nullptr)
		return;
	// The MAIN viewport's landing, read once: lower z-level viewports are synchronized too, and
	// everything below is about what the player is looking at.
	const std::array<int32_t,2> landed=animation_manager.get_last_shift(vp);
	update_camera(renderer,now_ms,animation_manager.get_frame_delta_ms(),landed[0],landed[1]);
	const double cam_tile=tile_px(renderer);
	const bool camera_active=camera_enabled&&!adventure_mode();

	const int32_t glide_x=camera_active?
		int32_t(std::lround(transient_x+rest_x*cam_tile)):0;
	const int32_t glide_y=camera_active?
		int32_t(std::lround(transient_y+rest_y*cam_tile)):0;
	const bool glide=glide_x!=0||glide_y!=0;

	if(!glide&&camera_was_offset)
		{
		// The camera just re-joined the grid: one engine redraw replaces the last shifted frame.
		camera_was_offset=false;
		if(gps!=nullptr)++gps->force_full_display_count;
		}
	if(glide)camera_was_offset=true;
	if(!glide&&!animation_manager.requires_full_redraw()&&
		(!flip_enabled||!has_mirrored_viewport_facing(viewports)))
		return;

	std::vector<viewport_renderst> viewport_renders=
		collect_viewport_renders(renderer,viewports);
	tile_coveragest coverage=collect_viewport_coverage(viewport_renders);

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
		draw_viewport_interpolation_stages(renderer,viewport_renders,coverage);
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
	draw_viewport_interpolation_stages(renderer,viewport_renders,coverage);

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
	camera_was_offset=false;
	camera_last_landing_ms=0;
	camera_glide_sustained=false;
	flip_enabled=false;
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
		out.print("sprite flipping: {}\n",
			flip_enabled?"on":"off");
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
			if(adventure_mode())
				out.print("note: the free camera applies in fortress mode only.\n");
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
	return CR_WRONG_USAGE;
}

} // namespace

DFhackCExport command_result
plugin_init(color_ostream &,std::vector<PluginCommand> &commands)
{
	commands.emplace_back(
		"smooth-movement",
		"Smooth movement status; free camera: camera on|off|reset|<fx> <fy>; "
		"sprite flipping: flip on|off.",
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
