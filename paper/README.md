# hitchtrace paper draft

USENIX two-column format (`usenix-2020-09.sty`, the official USENIX style file).

## Build

```sh
export PATH=$HOME/.TinyTeX/bin/x86_64-linux:$PATH   # TinyTeX, installed per-user
make            # regenerates data/ from ../tests/out, then pdflatex + bibtex
```

Packages beyond TinyTeX's base were installed with
`tlmgr install psnfss pslatex microtype cite breakurl booktabs multirow enumitem pgf pgfplots caption listings xspace`.

## Where the numbers come from

Every evaluation number about hitchbench is generated. None is typed by hand:

- `scripts/hitchbench_results.py` joins `../tests/out/<class>.gt.jsonl` (hitchbench ground truth)
  with `../tests/out/<class>.hitch.jsonl` (hitchtrace records) and writes
  - `data/hitchbench_table.tex`: rows of Table 4
  - `data/hitchbench_breakdown.dat`: Figure 4
  - `data/hitchbench_macros.tex`: `\hb...` macros used in the abstract and §5
- The vkcube numbers in §5.4 come from `../tests/out/vkcube_*.jsonl` (see commit 65c66fc).
- The §5.5 "real Vulkan application" numbers come from an earlier session whose artifacts were
  lost. They are marked in red and must be regenerated.

All runs so far are on the development VM (Hyper-V, lavapipe). §5.6 (overhead, M3) and §5.7
(prevalence, M0) are planned but not yet measured.

## Draft conventions

- `\todo{...}` (red, bracketed): something to write, check or decide.
- `\tbd{...}` (red, bold): a number or claim not yet measured.
- `references.bib` entries with `note = {VERIFY...}` were assembled from research notes and
  have not been checked against the primary source.
