// SPDX-License-Identifier: MIT

#ifndef VISUAL_ANIMATION_H
#define VISUAL_ANIMATION_H

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <vector>

enum class viewport_creature_layer : uint8_t
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

struct viewport_visual_animation_inputst
{
	const void *viewport=nullptr;
	int32_t dim_x=0;
	int32_t dim_y=0;
	uint64_t context_revision=0;
	std::array<const int32_t *,static_cast<size_t>(viewport_creature_layer::count)> current{};
	std::array<const int32_t *,static_cast<size_t>(viewport_creature_layer::count)> previous{};
	// Current map-scroll offset (window_x/window_y). A pure pan does not bump context_revision.
	// The scroll value is only a HINT: it changes at input time, while the viewport buffers shift
	// on a later render frame. The manager hypothesis-tests the buffers to find the frame the
	// shift actually lands, translates in-flight movements on that frame (so sprites track the
	// scrolled world), and detects new movements SHIFT-AWARE on that frame -- otherwise the
	// shifted buffers make every panned creature look like a real move and it slides across the
	// screen (the "floating sprites while jiggling the camera" bug).
	int32_t pan_x=0;
	int32_t pan_y=0;
	// Optional scroll-landing oracle: the background layer (current and last frame). The creature
	// layers alone cannot confirm a landing when the visible creatures move WITH the camera --
	// which is the normal case in adventure mode, where the camera follows the player on every
	// step. Background content is world-anchored, so it identifies the frame (and, under fast
	// scrolling, the partial prefix) a pending scroll actually lands. May be left null: the
	// manager then falls back to the creature-layer majority test with blanket suppression.
	const int32_t *background=nullptr;
	const int32_t *background_old=nullptr;

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

inline bool visual_moved_between_tiles(
	const int32_t *current,
	const int32_t *previous,
	int32_t source,
	int32_t target)
{
	return previous[source]!=0&&previous[target]==0&&current[source]==0;
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
			viewport_creature_layer layer;
			int32_t texpos;
			int32_t x;
			int32_t y;
			uint32_t start_time_ms;
		};

	private:
	struct movementst
	{
		viewport_creature_layer layer;
		int32_t texpos;
		float source_x;
		float source_y;
		int32_t target_x;
		int32_t target_y;
		uint32_t start_time_ms;
	};

	struct viewport_animationst
	{
		const void *viewport;
		int32_t dim_x;
		int32_t dim_y;
		uint64_t context_revision;
		bool has_context;
		bool seen;
		std::vector<movementst> movements;
		std::vector<pinnedst> pinned;
		int32_t pan_x;
		int32_t pan_y;
		bool has_pan;
		// Window-scroll delta not yet observed in the buffers, and how long it has been pending.
		int32_t pending_dx;
		int32_t pending_dy;
		int32_t pending_frames;
		// Frames left in which new-movement detection stays suppressed after scroll activity.
		int32_t suppress_frames;
		// Frames whose PREVIOUS buffer is a window excursion's off-view render: comparing
		// against it fabricates movements, so detection and content expiry sit out.
		int32_t untrusted_frames;
	};

	uint32_t frame_time_ms;
	uint32_t frame_delta_ms;
	bool has_frame;
	bool force_full_redraw;
	std::vector<viewport_animationst> viewports;

	public:
		// Diagnostic counters, readable via the console status command.
		struct statisticst
		{
			uint64_t movements=0;    // movements detected and started
			uint64_t landings=0;     // frames a scroll landing was attributed
			uint64_t suppressed=0;   // frames detection was suppressed
			uint64_t resets=0;       // safe resets (unattributable buffers)
			uint64_t pan_frames=0;   // frames a new pan delta was observed
			uint64_t static_frames=0;    // pending scroll, background static (prefix 0 best)
			uint64_t identical_frames=0; // pending scroll, background EXACTLY equals _old
			uint64_t unrecognized_frames=0;  // pending scroll, no prefix qualifies
			uint64_t absorbed=0;     // scroll deltas written off: one-frame window
			                         // excursions that reversed, and masked/phantom
			                         // scrolls that never appeared as a buffer diff
			int32_t last_pending_dx=0;   // live pending of the last-synchronized viewport
			int32_t last_pending_dy=0;
			int32_t last_shift_x=0;      // scroll landed THIS synchronize call (drives the
			int32_t last_shift_y=0;      // renderer's world-tile slide)
		};
		statisticst stats;

	private:

	static constexpr uint32_t movement_duration_ms=100;

	static bool independently_moving(viewport_creature_layer layer)
		{
		return layer==viewport_creature_layer::center||
			layer==viewport_creature_layer::vehicle||
			layer==viewport_creature_layer::item;
		}

	static bool same_visual(
		viewport_creature_layer layer,
		int32_t current,
		int32_t previous)
		{
		return layer==viewport_creature_layer::vehicle?
			previous!=0:
			previous==current;
		}

	viewport_animationst &get_viewport(const viewport_visual_animation_inputst &input)
		{
		for(viewport_animationst &state:viewports)
			{
			if(state.viewport==input.viewport)return state;
			}
		viewports.push_back({input.viewport,0,0,0,false,false,{},{},0,0,false,0,0,0,0,0});
		return viewports.back();
		}

	float movement_progress(uint32_t start_time_ms) const
		{
		const float linear=std::min(1.0f,float(frame_time_ms-start_time_ms)/movement_duration_ms);
		return linear*linear*(3.0f-2.0f*linear);
		}

	// Match ratio of "buffers shifted by (dwx,dwy)" on the background layer: 0..1, or -1 when
	// there is nothing to compare (empty background).
	static double background_shift_match(
		const viewport_visual_animation_inputst &input,
		int32_t dwx,
		int32_t dwy)
		{
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

	// Re-anchor in-flight movements to the viewport frame after a landed scroll of (dwx,dwy) and
	// drop anything scrolled off-screen.
	void translate_movements(viewport_animationst &state,int32_t dwx,int32_t dwy)
		{
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
					return movement.target_x<0||movement.target_x>=state.dim_x||
						movement.target_y<0||movement.target_y>=state.dim_y;
					}),
			state.movements.end());
		}

	public:
		visual_animation_managerst()
			{
			frame_time_ms=0;
			frame_delta_ms=0;
			has_frame=false;
			force_full_redraw=false;
			}

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

		void synchronize_viewport(const viewport_visual_animation_inputst &input,bool allow_new_movements)
			{
			if(input.viewport==nullptr)return;
			viewport_animationst &state=get_viewport(input);
			state.seen=true;

			if(!input.valid())
				{
				state.movements.clear();
				state.has_context=false;
				return;
				}

			const bool context_changed=!state.has_context||
				state.context_revision!=input.context_revision||
				state.dim_x!=input.dim_x||state.dim_y!=input.dim_y;
			// A pure map scroll is followable, but window_x/window_y change at INPUT time while the
			// viewport buffers shift on a later render frame. So the scroll delta is only accumulated
			// here as a pending hint; the buffers themselves are hypothesis-tested each frame to find
			// the frame the shift really lands. On that frame in-flight movements are translated so
			// they track the scrolled world. While a pan is pending (and briefly after), new-movement
			// detection is suppressed: a shifted buffer makes every panned creature look like a
			// "unique same-sprite move between empty cells", which is exactly the bogus 100ms slide
			// that made sprites float when the camera moved.
			if(state.has_pan&&(state.pan_x!=input.pan_x||state.pan_y!=input.pan_y))
				{
				const bool was_excursion=
					std::abs(state.pending_dx)>6||std::abs(state.pending_dy)>6;
				state.pending_dx+=input.pan_x-state.pan_x;
				state.pending_dy+=input.pan_y-state.pan_y;
				state.pending_frames=0;
				state.suppress_frames=2;
				++stats.pan_frames;
				if(was_excursion&&
					std::abs(state.pending_dx)<=6&&std::abs(state.pending_dy)<=6)
					{
					++stats.absorbed;   // the excursion reversed: net delta back in range
					// This frame's PREVIOUS buffer is still the excursion's off-view
					// render; give it one frame to flush before trusting comparisons.
					state.untrusted_frames=1;
					}
				}
			state.context_revision=input.context_revision;
			state.dim_x=input.dim_x;
			state.dim_y=input.dim_y;
			state.has_context=true;
			state.pan_x=input.pan_x;
			state.pan_y=input.pan_y;
			state.has_pan=true;
			if(context_changed||!allow_new_movements)
				{
				state.movements.clear();
				state.pinned.clear();
				state.pending_dx=0;
				state.pending_dy=0;
				state.pending_frames=0;
				state.suppress_frames=0;
				state.untrusted_frames=0;
				return;
				}

			// The buffer shift landed this frame (whole or, under fast scrolling, a prefix): new-
			// movement detection below compares current[x] against previous[x+shift]. A creature
			// that moved WITH the camera -- the adventure-mode player, whom the camera follows on
			// every step -- then shows its real one-tile world move and animates; a static
			// creature matches the shifted previous and stays quiet.
			int32_t shift_x=0;
			int32_t shift_y=0;
			bool translated=false;
			bool suppress_detection=false;
			const bool has_background=input.background!=nullptr&&input.background_old!=nullptr;
			if(state.pending_dx!=0||state.pending_dy!=0)
				{
				if(std::abs(state.pending_dx)>6||std::abs(state.pending_dy)>6)
					{
					// A jump this size is almost never a real scroll. The common case is a
					// one-frame window EXCURSION -- combat/announcement camera flicks point
					// the window elsewhere for a single frame and snap it back -- whose
					// reversal arrives a frame or two later and collapses the pending delta
					// to (nearly) zero on its own. Attribute nothing and reset nothing while
					// it is in flight: the buffers are untrusted (detection suppressed,
					// content expiry skipped), but in-flight movements and their clocks stay
					// alive, so the flick passes without a visible hitch. A delta that never
					// collapses is a genuine teleport: fall back to the safe reset.
					suppress_detection=true;
					if(++state.pending_frames>4)
						{
						++stats.resets;
						state.movements.clear();
						state.pinned.clear();
						state.pending_dx=0;
						state.pending_dy=0;
						state.pending_frames=0;
						state.suppress_frames=0;
						state.untrusted_frames=1;
						}
					}
				else if(has_background)
					{
					// Attribute the scroll on the background layer: find the LARGEST applied
					// prefix of the pending delta (fast scrolling lands piecemeal). Ties between
					// qualifying prefixes only happen on uniform terrain, where mistiming is
					// invisible.
					const int32_t stepx=(state.pending_dx>0)-(state.pending_dx<0);
					const int32_t stepy=(state.pending_dy>0)-(state.pending_dy<0);
					int32_t best_ax=0,best_ay=0,best_mag=-1;
					double best_score=-1.0;
					bool no_data=false;
					for(int32_t ix=0;ix<=std::abs(state.pending_dx)&&!no_data;++ix)
						{
						for(int32_t iy=0;iy<=std::abs(state.pending_dy);++iy)
							{
							const double score=
								background_shift_match(input,ix*stepx,iy*stepy);
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
						}
					if(no_data)
						{
						// Empty background: nothing to attribute against, nothing to animate.
						++stats.resets;
						state.movements.clear();
						state.pinned.clear();
						state.pending_dx=0;
						state.pending_dy=0;
						state.pending_frames=0;
						}
					else if(best_mag>0)
						{
						translate_movements(state,best_ax,best_ay);
						state.pending_dx-=best_ax;
						state.pending_dy-=best_ay;
						state.pending_frames=0;
						shift_x=best_ax;
						shift_y=best_ay;
						translated=true;
						++stats.landings;
						}
					else if(best_mag==0)
						{
						// Content demonstrably hasn't moved yet; unshifted buffers are safe to
						// detect on. But don't wait forever: a scroll whose buffer transition
						// happened during an untrusted frame (an excursion) is MASKED -- it
						// will never show up as a diff -- and a delta that never renders at
						// all is phantom. Either way, absorb it silently after a grace period
						// instead of letting it poison later attributions.
						++stats.static_frames;
						bool identical=true;
						for(int32_t i=0;identical&&i<input.dim_x*input.dim_y;++i)
							{
							if(input.background[i]!=input.background_old[i])identical=false;
							}
						if(identical)++stats.identical_frames;
						if(++state.pending_frames>6)
							{
							state.pending_dx=0;
							state.pending_dy=0;
							state.pending_frames=0;
							++stats.absorbed;
							}
						}
					else
						{
						// Neither static nor any prefix recognizable (heavy simultaneous change):
						// the buffers can't be trusted this frame.
						suppress_detection=true;
						++stats.unrecognized_frames;
						if(++state.pending_frames>4)
							{
							++stats.resets;
							state.movements.clear();
							state.pending_dx=0;
							state.pending_dy=0;
							state.pending_frames=0;
							}
						}
					}
				else
					{
					// No background data supplied: legacy path. Test the whole pending delta over
					// the visible independently-moving sprites; a majority match means the shift
					// landed this frame. Detection stays suppressed while a pan is pending (and
					// briefly after), since these frames cannot be attributed precisely.
					const int32_t dwx=state.pending_dx;
					const int32_t dwy=state.pending_dy;
					int32_t considered=0;
					int32_t matches=0;
					for(size_t layer=0;layer<input.current.size();++layer)
						{
						if(!independently_moving(
							static_cast<viewport_creature_layer>(layer)))continue;
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
								if(same_visual(
									static_cast<viewport_creature_layer>(layer),
									texpos,
									previous[sx*input.dim_y+sy]))++matches;
								}
							}
						}
					if(considered==0)
						{
						// Nothing visible to anchor the test on: nothing to animate either.
						++stats.resets;
						state.movements.clear();
						state.pinned.clear();
						state.pending_dx=0;
						state.pending_dy=0;
						state.pending_frames=0;
						}
					else if(matches*2>=considered)
						{
						translate_movements(state,dwx,dwy);
						state.pending_dx=0;
						state.pending_dy=0;
						state.pending_frames=0;
						translated=true;
						++stats.landings;
						}
					else if(++state.pending_frames>4)
						{
						// The shift never showed up recognizably (heavy simultaneous movement,
						// culled render, ...): fall back to the safe reset behavior.
						++stats.resets;
						state.movements.clear();
						state.pinned.clear();
						state.pending_dx=0;
						state.pending_dy=0;
						state.pending_frames=0;
						}
					}
				}

			if(state.untrusted_frames>0)
				{
				suppress_detection=true;
				--state.untrusted_frames;
				}

			if(translated&&(shift_x!=0||shift_y!=0))
				{
				// Record screen-static sprites on every layer as PINNED: they ride the
				// camera, so the renderer draws them without the world-tile slide.
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
							if(!same_visual(
								static_cast<viewport_creature_layer>(layer),
								texpos,previous[index]))continue;
							bool found=false;
							for(pinnedst &pin:state.pinned)
								{
								if(pin.layer==
										static_cast<viewport_creature_layer>(layer)&&
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
									static_cast<viewport_creature_layer>(layer),
									texpos,
									x,
									y,
									frame_time_ms
									});
							}
						}
					}
				}
			// With a background oracle, every frame is precisely attributed (unshifted, or shifted
			// by the landed prefix), so detection runs shift-aware instead of being suppressed --
			// this is what animates the adventure-mode player, whose every step coincides with a
			// camera scroll. The legacy path keeps the blanket suppression.
			const bool suppress=has_background?
				suppress_detection:
				(translated||state.pending_dx!=0||state.pending_dy!=0||state.suppress_frames>0);
			if(state.suppress_frames>0)--state.suppress_frames;
			if(suppress)++stats.suppressed;
			if(!suppress)
				{
				const int32_t tile_count=input.dim_x*input.dim_y;
				std::vector<uint8_t> claimed_sources(tile_count);
				for(size_t layer=0;layer<input.current.size();++layer)
					{
					if(!independently_moving(
						static_cast<viewport_creature_layer>(layer)))continue;
					std::fill(claimed_sources.begin(),claimed_sources.end(),0);
					const int32_t *current=input.current[layer];
					const int32_t *previous=input.previous[layer];
					for(int32_t x=0;x<input.dim_x;++x)
						{
						for(int32_t y=0;y<input.dim_y;++y)
							{
							const int32_t target=x*input.dim_y+y;
							const int32_t texpos=current[target];
							if(texpos==0)continue;
							// A sprite at the same SCREEN position across a landed
							// scroll is carried by the camera (the adventure player):
							// never synthesize a movement for it -- the world-tile
							// slide moves the world under it instead.
							if(translated&&same_visual(
								static_cast<viewport_creature_layer>(layer),
								texpos,previous[target]))continue;
							// Entity ids are unavailable, so only accept a unique
							// same-sprite move between empty cells in one layer. The previous
							// buffer is read through this frame's landed shift; sprites scrolled
							// in from off-buffer have no history and never animate.
							const int32_t prev_x=x+shift_x;
							const int32_t prev_y=y+shift_y;
							if(prev_x<0||prev_x>=input.dim_x||
								prev_y<0||prev_y>=input.dim_y)continue;
							if(previous[prev_x*input.dim_y+prev_y]!=0)continue;
							// An item "appearing" where a creature stood last frame is
							// the creature UN-OCCLUDING it as it walks away, not an item
							// move -- otherwise walking along a line of similar items
							// reads as the items hopping from tile to tile behind the
							// walker.
							if(static_cast<viewport_creature_layer>(layer)==
								viewport_creature_layer::item&&
								input.previous[static_cast<size_t>(
									viewport_creature_layer::center)]
									[prev_x*input.dim_y+prev_y]!=0)continue;

							int32_t source=-1;
							int32_t candidate_count=0;
							for(int32_t dx=-1;dx<=1;++dx)
								{
								for(int32_t dy=-1;dy<=1;++dy)
									{
									if(dx==0&&dy==0)continue;
									const int32_t source_x=x+dx;
									const int32_t source_y=y+dy;
									if(source_x<0||source_x>=input.dim_x||
										source_y<0||source_y>=input.dim_y)continue;
									const int32_t source_prev_x=source_x+shift_x;
									const int32_t source_prev_y=source_y+shift_y;
									if(source_prev_x<0||source_prev_x>=input.dim_x||
										source_prev_y<0||source_prev_y>=input.dim_y)continue;
									const int32_t candidate=source_x*input.dim_y+source_y;
									if(!claimed_sources[candidate]&&
										same_visual(
											static_cast<viewport_creature_layer>(layer),
											texpos,
											previous[source_prev_x*input.dim_y+
												source_prev_y])&&
										current[candidate]==0)
										{
										source=candidate;
										++candidate_count;
										}
									}
								}
							if(candidate_count!=1)continue;

							claimed_sources[source]=1;
							++stats.movements;
							float visual_source_x=float(source/input.dim_y);
							float visual_source_y=float(source%input.dim_y);
							for(const movementst &movement:state.movements)
								{
								if(movement.layer!=
										static_cast<viewport_creature_layer>(layer)||
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
								static_cast<viewport_creature_layer>(layer),
								texpos,
								visual_source_x,
								visual_source_y,
								x,
								y,
								frame_time_ms
								});
							}
						}
					}
				}
			state.movements.erase(
				std::remove_if(
					state.movements.begin(),
					state.movements.end(),
					[&](const movementst &movement)
						{
						if(frame_time_ms-movement.start_time_ms>=movement_duration_ms)
							return true;
						// Content checks only on trusted frames: during a window excursion
						// the whole buffer is elsewhere for a frame, and expiring against it
						// would wipe every animation at each combat camera flick.
						if(suppress_detection)return false;
						const size_t layer=static_cast<size_t>(movement.layer);
						const int32_t target=movement.target_x*input.dim_y+movement.target_y;
						const int32_t current=input.current[layer][target];
						return current==0||
							!same_visual(movement.layer,current,movement.texpos);
						}),
				state.movements.end());
			state.pinned.erase(
				std::remove_if(
					state.pinned.begin(),
					state.pinned.end(),
					[&](const pinnedst &pin)
						{
						if(frame_time_ms-pin.start_time_ms>=movement_duration_ms)
							return true;
						if(suppress_detection)return false;
						const size_t layer=static_cast<size_t>(pin.layer);
						const int32_t index=pin.x*input.dim_y+pin.y;
						const int32_t current=input.current[layer][index];
						return current==0||!same_visual(pin.layer,current,pin.texpos);
						}),
				state.pinned.end());

			stats.last_shift_x=translated?shift_x:0;
			stats.last_shift_y=translated?shift_y:0;
			stats.last_pending_dx=state.pending_dx;
			stats.last_pending_dy=state.pending_dy;
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

		bool requires_full_redraw() const
			{
			return force_full_redraw;
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
			viewport_creature_layer layer,
			int32_t target_x,
			int32_t target_y) const
			{
			for(const viewport_animationst &state:viewports)
				{
				if(state.viewport!=viewport)continue;
				const movementst *companion=nullptr;
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
					if(layer==viewport_creature_layer::vehicle||
						layer==viewport_creature_layer::center||
						movement.layer!=viewport_creature_layer::center||
						std::abs(movement.target_x-target_x)>1||
						std::abs(movement.target_y-target_y)>1)continue;
					if(companion!=nullptr&&
						(companion->source_x-companion->target_x!=
							movement.source_x-movement.target_x||
						companion->source_y-companion->target_y!=
							movement.source_y-movement.target_y||
						companion->start_time_ms!=movement.start_time_ms))
						return {};
					companion=&movement;
					}
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
