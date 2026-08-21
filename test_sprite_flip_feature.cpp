// SPDX-License-Identifier: MIT

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

#include "sprite_flip_feature.h"

int main()
{
	sprite_flip_featurest flip;
	assert(!flip.enabled());
	assert(!flip.should_mirror(visual_render_groupst::main,visual_facingst::east));
	flip.set_enabled(true);
	assert(flip.should_mirror(visual_render_groupst::main,visual_facingst::east));
	assert(!flip.should_mirror(visual_render_groupst::item,visual_facingst::east));
	assert(!flip.should_mirror(visual_render_groupst::main,native_sprite_facing));
	assert(flip.mirror_shift(6,5)==-2);
}
