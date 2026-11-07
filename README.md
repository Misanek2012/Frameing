# Frame Window Lab

A Geode mod for **Geometry Dash 2.2081** that measures frame windows: how many
ticks early or late an input can be while the attempt still passes. It records
inputs at tick precision, replays them deterministically, shifts one input
across a range of offsets, and reports which offsets pass, with hitboxes and
marker circles drawn on the level.

## Version requirements

| | Version |
|---|---|
| Geometry Dash | **2.2081** (2.209 shipped 2026-10-15; this mod does not support it) |
| Geode SDK | **5.11.2** (latest release, still primarily for 2.2081) |
| Bindings | pinned commit in `scripts/versions.env` |

Steam auto-updates GD. To stay on 2.2081, disable automatic updates for the
game or roll back before installing this mod.

## How ticks work

GD simulates at 240 ticks per second, independent of your frame rate. The mod
counts time in ticks (`Tick`, 1/240 s) and, while recording or testing, drives
the game one tick at a time so every input lands on an exact tick and replays
identically. A **frame window** is the set of offsets (in ticks, relative to the
recorded input) at which the attempt still reaches the goal.

Each tested offset gets one of four results:

- **Pass**: reached the goal (a tick, or level completion).
- **Fail**: died or missed the goal.
- **Untrusted**: the replay desynced from the baseline before the shifted
  input, so the result can't be attributed to the timing change.
- **Blocked**: the offset can't be tested without crossing another input on the
  same button.

Before a sweep, the mod replays the unchanged run twice and checks the two
traces match, so it only measures levels that replay deterministically.

## Using it

The timeline at the bottom works like NaN's counter: white ticks every 10
frames, yellow labelled ticks every 60, green boxes = presses, red = releases
(numbered underneath), cyan bar = current frame, blue bar = "Reset @" point.
Hover a box for `45: + Jump P1 @ 1637`. Top left shows `Frame: N, Input: M`.

1. Open a level and click the **R** button (or press **F3**; the R key is GD's restart). Play until you die; the attempt is
   recorded frame-exactly. In practice mode, dying respawns at the checkpoint
   and recording continues from there.
2. Press **A** to measure the frame window of every click. Each click gets a
   circle indicator in the level: a target at the spot where you clicked with
   the window size next to it (`4` = four frames work). The number also shows
   above the box on the timeline. **CI** (or F8) refreshes the indicators.
3. Click a box to select it, drag it (or **←/→** while paused) to move it a
   frame at a time. **T** (F5) tests the selected click at its new timing,
   Shift+A (F6) measures just that click.
4. **P** replays, **<** / **>** step one frame, click anywhere on the timeline
   to jump to that frame, drag the cyan bar to scrub.

The yellow line in the level is the player's path; small rings on it mark
where each input fires.

### Buttons and keys

| Button | Key | Action |
|---|---|---|
| CI | F8 | Refresh circle indicators |
| R | F3 | Record / stop recording |
| H | F2 | Hitboxes on/off |
| O | F7 | Circle tool (click to place, drag to size, 1-6 colour, Del removes) |
| P | F4 | Play / pause the replay |
| < / > | , / . | Step back / forward one frame (Shift = 10) |
| + | Insert / I | Add a click at the current frame (while paused) |
| T | F5 | Test the selected click at its current timing |
| A | Shift+F6 | Measure every click (Shift+A or F6: only the selected one) |
| | F9 | Pause / resume |
| | F10 | Stop |
| | F1 | Hide / show everything |
| | [ / ] | Select previous / next input |

While paused (or after clicking the timeline): ←/→ move the selected input
(Shift = 10 frames, Ctrl = move every later input too), ↑/↓ select, Del
deletes the click, G sets the goal to the current frame (Shift+G = complete the
level, Ctrl+G = automatic), M toggles ripple, F fits the whole attempt,
Home/End jump to the start / last input. Mouse wheel zooms the timeline
(Shift+wheel scrolls). Ctrl+Z/Ctrl+Y undo/redo, Ctrl+S saves.

The goal decides what "works" means: by default a timing works if the player
survives 240 frames (1 s) after the click, capped just before where your
recorded attempt died.

**Click Between Frames** moves clicks between physics frames. Turn it off
while measuring, or windows can't be reproduced exactly.

## Building

Push to GitHub; the workflow in `.github/workflows/build.yml`:

1. runs the core unit tests with libstdc++ and libc++, including a test under a
   decimal-comma locale,
2. compiles the whole mod against the generated GD 2.2081 headers and checks
   every hook against the Windows bindings (`scripts/header-check.sh`),
3. builds the Windows `.geode` with the pinned SDK and bindings. The file
   appears under the run's Artifacts.

Locally:

```sh
# core tests (any C++20 compiler, no GD needed)
cmake -S tests -B build-tests && cmake --build build-tests && ./build-tests/fwl_core_tests

# compile + hook check against 2.2081 (Linux; needs clang, libc++-dev,
# libc++abi-dev, libgles-dev, cmake, git, python3)
scripts/header-check.sh

# the mod itself (Windows, Geode CLI installed)
geode sdk install v5.11.2   # then set GEODE_SDK
cmake -B build && cmake --build build --config Release
```

### Why the hook check exists

A Geode `$modify` method whose signature differs from the binding (say
`postUpdate(double)` instead of `postUpdate(float)`) compiles fine and simply
never runs. `scripts/hook_check.py` fails the build if any hook doesn't
exactly match a binding that is hookable on Windows.

## Layout

```
src/core/   game-independent logic (input sequence, window analysis,
            project format, playback), unit tested in tests/
src/gd/     Geode hooks, session/test engine, hitboxes, sounds
src/gd/ui/  overlay, timeline and circles
scripts/    versions.env, header-check.sh, hook_check.py
```

## Not yet verified in-game

Everything above has been compiled and unit tested, but not run in GD. Still
to confirm on a real 2.2081 install: tick timing in locked mode, the injection
phase chosen by calibration, practice-mode checkpoints, and level restarts
during a sweep.
