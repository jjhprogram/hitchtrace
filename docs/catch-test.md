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

The measured run also ran `chaingraph`, a wake-chain profiler this repository
held until 2026-11-09; it is left out of these docs.

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

### `offwaketime`

It prints one summary for the whole run, so no hitch is recorded per frame.
Scored instead: is the cause visible in the summary? Each folded line is one
blocked stack with its wakers' stacks and its time; a line carries the
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
injected hitch; for offwaketime, visible or not and the signature's
share.

## Limits

- The development VM: no GPU stalls (lavapipe), a fast virtual disk.
- One rarity. A sweep (one injection per 2, 10, 60 s) would show how
  offwaketime's visibility falls with rarity; not run yet.
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

## Added tools

Written 2026-11-06, after the measured run and before these tools' own runs.
The rules above stay as they are; these tools are scored in runs of their
own, with the same cases, rarity and placement.

### `perfoffcpu`: `perf record --off-cpu --off-cpu-thresh 1`

`perf record --off-cpu -k CLOCK_MONOTONIC --off-cpu-thresh 1 -p PID`, with a
perf 7.0.14 built from the kernel source with BPF skeletons
(`build/perf/perf`; Ubuntu's perf has none, which `--off-cpu` needs). The
1 ms threshold is generous to perf: at its 500 ms default no wait in these
cases would be sampled at all.

What it records, from its BPF program (`util/bpf_skel/off_cpu.bpf.c`): for
each **blocked** interval (sleeping or uninterruptible) of a thread of the
process at least the threshold long, one sample when the thread runs again,
whose period is the interval's length (switch-out to switch-in, so it
includes the time spent queued after the wakeup) and whose stack is the
**user** stack it blocked in. Time spent preempted is never sampled, no
kernel stack is kept, and no waker is named. Intervals under the threshold
are summed per stack and written once at the end, with no time, so they
cannot be placed in a frame. `perf record` also samples on-CPU time as usual;
those samples are not scored.

- Recorded: a sample's interval overlaps the frame, and no lost-event record
  falls in the frame.
- Attributed, by case:
  - `iothread`: samples whose user stack contains `load_asset` or `pread`
    cover ≥ half the overrun within the frame.
  - `cpuspike`: no sample covers a quarter of the overrun within the frame.
  - `bgcpu`, `ipc`, `mem`: never. Preempted time is not sampled (`bgcpu`), and
    a sample of a thread waiting for a reply or for a page says nothing about
    why the server or the page was late (`ipc`, `mem`).
- Culprit: never; nothing in a sample names another task or an interrupt.

Check run `build/catch/check-perfoffcpu` (2026-11-06; `iothread` and
`cpuspike`, 20 s, one injection every 3 s): samples fall in the frames they
belong to on the game's clock, and `load_asset` appears only in load frames.
`perf script` prints the process's name rather than the thread's for every
off-CPU sample (the thread ids are right), so no rule uses thread names. No
rule was changed.

### `perfetto`: a flight recorder with a trigger

Perfetto v59.1's prebuilt binaries (`build/perfetto/v59.1/linux-amd64`, from
the google/perfetto GitHub release, checked against its SHA-256): `traced`
and `traced_probes` started for the run, and one session with
`sched/sched_switch`, `sched/sched_waking` and the layer's frame mark in a
ring buffer of Perfetto's default size (32 MB), process names from
`linux.process_stats`, on `CLOCK_MONOTONIC`, and trigger mode
`CLONE_SNAPSHOT`: each trigger saves a copy of the whole ring
(`perfetto-trace.N`) and tracing goes on.

Perfetto cannot see frames, so something outside it has to fire the trigger,
as a game or a system daemon would. Here a watcher reads the layer's frame
mark from a tracefs instance of its own and fires the trigger when the time
from one mark to the next is over budget: the same frames as hitchtrace's,
including present's own wait (which the ftrace recorder's in-kernel trigger
cannot see). The game is not changed. The watcher, the trigger and both
daemons run in one process tree, counted as the tool. The trace perfetto
writes when the session ends is not kept: a flight recorder's output is its
snapshots.

- Recorded: a snapshot holds the frame's own mark (within 1 ms of the frame's
  end in the game's log), and every CPU's oldest scheduler event in it is
  before the frame began. The ring is shared by all CPUs and full after its
  first seconds, so the second condition always applies (for the ftrace
  recorder it applies only when a CPU's ring overwrote events).
- Attributed and culprit: not scored, as for `ftrace` and `perfsched`: raw
  scheduler events hold the cause when they hold the frame, but say nothing
  about it until someone analyses them. Output per injected hitch is
  reported.

Check run `build/catch/check-perfetto` (2026-11-06; `iothread` and
`cpuspike`, 20 s, one injection every 3 s): every slow frame fired the
trigger and left a snapshot (14 in each run); the frame marks in the snapshots
agree with the game's log to within 85 µs, and every snapshot has events from
all 10 CPUs. Scheduler events arrive at about 150 KB/s here, so the 32 MB ring
fills only after about 3.5 minutes: in a 120 s run each snapshot holds
everything since tracing began and grows through the run, where in a longer
session it would hold the last few minutes. No rule was changed.

## Run

Measured run: `build/catch/catch-20261104-171550` (2026-11-04, all 30 runs
completed). No rule was changed for it after it ran; change 4 applies to a
rerun of a changed hitchtrace. Results and the misses looked at one by one:
[evaluation-status.md](evaluation-status.md#run-does-hitchtrace-catch-what-others-miss).

Second version, after change 4: `build/catch/catch-ipc-holder` (2026-11-05,
`ht` in `ipc` only, same rules otherwise). 29 injected hitches: 29 recorded,
29 attributed, 29 culprit. 88 noise hitches: 10 attributed, 0 culprit.

Added tools (2026-11-06, by the rules under [Added tools](#added-tools), each
alone in a run of its own; no rule was changed after these runs):

- `build/catch/catch-perfoffcpu`: 80 injected hitches, 80 recorded, 10
  attributed (all in `iothread`), 0 culprit; 0 of 348 noise hitches
  attributed. 1.3 to 3.7 MB per injected hitch.
- `build/catch/catch-perfetto`: 80 injected hitches, 80 recorded; 479
  triggers, 479 snapshots. 45 to 89 MB per injected hitch.
