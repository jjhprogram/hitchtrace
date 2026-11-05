# Hitch Lab

The test game from the case studies, as an app of its own. Pick a case, press
Start, and the game runs with that case's cause of hitches around it: a
program competing for its cores, a slow shared disk, a memory limit, a late
timer, a server it calls that can't get a CPU, and so on. Then record it with
Hitchtrace Explorer like any other game.

## Build and run

Needs Qt 6, CMake, Vulkan and XCB headers (on Ubuntu 24.04:
`sudo apt install qt6-base-dev cmake ninja-build libvulkan-dev libxcb1-dev`).

```sh
make layer layer-install                 # once: the Vulkan layer that marks frames
cmake -S hitchlab -B build/hitchlab -G Ninja && cmake --build build/hitchlab
cmake -S gui -B build/gui -G Ninja && cmake --build build/gui   # the explorer, to record with
build/hitchlab/hitchlab
```

1. Pick a case on the left. The right side says what it simulates, what to
   look for in the explorer, its settings, and the exact commands it will run.
2. Press **Start**. The game opens in its own window ("Hitch Lab: KEY").
3. Press **Record with Hitchtrace Explorer…**. The explorer opens its Record
   dialog with the game already selected; record, stop, and look.
4. **Stop** ends the game and everything the case started. Picking another
   case while one runs and pressing **Switch to this case** does both.

## The cases

| Key | What it simulates |
|---|---|
| `baseline` | nothing in the way: what normal frames look like |
| `bgcpu` | a background program burns CPU on the game's cores in bursts |
| `iomain` | the main thread reads an asset from disk itself |
| `iothread` | the main thread waits for a streaming thread that reads from disk |
| `iodisk` | another program reads from the same rate-limited disk in bursts |
| `mem` | the game walks memory that a memory limit pushed out to swap (optionally with huge pages) |
| `alloc` | the game allocates while its memory limit is full: direct reclaim |
| `compact` | the game grows with huge pages while free memory is in scattered pages |
| `slack` | the game's sleeps wake late in bursts (timer slack) |
| `ipc` | the game calls a server process whose one CPU is crowded in bursts |
| `cpuspike` | the game's own extra CPU work (a control: nothing to blame) |
| `squeeze` | all of the game's threads confined to one core in bursts |

They are the set-ups of `cases/run.sh` with the same defaults, so a
recording made here can be read with the guides in `cases/pages/`.
`cases/run.sh` is unchanged and still makes the recordings in `cases/out/`.

## How it works

- **One executable.** `hitchlab --run NAME ...` runs the game (`game`) or one
  of the programs around it (`bgload`, `bgio`, `ipcserver`, `squeeze`,
  `frag`). They are built from the case-study sources in `cases/`; each
  program's `main` is renamed so they can share one binary. The timer-slack
  shim for `slack`, `libhitchlab_slack.so`, sits next to it: it is preloaded
  into the game, so it has to be a separate shared library.
- **Set-up the same way as run.sh.** CPU pinning, `systemd-run --user
  --scope` for memory limits and the shared disk slice, `LD_PRELOAD` for the
  shim. The game always runs with `HITCHTRACE=1`, so the layer is loaded and
  it can be recorded.
- **Nothing outlives the lab.** Every process it starts gets
  `PR_SET_PDEATHSIG`, so if the lab is closed or killed they end with it.
- **Files.** `iomain`, `iothread` and `iodisk` read an asset pack (256 MB),
  and `iodisk`'s other program reads a 1 GB file. They are written once to
  `~/.cache/hitchtrace/hitchlab/`, of real blocks so that reading them costs a
  disk read.
- **Machine needs.** `iodisk` needs the io cgroup controller delegated to your
  user (the lab shows the one-time root command if it isn't). `compact` uses
  nearly all of the machine's free memory and stops by itself after 30 s.

## Without clicking

```sh
hitchlab --case iodisk --screenshot lab.png   # render the window
HITCHLAB_ECHO=1 hitchlab --case bgcpu --try 10   # run a case for 10 s, print its output
```
