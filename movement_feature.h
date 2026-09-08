// SPDX-License-Identifier: MIT

#ifndef MOVEMENT_FEATURE_H
#define MOVEMENT_FEATURE_H

#include "camera_feature.h"
#include "feature_context.h"
#include "sprite_flip_feature.h"

namespace movement_feature
{
	void reset();
	movement_prepare_result prepare(const movement_frame_context &frame);
	void render(
		const movement_frame_context &frame,
		const camera_render_offset &camera,
		const sprite_flip_feature &sprite_flip);
}

#endif
