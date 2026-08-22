CC ?= gcc
CFLAGS ?= -O2 -g -std=c11 -Wall -Wextra -Werror -D_POSIX_C_SOURCE=200809L
CPPFLAGS ?= -I.
BIN_DIR ?= bin

.PHONY: all clean test

all: $(BIN_DIR)/slsm_cn $(BIN_DIR)/slsm_sn $(BIN_DIR)/slsm_manifest_test \
	$(BIN_DIR)/slsm_sst_test $(BIN_DIR)/slsm_common_test \
	$(BIN_DIR)/slsm_nova_sst_test $(BIN_DIR)/slsm_compaction_test

$(BIN_DIR):
	mkdir -p $@

$(BIN_DIR)/slsm_cn: slsm_cn.c slsm_compaction.c slsm_compaction.h slsm_sst.c slsm_sst.h slsm_nova_sst.c slsm_nova_sst.h slsm_common.c slsm_common.h include/slsm_uapi.h | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) slsm_cn.c slsm_compaction.c slsm_sst.c slsm_nova_sst.c slsm_common.c -o $@

$(BIN_DIR)/slsm_sn: slsm_sn.c slsm_compaction.c slsm_compaction.h slsm_sst.c slsm_sst.h slsm_nova_sst.c slsm_nova_sst.h slsm_manifest.c slsm_manifest.h slsm_common.c slsm_common.h include/slsm_uapi.h | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) slsm_sn.c slsm_compaction.c slsm_sst.c slsm_nova_sst.c slsm_manifest.c slsm_common.c -o $@

$(BIN_DIR)/slsm_manifest_test: slsm_manifest_test.c slsm_manifest.c slsm_manifest.h include/slsm_uapi.h | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) slsm_manifest_test.c slsm_manifest.c -o $@

test: all
	$(BIN_DIR)/slsm_manifest_test
	$(BIN_DIR)/slsm_sst_test
	$(BIN_DIR)/slsm_common_test
	$(BIN_DIR)/slsm_nova_sst_test
	$(BIN_DIR)/slsm_compaction_test

$(BIN_DIR)/slsm_compaction_test: slsm_compaction_test.c slsm_compaction.c slsm_compaction.h slsm_sst.c slsm_sst.h slsm_nova_sst.c slsm_nova_sst.h include/slsm_uapi.h | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) slsm_compaction_test.c slsm_compaction.c slsm_sst.c slsm_nova_sst.c -o $@

$(BIN_DIR)/slsm_nova_sst_test: slsm_nova_sst_test.c slsm_nova_sst.c slsm_nova_sst.h | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) slsm_nova_sst_test.c slsm_nova_sst.c -o $@

$(BIN_DIR)/slsm_common_test: slsm_common_test.c slsm_common.c slsm_common.h include/slsm_uapi.h | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) slsm_common_test.c slsm_common.c -o $@

$(BIN_DIR)/slsm_sst_test: slsm_sst_test.c slsm_sst.c slsm_sst.h slsm_nova_sst.c slsm_nova_sst.h include/slsm_uapi.h | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) slsm_sst_test.c slsm_sst.c slsm_nova_sst.c -o $@

clean:
	rm -rf $(BIN_DIR)
