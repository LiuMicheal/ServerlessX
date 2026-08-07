ROOT_DIR := $(abspath ../..)
BUILD_DIR ?= $(ROOT_DIR)/build/spd-gdr
CUDA_HOME ?= /usr/local/cuda
CXX ?= c++
CPPFLAGS += -Iinclude -I$(CUDA_HOME)/include
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++17 -Wall -Wextra -Werror
CUDA_LIBDIR ?= $(CUDA_HOME)/lib64

LIBRARY := $(BUILD_DIR)/libserverlessx_spd_gdr.so
DMABUF_PROBE := $(BUILD_DIR)/vmm_dmabuf_gdr_checksum
STAGING_PROBE := $(BUILD_DIR)/vmm_staging_gdr

.PHONY: all check-deps library probes

all: check-deps library probes

check-deps:
	@printf '%s\n' '#include <cuda.h>' '#include <infiniband/verbs.h>' | \
	  $(CXX) $(CPPFLAGS) -E -x c++ - >/dev/null || { \
	  printf '%s\n' 'error: CUDA and libibverbs development headers are required'; \
	  exit 2; }

library: $(LIBRARY)

probes: $(DMABUF_PROBE) $(STAGING_PROBE)

$(BUILD_DIR):
	mkdir -p $@

$(LIBRARY): src/spd_gdr.cpp include/serverlessx/spd_gdr.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -DSPD_GDR_DIRECT_ONLY -fPIC -shared \
	  -Wl,-soname,libserverlessx_spd_gdr.so $< -o $@ \
	  -L$(CUDA_LIBDIR) -lcuda -lcudart -libverbs -ldl -pthread -lrt

$(DMABUF_PROBE): probes/vmm_dmabuf_gdr_checksum.cpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< -o $@ \
	  -L$(CUDA_LIBDIR) -lcuda -libverbs -ldl

$(STAGING_PROBE): probes/vmm_staging_gdr.cpp $(LIBRARY) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< -o $@ \
	  -L$(BUILD_DIR) -lserverlessx_spd_gdr -Wl,-rpath,'$$ORIGIN' \
	  -L$(CUDA_LIBDIR) -lcuda -lcudart -libverbs -ldl -pthread -lrt
