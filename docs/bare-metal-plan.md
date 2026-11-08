# Plan: bare metal and real games

Everything measured so far ran on the development VM (Hyper-V, lavapipe).
This plan covers the runs the paper still needs on real hardware:
- overhead with the `full` preset;
- real Proton and native titles;
- the catch test on them;
- the prevalence study (the paper's Q4).

It collects what still applies from the September plan and its review (last
present in commit `e4f0c13`; see [history.md](history.md)). Items marked
*(web)* came from web sources in September and have not been checked on a
machine.

## Why not the VM

- **No GPU.** lavapipe rasterizes inside `vkQueuePresentKHR`, so with the
  present bracket on, a software-rendered frame looks like display pacing.
  No DRM fence ever fires, so `block_gpu` is never exercised.
- **Pacing.** X11/Xwayland does not pace frames (about 990 fps, FIFO the same
  as IMMEDIATE), so use `vkcube-wayland`. A Wayland window that loses
  visibility stops presenting altogether: a run with zero frames may mean
  that, not a failure.
- **Timing.** It drifts 10 to 15% between runs, so compare within a run or
  interleave A/B runs. A context switch costs about 4.2 µs here, which
  inflates per-switch overheads.
- **No PMU.**
- **No split-lock detection.**

## The machine

- **First choice: Hetzner AX42.**
  - Ryzen 7 PRO 8700GE (8C/16T), Radeon 780M iGPU (RDNA3), 64 GB DDR5,
    2 × 512 GB NVMe, hourly billing.
  - September prices: €46/month plus a €39 setup fee, which is charged even
    if you cancel on day one. Check current prices; Hetzner raised them in
    June 2026.
  - The shared-memory APU is closer to a Steam Deck than a discrete GPU.
- **Fallbacks:**
  - a HOSTKEY RTX 4090/5090 box (NVIDIA: fewer DRM tracepoints, not
    Deck-like);
  - Linux booted from an external USB4 NVMe on your own machine.
  - Cloud GPU VMs only for functional runs, since hypervisor steal time ruins
    overhead numbers.
- **Rent it early.** The September schedule left everything risky to the
  bring-up week:
  - the iGPU and virtual display;
  - whether the layer loads inside pressure-vessel;
  - real wakeup rates;
  - GPU fences.

  With hourly billing a 1 to 2 day probe costs about €55.

## Day one, go or no-go

1. **Install.** `installimage` with software RAID off, or a separate non-RAID
   partition for the game library, since dm-delay cannot claim an md member.
   Same distro and kernel line as the VM (Ubuntu 24.04 + HWE, kernel 7.x).
2. **GPU.** Remove `nomodeset` from `/etc/default/grub.d/hetzner.cfg` and check
   `blacklist-hetzner.conf` for amdgpu/radeon lines *(web)*. Install
   linux-firmware and RADV (kisak-mesa). `ls /dev/dri` must show
   `renderD*`, and `vulkaninfo --summary` must show the 780M. If not, cancel.
3. **Display.** Either `amdgpu.virtual_display=<pci-id>,1` with a
   compositor on the DRM backend, or a purely headless one such as Sway with
   `WLR_BACKEND=headless`. Not both. For viewing: Sunshine over
   Tailscale/WireGuard to Moonlight. Never expose Sunshine to the internet;
   pin the Robot firewall to your IP. Disconnect the stream during measured
   runs.
4. **Steam.** Natively with multilib, as a dedicated `steam` user, with
   Steam Guard and downloads done once.
5. **hitchtrace.**
   - Check BTF.
   - `make`; layer installed under `/usr/share/vulkan/implicit_layer.d`.
     Never use `VK_LAYER_PATH`: pressure-vessel clears it.
   - The same passwordless sudo rule for `build/hitchtrace`.
6. **The checks the plan depends on:**
   - The layer's markers fire inside pressure-vessel. Attach by the host
     path; the container's `.so` is the same inode *(web)*. Save
     `VK_LOADER_DEBUG=layer` output from inside the container.
   - A RADV `vkWaitForFences` microtest gives `block_gpu > 0` and almost no
     `block_other`.
   - One Proton title runs with MangoHud and hitchtrace attached.
7. **perf.** Rebuild it with BPF skeletons on this kernel too (see
   `tools/overhead/README.md`).

## Deck-like limits, done so they reach the game

The September recipe (`systemd-run --scope … steam -applaunch`) silently
fails. With a client already running, `steam -applaunch` hands over to it,
and the game starts in the client's cgroup. `--user` drops the cpuset and io
limits without an error *(web)*.

**Instead, constrain the whole `steam` user slice, from root, before Steam
starts:**

```bash
systemctl set-property --runtime user-$(id -u steam).slice \
    MemoryMax=12G AllowedCPUs=0-3,8-11 IOReadBandwidthMax="/dev/mapper/slowdisk 90M"
# unconstrained condition: MemoryMax=infinity AllowedCPUs= IOReadBandwidthMax=
# background load inside the slice, pinned:
systemd-run --uid=steam --slice=user-$(id -u steam).slice --scope \
    taskset -c 2,3 stress-ng --cpu 2 --cpu-load 60 --timeout 45m &
```

**A cpuset is not a smaller machine:**
- **CPU count.** A cpuset does not change the CPU count a game sees: Wine
  and DXVK size their worker pools from it. Take CPUs offline instead,
  keeping 4 cores and their SMT siblings: confirm the sibling map with
  `lscpu -e=CPU,CORE`, then
  `echo 0 > /sys/devices/system/cpu/cpuN/online`.
- **GPU memory.** `MemoryMax` does not cap the APU's GPU memory: TTM pages
  are not charged to the memcg. For a 16 GB pool, boot with `mem=16G` or
  reserve the rest with `vm.nr_hugepages`.
- **Clocks.** Cap `scaling_max_freq`, pin EPP, and record amdgpu's
  `power_dpm_force_performance_level`.
- **Wording.** In the paper, call this "Deck-like emulated constraints". It
  reproduces thread count, SMT, the shared pool, storage latency and
  background load, not Zen 2 IPC or RDNA 2, and the GPU is not
  constrained.

**Slow storage with dm-delay:**
- Use the 6-argument form, delaying reads only, so the shader cache and
  downloads are not slowed.
- Below 50 ms of delay dm-delay runs a flush kthread at about 1 kHz. Pin it
  off the game's CPUs, and expect I/O chains to end at that worker and a
  timer rather than at the NVMe interrupt.
- Pick a delay a Deck's storage would plausibly have, not 30 ms.

```bash
dmsetup create slowdisk --table \
  "0 $(blockdev --getsz /dev/nvme1n1p3) delay /dev/nvme1n1p3 0 <READ_MS> /dev/nvme1n1p3 0 0"
taskset -cp <non-game CPUs> $(pgrep -f dm-delay-flush-worker)
```

## Proton specifics

- **The frame thread is not the root.** DXVK presents on `dxvk-submit`, two
  hand-offs from the game's `Present()` (game thread → `dxvk-cs` →
  `dxvk-submit`). vkd3d-proton presents on `vkd3d_queue` *(web)*. The root is
  then a translation thread, and the frame window lags by those hand-offs.
  - Identify the game's frame thread from the root's wake chain at the
    mark: the first link whose comm is not `dxvk-*`, `vkd3d*` or `WSI*`.
  - Measure the hand-off lag per frame.
  - The paper's discussion has a `\todo` for this.
- **GPU waits under DXVK** happen on `dxvk-queue`, while the game thread
  waits in a futex. A GPU-bound frame reads as a futex wait on the root,
  with `block_gpu` one hop away on `dxvk-queue`. Check that the hop's
  cause says so.
- **The sync backend decides the bucket.**
  - Proton 11 is ntsync-first: `ioctl(NTSYNC_IOC_WAIT_*)`, which lands in
    `block_other`.
  - fsync uses `futex_waitv`, which lands in `block_futex`.
  - Pin one for the whole study (ntsync, as SteamOS 3.7.20+ does: load the
    module in `/etc/modules-load.d/ntsync.conf`), and set the opposing
    variable explicitly in every launch script.
  - Record the backend from the wineserver line ("ntsync: up and running."
    or the fsync/server-sync line).
- **32-bit titles** without `PROTON_USE_WOW64=1` get no frame marks, since
  the layer is 64-bit only.
- **Thread names.** Wine copies Windows thread names into `comm` and DXVK
  names its threads, truncated to 15 characters *(web)*.
- **MangoHud is not ground truth** for frame times. It measures the same
  present-to-present interval one layer away. Use it as a cross-check that
  present counts match, with the same config in every arm, including the
  no-tool arm (`no_display=1`, `autostart_log=1`, `fps_limit=0`).
  - Implicit layer order is undefined, so pin it.
  - MangoHud starts a thread inside the game.

## Controls and the run manifest

Steam updates Proton, DXVK, vkd3d-proton and the runtime by itself.

**Hold fixed:**
- one Proton build in `compatibilitytools.d`, forced per title;
- the Mesa packages (`apt-mark hold`), since a Mesa change also makes every
  shader cache cold;
- the sync backend;
- Shader Pre-Caching off, or no `fossilize_replay` running.

**Per run:**
- Drop the page cache before every run, randomize run order, and snapshot
  `SteamLinuxRuntime_sniper/VERSIONS.txt` before and after each block.
  Discard any block where it changed.
- Replace "background Steam download" with a reproducible load: a fixed
  `steamcmd` download or a rate-limited copy, rate recorded.
- Never use `PROTON_LOG=1` on a measured run; it turns on heavy
  `WINEDEBUG` channels. Read versions on a separate calibration launch.

**Titles:** single-player games with built-in benchmarks (Cyberpunk 2077,
Shadow of the Tomb Raider), plus two open-source native titles. No
anti-cheat or Denuvo titles (*(web)*: Denuvo limits hardware changes). No
game updates during the study.

**The manifest records:**
- kernel, Mesa, GPU;
- the effective limits: for the game, wineserver and the pressure-vessel
  helpers, `/proc/<pid>/cgroup` and `Cpus_allowed_list`, plus the slice's
  `cpuset.cpus.effective`, `memory.max`, `io.max`, `memory.events` and
  `io.stat`;
- the CPU count the game sees;
- the game's `appmanifest` build id, the Steam client, Proton, DXVK and
  vkd3d-proton versions, and the sync backend;
- the shader-cache state, the stream on or off, the hitchtrace commit and
  options.

## The runs

- **Overhead:** `tools/overhead/run.py --preset full` (10 rounds of 30 s,
  about 4.5 hours), plus a disk-bound game for perf sched's write
  interference.
- **Real titles:** each with the layer, over 30-minute sessions, under
  three conditions:
  - unconstrained;
  - Deck-like;
  - Deck-like plus background load and a cold shader cache.

  Write an unattended runner first: benchmark loop, cache wipes, background
  load, manifest and MangoHud logs. The September estimate was about 56
  hours of wall clock for 5 titles; cut to 3 titles (2 Proton, 1 native) ×
  3 conditions × 3 runs × 30 min ≈ 13.5 hours.
- **Prevalence:** with `--share 0`, the distribution of the kernel-observable
  share of each over-budget frame's excess, by cause.
- **Catch test:** the same harness on the real machine, with the same rules.
