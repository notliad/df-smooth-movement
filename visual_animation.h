// SPDX-License-Identifier: MIT

#ifndef VISUAL_ANIMATION_H
#define VISUAL_ANIMATION_H

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

enum class viewport_creature_layer : uint8_t
{
	right,
	center,
	left,
	upright,
	up,
	upleft,
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
	int32_t source_x=0;
	int32_t source_y=0;
	float progress=1.0f;
};

class visual_animation_managerst
{
	struct movementst
	{
		viewport_creature_layer layer;
		int32_t texpos;
		int32_t source_x;
		int32_t source_y;
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
		int32_t pan_x;
		int32_t pan_y;
		bool has_pan;
		// Window-scroll delta not yet observed in the buffers, and how long it has been pending.
		int32_t pending_dx;
		int32_t pending_dy;
		int32_t pending_frames;
		// Frames left in which new-movement detection stays suppressed after scroll activity.
		int32_t suppress_frames;
	};

	uint32_t frame_time_ms;
	uint32_t frame_delta_ms;
	bool has_frame;
	bool force_full_redraw;
	std::vector<viewport_animationst> viewports;

	static constexpr uint32_t movement_duration_ms=100;

	viewport_animationst &get_viewport(const viewport_visual_animation_inputst &input)
		{
		for(viewport_animationst &state:viewports)
			{
			if(state.viewport==input.viewport)return state;
			}
		viewports.push_back({input.viewport,0,0,0,false,false,{},0,0,false,0,0,0,0});
		return viewports.back();
		}

	float movement_progress(uint32_t start_time_ms) const
		{
		const float linear=std::min(1.0f,float(frame_time_ms-start_time_ms)/movement_duration_ms);
		return linear*linear*(3.0f-2.0f*linear);
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
				state.movements.clear();
				state.pending_dx=0;
				state.pending_dy=0;
				state.pending_frames=0;
				state.suppress_frames=0;
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
							if(previous[sx*input.dim_y+sy]==texpos)++matches;
							}
						}
					}
				if(considered==0)
					{
					// Nothing visible to anchor the test on: nothing to animate either.
					state.movements.clear();
					state.pending_dx=0;
					state.pending_dy=0;
					state.pending_frames=0;
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
					state.pending_dx=0;
					state.pending_dy=0;
					state.pending_frames=0;
					translated=true;
					}
				else if(++state.pending_frames>4)
					{
					// The shift never showed up recognizably (heavy simultaneous movement, culled
					// render, ...): fall back to the safe reset behavior.
					state.movements.clear();
					state.pending_dx=0;
					state.pending_dy=0;
					state.pending_frames=0;
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
						return frame_time_ms-movement.start_time_ms>=movement_duration_ms||
							input.current[layer][target]!=movement.texpos;
						}),
				state.movements.end());

			const bool suppress=translated||
				state.pending_dx!=0||state.pending_dy!=0||state.suppress_frames>0;
			if(state.suppress_frames>0)--state.suppress_frames;
			if(!suppress)
				{
				const int32_t tile_count=input.dim_x*input.dim_y;
				std::vector<uint8_t> claimed_sources(tile_count);
				for(size_t layer=0;layer<input.current.size();++layer)
					{
					std::fill(claimed_sources.begin(),claimed_sources.end(),0);
					const int32_t *current=input.current[layer];
					const int32_t *previous=input.previous[layer];
					for(int32_t x=0;x<input.dim_x;++x)
						{
						for(int32_t y=0;y<input.dim_y;++y)
							{
							const int32_t target=x*input.dim_y+y;
							const int32_t texpos=current[target];
							// Entity ids are unavailable, so only accept a unique
							// same-sprite move between empty cells in one layer.
							if(texpos==0||previous[target]!=0)continue;

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
									const int32_t candidate=source_x*input.dim_y+source_y;
									if(!claimed_sources[candidate]&&previous[candidate]==texpos&&
										current[candidate]==0)
										{
										source=candidate;
										++candidate_count;
										}
									}
								}
							if(candidate_count!=1)continue;

							claimed_sources[source]=1;
							state.movements.push_back(
								{
								static_cast<viewport_creature_layer>(layer),
								texpos,
								source/input.dim_y,
								source%input.dim_y,
								x,
								y,
								frame_time_ms
								});
							}
						}
					}
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

		bool requires_full_redraw() const
			{
			return force_full_redraw;
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
					}
				break;
				}
			return {};
			}
};

#endif
