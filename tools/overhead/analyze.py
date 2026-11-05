#!/usr/bin/env python3
# SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
"""Summarize a tools/overhead/run.py directory.

Each run with a tool is compared with the no-tool runs on either side of it in
the same workload, so slow drift of the machine cancels out. The table gives
the median of those ratios over the rounds, with a bootstrap 95% interval.
Ratios above 1 for a time, or below 1 for a rate, mean the tool slowed things.

Writes summary.md and summary.csv next to runs.jsonl.

usage: analyze.py DIR
"""
import csv
import json
import os
import random
import statistics as st
import sys

GAME_METRICS = [("jobs_p50_us", "job phase p50"), ("frame_p50_us", "frame p50"), ("frame_p99_us", "frame p99"),
                ("fps", "frames/s"), ("low1_fps", "1% low fps"), ("calls_p50_us", "server calls p50")]
PIPE_METRICS = [("usecs_per_op", "us/round trip")]


def boot_ci(xs, n=2000):
    rng = random.Random(1)
    meds = sorted(st.median(rng.choices(xs, k=len(xs))) for _ in range(n))
    return meds[int(n * 0.025)], meds[int(n * 0.975)]


def neighbours(runs, i):
    """The nearest no-tool run on each side of runs[i], in the same workload and round."""
    r = runs[i]
    same = [x for x in runs if x["workload"] == r["workload"] and x["round"] == r["round"]
            and x["phase"] == r["phase"] and x["arm"] == "none" and "error" not in x]
    before = [x for x in same if x["seq"] < r["seq"]]
    after = [x for x in same if x["seq"] > r["seq"]]
    return ([before[-1]] if before else []) + ([after[0]] if after else [])


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    d = sys.argv[1].rstrip("/")
    with open(f"{d}/runs.jsonl") as f:
        runs = [json.loads(l) for l in f]
    env = json.load(open(f"{d}/env.json"))
    main_runs = [r for r in runs if r["phase"] == "main"]
    lines = [f"# Overhead: {os.path.basename(d)}", "",
             f"{env['cpu']}, {env['cpus']} CPUs, kernel {env['kernel']}, "
             f"{'virtual machine' if env['hypervisor'] else 'bare metal'}; commit {env['commit']}.",
             f"{env['rounds']} rounds of {env['seconds']} s; tool's own process {env['placement']}. "
             f"Ratios are against the neighbouring no-tool runs: median [95% interval] (n).", ""]
    if env["hypervisor"]:
        lines += ["**Pilot only: a virtual machine.** Slow context switches inflate the baseline, so "
                  "percentages understate overhead relative to bare metal; prefer absolute costs.", ""]
    rows = []
    workloads = sorted({r["workload"] for r in main_runs})
    for w in workloads:
        metrics = PIPE_METRICS if w.startswith("pipe") else GAME_METRICS
        if w != "ipc":
            metrics = [m for m in metrics if m[0] != "calls_p50_us"]
        arms = sorted({r["arm"] for r in main_runs if r["workload"] == w and r["arm"] != "none"})
        lines += [f"## {w}", "", "| Arm | " + " | ".join(m[1] for m in metrics)
                  + " | tool CPU | machine busy +s/s | MB/hour | drops | caught / over budget |",
                  "|---|" + "---|" * (len(metrics) + 5)]
        base = [r for r in main_runs if r["workload"] == w and r["arm"] == "none" and "error" not in r]
        if base:
            cells = []
            for k, _ in metrics:
                vals = [r[k] for r in base if k in r]
                cells.append(f"{st.median(vals):.1f} abs" if vals else "")
            over = sum(r.get("over_budget", 0) for r in base)
            lines.append(f"| no tool (n={len(base)}) | " + " | ".join(cells) + f" | | | | | {over} over |")
        for a in arms:
            rs = [r for r in main_runs if r["workload"] == w and r["arm"] == a]
            ok = [r for r in rs if "error" not in r]
            if not ok:
                lines.append(f"| {a} | failed: {rs[0]['error'][:120]} |" + " |" * (len(metrics) + 4))
                continue
            cells = []
            for k, _ in metrics:
                ratios = []
                for r in ok:
                    nb = [x[k] for x in neighbours(main_runs, main_runs.index(r)) if k in x]
                    if k in r and nb and st.mean(nb):
                        ratios.append(r[k] / st.mean(nb))
                if ratios:
                    lo, hi = boot_ci(ratios)
                    med = st.median(ratios)
                    cells.append(f"{med:.3f} [{lo:.3f}, {hi:.3f}] ({len(ratios)})")
                    rows.append(dict(workload=w, arm=a, metric=k, ratio=round(med, 4), lo=round(lo, 4),
                                     hi=round(hi, 4), n=len(ratios)))
                else:
                    cells.append("")
            cpu = st.median(r["tool_cpu_s"] / r["window_s"] for r in ok)
            busy = []
            for r in ok:
                nb = [x["busy_s"] / x["window_s"] for x in neighbours(main_runs, main_runs.index(r))]
                if nb:
                    busy.append(r["busy_s"] / r["window_s"] - st.mean(nb))
            mbh = st.median(r["bytes"] / r["window_s"] * 3600 / 1e6 for r in ok)
            out_cell = (f"summary at exit, {st.median(r['bytes'] for r in ok) / 1e3:.0f} KB" if ok[0].get("summary")
                        else f"{mbh:.1f}")
            drops = sum(r.get("drops", 0) for r in ok)
            caught = sum(r.get("caught", 0) for r in ok)
            over = sum(r.get("over_budget", 0) for r in ok)
            if w.startswith("pipe"):
                cap = ""		# no frames
            elif ok[0].get("captures_all"):
                cap = "all"
            else:
                cap = f"{caught} / {over}" if any("caught" in r for r in ok) else "no per-frame output"
            lines.append(f"| {a} | " + " | ".join(cells) + f" | {100 * cpu:.1f}% | {st.median(busy) if busy else 0:+.3f} "
                         f"| {out_cell} | {drops} | {cap} |")
            rows.append(dict(workload=w, arm=a, metric="tool_cpu_frac", ratio=round(cpu, 4), n=len(ok)))
            if not ok[0].get("summary"):
                rows.append(dict(workload=w, arm=a, metric="mb_per_hour", ratio=round(mbh, 2), n=len(ok)))
        lines.append("")

    # absolute cost per round trip: what carries over to another machine
    pipes = [w for w in workloads if w.startswith("pipe")]
    if pipes:
        lines += ["## Added time per context switch", "",
                  "A round trip is two wakeups and two context switches, so this is half the added time per "
                  "round trip against the neighbouring no-tool runs.", "", "| Arm | " + " | ".join(pipes) + " |",
                  "|---|" + "---|" * len(pipes)]
        for a in sorted({r["arm"] for r in main_runs if r["arm"] != "none"}):
            cells = []
            for w in pipes:
                d_ns = []
                for r in main_runs:
                    if r["arm"] == a and r["workload"] == w and "usecs_per_op" in r:
                        nb = [x["usecs_per_op"] for x in neighbours(main_runs, main_runs.index(r)) if "usecs_per_op" in x]
                        if nb:
                            d_ns.append((r["usecs_per_op"] - st.mean(nb)) * 1000 / 2)
                cells.append(f"{st.median(d_ns):.0f} ns (n={len(d_ns)})" if d_ns else "")
            lines.append(f"| {a} | " + " | ".join(cells) + " |")
        lines.append("")

    # what the BPF programs themselves took (kernel.bpf_stats_enabled, separate runs)
    cost = [r for r in runs if r["phase"] == "cost" and r.get("bpf_progs")]
    if cost:
        lines += ["## BPF programs: time per run", "",
                  "From separate runs with kernel.bpf_stats_enabled, which itself adds a little per run.", "",
                  "| Workload | Arm | Program | Runs/s | ns/run | CPU share of one core |", "|---|---|---|---|---|---|"]
        agg = {}
        for r in cost:
            for p in r["bpf_progs"]:
                if p["run_cnt"]:
                    agg.setdefault((r["workload"], r["arm"], p["name"]), []).append(
                        (p["run_cnt"] / r["window_s"], p["run_time_ns"] / p["run_cnt"],
                         p["run_time_ns"] / 1e9 / r["window_s"]))
        for (w, a, n), v in sorted(agg.items()):
            lines.append(f"| {w} | {a} | {n} | {st.median(x[0] for x in v):.0f} | {st.median(x[1] for x in v):.0f} "
                         f"| {100 * st.median(x[2] for x in v):.2f}% |")
        lines.append("")

    failed = [r for r in runs if "error" in r]
    if failed or env.get("unavailable"):
        lines += ["## Failures", ""]
        for a, why in env.get("unavailable", {}).items():
            lines.append(f"- **{a}**: not run, {why}")
        seen = set()
        for r in failed:
            if r["arm"] not in seen:
                seen.add(r["arm"])
                lines.append(f"- **{r['arm']}** ({r['workload']}): {r['error'][:400]}")
        lines.append("")

    text = "\n".join(lines)
    with open(f"{d}/summary.md", "w") as f:
        f.write(text)
    with open(f"{d}/summary.csv", "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=["workload", "arm", "metric", "ratio", "lo", "hi", "n"])
        w.writeheader()
        w.writerows(rows)
    print(text)


if __name__ == "__main__":
    main()
