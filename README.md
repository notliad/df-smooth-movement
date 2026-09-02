# DF Smooth Movement

A visual plugin for Dwarf Fortress that makes movement smoother.

## Features

- **Smooth movement:** creatures, hauled items, and vehicles glide between tiles.
- **Synced icons:** status icons follow their creature while it moves.
- **Animated carts:** wheelbarrows and minecarts move smoothly too.
- **Sprites flip** creatures can optionally face the direction they are walking.
- **Free camera:** the camera can optionally glide and be dragged with the mouse (WIP).

## Installation

1. Download the release archive for your operating system and DFHack version.
2. Extract it into the Dwarf Fortress/DFHack folder.
3. Check that the plugin is in one of these locations:
   - Linux: `hack/plugins/smooth-movement.plug.so`
   - Windows: `hack/plugins/smooth-movement.plug.dll`
4. Start Dwarf Fortress through DFHack and run this in the console:

```text
load smooth-movement
enable smooth-movement
```

## Useful commands

```text
smooth-movement             # show plugin status
disable smooth-movement     # disable the plugin
smooth-movement flip on     # enable sprites flip
smooth-movement camera on   # enable the free camera
smooth-movement easing linear   # constant-velocity movement (see below)
```

### Movement easing

Each tile-to-tile move is tweened over a fixed duration. By default it uses a
smoothstep curve, which eases in and out -- zero velocity at both the start
and end of every hop. For a unit moving every tick this reads as a rhythmic
stutter: accelerate, arrive, stop, repeat.

`smooth-movement easing linear` switches to constant velocity across each
hop instead, so continuously moving units glide rather than stutter. It does
not smooth out the sharp turn at corners (that would need velocity carried
across hops, which this plugin doesn't do), only the dead-stop at each tile.
Switch back with `smooth-movement easing smoothstep`, the default.

## Compatibility

Requires DFHack 53.16-r1.1 and the SDL 2D renderer. Always download the plugin version that matches your DFHack version.

## License

MIT. See [LICENSE](LICENSE).
