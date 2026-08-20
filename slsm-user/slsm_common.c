#include "slsm_common.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

#define FNV_OFFSET_BASIS UINT64_C(0xcbf29ce484222325)
#define FNV_PRIME UINT64_C(0x100000001b3)

_Static_assert(sizeof(struct slsm_remote_chunk) == 16, "unexpected chunk ABI");
_Static_assert(sizeof(struct slsm_sst_descriptor) == 192, "unexpected descriptor ABI");
_Static_assert(sizeof(struct slsm_connect_req) == 32, "unexpected connect ABI");
_Static_assert(sizeof(struct slsm_register_req) == 216, "unexpected register ABI");
_Static_assert(sizeof(struct slsm_fetch_req) == 232, "unexpected fetch ABI");
_Static_assert(sizeof(struct slsm_control_message) == 200, "unexpected control ABI");
_Static_assert(sizeof(struct slsm_control_ack) == 32, "unexpected ack ABI");

uint64_t slsm_fnv1a(const void *data, size_t length)
{
    const uint8_t *bytes = data;
    uint64_t hash = FNV_OFFSET_BASIS;
    size_t index;

    for (index = 0; index < length; ++index) {
        hash ^= bytes[index];
        hash *= FNV_PRIME;
    }
    return hash;
}

static uint8_t slsm_pattern_byte(uint64_t offset, uint64_t sst_id)
{
    return (uint8_t)((offset * UINT64_C(131) + (offset >> 7) +
                      sst_id * UINT64_C(17)) & UINT64_C(0xff));
}

void slsm_fill_sst(void *data, size_t length, uint64_t sst_id)
{
    uint8_t *bytes = data;
    size_t index;

    for (index = 0; index < length; ++index)
        bytes[index] = slsm_pattern_byte(index, sst_id);
}

int slsm_check_sst(const void *data, size_t length, uint64_t sst_id)
{
    const uint8_t *bytes = data;
    size_t index;

    for (index = 0; index < length; ++index) {
        if (bytes[index] != slsm_pattern_byte(index, sst_id))
            return -1;
    }
    return 0;
}

int slsm_read_full(int fd, void *buffer, size_t length)
{
    uint8_t *cursor = buffer;
    size_t remaining = length;

    while (remaining > 0) {
        ssize_t count = read(fd, cursor, remaining);
        if (count == 0) {
            errno = ECONNRESET;
            return -1;
        }
        if (count < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        cursor += count;
        remaining -= (size_t)count;
    }
    return 0;
}

int slsm_write_full(int fd, const void *buffer, size_t length)
{
    const uint8_t *cursor = buffer;
    size_t remaining = length;

    while (remaining > 0) {
        ssize_t count = send(fd, cursor, remaining, MSG_NOSIGNAL);
        if (count < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (count == 0) {
            errno = ECONNRESET;
            return -1;
        }
        cursor += count;
        remaining -= (size_t)count;
    }
    return 0;
}

int slsm_parse_u64(const char *text, uint64_t *value)
{
    char *end = NULL;
    unsigned long long parsed;

    errno = 0;
    parsed = strtoull(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0')
        return -1;
    *value = (uint64_t)parsed;
    return 0;
}

int slsm_parse_port(const char *text, uint16_t *port)
{
    uint64_t parsed;

    if (slsm_parse_u64(text, &parsed) != 0 || parsed == 0 || parsed > UINT16_MAX)
        return -1;
    *port = (uint16_t)parsed;
    return 0;
}
