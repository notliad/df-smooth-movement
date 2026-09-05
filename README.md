# DF Smooth Movement

A visual plugin for Dwarf Fortress that makes movement smoother.

## Features

- **Smooth movement:** creatures, hauled raw materials, and vehicles glide between tiles.
- **Sprites flip** creatures can optionally face the direction they are walking.
- **Smooth native follow:** Dwarf Fortress's unit follow glides automatically.
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
smooth-movement all on      # enable every flag except the WIP free camera
smooth-movement all off     # disable every flag except the WIP free camera
smooth-movement flip on     # enable sprites flip
smooth-movement camera on   # enable the free camera
smooth-movement linear on   # use linear easing with adaptive 150–500 ms movement tweens
smooth-movement hauled on   # show icons for carried boulders, bars, and wood
smooth-movement stats on    # time the render hook; `smooth-movement stats` prints the numbers
```

## Compatibility

Requires DFHack 53.16-r1.1 and the SDL 2D renderer. Always download the plugin version that matches your DFHack version.

## License

MIT. See [LICENSE](LICENSE).
