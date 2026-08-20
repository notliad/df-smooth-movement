// SPDX-License-Identifier: MIT

#ifndef VISUAL_ANIMATION_H
#define VISUAL_ANIMATION_H

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <vector>

enum class viewport_visual_layer : uint8_t
{
	right,
	center,
	left,
	upright,
	up,
	upleft,
	vehicle,
	item,
	designation,
	count
};

enum class visual_render_groupst : uint8_t
{
	item,
	vehicle,
	main,
	upper,
	designation,
	count
};

struct visual_layer_descriptorst
{
	viewport_visual_layer layer;
	visual_render_groupst render_group;
	bool moves_independently;
	bool matches_any_previous;
	uint8_t draw_order;
	int8_t center_x;
	int8_t center_y;
};

constexpr std::array visual_layer_descriptors=
	{
	visual_layer_descriptorst{viewport_visual_layer::right,
		visual_render_groupst::main,false,false,3,-1,0},
	visual_layer_descriptorst{viewport_visual_layer::center,
		visual_render_groupst::main,true,false,0,0,0},
	visual_layer_descriptorst{viewport_visual_layer::left,
		visual_render_groupst::main,false,false,4,1,0},
	visual_layer_descriptorst{viewport_visual_layer::upright,
		visual_render_groupst::upper,false,false,5,-1,1},
	visual_layer_descriptorst{viewport_visual_layer::up,
		visual_render_groupst::upper,false,false,6,0,1},
	visual_layer_descriptorst{viewport_visual_layer::upleft,
		visual_render_groupst::upper,false,false,7,1,1},
	visual_layer_descriptorst{viewport_visual_layer::vehicle,
		visual_render_groupst::vehicle,true,true,2,0,0},
	visual_layer_descriptorst{viewport_visual_layer::item,
		visual_render_groupst::item,true,false,1,0,0},
	visual_layer_descriptorst{viewport_visual_layer::designation,
		visual_render_groupst::designation,false,true,8,0,0}
	};

constexpr bool valid_visual_layer_descriptors()
{
	uint16_t draw_orders=0;
	for(size_t i=0;i<visual_layer_descriptors.size();++i)
		{
		const auto &descriptor=visual_layer_descriptors[i];
		if(static_cast<size_t>(descriptor.layer)!=i||
			descriptor.draw_order>=visual_layer_descriptors.size()||
			(draw_orders&(1U<<descriptor.draw_order)))return false;
		draw_orders|=uint16_t(1U<<descriptor.draw_order);
		}
	return true;
}

static_assert(valid_visual_layer_descriptors());

constexpr const visual_layer_descriptorst &visual_layer_descriptor(
	viewport_visual_layer layer)
{
	return visual_layer_descriptors[static_cast<size_t>(layer)];
}

constexpr viewport_visual_layer visual_layer_at_draw_order(uint8_t draw_order)
{
	for(const auto &descriptor:visual_layer_descriptors)
		if(descriptor.draw_order==draw_order)return descriptor.layer;
	return viewport_visual_layer::count;
}

constexpr visual_render_groupst visual_render_group(viewport_visual_layer layer)
{
	return visual_layer_descriptor(layer).render_group;
}

constexpr bool visual_layer_moves_independently(viewport_visual_layer layer)
{
	return visual_layer_descriptor(layer).moves_independently;
}

constexpr bool visual_layer_tracks_own_movement(viewport_visual_layer layer)
{
	const auto &descriptor=visual_layer_descriptor(layer);
	return descriptor.moves_independently||
		descriptor.render_group==visual_render_groupst::designation;
}

constexpr bool visual_layer_matches(
	viewport_visual_layer layer,
	int32_t current,
	int32_t previous)
{
	return visual_layer_descriptor(layer).matches_any_previous?
		previous!=0:
		previous==current;
}

struct viewport_visual_animation_inputst
{
	const void *viewport=nullptr;
	int32_t dim_x=0;
	int32_t dim_y=0;
	uint64_t context_revision=0;
	std::array<const int32_t *,static_cast<size_t>(viewport_visual_layer::count)> current{};
	std::array<const int32_t *,static_cast<size_t>(viewport_visual_layer::count)> previous{};
	// Optional dense terrain buffers. Scroll landing is detected from these when available, with
	// the sparse visual layers retained as a fallback for the dependency-free test harness and any
	// future renderer that does not expose a background layer.
	const int32_t *current_background=nullptr;
	const int32_t *previous_background=nullptr;
	// Current map-scroll offset (window_x/window_y). A pure pan does not bump context_revision.
	// Only a hint: it changes at input time, the buffers shift on a later render frame.
	int32_t pan_x=0;
	int32_t pan_y=0;

	bool valid() const
		{
		if(viewport==nullptr||dim_x<=0||dim_y<=0)return false;
		if(uint64_t(dim_x)*uint64_t(dim_y)>
			uint64_t(std::numeric_limits<int32_t>::max()))return false;
		for(size_t layer=0;layer<current.size();++layer)
			{
			if(current[layer]==nullptr||previous[layer]==nullptr)return false;
			}
		return true;
		}
};

using visual_movement_idst=uint64_t;
constexpr visual_movement_idst no_visual_movement=0;

struct visual_movement_renderst
{
	bool active=false;
	float source_x=0.0f;
	float source_y=0.0f;
	float progress=1.0f;
	bool inherited=false;
	visual_movement_idst movement_id=no_visual_movement;
};

struct visual_scroll_renderst
{
	bool pending=false;
	bool landed=false;
	bool abandoned=false;
	int32_t landed_x=0;
	int32_t landed_y=0;
	int32_t pending_x=0;
	int32_t pending_y=0;
	visual_movement_idst follow_candidate=no_visual_movement;
};

struct visual_follow_renderst
{
	bool active=false;
	visual_movement_idst movement_id=no_visual_movement;
	// Inverse of the proxy's displacement from its target, in tiles. Adding this to the camera
	// origin keeps the proxy fixed on screen while the world moves underneath it.
	float offset_x=0.0f;
	float offset_y=0.0f;
};

constexpr uint32_t default_movement_duration_ms=100;
constexpr float default_game_fps=100.0f;
constexpr uint32_t max_movement_cadence_baselines=4;
constexpr double camera_tau_ms=35.0;
constexpr double max_camera_offset_tiles=0.99;
constexpr double camera_drag_rest_limit_tiles=1.5;
constexpr double camera_drag_correction_tau_ms=120.0;

struct camera_drag_axisst
{
	double rest_tiles=0.0;
	double correction_px=0.0;
};

struct landed_scroll_axisst
{
	int32_t self=0;
	int32_t gameplay=0;
};

inline landed_scroll_axisst attribute_self_scroll_axis(
	int32_t self_scroll,
	int32_t landed)
{
	int32_t self=0;
	if(self_scroll!=0&&landed!=0&&(self_scroll>0)==(landed>0))
		self=std::abs(int64_t(self_scroll))<=std::abs(int64_t(landed))?
			self_scroll:
			landed;
	return {self,landed-self};
}

inline bool valid_camera_offset(double x,double y)
{
	return std::isfinite(x)&&std::isfinite(y)&&
		x>=-max_camera_offset_tiles&&x<=max_camera_offset_tiles&&
		y>=-max_camera_offset_tiles&&y<=max_camera_offset_tiles;
}

inline camera_drag_axisst constrain_camera_drag_axis(
	double requested_rest_tiles,
	double correction_px,
	double tile_size_px)
{
	if(!std::isfinite(requested_rest_tiles)||!std::isfinite(correction_px)||
		!std::isfinite(tile_size_px)||tile_size_px<=0.0)return {};
	const double rest_tiles=std::clamp(
		requested_rest_tiles,-camera_drag_rest_limit_tiles,camera_drag_rest_limit_tiles);
	return {
		rest_tiles,
		correction_px+(requested_rest_tiles-rest_tiles)*tile_size_px
		};
}

inline camera_drag_axisst persist_camera_drag_axis(
	double rest_tiles,
	double correction_px,
	double tile_size_px)
{
	if(!std::isfinite(rest_tiles)||!std::isfinite(correction_px)||
		!std::isfinite(tile_size_px)||tile_size_px<=0.0)return {};
	return {rest_tiles+correction_px/tile_size_px,0.0};
}

inline double decay_camera_drag_correction(double correction_px,uint32_t delta_ms)
{
	if(!std::isfinite(correction_px))return 0.0;
	return correction_px*std::exp(-double(delta_ms)/camera_drag_correction_tau_ms);
}

inline double camera_drag_anchor(
	double content_window_tiles,
	double rest_tiles,
	double transient_px,
	double correction_px,
	double follow_px,
	double tile_size_px)
{
	if(!std::isfinite(content_window_tiles)||!std::isfinite(rest_tiles)||
		!std::isfinite(transient_px)||!std::isfinite(correction_px)||
		!std::isfinite(follow_px)||!std::isfinite(tile_size_px)||tile_size_px<=0.0)
		return 0.0;
	return content_window_tiles-rest_tiles-
		(transient_px+correction_px+follow_px)/tile_size_px;
}

inline double camera_transient_after_landing(
	double existing_px,
	uint32_t delta_ms,
	double landed_px)
{
	if(!std::isfinite(existing_px)||!std::isfinite(landed_px))return 0.0;
	return existing_px*std::exp(-double(delta_ms)/camera_tau_ms)+landed_px;
}

inline uint32_t movement_duration_for_fps(float game_fps)
{
	if(!std::isfinite(game_fps)||game_fps<=0.0f)return default_movement_duration_ms;
	const double duration=double(default_movement_duration_ms)*default_game_fps/game_fps;
	return uint32_t(std::llround(std::clamp(duration,1.0,double(UINT32_MAX))));
}

inline bool valid_movement_cadence(uint32_t interval_ms,uint32_t fallback_duration_ms)
{
	return interval_ms!=0&&uint64_t(interval_ms)<=
		uint64_t(fallback_duration_ms)*max_movement_cadence_baselines;
}

inline float animation_progress(
	uint32_t now_ms,
	uint32_t start_time_ms,
	uint32_t duration_ms)
{
	return std::min(1.0f,float(now_ms-start_time_ms)/duration_ms);
}

inline bool visual_moved_between_tiles(
	viewport_visual_layer layer,
	const int32_t *current,
	const int32_t *previous,
	int32_t source,
	int32_t target)
{
	return previous[target]==0&&current[source]==0&&
		(layer==viewport_visual_layer::designation||previous[source]!=0);
}

inline int32_t inherited_visual_source_tile(
	int32_t overlay_target,
	float center_source,
	float center_target)
{
	return overlay_target+int32_t(std::lround(center_source-center_target));
}

enum class visual_facingst : int8_t
{
	east=0,
	west=1
};

// DF creature art faces west, so only east needs flipping. Also the default and cleared value.
constexpr visual_facingst native_sprite_facing=visual_facingst::west;

// Sticky facing: only a horizontal component changes it.
constexpr visual_facingst facing_after_move(
	int32_t dx,
	visual_facingst previous)
{
	if(dx>0)return visual_facingst::east;
	if(dx<0)return visual_facingst::west;
	return previous;
}

// center_x is only -1, 0 or +1, so a creature is at most three columns wide here.
constexpr int32_t mirrored_tile_x(int32_t piece_x,int32_t anchor_x)
{
	return anchor_x-(piece_x-anchor_x);
}

class visual_animation_managerst
{
	struct movementst
	{
		visual_movement_idst id=no_visual_movement;
		viewport_visual_layer layer;
		int32_t texpos;
		float source_x;
		float source_y;
		int32_t target_x;
		int32_t target_y;
		uint32_t start_time_ms;
		uint32_t duration_ms;
	};

	struct movement_cadencest
	{
		viewport_visual_layer layer;
		int32_t texpos;
		int32_t tile_x;
		int32_t tile_y;
		uint32_t last_move_time_ms;
	};

	struct viewport_animationst
	{
		const void *viewport=nullptr;
		int32_t dim_x=0;
		int32_t dim_y=0;
		uint64_t context_revision=0;
		bool has_context=false;
		bool seen=false;
		std::vector<movementst> movements;
		// Persists after an animation expires so the next step can reveal this visual's cadence.
		std::vector<movement_cadencest> movement_cadences;
		// One facing per tile, not per unit: the viewport exposes one creature texpos per tile.
		std::vector<int8_t> facing;
		// Stationary mirrored creatures are repainted every frame; this is the cheap pre-check.
		bool has_mirrored=false;
		int32_t pan_x=0;
		int32_t pan_y=0;
		bool has_pan=false;
		// Window scrolls not yet observed in the buffers, oldest first.
		// A signed total would cancel on a reversing drag while both shifts are still owed.
		std::vector<std::array<int32_t,2>> pending;
		// Redraws no prefix has matched.
		int32_t pending_frames=0;
		// Redraws spent waiting for the buffers to move at all.
		int32_t pending_age=0;
		// Redraws left in which new-movement detection stays suppressed after scroll activity.
		int32_t suppress_frames=0;
		// Buffer contents last seen, to recognize a repeat of them.
		uint64_t buffer_signature=0;
		bool has_buffer_signature=false;
		// Set while the previous buffer still belongs to a view that has been left behind.
		bool previous_view_stale=false;
		bool landed_this_frame=false;
		bool abandoned_this_frame=false;
		std::array<int32_t,2> landed_shift{};
		visual_movement_idst follow_candidate=no_visual_movement;
	};

	uint32_t frame_time_ms=0;
	uint32_t frame_delta_ms=0;
	uint32_t frame_movement_duration_ms=default_movement_duration_ms;
	bool has_frame=false;
	bool force_full_redraw=false;
	visual_movement_idst next_movement_id=1;
	std::vector<viewport_animationst> viewports;

	// Scrolling faster than detection keeps up: give up rather than test ever more prefixes.
	static constexpr size_t max_pending_shifts=8;
	static constexpr int32_t max_pending_shift_debt=6;
	// Bounds the wait on a scroll that never lands, so suppression cannot stick forever.
	static constexpr int32_t max_pending_age_frames=120;

	static void clear_pending(viewport_animationst &state)
		{
		state.pending.clear();
		state.pending_frames=0;
		state.pending_age=0;
		}

	static std::array<int32_t,2> pending_total(const viewport_animationst &state)
		{
		std::array<int64_t,2> total{};
		for(const auto &shift:state.pending)
			{
			total[0]+=shift[0];
			total[1]+=shift[1];
			}
		return {
			int32_t(std::clamp(total[0],int64_t(INT32_MIN),int64_t(INT32_MAX))),
			int32_t(std::clamp(total[1],int64_t(INT32_MIN),int64_t(INT32_MAX)))
			};
		}

	static int32_t saturated_pan_delta(int32_t current,int32_t previous)
		{
		return int32_t(std::clamp(
			int64_t(current)-previous,int64_t(INT32_MIN),int64_t(INT32_MAX)));
		}

	static void abandon_pending(viewport_animationst &state)
		{
		state.movements.clear();
		state.movement_cadences.clear();
		clear_pending(state);
		}

	static void reset_facing(viewport_animationst &state)
		{
		std::fill(
			state.facing.begin(),
			state.facing.end(),
			int8_t(native_sprite_facing));
		state.has_mirrored=false;
		}

	static void reset_tracking(viewport_animationst &state)
		{
		abandon_pending(state);
		state.suppress_frames=0;
		}

	// Identifies the buffer contents this frame, to tell a redrawn viewport from a repeated one.
	static uint64_t compute_buffer_signature(const viewport_visual_animation_inputst &input)
		{
		// FNV-1a. Only ever compared against the previous frame's value, never stored.
		constexpr uint64_t fnv_offset_basis=0xcbf29ce484222325ULL;
		constexpr uint64_t fnv_prime=0x100000001b3ULL;
		uint64_t hash=fnv_offset_basis;
		const int32_t tile_count=input.dim_x*input.dim_y;
		if(input.current_background!=nullptr&&input.previous_background!=nullptr)
			{
			for(int32_t i=0;i<tile_count;++i)
				{
				hash=(hash^uint64_t(uint32_t(input.current_background[i])))*fnv_prime;
				hash=(hash^uint64_t(uint32_t(input.previous_background[i])))*fnv_prime;
				}
			}
		for(size_t layer=0;layer<input.current.size();++layer)
			{
			if(!visual_layer_tracks_own_movement(
				static_cast<viewport_visual_layer>(layer)))continue;
			for(int32_t i=0;i<tile_count;++i)
				{
				hash=(hash^uint64_t(uint32_t(input.current[layer][i])))*fnv_prime;
				hash=(hash^uint64_t(uint32_t(input.previous[layer][i])))*fnv_prime;
				}
			}
		return hash;
		}

	// Fraction of tracked sprites consistent with a buffer shift: current[x]==previous[x+dwx].
	// Negative when there is nothing to compare.
	static double background_shift_match_ratio(
		const viewport_visual_animation_inputst &input,
		int32_t dwx,
		int32_t dwy)
		{
		if(input.current_background==nullptr||input.previous_background==nullptr)return -1.0;
		int32_t considered=0;
		int32_t matches=0;
		for(int32_t x=0;x<input.dim_x;++x)
			{
			const int32_t sx=x+dwx;
			if(sx<0||sx>=input.dim_x)continue;
			for(int32_t y=0;y<input.dim_y;++y)
				{
				const int32_t sy=y+dwy;
				if(sy<0||sy>=input.dim_y)continue;
				const int32_t value=input.current_background[x*input.dim_y+y];
				if(value==0)continue;
				++considered;
				if(input.previous_background[sx*input.dim_y+sy]==value)++matches;
				}
			}
		if(considered==0)return -1.0;
		return double(matches)/double(considered);
		}

	static double visual_shift_match_ratio(
		const viewport_visual_animation_inputst &input,
		int32_t dwx,
		int32_t dwy)
		{
		int32_t considered=0;
		int32_t matches=0;
		for(size_t layer=0;layer<input.current.size();++layer)
			{
			const auto id=static_cast<viewport_visual_layer>(layer);
			if(!visual_layer_tracks_own_movement(id))continue;
			// A layer matching any non-zero previous carries no position, so it would vote for
			// every hypothesis and carry an unapplied scroll over the bar.
			if(visual_layer_descriptor(id).matches_any_previous)continue;
			const int32_t *current=input.current[layer];
			const int32_t *previous=input.previous[layer];
			for(int32_t x=0;x<input.dim_x;++x)
				{
				const int32_t sx=x+dwx;
				if(sx<0||sx>=input.dim_x)continue;
				for(int32_t y=0;y<input.dim_y;++y)
					{
					const int32_t texpos=current[x*input.dim_y+y];
					if(texpos==0)continue;
					const int32_t sy=y+dwy;
					if(sy<0||sy>=input.dim_y)continue;
					++considered;
					if(visual_layer_matches(id,texpos,previous[sx*input.dim_y+sy]))++matches;
					}
				}
			}
		if(considered==0)return -1.0;
		return double(matches)/double(considered);
		}

	static double scroll_shift_match_ratio(
		const viewport_visual_animation_inputst &input,
		int32_t dwx,
		int32_t dwy,
		bool &used_background)
		{
		const double background=background_shift_match_ratio(input,dwx,dwy);
		used_background=background>=0.0;
		return used_background?
			background:
			visual_shift_match_ratio(input,dwx,dwy);
		}

	static std::array<int32_t,2> shared_movement_delta(
		const int32_t *current,
		const int32_t *previous,
		int32_t dim_x,
		int32_t dim_y)
		{
		std::array<int32_t,2> best{};
		int32_t best_count=1;
		bool ambiguous=false;
		for(int32_t dx=-1;dx<=1;++dx)
			{
			for(int32_t dy=-1;dy<=1;++dy)
				{
				if(dx==0&&dy==0)continue;
				int32_t count=0;
				for(int32_t x=0;x<dim_x;++x)
					{
					const int32_t source_x=x+dx;
					if(source_x<0||source_x>=dim_x)continue;
					for(int32_t y=0;y<dim_y;++y)
						{
						const int32_t source_y=y+dy;
						if(source_y<0||source_y>=dim_y)continue;
						const int32_t target=x*dim_y+y;
						const int32_t texpos=current[target];
						if(texpos!=0&&previous[target]!=texpos&&
							previous[source_x*dim_y+source_y]==texpos)++count;
						}
					}
				if(count>best_count)
					{
					best_count=count;
					best={dx,dy};
					ambiguous=false;
					}
				else if(count==best_count&&count>1)ambiguous=true;
				}
			}
		return ambiguous?std::array<int32_t,2>{}:best;
		}

	viewport_animationst &get_viewport(const viewport_visual_animation_inputst &input)
		{
		for(viewport_animationst &state:viewports)
			{
			if(state.viewport==input.viewport)return state;
			}
		viewports.emplace_back();
		viewports.back().viewport=input.viewport;
		return viewports.back();
		}

	float movement_progress(const movementst &movement) const
		{
		return animation_progress(
			frame_time_ms,movement.start_time_ms,movement.duration_ms);
		}

	visual_movement_idst allocate_movement_id()
		{
		const visual_movement_idst id=next_movement_id++;
		if(next_movement_id==no_visual_movement)next_movement_id=1;
		return id;
		}

	public:
		visual_animation_managerst()=default;

		void begin_frame(
			uint32_t now_ms,
			uint32_t movement_duration_ms=default_movement_duration_ms)
			{
			movement_duration_ms=std::max(1U,movement_duration_ms);
			if(has_frame&&frame_movement_duration_ms!=movement_duration_ms)
				{
				for(viewport_animationst &state:viewports)
					state.movement_cadences.clear();
				}
			frame_delta_ms=has_frame?now_ms-frame_time_ms:0;
			frame_time_ms=now_ms;
			frame_movement_duration_ms=movement_duration_ms;
			has_frame=true;
			force_full_redraw=false;
			// Keep one final full redraw when the last movement expires.
			for(viewport_animationst &state:viewports)
				{
				state.seen=false;
				state.landed_this_frame=false;
				state.abandoned_this_frame=false;
				state.landed_shift={};
				state.follow_candidate=no_visual_movement;
				if(!state.movements.empty())force_full_redraw=true;
				}
			}

		void synchronize_viewport(const viewport_visual_animation_inputst &input)
			{
			if(input.viewport==nullptr)return;
			viewport_animationst &state=get_viewport(input);
			state.seen=true;

			if(!input.valid())
				{
				reset_tracking(state);
				state.has_context=false;
				state.has_mirrored=false;
				return;
				}

			// Only a replaced view leaves a previous buffer belonging somewhere else; a first
			// sighting does not.
			const bool view_switched=state.has_context&&
				(state.context_revision!=input.context_revision||
				state.dim_x!=input.dim_x||state.dim_y!=input.dim_y);
			const bool context_changed=!state.has_context||view_switched;
			// The scroll delta is queued here as a hint; the buffers are hypothesis-tested each
			// frame to find where it lands. Detection stays suppressed until then: a shifted
			// buffer makes every panned creature look like a real move.
			if(state.has_pan&&(state.pan_x!=input.pan_x||state.pan_y!=input.pan_y))
				{
				if(state.pending.size()>=max_pending_shifts)
					{
					// Same give-up as the other two sites: the owed shifts are unknowable now.
					abandon_pending(state);
					reset_facing(state);
					state.abandoned_this_frame=true;
					}
				state.pending.push_back(
					{saturated_pan_delta(input.pan_x,state.pan_x),
						saturated_pan_delta(input.pan_y,state.pan_y)});
				const auto debt=pending_total(state);
				if(std::abs(int64_t(debt[0]))>max_pending_shift_debt||
					std::abs(int64_t(debt[1]))>max_pending_shift_debt)
					{
					abandon_pending(state);
					reset_facing(state);
					state.abandoned_this_frame=true;
					}
				state.pending_frames=0;
				state.suppress_frames=2;
				}
			state.context_revision=input.context_revision;
			state.dim_x=input.dim_x;
			state.dim_y=input.dim_y;
			state.has_context=true;
			state.pan_x=input.pan_x;
			state.pan_y=input.pan_y;
			state.has_pan=true;
			if(context_changed)
				{
				state.facing.assign(
					size_t(input.dim_x)*size_t(input.dim_y),
					int8_t(native_sprite_facing));
				state.has_mirrored=false;
				}
			// This hook runs per frame; the viewport is recomputed only when it changes, and while
			// paused hardly at all. Re-reading a landed scroll steps every sprite by a tile.
			const uint64_t signature=compute_buffer_signature(input);
			const bool buffers_advanced=!state.has_buffer_signature||
				state.buffer_signature!=signature;
			state.buffer_signature=signature;
			state.has_buffer_signature=true;

			if(context_changed)
				{
				// Skips the recompute sweep, so clear has_mirrored here or a stale true survives.
				state.has_mirrored=false;
				reset_tracking(state);
				// window_z, zoom and resize change at input time; the buffers cross later.
				// This reset covers only the input frame, not the crossing itself.
				if(view_switched)state.previous_view_stale=true;
				return;
				}

			// On the crossing frame `current` is the new view and `previous` the old one, so a
			// sprite on each side, a tile apart, reads as one that moved between them.
			const bool crossed_views=buffers_advanced&&state.previous_view_stale;
			if(buffers_advanced)state.previous_view_stale=false;
			// The new view is drawn at the current window, so a queued scroll is already in it.
			// Left queued it would never match, and suppress everything until it aged out.
			if(crossed_views)clear_pending(state);

			bool translated=false;
			std::array<int32_t,2> landed_shift{};
			bool pending_testable=buffers_advanced;
			bool repeated_landing_allowed=false;
			if(!pending_testable&&!state.pending.empty())
				{
				bool static_used_background=false;
				const double static_ratio=scroll_shift_match_ratio(
					input,0,0,static_used_background);
				pending_testable=static_ratio<0.0||static_ratio>=
					(static_used_background?0.6:0.5);
				// A uniform background can match a shift before DF swaps buffers. Retiring it
				// early is safe only when no sparse visual can contradict that hypothesis.
				repeated_landing_allowed=static_used_background&&static_ratio>=0.6&&
					visual_shift_match_ratio(input,0,0)<0.0;
				}
			if(pending_testable&&!crossed_views&&!state.pending.empty())
				{
				// Queued scrolls land in order and may coalesce. A single announced jump may also
				// reach the buffers piecemeal, so test every partial value of the next ordered event.
				// Prefer the strongest match; an equal score retires the shortest ordered prefix.
				struct landing_candidatest
					{
					std::array<int32_t,2> shift{};
					size_t full_events=0;
					std::array<int32_t,2> partial{};
					int32_t applied_components=0;
					double score=-1.0;
					bool valid=false;
					};
				landing_candidatest best;
				bool any_data=false;
				std::array<int32_t,2> base{};
				int32_t base_components=0;
				for(size_t event_index=0;event_index<state.pending.size();++event_index)
					{
					const auto event=state.pending[event_index];
					const int32_t step_x=(event[0]>0)-(event[0]<0);
					const int32_t step_y=(event[1]>0)-(event[1]<0);
					for(int32_t ix=0;ix<=std::abs(event[0]);++ix)
						{
						for(int32_t iy=0;iy<=std::abs(event[1]);++iy)
							{
							if(ix==0&&iy==0)continue;
							const std::array<int32_t,2> partial={ix*step_x,iy*step_y};
							const std::array<int32_t,2> shift=
								{base[0]+partial[0],base[1]+partial[1]};
							// A reversing prefix that nets to zero carries no observable position.
							if(shift[0]==0&&shift[1]==0)continue;
							bool used_background=false;
							const double ratio=scroll_shift_match_ratio(
								input,shift[0],shift[1],used_background);
							if(ratio<0.0)continue;
							any_data=true;
							const double threshold=used_background?0.6:0.5;
							const int32_t applied=base_components+ix+iy;
							if(ratio<threshold||
								(!buffers_advanced&&!repeated_landing_allowed)||
								(best.valid&&ratio<best.score)||
								(best.valid&&ratio==best.score&&
									applied>=best.applied_components))continue;
							const bool complete=ix==std::abs(event[0])&&
								iy==std::abs(event[1]);
							best={
								shift,
								event_index+(complete?1:0),
								complete?std::array<int32_t,2>{}:partial,
								applied,
								ratio,
								true
								};
							}
						}
					base[0]+=event[0];
					base[1]+=event[1];
					base_components+=std::abs(event[0])+std::abs(event[1]);
					}
				if(!any_data)
					{
					// Nothing visible to anchor the test on: nothing to animate either.
					abandon_pending(state);
					reset_facing(state);
					state.abandoned_this_frame=true;
					}
				else if(best.valid)
					{
					// Re-anchor in-flight movements and drop anything scrolled off-screen.
					const int32_t dwx=best.shift[0];
					const int32_t dwy=best.shift[1];
					state.movements.erase(
						std::remove_if(
							state.movements.begin(),
							state.movements.end(),
							[&](movementst &movement)
								{
								movement.source_x-=dwx;
								movement.source_y-=dwy;
								movement.target_x-=dwx;
								movement.target_y-=dwy;
								return movement.target_x<0||movement.target_x>=input.dim_x||
									movement.target_y<0||movement.target_y>=input.dim_y;
								}),
						state.movements.end());
					state.movement_cadences.erase(
						std::remove_if(
							state.movement_cadences.begin(),
							state.movement_cadences.end(),
							[&](movement_cadencest &cadence)
								{
								cadence.tile_x-=dwx;
								cadence.tile_y-=dwy;
								return cadence.tile_x<0||cadence.tile_x>=input.dim_x||
									cadence.tile_y<0||cadence.tile_y>=input.dim_y;
								}),
						state.movement_cadences.end());
					// Facing describes creatures still on screen, so translate it rather than drop it.
					if(state.facing.size()==
						size_t(input.dim_x)*size_t(input.dim_y))
						{
						std::vector<int8_t> shifted(
							state.facing.size(),int8_t(native_sprite_facing));
						for(int32_t x=0;x<input.dim_x;++x)
							{
							const int32_t sx=x+dwx;
							if(sx<0||sx>=input.dim_x)continue;
							for(int32_t y=0;y<input.dim_y;++y)
								{
								const int32_t sy=y+dwy;
								if(sy<0||sy>=input.dim_y)continue;
								shifted[x*input.dim_y+y]=
									state.facing[sx*input.dim_y+sy];
								}
							}
						state.facing.swap(shifted);
						}
					state.pending.erase(state.pending.begin(),
						state.pending.begin()+std::ptrdiff_t(best.full_events));
					if(best.partial[0]!=0||best.partial[1]!=0)
						{
						state.pending.front()[0]-=best.partial[0];
						state.pending.front()[1]-=best.partial[1];
						if(state.pending.front()[0]==0&&state.pending.front()[1]==0)
							state.pending.erase(state.pending.begin());
						}
					state.pending_frames=0;
					state.pending_age=0;
					// The scroll is accounted for; the settle window must not block the rebased pass.
					state.suppress_frames=0;
					landed_shift=best.shift;
					translated=true;
					state.landed_this_frame=true;
					state.landed_shift=best.shift;
					}
				else
					{
					bool static_used_background=false;
					const double static_ratio=scroll_shift_match_ratio(
						input,0,0,static_used_background);
					const double static_threshold=static_used_background?0.6:0.5;
					if(static_ratio>=static_threshold)
						{
						if(++state.pending_age<=max_pending_age_frames)
							{
							// The buffers have not moved yet, so the scroll is still in flight.
							state.pending_frames=0;
							}
						else
							{
							abandon_pending(state);
							reset_facing(state);
							state.suppress_frames=2;
							state.abandoned_this_frame=true;
							}
						}
					else if(++state.pending_frames>4)
						{
						// The shift never showed up recognizably: fall back to the safe reset.
						abandon_pending(state);
						reset_facing(state);
						state.suppress_frames=2;
						state.abandoned_this_frame=true;
						}
					}
				}

			const bool suppress=(!buffers_advanced&&!translated)||crossed_views||
				(!translated&&!state.pending.empty())||state.suppress_frames>0;
			// The countdown measures redraws, not frames, so a repeated viewport must not spend it.
			if(buffers_advanced&&state.suppress_frames>0)--state.suppress_frames;

			// On the landing frame `previous` is still framed on the pre-scroll view.
			// Rebasing it by the landed delta keeps a creature that walked during the scroll.
			auto previous_layers=input.previous;
			std::vector<std::vector<int32_t>> rebased_previous;
			if(translated&&!suppress)
				{
				rebased_previous.resize(input.previous.size());
				for(size_t layer=0;layer<input.previous.size();++layer)
					{
					if(!visual_layer_tracks_own_movement(
						static_cast<viewport_visual_layer>(layer)))continue;
					rebased_previous[layer].assign(
						size_t(input.dim_x)*size_t(input.dim_y),0);
					for(int32_t x=0;x<input.dim_x;++x)
						{
						const int32_t sx=x+landed_shift[0];
						if(sx<0||sx>=input.dim_x)continue;
						for(int32_t y=0;y<input.dim_y;++y)
							{
							const int32_t sy=y+landed_shift[1];
							if(sy<0||sy>=input.dim_y)continue;
							rebased_previous[layer][x*input.dim_y+y]=
								input.previous[layer][sx*input.dim_y+sy];
							}
						}
					previous_layers[layer]=rebased_previous[layer].data();
					}
				}
			if(!suppress)
				{
				const int32_t tile_count=input.dim_x*input.dim_y;
				std::vector<uint8_t> claimed_sources(tile_count);
				const size_t existing_movement_count=state.movements.size();
				const auto cadences_at_frame_start=state.movement_cadences;
				std::vector<uint8_t> claimed_cadences(cadences_at_frame_start.size());
				std::vector<movement_cadencest> new_cadences;
				// A chained movement's source may already have been rewritten this frame.
				const std::vector<int8_t> facing_at_frame_start=state.facing;
				// Source clears are deferred until every movement this frame is registered.
				// A source can be another movement's target in the same frame -- a chain.
				std::vector<uint8_t> facing_target_written;
				std::vector<int32_t> pending_facing_source_clears;
				long double best_follow_distance=std::numeric_limits<long double>::max();
				if(state.facing.size()==size_t(input.dim_x)*size_t(input.dim_y))
					facing_target_written.assign(state.facing.size(),0);
				for(size_t layer=0;layer<input.current.size();++layer)
					{
					if(!visual_layer_tracks_own_movement(
						static_cast<viewport_visual_layer>(layer)))continue;
					std::fill(claimed_sources.begin(),claimed_sources.end(),0);
					const int32_t *current=input.current[layer];
					const int32_t *previous=previous_layers[layer];
					const auto shared_delta=
						static_cast<viewport_visual_layer>(layer)==viewport_visual_layer::center?
						shared_movement_delta(
							current,previous,input.dim_x,input.dim_y):
						std::array<int32_t,2>{};
					for(int32_t x=0;x<input.dim_x;++x)
						{
						for(int32_t y=0;y<input.dim_y;++y)
							{
							const int32_t target=x*input.dim_y+y;
							const int32_t texpos=current[target];
							if(texpos==0)continue;
							if(static_cast<viewport_visual_layer>(layer)==
								viewport_visual_layer::item&&
								previous_layers[static_cast<size_t>(
									viewport_visual_layer::center)][target]!=0)continue;

							int32_t source=-1;
							int32_t candidate_count=0;
							if(shared_delta[0]!=0||shared_delta[1]!=0)
								{
								const int32_t source_x=x+shared_delta[0];
								const int32_t source_y=y+shared_delta[1];
								if(source_x>=0&&source_x<input.dim_x&&
									source_y>=0&&source_y<input.dim_y)
									{
									const int32_t candidate=source_x*input.dim_y+source_y;
									if(!claimed_sources[candidate]&&previous[target]!=texpos&&
										previous[candidate]==texpos)
										{
										source=candidate;
										candidate_count=1;
										}
									}
								}
							// Otherwise require a unique same-sprite move between empty cells.
							if(candidate_count==0&&previous[target]==0)
								{
								for(int32_t dx=-1;dx<=1;++dx)
									{
									for(int32_t dy=-1;dy<=1;++dy)
										{
										if(dx==0&&dy==0)continue;
										const int32_t source_x=x+dx;
										const int32_t source_y=y+dy;
										if(source_x<0||source_x>=input.dim_x||
											source_y<0||source_y>=input.dim_y)continue;
										const int32_t candidate=source_x*input.dim_y+source_y;
										if(!claimed_sources[candidate]&&
											visual_layer_matches(
												static_cast<viewport_visual_layer>(layer),
												texpos,
												previous[candidate])&&
											current[candidate]==0)
											{
											source=candidate;
											++candidate_count;
											}
										}
									}
								}
							if(candidate_count!=1)continue;

							claimed_sources[source]=1;
							const auto visual_layer=static_cast<viewport_visual_layer>(layer);
							const int32_t source_x=source/input.dim_y;
							const int32_t source_y=source%input.dim_y;
							size_t cadence_source=0;
							int32_t cadence_count=0;
							for(size_t i=0;i<cadences_at_frame_start.size();++i)
								{
								const auto &cadence=cadences_at_frame_start[i];
								if(claimed_cadences[i]||cadence.layer!=visual_layer||
									cadence.tile_x!=source_x||cadence.tile_y!=source_y||
									previous[source]==0||
									!visual_layer_matches(
										visual_layer,previous[source],cadence.texpos))continue;
								cadence_source=i;
								++cadence_count;
								}
							uint32_t movement_duration_ms=frame_movement_duration_ms;
							if(cadence_count==1)
								{
								claimed_cadences[cadence_source]=1;
								const uint32_t interval_ms=frame_time_ms-
									cadences_at_frame_start[cadence_source].last_move_time_ms;
								if(valid_movement_cadence(
									interval_ms,frame_movement_duration_ms))
									movement_duration_ms=interval_ms;
								}
							new_cadences.push_back(
								{visual_layer,texpos,x,y,frame_time_ms});
							float visual_source_x=float(source/input.dim_y);
							float visual_source_y=float(source%input.dim_y);
							for(size_t i=0;i<existing_movement_count;++i)
								{
								const movementst &movement=state.movements[i];
								if(movement.layer!=
										static_cast<viewport_visual_layer>(layer)||
									movement.target_x!=visual_source_x||
									movement.target_y!=visual_source_y)continue;
								const float progress=movement_progress(movement);
								visual_source_x=movement.source_x+
									(movement.target_x-movement.source_x)*progress;
								visual_source_y=movement.source_y+
									(movement.target_y-movement.source_y)*progress;
								break;
								}
							const visual_movement_idst movement_id=allocate_movement_id();
							state.movements.push_back(
								{
								movement_id,
								visual_layer,
								texpos,
								visual_source_x,
								visual_source_y,
								x,
								y,
								frame_time_ms,
								movement_duration_ms
								});
							if(translated&&visual_layer==viewport_visual_layer::center&&
								x-source_x==landed_shift[0]&&y-source_y==landed_shift[1])
								{
								const long double centered_x=
									static_cast<long double>(2)*x-(input.dim_x-1);
								const long double centered_y=
									static_cast<long double>(2)*y-(input.dim_y-1);
								const long double distance=
									centered_x*centered_x+centered_y*centered_y;
								// x/y iteration order supplies the deterministic coordinate tie-break.
								if(distance<best_follow_distance)
									{
									best_follow_distance=distance;
									state.follow_candidate=movement_id;
									}
								}
							if(static_cast<viewport_visual_layer>(layer)==
									viewport_visual_layer::center&&
								state.facing.size()==
									size_t(input.dim_x)*size_t(input.dim_y)&&
								!facing_at_frame_start.empty()&&
								facing_at_frame_start.size()==state.facing.size())
								{
								const int32_t source_tile_x=source/input.dim_y;
								const int32_t target_index=x*input.dim_y+y;
								state.facing[target_index]=int8_t(
									facing_after_move(
										x-source_tile_x,
										static_cast<visual_facingst>(
											facing_at_frame_start[source])));
								facing_target_written[size_t(target_index)]=1;
								pending_facing_source_clears.push_back(source);
								}
							}
						}
						}
				// A source vacates its tile only if no movement this frame claimed it as a target.
				if(!facing_target_written.empty())
					{
					for(int32_t pending_source:pending_facing_source_clears)
						{
						if(!facing_target_written[size_t(pending_source)])
							state.facing[size_t(pending_source)]=
								int8_t(native_sprite_facing);
						}
					}
				std::vector<movement_cadencest> retained_cadences;
				retained_cadences.reserve(
					cadences_at_frame_start.size()+new_cadences.size());
				const uint64_t max_cadence_age=uint64_t(frame_movement_duration_ms)*
					max_movement_cadence_baselines;
				for(size_t i=0;i<cadences_at_frame_start.size();++i)
					{
					if(claimed_cadences[i])continue;
					const auto &cadence=cadences_at_frame_start[i];
					const size_t layer=static_cast<size_t>(cadence.layer);
					const int32_t tile=cadence.tile_x*input.dim_y+cadence.tile_y;
					if(uint64_t(frame_time_ms-cadence.last_move_time_ms)>max_cadence_age||
						input.current[layer][tile]==0||
						!visual_layer_matches(
							cadence.layer,input.current[layer][tile],cadence.texpos))continue;
					retained_cadences.push_back(cadence);
					}
				for(const auto &cadence:new_cadences)
					{
					retained_cadences.erase(
						std::remove_if(
							retained_cadences.begin(),
							retained_cadences.end(),
							[&](const movement_cadencest &existing)
								{
								return existing.layer==cadence.layer&&
									existing.tile_x==cadence.tile_x&&
									existing.tile_y==cadence.tile_y;
								}),
						retained_cadences.end());
					retained_cadences.push_back(cadence);
					}
				state.movement_cadences=std::move(retained_cadences);
				}
			state.movements.erase(
				std::remove_if(
					state.movements.begin(),
					state.movements.end(),
					[&](const movementst &movement)
						{
						const size_t layer=static_cast<size_t>(movement.layer);
						const int32_t target=movement.target_x*input.dim_y+movement.target_y;
						const int32_t current=input.current[layer][target];
						return frame_time_ms-movement.start_time_ms>=movement.duration_ms||
							current==0||
							!visual_layer_matches(movement.layer,current,movement.texpos);
						}),
				state.movements.end());
			// has_mirrored is recomputed here rather than maintained at every write site.
			if(state.facing.size()==size_t(input.dim_x)*size_t(input.dim_y))
				{
				const int32_t *center_current=
					input.current[static_cast<size_t>(
						viewport_visual_layer::center)];
				bool any_mirrored=false;
				for(size_t i=0;i<state.facing.size();++i)
					{
					if(center_current[i]==0)
						state.facing[i]=int8_t(native_sprite_facing);
					else if(state.facing[i]!=int8_t(native_sprite_facing))
						any_mirrored=true;
					}
				state.has_mirrored=any_mirrored;
				}
			if(!state.movements.empty())force_full_redraw=true;
			}

		void end_frame()
			{
			viewports.erase(
				std::remove_if(
					viewports.begin(),
					viewports.end(),
					[](const viewport_animationst &state){return !state.seen;}),
				viewports.end());
			for(const viewport_animationst &state:viewports)
				{
				if(!state.movements.empty())force_full_redraw=true;
				}
			}

		uint32_t get_frame_time_ms() const
			{
			return frame_time_ms;
			}

		uint32_t get_frame_delta_ms() const
			{
			return frame_delta_ms;
			}

		visual_scroll_renderst get_scroll(const void *viewport) const
			{
			for(const viewport_animationst &state:viewports)
				{
				if(state.viewport!=viewport)continue;
				const auto pending=pending_total(state);
				return {
					!state.pending.empty(),
					state.landed_this_frame,
					state.abandoned_this_frame,
					state.landed_shift[0],
					state.landed_shift[1],
					pending[0],
					pending[1],
					state.follow_candidate
					};
				}
			return {};
			}

		visual_follow_renderst get_follow(
			const void *viewport,
			visual_movement_idst movement_id) const
			{
			if(movement_id==no_visual_movement)return {};
			for(const viewport_animationst &state:viewports)
				{
				if(state.viewport!=viewport)continue;
				for(const movementst &movement:state.movements)
					{
					if(movement.id!=movement_id)continue;
					const float remaining=1.0f-movement_progress(movement);
					return {
						true,
						movement.id,
						(movement.target_x-movement.source_x)*remaining,
						(movement.target_y-movement.source_y)*remaining
						};
					}
				break;
				}
			return {};
			}

		visual_facingst get_facing(
			const void *viewport,
			int32_t x,
			int32_t y) const
			{
			for(const viewport_animationst &state:viewports)
				{
				if(state.viewport!=viewport)continue;
				if(x<0||x>=state.dim_x||y<0||y>=state.dim_y)break;
				const size_t index=size_t(x)*size_t(state.dim_y)+size_t(y);
				if(index>=state.facing.size())break;
				return static_cast<visual_facingst>(state.facing[index]);
				}
			return native_sprite_facing;
			}

		bool has_mirrored_facing(const void *viewport) const
			{
			for(const viewport_animationst &state:viewports)
				{
				if(state.viewport!=viewport)continue;
				return state.has_mirrored;
				}
			return false;
			}

		bool requires_full_redraw() const
			{
			return force_full_redraw;
			}

		visual_movement_renderst get_movement(
			const void *viewport,
			viewport_visual_layer layer,
			int32_t target_x,
			int32_t target_y) const
			{
			for(const viewport_animationst &state:viewports)
				{
				if(state.viewport!=viewport)continue;
				const movementst *companion=nullptr;
				bool ambiguous=false;
				for(const movementst &movement:state.movements)
					{
					if(movement.layer==layer&&movement.target_x==target_x&&
						movement.target_y==target_y)
						{
						return {
							true,
							movement.source_x,
							movement.source_y,
							movement_progress(movement),
							false,
							movement.id
							};
						}
					if(layer==viewport_visual_layer::vehicle||
						layer==viewport_visual_layer::center||
						movement.layer!=viewport_visual_layer::center||
						std::abs(movement.target_x-target_x)>1||
						std::abs(movement.target_y-target_y)>1)continue;
					if(companion!=nullptr&&
						(companion->source_x-companion->target_x!=
							movement.source_x-movement.target_x||
						companion->source_y-companion->target_y!=
							movement.source_y-movement.target_y||
						companion->start_time_ms!=movement.start_time_ms||
						companion->duration_ms!=movement.duration_ms))
						ambiguous=true;
					else if(companion==nullptr)
						companion=&movement;
					}
				if(ambiguous)return {};
				if(companion!=nullptr)
					return {
						true,
						target_x+companion->source_x-companion->target_x,
						target_y+companion->source_y-companion->target_y,
						movement_progress(*companion),
						true,
						companion->id
						};
				break;
				}
			return {};
			}
};

#endif
