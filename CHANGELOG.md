# Changelog

## Unreleased (adventure-slide)

- The offset-repaint smoothers are OFF by default: gliding requires repainting the
  whole map rect every frame the view sits between tiles -- a full extra map render
  that is too heavy at fortress zoom levels. `smooth-movement slide on` (adventure
  world slide) and `smooth-movement camera on` (fortress free camera) opt in. The
  default experience is the cheap part: creature/NPC interpolation everywhere,
  including across adventure camera-follow scrolls.

- Retire the pixel-anchored middle-mouse drag: DF's native drag pans with its own
  stepping and rate, so the anchor inevitably diverged (visible lurching every dragged
  tile, and a camera stranded exactly half a tile off-grid on release, which also
  displaced every click correction -- misplaced dig designations). Middle-drag is now
  fully native and crisp; the camera glide covers scrolls and recenters, and manual
  sub-tile offsets remain via `camera <fx> <fy>`.

- Adventure mode support, world-slide style. Scroll landings are attributed on the
  background layer (largest applied prefix), so creatures moving during a
  camera-follow scroll animate relative to the world. The player is not interpolated:
  sprites that stay screen-static across a landing are "pinned" and the WORLD TILES
  slide instead -- the map glides to its new position with the creature-movement
  smoothstep, passing beneath the pinned player.
- The slide's trailing edge draws departed tiles from an outgoing-tile cache instead
  of rendering black; middle-mouse drag pans suspend the slide; ground items no
  longer hop behind walkers (un-occlusion gating at both detection and inheritance).
- One-frame window excursions are ridden out; masked/phantom scrolls are absorbed;
  clicks dispatch to the displayed tile mid-slide; diagnostics (`trace`, visual-jump
  detector, `slidems`).
- The fortress-mode free camera (`camera on|off|reset|<fx> <fy>|speed <ms>`) is
  available again: glide + pixel-perfect middle-drag + sub-tile rest, fortress mode
  only (adventure mode uses the world slide).

## Unreleased

- Target DFHack `develop` and reuse DFHack's SDL library handle instead of
  independently opening and closing SDL.
- Centralize viewport layer metadata, redraw stages, SDL bindings, and pending
  state cleanup to remove duplicated rendering policy.
- Animate creature status icons with their creature instead of letting their
  flashing texture fragments jump between tiles.
- Animate item-layer wheelbarrows and the vehicle layer used by minecarts;
  minecart sprite changes no longer interrupt interpolation, and consecutive
  steps retarget from the current fractional position instead of snapping back
  to the previous tile center.
- Optional free camera (`smooth-movement camera on`, off by default): map scrolls
  glide with an exponential catch-up, middle-mouse drag pans pixel-perfectly and
  can rest between tiles, and `camera <fx> <fy>` sets a persistent sub-tile
  offset. Render-only; the game's tile camera is untouched.
- Fix sprites floating while the camera pans. The scroll variables change at input time but the
  viewport buffers shift on a later render frame, where the shift used to read as a real creature
  move and started a bogus slide across the screen. The buffer shift is now detected directly
  (hypothesis-tested against the pending scroll delta): new-movement detection is suppressed while
  a pan is pending, and in-flight movements are translated on the frame the shift lands so they
  keep tracking the world. Zoom, Z-level, resize, and viewport changes still reset.

## 0.2.0 - 2026-07-28

- Add a Windows x86-64 build for DFHack 53.15-r2.
- Load SDL2 by its platform-specific library name.

## 0.1.0 - 2026-07-28

- Add smooth visual interpolation for adjacent creature movement.
- Preserve world layer ordering and render UI after animated creatures.
- Reset interpolation on camera, zoom, Z-level, resize, or viewport changes.
- Keep gameplay, simulation timing, and save data unchanged.
