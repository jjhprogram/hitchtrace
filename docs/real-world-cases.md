# Documented Linux gaming hitches

A catalogue of about 30 publicly documented stutter cases, mapped to
hitchtrace's causes. It was written in a separate research session in
September 2026 from web sources; `cases/` reproduces the mechanisms of several
of them. Bucket names were updated to the current ones (`block_io`,
`runnable`, ...) on 2026-11-09; the rest is as written.

**Read this first.**

- **Not yet verified.** Check each of these against its source before it
  reaches the paper:
  - the commit hashes (vkd3d-proton `54fbadcc9405`, `f39ece9a7ce5`,
    `9817c52d2441`; kernel `b006c439d58d`, `d6e152d905bd`,
    `4096fd0e8eaea1`);
  - every direct quote (Bacik, Min, Griffais, the ntsync docs), since a
    paraphrase presented as a quote is the likeliest error;
  - the scx #3739 figures (661.639 ms, 62 switches);
  - the issue numbers and version-date pairs.
- **Corrected 2026-11-09:** ntsync was merged for 6.14 in January 2025, not
  2026; "Faith Ekstrand", without the former name.
- **Checked 2026-11-09: the ntsync benchmark figures (D1).** They come from
  Elizabeth Figura's ntsync cover letter, "[RFC PATCH v2 00/29] NT
  synchronization primitive driver", 30 January 2024, in a table headed
  "Upstream" and "ntsync". The cover letter says the measurements were
  "taken by users Dmitry Skvortsov, FuzzyQuils, OnMars, and myself". Its
  rows include Dirt 3 110.6 → 860.7 (678%), Resident Evil 2 26 → 77, Call of
  Juarez 99.8 → 224.1, and Tiny Tina's Wonderlands 130 → 360. It also says
  "frame rate improvements of 50 to 150 percent are not atypical".
  - "Upstream" is Wine's server-side (RPC) synchronization. esync and fsync
    were never upstream, and the cover letter does not mention them.
  - So the catalogue is right, and the September claim that these are
    esync-era (2018) figures was wrong.
  - That gains over fsync are much smaller is community commentary, not in
    the source.
  - Source: <https://lkml.iu.edu/hypermail/linux/kernel/2401.3/09229.html>.
- **How to use it in the paper:**
  - These cases are the ones that got root-caused, by bisecting, toggling a
    setting, or a vendor engineer who knew the subsystem. That selects
    against rare, unreproducible, cross-thread stalls, which are the target.
    So the in-scope share of documented cases says what existing tools can
    reach, not how common such hitches are.
  - Cases are not frames: never carry "a third to a half in scope" over to
    prevalence.
  - Recommendation 3's threshold has it backwards: a frame that is *not* a
    kernel stall is a result.
  - scx #3739 and #3259 need hybrid Intel CPUs, which the evaluation machine
    (an AMD APU) cannot reproduce. Lead with the practitioners' statements
    (Min, Bacik), not with those bugs.
- **Status of the "Gaps" section:**
  1. The requested sleep is still not recorded; case 4 measured the gap.
  2. The run-queue occupant is recorded since 2026-11-05, as the last holder,
     for chain hops only.
  3. ntsync is open.
  4. IRQ and frequency are open.
  5. GPU-internal causes are out of scope by design.
  6. The split-lock stack refinement is open.

  All open items are in [todo.md](todo.md).


## TL;DR
- Across ~30 well-documented cases, the single best-evidenced kernel-observable hitch class in the wild is **run-queue / wakeup-delay + preemption** (scheduler cases), followed by **futex/poll Wine-sync stalls** and **iowait**; but a large share of the most famous "stutter" cases fall **outside** hitchtrace's scope — chiefly shader/pipeline compilation (on-CPU) and GPU/driver/firmware-internal causes.
- The dominant diagnostic methods were **A/B toggling of a sysctl or environment variable** and **git bisect** (both fast, hours-to-days, but blind to *why*), plus **expert manual trace analysis** (perf sched / Perfetto / systing) and **vendor domain insight**; practitioners (Igalia's Changwoo Min, ex-Meta's Josef Bacik) explicitly say existing tools cannot trigger on rare spikes and cannot correlate frame-level and scheduler-level views — exactly the gap hitchtrace targets.
- The catalogue exposes concrete design gaps: the "benign" timer bucket would hide late/oversleeping frame-limiter wakeups (the Linux 7.0 regression); run-queue delay needs the *preemptor* identity; several cases need IRQ/softirq, CPU-frequency, GPU-fence, and ntsync accounting that the current buckets under-serve.

## Key Findings
- **Scheduler stalls are the richest in-scope evidence.** The scx (sched_ext) tracker contains multiple reports with quantified wake-to-run delays — most strikingly issue #3739, where a user measured 200–660 ms wake-to-switch delays on an Intel Core i7-12700H hybrid CPU under scx_lavd, with two tasks stuck 661.639 ms and 656.828 ms behind an E-core (CPU 14) that was busy scheduling `data-loop.0` 62 times in that window. These map cleanly to hitchtrace's `runqueue` and `runnable` buckets and demonstrate the exact cross-thread wakeup-chain reconstruction the tool promises.
- **Wine's synchronization evolution is a decade-long futex/poll story.** wineserver round-trips → esync (eventfd, `block_poll`) → fsync (`futex_waitv`, Linux 5.16) → ntsync kernel driver (merged for Linux 6.14 in January 2025, released March 2025; framework partially present in 6.10, July 2024; adopted by Wine 11 / Proton 11). This is squarely in the `block_futex`/`block_poll`/`block_other` buckets, but the newest cases run under **ntsync ioctls**, which the taxonomy only captures under `block_other`.
- **The most famous "stutters" are largely out of scope.** Elden Ring (shader/asset streaming, vkd3d-proton memory-manager and command-pool churn), the general DXVK shader-compilation problem, and amdgpu clock-ramp/DPM stutter are on-CPU or GPU-internal — hitchtrace is deliberately blind to them. An honest motivation chapter must concede the in-scope share of *headline* cases is a minority.
- **Firmware/timer/regression cases were solved by bisect + log lines, fast.** The AMD fTPM stutter (kernel Bugzilla 216989) and the Linux 7.0 clockevents regression were both root-caused within days via git bisect and dmesg/commit inspection, but neither required per-frame stall attribution — they are system-wide, reproducible, and coarse.

## Details

### Category A — Scheduler-caused stutter

**A1. scx_lavd wake-to-run delays on Intel hybrid CPU (scx #3739).** Affected: general desktop + games on an Intel Core i7-12700H under scx_lavd 1.1.2 "autopilot" (August 2026). Symptom: repeated multi-hundred-millisecond freezes. Diagnostic: the end user ran `perf sched` and post-processed wake-to-switch latencies, isolating two tasks (a `MediaDe~hine` thread and a `kworker/u85:0`) delayed 661.639 ms and 656.828 ms while CPU 14 cycled `swapper/14 → data-loop.0` 62 times in the 660 ms window. Root cause: scheduler placement / overflow-set logic on heterogeneous cores (addressed by PRs such as #3779 extending the overflow set). hitchtrace buckets: `runqueue` (wake-to-run) + `runnable` (records the occupying task). A cross-thread/cross-CPU chain is *essential* here. In scope. Confidence: high (report with raw trace). URL: github.com/sched-ext/scx/issues/3739.

**A2. Helldivers 2 multi-second pauses under scx_lavd (scx #234).** Affected: Helldivers 2 (Proton) on Arch/CachyOS kernel 6.8.7, Ryzen 9 5900X, RX 6800 XT. Symptom: reliably reproducible stutters/pauses "several seconds long" while walking around the ship. Diagnostic: user captured `scx_lavd` debug-dump output during the stutters (referenced in the sched_ext v6 patch cover letter as an example of the debug dump being used to root-cause a scheduler bug). Root cause: early scx_lavd scheduling bug. Bucket: `runqueue`/`runnable`. In scope. Confidence: high. URL: github.com/sched-ext/scx/issues/234.

**A3. scx_lavd E-core over-preference on Lunar Lake (scx #3259).** Affected: Mount & Blade: Warband, Intel Core Ultra 7 258V. Symptom: single-threaded game pinned to E-cores, low performance; P-cores only used once all E-cores saturate. Diagnostic: user observation + reproduction with synthetic loads (`pv /dev/urandom`, `zstd -b1`). Root cause: LAVD core-placement heuristic not promoting a single busy thread to a P-core. Bucket: `oncpu` (wrong core = throttled throughput) plus `runqueue`. Partly in scope (placement observable), but the *performance* loss is on-CPU. Confidence: high.

**A4. scx_lavd 1.1.3 regression vs 1.1.2 (scx #3750, #3791).** Affected: CS2 and general desktop; watchdog "runnable task stall" ejections coinciding with a linux-zen 7.2.2→7.2.3 kernel bump. Diagnostic: before/after version A/B comparison by users. Root cause: regression between 1.1.2 and 1.1.3. Bucket: `runqueue`. In scope. Confidence: medium (open, self-diagnosed by version bisection).

**A5. Feral GameMode CPU-governor rationale.** Affected: all Feral titles (Rise of the Tomb Raider launch title, 2018 onward). GameMode was "designed primarily as a stop-gap solution to problems with the Intel and AMD CPU powersave or ondemand governors," forcing the `performance` governor. GameMode 1.5 (2020) added `igpu_desiredgov`/`igpu_power_threshold` after Faith Ekstrand's PR #179 showed that on an iGPU the shared CPU/GPU power envelope means the `performance` governor can *cost* performance: per the PR, "On my Razer Blade Stealth 13 with an i7-1065G7, this improves the performance of 'Shadow of the Tomb Raider' by around 25-30% according to its internal benchmark mode" when using `powersave` instead. Bucket: `oncpu` (CPU-frequency/thermal — deliberately out of scope). A motivating example that frequency state matters and is NOT captured. Confidence: high.

**A6. Igalia LAVD development narrative (LWN, LPC 2024/2025, OSS NA 2024).** Changwoo Min (Igalia, on Valve's SteamOS contract) built LAVD to minimize stutter. Key methodology statements: he built **VaporMark** (github.com/Igalia/vapormark), which collects `perf sched record` traces and post-processes them for high-level properties; he moved to **Perfetto** for "microscopic analysis to find out the pathological scheduling behaviors" but found it inadequate; the central problem is that FPS-drop events "happen rarely" (every few minutes) and he asked "Can we trigger monitoring only when the problem is detected?" and called to "extend the scope of resource monitoring beyond CPU time," arguing the scheduler is a "centralized entity for resource usage control." Games rely on tightly cooperating **task graphs** (waker→wakee chains), where a delay to a mid-chain task cascades. This is the thesis's core motivation, directly in scope. Confidence: high (LWN 1051430, Jan 8 2026; LPC 2025 slides "Steps Towards a Gaming-Optimized Scheduler," Dec 13 2025).

### Category B — Memory-management stutter

**B1. CryoUtilities vs SteamOS defaults (folklore, contested).** Affected: Steam Deck (all). CryoByte33's CryoUtilities set swap to 16 GB, `vm.swappiness=1`, enabled THP always and THP shared memory. SteamOS 3.5 later changed the default swappiness from 100 to 60 and enabled huge pages by default; SteamOS 3.6 introduced zram (with users noting Valve chose zstd, arguing lzo-rle/lz4 give better latency on a mobile device). Diagnostic: overwhelmingly **A/B toggling + folklore**, with contradictory user reports (RDR2 and Halo Infinite reportedly *regress*; Helldivers 2 and Last Epoch reportedly stabilize). Buckets: `oncpu_reclaim`, `oncpu_compact`, `block_io` (swap), `oncpu_fault*`. In scope, but **almost entirely unmeasured** — an ideal evaluation target where hitchtrace could replace folklore with per-frame attribution. Confidence: medium (claims plentiful, rigorous measurement scarce).

**B2. Arch Wiki gaming memory tweaks vs CryoUtilities (direct contradiction).** The Arch Wiki Gaming page advises *disabling* THP and setting `compaction_proactiveness=0` and tuning `page_lock_unfairness`/watermarks; CryoUtilities *enables* THP. A documented contradiction in community advice. Buckets: `oncpu_compact`/`oncpu_reclaim`/`oncpu_fault*`. In scope. Confidence: high that the contradiction exists.

**B3. vm.max_map_count game crashes/stalls.** Affected: Counter-Strike 2, DayZ, Hogwarts Legacy, Star Citizen, THE FINALS, Payday 2 under Proton. Symptom: crash-on-start or hangs; a user reported "Don't Starve" under Proton showed "not responding" at `vm.max_map_count=1048576` but "loads instantly" at 2,147,483,642. Diagnostic: A/B sysctl toggling; escalated via ValveProton issues (#2704, #3899, #6510, #7317), a March 2024 LKML thread (Bugzilla 218616), and distro changes (Fedora 39 → 1,048,576; Arch 2024.04.07-1; Ubuntu). Root cause: default 65,530 map limit too low for Wine's memory mapping. Bucket: the failure is usually a hard error, not a per-frame stall; where it hitches it would touch `block_other` (mmap_lock) / `oncpu_fault`. Mostly out of scope as a *hitch* but a good example of mmap-layer pathology. Confidence: high.

### Category C — Storage I/O stutter

**C1. Steam download/update saturating I/O (steam-for-linux #6073).** Symptom: during Steam downloads/updates, "everything freezes up from iowait," loadavg >100, mouse cursor lags — worsened after the reporter installed a faster hybrid HDD. Diagnostic: user observation via htop DISK R/W and Steam's own graph; user hypothesis that Steam writes to many files synchronously, disallowing the OS page cache. Root cause: Steam's synchronous, highly parallel write pattern. Bucket: `block_io` (and knock-on `runqueue` for other threads). In scope and a clean iowait example. Confidence: high (long-standing open issue).

**C2. "Stutter after an hour with Steam overlay disabled" (steam-for-linux #5727 and Steam/Mint threads).** Symptom: severe stutter after 30–60 minutes when the Steam overlay / video-recording feature is off; toggling the overlay back on fixes it; frametime graph "goes insane." Diagnostic: community A/B toggling. Root cause: attributed to the Steam client's video-recording/overlay path. Bucket: unclear — likely `oncpu`/`block_other`; the mechanism was never kernel-traced. Confidence: medium; **a good "closed without kernel-level root cause" example.**

### Category D — Wine/Proton-layer stalls

**D1. Wine sync evolution: wineserver → esync → fsync → ntsync.** esync used eventfd (`block_poll`); fsync used `futex_waitv` (Linux 5.16); ntsync — a `/dev/ntsync` char device modelling NT semaphores/mutexes/events — was developed by Elizabeth Figura (CodeWeavers), presented at LPC 2023, appeared partially in Linux 6.10, and had the full driver merged in **Linux 6.14 (merge window January 2025, released March 2025)**. The kernel docs describe it as "a support driver for emulation of NT synchronization primitives by user-space NT emulators." Wine 11 / Proton 11 adopt it; SteamOS 3.7.20 beta and GE-Proton enable it by default. Symptom addressed: micro-stutter and CPU overhead from thousands of sync round-trips/sec through wineserver. Buckets: `block_poll` (esync), `block_futex` (fsync), `block_other` (ntsync ioctls, wineserver `pipe_read`). In scope and central. **Caveat:** the widely-quoted developer benchmarks (vs *vanilla* Wine, no esync/fsync) — Dirt 3 110.6→860.7 FPS (678%), Resident Evil 2 26→77 FPS, Call of Juarez 99.8→224.1 FPS, Tiny Tina's Wonderlands 130→360 FPS, with patch documentation claiming "50–150 percent" for most games — are not deltas vs the fsync users already had; real-world frame-pacing gains are much smaller. Treat the headline percentages as vendor micro-benchmarks. Confidence: high on mechanism.

**D2. Elden Ring stutter fixed by Valve/vkd3d-proton (2022).** Affected: Elden Ring on Steam Deck/Linux (AppID 1245620). On **Feb 26, 2022** Pierre-Loup Griffais (Valve, @Plagman2) announced fixes for "heavy stutter during background streaming of assets" on the bleeding-edge Proton Experimental branch, to ship in a full Proton release "next week." Diagnostic: **vendor domain insight** by Hans-Kristian Arntzen. Root causes and fixes in vkd3d-proton v2.6: (1) commit `54fbadcc9405841cc018210ba6d1045e71f405c0` — a general 8-entry command-pool recycling cache ("Elden Ring in particular spam frees and allocates command pools despite this being a very bad idea. Add a simple 8-entry cache which seems to take care of it"); (2) commit `f39ece9a7ce579a56196455933a200ec04a9d8eb` — an app-specific `eldenring.exe` profile enabling `MEMORY_ALLOCATOR_SKIP_CLEAR` ("Game is really churny on committed memory allocations, and does not use NOT_ZEROED. Clearing works causes bubbles. It seems to work just fine however to skip the clears"); (3) commit `9817c52d2441bc6231afd2e5c472c7e7210a72c9` — ignoring PSO-library driver mismatch (the game's LoadPipeline→CreatePSO→StorePipeline pattern "could explain some stutters"). Griffais explained the hitches were "the game creating many thousand resources such as command buffers… making our memory manager go into overdrive," done "in the various driver layers." Note both a **general fix** (command-pool cache, all apps) and **executable-keyed profiles** were used. Buckets: mostly `oncpu` (driver CPU work) and GPU-internal — **largely out of scope**, though command-buffer "bubbles" and PSO-library reloads would surface as `block_gpu`/`oncpu`. A landmark case hitchtrace would *not* attribute to its true cause. Confidence: high (commits + tweet verified; exact per-commit dates not machine-confirmed to the day).

**D3. Elden Ring skeleton-revival FPS drop on NVIDIA (VKD3D_CONFIG=no_upload_hvv).** Symptom: FPS drop during skeletal revival animations, dGPU/BAR-specific (not strictly NVIDIA). Diagnostic: A/B env-var toggling. Bucket: GPU-internal (HVV/BAR upload) — out of scope. Confidence: medium.

### Category E — GPU/driver CPU-side and power management

**E1. amdgpu DPM clock-ramp stutter (`power_dpm_force_performance_level` / `pp_power_profile_mode`).** Affected: RX 6600 XT, 6700 XT and others on desktop Linux. Symptom: micro-stutter with low GPU clocks in light games (e.g. Genshin-like titles); users set `pp_power_profile_mode` to 3D_FULL_SCREEN or COMPUTE, or forced `power_dpm_force_performance_level=high`, to stop the card idling between frames. ArchWiki notes STUTTER_MODE (0x20000) "may cause stutters by trying to sleep between frames." Diagnostic: A/B sysfs toggling. Bucket: **GPU-internal (clock ramp) — explicitly out of scope.** A recurring, well-documented out-of-scope class. Confidence: high.

**E2. Capture tools perturbing frame times (MangoHud/OBS/GPU Screen Recorder).** Community reports that GPU Screen Recorder window-capture and OBS software capture cause periodic stutter at high refresh; the fix is obs-vkcapture. Diagnostic: A/B tool-swapping. Relevant to the thesis as an **observer-effect / perturbation** caution — the tool must not itself perturb frames. Bucket: `oncpu`/`runnable` (encoder threads preempting the game). Partly in scope (preemptor identity). Confidence: medium.

### Category F — Timer / CPU-frequency / firmware

**F1. AMD fTPM stutter (kernel Bugzilla 216989).** Affected: AMD Ryzen (4000–6000 series) desktops/laptops, kernel ≥6.1, fTPM enabled. Symptom (from the bug): "The stutter lasts for 1-2 seconds and causes the framerate of the display to decrease dramatically and causes bursts in audio output," recurring every few hours. Timeline: Reddit/Arch reports late Dec 2022 → Bugzilla 216989 filed 2 Feb 2023 → Thorsten Leemhuis forwarded to LKML the same day → a reporter's git bisect pointed at b006c439d58d ("hwrng: core - start hwrng kthread also for untrusted sources," merged 6.1) → Mario Limonciello (AMD) posted a fix 14 Feb 2023 ("AMD has issued an advisory indicating that having fTPM enabled in BIOS can cause 'stuttering' in the OS") disabling the defective fTPM hwrng, landing for 6.2. Root cause: firmware fTPM doing NVRAM/SPIROM writes on RNG requests; 6.1's change made the kernel poll the fTPM RNG by default, exposing it. Diagnostic: **git bisect + expert firmware insight** (James Bottomley predicted NVRAM writes; workaround `CONFIG_HW_RANDOM_TPM=n` or disable fTPM in BIOS). Bucket: `block_other` (threads blocked in the TPM/hwrng path) + system-wide preemption; the true stall is firmware/SMI-like and not fully kernel-visible — partly in scope. Confidence: high.

**F2. Linux 7.0 clockevents stutter regression (April 2026).** Affected: broad; CachyOS users, amdgpu, general desktop. Symptom: stuttering after the 7.0 merge. Timeline: culprit d6e152d905bd ("clockevents: Prevent timer interrupt starvation") authored 2026-04-07, merged for v7.0 (2026-04-12) → multiple independent user reports and bisects within days → Thomas Gleixner's fix 4096fd0e8eaea1 ("clockevents: Add missing resets of the next_event_forced flag"), reported-by/tested-by Hanabishi and Eric Naim, queued for 7.0.1; Thorsten Leemhuis coordinated via regzbot. Root cause: an anti-DoS timer-starvation-prevention patch mishandled the `next_event_forced` flag, delaying legitimate timer interrupts. Diagnostic: **git bisect + LKML/regzbot.** Bucket: **the killer for hitchtrace** — late timer interrupts mean the `block_timer` (frame-limiter sleep) bucket would *oversleep*, and the tool currently treats `block_timer` as benign and does not record the requested sleep duration. This is the strongest single argument for recording requested-vs-actual sleep. Confidence: high.

**F3. Steam Deck OLED Wi-Fi power-save streaming stutter (SteamOS #1445 and related).** Affected: Steam Deck OLED, remote play/Moonlight. Symptom: after sleep/wake the Wi-Fi interface re-enables power-save (`iw dev wlanX get power_save` → on) even with the developer toggle off, causing latency spikes, stutter, and ~20 Mbps caps; toggling Wi-Fi off/on fixes it. Diagnostic: A/B toggling + `iw` inspection. Bucket: `block_other`/`block_poll` (network wait) driven by a power-management IRQ-latency cause — mostly out of scope (display/network side), but a good example of latency injected outside CPU scheduling. Confidence: high.

### Category G — Kernel regressions found by mass bisection
F1 and F2 are the archetypes: independent users bisected the same commit, and fixes shipped in the next stable point release. The fTPM case also shows the "hardware/firmware bug exposed by a kernel default change" pattern. hitchtrace would help *localize* the stall (which syscall/wait), but the *fix* still required kernel/firmware expertise — it is a triage accelerator, not a root-cause oracle, for these.

### Category H — Tooling and methodology statements
- **Changwoo Min (Igalia):** VaporMark (`perf sched record` post-processing) for high-level properties; Perfetto for "microscopic analysis in figuring out pathological scheduling behaviors"; both fail on rare events; he wants triggered capture and resource monitoring beyond CPU time. (LPC 2025.)
- **Josef Bacik (ex-Meta):** built **systing** (libbpf tracer → Perfetto, later DuckDB + AI). In "Introducing systing" (May 8 2025) he wrote: "Oftentimes tools like perf or bpftrace are about aggregating the data, and this can be misleading for applications that do several things over a period of time, making it difficult to correlate different events and understand the interactions between them." In "Announcing systing 1.0" (Feb 23 2026): "A lot of the sched_ext developers used it because of how easy it made visualizing scheduler interactions on the system," but "there's entirely too much data in the trace." systing records sched_switch/sched_waking, IRQ/SOFTIRQ events, and stacks on uninterruptible-sleep. These are direct calls for exactly hitchtrace's frame↔scheduler correlation.
- **Elizabeth Figura (CodeWeavers):** historically found convincing benchmark data vs server-side sync hard to produce; the ntsync kernel docs are explicit the driver exists purely for Windows-emulation performance and that native apps should use futex/poll.
- Standard practitioner toolchain observed across cases: `perf sched`, ftrace/trace-cmd, KernelShark, Perfetto, gpuvis, bpftrace/offcputime, systing, MangoHud/FlightlessMango, plus git bisect and dmesg.

## Summary table (≈30 cases)

| # | Case | Year | Category | Diagnostic method | Time to root cause | hitchtrace bucket | In/out scope |
|---|------|------|----------|-------------------|--------------------|-------------------|--------------|
| A1 | scx_lavd 200–660 ms delays (#3739) | 2026 | Scheduler | perf sched manual | days | runqueue+preempt | In |
| A2 | Helldivers 2 pauses (#234) | 2024 | Scheduler | scx debug dump | days | runqueue/preempt | In |
| A3 | Lunar Lake E-core bias (#3259) | 2025 | Scheduler | observation+repro | days | oncpu+runqueue | Partly |
| A4 | scx_lavd 1.1.3 regression | 2026 | Scheduler | version A/B | open | runqueue | In |
| A5 | GameMode governor rationale | 2018–20 | Scheduler/freq | benchmark A/B | — | oncpu | Out |
| A6 | Igalia LAVD tooling narrative | 2024–26 | Methodology | perf/Perfetto | ongoing | runqueue/preempt | In |
| B1 | CryoUtilities vs SteamOS | 2022–26 | Memory | A/B + folklore | disputed | reclaim/compaction/iowait | In (unmeasured) |
| B2 | Arch Wiki vs CryoUtilities | 2022–26 | Memory | folklore | — | compaction/reclaim | In |
| B3 | vm.max_map_count crashes | 2023–24 | Memory | sysctl A/B | days | other_block/pagefault | Mostly out |
| C1 | Steam download iowait (#6073) | 2018+ | Storage | htop observation | — (open) | iowait | In |
| C2 | Overlay-off stutter (#5727) | 2018+ | Storage/client | overlay A/B | never (workaround) | oncpu/other_block | Uncertain |
| D1 | Wine sync esync→fsync→ntsync | 2018–26 | Wine/Proton | benchmark + design | years | poll/futex/other_block | In |
| D2 | Elden Ring vkd3d-proton | 2022 | Wine/Proton | vendor insight | ~weeks | oncpu/gpu_wait | Mostly out |
| D3 | Elden Ring skeleton BAR drop | 2022 | GPU | env-var A/B | — | GPU-internal | Out |
| E1 | amdgpu DPM clock ramp | 2021+ | GPU power | sysfs A/B | — (workaround) | GPU-internal | Out |
| E2 | Capture-tool perturbation | 2025 | Observer | tool A/B | — | oncpu/preempt | Partly |
| F1 | AMD fTPM stutter (216989) | 2022–23 | Firmware | git bisect | ~2 weeks | other_block | Partly |
| F2 | Linux 7.0 clockevents | 2026 | Timer regression | git bisect | days | timer (hidden!) | In (design gap) |
| F3 | Deck OLED Wi-Fi power-save | 2023–26 | Firmware/net | iw + A/B | — (workaround) | other_block/poll | Mostly out |
| — | Split-lock: God of War / F76 / Nightreign | 2022–26 | Wine/kernel | dmesg #AC + sysctl | ~months | other_block (split_lock_warn) | In |
| — | DXVK shader-compilation stutter | 2018–22 | Shader | state cache/GPL design | years | oncpu | Out |

## Diagnostic-method breakdown
- **A/B toggle of sysctl/env-var/tool + folklore:** the most common route (CryoUtilities, vm.max_map_count, split-lock sysctl, amdgpu DPM, Steam overlay, VKD3D_CONFIG, Wi-Fi power-save). Fast (minutes–days) but yields a *workaround*, not a mechanism. ~10–12 cases.
- **git bisect (+ LKML/regzbot):** fTPM, Linux 7.0 clockevents, amdgpu regressions. Root cause in days; requires reproducibility. ~4–6 cases.
- **Expert manual trace analysis (perf sched / Perfetto / systing):** the scx cases, LAVD development. Slow (hours per trace) and rare-event-limited. ~4–6 cases.
- **Vendor domain insight / architectural knowledge:** Elden Ring vkd3d-proton, Wine sync evolution, GameMode governor design. Often the *only* way to reach GPU/driver-internal causes. ~4–6 cases.
- **Log line / dmesg one-shot:** split-lock `#AC` warning, fTPM. Instant when the kernel prints it.

## Taxonomy-bucket breakdown: what's well-evidenced vs rare
- **Well-evidenced in-scope:** `runqueue` + `runnable` (scx scheduler cases with quantified delays), `block_futex`/`block_poll`/`block_other` (Wine esync/fsync/ntsync, wineserver, split-lock via `block_other`/`split_lock_warn`), `block_io` (Steam download saturation, slow microSD).
- **Present but usually folklore/unmeasured:** `oncpu_reclaim`, `oncpu_compact`, `oncpu_fault_major` (CryoUtilities/THP/swap debates — plentiful claims, almost no kernel-level measurement).
- **Rare/anecdotal in-scope:** `oncpu_fault`, `block_gpu`, `block_present` — mentioned in principle but rarely the documented root cause in public triage.
- **Out of scope but dominating headlines:** shader/pipeline compilation (`oncpu`), GPU clock-ramp/DPM and VRAM eviction (GPU-internal), CPU-frequency/thermal (`oncpu`), display/compositor. **Honest assessment: of the ~30 cases, a minority (roughly a third to a half) have their true root cause inside hitchtrace's buckets; the single most famous class — shader-compilation stutter — is entirely on-CPU and invisible to the tool.**

## Gaps the cases expose in the tool's design
1. **The "benign" timer bucket hides real bugs.** The Linux 7.0 clockevents regression and any oversleeping frame limiter produce *late* timer wakeups. Because the tool treats `block_timer` as benign and does not record the *requested* sleep duration, it would misattribute or hide these. **Recommendation: record requested-vs-actual sleep delta.**
2. **Run-queue delay needs the occupant's identity.** scx #3739 was only intelligible because the user saw *what* ran on CPU 14 (`data-loop.0`) during the delay. The tool records the preemptor for `runnable`; it must also record who occupied the target CPU during `runqueue` wait.
3. **ntsync vs fsync.** Newest Wine sync runs through `/dev/ntsync` ioctls; the taxonomy folds these into `block_other`. A dedicated refinement (ntsync vs futex vs poll vs wineserver pipe) matters for Proton engineers post-6.14.
4. **IRQ/softirq and CPU-frequency accounting.** fTPM (firmware/SMI-like), Wi-Fi power-save streaming stutter, and GameMode's governor rationale all show IRQ time and frequency state matter; these live in `oncpu` and are invisible. Consider surfacing IRQ/softirq steal and CPU-freq state alongside `oncpu`.
5. **GPU-side tracepoints.** Elden Ring, amdgpu DPM, and skeleton-revival BAR uploads need drm_sched/amdgpu/syncobj visibility to attribute correctly; the tool's `block_gpu` sees only the CPU-side fence wait, not the GPU-internal reason (by design). The thesis should state this boundary explicitly.
6. **Split-lock "misery mode."** God of War (Linux 5.19+), Fallout 76 (ValveSoftware/Proton #3766, fix via `sysctl kernel.split_lock_mitigate=0` in a 2024 comment), and Elden Ring Nightreign reports: the kernel serializes split-lockers (10 ms sleep + semaphore), causing system-wide slowdown. This lands in `block_other` refined by the `split_lock_warn` kernel stack — a case where the kernel-stack refinement is exactly right, and the dmesg `#AC` line is ground truth. Guilherme Piccoli's `kernel.split_lock_mitigate` sysctl (Linux 6.2) is the fix; the LWN piece "The search for the correct amount of split-lock misery" (Article 911219) documents the debate.

## Unresolved or disputed cases (good evaluation/motivation targets)
- **CryoUtilities vs SteamOS defaults vs Arch Wiki** — direct contradictions, minimal measurement. Prime target to demonstrate hitchtrace attributing THP/compaction/reclaim/swap stalls per-frame.
- **"Stutter after an hour with overlay off"** — closed via workaround, never kernel-root-caused.
- **scx_lavd 1.1.3 regression (#3750/#3791)** — open, self-diagnosed by version A/B.
- **amdgpu DPM stutter** — perennial, only ever worked around via sysfs; root cause GPU-internal.
- **Split-lock recurrences (Fallout 76, Elden Ring Nightreign)** — the same mechanism rediscovered by end users years after the kernel fix existed, because they didn't know the sysctl.

## Recommendations
1. **Lead the motivation chapter with the scheduler cases (scx #3739, #234) and the Igalia/Josef Bacik tooling complaints** — these are the strongest, most quantified, in-scope evidence and directly voice the "can't trigger on rare events / can't correlate frame and scheduler views" gap hitchtrace fills. Quote Bacik's "difficult to correlate different events and understand the interactions between them" and Min's "Can we trigger monitoring only when the problem is detected?"
2. **Explicitly bound scope up front** by naming shader compilation, amdgpu DPM/VRAM, and fTPM/firmware as the dominant *out-of-scope* classes, using Elden Ring and the DXVK GPL story as the canonical examples the tool will *not* attribute. This pre-empts the obvious reviewer objection.
3. **Add two design changes before evaluation:** record requested-vs-actual timer sleep (the Linux 7.0 regression motivates it) and record the CPU occupant during run-queue delay (scx #3739 motivates it). Threshold that would change priorities: if in a representative long play session >50% of over-budget frames land in `block_timer`/`oncpu`, the tool's value proposition weakens and the sleep-delta + IRQ/freq surfacing become mandatory rather than optional.
4. **Use the disputed memory-tuning cases as the headline evaluation** — reproducing CryoUtilities-style THP/swap stalls and attributing them per-frame would convert folklore into measurement, the tool's clearest win.
5. **Add an ntsync refinement to `block_other`** given Wine 11/Proton 11/SteamOS 3.7.20 adoption; post-6.14 traces will increasingly hit ntsync ioctls, and Proton engineers are the target user.

## Caveats
- This area moves fast; several sources are dated 2026 (LWN LAVD article Jan 8 2026; Systing 1.0 Feb 23 2026; ntsync in 6.14, released March 2025; Linux 7.0 regression April 2026). Where secondary tech-blog FPS figures appear (NTSYNC "678%"), they are vendor micro-benchmarks vs vanilla Wine, not real-world frame-pacing deltas — treat as marketing-adjacent.
- Many community cases (Steam overlay, CryoUtilities, amdgpu DPM) are self-reported and lack kernel-level traces; confidence is flagged accordingly, and user hypothesis is distinguished from verified root cause.
- Exact per-commit dates for the vkd3d-proton Elden Ring commits could not be machine-verified to the day; the commit hashes, messages, and v2.6 release framing are verified via the live GitHub commit pages.
- The ~30-case count includes several closely related sub-cases (split-lock recurrences, scx sibling issues #296/#1580/#3750/#3791); the count of *distinct mechanisms* is smaller, roughly a dozen.
- Primary sources were preferred throughout (GitHub issues/commits, LKML/lore, LWN, LPC slides, kernel Bugzilla, developer blogs); tech-news outlets were used only to corroborate dates and quotes already traceable to primary artifacts.