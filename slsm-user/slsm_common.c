#include "slsm_common.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <time.h>
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
_Static_assert(sizeof(struct slsm_sst_metadata) == 32, "unexpected metadata ABI");
_Static_assert(sizeof(struct slsm_lifecycle_message) == 248, "unexpected lifecycle message ABI");
_Static_assert(sizeof(struct slsm_lifecycle_ack) == 64, "unexpected lifecycle ack ABI");

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

/* Keep one absolute deadline across all fragments of a control frame. */
static int deadline_after_ms(struct timespec *deadline, int timeout_ms)
{
    if (deadline == NULL || timeout_ms <= 0) {
        errno = EINVAL;
        return -1;
    }
    if (clock_gettime(CLOCK_MONOTONIC, deadline) != 0)
        return -1;
    deadline->tv_sec += timeout_ms / 1000;
    deadline->tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (deadline->tv_nsec >= 1000000000L) {
        ++deadline->tv_sec;
        deadline->tv_nsec -= 1000000000L;
    }
    return 0;
}

static int wait_for_socket(int fd, short events, const struct timespec *deadline)
{
    struct pollfd poll_fd = {.fd = fd, .events = events};

    for (;;) {
        struct timespec now;
        time_t seconds;
        long nanoseconds;
        int timeout_ms;
        int status;

        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
            return -1;
        seconds = deadline->tv_sec - now.tv_sec;
        nanoseconds = deadline->tv_nsec - now.tv_nsec;
        if (nanoseconds < 0) {
            --seconds;
            nanoseconds += 1000000000L;
        }
        if (seconds < 0 || (seconds == 0 && nanoseconds == 0)) {
            errno = ETIMEDOUT;
            return -1;
        }
        if (seconds >= INT_MAX / 1000)
            timeout_ms = INT_MAX;
        else
            timeout_ms = (int)(seconds * 1000 +
                               (nanoseconds + 999999L) / 1000000L);

        status = poll(&poll_fd, 1, timeout_ms);
        if (status < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (status == 0) {
            errno = ETIMEDOUT;
            return -1;
        }
        if (poll_fd.revents & POLLNVAL) {
            errno = EBADF;
            return -1;
        }
        if (poll_fd.revents & (events | POLLERR | POLLHUP))
            return 0;
    }
}

int slsm_read_full_timeout(int fd, void *buffer, size_t length, int timeout_ms)
{
    uint8_t *cursor = buffer;
    size_t remaining = length;
    struct timespec deadline;

    if ((buffer == NULL && length != 0) ||
        deadline_after_ms(&deadline, timeout_ms) != 0)
        return -1;

    while (remaining > 0) {
        ssize_t count;

        if (wait_for_socket(fd, POLLIN, &deadline) != 0)
            return -1;
        count = recv(fd, cursor, remaining, MSG_DONTWAIT);
        if (count == 0) {
            errno = ECONNRESET;
            return -1;
        }
        if (count < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            return -1;
        }
        cursor += count;
        remaining -= (size_t)count;
    }
    return 0;
}

int slsm_write_full_timeout(int fd, const void *buffer, size_t length,
                            int timeout_ms)
{
    const uint8_t *cursor = buffer;
    size_t remaining = length;
    struct timespec deadline;

    if ((buffer == NULL && length != 0) ||
        deadline_after_ms(&deadline, timeout_ms) != 0)
        return -1;

    while (remaining > 0) {
        ssize_t count;

        if (wait_for_socket(fd, POLLOUT, &deadline) != 0)
            return -1;
        count = send(fd, cursor, remaining, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (count < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
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

int slsm_read_full(int fd, void *buffer, size_t length)
{
    return slsm_read_full_timeout(fd, buffer, length, SLSM_CONTROL_TIMEOUT_MS);
}

int slsm_write_full(int fd, const void *buffer, size_t length)
{
    return slsm_write_full_timeout(fd, buffer, length, SLSM_CONTROL_TIMEOUT_MS);
}

int slsm_parse_u64(const char *text, uint64_t *value)
{
    char *end = NULL;
    unsigned long long parsed;

    if (text == NULL || text[0] == '-' || text[0] == '+')
        return -1;
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
