#define _GNU_SOURCE

/*
 * Small MN-side fixture for the SRec DMerge RC-only experiment.
 *
 * It intentionally contains no DMerge implementation.  The producer uses the
 * stable syscall ABI already exposed by the lab heap.ko and publishes one
 * contiguous, deterministic float table.  The CN-side Python harness then
 * maps the same VMA and lets ordinary DLRM lookups fault pages on demand.
 */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

#define DMERGE_DEVICE "/dev/mitosis-syscalls"
#define DMERGE_BASE ((uintptr_t)0x4ffff5a00000ULL)
#define DMERGE_PAGE_SIZE 4096UL

enum dmerge_cmd {
    DMERGE_REGISTER = 0,
};

struct register_req {
    unsigned long long heap_base;
};

static volatile sig_atomic_t keep_running = 1;

static void stop_producer(int signal_number)
{
    (void)signal_number;
    keep_running = 0;
}

static void usage(const char *program)
{
    fprintf(stderr,
            "usage: %s producer ROWS DIM TABLE_ID [BASE_HEX]\n",
            program);
}

static int parse_u64(const char *text, uint64_t *value)
{
    char *end = NULL;
    errno = 0;
    unsigned long long parsed = strtoull(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0')
        return -1;
    *value = (uint64_t)parsed;
    return 0;
}

static float table_value(uint64_t table_id, uint64_t row, uint64_t col,
                         uint64_t dim)
{
    /* Keep values small enough that the DLRM comparison is numerically clear. */
    uint64_t ordinal = (table_id + 1) * UINT64_C(1000000) + row * dim + col;
    return (float)ordinal * 1.0e-6f;
}

int main(int argc, char **argv)
{
    if (argc < 5 || strcmp(argv[1], "producer") != 0 || argc > 6) {
        usage(argv[0]);
        return 2;
    }

    uint64_t rows = 0;
    uint64_t dim = 0;
    uint64_t table_id = 0;
    uint64_t base_u64 = (uint64_t)DMERGE_BASE;
    if (parse_u64(argv[2], &rows) != 0 || parse_u64(argv[3], &dim) != 0
        || parse_u64(argv[4], &table_id) != 0
        || (argc == 6 && parse_u64(argv[5], &base_u64) != 0)) {
        fprintf(stderr, "invalid numeric argument\n");
        return 2;
    }
    if (rows == 0 || dim == 0 || rows > UINT64_MAX / dim
        || rows * dim > SIZE_MAX / sizeof(float)) {
        fprintf(stderr, "table dimensions overflow\n");
        return 2;
    }

    size_t bytes = (size_t)(rows * dim * sizeof(float));
    if (bytes % DMERGE_PAGE_SIZE != 0) {
        fprintf(stderr, "table bytes must be page aligned (got %zu)\n", bytes);
        return 2;
    }

    void *mapping = mmap((void *)(uintptr_t)base_u64, bytes,
                         PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
                         -1, 0);
    if (mapping == MAP_FAILED) {
        fprintf(stderr, "producer mmap failed: %s\n", strerror(errno));
        return 1;
    }

    float *table = (float *)mapping;
    long double checksum = 0.0;
    for (uint64_t row = 0; row < rows; ++row) {
        for (uint64_t col = 0; col < dim; ++col) {
            float value = table_value(table_id, row, col, dim);
            table[row * dim + col] = value;
            checksum += value;
        }
    }

    int fd = open(DMERGE_DEVICE, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "open %s failed: %s\n", DMERGE_DEVICE,
                strerror(errno));
        munmap(mapping, bytes);
        return 1;
    }

    struct register_req request = {
        .heap_base = (unsigned long long)base_u64,
    };
    int heap_hint = ioctl(fd, DMERGE_REGISTER, &request);
    if (heap_hint < 0) {
        fprintf(stderr, "Register ioctl failed: %s\n", strerror(errno));
        close(fd);
        munmap(mapping, bytes);
        return 1;
    }

    signal(SIGINT, stop_producer);
    signal(SIGTERM, stop_producer);
    printf("TABLE_READY base=0x%" PRIx64 " bytes=%zu rows=%" PRIu64
           " dim=%" PRIu64 " table_id=%" PRIu64 " pages=%zu hint=%d"
           " checksum=%.9Lf\n",
           base_u64, bytes, rows, dim, table_id, bytes / DMERGE_PAGE_SIZE,
           heap_hint, checksum);
    fflush(stdout);

    while (keep_running)
        pause();

    close(fd);
    munmap(mapping, bytes);
    return 0;
}
