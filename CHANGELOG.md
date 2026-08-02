# Changelog

## Unreleased

- Adventure mode support, world-slide style. Scroll landings are attributed on the
  background layer (largest applied prefix), so creatures moving during a
  camera-follow scroll animate relative to the world. The player is not interpolated:
  sprites that stay screen-static across a landing are "pinned" and the WORLD TILES
  slide instead -- the map glides to its new position with the same 100 ms smoothstep
  as creature movement, passing beneath the pinned player. No camera state, no
  persistent offsets, no window writes.
- One-frame window excursions (combat/announcement camera flicks, common near
  loaded-chunk borders) are ridden out with animations and the slide intact; scrolls
  masked by untrusted frames are absorbed after a grace period; teleports still snap.
- Mouse clicks and the hover highlight dispatch to the displayed tile while the world
  slides (scoped precise_mouse shift around the map screens' input; UI grid untouched).
- The slide's trailing edge draws departed tiles from an outgoing-tile cache (padded,
  world-anchored, chained across stacked landings) instead of rendering black while
  the world catches up.
- Middle-mouse drag pans stay direct: world-slide animation is suspended while the
  button is held and for a few frames after release (the drag's final window steps
  land late and would otherwise bounce the view back).
- Diagnostic counters and a per-frame trace (`smooth-movement trace`), including a
  visual-continuity jump detector.

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
