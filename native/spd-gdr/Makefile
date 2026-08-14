ROOT_DIR := $(abspath ../..)
BUILD_DIR ?= $(ROOT_DIR)/build/spd-gdr
CUDA_HOME ?= /usr/local/cuda
CC ?= cc
CXX ?= c++
CPPFLAGS += -Iinclude -I$(CUDA_HOME)/include
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++17 -Wall -Wextra -Werror
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Werror -fno-omit-frame-pointer
CUDA_LIBDIR ?= $(CUDA_HOME)/lib64

LIBRARY := $(BUILD_DIR)/libserverlessx_spd_gdr.so
DMABUF_PROBE := $(BUILD_DIR)/vmm_dmabuf_gdr_checksum
STAGING_PROBE := $(BUILD_DIR)/vmm_staging_gdr
PHOS_RFORK_CANARY := $(BUILD_DIR)/phos_gpu_rfork_canary
PHOS_RFORK_BRIDGE_OBJECT := $(BUILD_DIR)/phos_gpu_rfork_bridge.o
RFORK_DIR := $(ROOT_DIR)/runtime/rfork
PHOS_RFORK_RUNTIME_BUILD_DIR := $(BUILD_DIR)/rfork-runtime
RFORK_LIBRARY := $(PHOS_RFORK_RUNTIME_BUILD_DIR)/libserverlessx_rfork.a
PHOS_RFORK_BRIDGE_CFLAGS := $(CFLAGS) -fno-stack-protector
PHOS_RFORK_CANARY_CXXFLAGS := $(CXXFLAGS) -fno-stack-protector

.PHONY: all check-deps check-cuda library probes phos-rfork-canary rfork-runtime

all: check-deps library probes

check-deps:
	@printf '%s\n' '#include <cuda.h>' '#include <infiniband/verbs.h>' | \
	  $(CXX) $(CPPFLAGS) -E -x c++ - >/dev/null || { \
	  printf '%s\n' 'error: CUDA and libibverbs development headers are required'; \
	  exit 2; }

check-cuda:
	@printf '%s\n' '#include <cuda.h>' | \
	  $(CXX) $(CPPFLAGS) -E -x c++ - >/dev/null || { \
	  printf '%s\n' 'error: CUDA development headers are required'; \
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

phos-rfork-canary: check-cuda $(PHOS_RFORK_CANARY)

rfork-runtime:
	$(MAKE) -C $(RFORK_DIR) BUILD_DIR=$(PHOS_RFORK_RUNTIME_BUILD_DIR) \
	  RFORK_OBJECT_CFLAGS=-fno-stack-protector all

$(PHOS_RFORK_BRIDGE_OBJECT): probes/phos_gpu_rfork_bridge.c \
    probes/phos_gpu_rfork_bridge.h rfork-runtime | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -I$(RFORK_DIR)/include $(PHOS_RFORK_BRIDGE_CFLAGS) -c $< -o $@

$(PHOS_RFORK_CANARY): probes/phos_gpu_rfork_canary.cpp \
    probes/phos_gpu_rfork_bridge.h $(PHOS_RFORK_BRIDGE_OBJECT) rfork-runtime | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) -I$(RFORK_DIR)/include $(PHOS_RFORK_CANARY_CXXFLAGS) \
	  -no-pie -rdynamic $< $(PHOS_RFORK_BRIDGE_OBJECT) $(RFORK_LIBRARY) -o $@ \
	  -ldl
