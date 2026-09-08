// SPDX-License-Identifier: MIT

#include "Core.h"
#include "MemAccess.h"
#include "PluginManager.h"
#include "VTableInterpose.h"

#include "modules/DFSDL.h"

#include "df/enabler.h"
#include "df/graphic.h"

#include "camera_feature.h"
#include "movement_feature.h"
#include "sprite_flip_feature.h"

#include <string>
#include <vector>

using namespace DFHack;

DFHACK_PLUGIN("stratum");
DFHACK_PLUGIN_IS_ENABLED(is_enabled);

REQUIRE_GLOBAL(enabler);
REQUIRE_GLOBAL(gps);
REQUIRE_GLOBAL(window_x);
REQUIRE_GLOBAL(window_y);
REQUIRE_GLOBAL(window_z);

namespace {

constexpr const char *plugin_version="0.3.0";

render_function render_functions;
camera_feature camera;
sprite_flip_feature sprite_flip;

movement_frame_context make_frame_context(df::renderer_2d_base *renderer)
{
	movement_frame_context frame;
	frame.renderer=renderer;
	frame.window_pos=df::coord(window_x?*window_x:0,window_y?*window_y:0,window_z?*window_z:0);
	frame.now_ms=Core::getInstance().p->getTickCount();
	frame.render=render_functions;
	if(gps!=nullptr)
		{
		frame.main_viewport=gps->main_viewport;
		frame.screen_dim_x=gps->dimx;
		frame.screen_dim_y=gps->dimy;
		for(size_t i=0;i<frame.lower_viewports.size();++i)
			frame.lower_viewports[i]=gps->lower_viewport[i];
		}
	return frame;
}

camera_frame_input make_camera_input(
	const movement_frame_context &frame,
	uint32_t delta_ms)
{
	camera_frame_input input;
	input.zoom_factor=frame.renderer->viewport_zoom_factor;
	input.window_x=window_x;
	input.window_y=window_y;
	input.middle_button=enabler!=nullptr&&gps!=nullptr&&enabler->mouse_mbut;
	input.delta_ms=delta_ms;
	if(gps!=nullptr)
		{
		input.mouse_x=gps->precise_mouse_x;
		input.mouse_y=gps->precise_mouse_y;
		}
	if(frame.main_viewport!=nullptr)
		{
		input.dim_x=frame.main_viewport->dim_x;
		input.dim_y=frame.main_viewport->dim_y;
		input.background=frame.main_viewport->screentexpos_background;
		input.previous_background=frame.main_viewport->screentexpos_background_old;
		}
	return input;
}

void render_stratum(df::renderer_2d_base *renderer)
{
	movement_frame_context frame=make_frame_context(renderer);
	const movement_prepare_result prepared=movement_feature::prepare(frame);
	if(prepared.context_changed)camera.cancel_transients();
	if(!prepared.main_viewport_readable||renderer->sdl_renderer==nullptr)return;

	camera.update(make_camera_input(frame,prepared.frame_delta_ms));
	const camera_render_offset offset=
		camera.render_offset(renderer->viewport_zoom_factor);
	if(offset.request_cleanup_redraw&&gps!=nullptr)++gps->force_full_display_count;
	movement_feature::render(frame,offset,sprite_flip);
}

struct renderer_hook : df::renderer_2d_base
{
	typedef df::renderer_2d_base interpose_base;
	DEFINE_VMETHOD_INTERPOSE(void,update_all,());
};

IMPLEMENT_VMETHOD_INTERPOSE(renderer_hook,update_all);

void renderer_hook::interpose_fn_update_all()
{
	render_stratum(this);
	INTERPOSE_NEXT(update_all)();
}

void clear_sdl_bindings()
{
	render_functions={};
}

bool load_sdl(color_ostream &out)
{
	clear_sdl_bindings();
	DFLibrary *sdl_handle=DFSDL::obtain_library_handle();
	#define bind(name,target) \
		target=reinterpret_cast<decltype(target)>(LookupPlugin(sdl_handle,#name)); \
		if(target==nullptr) { \
			out.printerr("stratum: SDL2 function unavailable: " #name "\n"); \
			clear_sdl_bindings(); \
			return false; \
		}
	bind(SDL_RenderCopyF,render_functions.copy);
	bind(SDL_RenderCopyExF,render_functions.copy_ex);
	bind(SDL_RenderFillRect,render_functions.fill_rect);
	bind(SDL_RenderSetClipRect,render_functions.set_clip_rect);
	bind(SDL_GetRenderDrawColor,render_functions.get_draw_color);
	bind(SDL_SetRenderDrawColor,render_functions.set_draw_color);
	#undef bind
	return true;
}

void reset_state()
{
	movement_feature::reset();
	camera.reset();
	sprite_flip.reset();
}

command_result status_command(
	color_ostream &out,
	std::vector<std::string> &parameters)
{
	if(parameters.empty())
		{
		out.print("stratum {}: {}\n",plugin_version,is_enabled?"enabled":"disabled");
		out.print(
			"free camera: {}, offset {:.3f} {:.3f} (tiles east/south of the grid)\n",
			camera.enabled()?"on":"off",
			camera.offset_x(),
			camera.offset_y());
		out.print("sprite flipping: {}\n",sprite_flip.enabled()?"on":"off");
		return CR_OK;
		}
	if(parameters[0]=="camera")
		{
		if(parameters.size()==1)
			{
			out.print(
				"free camera: {}, offset {:.3f} {:.3f}\n",
				camera.enabled()?"on":"off",
				camera.offset_x(),
				camera.offset_y());
			return CR_OK;
			}
		if(parameters.size()==2&&parameters[1]=="on")
			{
			camera.set_enabled(true);
			return CR_OK;
			}
		if(parameters.size()==2&&parameters[1]=="off")
			{
			camera.set_enabled(false);
			return CR_OK;
			}
		if(parameters.size()==2&&parameters[1]=="reset")
			{
			camera.reset_offset();
			return CR_OK;
			}
		if(parameters.size()==3)
			{
			try
				{
				const double x=std::stod(parameters[1]);
				const double y=std::stod(parameters[2]);
				if(x<-0.99||x>0.99||y<-0.99||y>0.99)
					{
					out.printerr("offsets must be within -0.99..0.99 tiles\n");
					return CR_FAILURE;
					}
				camera.set_offset(x,y,window_x,window_y);
				return CR_OK;
				}
			catch(...)
				{
				return CR_WRONG_USAGE;
				}
			}
		return CR_WRONG_USAGE;
		}
	if(parameters[0]=="flip")
		{
		if(parameters.size()==1)
			{
			out.print("sprite flipping: {}\n",sprite_flip.enabled()?"on":"off");
			return CR_OK;
			}
		if(parameters.size()==2&&
			(parameters[1]=="on"||parameters[1]=="off"))
			{
			const bool enable=parameters[1]=="on";
			sprite_flip.set_enabled(enable);
			if(gps!=nullptr)++gps->force_full_display_count;
			out.print(
				"stratum: sprite flipping {}\n",
				enable?"enabled":"disabled");
			return CR_OK;
			}
		return CR_WRONG_USAGE;
		}
	return CR_WRONG_USAGE;
}

} // namespace

DFhackCExport command_result
plugin_init(color_ostream &,std::vector<PluginCommand> &commands)
{
	commands.emplace_back(
		"stratum",
		"Smooth movement status; free camera: camera on|off|reset|<fx> <fy>; "
		"sprite flipping: flip on|off.",
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
			out.printerr("stratum: could not hook the 2D renderer\n");
			clear_sdl_bindings();
			return CR_FAILURE;
			}
		}
	else
		{
		INTERPOSE_HOOK(renderer_hook,update_all).remove();
		reset_state();
		clear_sdl_bindings();
		if(gps!=nullptr)++gps->force_full_display_count;
		}
	is_enabled=enable;
	out.print("stratum: {}\n",enable?"enabled":"disabled");
	return CR_OK;
}

DFhackCExport command_result plugin_shutdown(color_ostream &out)
{
	return plugin_enable(out,false);
}
