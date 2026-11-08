# hitchtrace paper draft

IEEEtran conference format (`\documentclass[conference,compsoc]{IEEEtran}`), laid out like the
GSan paper template: `main.tex` holds the preamble and inputs one file per section.

## Layout

- `main.tex`: preamble, macros, title, section order.
- `00_abstract.tex` … `09_conclusion.tex`: one file per section, in order (`07_discussion.tex`
  comes before related work, as in the template).
- `Fig/`: figure sources (TikZ/pgfplots, and the record in Figure 3), input from the section files.
- `Table/`: table bodies, input from the section files, plus the generated
  `hitchbench_table.tex` and `hitchbench_macros.tex`.
- `plot/src/`: generator script. `plot/data/`: generated plot data.

## Build

```sh
export PATH=$HOME/.TinyTeX/bin/x86_64-linux:$PATH   # TinyTeX, installed per-user
make            # regenerates the generated files from ../tests/out, then pdflatex + bibtex
```

Beyond TinyTeX's base the preamble needs, among others, `IEEEtran`, `titlesec`, `paralist`,
`authblk`, `fontawesome`, `arydshln`, `tcolorbox`, `makecell`, `hyphenat`, `datetime`, `ulem`,
`pgf`, `pgfplots`, `booktabs`, `multirow`, `caption`, `listings`, `xspace`, `xstring`, `cite`,
`psnfss`. On top of what the USENIX draft needed, these were installed with

```sh
tlmgr update --self
tlmgr install ieeetran titlesec paralist preprint fontawesome arydshln tcolorbox environ \
  trimspaces tikzfill pdfcol listingsutf8 makecell hyphenat datetime fmtcount comment xstring colortbl
```

On Overleaf, upload this directory as it is: the generated files are checked in, so only
`main.tex` needs to compile.

## Where the numbers come from

Every evaluation number about hitchbench is generated. None is typed by hand:

- `plot/src/hitchbench_results.py` joins `../tests/out/<class>.gt.jsonl` (hitchbench ground truth)
  with `../tests/out/<class>.hitch.jsonl` (hitchtrace records) and writes
  - `Table/hitchbench_table.tex`: rows of the attribution table (`Table/hitchbench_attribution.tex`)
  - `plot/data/hitchbench_breakdown.dat`: data for `Fig/hitchbench_breakdown.tex`
  - `Table/hitchbench_macros.tex`: `\hb...` macros used in the abstract, the introduction and the
    hitchbench subsections of the evaluation
- The vkcube numbers in the cause-gating subsection come from `../tests/out/vkcube_*.jsonl`
  (see commit 65c66fc).

The other evaluation numbers are typed from the runs' own summaries, and must be updated by hand
when those runs are repeated:

- Case studies (`Table/case_studies.tex`): `../cases/README.md`, data in `../cases/out/`.
- Rare hitches (`Table/catch.tex`): `../tools/catch/README.md` (protocol) and
  `../docs/evaluation.md`; runs `../build/catch/catch-20261104-171550`,
  `../build/catch/catch-ipc-holder`, `../build/catch/catch-perfoffcpu` and
  `../build/catch/catch-perfetto` (each run's `summary.md` and `scores.json`).
- Overhead (`Table/overhead.tex`): `../docs/evaluation.md`; runs
  `../build/overhead/pilot-20261103-113525`, `../build/overhead/holder-pipe`,
  `../build/overhead/perfoffcpu-pilot` and `../build/overhead/perfetto-pilot`
  (each run's `summary.md`; the BPF-time totals sum its per-program rows).
- The record in `Fig/record.tex`: frame 2475 of `../cases/out/ipc-holder/hitch.txt`, three thread
  lines cut.
- Record size (10,344 bytes) and line counts in the introduction and design: `src/hitchtrace/hitchtrace.h`,
  `wc -l`.

All runs so far are on the development VM (Hyper-V, lavapipe). The overhead subsection is a pilot;
bare-metal overhead, real titles and prevalence (M0) are planned but not yet measured.

## Draft conventions

- `\todo{...}` (red, bracketed): something to write, check or decide. Hidden when
  `publicversion` is set to `true` in `main.tex`.
- `\tbd{...}` (red, bold): a number or claim not yet measured. Printed without the red when
  `publicversion` is `true`.
- Cross-references use `\autoref{...}`; run-in headings use `\paragraph{Title}` with no
  trailing period (the class adds it).
- `references.bib` entries with `note = {VERIFY...}` were assembled from research notes and
  have not been checked against the primary source.
