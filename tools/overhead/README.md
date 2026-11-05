# Overhead: hitchtrace against existing tools

What does tracing cost a running game, and how does hitchtrace compare with
the tools people use today? This runs every tool on the same workloads in
shuffled order, with no-tool runs in between, and measures the slowdown from
the game's own frame log or the benchmark's own result, never from the tool
being measured.

```sh
make && make layer layer-install && make -C cases && make -C tools/overhead
tools/overhead/run.py --preset check     # each arm for a few seconds: does it work here?
tools/overhead/run.py --preset pilot     # the comparison, about 75 minutes
tools/overhead/analyze.py build/overhead/pilot-YYYYMMDD-HHMMSS
```

`run.py` asks for your password through sudo once: the tools run as root, the
game and the benchmarks run as you. The game opens a window for every run, so
leave the machine alone while it works. Results go to `build/overhead/NAME/`:
`runs.jsonl` (one line per run), `env.json`, and per-run logs; `analyze.py`
adds `summary.md` and `summary.csv`.

## The arms

| Arm | What it is | Output |
|---|---|---|
| `none` | no tool | |
| `ht`, `ht-notimeline`, `ht-stacks` | hitchtrace as recorded by default, without the per-frame timeline, and with user stacks (`-U`) | a record per over-budget frame |
| `perfsched` | `perf sched record`: every scheduler event, to a file | everything |
| `perfoffcpu` | `perf record --off-cpu --off-cpu-thresh 1`: off-CPU stacks, a sample per wait over 1 ms | per wait |
| `ftrace` | a flight recorder: `sched_switch`, `sched_waking` and the two frame marks in an overwriting ring, a snapshot when a frame runs long (uprobes, a synthetic event and a hist trigger; no BPF) | a window of events per slow frame |
| `offwaketime` | bcc `offwaketime`: blocked stacks with their wakers' stacks, summed | one summary at the end |
| `chaingraph` | this repository's wake-chain profiler | one summary at the end |
| `delayacct` | the kernel's delay accounting, read every frame through taskstats (`delayacct_frames.c`; a uprobe marks frames, no BPF) | per-thread delays per slow frame |

Two arms may not run everywhere: Ubuntu's `perf` is built without BPF
skeletons, which `--off-cpu` needs, and `offwaketime` needs kernel headers.
The runner checks for both before it starts and skips what cannot run; an
arm that fails twice during the runs is dropped too. Both are listed under
Failures.

## The workloads

| Workload | What it measures |
|---|---|
| `game60` | the game at its 60 fps cap: added time can hide in the frame limiter's sleep |
| `gamemax` | the game uncapped: added time shows as lost frames per second |
| `ipc` | the game making 30 calls per frame to a server process: many wakeups |
| `pipe-target` | `perf bench sched pipe` inside the traced process: cost per context switch where hitchtrace does the most work |
| `pipe-other` | the same in another process: what tracing one game costs everything else |
| `bg1s`, `bg10s` | a burst of background load per 1 s or 10 s (`--preset output`): output volume and capture at different hitch rates |

## What it reports

For each arm and workload, against the neighbouring no-tool runs: job phase,
frame time p50 and p99, frames per second and 1% low (game), time per round
trip (pipe); the tool's own CPU share; added machine-wide busy time; output in
MB per hour inside the measured window (for the tools that print one summary
at exit, that summary's size); drops; and slow frames caught against slow frames in the game's
log. The pipe workloads give the added time per context switch in
nanoseconds, the figure that carries over to other hardware. The pilot and
full presets end with runs under `kernel.bpf_stats_enabled` that give each BPF
program's time per run (kept apart, since the statistics cost a little
themselves).

By default the tool's own process is pinned to the game's CPUs
(`--placement shared`), as on a four-core handheld; `--placement free` lets it
run anywhere.

## Caveats

- **A virtual machine is a pilot.** Context switches are slow under a
  hypervisor, which makes percentages look smaller than on bare metal; prefer
  the nanoseconds. The paper's numbers need a bare-metal run.
- **The flight recorder's frame** runs from the return of present to the next
  present (ftrace can pair two events, not one with itself), so present's own
  wait is not in it.
- **Not equal work.** The arms answer different questions: a cheap tool that
  produces less is not simply better. The output columns are there to say what
  each one gave for its cost.
- The runner changes `kernel.bpf_stats_enabled` and `kernel.task_delayacct`
  and creates an ftrace instance, uprobe events and a synthetic event; it puts
  all of them back when it finishes or is interrupted.
