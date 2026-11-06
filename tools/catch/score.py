#!/usr/bin/env python3
# SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
"""Score a tools/catch/run.sh directory by the rules in docs/catch-test.md.

Per injected hitch (the case's cause at work, by ground truth no tool
supplies, and the game's own log over budget): did each tool's output record
the frame, attribute its extra time to the cause, and name the culprit? For
the tools that print one summary per run: is the cause among the summary's
ten heaviest lines? Writes summary.md and scores.json next to the runs.

usage: score.py DIR
"""
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "cases"))
import analyze_cases as ac	# noqa: E402  the case studies' own rules, so ht is judged the same

MATCH_NS = 1_000_000		# a tool's frame end vs the game's own
MIN_OVERLAP_NS = 1_000_000	# a frame is in a burst if it overlaps it this much
LEAD_NS = 500_000_000		# after the tool says it traces
TRAIL_NS = 300_000_000		# before it stops
TOP = 10			# "visible": among this many heaviest summary lines
SIGNATURE = {"bgcpu": ("bgload",), "ipc": ("bgload",), "iothread": ("io_schedule",),
             "mem": ("do_swap_page", "swap_read")}
DELAY = {"bgcpu": "cpu_ns", "iothread": "blkio_ns", "mem": "swapin_ns"}
DELAYS = ("cpu_ns", "blkio_ns", "swapin_ns", "reclaim_ns", "compact_ns", "thrashing_ns")


def load(path):
    with open(path) as f:
        return [json.loads(line) for line in f if line.strip()]


# ------------------------------------------------------------------ ground truth

def ground_truth(d, case, tool):
    """The run's frames inside the tool's window: injected and noise hitches."""
    man = json.load(open(f"{d}/manifest.json"))
    frames = load(f"{d}/frames.jsonl")
    budget_ns = man["budget_us"] * 1000
    if tool == "ht":
        lines = load(f"{d}/hitch.jsonl")
        lo = lines[0]["trace_start_ns"] + ac.ATTACH_NS
        hi = lines[-1]["trace_end_ns"] - ac.DETACH_NS
    else:
        t = json.load(open(f"{d}/tool.json"))
        lo, hi = t["t_start_ns"] + LEAD_NS, t["t_end_ns"] - TRAIL_NS
    bursts = load(f"{d}/bursts.jsonl") if case in ("bgcpu", "ipc") else []

    def affected(f):
        if case in ("bgcpu", "ipc"):
            s, e = f["t_end_ns"] - f["frame_us"] * 1000, f["t_end_ns"]
            return sum(max(0, min(e, b["end_ns"]) - max(s, b["start_ns"])) for b in bursts) >= MIN_OVERLAP_NS
        if case == "iothread":
            return bool(f["asset"])
        return bool(f["step"])		# mem walks, cpuspike burns

    win = [f for f in frames if f["t_end_ns"] - f["frame_us"] * 1000 > lo and f["t_end_ns"] < hi]
    over = [f for f in win if f["frame_us"] * 1000 > budget_ns]
    return ([f for f in over if affected(f)], [f for f in over if not affected(f)],
            budget_ns, (hi - lo) / 1e9)


def span(f):
    return f["t_end_ns"] - f["frame_us"] * 1000, f["t_end_ns"]


def nearest(items, key, t):
    best = min(items, key=lambda x: abs(key(x) - t), default=None)
    return best if best is not None and abs(key(best) - t) <= MATCH_NS else None


# ------------------------------------------------------------------ per-frame tools

def score_ht(d, case, frame, budget_ns, cache={}):
    recs = cache.get(d)
    if recs is None:
        recs = cache[d] = [r for r in load(f"{d}/hitch.jsonl") if "frame_id" in r]
    r = nearest(recs, lambda r: r["frame_end_ns"], frame["t_end_ns"])
    if r is None:
        return False, False, False
    excess = r["frame_ns"] - r["budget_ns"]
    c = r["cause_ns"]
    culprit = None
    if case == "bgcpu":
        att = any(t["cause_ns"]["runnable"] + t["cause_ns"]["runqueue"] >= 0.5 * excess for t in r["threads"])
        culprit = ac.names(r, "bgload")
    elif case == "iothread":
        grp = [s for s in r["stalls"] if ac.from_stream(s) and s["hops"][0]["cause"] == "block_io"]
        att = bool(grp)
        culprit = any(len(s["hops"]) > 1 and ac.is_irq(s["hops"][1]) for s in grp)
    elif case == "ipc":
        grp = [s for s in r["stalls"] if ac.from_server(s) and s["hops"][0]["cause"] == "runnable"]
        att = bool(grp)
        culprit = any((s["hops"][0].get("preemptor") or {}).get("comm", "").startswith("bgload") for s in grp)
    elif case == "mem":
        fio = [s for s in r["stalls"] if ac.is_fault_io(s)]
        mem_ns = (c["oncpu_fault"] + c["oncpu_fault_major"] + c["oncpu_reclaim"] + c["oncpu_compact"]
                  + sum(s["ns"] for s in fio))
        att = mem_ns >= 0.5 * excess
    else:		# cpuspike: running, and no stall to speak of
        top = ac.by_stall_time(r)
        att = c["oncpu"] >= 0.5 * excess and (ac.stall_time(top) if top else 0) < 0.25 * excess
    return True, att, (att and culprit) if culprit is not None else None


def score_delayacct(d, case, frame, budget_ns, cache={}):
    by_end = cache.get(d)
    if by_end is None:
        by_end = {}
        for l in load(f"{d}/delays.jsonl") if os.path.exists(f"{d}/delays.jsonl") else []:
            by_end.setdefault(l["end_ns"], []).append(l)
        cache[d] = by_end
    end = nearest(list(by_end), lambda e: e, frame["t_end_ns"])
    if end is None:
        return False, False, False
    lines = by_end[end]
    excess = lines[0]["frame_ns"] - budget_ns
    if case == "cpuspike":
        att = all(l[k] < 0.25 * excess for l in lines for k in DELAYS)
    elif case in DELAY:
        att = any(l[DELAY[case]] >= 0.5 * excess for l in lines)
    else:		# ipc: the server is another process, which delay accounting here does not see
        att = False
    return True, att, False if case in ("bgcpu", "iothread", "ipc") else None


def ftrace_snapshots(d, cache={}):
    """Per snapshot: each CPU's first event time and the last event time, the
    frame marks, and whether any CPU's ring overwrote events; in ns on the
    game's clock. The reader writes CLOCK_MONOTONIC into the trace ten times a
    second, which puts each snapshot on the game's clock whatever the trace's
    own clock is."""
    if d in cache:
        return cache[d]
    snaps, cur = [], None
    ev = re.compile(rb"\[(\d+)\]\s+(?:\S+\s+)?(\d+)\.(\d+):")
    entries = re.compile(rb"entries-in-buffer/entries-written: (\d+)/(\d+)")
    mono = re.compile(rb"_mono=(\d+)")
    with open(f"{d}/snapshots.txt", "rb") as f:
        for line in f:
            if line.startswith(b"### snapshot"):
                cur = {"first": {}, "last": 0, "wrapped": False, "marks": [], "cal": []}
                snaps.append(cur)
                continue
            if cur is not None and line.startswith(b"#"):
                m = entries.search(line)
                if m:
                    cur["wrapped"] = int(m.group(1)) < int(m.group(2))
                continue
            m = ev.search(line)
            if cur is None or not m:
                continue
            t = int(m.group(2)) * 1_000_000_000 + int(m.group(3).ljust(9, b"0")[:9])
            c = mono.search(line)
            if c:
                cur["cal"].append(int(c.group(1)) - t)
            if b" fmark:" in line:
                cur["marks"].append(t)
            cpu = int(m.group(1))
            cur["first"].setdefault(cpu, t)
            cur["first"][cpu] = min(cur["first"][cpu], t)
            cur["last"] = max(cur["last"], t)
    every = sorted(o for sn in snaps for o in sn["cal"])
    fallback = every[len(every) // 2] if every else 0
    for sn in snaps:
        cal = sorted(sn["cal"])
        off = cal[len(cal) // 2] if cal else fallback
        sn["offset"] = off
        sn["first"] = {k: v + off for k, v in sn["first"].items()}
        sn["last"] += off
        sn["marks"] = [m + off for m in sn["marks"]]
    cache[d] = snaps
    return snaps


def score_ftrace(d, case, frame, budget_ns):
    """A slow frame's snapshot is taken at its frame mark, which ends the frame
    (the game logs the frame's end there too), after which the ring starts
    empty. A snapshot covers the frame if it holds that mark and began before
    the frame did, and, if some CPU's ring overwrote events, every CPU's ring
    still did. (The last event alone says nothing: a frame limiter can wait
    for milliseconds without a scheduler event.)"""
    s, e = span(frame)
    rec = any(sn["first"] and min(sn["first"].values()) <= s
              and any(abs(m - e) <= MATCH_NS for m in sn["marks"])
              and (not sn["wrapped"] or max(sn["first"].values()) <= s)
              for sn in ftrace_snapshots(d))
    return rec, None, None


def perf_extent(d, cache={}):
    """First and last event time, and the times of lost-event records, from perf.data."""
    if d in cache:
        return cache[d]
    first = last = None
    lost, lost_untimed = [], 0
    p = subprocess.Popen(["perf", "script", "-i", f"{d}/perf.data", "--show-lost-events", "-F", "time"],
                         stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    ts = re.compile(r"(\d+)\.(\d+):")
    for line in p.stdout:
        m = ts.search(line)
        t = int(m.group(1)) * 1_000_000_000 + int(m.group(2).ljust(9, "0")[:9]) if m else None
        if "LOST" in line:
            if t is None:
                lost_untimed += 1
            else:
                lost.append(t)
            continue
        if t is not None:
            first = t if first is None else min(first, t)
            last = t if last is None else max(last, t)
    p.wait()
    cache[d] = (first, last, lost, lost_untimed)
    return cache[d]


def score_perfsched(d, case, frame, budget_ns):
    first, last, lost, untimed = perf_extent(d)
    s, e = span(frame)
    rec = (first is not None and first <= s and last >= e and not untimed
           and not any(s <= t <= e for t in lost))
    return rec, None, None


PERF_BPF = os.environ.get("PERF_BPF") or (f"{ROOT}/build/perf/perf" if os.path.exists(f"{ROOT}/build/perf/perf")
                                          else "perf")
# a game thread's off-CPU wait is the case's mechanism if its user stack has one of these
OFFCPU_SIG = {"iothread": ("load_asset", "pread")}


def perf_offcpu(d, cache={}):
    """perf record --off-cpu's direct samples: one per blocked interval of the
    game's threads at least the threshold long, emitted when the thread ran
    again, its period the interval's length, with the user stack it blocked
    in. Returns (comm, start, end, stack) per sample and the times of
    lost-event records. Shorter intervals are only summed per stack at the end
    of the run, with no time (OFF_CPU_TIMESTAMP), so they cannot be placed in
    a frame and are dropped here, as are the on-CPU samples."""
    if d in cache:
        return cache[d]
    samples, lost, cur = [], [], None
    head = re.compile(r"^\s*(?P<comm>\S.*?)\s+(?P<tid>\d+)\s+(?P<s>\d+)\.(?P<ns>\d+):\s+(?P<period>\d+)\s+(?P<ev>\S+):")
    p = subprocess.Popen([PERF_BPF, "script", "-i", f"{d}/perf.data", "--show-lost-events",
                          "-F", "comm,tid,time,period,event,ip,sym"],
                         stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, errors="replace")
    for line in p.stdout:
        if "LOST" in line:
            m = re.search(r"(\d+)\.(\d+):", line)
            if m:
                lost.append(int(m.group(1)) * 1_000_000_000 + int(m.group(2).ljust(9, "0")[:9]))
            cur = None
            continue
        m = head.match(line)
        if m:
            cur = None
            t = int(m.group("s")) * 1_000_000_000 + int(m.group("ns").ljust(9, "0")[:9])
            if m.group("ev").startswith("offcpu-time") and t < (1 << 62):
                cur = [m.group("comm"), t - int(m.group("period")), t, []]
                samples.append(cur)
            continue
        if cur is not None and line.strip():
            cur[3].append(line.split()[-1] if len(line.split()) > 1 else line.strip())
    p.wait()
    cache[d] = (samples, lost)
    return cache[d]


def score_perfoffcpu(d, case, frame, budget_ns):
    samples, lost = perf_offcpu(d)
    s, e = span(frame)
    excess = frame["frame_us"] * 1000 - budget_ns
    over = [(c, max(0, min(e, b) - max(s, a)), st) for c, a, b, st in samples if a < e and b > s]
    rec = bool(over) and not any(s <= t <= e for t in lost)
    if case in OFFCPU_SIG:
        att = sum(ns for c, ns, st in over if any(sig in f for f in st for sig in OFFCPU_SIG[case])) >= 0.5 * excess
    elif case == "cpuspike":
        att = all(ns < 0.25 * excess for c, ns, st in over)
    else:		# bgcpu: preempted time is never sampled; ipc, mem: a wait says nothing of its cause
        att = False
    return rec, rec and att, False if case in ("bgcpu", "iothread", "ipc") else None


PERFETTO_DIR = os.environ.get("PERFETTO_DIR") or f"{ROOT}/build/perfetto/v59.1/linux-amd64"
PERFETTO_QUERY = ("select 'cpu' as k, cpu as a, min(ts) as b from ftrace_event "
                  "where name in ('sched_switch', 'sched_waking') group by cpu "
                  "union all select 'mark', 0, ts from ftrace_event where name like '%fmark'")


def perfetto_snapshots(d, cache={}):
    """Per Perfetto snapshot (perfetto-trace.N): each CPU's first scheduler
    event and the frame marks it holds, from trace_processor_shell, on the
    trace's primary clock (CLOCK_MONOTONIC, the game's). Kept in
    perfetto-index.json, since loading every snapshot takes a while."""
    if d in cache:
        return cache[d]
    idx_path = f"{d}/perfetto-index.json"
    files = sorted((f for f in os.listdir(d) if re.fullmatch(r"perfetto-trace\.\d+", f)),
                   key=lambda f: int(f.rsplit(".", 1)[1]))
    try:
        idx = json.load(open(idx_path))
    except (OSError, ValueError):
        idx = {}
    for name in files:
        if name in idx:
            continue
        out = subprocess.run([f"{PERFETTO_DIR}/trace_processor_shell", "query", "--no-agent",
                              f"{d}/{name}", PERFETTO_QUERY],
                             capture_output=True, text=True).stdout
        first, marks = {}, []
        for line in out.splitlines()[1:]:
            k, a, b = (x.strip('"') for x in line.split(","))
            if k == "cpu" and b:
                first[a] = int(b)
            elif k == "mark":
                marks.append(int(b))
        idx[name] = {"first": first, "marks": marks}
    try:
        with open(idx_path, "w") as f:
            json.dump(idx, f)
    except OSError:
        pass
    cache[d] = [idx[n] for n in files]
    return cache[d]


def score_perfetto(d, case, frame, budget_ns):
    """As ftrace's flight recorder: a snapshot covers the frame if it holds the
    frame's own mark and every CPU's oldest event in it is older than the frame.
    Perfetto's buffer is one ring for all CPUs and always full after the first
    seconds, so the second condition always applies."""
    s, e = span(frame)
    rec = any(sn["first"] and max(sn["first"].values()) <= s
              and any(abs(m - e) <= MATCH_NS for m in sn["marks"])
              for sn in perfetto_snapshots(d))
    return rec, None, None


PER_FRAME = {"ht": score_ht, "delayacct": score_delayacct, "ftrace": score_ftrace, "perfsched": score_perfsched,
             "perfoffcpu": score_perfoffcpu, "perfetto": score_perfetto}


# ------------------------------------------------------------------ summary tools

def score_summary(d, case):
    lines = []
    with open(f"{d}/tool.log", errors="replace") as f:
        for line in f:
            m = re.match(r"^(.*;.*) (\d+)$", line.rstrip("\n"))
            if m:
                lines.append((int(m.group(2)), m.group(1)))
    lines.sort(reverse=True)
    total = sum(v for v, _ in lines)
    sig = SIGNATURE.get(case)
    if not sig or not lines:
        return {"lines": len(lines), "visible": None, "share": None, "rank": None}
    hits = [i for i, (_, l) in enumerate(lines) if any(s in l for s in sig)]
    share = sum(lines[i][0] for i in hits) / total if total else 0
    return {"lines": len(lines), "rank": hits[0] + 1 if hits else None,
            "visible": bool(hits) and hits[0] < TOP, "share": share}


# ------------------------------------------------------------------ report

def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    root = sys.argv[1].rstrip("/")
    plan = json.load(open(f"{root}/plan.json"))
    out, rows = [], []
    for case in plan["cases"].split():
        table = []
        for tool in plan["tools"].split():
            d = f"{root}/{case}-{tool}"
            if not os.path.exists(f"{d}/manifest.json"):
                table.append(f"| {tool} | not run (see {case}-{tool}.log) | | | | | |")
                continue
            inj, noise, budget_ns, window_s = ground_truth(d, case, tool)
            size = (os.path.getsize(f"{d}/hitch.jsonl") if tool == "ht"
                    else json.load(open(f"{d}/tool.json")).get("bytes", 0))
            row = dict(case=case, tool=tool, injected=len(inj), noise=len(noise), window_s=round(window_s, 1),
                       bytes=size)
            if tool in PER_FRAME:
                sc = [PER_FRAME[tool](d, case, f, budget_ns) for f in inj]
                nz = [PER_FRAME[tool](d, case, f, budget_ns) for f in noise]
                row.update(recorded=sum(bool(x[0]) for x in sc),
                           attributed=None if sc and sc[0][1] is None else sum(bool(x[1]) for x in sc),
                           culprit=None if not sc or sc[0][2] is None else sum(bool(x[2]) for x in sc),
                           noise_attributed=None if nz and nz[0][1] is None else sum(bool(x[1]) for x in nz),
                           noise_culprit=None if not nz or nz[0][2] is None else sum(bool(x[2]) for x in nz))
            else:
                row.update(score_summary(d, case))
            rows.append(row)
            n = row["injected"]
            mb = f"{size / 1e6:.1f} MB" + (f", {size / 1e6 / n:.2f} MB per injected hitch" if n else "")

            def cnt(k):
                v = row.get(k)
                return "n/a" if v is None else f"{v}/{n}"
            if tool in PER_FRAME:
                nn, fa, fc = row["noise"], row.get("noise_attributed"), row.get("noise_culprit")
                blame = ", ".join(f"{v} {k}" for k, v in (("attributed", fa), ("culprit", fc)) if v is not None)
                table.append(f"| {tool} | {n} | {cnt('recorded')} | {cnt('attributed')} | {cnt('culprit')} "
                             f"| {nn}{': ' + blame if blame else ''} | {mb} |")
            else:
                vis = ("n/a" if row["visible"] is None else
                       f"{'yes' if row['visible'] else 'no'} (rank {row['rank'] or '-'}, "
                       f"{100 * (row['share'] or 0):.1f}% of the summary)")
                table.append(f"| {tool} | {n} | summary only | {vis} | | {row['noise']} | {mb} |")
        out += [f"## {case}", "",
                "| Tool | Injected hitches | Recorded | Attributed | Culprit | Noise hitches | Output |",
                "|---|---|---|---|---|---|---|", *table, ""]
    head = [f"# Catch test: {os.path.basename(root)}", "",
            f"{plan['seconds']} s per run, one injection every {plan['rare_s']} s; commit {plan['commit']}. "
            "Rules: docs/catch-test.md. Counts are of injected hitches; for the summary tools, "
            f"'Attributed' says whether the cause is among the summary's {TOP} heaviest lines.", ""]
    text = "\n".join(head + out)
    with open(f"{root}/summary.md", "w") as f:
        f.write(text)
    with open(f"{root}/scores.json", "w") as f:
        json.dump(rows, f, indent=1)
    print(text)


if __name__ == "__main__":
    main()
