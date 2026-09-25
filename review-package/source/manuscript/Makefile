MAIN = main
LATEX = pdflatex
BIBTEX = bibtex

.PHONY: all clean watch

all: $(MAIN).pdf

$(MAIN).pdf: $(MAIN).tex references.bib sections/*.tex
	$(LATEX) $(MAIN)
	$(BIBTEX) $(MAIN)
	$(LATEX) $(MAIN)
	$(LATEX) $(MAIN)

watch:
	latexmk -pdf -pvc $(MAIN).tex

clean:
	latexmk -C
	rm -f *.bbl *.blg *.run.xml *.bcf *-blx.bib

count:
	@texcount -inc -total $(MAIN).tex
