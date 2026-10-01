# Implemented Changes: Movement and Camera Refinements

## Scope

This document describes the unreleased work in commits `af3b4c8` through
`8f7f2ad`, relative to `origin/main` (`85b76ab`). The changes improve movement
timing, free-camera behavior, render-thread safety, scroll detection, and test
coverage without changing Dwarf Fortress gameplay or simulation state.

The plugin remains a render-only modification. It observes renderer-owned
viewport buffers, creates visual proxies for moving sprites, and redraws those
proxies at fractional positions. Dwarf Fortress continues to own the real tile
positions, camera coordinates, simulation timing, and save data.

## Purpose

The work had five main goals:

1. Make visual movement continuous at different game frame rates and for units
   that move at different cadences.
2. Make free-camera scrolling and fast middle-mouse dragging feel continuous,
   including when Dwarf Fortress updates its camera coordinates before its
   viewport buffers.
3. Keep a followed Fortress unit or the Adventure player fixed on screen while
   both the unit and camera move.
4. Prevent races between the render hook, plugin commands, disable, and unload.
5. Fail safely when renderer state is incomplete, invalid, or too ambiguous to
   animate reliably.

## User-visible changes

### Linear movement interpolation

Movement now uses linear progress:

```text
progress = min(1, elapsed_ms / duration_ms)
```

The previous smoothstep curve accelerated at the start and decelerated at the
end of every tile. Linear interpolation keeps a constant visual velocity across
the tile, which looks more continuous when consecutive steps are chained.

### FPS-aware movement duration

The fallback duration is scaled from the original 100 ms at 100 game FPS:

```text
duration_ms = round(100 ms * 100 FPS / configured_game_fps)
```

For example, 50 FPS produces a 200 ms fallback and 200 FPS produces a 50 ms
fallback. The result is clamped to at least 1 ms. Invalid, zero, negative, or
non-finite FPS values fall back to 100 ms.

Each movement stores the duration selected when it begins, so an FPS change
does not change the speed of a proxy already in flight.

### Per-visual cadence estimation

The animation manager now remembers the last movement time for each identifiable
visual. When that visual makes another unambiguous adjacent step, the observed
step interval becomes the new animation duration. This lets a fast unit and a
slow unit remain visually continuous at the same time instead of forcing every
sprite to use one global duration.

Cadence identity uses the visual layer, texture match, and source screen tile.
A cadence record survives after its animation finishes, follows recognized
camera pans, and is consumed at most once in a frame. An interval is accepted
only when it is nonzero and no more than four fallback durations old. Stale or
ambiguous records are discarded. Cadence is also reset after an FPS baseline
change, context change, or abandoned scroll because the old identity can no
longer be trusted.

### Improved free-camera edges

When a fractional camera offset exposes a strip at the edge of the map, the
default `retain` mode leaves the snapped frame drawn by Dwarf Fortress beneath
that strip. This avoids the visually harsh black border used previously.

The original behavior remains available at runtime:

```text
smooth-movement camera border retain
smooth-movement camera border black
```

The current border mode is included in the general plugin status and in
`smooth-movement camera` output. Changing it requests a full redraw.

### Smooth fast middle-mouse dragging

The rendered camera offset now composes four contributions:

```text
rendered offset = transient glide
                + drag correction
                + follow offset
                + persistent rest * tile size
```

The persistent drag component is bounded to +/-1.5 tiles per axis. Displacement
beyond that range is transferred into a pixel-space elastic correction, so a
fast drag does not jump when the bound is reached. The correction decays with a
120 ms exponential time constant while dragging.

On release, the complete rendered offset is folded into the persistent rest
position instead of starting another drift toward the tile grid. Any Dwarf
Fortress tile scrolls already waiting to reach the viewport buffers are absorbed
first. Whole tiles are normalized into `window_x` and `window_y` only after the
pending shifts land, preserving the fractional resting position throughout.

Drag calculations use the content window (`window - pending scroll debt`) rather
than the newest game camera value. This avoids a stutter during the delay between
an input-time camera update and the later buffer update.

### Screen-locked camera follow

The free camera can now keep a followed Fortress unit or the Adventure player
visually fixed on the same screen position while the world scrolls underneath.
This is implemented entirely from render buffers; the render thread does not
read gameplay unit state.

When a viewport scroll lands, movement detection looks for a center-layer visual
whose world movement exactly compensates for the landed camera shift. Every
detected movement receives a stable nonzero ID. If several visuals qualify, the
target nearest the viewport center wins; normal x/y scan order provides a
deterministic tie-break.

The animation manager exposes the inverse of the chosen proxy's remaining
displacement:

```text
follow offset = (target - source) * (1 - progress)
```

Adding that offset to the camera origin cancels the proxy's interpolation, so
the followed visual remains screen-locked to sub-pixel precision. Consecutive
steps receive new IDs and can retarget from an already fractional source without
introducing a discontinuity.

If no trustworthy visual anchor exists, the camera retains the exponential
fallback glide with a 35 ms time constant. Existing transient motion is decayed
before a newly landed scroll is added, so the new event starts at its complete
offset. Jumps larger than three tiles, view/context changes, and abandoned
scrolls still snap safely.

Starting a middle-mouse drag includes any active follow offset in the drag
baseline before clearing the follow anchor. Switching from camera follow to
manual dragging therefore does not move the rendered view.

## Technical implementation

### One authoritative scroll detector

Scroll landing is now owned by `visual_animation_managerst` in
`visual_animation.h`. Previously, camera code and sprite interpolation had
separate matching logic, which could disagree about when a pan reached the
buffers. The manager now returns a `visual_scroll_renderst` event containing:

- whether scroll debt is still pending;
- whether a shift landed or was abandoned this frame;
- the landed and remaining x/y deltas; and
- the optional movement ID selected as a follow anchor.

`smooth-movement.cpp` consumes this event for camera bookkeeping. Sprite
translation, movement detection, drag coordinates, and follow selection now use
the same observed landing.

### Dense terrain matching with a sparse fallback

Viewport animation input now optionally includes the current and previous
background buffers. Dense terrain is the preferred evidence for a landed shift
and must reach a 0.6 match ratio. This is important in Adventure follow mode,
where the player can remain on the same screen tile and the creature layer alone
does not reveal that both player and camera moved.

If background buffers are unavailable, the manager retains the dependency-free
sparse visual-layer matcher with a 0.5 threshold. Layers whose texture matching
accepts any nonzero previous value do not vote in shift detection because they
do not provide reliable positional identity.

The manager hashes the relevant current and previous buffers with FNV-1a to
distinguish a newly advanced viewport from a repeated render of the same data.
This prevents one landed shift from being processed repeatedly while paused or
between viewport recomputes.

### Ordered scroll debt and partial landings

Pending camera changes are stored as an ordered event queue instead of one
signed total. This preserves information when the user reverses direction before
the first shift reaches the buffers.

For each redraw, the manager tests ordered prefixes and partial x/y components
of the next event. It can therefore handle:

- one large announced scroll landing over several buffer updates;
- multiple announced scrolls coalescing into one update;
- reversing scrolls without canceling unobserved history;
- simultaneous x/y movement; and
- uniform terrain where a shift can be visually equivalent before a buffer swap.

The strongest valid match wins. Equal scores prefer the shortest applied
prefix, preventing the detector from consuming more debt than the buffers prove.
When a shift lands, in-flight proxy sources and targets, cadence positions, and
the per-tile facing grid are translated by the same delta. Previous visual
layers are temporarily rebased on the landing frame so a unit movement hidden
by the simultaneous camera scroll can still be detected.

Safety limits bound uncertain state: at most eight pending events, no more than
six tiles of net debt on either axis, 120 unchanged frames waiting for a landing,
and four failed changed-buffer matches. Exceeding a limit abandons the pending
animation state and falls back to a snap. Empty terrain with no evidence is also
abandoned safely.

### Normalization-scroll attribution

Free-camera rest normalization writes whole-tile changes into `window_x` and
`window_y`. Those plugin-generated writes are visual no-ops and must not be
mistaken for gameplay camera movement. Each landed axis is split into a
same-direction self component and a remaining gameplay component. Self movement
is folded back into rest before follow-anchor selection; mixed or opposite
scrolls retain only the portion that can be attributed safely.

### Render-thread ownership and lifecycle safety

All mutable render state is now owned by the render thread. Plugin commands and
lifecycle callbacks use synchronous render-thread transactions built from
`DFHack::runOnRenderThread`, a packaged task/future, and a mutex that serializes
only these infrequent transactions. The render hot path takes no lock.

The first transaction records the render thread ID. Later transactions and the
`update_all` hook verify that the same thread still owns the state.

Enable now performs reset, SDL binding installation, and renderer-hook
installation on the render thread. If hook installation fails, the bindings are
cleared before returning failure. Disable and shutdown remove the hook before
destroying animation state or clearing SDL function pointers, then request a
full display redraw. `is_enabled` is changed only after the complete transaction
succeeds.

This ordering prevents an in-flight `update_all` call from observing a removed
`INTERPOSE_NEXT` target, cleared SDL functions, or destroyed animation state.
Camera commands, sprite-flip commands, and status snapshots use the same
ownership boundary.

The plugin command is registered as core-unlocked because it can synchronously
wait for render-thread work, and parsing and output need no core access.

Two rules govern when a transaction may wait for the render thread, and both
follow from one fact: `runOnRenderThread` merely appends to a queue that DFHack
drains from `dfhooks_sdl_loop`, on DF's main/render thread, once per frame just
before the screen buffer is drawn -- and only after DF's simulation thread has
finished producing that frame, because the render thread spends the simulation
phase parked in `enablerst::async_wait()`. The render thread can service a
callback only while the simulation thread is free to run.

First, a transaction must not wait while holding DFHack's core suspension. DF's
simulation thread owns the core for the whole of `Core::Update`, so waiting there
deadlocks unconditionally. Every `enable smooth-movement` arrives at
`plugin_enable` with the core suspended, and the core-unlocked command flag only
prevents DFHack adding a further suspension -- a command invoked from lua still
runs on the simulation thread, which already holds one. So transactions check
`Core::isSuspended()` and run inline on the calling thread when it is set. The
frame ordering that causes the deadlock is what makes that safe: while the core
is suspended the render thread is blocked before its render phase, so
`update_all` is neither running nor able to start, and the state has no other
reader.

Second, a transaction must not hold the transaction mutex while waiting. A waiter
that holds it can be joined by an inline transaction on the simulation thread,
which then blocks on the mutex; a blocked simulation thread never lets the render
thread reach the drain the waiter is waiting for. The queued task therefore takes
the mutex itself, on the render thread, which keeps it mutually exclusive with
inline transactions without ever placing it on a blocking path.

Because the render thread identifies itself by running a transaction, a plugin
enabled entirely through the inline path reaches its first frame with no owner
recorded, so `update_all` claims ownership there. The lock is taken once, on
that first frame.

### Renderer-state validation and checked arithmetic

The render path now validates each active viewport before use. Validation
requires positive dimensions, a dimension product representable by signed tile
indices, valid clip bounds, and every buffer required by the redraw passes.
Unreadable main viewports clear stale coverage and transient camera state rather
than attempting a partial render.

Potentially unsafe calculations were hardened:

- tile size and tile-to-pixel conversion use wider intermediate arithmetic;
- pixel positions, spans, and temporary renderer-origin additions saturate to
  representable ranges;
- camera offsets must be finite and safely roundable to `int32_t` pixels;
- explicit camera offsets reject NaN, infinity, and values outside
  `-0.99..0.99` tiles; and
- rest normalization validates finite/range-safe values before writing game
  camera coordinates.

## Tests and validation

`smooth-movement-test` is no longer excluded from the default build and is
registered with CTest. The expanded unit suite covers:

- exact linear progress and FPS scaling;
- per-visual cadence, expiry, chaining, ambiguity, pan translation, and reset;
- finite camera inputs and overflowing viewport dimensions;
- elastic drag bounds, decay, release persistence, and follow-to-drag handoff;
- screen-lock cancellation and sub-pixel residuals;
- consecutive and deterministic follow anchors;
- fallback-glide timing;
- partial, coalesced, diagonal, and reversing scrolls;
- uniform and empty terrain;
- scroll queue, debt, retry, and age limits; and
- normalization-scroll attribution.

The new `test_lifecycle_stress.lua` script exercises the live DFHack lifecycle.
By default it performs 100 enable/disable cycles followed by 100
enable/unload/load cycles while Dwarf Fortress continues rendering, then leaves
the plugin enabled.

Typical local unit-test commands are:

```sh
cmake --build build/dfhack-53.16-r1.1 --target smooth-movement-test
ctest --test-dir build/dfhack-53.16-r1.1 --output-on-failure \
  -R '^smooth-movement-test$'
```

The in-game stress script can be run from DFHack with:

```text
script test_lifecycle_stress.lua [cycles]
```

## Files changed

- `visual_animation.h`: movement timing, cadence state, authoritative scroll
  detection, movement IDs, follow offsets, drag helpers, and safety validation.
- `smooth-movement.cpp`: render-thread transactions, safe lifecycle ordering,
  camera composition, border modes, checked rendering arithmetic, and integration
  with manager-owned scroll events.
- `test_visual_animation_manager.cpp`: expanded unit and regression coverage.
- `test_lifecycle_stress.lua`: live enable/disable and unload/load stress test.
- `CMakeLists.txt`: builds the unit executable by default and registers it with
  CTest.
- `README.md`: documents camera edge modes and updated drag behavior.
- `CHANGELOG.md`: summarizes all user-facing behavior in the unreleased section.
