// SPDX-License-Identifier: MIT

#include "sprite_flip_feature.h"

bool sprite_flip_feature::should_mirror(
	visual_render_group group,
	visual_facing facing) const
{
	return enabled_&&
		(group==visual_render_group::main||group==visual_render_group::upper)&&
		facing!=native_sprite_facing;
}

int32_t sprite_flip_feature::mirror_shift(int32_t piece_x,int32_t anchor_x) const
{
	return 2*(anchor_x-piece_x);
}
