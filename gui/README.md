# hitchexplorer

A desktop viewer for hitchtrace recordings, in Qt 6 Widgets. It opens the
`hitch.jsonl` files `hitchtrace -o` writes and shows, for every over-budget
frame, what decided when that frame finished.

## Build and run

Needs Qt 6.4 or newer, CMake and a C++20 compiler (on Ubuntu 24.04:
`sudo apt install qt6-base-dev cmake ninja-build`).

```sh
cmake -S gui -B build/gui -G Ninja
cmake --build build/gui
build/gui/hitchexplorer cases/out/*/hitch.jsonl     # one recording per file
ctest --test-dir build/gui                         # analysis checks + GUI smoke test
```

Files can also be opened from File › Open or dropped on the window. Switch
between open recordings with the Recording box in the toolbar.

## Recording

File › Record (Ctrl+R) records a running program. It lists the programs that
have loaded the hitchtrace Vulkan layer, so start the game with
`HITCHTRACE=1` in its environment first (for a Steam game, launch options
`HITCHTRACE=1 %command%`; the layer comes from `make layer layer-install`).
Pick it, set the frame budget and how long to record, and press Record. Stop
and open ends the recording and opens it.

hitchtrace needs root. The dialog runs it through `sudo -n` when a sudo rule
lets you run it without a password, and otherwise through `pkexec`, which asks
for your password in the system's own dialog; the app never sees it. The
dialog shows the exact command before it runs. Stopping works by closing
hitchtrace's input (`hitchtrace --stop-on-eof`), since the app cannot signal a
root process. Recordings go to `~/hitchtrace/` by default, one new file each
time, created by the app so that it stays yours.

`hitchexplorer --record PID` opens straight into the Record dialog with that
process selected; Hitch Lab (`hitchlab/`) uses it to record its game.

## The window

- **Session** (docked on top): every hitch on one time axis, each bar stacked
  by what its critical path was doing. Click a bar to open that frame; drag to
  select a range; right-click or Esc clears it.
- **Critical path**: one bar the length of the frame. Walking back from the
  present: while the presenting thread waited, the bar follows whoever it was
  waiting for, and so on down the chain. The parts add up to the frame.
- **Threads**: one lane per thread of the game. The path is lit and outlined
  across lanes; other processes on it get hatched placeholder lanes, since
  hitchtrace records only their names. Click any wait to follow its own chain.
- **Blame tree** (docked right): an icicle where width is time and each row
  down is one hop of the chain, for this frame, the selected frames, all
  hitches, or the wait being followed. Click a box to zoom in; hover it to
  light its part of the path. The same tree is listed as a table below it.
- **Stacks** (docked right, under the blame tree): where the threads on the
  path were blocked, one row per thread and kernel stack, with the time it
  cost the path; expand a row for the frames, and for the presenting thread,
  the game's own frames when recorded with user stacks (`-U`). It follows the
  focus: the frame's critical path, the part picked on the ribbon, the wait
  being followed, or the blame-tree box zoomed into, over one frame, the
  selected frames or all of them. Recordings from before hitchtrace wrote
  every wait's stack show the closest stack they have, and say so.

Alt+Left / Alt+Right (or Page Up / Page Down) step through hitches. The dock
layout is remembered between runs; View › Reset layout restores it.

## Layout

```
src/data/       the recording: Run, Frame, Event; the JSONL loader
src/analysis/   critical path, explain() for one wait, blame trees (no widgets)
src/ui/         Session (shared selection state) and one widget per view
tests/          test_analysis: the analysis on cases/out, against figures
                computed independently by the HTML mockup
                gui_smoke: drives every view offscreen and saves screenshots
                to build/gui/gui-smoke/; `gui_smoke DIR --record PID` also
                records a running program through the dialog (needs root
                through sudo -n)
```

`hitchexplorer --screenshot FILE [--size WxH] [--frame ID] FILE.jsonl` renders
the window to an image and exits; with `QT_QPA_PLATFORM=offscreen` it needs
no display.

## Not done yet

- Docking beyond `QDockWidget` (Visual Studio–style tabs and splits), and a
  query engine for pivoting across many recordings.
- Placing in-kernel time (reclaim, compaction) in time, and anything about
  other processes beyond their names: the records do not have them.
