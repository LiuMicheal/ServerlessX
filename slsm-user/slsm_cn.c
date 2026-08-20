#include "slsm_common.h"

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
#include <unistd.h>

#define DEFAULT_DEVICE "/dev/slsm"
#define CONTROL_TIMEOUT_MS 60000

static void usage(const char *program)
{
    fprintf(stderr,
            "Usage: %s [--device PATH] [--bind IPv4] [--port PORT] "
            "[--size BYTES] [--sst-id ID] [--epoch E] [--level L]\n",
            program);
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
        {"size", required_argument, NULL, 's'},
        {"sst-id", required_argument, NULL, 'i'},
        {"epoch", required_argument, NULL, 'e'},
        {"level", required_argument, NULL, 'l'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };
    const char *device = DEFAULT_DEVICE;
    const char *bind_address = NULL;
    uint16_t port = SLSM_CONTROL_PORT;
    uint64_t size = SLSM_MAX_SST_SIZE;
    uint64_t sst_id = 1;
    uint64_t epoch = 1;
    uint64_t level = 0;
    struct slsm_register_req request = {0};
    struct slsm_lifecycle_message message = {0};
    struct slsm_lifecycle_ack ack = {0};
    struct pollfd poll_fd;
    uint64_t userspace_checksum;
    uint64_t fetch_elapsed_us = 0;
    uint64_t commit_manifest_version = 0;
    void *buffer = NULL;
    int device_fd = -1;
    int listener_fd = -1;
    int client_fd = -1;
    int result = EXIT_FAILURE;
    int option;

    while ((option = getopt_long(argc, argv, "d:b:p:s:i:e:l:h", options, NULL)) != -1) {
        switch (option) {
        case 'd': device = optarg; break;
        case 'b': bind_address = optarg; break;
        case 'p':
            if (slsm_parse_port(optarg, &port) != 0) {
                usage(argv[0]);
                return EXIT_FAILURE;
            }
            break;
        case 's':
            if (slsm_parse_u64(optarg, &size) != 0 || size == 0 || size > SLSM_MAX_SST_SIZE) {
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

    if (posix_memalign(&buffer, 4096, (size_t)size) != 0) {
        fprintf(stderr, "failed to allocate SST buffer\n");
        goto out;
    }
    slsm_fill_sst(buffer, (size_t)size, sst_id);
    userspace_checksum = slsm_fnv1a(buffer, (size_t)size);

    if (sst_id > (UINT64_MAX - UINT64_C(999)) / UINT64_C(1000)) {
        fprintf(stderr, "sst-id is too large for the default key range\n");
        goto out;
    }

    device_fd = open(device, O_RDWR | O_CLOEXEC);
    if (device_fd < 0) {
        perror("open /dev/slsm");
        goto out;
    }
    request.user_addr = (uintptr_t)buffer;
    request.length = size;
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
           sst_id, size, request.descriptor.chunk_count, request.descriptor.checksum,
           bind_address, port);
    fflush(stdout);

    poll_fd.fd = listener_fd;
    poll_fd.events = POLLIN;
    if (poll(&poll_fd, 1, CONTROL_TIMEOUT_MS) <= 0) {
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
    message.generation = 1;
    message.descriptor = request.descriptor;
    message.metadata = (struct slsm_sst_metadata){
        .version = SLSM_METADATA_VERSION,
        .level = (uint16_t)level,
        .epoch = epoch,
        .min_key = sst_id * UINT64_C(1000),
        .max_key = sst_id * UINT64_C(1000) + UINT64_C(999),
    };
    if (slsm_write_full(client_fd, &message, sizeof(message)) != 0 ||
        slsm_read_full(client_fd, &ack, sizeof(ack)) != 0) {
        perror("publish exchange");
        goto unregister;
    }
    if (ack.magic != SLSM_CONTROL_MAGIC || ack.version != SLSM_LIFECYCLE_VERSION ||
        ack.phase != SLSM_PHASE_FETCH || ack.generation != message.generation ||
        ack.status != 0 ||
        ack.fetched_length != size || ack.checksum != userspace_checksum) {
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

    printf("{\"event\":\"stage3_result\",\"role\":\"cn\",\"status\":\"pass\","
           "\"lifecycle\":\"publish-fetch-commit-revoke\",\"generation\":%" PRIu64
           ",\"sst_id\":%" PRIu64 ",\"bytes\":%" PRIu64 ",\"chunks\":%u,"
           "\"checksum\":\"0x%016" PRIx64 "\",\"sn_elapsed_us\":%" PRIu64
           ",\"level\":%u,\"epoch\":%" PRIu64
           ",\"key_range\":[%" PRIu64 ",%" PRIu64 "]"
           ",\"manifest_version\":%" PRIu64 "}\n",
           message.generation, sst_id, size, request.descriptor.chunk_count,
           userspace_checksum, fetch_elapsed_us, message.metadata.level,
           message.metadata.epoch, message.metadata.min_key, message.metadata.max_key,
           ack.manifest_version);
    result = EXIT_SUCCESS;

unregister:
    if (ioctl(device_fd, SLSM_IOCTL_UNREGISTER_REGION, 0) != 0) {
        perror("SLSM_IOCTL_UNREGISTER_REGION");
        result = EXIT_FAILURE;
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
