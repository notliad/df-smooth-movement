// SPDX-License-Identifier: MIT

#include <array>
#ifdef NDEBUG
#undef NDEBUG
#endif
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
		{empty.data(),current.data(),empty.data(),empty.data(),empty.data(),empty.data(),empty.data(),empty.data(),empty.data()},
		{empty.data(),previous.data(),empty.data(),empty.data(),empty.data(),empty.data(),empty.data(),empty.data(),empty.data()}
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
		{pan_empty.data(),pan_current.data(),pan_empty.data(),pan_empty.data(),pan_empty.data(),pan_empty.data(),pan_empty.data(),pan_empty.data(),pan_empty.data()},
		{pan_empty.data(),pan_previous.data(),pan_empty.data(),pan_empty.data(),pan_empty.data(),pan_empty.data(),pan_empty.data(),pan_empty.data(),pan_empty.data()},
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

	// Status fragments inherit nearby center motion even while their texture flashes.
	std::array<int32_t,9> status_current{};
	std::array<int32_t,9> status_previous{};
	current.fill(0);
	previous.fill(0);
	input.context_revision=1;
	input.current.fill(empty.data());
	input.previous.fill(empty.data());
	input.current[static_cast<size_t>(viewport_creature_layer::center)]=current.data();
	input.previous[static_cast<size_t>(viewport_creature_layer::center)]=previous.data();
	input.current[static_cast<size_t>(viewport_creature_layer::item)]=status_current.data();
	input.previous[static_cast<size_t>(viewport_creature_layer::item)]=status_previous.data();
	input.current[static_cast<size_t>(viewport_creature_layer::designation)]=status_current.data();
	input.previous[static_cast<size_t>(viewport_creature_layer::designation)]=status_previous.data();
	visual_animation_managerst companion;
	companion.begin_frame(8990);
	companion.synchronize_viewport(input,true);
	companion.end_frame();
	previous[0*3+1]=42;
	current[1*3+1]=42;
	status_previous[0*3+0]=90;
	status_current[1*3+0]=91;
	assert(visual_moved_between_tiles(
		status_current.data(),status_previous.data(),0*3+0,1*3+0));
	status_previous[1*3+0]=80;
	assert(!visual_moved_between_tiles(
		status_current.data(),status_previous.data(),0*3+0,1*3+0));
	status_previous[1*3+0]=0;
	companion.begin_frame(9000);
	companion.synchronize_viewport(input,true);
	companion.end_frame();
	auto status=companion.get_movement(viewport,viewport_creature_layer::designation,1,0);
	assert(status.active&&status.source_x==0&&status.source_y==0);
	const auto carried_item=companion.get_movement(
		viewport,viewport_creature_layer::item,1,0);
	assert(carried_item.active&&carried_item.inherited);
	previous=current;
	status_current[1*3+0]=92;
	companion.begin_frame(9050);
	companion.synchronize_viewport(input,true);
	companion.end_frame();
	status=companion.get_movement(viewport,viewport_creature_layer::designation,1,0);
	assert(status.active&&status.progress==0.5f);

	// Divergent nearby creature movements make companion ownership ambiguous, so the overlay snaps.
	current.fill(0);
	previous.fill(0);
	status_current.fill(0);
	visual_animation_managerst crowd;
	crowd.begin_frame(9990);
	crowd.synchronize_viewport(input,true);
	crowd.end_frame();
	previous[0*3+0]=41;
	current[0*3+1]=41;
	previous[2*3+2]=42;
	current[2*3+1]=42;
	status_current[1*3+1]=99;
	crowd.begin_frame(10000);
	crowd.synchronize_viewport(input,true);
	crowd.end_frame();
	assert(!crowd.get_movement(
		viewport,viewport_creature_layer::designation,1,1).active);

	// Wheelbarrows use the item layer and the same independent adjacent-movement detection.
	current.fill(0);
	previous.fill(0);
	input.current.fill(empty.data());
	input.previous.fill(empty.data());
	input.current[static_cast<size_t>(viewport_creature_layer::item)]=current.data();
	input.previous[static_cast<size_t>(viewport_creature_layer::item)]=previous.data();
	visual_animation_managerst item;
	item.begin_frame(10990);
	item.synchronize_viewport(input,true);
	item.end_frame();
	previous[0*3+1]=77;
	current[1*3+1]=77;
	item.begin_frame(11000);
	item.synchronize_viewport(input,true);
	item.end_frame();
	const auto item_move=item.get_movement(
		viewport,viewport_creature_layer::item,1,1);
	assert(item_move.active&&item_move.source_x==0&&item_move.source_y==1);

	// Minecart graphics can change texpos while moving; vehicle identity is tile occupancy.
	current.fill(0);
	previous.fill(0);
	input.current.fill(empty.data());
	input.previous.fill(empty.data());
	input.current[static_cast<size_t>(viewport_creature_layer::vehicle)]=current.data();
	input.previous[static_cast<size_t>(viewport_creature_layer::vehicle)]=previous.data();
	visual_animation_managerst vehicle;
	vehicle.begin_frame(11990);
	vehicle.synchronize_viewport(input,true);
	vehicle.end_frame();
	previous[0*3+1]=77;
	current[1*3+1]=78;
	vehicle.begin_frame(12000);
	vehicle.synchronize_viewport(input,true);
	vehicle.end_frame();
	assert(vehicle.get_movement(
		viewport,viewport_creature_layer::vehicle,1,1).active);
	previous=current;
	current[1*3+1]=79;
	vehicle.begin_frame(12050);
	vehicle.synchronize_viewport(input,true);
	vehicle.end_frame();
	const auto cart=vehicle.get_movement(
		viewport,viewport_creature_layer::vehicle,1,1);
	assert(cart.active&&cart.progress==0.5f);
	previous=current;
	current.fill(0);
	current[2*3+1]=80;
	vehicle.begin_frame(12060);
	vehicle.synchronize_viewport(input,true);
	vehicle.end_frame();
	const auto chained=vehicle.get_movement(
		viewport,viewport_creature_layer::vehicle,2,1);
	assert(chained.active&&chained.source_x>0.0f&&chained.source_x<1.0f&&
		chained.progress==0.0f);
}
