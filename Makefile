PYTHON ?= python3
BOARD ?= lsc-smart-indoor
ARGS ?=
.PHONY: list check application image web sd test source
list:
	$(PYTHON) tools/build.py list
check:
	$(PYTHON) tools/build.py check --board $(BOARD) $(ARGS)
application:
	$(PYTHON) tools/build.py application --board $(BOARD) $(ARGS)
image:
	$(PYTHON) tools/build.py image --board $(BOARD) $(ARGS)
web:
	./compile-web.sh --board $(BOARD) $(ARGS)
sd:
	./compile-sd.sh --board $(BOARD) $(ARGS)
test:
	$(PYTHON) tools/test.py
source:
	$(PYTHON) tools/export_source.py
