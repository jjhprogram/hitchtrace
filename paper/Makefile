# Build the paper. Needs pdflatex and bibtex (e.g. TinyTeX:
#   export PATH=$HOME/.TinyTeX/bin/x86_64-linux:$PATH).
PAPER = main

SECTIONS  = $(wildcard [0-9][0-9]_*.tex)
FIGURES   = $(wildcard Fig/*)
TABLES    = Table/comparison.tex Table/ftrace_expressiveness.tex Table/taxonomy.tex Table/hitchbench_attribution.tex \
            Table/case_studies.tex Table/catch.tex Table/overhead.tex
GENERATED = Table/hitchbench_table.tex Table/hitchbench_macros.tex plot/data/hitchbench_breakdown.dat

all: $(PAPER).pdf

$(GENERATED): plot/src/hitchbench_results.py
	python3 plot/src/hitchbench_results.py

$(PAPER).pdf: $(PAPER).tex $(SECTIONS) $(FIGURES) $(TABLES) $(GENERATED) references.bib
	pdflatex -interaction=nonstopmode -halt-on-error $(PAPER)
	bibtex $(PAPER)
	pdflatex -interaction=nonstopmode -halt-on-error $(PAPER)
	pdflatex -interaction=nonstopmode -halt-on-error $(PAPER)

data:
	python3 plot/src/hitchbench_results.py

clean:
	rm -f $(PAPER).{aux,bbl,blg,log,out,pdf}

.PHONY: all clean data
