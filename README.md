# DF Visual Overhaul (former Smooth movement)

> [!IMPORTANT]
> This project is AI-assisted. AI tools help write and maintain the code, so expect experiments, rough edges, and rapid changes.

An experimental DFHack plugin for pushing Dwarf Fortress' visuals further without changing the game or its simulation.

It started as Smooth Movement. Now it is a place to try movement, animation, camera, and rendering ideas that are hard to test anywhere else. Some experiments will stick. Some will turn out to be a bad fit. That is the point.

If you want the original standalone Smooth Movement plugin, use the [v0.4.1 release](../../releases/tag/v0.4.1). It is the last stable release before this repository broadened its scope. Smooth Movement also has an open PR for DFHack and may eventually live there.

## Current experiments

- Smooth movement.
- Characters flip facing movement direction.
- Hauled items are displayed above their carrier.

The free camera was an earlier experiment. It has been removed from the current build while the rendering path is reworked.

## Where this is going

This is a playground for visual work in DFHack's graphical layer. Ideas include directional sprites, combat feedback, particles, camera transitions, screen shake, environmental animation, projectile effects, and creature idle animations.

Nothing here is promised to become permanent. The useful outcome is often finding the limits of the renderer before committing to a feature.

## Installation

1. Download the release archive for your operating system and DFHack version.
2. Extract it into the Dwarf Fortress and DFHack folder.
3. Confirm the plugin is in one of these locations:

   - Linux: `hack/plugins/smooth-movement.plug.so`
   - Windows: `hack/plugins/smooth-movement.plug.dll`

4. Start Dwarf Fortress through DFHack and run:

```text
load smooth-movement
enable smooth-movement
```

Plugin commands may change as the project moves beyond Smooth Movement.

## Commands

```text
smooth-movement             # show plugin status
disable smooth-movement     # disable the plugin
smooth-movement flip on     # enable directional sprite flipping
smooth-movement carry-debug # inspect the carried-item render path
```

## Compatibility

Requires DFHack 53.16-r1.1 and the SDL 2D renderer. Download the plugin build that matches your DFHack version.

## License

MIT. See [LICENSE](LICENSE).
