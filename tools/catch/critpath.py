#!/usr/bin/env python3
# SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
"""Perfetto's critical-path analysis, run with no human in the loop on every
snapshot of the Perfetto flight recorder (tools/catch, tool `perfetto`), and
scored like hitchtrace. Rules: tools/catch/README.md, "Critical path on the
Perfetto snapshots".

Each snapshot (perfetto-trace.N, in trigger order) is loaded once into
trace_processor_shell, which finds the slow frames whose end mark is newer
than the previous snapshot's last mark (the frame that fired the trigger,
and any other slow frame not yet seen) and, for each, runs the standard
library's critical path (`sched.thread_executing_span`,
`_thread_executing_span_critical_path`) of the thread that hit the mark,
over the frame: previous mark to this one. Each path span is split by the
path thread's thread_state, as the library's critical-path stack shows it
(`sched.thread_executing_span_with_slice`, "blocking thread_state"). Two
fields beyond the library are kept for the "+ join" score: whether the wake
that ended a sleep came from interrupt context, and, for a run-queue wait of
at least 100 us, the task that held the CPU when it ended.

Per snapshot, kept in DIR/CASE-perfetto/critpath.jsonl (so a rerun only
scores): the frames, their path pieces, and the analysis's wall time and peak
memory. Writes DIR/critpath.md and DIR/critpath.json.

usage: critpath.py DIR [CASE ...]
"""
import csv
import io
import json
import os
import re
import statistics as st
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import score as S	# noqa: E402  the catch test's ground truth and matching

TP = f"{S.PERFETTO_DIR}/trace_processor_shell"
MIN_NS = 100_000	# hitchtrace's --min-stall, and its minimum queue wait for naming a holder
RUNNABLE = ("R", "R+")
CASES = ("bgcpu", "iothread", "ipc", "mem", "cpuspike")

SQL = """
INCLUDE PERFETTO MODULE sched.thread_executing_span;
CREATE PERFETTO TABLE _hf AS
SELECT utid, lag(ts) OVER (PARTITION BY utid ORDER BY ts) AS s, ts AS e
FROM ftrace_event WHERE name GLOB '*fmark';
CREATE PERFETTO TABLE _slow AS
SELECT utid, s, e FROM _hf WHERE s IS NOT NULL AND e - s > {budget} AND e > {after};
CREATE PERFETTO TABLE _cp AS
SELECT f.e AS frame_e, f.s AS frame_s, f.utid AS root, cp.utid, cp.ts, cp.dur
FROM _slow f JOIN _thread_executing_span_critical_path(f.utid, f.s, f.e - f.s) cp;
CREATE PERFETTO TABLE _piece AS
SELECT c.frame_e, c.frame_s, c.root, c.utid, ts.state, ts.ts AS st_ts, ts.dur AS st_dur,
  min(c.ts + c.dur, ts.ts + ts.dur) - max(c.ts, ts.ts) AS ns
FROM _cp c JOIN thread_state ts ON ts.utid = c.utid AND ts.ts < c.ts + c.dur AND ts.ts + ts.dur > c.ts;
CREATE PERFETTO TABLE _end AS
SELECT p.*, n.irq_context AS next_irq, n.cpu AS next_cpu
FROM _piece p LEFT JOIN thread_state n ON n.utid = p.utid AND n.ts = p.st_ts + p.st_dur;
SELECT f.e AS frame_e, f.s AS frame_s, f.utid AS root, rt.name AS root_name,
  e.utid, t.tid, t.name AS thread, pr.name AS process, e.state, e.ns, e.st_dur, e.next_irq,
  CASE WHEN e.state IN ('R', 'R+') AND e.st_dur >= {min_ns} THEN
    (SELECT h.name FROM sched s JOIN thread h USING (utid)
     WHERE s.cpu = e.next_cpu AND s.ts < e.st_ts + e.st_dur
       AND s.ts >= e.st_ts + e.st_dur - 1000000000 AND s.ts + s.dur = e.st_ts + e.st_dur) END AS holder
FROM _slow f JOIN thread rt ON rt.utid = f.utid
LEFT JOIN _end e ON e.frame_e = f.e AND e.root = f.utid
LEFT JOIN thread t ON t.utid = e.utid LEFT JOIN process pr ON pr.upid = t.upid
ORDER BY f.e, e.st_ts;
"""


def nul(v):
    return None if v in ("[NULL]", "") else v


def analyse(path, after, budget_ns):
    """One snapshot through trace_processor_shell: its new slow frames with
    their path pieces, the wall time and the peak RSS of the run."""
    sql = SQL.format(budget=budget_ns, after=after, min_ns=MIN_NS)
    with tempfile.TemporaryFile("w+") as fo, tempfile.TemporaryFile("w+") as fe:
        t0 = time.monotonic()
        p = subprocess.Popen([TP, "query", "--quiet", "--no-agent", "--format", "csv", path, sql],
                             stdout=fo, stderr=fe, text=True)
        _, status, ru = os.wait4(p.pid, 0)	# reaped here, for this child's own peak RSS
        wall = time.monotonic() - t0
        fo.seek(0)
        fe.seek(0)
        out, err = fo.read(), fe.read()
    if os.waitstatus_to_exitcode(status):
        raise RuntimeError(f"{path}: {err.strip()[:800]}")
    frames = {}
    for r in csv.DictReader(io.StringIO(out)):
        e = int(r["frame_e"])
        f = frames.setdefault(e, {"s": int(r["frame_s"]), "e": e, "root": int(r["root"]),
                                  "root_name": r["root_name"], "pieces": []})
        if nul(r["utid"]) is None:
            continue
        irq = nul(r["next_irq"])
        f["pieces"].append([int(r["utid"]), int(r["tid"]), nul(r["thread"]) or "", nul(r["process"]) or "",
                            r["state"], int(r["ns"]), int(r["st_dur"]),
                            None if irq is None else int(irq), nul(r["holder"])])
    return list(frames.values()), wall, ru.ru_maxrss


def tp_version():
    out = subprocess.run([TP, "--version"], capture_output=True, text=True).stdout.split()
    return out[1] if len(out) > 1 else "unknown version"


def snapshot_files(d):
    return sorted((f for f in os.listdir(d) if re.fullmatch(r"perfetto-trace\.\d+", f)),
                  key=lambda f: int(f.rsplit(".", 1)[1]))


def run_case(d, budget_ns):
    """Every snapshot of one case, in trigger order, cached in critpath.jsonl."""
    cache_path = f"{d}/critpath.jsonl"
    done = {}
    if os.path.exists(cache_path):
        for rec in S.load(cache_path):
            done[rec["snapshot"]] = rec
    files = snapshot_files(d)
    index = S.perfetto_snapshots(d)		# each snapshot's marks and per-CPU first events
    after = 0
    with open(cache_path, "a") as out:
        for i, name in enumerate(files):
            if name not in done:
                frames, wall, rss = analyse(f"{d}/{name}", after, budget_ns)
                rec = {"snapshot": name, "bytes": os.path.getsize(f"{d}/{name}"), "after": after,
                       "wall_s": round(wall, 3), "maxrss_kb": rss, "frames": frames}
                out.write(json.dumps(rec) + "\n")
                out.flush()
                done[name] = rec
                print(f"  {os.path.basename(d)} {name}: {len(frames)} frame(s), "
                      f"{rec['bytes'] / 1e6:.1f} MB, {wall:.1f} s", file=sys.stderr, flush=True)
            marks = index[i]["marks"]
            after = max([after] + marks)
    return [done[n] for n in files], index


# ------------------------------------------------------------------ scoring

def path_facts(f, budget_ns):
    """What the rules read from one frame's critical path."""
    P = [dict(zip(("utid", "tid", "thread", "process", "state", "ns", "st_dur", "irq", "holder"), p))
         for p in f["pieces"]]
    excess = (f["e"] - f["s"]) - budget_ns
    return P, excess


def judge(case, f, budget_ns):
    """(attributed, culprit from the library's output, culprit with the join),
    by the rules in tools/catch/README.md. None where a level is not scored."""
    P, excess = path_facts(f, budget_ns)
    on_path = {p["thread"] for p in P}
    lib_bgload = any(n.startswith("bgload") for n in on_path)
    if case == "bgcpu":
        att = sum(p["ns"] for p in P if p["state"] in RUNNABLE) >= 0.5 * excess
        join = any((p["holder"] or "").startswith("bgload") for p in P if p["state"] in RUNNABLE)
        return att, att and lib_bgload, att and join
    if case == "iothread":
        grp = [p for p in P if p["thread"] == "mg_stream" and p["state"].startswith("D")]
        att = sum(p["ns"] for p in grp) >= MIN_NS
        return att, False, att and any(p["irq"] == 1 for p in grp)
    if case == "ipc":
        grp = [p for p in P if p["thread"] == "mg_server" and p["state"] in RUNNABLE and p["ns"] >= MIN_NS]
        att = bool(grp)
        return att, att and lib_bgload, att and any((p["holder"] or "").startswith("bgload") for p in grp)
    if case == "mem":
        return False, None, None
    # cpuspike: the root running, and no runnable or uninterruptible time to speak of on the path
    run = sum(p["ns"] for p in P if p["utid"] == f["root"] and p["state"] == "Running")
    stall = sum(p["ns"] for p in P if p["state"] in RUNNABLE or p["state"].startswith("D"))
    return run >= 0.5 * excess and stall < 0.25 * excess, None, None


def root_uninterruptible(f, budget_ns):
    """mem, not scored: the root's own uninterruptible sleep on its path is at
    least half the overrun (a swap-in looks like this, and so does a file read)."""
    P, excess = path_facts(f, budget_ns)
    return sum(p["ns"] for p in P if p["utid"] == f["root"] and p["state"].startswith("D")) >= 0.5 * excess


def score_case(root, case):
    d = f"{root}/{case}-perfetto"
    man = json.load(open(f"{d}/manifest.json"))
    budget_ns = man["budget_us"] * 1000
    recs, index = run_case(d, budget_ns)
    reports = []		# (frame, snapshot index)
    for i, rec in enumerate(recs):
        reports += [(f, i) for f in rec["frames"]]
    inj, noise, _, _ = S.ground_truth(d, case, "perfetto")

    def one(frame):
        s, e = S.span(frame)
        hit = S.nearest(reports, lambda x: x[0]["e"], e)
        if hit is None:
            return None
        f, i = hit
        first = index[i]["first"]
        if not first or max(first.values()) > s:
            return None
        return f

    def tally(frames):
        culprit = 0 if case in ("bgcpu", "iothread", "ipc") else None
        n = {"n": len(frames), "recorded": 0, "attributed": 0, "culprit_lib": culprit,
             "culprit_join": culprit, "root_d": 0}
        for fr in frames:
            f = one(fr)
            if f is None:
                continue
            att, lib, join = judge(case, f, budget_ns)
            n["recorded"] += 1
            n["attributed"] += bool(att)
            if culprit is not None:
                n["culprit_lib"] += bool(lib)
                n["culprit_join"] += bool(join)
            n["root_d"] += root_uninterruptible(f, budget_ns)
        return n

    walls = [r["wall_s"] for r in recs]
    sizes = [r["bytes"] for r in recs]
    big = max(recs, key=lambda r: r["bytes"]) if recs else None
    return {"case": case, "injected": tally(inj), "noise": tally(noise),
            "snapshots": len(recs), "frames_analysed": len(reports),
            "wall_s": {"median": round(st.median(walls), 2) if walls else None,
                       "max": round(max(walls), 2) if walls else None, "total": round(sum(walls), 1),
                       "largest_snapshot": big and big["wall_s"]},
            "mb": {"median": round(st.median(sizes) / 1e6, 1) if sizes else None,
                   "max": round(max(sizes) / 1e6, 1) if sizes else None},
            "maxrss_mb": round(max((r["maxrss_kb"] or 0) for r in recs) / 1024) if recs else None}


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    root = sys.argv[1].rstrip("/")
    cases = sys.argv[2:] or [c for c in json.load(open(f"{root}/plan.json"))["cases"].split() if c in CASES]
    rows = [score_case(root, c) for c in cases]

    def cnt(t, k):
        v = t.get(k)
        return "n/a" if v is None else f"{v}/{t['n']}"
    out = [f"# Critical path on the Perfetto snapshots: {os.path.basename(root)}", "",
           "Perfetto's `_thread_executing_span_critical_path` (module `sched.thread_executing_span`, "
           f"{tp_version()}) of the frame-marking thread, per slow frame, on each "
           "snapshot as it would arrive. Rules: tools/catch/README.md, \"Critical path on the Perfetto "
           "snapshots\". Culprit (library): from the path's threads and states alone; culprit (+ join): "
           "also the interrupt flag of the wake that ended a sleep and the CPU's holder at the end of a "
           "run-queue wait.", "",
           "| Case | Injected | Recorded | Attributed | Culprit (library) | Culprit (+ join) "
           "| Noise: attributed, culprit (library / + join) | Snapshots | Analysis per snapshot "
           "(median / max) | Snapshot MB (median / max) | Peak RSS |",
           "|---|---|---|---|---|---|---|---|---|---|---|"]
    for r in rows:
        i, z = r["injected"], r["noise"]
        nz = f"{z['n']}: {z['attributed']}" + ("" if z["culprit_lib"] is None
                                               else f", {z['culprit_lib']} / {z['culprit_join']}")
        out.append(f"| `{r['case']}` | {i['n']} | {cnt(i, 'recorded')} | {cnt(i, 'attributed')} "
                   f"| {cnt(i, 'culprit_lib')} | {cnt(i, 'culprit_join')} | {nz} | {r['snapshots']} "
                   f"| {r['wall_s']['median']} / {r['wall_s']['max']} s | {r['mb']['median']} / {r['mb']['max']} "
                   f"| {r['maxrss_mb']} MB |")
    mem = [r for r in rows if r["case"] == "mem"]
    if mem:
        m = mem[0]["injected"]
        out += ["", f"`mem`, not scored: the root's own uninterruptible sleep on its path is at least half "
                f"the overrun in {m['root_d']} of {m['n']} injected hitches "
                f"({mem[0]['noise']['root_d']} of {mem[0]['noise']['n']} noise hitches)."]
    text = "\n".join(out) + "\n"
    with open(f"{root}/critpath.md", "w") as f:
        f.write(text)
    with open(f"{root}/critpath.json", "w") as f:
        json.dump(rows, f, indent=1)
    print(text)


if __name__ == "__main__":
    main()
