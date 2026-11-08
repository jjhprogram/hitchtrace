# CLAUDE.md

hitchtrace: an eBPF tool that explains frame hitches in unmodified Vulkan
games on Linux, built for a master's thesis. Overview and layout:
[README.md](README.md).

## Where things are written down

| Question | File |
|---|---|
| How does the code work, and why? | [docs/design.md](docs/design.md) |
| What has been measured? (all results) | [docs/evaluation.md](docs/evaluation.md) |
| What is still open? (tool, runs, paper, citations) | [docs/todo.md](docs/todo.md) |
| Why is the design the way it is? | [docs/history.md](docs/history.md) |
| Plan for bare metal and real games | [docs/bare-metal-plan.md](docs/bare-metal-plan.md) |
| Documented real-world stutters | [docs/real-world-cases.md](docs/real-world-cases.md) |
| How to run a harness | `tests/run_hitch_tests.sh` header, [cases/README.md](cases/README.md), [tools/overhead/README.md](tools/overhead/README.md), [tools/catch/README.md](tools/catch/README.md) |
| Where each paper number comes from | [paper/README.md](paper/README.md) |

Each fact has one home. When a result changes, update `docs/evaluation.md`
(and the paper's table that shows it). When a gap closes, take it off
`docs/todo.md`. A design change goes in `docs/design.md`, with a line in
`docs/history.md`. Do not start new status or summary files.

## Build and test

```sh
make                         # build/hitchtrace, build/hitchbench
make layer layer-install     # the Vulkan layer, for this user
make -C cases                # the case-study game and background programs
make -C tools/overhead       # delayacct_frames
cmake -S src/hitchtracegui -B build/hitchtracegui -G Ninja && cmake --build build/hitchtracegui
python3 -m unittest tests/test_check_hitches.py
export PATH=$HOME/.TinyTeX/bin/x86_64-linux:$PATH && make -C paper
```

After editing `src/hitchtrace/`, run `make` and the unit tests. The
end-to-end tests (`sudo tests/run_hitch_tests.sh`) need root: the user runs
them.

## Rules for working here

- **Passwords.** Never type, store or handle a password. A command that needs
  root goes to the user, to run in their terminal.
- **The one passwordless sudo rule** is
  `asdf ALL=(root) NOPASSWD: /home/asdf/ebpf/build/hitchtrace`
  (`/etc/sudoers.d/hitchtrace`).
  - It is root-equivalent, since hitchtrace loads BPF programs. Use it only
    to run hitchtrace for the task at hand.
  - Keep the binary at that path.
- **Long or root-needing runs** (overhead, catch, end-to-end tests) are
  started by the user. While one is running, run no measurements and open no
  game windows of your own.
- **Git.** Commit only when asked; never push unless asked.
- **Downloads.** Ask before downloading anything: say the file, source and
  size. Auto mode blocks compiling downloaded source; the user runs those
  builds.
- **The paper.** The user also edits it in Overleaf.
  - When asked how a change would look, give LaTeX edits for the user to
    type; don't apply them.
  - The local `paper/` is what builds. `build/paper-overleaf.zip` is the
    upload copy: rebuild it with
    `cd paper && git ls-files | zip -q ../build/paper-overleaf.zip -@`.

## Facts that bite

- **The VM.** All development and every result so far is on a Hyper-V VM:
  Ubuntu 24.04, kernel 7.0, 10 vCPUs, lavapipe software Vulkan, no PMU, no
  GPU.
  - Its timing drifts 10 to 15% between runs, so compare frames within one
    run against ground truth, or interleave A/B runs. Never compare two
    separate runs.
  - lavapipe renders inside `vkQueuePresentKHR`. X11 does not pace frames;
    use Wayland.
- **Paper numbers.**
  - hitchbench numbers are generated from `tests/out/` by
    `paper/plot/src/hitchbench_results.py`; never type them by hand.
  - Other numbers are typed from run summaries; `paper/README.md` lists
    each source.
- **Runs in `build/`** (untracked, about 16 GB). Runs named in
  `docs/evaluation.md` or `paper/README.md` are the evidence for the paper;
  don't delete them.
- **Tool locations.**
  - perf with BPF skeletons (for `--off-cpu`): `build/perf/perf`.
  - Perfetto: `build/perfetto/v59.1/linux-amd64`.
  - Ubuntu's own perf lacks the BPF skeletons that `--off-cpu` needs.
- **Delay accounting** counts a thread only if `kernel.task_delayacct` was on
  when the thread was created.
