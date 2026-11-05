#!/usr/bin/env python3
# SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
"""Measure what hitchtrace and existing tools cost a running game.

Every arm (a tool, or no tool) runs on the same workloads in shuffled order,
with no-tool runs in between, because this machine's timing drifts: each run is
compared with its neighbouring no-tool runs, never across a long stretch of
time. Slowdown comes from the game's own frame log or the benchmark's own
output, never from the tool being measured.

    tools/overhead/run.py --preset check     # every arm for a few seconds: does it work?
    tools/overhead/run.py --preset pilot     # the comparison, about 75 minutes
    tools/overhead/analyze.py build/overhead/NAME

It asks for your password through sudo once. Tools run as root; the game and
the benchmarks run as you. The game opens a window for each run, so leave the
machine alone while it works. --no-root runs only the arms that need no more
than a passwordless sudo rule for hitchtrace.

Arms       none, ht (hitchtrace), ht-notimeline, ht-stacks (-U), perfsched,
           perfoffcpu, ftrace (flight recorder), offwaketime, chaingraph, delayacct
Workloads  game60 (the game at its 60 fps cap), gamemax (uncapped), ipc (30
           server calls per frame), bg1s, bg10s (a burst of background load per
           1 s or 10 s, to vary the hitch rate), pipe-target (context switches
           inside the traced process), pipe-other (in some other process)
"""
import argparse
import json
import os
import pwd
import random
import re
import shutil
import signal
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
B = os.path.join(ROOT, "build")
GAME = f"{B}/cases/minigame"
BGLOAD = f"{B}/cases/bgload"
IPCSERVER = f"{B}/cases/ipcserver"
HITCHTRACE = f"{B}/hitchtrace"
CHAINGRAPH = f"{B}/chaingraph"
LAYER = f"{B}/libVkLayer_hitchtrace.so"
DELAYACCT = f"{B}/overhead/delayacct_frames"
TRACEFS = "/sys/kernel/tracing"
FT = "hitchoh"		# ftrace instance, uprobe group and synthetic event prefix

GAME_CPUS = [0, 1, 2, 3]
BUDGET_US = 20833
PIPE_OPS_PER_S = 70000	# about this machine's rate with no tool

ALL_ARMS = ["ht", "ht-notimeline", "ht-stacks", "perfsched", "perfoffcpu", "ftrace",
            "offwaketime", "chaingraph", "delayacct"]
BPF_ARMS = ["ht", "ht-notimeline", "ht-stacks", "offwaketime", "chaingraph"]
NO_ROOT_ARMS = ["ht", "ht-notimeline", "ht-stacks"]

PRESETS = {
    "check": dict(rounds=1, seconds=3, workloads=["game60", "pipe-target"], cost=False),
    "pilot": dict(rounds=3, seconds=15, workloads=["game60", "gamemax", "ipc", "pipe-target", "pipe-other"], cost=True),
    "full": dict(rounds=10, seconds=30, workloads=["game60", "gamemax", "ipc", "pipe-target", "pipe-other"], cost=True),
    "output": dict(rounds=2, seconds=120, workloads=["bg1s", "bg10s"], cost=False),
}

is_root = os.geteuid() == 0
user = None		# pwd entry of the person running this
CATCH = False		# --attach: tools/catch, whose scoring reads the tools' output


# ------------------------------------------------------------- processes

def child_setup(cpus):
    """Own session, pinned, and gone if this runner dies."""
    def setup():
        os.setsid()
        if cpus:
            os.sched_setaffinity(0, cpus)
        import ctypes
        ctypes.CDLL(None).prctl(1, signal.SIGTERM)	# PR_SET_PDEATHSIG
    return setup


def spawn(cmd, log, cpus=None, as_user=False, env=None, stdin=subprocess.DEVNULL):
    kw = {}
    e = dict(os.environ)
    if as_user and is_root:
        kw = dict(user=user.pw_uid, group=user.pw_gid, extra_groups=os.getgrouplist(user.pw_name, user.pw_gid))
        e.update(HOME=user.pw_dir, USER=user.pw_name, LOGNAME=user.pw_name,
                 XDG_RUNTIME_DIR=e.get("XDG_RUNTIME_DIR", f"/run/user/{user.pw_uid}"))
        e.setdefault("DISPLAY", ":0")
    e.update(env or {})
    f = open(log, "ab")
    return subprocess.Popen(cmd, stdin=stdin, stdout=f, stderr=subprocess.STDOUT, env=e,
                            preexec_fn=child_setup(cpus), close_fds=True, **kw)


def descendants(pid):
    """@pid and everything under it, from /proc."""
    kids = {}
    for d in os.listdir("/proc"):
        if d.isdigit():
            try:
                with open(f"/proc/{d}/stat") as f:
                    s = f.read()
                kids.setdefault(int(s[s.rindex(")") + 2:].split()[1]), []).append(int(d))
            except OSError:
                pass
    out, todo = [], [pid]
    while todo:
        p = todo.pop()
        out.append(p)
        todo += kids.get(p, [])
    return out


def cpu_seconds(pid):
    """CPU time of a process tree so far."""
    tick = os.sysconf("SC_CLK_TCK")
    total = 0
    for p in descendants(pid):
        try:
            with open(f"/proc/{p}/stat") as f:
                s = f.read()
            fields = s[s.rindex(")") + 2:].split()
            total += int(fields[11]) + int(fields[12])
        except OSError:
            pass
    return total / tick


def busy_seconds():
    """The whole machine's non-idle CPU time so far."""
    with open("/proc/stat") as f:
        v = [int(x) for x in f.readline().split()[1:]]
    return (sum(v[:8]) - v[3] - v[4]) / os.sysconf("SC_CLK_TCK")


def wait_for(path, pattern, timeout, proc=None):
    """Until @pattern shows up in the log; False if not, or if @proc died."""
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        try:
            with open(path, errors="replace") as f:
                if re.search(pattern, f.read()):
                    return True
        except OSError:
            pass
        if proc and proc.poll() is not None:
            return False
        time.sleep(0.05)
    return False


def tail(path, n=2000):
    try:
        with open(path, errors="replace") as f:
            return f.read()[-n:]
    except OSError:
        return ""


def marker_offset(symbol):
    """The marker's file offset in the layer, as a uprobe wants it."""
    nm = subprocess.run(["nm", "-D", LAYER], capture_output=True, text=True, check=True).stdout
    addr = next(int(l.split()[0], 16) for l in nm.splitlines() if l.endswith(" " + symbol))
    ph = subprocess.run(["readelf", "-lW", LAYER], capture_output=True, text=True, check=True).stdout
    for m in re.finditer(r"LOAD\s+(0x[0-9a-f]+)\s+(0x[0-9a-f]+)\s+\S+\s+(0x[0-9a-f]+)\s+(0x[0-9a-f]+)", ph):
        off, vaddr, _, memsz = (int(x, 16) for x in m.groups())
        if vaddr <= addr < vaddr + memsz:
            return addr - vaddr + off
    raise RuntimeError(f"{symbol} is in no segment of {LAYER}")


def sysctl_set(name, value):
    path = "/proc/sys/" + name.replace(".", "/")
    with open(path) as f:
        old = f.read().strip()
    with open(path, "w") as f:
        f.write(str(value))
    return old


# ------------------------------------------------------------------ arms

class ToolError(RuntimeError):
    """The tool itself would not start or attach, as opposed to the workload failing."""


class Arm:
    """One tool. start() has it tracing @pid; stop() ends it and says what it did."""
    name = "none"
    ready = None		# pattern in its log once it traces; None: just wait a moment
    ready_timeout = 60
    summary = False		# writes one summary at exit, so its output does not grow with time

    def __init__(self, rundir, pid, tool_cpus):
        self.dir, self.pid, self.cpus = rundir, pid, tool_cpus
        self.log = f"{rundir}/tool.log"
        self.proc = None
        self.outputs = []
        self.t_start = None

    def command(self):
        return None

    def start(self):
        cmd = self.command()
        if not cmd:
            return
        self.proc = spawn(cmd, self.log, cpus=self.cpus, env={"PYTHONUNBUFFERED": "1"})
        if self.ready:
            if not wait_for(self.log, self.ready, self.ready_timeout, self.proc):
                raise RuntimeError(f"{self.name} did not start: {tail(self.log, 800)}")
        else:
            time.sleep(1.5)
            if self.proc.poll() is not None:
                raise RuntimeError(f"{self.name} exited at once: {tail(self.log, 800)}")

    def cpu(self):
        return cpu_seconds(self.proc.pid) if self.proc else 0.0

    def stop(self):
        if self.proc:
            self.signal()
            try:
                self.proc.wait(timeout=60)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        return self.results()

    def signal(self):
        try:
            os.killpg(self.proc.pid, signal.SIGINT)
        except ProcessLookupError:
            pass

    def results(self):
        return {}

    def output_bytes(self):
        return sum(os.path.getsize(p) for p in self.outputs if os.path.exists(p))

    def cleanup(self, keep):
        if not keep:
            for p in self.outputs:
                if os.path.exists(p):
                    os.unlink(p)


class Hitchtrace(Arm):
    name = "ht"
    extra = []

    def command(self):
        self.out = f"{self.dir}/hitch.jsonl"
        open(self.out, "a").close()	# ours, so it stays ours when root appends
        self.outputs = [self.out]
        cmd = [HITCHTRACE, "-p", str(self.pid), "-x", LAYER, "-b", str(BUDGET_US), "-o", self.out, "-q",
               "--stop-on-eof"] + self.extra
        return cmd if is_root else ["sudo", "-n"] + cmd

    def start(self):
        cmd = self.command()
        self.proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=open(self.log, "ab"),
                                     stderr=subprocess.STDOUT, preexec_fn=child_setup(self.cpus))
        time.sleep(1.5)
        if self.proc.poll() is not None:
            raise RuntimeError(f"hitchtrace exited at once: {tail(self.log, 800)}")

    def signal(self):
        self.proc.stdin.close()		# a root process: this is how it is asked to stop

    def results(self):
        m = re.search(r"stats: frames=(\d+) hitches=(\d+) emitted=(\d+) ringbuf-full=(\d+)", tail(self.log))
        r = {"drops": int(m.group(4)), "tool_frames": int(m.group(1))} if m else {}
        ends = []
        try:
            with open(self.out) as f:
                for line in f:
                    if '"frame_id"' in line:
                        ends.append(int(re.search(r'"frame_end_ns":(\d+)', line).group(1)))
        except OSError:
            pass
        r["record_ends_ns"] = ends
        return r


class HitchtraceNoTimeline(Hitchtrace):
    name = "ht-notimeline"
    extra = ["--no-timeline"]


class HitchtraceStacks(Hitchtrace):
    name = "ht-stacks"
    extra = ["-U"]


class PerfSched(Arm):
    name = "perfsched"

    def command(self):
        self.out = f"{self.dir}/perf.data"
        self.outputs = [self.out]
        clock = ["-k", "CLOCK_MONOTONIC"] if CATCH else []	# comparable with the game's log
        return ["perf", "sched", "record", *clock, "-o", self.out, "--", "sleep", "100000"]

    def results(self):
        t = tail(self.log, 4000)
        m = re.search(r"lost (\d+) chunks", t)
        return {"drops": int(m.group(1)) if m else 0, "captures_all": True}


class PerfOffCpu(Arm):
    name = "perfoffcpu"

    def command(self):
        self.out = f"{self.dir}/perf.data"
        self.outputs = [self.out]
        return ["perf", "record", "--off-cpu", "--off-cpu-thresh", "1", "-p", str(self.pid), "-o", self.out]

    def results(self):
        m = re.search(r"lost (\d+) chunks", tail(self.log, 4000))
        return {"drops": int(m.group(1)) if m else 0}


class Offwaketime(Arm):
    name = "offwaketime"
    summary = True
    ready = r"Tracing blocked"

    def command(self):
        return ["offwaketime-bpfcc", *(["-f"] if CATCH else []), "-p", str(self.pid)]

    def start(self):
        if not CATCH:
            return super().start()
        # folded output has no "Tracing" line: it traces once its programs are in
        before = bpf_prog_ids()
        self.proc = spawn(self.command(), self.log, cpus=self.cpus, env={"PYTHONUNBUFFERED": "1"})
        end = time.monotonic() + self.ready_timeout
        while time.monotonic() < end:
            if any(p.get("name") == "waker" for i, p in bpf_prog_ids().items() if i not in before):
                time.sleep(0.5)
                return
            if self.proc.poll() is not None:
                break
            time.sleep(0.2)
        raise RuntimeError(f"{self.name} did not start: {tail(self.log, 800)}")

    def output_bytes(self):
        return os.path.getsize(self.log) if os.path.exists(self.log) else 0


class Chaingraph(Arm):
    name = "chaingraph"
    summary = True
    ready = r"Tracing off-CPU"

    def command(self):
        return [CHAINGRAPH, "-p", str(self.pid), "-f"]

    def output_bytes(self):
        return os.path.getsize(self.log) if os.path.exists(self.log) else 0


class Delayacct(Arm):
    name = "delayacct"
    ready = r"Tracing delays"

    def command(self):
        # a thread gets its delay record when it is created, and only if
        # accounting is on then: enabling it later leaves running threads out
        with open("/proc/sys/kernel/task_delayacct") as f:
            if f.read().strip() != "1":
                raise RuntimeError("kernel.task_delayacct must be on before the traced program starts")
        self.out = f"{self.dir}/delays.jsonl"
        self.outputs = [self.out]
        return [DELAYACCT, str(self.pid), LAYER, str(marker_offset("hitch_frame_mark")), str(BUDGET_US), self.out]

    def signal(self):
        self.proc.terminate()

    def results(self):
        m = re.search(r"frames=(\d+) over_budget=(\d+) lines=(\d+) lost=(\d+)", tail(self.log))
        r = {"tool_frames": int(m.group(1)), "drops": int(m.group(4))} if m else {}
        ends = set()		# a slow frame has a line per thread whose delays changed
        try:
            with open(self.out) as f:
                for line in f:
                    ends.add(int(re.search(r'"end_ns":(\d+)', line).group(1)))
        except OSError:
            pass
        r["record_ends_ns"] = sorted(ends)
        return r


def tw(path, text):
    """One write to a tracefs file. Not through open(): "w" truncates, which on a
    trigger or probe file removes everything already there, and "a" seeks to the
    end, which seq_file files refuse with EINVAL."""
    fd = os.open(path, os.O_WRONLY)
    try:
        os.write(fd, text.encode())
    finally:
        os.close(fd)


class Ftrace(Arm):
    """A flight recorder: scheduler events in an overwriting ring, and a snapshot
    when a frame runs long. ftrace can pair two events, so a frame here runs from
    the return of present to the next present: present's own wait is not in it."""
    name = "ftrace"
    SNAP = f"snapshot if lat > {BUDGET_US}"
    COUNT = f"hist:keys=common_pid:vals=hitcount if lat > {BUDGET_US}"
    PEND = "hist:keys=common_pid:ts0=common_timestamp.usecs"
    MARK = f"hist:keys=common_pid:lat=common_timestamp.usecs-$ts0:onmatch({FT}.pend).{FT}_lat($lat)"

    @classmethod
    def teardown(cls):
        """Remove whatever a run left. Quietly: most of it may not be there."""
        inst = f"{TRACEFS}/instances/{FT}"
        steps = [(f"{inst}/tracing_on", "0"),
                 (f"{inst}/events/synthetic/{FT}_lat/trigger", "!" + cls.SNAP),
                 (f"{inst}/events/synthetic/{FT}_lat/trigger", "!" + cls.COUNT),
                 (f"{inst}/events/{FT}/fmark/trigger", "!" + cls.MARK),
                 (f"{inst}/events/{FT}/pend/trigger", "!" + cls.PEND)]
        for path, text in steps:
            try:
                tw(path, text)
            except OSError:
                pass
        try:
            os.rmdir(inst)
        except OSError:
            pass
        for path, text in ((f"{TRACEFS}/synthetic_events", f"!{FT}_lat"),
                           (f"{TRACEFS}/uprobe_events", f"-:{FT}/fmark"),
                           (f"{TRACEFS}/uprobe_events", f"-:{FT}/pend")):
            try:
                tw(path, text)
            except OSError:
                pass

    def command(self):
        self.teardown()
        inst = f"{TRACEFS}/instances/{FT}"
        steps = [(f"{TRACEFS}/uprobe_events", f"p:{FT}/pend {LAYER}:{marker_offset('hitch_present_end'):#x}\n"),
                 (f"{TRACEFS}/uprobe_events", f"p:{FT}/fmark {LAYER}:{marker_offset('hitch_frame_mark'):#x}\n"),
                 # raised from the game thread's own uprobe, so its common_pid is that thread
                 (f"{TRACEFS}/synthetic_events", f"{FT}_lat u64 lat\n"),
                 (inst, None),
                 *([(f"{inst}/trace_clock", "mono")] if CATCH else []),
                 (f"{inst}/buffer_size_kb", "1024"),
                 (f"{inst}/events/{FT}/pend/trigger", self.PEND),
                 (f"{inst}/events/{FT}/fmark/trigger", self.MARK),
                 (f"{inst}/snapshot", "1"),
                 (f"{inst}/snapshot", "2"),
                 (f"{inst}/events/synthetic/{FT}_lat/trigger", self.COUNT),
                 (f"{inst}/events/synthetic/{FT}_lat/trigger", self.SNAP),
                 (f"{inst}/events/sched/sched_switch/enable", "1"),
                 (f"{inst}/events/sched/sched_waking/enable", "1"),
                 # the frame marks themselves, so frames can be found in a snapshot
                 (f"{inst}/events/{FT}/fmark/enable", "1"),
                 (f"{inst}/events/{FT}/pend/enable", "1"),
                 (f"{inst}/tracing_on", "1")]
        for path, text in steps:
            try:
                if text is None:
                    os.mkdir(path)
                else:
                    tw(path, text)
            except OSError as e:
                # hist trigger errors go to the instance's own log, which teardown removes
                log = tail(f"{TRACEFS}/error_log", 1500) + tail(f"{inst}/error_log", 1500)
                self.teardown()
                raise RuntimeError(f"ftrace setup failed: {os.path.relpath(path, TRACEFS)} "
                                   f"<- {text!r}: {e}\n{log}") from e
        with open(f"{inst}/trace_clock") as f:
            self.clock = re.search(r"\[(\S+)\]", f.read()).group(1)	# the one in effect
        self.out = f"{self.dir}/snapshots.{'txt' if CATCH else 'bin'}"
        self.stats = f"{self.dir}/drain.json"
        self.outputs = [self.out]
        return [sys.executable, os.path.abspath(__file__), "--ftrace-drain", self.out, self.stats,
                *(["text"] if CATCH else [])]

    def signal(self):
        self.proc.terminate()

    def results(self):
        try:
            with open(self.stats) as f:
                r = json.load(f)
        except (OSError, ValueError):
            r = {}
        self.teardown()
        r["record_ends_ns"] = r.pop("hit_ns", [])	# up to one poll (0.1 s) late
        r["trace_clock"] = getattr(self, "clock", None)
        return r


def ftrace_drain(out, stats, text=False):
    """The flight recorder's reader, a process of its own so that its CPU is counted.
    With @text, each snapshot is saved formatted, for reading, rather than raw."""
    inst = f"{TRACEFS}/instances/{FT}"
    stop = []
    signal.signal(signal.SIGTERM, lambda *a: stop.append(1))
    signal.signal(signal.SIGINT, lambda *a: stop.append(1))
    seen = drains = overwritten = 0
    drain_s = 0.0
    hit_ns = []		# when each slow frame was noticed: the hist has counts, not times
    with open(out, "wb") as f:
        while not stop:
            time.sleep(0.1)
            if text:
                # CLOCK_MONOTONIC as the trace's clock sees it, ten times a second, so
                # that every snapshot can be put on the game's clock whatever its own
                try:
                    tw(f"{inst}/trace_marker", f"{FT}_mono={time.monotonic_ns()}")
                except OSError:
                    pass
            try:
                with open(f"{inst}/events/synthetic/{FT}_lat/hist") as h:
                    m = re.search(r"Hits:\s*(\d+)", h.read())
            except OSError:
                continue
            hits = int(m.group(1)) if m else 0
            if hits <= seen:
                continue
            hit_ns += [time.monotonic_ns()] * (hits - seen)
            t0 = time.monotonic()
            if text:
                f.write(f"### snapshot {hit_ns[-1]}\n".encode())
                with open(f"{inst}/snapshot", "rb") as snap:
                    shutil.copyfileobj(snap, f)
            for cpu in ([] if text else os.listdir(f"{inst}/per_cpu")):
                try:
                    fd = os.open(f"{inst}/per_cpu/{cpu}/snapshot_raw", os.O_RDONLY | os.O_NONBLOCK)
                except OSError:
                    continue
                try:
                    while True:
                        b = os.read(fd, 1 << 16)
                        if not b:
                            break
                        f.write(b)
                except OSError:
                    pass
                os.close(fd)
            f.flush()		# so the runner can measure output inside its window
            tw(f"{inst}/snapshot", "2")
            drain_s += time.monotonic() - t0
            overwritten += hits - seen - 1
            drains += 1
            seen = hits
    overrun = 0
    for cpu in os.listdir(f"{inst}/per_cpu"):
        try:
            with open(f"{inst}/per_cpu/{cpu}/stats") as s:
                overrun += int(re.search(r"overrun: (\d+)", s.read()).group(1))
        except (OSError, AttributeError):
            pass
    with open(stats, "w") as s:
        json.dump({"caught": seen, "drains": drains, "drops": overwritten, "drain_s": round(drain_s, 3),
                   "ring_overrun_events": overrun, "hit_ns": hit_ns}, s)


ARMS = {c.name: c for c in (Arm, Hitchtrace, HitchtraceNoTimeline, HitchtraceStacks, PerfSched, PerfOffCpu,
                            Ftrace, Offwaketime, Chaingraph, Delayacct)}


# ------------------------------------------------------------- bpf costs

def bpf_prog_ids():
    r = subprocess.run(["bpftool", "prog", "show", "-j"], capture_output=True, text=True)
    return {p["id"]: p for p in json.loads(r.stdout or "[]")}


# ------------------------------------------------------------- workloads

def percentile(xs, q):
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(len(xs) * q))] if xs else None


def frame_stats(path, t0, t1):
    with open(path) as f:
        frames = [json.loads(l) for l in f]
    win = [x for x in frames if t0 <= x["t_end_ns"] <= t1]
    if not win:
        return {"error": "no frames in the window"}
    us = [x["frame_us"] for x in win]
    slowest = sorted(us, reverse=True)[:max(1, len(us) // 100)]
    return {
        "frames": len(win), "fps": len(win) / ((t1 - t0) / 1e9),
        "frame_p50_us": percentile(us, 0.50), "frame_p99_us": percentile(us, 0.99),
        "low1_fps": 1e6 / (sum(slowest) / len(slowest)),
        "over_budget": sum(u > BUDGET_US for u in us),
        "jobs_p50_us": percentile([x["jobs_us"] for x in win], 0.50),
        "calls_p50_us": percentile([x["calls_us"] for x in win], 0.50),
        "present_p50_us": percentile([x["present_us"] for x in win], 0.50),
    }


def abandon(arm):
    """A run that failed part-way must not leave its tool tracing the next one."""
    if arm and arm.proc and arm.proc.poll() is None:
        try:
            arm.stop()
        except Exception:
            arm.proc.kill()


def accounting_on(arm_name):
    """Delay accounting for the delayacct arm, on before the workload's threads
    are created (they are accounted only if it was on then); the old value."""
    return sysctl_set("kernel.task_delayacct", 1) if arm_name == "delayacct" else None


def run_game(workload, arm_name, rundir, seconds, tool_cpus, cost):
    frames = f"{rundir}/frames.jsonl"
    args = ["-d", str(seconds + 600), "-w", "3", "-n", "120", "-o", frames]
    helpers = []
    arm = None
    if workload == "gamemax":
        args += ["-f", "1000"]
    server_cpu = 5 if os.cpu_count() > 5 else os.cpu_count() - 1
    da_old = accounting_on(arm_name)
    try:
        if workload == "ipc":
            sock = f"{rundir}/srv.sock"
            helpers.append(spawn([IPCSERVER, sock], f"{rundir}/server.log", cpus=[server_cpu], as_user=True))
            if not wait_for(f"{rundir}/server.log", "socket=", 5):
                raise RuntimeError("the server did not start")
            args += ["-i", sock, "-c", "30"]
        game = spawn([GAME] + args, f"{rundir}/game.log", cpus=GAME_CPUS, as_user=True, env={"HITCHTRACE": "1"})
        helpers.append(game)
        time.sleep(3.0)		# its first seconds have slow frames of their own
        if game.poll() is not None:
            raise RuntimeError(f"the game did not start: {tail(rundir + '/game.log', 500)}")
        before = bpf_prog_ids() if cost else None
        arm = ARMS[arm_name](rundir, game.pid, tool_cpus)
        try:
            arm.start()
        except Exception as e:
            raise ToolError(str(e)) from e
        time.sleep(1.0)
        if workload in ("bg1s", "bg10s"):
            period = 1000 if workload == "bg1s" else 10000
            helpers.append(spawn([BGLOAD, "-t", "4", "-p", str(period), "-b", "20", "-d", str(seconds + 600)],
                                 f"{rundir}/bgload.log", cpus=GAME_CPUS, as_user=True))
        c0, b0, o0, t0 = arm.cpu(), busy_seconds(), arm.output_bytes(), time.monotonic_ns()
        time.sleep(seconds)
        c1, b1, o1, t1 = arm.cpu(), busy_seconds(), arm.output_bytes(), time.monotonic_ns()
        progs = None
        if cost and before is not None:
            after = bpf_prog_ids()
            progs = [{"name": p.get("name"), "run_cnt": p.get("run_cnt", 0), "run_time_ns": p.get("run_time_ns", 0)}
                     for i, p in after.items() if i not in before]
        res = arm.stop()
        ends = res.pop("record_ends_ns", None)
        bytes_out = arm.output_bytes() if arm.summary else o1 - o0
    finally:
        abandon(arm)
        if da_old is not None:
            sysctl_set("kernel.task_delayacct", da_old)
        # last started first: the game must be gone before its server, or its
        # next call fails and it exits without writing its frame log
        for h in reversed(helpers):
            if h.poll() is None:
                h.terminate()
            try:
                h.wait(timeout=20)
            except subprocess.TimeoutExpired:
                h.kill()
                h.wait()
    r = {"window_s": (t1 - t0) / 1e9, "tool_cpu_s": c1 - c0, "busy_s": b1 - b0, "bytes": bytes_out,
         "summary": arm.summary}
    r.update(res)
    r.update(frame_stats(frames, t0, t1))
    if ends is not None:
        r["caught"] = sum(t0 <= e <= t1 for e in ends)
    if progs is not None:
        r["bpf_progs"] = progs
    arm.cleanup(keep=False)
    return r


def run_pipe(workload, arm_name, rundir, seconds, tool_cpus, cost):
    ops = PIPE_OPS_PER_S * seconds
    bench = ["perf", "bench", "sched", "pipe", "-l", str(ops)]
    other = proc = arm = None
    log = f"{rundir}/bench.log"
    da_old = accounting_on(arm_name)
    try:
        if workload == "pipe-target":
            # stopped until the tool is on it; exec keeps the process id
            proc = spawn(["sh", "-c", 'kill -STOP $$; exec "$@"', "sh"] + bench + ["-T"], log, cpus=GAME_CPUS, as_user=True)
            for _ in range(200):
                with open(f"/proc/{proc.pid}/stat") as f:
                    if ") T " in f.read():
                        break
                time.sleep(0.01)
            target = proc.pid
        else:
            other = spawn(["sleep", "100000"], f"{rundir}/other.log", as_user=True)
            target = other.pid
            proc = None
        before = bpf_prog_ids() if cost else None
        arm = ARMS[arm_name](rundir, target, tool_cpus)
        try:
            arm.start()
        except Exception as e:
            raise ToolError(str(e)) from e
        time.sleep(0.5)
        c0, b0, o0, t0 = arm.cpu(), busy_seconds(), arm.output_bytes(), time.monotonic_ns()
        if proc:
            os.kill(proc.pid, signal.SIGCONT)
        else:
            proc = spawn(bench, log, cpus=GAME_CPUS, as_user=True)
        proc.wait(timeout=seconds * 40 + 60)
        c1, b1, o1, t1 = arm.cpu(), busy_seconds(), arm.output_bytes(), time.monotonic_ns()
        progs = None
        if cost and before is not None:
            after = bpf_prog_ids()
            progs = [{"name": p.get("name"), "run_cnt": p.get("run_cnt", 0), "run_time_ns": p.get("run_time_ns", 0)}
                     for i, p in after.items() if i not in before]
        res = arm.stop()
        res.pop("record_ends_ns", None)
        bytes_out = arm.output_bytes() if arm.summary else o1 - o0
    finally:
        abandon(arm)
        if da_old is not None:
            sysctl_set("kernel.task_delayacct", da_old)
        for p in (other, proc):
            if p and p.poll() is None:
                if p is proc:
                    os.kill(p.pid, signal.SIGCONT)
                p.terminate()
                p.wait()
    m = re.search(r"([\d.]+) usecs/op", tail(log))
    if not m:
        return {"error": f"no result from the benchmark: {tail(log, 300)}"}
    r = {"window_s": (t1 - t0) / 1e9, "tool_cpu_s": c1 - c0, "busy_s": b1 - b0, "bytes": bytes_out,
         "summary": arm.summary, "usecs_per_op": float(m.group(1)), "ops": ops}
    r.update(res)
    if progs is not None:
        r["bpf_progs"] = progs
    arm.cleanup(keep=False)
    return r


WORKLOADS = {"game60": run_game, "gamemax": run_game, "ipc": run_game, "bg1s": run_game, "bg10s": run_game,
             "pipe-target": run_pipe, "pipe-other": run_pipe}


# ------------------------------------------------------------------ main

def sequence(arms, rng):
    """Shuffled arms, starting with no tool and with no tool after every second arm."""
    order = list(arms)
    rng.shuffle(order)
    seq = ["none"]
    for i, a in enumerate(order):
        seq.append(a)
        if i % 2 == 1 or i == len(order) - 1:
            seq.append("none")
    return seq


def describe():
    def sh(*c):
        try:
            return subprocess.run(c, capture_output=True, text=True, timeout=5).stdout.strip()
        except OSError:
            return ""
    return {"date": time.strftime("%Y-%m-%dT%H:%M:%S%z"), "kernel": os.uname().release, "cpus": os.cpu_count(),
            "cpu": next((l.split(":")[1].strip() for l in open("/proc/cpuinfo") if l.startswith("model name")), ""),
            "hypervisor": "hypervisor" in open("/proc/cpuinfo").read(), "perf": sh("perf", "--version"),
            "commit": sh("git", "-C", ROOT, "describe", "--always", "--dirty"), "budget_us": BUDGET_US,
            "game_cpus": GAME_CPUS}


def unavailable_arms():
    """Arms this machine cannot run, and why, found before any run rather than by failing."""
    out = {}
    try:
        opts = subprocess.run(["perf", "version", "--build-options"], capture_output=True, text=True).stdout
        if re.search(r"bpf_skeletons:\s*\[\s*OFF", opts):
            out["perfoffcpu"] = "this perf is built without BPF skeletons, which --off-cpu needs"
    except OSError:
        out["perfoffcpu"] = out["perfsched"] = "no perf"
    if not shutil.which("offwaketime-bpfcc"):
        out["offwaketime"] = "no offwaketime-bpfcc (bpfcc-tools)"
    elif not os.path.isdir(f"/lib/modules/{os.uname().release}/build"):
        out["offwaketime"] = "no kernel headers for bcc"
    for a, path in (("chaingraph", CHAINGRAPH), ("delayacct", DELAYACCT)):
        if not os.path.exists(path):
            out[a] = f"no {os.path.relpath(path, ROOT)}"
    return out


def attach(tool, pid, d, seconds):
    """One tool on a process someone else started (cases/run.sh, for tools/catch):
    trace it for @seconds, or until it exits, and keep the output in @d.
    tool.ready appears once it traces; tool.json says what it did, and when."""
    global CATCH
    CATCH = True
    if tool not in ARMS or tool == "none":
        sys.exit(f"unknown tool {tool}")
    why = unavailable_arms().get(tool)
    if why:
        sys.exit(f"{tool} cannot run here: {why}")
    stop = []
    signal.signal(signal.SIGTERM, lambda *a: stop.append(1))
    signal.signal(signal.SIGINT, lambda *a: stop.append(1))
    arm = ARMS[tool](d, pid, None)
    res = {}
    try:
        arm.start()
        t0 = time.monotonic_ns()
        with open(f"{d}/tool.ready", "w") as f:
            f.write(f"{t0}\n")
        while not stop and time.monotonic_ns() - t0 < seconds * 1e9 and os.path.exists(f"/proc/{pid}"):
            time.sleep(0.1)
        t1 = time.monotonic_ns()
        res = arm.stop()
        res.pop("record_ends_ns", None)
        res.update(tool=tool, t_start_ns=t0, t_end_ns=t1, bytes=arm.output_bytes(), summary=arm.summary,
                   outputs=[os.path.basename(p) for p in arm.outputs] or ["tool.log"])
    finally:
        abandon(arm)
        if tool == "ftrace":
            Ftrace.teardown()
        with open(f"{d}/tool.json", "w") as f:
            json.dump(res, f)
        chown_tree(d)
    return 0


def chown_tree(path):
    if is_root:
        for d, dirs, files in os.walk(path):
            for n in [d] + [os.path.join(d, x) for x in dirs + files]:
                try:
                    os.chown(n, user.pw_uid, user.pw_gid)
                except OSError:
                    pass


def one_run(out, seq_no, round_no, workload, arm, seconds, placement, phase, runs):
    rundir = f"{out}/runs/{seq_no:04d}-{workload}-{arm}"
    os.makedirs(rundir)
    chown_tree(rundir)
    tool_cpus = GAME_CPUS if placement == "shared" else None
    started = time.time()
    try:
        r = WORKLOADS[workload](workload, arm, rundir, seconds, tool_cpus, phase == "cost" and arm != "none")
    except Exception as e:	# one arm failing must not lose the rest
        r = {"error": str(e)[:1500]}
        if isinstance(e, ToolError):
            r["tool_failed"] = True
    r.update(seq=seq_no, round=round_no, workload=workload, arm=arm, placement=placement, phase=phase,
             wall_s=round(time.time() - started, 1))
    with open(f"{out}/runs.jsonl", "a") as f:
        f.write(json.dumps(r) + "\n")
    chown_tree(rundir)
    os.sync()		# the last tool's writes must not land on the next run
    time.sleep(1.0)
    runs.append(r)
    status = "FAILED: " + r["error"][:200] if "error" in r else (
        f"{r.get('usecs_per_op', '')} us/op" if "usecs_per_op" in r else
        f"{r['fps']:.1f} fps, jobs {r['jobs_p50_us'] / 1000:.1f} ms")
    print(f"[{seq_no:4d}] {workload:12} {arm:14} {status}", flush=True)
    return r


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--preset", choices=PRESETS, default="check")
    ap.add_argument("--rounds", type=int)
    ap.add_argument("--seconds", type=int)
    ap.add_argument("--workloads", help="comma-separated; default from the preset")
    ap.add_argument("--arms", help="comma-separated; default all that can run")
    ap.add_argument("--placement", choices=["shared", "free"], default="shared",
                    help="shared: the tool's own process is pinned to the game's CPUs (default); free: it may run anywhere")
    ap.add_argument("--name", help="directory name under build/overhead")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--no-root", action="store_true", help="only the hitchtrace arms, through sudo -n")
    ap.add_argument("--attach", nargs=4, metavar=("TOOL", "PID", "DIR", "SECONDS"),
                    help="trace PID with one tool for SECONDS and keep its output in DIR (for tools/catch)")
    ap.add_argument("--ftrace-drain", nargs="+", help=argparse.SUPPRESS)
    args = ap.parse_args()

    if args.ftrace_drain:
        ftrace_drain(args.ftrace_drain[0], args.ftrace_drain[1], text=args.ftrace_drain[2:] == ["text"])
        return 0

    if not is_root and not args.no_root:
        keep = "DISPLAY,XAUTHORITY,WAYLAND_DISPLAY,XDG_RUNTIME_DIR"
        os.execvp("sudo", ["sudo", f"--preserve-env={keep}", sys.executable, os.path.abspath(__file__)] + sys.argv[1:])

    global user
    user = pwd.getpwnam(os.environ.get("SUDO_USER") or pwd.getpwuid(os.getuid()).pw_name)
    if args.attach:
        tool, pid, d, secs = args.attach
        return attach(tool, int(pid), d, float(secs))

    missing = [p for p in (GAME, BGLOAD, IPCSERVER, HITCHTRACE, LAYER) if not os.path.exists(p)]
    if missing:
        sys.exit(f"missing {', '.join(missing)}: run make, make layer layer-install, make -C cases")
    if subprocess.run(["pgrep", "-x", "mg_main"], capture_output=True).returncode == 0:
        sys.exit("a copy of the game is already running: stop it first")

    p = PRESETS[args.preset]
    rounds = args.rounds or p["rounds"]
    seconds = args.seconds or p["seconds"]
    workloads = args.workloads.split(",") if args.workloads else p["workloads"]
    arms = args.arms.split(",") if args.arms else (NO_ROOT_ARMS if args.no_root else ALL_ARMS)
    arms = [a for a in arms if a != "none"]
    unavailable = {a: why for a, why in unavailable_arms().items() if a in arms}
    for a, why in unavailable.items():
        print(f"skipping {a}: {why}", flush=True)
    arms = [a for a in arms if a not in unavailable]
    name = args.name or time.strftime(f"{args.preset}-%Y%m%d-%H%M%S")
    out = f"{B}/overhead/{name}"
    os.makedirs(f"{out}/runs")
    chown_tree(f"{B}/overhead")
    rng = random.Random(args.seed)
    info = describe()
    info.update(preset=args.preset, rounds=rounds, seconds=seconds, workloads=workloads, arms=arms,
                placement=args.placement, seed=args.seed, no_root=args.no_root, unavailable=unavailable)
    with open(f"{out}/env.json", "w") as f:
        json.dump(info, f, indent=1)

    plan = []
    for r in range(rounds):
        order = list(workloads)
        rng.shuffle(order)
        for w in order:
            plan += [(r, w, a, "main") for a in sequence(arms, rng)]
    if p["cost"] and is_root:
        costs = [a for a in arms if a in BPF_ARMS]
        plan += [(0, w, a, "cost") for w in ("game60", "pipe-target") for a in costs for _ in range(2)]
    est = len(plan) * (seconds + 7) / 60
    print(f"{len(plan)} runs, about {est:.0f} minutes; results in {os.path.relpath(out, ROOT)}", flush=True)

    runs = []
    failed = {}
    old_stats = sysctl_set("kernel.bpf_stats_enabled", 0) if is_root else None
    old_delayacct = open("/proc/sys/kernel/task_delayacct").read().strip()
    seq_no = 0
    try:
        for r, w, a, phase in plan:
            if failed.get(a, 0) >= 2:
                continue		# it does not work here; say so once, in the results
            if is_root:
                sysctl_set("kernel.bpf_stats_enabled", 1 if phase == "cost" else 0)
            seq_no += 1
            res = one_run(out, seq_no, r, w, a, seconds, args.placement, phase, runs)
            if res.get("tool_failed"):	# not the workload's failures, nor the baseline's
                failed[a] = failed.get(a, 0) + 1
    except KeyboardInterrupt:
        print("stopped; what ran so far is in the results", flush=True)
    finally:
        if is_root:
            sysctl_set("kernel.bpf_stats_enabled", old_stats)
            sysctl_set("kernel.task_delayacct", old_delayacct)
            Ftrace.teardown()
        chown_tree(out)

    print()
    for a in ["none"] + arms:
        rs = [x for x in runs if x["arm"] == a]
        bad = [x for x in rs if "error" in x]
        print(f"{a:14} {len(rs) - len(bad)} ok, {len(bad)} failed" + (f": {bad[0]['error'][:300]}" if bad else ""))
    print(f"\nnext: tools/overhead/analyze.py {os.path.relpath(out, ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
