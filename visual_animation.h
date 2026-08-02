// SPDX-License-Identifier: MIT

#ifndef VISUAL_ANIMATION_H
#define VISUAL_ANIMATION_H

#include <algorithm>
#include <array>
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
};

constexpr std::array visual_layer_descriptors=
	{
	visual_layer_descriptorst{viewport_visual_layer::right,
		visual_render_groupst::main,false,false,3},
	visual_layer_descriptorst{viewport_visual_layer::center,
		visual_render_groupst::main,true,false,0},
	visual_layer_descriptorst{viewport_visual_layer::left,
		visual_render_groupst::main,false,false,4},
	visual_layer_descriptorst{viewport_visual_layer::upright,
		visual_render_groupst::upper,false,false,5},
	visual_layer_descriptorst{viewport_visual_layer::up,
		visual_render_groupst::upper,false,false,6},
	visual_layer_descriptorst{viewport_visual_layer::upleft,
		visual_render_groupst::upper,false,false,7},
	visual_layer_descriptorst{viewport_visual_layer::vehicle,
		visual_render_groupst::vehicle,true,true,2},
	visual_layer_descriptorst{viewport_visual_layer::item,
		visual_render_groupst::item,true,false,1},
	visual_layer_descriptorst{viewport_visual_layer::designation,
		visual_render_groupst::designation,false,true,8}
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
	// The scroll value is only a HINT: it changes at input time, while the viewport buffers shift
	// on a later render frame. The manager hypothesis-tests the buffers to find the frame the
	// shift actually lands, translates in-flight movements on that frame (so sprites track the
	// scrolled world), and suppresses new-movement detection while a pan is pending -- otherwise
	// the shifted buffers make every panned creature look like a real move and it slides across
	// the screen (the "floating sprites while jiggling the camera" bug).
	int32_t pan_x=0;
	int32_t pan_y=0;

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
	viewport_visual_layer layer,
	const int32_t *current,
	const int32_t *previous,
	int32_t source,
	int32_t target)
{
	return previous[target]==0&&current[source]==0&&
		(layer==viewport_visual_layer::designation||previous[source]!=0);
}

class visual_animation_managerst
{
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
		int32_t pan_x=0;
		int32_t pan_y=0;
		bool has_pan=false;
		// Window-scroll delta not yet observed in the buffers, and how long it has been pending.
		int32_t pending_dx=0;
		int32_t pending_dy=0;
		int32_t pending_frames=0;
		// Frames left in which new-movement detection stays suppressed after scroll activity.
		int32_t suppress_frames=0;
	};

	uint32_t frame_time_ms=0;
	uint32_t frame_delta_ms=0;
	bool has_frame=false;
	bool force_full_redraw=false;
	std::vector<viewport_animationst> viewports;

	static constexpr uint32_t movement_duration_ms=100;

	static void clear_pending(viewport_animationst &state)
		{
		state.pending_dx=0;
		state.pending_dy=0;
		state.pending_frames=0;
		}

	static void abandon_pending(viewport_animationst &state)
		{
		state.movements.clear();
		clear_pending(state);
		}

	static void reset_tracking(viewport_animationst &state)
		{
		abandon_pending(state);
		state.suppress_frames=0;
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
		const float linear=std::min(1.0f,float(frame_time_ms-start_time_ms)/movement_duration_ms);
		return linear*linear*(3.0f-2.0f*linear);
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

		void synchronize_viewport(const viewport_visual_animation_inputst &input,bool allow_new_movements)
			{
			if(input.viewport==nullptr)return;
			viewport_animationst &state=get_viewport(input);
			state.seen=true;

			if(!input.valid())
				{
				reset_tracking(state);
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
				state.pending_dx+=input.pan_x-state.pan_x;
				state.pending_dy+=input.pan_y-state.pan_y;
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
			if(context_changed||!allow_new_movements)
				{
				reset_tracking(state);
				return;
				}

			bool translated=false;
			if(state.pending_dx!=0||state.pending_dy!=0)
				{
				// Did the buffers apply the pending scroll? After a scroll of (dwx,dwy) the world
				// content moves so that current[x] == previous[x+dwx] (same for y). Test that over
				// the visible creature sprites; a majority match means the shift landed this frame.
				const int32_t dwx=state.pending_dx;
				const int32_t dwy=state.pending_dy;
				int32_t considered=0;
				int32_t matches=0;
				for(size_t layer=0;layer<input.current.size();++layer)
					{
					if(!visual_layer_tracks_own_movement(
						static_cast<viewport_visual_layer>(layer)))continue;
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
						if(visual_layer_matches(
								static_cast<viewport_visual_layer>(layer),
								texpos,
								previous[sx*input.dim_y+sy]))++matches;
							}
						}
					}
				if(considered==0)
					{
					// Nothing visible to anchor the test on: nothing to animate either.
					abandon_pending(state);
					}
				else if(matches*2>=considered)
					{
					// The shift landed: re-anchor in-flight movements to the new viewport frame and
					// drop anything scrolled off-screen.
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
					clear_pending(state);
					translated=true;
					}
				else if(++state.pending_frames>4)
					{
					// The shift never showed up recognizably (heavy simultaneous movement, culled
					// render, ...): fall back to the safe reset behavior.
					abandon_pending(state);
					}
				}

			const bool suppress=translated||
				state.pending_dx!=0||state.pending_dy!=0||state.suppress_frames>0;
			if(state.suppress_frames>0)--state.suppress_frames;
			if(!suppress)
				{
				const int32_t tile_count=input.dim_x*input.dim_y;
				std::vector<uint8_t> claimed_sources(tile_count);
				const size_t existing_movement_count=state.movements.size();
				for(size_t layer=0;layer<input.current.size();++layer)
					{
					if(!visual_layer_tracks_own_movement(
						static_cast<viewport_visual_layer>(layer)))continue;
					std::fill(claimed_sources.begin(),claimed_sources.end(),0);
					const int32_t *current=input.current[layer];
					const int32_t *previous=input.previous[layer];
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
						const size_t layer=static_cast<size_t>(movement.layer);
						const int32_t target=movement.target_x*input.dim_y+movement.target_y;
						const int32_t current=input.current[layer][target];
						return frame_time_ms-movement.start_time_ms>=movement_duration_ms||
							current==0||
							!visual_layer_matches(movement.layer,current,movement.texpos);
						}),
				state.movements.end());
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
