CC ?= gcc
CFLAGS ?= -O2 -g -std=c11 -Wall -Wextra -Werror -D_POSIX_C_SOURCE=200809L
CPPFLAGS ?= -I.
BIN_DIR ?= bin

.PHONY: all clean

all: $(BIN_DIR)/slsm_cn $(BIN_DIR)/slsm_sn

$(BIN_DIR):
	mkdir -p $@

$(BIN_DIR)/slsm_cn: slsm_cn.c slsm_common.c slsm_common.h include/slsm_uapi.h | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) slsm_cn.c slsm_common.c -o $@

$(BIN_DIR)/slsm_sn: slsm_sn.c slsm_common.c slsm_common.h include/slsm_uapi.h | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) slsm_sn.c slsm_common.c -o $@

clean:
	rm -rf $(BIN_DIR)
