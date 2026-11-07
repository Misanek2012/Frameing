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

1. Open a level. Press **F3** to record and play the section you care about.
2. Set a goal: **G** = current tick, **Shift+G** = level completion, or
   **Alt+click** on the timeline.
3. Select an input on the timeline and press **F6** to sweep it. The timeline
   shows the result band and the passing window.
4. **F8** places circles at the player's position for each tested offset.

### Keys

| Key | Action |
|---|---|
| F1 | Toggle overlay |
| F2 | Toggle hitboxes |
| F3 | Record |
| F4 | Replay |
| F5 | Manual test of the selected input |
| F6 | Sweep the selected input |
| F7 | Circle tool |
| F8 | Generate circles from the last measurement |
| F9 / F10 | Pause / stop |
| `.` / `,` | Step one tick forward / back (Shift = 10) |
| ← / → | Move selected input one tick |
| M | Toggle ripple (later inputs move with the selected one) |
| G, Shift+G, Alt+click | Set goal |
| Ctrl+Z / Ctrl+Y | Undo / redo |
| Ctrl+S / Ctrl+O | Save / load project |
| 1–6, Q/E, Z/X | Circle colour, thickness, fill |
| H / Shift+H | Hide circle / show all |
| L | Link circle to the selected input |
| Del | Delete selection |

Mouse: drag markers to move inputs, drag empty timeline to pan, scroll to zoom
(Shift+scroll pans). With the circle tool, click to place, drag to move,
Shift+drag to resize.

Projects save to the mod's save folder as `projects/<levelId>-<name>.fwl`
(plain text, with a `.bak` of the previous version). The format is
locale-independent.

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
