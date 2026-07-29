# DF Smooth Movement

Smooth, render-only creature movement for Dwarf Fortress through DFHack.

The plugin interpolates matching creature sprites between adjacent tiles. It
does not change unit positions, pathfinding, simulation ticks, saves, or
gameplay state.

## Compatibility

The current prebuilt release supports:

- Dwarf Fortress 53.15
- DFHack 53.15-r2
- Linux or Windows x86-64
- the SDL 2D renderer

DFHack C++ plugins are ABI-specific. Do not install the prebuilt binary on a
different DFHack or Dwarf Fortress version. Rebuild from source instead.

## Install

1. Download the release archive for your operating system and exact DFHack
   version.
2. Extract it into the Dwarf Fortress installation directory. The resulting
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

The status command prints the plugin version and whether it is enabled.

## Build

Check out the matching DFHack source release and clone this repository under
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
- Camera panning is followed: in-flight interpolations are translated by the
  scroll delta so sprites track the world, and drop once they scroll off-screen.
- Zoom, Z-level changes, resize, and viewport changes reset interpolation for one
  frame.
- Ambiguous movements between identical sprites snap to the destination.
- Main creature sprites crossing fire snap instead of being replayed over an
  unsafe layer reconstruction.
- UI and menus are rendered after interpolated world sprites.

## Scope

This is a visual renderer plugin. It does not serialize animation state or
read or modify gameplay units, jobs, movement paths, or saves.

## License

MIT. See [LICENSE](LICENSE).
