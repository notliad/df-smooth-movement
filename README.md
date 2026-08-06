# DF Smooth Movement

A visual plugin for Dwarf Fortress that makes movement smoother.

## Features

- **Smooth movement:** creatures, hauled items, and vehicles glide between tiles.
- **Synced icons:** status icons follow their creature while it moves.
- **Animated carts:** wheelbarrows and minecarts move smoothly too.
- **Sprites flip** creatures can optionally face the direction they are walking.
- **Free camera:** the camera can optionally glide and be dragged with the mouse (WIP).
- **World slide:** in adventure mode the world can glide under the adventurer
  instead of the adventurer snapping between tiles.

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
smooth-movement slide on    # enable the adventure-mode world slide
```

## Compatibility

Requires DFHack 53.16-r1 or newer and the SDL 2D renderer. Always download the plugin version that matches your DFHack version.

## Adventure mode

The camera follows the adventurer, so every step is a map scroll. Instead of
interpolating the player, the plugin holds it where it is and slides the WORLD
beneath it with the same 100 ms smoothstep -- a step reads as the map moving
rather than the character snapping a tile ahead of it. Other creatures still
animate normally across the scroll. Tiles that leave the viewport mid-slide are
drawn from a small cache instead of flashing black, and mouse clicks land on the
tile shown under the cursor while the world is moving.

Off by default -- the offset repaint costs a full extra map render per animated
frame. Turn it on with `smooth-movement slide on`.

## Free camera

`smooth-movement camera on` unbinds the view from the tile grid. It is
render-only: the game's own tile camera is never written to.

Map scrolls glide instead of stepping, and `smooth-movement camera <fx> <fy>`
parks the view at a persistent sub-tile offset (positive = east/south, up to
±0.99 tiles); `smooth-movement camera reset` returns to the grid. While the view
rests off-grid a sub-tile strip at one screen edge has no viewport data and
renders black. Middle-mouse drag panning is native: the glide compensates each
scroll on the frame its content lands in the render buffers, so it neither
fights DF's own drag stepping nor strands the view off-grid on release.

## License

MIT. See [LICENSE](LICENSE).
