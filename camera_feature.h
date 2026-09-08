// SPDX-License-Identifier: MIT

#ifndef CAMERA_FEATURE_H
#define CAMERA_FEATURE_H

#include <cstdint>

struct camera_background_observation
{
	const void *renderer=nullptr;
	const void *viewport=nullptr;
	const void *current=nullptr;
	const void *previous=nullptr;
	int32_t dim_x=0;
	int32_t dim_y=0;
	uint32_t gputicks=0;
	bool readable=false;
	bool context_changed=false;
};

struct camera_background_result
{
	uint64_t generation=0;
	bool discontinuity=false;
};

class camera_background_observer
{
	camera_background_observation previous_;
	uint64_t generation_=0;
	bool observed_=false;
	bool readable_=false;

	public:
		camera_background_result observe(const camera_background_observation &input);
};

struct camera_frame_input
{
	int32_t zoom_factor=128;
	int32_t *window_x=nullptr;
	int32_t *window_y=nullptr;
	bool middle_button=false;
	int32_t mouse_x=0;
	int32_t mouse_y=0;
	int32_t dim_x=0;
	int32_t dim_y=0;
	const int32_t *background=nullptr;
	const int32_t *previous_background=nullptr;
	uint64_t background_generation=0;
	bool background_discontinuity=false;
	uint32_t delta_ms=0;
};

struct camera_render_offset
{
	int32_t x=0;
	int32_t y=0;
	bool request_cleanup_redraw=false;
};

class camera_feature
{
	static constexpr int32_t max_glide_tiles=3;
	static constexpr double tau_ms=35.0;

	bool enabled_=false;
	double transient_x_=0.0;
	double transient_y_=0.0;
	double rest_x_=0.0;
	double rest_y_=0.0;
	int32_t pending_dx_=0;
	int32_t pending_dy_=0;
	int32_t pending_frames_=0;
	int32_t self_scroll_x_=0;
	int32_t self_scroll_y_=0;
	bool drag_active_=false;
	double drag_anchor_vx_=0.0;
	double drag_anchor_vy_=0.0;
	int32_t drag_anchor_mx_=0;
	int32_t drag_anchor_my_=0;
	bool was_offset_=false;
	int32_t previous_window_x_=0;
	int32_t previous_window_y_=0;
	bool has_previous_window_=false;
	uint64_t background_generation_=0;
	bool background_consumed_=false;

	static double tile_pixels(int32_t zoom_factor);
	static double background_match_ratio(
		const camera_frame_input &input,
		int32_t dx,
		int32_t dy);
	void clear_pending();
	void normalize_rest(int32_t *window_x,int32_t *window_y);
	void attribute_landed(int32_t dx,int32_t dy,double tile);

	public:
		void reset();
		void cancel_transients();
		void set_enabled(bool enable);
		void set_offset(double east,double south,int32_t *window_x,int32_t *window_y);
		void reset_offset();
		void update(const camera_frame_input &input);
		camera_render_offset render_offset(int32_t zoom_factor);

		bool enabled() const { return enabled_; }
		double offset_x() const { return -rest_x_; }
		double offset_y() const { return -rest_y_; }
};

#endif
