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

	// Camera-pan handling. window_x/window_y change at input time but the buffers shift on a later
	// render frame, so the manager must (a) NOT create movements from the buffer shift itself (the
	// floating-sprite bug), and (b) translate in-flight movements on the frame the shift lands.
	std::array<int32_t,9> pan_current{};
	std::array<int32_t,9> pan_previous{};
	std::array<int32_t,9> pan_empty{};
	viewport_visual_animation_inputst pan_input=
		{
		viewport,
		3,
		3,
		1,
		{pan_empty.data(),pan_current.data(),pan_empty.data(),pan_empty.data(),pan_empty.data(),pan_empty.data()},
		{pan_empty.data(),pan_previous.data(),pan_empty.data(),pan_empty.data(),pan_empty.data(),pan_empty.data()},
		0,
		0
		};

	// FLOAT REGRESSION: a stationary creature, pan announced at frame A, buffers shift at frame B.
	// Frame B's buffers look exactly like a real move ((1,1)->(0,1) with a unique source) — the
	// manager must recognize it as the pending pan and create NO movement.
	visual_animation_managerst floaty;
	pan_previous[1*3+1]=42;
	pan_current[1*3+1]=42;
	floaty.begin_frame(4990);
	floaty.synchronize_viewport(pan_input,true);
	floaty.end_frame();
	pan_input.pan_x=1;                   // frame A: window scrolled, buffers unchanged
	floaty.begin_frame(5000);
	floaty.synchronize_viewport(pan_input,true);
	floaty.end_frame();
	assert(!floaty.get_movement(viewport,viewport_creature_layer::center,1,1).active);
	pan_previous[1*3+1]=42;              // frame B: buffers apply the shift
	pan_current.fill(0);
	pan_current[0*3+1]=42;
	floaty.begin_frame(5010);
	floaty.synchronize_viewport(pan_input,true);
	floaty.end_frame();
	assert(!floaty.get_movement(viewport,viewport_creature_layer::center,0,1).active);

	// FOLLOW: an in-flight movement survives the announce frame untouched and is translated on the
	// frame the buffers shift, so the sprite tracks the scrolled world.
	visual_animation_managerst panner;
	pan_current.fill(0);
	pan_previous.fill(0);
	pan_input.pan_x=0;
	panner.begin_frame(5990);
	panner.synchronize_viewport(pan_input,true);
	panner.end_frame();
	pan_previous[0*3+1]=42;              // creature steps (0,1) -> (1,1)
	pan_current[1*3+1]=42;
	panner.begin_frame(6000);
	panner.synchronize_viewport(pan_input,true);
	panner.end_frame();
	auto moved=panner.get_movement(viewport,viewport_creature_layer::center,1,1);
	assert(moved.active&&moved.source_x==0&&moved.source_y==1);

	pan_input.pan_x=1;                   // frame A: pan announced, buffers unchanged
	pan_previous[0*3+1]=0;
	pan_previous[1*3+1]=42;              // previous now matches current (stationary at (1,1))
	panner.begin_frame(6010);
	panner.synchronize_viewport(pan_input,true);
	panner.end_frame();
	moved=panner.get_movement(viewport,viewport_creature_layer::center,1,1);
	assert(moved.active&&moved.source_x==0);   // untouched: still anchored to the old frame

	pan_current.fill(0);                 // frame B: buffers shift east by one
	pan_current[0*3+1]=42;
	panner.begin_frame(6020);
	panner.synchronize_viewport(pan_input,true);
	panner.end_frame();
	assert(!panner.get_movement(viewport,viewport_creature_layer::center,1,1).active);
	auto followed=panner.get_movement(viewport,viewport_creature_layer::center,0,1);
	assert(followed.active&&followed.source_x==-1&&followed.source_y==1);

	// SAME-FRAME: pan announced and buffers shifted in the same call — translated immediately.
	visual_animation_managerst same_frame;
	pan_current.fill(0);
	pan_previous.fill(0);
	pan_input.pan_x=0;
	same_frame.begin_frame(6990);
	same_frame.synchronize_viewport(pan_input,true);
	same_frame.end_frame();
	pan_previous[0*3+1]=42;
	pan_current[1*3+1]=42;
	same_frame.begin_frame(7000);
	same_frame.synchronize_viewport(pan_input,true);
	same_frame.end_frame();
	assert(same_frame.get_movement(viewport,viewport_creature_layer::center,1,1).active);
	pan_input.pan_x=1;
	pan_previous=pan_current;
	pan_current.fill(0);
	pan_current[0*3+1]=42;
	same_frame.begin_frame(7010);
	same_frame.synchronize_viewport(pan_input,true);
	same_frame.end_frame();
	followed=same_frame.get_movement(viewport,viewport_creature_layer::center,0,1);
	assert(followed.active&&followed.source_x==-1);

	// A change that is NOT a pure pan (context revision bump) still resets, even with in-flight work.
	visual_animation_managerst reset_on_zoom;
	pan_current.fill(0);
	pan_previous.fill(0);
	pan_input.pan_x=0;
	pan_input.context_revision=1;
	reset_on_zoom.begin_frame(8000);
	reset_on_zoom.synchronize_viewport(pan_input,true);
	reset_on_zoom.end_frame();
	pan_previous[0*3+1]=42;
	pan_current[1*3+1]=42;
	reset_on_zoom.begin_frame(8010);
	reset_on_zoom.synchronize_viewport(pan_input,true);
	reset_on_zoom.end_frame();
	assert(reset_on_zoom.get_movement(viewport,viewport_creature_layer::center,1,1).active);
	pan_input.context_revision=2;       // e.g. zoom / z-level / resize
	reset_on_zoom.begin_frame(8020);
	reset_on_zoom.synchronize_viewport(pan_input,true);
	reset_on_zoom.end_frame();
	assert(!reset_on_zoom.get_movement(viewport,viewport_creature_layer::center,1,1).active);
}
