#include "slsm_common.h"

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
    struct slsm_control_message message = {0};
    struct slsm_control_ack ack = {.magic = SLSM_CONTROL_MAGIC};
    struct slsm_connect_req connect_request = {0};
    struct slsm_fetch_req fetch_request = {0};
    uint64_t userspace_checksum = 0;
    void *buffer = NULL;
    int control_fd = -1;
    int device_fd = -1;
    int connected = 0;
    int result = EXIT_FAILURE;
    int option;

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
        perror("read descriptor");
        goto send_ack;
    }
    if (message.magic != SLSM_CONTROL_MAGIC || message.version != SLSM_ABI_VERSION ||
        message.descriptor.version != SLSM_ABI_VERSION ||
        message.descriptor.total_length == 0 ||
        message.descriptor.total_length > SLSM_MAX_SST_SIZE) {
        fprintf(stderr, "invalid descriptor from CN\n");
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
        ack.status = -errno;
        ack.fetched_length = fetch_request.fetched_length;
        ack.checksum = fetch_request.checksum;
        ack.elapsed_us = fetch_request.elapsed_us;
        goto disconnect;
    }
    userspace_checksum = slsm_fnv1a(buffer, (size_t)fetch_request.fetched_length);
    if (fetch_request.fetched_length != message.descriptor.total_length ||
        fetch_request.checksum != message.descriptor.checksum ||
        userspace_checksum != message.descriptor.checksum ||
        slsm_check_sst(buffer, (size_t)fetch_request.fetched_length,
                       message.descriptor.sst_id) != 0) {
        fprintf(stderr, "SST validation failed after RDMA READ\n");
        ack.status = -EBADMSG;
        goto disconnect;
    }
    ack.status = 0;
    ack.fetched_length = fetch_request.fetched_length;
    ack.checksum = userspace_checksum;
    ack.elapsed_us = fetch_request.elapsed_us;
    result = EXIT_SUCCESS;

disconnect:
    if (connected && ioctl(device_fd, SLSM_IOCTL_DISCONNECT, 0) != 0) {
        perror("SLSM_IOCTL_DISCONNECT");
        ack.status = -errno;
        result = EXIT_FAILURE;
    }
send_ack:
    if (control_fd >= 0 && slsm_write_full(control_fd, &ack, sizeof(ack)) != 0) {
        perror("write result ack");
        result = EXIT_FAILURE;
    }
    if (result == EXIT_SUCCESS) {
        printf("{\"event\":\"stage1_result\",\"role\":\"sn\",\"status\":\"pass\","
               "\"sst_id\":%" PRIu64 ",\"bytes\":%" PRIu64 ",\"chunks\":%u,"
               "\"checksum\":\"0x%016" PRIx64 "\",\"elapsed_us\":%" PRIu64 "}\n",
               message.descriptor.sst_id, fetch_request.fetched_length,
               message.descriptor.chunk_count, userspace_checksum, fetch_request.elapsed_us);
    }
out:
    if (device_fd >= 0)
        close(device_fd);
    if (control_fd >= 0)
        close(control_fd);
    free(buffer);
    return result;
}
