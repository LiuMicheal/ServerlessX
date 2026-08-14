PYTHON ?= python3

.PHONY: help test test-python test-rfork rfork spd-gdr gpu-rfork-canary

help:
	@printf '%s\n' \
	  'make test       Run portable Python and C tests' \
	  'make rfork      Build the C rfork userspace runtime' \
	  'make spd-gdr    Build the optional CUDA DMA-BUF/RDMA data plane' \
	  'make gpu-rfork-canary  Build the private-lab PhOS GPU canary'

test: test-python test-rfork

test-python:
	PYTHONPATH=src $(PYTHON) -m unittest discover -s tests -v

test-rfork:
	$(MAKE) -C runtime/rfork test

rfork:
	$(MAKE) -C runtime/rfork all

spd-gdr:
	$(MAKE) -C native/spd-gdr all

gpu-rfork-canary:
	$(MAKE) -C native/spd-gdr phos-rfork-canary
