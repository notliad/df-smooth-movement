// SPDX-License-Identifier: MIT

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstring>
#include <cstdio>
#include <initializer_list>
#include <utility>

#include "camera_feature.h"

struct camera_fixture
{
	static constexpr int32_t width=7;
	static constexpr int32_t height=5;
	camera_feature camera;
	int32_t window_x=10;
	int32_t window_y=10;
	int32_t current[width*height];
	int32_t previous[width*height];
	camera_frame_input input;

	camera_fixture()
	{
		input.window_x=&window_x;
		input.window_y=&window_y;
		input.dim_x=width;
		input.dim_y=height;
		input.background=current;
		input.previous_background=previous;
		backgrounds(0,0,0,0);
	}

	void backgrounds(int32_t x,int32_t y,int32_t old_x,int32_t old_y)
	{
		++input.background_generation;
		for(int32_t i=0;i<width;++i)
			for(int32_t j=0;j<height;++j)
				{
				current[i*height+j]=1000+(i+x)*100+j+y;
				previous[i*height+j]=1000+(i+old_x)*100+j+old_y;
				}
	}

	void expect(int32_t x,int32_t y)
	{
		const auto offset=camera.render_offset(128);
		if(offset.x!=x||offset.y!=y)
			std::fprintf(stderr,"Expected (%d,%d), got (%d,%d)\n",x,y,offset.x,offset.y);
		assert(offset.x==x&&offset.y==y);
	}
};

static void test_initial_normalization()
{
	for(bool enabled : {false,true})
		for(bool baseline : {false,true})
			for(int32_t sign : {-1,1})
				{
				camera_fixture f;
				f.camera.set_enabled(enabled);
				if(baseline)
					{
					f.camera.set_enabled(true);
					f.camera.update(f.input);
					}
				f.camera.set_offset(sign*0.75,-sign*0.75,&f.window_x,&f.window_y);
				assert(f.window_x==10+sign&&f.window_y==10-sign);
				f.camera.update(f.input);
				f.expect(-sign*24,sign*24);
				f.backgrounds(sign,-sign,0,0);
				f.camera.update(f.input);
				f.expect(sign*8,-sign*8);
				assert(f.camera.offset_x()==-sign*0.25);
				assert(f.camera.offset_y()==sign*0.25);
				f.camera.update(f.input);
				f.expect(sign*8,-sign*8);
				f.window_x+=sign;
				f.window_y-=sign;
				f.backgrounds(2*sign,-2*sign,sign,-sign);
				f.camera.update(f.input);
				f.expect(sign*40,-sign*40);
				assert(f.camera.offset_x()==-sign*0.25);
				assert(f.camera.offset_y()==sign*0.25);
				f.input.delta_ms=1000;
				f.camera.update(f.input);
				f.expect(sign*8,-sign*8);
				}
}

static void test_repeated_landings()
{
	for(int32_t dx : {-1,0,1})
		for(int32_t dy : {-1,0,1})
			for(int32_t landing : {1,2})
				{
				if(dx==0&&dy==0)continue;
				camera_fixture f;
				f.camera.set_enabled(true);
				f.camera.update(f.input);
				f.window_x+=2*dx;
				f.window_y+=2*dy;
				for(int i=0;i<6;++i)f.camera.update(f.input);
				f.expect(0,0);
				f.backgrounds(landing*dx,landing*dy,0,0);
				f.camera.update(f.input);
				f.expect(landing*dx*32,landing*dy*32);
				int32_t current_copy[f.width*f.height];
				int32_t previous_copy[f.width*f.height];
				std::memcpy(current_copy,f.current,sizeof(current_copy));
				std::memcpy(previous_copy,f.previous,sizeof(previous_copy));
				f.input.background=current_copy;
				f.input.previous_background=previous_copy;
				for(int i=0;i<6;++i)f.camera.update(f.input);
				f.expect(landing*dx*32,landing*dy*32);
				f.input.background=f.current;
				f.input.previous_background=f.previous;
				if(landing==1)
					{
					f.backgrounds(2*dx,2*dy,dx,dy);
					f.camera.update(f.input);
					f.expect(dx*64,dy*64);
					}
				f.window_x+=dx;
				f.window_y+=dy;
				f.camera.update(f.input);
				f.expect(dx*64,dy*64);
				f.backgrounds(3*dx,3*dy,2*dx,2*dy);
				f.camera.update(f.input);
				f.expect(dx*96,dy*96);
				f.input.delta_ms=1000;
				f.camera.update(f.input);
				f.expect(0,0);
				f.input.middle_button=true;
				f.camera.update(f.input);
				f.input.mouse_x=8;
				f.input.mouse_y=-8;
				f.camera.update(f.input);
				f.expect(8,-8);
				}
}

static void test_transition_lifetime()
{
	{
	camera_fixture f;
	f.camera.set_enabled(true);
	f.camera.update(f.input);
	f.window_x+=2;
	f.backgrounds(1,0,0,0);
	f.camera.update(f.input);
	f.expect(32,0);
	f.input.delta_ms=1000;
	f.camera.update(f.input);
	f.expect(0,0);
	f.input.middle_button=true;
	f.camera.update(f.input);
	f.input.mouse_x=8;
	f.camera.update(f.input);
	f.expect(8,0);
	}

	{
	camera_fixture f;
	f.camera.set_enabled(true);
	f.backgrounds(1,0,1,0);
	f.camera.update(f.input);
	++f.window_x;
	for(int i=0;i<6;++i)f.camera.update(f.input);
	f.expect(0,0);
	f.backgrounds(1,0,0,0);
	f.camera.update(f.input);
	f.expect(32,0);
	}

	for(int mode : {0,1,2,3})
		{
		camera_fixture f;
		f.camera.set_enabled(true);
		f.camera.update(f.input);
		f.backgrounds(1,0,0,0);
		f.camera.update(f.input);
		f.expect(0,0);
		++f.window_x;
		f.camera.update(f.input);
		f.expect(32,0);
		if(mode==0)f.camera.cancel_transients();
		if(mode==1)
			{
			f.camera.set_enabled(false);
			f.camera.set_enabled(true);
			}
		if(mode==2)
			{
			f.camera.reset();
			f.camera.set_enabled(true);
			}
		if(mode==3)
			{
			f.camera.cancel_transients();
			f.backgrounds(1,0,1,0);
			f.camera.update(f.input);
			f.backgrounds(1,0,0,0);
			}
		f.camera.update(f.input);
		++f.window_x;
		f.camera.update(f.input);
		f.expect(mode>=2?32:0,0);
		f.backgrounds(2,0,1,0);
		f.camera.update(f.input);
		f.expect(32,0);
		}

	camera_fixture f;
	f.camera.set_enabled(true);
	f.camera.update(f.input);
	++f.window_x;
	f.camera.set_offset(0.75,0,&f.window_x,&f.window_y);
	f.backgrounds(2,0,0,0);
	f.camera.update(f.input);
	f.expect(40,0);
	assert(f.camera.offset_x()==-0.25);
}

static void test_background_observer()
{
	int a=0,b=0,c=0,renderer=0,viewport=0;
	const camera_background_observation initial={
		&renderer,&viewport,&a,&b,7,5,100,true,true};
	camera_background_observer observer;
	auto input=initial;
	auto result=observer.observe(input);
	assert(result.generation==1&&!result.discontinuity);
	input.context_changed=false;
	result=observer.observe(input);
	assert(result.generation==1&&!result.discontinuity);
	++input.gputicks;
	result=observer.observe(input);
	assert(result.generation==1&&!result.discontinuity);
	for(uint64_t generation : {2,3,4})
		{
		std::swap(input.current,input.previous);
		++input.gputicks;
		result=observer.observe(input);
		assert(result.generation==generation&&!result.discontinuity);
		result=observer.observe(input);
		assert(result.generation==generation&&!result.discontinuity);
		}
	std::swap(input.current,input.previous);
	input.context_changed=true;
	result=observer.observe(input);
	assert(result.generation==5&&result.discontinuity);
	input.context_changed=false;
	result=observer.observe(input);
	assert(result.generation==5&&!result.discontinuity);

	for(int mode=0;mode<11;++mode)
		{
		observer={};
		input=initial;
		input.context_changed=false;
		observer.observe(input);
		++input.gputicks;
		switch(mode)
			{
			case 0: input.current=&c; break;
			case 1:
				input.gputicks+=2;
				std::swap(input.current,input.previous);
				break;
			case 2: input.gputicks=0; break;
			case 3: input.readable=false; break;
			case 4: input.current=nullptr; break;
			case 5: input.viewport=nullptr; break;
			case 6: input.renderer=&c; break;
			case 7: input.viewport=&c; break;
			case 8: ++input.dim_x; break;
			case 9: input.dim_y=0; break;
			case 10: input.context_changed=true; break;
			}
		result=observer.observe(input);
		assert(result.generation==1&&result.discontinuity);
		input=initial;
		input.context_changed=false;
		input.gputicks=110;
		result=observer.observe(input);
		assert(result.generation==1&&result.discontinuity);
		++input.gputicks;
		result=observer.observe(input);
		assert(result.generation==1&&!result.discontinuity);
		std::swap(input.current,input.previous);
		++input.gputicks;
		result=observer.observe(input);
		assert(result.generation==2&&!result.discontinuity);
		}

	observer={};
	input=initial;
	input.context_changed=false;
	input.gputicks=UINT32_MAX;
	observer.observe(input);
	input.gputicks=0;
	std::swap(input.current,input.previous);
	result=observer.observe(input);
	assert(result.generation==2&&!result.discontinuity);

	observer={};
	input.current=nullptr;
	result=observer.observe(input);
	assert(result.generation==0&&result.discontinuity);
	input.current=&b;
	result=observer.observe(input);
	assert(result.generation==0&&result.discontinuity);
	result=observer.observe(input);
	assert(result.generation==0&&!result.discontinuity);
}

static void test_observed_camera_lifecycle()
{
	for(bool landed : {false,true})
		{
		camera_fixture f;
		camera_background_observer observer;
		f.camera.set_offset(-0.75,0.75,&f.window_x,&f.window_y);
		if(landed)f.backgrounds(-1,1,0,0);
		const auto result=observer.observe({
			&f.camera,&f,f.current,f.previous,f.width,f.height,0,true,true});
		f.input.background_generation=result.generation;
		f.input.background_discontinuity=result.discontinuity;
		f.camera.update(f.input);
		f.expect(landed?-8:24,landed?8:-24);
		}

	{
	camera_fixture f;
	camera_background_observer observer;
	camera_background_observation observation={
		&f.camera,&f,f.current,f.previous,f.width,f.height,0,true,false};
	const auto update=[&]()
		{
		const auto result=observer.observe(observation);
		f.input.background_generation=result.generation;
		f.input.background_discontinuity=result.discontinuity;
		f.camera.update(f.input);
		++observation.gputicks;
		};
	f.camera.set_enabled(true);
	update();
	++f.window_x;
	update();
	f.expect(0,0);
	++f.window_x;
	observation.gputicks+=2;
	f.backgrounds(1,0,0,0);
	std::swap(observation.current,observation.previous);
	update();
	f.expect(0,0);
	update();
	std::swap(observation.current,observation.previous);
	update();
	f.expect(0,0);
	++f.window_x;
	update();
	f.expect(32,0);
	}

	for(int mode=0;mode<6;++mode)
		{
		camera_fixture f;
		camera_background_observer observer;
		camera_background_observation observation={
			&f.camera,&f,f.current,f.previous,f.width,f.height,0,true,true};
		const auto update=[&]()
			{
			const auto result=observer.observe(observation);
			f.input.background_generation=result.generation;
			f.input.background_discontinuity=result.discontinuity;
			f.camera.update(f.input);
			observation.context_changed=false;
			++observation.gputicks;
			};
		f.camera.set_offset(0.75,0,&f.window_x,&f.window_y);
		update();
		f.expect(-24,0);
		f.backgrounds(1,0,0,0);
		std::swap(observation.current,observation.previous);
		update();
		f.expect(8,0);
		f.camera.reset_offset();
		++f.window_x;
		update();
		f.expect(0,0);
		if(mode==0)f.camera.cancel_transients();
		if(mode==1)
			{
			f.camera.set_enabled(false);
			update();
			f.camera.set_enabled(true);
			}
		if(mode==2)observation.context_changed=true;
		if(mode==3)observation.gputicks+=2;
		if(mode==4)observation.current=&observer;
		if(mode==5)
			{
			observation.readable=false;
			f.input.background=nullptr;
			f.input.previous_background=nullptr;
			update();
			observation.readable=true;
			f.input.background=f.current;
			f.input.previous_background=f.previous;
			}
		++f.window_x;
		update();
		update();
		f.expect(0,0);
		std::swap(observation.current,observation.previous);
		update();
		f.expect(mode==0?32:0,0);
		f.camera.cancel_transients();
		++f.window_x;
		update();
		f.expect(mode==0?0:32,0);
		f.camera.cancel_transients();
		++f.window_x;
		update();
		f.expect(0,0);
		std::swap(observation.current,observation.previous);
		update();
		f.expect(32,0);
		}

	camera_fixture f;
	camera_background_observer observer;
	camera_background_observation observation={
		&f.camera,&f,f.current,f.previous,f.width,f.height,0,true,false};
	assert(observer.observe(observation).generation==1);
	std::swap(observation.current,observation.previous);
	++observation.gputicks;
	assert(observer.observe(observation).generation==2);
	std::swap(observation.current,observation.previous);
	++observation.gputicks;
	f.input.background_generation=observer.observe(observation).generation;
	assert(f.input.background_generation==3);
	f.camera.set_enabled(true);
	f.camera.update(f.input);
	f.backgrounds(1,0,0,0);
	f.input.background_generation=3;
	++f.window_x;
	f.camera.update(f.input);
	f.expect(32,0);
	f.camera.set_enabled(false);
	std::swap(observation.current,observation.previous);
	++observation.gputicks;
	f.input.background_generation=observer.observe(observation).generation;
	f.camera.update(f.input);
	f.camera.set_enabled(true);
	f.camera.update(f.input);
	++f.window_x;
	f.camera.update(f.input);
	f.expect(32,0);
}

int main(int argc,char **argv)
{
	if(argc==1||std::strcmp(argv[1],"observer")==0)
		{
		test_background_observer();
		test_observed_camera_lifecycle();
		}
	if(argc==1||std::strcmp(argv[1],"normalization")==0)
		test_initial_normalization();
	if(argc==1||std::strcmp(argv[1],"landings")==0)
		test_repeated_landings();
	if(argc==1||std::strcmp(argv[1],"lifetime")==0)
		test_transition_lifetime();
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
