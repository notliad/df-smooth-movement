// SPDX-License-Identifier: MIT

#include "sprite_flip_feature.h"

bool sprite_flip_featurest::should_mirror(
	visual_render_groupst group,
	visual_facingst facing) const
{
	return enabled_&&
		(group==visual_render_groupst::main||group==visual_render_groupst::upper)&&
		facing!=native_sprite_facing;
}

int32_t sprite_flip_featurest::mirror_shift(int32_t piece_x,int32_t anchor_x) const
{
	return mirrored_tile_x(piece_x,anchor_x)-piece_x;
}
