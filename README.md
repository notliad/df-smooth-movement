# DF Smooth Movement

Smooth, render-only creature, hauled-item, and vehicle movement for Dwarf Fortress through DFHack.

The plugin interpolates matching creature, moving-item, and vehicle sprites
between adjacent tiles. Creature status icons follow the same interpolation as
their creature. It does not change unit positions, pathfinding, simulation
ticks, saves, or gameplay state.

## Compatibility

v0.3.0 requires DFHack 53.16-r1 or newer and the SDL 2D renderer. DFHack C++
plugins are ABI-specific, so rebuild the plugin whenever the DFHack or Dwarf
Fortress version changes.

## Install

1. Download the release archive for your operating system and exact DFHack
   version.
2. Extract it into the DFHack folder for a Steam DFHack installation, or into
   the Dwarf Fortress folder for a manual DFHack installation. The resulting
   plugin must be:
   - Linux: `hack/plugins/smooth-movement.plug.so`
   - Windows: `hack/plugins/smooth-movement.plug.dll`
3. Start Dwarf Fortress through DFHack.
4. Run these commands in the DFHack console:

```text
load smooth-movement
enable smooth-movement
```

Check or disable the plugin with:

```text
smooth-movement
disable smooth-movement
```

The status command prints the plugin version, whether it is enabled, the free
camera state, and whether sprite flipping is on.

## Build

Check out DFHack's `develop` branch and clone this repository under
`plugins/external/df-smooth-movement`. Add this line to
`plugins/external/CMakeLists.txt`:

```cmake
add_subdirectory(df-smooth-movement)
```

Configure DFHack normally, then build:

```sh
cmake --build /path/to/dfhack-build --target smooth-movement
```

Windows builds require the MSVC 2022 toolchain used by DFHack. From Linux, the
official DFHack Docker build environment can cross-compile the plugin with
`build/build-win64-from-linux.sh`.

The plugin uses a vmethod interpose, so its CMake target links to DFHack's Lua
library as required by the DFHack build system.

The animation manager has a dependency-free test target:

```sh
cmake --build /path/to/dfhack-build --target smooth-movement-test
/path/to/dfhack-build/plugins/external/df-smooth-movement/smooth-movement-test
```

## Behavior

- Movement uses a 100 ms smoothstep interpolation.
- Creature status icons move with their creature, including while flashing.
- Creatures visible above or below the camera z-level animate in their own render layer.
- Item-layer wheelbarrows and vehicle-layer minecarts are interpolated.
- Camera panning is followed: in-flight interpolations are translated by the
  scroll delta so sprites track the world, and drop once they scroll off-screen.
- Zoom, Z-level changes, resize, and viewport changes reset interpolation for one
  frame.

- Ambiguous movements between identical sprites snap to the destination.
- Main creature sprites crossing fire snap instead of being replayed over an
  unsafe layer reconstruction.
- UI and menus are rendered after interpolated world sprites.

## Directional sprite flipping (off by default)

Creature sprites mirror horizontally to face their direction of travel.
Dwarf Fortress creature art natively faces west; a creature moving east is
mirrored, a creature moving west is left alone. Facing is sticky: only
horizontal movement changes it, so walking north or south, and standing
still, keep the last horizontal facing. Worn clothing, armour, and weapons
flip with the creature automatically because Dwarf Fortress composites them
into a single tile sprite before the plugin sees it. Multi-tile creatures
mirror as one composite, reflected about their anchor tile. Items, vehicles,
and designations are never mirrored.

Turn it on with `smooth-movement flip on`; `smooth-movement flip` alone
prints the current state. Turning it off restores every creature to its
native orientation immediately and lets the plugin go back to idling rather
than repainting tiles every frame for a feature that is switched off.

## Free camera (optional, off by default)

`smooth-movement camera on` unbinds the camera from the tile grid — render-only,
the game's own tile camera (`window_x`/`window_y`) is untouched:

- Map scrolls glide: the view exponentially catches up to the new position
  (~100 ms) instead of stepping, drawing the world at sub-tile pixel offsets.
  Jumps larger than 3 tiles (recenter, minimap) snap instantly.
- Pixel-perfect middle-mouse drag panning: the view follows the mouse 1:1 and
  rests wherever it is released — including half-way between tiles.
- `smooth-movement camera <fx> <fy>` sets a persistent sub-tile offset directly
  (positive = view east/south of the grid, up to ±0.99 tiles);
  `smooth-movement camera reset` returns to the grid; `smooth-movement camera`
  prints the state. Setting an offset implies `camera on`.
- While the view rests off-grid, a sub-tile strip at one screen edge has no
  viewport data and renders black, and the map rect is repainted every frame.
  Whole tiles of offset are folded into the game camera automatically so the
  strip never exceeds half a tile.

Plain `enable smooth-movement` never activates the free camera; the toggle also
resets to off whenever the plugin is re-enabled.

## Scope

This is a visual renderer plugin. It does not serialize animation state or
read or modify gameplay units, jobs, movement paths, or saves.

## License

MIT. See [LICENSE](LICENSE).
