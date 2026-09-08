// SPDX-License-Identifier: MIT

#include "movement_feature.h"

#include "df/coord2d.h"
#include "df/texture_fullid.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <set>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

visual_animation_manager animation_manager;
using tile_coverage=std::set<df::coord2d>;
tile_coverage previous_coverage;
uint64_t visual_context_revision=0;
const void *previous_viewport=nullptr;
std::array<int32_t,12> previous_view_signature{};
bool has_view_signature=false;
df::coord2d previous_pan{0,0};
bool has_pan_context=false;
// ponytail: DFHack invokes this renderer hook serially; thread the API through the draw stack if
// rendering ever becomes reentrant.
const render_function *render_api=nullptr;

constexpr uint32_t fire_bits=0x70000000U;

bool update_visual_context(const movement_frame_context &frame)
{
	const df::renderer_2d_base *renderer=frame.renderer;
	const df::graphic_viewportst *vp=frame.main_viewport;
	// window_x/window_y are deliberately excluded: a horizontal/vertical scroll is followed, not
	// reset. window_z (z-level) stays, since a z change is not followable.
	const std::array<int32_t,12> signature=
		{
		frame.window_pos.z,
		vp->dim_x,
		vp->dim_y,
		vp->clipx[0],
		vp->clipx[1],
		vp->clipy[0],
		vp->clipy[1],
		renderer->viewport_zoom_factor,
		renderer->origin_x,
		renderer->origin_y,
		frame.screen_dim_x,
		frame.screen_dim_y
		};
	const bool changed=!has_view_signature||previous_viewport!=vp||
		previous_view_signature!=signature;
	if(changed)
		{
		++visual_context_revision;
		previous_coverage.clear();
		}
	previous_viewport=vp;
	previous_view_signature=signature;
	has_view_signature=true;

	// On a pure pan the reset signature is unchanged, but last frame's blackout coverage is in the
	// old viewport frame, so discard it (the engine repaints the whole scrolled viewport anyway).
	const df::coord2d pan=frame.window_pos;
	if(!has_pan_context||previous_pan!=pan)
		previous_coverage.clear();
	previous_pan=pan;
	has_pan_context=true;
	return changed;
}

using viewport_layer_member=int32_t *df::graphic_viewportst::*;

struct visual_layer_buffer
{
	viewport_visual_layer layer;
	viewport_layer_member current;
	viewport_layer_member previous;
};

constexpr size_t visual_layer_count=static_cast<size_t>(viewport_visual_layer::count);
constexpr std::array visual_layer_buffers=
	{
	visual_layer_buffer{viewport_visual_layer::right,
		&df::graphic_viewportst::screentexpos_right_creature,
		&df::graphic_viewportst::screentexpos_right_creature_old},
	visual_layer_buffer{viewport_visual_layer::center,
		&df::graphic_viewportst::screentexpos,
		&df::graphic_viewportst::screentexpos_old},
	visual_layer_buffer{viewport_visual_layer::left,
		&df::graphic_viewportst::screentexpos_left_creature,
		&df::graphic_viewportst::screentexpos_left_creature_old},
	visual_layer_buffer{viewport_visual_layer::upright,
		&df::graphic_viewportst::screentexpos_upright_creature,
		&df::graphic_viewportst::screentexpos_upright_creature_old},
	visual_layer_buffer{viewport_visual_layer::up,
		&df::graphic_viewportst::screentexpos_up_creature,
		&df::graphic_viewportst::screentexpos_up_creature_old},
	visual_layer_buffer{viewport_visual_layer::upleft,
		&df::graphic_viewportst::screentexpos_upleft_creature,
		&df::graphic_viewportst::screentexpos_upleft_creature_old},
	visual_layer_buffer{viewport_visual_layer::vehicle,
		&df::graphic_viewportst::screentexpos_vehicle,
		&df::graphic_viewportst::screentexpos_vehicle_old},
	visual_layer_buffer{viewport_visual_layer::item,
		&df::graphic_viewportst::screentexpos_item,
		&df::graphic_viewportst::screentexpos_item_old},
	visual_layer_buffer{viewport_visual_layer::designation,
		&df::graphic_viewportst::screentexpos_designation,
		&df::graphic_viewportst::screentexpos_designation_old}
	};

constexpr bool valid_visual_layer_buffers()
{
	uint16_t layers=0;
	for(const auto &buffer:visual_layer_buffers)
		{
		const uint16_t layer=uint16_t(1U<<static_cast<uint8_t>(buffer.layer));
		if(layers&layer)return false;
		layers|=layer;
		}
	return layers==uint16_t((1U<<visual_layer_count)-1);
}

static_assert(valid_visual_layer_buffers());

template<typename Viewport>
auto visual_layers(Viewport *vp,bool previous=false)
{
	using layer_pointer=std::conditional_t<
		std::is_const_v<Viewport>,const int32_t *,int32_t *>;
	std::array<layer_pointer,visual_layer_count> layers{};
	for(const auto &buffer:visual_layer_buffers)
		layers[static_cast<size_t>(buffer.layer)]=vp->*(previous?
			buffer.previous:buffer.current);
	return layers;
}

viewport_visual_animation_input animation_input(
	df::graphic_viewportst *vp,
	df::coord2d pan)
{
	const df::graphic_viewportst *const_viewport=vp;
	return {
		vp,
		vp->dim_x,
		vp->dim_y,
		visual_context_revision,
		visual_layers(const_viewport),
		visual_layers(const_viewport,true),
		pan.x,
		pan.y
		};
}

// The layer buffers are freed and nulled without clearing the active flag.
bool viewport_readable(df::graphic_viewportst *vp,df::coord2d pan)
{
	return vp!=nullptr&&vp->flag.bits.active&&animation_input(vp,pan).valid();
}

int32_t tile_pixel(int32_t tile,int32_t origin,int32_t zoom)
{
	return zoom==128?32*tile+origin:(zoom*32*tile)/128+origin;
}

bool inside_clip(const df::graphic_viewportst *vp,int32_t x,int32_t y)
{
	return x>=vp->clipx[0]&&x<=vp->clipx[1]&&
		y>=vp->clipy[0]&&y<=vp->clipy[1];
}

bool has_fire(const df::graphic_viewportst *vp,int32_t x,int32_t y)
{
	return vp->screentexpos_spatter_flag!=nullptr&&
		(vp->screentexpos_spatter_flag[x*vp->dim_y+y]&fire_bits)!=0;
}

template<typename T>
class scoped_value_restore
{
	T &value;
	T saved;

	public:
		explicit scoped_value_restore(T &value,T replacement=T{}):
			value(value),
			saved(std::exchange(value,std::move(replacement)))
			{
			static_assert(std::is_nothrow_move_assignable_v<T>);
			}

		~scoped_value_restore() noexcept
			{
			value=std::move(saved);
			}

		scoped_value_restore(const scoped_value_restore &)=delete;
		scoped_value_restore &operator=(const scoped_value_restore &)=delete;
		scoped_value_restore(scoped_value_restore &&)=delete;
		scoped_value_restore &operator=(scoped_value_restore &&)=delete;
};

template<typename Callback>
void with_zeroed_values(const Callback &callback)
{
	callback();
}

template<typename Callback,typename T,typename... Values>
void with_zeroed_values(const Callback &callback,T &value,Values &...values)
{
	scoped_value_restore<T> zero(value);
	with_zeroed_values(callback,values...);
}

struct render_proxy
{
	viewport_visual_layer layer;
	float source_x;
	float source_y;
	df::coord2d target{0,0};
	float progress;
	SDL_Texture *texture;
	bool mirrored=false;
	int32_t mirror_shift=0;
	tile_coverage coverage;
};

struct render_coverage
{
	tile_coverage all;
	std::array<tile_coverage,static_cast<size_t>(visual_render_group::count)> groups;
	std::unordered_map<int32_t,uint16_t> selected;
};

struct viewport_render
{
	df::graphic_viewportst *viewport;
	std::vector<render_proxy> proxies;
	render_coverage coverage;
};

constexpr uint16_t visual_layer_bit(viewport_visual_layer layer)
{
	return uint16_t(1U<<static_cast<uint8_t>(layer));
}

uint16_t selected_mask(
	const std::unordered_map<int32_t,uint16_t> &selected,
	int32_t index)
{
	const auto found=selected.find(index);
	return found==selected.end()?0:found->second;
}

template<size_t Layer=0,typename Callback>
void with_suppressed_visual_layers(
	const std::array<int32_t *,visual_layer_count> &layers,
	int32_t index,
	uint16_t mask,
	const Callback &callback)
{
	if constexpr(Layer==visual_layer_count)
		callback();
	else if(mask&(1U<<Layer))
		{
		scoped_value_restore<int32_t> zero(layers[Layer][index]);
		with_suppressed_visual_layers<Layer+1>(layers,index,mask,callback);
		}
	else
		with_suppressed_visual_layers<Layer+1>(layers,index,mask,callback);
}

template<typename Callback>
void with_base_suppressed(
	df::graphic_viewportst *vp,
	int32_t index,
	const Callback &callback)
{
	with_zeroed_values(
		callback,
		vp->screentexpos_background[index],
		vp->screentexpos_floor_flag[index],
		vp->screentexpos_background_two[index],
		vp->screentexpos_liquid_flag[index],
		vp->screentexpos_spatter_flag[index],
		vp->screentexpos_spatter[index],
		vp->screentexpos_ramp_flag[index],
		vp->screentexpos_shadow_flag[index],
		vp->screentexpos_building_one[index]);
}

template<typename Callback>
void with_main_suppressed(
	df::graphic_viewportst *vp,
	int32_t index,
	const Callback &callback)
{
	with_base_suppressed(vp,index,[&]
		{
		with_zeroed_values(callback,vp->screentexpos_vermin[index]);
		});
}

template<typename Callback>
void with_upper_suppressed(
	df::graphic_viewportst *vp,
	int32_t index,
	const Callback &callback)
{
	with_main_suppressed(vp,index,[&]
		{
		with_zeroed_values(
			callback,
			vp->screentexpos_building_two[index],
			vp->screentexpos_projectile[index],
			vp->screentexpos_high_flow[index],
			vp->screentexpos_top_shadow[index],
			vp->screentexpos_signpost[index]);
		});
}

void redraw_viewport_tile(
	df::renderer_2d_base *renderer,
	const viewport_render &viewport,
	int32_t x,
	int32_t y,
	bool defer_interface)
{
	df::graphic_viewportst *vp=viewport.viewport;
	const int32_t index=x*vp->dim_y+y;
	const auto redraw=[&]{renderer->update_viewport_tile(vp,x,y);};
	const auto stage=[&]
		{
		with_suppressed_visual_layers(
			visual_layers(vp),index,
			selected_mask(viewport.coverage.selected,index),redraw);
		};
	// The interface layer is the shading for levels below the camera.
	// A staged tile has a sprite drawn over it afterwards, so draw_interface_only places it instead.
	if(!defer_interface||vp->screentexpos_interface==nullptr)stage();
	else with_zeroed_values(stage,vp->screentexpos_interface[index]);
}

// Every buffer the interface-only pass zeroes has to exist before it can be zeroed.
bool interface_pass_readable(const df::graphic_viewportst *vp)
{
	return vp!=nullptr&&
		vp->screentexpos_interface!=nullptr&&
		vp->screentexpos_background!=nullptr&&
		vp->screentexpos_floor_flag!=nullptr&&
		vp->screentexpos_background_two!=nullptr&&
		vp->screentexpos_liquid_flag!=nullptr&&
		vp->screentexpos_spatter_flag!=nullptr&&
		vp->screentexpos_spatter!=nullptr&&
		vp->screentexpos_ramp_flag!=nullptr&&
		vp->screentexpos_shadow_flag!=nullptr&&
		vp->screentexpos_building_one!=nullptr&&
		vp->screentexpos_vermin!=nullptr&&
		vp->screentexpos_building_two!=nullptr&&
		vp->screentexpos_projectile!=nullptr&&
		vp->screentexpos_high_flow!=nullptr&&
		vp->screentexpos_signpost!=nullptr;
}

// Runs after the proxies so the shading covers them rather than sitting underneath.
void draw_interface_only(
	df::renderer_2d_base *renderer,
	df::graphic_viewportst *vp,
	int32_t x,
	int32_t y)
{
	if(!interface_pass_readable(vp))return;
	const int32_t index=x*vp->dim_y+y;
	const auto redraw=[&]{renderer->update_viewport_tile(vp,x,y);};
	const auto without_visuals=[&]
		{
		with_suppressed_visual_layers(
			visual_layers(vp),
			index,
			uint16_t((1U<<visual_layer_count)-1),
			redraw);
		};
	with_zeroed_values(
		without_visuals,
		vp->screentexpos_background[index],
		vp->screentexpos_floor_flag[index],
		vp->screentexpos_background_two[index],
		vp->screentexpos_liquid_flag[index],
		vp->screentexpos_spatter_flag[index],
		vp->screentexpos_spatter[index],
		vp->screentexpos_ramp_flag[index],
		vp->screentexpos_shadow_flag[index],
		vp->screentexpos_building_one[index],
		vp->screentexpos_vermin[index],
		vp->screentexpos_building_two[index],
		vp->screentexpos_projectile[index],
		vp->screentexpos_high_flow[index],
		vp->screentexpos_signpost[index]);
}

void redraw_world_tile(
	df::renderer_2d_base *renderer,
	const std::vector<viewport_render> &viewports,
	const tile_coverage &staged,
	int32_t x,
	int32_t y)
{
	// The stage pass repaints everything above the lowest across the staged tiles, after the proxies.
	const bool staged_tile=staged.count(df::coord2d(x,y))!=0;
	for(const viewport_render &viewport:viewports)
		{
		if(inside_clip(viewport.viewport,x,y))
			redraw_viewport_tile(renderer,viewport,x,y,staged_tile);
		if(staged_tile)break;
		}
}

constexpr uint16_t visual_layers_through_group(visual_render_group group)
{
	uint16_t mask=0;
	for(const auto &descriptor:visual_layer_descriptors)
		if(descriptor.render_group!=visual_render_group::designation&&
			static_cast<uint8_t>(descriptor.render_group)<=static_cast<uint8_t>(group))
			mask|=visual_layer_bit(descriptor.layer);
	return mask;
}

void redraw_above(
	df::renderer_2d_base *renderer,
	df::graphic_viewportst *vp,
	int32_t x,
	int32_t y,
	visual_render_group group,
	const std::unordered_map<int32_t,uint16_t> &selected)
{
	const int32_t index=x*vp->dim_y+y;
	const auto redraw=[&]{renderer->update_viewport_tile(vp,x,y);};
	const auto suppress_visuals=[&]
		{
		const auto stage=[&]
			{
			with_suppressed_visual_layers(
				visual_layers(vp),
				index,
				selected_mask(selected,index)|visual_layers_through_group(group),
				redraw);
			};
		// The interface layer sits above every group, so each group's redraw would paint it again.
		// draw_interface_only places it once, after the sprites.
		if(vp->screentexpos_interface==nullptr)stage();
		else with_zeroed_values(stage,vp->screentexpos_interface[index]);
		};
	if(group==visual_render_group::item||group==visual_render_group::vehicle)
		with_base_suppressed(vp,index,suppress_visuals);
	else if(group==visual_render_group::main)
		with_main_suppressed(vp,index,suppress_visuals);
	else
		with_upper_suppressed(vp,index,suppress_visuals);
}

SDL_Texture *cached_texture(
	df::renderer_2d_base *renderer,
	int32_t texpos,
	bool transparent_background=true)
{
	if(texpos==0)return nullptr;
	df::texture_fullid texture_id;
	texture_id.texpos=texpos;
	texture_id.r=texture_id.g=texture_id.b=1.0f;
	texture_id.br=texture_id.bg=texture_id.bb=0.0f;
	texture_id.flag=transparent_background?
		df::texture_fullid_flag::mask_transparent_background:
		0;
	const auto texture=renderer->tile_cache.tile_cache.find(texture_id);
	return texture==renderer->tile_cache.tile_cache.end()?
		nullptr:
		static_cast<SDL_Texture *>(texture->second);
}

void render_copy_maybe_mirrored(
	SDL_Renderer *renderer,
	SDL_Texture *texture,
	const SDL_FRect &destination,
	bool mirrored)
{
	if(mirrored&&render_api->copy_ex!=nullptr)
		{
		render_api->copy_ex(
			renderer,texture,nullptr,&destination,
			0.0,nullptr,SDL_FLIP_HORIZONTAL);
		return;
		}
	render_api->copy(renderer,texture,nullptr,&destination);
}

void draw_proxy(df::renderer_2d_base *renderer,const render_proxy &proxy)
{
	const int32_t zoom=renderer->viewport_zoom_factor;
	const int32_t target_x=tile_pixel(proxy.target.x,renderer->origin_x,zoom);
	const int32_t target_y=tile_pixel(proxy.target.y,renderer->origin_y,zoom);
	const float tile_size=float(std::max(1,zoom/4));
	const float remaining=1.0f-proxy.progress;
	const float mirror_offset=float(proxy.mirror_shift)*tile_size;
	const SDL_FRect destination=
		{
		target_x+(proxy.source_x-proxy.target.x)*tile_size*remaining+mirror_offset,
		target_y+(proxy.source_y-proxy.target.y)*tile_size*remaining,
		tile_size,
		tile_size
		};
	render_copy_maybe_mirrored(
		static_cast<SDL_Renderer *>(renderer->sdl_renderer),
		proxy.texture,
		destination,
		proxy.mirrored);
}

std::vector<render_proxy> collect_proxies(
	df::renderer_2d_base *renderer,
	df::graphic_viewportst *vp,
	const sprite_flip_feature &sprite_flip)
{
	std::vector<render_proxy> proxies;
	auto layers=visual_layers(vp);
	auto previous_layers=visual_layers(vp,true);
	for(uint8_t draw_order=0;draw_order<visual_layer_count;++draw_order)
		{
		const viewport_visual_layer visual_layer=visual_layer_at_draw_order(draw_order);
		const size_t layer=static_cast<size_t>(visual_layer);
		const auto &descriptor=get_visual_layer_descriptor(visual_layer);
		const df::coord2d center_offset(descriptor.center_x,descriptor.center_y);
		for(int32_t y=0;y<vp->dim_y;++y)
			{
			for(int32_t x=0;x<vp->dim_x;++x)
				{
				const int32_t index=x*vp->dim_y+y;
				const int32_t texpos=layers[layer][index];
				if(texpos==0)continue;
				const auto movement=animation_manager.get_movement(
					vp,static_cast<viewport_visual_layer>(layer),x,y);
				if(!movement.active)continue;
				const df::coord2d tile(x,y);
				const df::coord2d anchor_pos=tile+center_offset;
				const int32_t inherited_source_x=inherited_visual_source_tile(
					x,movement.source_x,x);
				const int32_t inherited_source_y=inherited_visual_source_tile(
					y,movement.source_y,y);
				const bool inherited_source_in_bounds=
					inherited_source_x>=0&&inherited_source_x<vp->dim_x&&
					inherited_source_y>=0&&inherited_source_y<vp->dim_y;
				if(!visual_layer_moves_independently(visual_layer))
					{
					bool anchored=false;
					for(const render_proxy &anchor:proxies)
						{
						if(anchor.layer==viewport_visual_layer::center&&
							std::abs(anchor.target.x-x)<=1&&
							std::abs(anchor.target.y-y)<=1&&
							anchor.source_x-anchor.target.x==movement.source_x-x&&
							anchor.source_y-anchor.target.y==movement.source_y-y&&
							anchor.progress==movement.progress)anchored=true;
						}
					if(!anchored)continue;
					}
					if((visual_layer==viewport_visual_layer::item||
						visual_layer==viewport_visual_layer::designation)&&
						movement.inherited)
					{
					if(visual_layer==viewport_visual_layer::item&&
						vp->screentexpos_old[index]!=0)continue;
					if(!inherited_source_in_bounds)continue;
					const int32_t source=
						inherited_source_x*vp->dim_y+inherited_source_y;
					if(!visual_moved_between_tiles(
						visual_layer,
							layers[layer],
							previous_layers[layer],
							source,
							index))continue;
					}
				if(!visual_layer_moves_independently(visual_layer)&&
					visual_layer!=viewport_visual_layer::designation&&movement.inherited)
					{
					const bool fragment_moved=inherited_source_in_bounds&&
						visual_moved_between_tiles(
							visual_layer,layers[layer],previous_layers[layer],
							inherited_source_x*vp->dim_y+inherited_source_y,index);
					if(!fragment_moved)
						{
					bool owns_fragment=false;
					for(const render_proxy &anchor:proxies)
						if(anchor.layer==viewport_visual_layer::center&&
							anchor.target==anchor_pos&&
							anchor.source_x-anchor.target.x==movement.source_x-x&&
							anchor.source_y-anchor.target.y==movement.source_y-y&&
							anchor.progress==movement.progress)owns_fragment=true;
					if(!owns_fragment)continue;
						}
					}

				// Items, vehicles and designations keep their vanilla orientation.
				const visual_render_group group=
					get_visual_render_group(visual_layer);
				// Facing is read from the anchor tile so every fragment of one creature agrees.
				const bool mirrored=sprite_flip.should_mirror(
					group,
					animation_manager.get_facing(
						vp,anchor_pos.x,anchor_pos.y));
				// The anchor's own layer has center_x 0, so it flips in place.
				const int32_t mirror_shift=
					mirrored?
					sprite_flip.mirror_shift(tile.x,anchor_pos.x):
					0;
				render_proxy proxy=
					{
					static_cast<viewport_visual_layer>(layer),
					movement.source_x,
					movement.source_y,
					tile,
					movement.progress,
					nullptr,
					mirrored,
					mirror_shift,
					{}
					};
				bool blocked=false;
				for(int32_t coverage_x=int32_t(std::floor(
						std::min(proxy.source_x,float(x))));
					coverage_x<=int32_t(std::ceil(
						std::max(proxy.source_x,float(x))));++coverage_x)
					{
					for(int32_t coverage_y=int32_t(std::floor(
							std::min(proxy.source_y,float(y))));
						coverage_y<=int32_t(std::ceil(
							std::max(proxy.source_y,float(y))));++coverage_y)
						{
						if(!inside_clip(vp,coverage_x,coverage_y))
							{
							blocked=true;
							break;
							}
						if(get_visual_render_group(proxy.layer)==visual_render_group::main&&
							has_fire(vp,coverage_x,coverage_y))
							{
							blocked=true;
							break;
							}
						proxy.coverage.emplace(coverage_x,coverage_y);
						}
					if(blocked)break;
					}
				if(blocked)continue;
				if(proxy.mirror_shift!=0)
					{
					tile_coverage mirrored_coverage;
					for(const auto &tile:proxy.coverage)
						mirrored_coverage.insert(tile+df::coord2d(proxy.mirror_shift,0));
					for(const auto &tile:mirrored_coverage)
						{
						if(!inside_clip(vp,tile.x,tile.y))
							{
							blocked=true;
							break;
							}
						if(get_visual_render_group(proxy.layer)==visual_render_group::main&&
							has_fire(vp,tile.x,tile.y))
							{
							blocked=true;
							break;
							}
						proxy.coverage.insert(tile);
						}
					if(blocked)continue;
					}

				proxy.texture=cached_texture(renderer,texpos);
				if(proxy.texture==nullptr)continue;
				proxies.push_back(std::move(proxy));
				}
			}
		}

	// A creature that has stopped still needs its mirrored sprite painted each frame.
	// Otherwise the engine repaints it natively and the two orientations alternate between steps.
	// A fragment's tile is its anchor minus the layer's centre offset, inverting the moving path.
	if(sprite_flip.enabled())
		{
		for(int32_t anchor_x=0;anchor_x<vp->dim_x;++anchor_x)
			{
			for(int32_t anchor_y=0;anchor_y<vp->dim_y;++anchor_y)
				{
				if(animation_manager.get_facing(vp,anchor_x,anchor_y)==
					native_sprite_facing)continue;
				for(uint8_t draw_order=0;draw_order<visual_layer_count;++draw_order)
					{
					const viewport_visual_layer visual_layer=
						visual_layer_at_draw_order(draw_order);
					const visual_render_group group=
						get_visual_render_group(visual_layer);
					if(group!=visual_render_group::main&&
						group!=visual_render_group::upper)continue;
					const auto &descriptor=get_visual_layer_descriptor(visual_layer);
					const df::coord2d tile=df::coord2d(anchor_x,anchor_y)-
						df::coord2d(descriptor.center_x,descriptor.center_y);
					const auto &[x,y]=tile;
					if(x<0||x>=vp->dim_x||y<0||y>=vp->dim_y)continue;
					const size_t layer=static_cast<size_t>(visual_layer);
					const int32_t texpos=layers[layer][x*vp->dim_y+y];
					if(texpos==0)continue;
					bool already_drawn=false;
					for(const render_proxy &existing:proxies)
						if(existing.layer==visual_layer&&
							existing.target==tile)
							already_drawn=true;
					if(already_drawn)continue;

					// source == target at progress 1.0 draws in place, moved only by mirror_shift.
					render_proxy proxy=
						{
						visual_layer,
						float(x),
						float(y),
						tile,
						1.0f,
						nullptr,
						true,
						sprite_flip.mirror_shift(x,anchor_x),
						{}
						};
					// The sprite lands on x+mirror_shift, so that interval must be repaintable.
					// The shift has either sign, so order the interval ends first.
					const int32_t coverage_first=std::min<int32_t>(x,x+proxy.mirror_shift);
					const int32_t coverage_last=std::max<int32_t>(x,x+proxy.mirror_shift);
					bool blocked=false;
					for(int32_t coverage_x=coverage_first;
						coverage_x<=coverage_last;++coverage_x)
						{
						if(!inside_clip(vp,coverage_x,y)||
							(group==visual_render_group::main&&
							has_fire(vp,coverage_x,y)))
							{
							blocked=true;
							break;
							}
						proxy.coverage.emplace(coverage_x,y);
						}
					if(blocked)continue;

					proxy.texture=cached_texture(renderer,texpos);
					if(proxy.texture==nullptr)continue;
					proxies.push_back(std::move(proxy));
					}
				}
			}
		}
	return proxies;
}

render_coverage collect_coverage(
	const std::vector<render_proxy> &proxies,
	int32_t dim_y)
{
	render_coverage coverage;
	for(const render_proxy &proxy:proxies)
		{
		coverage.all.insert(proxy.coverage.begin(),proxy.coverage.end());
		coverage.selected[proxy.target.x*dim_y+proxy.target.y]|=
			visual_layer_bit(proxy.layer);
		auto &group=coverage.groups[static_cast<size_t>(get_visual_render_group(proxy.layer))];
		group.insert(proxy.coverage.begin(),proxy.coverage.end());
		}
	return coverage;
}

std::vector<df::graphic_viewportst *> active_viewports(
	const movement_frame_context &frame)
{
	std::vector<df::graphic_viewportst *> viewports;
	for(int32_t lower=7;lower>=0;--lower)
		{
		df::graphic_viewportst *vp=frame.lower_viewports[size_t(lower)];
		if(viewport_readable(vp,frame.window_pos))
			viewports.push_back(vp);
		}
	if(viewport_readable(frame.main_viewport,frame.window_pos))
		viewports.push_back(frame.main_viewport);
	return viewports;
}

std::vector<viewport_render> collect_viewport_renders(
	df::renderer_2d_base *renderer,
	const std::vector<df::graphic_viewportst *> &viewports,
	const sprite_flip_feature &sprite_flip)
{
	std::vector<viewport_render> renders;
	renders.reserve(viewports.size());
	for(df::graphic_viewportst *vp:viewports)
		{
		viewport_render render={vp,collect_proxies(renderer,vp,sprite_flip),{}};
		render.coverage=collect_coverage(render.proxies,vp->dim_y);
		renders.push_back(std::move(render));
		}
	return renders;
}

tile_coverage collect_viewport_coverage(
	const std::vector<viewport_render> &viewports)
{
	tile_coverage coverage;
	for(const viewport_render &viewport:viewports)
		coverage.insert(
			viewport.coverage.all.begin(),viewport.coverage.all.end());
	return coverage;
}

void draw_interpolation_stages(
	df::renderer_2d_base *renderer,
	df::graphic_viewportst *vp,
	const std::vector<render_proxy> &proxies,
	const render_coverage &coverage)
{
	for(size_t index=0;index<coverage.groups.size();++index)
		{
		const auto group=static_cast<visual_render_group>(index);
		for(const render_proxy &proxy:proxies)
			if(get_visual_render_group(proxy.layer)==group)draw_proxy(renderer,proxy);
		if(group==visual_render_group::designation)continue;
		for(const auto &[x,y]:coverage.groups[index])
			redraw_above(renderer,vp,x,y,group,coverage.selected);
		}
}

void redraw_viewport_tiles(
	df::renderer_2d_base *renderer,
	const viewport_render &viewport,
	const tile_coverage &coverage)
{
	df::graphic_viewportst *vp=viewport.viewport;
	for(const auto &[x,y]:coverage)
		{
		if(!inside_clip(vp,x,y))continue;
		redraw_viewport_tile(renderer,viewport,x,y,true);
		}
}

void draw_viewport_interpolation_stages(
	df::renderer_2d_base *renderer,
	const std::vector<viewport_render> &viewports,
	const tile_coverage &coverage)
{
	for(size_t index=0;index<viewports.size();++index)
		{
		// A lower z-level's proxy must be covered by the next viewport's fog and terrain.
		// Reapply that viewport before its own proxies, matching DF's lower-to-main draw order.
		if(index>0)redraw_viewport_tiles(renderer,viewports[index],coverage);
		const viewport_render &viewport=viewports[index];
		draw_interpolation_stages(
			renderer,viewport.viewport,viewport.proxies,viewport.coverage);
		// A viewport shades everything drawn beneath it, so this covers every staged tile.
		// Restricting it to the tiles this viewport has sprites on would not deepen with distance.
		for(const auto &[x,y]:coverage)
			{
			if(inside_clip(viewport.viewport,x,y))
				draw_interface_only(renderer,viewport.viewport,x,y);
			}
		}
}

bool has_mirrored_viewport_facing(
	const std::vector<df::graphic_viewportst *> &viewports)
{
	for(const df::graphic_viewportst *vp:viewports)
		if(animation_manager.has_mirrored_facing(vp))return true;
	return false;
}

void render_world(
	const movement_frame_context &frame,
	const camera_render_offset &camera,
	const sprite_flip_feature &sprite_flip)
{
	df::renderer_2d_base *renderer=frame.renderer;
	df::graphic_viewportst *vp=frame.main_viewport;
	const std::vector<df::graphic_viewportst *> viewports=active_viewports(frame);
	if(renderer==nullptr||
		!viewport_readable(vp,frame.window_pos)||
		renderer->sdl_renderer==nullptr||
		!frame.render.valid())return;

	const bool glide=camera.x!=0||camera.y!=0;
	if(!glide&&!animation_manager.requires_full_redraw()&&
		(!sprite_flip.enabled()||!has_mirrored_viewport_facing(viewports)))
		return;

	render_api=&frame.render;
	std::vector<viewport_render> viewport_renders=
		collect_viewport_renders(renderer,viewports,sprite_flip);
	tile_coverage coverage=collect_viewport_coverage(viewport_renders);

	SDL_Renderer *sdl_renderer=static_cast<SDL_Renderer *>(renderer->sdl_renderer);
	const int32_t zoom=renderer->viewport_zoom_factor;
	const int32_t tile_size=std::max(1,zoom/4);

	if(glide)
		{
		const SDL_Rect map_rect=
			{
			tile_pixel(vp->clipx[0],renderer->origin_x,zoom),
			tile_pixel(vp->clipy[0],renderer->origin_y,zoom),
			tile_pixel(vp->clipx[1]+1,renderer->origin_x,zoom)-
				tile_pixel(vp->clipx[0],renderer->origin_x,zoom),
			tile_pixel(vp->clipy[1]+1,renderer->origin_y,zoom)-
				tile_pixel(vp->clipy[0],renderer->origin_y,zoom)
			};
		render_api->set_clip_rect(sdl_renderer,&map_rect);
		Uint8 old_r=0,old_g=0,old_b=0,old_a=255;
		render_api->get_draw_color(sdl_renderer,&old_r,&old_g,&old_b,&old_a);
		render_api->set_draw_color(sdl_renderer,0,0,0,255);
		render_api->fill_rect(sdl_renderer,&map_rect);
		render_api->set_draw_color(sdl_renderer,old_r,old_g,old_b,old_a);

		const int32_t saved_origin_x=renderer->origin_x;
		const int32_t saved_origin_y=renderer->origin_y;
		renderer->origin_x+=camera.x;
		renderer->origin_y+=camera.y;
		for(int32_t x=vp->clipx[0];x<=vp->clipx[1];++x)
			{
			for(int32_t y=vp->clipy[0];y<=vp->clipy[1];++y)
				redraw_world_tile(renderer,viewport_renders,coverage,x,y);
			}
		draw_viewport_interpolation_stages(renderer,viewport_renders,coverage);
		renderer->origin_x=saved_origin_x;
		renderer->origin_y=saved_origin_y;
		render_api->set_clip_rect(sdl_renderer,nullptr);
		previous_coverage.clear();
		render_api=nullptr;
		return;
		}

	tile_coverage redraw_coverage=coverage;
	redraw_coverage.insert(previous_coverage.begin(),previous_coverage.end());
	Uint8 old_r=0,old_g=0,old_b=0,old_a=255;
	render_api->get_draw_color(sdl_renderer,&old_r,&old_g,&old_b,&old_a);
	render_api->set_draw_color(sdl_renderer,0,0,0,255);
	for(const auto &[x,y]:redraw_coverage)
		{
		if(!inside_clip(vp,x,y))continue;
		const SDL_Rect tile_rect=
			{
			tile_pixel(x,renderer->origin_x,zoom),
			tile_pixel(y,renderer->origin_y,zoom),
			tile_size,
			tile_size
			};
		render_api->fill_rect(sdl_renderer,&tile_rect);
		}
	render_api->set_draw_color(sdl_renderer,old_r,old_g,old_b,old_a);

	for(const auto &[x,y]:redraw_coverage)
		{
		if(inside_clip(vp,x,y))
			redraw_world_tile(renderer,viewport_renders,coverage,x,y);
		}
	draw_viewport_interpolation_stages(renderer,viewport_renders,coverage);
	previous_coverage=std::move(coverage);
	render_api=nullptr;
}

} // namespace

void movement_feature::reset()
{
	animation_manager=visual_animation_manager();
	previous_coverage.clear();
	visual_context_revision=0;
	previous_viewport=nullptr;
	previous_view_signature={};
	has_view_signature=false;
	previous_pan={0,0};
	has_pan_context=false;
	render_api=nullptr;
}

movement_prepare_result movement_feature::prepare(
	const movement_frame_context &frame)
{
	movement_prepare_result result;
	const std::vector<df::graphic_viewportst *> viewports=active_viewports(frame);
	if(frame.main_viewport!=nullptr&&frame.renderer!=nullptr)
		result.context_changed=update_visual_context(frame);
	animation_manager.begin_frame(frame.now_ms);
	for(df::graphic_viewportst *viewport:viewports)
		animation_manager.synchronize_viewport(
			animation_input(viewport,frame.window_pos));
	animation_manager.end_frame();
	result.frame_delta_ms=animation_manager.get_frame_delta_ms();
	result.main_viewport_readable=viewport_readable(
		frame.main_viewport,frame.window_pos);
	return result;
}

void movement_feature::render(
	const movement_frame_context &frame,
	const camera_render_offset &camera,
	const sprite_flip_feature &sprite_flip)
{
	render_world(frame,camera,sprite_flip);
}
