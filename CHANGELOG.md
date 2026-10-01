# Changelog

## Unreleased

- Fix two deadlocks in the render-thread transaction path, either of which froze
  Dwarf Fortress with no output. A transaction no longer waits for the render
  thread while holding DFHack's core suspension, and no longer holds the
  transaction mutex while waiting; the render thread can only drain its callback
  queue once DF's simulation thread is free, so both cases stranded the very
  drain they were waiting for. Enabling the plugin from `onMapLoad.init` froze
  the game on the way into a fort, and a `smooth-movement camera on` issued from
  lua froze it the same way.
- Keep a followed unit (including the Adventure player) screen-locked while the
  camera glides by sharing scroll detection with unit interpolation and deriving
  the camera offset from the compensating visual movement.
- Replace the hard correction during fast middle-mouse dragging with an elastic
  offset while the button is held. Releasing preserves the current fractional
  camera position instead of continuing to drift toward the tile grid.
- Retain the snapped camera frame under edges exposed by sub-tile camera movement
  instead of clearing them to black. The previous black-border rendering remains
  available with `smooth-movement camera border black`; use `retain` to switch back.
- Scale fallback movement interpolation duration with the configured game FPS
  and infer each moving visual's cadence from consecutive steps, keeping units
  with different movement speeds continuous.
- Mirror creature sprites horizontally so they face their direction of travel.
  Dwarf Fortress creature art natively faces west, so only creatures moving
  east are mirrored. Facing is sticky: only horizontal movement changes it,
  so walking north or south, and standing still, keep the last facing. Worn
  clothing and equipment flip with the creature because Dwarf Fortress
  composites them into a single tile sprite. Multi-tile creatures mirror as
  one composite, reflected about their anchor tile. Items, vehicles, and
  designations are never mirrored. Off by default; turn it on with
  `smooth-movement flip on`.

## 0.3.0 - 2026-08-03

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
