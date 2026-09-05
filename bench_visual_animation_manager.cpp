// SPDX-License-Identifier: MIT
//
// Timing for the parts of visual_animation_managerst the render hook calls every frame:
// synchronize_viewport on a fortress-sized viewport, and the per-tile get_movement /
// get_facing lookups the plugin makes while collecting proxies. Prints microseconds; there
// are no assertions. Build with `make smooth-movement-bench` (or the compiler line at the
// top of test_visual_animation_manager.cpp) and run it before and after a change.

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

#include "visual_animation.h"

namespace {

constexpr int32_t dim_x=100;
constexpr int32_t dim_y=60;
constexpr size_t tiles=size_t(dim_x)*size_t(dim_y);
constexpr size_t layer_count=static_cast<size_t>(viewport_visual_layer::count);

struct viewport_bufferst
{
	std::array<std::vector<int32_t>,layer_count> current;
	std::array<std::vector<int32_t>,layer_count> previous;
	std::vector<int32_t> background;
	std::vector<int32_t> background_previous;

	viewport_bufferst()
		{
		for(auto &layer:current)layer.assign(tiles,0);
		for(auto &layer:previous)layer.assign(tiles,0);
		background.assign(tiles,0);
		background_previous.assign(tiles,0);
		for(size_t i=0;i<tiles;++i)background[i]=1000+int32_t(i%7);
		background_previous=background;
		}

	// The engine's redraw: current becomes previous, then current is drawn again.
	void advance()
		{
		for(size_t layer=0;layer<layer_count;++layer)
			{
			previous[layer]=current[layer];
			std::fill(current[layer].begin(),current[layer].end(),0);
			}
		background_previous=background;
		}

	viewport_visual_animation_inputst input() const
		{
		viewport_visual_animation_inputst in;
		in.viewport=this;
		in.dim_x=dim_x;
		in.dim_y=dim_y;
		in.context_revision=1;
		for(size_t layer=0;layer<layer_count;++layer)
			{
			in.current[layer]=current[layer].data();
			in.previous[layer]=previous[layer].data();
			}
		in.current_background=background.data();
		in.previous_background=background_previous.data();
		return in;
		}
};

struct creaturest
{
	int32_t x;
	int32_t y;
	int32_t texpos;
};

void draw(viewport_bufferst &buffers,const std::vector<creaturest> &creatures)
{
	const size_t center=static_cast<size_t>(viewport_visual_layer::center);
	const size_t right=static_cast<size_t>(viewport_visual_layer::right);
	for(const creaturest &creature:creatures)
		{
		buffers.current[center][size_t(creature.x)*dim_y+size_t(creature.y)]=creature.texpos;
		if(creature.x+1<dim_x&&creature.texpos%3==0)
			buffers.current[right][size_t(creature.x+1)*dim_y+size_t(creature.y)]=creature.texpos+1;
		}
}

uint64_t now_us()
{
	return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count());
}

void run(int32_t creature_count)
{
	std::mt19937 rng(7);
	viewport_bufferst buffers;
	std::vector<creaturest> creatures;
	for(int32_t i=0;i<creature_count;++i)
		creatures.push_back({int32_t(rng()%(dim_x-2))+1,int32_t(rng()%(dim_y-2))+1,100+int32_t(rng()%30)});
	draw(buffers,creatures);

	visual_animation_managerst manager;
	uint32_t now_ms=1000;
	uint64_t sync_us=0,lookup_us=0,facing_us=0;
	uint64_t active=0;
	const int frames=300;
	for(int frame=0;frame<frames;++frame)
		{
		// A step every sixth frame: half the creatures move one tile.
		if(frame%6==0)
			{
			buffers.advance();
			for(creaturest &creature:creatures)
				{
				if(rng()%2)continue;
				const int32_t nx=creature.x+int32_t(rng()%3)-1;
				const int32_t ny=creature.y+int32_t(rng()%3)-1;
				if(nx>0&&nx<dim_x-1&&ny>0&&ny<dim_y-1)
					{
					creature.x=nx;
					creature.y=ny;
					}
				}
			draw(buffers,creatures);
			}
		manager.begin_frame(now_ms);
		const uint64_t t0=now_us();
		manager.synchronize_viewport(buffers.input());
		manager.end_frame();
		const uint64_t t1=now_us();
		// What collect_proxies does: one lookup per drawn tile in every layer.
		for(size_t layer=0;layer<layer_count;++layer)
			{
			const std::vector<int32_t> &current=buffers.current[layer];
			for(int32_t x=0;x<dim_x;++x)
				for(int32_t y=0;y<dim_y;++y)
					{
					if(current[size_t(x)*dim_y+size_t(y)]==0)continue;
					const visual_movement_renderst movement=manager.get_movement(
						&buffers,static_cast<viewport_visual_layer>(layer),x,y);
					if(movement.active)++active;
					}
			}
		const uint64_t t2=now_us();
		// What the resting mirrored pass does: one facing lookup per tile.
		uint64_t mirrored=0;
		for(int32_t x=0;x<dim_x;++x)
			for(int32_t y=0;y<dim_y;++y)
				if(manager.get_facing(&buffers,x,y)!=native_sprite_facing)++mirrored;
		const uint64_t t3=now_us();
		sync_us+=t1-t0;
		lookup_us+=t2-t1;
		facing_us+=t3-t2;
		if(mirrored>tiles)std::printf("unreachable\n");
		now_ms+=16;
		}
	std::printf("%4d creatures: sync %6.1f us  movement lookups %6.1f us  facing lookups %6.1f us  per frame (%.1f active lookups/frame)\n",
		creature_count,double(sync_us)/frames,double(lookup_us)/frames,double(facing_us)/frames,double(active)/frames);
}

} // namespace

int main()
{
	std::printf("viewport %dx%d, 300 frames of 16 ms, a step every 6 frames\n",dim_x,dim_y);
	for(int32_t creatures:{10,50,200})run(creatures);
	return 0;
}
