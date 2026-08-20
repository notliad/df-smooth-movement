// SPDX-License-Identifier: MIT

#include <array>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdint>
#include <limits>

#include "visual_animation.h"

namespace {

viewport_visual_animation_inputst make_input(
	const void *viewport,
	int32_t dimension,
	const int32_t *empty)
{
	viewport_visual_animation_inputst input;
	input.viewport=viewport;
	input.dim_x=dimension;
	input.dim_y=dimension;
	input.context_revision=1;
	input.current.fill(empty);
	input.previous.fill(empty);
	return input;
}

void set_layer(
	viewport_visual_animation_inputst &input,
	viewport_visual_layer layer,
	const int32_t *current,
	const int32_t *previous)
{
	const size_t index=static_cast<size_t>(layer);
	input.current[index]=current;
	input.previous[index]=previous;
}

template<size_t TileCount>
void fill_background(std::array<int32_t,TileCount> &background,int32_t seed)
{
	for(size_t i=0;i<TileCount;++i)background[i]=seed+int32_t(i);
}

template<size_t TileCount>
void shift_background(
	std::array<int32_t,TileCount> &current,
	const std::array<int32_t,TileCount> &previous,
	int32_t dimension,
	int32_t dx,
	int32_t dy,
	int32_t exposed_seed)
{
	for(int32_t x=0;x<dimension;++x)
		{
		for(int32_t y=0;y<dimension;++y)
			{
			const int32_t sx=x+dx;
			const int32_t sy=y+dy;
			const int32_t index=x*dimension+y;
			current[size_t(index)]=sx>=0&&sx<dimension&&sy>=0&&sy<dimension?
				previous[size_t(sx*dimension+sy)]:
				exposed_seed+index;
			}
		}
}

void run_frame(
	visual_animation_managerst &manager,
	const viewport_visual_animation_inputst &input,
	uint32_t now_ms,
	uint32_t movement_duration_ms=default_movement_duration_ms)
{
	manager.begin_frame(now_ms,movement_duration_ms);
	manager.synchronize_viewport(input);
	manager.end_frame();
}

} // namespace

int main()
{
	// Camera command offsets must be safe to round before they enter render state.
	assert(valid_camera_offset(0.0,0.0));
	assert(valid_camera_offset(-max_camera_offset_tiles,max_camera_offset_tiles));
	assert(!valid_camera_offset(
		std::numeric_limits<double>::quiet_NaN(),0.0));
	assert(!valid_camera_offset(
		std::numeric_limits<double>::infinity(),0.0));
	assert(!valid_camera_offset(
		-std::numeric_limits<double>::infinity(),0.0));
	assert(!valid_camera_offset(max_camera_offset_tiles+0.001,0.0));

	// A fast drag keeps the rendered position continuous when its persistent part is bounded.
	constexpr double tile_size=32.0;
	for(double requested:{1.25,1.5,1.75,-1.25,-1.5,-1.75})
		{
		const auto constrained=constrain_camera_drag_axis(requested,0.0,tile_size);
		assert(std::abs(
			constrained.rest_tiles*tile_size+constrained.correction_px-
			requested*tile_size)<0.000001);
		assert(constrained.rest_tiles>=-camera_drag_rest_limit_tiles&&
			constrained.rest_tiles<=camera_drag_rest_limit_tiles);
		}
	const auto diagonal_x=constrain_camera_drag_axis(2.0,8.0,tile_size);
	const auto diagonal_y=constrain_camera_drag_axis(-2.25,-4.0,tile_size);
	assert(diagonal_x.rest_tiles==camera_drag_rest_limit_tiles);
	assert(diagonal_y.rest_tiles==-camera_drag_rest_limit_tiles);
	assert(diagonal_x.correction_px==24.0);
	assert(diagonal_y.correction_px==-28.0);

	// Releasing persists the complete rendered offset without starting another animation.
	const auto persisted=persist_camera_drag_axis(0.75,12.0,tile_size);
	assert(persisted.rest_tiles==1.125);
	assert(persisted.correction_px==0.0);
	assert(persisted.rest_tiles*tile_size==0.75*tile_size+12.0);

	// The drag correction has a 120 ms time constant while the mouse is held.
	const double after_one_tau=decay_camera_drag_correction(32.0,120);
	assert(std::abs(after_one_tau-32.0/std::exp(1.0))<0.000001);
	const double after_three_tau=decay_camera_drag_correction(32.0,360);
	assert(std::abs(after_three_tau-32.0/std::exp(3.0))<0.000001);
	assert(after_three_tau<32.0*0.05);

	// Starting a drag absorbs every rendered camera layer, including a follow anchor, into the
	// baseline. Clearing the anchor afterward therefore cannot move the view.
	const double drag_anchor=camera_drag_anchor(10.0,0.25,8.0,-2.0,16.0,tile_size);
	assert(std::abs(drag_anchor-9.0625)<0.000001);
	assert(std::abs(
		(10.0-drag_anchor)*tile_size-(0.25*tile_size+8.0-2.0+16.0))<0.000001);

	// Normalization scrolls are consumed before a landing can become a gameplay follow anchor.
	const auto self_only=attribute_self_scroll_axis(1,1);
	assert(self_only.self==1&&self_only.gameplay==0);
	const auto mixed=attribute_self_scroll_axis(1,2);
	assert(mixed.self==1&&mixed.gameplay==1);
	const auto opposite=attribute_self_scroll_axis(-1,1);
	assert(opposite.self==0&&opposite.gameplay==1);

	// Positive dimensions are not enough: all signed tile indices also need a safe product.
	{
		int32_t one_tile[1]={};
		const int viewport_token=0;
		auto input=make_input(&viewport_token,1,one_tile);
		assert(input.valid());
		input.dim_x=std::numeric_limits<int32_t>::max();
		input.dim_y=2;
		assert(!input.valid());
		input.dim_x=1;
		input.dim_y=1;
		input.current[0]=nullptr;
		assert(!input.valid());
	}

	visual_animation_managerst manager;
	manager.begin_frame(1000);
	assert(manager.get_frame_time_ms()==1000);
	assert(manager.get_frame_delta_ms()==0);

	manager.begin_frame(1020);
	assert(manager.get_frame_time_ms()==1020);
	assert(manager.get_frame_delta_ms()==20);

	manager.begin_frame(1020);
	assert(manager.get_frame_delta_ms()==0);

	visual_animation_managerst rollover;
	rollover.begin_frame(std::numeric_limits<uint32_t>::max()-5);
	rollover.begin_frame(3);
	assert(rollover.get_frame_delta_ms()==9);
	// Facing rule: only a horizontal component changes facing.
	assert(facing_after_move(1,visual_facingst::west)==visual_facingst::east);
	assert(facing_after_move(-1,visual_facingst::east)==visual_facingst::west);
	// dy is not an input, so a diagonal is only ever the sign of dx.
	// The four real diagonals run end to end through the manager further down.
	assert(facing_after_move(1,visual_facingst::east)==visual_facingst::east);
	assert(facing_after_move(-1,visual_facingst::west)==visual_facingst::west);
	// Pure vertical and idle carry the previous facing (sticky).
	assert(facing_after_move(0,visual_facingst::west)==visual_facingst::west);
	assert(facing_after_move(0,visual_facingst::east)==visual_facingst::east);

	assert(mirrored_tile_x(5,5)==5);   // anchor reflects to itself
	assert(mirrored_tile_x(6,5)==4);   // right spill -> left
	assert(mirrored_tile_x(4,5)==6);   // left spill -> right
	// The formula is a general reflection, so it holds for offsets no layer can express.
	assert(mirrored_tile_x(8,5)==2);
	// center_x is only ever -1, 0 or +1, so the real mirror shift is only ever -2, 0 or +2.
	for(const auto &descriptor:visual_layer_descriptors)
		assert(descriptor.center_x>=-1&&descriptor.center_x<=1);
	// Reflection is self-inverse.
	assert(mirrored_tile_x(mirrored_tile_x(6,5),5)==6);
	assert(mirrored_tile_x(mirrored_tile_x(8,5),5)==8);

	{
	constexpr int32_t dim=4;
	int32_t empty[dim*dim]={};
	int32_t before[dim*dim]={};
	int32_t west_after[dim*dim]={};
	const int viewport_token=0;
	const void *viewport=&viewport_token;

	before[2*dim+2]=77;
	west_after[1*dim+2]=77;   // moved west: x 2 -> 1

	// Moving west sets west facing on the target tile.
	{
	visual_animation_managerst manager;
	auto input=make_input(viewport,dim,empty);
	set_layer(input,viewport_visual_layer::center,before,empty);
	run_frame(manager,input,1000);
	set_layer(input,viewport_visual_layer::center,west_after,before);
	run_frame(manager,input,1016);
	assert(manager.get_facing(viewport,1,2)==visual_facingst::west);
	}

	// Moving east sets east facing.
	// East is neither the grid default nor the source facing, so the assertion is not vacuous.
	{
	visual_animation_managerst manager;
	auto input=make_input(viewport,dim,empty);
	set_layer(input,viewport_visual_layer::center,before,empty);
	run_frame(manager,input,1000);
	set_layer(input,viewport_visual_layer::center,west_after,before);
	run_frame(manager,input,1016);
	assert(manager.get_facing(viewport,1,2)==visual_facingst::west);
	set_layer(input,viewport_visual_layer::center,before,west_after);
	run_frame(manager,input,1032);
	assert(manager.get_facing(viewport,2,2)==visual_facingst::east);
	}

	// The mirrored flag gates the render path's early return.
	// It must rise only for a genuinely mirrored creature and fall when that tile empties.
	{
	visual_animation_managerst manager;
	auto input=make_input(viewport,dim,empty);
	set_layer(input,viewport_visual_layer::center,before,empty);
	run_frame(manager,input,1000);
	assert(!manager.has_mirrored_facing(viewport));
	// Moving west matches the art, so nothing is mirrored yet.
	set_layer(input,viewport_visual_layer::center,west_after,before);
	run_frame(manager,input,1016);
	assert(manager.get_facing(viewport,1,2)==visual_facingst::west);
	assert(!manager.has_mirrored_facing(viewport));
	// Moving east faces away from the art and raises the flag.
	set_layer(input,viewport_visual_layer::center,before,west_after);
	run_frame(manager,input,1032);
	assert(manager.get_facing(viewport,2,2)==visual_facingst::east);
	assert(manager.has_mirrored_facing(viewport));
	// The creature leaves: the tile clears and so does the flag.
	set_layer(input,viewport_visual_layer::center,empty,before);
	run_frame(manager,input,1048);
	assert(manager.get_facing(viewport,2,2)==native_sprite_facing);
	assert(!manager.has_mirrored_facing(viewport));
	assert(!manager.has_mirrored_facing(nullptr));
	}

	// Pure vertical movement carries the existing facing to the new tile.
	{
	visual_animation_managerst manager;
	auto input=make_input(viewport,dim,empty);
	set_layer(input,viewport_visual_layer::center,before,empty);
	run_frame(manager,input,1000);
	set_layer(input,viewport_visual_layer::center,west_after,before);
	run_frame(manager,input,1016);
	assert(manager.get_facing(viewport,1,2)==visual_facingst::west);
	int32_t up[dim*dim]={};
	up[1*dim+1]=77;   // north: (1,2) -> (1,1), no horizontal component
	set_layer(input,viewport_visual_layer::center,up,west_after);
	run_frame(manager,input,1032);
	assert(manager.get_facing(viewport,1,1)==visual_facingst::west);
	}

	// Out-of-range and unknown viewports fall back to the native facing.
	{
	visual_animation_managerst manager;
	auto input=make_input(viewport,dim,empty);
	set_layer(input,viewport_visual_layer::center,before,empty);
	run_frame(manager,input,1000);
	assert(manager.get_facing(viewport,-1,0)==native_sprite_facing);
	assert(manager.get_facing(viewport,dim,0)==native_sprite_facing);
	assert(manager.get_facing(nullptr,0,0)==native_sprite_facing);
	}

	// Each rendered z-level has its own viewport buffers; tracking one must not suppress another.
	{
	const int lower_token=0;
	const int main_token=0;
	const void *lower_viewport=&lower_token;
	const void *main_viewport=&main_token;
	visual_animation_managerst z_levels;
	auto lower_input=make_input(lower_viewport,dim,empty);
	auto main_input=make_input(main_viewport,dim,empty);
	set_layer(lower_input,viewport_visual_layer::center,before,empty);
	set_layer(main_input,viewport_visual_layer::center,before,empty);
	z_levels.begin_frame(1000);
	z_levels.synchronize_viewport(lower_input);
	z_levels.synchronize_viewport(main_input);
	z_levels.end_frame();
	set_layer(lower_input,viewport_visual_layer::center,west_after,before);
	set_layer(main_input,viewport_visual_layer::center,west_after,before);
	z_levels.begin_frame(1016);
	z_levels.synchronize_viewport(lower_input);
	z_levels.synchronize_viewport(main_input);
	z_levels.end_frame();
	assert(z_levels.get_movement(
		lower_viewport,viewport_visual_layer::center,1,2).active);
	assert(z_levels.get_movement(
		main_viewport,viewport_visual_layer::center,1,2).active);
	}
	}

	// All four diagonals through the manager, so every step carries a real dy as well as a dx.
	// The chain alternates direction, so no assertion can pass by inheriting the previous facing.
	{
	constexpr int32_t diag_dim=5;
	int32_t diag_empty[diag_dim*diag_dim]={};
	int32_t start[diag_dim*diag_dim]={};
	int32_t north_east[diag_dim*diag_dim]={};
	int32_t south_west[diag_dim*diag_dim]={};
	int32_t south_east[diag_dim*diag_dim]={};
	int32_t north_west[diag_dim*diag_dim]={};
	const int diag_token=0;
	const void *diag_viewport=&diag_token;

	start[2*diag_dim+2]=77;        // (2,2)
	north_east[3*diag_dim+1]=77;   // (2,2) -> (3,1): dx +1, dy -1
	south_west[2*diag_dim+2]=77;   // (3,1) -> (2,2): dx -1, dy +1
	south_east[3*diag_dim+3]=77;   // (2,2) -> (3,3): dx +1, dy +1
	north_west[2*diag_dim+2]=77;   // (3,3) -> (2,2): dx -1, dy -1

	visual_animation_managerst diagonal;
	auto input=make_input(diag_viewport,diag_dim,diag_empty);
	set_layer(input,viewport_visual_layer::center,start,diag_empty);
	run_frame(diagonal,input,1000);
	assert(diagonal.get_facing(diag_viewport,2,2)==native_sprite_facing);

	set_layer(input,viewport_visual_layer::center,north_east,start);
	run_frame(diagonal,input,1016);
	assert(diagonal.get_facing(diag_viewport,3,1)==visual_facingst::east);

	// West is the grid default, so the westward legs also assert a movement was registered.
	// Otherwise an untracked step leaving the tile at its default would pass.
	set_layer(input,viewport_visual_layer::center,south_west,north_east);
	run_frame(diagonal,input,1032);
	assert(diagonal.get_movement(
		diag_viewport,viewport_visual_layer::center,2,2).active);
	assert(diagonal.get_facing(diag_viewport,2,2)==visual_facingst::west);

	set_layer(input,viewport_visual_layer::center,south_east,south_west);
	run_frame(diagonal,input,1048);
	assert(diagonal.get_facing(diag_viewport,3,3)==visual_facingst::east);

	set_layer(input,viewport_visual_layer::center,north_west,south_east);
	run_frame(diagonal,input,1064);
	assert(diagonal.get_movement(
		diag_viewport,viewport_visual_layer::center,2,2).active);
	assert(diagonal.get_facing(diag_viewport,2,2)==visual_facingst::west);
	}

	// Facing is keyed by SCREEN tile, so a scroll moves the creatures out from under it.
	// The two outcomes differ fundamentally: a landed shift has a known delta and is translated.
	// An abandoned shift never identifies a delta, so the grid can only be dropped.
	{
	constexpr int32_t pan_dim=4;
	int32_t pan_empty[pan_dim*pan_dim]={};
	int32_t at_one[pan_dim*pan_dim]={};
	int32_t at_two[pan_dim*pan_dim]={};
	int32_t unmatched_a[pan_dim*pan_dim]={};
	int32_t unmatched_b[pan_dim*pan_dim]={};
	const int pan_token=0;
	const void *pan_viewport=&pan_token;

	at_one[1*pan_dim+1]=77;
	at_two[2*pan_dim+1]=77;   // steps east, x 1 -> 2, so it faces east
	unmatched_a[2*pan_dim+1]=78;
	unmatched_b[2*pan_dim+1]=79;

	// LANDED: the shift is recognized, so facing follows the buffers.
	{
	visual_animation_managerst landed;
	auto input=make_input(pan_viewport,pan_dim,pan_empty);
	set_layer(input,viewport_visual_layer::center,at_one,pan_empty);
	run_frame(landed,input,1000);
	set_layer(input,viewport_visual_layer::center,at_two,at_one);
	run_frame(landed,input,1016);
	assert(landed.get_facing(pan_viewport,2,1)==visual_facingst::east);
	assert(landed.has_mirrored_facing(pan_viewport));

	// Frame A: the scroll is announced but the buffers have not moved yet.
	input.pan_x=1;
	set_layer(input,viewport_visual_layer::center,at_two,at_two);
	run_frame(landed,input,1032);
	assert(landed.get_facing(pan_viewport,2,1)==visual_facingst::east);

	// Frame B: the buffers shift east by one and the majority-match test recognizes it.
	set_layer(input,viewport_visual_layer::center,at_one,at_two);
	run_frame(landed,input,1048);
	assert(landed.get_facing(pan_viewport,1,1)==visual_facingst::east);
	assert(landed.get_facing(pan_viewport,2,1)==native_sprite_facing);
	assert(landed.has_mirrored_facing(pan_viewport));
	}

	// ABANDONED: the shift never shows up in the buffers, so no delta is ever identified.
	// The grid must be back at the default while the tile is still OCCUPIED.
	// The empty-tile sweep cannot reach that case, so only an explicit reset clears it.
	{
	visual_animation_managerst abandoned;
	auto input=make_input(pan_viewport,pan_dim,pan_empty);
	set_layer(input,viewport_visual_layer::center,at_one,pan_empty);
	run_frame(abandoned,input,2000);
	set_layer(input,viewport_visual_layer::center,at_two,at_one);
	run_frame(abandoned,input,2016);
	assert(abandoned.get_facing(pan_viewport,2,1)==visual_facingst::east);

	// Changed buffers keep the failed majority-match test running every frame.
	// It tolerates four before giving up on the fifth.
	input.pan_x=1;
	for(int32_t frame=0;frame<4;++frame)
		{
		set_layer(input,viewport_visual_layer::center,at_two,
			frame%2==0?unmatched_a:unmatched_b);
		run_frame(abandoned,input,2032+uint32_t(frame)*16);
		// Still pending, so the facing survives.
		// The assertion after the giving-up frame therefore tests the reset, not an empty grid.
		assert(abandoned.get_facing(pan_viewport,2,1)==visual_facingst::east);
		assert(abandoned.has_mirrored_facing(pan_viewport));
		}
	set_layer(input,viewport_visual_layer::center,at_two,unmatched_a);
	run_frame(abandoned,input,2096);
	assert(abandoned.get_facing(pan_viewport,2,1)==native_sprite_facing);
	assert(!abandoned.has_mirrored_facing(pan_viewport));
	}
	}

	// Regression: A and B move in the same frame, chained -- A's target tile is B's source tile.
	// A's write must not corrupt B's read of its own pre-frame facing.
	// A is sent east first, so its facing differs from the default B carries and a swap shows.
	{
	constexpr int32_t chase_dim=5;
	int32_t chase_empty[chase_dim*chase_dim]={};
	int32_t frame_a[chase_dim*chase_dim]={};
	int32_t frame_b[chase_dim*chase_dim]={};
	int32_t frame_c[chase_dim*chase_dim]={};
	const int chase_token=0;
	const void *chase_viewport=&chase_token;

	frame_a[1*chase_dim+1]=77;   // A at (1,1)
	frame_a[2*chase_dim+2]=88;   // B at (2,2), stationary, keeps the default facing
	frame_b[2*chase_dim+1]=77;   // A steps east: (1,1) -> (2,1), picks up an east facing
	frame_b[2*chase_dim+2]=88;   // B unchanged

	// Both step south in the same frame: shared_movement_delta needs two tiles on the same delta.
	frame_c[2*chase_dim+2]=77;   // A: (2,1) -> (2,2)
	frame_c[2*chase_dim+3]=88;   // B: (2,2) -> (2,3)

	visual_animation_managerst manager;
	auto input=make_input(chase_viewport,chase_dim,chase_empty);
	set_layer(input,viewport_visual_layer::center,frame_a,chase_empty);
	run_frame(manager,input,1000);
	set_layer(input,viewport_visual_layer::center,frame_b,frame_a);
	run_frame(manager,input,1016);
	assert(manager.get_facing(chase_viewport,2,1)==visual_facingst::east);
	assert(manager.get_facing(chase_viewport,2,2)==native_sprite_facing);

	set_layer(input,viewport_visual_layer::center,frame_c,frame_b);
	run_frame(manager,input,1032);
	assert(manager.get_facing(chase_viewport,2,2)==visual_facingst::east);
	// B carries its own default forward, not A's, though A wrote (2,2) earlier in the same pass.
	assert(manager.get_facing(chase_viewport,2,3)==native_sprite_facing);
	}

	// Regression: a vacated source tile is reoccupied the same frame by an UNTRACKED creature.
	// Nothing targets that tile, so no target write clears it, and it is not empty either.
	// Only an explicit, order-independent source clear restores the default there.
	{
	constexpr int32_t gap_dim=5;
	int32_t gap_empty[gap_dim*gap_dim]={};
	int32_t frame_a[gap_dim*gap_dim]={};
	int32_t frame_b[gap_dim*gap_dim]={};
	int32_t frame_c[gap_dim*gap_dim]={};
	const int gap_token=0;
	const void *gap_viewport=&gap_token;

	// E is a companion so that D's later departure shares a delta with E's own move.
	// shared_movement_delta engages only once two tiles move on the same delta.
	frame_a[1*gap_dim+1]=77;   // D at (1,1)
	frame_a[2*gap_dim+2]=88;   // E at (2,2), stationary companion
	frame_b[2*gap_dim+1]=77;   // D steps east: (1,1) -> (2,1), facing differs from the default
	frame_b[2*gap_dim+2]=88;   // E unchanged

	// D and E both step south, chained, and F appears at D's just-vacated tile the same frame.
	// previous[(2,1)] was occupied by D, so the empty-cell fallback cannot see F.
	// No shared-delta source matches F's texpos either, so its arrival registers no movement.
	frame_c[2*gap_dim+2]=77;   // D: (2,1) -> (2,2)
	frame_c[2*gap_dim+3]=88;   // E: (2,2) -> (2,3)
	frame_c[2*gap_dim+1]=55;   // F appears at (2,1), untracked

	visual_animation_managerst manager;
	auto input=make_input(gap_viewport,gap_dim,gap_empty);
	set_layer(input,viewport_visual_layer::center,frame_a,gap_empty);
	run_frame(manager,input,1000);
	set_layer(input,viewport_visual_layer::center,frame_b,frame_a);
	run_frame(manager,input,1016);
	assert(manager.get_facing(gap_viewport,2,1)==visual_facingst::east);

	set_layer(input,viewport_visual_layer::center,frame_c,frame_b);
	run_frame(manager,input,1032);
	assert(!manager.get_movement(
		gap_viewport,viewport_visual_layer::center,2,1).active);
	// F must not inherit D's stale east facing, though the tile is occupied rather than empty.
	assert(manager.get_facing(gap_viewport,2,1)==native_sprite_facing);
	assert(manager.get_facing(gap_viewport,2,2)==visual_facingst::east);
	assert(manager.get_facing(gap_viewport,2,3)==native_sprite_facing);
	}

	assert(animation_progress(25,0,100)==0.25f);
	assert(animation_progress(75,0,100)==0.75f);
	assert(animation_progress(100,0,100)==1.0f);
	assert(movement_duration_for_fps(50.0f)==200);
	assert(movement_duration_for_fps(100.0f)==100);
	assert(movement_duration_for_fps(200.0f)==50);
	assert(movement_duration_for_fps(0.0f)==default_movement_duration_ms);
	assert(movement_duration_for_fps(-1.0f)==default_movement_duration_ms);
	assert(movement_duration_for_fps(
		std::numeric_limits<float>::infinity())==default_movement_duration_ms);
	assert(movement_duration_for_fps(20000.0f)==1);
	assert(valid_movement_cadence(1,100));
	assert(valid_movement_cadence(400,100));
	assert(!valid_movement_cadence(0,100));
	assert(!valid_movement_cadence(401,100));
	assert(inherited_visual_source_tile(0,0,1)==-1);
	assert(inherited_visual_source_tile(2,0,1)==1);
	assert(visual_layer_descriptor(viewport_visual_layer::right).center_x==-1);
	assert(visual_layer_descriptor(viewport_visual_layer::left).center_x==1);
	assert(visual_layer_descriptor(viewport_visual_layer::upright).center_x==-1&&
		visual_layer_descriptor(viewport_visual_layer::upright).center_y==1);
	assert(visual_layer_descriptor(viewport_visual_layer::up).center_x==0&&
		visual_layer_descriptor(viewport_visual_layer::up).center_y==1);
	assert(visual_layer_descriptor(viewport_visual_layer::upleft).center_x==1&&
		visual_layer_descriptor(viewport_visual_layer::upleft).center_y==1);

	std::array<int32_t,9> empty{};
	std::array<int32_t,9> current{};
	std::array<int32_t,9> previous{};
	const void *viewport=reinterpret_cast<const void *>(uintptr_t(1));
	auto input=make_input(viewport,3,empty.data());
	set_layer(input,viewport_visual_layer::center,current.data(),previous.data());

	visual_animation_managerst movement;
	run_frame(movement,input,1990);
	assert(!movement.requires_full_redraw());

	previous[0*3+1]=42;
	current[1*3+1]=42;
	run_frame(movement,input,2000);
	auto render=movement.get_movement(viewport,viewport_visual_layer::center,1,1);
	assert(render.active);
	assert(render.source_x==0&&render.source_y==1);
	assert(render.progress==0.0f);
	assert(movement.requires_full_redraw());

	previous=current;
	set_layer(input,viewport_visual_layer::center,current.data(),previous.data());
	run_frame(movement,input,2050);
	render=movement.get_movement(viewport,viewport_visual_layer::center,1,1);
	assert(render.active);
	assert(render.progress==0.5f);

	run_frame(movement,input,2100);
	assert(!movement.get_movement(
		viewport,viewport_visual_layer::center,1,1).active);
	assert(movement.requires_full_redraw());

	run_frame(movement,input,2120);
	assert(!movement.requires_full_redraw());

	// A movement keeps the duration captured when it starts, while an FPS change clears cadence.
	visual_animation_managerst scaled;
	current.fill(0);
	previous.fill(0);
	set_layer(input,viewport_visual_layer::center,current.data(),previous.data());
	run_frame(scaled,input,3990,200);
	previous[0*3+1]=42;
	current[1*3+1]=42;
	run_frame(scaled,input,4000,200);
	previous=current;
	set_layer(input,viewport_visual_layer::center,current.data(),previous.data());
	run_frame(scaled,input,4100,100);
	render=scaled.get_movement(viewport,viewport_visual_layer::center,1,1);
	assert(render.active&&render.progress==0.5f);
	previous=current;
	current.fill(0);
	current[2*3+1]=42;
	run_frame(scaled,input,4150,100);
	previous=current;
	set_layer(input,viewport_visual_layer::center,current.data(),previous.data());
	run_frame(scaled,input,4200,100);
	render=scaled.get_movement(viewport,viewport_visual_layer::center,2,1);
	assert(render.active&&render.progress==0.5f);
	run_frame(scaled,input,4250,100);
	assert(!scaled.get_movement(
		viewport,viewport_visual_layer::center,2,1).active);

	// Cadence survives animation expiry. The second step uses its 150ms observed interval;
	// after more than four fallback durations idle, the following step restarts at 100ms.
	{
	constexpr int32_t cadence_dim=4;
	int32_t cadence_empty[cadence_dim*cadence_dim]={};
	int32_t at_zero[cadence_dim*cadence_dim]={};
	int32_t at_one[cadence_dim*cadence_dim]={};
	int32_t at_two[cadence_dim*cadence_dim]={};
	at_zero[0*cadence_dim+1]=42;
	at_one[1*cadence_dim+1]=42;
	at_two[2*cadence_dim+1]=42;
	const int cadence_token=0;
	const void *cadence_viewport=&cadence_token;
	visual_animation_managerst cadence;
	auto cadence_input=make_input(cadence_viewport,cadence_dim,cadence_empty);
	set_layer(cadence_input,viewport_visual_layer::center,cadence_empty,cadence_empty);
	run_frame(cadence,cadence_input,4980);
	set_layer(cadence_input,viewport_visual_layer::center,at_zero,cadence_empty);
	run_frame(cadence,cadence_input,4990);
	assert(!cadence.get_movement(
		cadence_viewport,viewport_visual_layer::center,0,1).active);
	set_layer(cadence_input,viewport_visual_layer::center,at_one,at_zero);
	run_frame(cadence,cadence_input,5000);
	set_layer(cadence_input,viewport_visual_layer::center,at_one,at_one);
	run_frame(cadence,cadence_input,5050);
	render=cadence.get_movement(
		cadence_viewport,viewport_visual_layer::center,1,1);
	assert(render.active&&render.progress==0.5f);
	set_layer(cadence_input,viewport_visual_layer::center,at_one,at_one);
	run_frame(cadence,cadence_input,5100);
	assert(!cadence.get_movement(
		cadence_viewport,viewport_visual_layer::center,1,1).active);
	set_layer(cadence_input,viewport_visual_layer::center,at_two,at_one);
	run_frame(cadence,cadence_input,5150);
	set_layer(cadence_input,viewport_visual_layer::center,at_two,at_two);
	run_frame(cadence,cadence_input,5225);
	render=cadence.get_movement(
		cadence_viewport,viewport_visual_layer::center,2,1);
	assert(render.active&&render.progress==0.5f);
	set_layer(cadence_input,viewport_visual_layer::center,at_one,at_two);
	run_frame(cadence,cadence_input,5600);
	set_layer(cadence_input,viewport_visual_layer::center,at_one,at_one);
	run_frame(cadence,cadence_input,5650);
	render=cadence.get_movement(
		cadence_viewport,viewport_visual_layer::center,1,1);
	assert(render.active&&render.progress==0.5f);
	}

	// Consecutive diagonal steps use their own observed 150, 200 and 250ms intervals.
	{
	constexpr int32_t cadence_dim=5;
	int32_t cadence_empty[cadence_dim*cadence_dim]={};
	int32_t center[cadence_dim*cadence_dim]={};
	int32_t north_east[cadence_dim*cadence_dim]={};
	int32_t south_east[cadence_dim*cadence_dim]={};
	center[2*cadence_dim+2]=42;
	north_east[3*cadence_dim+1]=42;
	south_east[3*cadence_dim+3]=42;
	const int cadence_token=0;
	const void *cadence_viewport=&cadence_token;
	visual_animation_managerst cadence;
	auto cadence_input=make_input(cadence_viewport,cadence_dim,cadence_empty);
	set_layer(cadence_input,viewport_visual_layer::center,center,cadence_empty);
	run_frame(cadence,cadence_input,9990);
	set_layer(cadence_input,viewport_visual_layer::center,north_east,center);
	run_frame(cadence,cadence_input,10000);
	set_layer(cadence_input,viewport_visual_layer::center,center,north_east);
	run_frame(cadence,cadence_input,10150);
	set_layer(cadence_input,viewport_visual_layer::center,center,center);
	run_frame(cadence,cadence_input,10225);
	render=cadence.get_movement(
		cadence_viewport,viewport_visual_layer::center,2,2);
	assert(render.active&&render.progress==0.5f);
	set_layer(cadence_input,viewport_visual_layer::center,south_east,center);
	run_frame(cadence,cadence_input,10350);
	set_layer(cadence_input,viewport_visual_layer::center,south_east,south_east);
	run_frame(cadence,cadence_input,10450);
	render=cadence.get_movement(
		cadence_viewport,viewport_visual_layer::center,3,3);
	assert(render.active&&render.progress==0.5f);
	set_layer(cadence_input,viewport_visual_layer::center,center,south_east);
	run_frame(cadence,cadence_input,10600);
	set_layer(cadence_input,viewport_visual_layer::center,center,center);
	run_frame(cadence,cadence_input,10725);
	render=cadence.get_movement(
		cadence_viewport,viewport_visual_layer::center,2,2);
	assert(render.active&&render.progress==0.5f);
	}

	// A context change clears cadence. Once the delayed buffer crossing settles, movement falls
	// back to 100ms instead of inheriting the 150ms since the previous view's last step.
	{
	constexpr int32_t cadence_dim=4;
	int32_t cadence_empty[cadence_dim*cadence_dim]={};
	int32_t at_zero[cadence_dim*cadence_dim]={};
	int32_t at_one[cadence_dim*cadence_dim]={};
	int32_t at_two[cadence_dim*cadence_dim]={};
	at_zero[0*cadence_dim+1]=42;
	at_one[1*cadence_dim+1]=42;
	at_two[2*cadence_dim+1]=42;
	const int cadence_token=0;
	const void *cadence_viewport=&cadence_token;
	visual_animation_managerst cadence;
	auto cadence_input=make_input(cadence_viewport,cadence_dim,cadence_empty);
	set_layer(cadence_input,viewport_visual_layer::center,at_zero,cadence_empty);
	run_frame(cadence,cadence_input,6990);
	set_layer(cadence_input,viewport_visual_layer::center,at_one,at_zero);
	run_frame(cadence,cadence_input,7000);
	++cadence_input.context_revision;
	set_layer(cadence_input,viewport_visual_layer::center,at_one,at_one);
	run_frame(cadence,cadence_input,7010);
	set_layer(cadence_input,viewport_visual_layer::center,at_one,cadence_empty);
	run_frame(cadence,cadence_input,7020);
	set_layer(cadence_input,viewport_visual_layer::center,at_two,at_one);
	run_frame(cadence,cadence_input,7150);
	set_layer(cadence_input,viewport_visual_layer::center,at_two,at_two);
	run_frame(cadence,cadence_input,7200);
	render=cadence.get_movement(
		cadence_viewport,viewport_visual_layer::center,2,1);
	assert(render.active&&render.progress==0.5f);
	}

	// Giving up on an unrecognized pan clears cadence before movement detection resumes.
	{
	constexpr int32_t cadence_dim=4;
	int32_t cadence_empty[cadence_dim*cadence_dim]={};
	int32_t at_zero[cadence_dim*cadence_dim]={};
	int32_t at_one[cadence_dim*cadence_dim]={};
	int32_t at_two[cadence_dim*cadence_dim]={};
	int32_t unmatched_a[cadence_dim*cadence_dim]={};
	int32_t unmatched_b[cadence_dim*cadence_dim]={};
	at_zero[0*cadence_dim+1]=42;
	at_one[1*cadence_dim+1]=42;
	at_two[2*cadence_dim+1]=42;
	unmatched_a[1*cadence_dim+1]=78;
	unmatched_b[1*cadence_dim+1]=79;
	const int cadence_token=0;
	const void *cadence_viewport=&cadence_token;
	visual_animation_managerst cadence;
	auto cadence_input=make_input(cadence_viewport,cadence_dim,cadence_empty);
	set_layer(cadence_input,viewport_visual_layer::center,at_zero,cadence_empty);
	run_frame(cadence,cadence_input,7990);
	set_layer(cadence_input,viewport_visual_layer::center,at_one,at_zero);
	run_frame(cadence,cadence_input,8000);
	cadence_input.pan_x=1;
	set_layer(cadence_input,viewport_visual_layer::center,at_one,at_one);
	run_frame(cadence,cadence_input,8010);
	for(int32_t frame=0;frame<5;++frame)
		{
		set_layer(cadence_input,viewport_visual_layer::center,at_one,
			frame%2==0?unmatched_a:unmatched_b);
		run_frame(cadence,cadence_input,8020+uint32_t(frame)*10);
		}
	set_layer(cadence_input,viewport_visual_layer::center,at_one,unmatched_b);
	run_frame(cadence,cadence_input,8070);
	set_layer(cadence_input,viewport_visual_layer::center,at_two,at_one);
	run_frame(cadence,cadence_input,8080);
	set_layer(cadence_input,viewport_visual_layer::center,at_two,at_two);
	run_frame(cadence,cadence_input,8130);
	render=cadence.get_movement(
		cadence_viewport,viewport_visual_layer::center,2,1);
	assert(render.active&&render.progress==0.5f);
	}

	// Two same-sprite sources make a movement unknowable. Their histories are dropped, and the
	// next unambiguous step from the merged target starts with the fallback duration.
	{
	constexpr int32_t cadence_dim=5;
	int32_t cadence_empty[cadence_dim*cadence_dim]={};
	int32_t starts[cadence_dim*cadence_dim]={};
	int32_t tracked[cadence_dim*cadence_dim]={};
	int32_t ambiguous_target[cadence_dim*cadence_dim]={};
	int32_t next[cadence_dim*cadence_dim]={};
	starts[0*cadence_dim+1]=42;
	starts[3*cadence_dim+3]=42;
	tracked[1*cadence_dim+1]=42;
	tracked[2*cadence_dim+2]=42;
	ambiguous_target[2*cadence_dim+1]=42;
	next[3*cadence_dim+1]=42;
	const int cadence_token=0;
	const void *cadence_viewport=&cadence_token;
	visual_animation_managerst cadence;
	auto cadence_input=make_input(cadence_viewport,cadence_dim,cadence_empty);
	set_layer(cadence_input,viewport_visual_layer::center,starts,cadence_empty);
	run_frame(cadence,cadence_input,8990);
	set_layer(cadence_input,viewport_visual_layer::center,tracked,starts);
	run_frame(cadence,cadence_input,9000);
	set_layer(cadence_input,viewport_visual_layer::center,ambiguous_target,tracked);
	run_frame(cadence,cadence_input,9150);
	assert(!cadence.get_movement(
		cadence_viewport,viewport_visual_layer::center,2,1).active);
	set_layer(cadence_input,viewport_visual_layer::center,next,ambiguous_target);
	run_frame(cadence,cadence_input,9300);
	set_layer(cadence_input,viewport_visual_layer::center,next,next);
	run_frame(cadence,cadence_input,9350);
	render=cadence.get_movement(
		cadence_viewport,viewport_visual_layer::center,3,1);
	assert(render.active&&render.progress==0.5f);
	}

	visual_animation_managerst ambiguous;
	run_frame(ambiguous,input,2990);
	previous.fill(0);
	previous[0*3+1]=42;
	previous[1*3+0]=42;
	set_layer(input,viewport_visual_layer::center,current.data(),previous.data());
	run_frame(ambiguous,input,3000);
	assert(!ambiguous.get_movement(
		viewport,viewport_visual_layer::center,1,1).active);

	// A handler and led animal form an occupied chain: each enters the other's old space.
	visual_animation_managerst convoy;
	current.fill(0);
	previous.fill(0);
	run_frame(convoy,input,3490);
	previous[0*3+1]=41;
	previous[1*3+1]=42;
	current[1*3+1]=41;
	current[2*3+1]=42;
	run_frame(convoy,input,3500);
	const auto animal=convoy.get_movement(
		viewport,viewport_visual_layer::center,1,1);
	const auto handler=convoy.get_movement(
		viewport,viewport_visual_layer::center,2,1);
	assert(animal.active&&animal.source_x==0&&animal.source_y==1);
	assert(handler.active&&handler.source_x==1&&handler.source_y==1);

	// A multi-tile fragment can use its own movement or its mapped center tile as proof of ownership.
	for(const auto layer:{viewport_visual_layer::right,viewport_visual_layer::left,
		viewport_visual_layer::upright,viewport_visual_layer::up,
		viewport_visual_layer::upleft})
		{
		current.fill(0);
		previous.fill(0);
		previous[0*3+1]=50;
		current[1*3+1]=50;
		assert(visual_moved_between_tiles(
			layer,current.data(),previous.data(),0*3+1,1*3+1));
		previous[1*3+1]=50;
		assert(!visual_moved_between_tiles(
			layer,current.data(),previous.data(),0*3+1,1*3+1));
		}

	visual_animation_managerst context;
	current.fill(0);
	previous.fill(0);
	run_frame(context,input,4000);
	previous[0*3+1]=42;
	current[1*3+1]=42;
	run_frame(context,input,4010);
	assert(context.get_movement(
		viewport,viewport_visual_layer::center,1,1).active);

	++input.context_revision;
	run_frame(context,input,4020);
	assert(!context.get_movement(
		viewport,viewport_visual_layer::center,1,1).active);

	// Camera-pan handling. window_x/window_y change at input time but the buffers shift on a later
	// render frame, so the manager must (a) NOT create movements from the buffer shift itself (the
	// floating-sprite bug), and (b) translate in-flight movements on the frame the shift lands.
	std::array<int32_t,9> pan_current{};
	std::array<int32_t,9> pan_previous{};
	std::array<int32_t,9> pan_empty{};
	auto pan_input=make_input(viewport,3,pan_empty.data());
	set_layer(
		pan_input,
		viewport_visual_layer::center,
		pan_current.data(),
		pan_previous.data());

	// FLOAT REGRESSION: a stationary creature, pan announced at frame A, buffers shift at frame B.
	// Frame B's buffers look exactly like a real move ((1,1)->(0,1) with a unique source) — the
	// manager must recognize it as the pending pan and create NO movement.
	visual_animation_managerst floaty;
	pan_previous[1*3+1]=42;
	pan_current[1*3+1]=42;
	run_frame(floaty,pan_input,4990);
	pan_input.pan_x=1;                   // frame A: window scrolled, buffers unchanged
	run_frame(floaty,pan_input,5000);
	assert(!floaty.get_movement(viewport,viewport_visual_layer::center,1,1).active);
	pan_previous[1*3+1]=42;              // frame B: buffers apply the shift
	pan_current.fill(0);
	pan_current[0*3+1]=42;
	run_frame(floaty,pan_input,5010);
	assert(!floaty.get_movement(viewport,viewport_visual_layer::center,0,1).active);

	// FOLLOW: an in-flight movement survives the announce frame untouched and is translated on the
	// frame the buffers shift, so the sprite tracks the scrolled world.
	visual_animation_managerst panner;
	pan_current.fill(0);
	pan_previous.fill(0);
	pan_input.pan_x=0;
	run_frame(panner,pan_input,5990);
	pan_previous[0*3+1]=42;              // creature steps (0,1) -> (1,1)
	pan_current[1*3+1]=42;
	run_frame(panner,pan_input,6000);
	auto moved=panner.get_movement(viewport,viewport_visual_layer::center,1,1);
	assert(moved.active&&moved.source_x==0&&moved.source_y==1);

	pan_input.pan_x=1;                   // frame A: pan announced, buffers unchanged
	pan_previous[0*3+1]=0;
	pan_previous[1*3+1]=42;              // previous now matches current (stationary at (1,1))
	run_frame(panner,pan_input,6010);
	moved=panner.get_movement(viewport,viewport_visual_layer::center,1,1);
	assert(moved.active&&moved.source_x==0);   // untouched: still anchored to the old frame

	pan_current.fill(0);                 // frame B: buffers shift east by one
	pan_current[0*3+1]=42;
	run_frame(panner,pan_input,6020);
	assert(!panner.get_movement(viewport,viewport_visual_layer::center,1,1).active);
	auto followed=panner.get_movement(viewport,viewport_visual_layer::center,0,1);
	assert(followed.active&&followed.source_x==-1&&followed.source_y==1);
	// Cadence follows the same translation. The next step uses the 120ms since the first one.
	pan_previous=pan_current;
	pan_current.fill(0);
	pan_current[1*3+1]=42;
	run_frame(panner,pan_input,6120);
	pan_previous=pan_current;
	run_frame(panner,pan_input,6180);
	moved=panner.get_movement(viewport,viewport_visual_layer::center,1,1);
	assert(moved.active&&moved.progress==0.5f);

	// SAME-FRAME: pan announced and buffers shifted in the same call — translated immediately.
	visual_animation_managerst same_frame;
	pan_current.fill(0);
	pan_previous.fill(0);
	pan_input.pan_x=0;
	run_frame(same_frame,pan_input,6990);
	pan_previous[0*3+1]=42;
	pan_current[1*3+1]=42;
	run_frame(same_frame,pan_input,7000);
	assert(same_frame.get_movement(viewport,viewport_visual_layer::center,1,1).active);
	pan_input.pan_x=1;
	pan_previous=pan_current;
	pan_current.fill(0);
	pan_current[0*3+1]=42;
	run_frame(same_frame,pan_input,7010);
	followed=same_frame.get_movement(viewport,viewport_visual_layer::center,0,1);
	assert(followed.active&&followed.source_x==-1);

	// A change that is NOT a pure pan (context revision bump) still resets, even with in-flight work.
	visual_animation_managerst reset_on_zoom;
	pan_current.fill(0);
	pan_previous.fill(0);
	pan_input.pan_x=0;
	pan_input.context_revision=1;
	run_frame(reset_on_zoom,pan_input,8000);
	pan_previous[0*3+1]=42;
	pan_current[1*3+1]=42;
	run_frame(reset_on_zoom,pan_input,8010);
	assert(reset_on_zoom.get_movement(viewport,viewport_visual_layer::center,1,1).active);
	pan_input.context_revision=2;       // e.g. zoom / z-level / resize
	run_frame(reset_on_zoom,pan_input,8020);
	assert(!reset_on_zoom.get_movement(viewport,viewport_visual_layer::center,1,1).active);

	// Status fragments inherit nearby center motion even while their texture flashes.
	std::array<int32_t,9> status_current{};
	std::array<int32_t,9> status_previous{};
	current.fill(0);
	previous.fill(0);
	input.context_revision=1;
	input.current.fill(empty.data());
	input.previous.fill(empty.data());
	set_layer(input,viewport_visual_layer::center,current.data(),previous.data());
	set_layer(input,viewport_visual_layer::item,status_current.data(),status_previous.data());
	set_layer(
		input,
		viewport_visual_layer::designation,
		status_current.data(),
		status_previous.data());
	visual_animation_managerst companion;
	run_frame(companion,input,8990);
	previous[0*3+1]=42;
	current[1*3+1]=42;
	status_previous[0*3+0]=90;
	status_current[1*3+0]=91;
	assert(visual_moved_between_tiles(
		viewport_visual_layer::designation,
		status_current.data(),status_previous.data(),0*3+0,1*3+0));
	status_previous[0*3+0]=0;
	assert(visual_moved_between_tiles(
		viewport_visual_layer::designation,
		status_current.data(),status_previous.data(),0*3+0,1*3+0));
	assert(!visual_moved_between_tiles(
		viewport_visual_layer::item,
		status_current.data(),status_previous.data(),0*3+0,1*3+0));
	status_previous[1*3+0]=80;
	assert(!visual_moved_between_tiles(
		viewport_visual_layer::designation,
		status_current.data(),status_previous.data(),0*3+0,1*3+0));
	status_previous[0*3+0]=90;
	status_previous[1*3+0]=0;
	run_frame(companion,input,9000);
	auto status=companion.get_movement(viewport,viewport_visual_layer::designation,1,0);
	assert(status.active&&!status.inherited&&status.source_x==0&&status.source_y==0);
	const auto carried_item=companion.get_movement(
		viewport,viewport_visual_layer::item,1,0);
	assert(carried_item.active&&carried_item.inherited);
	previous=current;
	status_current[1*3+0]=92;
	run_frame(companion,input,9050);
	status=companion.get_movement(viewport,viewport_visual_layer::designation,1,0);
	assert(status.active&&status.progress==0.5f);

	// Divergent nearby creature movements make companion ownership ambiguous, so the overlay snaps.
	current.fill(0);
	previous.fill(0);
	status_current.fill(0);
	status_previous.fill(0);
	visual_animation_managerst crowd;
	run_frame(crowd,input,9990);
	previous[0*3+0]=41;
	current[0*3+1]=41;
	previous[2*3+2]=42;
	current[2*3+1]=42;
	status_current[1*3+1]=99;
	run_frame(crowd,input,10000);
	assert(!crowd.get_movement(
		viewport,viewport_visual_layer::designation,1,1).active);
	status_previous[1*3+0]=90;
	status_current[1*3+1]=91;
	run_frame(crowd,input,10010);
	status=crowd.get_movement(viewport,viewport_visual_layer::designation,1,1);
	assert(status.active);
	assert(!status.inherited);
	assert(status.source_x==1&&status.source_y==0);

	// Wheelbarrows use the item layer and the same independent adjacent-movement detection.
	current.fill(0);
	previous.fill(0);
	input.current.fill(empty.data());
	input.previous.fill(empty.data());
	set_layer(input,viewport_visual_layer::item,current.data(),previous.data());
	visual_animation_managerst item;
	run_frame(item,input,10990);
	previous[0*3+1]=77;
	current[1*3+1]=77;
	run_frame(item,input,11000);
	const auto item_move=item.get_movement(
		viewport,viewport_visual_layer::item,1,1);
	assert(item_move.active&&item_move.source_x==0&&item_move.source_y==1);

	// Minecart graphics can change texpos while moving; vehicle identity is tile occupancy.
	current.fill(0);
	previous.fill(0);
	input.current.fill(empty.data());
	input.previous.fill(empty.data());
	set_layer(input,viewport_visual_layer::vehicle,current.data(),previous.data());
	visual_animation_managerst vehicle;
	run_frame(vehicle,input,11990);
	previous[0*3+1]=77;
	current[1*3+1]=78;
	run_frame(vehicle,input,12000);
	assert(vehicle.get_movement(
		viewport,viewport_visual_layer::vehicle,1,1).active);
	previous=current;
	current[1*3+1]=79;
	run_frame(vehicle,input,12050);
	const auto cart=vehicle.get_movement(
		viewport,viewport_visual_layer::vehicle,1,1);
	assert(cart.active&&cart.progress==0.5f);
	previous=current;
	current.fill(0);
	current[2*3+1]=80;
	run_frame(vehicle,input,12060);
	const auto chained=vehicle.get_movement(
		viewport,viewport_visual_layer::vehicle,2,1);
	assert(chained.active&&chained.source_x>0.0f&&chained.source_x<1.0f&&
		chained.progress==0.0f);

	// FOLLOW CAMERA / ADVENTURE PLAYER: the unit and camera advance southeast together,
	// leaving the unit on the same viewport tile. Dense terrain is the authoritative evidence that
	// the scroll landed; rebasing the old creature layer reveals the otherwise-hidden unit step.
	{
	constexpr int32_t follow_dim=7;
	constexpr size_t follow_tiles=size_t(follow_dim)*size_t(follow_dim);
	const int follow_token=0;
	const void *follow_viewport=&follow_token;
	std::array<int32_t,follow_tiles> follow_empty{};
	std::array<int32_t,follow_tiles> follow_current{};
	std::array<int32_t,follow_tiles> follow_previous{};
	std::array<int32_t,follow_tiles> background_current{};
	std::array<int32_t,follow_tiles> background_previous{};
	fill_background(background_previous,1000);
	background_current=background_previous;
	const int32_t center=follow_dim/2;
	follow_current[size_t(center*follow_dim+center)]=42;
	follow_previous=follow_current;
	auto follow_input=make_input(follow_viewport,follow_dim,follow_empty.data());
	set_layer(follow_input,viewport_visual_layer::center,
		follow_current.data(),follow_previous.data());
	follow_input.current_background=background_current.data();
	follow_input.previous_background=background_previous.data();

	visual_animation_managerst follow_manager;
	run_frame(follow_manager,follow_input,20000);
	follow_input.pan_x=1;
	follow_input.pan_y=1;
	run_frame(follow_manager,follow_input,20010);
	assert(follow_manager.get_scroll(follow_viewport).pending);
	shift_background(background_current,background_previous,follow_dim,1,1,2000);
	run_frame(follow_manager,follow_input,20020);
	auto scroll=follow_manager.get_scroll(follow_viewport);
	assert(scroll.landed&&scroll.landed_x==1&&scroll.landed_y==1&&!scroll.pending);
	assert(scroll.follow_candidate!=no_visual_movement);
	auto follow_move=follow_manager.get_movement(
		follow_viewport,viewport_visual_layer::center,center,center);
	assert(follow_move.active&&follow_move.source_x==center-1&&
		follow_move.source_y==center-1&&
		follow_move.movement_id==scroll.follow_candidate);
	auto follow=follow_manager.get_follow(follow_viewport,scroll.follow_candidate);
	assert(follow.active&&follow.offset_x==1.0f&&follow.offset_y==1.0f);

	const auto assert_screen_locked=[&](visual_movement_idst movement_id)
		{
		const auto movement=follow_manager.get_movement(
			follow_viewport,viewport_visual_layer::center,center,center);
		const auto camera=follow_manager.get_follow(follow_viewport,movement_id);
		assert(movement.active&&camera.active);
		const float proxy_offset_x=
			(movement.source_x-center)*(1.0f-movement.progress);
		const float proxy_offset_y=
			(movement.source_y-center)*(1.0f-movement.progress);
		assert(std::abs(proxy_offset_x+camera.offset_x)<0.000001f);
		assert(std::abs(proxy_offset_y+camera.offset_y)<0.000001f);
		const double pixel_residual=
			std::abs(std::lround(double(camera.offset_x)*tile_size)+
				double(proxy_offset_x)*tile_size);
		assert(pixel_residual<=0.5);
		};
	assert_screen_locked(scroll.follow_candidate);
	run_frame(follow_manager,follow_input,20050);
	assert_screen_locked(scroll.follow_candidate);

	// A consecutive step replaces the stable anchor ID, but the new inverse displacement still
	// exactly cancels the retargeted fractional proxy, so its screen position is continuous.
	follow_input.pan_x=2;
	follow_input.pan_y=2;
	run_frame(follow_manager,follow_input,20060);
	background_previous=background_current;
	shift_background(background_current,background_previous,follow_dim,1,1,3000);
	follow_previous=follow_current;
	run_frame(follow_manager,follow_input,20070);
	const auto chained_scroll=follow_manager.get_scroll(follow_viewport);
	assert(chained_scroll.landed&&chained_scroll.landed_x==1&&
		chained_scroll.landed_y==1&&
		chained_scroll.follow_candidate!=no_visual_movement&&
		chained_scroll.follow_candidate!=scroll.follow_candidate);
	const auto chained_follow=
		follow_manager.get_follow(follow_viewport,chained_scroll.follow_candidate);
	assert(chained_follow.active&&chained_follow.offset_x>1.0f&&
		chained_follow.offset_y>1.0f);
	assert_screen_locked(chained_scroll.follow_candidate);
	}

	// Several compensating movers are deterministic: the center-nearest target wins, with the
	// normal x/y scan order acting as the coordinate tie-breaker.
	{
	constexpr int32_t crowd_dim=7;
	constexpr size_t crowd_tiles=size_t(crowd_dim)*size_t(crowd_dim);
	const int anchor_token=0;
	const void *anchor_viewport=&anchor_token;
	std::array<int32_t,crowd_tiles> anchor_empty{};
	std::array<int32_t,crowd_tiles> anchor_current{};
	std::array<int32_t,crowd_tiles> anchor_previous{};
	std::array<int32_t,crowd_tiles> terrain_current{};
	std::array<int32_t,crowd_tiles> terrain_previous{};
	fill_background(terrain_previous,4000);
	terrain_current=terrain_previous;
	for(const auto &[x,y,texpos]:{
		std::array<int32_t,3>{1,1,41},
		std::array<int32_t,3>{2,3,42},
		std::array<int32_t,3>{4,3,43}})
		anchor_current[size_t(x*crowd_dim+y)]=texpos;
	anchor_previous=anchor_current;
	auto anchor_input=make_input(anchor_viewport,crowd_dim,anchor_empty.data());
	set_layer(anchor_input,viewport_visual_layer::center,
		anchor_current.data(),anchor_previous.data());
	anchor_input.current_background=terrain_current.data();
	anchor_input.previous_background=terrain_previous.data();
	visual_animation_managerst anchors;
	run_frame(anchors,anchor_input,21000);
	anchor_input.pan_x=1;
	run_frame(anchors,anchor_input,21010);
	shift_background(terrain_current,terrain_previous,crowd_dim,1,0,5000);
	run_frame(anchors,anchor_input,21020);
	const auto chosen_scroll=anchors.get_scroll(anchor_viewport);
	const auto first_center_tie=anchors.get_movement(
		anchor_viewport,viewport_visual_layer::center,2,3);
	assert(chosen_scroll.follow_candidate!=no_visual_movement&&first_center_tie.active&&
		chosen_scroll.follow_candidate==first_center_tie.movement_id);
	}

	// With no visual follow anchor, terrain still retires the pan and the camera uses a complete
	// one-tile exponential fallback. Existing motion decays first; a new landing is not decayed on
	// its creation frame.
	{
	constexpr int32_t fallback_dim=4;
	constexpr size_t fallback_tiles=size_t(fallback_dim)*size_t(fallback_dim);
	const int fallback_token=0;
	std::array<int32_t,fallback_tiles> no_visuals{};
	std::array<int32_t,fallback_tiles> terrain_current{};
	std::array<int32_t,fallback_tiles> terrain_previous{};
	fill_background(terrain_previous,6000);
	terrain_current=terrain_previous;
	auto fallback_input=make_input(&fallback_token,fallback_dim,no_visuals.data());
	fallback_input.current_background=terrain_current.data();
	fallback_input.previous_background=terrain_previous.data();
	visual_animation_managerst fallback;
	run_frame(fallback,fallback_input,22000);
	fallback_input.pan_x=1;
	run_frame(fallback,fallback_input,22010);
	shift_background(terrain_current,terrain_previous,fallback_dim,1,0,7000);
	run_frame(fallback,fallback_input,22020);
	const auto fallback_scroll=fallback.get_scroll(&fallback_token);
	assert(fallback_scroll.landed&&fallback_scroll.follow_candidate==no_visual_movement);
	assert(camera_transient_after_landing(0.0,16,32.0)==32.0);
	const double existing_then_new=camera_transient_after_landing(32.0,35,32.0);
	assert(existing_then_new>43.0&&existing_then_new<44.0);
	}

	// One announced three-tile scroll can land piecemeal. The remaining debt stays pending and is
	// exposed to drag bookkeeping in content-window coordinates.
	{
	constexpr int32_t partial_dim=6;
	constexpr size_t partial_tiles=size_t(partial_dim)*size_t(partial_dim);
	const int partial_token=0;
	std::array<int32_t,partial_tiles> no_visuals{};
	std::array<int32_t,partial_tiles> terrain_current{};
	std::array<int32_t,partial_tiles> terrain_previous{};
	fill_background(terrain_previous,8000);
	terrain_current=terrain_previous;
	auto partial_input=make_input(&partial_token,partial_dim,no_visuals.data());
	partial_input.current_background=terrain_current.data();
	partial_input.previous_background=terrain_previous.data();
	visual_animation_managerst partial;
	run_frame(partial,partial_input,23000);
	partial_input.pan_x=3;
	run_frame(partial,partial_input,23010);
	shift_background(terrain_current,terrain_previous,partial_dim,1,0,9000);
	run_frame(partial,partial_input,23020);
	auto partial_scroll=partial.get_scroll(&partial_token);
	assert(partial_scroll.landed&&partial_scroll.landed_x==1&&
		partial_scroll.pending&&partial_scroll.pending_x==2);
	terrain_previous=terrain_current;
	shift_background(terrain_current,terrain_previous,partial_dim,2,0,10000);
	run_frame(partial,partial_input,23030);
	partial_scroll=partial.get_scroll(&partial_token);
	assert(partial_scroll.landed&&partial_scroll.landed_x==2&&!partial_scroll.pending);
	}

	// Coalesced ordered announcements can land in one redraw, and a later reversing scroll is
	// tracked independently instead of cancelling an already-landed prefix.
	{
	constexpr int32_t ordered_dim=5;
	constexpr size_t ordered_tiles=size_t(ordered_dim)*size_t(ordered_dim);
	const int ordered_token=0;
	std::array<int32_t,ordered_tiles> no_visuals{};
	std::array<int32_t,ordered_tiles> terrain_current{};
	std::array<int32_t,ordered_tiles> terrain_previous{};
	fill_background(terrain_previous,11000);
	terrain_current=terrain_previous;
	auto ordered_input=make_input(&ordered_token,ordered_dim,no_visuals.data());
	ordered_input.current_background=terrain_current.data();
	ordered_input.previous_background=terrain_previous.data();
	visual_animation_managerst ordered;
	run_frame(ordered,ordered_input,24000);
	ordered_input.pan_x=1;
	run_frame(ordered,ordered_input,24010);
	ordered_input.pan_x=2;
	run_frame(ordered,ordered_input,24020);
	shift_background(terrain_current,terrain_previous,ordered_dim,2,0,12000);
	run_frame(ordered,ordered_input,24030);
	auto ordered_scroll=ordered.get_scroll(&ordered_token);
	assert(ordered_scroll.landed&&ordered_scroll.landed_x==2&&!ordered_scroll.pending);
	ordered_input.pan_x=1;
	run_frame(ordered,ordered_input,24040);
	terrain_previous=terrain_current;
	shift_background(terrain_current,terrain_previous,ordered_dim,-1,0,13000);
	run_frame(ordered,ordered_input,24050);
	ordered_scroll=ordered.get_scroll(&ordered_token);
	assert(ordered_scroll.landed&&ordered_scroll.landed_x==-1&&!ordered_scroll.pending);
	}

	// Reversing announcements remain ordered while both are pending; their signed total is zero,
	// but the first landed buffer shift must still retire only the first event.
	{
	constexpr int32_t reversing_dim=5;
	constexpr size_t reversing_tiles=size_t(reversing_dim)*size_t(reversing_dim);
	const int reversing_token=0;
	std::array<int32_t,reversing_tiles> no_visuals{};
	std::array<int32_t,reversing_tiles> terrain_current{};
	std::array<int32_t,reversing_tiles> terrain_previous{};
	fill_background(terrain_previous,16000);
	terrain_current=terrain_previous;
	auto reversing_input=
		make_input(&reversing_token,reversing_dim,no_visuals.data());
	reversing_input.current_background=terrain_current.data();
	reversing_input.previous_background=terrain_previous.data();
	visual_animation_managerst reversing;
	run_frame(reversing,reversing_input,24500);
	reversing_input.pan_x=1;
	run_frame(reversing,reversing_input,24510);
	reversing_input.pan_x=0;
	run_frame(reversing,reversing_input,24520);
	shift_background(terrain_current,terrain_previous,reversing_dim,1,0,17000);
	run_frame(reversing,reversing_input,24530);
	auto reversing_scroll=reversing.get_scroll(&reversing_token);
	assert(reversing_scroll.landed&&reversing_scroll.landed_x==1&&
		reversing_scroll.pending&&reversing_scroll.pending_x==-1);
	terrain_previous=terrain_current;
	shift_background(terrain_current,terrain_previous,reversing_dim,-1,0,18000);
	run_frame(reversing,reversing_input,24540);
	reversing_scroll=reversing.get_scroll(&reversing_token);
	assert(reversing_scroll.landed&&reversing_scroll.landed_x==-1&&
		!reversing_scroll.pending);
	}

	// Uniform terrain may make a shift observable before DF swaps buffers. Accepting it early is
	// visually equivalent and prevents pending suppression from sticking. Empty terrain has no
	// evidence and abandons safely; excessive debt also abandons and requests a snap.
	{
	constexpr int32_t edge_dim=4;
	constexpr size_t edge_tiles=size_t(edge_dim)*size_t(edge_dim);
	const int uniform_token=0,uniform_sprite_token=0,empty_token=0,debt_token=0,
		queue_token=0,aged_token=0;
	std::array<int32_t,edge_tiles> no_visuals{};
	std::array<int32_t,edge_tiles> uniform_current{};
	std::array<int32_t,edge_tiles> uniform_previous{};
	uniform_current.fill(77);
	uniform_previous=uniform_current;
	auto uniform_input=make_input(&uniform_token,edge_dim,no_visuals.data());
	uniform_input.current_background=uniform_current.data();
	uniform_input.previous_background=uniform_previous.data();
	visual_animation_managerst uniform;
	run_frame(uniform,uniform_input,25000);
	uniform_input.pan_x=1;
	run_frame(uniform,uniform_input,25010);
	const auto uniform_scroll=uniform.get_scroll(&uniform_token);
	assert(uniform_scroll.landed&&uniform_scroll.landed_x==1&&!uniform_scroll.pending);

	std::array<int32_t,edge_tiles> uniform_sprite_current{};
	std::array<int32_t,edge_tiles> uniform_sprite_previous{};
	uniform_sprite_current[2*edge_dim+1]=42;
	uniform_sprite_previous=uniform_sprite_current;
	auto uniform_sprite_input=
		make_input(&uniform_sprite_token,edge_dim,no_visuals.data());
	set_layer(uniform_sprite_input,viewport_visual_layer::center,
		uniform_sprite_current.data(),uniform_sprite_previous.data());
	uniform_sprite_input.current_background=uniform_current.data();
	uniform_sprite_input.previous_background=uniform_previous.data();
	visual_animation_managerst uniform_sprite;
	run_frame(uniform_sprite,uniform_sprite_input,25020);
	uniform_sprite_input.pan_x=1;
	run_frame(uniform_sprite,uniform_sprite_input,25030);
	auto uniform_sprite_scroll=uniform_sprite.get_scroll(&uniform_sprite_token);
	assert(!uniform_sprite_scroll.landed&&uniform_sprite_scroll.pending);
	uniform_sprite_previous=uniform_sprite_current;
	uniform_sprite_current.fill(0);
	uniform_sprite_current[1*edge_dim+1]=42;
	run_frame(uniform_sprite,uniform_sprite_input,25040);
	uniform_sprite_scroll=uniform_sprite.get_scroll(&uniform_sprite_token);
	assert(uniform_sprite_scroll.landed&&!uniform_sprite_scroll.pending);
	assert(!uniform_sprite.get_movement(
		&uniform_sprite_token,viewport_visual_layer::center,1,1).active);

	std::array<int32_t,edge_tiles> empty_background{};
	auto empty_input=make_input(&empty_token,edge_dim,no_visuals.data());
	empty_input.current_background=empty_background.data();
	empty_input.previous_background=empty_background.data();
	visual_animation_managerst empty_scroll;
	run_frame(empty_scroll,empty_input,25100);
	empty_input.pan_x=1;
	run_frame(empty_scroll,empty_input,25110);
	assert(empty_scroll.get_scroll(&empty_token).abandoned);

	std::array<int32_t,edge_tiles> debt_background{};
	fill_background(debt_background,14000);
	auto debt_input=make_input(&debt_token,edge_dim,no_visuals.data());
	debt_input.current_background=debt_background.data();
	debt_input.previous_background=debt_background.data();
	visual_animation_managerst debt;
	run_frame(debt,debt_input,25200);
	debt_input.pan_x=7;
	run_frame(debt,debt_input,25210);
	const auto debt_scroll=debt.get_scroll(&debt_token);
	assert(debt_scroll.abandoned&&!debt_scroll.pending);

	std::array<int32_t,edge_tiles> queue_background{};
	fill_background(queue_background,19000);
	auto queue_input=make_input(&queue_token,edge_dim,no_visuals.data());
	queue_input.current_background=queue_background.data();
	queue_input.previous_background=queue_background.data();
	visual_animation_managerst queue_limit;
	run_frame(queue_limit,queue_input,25220);
	for(int32_t frame=1;frame<=8;++frame)
		{
		queue_input.pan_x=frame%2;
		run_frame(queue_limit,queue_input,25220+uint32_t(frame));
		assert(!queue_limit.get_scroll(&queue_token).abandoned);
		}
	queue_input.pan_x=1;
	run_frame(queue_limit,queue_input,25229);
	const auto queue_scroll=queue_limit.get_scroll(&queue_token);
	assert(queue_scroll.abandoned&&queue_scroll.pending&&queue_scroll.pending_x==1);

	std::array<int32_t,edge_tiles> aged_background{};
	fill_background(aged_background,15000);
	auto aged_input=make_input(&aged_token,edge_dim,no_visuals.data());
	aged_input.current_background=aged_background.data();
	aged_input.previous_background=aged_background.data();
	visual_animation_managerst aged;
	run_frame(aged,aged_input,25300);
	aged_input.pan_x=1;
	run_frame(aged,aged_input,25301); // pending age 1
	for(uint32_t frame=2;frame<=120;++frame)
		run_frame(aged,aged_input,25300+frame);
	assert(aged.get_scroll(&aged_token).pending);
	run_frame(aged,aged_input,25421);
	const auto aged_scroll=aged.get_scroll(&aged_token);
	assert(aged_scroll.abandoned&&!aged_scroll.pending);
	}
}
