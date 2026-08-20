#ifndef SLSM_COMMON_H
#define SLSM_COMMON_H

#include <stddef.h>
#include <stdint.h>

#include "include/slsm_uapi.h"

uint64_t slsm_fnv1a(const void *data, size_t length);
void slsm_fill_sst(void *data, size_t length, uint64_t sst_id);
int slsm_check_sst(const void *data, size_t length, uint64_t sst_id);
int slsm_read_full(int fd, void *buffer, size_t length);
int slsm_write_full(int fd, const void *buffer, size_t length);
int slsm_parse_u64(const char *text, uint64_t *value);
int slsm_parse_port(const char *text, uint16_t *port);

#endif
