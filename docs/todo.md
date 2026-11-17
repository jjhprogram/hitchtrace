# Open work

Everything still to do, in one list. When an item is done, delete it here
and record it in [history.md](history.md) if it changed the design, or in
[evaluation.md](evaluation.md) if it produced a result. The paper keeps its
own `\todo{}`/`\tbd{}` marks (about 20; `grep -n '\\todo\|\\tbd' paper/*.tex`),
and those are not repeated here.

## The tool

What the records cannot say yet, each with the case that showed it
(`cases/README.md`):

- **How long a sleep asked for** (case 4 `slack`; the Linux 7.0 clockevents
  regression in [real-world-cases.md](real-world-cases.md)). A late frame-limiter sleep looks
  the same as a long one.
  - Read the request at `sys_enter`: `clock_nanosleep`'s flags (absolute
    or relative) and clock id, or `nanosleep`'s duration.
  - Report the overshoot as a field, not a new bucket, so the partition
    stays intact.
- **On-CPU kernel stalls as stall groups** (cases 8 `alloc`, 9 `compact`).
  Fault, reclaim and compaction time are buckets, never ranked. The top
  group is then the ordinary wait for the workers, and a reader who goes
  straight to it is sent the wrong way. These need a group of their own,
  with the kernel stack that ran.
- **Which CPU, and whether other allowed cores sat idle** (case 7 `squeeze`;
  scx #3739). Record the CPU with every logged interval, and the idle
  allowed cores when a thread is made to wait.
- **Who else was using the disk** (case 2b `iodisk`). A slow disk and a busy
  disk give the same record.
  - Take block-layer data only while a target thread waits on I/O
    (`block_rq_insert`, `block_rq_issue`, `block_rq_complete`): queued time
    against device time, and who else had requests in flight.
  - A cgroup throttle delays requests before the device queue, so that stage
    counts too.
- **The holder that held the CPU longest, not the last one** (cases 1 and 5).
  - The last holder was a game thread or a kworker in 10% (`bgcpu`) and 2%
    (`ipc`) of frames.
  - A holder on a CPU the task migrated from is not seen.
  - A game thread's own run-queue wait still names nobody; only chain hops
    do.
- **Compaction's outcome** (case 9). Record whether compaction failed and the
  fault fell back to small pages: the end of compaction carries its result.
- **ntsync and split-lock waits** land in `block_other`. Refine them by
  kernel stack in the front end, or by ioctl command at switch-out
  (`NTSYNC_IOC_WAIT_ANY`/`_ALL`). This matters for Proton 11, which is
  ntsync-first.
- **IRQ and softirq time, and CPU frequency.** Both are hidden in `oncpu`.
  Real cases: fTPM stalls, frequency ramp after short sleeps
  (`epp_boost`), GameMode's governor.
- **The cause gate.**
  - Move the "resolved gate" (subtract `resolved_wait_oncpu`) from offline
    into the BPF program.
  - Add a minimum-overrun floor: the ratio test passes 150 µs of jitter
    because its denominator is tiny.
  - `stall_ns` still counts the frame limiter's sleep.
- **What made this frame different** (case 2). The ranking by total puts
  each frame's normal waits (the wait for the workers, the limiter's sleep)
  above a short load. Ranking against a per-group baseline of normal frames
  would fix it, but normal frames leave nothing behind. A person judging the
  hitchview page was chosen instead; revisit only if blind diagnosis shows a
  need.
- **Cost on other processes:** about 410 ns per context switch in other
  processes, among the highest of the tools measured. Track a task fully only once it touches the game's wake
  chain; then rerun `pipe-other`, `pipe-target` and the cases whose chains
  cross processes (1, 2b, 5).
- **The layer.**
  - Build an i386 copy for 32-bit Proton titles (without
    `PROTON_USE_WOW64=1` they run 32-bit and get no frame marks).
  - Wrap `vkAcquireNextImageKHR` and present-wait, where some WSI stacks
    throttle outside the present bracket.
- **Check:** whether a preempting thread's line names its process. In an
  early `bgcpu` run the top preemptor was `llvmpipe-N`, a thread of the
  compositor, shown by thread name only.

## Runs

- **Bare metal**, by [bare-metal-plan.md](bare-metal-plan.md):
  - overhead with `--preset full`;
  - real Proton and native titles;
  - the catch test on them.
- **Rerun the overhead pilot's delayacct arm.** Its figures are invalid
  (accounting was switched on after the game's threads existed).
- **perf sched's disk writes:** 10 to 25 GB/hour, measured with a
  disk-bound game.
- **A rarity sweep** for the catch test: one injection per 2, 10 and 60 s.
- **Repeat runs:** every catch and case result so far is one run.
- **Perfetto in a session long enough to fill its ring**, for its steady-state
  output per hitch, and the critical path's steady-state cost per snapshot
  (`tools/catch/critpath.py`).
- **The per-event threshold sweep the paper asks for:** `thousand_cuts`
  under `perf record --off-cpu-thresh` at several thresholds.
- **Optional: rerun vkcube on the current version**, so that every paper
  number comes from one version. `tests/out/vkcube_*.jsonl` (the paper's
  cause-gate numbers) were recorded with the September prototype (`65c66fc`).
  Checked 2026-11-09:
  - the paper's numbers match that data (96.8% `block_present`; `block_poll`
    in all 223 records without the bracket; a 147 µs median overrun of the
    25 gated frames);
  - no later commit changed the cause rules, the present bracket, the
    syscall classes or the gate, which are all those numbers depend on.

  A rerun should give the same picture, within this VM's noise.
- **Blind diagnosis:** someone diagnoses rare hitches from each tool's output.
- **Prevalence** (the paper's Q4): what share of real hitches are
  kernel-observable, by cause.

## The paper

- **"Record but do not explain" against the critical-path run**
  ([evaluation.md](evaluation.md#perfettos-critical-path-on-each-snapshot),
  2026-11-09). Perfetto's own critical path, run on each snapshot with no
  one in the loop, attributed 68 of 80 injected hitches. Its output named
  0 of 56 culprits, and one join named all 56. The claim in the paper's
  prose and the comparison table's rows ("Offline critical path": "by
  hand", "--"; flight recorder: "--") predate this run. The source for
  any number taken from it is `build/catch/catch-perfetto/critpath.md`, to
  be added to `paper/README.md` when the paper uses it.
- **Related work: latency-tracker's rt tracker** (checked 2026-11-09) is the
  closest precedent for online, deadline-scoped wake chains, and the paper
  cites latency-tracker only as a flight recorder.
  - What it is: a kernel module, `latency_tracker_rt` (`trackers/rt.c` in
    `efficios/latency-tracker`, © 2015 Julien Desfossez).
  - What it does: it follows one interrupt online from `do_IRQ` (or
    softirq, hrtimer) through `sched_waking` and switch-in to the next task
    that task wakes. It keeps a per-step time breakdown, and branches when
    one event wakes several tasks.
  - Where it stops and what it emits: tracking ends when the target
    process (`procname`) starts running, enters user space, or writes a
    `work_done` cookie. Above a `threshold` it emits a tracepoint with the
    breakdown and wakes a pipe, which a script uses to take an LTTng
    snapshot.
  - Cost: about 740 ns per state change, 4 to 6 state changes per
    interrupt.
  - Sources: the LTTng blog post "Monitoring real-time latencies"
    (Desfossez, 6 January 2016, https://lttng.org/blog/2016/01/06/monitoring-realtime-latencies/)
    and the code. The SP&E 2016 paper already in `references.bib`
    (`desfossez2016latency`) presents the framework.
  - The difference to state: it starts from an interrupt and follows the
    chain forward to one named task. hitchtrace starts from a frame's waits
    and follows each back to its cause, sums many waits per frame,
    partitions the frame by cause, and runs as verified BPF rather than a
    module.

  Also:
  - `scx_lavd`'s latency-criticality score, passed between waker and wakee
    online: adjacent, no chain;
  - Windows WPA ready-thread analysis and UIforETW's circular buffer.
- **Gregg's "yet".** Gregg named latency outliers in a parenthetical under
  "future" as a problem chain graphs do not address, because they sum over a
  run. Cite it as motivation, not as an open problem the paper solves.
- **Framing for the motivation:**
  - publicly root-caused hitches are biased toward the ones that were
    reproducible, toggleable or known to a vendor engineer, which are
    exactly the ones a frame-scoped tool is not needed for;
  - a count of cases is not a share of frames, so the catalogue's "a third
    to a half in scope" must not stand in for prevalence;
  - "this frame was not a kernel stall" is a result: it sends the developer
    to the CPU or GPU profiler.
- **Overleaf.** The Overleaf project has edits of yours to `00_abstract.tex`
  and `03_motivation.tex` that the local copy lacks. You chose the local
  copy (2026-11-08); uploading `build/paper-overleaf.zip` overwrites them.

## Citations to check

Before they reach the paper's text:
- **The LPC 2025 "Gaming on Linux" recording and slides** (Min, Vernet),
  instead of LWN's paraphrase; quote from the recording.
- **VarMRI** (arXiv 2601.10572), read in full.
- **Whether an ftrace histogram variable can hold a running sum** reset by a
  third event (the paper's ftrace table, "verify on 7.0").
- **Proton specifics:**
  - DXVK's and vkd3d-proton's presenting threads;
  - that DXVK resolves through `vkGetDeviceProcAddr`;
  - that Wine copies Windows thread names into `comm`.
- **The BPF feature version list**, against the kernel changelogs.
- **`references.bib` entries marked `VERIFY`** (13).
- **The real-world cases:** the corrections listed at the top of
  [real-world-cases.md](real-world-cases.md).

## The repository

- **Unpushed commits** on `hitchtrace-prototype`.
- **Data in `build/` that nothing refers to:**
  - `build/catch/catch-20261104-162510` (1.8 GB, an earlier catch attempt);
  - `build/overhead/pilot-20261103-103426` (182 MB, the spoiled pilot);
  - `build/src` (1.9 GB, kernel source, needed only to rebuild perf).
- **`cases/out/`** is tracked in git (82 MB); `cases/out/baseline/` is an
  untracked run.
- **`cases/minigame/`** is a folder holding one file while the background
  programs sit loose in `cases/`. Deferred.
