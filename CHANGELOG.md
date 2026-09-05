# Changelog

## Unreleased

- Add `smooth-movement stats [on|off|reset]`: counts frames, frames that reached the draw
  stage and engine tile repaints, and (while on) times the render hook split into movement
  detection and drawing. Off by default; the counters cost a few increments per frame.
- Skip engine repaints of tiles that have nothing to paint. With the layers a stage hides
  zeroed, most tiles of a level below the camera are all zero, and the engine paints nothing
  for them; the render hook now checks that before asking. `smooth-movement stats` reports
  how many repaints were skipped.
- Set smoothstep movement tweens to 150 ms. Add optional linear easing with
  adaptive 150–500 ms durations based on the cadence between consecutive steps
  (`smooth-movement linear on`) and icons for boulders, bars, and wood hauled by units
  (`smooth-movement hauled on`). Both flags are off by default.
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
