// SPDX-License-Identifier: MIT

#ifndef SPRITE_FLIP_FEATURE_H
#define SPRITE_FLIP_FEATURE_H

#include "visual_animation.h"

class sprite_flip_feature
{
	bool enabled_=false;

	public:
		void reset() { enabled_=false; }
		void set_enabled(bool enable) { enabled_=enable; }
		bool enabled() const { return enabled_; }

		bool should_mirror(visual_render_group group,visual_facing facing) const;
		int32_t mirror_shift(int32_t piece_x,int32_t anchor_x) const;
};

#endif
