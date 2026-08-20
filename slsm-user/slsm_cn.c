#include "slsm_common.h"
#include "slsm_sst.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_DEVICE "/dev/slsm"
static void usage(const char *program)
{
    fprintf(stderr,
            "Usage: %s [--device PATH] [--bind IPv4] [--port PORT] "
            "[--records COUNT] [--sst-id ID] [--epoch E] [--level L]\n",
            program);
}

static uint64_t make_generation(uint64_t sst_id)
{
    struct timespec now;
    uint64_t generation;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        now = (struct timespec){.tv_sec = 0, .tv_nsec = 0};
    generation = ((uint64_t)now.tv_sec << 32) ^ (uint64_t)now.tv_nsec ^
                 ((uint64_t)getpid() << 16) ^ sst_id;
    return generation == 0 ? UINT64_C(1) : generation;
}

static int create_listener(const char *address, uint16_t port)
{
    struct sockaddr_in local = {0};
    int fd;
    int one = 1;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) != 0)
        goto fail;
    local.sin_family = AF_INET;
    local.sin_port = htons(port);
    if (inet_pton(AF_INET, address, &local.sin_addr) != 1) {
        errno = EINVAL;
        goto fail;
    }
    if (bind(fd, (struct sockaddr *)&local, sizeof(local)) != 0 || listen(fd, 1) != 0)
        goto fail;
    return fd;

fail:
    close(fd);
    return -1;
}

int main(int argc, char **argv)
{
    static const struct option options[] = {
        {"device", required_argument, NULL, 'd'},
        {"bind", required_argument, NULL, 'b'},
        {"port", required_argument, NULL, 'p'},
        {"records", required_argument, NULL, 'r'},
        {"sst-id", required_argument, NULL, 'i'},
        {"epoch", required_argument, NULL, 'e'},
        {"level", required_argument, NULL, 'l'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };
    const char *device = DEFAULT_DEVICE;
    const char *bind_address = NULL;
    uint16_t port = SLSM_CONTROL_PORT;
    uint64_t records = 8;
    uint64_t sst_id = 1;
    uint64_t epoch = 1;
    uint64_t level = 0;
    struct slsm_register_req request = {0};
    struct slsm_memtable memtable;
    struct slsm_sst_metadata metadata;
    struct slsm_lifecycle_message message = {0};
    struct slsm_lifecycle_ack ack = {0};
    struct pollfd poll_fd;
    uint64_t userspace_checksum;
    uint64_t fetch_elapsed_us = 0;
    uint64_t commit_manifest_version = 0;
    size_t encoded_length = 0;
    size_t record_index;
    uint64_t key_base;
    void *buffer = NULL;
    int device_fd = -1;
    int listener_fd = -1;
    int client_fd = -1;
    int result = EXIT_FAILURE;
    int option;

    while ((option = getopt_long(argc, argv, "d:b:p:r:i:e:l:h", options, NULL)) != -1) {
        switch (option) {
        case 'd': device = optarg; break;
        case 'b': bind_address = optarg; break;
        case 'p':
            if (slsm_parse_port(optarg, &port) != 0) {
                usage(argv[0]);
                return EXIT_FAILURE;
            }
            break;
        case 'r':
            if (slsm_parse_u64(optarg, &records) != 0 || records == 0 ||
                records > SLSM_SST_MAX_RECORDS) {
                usage(argv[0]);
                return EXIT_FAILURE;
            }
            break;
        case 'i':
            if (slsm_parse_u64(optarg, &sst_id) != 0) {
                usage(argv[0]);
                return EXIT_FAILURE;
            }
            break;
        case 'e':
            if (slsm_parse_u64(optarg, &epoch) != 0 || epoch == 0) {
                usage(argv[0]);
                return EXIT_FAILURE;
            }
            break;
        case 'l':
            if (slsm_parse_u64(optarg, &level) != 0 || level > SLSM_MAX_SST_LEVEL) {
                usage(argv[0]);
                return EXIT_FAILURE;
            }
            break;
        case 'h': usage(argv[0]); return EXIT_SUCCESS;
        default: usage(argv[0]); return EXIT_FAILURE;
        }
    }

    if (bind_address == NULL) {
        fprintf(stderr, "--bind is required\n");
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (sst_id > (UINT64_MAX - UINT64_C(7) - (records - 1)) / UINT64_C(1000)) {
        fprintf(stderr, "sst-id is too large for the default key range\n");
        goto out;
    }
    key_base = sst_id * UINT64_C(1000);
    slsm_memtable_init(&memtable);
    for (record_index = 0; record_index < records; ++record_index) {
        uint64_t key = key_base + record_index;

        if (slsm_memtable_put(&memtable, key, key + UINT64_C(7)) != 0) {
            fprintf(stderr, "failed to populate MemTable\n");
            goto out;
        }
    }
    if (posix_memalign(&buffer, 4096, SLSM_SST_MAX_SIZE) != 0) {
        fprintf(stderr, "failed to allocate SST buffer\n");
        goto out;
    }
    if (slsm_sst_flush(&memtable, sst_id, epoch, buffer, SLSM_SST_MAX_SIZE,
                       &encoded_length, &metadata) != 0) {
        fprintf(stderr, "failed to flush MemTable to SST\n");
        goto out;
    }
    metadata.level = (uint16_t)level;
    userspace_checksum = slsm_fnv1a(buffer, encoded_length);

    device_fd = open(device, O_RDWR | O_CLOEXEC);
    if (device_fd < 0) {
        perror("open /dev/slsm");
        goto out;
    }
    request.user_addr = (uintptr_t)buffer;
    request.length = encoded_length;
    request.sst_id = sst_id;
    if (ioctl(device_fd, SLSM_IOCTL_REGISTER_REGION, &request) != 0) {
        perror("SLSM_IOCTL_REGISTER_REGION");
        goto out;
    }
    if (request.descriptor.checksum != userspace_checksum) {
        fprintf(stderr, "kernel/userspace checksum disagreement\n");
        goto unregister;
    }

    listener_fd = create_listener(bind_address, port);
    if (listener_fd < 0) {
        perror("control listener");
        goto unregister;
    }
    printf("{\"event\":\"cn_ready\",\"status\":\"ok\",\"sst_id\":%" PRIu64
           ",\"bytes\":%" PRIu64 ",\"chunks\":%u,\"checksum\":\"0x%016" PRIx64
           "\",\"bind\":\"%s\",\"port\":%u}\n",
           sst_id, encoded_length, request.descriptor.chunk_count, request.descriptor.checksum,
           bind_address, port);
    fflush(stdout);

    poll_fd.fd = listener_fd;
    poll_fd.events = POLLIN;
    if (poll(&poll_fd, 1, SLSM_CONTROL_TIMEOUT_MS) <= 0) {
        if (errno == 0)
            errno = ETIMEDOUT;
        perror("waiting for SN");
        goto unregister;
    }
    client_fd = accept(listener_fd, NULL, NULL);
    if (client_fd < 0) {
        perror("accept");
        goto unregister;
    }
    message.magic = SLSM_CONTROL_MAGIC;
    message.version = SLSM_LIFECYCLE_VERSION;
    message.phase = SLSM_PHASE_PUBLISH;
    message.generation = make_generation(sst_id);
    message.descriptor = request.descriptor;
    message.metadata = metadata;
    if (slsm_write_full(client_fd, &message, sizeof(message)) != 0 ||
        slsm_read_full(client_fd, &ack, sizeof(ack)) != 0) {
        perror("publish exchange");
        goto unregister;
    }
    if (ack.magic != SLSM_CONTROL_MAGIC || ack.version != SLSM_LIFECYCLE_VERSION ||
        ack.phase != SLSM_PHASE_FETCH || ack.generation != message.generation ||
        ack.status != 0 ||
        ack.fetched_length != encoded_length || ack.checksum != userspace_checksum) {
        fprintf(stderr,
                "SN rejected FETCH: status=%d bytes=%" PRIu64 " checksum=0x%016" PRIx64 "\n",
                ack.status, ack.fetched_length, ack.checksum);
        goto unregister;
    }
    fetch_elapsed_us = ack.elapsed_us;

    message.phase = SLSM_PHASE_COMMIT;
    ack = (struct slsm_lifecycle_ack){0};
    if (slsm_write_full(client_fd, &message, sizeof(message)) != 0 ||
        slsm_read_full(client_fd, &ack, sizeof(ack)) != 0) {
        perror("commit exchange");
        goto unregister;
    }
    if (ack.magic != SLSM_CONTROL_MAGIC || ack.version != SLSM_LIFECYCLE_VERSION ||
        ack.phase != SLSM_PHASE_COMMIT || ack.generation != message.generation ||
        ack.status != 0 || ack.manifest_version == 0 || ack.lookup_sst_id != sst_id) {
        fprintf(stderr, "SN rejected COMMIT: status=%d generation=%" PRIu64 "\n",
                ack.status, ack.generation);
        goto unregister;
    }
    commit_manifest_version = ack.manifest_version;

    message.phase = SLSM_PHASE_REVOKE;
    ack = (struct slsm_lifecycle_ack){0};
    if (slsm_write_full(client_fd, &message, sizeof(message)) != 0 ||
        slsm_read_full(client_fd, &ack, sizeof(ack)) != 0) {
        perror("revoke exchange");
        goto unregister;
    }
    if (ack.magic != SLSM_CONTROL_MAGIC || ack.version != SLSM_LIFECYCLE_VERSION ||
        ack.phase != SLSM_PHASE_REVOKE || ack.generation != message.generation ||
        ack.status != 0 || ack.manifest_version <= commit_manifest_version ||
        ack.lookup_sst_id == sst_id) {
        fprintf(stderr, "SN rejected REVOKE: status=%d generation=%" PRIu64 "\n",
                ack.status, ack.generation);
        goto unregister;
    }

    result = EXIT_SUCCESS;

unregister:
    if (ioctl(device_fd, SLSM_IOCTL_UNREGISTER_REGION, 0) != 0) {
        perror("SLSM_IOCTL_UNREGISTER_REGION");
        result = EXIT_FAILURE;
    }
    if (result == EXIT_SUCCESS) {
        printf("{\"event\":\"stage4_result\",\"role\":\"cn\",\"status\":\"pass\","
               "\"lifecycle\":\"publish-fetch-commit-revoke\",\"generation\":%" PRIu64
               ",\"sst_id\":%" PRIu64 ",\"bytes\":%" PRIu64 ",\"chunks\":%u,"
               "\"checksum\":\"0x%016" PRIx64 "\",\"sn_elapsed_us\":%" PRIu64
               ",\"level\":%u,\"epoch\":%" PRIu64
               ",\"key_range\":[%" PRIu64 ",%" PRIu64 "]"
               ",\"records\":%u,\"manifest_version\":%" PRIu64 "}\n",
               message.generation, sst_id, encoded_length, request.descriptor.chunk_count,
               userspace_checksum, fetch_elapsed_us, message.metadata.level,
               message.metadata.epoch, message.metadata.min_key, message.metadata.max_key,
               (unsigned)memtable.count, ack.manifest_version);
    }
out:
    if (client_fd >= 0)
        close(client_fd);
    if (listener_fd >= 0)
        close(listener_fd);
    if (device_fd >= 0)
        close(device_fd);
    free(buffer);
    return result;
}
