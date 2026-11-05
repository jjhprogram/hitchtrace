#!/usr/bin/env python3
# SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
"""Score a `cases/run.sh mem|memthp|slack|ipc|cpuspike|iodisk|squeeze|alloc|compact` run.

Does a hitchtrace record alone explain the hitch? Each over-budget record is
set against ground truth that does not come from hitchtrace, and sorted into
*affected* frames (the case's cause was at work) and *noise* frames (it was
not; on the development VM some frames run long with nothing going on). A
good tool answers the case's questions yes on the first and no on the second.

  mem, memthp   affected: the frame walked the world (the game's log), under
                a memory limit. Does the record put the time on memory: major
                faults, and I/O waits whose kernel stack is a page fault?
  slack         affected: the limiter slept with its timer slack raised (the
                preload's log). Does the record show the sleep, and can it
                tell a late wakeup from an intended one?
  ipc           affected: bgload was bursting on the server's CPU (its log).
                Does the record show the waits on the server, that the server
                was kept off the CPU, and by whom?
  cpuspike      affected: the frame burned CPU on the main thread (the game's
                log). A negative control: does the record show the time as
                running, and blame no stall?
  iodisk        affected: the frame loaded an asset (the game's log) while
                bgio was reading from the same limited disk (its log). Does
                the record show the wait on the streamer, that the streamer
                was on I/O, and does anything in it name bgio?
  squeeze       affected: the game's threads were confined to one CPU (the
                squeezer's log). Does the record show the workers kept off
                the CPU by each other, and that the process ran one thread at
                a time?
  alloc         affected: the frame allocated new memory with the cgroup at
                its limit, and the allocation took over twice as long as one
                that needs no room made (the game's log). Does the record put
                the extra time on reclaim, as distinct from the page faults
                any allocation costs?
  compact       affected: the same test of a frame that asked for huge pages
                while the machine's free memory was in single pages. Does the
                record separate compaction from reclaim and from the faults?

Rankings other than the record's own (by total) are replayed on the same
records: the old rule (longest single wait), and "stall time" (a wait on a
thread that was running does not count, and neither does a timer sleep).

usage: analyze_cases.py RUN_DIR
"""
import json
import statistics as st
import sys

ATTACH_NS = 1_500_000_000	# trace_start is taken before the BPF attach
DETACH_NS = 300_000_000
MATCH_NS = 500_000		# record's frame end vs the game's own
MIN_OVERLAP_NS = 1_000_000
FAULT_FRAMES = ("do_swap_page", "handle_mm_fault", "exc_page_fault",
                "do_user_addr_fault")


def load(path):
    with open(path) as f:
        return [json.loads(line) for line in f]


def pct(n, d):
    return f"{n}/{d} ({100 * n / d:.0f}%)" if d else "0/0"


def med(xs):
    xs = list(xs)
    return st.median(xs) if xs else 0.0


def is_irq(hop):
    return bool({"irq", "softirq", "irqexit"} & set(hop["flags"]))


def task_ended(stall):
    hops = stall["hops"]
    return bool(hops) and not ({"irq", "softirq", "irqexit", "idle", "preemptor"}
                               & set(hops[0]["flags"]))


def stall_time(stall):
    if stall["cause"] == "block_timer":
        return 0
    return stall["inherited_ns"] if task_ended(stall) else stall["ns"]


def by_stall_time(rec):
    return max(rec["stalls"], key=stall_time) if rec["stalls"] else None


def old_rule(rec):
    """The group whose single longest wait the old rule would have reported."""
    blocked = [s for s in rec["stalls"] if s["cause"] not in ("runnable", "runqueue")]
    return max(blocked, key=lambda s: s["longest_ns"]) if blocked else None


def is_fault_io(stall):
    return (stall["cause"] == "block_io" and not stall["other"]
            and any(f in FAULT_FRAMES for f in stall["kstack"]))


def from_stream(stall):
    return bool(stall["hops"]) and stall["hops"][0]["comm"] == "mg_stream"


def names(rec, prefix):
    """Does any thread name anywhere in the record start with @prefix?"""
    seen = [t["comm"] for t in rec["threads"]]
    seen += [(t.get("preemptor") or {}).get("comm", "") for t in rec["threads"]]
    for s in rec["stalls"]:
        for h in s["hops"]:
            seen += [h["comm"], (h.get("preemptor") or {}).get("comm", "")]
    seen += [(e.get("by") or {}).get("comm", "") for e in rec.get("events") or []]
    return any(c.startswith(prefix) for c in seen)


def from_server(stall):
    return bool(stall["hops"]) and stall["hops"][0]["comm"] == "mg_server"


def main():
    run = sys.argv[1].rstrip("/")
    lines = load(f"{run}/hitch.jsonl")
    hdr, summ = lines[0], lines[-1]
    recs = [r for r in lines if "frame_id" in r]
    frames = load(f"{run}/frames.jsonl")
    manifest = json.load(open(f"{run}/manifest.json"))
    variant = manifest["variant"]
    budget_us = hdr["budget_ns"] / 1e3
    bursts = (load(f"{run}/bursts.jsonl")
              if variant in ("slack", "ipc", "iodisk", "squeeze") else [])

    def overlap(s, e):
        return sum(max(0, min(e, b["end_ns"]) - max(s, b["start_ns"])) for b in bursts)

    lo = hdr["trace_start_ns"] + ATTACH_NS
    hi = summ["trace_end_ns"] - DETACH_NS
    win = [f for f in frames
           if f["t_end_ns"] - f["frame_us"] * 1000 > lo and f["t_end_ns"] < hi]

    # what an allocation costs when no room has to be made: the quickest
    # tenth of this run's (a MEM_MAX=none run gives the same figure)
    alloc_usual = 0
    if variant in ("alloc", "compact"):
        fast = sorted(f["alloc_us"] for f in win if f["step"])
        alloc_usual = med(fast[:max(1, len(fast) // 10)])

    def affected(f):
        if variant in ("alloc", "compact"):
            return bool(f["step"]) and f["alloc_us"] > 2 * alloc_usual
        hit = overlap(f["t_end_ns"] - f["frame_us"] * 1000,
                      f["t_end_ns"]) >= MIN_OVERLAP_NS
        if variant in ("slack", "ipc", "squeeze"):
            return hit
        if variant == "iodisk":
            return bool(f["asset"]) and hit
        return bool(f["step"])

    aff = [f for f in win if affected(f)]
    quiet = [f for f in win if not affected(f)]
    phase = {"mem": "touch_us", "memthp": "touch_us", "slack": "late_us",
             "ipc": "calls_us", "cpuspike": "burn_us",
             "iodisk": "asset_us", "squeeze": "jobs_us",
             "alloc": "alloc_us", "compact": "alloc_us"}[variant]
    print(f"run {run} ({variant}): {len(win)} frames traced")
    print("\nThe game's own log (frames.jsonl), median per frame:")
    for name, fs in (("other frames", quiet), ("affected frames", aff)):
        over = sum(f["frame_us"] > budget_us for f in fs)
        extra = ""
        if variant.startswith("mem"):
            extra = (f", {med(f['majflt'] for f in fs):.0f} major and "
                     f"{med(f['minflt'] for f in fs):.0f} minor faults")
        print(f"  {name:16}{len(fs):>6} frames, {pct(over, len(fs)):>13} over budget, "
              f"{phase[:-3]} {med(f[phase] for f in fs) / 1e3:.1f} ms{extra}")
    if variant == "iodisk":
        calm = [f for f in quiet if f["asset"]]
        over = sum(f["frame_us"] > budget_us for f in calm)
        print(f"  of the others, loads with bgio idle: {len(calm)} frames, "
              f"{pct(over, len(calm))} over budget, asset "
              f"{med(f['asset_us'] for f in calm) / 1e3:.1f} ms")
        # the workers sit idle while the frame waits for the load; how long
        # does their job phase take then, and in the frames after?
        at = {id(f): i for i, f in enumerate(win)}
        print(f"  job phase by how long the load took "
              f"(usual {med(f['jobs_us'] for f in win if not f['asset']) / 1e3:.1f} ms):")
        for a, b in ((0, 5), (5, 20), (20, 45), (45, 70), (70, 1e9)):
            ids = [at[id(f)] for f in win
                   if f["asset"] and a <= f["asset_us"] / 1e3 < b and at[id(f)] + 2 < len(win)]
            if len(ids) < 5:
                continue
            name = f"{a}-{b} ms" if b < 1e9 else f"over {a} ms"
            row = [med(win[i + d]["jobs_us"] for i in ids) / 1e3 for d in (0, 1, 2)]
            late = sum(win[i + 1]["frame_us"] > budget_us for i in ids)
            print(f"    load {name:11}{len(ids):>4} loads: same frame {row[0]:.1f} ms, next "
                  f"{row[1]:.1f}, the one after {row[2]:.1f}; next frame over budget {pct(late, len(ids))}")
    usual = med(f[phase] for f in quiet) * 1000		# ns, a usual frame's worth
    if variant in ("alloc", "compact"):
        usual = alloc_usual * 1000
        calm = [f for f in quiet if f["step"]]
        over = sum(f["frame_us"] > budget_us for f in calm)
        print(f"  of the others, allocations at normal speed: {len(calm)} frames, "
              f"{pct(over, len(calm))} over budget, alloc "
              f"{med(f['alloc_us'] for f in calm) / 1e3:.1f} ms")
        print(f"  an allocation that needed no room made: {usual / 1e6:.1f} ms; "
              f"{med(f['minflt'] for f in calm):.0f} minor faults")
        if variant == "compact":
            small = sum(f["minflt"] > 2 * med(f["minflt"] for f in calm) for f in aff)
            print(f"  slow allocations that got small pages for some of it "
                  f"(more faults): {pct(small, len(aff))}")

    by_end = sorted(win, key=lambda f: f["t_end_ns"])
    groups = {"affected": [], "noise": []}
    for r in recs:
        if not lo < r["frame_start_ns"] or not r["frame_end_ns"] < hi:
            continue
        f = min(by_end, key=lambda f: abs(f["t_end_ns"] - r["frame_end_ns"]), default=None)
        if f is None or abs(f["t_end_ns"] - r["frame_end_ns"]) > MATCH_NS:
            continue
        groups["affected" if affected(f) else "noise"].append((r, f))

    usual_c = {}
    if groups["noise"]:
        usual_c = {k: st.median(r["cause_ns"][k] for r, _ in groups["noise"])
                   for k in groups["noise"][0][0]["cause_ns"]}

    print("\nhitchtrace records (over-budget frames), by ground truth:")
    for key, pairs in groups.items():
        n = len(pairs)
        if not n:
            print(f"  {key}: none")
            continue
        q = {}		# question -> count

        def yes(name, cond):
            q[name] = q.get(name, 0) + bool(cond)

        amounts, counts, extra_ms = [], [], []
        for r, f in pairs:
            c = r["cause_ns"]
            excess = r["frame_ns"] - r["budget_ns"]
            top, top_st, top_old = r["stalls"][0] if r["stalls"] else None, \
                by_stall_time(r), old_rule(r)
            game_ns = f[phase] * 1000

            if variant.startswith("mem"):
                fio = [s for s in r["stalls"] if is_fault_io(s)]
                fault_ns = (c["oncpu_fault"] + c["oncpu_fault_major"]
                            + c["oncpu_reclaim"] + c["oncpu_compact"])
                mem_ns = fault_ns + sum(s["ns"] for s in fio)
                yes("names memory: an I/O group whose stack is a page fault", fio)
                yes("memory (faults + that I/O) is >= half the overrun", mem_ns >= 0.5 * excess)
                yes("that I/O group is top-ranked, by total", top and is_fault_io(top))
                yes("... ranked by stall time", top_st and is_fault_io(top_st))
                yes("... under the old rule (longest single wait)", top_old and is_fault_io(top_old))
                if fio:
                    yes("its wait count is the game's major-fault count, +-2",
                        abs(fio[0]["count"] - f["majflt"]) <= 2)
                    counts.append(fio[0]["count"])
                if game_ns:
                    amounts.append(mem_ns / game_ns)
            elif variant == "slack":
                timer = [s for s in r["stalls"] if s["cause"] == "block_timer"]
                yes("the timer sleep is top-ranked, by total", top and top["cause"] == "block_timer")
                yes("the record has any field saying the sleep ended late", False)
                if timer:
                    amounts.append(timer[0]["ns"] / (f["limiter_us"] * 1000 or 1))
                    extra_ms.append(timer[0]["ns"] / 1e6)
            elif variant == "ipc":
                srv = [s for s in r["stalls"] if from_server(s)]
                h0 = srv[0]["hops"][0] if srv else None
                kept = bool(h0) and h0["cause"] == "runnable"
                named = kept and (h0.get("preemptor") or {}).get("comm", "").startswith("bgload")
                yes("a group of waits ended by mg_server", srv)
                yes("... saying the server was kept off the CPU", kept)
                yes("... and naming bgload as its preemptor", named)
                yes("that group is top-ranked, by total", top and from_server(top))
                yes("... ranked by stall time", top_st and from_server(top_st))
                yes("... under the old rule (longest single wait)", top_old and from_server(top_old))
                if srv:
                    counts.append(srv[0]["count"])
                    if game_ns > usual:
                        amounts.append(srv[0]["inherited_ns"] / (game_ns - usual))
            elif variant == "iodisk":
                grp = [s for s in r["stalls"] if from_stream(s)]
                h0 = grp[0]["hops"][0] if grp else None
                on_io = bool(h0) and h0["cause"] == "block_io"
                yes("a wait ended by mg_stream, which was blocked on I/O", on_io)
                yes("... ending at an interrupt",
                    on_io and len(grp[0]["hops"]) > 1 and is_irq(grp[0]["hops"][1]))
                yes("that group is top-ranked, by total", top and from_stream(top))
                yes("... ranked by stall time", top_st and from_stream(top_st))
                yes("anything in the record names bgio", names(r, "bgio"))
                if on_io and game_ns:
                    amounts.append(h0["ns"] / game_ns)
            elif variant in ("alloc", "compact"):
                extra = game_ns - usual
                kernel = c["oncpu_reclaim"] + c["oncpu_compact"]
                yes("memory (faults + reclaim + compaction) is >= half the overrun",
                    kernel + c["oncpu_fault"] + c["oncpu_fault_major"] >= 0.5 * excess)
                if variant == "compact":
                    yes("compaction + reclaim is >= half the overrun", kernel >= 0.5 * excess)
                    yes("compaction is the larger of the two",
                        c["oncpu_compact"] > c["oncpu_reclaim"])
                    yes("compaction is >= half the overrun",
                        c["oncpu_compact"] >= 0.5 * excess)
                mem_group = top and (top["cause"].startswith("oncpu") or is_fault_io(top))
                yes("reclaim is >= half the overrun", c["oncpu_reclaim"] >= 0.5 * excess)
                yes("fault time is >= half the overrun", c["oncpu_fault"] >= 0.5 * excess)
                yes("any stall group is about memory", any(
                    s["cause"].startswith("oncpu") or is_fault_io(s) for s in r["stalls"]))
                yes("the top-ranked group is about memory", mem_group)
                yes("anything says what was reclaimed, or why memory was short", False)
                extra_ms.append(c["oncpu_fault"] / 1e6)
                if extra > 1e6:
                    amounts.append(kernel / extra)
            elif variant == "squeeze":
                own = lambda p: bool(p) and p["tgid"] == hdr["pid"]
                workers = [t for t in r["threads"] if t["comm"].startswith("mg_worker")]
                queued = max((t["cause_ns"]["runnable"] + t["cause_ns"]["runqueue"]
                              for t in workers), default=0)
                h0 = top["hops"][0] if top and top["hops"] else None
                kept = bool(h0) and h0["comm"].startswith("mg_worker") \
                    and h0["cause"] == "runnable"
                lines = [t["preemptor"] for t in r["threads"] if t.get("preemptor")]
                ran = sum(t["cause_ns"]["oncpu"] for t in r["threads"])
                yes("some worker waited for a CPU >= half the overrun", queued >= 0.5 * excess)
                yes("top group: a wait on a worker that was kept off the CPU", kept)
                yes("... by a thread of the game itself", kept and own(h0.get("preemptor")))
                yes("every preemptor named in the record is the game's own",
                    lines and all(own(p) for p in lines))
                yes("another process named as a preemptor", any(not own(p) for p in lines))
                yes("all threads together ran <= 1.1 CPUs' worth",
                    ran <= 1.1 * r["frame_ns"])
                yes("any field saying which CPU, or that other CPUs were idle",
                    False)
                counts.append(ran / r["frame_ns"])
                if kept and game_ns > usual:
                    amounts.append(h0["sched_ns"] / (game_ns - usual))
            elif variant == "cpuspike":
                run_extra = c["oncpu"] - usual_c.get("oncpu", 0)
                st_top = stall_time(top_st) if top_st else 0
                yes("running is >= half the overrun", c["oncpu"] >= 0.5 * excess)
                yes("no stall worth a quarter of the overrun (stall-time rule)",
                    st_top < 0.25 * excess)
                yes("inherited stalls under a quarter of the overrun",
                    c["resolved_inherited"] < 0.25 * excess)
                if game_ns:
                    amounts.append(run_extra / game_ns)

        print(f"  {key} frames: {n} records, overrun median "
              f"{med((r['frame_ns'] - r['budget_ns']) / 1e6 for r, _ in pairs):.1f} ms, "
              f"{med(len(r.get('events') or []) for r, _ in pairs):.0f} timeline intervals")
        for name, v in q.items():
            print(f"    {name + ':':62}{pct(v, n)}")
        if amounts:
            what = {"mem": "memory time / the game's own walk time",
                    "memthp": "memory time / the game's own walk time",
                    "slack": "timer group / the game's own limiter time",
                    "ipc": "server group's inherited time / the game's extra call time",
                    "cpuspike": "extra running time / the game's own burn time",
                    "iodisk": "the streamer's I/O in the chain / the game's own load time",
                    "squeeze": "the worker's time off the CPU / the game's extra job time",
                    "alloc": "reclaim time / the allocation's extra time",
                    "compact": "compaction + reclaim / the allocation's extra time"}[variant]
            print(f"    {what + ':':62}median {100 * med(amounts):.0f}%")
        if counts and variant == "squeeze":
            what = "CPUs' worth the game's threads ran, per frame:"
            print(f"    {what:62}median {med(counts):.2f}")
        elif counts:
            print(f"    {'waits in that group:':62}median {med(counts):.0f}")
        if extra_ms:
            what = "timer group, median:" if variant == "slack" else "fault time, median:"
            print(f"    {what:62}{med(extra_ms):.1f} ms")


if __name__ == "__main__":
    main()
