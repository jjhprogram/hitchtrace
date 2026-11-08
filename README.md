# hitchtrace

Explains frame hitches in unmodified Vulkan games on Linux. hitchtrace is an
eBPF tool. For each frame over its budget it records:

- how the presenting thread's frame time splits into on-CPU, preempted,
  run-queue and blocked time, by kernel cause;
- for each of its waits, the chain of wakers behind it: who woke it, whether
  that waker was itself stalled, and who held its CPU.

The record is built in the kernel as the frame runs, and leaves the kernel
only when the frame ends over budget.

This is a thesis prototype. The paper draft is in [`paper/`](paper/), and
what has been measured so far is in
[`docs/evaluation-status.md`](docs/evaluation-status.md).

## Requirements

Root, and a kernel with BTF (`/sys/kernel/btf/vmlinux`). Only Linux 7.0 has
been tested. The typed ksym `__preempt_count` needs `CONFIG_KALLSYMS_ALL=y`
(set by major distributions). User stacks (`-U`) need frame pointers.

To build: `clang gcc make libelf-dev zlib1g-dev` and `bpftool` (on Ubuntu:
`linux-tools-common linux-tools-$(uname -r)`). libbpf v1.7.0 is a submodule
(`libbpf/`), built statically.

## Build and run

```bash
git submodule update --init       # libbpf
make                              # build/hitchtrace and build/hitchbench
make layer layer-install          # the Vulkan layer that marks frames, for this user
```

The frame marks come from an implicit Vulkan layer, enabled per app with
`HITCHTRACE=1` ([`src/layer/README.md`](src/layer/README.md)). Point `-x` at the
layer, not at the app:

```bash
HITCHTRACE=1 vkcube &
sudo build/hitchtrace -p $! -x build/libVkLayer_hitchtrace.so \
     -m hitch_frame_mark -M hitch_present_end -b 16667 -o hitches.jsonl
```

`build/hitchtrace --help` lists all options. `tools/hitchview.py` turns a
JSONL file into an HTML page, and `src/hitchtracegui/` is a desktop viewer
for the same files.

## Tests

```bash
sudo tests/run_hitch_tests.sh               # hitchbench end to end, every injector class
python3 -m unittest tests/test_check_hitches.py
gcc -g -O1 -fno-omit-frame-pointer -fsanitize=address,undefined -Isrc/hitchtrace \
    tests/syms_selftest.c src/hitchtrace/syms.c -lelf -lz -o build/syms_selftest && build/syms_selftest
```

## Layout

| Path | What it is |
|---|---|
| `src/hitchtrace/` | the tool: BPF programs (`hitchtrace.bpf.c`), shared types (`hitchtrace.h`), loader and output (`hitchtrace.c`), symbolizer (`syms.c`) |
| `src/layer/` | the implicit Vulkan layer that marks frames |
| `src/hitchtracegui/` | a Qt desktop viewer for records, which can also start a recording |
| `tests/` | hitchbench (a synthetic frame loop with injectable stalls), its checkers and end-to-end test |
| `cases/` | ten case studies reproducing documented Linux gaming stutters: the game, the background programs, `run.sh`, results; `cases/hitchlab/` runs them as an app |
| `tools/` | `hitchview` (HTML page per run), `overhead/` (cost against other tools), `catch/` (rare hitches, seven tools) |
| `paper/` | the paper draft (IEEEtran) |
| `docs/` | evaluation status, protocols, the case catalogue, the original design notes |
| `libbpf/` | upstream libbpf (its own license) |
