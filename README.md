# Stratum

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
   - Linux: `hack/plugins/stratum.plug.so`
   - Windows: `hack/plugins/stratum.plug.dll`
4. Start Dwarf Fortress through DFHack and run this in the console:

```text
load stratum
enable stratum
```

## Useful commands

```text
stratum             # show plugin status
disable stratum     # disable the plugin
stratum flip on     # enable sprites flip
stratum camera on   # enable the free camera
```

## Compatibility

Requires DFHack 53.16-r1.1 and the SDL 2D renderer. Always download the plugin version that matches your DFHack version.

## License

MIT. See [LICENSE](LICENSE).
