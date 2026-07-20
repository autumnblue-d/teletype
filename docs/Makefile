.PHONY: build docs clean

# Pick the Python interpreter in priority order:
#   1. An explicitly activated virtualenv ($VIRTUAL_ENV) -- e.g. the /opt/venv
#      baked into the AVR32 Docker image, activated in the run command. This
#      must win over ../.venv, which the bind mount exposes as the host's
#      (macOS, unrunnable-on-Linux) venv inside the container.
#   2. A repo-root .venv created via `uv venv` on the host.
#   3. The system python3 (e.g. on CI, where deps are installed globally).
VENV_PY := $(wildcard ../.venv/bin/python)
PYTHON := $(if $(VIRTUAL_ENV),$(VIRTUAL_ENV)/bin/python,$(if $(VENV_PY),$(VENV_PY),python3))

# `docs` is an alias for the default build (all four doc artifacts).
docs: build

build: teletype.pdf teletype.html cheatsheet.pdf cheatsheet-i2c.pdf

clean:
	rm -f teletype.pdf teletype.html
	rm -f cheatsheet.pdf cheatsheet-i2c.pdf
	rm -f cheatsheet/cheatsheet-*.*
	cd cheatsheet && latexmk -xelatex -c

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
