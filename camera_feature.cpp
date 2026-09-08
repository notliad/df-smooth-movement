// SPDX-License-Identifier: MIT

#include "camera_feature.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

camera_background_result camera_background_observer::observe(
	const camera_background_observation &input)
{
	const bool readable=input.readable&&input.renderer&&input.viewport&&
		input.current&&input.previous&&input.dim_x>0&&input.dim_y>0;
	const bool same=input.current==previous_.current&&input.previous==previous_.previous;
	const bool reversed=input.current==previous_.previous&&input.previous==previous_.current;
	// The engine increments gputicks after display, not before the hook.
	const uint32_t elapsed=input.gputicks-previous_.gputicks;
	const bool discontinuity=!readable||(observed_&&
		(!readable_||elapsed>1||input.renderer!=previous_.renderer||
		input.viewport!=previous_.viewport||input.dim_x!=previous_.dim_x||
		input.dim_y!=previous_.dim_y||(!same&&!reversed)));
	// The first valid hook may already have a normalized offset pending.
	if(!discontinuity&&(!observed_||!same))++generation_;
	const camera_background_result result={
		generation_,discontinuity||(observed_&&input.context_changed)};
	previous_=input;
	observed_=true;
	readable_=readable;
	return result;
}

double camera_feature::tile_pixels(int32_t zoom_factor)
{
	return double(std::max(1,zoom_factor/4));
}

double camera_feature::background_match_ratio(
	const camera_frame_input &input,
	int32_t dx,
	int32_t dy)
{
	if(input.background==nullptr||input.previous_background==nullptr)return -1.0;
	int32_t considered=0;
	int32_t matches=0;
	for(int32_t x=0;x<input.dim_x;++x)
		{
		const int32_t source_x=x+dx;
		if(source_x<0||source_x>=input.dim_x)continue;
		for(int32_t y=0;y<input.dim_y;++y)
			{
			const int32_t source_y=y+dy;
			if(source_y<0||source_y>=input.dim_y)continue;
			const int32_t current=input.background[x*input.dim_y+y];
			if(current==0)continue;
			++considered;
			if(input.previous_background[source_x*input.dim_y+source_y]==current)
				++matches;
			}
		}
	return considered==0?-1.0:double(matches)/double(considered);
}

void camera_feature::clear_pending()
{
	pending_dx_=0;
	pending_dy_=0;
	pending_frames_=0;
	self_scroll_x_=0;
	self_scroll_y_=0;
}

void camera_feature::cancel_transients()
{
	transient_x_=0.0;
	transient_y_=0.0;
	clear_pending();
	drag_active_=false;
}

void camera_feature::reset()
{
	cancel_transients();
	enabled_=false;
	rest_x_=0.0;
	rest_y_=0.0;
	was_offset_=false;
	previous_window_x_=0;
	previous_window_y_=0;
	has_previous_window_=false;
	background_generation_=0;
	background_consumed_=false;
}

void camera_feature::set_enabled(bool enable)
{
	if(enabled_==enable)return;
	enabled_=enable;
	cancel_transients();
	rest_x_=0.0;
	rest_y_=0.0;
	has_previous_window_=false;
}

void camera_feature::normalize_rest(int32_t *window_x,int32_t *window_y)
{
	const int32_t dx=int32_t(-std::llround(rest_x_));
	const int32_t dy=int32_t(-std::llround(rest_y_));
	if(dx!=0&&window_x!=nullptr&&*window_x+dx>=0)
		{
		*window_x+=dx;
		self_scroll_x_+=dx;
		}
	if(dy!=0&&window_y!=nullptr&&*window_y+dy>=0)
		{
		*window_y+=dy;
		self_scroll_y_+=dy;
		}
}

void camera_feature::set_offset(
	double east,
	double south,
	int32_t *window_x,
	int32_t *window_y)
{
	set_enabled(true);
	if(!has_previous_window_)
		{
		previous_window_x_=window_x?*window_x:0;
		previous_window_y_=window_y?*window_y:0;
		has_previous_window_=true;
		}
	rest_x_=-east;
	rest_y_=-south;
	normalize_rest(window_x,window_y);
}

void camera_feature::reset_offset()
{
	rest_x_=0.0;
	rest_y_=0.0;
}

void camera_feature::attribute_landed(int32_t dx,int32_t dy,double tile)
{
	int32_t self_x=0;
	if(self_scroll_x_!=0&&(self_scroll_x_>0)==(dx>0)&&dx!=0)
		self_x=std::abs(self_scroll_x_)<=std::abs(dx)?self_scroll_x_:dx;
	int32_t self_y=0;
	if(self_scroll_y_!=0&&(self_scroll_y_>0)==(dy>0)&&dy!=0)
		self_y=std::abs(self_scroll_y_)<=std::abs(dy)?self_scroll_y_:dy;
	self_scroll_x_-=self_x;
	self_scroll_y_-=self_y;
	rest_x_+=self_x;
	rest_y_+=self_y;
	const int32_t glide_x=dx-self_x;
	const int32_t glide_y=dy-self_y;
	if(drag_active_)
		{
		rest_x_+=glide_x;
		rest_y_+=glide_y;
		}
	else
		{
		transient_x_+=glide_x*tile;
		transient_y_+=glide_y*tile;
		const double cap=tile*(max_glide_tiles+0.5);
		transient_x_=std::clamp(transient_x_,-cap,cap);
		transient_y_=std::clamp(transient_y_,-cap,cap);
		}
}

void camera_feature::update(const camera_frame_input &input)
{
	// Cancellation alone does not make a consumed landing fresh again.
	if(input.background_generation!=background_generation_)
		{
		background_generation_=input.background_generation;
		background_consumed_=false;
		}
	if(input.background_discontinuity)
		{
		cancel_transients();
		previous_window_x_=input.window_x?*input.window_x:0;
		previous_window_y_=input.window_y?*input.window_y:0;
		has_previous_window_=true;
		background_consumed_=true;
		return;
		}
	if(!enabled_)return;
	const double tile=tile_pixels(input.zoom_factor);
	const int32_t window_x=input.window_x?*input.window_x:0;
	const int32_t window_y=input.window_y?*input.window_y:0;
	if(has_previous_window_&&
		(window_x!=previous_window_x_||window_y!=previous_window_y_))
		{
		const int32_t dx=window_x-previous_window_x_;
		const int32_t dy=window_y-previous_window_y_;
		if((std::abs(dx)>max_glide_tiles||std::abs(dy)>max_glide_tiles)&&
			!drag_active_)
			cancel_transients();
		else
			{
			pending_dx_+=dx;
			pending_dy_+=dy;
			pending_frames_=0;
			}
		}
	previous_window_x_=window_x;
	previous_window_y_=window_y;
	has_previous_window_=true;

	if(pending_dx_!=0||pending_dy_!=0)
		{
		if(std::abs(pending_dx_)>6||std::abs(pending_dy_)>6)
			{
			transient_x_=0.0;
			transient_y_=0.0;
			clear_pending();
			}
		else if(!background_consumed_)
			{
			const int32_t step_x=(pending_dx_>0)-(pending_dx_<0);
			const int32_t step_y=(pending_dy_>0)-(pending_dy_<0);
			int32_t best_x=0;
			int32_t best_y=0;
			int32_t best_magnitude=-1;
			double best_score=-1.0;
			bool no_data=false;
			for(int32_t x=0;x<=std::abs(pending_dx_);++x)
				{
				for(int32_t y=0;y<=std::abs(pending_dy_);++y)
					{
					const double score=background_match_ratio(
						input,x*step_x,y*step_y);
					if(score<0.0)
						{
						no_data=true;
						break;
						}
					const int32_t magnitude=x+y;
					if(score>=0.6&&(magnitude>best_magnitude||
						(magnitude==best_magnitude&&score>best_score)))
						{
						best_magnitude=magnitude;
						best_score=score;
						best_x=x*step_x;
						best_y=y*step_y;
						}
					}
				if(no_data)break;
				}
			if(no_data)
				clear_pending();
			else if(best_magnitude>0)
				{
				background_consumed_=true;
				attribute_landed(best_x,best_y,tile);
				pending_dx_-=best_x;
				pending_dy_-=best_y;
				pending_frames_=0;
				}
			else if(best_magnitude==0)
				pending_frames_=0;
			else if(++pending_frames_>4)
				clear_pending();
			}
		}

	const double content_x=double(window_x-pending_dx_);
	const double content_y=double(window_y-pending_dy_);
	if(input.middle_button&&!drag_active_)
		{
		drag_active_=true;
		drag_anchor_vx_=content_x-rest_x_-transient_x_/tile;
		drag_anchor_vy_=content_y-rest_y_-transient_y_/tile;
		drag_anchor_mx_=input.mouse_x;
		drag_anchor_my_=input.mouse_y;
		transient_x_=0.0;
		transient_y_=0.0;
		}
	if(drag_active_)
		{
		if(!input.middle_button)
			{
			drag_active_=false;
			normalize_rest(input.window_x,input.window_y);
			}
		else
			{
			double visual_x=drag_anchor_vx_-
				double(input.mouse_x-drag_anchor_mx_)/tile;
			double visual_y=drag_anchor_vy_-
				double(input.mouse_y-drag_anchor_my_)/tile;
			rest_x_=content_x-visual_x;
			rest_y_=content_y-visual_y;
			constexpr double limit=1.5;
			if(rest_x_<-limit||rest_x_>limit||rest_y_<-limit||rest_y_>limit)
				{
				rest_x_=std::clamp(rest_x_,-limit,limit);
				rest_y_=std::clamp(rest_y_,-limit,limit);
				drag_anchor_vx_=content_x-rest_x_+
					double(input.mouse_x-drag_anchor_mx_)/tile;
				drag_anchor_vy_=content_y-rest_y_+
					double(input.mouse_y-drag_anchor_my_)/tile;
				}
			}
		}

	if(transient_x_!=0.0||transient_y_!=0.0)
		{
		const double decay=std::exp(-double(input.delta_ms)/tau_ms);
		transient_x_*=decay;
		transient_y_*=decay;
		if(std::abs(transient_x_)<0.5&&std::abs(transient_y_)<0.5)
			{
			transient_x_=0.0;
			transient_y_=0.0;
			}
		}
}

camera_render_offset camera_feature::render_offset(int32_t zoom_factor)
{
	const double tile=tile_pixels(zoom_factor);
	camera_render_offset offset;
	offset.x=int32_t(std::lround(transient_x_+rest_x_*tile));
	offset.y=int32_t(std::lround(transient_y_+rest_y_*tile));
	const bool active=offset.x!=0||offset.y!=0;
	offset.request_cleanup_redraw=!active&&was_offset_;
	was_offset_=active;
	return offset;
}
