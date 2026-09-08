// SPDX-License-Identifier: MIT

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

#include "sprite_flip_feature.h"

int main()
{
	sprite_flip_feature flip;
	assert(!flip.enabled());
	assert(!flip.should_mirror(visual_render_group::main,visual_facing::east));
	flip.set_enabled(true);
	assert(flip.should_mirror(visual_render_group::main,visual_facing::east));
	assert(!flip.should_mirror(visual_render_group::item,visual_facing::east));
	assert(!flip.should_mirror(visual_render_group::main,native_sprite_facing));
	assert(flip.mirror_shift(6,5)==-2);
	for(int32_t anchor=-5;anchor<=5;++anchor)
		for(int32_t offset=-3;offset<=3;++offset)
			{
			const int32_t piece=anchor+offset;
			const int32_t shift=flip.mirror_shift(piece,anchor);
			assert(shift==-2*offset);
			assert(piece+shift==mirrored_tile_x(piece,anchor));
			assert(flip.mirror_shift(piece+shift,anchor)==-shift);
			}
}
