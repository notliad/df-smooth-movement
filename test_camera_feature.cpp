// SPDX-License-Identifier: MIT

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

#include "camera_feature.h"

int main()
{
	{
	camera_feature sizing;
	sizing.set_offset(0.48,-0.48,nullptr,nullptr);
	const auto small=sizing.render_offset(64);
	assert(small.x==-8&&small.y==8);
	const auto normal=sizing.render_offset(128);
	assert(normal.x==-15&&normal.y==15);
	const auto fractional=sizing.render_offset(130);
	assert(fractional.x==-15&&fractional.y==15);
	}

	camera_feature camera;
	assert(!camera.enabled());
	int32_t window_x=10;
	int32_t window_y=10;
	camera.set_offset(0.25,-0.5,&window_x,&window_y);
	assert(camera.enabled());
	assert(camera.offset_x()==0.25);
	assert(camera.offset_y()==-0.5);
	auto offset=camera.render_offset(128);
	assert(offset.x==-8);
	assert(offset.y==16);
	assert(!offset.request_cleanup_redraw);
	camera.reset_offset();
	offset=camera.render_offset(128);
	assert(offset.x==0&&offset.y==0);
	assert(offset.request_cleanup_redraw);

	constexpr int32_t dimension=2;
	int32_t background[dimension*dimension]={1,2,3,4};
	camera_frame_input input;
	input.zoom_factor=128;
	input.window_x=&window_x;
	input.window_y=&window_y;
	input.dim_x=dimension;
	input.dim_y=dimension;
	input.background=background;
	input.previous_background=background;
	camera.update(input);
	input.middle_button=true;
	input.mouse_x=32;
	camera.update(input);
	input.mouse_x=48;
	camera.update(input);
	offset=camera.render_offset(128);
	assert(offset.x==16&&offset.y==0);
	input.middle_button=false;
	camera.update(input);

	camera.set_enabled(false);
	assert(!camera.enabled());
	assert(camera.offset_x()==0.0&&camera.offset_y()==0.0);
}
