// SPDX-License-Identifier: MIT

#include "PluginManager.h"
#include "VTableInterpose.h"

#include "df/enabler.h"
#include "df/graphic.h"
#include "df/graphic_viewportst.h"
#include "df/interface_key.h"
#include "df/renderer_2d_base.h"
#include "df/texture_fullid.h"
#include "df/viewscreen_dungeonmodest.h"
#include "df/viewscreen_dwarfmodest.h"

#include "visual_animation.h"

#include <SDL_render.h>
#include <SDL_timer.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace DFHack;

DFHACK_PLUGIN("smooth-movement");
DFHACK_PLUGIN_IS_ENABLED(is_enabled);

REQUIRE_GLOBAL(enabler);
REQUIRE_GLOBAL(gps);
REQUIRE_GLOBAL(window_x);
REQUIRE_GLOBAL(window_y);
REQUIRE_GLOBAL(window_z);

namespace {

constexpr const char *plugin_version="0.4.0-dev";

#ifdef WIN32
constexpr const char *sdl_library="SDL2.dll";
#else
constexpr const char *sdl_library="libSDL2-2.0.so.0";
#endif

// Runtime harness for the engine-owned visual state; gameplay data is never read.
DFLibrary *sdl_handle=nullptr;
using RenderCopyF=int (*)(SDL_Renderer *,SDL_Texture *,const SDL_Rect *,const SDL_FRect *);
using RenderFillRect=int (*)(SDL_Renderer *,const SDL_Rect *);
using RenderSetClipRect=int (*)(SDL_Renderer *,const SDL_Rect *);
using GetRenderDrawColor=int (*)(SDL_Renderer *,Uint8 *,Uint8 *,Uint8 *,Uint8 *);
using SetRenderDrawColor=int (*)(SDL_Renderer *,Uint8,Uint8,Uint8,Uint8);
using GetTicks=Uint32 (*)();
RenderCopyF render_copy_f=nullptr;
RenderFillRect render_fill_rect=nullptr;
RenderSetClipRect render_set_clip_rect=nullptr;
GetRenderDrawColor get_render_draw_color=nullptr;
SetRenderDrawColor set_render_draw_color=nullptr;
GetTicks get_ticks=nullptr;

visual_animation_managerst animation_manager;
uint64_t stat_proxies=0;        // interpolated sprites actually drawn
uint64_t stat_cache_misses=0;   // active movements dropped: texture not in the tile cache
uint64_t stat_glide_frames=0;   // frames the world was drawn at a sub-tile camera offset
uint64_t stat_mouse_shifts=0;   // input dispatches corrected for the visual camera offset
uint32_t last_sig_diff=0;       // bitmask: which context-signature fields changed this frame
uint64_t stat_visual_jumps=0;   // frames the drawn world position moved > 0.6 tile at once
double prev_visual_x=0.0;       // drawn world offset last frame, pixels
double prev_visual_y=0.0;
bool has_prev_visual=false;
uint64_t prev_visual_revision=0;
float last_jump_x=0.0f;         // this frame's discontinuity, for the trace row
float last_jump_y=0.0f;

// Per-frame diagnostic trace (dump with `smooth-movement trace`): enough to reconstruct what
// the window, the context signature, the manager and the camera did around a glitch.
struct trace_framest
{
	uint32_t frame;
	int32_t wx,wy,wz;
	uint32_t sig_diff;
	uint64_t revision;
	uint32_t landings,static_f,identical,unrecognized,absorbed,resets,movements,suppressed,pans;
	int32_t pending_dx,pending_dy;
	float slide_px_x,slide_px_y;
	float jump_x,jump_y;
};
constexpr size_t trace_capacity=2700;
std::array<trace_framest,trace_capacity> trace_ring{};
uint32_t trace_frames=0;
std::set<std::pair<int32_t,int32_t>> previous_coverage;
uint64_t visual_context_revision=0;
const void *previous_viewport=nullptr;
std::array<int32_t,12> previous_view_signature{};
bool has_view_signature=false;
// Map scroll (window_x/window_y) is tracked separately from the reset signature: a pure pan is
// followed (movements are translated) instead of triggering a full reset, so it must NOT bump the
// context revision. It only invalidates the viewport-space blackout coverage from the prior frame.
int32_t previous_pan_x=0;
int32_t previous_pan_y=0;
bool has_pan_context=false;

// --- world-tile slide ---------------------------------------------------------------------------
// When a map scroll lands, the world content jumps by -shift tiles on screen. The slide draws the
// world (terrain and everything world-anchored) at a decaying pixel offset so it visibly GLIDES to
// its new position instead of stepping -- while sprites the camera carries (the adventure-mode
// player: screen-static across the landing, reported by the manager as "pinned") are drawn WITHOUT
// the offset, so the world moves under them. No camera state, no persistent offsets, no window
// writes: purely a per-landing render animation, eased with the same 100 ms smoothstep as creature
// movement so world-anchored creatures and terrain stay in lockstep.
constexpr double slide_max_tiles=4.0;         // beyond this the view just steps (fast scrolling)
constexpr uint32_t slide_duration_ms=100;
double slide_from_x=0.0;                      // pixel offset at slide start
double slide_from_y=0.0;
uint32_t slide_start_ms=0;
bool slide_active=false;
bool slide_was_offset=false;                  // edge-detects offset->0 for one cleanup redraw
uint64_t slide_resets_seen=0;                 // manager resets cancel the slide
uint64_t slide_revision_seen=0;               // context changes cancel the slide
int32_t slide_drag_cooldown=0;                // frames after a drag whose landings don't slide

// --- outgoing-tile cache -------------------------------------------------------------------------
// While the world slides, the trailing screen edge shows tiles that have already scrolled OUT of
// the viewport buffers; without help they render black until the slide lands. At each landing the
// pre-scroll frame still sits in the engine's *_old buffers, so a padded cache in the CURRENT
// window's coordinate frame is rebuilt from them -- chaining the previous cache so stacked
// landings keep up to `strip_pad` tiles of departed world alive -- and the renderer draws the
// trailing band from it.
int32_t tile_pixel(int32_t tile,int32_t origin,int32_t zoom);

constexpr int32_t strip_pad=int32_t(slide_max_tiles);
constexpr size_t strip_layer_count=10;
struct strip_cachest
{
	int32_t dim_x=0;
	int32_t dim_y=0;
	bool valid=false;
	std::array<std::vector<int32_t>,strip_layer_count> layers;
};
strip_cachest strip_caches[2];
int strip_cache_front=0;
uint64_t stat_strip_draws=0;    // cached strip tiles drawn under the slide's trailing edge
uint64_t stat_strip_misses=0;   // cached texpos with no tile-cache texture (stays black)

int32_t strip_index(const strip_cachest &cache,int32_t x,int32_t y)
{
	return (x+strip_pad)*(cache.dim_y+2*strip_pad)+(y+strip_pad);
}

std::array<const int32_t *,strip_layer_count> strip_old_layers(df::graphic_viewportst *vp)
{
	return {
		vp->screentexpos_background_old,
		vp->screentexpos_background_two_old,
		vp->screentexpos_building_one_old,
		vp->screentexpos_item_old,
		vp->screentexpos_vehicle_old,
		vp->screentexpos_vermin_old,
		vp->screentexpos_right_creature_old,
		vp->screentexpos_old,
		vp->screentexpos_left_creature_old,
		vp->screentexpos_building_two_old
		};
}

void strip_cache_invalidate()
{
	strip_caches[0].valid=false;
	strip_caches[1].valid=false;
}

// Rebuild the cache in the new window's coordinate frame: the freshly departed strip comes from
// the *_old buffers (the pre-landing frame), older strips ride over from the previous cache.
void strip_cache_update(df::graphic_viewportst *vp,int32_t shift_x,int32_t shift_y)
{
	strip_cachest &prev=strip_caches[strip_cache_front];
	strip_cachest &next=strip_caches[1-strip_cache_front];
	next.dim_x=vp->dim_x;
	next.dim_y=vp->dim_y;
	const size_t cells=size_t(vp->dim_x+2*strip_pad)*size_t(vp->dim_y+2*strip_pad);
	const bool chain=prev.valid&&prev.dim_x==vp->dim_x&&prev.dim_y==vp->dim_y;
	const auto old_layers=strip_old_layers(vp);
	for(size_t layer=0;layer<strip_layer_count;++layer)
		{
		next.layers[layer].assign(cells,0);
		for(int32_t x=-strip_pad;x<vp->dim_x+strip_pad;++x)
			{
			for(int32_t y=-strip_pad;y<vp->dim_y+strip_pad;++y)
				{
				const int32_t sx=x+shift_x;
				const int32_t sy=y+shift_y;
				int32_t value=0;
				if(sx>=0&&sx<vp->dim_x&&sy>=0&&sy<vp->dim_y)
					value=old_layers[layer][sx*vp->dim_y+sy];
				else if(chain&&sx>=-strip_pad&&sx<vp->dim_x+strip_pad&&
					sy>=-strip_pad&&sy<vp->dim_y+strip_pad)
					value=prev.layers[layer][strip_index(prev,sx,sy)];
				next.layers[layer][strip_index(next,x,y)]=value;
				}
			}
		}
	next.valid=true;
	strip_cache_front=1-strip_cache_front;
}

void draw_strip_tile(
	df::renderer_2d_base *renderer,
	SDL_Renderer *sdl_renderer,
	const strip_cachest &cache,
	int32_t x,
	int32_t y,
	int32_t zoom,
	int32_t tile_size)
{
	const int32_t index=strip_index(cache,x,y);
	for(size_t layer=0;layer<strip_layer_count;++layer)
		{
		const int32_t texpos=cache.layers[layer][index];
		if(texpos==0)continue;
		df::texture_fullid texture_id;
		texture_id.texpos=texpos;
		texture_id.r=texture_id.g=texture_id.b=1.0f;
		texture_id.br=texture_id.bg=texture_id.bb=0.0f;
		if(layer!=0)
			texture_id.flag=df::texture_fullid_flag::mask_transparent_background;
		const auto texture=renderer->tile_cache.tile_cache.find(texture_id);
		if(texture==renderer->tile_cache.tile_cache.end())
			{
			++stat_strip_misses;
			continue;
			}
		const SDL_FRect destination=
			{
			float(tile_pixel(x,renderer->origin_x,zoom)),
			float(tile_pixel(y,renderer->origin_y,zoom)),
			float(tile_size),
			float(tile_size)
			};
		render_copy_f(
			sdl_renderer,
			static_cast<SDL_Texture *>(texture->second),
			nullptr,
			&destination);
		++stat_strip_draws;
		}
}

double slide_offset_now(uint32_t now_ms,double from)
{
	if(!slide_active)return 0.0;
	const double t=std::min(1.0,double(now_ms-slide_start_ms)/double(slide_duration_ms));
	const double ease=1.0-t*t*(3.0-2.0*t);
	return from*ease;
}

double tile_px(const df::renderer_2d_base *renderer)
{
	const int32_t zoom=renderer->viewport_zoom_factor;
	return double(zoom==128?32:std::max(1,zoom*32/128));
}

constexpr uint32_t fire_bits=0x70000000U;

void update_visual_context(
	const df::renderer_2d_base *renderer,
	const df::graphic_viewportst *vp)
{
	// window_x/window_y are deliberately excluded: a horizontal/vertical scroll is followed, not
	// reset. window_z (z-level) stays, since a z change is not followable.
	const std::array<int32_t,12> signature=
		{
		window_z?*window_z:0,
		vp->dim_x,
		vp->dim_y,
		vp->clipx[0],
		vp->clipx[1],
		vp->clipy[0],
		vp->clipy[1],
		renderer->viewport_zoom_factor,
		renderer->origin_x,
		renderer->origin_y,
		gps->dimx,
		gps->dimy
		};
	last_sig_diff=0;
	if(has_view_signature&&previous_viewport==vp)
		{
		for(size_t i=0;i<signature.size();++i)
			{
			if(previous_view_signature[i]!=signature[i])last_sig_diff|=1U<<i;
			}
		}
	const bool changed=!has_view_signature||previous_viewport!=vp||
		previous_view_signature!=signature;
	if(changed)
		{
		++visual_context_revision;
		previous_coverage.clear();
		}
	previous_viewport=vp;
	previous_view_signature=signature;
	has_view_signature=true;

	// On a pure pan the reset signature is unchanged, but last frame's blackout coverage is in the
	// old viewport frame, so discard it (the engine repaints the whole scrolled viewport anyway).
	const int32_t pan_x=window_x?*window_x:0;
	const int32_t pan_y=window_y?*window_y:0;
	if(!has_pan_context||previous_pan_x!=pan_x||previous_pan_y!=pan_y)
		previous_coverage.clear();
	previous_pan_x=pan_x;
	previous_pan_y=pan_y;
	has_pan_context=true;
}

std::array<int32_t *,6> creature_layers(df::graphic_viewportst *vp)
{
	return {
		vp->screentexpos_right_creature,
		vp->screentexpos,
		vp->screentexpos_left_creature,
		vp->screentexpos_upright_creature,
		vp->screentexpos_up_creature,
		vp->screentexpos_upleft_creature
		};
}

std::array<int32_t *,9> visual_layers(df::graphic_viewportst *vp)
{
	auto creature=creature_layers(vp);
	return {
		creature[0],
		creature[1],
		creature[2],
		creature[3],
		creature[4],
		creature[5],
		vp->screentexpos_vehicle,
		vp->screentexpos_item,
		vp->screentexpos_designation
		};
}

viewport_visual_animation_inputst animation_input(df::graphic_viewportst *vp)
{
	return {
		vp,
		vp->dim_x,
		vp->dim_y,
		visual_context_revision,
		{
			vp->screentexpos_right_creature,
			vp->screentexpos,
			vp->screentexpos_left_creature,
			vp->screentexpos_upright_creature,
			vp->screentexpos_up_creature,
			vp->screentexpos_upleft_creature,
			vp->screentexpos_vehicle,
			vp->screentexpos_item,
			vp->screentexpos_designation
			},
		{
			vp->screentexpos_right_creature_old,
			vp->screentexpos_old,
			vp->screentexpos_left_creature_old,
			vp->screentexpos_upright_creature_old,
			vp->screentexpos_up_creature_old,
			vp->screentexpos_upleft_creature_old,
			vp->screentexpos_vehicle_old,
			vp->screentexpos_item_old,
			vp->screentexpos_designation_old
			},
		window_x?*window_x:0,
		window_y?*window_y:0,
		// Scroll-landing oracle: lets creature movements that coincide with a camera scroll
		// animate (adventure mode -- the camera follows the player, so EVERY step is one).
		vp->screentexpos_background,
		vp->screentexpos_background_old
		};
}

int32_t tile_pixel(int32_t tile,int32_t origin,int32_t zoom)
{
	return zoom==128?32*tile+origin:(zoom*32*tile)/128+origin;
}

bool inside_clip(const df::graphic_viewportst *vp,int32_t x,int32_t y)
{
	return x>=vp->clipx[0]&&x<=vp->clipx[1]&&
		y>=vp->clipy[0]&&y<=vp->clipy[1];
}

bool has_fire(const df::graphic_viewportst *vp,int32_t x,int32_t y)
{
	return vp->screentexpos_spatter_flag[x*vp->dim_y+y]&fire_bits;
}

bool is_main_creature(viewport_creature_layer layer)
{
	return layer==viewport_creature_layer::right||
		layer==viewport_creature_layer::center||
		layer==viewport_creature_layer::left;
}

template<typename T>
class scoped_zerost
{
	T *value_ptr;
	T saved{};

	public:
		scoped_zerost(T *ptr,bool enabled=true):value_ptr(enabled?ptr:nullptr)
			{
			if(value_ptr!=nullptr)
				{
				saved=*value_ptr;
				*value_ptr=0;
				}
			}

		~scoped_zerost()
			{
			if(value_ptr!=nullptr)*value_ptr=saved;
			}

		scoped_zerost(const scoped_zerost &)=delete;
		scoped_zerost &operator=(const scoped_zerost &)=delete;
};

struct render_proxyst
{
	viewport_creature_layer layer;
	float source_x;
	float source_y;
	int32_t target_x;
	int32_t target_y;
	int32_t texpos;
	float progress;
	SDL_Texture *texture;
	std::set<std::pair<int32_t,int32_t>> coverage;
};

void redraw_world_tile(
	df::renderer_2d_base *renderer,
	df::graphic_viewportst *vp,
	int32_t x,
	int32_t y,
	const std::unordered_map<int32_t,uint16_t> &selected)
{
	const int32_t index=x*vp->dim_y+y;
	const auto found=selected.find(index);
	const uint16_t mask=found==selected.end()?0:found->second;
	auto layers=creature_layers(vp);
	scoped_zerost<int32_t> item(
		vp->screentexpos_item+index,
		mask&(1U<<static_cast<uint8_t>(viewport_creature_layer::item)));
	scoped_zerost<int32_t> vehicle(
		vp->screentexpos_vehicle+index,
		mask&(1U<<static_cast<uint8_t>(viewport_creature_layer::vehicle)));
	scoped_zerost<int32_t> right(layers[0]+index,mask&(1U<<0));
	scoped_zerost<int32_t> center(layers[1]+index,mask&(1U<<1));
	scoped_zerost<int32_t> left(layers[2]+index,mask&(1U<<2));
	scoped_zerost<int32_t> upright(layers[3]+index,mask&(1U<<3));
	scoped_zerost<int32_t> up(layers[4]+index,mask&(1U<<4));
	scoped_zerost<int32_t> upleft(layers[5]+index,mask&(1U<<5));
	scoped_zerost<int32_t> designation(
		vp->screentexpos_designation+index,
		mask&(1U<<static_cast<uint8_t>(viewport_creature_layer::designation)));

	for(int32_t lower=7;lower>=0;--lower)
		{
		df::graphic_viewportst *lower_vp=gps->lower_viewport[lower];
		if(lower_vp!=nullptr&&lower_vp->flag.bits.active)
			renderer->update_viewport_tile(lower_vp,x,y);
		}
	renderer->update_viewport_tile(vp,x,y);
}

void redraw_above_base(
	df::renderer_2d_base *renderer,
	df::graphic_viewportst *vp,
	int32_t x,
	int32_t y,
	viewport_creature_layer layer,
	const std::unordered_map<int32_t,uint16_t> &selected)
{
	const int32_t index=x*vp->dim_y+y;
	const auto found=selected.find(index);
	const uint16_t mask=found==selected.end()?0:found->second;

	scoped_zerost<int32_t> background(vp->screentexpos_background+index);
	scoped_zerost<uint64_t> floor_flag(vp->screentexpos_floor_flag+index);
	scoped_zerost<int32_t> background_two(vp->screentexpos_background_two+index);
	scoped_zerost<uint32_t> liquid_flag(vp->screentexpos_liquid_flag+index);
	scoped_zerost<uint32_t> spatter_flag(vp->screentexpos_spatter_flag+index);
	scoped_zerost<int32_t> spatter(vp->screentexpos_spatter+index);
	scoped_zerost<uint64_t> ramp_flag(vp->screentexpos_ramp_flag+index);
	scoped_zerost<uint32_t> shadow_flag(vp->screentexpos_shadow_flag+index);
	scoped_zerost<int32_t> building_one(vp->screentexpos_building_one+index);
	scoped_zerost<int32_t> item(vp->screentexpos_item+index);
	scoped_zerost<int32_t> vehicle(
		vp->screentexpos_vehicle+index,
		layer==viewport_creature_layer::vehicle||
			mask&(1U<<static_cast<uint8_t>(viewport_creature_layer::vehicle)));
	scoped_zerost<int32_t> right(
		vp->screentexpos_right_creature+index,mask&(1U<<0));
	scoped_zerost<int32_t> center(
		vp->screentexpos+index,mask&(1U<<1));
	scoped_zerost<int32_t> left(
		vp->screentexpos_left_creature+index,mask&(1U<<2));
	scoped_zerost<int32_t> upright(
		vp->screentexpos_upright_creature+index,mask&(1U<<3));
	scoped_zerost<int32_t> up(
		vp->screentexpos_up_creature+index,mask&(1U<<4));
	scoped_zerost<int32_t> upleft(
		vp->screentexpos_upleft_creature+index,mask&(1U<<5));
	scoped_zerost<int32_t> designation(
		vp->screentexpos_designation+index,
		mask&(1U<<static_cast<uint8_t>(viewport_creature_layer::designation)));
	renderer->update_viewport_tile(vp,x,y);
}

void redraw_above_main(
	df::renderer_2d_base *renderer,
	df::graphic_viewportst *vp,
	int32_t x,
	int32_t y,
	const std::unordered_map<int32_t,uint16_t> &selected)
{
	const int32_t index=x*vp->dim_y+y;
	const auto found=selected.find(index);
	const uint16_t mask=found==selected.end()?0:found->second;

	scoped_zerost<int32_t> background(vp->screentexpos_background+index);
	scoped_zerost<uint64_t> floor_flag(vp->screentexpos_floor_flag+index);
	scoped_zerost<int32_t> background_two(vp->screentexpos_background_two+index);
	scoped_zerost<uint32_t> liquid_flag(vp->screentexpos_liquid_flag+index);
	scoped_zerost<uint32_t> spatter_flag(vp->screentexpos_spatter_flag+index);
	scoped_zerost<int32_t> spatter(vp->screentexpos_spatter+index);
	scoped_zerost<uint64_t> ramp_flag(vp->screentexpos_ramp_flag+index);
	scoped_zerost<uint32_t> shadow_flag(vp->screentexpos_shadow_flag+index);
	scoped_zerost<int32_t> building_one(vp->screentexpos_building_one+index);
	scoped_zerost<int32_t> item(vp->screentexpos_item+index);
	scoped_zerost<int32_t> vehicle(vp->screentexpos_vehicle+index);
	scoped_zerost<int32_t> vermin(vp->screentexpos_vermin+index);
	scoped_zerost<int32_t> right(vp->screentexpos_right_creature+index);
	scoped_zerost<int32_t> center(vp->screentexpos+index);
	scoped_zerost<int32_t> left(vp->screentexpos_left_creature+index);
	scoped_zerost<int32_t> upright(
		vp->screentexpos_upright_creature+index,mask&(1U<<3));
	scoped_zerost<int32_t> up(
		vp->screentexpos_up_creature+index,mask&(1U<<4));
	scoped_zerost<int32_t> upleft(
		vp->screentexpos_upleft_creature+index,mask&(1U<<5));
	scoped_zerost<int32_t> designation(
		vp->screentexpos_designation+index,
		mask&(1U<<static_cast<uint8_t>(viewport_creature_layer::designation)));
	renderer->update_viewport_tile(vp,x,y);
}

void redraw_above_upper(
	df::renderer_2d_base *renderer,
	df::graphic_viewportst *vp,
	int32_t x,
	int32_t y,
	const std::unordered_map<int32_t,uint16_t> &selected)
{
	const int32_t index=x*vp->dim_y+y;
	const auto found=selected.find(index);
	const uint16_t mask=found==selected.end()?0:found->second;
	scoped_zerost<int32_t> background(vp->screentexpos_background+index);
	scoped_zerost<uint64_t> floor_flag(vp->screentexpos_floor_flag+index);
	scoped_zerost<int32_t> background_two(vp->screentexpos_background_two+index);
	scoped_zerost<uint32_t> liquid_flag(vp->screentexpos_liquid_flag+index);
	scoped_zerost<uint32_t> spatter_flag(vp->screentexpos_spatter_flag+index);
	scoped_zerost<int32_t> spatter(vp->screentexpos_spatter+index);
	scoped_zerost<uint64_t> ramp_flag(vp->screentexpos_ramp_flag+index);
	scoped_zerost<uint32_t> shadow_flag(vp->screentexpos_shadow_flag+index);
	scoped_zerost<int32_t> building_one(vp->screentexpos_building_one+index);
	scoped_zerost<int32_t> item(vp->screentexpos_item+index);
	scoped_zerost<int32_t> vehicle(vp->screentexpos_vehicle+index);
	scoped_zerost<int32_t> vermin(vp->screentexpos_vermin+index);
	scoped_zerost<int32_t> right(vp->screentexpos_right_creature+index);
	scoped_zerost<int32_t> center(vp->screentexpos+index);
	scoped_zerost<int32_t> left(vp->screentexpos_left_creature+index);
	scoped_zerost<int32_t> building_two(vp->screentexpos_building_two+index);
	scoped_zerost<int32_t> projectile(vp->screentexpos_projectile+index);
	scoped_zerost<int32_t> high_flow(vp->screentexpos_high_flow+index);
	scoped_zerost<int32_t> top_shadow(vp->screentexpos_top_shadow+index);
	scoped_zerost<int32_t> signpost(vp->screentexpos_signpost+index);
	scoped_zerost<int32_t> upright(vp->screentexpos_upright_creature+index);
	scoped_zerost<int32_t> up(vp->screentexpos_up_creature+index);
	scoped_zerost<int32_t> upleft(vp->screentexpos_upleft_creature+index);
	scoped_zerost<int32_t> designation(
		vp->screentexpos_designation+index,
		mask&(1U<<static_cast<uint8_t>(viewport_creature_layer::designation)));
	renderer->update_viewport_tile(vp,x,y);
}

void draw_proxy(df::renderer_2d_base *renderer,const render_proxyst &proxy)
{
	const int32_t zoom=renderer->viewport_zoom_factor;
	const int32_t target_x=tile_pixel(proxy.target_x,renderer->origin_x,zoom);
	const int32_t target_y=tile_pixel(proxy.target_y,renderer->origin_y,zoom);
	const float tile_size=float(zoom==128?32:std::max(1,zoom*32/128));
	const float source_x=target_x+(proxy.source_x-proxy.target_x)*tile_size;
	const float source_y=target_y+(proxy.source_y-proxy.target_y)*tile_size;
	const SDL_FRect destination=
		{
		source_x+(target_x-source_x)*proxy.progress,
		source_y+(target_y-source_y)*proxy.progress,
		tile_size,
		tile_size
		};
	render_copy_f(
		static_cast<SDL_Renderer *>(renderer->sdl_renderer),
		proxy.texture,
		nullptr,
		&destination);
}

std::vector<render_proxyst> collect_proxies(
	df::renderer_2d_base *renderer,
	df::graphic_viewportst *vp)
{
	std::vector<render_proxyst> proxies;
	auto layers=visual_layers(vp);
	const std::array order={
		viewport_creature_layer::center,
		viewport_creature_layer::item,
		viewport_creature_layer::vehicle,
		viewport_creature_layer::right,
		viewport_creature_layer::left,
		viewport_creature_layer::upright,
		viewport_creature_layer::up,
		viewport_creature_layer::upleft,
		viewport_creature_layer::designation
		};
	for(viewport_creature_layer visual_layer:order)
		{
		const size_t layer=static_cast<size_t>(visual_layer);
		for(int32_t y=0;y<vp->dim_y;++y)
			{
			for(int32_t x=0;x<vp->dim_x;++x)
				{
				const int32_t index=x*vp->dim_y+y;
				const int32_t texpos=layers[layer][index];
				if(texpos==0)continue;
				const auto movement=animation_manager.get_movement(
					vp,static_cast<viewport_creature_layer>(layer),x,y);
				if(!movement.active)continue;
				if(layer!=static_cast<size_t>(viewport_creature_layer::center)&&
					layer!=static_cast<size_t>(viewport_creature_layer::vehicle)&&
					layer!=static_cast<size_t>(viewport_creature_layer::item))
					{
					bool anchored=false;
					for(const render_proxyst &anchor:proxies)
						{
						if(anchor.layer==viewport_creature_layer::center&&
							std::abs(anchor.target_x-x)<=1&&
							std::abs(anchor.target_y-y)<=1&&
							anchor.source_x-anchor.target_x==movement.source_x-x&&
							anchor.source_y-anchor.target_y==movement.source_y-y&&
							anchor.progress==movement.progress)anchored=true;
						}
					if(!anchored)continue;
					}
				if((visual_layer==viewport_creature_layer::item||
					visual_layer==viewport_creature_layer::designation)&&
					movement.inherited)
					{
					const int32_t source_x=x-
						((x>movement.source_x)-(x<movement.source_x));
					const int32_t source_y=y-
						((y>movement.source_y)-(y<movement.source_y));
					if(source_x<0||source_x>=vp->dim_x||
						source_y<0||source_y>=vp->dim_y)continue;
					const int32_t source=
						source_x*vp->dim_y+source_y;
					if(!visual_moved_between_tiles(
						layers[layer],
						visual_layer==viewport_creature_layer::item?
							vp->screentexpos_item_old:
							vp->screentexpos_designation_old,
						source,
						index))continue;
					}

				render_proxyst proxy=
					{
					static_cast<viewport_creature_layer>(layer),
					movement.source_x,
					movement.source_y,
					x,
					y,
					texpos,
					movement.progress,
					nullptr,
					{}
					};
				bool blocked=false;
				for(int32_t coverage_x=int32_t(std::floor(
						std::min(proxy.source_x,float(x))));
					coverage_x<=int32_t(std::ceil(
						std::max(proxy.source_x,float(x))));++coverage_x)
					{
					for(int32_t coverage_y=int32_t(std::floor(
							std::min(proxy.source_y,float(y))));
						coverage_y<=int32_t(std::ceil(
							std::max(proxy.source_y,float(y))));++coverage_y)
						{
						if(!inside_clip(vp,coverage_x,coverage_y))
							{
							blocked=true;
							break;
							}
						if(is_main_creature(proxy.layer)&&
							has_fire(vp,coverage_x,coverage_y))
							{
							blocked=true;
							break;
							}
						proxy.coverage.emplace(coverage_x,coverage_y);
						}
					if(blocked)break;
					}
				if(blocked)continue;

				df::texture_fullid texture_id;
				texture_id.texpos=texpos;
				texture_id.r=texture_id.g=texture_id.b=1.0f;
				texture_id.br=texture_id.bg=texture_id.bb=0.0f;
				texture_id.flag=
					df::texture_fullid_flag::mask_transparent_background;
				const auto texture=renderer->tile_cache.tile_cache.find(texture_id);
				if(texture==renderer->tile_cache.tile_cache.end())
					{
					++stat_cache_misses;
					continue;
					}
				proxy.texture=static_cast<SDL_Texture *>(texture->second);
				++stat_proxies;
				proxies.push_back(std::move(proxy));
				}
			}
		}
	return proxies;
}

void render_interpolated_world(df::renderer_2d_base *renderer)
{
	df::graphic_viewportst *vp=gps?gps->main_viewport:nullptr;

	if(vp!=nullptr)update_visual_context(renderer,vp);
	animation_manager.begin_frame(get_ticks());
	if(vp!=nullptr)
		{
		const auto input=animation_input(vp);
		animation_manager.synchronize_viewport(
			input,
			vp->flag.bits.active);
		}
	animation_manager.end_frame();

	if(vp==nullptr||!vp->flag.bits.active||
		render_copy_f==nullptr||renderer->sdl_renderer==nullptr)
		return;
	const double cam_tile=tile_px(renderer);
	// --- world-tile slide update ---
	const uint32_t now_ms=animation_manager.get_frame_time_ms();
	if(animation_manager.stats.resets!=slide_resets_seen||
		visual_context_revision!=slide_revision_seen)
		{
		slide_active=false;   // unfollowable change: the world snaps to the grid
		strip_cache_invalidate();
		}
	slide_resets_seen=animation_manager.stats.resets;
	slide_revision_seen=visual_context_revision;
	// A middle-mouse drag is a direct manipulation: the view must track the mouse
	// crisply, so the slide sits out while the button is held -- and for a few frames
	// after release, since the drag's final window steps land in the buffers late and
	// would otherwise bounce the view back as a parting slide.
	const bool dragging=enabler!=nullptr&&enabler->mouse_mbut;
	if(dragging)
		{
		slide_active=false;
		slide_drag_cooldown=3;
		}
	else if(slide_drag_cooldown>0)--slide_drag_cooldown;
	double slide_now_x=slide_offset_now(now_ms,slide_from_x);
	double slide_now_y=slide_offset_now(now_ms,slide_from_y);
	if(slide_active&&now_ms-slide_start_ms>=slide_duration_ms)
		{
		slide_active=false;
		slide_now_x=0.0;
		slide_now_y=0.0;
		}
	if(animation_manager.stats.last_shift_x!=0||animation_manager.stats.last_shift_y!=0)
		strip_cache_update(
			vp,
			animation_manager.stats.last_shift_x,
			animation_manager.stats.last_shift_y);
	if((animation_manager.stats.last_shift_x!=0||animation_manager.stats.last_shift_y!=0)&&
		!dragging&&slide_drag_cooldown==0)
		{
		// A scroll landed this frame: the content jumped by -shift tiles on screen. Start
		// (or retarget from the current fractional offset) a slide back to the grid.
		const double cap=slide_max_tiles*cam_tile;
		slide_from_x=std::clamp(
			slide_now_x+double(animation_manager.stats.last_shift_x)*cam_tile,-cap,cap);
		slide_from_y=std::clamp(
			slide_now_y+double(animation_manager.stats.last_shift_y)*cam_tile,-cap,cap);
		slide_start_ms=now_ms;
		slide_active=true;
		slide_now_x=slide_from_x;
		slide_now_y=slide_from_y;
		}
	const int32_t glide_x=int32_t(std::lround(slide_now_x));
	const int32_t glide_y=int32_t(std::lround(slide_now_y));
	// Visual continuity check: the drawn world offset is -window*tile + glide. Any one-frame
	// move above 0.6 tile is a visible jump -- either an intended snap or the jank we hunt.
	{
	const double visual_x=-double(window_x?*window_x:0)*cam_tile+slide_now_x;
	const double visual_y=-double(window_y?*window_y:0)*cam_tile+slide_now_y;
	last_jump_x=0.0f;
	last_jump_y=0.0f;
	if(has_prev_visual&&prev_visual_revision==visual_context_revision)
		{
		const double dvx=visual_x-prev_visual_x;
		const double dvy=visual_y-prev_visual_y;
		if(std::abs(dvx)>0.6*cam_tile||std::abs(dvy)>0.6*cam_tile)
			{
			++stat_visual_jumps;
			last_jump_x=float(dvx);
			last_jump_y=float(dvy);
			}
		}
	prev_visual_x=visual_x;
	prev_visual_y=visual_y;
	prev_visual_revision=visual_context_revision;
	has_prev_visual=true;
	}
	const bool glide=glide_x!=0||glide_y!=0;
	if(!glide&&slide_was_offset)
		{
		// The world just re-joined the grid: one engine redraw replaces the last shifted frame.
		slide_was_offset=false;
		if(gps!=nullptr)++gps->force_full_display_count;
		}
	if(glide)slide_was_offset=true;
	if(!glide&&!animation_manager.requires_full_redraw())
		return;

	std::vector<render_proxyst> proxies=collect_proxies(renderer,vp);
	std::set<std::pair<int32_t,int32_t>> current_coverage;
	std::set<std::pair<int32_t,int32_t>> item_coverage;
	std::set<std::pair<int32_t,int32_t>> vehicle_coverage;
	std::set<std::pair<int32_t,int32_t>> main_coverage;
	std::set<std::pair<int32_t,int32_t>> upper_coverage;
	std::unordered_map<int32_t,uint16_t> selected;
	for(const render_proxyst &proxy:proxies)
		{
		current_coverage.insert(proxy.coverage.begin(),proxy.coverage.end());
		auto &layer_mask=selected[proxy.target_x*vp->dim_y+proxy.target_y];
		layer_mask|=uint16_t(1U<<static_cast<uint8_t>(proxy.layer));
		if(proxy.layer==viewport_creature_layer::item)
			item_coverage.insert(proxy.coverage.begin(),proxy.coverage.end());
		else if(proxy.layer==viewport_creature_layer::vehicle)
			vehicle_coverage.insert(proxy.coverage.begin(),proxy.coverage.end());
		else if(is_main_creature(proxy.layer))
			main_coverage.insert(proxy.coverage.begin(),proxy.coverage.end());
		else if(proxy.layer!=viewport_creature_layer::designation)
			upper_coverage.insert(proxy.coverage.begin(),proxy.coverage.end());
		}

	SDL_Renderer *sdl_renderer=static_cast<SDL_Renderer *>(renderer->sdl_renderer);
	const int32_t zoom=renderer->viewport_zoom_factor;
	const int32_t tile_size=zoom==128?32:std::max(1,zoom*32/128);

	if(glide)
		{
		++stat_glide_frames;
		// Camera-carried sprites (screen-static across a landing: the adventure player and
		// its overlays) are masked out of the sliding repaint and drawn pinned afterwards.
		const auto &pinned=animation_manager.get_pinned(vp);
		for(const auto &pin:pinned)
			{
			auto &layer_mask=selected[pin.x*vp->dim_y+pin.y];
			layer_mask|=uint16_t(1U<<static_cast<uint8_t>(pin.layer));
			}
		// Camera mid-glide: repaint the WHOLE map rect at the shifted origin so the world (and
		// the creature proxies, which read origin at draw time) renders between tiles. The engine
		// already drew this frame at the snapped position; everything here overdraws it, clipped
		// to the map rect so shifted tiles never spill over the UI. The uncovered strip on the
		// trailing edge stays black until the glide lands.
		const SDL_Rect map_rect=
			{
			tile_pixel(vp->clipx[0],renderer->origin_x,zoom),
			tile_pixel(vp->clipy[0],renderer->origin_y,zoom),
			tile_pixel(vp->clipx[1]+1,renderer->origin_x,zoom)-
				tile_pixel(vp->clipx[0],renderer->origin_x,zoom),
			tile_pixel(vp->clipy[1]+1,renderer->origin_y,zoom)-
				tile_pixel(vp->clipy[0],renderer->origin_y,zoom)
			};
		render_set_clip_rect(sdl_renderer,&map_rect);
		Uint8 old_r=0,old_g=0,old_b=0,old_a=255;
		get_render_draw_color(sdl_renderer,&old_r,&old_g,&old_b,&old_a);
		set_render_draw_color(sdl_renderer,0,0,0,255);
		render_fill_rect(sdl_renderer,&map_rect);
		set_render_draw_color(sdl_renderer,old_r,old_g,old_b,old_a);

		const int32_t saved_origin_x=renderer->origin_x;
		const int32_t saved_origin_y=renderer->origin_y;
		renderer->origin_x+=glide_x;
		renderer->origin_y+=glide_y;
		for(int32_t x=vp->clipx[0];x<=vp->clipx[1];++x)
			{
			for(int32_t y=vp->clipy[0];y<=vp->clipy[1];++y)
				redraw_world_tile(renderer,vp,x,y,selected);
			}
		// Trailing edge: the band uncovered by the slide has no viewport data any more --
		// draw it from the outgoing-tile cache so departed world stays visible while the
		// slide lands (the clip rect confines any overdraw to the map).
		{
		const strip_cachest &strip=strip_caches[strip_cache_front];
		if(strip.valid&&strip.dim_x==vp->dim_x&&strip.dim_y==vp->dim_y)
			{
			const int32_t band_w=std::min(strip_pad,glide_x>0?(glide_x+tile_size-1)/tile_size:0);
			const int32_t band_e=std::min(strip_pad,glide_x<0?(-glide_x+tile_size-1)/tile_size:0);
			const int32_t band_n=std::min(strip_pad,glide_y>0?(glide_y+tile_size-1)/tile_size:0);
			const int32_t band_s=std::min(strip_pad,glide_y<0?(-glide_y+tile_size-1)/tile_size:0);
			for(int32_t x=vp->clipx[0]-band_w;x<=vp->clipx[1]+band_e;++x)
				{
				for(int32_t y=vp->clipy[0]-band_n;y<=vp->clipy[1]+band_s;++y)
					{
					if(x>=vp->clipx[0]&&x<=vp->clipx[1]&&
						y>=vp->clipy[0]&&y<=vp->clipy[1])continue;
					if(x<-strip_pad||x>=vp->dim_x+strip_pad||
						y<-strip_pad||y>=vp->dim_y+strip_pad)continue;
					draw_strip_tile(renderer,sdl_renderer,strip,x,y,zoom,tile_size);
					}
				}
			}
		}
		for(const render_proxyst &proxy:proxies)
			{
			if(proxy.layer==viewport_creature_layer::item)draw_proxy(renderer,proxy);
			}
		for(const auto &[x,y]:item_coverage)
			redraw_above_base(
				renderer,vp,x,y,viewport_creature_layer::item,selected);
		for(const render_proxyst &proxy:proxies)
			{
			if(proxy.layer==viewport_creature_layer::vehicle)draw_proxy(renderer,proxy);
			}
		for(const auto &[x,y]:vehicle_coverage)
			redraw_above_base(
				renderer,vp,x,y,viewport_creature_layer::vehicle,selected);
		for(const render_proxyst &proxy:proxies)
			{
			if(is_main_creature(proxy.layer))draw_proxy(renderer,proxy);
			}
		for(const auto &[x,y]:main_coverage)
			redraw_above_main(renderer,vp,x,y,selected);
		for(const render_proxyst &proxy:proxies)
			{
			if(proxy.layer!=viewport_creature_layer::item&&
				proxy.layer!=viewport_creature_layer::vehicle&&
				proxy.layer!=viewport_creature_layer::designation&&
				!is_main_creature(proxy.layer))draw_proxy(renderer,proxy);
			}
		for(const auto &[x,y]:upper_coverage)
			redraw_above_upper(renderer,vp,x,y,selected);
		for(const render_proxyst &proxy:proxies)
			{
			if(proxy.layer==viewport_creature_layer::designation)draw_proxy(renderer,proxy);
			}
		renderer->origin_x=saved_origin_x;
		renderer->origin_y=saved_origin_y;
		// The pinned sprites render at their true screen tiles, WITHOUT the slide: the
		// world visibly moves beneath them.
		for(const auto &pin:pinned)
			{
			df::texture_fullid texture_id;
			texture_id.texpos=pin.texpos;
			texture_id.r=texture_id.g=texture_id.b=1.0f;
			texture_id.br=texture_id.bg=texture_id.bb=0.0f;
			texture_id.flag=df::texture_fullid_flag::mask_transparent_background;
			const auto texture=renderer->tile_cache.tile_cache.find(texture_id);
			if(texture==renderer->tile_cache.tile_cache.end())continue;
			const SDL_FRect destination=
				{
				float(tile_pixel(pin.x,renderer->origin_x,zoom)),
				float(tile_pixel(pin.y,renderer->origin_y,zoom)),
				float(tile_size),
				float(tile_size)
				};
			render_copy_f(
				sdl_renderer,
				static_cast<SDL_Texture *>(texture->second),
				nullptr,
				&destination);
			}
		render_set_clip_rect(sdl_renderer,nullptr);

		// Everything was repainted; per-tile coverage bookkeeping restarts after the glide.
		previous_coverage.clear();
		return;
		}

	std::set<std::pair<int32_t,int32_t>> redraw_coverage=current_coverage;
	redraw_coverage.insert(previous_coverage.begin(),previous_coverage.end());
	Uint8 old_r=0,old_g=0,old_b=0,old_a=255;
	get_render_draw_color(sdl_renderer,&old_r,&old_g,&old_b,&old_a);
	set_render_draw_color(sdl_renderer,0,0,0,255);
	for(const auto &[x,y]:redraw_coverage)
		{
		if(!inside_clip(vp,x,y))continue;
		const SDL_Rect tile_rect=
			{
			tile_pixel(x,renderer->origin_x,zoom),
			tile_pixel(y,renderer->origin_y,zoom),
			tile_size,
			tile_size
			};
		render_fill_rect(sdl_renderer,&tile_rect);
		}
	set_render_draw_color(sdl_renderer,old_r,old_g,old_b,old_a);

	for(const auto &[x,y]:redraw_coverage)
		{
		if(inside_clip(vp,x,y))redraw_world_tile(renderer,vp,x,y,selected);
		}
	for(const render_proxyst &proxy:proxies)
		{
		if(proxy.layer==viewport_creature_layer::item)draw_proxy(renderer,proxy);
		}
	for(const auto &[x,y]:item_coverage)
		redraw_above_base(renderer,vp,x,y,viewport_creature_layer::item,selected);
	for(const render_proxyst &proxy:proxies)
		{
		if(proxy.layer==viewport_creature_layer::vehicle)draw_proxy(renderer,proxy);
		}
	for(const auto &[x,y]:vehicle_coverage)
		redraw_above_base(renderer,vp,x,y,viewport_creature_layer::vehicle,selected);
	for(const render_proxyst &proxy:proxies)
		{
		if(is_main_creature(proxy.layer))draw_proxy(renderer,proxy);
		}
	for(const auto &[x,y]:main_coverage)
		redraw_above_main(renderer,vp,x,y,selected);
	for(const render_proxyst &proxy:proxies)
		{
		if(proxy.layer!=viewport_creature_layer::item&&
			proxy.layer!=viewport_creature_layer::vehicle&&
			proxy.layer!=viewport_creature_layer::designation&&
			!is_main_creature(proxy.layer))draw_proxy(renderer,proxy);
		}
	for(const auto &[x,y]:upper_coverage)
		redraw_above_upper(renderer,vp,x,y,selected);
	for(const render_proxyst &proxy:proxies)
		{
		if(proxy.layer==viewport_creature_layer::designation)draw_proxy(renderer,proxy);
		}

	previous_coverage=std::move(current_coverage);
}

// --- mouse dispatch under a camera offset ------------------------------------------------------
// While the free camera draws the world at a visual offset, DF's mouse->map-tile math (in
// graphics mode: viewport position + precise_mouse / tile_pixels) still resolves against the
// snapped grid, so clicks dispatch to the tile UNDER the grid instead of the tile the user sees
// under the cursor -- up to several tiles off mid-glide, and one tile off near tile edges while
// the camera rests between tiles. The fix presents DF's input consumers with pixel coordinates
// in the DISPLAYED frame: precise_mouse minus the visual offset, applied scoped around the map
// screens' feed/logic and around the engine's UI render stage (hover highlight), then restored.
// The interface-grid mouse (gps->mouse_x/y) is untouched, so UI widget hit-testing -- which runs
// on the interface grid, not map pixels -- is unaffected, and everything outside the wrapped
// calls (including this plugin's own drag-pan anchors) sees raw hardware values.

// Current visual camera offset in pixels, from gps's copy of the viewport zoom so input hooks
// need no renderer object. Matches the glide offset the render path applies to the world.
bool visual_mouse_shift(int32_t &shift_px_x,int32_t &shift_px_y)
{
	if(!slide_active||gps==nullptr)return false;
	const uint32_t now_ms=animation_manager.get_frame_time_ms();
	shift_px_x=int32_t(std::lround(slide_offset_now(now_ms,slide_from_x)));
	shift_px_y=int32_t(std::lround(slide_offset_now(now_ms,slide_from_y)));
	return shift_px_x!=0||shift_px_y!=0;
}

class scoped_mouse_shiftst
{
	int32_t saved_x=0;
	int32_t saved_y=0;
	bool active=false;

	public:
		scoped_mouse_shiftst()
			{
			int32_t shift_x=0;
			int32_t shift_y=0;
			if(!visual_mouse_shift(shift_x,shift_y))return;
			if(gps->precise_mouse_x<0||gps->precise_mouse_y<0)return;
			const df::graphic_viewportst *vp=gps->main_viewport;
			if(vp==nullptr)return;
			const int32_t zoom=gps->viewport_zoom_factor;
			const int32_t tile=zoom==128?32:std::max(1,zoom*32/128);
			saved_x=gps->precise_mouse_x;
			saved_y=gps->precise_mouse_y;
			// Clamp into the viewport: pixels over the trailing black strip have no displayed
			// tile, so they resolve to the nearest edge tile instead of going out of range.
			gps->precise_mouse_x=std::clamp(saved_x-shift_x,0,vp->dim_x*tile-1);
			gps->precise_mouse_y=std::clamp(saved_y-shift_y,0,vp->dim_y*tile-1);
			active=true;
			}

		~scoped_mouse_shiftst()
			{
			if(!active)return;
			gps->precise_mouse_x=saved_x;
			gps->precise_mouse_y=saved_y;
			}

		bool applied() const
			{
			return active;
			}

		scoped_mouse_shiftst(const scoped_mouse_shiftst &)=delete;
		scoped_mouse_shiftst &operator=(const scoped_mouse_shiftst &)=delete;
};

struct adventure_input_hook : df::viewscreen_dungeonmodest
{
	typedef df::viewscreen_dungeonmodest interpose_base;
	DEFINE_VMETHOD_INTERPOSE(void,feed,(std::set<df::interface_key> *events));
	DEFINE_VMETHOD_INTERPOSE(void,logic,());
};

IMPLEMENT_VMETHOD_INTERPOSE(adventure_input_hook,feed);
IMPLEMENT_VMETHOD_INTERPOSE(adventure_input_hook,logic);

void adventure_input_hook::interpose_fn_feed(std::set<df::interface_key> *events)
{
	scoped_mouse_shiftst shifted;
	if(shifted.applied())++stat_mouse_shifts;
	INTERPOSE_NEXT(feed)(events);
}

void adventure_input_hook::interpose_fn_logic()
{
	scoped_mouse_shiftst shifted;
	INTERPOSE_NEXT(logic)();
}

struct fort_input_hook : df::viewscreen_dwarfmodest
{
	typedef df::viewscreen_dwarfmodest interpose_base;
	DEFINE_VMETHOD_INTERPOSE(void,feed,(std::set<df::interface_key> *events));
	DEFINE_VMETHOD_INTERPOSE(void,logic,());
};

IMPLEMENT_VMETHOD_INTERPOSE(fort_input_hook,feed);
IMPLEMENT_VMETHOD_INTERPOSE(fort_input_hook,logic);

void fort_input_hook::interpose_fn_feed(std::set<df::interface_key> *events)
{
	scoped_mouse_shiftst shifted;
	if(shifted.applied())++stat_mouse_shifts;
	INTERPOSE_NEXT(feed)(events);
}

void fort_input_hook::interpose_fn_logic()
{
	scoped_mouse_shiftst shifted;
	INTERPOSE_NEXT(logic)();
}

struct renderer_hook : df::renderer_2d_base
{
	typedef df::renderer_2d_base interpose_base;
	DEFINE_VMETHOD_INTERPOSE(void,update_all,());
};

IMPLEMENT_VMETHOD_INTERPOSE(renderer_hook,update_all);

void record_trace_frame()
{
	trace_framest &rec=trace_ring[trace_frames%trace_capacity];
	++trace_frames;
	rec.frame=trace_frames;
	rec.wx=window_x?*window_x:0;
	rec.wy=window_y?*window_y:0;
	rec.wz=window_z?*window_z:0;
	rec.sig_diff=last_sig_diff;
	rec.revision=visual_context_revision;
	const auto &st=animation_manager.stats;
	rec.landings=uint32_t(st.landings);
	rec.static_f=uint32_t(st.static_frames);
	rec.identical=uint32_t(st.identical_frames);
	rec.unrecognized=uint32_t(st.unrecognized_frames);
	rec.absorbed=uint32_t(st.absorbed);
	rec.resets=uint32_t(st.resets);
	rec.movements=uint32_t(st.movements);
	rec.suppressed=uint32_t(st.suppressed);
	rec.pans=uint32_t(st.pan_frames);
	rec.pending_dx=st.last_pending_dx;
	rec.pending_dy=st.last_pending_dy;
	const uint32_t now_ms=animation_manager.get_frame_time_ms();
	rec.slide_px_x=float(slide_offset_now(now_ms,slide_from_x));
	rec.slide_px_y=float(slide_offset_now(now_ms,slide_from_y));
	rec.jump_x=last_jump_x;
	rec.jump_y=last_jump_y;
}

void renderer_hook::interpose_fn_update_all()
{
	// update_all is the existing UI stage, so world correction must run first.
	render_interpolated_world(this);
	record_trace_frame();
	// The UI stage computes the hover highlight from the mouse; shift it into the displayed
	// frame too, so the highlighted tile is the one a click would dispatch to.
	scoped_mouse_shiftst shifted;
	INTERPOSE_NEXT(update_all)();
}

bool load_sdl(color_ostream &out)
{
	sdl_handle=OpenPlugin(sdl_library);
	if(sdl_handle==nullptr)
		{
		out.printerr("smooth-movement: could not load SDL2\n");
		return false;
		}
	render_copy_f=reinterpret_cast<RenderCopyF>(
		LookupPlugin(sdl_handle,"SDL_RenderCopyF"));
	render_fill_rect=reinterpret_cast<RenderFillRect>(
		LookupPlugin(sdl_handle,"SDL_RenderFillRect"));
	render_set_clip_rect=reinterpret_cast<RenderSetClipRect>(
		LookupPlugin(sdl_handle,"SDL_RenderSetClipRect"));
	get_render_draw_color=reinterpret_cast<GetRenderDrawColor>(
		LookupPlugin(sdl_handle,"SDL_GetRenderDrawColor"));
	set_render_draw_color=reinterpret_cast<SetRenderDrawColor>(
		LookupPlugin(sdl_handle,"SDL_SetRenderDrawColor"));
	get_ticks=reinterpret_cast<GetTicks>(LookupPlugin(sdl_handle,"SDL_GetTicks"));
	if(render_copy_f==nullptr||render_fill_rect==nullptr||
		render_set_clip_rect==nullptr||
		get_render_draw_color==nullptr||set_render_draw_color==nullptr||
		get_ticks==nullptr)
		{
		out.printerr("smooth-movement: required SDL2 functions are unavailable\n");
		ClosePlugin(sdl_handle);
		sdl_handle=nullptr;
		return false;
		}
	return true;
}

void reset_state()
{
	animation_manager=visual_animation_managerst();
	stat_proxies=0;
	stat_cache_misses=0;
	stat_glide_frames=0;
	stat_mouse_shifts=0;
	stat_visual_jumps=0;
	has_prev_visual=false;
	previous_coverage.clear();
	visual_context_revision=0;
	previous_viewport=nullptr;
	previous_view_signature={};
	has_view_signature=false;
	previous_pan_x=0;
	previous_pan_y=0;
	has_pan_context=false;
	slide_from_x=0.0;
	slide_from_y=0.0;
	slide_start_ms=0;
	slide_active=false;
	slide_was_offset=false;
	slide_resets_seen=0;
	slide_revision_seen=0;
	slide_drag_cooldown=0;
	strip_cache_invalidate();
	stat_strip_draws=0;
	stat_strip_misses=0;
}

command_result status_command(
	color_ostream &out,
	std::vector<std::string> &parameters)
{
	if(parameters.empty())
		{
		out.print(
			"smooth-movement {}: {}\n",
			plugin_version,
			is_enabled?"enabled":"disabled");
		out.print("world slide: {}, offset {:.1f} {:.1f} px\n",
			slide_active?"active":"idle",
			slide_offset_now(animation_manager.get_frame_time_ms(),slide_from_x),
			slide_offset_now(animation_manager.get_frame_time_ms(),slide_from_y));
		out.print(
			"stats: movements {} landings {} suppressed {} resets {} | proxies {} cache-misses {}\n",
			animation_manager.stats.movements,
			animation_manager.stats.landings,
			animation_manager.stats.suppressed,
			animation_manager.stats.resets,
			stat_proxies,
			stat_cache_misses);
		out.print("glide frames: {}, shifted input dispatches: {}, visual jumps: {}\n",
			stat_glide_frames,stat_mouse_shifts,stat_visual_jumps);
		out.print("strip: draws {} cache-misses {}\n",stat_strip_draws,stat_strip_misses);
		out.print(
			"scroll: pans {} static {} identical {} unrecognized {} absorbed {} pending {} {}\n",
			animation_manager.stats.pan_frames,
			animation_manager.stats.static_frames,
			animation_manager.stats.identical_frames,
			animation_manager.stats.unrecognized_frames,
			animation_manager.stats.absorbed,
			animation_manager.stats.last_pending_dx,
			animation_manager.stats.last_pending_dy);
		return CR_OK;
		}
	if(parameters[0]=="trace")
		{
		// Dump the interesting rows of the per-frame ring: window moves, signature
		// flickers, manager attribution outcomes, camera pending/transient activity.
		static const char *sig_names[12]=
			{"z","dimx","dimy","cx0","cx1","cy0","cy1","zoom","ox","oy","gdimx","gdimy"};
		const uint32_t have=trace_frames<trace_capacity?trace_frames:uint32_t(trace_capacity);
		const trace_framest *prev=nullptr;
		uint32_t printed=0;
		for(uint32_t i=0;i<have;++i)
			{
			const trace_framest &rec=
				trace_ring[(trace_frames-have+i)%trace_capacity];
			bool interesting=rec.jump_x!=0.0f||rec.jump_y!=0.0f||rec.sig_diff!=0||
				rec.pending_dx!=0||rec.pending_dy!=0||
				std::abs(rec.slide_px_x)>0.5f||std::abs(rec.slide_px_y)>0.5f;
			std::string delta;
			if(prev!=nullptr)
				{
				if(rec.wx!=prev->wx||rec.wy!=prev->wy||rec.wz!=prev->wz)interesting=true;
				const auto d=[&](uint32_t now,uint32_t before,const char *tag)
					{
					if(now!=before)
						delta+=std::string(" ")+tag+"+"+std::to_string(now-before);
					};
				d(rec.pans,prev->pans,"pan");
				d(rec.landings,prev->landings,"land");
				d(rec.static_f,prev->static_f,"stat");
				d(rec.identical,prev->identical,"ident");
				d(rec.unrecognized,prev->unrecognized,"unrec");
				d(rec.absorbed,prev->absorbed,"absorb");
				d(rec.resets,prev->resets,"reset");
				d(rec.movements,prev->movements,"mov");
				d(rec.suppressed,prev->suppressed,"sup");
				if(rec.revision!=prev->revision)
					delta+=" REV+"+std::to_string(rec.revision-prev->revision);
				if(!delta.empty())interesting=true;
				}
			if(interesting)
				{
				std::string sig;
				for(uint32_t b=0;b<12;++b)
					{
					if(rec.sig_diff&(1U<<b))
						sig+=std::string(sig.empty()?"":"|")+sig_names[b];
					}
				std::string jump;
				if(rec.jump_x!=0.0f||rec.jump_y!=0.0f)
					jump=" JUMP="+std::to_string(int(rec.jump_x))+","+
						std::to_string(int(rec.jump_y));
				out.print(
					"f={} w={},{},{}{}{}{} pend={},{} slide={:.1f},{:.1f}\n",
					rec.frame,rec.wx,rec.wy,rec.wz,
					sig.empty()?"":(" sig="+sig),
					delta,
					jump,
					rec.pending_dx,rec.pending_dy,
					rec.slide_px_x,rec.slide_px_y);
				++printed;
				}
			prev=&rec;
			}
		out.print("({} of {} frames interesting)\n",printed,have);
		return CR_OK;
		}
	return CR_WRONG_USAGE;
}

} // namespace

DFhackCExport command_result
plugin_init(color_ostream &,std::vector<PluginCommand> &commands)
{
	commands.emplace_back(
		"smooth-movement",
		"Smooth movement status and diagnostics (trace).",
		status_command);
	return CR_OK;
}

DFhackCExport command_result plugin_enable(color_ostream &out,bool enable)
{
	if(is_enabled==enable)return CR_OK;
	if(enable)
		{
		reset_state();
		if(!load_sdl(out))return CR_FAILURE;
		if(!INTERPOSE_HOOK(renderer_hook,update_all).apply()||
			!INTERPOSE_HOOK(adventure_input_hook,feed).apply()||
			!INTERPOSE_HOOK(adventure_input_hook,logic).apply()||
			!INTERPOSE_HOOK(fort_input_hook,feed).apply()||
			!INTERPOSE_HOOK(fort_input_hook,logic).apply())
			{
			out.printerr("smooth-movement: could not hook the renderer/input\n");
			INTERPOSE_HOOK(renderer_hook,update_all).remove();
			INTERPOSE_HOOK(adventure_input_hook,feed).remove();
			INTERPOSE_HOOK(adventure_input_hook,logic).remove();
			INTERPOSE_HOOK(fort_input_hook,feed).remove();
			INTERPOSE_HOOK(fort_input_hook,logic).remove();
			ClosePlugin(sdl_handle);
			sdl_handle=nullptr;
			return CR_FAILURE;
			}
		}
	else
		{
		INTERPOSE_HOOK(renderer_hook,update_all).remove();
		INTERPOSE_HOOK(adventure_input_hook,feed).remove();
		INTERPOSE_HOOK(adventure_input_hook,logic).remove();
		INTERPOSE_HOOK(fort_input_hook,feed).remove();
		INTERPOSE_HOOK(fort_input_hook,logic).remove();
		reset_state();
		if(sdl_handle!=nullptr)ClosePlugin(sdl_handle);
		sdl_handle=nullptr;
		render_copy_f=nullptr;
		render_fill_rect=nullptr;
		get_render_draw_color=nullptr;
		set_render_draw_color=nullptr;
		get_ticks=nullptr;
		if(gps!=nullptr)++gps->force_full_display_count;
		}
	is_enabled=enable;
	out.print("smooth-movement: {}\n",enable?"enabled":"disabled");
	return CR_OK;
}

DFhackCExport command_result plugin_shutdown(color_ostream &out)
{
	return plugin_enable(out,false);
}
