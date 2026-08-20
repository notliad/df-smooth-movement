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
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
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

// Runtime harness for the engine-owned visual state; gameplay data is never read.
decltype(&SDL_RenderCopyF) render_copy_f=nullptr;
decltype(&SDL_RenderCopyExF) render_copy_ex_f=nullptr;
decltype(&SDL_RenderFillRect) render_fill_rect=nullptr;
decltype(&SDL_RenderSetClipRect) render_set_clip_rect=nullptr;
decltype(&SDL_GetRenderDrawColor) get_render_draw_color=nullptr;
decltype(&SDL_SetRenderDrawColor) set_render_draw_color=nullptr;

// Mutable render state is owned by the render thread. Plugin commands and lifecycle callbacks run
// elsewhere, so they rendezvous with that thread before reading or changing it. This mutex only
// serializes those rare transactions; the per-frame hook never takes it.
std::mutex render_transaction_mutex;
// Published as a pair: render_thread_id is written before the flag is set, and read after the flag
// is seen, so the id is visible to any thread that observes the flag. Read outside the mutex --
// render_thread_transaction has to know which thread it is on before deciding whether to take it.
std::thread::id render_thread_id;
std::atomic<bool> has_render_thread_id{false};
// Set while a transaction that could not rendezvous owns the state on its own thread; see
// render_thread_transaction. Written under render_transaction_mutex, and only ever set while the
// render hook cannot run.
std::atomic<bool> render_state_owned_inline{false};

struct inline_ownershipst
{
	inline_ownershipst() { render_state_owned_inline.store(true,std::memory_order_release); }
	~inline_ownershipst() { render_state_owned_inline.store(false,std::memory_order_release); }
	inline_ownershipst(const inline_ownershipst &)=delete;
	inline_ownershipst &operator=(const inline_ownershipst &)=delete;
};

bool on_render_thread()
{
	return has_render_thread_id.load(std::memory_order_acquire)&&
		render_thread_id==std::this_thread::get_id();
}

// Two rules govern how a transaction reaches the render state, and both come from one fact:
// runOnRenderThread merely APPENDS to a queue. DFHack drains that queue from dfhooks_sdl_loop, on
// DF's main/render thread, once per frame -- and only after DF's simulation thread has finished
// producing the frame, because the render thread spends the simulation phase parked in
// enablerst::async_wait(). So the render thread can service a callback only while the simulation
// thread is free to run.
//
// Rule 1: do not wait for the render thread while holding DFHack's core suspension.
//
// DF's simulation thread owns the core for the whole of Core::Update, so waiting there is an
// unconditional deadlock: the render thread cannot drain the queue until we return, and we do not
// return until it drains the queue. This is not a corner case. Every `enable smooth-movement`
// reaches plugin_enable with the core suspended, so the plugin froze DF outright whenever it was
// enabled from anywhere but the console -- Core::Update -> handleLoadAndUnloadScripts -> the
// script -> Commands::enable -> plugin_enable -> block forever. Enabling from onMapLoad.init hung
// the game on the way into a fort. Registering the command core-unlocked does not help either: it
// stops DFHack adding a suspension of its own, but a command invoked from lua still runs on the
// simulation thread, which already holds one, so `smooth-movement camera on` from a script hung it
// the same way.
//
// The frame ordering that causes the deadlock is what makes the alternative safe: while the core is
// suspended the render thread is blocked BEFORE its render phase, so update_all is neither running
// nor able to start. The state has no other reader and can be touched directly.
//
// Rule 2: do not hold render_transaction_mutex while waiting for the render thread.
//
// A waiter that holds it can be joined by an inline transaction on the simulation thread, which
// then blocks on the mutex -- and a blocked simulation thread never lets the render thread reach
// the drain the waiter is waiting for. Three threads, one cycle, same freeze. The queued task takes
// the mutex itself, on the render thread, which keeps it mutually exclusive with inline
// transactions without ever putting it on a blocking path.
template<typename Callback>
auto render_thread_transaction(Callback callback)
{
	using result_type=std::invoke_result_t<Callback>;
	if(on_render_thread())
		{
		std::lock_guard<std::mutex> transaction(render_transaction_mutex);
		return callback();
		}
	if(Core::getInstance().isSuspended())
		{
		std::lock_guard<std::mutex> transaction(render_transaction_mutex);
		inline_ownershipst ownership;
		return callback();
		}

	auto task=std::make_shared<std::packaged_task<result_type()>>(
		[callback=std::move(callback)]() mutable -> result_type
			{
			std::lock_guard<std::mutex> transaction(render_transaction_mutex);
			const std::thread::id current=std::this_thread::get_id();
			if(!has_render_thread_id.load(std::memory_order_relaxed))
				{
				render_thread_id=current;
				has_render_thread_id.store(true,std::memory_order_release);
				}
			else if(render_thread_id!=current)
				throw std::runtime_error("DFHack render thread changed");
			return callback();
			});
	std::future<result_type> result=task->get_future();
	DFHack::runOnRenderThread([task]{(*task)();});
	return result.get();
}

// The render thread identifies itself by running a transaction, so a plugin enabled entirely
// through the inline path above reaches its first frame with no owner recorded. Claim it there
// instead. The lock is taken once, on that first frame; every later frame is a single atomic read.
void adopt_render_thread()
{
	if(has_render_thread_id.load(std::memory_order_acquire))return;
	std::lock_guard<std::mutex> transaction(render_transaction_mutex);
	if(has_render_thread_id.load(std::memory_order_relaxed))return;
	render_thread_id=std::this_thread::get_id();
	has_render_thread_id.store(true,std::memory_order_release);
}

void assert_render_thread()
{
	if(render_state_owned_inline.load(std::memory_order_acquire))return;
	assert(has_render_thread_id.load(std::memory_order_acquire));
	assert(render_thread_id==std::this_thread::get_id());
}

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
enum class camera_border_behaviorst : uint8_t
{
	retain,
	black
};

const char *camera_border_behavior_name(camera_border_behaviorst behavior)
{
	return behavior==camera_border_behaviorst::black?"black":"retain";
}

// The camera is visually unbound from the tile grid. Three layered offsets:
//   rest            -- the persistent sub-tile free-camera offset, in tiles.
//   transient       -- the short decaying glide for ordinary DF scrolls, in pixels.
//   drag correction -- the gentler correction for excess displacement during a fast mouse drag.
// Render offset = transient + drag_correction + rest*tile. window_x/window_y remain DF's camera.
bool camera_enabled=false;                    // OFF by default: plain `enable smooth-movement`
                                              // keeps upstream behavior (creature interpolation
                                              // only); `smooth-movement camera on` opts in.
camera_border_behaviorst camera_border_behavior=camera_border_behaviorst::retain;
constexpr int32_t camera_max_glide_tiles=3;   // per-jump: farther than this snaps instantly
double transient_x=0.0;                       // decaying glide offset, pixels
double transient_y=0.0;
double drag_correction_x=0.0;                 // elastic drag overflow, pixels
double drag_correction_y=0.0;
bool drag_release_pending=false;              // preserve late DF buffer shifts after release
double rest_x=0.0;                            // persistent or bounded drag offset, tiles
double rest_y=0.0;                            // (positive = view sits WEST/NORTH of window)
int32_t self_scroll_x=0;                      // window deltas WE wrote: visual no-ops when landing
int32_t self_scroll_y=0;
visual_movement_idst camera_follow_id=no_visual_movement;
const void *camera_follow_viewport=nullptr;
double camera_follow_x=0.0;                   // inverse proxy displacement, pixels
double camera_follow_y=0.0;
bool camera_ignore_pending=false;             // a teleport-like announcement must remain snapped
bool drag_active=false;
double drag_anchor_vx=0.0;                    // visual camera at drag start, tiles
double drag_anchor_vy=0.0;
int32_t drag_anchor_mx=0;                     // precise mouse at drag start, pixels
int32_t drag_anchor_my=0;
bool camera_was_offset=false;                 // edge-detects offset->0 for one cleanup redraw
int32_t camera_prev_wx=0;                     // window-scroll observation baseline
int32_t camera_prev_wy=0;
bool camera_has_prev=false;

int32_t tile_size_px(int32_t zoom)
{
	const int64_t scaled=int64_t(zoom)*32/128;
	return int32_t(std::clamp(
		scaled,int64_t(1),int64_t(std::numeric_limits<int32_t>::max())));
}

double tile_px(const df::renderer_2d_base *renderer)
{
	return double(tile_size_px(renderer->viewport_zoom_factor));
}

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
	drag_correction_x=0.0;
	drag_correction_y=0.0;
	drag_release_pending=false;
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
	const double limit=double(std::numeric_limits<int32_t>::max());
	if(!std::isfinite(rest_x)||!std::isfinite(rest_y)||
		std::abs(rest_x)>limit||std::abs(rest_y)>limit)
		{
		rest_x=0.0;
		rest_y=0.0;
		cancel_camera_transients();
		return;
		}
	const int32_t kx=int32_t(-std::llround(rest_x));
	const int32_t ky=int32_t(-std::llround(rest_y));
	const int64_t new_window_x=window_x?int64_t(*window_x)+kx:-1;
	const int64_t new_window_y=window_y?int64_t(*window_y)+ky:-1;
	if(kx!=0&&window_x!=nullptr&&new_window_x>=0&&
		new_window_x<=std::numeric_limits<int32_t>::max())
		{
		*window_x=int32_t(new_window_x);
		self_scroll_x=int32_t(std::clamp(
			int64_t(self_scroll_x)+kx,
			int64_t(std::numeric_limits<int32_t>::min()),
			int64_t(std::numeric_limits<int32_t>::max())));
		}
	if(ky!=0&&window_y!=nullptr&&new_window_y>=0&&
		new_window_y<=std::numeric_limits<int32_t>::max())
		{
		*window_y=int32_t(new_window_y);
		self_scroll_y=int32_t(std::clamp(
			int64_t(self_scroll_y)+ky,
			int64_t(std::numeric_limits<int32_t>::min()),
			int64_t(std::numeric_limits<int32_t>::max())));
		}
}
// A scroll of (ax,ay) tiles has landed in the buffers. Return the portion DF initiated; our own
// normalization writes are visual no-ops and fold into rest instead.
std::array<int32_t,2> attribute_landed(int32_t ax,int32_t ay)
{
	const landed_scroll_axisst attributed_x=attribute_self_scroll_axis(self_scroll_x,ax);
	const landed_scroll_axisst attributed_y=attribute_self_scroll_axis(self_scroll_y,ay);
	self_scroll_x-=attributed_x.self;
	self_scroll_y-=attributed_y.self;
	rest_x+=attributed_x.self;
	rest_y+=attributed_y.self;
	const int32_t gx=attributed_x.gameplay;
	const int32_t gy=attributed_y.gameplay;
	if(drag_active||drag_release_pending)
		{
		rest_x+=gx;
		rest_y+=gy;
		}
	return {gx,gy};
}

// Per-frame camera bookkeeping consumes the animation manager's authoritative scroll landing.
// A visual follow movement supplies the exact inverse proxy offset; unmatched pans retain the
// exponential glide. Existing transients decay before a newly landed fallback is added.
void update_camera(
	df::renderer_2d_base *renderer,
	const df::graphic_viewportst *vp,
	uint32_t delta_ms)
{
	if(!camera_enabled)return;
	const double tile=tile_px(renderer);
	transient_x=camera_transient_after_landing(transient_x,delta_ms,0.0);
	transient_y=camera_transient_after_landing(transient_y,delta_ms,0.0);
	drag_correction_x=decay_camera_drag_correction(drag_correction_x,delta_ms);
	drag_correction_y=decay_camera_drag_correction(drag_correction_y,delta_ms);
	const int32_t wx=window_x?*window_x:0;
	const int32_t wy=window_y?*window_y:0;
	if(camera_has_prev&&(wx!=camera_prev_wx||wy!=camera_prev_wy))
		{
		const int64_t dx=int64_t(wx)-camera_prev_wx;
		const int64_t dy=int64_t(wy)-camera_prev_wy;
		if((std::abs(dx)>camera_max_glide_tiles||std::abs(dy)>camera_max_glide_tiles)&&
			!drag_active&&!drag_release_pending)
			{
			transient_x=0.0;
			transient_y=0.0;
			camera_follow_id=no_visual_movement;
			camera_follow_x=0.0;
			camera_follow_y=0.0;
			camera_ignore_pending=true;   // recenter/minimap/teleport-like jump: snap
			}
		}
	camera_prev_wx=wx;
	camera_prev_wy=wy;
	camera_has_prev=true;

	const visual_scroll_renderst scroll=animation_manager.get_scroll(vp);
	if(scroll.abandoned)
		{
		camera_follow_id=no_visual_movement;
		camera_follow_viewport=nullptr;
		camera_follow_x=0.0;
		camera_follow_y=0.0;
		camera_ignore_pending=false;
		self_scroll_x=0;
		self_scroll_y=0;
		}
	if(scroll.landed)
		{
		const auto gameplay=attribute_landed(scroll.landed_x,scroll.landed_y);
		const bool self_only=gameplay[0]==0&&gameplay[1]==0;
		const bool ignore=camera_ignore_pending;
		if(!scroll.pending)camera_ignore_pending=false;
		const bool has_self_component=
			gameplay[0]!=scroll.landed_x||gameplay[1]!=scroll.landed_y;
		if(!self_only&&!has_self_component&&!ignore&&!drag_active&&!drag_release_pending&&
			scroll.follow_candidate!=no_visual_movement)
			{
			camera_follow_id=scroll.follow_candidate;
			camera_follow_viewport=vp;
			transient_x=0.0;
			transient_y=0.0;
			}
		else if(!self_only&&!ignore&&!drag_active&&!drag_release_pending)
			{
			camera_follow_id=no_visual_movement;
			camera_follow_viewport=nullptr;
			camera_follow_x=0.0;
			camera_follow_y=0.0;
			const double cap=tile*(camera_max_glide_tiles+0.5);
			transient_x=std::clamp(transient_x+gameplay[0]*tile,-cap,cap);
			transient_y=std::clamp(transient_y+gameplay[1]*tile,-cap,cap);
			}
		}
	if(camera_follow_id!=no_visual_movement)
		{
		const visual_follow_renderst follow=
			animation_manager.get_follow(camera_follow_viewport,camera_follow_id);
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

	// --- pixel-perfect middle-mouse drag: the view follows the mouse 1:1 inside the bounded rest
	// range. Excess displacement becomes an elastic correction; release keeps the current position.
	// DF's own drag still moves window in tile steps; rest carries the unresolved remainder.
	// Positions are tracked against the CONTENT window (window minus unlanded jumps) so the
	// buffer lag never causes a visible stutter.
	const bool mbut=enabler!=nullptr&&enabler->mouse_mbut;
	const double content_wx=double(wx-scroll.pending_x);
	const double content_wy=double(wy-scroll.pending_y);
	if(mbut&&!drag_active&&gps!=nullptr)
		{
		drag_active=true;
		drag_release_pending=false;
		drag_anchor_vx=camera_drag_anchor(
			content_wx,rest_x,transient_x,drag_correction_x,camera_follow_x,tile);
		drag_anchor_vy=camera_drag_anchor(
			content_wy,rest_y,transient_y,drag_correction_y,camera_follow_y,tile);
		drag_anchor_mx=gps->precise_mouse_x;
		drag_anchor_my=gps->precise_mouse_y;
		transient_x=0.0;
		transient_y=0.0;
		drag_correction_x=0.0;
		drag_correction_y=0.0;
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
			const camera_drag_axisst released_x=persist_camera_drag_axis(
				rest_x,transient_x+drag_correction_x,tile);
			const camera_drag_axisst released_y=persist_camera_drag_axis(
				rest_y,transient_y+drag_correction_y,tile);
			rest_x=released_x.rest_tiles;
			rest_y=released_y.rest_tiles;
			drag_correction_x=released_x.correction_px;
			drag_correction_y=released_y.correction_px;
			transient_x=0.0;
			transient_y=0.0;
			drag_release_pending=true;
			}
		else if(gps!=nullptr)
			{
			const double vx=
				drag_anchor_vx-double(gps->precise_mouse_x-drag_anchor_mx)/tile;
			const double vy=
				drag_anchor_vy-double(gps->precise_mouse_y-drag_anchor_my)/tile;
			const double requested_rest_x=content_wx-vx;
			const double requested_rest_y=content_wy-vy;
			const camera_drag_axisst constrained_x=constrain_camera_drag_axis(
				requested_rest_x,drag_correction_x,tile);
			const camera_drag_axisst constrained_y=constrain_camera_drag_axis(
				requested_rest_y,drag_correction_y,tile);
			rest_x=constrained_x.rest_tiles;
			rest_y=constrained_y.rest_tiles;
			drag_correction_x=constrained_x.correction_px;
			drag_correction_y=constrained_y.correction_px;
			// Rebase only when either axis overflowed; correction preserves the rendered position.
			if(rest_x!=requested_rest_x||rest_y!=requested_rest_y)
				{
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

	// Keep any DF tile steps that were already buffered at release visually stationary. Once they
	// have landed, normalize whole tiles without changing the fractional resting position.
	if(drag_release_pending&&!scroll.pending&&
		self_scroll_x==0&&self_scroll_y==0)
		{
		normalize_rest();
		if(self_scroll_x==0&&self_scroll_y==0)drag_release_pending=false;
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
		vp->screentexpos_background,
		vp->screentexpos_background_old,
		window_x?*window_x:0,
		window_y?*window_y:0
		};
}

// The layer buffers are freed and nulled without clearing the active flag.
bool viewport_readable(df::graphic_viewportst *vp)
{
	return vp!=nullptr&&vp->flag.bits.active&&animation_input(vp).valid()&&
		vp->clipx[0]>=0&&vp->clipx[0]<=vp->clipx[1]&&vp->clipx[1]<vp->dim_x&&
		vp->clipy[0]>=0&&vp->clipy[0]<=vp->clipy[1]&&vp->clipy[1]<vp->dim_y&&
		vp->screentexpos_background!=nullptr&&
		vp->screentexpos_background_old!=nullptr&&
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
		vp->screentexpos_top_shadow!=nullptr&&
		vp->screentexpos_signpost!=nullptr;
}

int32_t tile_pixel(int32_t tile,int32_t origin,int32_t zoom)
{
	// Algebraically this is the renderer's (zoom*32*tile)/128+origin, but the
	// reordered expression cannot overflow int64_t for int32_t inputs.
	const int64_t pixel=(int64_t(zoom)*tile)/4+origin;
	return int32_t(std::clamp(
		pixel,
		int64_t(std::numeric_limits<int32_t>::min()),
		int64_t(std::numeric_limits<int32_t>::max())));
}

int32_t saturated_pixel_span(int32_t first,int32_t last)
{
	return int32_t(std::clamp(
		int64_t(last)-first,
		int64_t(0),
		int64_t(std::numeric_limits<int32_t>::max())));
}

int32_t saturated_add(int32_t first,int32_t second)
{
	return int32_t(std::clamp(
		int64_t(first)+second,
		int64_t(std::numeric_limits<int32_t>::min()),
		int64_t(std::numeric_limits<int32_t>::max())));
}

bool rounded_pixel_offset(double value,int32_t &result)
{
	if(!std::isfinite(value)||
		value<double(std::numeric_limits<int32_t>::min())||
		value>double(std::numeric_limits<int32_t>::max()))return false;
	result=int32_t(std::lround(value));
	return true;
}

bool inside_clip(const df::graphic_viewportst *vp,int32_t x,int32_t y)
{
	return x>=vp->clipx[0]&&x<=vp->clipx[1]&&
		y>=vp->clipy[0]&&y<=vp->clipy[1];
}

bool has_fire(const df::graphic_viewportst *vp,int32_t x,int32_t y)
{
	return (vp->screentexpos_spatter_flag[x*vp->dim_y+y]&fire_bits)!=0;
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

// Runs after the proxies so the shading covers them rather than sitting underneath.
void draw_interface_only(
	df::renderer_2d_base *renderer,
	df::graphic_viewportst *vp,
	int32_t x,
	int32_t y)
{
	// All required buffers were checked once by viewport_readable(). Interface is optional.
	if(vp->screentexpos_interface==nullptr)return;
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
	const float tile_size=float(tile_size_px(zoom));
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

std::vector<df::graphic_viewportst *> active_viewports(bool main_readable)
{
	std::vector<df::graphic_viewportst *> viewports;
	if(gps==nullptr)return viewports;
	for(int32_t lower=7;lower>=0;--lower)
		{
		df::graphic_viewportst *vp=gps->lower_viewport[lower];
		if(viewport_readable(vp))viewports.push_back(vp);
		}
	if(main_readable)
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
	const bool main_readable=viewport_readable(vp);
	const std::vector<df::graphic_viewportst *> viewports=active_viewports(main_readable);

	if(main_readable)update_visual_context(renderer,vp);
	else
		{
		previous_coverage.clear();
		previous_viewport=nullptr;
		has_view_signature=false;
		has_pan_context=false;
		cancel_camera_transients();
		}
	const uint32_t now_ms=Core::getInstance().p->getTickCount();
	animation_manager.begin_frame(
		now_ms,
		movement_duration_for_fps(enabler?enabler->fps:default_game_fps));
	for(df::graphic_viewportst *viewport:viewports)
		animation_manager.synchronize_viewport(animation_input(viewport));
	animation_manager.end_frame();

	if(!main_readable||renderer->sdl_renderer==nullptr)
		return;
	update_camera(renderer,vp,animation_manager.get_frame_delta_ms());
	const double cam_tile=tile_px(renderer);
	int32_t glide_x=0;
	int32_t glide_y=0;
	if(!rounded_pixel_offset(
			transient_x+drag_correction_x+camera_follow_x+rest_x*cam_tile,glide_x)||
		!rounded_pixel_offset(
			transient_y+drag_correction_y+camera_follow_y+rest_y*cam_tile,glide_y))
		{
		rest_x=0.0;
		rest_y=0.0;
		cancel_camera_transients();
		glide_x=0;
		glide_y=0;
		}
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
	const int32_t tile_size=tile_size_px(zoom);

	if(glide)
		{
		// Camera mid-glide: repaint the WHOLE map rect at the shifted origin so the world (and
		// the creature proxies, which read origin at draw time) renders between tiles. The engine
		// already drew this frame at the snapped position; everything here overdraws it, clipped
		// to the map rect so shifted tiles never spill over the UI. The uncovered strip on the
		// trailing edge retains that snapped frame by default, or is cleared in black mode.
		const int32_t map_left=tile_pixel(vp->clipx[0],renderer->origin_x,zoom);
		const int32_t map_top=tile_pixel(vp->clipy[0],renderer->origin_y,zoom);
		const int32_t map_right=tile_pixel(vp->clipx[1]+1,renderer->origin_x,zoom);
		const int32_t map_bottom=tile_pixel(vp->clipy[1]+1,renderer->origin_y,zoom);
		const SDL_Rect map_rect=
			{
			map_left,
			map_top,
			saturated_pixel_span(map_left,map_right),
			saturated_pixel_span(map_top,map_bottom)
			};
		render_set_clip_rect(sdl_renderer,&map_rect);
		if(camera_border_behavior==camera_border_behaviorst::black)
			{
			Uint8 old_r=0,old_g=0,old_b=0,old_a=255;
			get_render_draw_color(sdl_renderer,&old_r,&old_g,&old_b,&old_a);
			set_render_draw_color(sdl_renderer,0,0,0,255);
			render_fill_rect(sdl_renderer,&map_rect);
			set_render_draw_color(sdl_renderer,old_r,old_g,old_b,old_a);
			}

		const int32_t saved_origin_x=renderer->origin_x;
		const int32_t saved_origin_y=renderer->origin_y;
		renderer->origin_x=saturated_add(renderer->origin_x,glide_x);
		renderer->origin_y=saturated_add(renderer->origin_y,glide_y);
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
	adopt_render_thread();
	assert_render_thread();
	// update_all is the existing UI stage, so world correction must run first.
	render_interpolated_world(this);
	INTERPOSE_NEXT(update_all)();
}

struct sdl_bindingst
{
	decltype(&SDL_RenderCopyF) render_copy_f=nullptr;
	decltype(&SDL_RenderCopyExF) render_copy_ex_f=nullptr;
	decltype(&SDL_RenderFillRect) render_fill_rect=nullptr;
	decltype(&SDL_RenderSetClipRect) render_set_clip_rect=nullptr;
	decltype(&SDL_GetRenderDrawColor) get_render_draw_color=nullptr;
	decltype(&SDL_SetRenderDrawColor) set_render_draw_color=nullptr;
};

void install_sdl_bindings(const sdl_bindingst &bindings)
{
	assert_render_thread();
	render_copy_f=bindings.render_copy_f;
	render_copy_ex_f=bindings.render_copy_ex_f;
	render_fill_rect=bindings.render_fill_rect;
	render_set_clip_rect=bindings.render_set_clip_rect;
	get_render_draw_color=bindings.get_render_draw_color;
	set_render_draw_color=bindings.set_render_draw_color;
}

void clear_sdl_bindings()
{
	assert_render_thread();
	render_copy_f=nullptr;
	render_copy_ex_f=nullptr;
	render_fill_rect=nullptr;
	render_set_clip_rect=nullptr;
	get_render_draw_color=nullptr;
	set_render_draw_color=nullptr;
}

bool load_sdl(color_ostream &out,sdl_bindingst &bindings)
{
	DFLibrary *sdl_handle=DFSDL::obtain_library_handle();
	#define bind(name,target) \
		bindings.target=reinterpret_cast<decltype(bindings.target)>( \
			LookupPlugin(sdl_handle,#name)); \
		if(bindings.target==nullptr) { \
			out.printerr("smooth-movement: SDL2 function unavailable: " #name "\n"); \
			bindings={}; \
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
	assert_render_thread();
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
	camera_border_behavior=camera_border_behaviorst::retain;
	camera_has_prev=false;
	camera_was_offset=false;
	flip_enabled=false;
}

struct render_statusst
{
	bool camera_enabled;
	double rest_x;
	double rest_y;
	camera_border_behaviorst camera_border_behavior;
	bool flip_enabled;
};

bool get_render_status(color_ostream &out,render_statusst &status)
{
	try
		{
		status=render_thread_transaction([]
			{
			assert_render_thread();
			return render_statusst{
				camera_enabled,rest_x,rest_y,camera_border_behavior,flip_enabled};
			});
		return true;
		}
	catch(const std::exception &error)
		{
		out.printerr("smooth-movement: render transaction failed: {}\n",error.what());
		}
	catch(...)
		{
		out.printerr("smooth-movement: render transaction failed\n");
		}
	return false;
}

template<typename Callback>
bool update_render_state(color_ostream &out,Callback callback)
{
	try
		{
		render_thread_transaction([callback=std::move(callback)]() mutable
			{
			assert_render_thread();
			callback();
			});
		return true;
		}
	catch(const std::exception &error)
		{
		out.printerr("smooth-movement: render transaction failed: {}\n",error.what());
		}
	catch(...)
		{
		out.printerr("smooth-movement: render transaction failed\n");
		}
	return false;
}

command_result status_command(
	color_ostream &out,
	std::vector<std::string> &parameters)
{
	if(parameters.empty())
		{
		render_statusst status{};
		if(!get_render_status(out,status))return CR_FAILURE;
		out.print(
			"smooth-movement {}: {}\n",
			plugin_version,
			is_enabled?"enabled":"disabled");
		out.print("free camera: {}, offset {:.3f} {:.3f} (tiles east/south of the grid)\n",
			status.camera_enabled?"on":"off",-status.rest_x,-status.rest_y);
		out.print("camera border: {}\n",
			camera_border_behavior_name(status.camera_border_behavior));
		out.print("sprite flipping: {}\n",
			status.flip_enabled?"on":"off");
		return CR_OK;
		}
	if(parameters[0]=="camera")
		{
		if(parameters.size()==1)
			{
			render_statusst status{};
			if(!get_render_status(out,status))return CR_FAILURE;
			out.print("free camera: {}, offset {:.3f} {:.3f}\n",
				status.camera_enabled?"on":"off",-status.rest_x,-status.rest_y);
			out.print("camera border: {}\n",
				camera_border_behavior_name(status.camera_border_behavior));
			return CR_OK;
			}
		if(parameters.size()==2&&parameters[1]=="border")
			{
			render_statusst status{};
			if(!get_render_status(out,status))return CR_FAILURE;
			out.print("camera border: {}\n",
				camera_border_behavior_name(status.camera_border_behavior));
			return CR_OK;
			}
		if(parameters.size()==2&&parameters[1]=="on")
			{
			if(!update_render_state(out,[]{set_camera_enabled(true);}))return CR_FAILURE;
			return CR_OK;
			}
		if(parameters.size()==2&&parameters[1]=="off")
			{
			if(!update_render_state(out,[]{set_camera_enabled(false);}))return CR_FAILURE;
			return CR_OK;
			}
		if(parameters.size()==2&&parameters[1]=="reset")
			{
			if(!update_render_state(out,[]
				{
				cancel_camera_transients();
				rest_x=0.0;
				rest_y=0.0;
				if(gps!=nullptr)++gps->force_full_display_count;
				}))return CR_FAILURE;
			return CR_OK;
			}
		if(parameters.size()==3&&parameters[1]=="border"&&
			(parameters[2]=="retain"||parameters[2]=="black"))
			{
			const camera_border_behaviorst behavior=parameters[2]=="black"?
				camera_border_behaviorst::black:
				camera_border_behaviorst::retain;
			if(!update_render_state(out,[behavior]
				{
				if(camera_border_behavior==behavior)return;
				camera_border_behavior=behavior;
				if(gps!=nullptr)++gps->force_full_display_count;
				}))return CR_FAILURE;
			out.print("smooth-movement: camera border set to {}\n",
				camera_border_behavior_name(behavior));
			return CR_OK;
			}
		if(parameters.size()==3)
			{
			try
				{
				const double fx=std::stod(parameters[1]);
				const double fy=std::stod(parameters[2]);
				if(!valid_camera_offset(fx,fy))
					{
					out.printerr(
						"offsets must be finite and within -0.99..0.99 tiles\n");
					return CR_FAILURE;
					}
				// User-facing: positive = view sits east/south of the grid position.
				if(!update_render_state(out,[fx,fy]
					{
					set_camera_enabled(true);
					// An explicit persistent offset supersedes any in-flight drag correction.
					cancel_camera_transients();
					rest_x=-fx;
					rest_y=-fy;
					normalize_rest();
					}))return CR_FAILURE;
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
			render_statusst status{};
			if(!get_render_status(out,status))return CR_FAILURE;
			out.print("sprite flipping: {}\n",
				status.flip_enabled?"on":"off");
			return CR_OK;
			}
		// A toggle changes the screen without changing anything DF knows, so DF will not repaint.
		// OFF matters most: the render path stops touching tiles it painted every frame.
		// The last mirrored frame would persist.
		// Same flush plugin_enable(false) uses.
		if(parameters.size()==2&&parameters[1]=="on")
			{
			if(!update_render_state(out,[]
				{
				flip_enabled=true;
				if(gps!=nullptr)++gps->force_full_display_count;
				}))return CR_FAILURE;
			out.print("smooth-movement: sprite flipping enabled\n");
			return CR_OK;
			}
		if(parameters.size()==2&&parameters[1]=="off")
			{
			if(!update_render_state(out,[]
				{
				flip_enabled=false;
				if(gps!=nullptr)++gps->force_full_display_count;
				}))return CR_FAILURE;
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
		"Smooth movement status; free camera: camera on|off|reset|<fx> <fy> or "
		"camera border retain|black; "
		"sprite flipping: flip on|off.",
		status_command,
		false,
		// The command can synchronously wait for render-thread work, and parsing/output need no
		// core. Note this only avoids DFHack adding a suspension: a command invoked from lua still
		// runs on the simulation thread with the core already suspended, which is why
		// render_thread_transaction, not this flag, is what keeps the wait from deadlocking.
		true);
	return CR_OK;
}

DFhackCExport command_result plugin_enable(color_ostream &out,bool enable)
{
	if(is_enabled==enable)return CR_OK;
	if(enable)
		{
		sdl_bindingst bindings;
		if(!load_sdl(out,bindings))return CR_FAILURE;
		bool applied=false;
		try
			{
			applied=render_thread_transaction([bindings]
				{
				assert_render_thread();
				reset_state();
				install_sdl_bindings(bindings);
				if(INTERPOSE_HOOK(renderer_hook,update_all).apply())return true;
				clear_sdl_bindings();
				return false;
				});
			}
		catch(const std::exception &error)
			{
			out.printerr("smooth-movement: render transaction failed: {}\n",error.what());
			return CR_FAILURE;
			}
		catch(...)
			{
			out.printerr("smooth-movement: render transaction failed\n");
			return CR_FAILURE;
			}
		if(!applied)
			{
			out.printerr("smooth-movement: could not hook the 2D renderer\n");
			return CR_FAILURE;
			}
		}
	else
		{
		try
			{
			render_thread_transaction([]
				{
				assert_render_thread();
				INTERPOSE_HOOK(renderer_hook,update_all).remove();
				reset_state();
				clear_sdl_bindings();
				if(gps!=nullptr)++gps->force_full_display_count;
				});
			}
		catch(const std::exception &error)
			{
			out.printerr("smooth-movement: render transaction failed: {}\n",error.what());
			return CR_FAILURE;
			}
		catch(...)
			{
			out.printerr("smooth-movement: render transaction failed\n");
			return CR_FAILURE;
			}
		}
	is_enabled=enable;
	out.print("smooth-movement: {}\n",enable?"enabled":"disabled");
	return CR_OK;
}

DFhackCExport command_result plugin_shutdown(color_ostream &out)
{
	return plugin_enable(out,false);
}
