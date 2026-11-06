# Evaluation status

What the paper claims, what each claim needs, and what has been measured so
far. Updated 2026-11-06.

## Claims and evidence

| Claim | Evidence required | Status |
|---|---|---|
| The records are correct | Injected causes with independent ground truth | **Done** in a VM: hitchbench, 10 case studies ([below](#done-the-records-are-correct)) |
| It works on real software | Proton and native free titles on bare metal | Not started |
| It's cheap enough for long sessions | Overhead against no tool and existing tools | **Pilot only**, in a VM ([below](#pilot-overhead-against-existing-tools)); bare metal not started |
| It catches what others miss | Same rare hitches, same machine, hitchtrace vs existing tools | **Run once in the VM** on five case studies, eight tools ([below](#run-does-hitchtrace-catch-what-others-miss)); real software not started |
| It finds the cause without being told | Blind diagnosis | Not started |
| This class of hitch matters | Share of real hitches that are kernel-observable | Not started (the draft's Q8) |
| It tells us something new | A finding nobody had measured | Not started |

Everything so far ran on the development VM (Hyper-V, 10 vCPUs of an
i7-12700H, kernel 7.0, lavapipe). None of it is a bare-metal number.

## Done: the records are correct

### hitchbench (paper §5, Q1–Q5)

`tests/hitchbench.c` injects one cause into every 20th frame and writes its
own ground truth per frame; `tests/run_hitch_tests.sh` attaches hitchtrace and
`tests/check_hitches.py` joins the two. Thirteen injector classes, including
two negative controls (`cpu_spike`, `quiet_baseline`). The paper's numbers
are generated from `tests/out/` by `paper/plot/src/hitchbench_results.py`;
none is typed by hand. Since 2026-11-05 the script covers all twelve
injected classes and the baseline, and the paper's evaluation takes its numbers from
the 13-class run now in `tests/out/`.

- Every class passes: exactly the over-budget frames recorded (Q1), the
  injected cause named (Q2), same-symptom causes told apart (Q3), many small
  waits added up (Q4), negative controls not blamed on a stall (Q5).
- Stall groups (ranking by group, not by longest single wait): the injected
  cause ranked first in 146 of 151 injected frames (97%), against 113 (75%)
  under the old rule. In the 13-class run, scored as the paper scores it
  (frames wholly inside the trace, `server_queued` included): 151 of 160,
  against 119; the nine misses are all `io`, where the present wait
  outweighs the I/O.
- The reclaim-inside-a-page-fault fix (found in case 8) kept all 12 classes
  passing.
- `server_queued` (2026-11-05): a server at the lowest priority shares a CPU
  with three hogs and is asked for a reply while they run, so it waits in
  the run queue without being preempted. The server's hop names a hog as the
  CPU's last holder, flag `queued`, in 15 of 19 injected frames; the other
  four are three frames at the trace window's edges with no record and the
  first record after attach. All 13 classes pass. In that first record the
  server had gone to sleep before hitchtrace attached, so the root's wait
  resolves as waiting on a running task, and the resolved gate drops that
  one frame (15 of 16 inside the window).

Still open in the draft: §5.4's vkcube numbers and §5.5's "real Vulkan
application" numbers. The artifacts for §5.5 were lost and must be
regenerated (Q6).

### Case studies (`cases/README.md`)

A small Vulkan game (`cases/minigame`, also Hitch Lab) with one real
mechanism per case, the game's own profiler as ground truth, and every
figure computed from one record at a time. Each case has a reading guide in
`cases/pages/`.

| Case | What happens | Explanation in the record | Right amount | Ranked first (total / stall time) | What it cannot say |
|---|---|---|---|---|---|
| 1 `bgcpu` | a background app takes the game's cores | 99% | 100% of the worker's delay | on top (one long wait) | names only the last preemptor |
| 2 `iomain`, `iothread` | a frame waits on a disk read | 100% | 91–94% | 66–71% / 81–88% | why the disk was slow |
| 2b `iodisk` | another program reads from the same slow disk | 100% | 99% | 94% / 100% | who else was using the disk |
| 3 `mem`, `memthp` | walking memory that was swapped out | 99–100% | 95–96% | 33–60% / 99–100% | which pressure caused the swapping |
| 4 `slack` | the frame limiter's sleep ends late | sees the sleep | 99% | 0% | that the sleep was late |
| 5 `ipc` | calls to a server that cannot get its CPU | 100% | 97–105% | 8–83% / 98–100% | names `bgload` as the holder of the server's CPU in 98% since the second version; only the last holder |
| 6 `cpuspike` | CPU work in the game (negative control) | "running", 100% | 99% | blames no stall in 91% | where the CPU time went |
| 7 `squeeze` | the game's threads are confined to one core | 100% | 96% | 100% | which CPU; that the other cores were idle |
| 8 `alloc` | new memory while memory is full | 97%, after a fix | 94% | not ranked at all | what was pushed out, and why memory was short |
| 9 `compact` | huge pages while free memory is in pieces | 100% as "memory" | 78% of the allocation | not ranked at all | that the kernel gave up and used small pages |

These are reproductions of mechanisms, not of the real reports in
`cases.md`: they show the records are right when the cause is known, not
that the real cases look like this.

## Pilot: overhead against existing tools

### Method

Harness: `tools/overhead/` (`run.py`, `analyze.py`, `delayacct_frames.c`;
see its README). Every tool runs on the same workloads in shuffled order,
with a no-tool run before every second tool run. Each tool run is compared
with its neighbouring no-tool runs in the same workload and round (median
ratio, bootstrap 95% interval). Slowdown is measured from the game's own
frame log or the benchmark's own result, never from the tool being
measured. Tools' own processes are pinned to the game's four CPUs.

| Arm | What it is |
|---|---|
| `ht`, `ht-notimeline`, `ht-stacks` | hitchtrace: default, without the per-frame timeline, with user stacks |
| `perfsched` | `perf sched record`: every scheduler event |
| `ftrace` | flight recorder: `sched_switch`/`sched_waking` in an overwriting ring, snapshot on a slow frame (uprobes, synthetic event, hist trigger; no BPF) |
| `offwaketime` | bcc `offwaketime`: blocked and waker stacks, summed |
| `chaingraph` | this repository's wake-chain profiler |
| `delayacct` | kernel delay accounting read per frame through taskstats (no BPF) |
| `perfoffcpu` | not run: Ubuntu's perf is built without BPF skeletons |

| Workload | What it measures |
|---|---|
| `game60` | the game at its 60 fps cap |
| `gamemax` | the game uncapped |
| `ipc` | the game making 30 calls per frame to a server process |
| `pipe-target` | `perf bench sched pipe` inside the traced process: cost per context switch |
| `pipe-other` | the same in another process: what tracing the game costs everything else |

Run: `build/overhead/pilot-20261103-113525/` (`--preset pilot`: 3 rounds of
15 s, 215 runs, no failures; `summary.md` there). An earlier pilot
(`pilot-20261103-103426`) was spoiled by harness bugs and is not used.

### Results (VM, 3 rounds per tool)

**Frame time.** On `game60`, `gamemax` and `ipc`, frame p50 and frames per
second are within ±1% of no tool for every tool; job-phase medians move
2–3% with intervals that cross 1. At game rates (about 10k context switches
per second) no tool's cost is visible above this VM's noise.

**Slow frames caught, and output** (caught / over budget in the game's own
log, inside the measured window):

| Arm | `game60` | `ipc` | Output, MB/hour |
|---|---|---|---|
| `ht` | 27 / 27 | 49 / 49 | 28–75 |
| `ht-notimeline` | 34 / 34 | 56 / 56 | 20–39 |
| `ht-stacks` | 22 / 21 | 32 / 32 | 23–48 |
| `delayacct` | 16 / 16 | 86 / 87 | 0.3–3.6, delay counters only, no wake chain |
| `ftrace` | 10 / 21 | 44 / 60 | 1,766–5,154 |
| `perfsched` | all | all | 9,646–25,215 on the game; about 108,000 on pipe |
| `offwaketime` | no per-frame output | | one summary at exit, 84–112 KB |
| `chaingraph` | no per-frame output | | one summary at exit, 111–357 KB |

ftrace misses the frames whose extra time was inside present: its frame
runs from present's return to the next frame mark, since ftrace can pair two
events but not one with itself. `ht-stacks`' 22 / 21 is one frame at the
edge of the window or of the budget.

**Added time per context switch** (half the added time per pipe round trip;
baseline about 4.2 µs per switch in this VM):

| Arm | Traced process (`pipe-target`) | Another process (`pipe-other`) |
|---|---|---|
| `ht` | 479 ns | 409 ns |
| `ht-notimeline` | 569 ns | 413 ns |
| `ht-stacks` | 458 ns | 454 ns |
| `offwaketime` | 881 ns | 322 ns |
| `chaingraph` | 430 ns | 18 ns |
| `perfsched` | 325 ns | 73 ns |
| `delayacct` | 199 ns | −132 ns (noise) |
| `ftrace` | 134 ns | 168 ns |

**In-kernel BPF time**, share of one core (separate runs with
`kernel.bpf_stats_enabled`):

| Arm | `game60` | `pipe-target` |
|---|---|---|
| `ht` | 2.10% | 7.50% |
| `ht-notimeline` | 2.14% | 7.40% |
| `ht-stacks` | 2.26% | 10.47% |
| `offwaketime` | 1.13% | 11.81% |
| `chaingraph` | 2.17% | 13.87% |

hitchtrace's switch hook takes about 600 ns per call on `game60` and its
wakeup hook about 700 ns.

After the run-queue holder was added (2026-11-05), the three `ht` arms were
rerun on the pipe workloads without root (`build/overhead/holder-pipe`,
n=3): 70 to 880 ns per switch on `pipe-other`, 481 to 709 ns on
`pipe-target`. The arms run the same switch hook, so that spread is the
noise; no change from the pilot can be resolved. The holder adds a few
comparisons per switch and writes only after a run-queue wait of at least
100 µs, which a pipe round trip (about 9 µs) never reaches. The hook's own
time per call (the cost phase, needs root) was not rerun.

**Added 2026-11-06: perf `--off-cpu` and Perfetto.** Each was run with
`ht` in a pilot of its own (`build/overhead/perfoffcpu-pilot`,
`build/overhead/perfetto-pilot`; 64 runs each, no failures). The VM drifted
between the runs: `ht` itself came out at 780–800 ns per switch in the first
and 419–505 ns in the second, against 409–479 ns on 2026-11-03, so compare
each tool with `ht` in its own run.

| | `ht`, same run | perf `--off-cpu` | `ht`, same run | Perfetto |
|---|---|---|---|---|
| Added ns per switch, `pipe-target` | 800 | **2,463** | 419 | −44 (noise) |
| … `pipe-other` | 780 | 834 | 505 | 195 |
| `gamemax` frames/s | 0.996 [0.960, 1.007] | **0.945 [0.907, 0.985]** | 1.000 | 1.000 |
| `game60` job phase p50 | 1.015 | 1.030 [1.028, 1.038] | 0.980 | 0.988 |
| Tool's own processes, CPU, game workloads | 0.1% | 0.0–0.1% | 0.1% | 1.0–2.3% |
| MB/hour, `game60` / `ipc` | 31 / 57 | 1,375 / 1,450 | 22 / 61 | 2,557 / 15,766 |
| Slow frames caught, `game60` / `ipc` | 29/28, 40/39 | no per-frame output | 23/23, 39/39 | 23/22, 65/65 |

- perf `--off-cpu` is the costliest tool on the traced process (it takes the
  user stack at every switch-out and keeps perf's usual on-CPU sampling) and
  the only one with a measurable frame slowdown: 5.5% fewer frames per
  second on the uncapped game.
- Perfetto costs about nothing per switch, like the ftrace recorder (the
  kernel only writes each event into its ring); its cost is `traced_probes`
  reading and encoding events in user space, 1.0–2.3% of a core, against
  `ht`'s BPF programs at about 1.8% of a core on `game60` in the same run.
  It caught every slow frame. Its output is an underestimate: in 15 s runs
  each snapshot is a few MB, where in a long session each would be the whole
  32 MB ring.
- The cost phase (`kernel.bpf_stats_enabled`) covers `ht` only; perf's BPF
  time was not measured that way.

**Other observations.** Server calls in `ipc` came out about 10% faster
under hitchtrace and about 15% slower under chaingraph and offwaketime, with
wide intervals. The speed-up may be the extra kernel work keeping vCPUs out
of idle; untested.

### Does this show a clear overhead advantage? No.

**Where hitchtrace is not ahead:**

- **Per context switch** it is among the more expensive tools: about 480 ns
  on the traced process (only offwaketime is higher, and perf `--off-cpu`,
  measured later: about three times `ht` in its run), and the highest of all
  on other processes (about 410 ns against chaingraph's 18 and perf sched's
  73; level with perf `--off-cpu` in its run, above Perfetto's).
- **In-kernel time on the game** it is level with chaingraph and about
  twice offwaketime's. It is cheaper than both only on the pipe benchmark.
- **At frame level** every tool is indistinguishable from no tool, so no tool
  has a measurable advantage there, hitchtrace included.

**What the data does support:**

- **Output, a few hundred times less than full tracing:** 20–75 MB/hour
  against perf sched's 10–25 GB/hour, the only other tool that keeps
  per-frame detail of everything.
- **Completeness:** every slow frame caught, where the ftrace flight
  recorder, the cheap way to keep only the moments around a slow frame,
  caught half to three quarters at 2–5 GB/hour. Perfetto as a flight
  recorder, with a trigger that sees the whole frame, also caught every one,
  at 2.6–15.8 GB/hour in 15 s runs (more in a long session).
- **Per-frame causes:** offwaketime and chaingraph cost about the same but
  give one summary for the whole run, with nothing per frame. delayacct is
  cheaper but has no wake chain. They are not doing the same work.

**A claim that can be defended now.** hitchtrace's overhead is in the same
range as existing scheduler tracers, with no measurable effect on frame
time. For that cost it produces a record of causes for every slow frame at a
few hundred times less output than full tracing, and it misses none, where a
flight recorder misses many. The advantage is that it can be left on for
hours of play. It is not lower CPU overhead.

**What a claim of an overhead advantage would need:**

1. **The cost to other processes fixed.** `on_switch`
   (`src/hitchtrace.bpf.c`) creates and updates per-task state for every
   task on the machine. This is deliberate, since a wake chain can run
   through another process (a server, wineserver) whose history is needed
   before it is known to matter. Options: a much cheaper path for tasks
   outside the game (timestamps only), or tracking another task only once it
   touches the game's wake chain.
2. **Bare metal and the `full` preset** (10 rounds of 30 s, about 4.5
   hours). The VM inflates the cost of a context switch, and 3 rounds give
   wide intervals.
3. **The costs these runs do not capture:**
   - perf sched writing 10–25 GB/hour to the disk a game streams from,
     measured with a disk-bound game. That interference is itself a hitch
     source.
   - The offline analysis that perf sched and ftrace data need afterwards.
   - Sessions hours long, where output volume becomes the limit.

**The delayacct figures above are not valid.** Found 2026-11-04 in the catch
test: a thread is delay-accounted only if `kernel.task_delayacct` was on when
the thread was created, and the harness turned it on after the game had
started. So delayacct's cost here is the cost of its per-frame reading alone,
without the kernel's accounting for the game's threads, and its catch figures
counted lines carrying CPU delay only. Fixed in `tools/overhead/run.py`;
the delayacct arm needs rerunning.

### Harness problems found and fixed during the pilot

Recorded so that the same mistakes are not made again in the bare-metal run.

- Python's `open(path, "a")` seeks to the end of the file, and tracefs's
  seq_file files (`uprobe_events`, `synthetic_events`) refuse that with
  EINVAL. Opening a trigger file with `"w"` truncates it, which removes the
  triggers already there. tracefs is now written with a plain
  `os.open(O_WRONLY)`.
- A synthetic-event field `pid_t` did not match `common_pid` (`int`). The
  field was dropped; the synthetic event's own `common_pid` is the game
  thread.
- Slow-frame counts and output sizes covered the tool's whole life, which
  includes the game's startup frames, while the game's count covered only
  the window. Both now count only the window. The game also warms up for 3 s
  before the tool starts.
- The ipc server was stopped before the game, so some games died without
  writing their frame log. Helpers are now stopped in reverse order.
- The rule that drops an arm after two failures also dropped the no-tool
  baseline. Only failures of the tool itself to start or attach now count,
  and never the baseline's.

## Run: does hitchtrace catch what others miss?

Protocol, with its rules fixed before the run: [catch-test.md](catch-test.md).
Run `build/catch/catch-20261104-171550` (`tools/catch/run.sh`, scored by
`tools/catch/score.py`; table in its `summary.md`): five cases, six tools,
one 120 s run each, one injection every 10 s, tools on CPUs 6-9. All 30 runs
completed; no rule was changed after this run. Two more tools were added on
2026-11-06, with their rules written into the protocol before their own runs
(["Added tools"](catch-test.md#added-tools)): `perf record --off-cpu`
(`build/catch/catch-perfoffcpu`) and Perfetto as a flight recorder
(`build/catch/catch-perfetto`), same cases, rarity and placement.

Injected hitches recorded / attributed / culprit named:

| Case | ht | delayacct | perf off-cpu | ftrace | Perfetto | perf sched | offwaketime | chaingraph |
|---|---|---|---|---|---|---|---|---|
| `bgcpu` | 8/8/8 of 8 | 11/11/– of 11 | 11/0/0 of 11 | 8 of 8 | 11 of 11 | 10 of 10 | cause ranked 39th | cause absent |
| `iothread` | 12/12/12 of 12 | 12/12/– of 12 | 12/10/0 of 12 | 11 of 12 | 12 of 12 | 12 of 12 | 22nd | 47th |
| `ipc` | 33/33/**0** of 33; second version 29/29/**29** of 29 | 28/0/– of 30 | 33/0/0 of 33 | 29 of 33 | 33 of 33 | 32 of 32 | absent | absent |
| `mem` | 12/11 of 12 | 9/**0** of 9 | 12/0 of 12 | 12 of 12 | 12 of 12 | 12 of 12 | 20th | 37th |
| `cpuspike` | 12/12 of 12 | 12/12 of 12 | 12/0 of 12 | 12 of 12 | 12 of 12 | 12 of 12 | n/a | n/a |
| MB per injected hitch | 0.08–0.10 | < 0.01 | 1.3–3.7 | 15–42 | 45–89 | 25–33 | 0.01–0.02 (total) | 0.01–0.04 (total) |

ftrace, Perfetto and perf sched are scored for recording only: they hold the cause
when they hold the frame, but say nothing until someone analyses them (the
blind test's question). The summary tools' column is the rank of the
heaviest line carrying the cause; "visible" needs the top 10. The number of
injected hitches differs between tools because a burst makes a hitch only
when it lands in a frame without enough slack: same mechanism, not the same
frames.

What it shows:

- **Only hitchtrace names the cause per frame across the cases**: 76 of 77
  injected hitches attributed, against 35 of 74 for delay accounting (which
  cannot see the `ipc` server, and books swap-in waits as block I/O, below)
  and 10 of 80 for perf `--off-cpu`. perf samples only blocked intervals of
  at least the threshold (1 ms), with the user stack and the interval's
  length: it names the read in `iothread` (the other two load frames' waits
  were each under 1 ms, which perf only sums at the end of the run), but
  preemption is never sampled, a wait on the server or on a swapped-out page
  says nothing of its cause, and the main thread's usual wait for its
  workers looks the same as a stall (`cpuspike`). It attributed none of the
  noise hitches.
- **Only hitchtrace names a culprit**: `bgload` in 8 of 8, the disk's
  interrupt in 12 of 12. In `ipc` it names none (0 of 33): it records that
  the server was runnable but not who held its CPU, the gap already listed
  for case 5. Fixed after the run (change 4 in the protocol): a chain hop
  now names the task that held the CPU at the end of a run-queue wait. In a
  second version of `ht` in `ipc` only (`build/catch/catch-ipc-holder`), it
  names `bgload` in 29 of 29, and in none of 88 noise hitches.
- **The summary tools lose rare hitches**: the cause was never among the ten
  heaviest lines, and its share of the summary rounded to 0.0% in every
  case. offwaketime and chaingraph never show `bgload` at all in `bgcpu` and
  `ipc`, where it preempts rather than wakes.
- **The raw recorders hold almost everything at 155 to 900 times
  hitchtrace's output** per injected hitch (ftrace 155–454, perf sched
  293–420, Perfetto 502–903, case by case). perf sched and Perfetto recorded
  every injected hitch. ftrace's flight recorder missed 5 of 77: four were
  slow frames 22 to 57 ms from another slow frame (below); one is a scoring
  edge case (below). Perfetto's trigger, fired by a watcher of the frame
  marks, sees the whole frame including present, and `CLONE_SNAPSHOT` copies
  the ring rather than swapping it, so every slow frame left a snapshot (479
  in all). Each snapshot is the whole ring: Perfetto's default 32 MB, which
  at about 150 KB/s of scheduler events here does not fill in a 120 s run, so
  each holds everything since tracing began.
- **No false culprits**: across 255 noise hitches in the three cases with a
  culprit, hitchtrace never named `bgload` or the disk.

Misses and weak spots, looked at one by one:

- `mem`, hitchtrace 11 of 12: in the one miss, 42 ms of the frame was the
  game writing pages out to swap inside reclaim, inside a swap-in. The kernel
  stack was cut at 32 frames (`HT_PERF_MAX_STACK`) before the page fault, so
  the rule's "stack is a page fault" did not match. The record's 18.6 ms of
  major faults and 14.6 ms of reclaim were not enough alone.
- `mem`, delayacct 0 of 9: the swap-in wait appears as **block I/O** on the
  main thread (8.7 to 31 ms per frame), while the swap-in delay itself is
  about 0.15 ms. The rule asked for swap-in delay, and was kept: block I/O
  alone does not tell a swap-in from a file read.
- `ipc`, delayacct 28 of 30 recorded: in two frames no game thread's delays
  changed (the main thread slept waiting for the server), so the tool wrote
  nothing.
- ftrace, four frames lost to a neighbour (1 in `iothread`, 3 in `ipc`; the
  reader counted 19 snapshots it never saved in `ipc`): a snapshot *swaps*
  the ring with the spare buffer rather than copying it, and the reader must
  save and clear the spare before the next swap. A second slow frame before
  that leaves the spare holding the old ring plus the newest events, with the
  frame between missing; one during the reader's copy is erased by the
  reader's clear afterwards. This test's reader widened the window: it saves
  each snapshot as text, which took up to about 250 ms for snapshots that
  reach 6 s back on quiet CPUs. A raw reader would narrow the window, not
  close it.
- ftrace, one `ipc` frame: the snapshot holds the frame, but its ring began
  just after the frame did, because the slow frame before it had triggered a
  snapshot at that moment and swapped in the empty spare as the ring. The
  rule asks the ring to begin before the frame; kept as is.
- "Attributed" noise hitches are mostly meaningless in `bgcpu` (hitchtrace
  44 of 55, delay accounting 32 of 72): 39 of hitchtrace's 44 were over
  budget by under 1 ms, where any short run-queue wait is "half the
  overrun". The culprit column is the one that measures false blame.

Not shown by these runs: real software; GPU stalls; other rarities (the
summary tools' ranks should fall with rarer hitches, not tested); repeat
runs; Perfetto in a session long enough to fill its ring.

## Next

- Future work, not scheduled: cut hitchtrace's cost for processes outside
  the game (track a task fully only once it is related to the game's wake
  chain), then rerun `pipe-other` and `pipe-target`, and the case studies
  whose chains cross processes (1, 2b, 5).
- Overhead on bare metal with `--preset full`, plus a disk-bound game for
  perf sched's write interference.
- Real software on bare metal (Proton and native free titles), then the
  catch-what-others-miss comparison on the same rare hitches.
- Blind diagnosis, and the prevalence study (Q8).
- Perfetto in a session long enough to fill its ring, for its output per
  hitch at steady state; and the per-event threshold sweep the paper's Q4
  asks for (`thousand_cuts` under `perf record --off-cpu-thresh` at several
  thresholds), now that a perf with BPF skeletons is built.
