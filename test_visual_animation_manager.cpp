// SPDX-License-Identifier: MIT

#include <array>
#include <cassert>
#include <cstdint>
#include <limits>

#include "visual_animation.h"

int main()
{
	visual_animation_managerst manager;
	manager.begin_frame(1000);
	assert(manager.get_frame_time_ms()==1000);
	assert(manager.get_frame_delta_ms()==0);

	manager.begin_frame(1020);
	assert(manager.get_frame_time_ms()==1020);
	assert(manager.get_frame_delta_ms()==20);

	manager.begin_frame(1020);
	assert(manager.get_frame_delta_ms()==0);

	visual_animation_managerst rollover;
	rollover.begin_frame(std::numeric_limits<uint32_t>::max()-5);
	rollover.begin_frame(3);
	assert(rollover.get_frame_delta_ms()==9);

	std::array<int32_t,9> empty{};
	std::array<int32_t,9> current{};
	std::array<int32_t,9> previous{};
	const void *viewport=reinterpret_cast<const void *>(uintptr_t(1));
	viewport_visual_animation_inputst input=
		{
		viewport,
		3,
		3,
		1,
		{empty.data(),current.data(),empty.data(),empty.data(),empty.data(),empty.data()},
		{empty.data(),previous.data(),empty.data(),empty.data(),empty.data(),empty.data()}
		};

	visual_animation_managerst movement;
	movement.begin_frame(1990);
	movement.synchronize_viewport(input,true);
	movement.end_frame();
	assert(!movement.requires_full_redraw());

	previous[0*3+1]=42;
	current[1*3+1]=42;
	movement.begin_frame(2000);
	movement.synchronize_viewport(input,true);
	movement.end_frame();
	auto render=movement.get_movement(viewport,viewport_creature_layer::center,1,1);
	assert(render.active);
	assert(render.source_x==0&&render.source_y==1);
	assert(render.progress==0.0f);
	assert(movement.requires_full_redraw());

	previous=current;
	input.previous[1]=previous.data();
	movement.begin_frame(2050);
	movement.synchronize_viewport(input,true);
	movement.end_frame();
	render=movement.get_movement(viewport,viewport_creature_layer::center,1,1);
	assert(render.active);
	assert(render.progress==0.5f);

	movement.begin_frame(2100);
	movement.synchronize_viewport(input,true);
	movement.end_frame();
	assert(!movement.get_movement(
		viewport,viewport_creature_layer::center,1,1).active);
	assert(movement.requires_full_redraw());

	movement.begin_frame(2120);
	movement.synchronize_viewport(input,true);
	movement.end_frame();
	assert(!movement.requires_full_redraw());

	visual_animation_managerst ambiguous;
	ambiguous.begin_frame(2990);
	ambiguous.synchronize_viewport(input,true);
	ambiguous.end_frame();
	previous.fill(0);
	previous[0*3+1]=42;
	previous[1*3+0]=42;
	input.previous[1]=previous.data();
	ambiguous.begin_frame(3000);
	ambiguous.synchronize_viewport(input,true);
	ambiguous.end_frame();
	assert(!ambiguous.get_movement(
		viewport,viewport_creature_layer::center,1,1).active);

	visual_animation_managerst context;
	current.fill(0);
	previous.fill(0);
	context.begin_frame(4000);
	context.synchronize_viewport(input,true);
	context.end_frame();
	previous[0*3+1]=42;
	current[1*3+1]=42;
	context.begin_frame(4010);
	context.synchronize_viewport(input,true);
	context.end_frame();
	assert(context.get_movement(
		viewport,viewport_creature_layer::center,1,1).active);

	++input.context_revision;
	context.begin_frame(4020);
	context.synchronize_viewport(input,true);
	context.end_frame();
	assert(!context.get_movement(
		viewport,viewport_creature_layer::center,1,1).active);
}
