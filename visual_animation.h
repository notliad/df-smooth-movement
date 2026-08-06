// SPDX-License-Identifier: MIT

#ifndef VISUAL_ANIMATION_H
#define VISUAL_ANIMATION_H

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
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
	// Current map-scroll offset (window_x/window_y). A pure pan does not bump context_revision.
	// Only a hint: it changes at input time, the buffers shift on a later render frame.
	int32_t pan_x=0;
	int32_t pan_y=0;
	// Optional scroll-landing oracle: the background layer (current and last frame). The
	// visual layers alone cannot confirm a landing when the visible sprites move WITH the
	// camera -- the normal case in adventure mode, where the camera follows the player on
	// every step. Background content is world-anchored, so it identifies the frame (and,
	// under fast scrolling, the partial prefix) a pending scroll actually lands. May be
	// left null: the manager then falls back to the layer majority test with suppression.
	const int32_t *background=nullptr;
	const int32_t *background_old=nullptr;
	// The renderer only draws pinned sprites under the world-tile slide; when something
	// else owns smoothing (the fortress free camera), skip the per-landing pin scan.
	bool want_pinned=true;

	bool valid() const
		{
		if(viewport==nullptr||dim_x<=0||dim_y<=0)return false;
		for(size_t layer=0;layer<current.size();++layer)
			{
			if(current[layer]==nullptr||previous[layer]==nullptr)return false;
			}
		return true;
		}
};

struct visual_movement_renderst
{
	bool active=false;
	float source_x=0.0f;
	float source_y=0.0f;
	float progress=1.0f;
	bool inherited=false;
};

inline float animation_progress(
	uint32_t now_ms,
	uint32_t start_time_ms,
	uint32_t duration_ms)
{
	const float linear=std::min(
		1.0f,float(now_ms-start_time_ms)/duration_ms);
	return linear*linear*(3.0f-2.0f*linear);
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
	public:
		// A sprite that stayed at the same SCREEN position across a landed scroll: it is
		// being carried by the camera (the adventure-mode player, primarily). The renderer
		// pins these -- draws them WITHOUT the world-tile slide -- so the world visibly
		// moves under them instead of them snapping ahead of it.
		struct pinnedst
		{
			viewport_visual_layer layer;
			int32_t texpos;
			int32_t x;
			int32_t y;
			uint32_t start_time_ms;
		};

		// Diagnostic counters, readable via the console status command.
		struct statisticst
		{
			uint64_t movements=0;    // movements detected and started
			uint64_t landings=0;     // frames a scroll landing was attributed
			uint64_t suppressed=0;   // frames detection was suppressed
			uint64_t resets=0;       // safe resets (unattributable buffers)
			uint64_t pan_frames=0;   // frames a new pan delta was observed
			uint64_t static_frames=0;    // pending scroll, background static
			uint64_t identical_frames=0; // pending scroll, background EXACTLY equals _old
			uint64_t unrecognized_frames=0;  // pending scroll, no prefix qualifies
			uint64_t absorbed=0;     // deltas written off: reversed excursions and
			                         // masked/phantom scrolls that never diffed
			int32_t last_pending_dx=0;
			int32_t last_pending_dy=0;
			int32_t last_shift_x=0;  // scroll landed on the last synchronize call of the
			int32_t last_shift_y=0;  // frame -- diagnostic only, see get_last_shift
		};
		statisticst stats;

	private:
	struct movementst
	{
		viewport_visual_layer layer;
		int32_t texpos;
		float source_x;
		float source_y;
		int32_t target_x;
		int32_t target_y;
		uint32_t start_time_ms;
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
		std::vector<pinnedst> pinned;
		// The scroll that landed in THIS viewport's buffers on the current synchronize call.
		// Per-viewport rather than global: lower z-level viewports are synchronized too, and
		// the renderer's glide compensation must read the main viewport's landing specifically,
		// not whichever viewport happened to be synchronized last.
		int32_t last_shift_x=0;
		int32_t last_shift_y=0;
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
	};

	uint32_t frame_time_ms=0;
	uint32_t frame_delta_ms=0;
	bool has_frame=false;
	bool force_full_redraw=false;
	std::vector<viewport_animationst> viewports;

	static constexpr uint32_t movement_duration_ms=100;
	// Scrolling faster than detection keeps up: give up rather than test ever more prefixes.
	static constexpr size_t max_pending_shifts=8;
	// Bounds the wait on a scroll that never lands, so suppression cannot stick forever.
	static constexpr int32_t max_pending_age_frames=120;

	static void clear_pending(viewport_animationst &state)
		{
		state.pending.clear();
		state.pending_frames=0;
		state.pending_age=0;
		}

	static void abandon_pending(viewport_animationst &state)
		{
		state.movements.clear();
		state.pinned.clear();
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
		// The background oracle, when supplied, is world-anchored, and it is what the landing
		// test actually reads. Sprite layers alone can be byte-identical across a real scroll --
		// the adventure camera follows the player, so the player rides along unmoved and may be
		// the only sprite on screen. Fingerprinting the sprites only would report "buffers
		// unchanged" for precisely the frames a landing needs to be attributed on. Terrain lives
		// in the same viewport recompute, so this does not make the detector fire any more often
		// than DF actually redraws.
		if(input.background!=nullptr&&input.background_old!=nullptr)
			{
			for(int32_t i=0;i<tile_count;++i)
				{
				hash=(hash^uint64_t(uint32_t(input.background[i])))*fnv_prime;
				hash=(hash^uint64_t(uint32_t(input.background_old[i])))*fnv_prime;
				}
			}
		return hash;
		}

	// Fraction of tracked sprites consistent with a buffer shift: current[x]==previous[x+dwx].
	// Negative when there is nothing to compare.
	static double shift_match_ratio(
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

	// Match ratio for a hypothesized shift: prefers the background-layer oracle when supplied
	// (adventure mode -- the visible sprites move WITH the camera there, so the sprite-only
	// test above cannot tell a landed scroll from one still pending), falling back to the
	// sprite majority test otherwise (fortress mode, where no background oracle is given).
	static double landing_match_ratio(
		const viewport_visual_animation_inputst &input,
		int32_t dwx,
		int32_t dwy)
		{
		if(input.background==nullptr||input.background_old==nullptr)
			return shift_match_ratio(input,dwx,dwy);
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
				const int32_t cur=input.background[x*input.dim_y+y];
				if(cur==0)continue;
				++considered;
				if(input.background_old[sx*input.dim_y+sy]==cur)++matches;
				}
			}
		if(considered==0)return -1.0;
		return double(matches)/double(considered);
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

	float movement_progress(uint32_t start_time_ms) const
		{
		return animation_progress(
			frame_time_ms,start_time_ms,movement_duration_ms);
		}

	public:
		visual_animation_managerst()=default;

		void begin_frame(uint32_t now_ms)
			{
			frame_delta_ms=has_frame?now_ms-frame_time_ms:0;
			frame_time_ms=now_ms;
			has_frame=true;
			force_full_redraw=false;
			// Keep one final full redraw when the last movement expires.
			for(viewport_animationst &state:viewports)
				{
				state.seen=false;
				if(!state.movements.empty())force_full_redraw=true;
				}
			}

		void synchronize_viewport(const viewport_visual_animation_inputst &input)
			{
			stats.last_shift_x=0;
			stats.last_shift_y=0;
			if(input.viewport==nullptr)return;
			viewport_animationst &state=get_viewport(input);
			state.seen=true;
			// Cleared up front rather than only on the paths that reach the landing test below:
			// the renderer keys its glide compensation off this, and an early return that left
			// the PREVIOUS frame's landing standing would compensate the same scroll twice.
			state.last_shift_x=0;
			state.last_shift_y=0;

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
					}
				state.pending.push_back(
					{input.pan_x-state.pan_x,input.pan_y-state.pan_y});
				state.pending_frames=0;
				state.suppress_frames=2;
				++stats.pan_frames;
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
			if(buffers_advanced&&!crossed_views&&!state.pending.empty())
				{
				// Queued scrolls land in order and may coalesce, so each hypothesis is a prefix.
				// Shortest first: over-retiring leaves owed shifts to be read as movement.
				std::array<int32_t,2> landed{};
				size_t landed_count=0;
				bool any_data=false;
				std::array<int32_t,2> shift{};
				for(size_t count=1;count<=state.pending.size();++count)
					{
					shift[0]+=state.pending[count-1][0];
					shift[1]+=state.pending[count-1][1];
					// A prefix netting to zero is indistinguishable from "nothing landed yet".
					// Accepting it would retire shifts the buffers have still to apply.
					if(shift[0]==0&&shift[1]==0)continue;
					const double ratio=landing_match_ratio(input,shift[0],shift[1]);
					// Emptiness is per-prefix: a long one can push every sprite out of range
					// while a shorter one still has something to say.
					if(ratio<0.0)continue;
					any_data=true;
					if(ratio>=0.5)
						{
						landed=shift;
						landed_count=count;
						break;
						}
					}
				if(!any_data)
					{
					// Nothing visible to anchor the test on: nothing to animate either.
					++stats.resets;
					abandon_pending(state);
					reset_facing(state);
					}
				else if(landed_count>0)
					{
					// Re-anchor in-flight movements and drop anything scrolled off-screen.
					const int32_t dwx=landed[0];
					const int32_t dwy=landed[1];
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
					state.pending.erase(
						state.pending.begin(),
						state.pending.begin()+std::ptrdiff_t(landed_count));
					state.pending_frames=0;
					state.pending_age=0;
					// The scroll is accounted for; the settle window must not block the rebased pass.
					state.suppress_frames=0;
					landed_shift=landed;
					translated=true;
					++stats.landings;
					}
				else if(landing_match_ratio(input,0,0)>=0.5&&
					++state.pending_age<=max_pending_age_frames)
					{
					// The buffers have not moved yet, so the scroll is still in flight.
					state.pending_frames=0;
					++stats.static_frames;
					}
				else if(++state.pending_frames>4)
					{
					// The shift never showed up recognizably: fall back to the safe reset.
					++stats.resets;
					abandon_pending(state);
					// The delta was never identified, so the grid cannot be translated.
					reset_facing(state);
					// It may yet land, so do not resume detection on the very next redraw.
					state.suppress_frames=2;
					}
				}

			const bool suppress=!buffers_advanced||crossed_views||
				!state.pending.empty()||state.suppress_frames>0;
			// The countdown measures redraws, not frames, so a repeated viewport must not spend it.
			if(buffers_advanced&&state.suppress_frames>0)--state.suppress_frames;
			if(suppress)++stats.suppressed;

			if(translated&&(landed_shift[0]!=0||landed_shift[1]!=0)&&!suppress&&
				input.want_pinned)
				{
				// Record screen-static sprites on every layer as PINNED: they ride the
				// camera (the adventure player), so the renderer draws them without the
				// world-tile slide.
				for(size_t layer=0;layer<input.current.size();++layer)
					{
					const int32_t *current=input.current[layer];
					const int32_t *previous=input.previous[layer];
					for(int32_t x=0;x<input.dim_x;++x)
						{
						for(int32_t y=0;y<input.dim_y;++y)
							{
							const int32_t index=x*input.dim_y+y;
							const int32_t texpos=current[index];
							if(texpos==0)continue;
							if(!visual_layer_matches(
								static_cast<viewport_visual_layer>(layer),
								texpos,previous[index]))continue;
							bool found=false;
							for(pinnedst &pin:state.pinned)
								{
								if(pin.layer==
										static_cast<viewport_visual_layer>(layer)&&
									pin.x==x&&pin.y==y)
									{
									pin.texpos=texpos;
									pin.start_time_ms=frame_time_ms;
									found=true;
									break;
									}
								}
							if(!found)
								state.pinned.push_back(
									{
									static_cast<viewport_visual_layer>(layer),
									texpos,
									x,
									y,
									frame_time_ms
									});
							}
						}
					}
				}

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
				// Sprites pinned THIS frame rode the camera: they held the same SCREEN tile
				// across the landing, and the renderer draws them without the world slide. The
				// rebased previous buffer above reframes exactly those as having moved by
				// -landed_shift, so left alone they would also be detected as a movement --
				// drawn twice, and animated sliding against the world they are anchored to.
				std::vector<uint16_t> pinned_this_frame;
				for(const pinnedst &pin:state.pinned)
					{
					if(pin.start_time_ms!=frame_time_ms)continue;
					if(pinned_this_frame.empty())
						pinned_this_frame.assign(size_t(tile_count),0);
					pinned_this_frame[size_t(pin.x)*size_t(input.dim_y)+size_t(pin.y)]|=
						uint16_t(1U<<static_cast<size_t>(pin.layer));
					}
				std::vector<uint8_t> claimed_sources(tile_count);
				const size_t existing_movement_count=state.movements.size();
				// A chained movement's source may already have been rewritten this frame.
				const std::vector<int8_t> facing_at_frame_start=state.facing;
				// Source clears are deferred until every movement this frame is registered.
				// A source can be another movement's target in the same frame -- a chain.
				std::vector<uint8_t> facing_target_written;
				std::vector<int32_t> pending_facing_source_clears;
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
							if(!pinned_this_frame.empty()&&
								(pinned_this_frame[size_t(target)]&
									uint16_t(1U<<layer))!=0)continue;
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
							float visual_source_x=float(source/input.dim_y);
							float visual_source_y=float(source%input.dim_y);
							for(size_t i=0;i<existing_movement_count;++i)
								{
								const movementst &movement=state.movements[i];
								if(movement.layer!=
										static_cast<viewport_visual_layer>(layer)||
									movement.target_x!=visual_source_x||
									movement.target_y!=visual_source_y)continue;
								const float progress=
									movement_progress(movement.start_time_ms);
								visual_source_x=movement.source_x+
									(movement.target_x-movement.source_x)*progress;
								visual_source_y=movement.source_y+
									(movement.target_y-movement.source_y)*progress;
								break;
								}
							state.movements.push_back(
								{
								static_cast<viewport_visual_layer>(layer),
								texpos,
								visual_source_x,
								visual_source_y,
								x,
								y,
								frame_time_ms
								});
							++stats.movements;
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
						return frame_time_ms-movement.start_time_ms>=movement_duration_ms||
							current==0||
							!visual_layer_matches(movement.layer,current,movement.texpos);
						}),
				state.movements.end());
			state.pinned.erase(
				std::remove_if(
					state.pinned.begin(),
					state.pinned.end(),
					[&](const pinnedst &pin)
						{
						const size_t layer=static_cast<size_t>(pin.layer);
						const int32_t index=pin.x*input.dim_y+pin.y;
						const int32_t current=input.current[layer][index];
						return frame_time_ms-pin.start_time_ms>=movement_duration_ms||
							current==0||
							!visual_layer_matches(pin.layer,current,pin.texpos);
						}),
				state.pinned.end());
			state.last_shift_x=translated?landed_shift[0]:0;
			state.last_shift_y=translated?landed_shift[1]:0;
			stats.last_shift_x=state.last_shift_x;
			stats.last_shift_y=state.last_shift_y;
			{
			int32_t pending_sum_x=0,pending_sum_y=0;
			for(const auto &shift:state.pending)
				{
				pending_sum_x+=shift[0];
				pending_sum_y+=shift[1];
				}
			stats.last_pending_dx=pending_sum_x;
			stats.last_pending_dy=pending_sum_y;
			}
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

		// The scroll that landed in this viewport's buffers this frame, {0,0} if none did.
		std::array<int32_t,2> get_last_shift(const void *viewport) const
			{
			for(const viewport_animationst &state:viewports)
				{
				if(state.viewport==viewport)
					return {state.last_shift_x,state.last_shift_y};
				}
			return {0,0};
			}

		static inline const std::vector<pinnedst> empty_pinned{};

		const std::vector<pinnedst> &get_pinned(const void *viewport) const
			{
			for(const viewport_animationst &state:viewports)
				{
				if(state.viewport==viewport)return state.pinned;
				}
			return empty_pinned;
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
							movement_progress(movement.start_time_ms)
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
						companion->start_time_ms!=movement.start_time_ms))
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
						movement_progress(companion->start_time_ms),
						true
						};
				break;
				}
			return {};
			}
};

#endif
