.PHONY: build clean

# Use the project venv's Python if it exists (created via `uv venv` at the repo
# root), otherwise fall back to the system python3 (e.g. on CI, where deps are
# installed globally).
VENV_PY := $(wildcard ../.venv/bin/python)
PYTHON := $(if $(VENV_PY),$(VENV_PY),python3)

build: teletype.pdf teletype.html cheatsheet.pdf cheatsheet-i2c.pdf

clean:
	rm -f teletype.pdf && \
		rm -f teletype.html && \
		rm -f cheatsheet/cheatsheet-*.* && \
		cd cheatsheet && \
			latexmk -xelatex -c && \
			rm -f cheatsheet.pdf

teletype.pdf: $(wildcard *.md ops/*.md ops/*.toml) \
		../utils/docs.py ../CHANGELOG.md
	$(PYTHON) ../utils/docs.py teletype.pdf

teletype.html: $(wildcard *.md ops/*.md ops/*.toml) \
		../utils/docs.py ../CHANGELOG.md $(wildcard ../utils/templates/*)
	$(PYTHON) ../utils/docs.py teletype.html

cheatsheet/cheatsheet-%.tex: $(wildcard ops/*.toml) ../utils/cheatsheet.py
	$(PYTHON) ../utils/cheatsheet.py $* "cheatsheet/cheatsheet-$*.tex"

cheatsheet/cheatsheet-%.pdf: cheatsheet/cs-common.tex cheatsheet/cheatsheet-%.tex
	cd cheatsheet && latexmk -xelatex cs-common.tex -jobname=cheatsheet-$*

cheatsheet.pdf: cheatsheet/cheatsheet-core.pdf
	cp cheatsheet/cheatsheet-core.pdf cheatsheet.pdf

cheatsheet-i2c.pdf: cheatsheet/cheatsheet-i2c.pdf
	cp cheatsheet/cheatsheet-i2c.pdf .
