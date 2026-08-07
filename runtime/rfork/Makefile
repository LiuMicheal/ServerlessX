ROOT_DIR := $(abspath ../..)
BUILD_DIR ?= $(ROOT_DIR)/build/rfork
CC ?= cc
AR ?= ar
CPPFLAGS += -Iinclude
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Werror

LIBRARY := $(BUILD_DIR)/libserverlessx_rfork.a
OBJECT := $(BUILD_DIR)/rfork_runtime.o
TEST_BINARY := $(BUILD_DIR)/test_rfork_runtime

.PHONY: all test

all: $(LIBRARY)

test: $(TEST_BINARY)
	$(TEST_BINARY)

$(BUILD_DIR):
	mkdir -p $@

$(OBJECT): src/rfork_runtime.c include/serverlessx/rfork_runtime.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(LIBRARY): $(OBJECT)
	$(AR) rcs $@ $^

$(TEST_BINARY): tests/test_rfork_runtime.c $(LIBRARY)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(LIBRARY) -o $@
