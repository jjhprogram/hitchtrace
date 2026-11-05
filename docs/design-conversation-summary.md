# hitchtrace design conversation: summary

Source: claude.ai shared chat `e8ed3517-0033-4883-b43f-1cc7f37ec5c8` (Sep 7 – Sep 19, 2026). The chat produced `hitchtrace-implementation-plan.md`.

> **Gaps in the shared copy.** The share hides attached files. That hides two things: the opening prompt, which described the project idea and a list of search areas, and a summary of a separate Claude Code session that the user pasted on Sep 7–8. What those contained is inferred below from Claude's replies.

---

## TL;DR: where the conversation ended up

- **Verdict: (b), close prior work exists; the novelty is narrow but real.** Every piece exists somewhere. Nobody has combined them for unmodified Linux games:
  - present-call frame boundaries;
  - in-kernel per-frame stall accumulation that emits only over-budget frames;
  - kernel-cause attribution with a cross-thread wakeup chain.
- **Not the contribution:**
  - "eBPF reduces kernel↔user data movement." This is the founding pitch of BPF tracing, and ftrace can also emit compact, selective records.
  - Selective or triggered capture. Flight recorders solve it: ftrace snapshot, Perfetto `STOP_TRACING`, LTTng + latency-tracker, Hindsight, DTrace speculation.
  - The chain-walk algorithm, which is Giraldeau & Dagenais critical path, also shipped as Perfetto's `thread_executing_span`.
- **The defensible contribution:**
  1. **Frame-scoped accumulation with a deferred gate.** The commit/discard decision is made at frame end over state accumulated across the whole frame. This catches "death by a thousand cuts" hitches that per-event thresholds cannot. ftrace's declarative language can't express it.
  2. **Cause-gated emission.** Frames are emitted only if kernel stalls explain the overrun, not merely because the frame was long.
  3. **On-demand, frame-scoped wakeup chains for outlier frames.** This was stated publicly as an open problem at Linux Plumbers 2025 (Min / Vernet), and it is the "latency outliers" case Gregg's 2016 chain-graph work left open.
  4. **Measurements:**
     - how prevalent kernel-observable hitches are in Linux games (M0);
     - how much is lost at each summary level (M1);
     - aggregate predicate vs per-event threshold (M2);
     - perturbation and output volume vs a flight recorder (M3).
- **Positioning:** a bench instrument for platform, scheduler and Proton/Mesa engineers (Valve, Igalia, CachyOS, sched_ext developers). It is not a consumer tool. It has a real user base on Linux because Linux gaming has a vertically integrated platform owner (Valve) who can act on kernel-cause data.
- **Environment:** develop in a Hyper-V VM with lavapipe, then evaluate on a rented Hetzner AX42 (Ryzen 7 PRO 8700GE with Radeon 780M iGPU). Deck-class limits are imposed with cgroups.

---

## 1. Initial prior-art search (Sep 7)

### Project as understood from the hidden prompt
Three components:
- **C1:** a uprobe on the present/swap call (e.g. `vkQueuePresentKHR` in `libvulkan.so`) marks frame boundaries without modifying the engine.
- **C2:** in-kernel, per-frame accumulation of stall time; a record is emitted only for frames that exceed the budget.
- **C3:** kernel-cause classification plus a cross-thread critical path.

### Verdict
**(b) Close prior work exists; the novelty is narrow but real.** No single tool does all three on desktop Linux. Each component exists separately. The design pattern behind C2 is about 20 years old and has been reimplemented at least five times under different names.

### Closest prior work

| # | Work | What it does | Overlap |
|---|---|---|---|
| 1 | **hwui_mon** (LibXZR, May 2023) + **frame-analyzer-ebpf** (shadow3aaa, crates.io, 2024–Oct 2025) | Android only. A kernel module places uprobes in libhwui's frame path and fires only when frame time ≥ a jank threshold. A Rust crate tracks Android frametime with eBPF + uprobes. | C1 fully (on Android). C2 partially: threshold gate at the frame boundary, but no stall accumulation. No C3. |
| 2 | **VarMRI** (Zhou et al., Ohio State, arXiv 2601.10572, Jan 2026) | Per application request, records cumulative kernel events (scheduling waits, interrupts, faults) and HW counters per thread, written out at request end. This avoids the 104 MB/s of full tracing for Memcached. Needed a 563-line kernel patch because kprobes/eBPF lacked the hooks it needed (e.g. `do_page_fault`). | Strong on C2: the same "accumulate per unit of work in kernel, emit one record at the boundary" design, with requests instead of frames. Partial C3. No C1. **The single most damaging item for the "in-kernel aggregation" framing.** |
| 3 | **latency-tracker** (Desfossez, Desnoyers, Dagenais, SP&E 2016) and lineage | A kernel module tracks latency between a punch-in and a punch-out event. With LTTng snapshot or ftrace, it extracts the trace only on unusual latency. Predecessors: DTrace speculative tracing (2005), ftrace hist triggers, LTTng 2.13 triggers. | C2's pattern, thoroughly. No frames, no C3. |
| 4 | **Linux delay accounting** (delayacct/taskstats, `delaytop`) + **PSI triggers** | The kernel already accumulates per-task stall time by cause: CPU delay, block I/O, IRQ, swapin, freepages reclaim, thrashing, compaction, write-page-copy. PSI can wake user space when cumulative stall exceeds a threshold within a 500 ms–10 s window. | C3's taxonomy almost exactly, in kernel, always on. Futex is the missing category. **Second most damaging:** a Vulkan layer that diffs taskstats at each present gets most of C3 with no BPF. |
| 5 | **Android Perfetto stack**: FrameTimeline, `sched_blocked_reason`, critical-path stdlib, triggers | FrameTimeline classifies jank source. `sched_blocked_reason` reports the function responsible for an uninterruptible sleep. `thread_executing_span` builds a wakeup graph. `STOP_TRACING` gives flight-recorder mode. JankStats reports only frames above 2× the refresh interval. | C1 via SurfaceFlinger. C3 but offline. C2 as a flight recorder, not an in-kernel predicate. |
| 6 | **NVIDIA Nsight Systems** | Vulkan frames are delimited at `vkQueuePresentKHR`. A "Frame Health" row flags actions whose per-frame accumulated time far exceeds the rolling median of the surrounding 19 frames. Blocked-state backtraces are taken only for OS-runtime calls above a threshold. Linux context-switch tracing since 2022.1. | C1 (by interception). C3 partially. No C2 (full trace, offline). **The closest shipping thing to frame-granular stutter diagnosis on Linux.** |
| 7 | **perf `--off-cpu` / `--off-cpu-thresh`** (Namhyung Kim 2022; Howard Chu 2025) and bcc off-CPU tools (Gregg 2016) | Off-CPU time aggregated in a BPF map. `--off-cpu-thresh` (default 500 ms) emits a direct sample. `offwaketime` combines off-CPU and waker stacks in kernel. `runqslower` prints only over-threshold latencies. | C2 as per-event thresholds, not per-frame accumulation. Partial C3. No C1. |
| 8 | **Giraldeau & Dagenais** (IEEE TPDS 2016), **wPerf** (OSDI 2018) | Critical path recovered recursively across threads and machines from kernel traces. wPerf builds a wait-for graph at about 5.1% recording overhead. | The critical-path part of C3, offline over full traces. |

**Also surfaced:**
- rtla timerlat/osnoise;
- Hindsight (NSDI '23, retroactive sampling);
- Superluminal (now on Linux);
- GamePulse/RigSignal (May 2026, gaming telemetry with separate eBPF metric streams).

### Component-by-component
- **C1: already done in essence.** Nsight, MangoHud (an overlay layer with no kernel data) and the Android uprobe tools all do it. A uprobe on `libvulkan.so` is an engineering choice.
  - Flagged risk: the loader export is a trampoline, and `vkGetDeviceProcAddr` users bypass it. A layer with a USDT probe, or an uprobe on the ICD, would be more robust.
- **C2: the pattern is done; this instance is new.** Keyed by thread, scoped to a uprobe-defined frame, held in BPF maps, on desktop Linux: that combination was not found. "Real but small."
- **C3: partially covered.**
  - Classification is done in kernel by delayacct and in traces by `sched_blocked_reason` / offwaketime.
  - The critical path exists only offline.
  - **An online, in-kernel, frame-scoped critical path was not found.** This is the most defensible novelty and also the hardest part, given verifier limits on graph walking.

### Coverage notes
- Continuous profilers really are samplers: Parca and the OTel eBPF profiler sample at 19 Hz per CPU; Google-Wide Profiling is sampling-based. Parca's `--off-cpu-threshold` is a probability.
- gpuvis is a manual ftrace flight recorder. SteamVR binds capture to F6. There is no threshold logic.
- Nothing found: no ACM/IEEE/USENIX paper, LPC or eBPF Summit talk, or kernel-list thread on eBPF frame-aware kernel attribution for games. GDC was unchecked when the search budget ran out.

### Strongest argument that the project is redundant (steelman)
- Perfetto on Linux plus a 200-line Vulkan layer that fires a missed-frame `STOP_TRACING` trigger reproduces the user-visible outcome with no kernel code.
- If the objection is that Perfetto perturbs frame timing, VarMRI has already published per-request in-kernel accumulation, and delayacct has done per-thread cause accounting since 2.6.
- Nsight already marks frames at present and flags per-frame outliers.
- Under this reading the project is "delayacct sampled at present calls," a weekend script, with a critical-path reimplementation inside a verifier-constrained VM.

### Recommended narrowing
- **Lead with measurement.** The unproven claim is that in-kernel aggregation reduces observer effect compared with a ring-buffer flight recorder. Quantify p99 and 1% lows for Perfetto/ftrace vs BPF aggregator vs delayacct polling on Deck-class hardware.
  - David Vernet's `epp_boost` work shows current appetite for this kind of evidence: brief game-thread sleeps were misread as low demand, and the fix gave +31.8% 1% lows on Steam Deck.
- **Position C2 as "frame-scoped delay accounting,"** a descendant of delayacct + latency-tracker + VarMRI. Claim the uprobe-bounded, thread-keyed interval with zero bytes for good frames, plus futex attribution, which delayacct lacks and which matters for Proton fsync/ntsync.
- **Scope C3 to "critical-path-lite":** the presenting thread only, with a one-hop waker.
- **Fix the hook:** validate against direct-dispatch apps and DXVK; consider a layer-emitted USDT probe.
- **Evaluation:** use Perfetto on Linux as attribution ground truth, and Nsight/GamePulse as baselines.

---

## 2. Addendum after "Continue" (Sep 7)

The verdict stayed (b), but the "real" half got stronger.

### Verified
- **Loader bypass is real.**
  - LunarG loader docs: querying device functions via `vkGetDeviceProcAddr` removes the loader from the call chain; with no layers enabled, the pointers go straight to the driver.
  - RenderDoc's layer guide says the same.
  - A uprobe on the loader's `vkQueuePresentKHR` therefore misses engines that use the recommended fast dispatch (volk users; likely DXVK, unverified).
  - Fixes: (a) uprobe the ICD's own present (RADV, ANV), or (b) ship a trivial implicit layer that exports `vkQueuePresentKHR` with a USDT probe. A layer can also sit next to MangoHud, which is already present on every Deck.
  - **This must be fixed before writing any BPF, and it is itself a finding.**
- **LPC 2025 "Gaming on Linux" microconference** (Dec 2025; LWN 1051430, Jan 2026):
  - **Changwoo Min** (Igalia, scx_lavd author): problems occur rarely (a spike every ~5 minutes). He couldn't trigger a trace on them, fell back to brute-force sampling every 10 s, and called the correlation between the high-level FPS view and the microscopic kernel view the missing piece.
  - **David Vernet** (Meta) asked whether anyone had analyzed the whole chain of tasks from first wakeup to last completion to derive deadlines and frame rates. Min said no.
  - These are C2 and C3, stated as open problems by practitioners. Min's VaporMark post-processes `perf sched record` for an hour or two, which is the full-trace approach.
  - **The strongest single citation for motivation.**
- **systing** (Josef Bacik, 2025; 1.0 Feb 2026), added as prior-work item 9:
  - A libbpf tracer that records all scheduling events for a system, cgroup or process to Perfetto, with stacks on uninterruptible sleep and configurable uprobes/USDT.
  - The author's first complaint is too much data; 1.0 moved to DuckDB output.
  - It is the Linux-desktop equivalent of Perfetto on Android and is used by sched_ext developers.
  - It has no C2 and no C3 taxonomy or critical path.
  - **The natural output target:** emit Perfetto/DuckDB records so the tool complements systing instead of competing with it.
- **Mesa** 23.3 added CPU tracing for `vkWaitForFences` and more wait tracepoints, plus gpuvis integration and experimental Perfetto support. These are trace events, not per-frame accumulation.
- **SteamOS telemetry** (Piccoli, Igalia, LPC 2025): OOM and split-lock events via tracing instances, GPU and gamescope traces, sent through steamos-log-submitter to Sentry. Nothing frame-scoped, but a per-hitch record would fit this pipeline.
- **Tracy** needs a `FrameMark` in the frame loop, i.e. instrumentation. It captures context switches but has no taxonomy.
- **Intel GPA** captures a few seconds manually. It is not always-on.
- **GamePulse** was confirmed as separate streams: frame timing comes from MangoHud, the eBPF stream carries scheduler/IO/fence metrics, and correlation happens in Kibana.
- **In-kernel critical path is still open.** The only attempt is Gregg's 2016 chain-graph prototype, which was limited by the 512-byte stack and kernel-only stacks. Gregg noted that chain graphs "don't yet" address latency outliers. It was never shipped, and nobody has scoped it to a frame.

### Still unverified
frame-analyzer-ebpf's exact hook symbol; whether DXVK uses `vkGetDeviceProcAddr`; GDC prior art.

### Assessment changes
- **C1:** downgraded to "already done, and your specific mechanism is fragile."
- **C2:** unchanged, now with a documented user for whom Perfetto and periodic sampling failed.
- **C3:** frame-scoped wakeup chains are now a named open problem.

### Revised steelman
"Add a missed-frame trigger to systing." Min's testimony argues for a trigger (a flight recorder) at least as much as for in-kernel aggregation. **You still have to show that aggregation buys something the trigger doesn't.**

### Revised reframing
1. Open with the LPC exchange as the problem statement. Cite VaporMark's hour-long post-processing as the cost of the status quo.
2. Build the hook as a USDT layer or an ICD-symbol uprobe, and report the loader bypass as a result.
3. Run a three-way comparison (systing/Perfetto ring buffer + `STOP_TRACING`; delayacct polled at frames; the in-kernel accumulator) on a synthetic "spike every N minutes" workload. Measure capture, p99/1%-low perturbation, and bytes to user space. If the trigger wins, that is still a thesis.
4. Implement a one-hop, frame-scoped critical-path-lite as a first step toward Vernet's question.
5. Emit Perfetto/DuckDB-compatible records, because the LAVD developers already live in those tools.

---

## 3. Verdict summary (Sep 7)

- Nobody has built the combination on desktop Linux: that is the "real" part.
- Every piece exists separately: that is the "narrow" part.
  - Frame hooking exists in Nsight and the Android tools, and the loader uprobe is fragile.
  - Accumulate-and-emit-if-abnormal is a 20-year-old pattern: DTrace, latency-tracker, ftrace hist, PSI, perf, VarMRI.
  - Classification exists in delayacct. Critical path exists offline.
- **Open:** an online, in-kernel, frame-scoped critical path, backed by the LPC 2025 testimony.
- **Biggest risk:** "systing/Perfetto + missed-frame trigger gets the same result." The thesis must measure whether aggregation lowers observer effect compared with a ring-buffer flight recorder. Framed as that measurement plus a one-hop critical path, it is defensible in three months. Framed as an "eBPF frame profiler," it reads as a reimplementation.

---

## 4. "The focus is eBPF: decide in kernel, reduce data movement, use eBPF's flexibility and security." Does that change the verdict? (Sep 7)

**No, and the framing moves the least novel part to the centre.** Deciding in kernel to reduce data movement is the founding rationale of DTrace aggregations (2004), every bcc tool, and `perf --off-cpu`. Reviewers will read it as a premise.

**What the eBPF framing does buy:**
- Separation from the kernel-module/patch lineage: latency-tracker (module), hwui_mon (module), VarMRI (a 563-line patch after its eBPF attempt failed). "The same analysis, verifier-safe, loadable on a stock SteamOS kernel" is a modest but real engineering distinction.
- It does *not* separate the project from other eBPF tools (perf, bcc, systing, frame-analyzer-ebpf, GamePulse).
- The security claim only beats kernel modules. Perfetto, ftrace and gpuvis run no custom kernel code at all.

**Two problems the framing creates:**
1. **The premise is testable and there is contrary evidence.** BPF handlers still run on every `sched_switch`, futex and reclaim event. ftrace's ring-buffer write is cheap. VarMRI found higher overhead than plain ftrace even though its trace was smaller. You must measure perturbation against a flight recorder, and the trigger approach may win on overhead while losing on capture guarantees.
2. **Expressibility under the verifier is the real eBPF research question.** VarMRI couldn't do it in eBPF, and Gregg hit the stack limit. Frame-scoped accumulation across N threads, nested preemption, and multi-hop chains all fight the verifier: bounded loops, a 512 B stack, no allocation, lookup costs.

**Replacement claims:**
- verified-BPF expressibility of frame-scoped attribution;
- perturbation compared with a flight recorder.

"The data-movement argument is Brendan Gregg's."

---

## 5. "If ftrace's ring buffer is in the kernel, what does eBPF actually give us?" (Sep 7)

**The buffer's location isn't the advantage; computation at the event is, and only in certain regimes.**

**Where ftrace's cost actually is:**
1. The per-event write, tens of nanoseconds per fire.
2. The user-space consumer that must drain the buffer, competing with the game and generating scheduler events of its own.
3. Storage and offline processing (VaporMark's hours).

In overwrite (flight-recorder) mode, costs 2 and 3 disappear in steady state. So against a properly configured flight recorder, "eBPF reduces data movement" is nearly false. It is only true against the streaming setups practitioners actually use: `perf sched record`, `trace-cmd record`, Perfetto long-trace mode.

**What eBPF gives:**
- **Stateful logic across events.** Per-thread accumulators in maps, and a predicate evaluated at the present uprobe. Hist triggers do a narrow version, but not arbitrary arithmetic, cross-thread sums, or wakeup chains.
- **Kernel state ftrace never emits:** `task_struct` fields including delayacct counters, `wchan`, blocked stacks for game threads only, uprobe arguments.
- **Output proportional to hitches.** A flight recorder dumps megabytes per trigger and degrades as hitches get more frequent. An accumulator emits about 100 B per bad frame. *(Corrected in §7: ftrace can also produce compact records.)*
- **Bounded static memory with no wraparound loss.**

**What eBPF does not give:**
- **Lower per-event cost.** VarMRI saw a smaller trace but higher per-event overhead.
- **Post-hoc flexibility.** An aggregator answers only the questions compiled into it; unknown unknowns are lost.
- **Security over ftrace/Perfetto.**

**Implication:** the strongest design is **DTrace speculation reimplemented in BPF**:
- buffer the frame's raw events in a bounded per-thread map;
- evaluate the predicate at present;
- copy to the ring buffer only for over-budget frames.

That gives the aggregator's scaling plus the flight recorder's detail, and the justification for eBPF becomes "the commit/discard decision needs frame-scoped state only a program at the event can hold." Measure it against the flight recorder and delayacct. If it doesn't beat both on at least one axis, it's a negative result; find that out by week 4, not week 10.

---

## 6. "Why can't ftrace decide what to write in the first place?" (Sep 7)

**It can, up to a well-defined point, and that point is where the contribution boundary should sit.**

**What ftrace decides in kernel:**
- **Stateless filters** on an event's own fields.
- **Triggers:** `snapshot`, `stacktrace`, `traceoff`, `enable_event`, with a filter and count.
- **Histogram triggers with variables + synthetic events.** Save a timestamp keyed by pid, compute a delta on a later event, fire a synthetic event carrying it, and attach actions such as saving context at a new max.

**The frame gate needs zero BPF:**
```
p:vk/present /usr/lib/libvulkan.so.1:vkQueuePresentKHR      # uprobe event
frame_lat u64 lat; pid_t pid                                 # synthetic event
hist:keys=common_pid:ts0=common_timestamp.usecs               # on vk/present
hist:keys=common_pid:lat=common_timestamp.usecs-$ts0:onmatch(vk.present).frame_lat($lat,common_pid)
snapshot if lat > 16600                                      # on synthetic/frame_lat
```
This is an overwrite ring buffer where the kernel decides at present time and dumps only bad frames. There is no BPF and no user-space trigger latency. The loader-bypass caveat still applies.

**Where it runs out:**
- **Scoping and reset:** there is no "reset this thread's accumulator when present fires," only cumulative sums that user space must diff.
- **Aggregation across keys:** no "sum stall time over all threads now."
- **Kernel state that isn't an event field:** `task_struct`, conditional stack capture, uprobe-argument struct walks.
- **Graph state:** chains need per-edge state and multi-hop lookups.
- **Speculative buffering:** no per-thread commit/discard, only whole-buffer snapshots.
- **Compact computed output:** synthetic events carry a fixed field list.

**Why it stops there:** this is deliberate. ftrace's language is kept declarative (filter → trigger → histogram) because arbitrary user logic in kernel context is exactly what BPF and its verifier exist to handle. The two systems share hooks and differ only in what runs when a hook fires.

**Advice:**
- Build the ftrace version first. It takes an afternoon and gives a no-BPF baseline with full context.
- Then produce a table of which pieces of logic the DSL could express and which forced BPF.
- The eBPF claim becomes: "frame-scoped, cross-thread stall attribution with a commit/discard decision, which exceeds ftrace's in-kernel decision language."

---

## 7. "So the output-proportional-to-hitches claim doesn't hold?" (Sep 7)

**Correct, that paragraph overstated it.** Scaling separates *computed compact records* from *raw-event snapshots*, not eBPF from ftrace.

Three regimes:
1. **Snapshot on a frame predicate** (ftrace sketch, Perfetto, gpuvis, latency-tracker): megabytes per trigger, and the snapshot must be drained before the next one. The scaling argument holds against this.
2. **Per-event thresholded synthetic events in ftrace** (`lat > 16600`, `lat > 500` on a futex wait): tens of bytes each. **Compact output proportional to hitches is achievable without BPF** for anything the hist DSL can compute.
3. **Frame-scoped accumulation with a deferred gate** (this project): one record per bad frame with the per-cause breakdown.

**The difference between 2 and 3 is when the gate is evaluated.** ftrace decides on each stall event, using only that event's values, as it happens. It can't say "keep this 300 µs futex wait because its frame will turn out to be 21 ms." So with ftrace you either emit every stall above a threshold (volume proportional to stall count, sub-threshold stalls invisible) or you snapshot.

> **ftrace can make the *detection* record proportional to hitches. It cannot make the *attribution* record proportional to hitches**, because attribution needs state accumulated across the frame plus a commit/discard decision at frame end.

- **Caveat to verify:** whether hist-trigger variables can hold a running sum that resets on a third event. The documented model is pairwise begin/end.
- **Decisive early experiment:** classify over-budget frames as "one dominant stall" vs "many sub-threshold stalls." If most are single big stalls, regime 2 suffices and the thesis narrows to the critical path. If a meaningful share are aggregate hitches, that share quantifies the design's value, and nobody has reported it for Linux games.

---

## 8. The "death by a thousand cuts" case in detail (Sep 7)

**Scenario (60 Hz, 16.7 ms budget):**
- 7 ms of main-thread CPU work;
- plus 40 short waits of 200–600 µs each: futex handoffs, minor faults, two preemptions by a background process, one direct reclaim;
- total stall about 16 ms, frame time 23 ms.

It's a hitch, but no single event is remarkable.

**What each mechanism sees:**
- **Per-event threshold** (ftrace synthetic filter, bcc `*slower`, `perf --off-cpu-thresh`):
  - at a 1 ms threshold it emits nothing;
  - at 100 µs it emits all 40, but also a similar number for normal frames, because job systems hand work off via sub-ms futexes by design.
  - The distinguishing signal is the sum, which no single event carries. Lowering the threshold just becomes "stream everything."
- **Snapshot:** catches it, but pays megabytes to learn that 40 small things happened.
- **Per-frame accumulation:** add each stall to a per-thread/cause counter. At present, if the frame is over budget, emit `{frame 23.1 ms, futex 9.2, preempt 4.1, minor-fault 1.8, reclaim 0.6}` and zero the counters; otherwise just zero them. There is no per-stall threshold; the gate applies to the aggregate at the only moment the aggregate is known.

**Timing is the whole issue.** When a 300 µs wait ends, whether the frame will go over depends on the future. A running sum doesn't help, because by stall 35 you've already dropped stalls 1–34. There are two escapes, both needing state that survives to frame end:
- **summary** (accumulation: cheap, loses detail);
- **speculation** (a bounded tentative buffer: keeps detail).

ftrace has neither.

**delayacct catches this too:** its counters increment at the stall site with no threshold, so diffing at consecutive presents gives the aggregate. That makes it the baseline to beat. It lacks:
- a futex category;
- the waker;
- per-frame scoping without diffing every thread in user space;
- within-frame timeline attribution.

**Why this is plausible in games:**
- lock-holder preemption in Wine/Proton futex emulation (raised at LPC) creates convoys of short waits;
- asset streaming and shader-cache warm-up create thousands of µs-scale minor faults;
- background load (compositor, Steam, shader compilers) preempts for one slice at a time;
- direct reclaim under pressure happens in short slices.

Nsight's Frame Health (per-frame accumulated action time vs a rolling median) suggests practitioners already concluded that aggregates are the unit of analysis.

**First result:** capture hours of play and classify each over-budget frame by the fraction of its excess attributable to the single largest stall.
- Bimodal with mass near 1.0 → thresholds suffice.
- A substantial share below 0.5 → that share is the value of frame-scoped accumulation.

---

## 9. Back to the problem statement (Sep 7)

**User's problem:** continuous profiling floods RAM or disk. They want a criterion (frame > 16 ms) that captures full-verbosity data only for that event.

**Claude's answer: stated that way, it's solved and has been for years, as a "flight recorder" or "retroactive sampling."** Record into a fixed-size overwrite ring. Nothing drains in steady state, so RAM is constant and the disk never sees the stream. On trigger, freeze and dump. The recorder looks backward, so the trigger can fire after the fact.

**Existing implementations:**
- ftrace overwrite buffer + synthetic `frame_lat` snapshot trigger;
- Perfetto `RING_BUFFER` + `STOP_TRACING` (how Android captures jank in the field);
- LTTng snapshot + latency-tracker;
- gpuvis/SteamVR (F6, manual trigger);
- Hindsight (NSDI '23, the "dash-cam" analogy);
- DTrace speculation (2005, per request);
- `perf record --overwrite --switch-output` for sampling profiles.

"Define a criterion, capture only when it fires, keep full verbosity" **cannot be the contribution.**

**What the flight recorder doesn't give (the residual problem):**
1. **Cost scales with hitch frequency.** Each trigger dumps the whole window, and the snapshot must be drained before the next. Fine for "every five minutes"; bad for a game hitching every couple of seconds over a 3-hour QA session.
2. **The criterion is a symptom, not a cause.** It fires equally for the game's own 20 ms pathfinding spike and for a kernel stall. "Over budget *and* kernel stall explains most of the excess" needs frame-accumulated state that no trigger can express.
3. **Capture is solved; diagnosis isn't.** A human still has to work out which thread waited on whom and why. Automatic attribution (per-cause breakdown plus blocking chain) doesn't exist on the Linux desktop, and it is what the LPC scheduler people asked for.
4. **The pieces exist but the product doesn't.** Nobody has wired present uprobe + stall events + trigger together for unmodified Linux games, but that is engineering, not a thesis, unless something is measured.

**Revised problem statement (Claude's draft):**
> Flight recorders can capture full-verbosity traces around anomalous frames, but their trigger sees only the symptom and their output scales with hitch frequency, leaving a human to diagnose each dump. This work evaluates whether frame-scoped, kernel-side stall accounting can (a) gate capture on cause rather than symptom, (b) reduce per-hitch output from the whole trace window to the frame's own attributed record, and (c) produce a per-frame blocking-chain attribution automatically, at overhead low enough for hours-long sessions on an unmodified game — and quantifies what fraction of real hitches this approach captures versus per-event thresholds and a delayacct baseline.

"That's smaller than what you started with. It is also true."

---

## 10. Feedback on a summary from another Claude Code session (Sep 8)

The pasted summary is hidden. From the reply, it proposed:
- **"on-demand chain graphs for outlier frames"**;
- an L0–L3 information ladder;
- a time-interval-matching chain walk with a hop rule;
- per-wakee edge rings;
- a "stack on the last hop" option;
- two skeptic arguments: #1 emitted volume, #2 ring wraparound during trigger latency.

**Overall:** the best framing either session had produced. It names a concrete artifact, answers Gregg's stated gap, and gives a measurable claim. Keep it.

### Verification closed
- **Plumbers testimony:** confirmed via LWN (Jake Edge, Jan 7 2026). LWN is a paraphrase, so **cite the YouTube recording and slides.**
- **VarMRI:** confirmed, including that they tried kprobes/eBPF and then patched the kernel. That is a data point for an "expressibility as observation" section.
- **Algorithmic prior art the summary underweighted:** time-interval matching plus the hop rule *is* Giraldeau–Dagenais (iterate states to a block, find its origin in another thread, recurse). Perfetto's `thread_executing_span` is an offline version, and its source notes that interrupt-context wakeups may make the graph inaccurate (the same IRQ pitfall). **Cite both; claim only the online, bounded-window, frame-gated execution.**
- The on-demand variant: nothing found, with the same absence-of-evidence caveat.

### Technical issues, ranked
1. **User stacks on Proton games are mostly unresolvable.** PE code under Wine has no frame pointers and no resolvable symbols. The "stack on the last hop" should be a **kernel stack**: a depth-3 kstack at switch-out gives the blocked reason (`futex_wait_queue`, `io_schedule`, `folio_wait_bit`, `__alloc_pages_slowpath`). Upstream kernels lack Android's `sched_blocked_reason`, so BPF is the only way to get blocked reasons on stock SteamOS. **Put this in the thesis as a selling point.** Drop user stacks from L2 for closed-source targets.
2. **Use `sched_waking`, not `sched_wakeup`.** `sched_waking` is always emitted on the waker's CPU, so waker identity and IRQ context are only meaningful there. Recording both double-counts edges.
3. **Concurrency in edge rings.** A per-wakee ring is written by the waker's CPU while stall intervals are written by the wakee's CPU, which needs atomics or `bpf_spin_lock`. Alternatively, use **per-CPU time-ordered rings** (lock-free, fixed slots, variable time window). 16k slots × 32 B × 8 CPUs ≈ 4 MB covers more than 100 ms at 1M wakeups/s.
4. **Chains give "who," not "why."** W ← K ← IRQ doesn't say "block I/O." Attach cause classification to each hop via the blocked reason. Add **"timer"** as a terminal: an hrtimer wakeup means an intentional sleep (frame limiter, `Sleep()`).
5. **Skeptic argument #2 (wraparound during trigger latency) is weak.** The ring holds seconds and a trigger fires within about 1 ms; the in-kernel ftrace trigger has no user-space latency at all. The durable differences are #1 (volume, which only materializes with an in-kernel walk or L0–L2 records) and what ftrace can't express: frame-scoped resettable accumulation, and blocked-reason capture on stock kernels.
6. **L3 v1 (dump window, walk in user space) ≈ an ftrace snapshot with two events.** Neither can be pid-filtered, because chains cross processes. The volume claim must rest on the in-kernel walk or on L0–L2 records.
7. **Proton specifics.** Game threads block on wineserver via sockets, so chains cross into another process (an argument for system-wide tracing). fsync, esync and ntsync each change what the waker looks like; document which are supported.
8. **Version numbers check out:**

   | Feature | Kernel |
   |---|---|
   | 1M-instruction limit | 5.2 (not 5.8) |
   | Bounded loops | 5.3 |
   | `bpf_get_task_stack` | 5.9 (matters for a blocked thread's kstack from another context) |
   | `bpf_loop` | 5.17 |
   | Open-coded iterators | 6.4 |
   | Arena | 6.9 |
   | Private stack | 6.13 |

### Scope changes
- "L0–L2 guaranteed, L3 stretch" is right, but L2 is only cheap if its stack is **kernel-only**.
- **Build the evaluation harness first.** You need a Vulkan present loop with a job system and injectors for lock-holder preemption, I/O, reclaim and compaction. Min asked at LPC for a schbench-style gaming microbenchmark and said none exists, so **"hitchbench" would be a citable artifact on its own** and makes M1 possible.
- Keep the fixed 16.6 ms budget as the domain justification for choosing games.

**One-line verdict:** the artifact is real; the algorithm is prior art to cite; the cost argument should rest on the in-kernel walk and frame-scoped state, not on trigger latency; and the constraint that bites first is stack resolution on Proton.

---

## 11. Real-world use cases (Sep 8)

**Genuine fit:**
1. **Distro/platform engineering** (Valve, Igalia, CachyOS, Bazzite). This is the strongest fit. Someone tuning scx_lavd or a SteamOS kernel config needs to know why a title's 1% lows are bad, can't ship a continuous profiler to customers, and runs a game on a bench for an hour asking "futex contention, reclaim, or the compositor preempting render?" Valve's existing telemetry (Piccoli) is a plausible sink for a hitch-cause stream.
2. **The scheduler-development inner loop.** Min's stated problem. Small audience, but it's the audience whose problem is being solved.
3. **Proton/Wine and Mesa debugging.** "Stutters on Linux, not Windows" reports are currently diagnosed by hand with gpuvis. Contingent on chains cleanly crossing into wineserver.

**Partial fit:**

4. **Game studios on Linux/Deck.** Actionable hitches are usually in their own code, which the engine profiler already shows. The tool's value is the residual: "not your code; kernel/driver/compositor." Advisory, not directive.
5. **QA telemetry at scale.** Technically supported, but "12% of hitches were direct reclaim" only converts to action for a platform owner, not a game owner.

**No fit:**
- End users.
- Windows developers (WPA/PIX/GPUView already exist).
- CPU- or GPU-bound hitches. L0's **"off-CPU share of excess"** is the single most important number because it tells the user whether to keep using the tool.

**One-paragraph statement:**
> The tool is a bench instrument for platform and driver engineers — Valve/Igalia/CachyOS, sched_ext developers, Proton/Mesa maintainers — who need to know whether a given game's intermittent hitches come from kernel-side contention (scheduling, locks, reclaim, I/O) and, if so, what blocking chain produced them, gathered over sessions long enough that a full-trace profiler would drown in its own data. It is not a consumer tool, it does not diagnose in-engine or GPU-bound hitches, and its output is most actionable to whoever owns the kernel/driver/compositor, which on Linux gaming is increasingly one organization (Valve) — which is precisely why the use case exists at all.

The last clause is load-bearing. For the thesis, use cases 1–3 require demonstrating that the method works on real games and on injected ground truth, not adoption.

---

## 12. Are kernel-caused hitches common in games? (Sep 8)

**Yes, with two qualifications.** No one has published a number for Linux games, and the single biggest PC hitch cause (shader compilation) is not a kernel operation.

**Evidence:**
- **Scheduling and locks.** LAVD's stated primary goal is minimizing stutter. Min described lock-holder preemption in Wine's futex emulation as still being worked on. Valve funded a scheduler, ntsync, and futex API work because of this.
- **CPU frequency.** Vernet's `epp_boost` (July 2026): brief game-thread sleeps were misread as low demand, so cores downclocked and resumed too slowly. The fix gave +31.8% 1% lows in Civilization VI on Deck. This shows up as slower on-CPU time, **not** an off-CPU stall, so it is invisible unless per-frame frequency or APERF/MPERF is recorded.
- **Kernel mitigations.** Valve's telemetry engineer lists split/bus locks. The split-lock mitigation deliberately delays the offender, and it stuttered games (LWN 911219, 2022).
- **Android, same kernel.** AOSP calls uninterruptible sleep "a very common jitter indicator" and mandates `sched_blocked_reason`. An AOSP jank case study traced a dropped frame to a Wi-Fi driver's `pm_runtime` work with preemption disabled. Google's Feb 2026 Android 17 post shows a Launcher missed frame caused by a background-priority lock holder preempted by camera threads.

**Why the Deck makes these common:**
- 16 GB shared with the GPU → memory pressure, reclaim, zram;
- microSD → long I/O waits;
- 4C/8T shared with gamescope, the Steam client, MangoHud and shader compilers;
- tight thermal/power budgets;
- Proton futex sync.

A 16-core, 32 GB NVMe desktop sees far fewer. The tool's value is concentrated in handheld and low-end hardware; say so.

**Not kernel operations:**
- **Shader compilation** is driver/app CPU work. Partly moved to worker threads (DXVK GPL, Fossilize), where it shows up as scheduling contention.
- **GC, engine spikes, GPU-bound frames.**
- **App-initiated synchronous I/O,** where the kernel is only the messenger. The fix is in the game, but naming the mechanism is still useful.

**Measurement #0:** in week one, run a bpftrace accumulator of per-frame off-CPU time by `prev_state` and blocked function across five Proton titles on Deck-like hardware, under realistic conditions (background Steam download, cold shader cache, memory near full). Report the distribution of the off-CPU share of excess. Either outcome is defensible; guessing is not.

---

## 13. Draft abstract (Sep 8)

**Working title:** *Frame-Scoped, Cause-Gated Attribution of Kernel-Side Frame Hitches in Linux Games with eBPF*

> Intermittent frame hitches in games are hard to diagnose because they are rare and not reliably reproducible: catching one requires tracing across hours of play, and full kernel tracing at that duration produces data faster than it can be stored while perturbing the very frame timing under study. Flight-recorder tracers (Perfetto, ftrace snapshots, LTTng) solve the capture problem — keep a ring buffer, dump it when a frame misses its budget — but the trigger sees only the symptom, every dump costs the whole trace window, and a human must still determine which thread waited on whom and why. We present [NAME], an eBPF tool for unmodified Linux games that attributes each over-budget frame to kernel-side stall causes at the moment the frame ends, on stock kernels, without kernel modules or engine changes. Frame boundaries are taken from the presentation call via a Vulkan layer; stall time is accumulated in-kernel per thread and per cause (involuntary preemption, futex wait, page faults, block I/O, direct reclaim, compaction) and gated on the frame budget, so frames that meet it emit nothing. Wakeup edges are retained in a bounded in-kernel window and walked on demand — only for over-budget frames — to recover the blocking chain across threads and processes (e.g. render thread ← worker ← kworker ← disk interrupt); this executes the critical-path analysis of Giraldeau and Dagenais online over a bounded window instead of offline over a full trace, and addresses the latency-outlier case that Gregg's chain-graph work left open. Evaluating on [N] Proton titles on Steam Deck-class hardware and on a synthetic benchmark with injected kernel stalls, we (i) measure the fraction of over-budget frames whose excess is dominated by kernel-side stalls [X%]; (ii) show that frame-scoped aggregation captures hitches composed of many sub-threshold stalls, which per-event thresholds such as perf --off-cpu-thresh miss [Y% of injected hitches]; (iii) quantify diagnostic accuracy across four summary levels, from per-cause totals to full chains, against a complete trace; and (iv) compare frame-time perturbation and emitted bytes per hour against a Perfetto/ftrace flight recorder, finding [Z]. [One sentence stating the main empirical finding once known.]

**Notes:**
- It deliberately does not claim that eBPF reduces data movement, that selective capture is new, or that the walk algorithm is new.
- If L3 doesn't ship, replace the wakeup-edge sentence with "records, for each stall, the waking thread and kernel blocked reason," and drop the Giraldeau/Gregg clause.
- If X is small, reword the final sentence as a scoping finding rather than hiding it.
- Verify the LPC recording before citing Min/Vernet, and keep them out of the abstract.
- Name the Deck-class hardware; it's a strength.

---

## 14. Can the tool measure the game's own system operations (asset I/O, syscalls)? (Sep 8)

**Yes; it already does.** "Kernel-side cause vs game's own operation" is labelling, not mechanism. Every wait goes through the scheduler, every read through a syscall or page fault, every GPU wait through a DRM fence.

**Measurable per frame with the same machinery:**
- **File I/O with the file named.** Capture the filename at `openat`, keep an fd → name map, and report e.g. "8.3 ms in `read()` on `level3.pak`, StreamingThread, frame 4712." For mmap'd assets, use major faults plus `bpf_find_vma` (5.17) to reach the backing file. This catches the "synchronous asset load on the wrong thread" bug without instrumenting the engine.
- **GPU waits, a correction to earlier replies.** The GPU-internal *cause* is invisible, but the *wait* is not. A render thread blocked in `drm_syncobj_wait`/`dma_fence_wait` (via `vkWaitForFences` or submit back-pressure) is a classifiable off-CPU stall, which answers the CPU-bound vs GPU-bound triage question. **Add it as an L0 category, probably the most useful one for studios.**
- **Present/vblank waits** inside `vkQueuePresentKHR`: classify separately, because capped games spend most of every frame there by design.
- **Timer sleeps:** a terminal category, usually benign.
- **Wineserver and sync primitives.** `ReadFile` → `pread64`. `WaitForSingleObject` → futex (fsync), eventfd read (esync) or `/dev/ntsync` ioctl. Handle operations cross a socket to wineserver, which the chain follows. Windows studios have no visibility into this.
- **Syscall counts and kernel CPU time per frame** (via `raw_syscalls`): e.g. "3,900 ioctls/frame," a translation-overhead finding.

**Prior art:** Nsight (OS-runtime calls aligned to frames, with backtraces above a threshold), `perf trace`, bcc `fileslower`/`biosnoop`, Superluminal. All are offline or per-event. The differentiator is unchanged; the audience grows.

**Thesis changes:**
- The studio use case moves from "partial" to "real fit," especially for Proton titles.
- **Rename "kernel-side causes" to "kernel-observable causes,"** and report a **game-initiated vs external** split: sync I/O, own-thread waits and GPU fences vs preemption by others, reclaim/compaction and IRQ.
- Add GPU fence wait and file I/O with path to the abstract.
- **Scope-creep warning:** each category (fd → path, VMA → file, DRM ioctl decoding) costs about a week. Pick three (file I/O with path, GPU fence wait, present wait) and leave the rest as counts. If the tool becomes "a generic syscall profiler with a frame counter," it is back in Nsight's territory.

---

## 15. Do we need the game's user-space call stack? (Sep 8)

**Yes, to answer "which part of the game waited."** It's the most expensive and fragile part of the design, so it needs its own decision.

**1. Whether it's needed depends on the user:**
- **Platform, scheduler and driver engineers mostly don't.** Thread, blocked reason, waker chain and classification answer their question.
- **Studios do, but have a cheaper route.** Emit (thread, interval, cause, file, chain) on `CLOCK_MONOTONIC` and join by thread and time against their engine profiler. Wine propagates Windows thread names to `comm` in recent versions (verify on Proton).
- So make user stacks **an optional capture policy**, with the timestamp join as the zero-cost alternative.

**2. Capture options** (at `sched_switch`, where `prev` is still current and its user memory is readable):
- **Frame-pointer walk** (`bpf_get_stackid` + `BPF_F_USER_STACK`): cheap, but useless on most games (no frame pointers), giving one or two garbage frames.
- **`.eh_frame` unwinding in BPF** (the OTel eBPF profiler / Parca approach): works for ELF without frame pointers, covering native games, Mesa, libc, DXVK and Wine's ELF side. Reuse theirs; it's large and verifier-hostile.
- **Windows PE code under Proton:** PE uses `.pdata`/unwind codes, which no BPF unwinder understands. Wine's syscall dispatcher switches to a separate per-thread stack for the Unix side (verify per Wine version), so an unwinder stops at the dispatcher. Reaching PE frames needs a Wine-aware unwinder: **out of scope, future work.**
- **Raw stack copy** (1–2 KB per stall, unwind later, as with `perf --call-graph dwarf`): the only generic option for PE. The copy must happen at switch-out because the stack changes. It fits the speculative-buffer design (copy only for game-thread stalls above a duration, commit only on a miss), but 30 threads × tens of stalls × 1 KB per frame is the wrong side of the data-volume argument.

**3. Symbolization:** emit module + offset plus a `/proc/pid/maps` snapshot at flush. Wine maps PE images file-backed (verify). Studios symbolize offline with PDBs; platform engineers get "inside game.exe vs ntdll.so/libdxvk/radv/libc," which is their triage boundary. **Symbolize offline from day one.**

**Recommendation:**
- **Core:** thread + kernel stack + cause + chain, with no user stacks.
- **Level-2 option:** `.eh_frame` unwinding for ELF (reuse OTel/Parca), emitting module + offset, gated to game-tgid stalls above a threshold. Report cost and coverage: "resolved to a game frame in N% of stalls, ~0% for PE under Proton."
- **Explicit non-goal:** PE unwinding under Wine, explained in one paragraph. Nobody has written this down for the Proton case.

---

## 16. Concrete problems the tool can diagnose (asked Sep 8, answered Sep 15)

1. **Lock-holder preemption in a Proton job system.**
   - Symptom: a hitch every minute in busy scenes.
   - Output: frame 18,204 over by 9 ms. Render futex wait 7.8 ms. Chain: Render ← Worker3 (futex wake at +7.6 ms) ← Worker3 was *preempted* 6.9 ms by steamwebhelper. Twenty workers show short futex waits on the same address.
   - Action: a scheduler engineer recognizes lock-holder preemption (LAVD's slice extension) and has a reproducer with the holder tid and the preemptor name.
2. **Thousand cuts from memory pressure on a 16 GB Deck.**
   - Symptom: stutter only after 40 minutes.
   - Output: 30–50 direct-reclaim entries of 100–400 µs on three threads' allocation paths; largest stall 0.4 ms; frame sum 12 ms of reclaim. Per-event thresholds see nothing.
   - Action: tune reclaim/zram or the texture streaming budget; the studio learns its allocator churns. **This case justifies frame-scoped aggregation.**
3. **Synchronous asset read from microSD.**
   - Output: StreamingThread blocked 4 ms in `pread` on `paks/world_03.pak`; render futex 19 ms. Chain: Render ← StreamingThread ← kworker ← mmc IRQ.
   - Without the chain, render just shows "futex 19 ms."
4. **Periodic background process.**
   - Symptom: stutter every exactly N seconds in every game.
   - Output: the render thread is runnable (preempted) 3–5 ms, always by the same pid (monitoring daemon, RGB controller, indexer).
   - Action: kill or renice it. Windows users know this pattern from WPA; on Linux it's guesswork.
5. **Split-lock mitigation.**
   - Output: a game thread blocked in the kernel's split-lock delay path (the God of War case).
   - Action: `split_lock_detect=off` or a vendor report. Valve collects split-lock events, but not per-frame attribution.
6. **Wineserver round-trips.**
   - Output: dozens of socket reads to wineserver per frame; the chain crosses into wineserver, which was itself preempted or busy; about 6 ms per bad frame.
   - Action: Proton developers can judge whether ntsync/fsync would help. The game's profiler can't see wineserver.
7. **"Is it the kernel?", a valuable negative.**
   - Output: 0.3 ms off-CPU; render was running for 24 ms (shader compilation) or blocked 20 ms in a DRM fence (GPU-bound).
   - Probably the tool's most common real-world output, and it saves a platform engineer a day each time.
8. **Frequency ramp after short sleeps (the `epp_boost` case).**
   - As designed: inconclusive.
   - With per-frame APERF/MPERF or cpufreq deltas: "render on-CPU 14 ms at 2.4 GHz vs 3.5 GHz, right after a 200 µs sleep." It's one counter read per frame; worth adding.

**Cannot solve:**
- shader compilation on the render thread (on-CPU; it can only point to Fossilize/DXVK GPL settings);
- GPU-internal causes (it sees the fence wait, not why; that's RGP / Nsight Graphics territory).

**Common thread:** each answer names a thread, a kernel mechanism and a counterpart (preemptor, waker, file, device), for a frame that happened once an hour, at a cost that can run all session.

---

## 17. Do these cases need the full wakeup chain? (Sep 15)

**No.**

| Case | What reveals the cause | Chain needed? | What an engine profiler sees |
|---|---|---|---|
| 1 Lock-holder preemption | Holder runnable-not-running + preemptor (L1) | One hop, which the engine already knows | The wait, not that the holder was preempted |
| 2 Reclaim thousand-cuts | Per-cause sum (L0) | No | "malloc is slow" |
| 3 SD-card asset read | Streaming thread's block I/O + device (L1) | Only to link Render → Streaming, known from the job graph | "ReadFile took 4 ms" |
| 4 Periodic daemon | Preemptor (L1) | No | Nothing without system-wide context switches |
| 5 Split-lock | Blocked reason (L1) | No | Nothing ("10 ms on an instruction") |
| 6 Wineserver | Cross-process hop + wineserver preemption (L3) | **Yes** | Nothing (no wineserver on Windows) |
| 7 Not the kernel | Off-CPU share (L0) | No | Knows already, if it can run |
| 8 Frequency ramp | Counters | No | Nothing |

Five of eight need only L0/L1, two need a hop the engine already has, and one genuinely needs the walk: wineserver, and more generally anything crossing a process boundary (kworkers, compositor, IRQ threads).

**The structural point:**
- **An engine profiler has** the dependency graph and its own code's names.
- **It lacks** the kernel-side reason a thread was off-CPU and everything outside the process.
- **The tool is the mirror image.** It lacks the dependency graph, and the wakeup chain reconstructs it, because a wakeup edge is the kernel's record of a dependency being satisfied.
- **"The chain is the kernel's substitute for the engine's dependency graph,"** needed for unmodified games, Proton titles, and every hop past the process boundary.

**Footnotes:**
- The chain isn't new relative to Windows. ETW records the readying thread per context switch, Bruce Dawson's wait analysis follows it by hand in WPA, and Superluminal shows blocking/unblocking stacks. What Linux lacks is the automated, per-frame, budget-gated version.
- **L1 (per-cause totals + blocked reason + preemptor + one-hop waker) delivers most of the value and is cheap; ship it as the guaranteed core.** L3 earns its place on the wineserver/kworker/IRQ cases and the unmodified-game argument, and that argument should be made explicitly.

---

## 18. Getting started (asked Sep 15, answered Sep 16)

**Don't build on a VM for anything you'll measure.** Scheduler timing, frame timing and a real GPU are exactly what hypervisors distort. A VM is fine for the first two weeks of plumbing; use bare metal from week 3.

**Environment:**
- Spare machine or a second SSD; AMD (RADV) or Intel GPU (open drivers, tracepoints, no NVIDIA blob).
- CachyOS/Arch (latest kernel, BTF, sched_ext) or Fedora (frame pointers by default). Kernel ≥ 6.12.
- A Steam Deck or Deck-class APU is ideal for evaluation.
- On a VM: QEMU/KVM with lavapipe for the hook and pipeline; WSL2 runs BPF but has no real Vulkan.
- On a Deck: developer mode, `steamos-readonly disable`, and check `/sys/kernel/btf/vmlinux` on day one.

**Toolchain:**
- clang/llvm, libbpf, bpftool, bpftrace, pahole, perf, trace-cmd, gpuvis, mangohud, the Vulkan SDK (validation layers), and systing for a Perfetto ground-truth view.
- BPF in C with libbpf CO-RE and `bpftool gen skeleton`; user space in C++ or Rust (libbpf-rs). Avoid bcc/Python.

**Week 1: validate the data model with bpftrace:**
- Does the present uprobe fire for vkcube, and for a Proton game? If only vkcube, the loader bypass is confirmed; write it up. Cross-check against a MangoHud log.
- Per-thread off-CPU by `prev_state` for the game tgid gives stalls per frame, which sets K.
- The system-wide `sched_waking` rate is the always-on cost driver.
- Kernel stacks at depth 3 for D/S switch-outs show what blocked reasons look like.
- `task_delayacct=1` + `getdelays` shows what the baseline reports.

**Weeks 2–3:**
- Implicit layer + USDT (`DTRACE_PROBE2`).
- BPF: `sched_switch` accumulator per (tgid, tid, cause), `sched_waking` edges, futex enter/exit, `block_rq_issue/complete`, `mm_vmscan_direct_reclaim_*`, `mm_compaction_*`, page faults, a tgid filter, and a gate at the USDT.
- User side: skeleton loader, ringbuf consumer, JSONL, CLI (pid or launch command).

**Week 4, before L1:**
- hitchbench with injectors: sync read after dropping caches, futex holder pinned with a stress-ng hog, memory hog, THP fragmentation, periodic poller.
- Three scripted baselines: ftrace synthetic flight recorder, `perf --off-cpu-thresh`, delayacct at present. If these aren't running by the end of week 4, the measurement chapter is at risk.

**Then:** L1, the prevalence study, and L3 only if week-1 edge-rate numbers make it tractable.

**Habits:**
- Record kernel, distro, GPU and game build with every result (game updates invalidate data, as Min noted).
- Keep a **"surprises" file** (loader bypass, BTF missing, thread names, stacks stopping at the dispatcher). Each is a thesis sentence.

---

## 19. Renting a machine (Sep 16)

The user can't dual boot or get another machine.

**What a rental needs:** physical hardware, root and your own kernel, a Vulkan GPU, and cancellable billing.

- **Option A: Hetzner AX42 (recommended).**
  - Ryzen 7 PRO 8700GE (8C), 64 GB DDR5 ECC, 2×512 GB NVMe, **Radeon 780M RDNA3 iGPU** (roughly GTX 1060 class).
  - Its shared-memory APU design is Deck-like, which matters more than speed for the reclaim cases.
  - Listed at €46/mo, hourly billing, €39 setup; Falkenstein/Helsinki. Hetzner raised prices on 15 June 2026, so check.
  - Images boot with `nomodeset` and blacklisted GPU drivers. Remove it, check `/dev/dri`, and confirm `vulkaninfo` in the first hour, or cancel.
- **Option B: NVIDIA boxes.**
  - Hetzner GEX44 unavailable (July 2026); GEX131 is €889/mo.
  - HOSTKEY offers RTX 4090/5090 bare metal.
  - NVIDIA exposes fewer DRM tracepoints and isn't Deck-like. OK for development or a desktop comparison point.
- **Option C: GPU cloud VMs** (AWS g5/g6, Lambda, Paperspace). Under $1/h and BPF works, but for development only; hypervisor steal time ruins overhead numbers.
- **Option D: boot Linux from an external USB4/Thunderbolt NVMe** on your own machine. Windows stays untouched, you get bare metal, and it only fails with locked firmware.

**Running games headless:**
- **Display:** on AMD, `amdgpu.virtual_display` gives a DRM virtual connector, with Sway headless on the GPU (the cachyOS-headless project) and Sunshine via wlr-screencopy + VA-API. On NVIDIA, Xorg with a virtual EDID. This gives present a real vblank cadence.
- **Steam + Proton:** the Steam-Headless Docker project, or replicate it natively (keep the game outside a container so pids and cgroups stay simple).
- **Viewing:** Sunshine → Moonlight on Windows (20–40 ms is fine). Disconnect the stream during measurements; use in-game benchmarks or scripted input, with MangoHud logs as frame-time ground truth.

**Deck-class testbed via cgroups:**
- `memory.max` of 12 GB → reclaim;
- a 4-core cpuset → contention;
- `io.max` or `dm-delay` → microSD latency.

These become controlled variables, which is a methodological strength.

**Notes:**
- Avoid anti-cheat titles. Use single-player games with built-in benchmarks (Cyberpunk 2077, Shadow of the Tomb Raider) plus open-source native titles.
- Set up Steam Guard, the downloads, and a persistent `steam` user once.
- Cost: AX42 about €150–200 for three months; a 4090 box 2–4× that; a used Deck about €300.
- **Choice:** AX42 primary, a cloud-VM weekend to get the skeleton compiling, and the external-SSD check as a free wildcard.

---

## 20. Plan: develop in a Hyper-V VM, test on Hetzner (Sep 16)

**Hyper-V VM:**
- Gen 2, Secure Boot off or the "Microsoft UEFI CA" template.
- **Fixed 8 GB memory with dynamic memory off**, since ballooning breaks reclaim tests. ≥ 4 vCPUs.
- **No PMU in the guest:** `perf` falls back to cpu-clock, and there's no APERF/MPERF. Counter experiments wait for Hetzner.
- GPU = lavapipe. vkcube, MangoHud, the layer and present all work; pacing is fake.
- **Headless from day one** (Xorg dummy or Sway `WLR_BACKEND=headless`) + SSH + VS Code Remote-SSH, identical to Hetzner.
- Take checkpoints after installing the toolchain and after the headless stack works.

**One distro for both:**
- **Ubuntu 24.04 LTS + HWE** is recommended: BTF, packaged bpftrace/libbpf/linux-tools, frame pointers in system libraries, plus the kisak-mesa PPA.
- Or Arch: newest kernel and Mesa, but more moving parts and no frame pointers.
- Build both machines from one setup script.
- Generate `vmlinux.h` per machine from its own BTF and rely on CO-RE.

**What the VM fully validates:** everything except timing and the GPU. `sched_switch`/`waking`, futex, faults, block, reclaim and compaction fire on the same code paths.

**VM-phase definition of done:**
- layer + USDT fires under vkcube and BPF attaches;
- accumulators reset every present, and an injected 30 ms `usleep` or sync read yields **exactly one** record with the right cause;
- the ftrace flight-recorder baseline works end to end;
- the delayacct reader diffs at frame boundaries;
- chain-walk unit tests pass on synthetic data with no kernel.

**Hetzner day one:**
1. `installimage`.
2. Remove `nomodeset`, check `/dev/dri`, install linux-firmware + RADV. `vulkaninfo --summary` must show Phoenix, or cancel.
3. `amdgpu.virtual_display=<pci-id>,1` → Sway headless + Sunshine.
4. Tailscale/WireGuard + Moonlight. Never expose Sunshine; pin the Robot firewall to your IP.
5. Steam via the Steam-Headless recipe or natively with multilib; a dedicated `steam` user; single-player benchmark titles.
6. Keep the box during evaluation weeks, and keep the rebuild script current.

**Habits:**
- JSONL results tagged with kernel, Mesa, game build and cgroup limits.
- Log every environment surprise the day it happens.

"Ping me when the VM passes item 2 (one injected stall → exactly one record)."

---

## 21. Implementation plan created (asked Sep 16, delivered Sep 19)

Claude produced **`hitchtrace-implementation-plan.md`** (in this directory). It covers:

| § | Contents |
|---|---|
| §0–1 | Problem statement; claims vs concessions |
| §2 | Architecture, cause taxonomy, map layout, lazy epoch reset, chain-walk rules, stack policy |
| §3 | Hyper-V → Hetzner environment, iGPU day-one check, cgroup Deck-class constraints |
| §4 | 12-week milestones with definition of done and cut lines |
| §5 | Measurements M0–M3 as a spec |
| §6 | Exact ftrace / perf / delayacct baseline recipes |
| §7 | Risks |
| §8 | Repo conventions |
| §9 | Related-work verification checklist |
| Appendices | W1 bpftrace scripts |

**Left open for weeks 1–2:**
- the daemon language (C++ vs Rust; pick once);
- K and edge-ring sizes (from W1 measurements).

**Guaranteed scope:** L0 + L1 + harness + baselines + M0/M2/M3. **Stretch:** L3 + M1, with the cut line at the end of W8.

---

## Appendix A: how key positions evolved

| Topic | Early position | Final position |
|---|---|---|
| Core claim | In-kernel aggregation reduces data movement / observer effect | Frame-scoped, deferred-gate attribution that ftrace's DSL can't express; on-demand chains; measurements. Data movement is conceded. |
| Output proportional to hitches | An eBPF advantage over ftrace (§5) | **Retracted:** ftrace per-event synthetic events are also compact. Only the *attribution* record needs frame state (§7). |
| Frame hook | uprobe on `libvulkan.so` `vkQueuePresentKHR` | Implicit Vulkan layer with USDT (or ICD-symbol uprobe); loader bypass reported as a finding |
| Critical path | Full cross-thread chain | Critical-path-lite / one hop as the core. Full chain (L3) is a stretch, justified mainly for cross-process cases (wineserver, kworker, IRQ). |
| GPU-bound hitches | Invisible | The *wait* is visible as `gpu_wait` (DRM fence); only the GPU-internal *cause* is invisible |
| Cause framing | "Kernel-side causes" | "Kernel-observable causes," with a game-initiated vs external split |
| User stacks | Stack on the last hop | Kernel stacks (blocked reason) are core; `.eh_frame` ELF unwinding optional; PE under Wine an explicit non-goal |
| Use case | eBPF frame profiler | Bench instrument for platform/scheduler/Proton/Mesa engineers; studios as a secondary fit |
| Priority of levels | L3 as the headline novelty | L1 delivers most diagnostic value; L3 earns its place on cross-process cases |
| Environment | Bare metal or dual boot | Hyper-V VM (lavapipe) for development → Hetzner AX42 for evaluation |

## Appendix B: verification to-dos raised in the conversation

- Watch the LPC 2025 "Gaming on Linux" recording and slides; cite those, not LWN's paraphrase.
- Read VarMRI in full (arXiv 2601.10572).
- Whether ftrace hist-trigger variables can hold a running sum that resets on a third event.
- Whether DXVK uses `vkGetDeviceProcAddr` (loader bypass).
- frame-analyzer-ebpf's exact hook symbol.
- GDC prior art (unchecked).
- Whether Wine/Proton propagates Windows thread names to `comm`.
- Wine's syscall dispatcher stack switching on the target Wine version.
- Whether Wine maps PE images file-backed in `/proc/pid/maps`.
- `perf --off-cpu-thresh` requires perf ≥ 6.16 (from the plan; verify).
- The BPF feature version list (§10.8) against kernel changelogs before citing.
- The current Hetzner AX42 price after the June 2026 increase.

## Appendix C: key prior work referenced

- **Capture / flight recorders:** DTrace speculation (2005); latency-tracker (Desfossez et al., SP&E 2016); ftrace hist triggers, synthetic events and snapshot; LTTng 2.13 triggers; Perfetto `RING_BUFFER` + `STOP_TRACING`; Hindsight (NSDI '23); gpuvis / SteamVR F6; `perf record --overwrite --switch-output`; rtla timerlat/osnoise.
- **Accumulation / accounting:** VarMRI (arXiv 2601.10572, Jan 2026); Linux delayacct/taskstats, `delaytop`; PSI triggers; `perf --off-cpu` (2022) / `--off-cpu-thresh` (2025); bcc `offcputime`, `offwaketime`, `runqslower`.
- **Critical path:** Giraldeau & Dagenais (IEEE TPDS 2016); wPerf (OSDI 2018); Perfetto `thread_executing_span`; Gregg's 2016 chain-graph prototype; ETW readying-thread / Bruce Dawson wait analysis; Superluminal.
- **Frame-aware tools:** Nsight Systems (Frame Health, `vkQueuePresentKHR` frames); Android FrameTimeline, `sched_blocked_reason`, JankStats; hwui_mon; frame-analyzer-ebpf; MangoHud; Tracy; Intel GPA; GamePulse/RigSignal.
- **Linux gaming ecosystem:** systing (Bacik, 1.0 Feb 2026); VaporMark (Igalia); scx_lavd; Mesa 23.3 CPU wait tracepoints + Perfetto; SteamOS telemetry (Piccoli, LPC 2025); `epp_boost` (Vernet, July 2026); split-lock mitigation (LWN 911219); Wine esync/fsync/ntsync.
- **Continuous profilers (sampling, not per-frame):** Parca, OTel eBPF profiler (19 Hz), Google-Wide Profiling.
