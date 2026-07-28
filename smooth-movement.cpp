// SPDX-License-Identifier: MIT

#include "PluginManager.h"
#include "VTableInterpose.h"

#include "df/graphic.h"
#include "df/graphic_viewportst.h"
#include "df/renderer_2d_base.h"
#include "df/texture_fullid.h"

#include "visual_animation.h"

#include <SDL_render.h>
#include <SDL_timer.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace DFHack;

DFHACK_PLUGIN("smooth-movement");
DFHACK_PLUGIN_IS_ENABLED(is_enabled);

REQUIRE_GLOBAL(gps);
REQUIRE_GLOBAL(window_x);
REQUIRE_GLOBAL(window_y);
REQUIRE_GLOBAL(window_z);

namespace {

constexpr const char *plugin_version="0.2.0";

#ifdef WIN32
constexpr const char *sdl_library="SDL2.dll";
#else
constexpr const char *sdl_library="libSDL2-2.0.so.0";
#endif

// Runtime harness for the engine-owned visual state; gameplay data is never read.
DFLibrary *sdl_handle=nullptr;
using RenderCopyF=int (*)(SDL_Renderer *,SDL_Texture *,const SDL_Rect *,const SDL_FRect *);
using RenderFillRect=int (*)(SDL_Renderer *,const SDL_Rect *);
using GetRenderDrawColor=int (*)(SDL_Renderer *,Uint8 *,Uint8 *,Uint8 *,Uint8 *);
using SetRenderDrawColor=int (*)(SDL_Renderer *,Uint8,Uint8,Uint8,Uint8);
using GetTicks=Uint32 (*)();
RenderCopyF render_copy_f=nullptr;
RenderFillRect render_fill_rect=nullptr;
GetRenderDrawColor get_render_draw_color=nullptr;
SetRenderDrawColor set_render_draw_color=nullptr;
GetTicks get_ticks=nullptr;

visual_animation_managerst animation_manager;
std::set<std::pair<int32_t,int32_t>> previous_coverage;
uint64_t visual_context_revision=0;
const void *previous_viewport=nullptr;
std::array<int32_t,14> previous_view_signature{};
bool has_view_signature=false;

constexpr uint32_t fire_bits=0x70000000U;

void update_visual_context(
	const df::renderer_2d_base *renderer,
	const df::graphic_viewportst *vp)
{
	const std::array<int32_t,14> signature=
		{
		window_x?*window_x:0,
		window_y?*window_y:0,
		window_z?*window_z:0,
		vp->dim_x,
		vp->dim_y,
		vp->clipx[0],
		vp->clipx[1],
		vp->clipy[0],
		vp->clipy[1],
		renderer->viewport_zoom_factor,
		renderer->origin_x,
		renderer->origin_y,
		gps->dimx,
		gps->dimy
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
}

std::array<int32_t *,6> creature_layers(df::graphic_viewportst *vp)
{
	return {
		vp->screentexpos_right_creature,
		vp->screentexpos,
		vp->screentexpos_left_creature,
		vp->screentexpos_upright_creature,
		vp->screentexpos_up_creature,
		vp->screentexpos_upleft_creature
		};
}

viewport_visual_animation_inputst animation_input(df::graphic_viewportst *vp)
{
	return {
		vp,
		vp->dim_x,
		vp->dim_y,
		visual_context_revision,
		{
			vp->screentexpos_right_creature,
			vp->screentexpos,
			vp->screentexpos_left_creature,
			vp->screentexpos_upright_creature,
			vp->screentexpos_up_creature,
			vp->screentexpos_upleft_creature
			},
		{
			vp->screentexpos_right_creature_old,
			vp->screentexpos_old,
			vp->screentexpos_left_creature_old,
			vp->screentexpos_upright_creature_old,
			vp->screentexpos_up_creature_old,
			vp->screentexpos_upleft_creature_old
			}
		};
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
	return vp->screentexpos_spatter_flag[x*vp->dim_y+y]&fire_bits;
}

template<typename T>
class scoped_zerost
{
	T *value_ptr;
	T saved{};

	public:
		scoped_zerost(T *ptr,bool enabled=true):value_ptr(enabled?ptr:nullptr)
			{
			if(value_ptr!=nullptr)
				{
				saved=*value_ptr;
				*value_ptr=0;
				}
			}

		~scoped_zerost()
			{
			if(value_ptr!=nullptr)*value_ptr=saved;
			}

		scoped_zerost(const scoped_zerost &)=delete;
		scoped_zerost &operator=(const scoped_zerost &)=delete;
};

struct render_proxyst
{
	viewport_creature_layer layer;
	int32_t source_x;
	int32_t source_y;
	int32_t target_x;
	int32_t target_y;
	int32_t texpos;
	float progress;
	SDL_Texture *texture;
	std::set<std::pair<int32_t,int32_t>> coverage;
};

void redraw_world_tile(
	df::renderer_2d_base *renderer,
	df::graphic_viewportst *vp,
	int32_t x,
	int32_t y,
	const std::unordered_map<int32_t,uint8_t> &selected)
{
	const int32_t index=x*vp->dim_y+y;
	const auto found=selected.find(index);
	const uint8_t mask=found==selected.end()?0:found->second;
	auto layers=creature_layers(vp);
	scoped_zerost<int32_t> right(layers[0]+index,mask&(1U<<0));
	scoped_zerost<int32_t> center(layers[1]+index,mask&(1U<<1));
	scoped_zerost<int32_t> left(layers[2]+index,mask&(1U<<2));
	scoped_zerost<int32_t> upright(layers[3]+index,mask&(1U<<3));
	scoped_zerost<int32_t> up(layers[4]+index,mask&(1U<<4));
	scoped_zerost<int32_t> upleft(layers[5]+index,mask&(1U<<5));

	for(int32_t lower=7;lower>=0;--lower)
		{
		df::graphic_viewportst *lower_vp=gps->lower_viewport[lower];
		if(lower_vp!=nullptr&&lower_vp->flag.bits.active)
			renderer->update_viewport_tile(lower_vp,x,y);
		}
	renderer->update_viewport_tile(vp,x,y);
}

void redraw_above_main(
	df::renderer_2d_base *renderer,
	df::graphic_viewportst *vp,
	int32_t x,
	int32_t y,
	const std::unordered_map<int32_t,uint8_t> &selected)
{
	const int32_t index=x*vp->dim_y+y;
	const auto found=selected.find(index);
	const uint8_t mask=found==selected.end()?0:found->second;

	scoped_zerost<int32_t> background(vp->screentexpos_background+index);
	scoped_zerost<uint64_t> floor_flag(vp->screentexpos_floor_flag+index);
	scoped_zerost<int32_t> background_two(vp->screentexpos_background_two+index);
	scoped_zerost<uint32_t> liquid_flag(vp->screentexpos_liquid_flag+index);
	scoped_zerost<uint32_t> spatter_flag(vp->screentexpos_spatter_flag+index);
	scoped_zerost<int32_t> spatter(vp->screentexpos_spatter+index);
	scoped_zerost<uint64_t> ramp_flag(vp->screentexpos_ramp_flag+index);
	scoped_zerost<uint32_t> shadow_flag(vp->screentexpos_shadow_flag+index);
	scoped_zerost<int32_t> building_one(vp->screentexpos_building_one+index);
	scoped_zerost<int32_t> item(vp->screentexpos_item+index);
	scoped_zerost<int32_t> vehicle(vp->screentexpos_vehicle+index);
	scoped_zerost<int32_t> vermin(vp->screentexpos_vermin+index);
	scoped_zerost<int32_t> right(vp->screentexpos_right_creature+index);
	scoped_zerost<int32_t> center(vp->screentexpos+index);
	scoped_zerost<int32_t> left(vp->screentexpos_left_creature+index);
	scoped_zerost<int32_t> upright(
		vp->screentexpos_upright_creature+index,mask&(1U<<3));
	scoped_zerost<int32_t> up(
		vp->screentexpos_up_creature+index,mask&(1U<<4));
	scoped_zerost<int32_t> upleft(
		vp->screentexpos_upleft_creature+index,mask&(1U<<5));
	renderer->update_viewport_tile(vp,x,y);
}

void redraw_above_upper(
	df::renderer_2d_base *renderer,
	df::graphic_viewportst *vp,
	int32_t x,
	int32_t y)
{
	const int32_t index=x*vp->dim_y+y;
	scoped_zerost<int32_t> background(vp->screentexpos_background+index);
	scoped_zerost<uint64_t> floor_flag(vp->screentexpos_floor_flag+index);
	scoped_zerost<int32_t> background_two(vp->screentexpos_background_two+index);
	scoped_zerost<uint32_t> liquid_flag(vp->screentexpos_liquid_flag+index);
	scoped_zerost<uint32_t> spatter_flag(vp->screentexpos_spatter_flag+index);
	scoped_zerost<int32_t> spatter(vp->screentexpos_spatter+index);
	scoped_zerost<uint64_t> ramp_flag(vp->screentexpos_ramp_flag+index);
	scoped_zerost<uint32_t> shadow_flag(vp->screentexpos_shadow_flag+index);
	scoped_zerost<int32_t> building_one(vp->screentexpos_building_one+index);
	scoped_zerost<int32_t> item(vp->screentexpos_item+index);
	scoped_zerost<int32_t> vehicle(vp->screentexpos_vehicle+index);
	scoped_zerost<int32_t> vermin(vp->screentexpos_vermin+index);
	scoped_zerost<int32_t> right(vp->screentexpos_right_creature+index);
	scoped_zerost<int32_t> center(vp->screentexpos+index);
	scoped_zerost<int32_t> left(vp->screentexpos_left_creature+index);
	scoped_zerost<int32_t> building_two(vp->screentexpos_building_two+index);
	scoped_zerost<int32_t> projectile(vp->screentexpos_projectile+index);
	scoped_zerost<int32_t> high_flow(vp->screentexpos_high_flow+index);
	scoped_zerost<int32_t> top_shadow(vp->screentexpos_top_shadow+index);
	scoped_zerost<int32_t> signpost(vp->screentexpos_signpost+index);
	scoped_zerost<int32_t> upright(vp->screentexpos_upright_creature+index);
	scoped_zerost<int32_t> up(vp->screentexpos_up_creature+index);
	scoped_zerost<int32_t> upleft(vp->screentexpos_upleft_creature+index);
	renderer->update_viewport_tile(vp,x,y);
}

void draw_proxy(df::renderer_2d_base *renderer,const render_proxyst &proxy)
{
	const int32_t zoom=renderer->viewport_zoom_factor;
	const int32_t target_x=tile_pixel(proxy.target_x,renderer->origin_x,zoom);
	const int32_t target_y=tile_pixel(proxy.target_y,renderer->origin_y,zoom);
	const int32_t source_x=tile_pixel(proxy.source_x,renderer->origin_x,zoom);
	const int32_t source_y=tile_pixel(proxy.source_y,renderer->origin_y,zoom);
	const float tile_size=float(zoom==128?32:std::max(1,zoom*32/128));
	const SDL_FRect destination=
		{
		source_x+(target_x-source_x)*proxy.progress,
		source_y+(target_y-source_y)*proxy.progress,
		tile_size,
		tile_size
		};
	render_copy_f(
		static_cast<SDL_Renderer *>(renderer->sdl_renderer),
		proxy.texture,
		nullptr,
		&destination);
}

std::vector<render_proxyst> collect_proxies(
	df::renderer_2d_base *renderer,
	df::graphic_viewportst *vp)
{
	std::vector<render_proxyst> proxies;
	auto layers=creature_layers(vp);
	for(int32_t y=0;y<vp->dim_y;++y)
		{
		for(int32_t x=0;x<vp->dim_x;++x)
			{
			const int32_t index=x*vp->dim_y+y;
			for(size_t layer=0;layer<layers.size();++layer)
				{
				const int32_t texpos=layers[layer][index];
				if(texpos==0)continue;
				const auto movement=animation_manager.get_movement(
					vp,static_cast<viewport_creature_layer>(layer),x,y);
				if(!movement.active)continue;

				render_proxyst proxy=
					{
					static_cast<viewport_creature_layer>(layer),
					movement.source_x,
					movement.source_y,
					x,
					y,
					texpos,
					movement.progress,
					nullptr,
					{}
					};
				bool blocked=false;
				for(int32_t coverage_x=std::min(proxy.source_x,x);
					coverage_x<=std::max(proxy.source_x,x);++coverage_x)
					{
					for(int32_t coverage_y=std::min(proxy.source_y,y);
						coverage_y<=std::max(proxy.source_y,y);++coverage_y)
						{
						if(!inside_clip(vp,coverage_x,coverage_y))
							{
							blocked=true;
							break;
							}
						if(layer<3&&has_fire(vp,coverage_x,coverage_y))
							{
							blocked=true;
							break;
							}
						proxy.coverage.emplace(coverage_x,coverage_y);
						}
					if(blocked)break;
					}
				if(blocked)continue;

				df::texture_fullid texture_id;
				texture_id.texpos=texpos;
				texture_id.r=texture_id.g=texture_id.b=1.0f;
				texture_id.br=texture_id.bg=texture_id.bb=0.0f;
				texture_id.flag=
					df::texture_fullid_flag::mask_transparent_background;
				const auto texture=renderer->tile_cache.tile_cache.find(texture_id);
				if(texture==renderer->tile_cache.tile_cache.end())
					{
					continue;
					}
				proxy.texture=static_cast<SDL_Texture *>(texture->second);
				proxies.push_back(std::move(proxy));
				}
			}
		}
	return proxies;
}

void render_interpolated_world(df::renderer_2d_base *renderer)
{
	df::graphic_viewportst *vp=gps?gps->main_viewport:nullptr;

	if(vp!=nullptr)update_visual_context(renderer,vp);
	animation_manager.begin_frame(get_ticks());
	if(vp!=nullptr)
		{
		const auto input=animation_input(vp);
		animation_manager.synchronize_viewport(
			input,
			vp->flag.bits.active);
		}
	animation_manager.end_frame();

	if(vp==nullptr||!vp->flag.bits.active||
		!animation_manager.requires_full_redraw()||
		render_copy_f==nullptr||renderer->sdl_renderer==nullptr)
		return;

	std::vector<render_proxyst> proxies=collect_proxies(renderer,vp);
	std::set<std::pair<int32_t,int32_t>> current_coverage;
	std::set<std::pair<int32_t,int32_t>> main_coverage;
	std::set<std::pair<int32_t,int32_t>> upper_coverage;
	std::unordered_map<int32_t,uint8_t> selected;
	for(const render_proxyst &proxy:proxies)
		{
		current_coverage.insert(proxy.coverage.begin(),proxy.coverage.end());
		auto &layer_mask=selected[proxy.target_x*vp->dim_y+proxy.target_y];
		layer_mask|=uint8_t(1U<<static_cast<uint8_t>(proxy.layer));
		if(static_cast<uint8_t>(proxy.layer)<3)
			main_coverage.insert(proxy.coverage.begin(),proxy.coverage.end());
		else
			upper_coverage.insert(proxy.coverage.begin(),proxy.coverage.end());
		}

	std::set<std::pair<int32_t,int32_t>> redraw_coverage=current_coverage;
	redraw_coverage.insert(previous_coverage.begin(),previous_coverage.end());
	SDL_Renderer *sdl_renderer=static_cast<SDL_Renderer *>(renderer->sdl_renderer);
	Uint8 old_r=0,old_g=0,old_b=0,old_a=255;
	get_render_draw_color(sdl_renderer,&old_r,&old_g,&old_b,&old_a);
	set_render_draw_color(sdl_renderer,0,0,0,255);
	const int32_t zoom=renderer->viewport_zoom_factor;
	const int32_t tile_size=zoom==128?32:std::max(1,zoom*32/128);
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
		render_fill_rect(sdl_renderer,&tile_rect);
		}
	set_render_draw_color(sdl_renderer,old_r,old_g,old_b,old_a);

	for(const auto &[x,y]:redraw_coverage)
		{
		if(inside_clip(vp,x,y))redraw_world_tile(renderer,vp,x,y,selected);
		}
	for(const render_proxyst &proxy:proxies)
		{
		if(static_cast<uint8_t>(proxy.layer)<3)draw_proxy(renderer,proxy);
		}
	for(const auto &[x,y]:main_coverage)
		redraw_above_main(renderer,vp,x,y,selected);
	for(const render_proxyst &proxy:proxies)
		{
		if(static_cast<uint8_t>(proxy.layer)>=3)draw_proxy(renderer,proxy);
		}
	for(const auto &[x,y]:upper_coverage)
		redraw_above_upper(renderer,vp,x,y);

	previous_coverage=std::move(current_coverage);
}

struct renderer_hook : df::renderer_2d_base
{
	typedef df::renderer_2d_base interpose_base;
	DEFINE_VMETHOD_INTERPOSE(void,update_all,());
};

IMPLEMENT_VMETHOD_INTERPOSE(renderer_hook,update_all);

void renderer_hook::interpose_fn_update_all()
{
	// update_all is the existing UI stage, so world correction must run first.
	render_interpolated_world(this);
	INTERPOSE_NEXT(update_all)();
}

bool load_sdl(color_ostream &out)
{
	sdl_handle=OpenPlugin(sdl_library);
	if(sdl_handle==nullptr)
		{
		out.printerr("smooth-movement: could not load SDL2\n");
		return false;
		}
	render_copy_f=reinterpret_cast<RenderCopyF>(
		LookupPlugin(sdl_handle,"SDL_RenderCopyF"));
	render_fill_rect=reinterpret_cast<RenderFillRect>(
		LookupPlugin(sdl_handle,"SDL_RenderFillRect"));
	get_render_draw_color=reinterpret_cast<GetRenderDrawColor>(
		LookupPlugin(sdl_handle,"SDL_GetRenderDrawColor"));
	set_render_draw_color=reinterpret_cast<SetRenderDrawColor>(
		LookupPlugin(sdl_handle,"SDL_SetRenderDrawColor"));
	get_ticks=reinterpret_cast<GetTicks>(LookupPlugin(sdl_handle,"SDL_GetTicks"));
	if(render_copy_f==nullptr||render_fill_rect==nullptr||
		get_render_draw_color==nullptr||set_render_draw_color==nullptr||
		get_ticks==nullptr)
		{
		out.printerr("smooth-movement: required SDL2 functions are unavailable\n");
		ClosePlugin(sdl_handle);
		sdl_handle=nullptr;
		return false;
		}
	return true;
}

void reset_state()
{
	animation_manager=visual_animation_managerst();
	previous_coverage.clear();
	visual_context_revision=0;
	previous_viewport=nullptr;
	previous_view_signature={};
	has_view_signature=false;
}

command_result status_command(
	color_ostream &out,
	std::vector<std::string> &parameters)
{
	if(!parameters.empty())return CR_WRONG_USAGE;
	out.print(
		"smooth-movement {}: {}\n",
		plugin_version,
		is_enabled?"enabled":"disabled");
	return CR_OK;
}

} // namespace

DFhackCExport command_result
plugin_init(color_ostream &,std::vector<PluginCommand> &commands)
{
	commands.emplace_back(
		"smooth-movement",
		"Show smooth movement status.",
		status_command);
	return CR_OK;
}

DFhackCExport command_result plugin_enable(color_ostream &out,bool enable)
{
	if(is_enabled==enable)return CR_OK;
	if(enable)
		{
		reset_state();
		if(!load_sdl(out))return CR_FAILURE;
		if(!INTERPOSE_HOOK(renderer_hook,update_all).apply())
			{
			out.printerr("smooth-movement: could not hook the 2D renderer\n");
			ClosePlugin(sdl_handle);
			sdl_handle=nullptr;
			return CR_FAILURE;
			}
		}
	else
		{
		INTERPOSE_HOOK(renderer_hook,update_all).remove();
		reset_state();
		if(sdl_handle!=nullptr)ClosePlugin(sdl_handle);
		sdl_handle=nullptr;
		render_copy_f=nullptr;
		render_fill_rect=nullptr;
		get_render_draw_color=nullptr;
		set_render_draw_color=nullptr;
		get_ticks=nullptr;
		if(gps!=nullptr)++gps->force_full_display_count;
		}
	is_enabled=enable;
	out.print("smooth-movement: {}\n",enable?"enabled":"disabled");
	return CR_OK;
}

DFhackCExport command_result plugin_shutdown(color_ostream &out)
{
	return plugin_enable(out,false);
}
