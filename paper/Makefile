# Build the paper. Needs pdflatex and bibtex (e.g. TinyTeX:
#   export PATH=$HOME/.TinyTeX/bin/x86_64-linux:$PATH).
PAPER = main

all: $(PAPER).pdf

data/hitchbench_table.tex data/hitchbench_breakdown.dat data/hitchbench_macros.tex: scripts/hitchbench_results.py
	python3 scripts/hitchbench_results.py

$(PAPER).pdf: $(PAPER).tex references.bib usenix-2020-09.sty data/hitchbench_table.tex data/hitchbench_breakdown.dat data/hitchbench_macros.tex
	pdflatex -interaction=nonstopmode -halt-on-error $(PAPER)
	bibtex $(PAPER)
	pdflatex -interaction=nonstopmode -halt-on-error $(PAPER)
	pdflatex -interaction=nonstopmode -halt-on-error $(PAPER)

data: 
	python3 scripts/hitchbench_results.py

clean:
	rm -f $(PAPER).{aux,bbl,blg,log,out,pdf}

.PHONY: all clean data
