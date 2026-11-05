#!/usr/bin/env python3
# SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
"""Make one hitchview page per case recording, with its reading guide.

Each page is named after its recording, the name hitchexplorer shows for it
(cases/out/iodisk/iodisk.html for cases/out/iodisk/hitch.jsonl), opens on the
same typical frame the explorer's examples use, and starts with the guide in
cases/pages/<recording>.md: what was going on, how to read the page, what the
record says about the cause, and what it cannot say.

usage: cases/make_pages.py [RECORDING ...]     (default: all of them)
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# recording (directory in cases/out), frame to open on, what happens in it
PAGES = [
    ("bgcpu-timeline", 2241, "Another program takes the game's cores"),
    ("iothread-timeline", 2129, "The main thread waits for a streaming thread"),
    ("iodisk", 2144, "Another program reads from the same slow disk"),
    ("mem", 779, "Touching memory that was swapped out"),
    ("slack", 1030, "The frame limiter's sleep ends late"),
    ("ipc", 2475, "Calls to a server that cannot get its CPU"),
    ("cpuspike", 2009, "The game's own CPU work"),
    ("squeeze", 540, "The game's threads confined to one core"),
    ("alloc", 1604, "New memory while memory is full"),
    ("compact", 1514, "Huge pages while free memory is in pieces"),
]


def main():
    want = set(sys.argv[1:])
    unknown = want - {p[0] for p in PAGES}
    if unknown:
        sys.exit("unknown recording: " + ", ".join(sorted(unknown)))
    for rec, frame, title in PAGES:
        if want and rec not in want:
            continue
        run = os.path.join(ROOT, "cases", "out", rec)
        src = os.path.join(run, "hitch.jsonl")
        if not os.path.exists(src):
            print(f"skip {rec}: no {os.path.relpath(src, ROOT)}")
            continue
        subprocess.run([sys.executable, os.path.join(ROOT, "tools", "hitchview.py"), src,
                        "-o", os.path.join(run, rec + ".html"), "--title", f"{rec}: {title}",
                        "--frame", str(frame), "--notes", os.path.join(HERE, "pages", rec + ".md")],
                       check=True)


if __name__ == "__main__":
    main()
