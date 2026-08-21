// SPDX-License-Identifier: MIT

#ifndef MOVEMENT_FEATURE_H
#define MOVEMENT_FEATURE_H

#include "camera_feature.h"
#include "feature_context.h"
#include "sprite_flip_feature.h"

namespace movement_feature
{
	void reset();
	movement_prepare_resultst prepare(const movement_frame_contextst &frame);
	void render(
		const movement_frame_contextst &frame,
		const camera_render_offsetst &camera,
		const sprite_flip_featurest &sprite_flip);
}

#endif
