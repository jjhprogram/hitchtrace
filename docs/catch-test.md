# Protocol: does hitchtrace catch what other tools miss?

Written 2026-11-04, before any run. The scoring rules below are fixed here so
that they cannot be fitted to the results; a change after the first run is
recorded under [Changes](#changes) with its reason.

## Question

When a game hitches rarely, which tools' output shows each hitch, and which
show its cause? Tools are run the way their documentation says, with their
output read mechanically by the rules below. What an analyst can dig out of
a tool's raw output by hand is the blind-diagnosis test's question, not this
one's.

## Hitches

Five of the case studies (`cases/run.sh`, `cases/README.md`), each made rare:
one injection every `RARE_S` = 10 seconds, for 120 seconds per run, so about
11 injections per run among the VM's own occasional slow frames.

| Case | Injection | Ground truth (not from any tool) | Culprit |
|---|---|---|---|
| `bgcpu` | `bgload` burns 20 ms on 4 threads on the game's CPUs | `bursts.jsonl`: the frame overlaps a burst by ≥ 1 ms | `bgload` |
| `iothread` | the streaming thread reads an 8 MB asset; the main thread waits for it | `frames.jsonl`: the frame loaded an asset | the disk (an interrupt ends the chain) |
| `ipc` | `bgload` bursts on the CPU of the server the game calls 30 times a frame | `bursts.jsonl`: the frame overlaps a burst by ≥ 1 ms | `bgload`, as the server's preemptor |
| `mem` | the game walks 2 MB of a world partly swapped out (cgroup `MemoryMax`) | `frames.jsonl`: the frame walked | none: no tool can say what caused the pressure |
| `cpuspike` | the main thread burns 12 ms (negative control: no kernel stall) | `frames.jsonl`: the frame burned | none |

An **injected hitch** is a frame that the ground truth marks as affected and
that the game's own log shows over budget (20.833 ms). A **noise hitch** is an
over-budget frame the ground truth does not mark. Only frames inside the
tool's tracing window count.

## Tools and runs

One run per case and tool, the tools in shuffled order, each run alone, so
that no tool's own cost or output (perf sched writes gigabytes) changes the
hitches another tool sees. The injection schedule is the same in every run;
the hitches are the same mechanism at the same rate, not the same frames.

| Tool | How it runs |
|---|---|
| `ht` | `hitchtrace`, as in the case studies |
| `delayacct` | `tools/overhead/delayacct_frames.c`: per-thread delay accounting per frame, a line per thread whose delays changed, for over-budget frames |
| `ftrace` | the flight recorder of `tools/overhead`, with `trace_clock` set to `mono` and each snapshot saved as text |
| `perfsched` | `perf sched record -k CLOCK_MONOTONIC` |
| `offwaketime` | bcc `offwaketime -f -p PID` |
| `chaingraph` | `chaingraph -p PID -f` |

## Scoring

Per injected hitch, three levels, each requiring the one before:

1. **Recorded**: the output has data covering that frame.
2. **Attributed**: the output itself puts the frame's extra time on the
   injected cause, with no further analysis.
3. **Culprit**: the output names the culprit in the table above.

A per-frame output is matched to a game frame by the frame's end time,
within 1 ms.

### `ht`

- Recorded: a record for the frame.
- Attributed, by case:
  - `bgcpu`: some game thread was runnable but not running for ≥ half the
    overrun (`runnable` + `runqueue`).
  - `iothread`: a stall group whose first hop is `mg_stream` blocked on I/O.
  - `ipc`: a stall group whose first hop is `mg_server`, which was runnable.
  - `mem`: page faults, reclaim, compaction and I/O waits whose kernel stack
    is a page fault come to ≥ half the overrun.
  - `cpuspike`: running is ≥ half the overrun and no stall group's stall
    time is ≥ a quarter of it (the case studies' rule).
- Culprit: `bgcpu` names `bgload` anywhere; `iothread`'s group continues to
  an interrupt; `ipc`'s first hop names `bgload` as the server's preemptor.

### `delayacct`

- Recorded: a line for the frame.
- Attributed: one delay type on one game thread is ≥ half the overrun, the
  type being CPU delay (`bgcpu`), block I/O (`iothread`), swap-in (`mem`).
  For `ipc` the server is another process, so no game-thread delay type
  stands for it: never attributed. For `cpuspike`, attributed means no delay
  type on any thread is ≥ a quarter of the overrun.
- Culprit: never; delay accounting has no names.

### `ftrace`, `perfsched`

- Recorded: `ftrace`: one snapshot that began before the frame did and holds
  the frame's own frame mark (within 1 ms of the frame's end in the game's
  log); if any CPU's ring in it overwrote events, every CPU's ring must also
  begin before the frame. (Changed after the check runs; see below.) `perfsched`: events before
  and after the frame, and no lost-events record between them.
- Attributed and culprit: not scored. Both give raw scheduler events, which
  hold the cause when they hold the frame but say nothing about it until
  someone analyses them; that is the blind test's question. Their cost to
  get there is reported instead: MB of output per injected hitch.

### `offwaketime`, `chaingraph`

Both print one summary for the whole run, so no hitch is recorded per frame.
Scored instead: is the cause visible in the summary? Each folded line is one
stack (offwaketime) or chain (chaingraph) with its time; a line carries the
case's signature if it contains:

| Case | Signature |
|---|---|
| `bgcpu`, `ipc` | `bgload` |
| `iothread` | `io_schedule` |
| `mem` | `do_swap_page` or `swap_read` |

**Visible** means the heaviest line with the signature is among the ten
heaviest lines of the summary. Also reported: the signature's share of the
summary's total time.

### Noise hitches

For `ht` and `delayacct`, the same attribution and culprit rules applied to
noise hitches: how often the tool blames the injected cause when it was not
at work.

## Reported

Per case and tool: injected hitches, then recorded / attributed / culprit as
counts of them; noise hitches and false attributions; output size and MB per
injected hitch; for the summary tools, visible or not and the signature's
share.

## Limits

- The development VM: no GPU stalls (lavapipe), a fast virtual disk.
- One rarity. A sweep (one injection per 2, 10, 60 s) would show how the
  summary tools' visibility falls with rarity; not run yet.
- One run per case and tool.

## Changes

All found in a short check run (`build/catch/check`: two cases, 20 s, one
injection every 3 s), before the measured run.

1. **delayacct was not accounting.** The kernel gives a thread its delay
   record when the thread is created, and only if `kernel.task_delayacct` is
   on then. The harness turned it on when the tool attached, after the game's
   threads existed, so only CPU delay (from scheduler statistics, always on)
   was reported: no block I/O delay even for a 31 ms asset load. Accounting is
   now turned on before the game starts. (The same fault affects the overhead
   pilot's delayacct figures; see `evaluation-status.md`.)
2. **The ftrace rule asked for the wrong span.** The snapshot a slow frame
   triggers is taken at its frame mark, where present starts, and the ring
   then starts empty. So that snapshot never runs past the frame's end, and in
   it a quiet CPU's first event comes after the frame started without anything
   having been lost. The rule asked both, and scored 0 of 6 frames recorded
   that the snapshots did hold. It now asks for the frame up to present, and
   asks every CPU's ring to begin before the frame only when some ring
   overwrote events. Present itself is outside this tool's frame by design
   (see `tools/overhead/README.md`).

   A second check run (`build/catch/check2`) showed that rule wrong too: it
   took "up to present" as the last event's time, but the game's frame limiter
   can wait for milliseconds without any scheduler event, so the last event
   came 3 ms before present in frames the snapshots did hold. The flight
   recorder now also records its two frame-mark events (`fmark`, `pend`; two
   events a frame, against thousands of scheduler events a second), which an
   analyst needs to find frames in a snapshot anyway, and the rule asks for
   the frame's own mark.

   A third (`build/catch/check3`) found the marks 7.1 ms early against the
   game's log, by the same amount every frame: the snapshots were not on
   CLOCK_MONOTONIC, although `trace_clock` had been set to `mono`. hitchtrace,
   perf sched and delayacct all agree with the game's clock within 1 ms. The
   reader now writes CLOCK_MONOTONIC into the trace (`trace_marker`) ten times
   a second and the scorer puts each snapshot on the game's clock by it; the
   clock in effect is saved in `tool.json` (`trace_clock`). In the next check
   (`build/catch/check4`) that clock was `global`, not the `mono` written to
   it; with the calibration, each snapshot's offset (about 5.9 ms) varied by
   under 0.1 ms, and all 6 injected hitches scored as recorded.
3. **Tools' own processes moved off the game's CPUs** (`TOOL_CPUS=6-9`, the
   game on 0-3, the ipc server on 5). Formatting each ftrace snapshot as text
   on the game's CPUs made slow frames of its own: 25 noise hitches against
   about 10 with the other tools. The text is this test's addition, not part
   of the flight recorder, so it should not cost the game; the same placement
   applies to every tool, hitchtrace included.

4. **After the measured run: hitchtrace names who held a queued task's CPU**
   (2026-11-05). The run found `ipc`'s culprit named in 0 of 33 hitches: the
   server is asleep when `bgload` takes its CPU, so it is never preempted,
   and hitchtrace named a CPU's holder only for a preemption. A chain hop now
   also names the task that held the CPU at the end of a run-queue wait of
   at least 100 µs, with the flag `queued`. The `ipc` culprit rule is
   unchanged in form (the first hop's `preemptor` names `bgload`) but now
   accepts that holder. Only `ht` in `ipc` is rerun, as a second version;
   the measured run's 0 of 33 stands.

## Run

Measured run: `build/catch/catch-20261104-171550` (2026-11-04, all 30 runs
completed). No rule was changed for it after it ran; change 4 applies to a
rerun of a changed hitchtrace. Results and the misses looked at one by one:
[evaluation-status.md](evaluation-status.md#run-does-hitchtrace-catch-what-others-miss).

Second version, after change 4: `build/catch/catch-ipc-holder` (2026-11-05,
`ht` in `ipc` only, same rules otherwise). 29 injected hitches: 29 recorded,
29 attributed, 29 culprit. 88 noise hitches: 10 attributed, 0 culprit.
