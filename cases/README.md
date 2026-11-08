# hitchtrace case studies

Each case reproduces the *mechanism* behind a documented Linux gaming stutter (see
[`../docs/real-world-cases.md`](../docs/real-world-cases.md)) around one unmodified workload, and asks
whether a hitchtrace record alone explains the hitch. The summary across cases is in
[`../docs/evaluation.md`](../docs/evaluation.md#case-studies-casesreadmemd); what the records still
cannot say is in [`../docs/todo.md`](../docs/todo.md). Results below are from the current design
unless a case says otherwise; earlier versions are in [`../docs/history.md`](../docs/history.md).

## Setup

- **`minigame/`**: a small game-shaped Vulkan program with a main thread (`mg_main`: frame limiter,
  simulation, job submission, upload, present) and a worker pool (`mg_worker*`) that draws the frame in
  software. It has **no stall injectors**. It also logs its own per-frame phase timings
  (`frames.jsonl`), which is what an engine's built-in profiler would show.
- **The environment** supplies the cause. `bgload.c` is a background application that wakes
  periodically and burns CPU. It logs every burst, and that log is the ground truth.
  `bgio.c` is its disk counterpart: it reads or writes a scratch file in bursts (case 2b).
  `squeeze.c` confines the game's threads to one core in bursts (case 7), and `frag.c` leaves the
  machine's free memory in single pages (case 9).
- **`run.sh VARIANT [SECONDS] [RUN_NAME]`** runs the game on CPUs 0–3, attaches hitchtrace through the
  Vulkan layer, and writes everything to `out/RUN_NAME/`.

```sh
make && make layer layer-install && make -C cases
cases/run.sh bgcpu 60 bgcpu-confirm
python3 cases/analyze_bgcpu.py cases/out/bgcpu-confirm
tools/hitchview.py cases/out/bgcpu-confirm/hitch.jsonl    # writes hitch.html: one page, every hitch
cases/make_pages.py                    # one page per case, with its reading guide (cases/pages/)
```

`tools/hitchview.py` turns any hitchtrace JSONL file into a self-contained HTML page: every hitch of
the run on a strip, and for the selected frame its **timeline** (one lane per thread, every off-CPU
interval at its real time, an arrow from whichever thread ended a wait), where its thread's time
went, each stall group with its wakeup chain, and the other threads' totals on the same scale.

## How to read the results

"Stall time" is a ranking replayed on the same records: a wait on a thread that was itself running
does not count, and neither does a timer sleep. No case produced a false explanation in its noise
frames, with one qualification in case 5.

All results below are from the development VM (Hyper-V, 10 vCPUs, lavapipe software Vulkan). Its
timing is noisy: with nothing injected, 2–6% of frames run long, and the noise level drifts between
runs. So every comparison is made **between frames of the same run**, using the ground-truth log.
Don't compare separate runs. An interleaved A/B test (3 × 12 s each) found no measurable slowdown
from tracing: the job phase took a median 9.8 ms traced vs 10.2 ms untraced.

## Case 1: a background application takes the game's cores (`bgcpu`)

**What it models.** `real-world-cases.md` categories A and E2, and the "lock-holder preemption" and "periodic
background process" examples: a launcher, web helper or capture tool competing with the game for CPU.
The game runs with 3 workers on 4 CPUs and about 10 ms of work per 16.7 ms frame. Every 700 ms
`bgload` wakes 4 threads on the same CPUs, each burning 20 ms. A hitch is a frame over 20.8 ms
(25% over the 60 fps cap).

**The effect**, in the game's own log (`out/bgcpu-confirm`, 60 s):

| | Quiet frames | Frames during a burst |
|---|---|---|
| Frames | 3,317 | 193 |
| Over budget | 51 (2%) | 81 (42%) |
| Game's own profiler: job phase, median | 9.1 ms | 15.2 ms |

The game's own profiler shows *that* the job phase got slower, with no reason.

**Result** (`out/bgcpu-fixed`, 60 s: 183 burst frames, 77 over budget; the same with stall groups in
`out/bgcpu-groups`):

| From one record | Burst frames (77) | Noise frames (54) |
|---|---|---|
| Share of the waking worker's real delay credited by the chain, median | **100%** | |
| Time the chain's first hop explains, median | 7.6 ms | |
| First hop says "preempted" and explains ≥ half the overrun | 74 (96%) | |
| First hop names `bgload` as the preemptor | **69 (90%)** | |
| `bgload` named on a thread line | 74 (96%) | |
| `bgload` named anywhere | | **0** |

The first version credited a median 40% of the worker's delay and could not name `bgload` on the
chain at all ([history](../docs/history.md)). A record now:

```
hitch frame 3119  28.9 ms  (budget 20.8 ms, over by 8.1 ms)  root mg_main
  blocked   28.2 ms  futex 21.4 | timer 6.6 | present 0.2
  resolved   waiting on a running task 10.9 ms, inherited stalls 10.7 ms
  stalls     off-CPU time by cause and by who ended it, largest first
     21.4 ms  futex    blocked in __futex_wait
              <- mg_worker2   kept off the CPU 10.6 ms, last preempted by bgload0
      6.6 ms  timer    blocked in do_nanosleep
              <- [hardirq]    woken by an interrupt
      0.4 ms  in 3 smaller groups
```

The remaining ~10 ms of "waiting on a running task" is correct: it is the workers' normal drawing
time, present in every frame.

**Still open.**

- The chain names the **last** preemptor in the window, not the one that took the most time. In 10% of
  burst frames that was another game thread or a driver thread, because once `bgload` takes cores the
  game's own threads also start preempting each other.
- One waker per wait: the main thread waits for all three workers, and the chain follows the one that
  finished last.
- The header's "% kernel-observable" counts the frame limiter's sleep and the normal wait for the
  workers as stall time (327% in a typical record), so it is not usable as it stands.
- In the first, noisier run (`out/bgcpu`) the top preemptor in the *noise* frames was `llvmpipe-N`,
  threads of another process (most likely the compositor), printed by thread name only.

## Case 2: a frame waits on storage (`iomain`, `iothread`)

**What it models.** `real-world-cases.md` category C and the "synchronous asset read" example: every 30th frame
the game needs an 8 MB asset from a pack file before it can draw, as at an area transition. Two
variants, the two ways games end up waiting on storage:

- **`iomain`**: the main thread reads the asset itself, 1 MB at a time.
- **`iothread`**: the main thread asks a streaming thread (`mg_stream`) for it and waits.

The pack is read with `O_DIRECT`. A real game does not do that; it stands in for assets that are not
yet in the page cache, without needing root to drop caches. Ground truth is the game's own frame log:
which frames loaded an asset and how long the game spent on it.

**Contention needs a throttled disk.** The documented case (C1) is a *download* saturating the disk.
On this VM a background writer (`bgio`, 8 threads writing 1 MB blocks with `O_DIRECT`) made no
reliable difference: an 8 MB load took a median 7.3 ms without it and 8.7 ms during its bursts, inside
the run-to-run spread. The virtual disk sits on fast host storage. These two variants therefore
reproduce the wait itself; the contended disk is `iodisk`, below, which needs a one-time root step.

**Result** (60 s each, `HITCHTRACE_ARGS=-U`, hitchtrace at 4ca8a3b):

| | `iomain` | `iothread` |
|---|---|---|
| Frames that loaded an asset | 117 | 117 |
| Of those, over budget | 89 (76%) | 91 (78%) |
| Game's own profiler: asset phase in load frames, median | 16.2 ms | 14.4 ms |
| Other frames over budget (noise) | 76 of 3,391 | 74 of 3,380 |

Scoring each over-budget record against the ground truth:

| `iomain`: from one record | Load frames (89) | Noise frames (77) |
|---|---|---|
| The record has an I/O group whose chain ends at a device interrupt | **89 (100%)** | 0 |
| That group's time / the game's own load time, median | **91%**, in 8 waits | |
| I/O is the **top-ranked** group | 63 (71%) | 0 |
| I/O would be top under the old rule (longest single wait) | 62 (70%) | 0 |

| `iothread`: from one record | Load frames (91) | Noise frames (75) |
|---|---|---|
| Some group reads "waited for `mg_stream`, which was blocked on I/O, ended by an interrupt" | **91 (100%)** | 0 |
| The chain's first hop / the game's own load time, median | **94%** | |
| Root's "resolved" line calls ≥ half the overrun an inherited stall | 91 (100%) | 18 (24%) |
| That group is **top-ranked** | 60 (66%) | 0 |

So the record explains every one of these hitches, with the right amount, and never invents an I/O
stall in a frame that had none. With `-U` it also names the game's function:
`pread <- load_asset <- main`. A record from each variant:

```
hitch frame 809  33.2 ms  (budget 20.8 ms, over by 12.3 ms)  root mg_main          [iomain]
  blocked   30.5 ms  futex 8.4 | io 15.8 | timer 6.1 | present 0.2
  stalls     off-CPU time by cause and by who ended it, largest first
     15.8 ms  io       blocked in blk_io_schedule, 8 times, longest 11.2 ms
              <- [softirq]         woken from a softirq
      8.4 ms  futex    blocked in __futex_wait
              <- mg_worker2        ...

hitch frame 1619  31.6 ms  (budget 20.8 ms, over by 10.7 ms)  root mg_main         [iothread]
  blocked   29.4 ms  futex 23.7 | timer 5.5 | present 0.2
  resolved   waiting on a running task 10.1 ms, inherited stalls 13.8 ms
  stalls     off-CPU time by cause and by who ended it, largest first
     14.5 ms  futex    blocked in __futex_wait
              <- mg_stream         blocked 13.8 ms in blk_io_schedule
                <- [softirq]       woken from a softirq
      9.2 ms  futex    blocked in __futex_wait
              <- mg_worker0        was running (waiting for work) 0.0 ms
```

In the second record the buckets alone say "futex 23.7 ms". The groups split that into a wait on the
streamer, which was on disk, and the ordinary wait for the workers, which were running. That split is
what the chain is for.

**Finding: the ranking is polluted by the frame's normal waits.** The I/O explanation is in every
record but on top in only 66–71% of them. Every frame of this game, good or bad, has about 10 ms of
waiting for its workers and about 6 ms of frame-limiter sleep. When a load is short, those rank above
it. In the misses the I/O was a median 6.4 ms against a 9.8 ms worker wait. Ranking by total time
answers "where did the off-CPU time go", not "what was different about this frame".

Two other rules, replayed on the same records by `analyze_io.py`:

| I/O (or the wait on `mg_stream`) is top-ranked | `iomain` (89) | `iothread` (91) |
|---|---|---|
| By total time (current) | 63 (71%) | 60 (66%) |
| By stall time: a task-ended wait counts only its inherited part | 78 (88%) | 74 (81%) |
| By excess over a usual frame (per-cause median of the noise records) | 89 (100%) | not computable per cause |

Dropping "waiting on a running task" from the ranking helps but leaves the limiter sleep in the way.
Ranking by what the frame has **more of than usual** gets every one. It needs a baseline of normal
frames, which hitchtrace does not keep: normal frames emit nothing and leave nothing behind. A
per-cause baseline is not enough for `iothread`, where the wait on the streamer and the wait on the
workers are both futex waits; the baseline has to be per group. The same baseline would also fix the
"% kernel-observable" header and the share gate, which count these normal waits as stall time.

**Other observations.**

- **Grouping made no difference here** (71% against 70% under the old rule). The load is 8 waits, but
  one of them is usually long (a median longest of over half the group), so the longest single wait
  was already I/O. Grouping matters when no single wait stands out, as in `thousand_cuts`.
- **The chain ends at the device interrupt.** hitchtrace says the thread waited on storage and for how
  long. It cannot say why the storage was slow or who else was using it, which is exactly what the
  download case (C1) needs. `iodisk` below confirms it.
- **No measurable tracing overhead on the load**: interleaved A/B, 3 × 12 s per arm, 48 loads each:
  a median 7.8 ms untraced, 8.7 ms traced, 9.1 ms traced with `-U`, with a 90th percentile of 18–19 ms
  in all three. The 14–16 ms medians in the two case runs are this VM's disk being slower at the time.
- A waker delayed 0.2 ms in an 8.4 ms wait is printed as "kept off the CPU 0.2 ms". True, but the
  wording gives it more weight than the 8.2 ms it spent running.

## Case 2b: another program is reading from the same slow disk (`iodisk`)

**What it models.** `real-world-cases.md` C1: a launcher downloads or verifies files while the game streams
assets from the same disk. Every 15th frame the game asks its streaming thread for a 2 MB asset and
waits, as in `iothread`. Next to it `bgio -R` reads a large file with 4 threads, in 400 ms bursts
every 1.5 s, pinned to CPUs 6–9 so it never takes the game's cores. The two share nothing but the
disk.

The VM's disk is too fast for the reader to matter, so the disk is made slow: the game and the reader
run in one cgroup (`hitchdisk.slice`) whose reads from that disk are capped at 200 MB/s
(`IOReadBandwidthMax`). That is a slow disk with one budget for both: while the reader is idle the
game has all of it, and during a burst the reader's four threads take most of it. Ground truth is two
logs that hitchtrace never sees: the game's (which frames loaded, and how long the load took) and the
reader's (when it was reading).

The cap needs the I/O controller delegated to the user's systemd, once, as root:

```
sudo mkdir -p /etc/systemd/system/user@.service.d
printf '[Service]\nDelegate=cpu cpuset io memory pids\n' | sudo tee /etc/systemd/system/user@.service.d/delegate.conf
sudo systemctl daemon-reload && systemctl --user daemon-reexec
```

`run.sh iodisk` refuses to run without it. `DISK_BPS=none` runs the same thing with no cap.

**Result** (60 s, `HITCHTRACE_ARGS=-U`, hitchtrace at 2fef08b, 3,290 frames):

| The game's own log | Loads with the reader idle | Loads during a reader burst |
|---|---|---|
| Frames | 162 | 58 |
| Load time, median | 2.2 ms | **57.3 ms** |
| Over budget | 23 (14%) | 54 (93%) |

The same 2 MB read is 26 times slower while the other program reads. Scoring each over-budget record:

| From one record | Loads during a burst (54) | Noise (83) |
|---|---|---|
| Some group reads "waited for `mg_stream`, which was blocked on I/O, ended by an interrupt" | **54 (100%)** | 22 (quiet loads) |
| The streamer's I/O in the chain / the game's own load time, median | **99%** | 97% |
| That group is top-ranked by total | 51 (94%) | 7 |
| … by stall time | 54 (100%) | 20 |
| **Anything in the record names `bgio`** | **0 (0%)** | 0 |

The 22 noise records with an I/O group are loads made while the reader was idle. They are short (about
2 ms), correctly reported, and mostly not on top. A typical record from a burst:

```
hitch frame 2144  86.2 ms  (budget 20.8 ms, over by 65.4 ms)  root mg_main
  blocked   85.4 ms  futex 82.2 | timer 2.8 | present 0.3
  resolved   waiting on a running task 19.4 ms, inherited stalls 63.1 ms
  stalls     off-CPU time by cause and by who ended it, largest first
     63.7 ms  futex    blocked in __futex_wait
              <- mg_stream         blocked 63.1 ms in blk_io_schedule
                <- [softirq]       woken from a softirq
     18.5 ms  futex    blocked in __futex_wait
              <- mg_worker2        was running (waiting for work) 0.0 ms
    mg_stream    oncpu 0.6  blocked 66.3  futex 3.4 | io 63.0   largest 61.9 ms in blk_io_schedule
```

**What the record gives.** "This frame was 65 ms late because the streaming thread waited 63 ms for
the disk, in two waits." That is complete and correct, and it settles the first question a developer
has: the game did nothing unusual, the storage answered slowly. With `-U` the game's own stack says
which load it was.

**What it cannot give: the other program.** Nothing in any of the 54 records mentions `bgio`, and
nothing can. A wakeup chain follows who woke whom, and the streamer was woken by the disk's
completion interrupt. The reader never woke or preempted any thread of the game; it took the disk's
time by having its own requests served first. The competition happens inside the kernel's block
layer (the part that queues read and write requests for a device), which hitchtrace does not watch.
The record for a slow disk and the record for a busy disk are the same record.

A developer could still infer it: 2 MB in 63 ms is 32 MB/s, far below what the disk does when idle.
But hitchtrace does not record the size of the read, and the same load reported 2 ms a second earlier
lives in a different record, or in none if that frame was on time.

What would close the gap is block-layer data taken only while a target thread is waiting on I/O: how
long its request sat queued before the device started it, against how long the device took, and which
other processes had requests issued or completed in that window. The kernel has tracepoints for each
step (`block_rq_insert`, `block_rq_issue`, `block_rq_complete`). On this setup one more caveat
applies: the cap delays requests in the cgroup throttle, before they reach the device queue, so the
queued-versus-served split would have to count that stage as well.

**A second effect: the frames after a long load are slow too.** The game's log shows the workers' job
phase, usually 13.7 ms, taking longer in the frame that waited and in the ones after it, and the more
so the longer the wait was:

| Load took | Loads | Job phase, same frame | Next frame | The one after | Next frame over budget |
|---|---|---|---|---|---|
| under 5 ms | 151 | 13.6 ms | 14.0 | 13.4 | 1% |
| 5–20 ms | 11 | 15.4 | 15.3 | 14.2 | 0% |
| 20–45 ms | 23 | 19.8 | 18.4 | 16.7 | 26% |
| 45–70 ms | 25 | 24.1 | 20.9 | 17.0 | 60% |
| over 70 ms | 10 | 24.3 | 21.4 | 19.9 | 60% |

So one slow load costs two or three hitches, not one. The reader's bursts alone do not slow the
workers: frames with no load have the same 13.8 ms job phase inside and outside a burst. What the
records of those following frames (26 of them) say: no I/O at all, a 23 ms wait on the workers, and
the three workers on the CPU for 60 ms between them where a normal frame needs about 41 ms. They were
not queued (4 ms runnable in total). They ran the same work 50% slower. The likely reason is that the
cores sat idle for the length of the load and came back slow (lower clock speed, or the host having
parked the virtual CPUs), but that is not verified: nothing inside the VM shows it.

For hitchtrace this is the `cpuspike` blind spot again. The record says "the workers were running"
and blames no stall, which is true, but it has no way to say they ran slower than usual, and nothing
links the follow-on hitch to the load one frame earlier. In the viewer's overview strip the pattern
is visible to a person: a high dot followed closely by one or two just over the budget line.

**Other observations.**

- **Ranking is not the problem here** (94% by total). The load is long enough to outweigh the frame's
  normal waits; the three misses are loads of 11–16 ms.
- **The timeline agrees with the buckets**: `check_timeline.py` finds 1 thread line in 1,104 off by
  more than 50 µs (77 µs of run-queue time on a helper thread).
- The interrupt that ends the chain is reported the same way whether the wait was in the device or in
  the cgroup throttle, so the record does not show that a throttle was involved.

## Case 3: walking memory that was swapped out (`mem`, `memthp`)

**What it models.** `real-world-cases.md` category B, the memory-tuning folklore: a game that runs well until
memory is tight. The game keeps a 512 MB world in memory and, every 30th frame, walks 2 MB of it
(reads and writes every page), as on entering an area whose data has not been touched for a while. It
runs in a cgroup limited to 420 MB (`systemd-run --user --scope -p MemoryMax=420M`), so part of the
world is always in swap and the walk faults it back in. `memthp` asks for transparent huge pages for
the world (`MADV_HUGEPAGE`). Ground truth is the game's log: which frames walked, how long the walk
took, and the main thread's own page-fault counts from `getrusage`.

Without the limit the walk takes 0.1 ms. With it, a median 18 ms, with 64 major and 448 minor faults.

| From one record | `mem`: walk frames (81) | `memthp`: walk frames (83) | Noise (38, 39) |
|---|---|---|---|
| Has an I/O group whose kernel stack is a page fault | 80 (99%) | 83 (100%) | 0 |
| Memory (fault buckets + that I/O) is ≥ half the overrun | 81 (100%) | 83 (100%) | 0 |
| Memory time / the game's own walk time, median | 95% | 96% | |
| The group's wait count equals the game's major-fault count (±2) | 80 (99%) | 83 (100%) | |
| That group is top-ranked by total | 27 (33%) | 50 (60%) | 0 |
| … by stall time | 80 (99%) | 83 (100%) | 0 |
| … under the old rule (longest single wait) | 12 (15%) | 7 (8%) | 0 |

The record reads: 64 waits on I/O, none longer than about 0.7 ms, 13–18 ms in all, blocked in

```
io_schedule <- folio_wait_bit_common <- do_swap_page <- handle_mm_fault <- exc_page_fault
```

which is a page fault waiting for a page to come back from swap, plus about 3 ms of on-CPU time
inside major faults. The count matching the kernel's own major-fault counter, frame by frame, is a
second independent check that the grouping is right.

- **This is the case grouping was built for.** No single wait stands out, so the old rule picked the
  swap-in in 8–15% of frames. Summed, it is the largest stall in every one.
- **Huge pages made no difference here.** Same fault counts with and without them: pages come back
  from swap one small page at a time. This is one data point under one kind of pressure, not a
  verdict on the THP advice in `real-world-cases.md`.
- **The record says "swap-in", not why memory was tight.** It cannot see the limit, or who else used
  the memory. No reclaim time showed up on the main thread.

## Case 4: the frame limiter's sleep ends late (`slack`)

**What it models.** `real-world-cases.md` F2, the Linux 7.0 clockevents regression: timers delivered late, so a
frame limiter oversleeps. A preloaded library (`slack.so`) wraps `clock_nanosleep` in the unmodified
game and, for 400 ms in every 1.5 s, raises the thread's timer slack to 8 ms before the sleep. (Another
process may not set a thread's timer slack, so this has to run inside the game.) Ground truth is the
library's log of the affected sleeps, and the game's own measure of how late each sleep ended.

In affected frames the sleep ended a median 8.1 ms late and 98% went over budget.

| From one record | Affected frames (244) | Noise (29) |
|---|---|---|
| Timer group / the game's own limiter time, median | 99% | 99% |
| The timer sleep is top-ranked by total | 0 | 0 |
| The record has a field saying the sleep ended late | **0** | 0 |

**hitchtrace cannot explain this hitch.** It measures the sleep exactly (9.5 ms against a usual 6),
but a sleep that was asked to be 6 ms and took 14 looks the same as one asked to be 14. The timeline
shows a longer pink bar and nothing else. The stall-time ranking, which ignores timer sleeps, is blind
to it by construction. This is the gap `real-world-cases.md` predicted, now measured: hitchtrace has to record
how long a sleep **asked for**, and report the overshoot.

## Case 5: calls to a server that cannot get its CPU (`ipc`)

**What it models.** `real-world-cases.md` D1 and the "wineserver round-trips" example: a game thread that makes
many blocking calls per frame to another process. `ipcserver` stands in for wineserver, pinned to
CPU 5; the game's main thread makes 30 request/reply calls to it every frame. `bgload` bursts on CPU 5
only (4 threads, 25 ms every 700 ms), so the game's own cores are untouched and only the server is
delayed. Ground truth is `bgload`'s burst log.

**Result** (`out/ipc-holder`, 2026-11-05). Normally the 30 calls take 2.0 ms; during a burst, a median
7.0 ms, and 45% of those frames go over.

| From one record | Burst frames (167) | Noise (26) |
|---|---|---|
| Has a group of waits ended by `mg_server` | 167 (100%) | 22 (85%) |
| … saying the server was kept off the CPU | 167 (100%) | 2 (8%) |
| … and naming `bgload` as the holder of its CPU | **163 (98%)** | 1 |
| The group's inherited time / the game's extra call time, median | 97% | |
| That group is top-ranked by total | 139 (83%) | 0 |
| … by stall time | 167 (100%) | 1 |

The chain crosses the process boundary: 30 waits, ended by a thread of another process, which was
itself kept off the CPU for the right amount of time, queued behind `bgload` ("kept off the CPU
10.0 ms, last queued behind bgload0"). The server was never preempted: it was asleep, was woken by the
game's request, and then waited in the run queue. So the hop names the task that held the CPU when
that wait ended, flagged `queued`.

- In the other 4 the last holder was a kernel worker on CPU 5 (`kworker/5:1`), which ran between
  `bgload` and the server. Only the last holder is recorded, not the one that held the CPU longest.
- The one noise record naming `bgload` is a frame that overlapped a burst by 0.5 ms, under the
  1 ms the ground truth asks for: `bgload` did hold the CPU then.
- Holders are named on chain hops only. A game thread's own wait in the run queue still names nobody.
- **The noise frames show the same group, smaller.** Every call costs the server's wake-up latency, so
  even an untroubled frame has 1–2 ms of "server kept off the CPU". In a frame over budget by 1 ms for
  another reason, that can rank first by stall time. It is real time, but not what made that frame
  different. The server has no lane on the timeline: only the game's process is logged.

**First version** (`out/ipc`, before holders were recorded, kernel 7.0.0-38): the same chain, but
`bgload` named in 0 of 233 burst frames. The calls took 2.8 ms normally and 9.2 ms in a burst, 81% of
burst frames went over, and the main thread's wait for its workers was twice as long (median 14.1 ms
against 7.5), so the server's group ranked first by total in only 8%.

## Case 6: CPU work in the game (`cpuspike`), a negative control

**What it models.** The most famous stutter in `real-world-cases.md`, shader compilation, and any spike of the
game's own: every 30th frame the main thread burns 12 ms of CPU. hitchtrace should not blame a stall.

| From one record | Spike frames (86) |
|---|---|
| Running is ≥ half the overrun | 86 (100%) |
| Extra running time / the game's own burn time, median | 99% |
| No stall worth a quarter of the overrun | 78 (91%) |

The record says "the main thread was running 12 ms more than usual", which is the right answer and
the signal to switch to a CPU profiler. It cannot say what the thread was running: stacks are taken
when a thread leaves the CPU, not while it runs. In the other 9% an unrelated stall of some size
happened in the same frame.

## Case 7: the game's threads are confined to one core (`squeeze`)

**What it models.** `real-world-cases.md` category A, the scheduler reports: threads that are ready to run but
are kept waiting while cores sit idle, because the scheduler packed them together (A1, A3) or the
game was handed a wrong affinity mask (the list of cores a thread may run on). A custom scheduler
cannot be loaded here without root, so the placement is forced from outside: every 1.5 s `squeeze`
restricts all of the game's threads to CPU 0 for 300 ms, then gives the old masks back. Nothing else
runs on that core. The game's threads only get in each other's way. Ground truth is `squeeze`'s log.

| The game's own log | Other frames | Frames during a squeeze |
|---|---|---|
| Frames | 2,783 | 307 |
| Over budget | 55 (2%) | 303 (99%) |
| Job phase, median | 14.1 ms | 39.9 ms |

The game's profiler would show "the job phase took three times as long" and nothing about why.

| From one record | Squeezed frames (303) | Noise (55) |
|---|---|---|
| Top group: a wait on a worker that was kept off the CPU | **303 (100%)** | 43 |
| … by a thread of the game itself | 296 (98%) | 27 |
| Every preemptor named in the record is the game's own | 286 (94%) | 15 |
| The worker's time off the CPU / the game's extra job time, median | **96%** | |
| All threads together ran no more than 1.1 cores' worth | 263 (87%) | **0** |
| Cores' worth of running per frame, median | 1.00 | 1.93 |
| Any field saying which CPU, or that other CPUs were idle | **0** | 0 |

```
hitch frame 540  46.7 ms  (budget 20.8 ms, over by 25.9 ms)  root mg_main
  blocked   45.7 ms  futex 45.5 | present 0.1
  resolved   waiting on a running task 14.9 ms, inherited stalls 30.8 ms
  stalls     off-CPU time by cause and by who ended it, largest first
     45.5 ms  futex    blocked in __futex_wait, 2 times, longest 45.5 ms
              <- mg_worker0        kept off the CPU 30.8 ms, last preempted by mg_worker1
  threads
    mg_worker0    oncpu 14.7  runnable 31.1   preempted by mg_main for 7.3 ms
    mg_worker2    oncpu 15.9  runnable 28.6   preempted by WSI swapchain q for 12.0 ms
    mg_worker1    oncpu 14.2  runnable 23.3   preempted by mg_worker0 for 9.0 ms
```

**What the record gives.** Each worker did its usual 15 ms of work and spent twice that waiting for
a core, pushed aside by the game's own threads. That already rules out the two explanations a
developer would try first: the work did not get heavier (same on-CPU time as any frame), and no other
program took the cores (as in `bgcpu`, where the preemptor is `bgload`). The three workers' running
times add up to the frame's length, so they ran one after another. On the timeline this is plain to
see: the lanes take turns and never run side by side.

**What it cannot give: the core.** "One at a time" is an inference from the totals. Nothing in the
record says the threads were all on CPU 0, or that CPUs 1–3 had nothing to do, which is the fact that
separates a bad placement from a machine that is simply full. The scx #3739 report in `real-world-cases.md`
turned on exactly this: seeing what ran on the one CPU. The fix is small: record the CPU with every
interval on the timeline, and how many of the process's allowed cores were idle when a thread was
made to wait. It belongs with the run-queue occupant from case 5.

**The noise frames look similar in kind, not in size.** In 43 of 55 the top group is also a worker
kept off the CPU, by a sibling or by a desktop thread, for a millisecond or two. The parallelism
figure is what tells the two apart: 1.0 against 1.9.

## Case 8: new memory while memory is full (`alloc`)

**What it models.** `real-world-cases.md` category B again, from the other side. Case 3 was about getting old
memory back from swap. This is about getting new memory when there is none free: the kernel has to
push something else out first, and it does that work on the thread that asked (*direct reclaim*). The
game keeps its 512 MB world, never walked, under the same 420 MB limit, so the cgroup is always
full. Every 15th frame the main thread allocates and fills 8 MB and keeps the newest 64 such blocks.
Ground truth is the game's log: how long each allocation took. Without the limit
(`MEM_MAX=none`, run `alloc-nolimit`) it is 3.5–4.0 ms every time, for 2,048 page faults.

| The game's own log | Allocations at normal speed | Slow allocations (over 2× the usual) |
|---|---|---|
| Frames | 113 | 112 |
| Allocation time, median | 4.4 ms | 13.6 ms (90th percentile 32 ms) |
| Over budget | 81 | 112 |

An earlier version counted the reclaim that runs inside a page fault as fault time, so reclaim read
1.7 ms against 10.9 ms of "fault" per record. Fixed ([history](../docs/history.md)): a fault's time
excludes reclaim and compaction nested in it. A record now:

```
hitch frame 1604  29.5 ms  (budget 20.8 ms, over by 8.6 ms)  root mg_main
  oncpu     14.5 ms  (fault 2.4, reclaim 9.5)
  blocked   14.2 ms  futex 13.0 | timer 1.1 | present 0.1
  stalls     off-CPU time by cause and by who ended it, largest first
     13.0 ms  futex    blocked in __futex_wait
              <- mg_worker1        was running (waiting for work) 0.1 ms
```

Fault time is the same 2.3 ms in every allocation, slow or fast, which is what 2,048 faults cost
with no pressure. Everything above that is reclaim.

| From one record | Slow allocations (112) | Noise (128) |
|---|---|---|
| Reclaim is ≥ half the overrun | **109 (97%)** | 28 |
| Reclaim time / the allocation's extra time, median | **94%** | |
| Fault time, median | 2.3 ms | 1.9 ms |
| Any stall group is about memory | 1 | 0 |
| The top-ranked group is about memory | **0** | 0 |
| Anything says what was reclaimed, or why memory was short | 0 | 0 |

The 28 noise records with reclaim are allocations just under the 2× line: a millisecond of reclaim in
a frame a millisecond over.

**What the record gives.** "The main thread spent 9.5 ms freeing memory before it could have its
own." Amount right to within 6%, on the right thread, separated from the ordinary cost of the faults.
That answers the folklore question in `real-world-cases.md` for this frame: the stall was reclaim, not
swap-in and not compaction.

**What it does not do: rank it.** The stall list is built from time *off* the CPU. Reclaim here is
time *on* the CPU, in the kernel, so it is not a group, has no stack, and is not on the timeline. The
list is headed by the ordinary 13 ms wait for the workers in every one of the 112 records, and the
explanation sits in the bucket line above it. A reader who goes straight to the top-ranked stall is
sent the wrong way. In-kernel running time needs to be a rankable entry of its own, with the kernel
stack that was running.

**Nor why memory was short.** The record cannot see the cgroup limit, what was pushed out (here the
unwalked world, to swap), or that it was written to disk. The workers' lines show the knock-on
effect: across the run they did 175 ms of reclaim of their own and waited 64 ms on swap-ins for pages
of theirs that had been pushed out.

## Case 9: huge pages while free memory is in pieces (`compact`)

**What it models.** `real-world-cases.md` B2, the contradictory advice about transparent huge pages (2 MB pages
the kernel hands out in place of 512 small ones). A huge page needs 2 MB of free memory in one
piece. If the free memory is scattered, the kernel can move used pages together to make a piece
(*compaction*), again on the thread that asked. `frag` takes nearly all free memory one small page at
a time and gives every other page back, so that about 1.5 GB is free and almost none of it is
contiguous. The game then grows by 24 MB of huge pages (`MADV_HUGEPAGE`) every 15th frame and frees
nothing. This is not confined to a cgroup: compaction is a property of the whole machine's memory.

**Compaction alone could not be made to stall the game.** With free memory in pieces but plentiful,
8 MB of huge pages took 1.0 ms, the same as with free memory whole (1.1 ms). The kernel's counters
show why: it never compacted on the game's thread (`compact_stall` 0), because its background thread
(`kcompactd`) had moved 125,000 pages and kept whole pieces ready. At three times the rate the
allocations stayed flat as well, at about 3 ms for 24 MB. Stalls began only when the free memory ran
out altogether, about 15 s into the run, and from then on the machine was short of memory as well as
fragmented. The run is kept short (27 s) for that reason.
Changing how eagerly the kernel compacts, which is what the advice in `real-world-cases.md` is about, needs
root.

| The game's own log | Allocations at normal speed | Slow allocations |
|---|---|---|
| Frames | 57 | 42 |
| Allocation time, median | 3.1 ms | 19.7 ms |
| Page faults, median | 12 (all huge pages) | 6,144 in 40 of 42 (small pages for part or all) |

Over the run the kernel counted 1,473 compactions on a thread that was waiting for the result, 1,369
of which failed, and 440 huge-page requests that fell back to small pages.

| From one record | Slow allocations (42) | Noise (53) |
|---|---|---|
| Memory (faults + reclaim + compaction) is ≥ half the overrun | **42 (100%)** | 15 (normal allocations) |
| Compaction + reclaim is ≥ half the overrun | 23 (55%) | 0 |
| Compaction is the larger of the two | 16 (38%) | 0 |
| Faults + reclaim + compaction / the allocation's time, median | 78% | |
| The top-ranked group is about memory | **0** | 0 |

Two records, one where the kernel got most of its huge pages and one where it got none (the fault
counts are the game's own):

```
hitch frame 1514  47.8 ms  (budget 20.8 ms, over by 27.0 ms)  root mg_main      [1,034 faults]
  oncpu     33.7 ms  (fault 4.3, reclaim 5.8, compact 22.1)

hitch frame 1244  29.3 ms  (budget 20.8 ms, over by 8.5 ms)   root mg_main      [6,144 faults]
  oncpu     13.5 ms  (fault 6.7, reclaim 0.9, compact 0.3)
```

**What the record gives.** The split is real and it is the useful part. When the kernel does make
its huge pages, the extra time is compaction, 15–22 ms in the frames where it mostly succeeded. When
compaction fails, the kernel hands out small pages instead and the cost moves to faults: 6,144 of
them where 12 would have done, about 7 ms in the fault handler and another 5 ms getting in and out of
it. Later in the run, as memory gets shorter still, reclaim grows to 10–28 ms on top. The record
shows which of the three a given frame paid for. It tells a developer that asking for huge pages
cost these frames more than not asking would have.

**What it cannot give.** That the compaction *failed*, and that the faults were for small pages
standing in for huge ones. The game's own fault counter shows it; the record has the time but not the
count or the outcome. The kernel reports the outcome where hitchtrace already listens (the end of a
compaction carries its result). And the cost of entering and leaving 6,144 faults lands in plain
"running", because the fault timer covers only the kernel's handler.

As in case 8 none of this is ranked: the list is headed by the wait for the workers.

**Sample size.** 42 frames from one short run, on a machine pushed to the edge of its memory. The
split between compaction and reclaim will differ on other machines; the point here is that the record
separates them.

## What could not be reproduced on this machine

- **A real scheduler bug** (A1–A4): loading a sched_ext scheduler needs root. `squeeze` reproduces
  the symptom, not a scheduler.
- **Wine's synchronisation paths** (D1): Wine is not installed, and ntsync needs a kernel module
  loaded. `ipc` stands in for the wineserver round trip only.
- **Split locks**: the VM does not expose split-lock detection.
- **Firmware stalls** (F1) and **anything inside a GPU** (D2, D3, E1): there is no firmware to stall
  and no GPU; rendering is in software.
- **A sustained compaction stall** (B2): see case 9.
