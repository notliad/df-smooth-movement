// SPDX-License-Identifier: MIT

#ifndef FEATURE_CONTEXT_H
#define FEATURE_CONTEXT_H

#include "df/graphic_viewportst.h"
#include "df/renderer_2d_base.h"

#include <SDL_render.h>

#include <array>
#include <cstdint>

struct render_functionst
{
	decltype(&SDL_RenderCopyF) copy=nullptr;
	decltype(&SDL_RenderCopyExF) copy_ex=nullptr;
	decltype(&SDL_RenderFillRect) fill_rect=nullptr;
	decltype(&SDL_RenderSetClipRect) set_clip_rect=nullptr;
	decltype(&SDL_GetRenderDrawColor) get_draw_color=nullptr;
	decltype(&SDL_SetRenderDrawColor) set_draw_color=nullptr;

	bool valid() const
		{
		return copy!=nullptr&&copy_ex!=nullptr&&fill_rect!=nullptr&&
			set_clip_rect!=nullptr&&get_draw_color!=nullptr&&set_draw_color!=nullptr;
		}
};

struct movement_frame_contextst
{
	df::renderer_2d_base *renderer=nullptr;
	df::graphic_viewportst *main_viewport=nullptr;
	std::array<df::graphic_viewportst *,8> lower_viewports{};
	int32_t window_x=0;
	int32_t window_y=0;
	int32_t window_z=0;
	int32_t screen_dim_x=0;
	int32_t screen_dim_y=0;
	uint32_t now_ms=0;
	render_functionst render;
};

struct movement_prepare_resultst
{
	uint32_t frame_delta_ms=0;
	bool context_changed=false;
	bool main_viewport_readable=false;
};

#endif
