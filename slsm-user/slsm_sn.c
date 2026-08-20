#include "slsm_common.h"
#include "slsm_manifest.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_DEVICE "/dev/slsm"
#define CONNECT_ATTEMPTS 100
#define CONNECT_RETRY_NSEC 100000000L

static void usage(const char *program)
{
    fprintf(stderr, "Usage: %s [--device PATH] [--server IPv4] [--port PORT]\n", program);
}

static int connect_with_retry(const char *address, uint16_t port)
{
    struct sockaddr_in remote = {0};
    struct timespec delay = {.tv_sec = 0, .tv_nsec = CONNECT_RETRY_NSEC};
    int attempt;

    remote.sin_family = AF_INET;
    remote.sin_port = htons(port);
    if (inet_pton(AF_INET, address, &remote.sin_addr) != 1) {
        errno = EINVAL;
        return -1;
    }
    for (attempt = 0; attempt < CONNECT_ATTEMPTS; ++attempt) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
            return -1;
        if (connect(fd, (struct sockaddr *)&remote, sizeof(remote)) == 0)
            return fd;
        close(fd);
        nanosleep(&delay, NULL);
    }
    errno = ETIMEDOUT;
    return -1;
}

int main(int argc, char **argv)
{
    static const struct option options[] = {
        {"device", required_argument, NULL, 'd'},
        {"server", required_argument, NULL, 's'},
        {"port", required_argument, NULL, 'p'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };
    const char *device = DEFAULT_DEVICE;
    const char *server_address = NULL;
    uint16_t port = SLSM_CONTROL_PORT;
    struct slsm_lifecycle_message message = {0};
    struct slsm_lifecycle_message next = {0};
    struct slsm_lifecycle_ack ack = {
        .magic = SLSM_CONTROL_MAGIC,
        .version = SLSM_LIFECYCLE_VERSION,
    };
    struct slsm_connect_req connect_request = {0};
    struct slsm_fetch_req fetch_request = {0};
    struct slsm_manifest manifest;
    const struct slsm_manifest_entry *lookup_entry;
    int manifest_status;
    uint64_t userspace_checksum = 0;
    void *buffer = NULL;
    int control_fd = -1;
    int device_fd = -1;
    int connected = 0;
    int result = EXIT_FAILURE;
    int option;

    slsm_manifest_init(&manifest);

    while ((option = getopt_long(argc, argv, "d:s:p:h", options, NULL)) != -1) {
        switch (option) {
        case 'd': device = optarg; break;
        case 's': server_address = optarg; break;
        case 'p':
            if (slsm_parse_port(optarg, &port) != 0) {
                usage(argv[0]);
                return EXIT_FAILURE;
            }
            break;
        case 'h': usage(argv[0]); return EXIT_SUCCESS;
        default: usage(argv[0]); return EXIT_FAILURE;
        }
    }

    if (server_address == NULL) {
        fprintf(stderr, "--server is required\n");
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    control_fd = connect_with_retry(server_address, port);
    if (control_fd < 0) {
        perror("connect to CN control plane");
        goto out;
    }
    if (slsm_read_full(control_fd, &message, sizeof(message)) != 0) {
        perror("read PUBLISH");
        goto send_ack;
    }
    ack.phase = SLSM_PHASE_PUBLISH;
    ack.generation = message.generation;
    if (message.magic != SLSM_CONTROL_MAGIC ||
        message.version != SLSM_LIFECYCLE_VERSION ||
        message.phase != SLSM_PHASE_PUBLISH ||
        message.descriptor.version != SLSM_ABI_VERSION ||
        message.descriptor.total_length == 0 ||
        message.descriptor.total_length > SLSM_MAX_SST_SIZE ||
        message.metadata.version != SLSM_METADATA_VERSION ||
        message.metadata.level > SLSM_MAX_SST_LEVEL ||
        message.metadata.epoch == 0 ||
        message.metadata.min_key > message.metadata.max_key) {
        fprintf(stderr, "invalid PUBLISH from CN\n");
        ack.status = -EINVAL;
        goto send_ack;
    }
    if (posix_memalign(&buffer, 4096, (size_t)message.descriptor.total_length) != 0) {
        ack.status = -ENOMEM;
        goto send_ack;
    }
    memset(buffer, 0, (size_t)message.descriptor.total_length);

    device_fd = open(device, O_RDWR | O_CLOEXEC);
    if (device_fd < 0) {
        perror("open /dev/slsm");
        ack.status = -errno;
        goto send_ack;
    }
    connect_request.version = SLSM_ABI_VERSION;
    memcpy(connect_request.peer_gid, message.descriptor.owner_gid,
           sizeof(connect_request.peer_gid));
    connect_request.service_id = message.descriptor.service_id;
    if (ioctl(device_fd, SLSM_IOCTL_CONNECT_PEER, &connect_request) != 0) {
        perror("SLSM_IOCTL_CONNECT_PEER");
        ack.status = -errno;
        goto send_ack;
    }
    connected = 1;

    fetch_request.user_addr = (uintptr_t)buffer;
    fetch_request.capacity = message.descriptor.total_length;
    fetch_request.descriptor = message.descriptor;
    if (ioctl(device_fd, SLSM_IOCTL_FETCH_REGION, &fetch_request) != 0) {
        perror("SLSM_IOCTL_FETCH_REGION");
        ack.phase = SLSM_PHASE_FETCH;
        ack.generation = message.generation;
        ack.status = -errno;
        ack.fetched_length = fetch_request.fetched_length;
        ack.checksum = fetch_request.checksum;
        ack.elapsed_us = fetch_request.elapsed_us;
        goto disconnect_send_ack;
    }
    userspace_checksum = slsm_fnv1a(buffer, (size_t)fetch_request.fetched_length);
    if (fetch_request.fetched_length != message.descriptor.total_length ||
        fetch_request.checksum != message.descriptor.checksum ||
        userspace_checksum != message.descriptor.checksum ||
        slsm_check_sst(buffer, (size_t)fetch_request.fetched_length,
                       message.descriptor.sst_id) != 0) {
        fprintf(stderr, "SST validation failed after RDMA READ\n");
        ack.phase = SLSM_PHASE_FETCH;
        ack.generation = message.generation;
        ack.status = -EBADMSG;
        goto disconnect_send_ack;
    }
    manifest_status = slsm_manifest_publish(&manifest, &message.metadata,
                                            &message.descriptor);
    if (manifest_status != 0) {
        ack.phase = SLSM_PHASE_FETCH;
        ack.generation = message.generation;
        ack.status = manifest_status;
        goto disconnect_send_ack;
    }
    ack.phase = SLSM_PHASE_FETCH;
    ack.generation = message.generation;
    ack.status = 0;
    ack.fetched_length = fetch_request.fetched_length;
    ack.checksum = userspace_checksum;
    ack.elapsed_us = fetch_request.elapsed_us;
    ack.manifest_version = manifest.version;
    ack.lookup_sst_id = 0;
    if (slsm_write_full(control_fd, &ack, sizeof(ack)) != 0) {
        perror("write FETCH ack");
        goto disconnect;
    }

    if (slsm_read_full(control_fd, &next, sizeof(next)) != 0) {
        perror("read COMMIT");
        goto disconnect;
    }
    ack = (struct slsm_lifecycle_ack){
        .magic = SLSM_CONTROL_MAGIC,
        .version = SLSM_LIFECYCLE_VERSION,
        .phase = SLSM_PHASE_COMMIT,
        .generation = message.generation,
    };
    if (next.magic != SLSM_CONTROL_MAGIC ||
        next.version != SLSM_LIFECYCLE_VERSION ||
        next.phase != SLSM_PHASE_COMMIT ||
        next.generation != message.generation ||
        memcmp(&next.descriptor, &message.descriptor, sizeof(next.descriptor)) != 0 ||
        memcmp(&next.metadata, &message.metadata, sizeof(next.metadata)) != 0) {
        fprintf(stderr, "invalid COMMIT for generation=%" PRIu64 "\n", message.generation);
        ack.status = -EPROTO;
        if (slsm_write_full(control_fd, &ack, sizeof(ack)) != 0)
            result = EXIT_FAILURE;
        goto disconnect;
    }
    manifest_status = slsm_manifest_commit(&manifest, message.descriptor.sst_id,
                                           message.metadata.epoch);
    lookup_entry = slsm_manifest_lookup(&manifest, message.metadata.min_key,
                                        message.metadata.epoch);
    if (manifest_status != 0 || lookup_entry == NULL ||
        lookup_entry->descriptor.sst_id != message.descriptor.sst_id) {
        ack.status = manifest_status != 0 ? manifest_status : -EBADMSG;
    } else {
        ack.status = 0;
        ack.manifest_version = manifest.version;
        ack.lookup_sst_id = lookup_entry->descriptor.sst_id;
    }
    if (slsm_write_full(control_fd, &ack, sizeof(ack)) != 0) {
        perror("write COMMIT ack");
        goto disconnect;
    }

    if (slsm_read_full(control_fd, &next, sizeof(next)) != 0) {
        perror("read REVOKE");
        goto disconnect;
    }
    ack = (struct slsm_lifecycle_ack){
        .magic = SLSM_CONTROL_MAGIC,
        .version = SLSM_LIFECYCLE_VERSION,
        .phase = SLSM_PHASE_REVOKE,
        .generation = message.generation,
    };
    if (next.magic != SLSM_CONTROL_MAGIC ||
        next.version != SLSM_LIFECYCLE_VERSION ||
        next.phase != SLSM_PHASE_REVOKE ||
        next.generation != message.generation ||
        memcmp(&next.descriptor, &message.descriptor, sizeof(next.descriptor)) != 0 ||
        memcmp(&next.metadata, &message.metadata, sizeof(next.metadata)) != 0) {
        fprintf(stderr, "invalid REVOKE for generation=%" PRIu64 "\n", message.generation);
        ack.status = -EPROTO;
        if (slsm_write_full(control_fd, &ack, sizeof(ack)) != 0)
            result = EXIT_FAILURE;
        goto disconnect;
    }
    manifest_status = slsm_manifest_revoke(&manifest, message.descriptor.sst_id);
    lookup_entry = slsm_manifest_lookup(&manifest, message.metadata.min_key,
                                        message.metadata.epoch);
    if (manifest_status != 0 ||
        (lookup_entry != NULL &&
         lookup_entry->descriptor.sst_id == message.descriptor.sst_id)) {
        ack.status = manifest_status != 0 ? manifest_status : -EBUSY;
    } else {
        ack.status = 0;
        ack.manifest_version = manifest.version;
        ack.lookup_sst_id = lookup_entry == NULL ? 0 : lookup_entry->descriptor.sst_id;
    }
    if (slsm_write_full(control_fd, &ack, sizeof(ack)) != 0) {
        perror("write REVOKE ack");
        goto disconnect;
    }
    result = EXIT_SUCCESS;

disconnect:
    if (connected && ioctl(device_fd, SLSM_IOCTL_DISCONNECT, 0) != 0) {
        perror("SLSM_IOCTL_DISCONNECT");
        ack.status = -errno;
        result = EXIT_FAILURE;
    }
    goto print_result;

disconnect_send_ack:
    if (connected && ioctl(device_fd, SLSM_IOCTL_DISCONNECT, 0) != 0) {
        perror("SLSM_IOCTL_DISCONNECT");
        result = EXIT_FAILURE;
    }
send_ack:
    if (control_fd >= 0 && slsm_write_full(control_fd, &ack, sizeof(ack)) != 0) {
        perror("write result ack");
        result = EXIT_FAILURE;
    }
print_result:
    if (result == EXIT_SUCCESS) {
        printf("{\"event\":\"stage3_result\",\"role\":\"sn\",\"status\":\"pass\","
               "\"lifecycle\":\"publish-fetch-commit-revoke\",\"generation\":%" PRIu64
               ",\"sst_id\":%" PRIu64 ",\"bytes\":%" PRIu64 ",\"chunks\":%u,"
               "\"checksum\":\"0x%016" PRIx64 "\",\"elapsed_us\":%" PRIu64
               ",\"manifest_version\":%" PRIu64 "}\n",
               message.generation,
               message.descriptor.sst_id, fetch_request.fetched_length,
               message.descriptor.chunk_count, userspace_checksum, fetch_request.elapsed_us,
               manifest.version);
    }
out:
    if (device_fd >= 0)
        close(device_fd);
    if (control_fd >= 0)
        close(control_fd);
    free(buffer);
    return result;
}
