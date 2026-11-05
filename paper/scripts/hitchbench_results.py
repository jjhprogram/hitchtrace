#!/usr/bin/env python3
"""Summarize the hitchbench runs in tests/out/ for the paper.

Joins each injector class's ground truth (<class>.gt.jsonl, written by
tests/hitchbench.c) with the hitchtrace records for the same run
(<class>.hitch.jsonl) on frame_id, and writes:

  data/hitchbench_table.tex      rows of the attribution table
  data/hitchbench_breakdown.dat  mean root-timeline composition per class (ms)
  data/hitchbench_macros.tex     \\newcommand numbers quoted in the text

Only frames that lie wholly inside the trace are scored. hitchtrace opens its
first frame at the first mark it sees after attaching, and the run ends
mid-frame, so the first and last injected frame of every run straddle the
trace boundary. The window is taken from the records' own frame ids: from the
first mark hitchtrace saw to the last, exclusive of both ends.
"""
import json
import os
import statistics as st
import sys
from collections import Counter

HERE = os.path.dirname(os.path.abspath(__file__))
PAPER = os.path.dirname(HERE)
OUT = os.path.join(PAPER, "..", "tests", "out")
DATA = os.path.join(PAPER, "data")

PART = ["oncpu", "oncpu_fault", "oncpu_fault_major", "oncpu_reclaim",
        "oncpu_compact", "runnable", "runqueue", "block_futex", "block_poll",
        "block_io", "block_timer", "block_gpu", "block_present", "block_task",
        "block_other"]

# class, what the injector does, bucket expected (as tests/run_hitch_tests.sh)
CLASSES = [
    ("sleep", r"root \texttt{nanosleep}s 25\,ms", "block_timer"),
    ("poll", r"root \texttt{epoll\_wait}s on feeder", "block_poll"),
    ("io", r"8\,MB cold read", "block_io"),
    ("fault", r"burst of minor faults", "oncpu_fault"),
    ("preempt", r"root shares a CPU with hogs", "runnable"),
    ("worker_block", r"root waits on blocked worker", "block_futex"),
    ("worker_cpu", r"root waits on busy worker", "block_futex"),
    ("cpu_spike", r"root burns 25\,ms of CPU", "oncpu"),
    ("thousand_cuts", r"$\sim$40 sleeps of 0.5\,ms", "block_timer"),
    ("present_long", r"present waits 20\,ms longer", "block_present"),
    ("present_off", r"\texttt{sleep}, present bracket off", "block_timer"),
]
BASELINE = "quiet_baseline"

TEX_NAME = {
    "oncpu": "oncpu", "oncpu_fault": "fault", "block_timer": "timer",
    "block_poll": "poll", "block_io": "io", "runnable": "runnable",
    "block_futex": "futex", "block_present": "present",
}

# stacked-bar groups, in stacking order
GROUPS = [
    ("oncpu", ["oncpu"]),
    ("oncpukern", ["oncpu_fault", "oncpu_fault_major", "oncpu_reclaim",
                   "oncpu_compact"]),
    ("sched", ["runnable", "runqueue"]),
    ("futex", ["block_futex"]),
    ("poll", ["block_poll"]),
    ("io", ["block_io"]),
    ("timer", ["block_timer"]),
    ("present", ["block_present"]),
    ("other", ["block_gpu", "block_task", "block_other"]),
]


def load(cls):
    gt = {}
    with open(os.path.join(OUT, f"{cls}.gt.jsonl")) as f:
        for line in f:
            g = json.loads(line)
            gt[g["frame_id"]] = g
    recs, lo, hi = {}, None, None
    with open(os.path.join(OUT, f"{cls}.hitch.jsonl")) as f:
        for line in f:
            r = json.loads(line)
            if "frame_id" in r:
                recs[r["frame_id"]] = r
            elif r.get("type") == "header":
                lo = r["trace_start_ns"]
            elif r.get("type") == "summary":
                hi = r["trace_end_ns"]
    return gt, recs, (lo, hi)


def window(gt, bounds):
    """Frame ids wholly inside the trace.

    trace_start_ns is taken before the BPF object is loaded and attached, so
    the first ~300 ms of a run are not traced; the tool then opens its first
    frame at the next mark. trace_end_ns is taken after the ring buffer is
    drained, up to ~150 ms after the last mark it acted on. Frames that start
    within ATTACH_NS of the start, or end within DETACH_NS of the end, are not
    scored: in every run these are the frame hitchtrace was attaching during
    and the frame it was detaching during.
    """
    lo, hi = bounds
    return sorted(i for i, g in gt.items()
                  if g["t_end_ns"] - g["frame_us"] * 1000 >= lo + ATTACH_NS
                  and g["t_end_ns"] <= hi - DETACH_NS)


ATTACH_NS = 400_000_000
DETACH_NS = 200_000_000


def kern_share(r, resolved=False):
    """stall_ns / excess, as the in-kernel gate weighs it.

    stall_ns is every root-timeline bucket except on-CPU and present time.
    With resolved=True, the part of blocked time spent waiting on a task
    that was itself running (resolved_wait_oncpu) is also taken out -- a
    counterfactual gate the prototype does not implement.
    """
    c = r["cause_ns"]
    stall = sum(c[k] for k in PART if k not in ("oncpu", "block_present"))
    if resolved:
        stall -= c["resolved_wait_oncpu"]
    excess = r["frame_ns"] - r["budget_ns"]
    return stall / excess if excess > 0 else float("nan")


def main():
    rows, bars, macro = [], [], {}
    tot_frames = tot_over = tot_rec = tot_extra = tot_miss = 0
    tot_inj = tot_inj_rec = tot_match = 0
    max_dt_us = 0.0

    for cls, what, expect in CLASSES + [(BASELINE, None, None)]:
        gt, recs, bounds = load(cls)
        ids = window(gt, bounds)
        frames = [gt[i] for i in ids]
        over = [g for g in frames if g["frame_us"] > summary_budget_us(recs)]
        rec = [g for g in frames if g["frame_id"] in recs]
        tot_frames += len(frames)
        tot_over += len(over)
        tot_rec += len(rec)
        tot_extra += sum(1 for g in rec if g["frame_us"] <= summary_budget_us(recs))
        tot_miss += sum(1 for g in over if g["frame_id"] not in recs)
        for g in rec:
            dt = abs(recs[g["frame_id"]]["frame_ns"] / 1e3 - g["frame_us"])
            max_dt_us = max(max_dt_us, dt)
        if cls == BASELINE:
            macro["baselineFrames"] = len(frames)
            macro["baselineRecords"] = len(rec)
            continue

        inj = [g for g in frames if g["injected"]]
        caught = [g for g in inj if g["frame_id"] in recs]
        tops = Counter(max(PART, key=lambda k: recs[g["frame_id"]]["cause_ns"][k])
                       for g in caught)
        match = tops.get(expect, 0)
        tot_inj += len(inj)
        tot_inj_rec += len(caught)
        tot_match += match
        worst_frac = st.median(
            recs[g["frame_id"]]["worst"]["ns"] /
            (recs[g["frame_id"]]["frame_ns"] - recs[g["frame_id"]]["budget_ns"])
            for g in caught)
        nstalls = st.median(recs[g["frame_id"]]["nstalls"] for g in caught)
        share = st.median(kern_share(recs[g["frame_id"]]) for g in caught)
        gated = sum(1 for g in caught if kern_share(recs[g["frame_id"]]) >= 0.5)
        rshare = st.median(kern_share(recs[g["frame_id"]], True) for g in caught)
        rgated = sum(1 for g in caught
                     if kern_share(recs[g["frame_id"]], True) >= 0.5)
        inh = st.median(recs[g["frame_id"]]["cause_ns"]["resolved_inherited"] / 1e6
                        for g in caught)
        won = st.median(recs[g["frame_id"]]["cause_ns"]["resolved_wait_oncpu"] / 1e6
                        for g in caught)
        chains = Counter(
            " $\\leftarrow$ ".join(
                (h["comm"].replace("_", "\\_") if h["comm"] else "[irq]")
                for h in recs[g["frame_id"]]["worst"]["hops"])
            for g in caught)
        chain, chain_n = chains.most_common(1)[0]
        other = ", ".join(f"{TEX_NAME.get(k, k)} {n}" for k, n in tops.items()
                          if k != expect)
        top_cell = f"{match}/{len(caught)}"
        if other:
            top_cell += f" ({other})"
        rows.append(
            f"\\texttt{{{cls.replace('_', '\\_')}}} & {what} & "
            f"\\texttt{{{TEX_NAME.get(expect, expect)}}} & "
            f"{len(caught)}/{len(inj)} & {top_cell} & "
            f"{worst_frac:.2f} & {share:.2f} & {gated}/{len(caught)} & "
            f"{rshare:.2f} & {rgated}/{len(caught)} \\\\")

        means = {}
        for key, members in GROUPS:
            means[key] = st.mean(
                sum(recs[g["frame_id"]]["cause_ns"][m] for m in members) / 1e6
                for g in caught)
        bars.append((cls, means))

        k = cls.replace("_", "")
        macro[f"{k}Inherited"] = f"{inh:.1f}"
        macro[f"{k}WaitOncpu"] = f"{won:.1f}"
        macro[f"{k}Nstalls"] = f"{nstalls:.0f}"
        macro[f"{k}WorstFrac"] = f"{worst_frac:.2f}"
        macro[f"{k}Share"] = f"{share:.2f}"
        macro[f"{k}RShare"] = f"{rshare:.2f}"
        macro[f"{k}Gated"] = f"{gated}/{len(caught)}"
        macro[f"{k}RGated"] = f"{rgated}/{len(caught)}"
        macro[f"{k}Chain"] = chain
        macro[f"{k}ChainN"] = f"{chain_n}/{len(caught)}"
        macro[f"{k}Match"] = f"{match}/{len(caught)}"
        if cls in ("fault", "io", "cpu_spike", "preempt"):
            for key in ("oncpu", "oncpu_fault", "block_io", "block_present",
                        "runnable"):
                v = st.median(recs[g["frame_id"]]["cause_ns"][key] / 1e6
                              for g in caught)
                macro[f"{k}{key.replace('_', '')}"] = f"{v:.1f}"

    macro.update(totFrames=tot_frames, totOver=tot_over, totRec=tot_rec,
                 totExtra=tot_extra, totMiss=tot_miss, totInj=tot_inj,
                 totInjRec=tot_inj_rec, totMatch=tot_match,
                 maxDtUs=f"{max_dt_us:.0f}", nClasses=len(CLASSES),
                 nRuns=len(CLASSES) + 1)

    os.makedirs(DATA, exist_ok=True)
    with open(os.path.join(DATA, "hitchbench_table.tex"), "w") as f:
        # a macro, not bare rows: \input inside a tabular breaks \bottomrule
        f.write("% generated by scripts/hitchbench_results.py -- do not edit\n")
        f.write("\\newcommand{\\hbTableRows}{%\n" + "\n".join(rows) + "\n}\n")
    with open(os.path.join(DATA, "hitchbench_breakdown.dat"), "w") as f:
        f.write("class " + " ".join(k for k, _ in GROUPS) + "\n")
        for cls, means in bars:
            f.write(cls.replace("_", "\\_") + " " +
                    " ".join(f"{means[k]:.2f}" for k, _ in GROUPS) + "\n")
    with open(os.path.join(DATA, "hitchbench_macros.tex"), "w") as f:
        f.write("% generated by scripts/hitchbench_results.py -- do not edit\n")
        for k, v in macro.items():
            f.write(f"\\newcommand{{\\hb{k}}}{{{v}}}\n")
    for k, v in macro.items():
        print(f"{k:28} {v}", file=sys.stderr)


def summary_budget_us(recs):
    return next(iter(recs.values()))["budget_ns"] / 1e3 if recs else 16667


if __name__ == "__main__":
    main()
