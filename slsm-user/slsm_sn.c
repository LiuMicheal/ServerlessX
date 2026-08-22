#include "slsm_common.h"
#include "slsm_compaction.h"
#include "slsm_manifest.h"
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
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_DEVICE "/dev/slsm"
#define CONNECT_ATTEMPTS 100
#define CONNECT_ATTEMPT_TIMEOUT_MS 100
#define CONNECT_RETRY_NSEC 100000000L

static void usage(const char *program)
{
    fprintf(stderr,
            "Usage: %s [--device PATH] [--server IPv4] [--port PORT] "
            "[--on-demand] [--compaction]\n",
            program);
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
        struct pollfd poll_fd = {0};
        int flags;
        int fd = socket(AF_INET, SOCK_STREAM, 0);

        if (fd < 0)
            return -1;
        flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            close(fd);
            return -1;
        }
        if (connect(fd, (struct sockaddr *)&remote, sizeof(remote)) == 0)
            return fd;
        if (errno == EINPROGRESS) {
            int socket_error = 0;
            socklen_t error_length = sizeof(socket_error);
            int status;

            poll_fd.fd = fd;
            poll_fd.events = POLLOUT;
            status = poll(&poll_fd, 1, CONNECT_ATTEMPT_TIMEOUT_MS);
            if (status > 0 && (poll_fd.revents & (POLLOUT | POLLERR | POLLHUP)) &&
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_length) == 0 &&
                socket_error == 0)
                return fd;
            if (socket_error != 0)
                errno = socket_error;
        }
        close(fd);
        nanosleep(&delay, NULL);
    }
    errno = ETIMEDOUT;
    return -1;
}

static void make_local_descriptor(const struct slsm_sst_descriptor *base,
                                  uint64_t sst_id, size_t length, const void *data,
                                  struct slsm_sst_descriptor *descriptor)
{
    *descriptor = *base;
    descriptor->sst_id = sst_id;
    descriptor->total_length = length;
    descriptor->chunk_count = 1;
    descriptor->chunk_size = SLSM_CHUNK_SIZE;
    descriptor->checksum = slsm_fnv1a(data, length);
    memset(descriptor->chunks, 0, sizeof(descriptor->chunks));
}

static int cleanup_compaction_manifest(struct slsm_manifest *manifest,
                                       uint64_t first_sst_id,
                                       uint64_t second_sst_id,
                                       uint64_t output_sst_id,
                                       int *first_active, int *second_active,
                                       int *output_active, int output_committed)
{
    int result = 0;
    int status;

    if (*first_active) {
        status = slsm_manifest_abort(manifest, first_sst_id);
        if (status != 0 && result == 0)
            result = status;
        *first_active = 0;
    }
    if (*second_active) {
        status = slsm_manifest_abort(manifest, second_sst_id);
        if (status != 0 && result == 0)
            result = status;
        *second_active = 0;
    }
    if (*output_active) {
        status = output_committed ?
                 slsm_manifest_revoke(manifest, output_sst_id) :
                 slsm_manifest_abort(manifest, output_sst_id);
        if (status != 0 && result == 0)
            result = status;
        *output_active = 0;
    }
    return result;
}

int main(int argc, char **argv)
{
    static const struct option options[] = {
        {"device", required_argument, NULL, 'd'},
        {"server", required_argument, NULL, 's'},
        {"port", required_argument, NULL, 'p'},
        {"on-demand", no_argument, NULL, 'o'},
        {"compaction", no_argument, NULL, 'c'},
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
    struct slsm_sst_header sst_header = {0};
    struct slsm_sst_header output_header = {0};
    struct slsm_sst_header input_headers[SLSM_COMPACTION_INPUTS] = {{0}};
    struct slsm_compaction_batch_header batch_header = {0};
    struct slsm_sst_metadata output_metadata = {0};
    struct slsm_sst_metadata input_metadata[SLSM_COMPACTION_INPUTS] = {{0}};
    struct slsm_sst_descriptor input_descriptors[SLSM_COMPACTION_INPUTS] = {{0}};
    struct slsm_sst_descriptor output_descriptor = {0};
    const struct slsm_manifest_entry *lookup_entry;
    int manifest_status;
    uint64_t userspace_checksum = 0;
    uint64_t lookup_value = 0;
    uint64_t overlap_key = 0;
    uint64_t overlap_value = 0;
    void *buffer = NULL;
    void *output_buffer = NULL;
    int control_fd = -1;
    int device_fd = -1;
    int connected = 0;
    int manifest_active = 0;
    int on_demand = 0;
    int compaction = 0;
    int compaction_first_active = 0;
    int compaction_second_active = 0;
    int compaction_output_active = 0;
    int compaction_output_committed = 0;
    uint64_t compaction_first_id = 0;
    uint64_t compaction_second_id = 0;
    uint64_t compaction_output_id = 0;
    size_t output_length = 0;
    int result = EXIT_FAILURE;
    int option;

    slsm_manifest_init(&manifest);

    while ((option = getopt_long(argc, argv, "d:s:p:och", options, NULL)) != -1) {
        switch (option) {
        case 'd': device = optarg; break;
        case 's': server_address = optarg; break;
        case 'p':
            if (slsm_parse_port(optarg, &port) != 0) {
                usage(argv[0]);
                return EXIT_FAILURE;
            }
            break;
        case 'o': on_demand = 1; break;
        case 'c': compaction = 1; break;
        case 'h': usage(argv[0]); return EXIT_SUCCESS;
        default: usage(argv[0]); return EXIT_FAILURE;
        }
    }

    if (server_address == NULL) {
        fprintf(stderr, "--server is required\n");
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (on_demand) {
        printf("{\"event\":\"ondemand_worker\",\"status\":\"waiting\","
               "\"device_open\":false}\n");
        fflush(stdout);
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
        message.generation == 0 ||
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

    if (on_demand) {
        printf("{\"event\":\"ondemand_worker\",\"status\":\"triggered\","
               "\"device_open\":false}\n");
        fflush(stdout);
    }

    /*
     * The dispatcher has received a valid request but has not opened the
     * SLSM device or issued an RDMA ioctl.  In on-demand mode, only the child
     * becomes the short-lived session worker; the parent waits and reports its
     * exit status.  The default mode keeps the original one-process behavior.
     */
    if (on_demand) {
        pid_t worker_pid;
        int worker_status;

        fflush(NULL);
        worker_pid = fork();
        if (worker_pid < 0) {
            ack.phase = SLSM_PHASE_FETCH;
            ack.generation = message.generation;
            ack.status = -errno;
            goto send_ack;
        }
        if (worker_pid > 0) {
            int wait_status;

            close(control_fd);
            control_fd = -1;
            printf("{\"event\":\"ondemand_worker\",\"status\":\"started\","
                   "\"pid\":%ld}\n", (long)worker_pid);
            fflush(stdout);
            do {
                worker_status = waitpid(worker_pid, &wait_status, 0);
            } while (worker_status < 0 && errno == EINTR);
            if (worker_status < 0)
                return EXIT_FAILURE;
            if (WIFEXITED(wait_status))
                worker_status = WEXITSTATUS(wait_status);
            else
                worker_status = EXIT_FAILURE;
            printf("{\"event\":\"ondemand_worker\",\"status\":\"%s\","
                   "\"pid\":%ld,\"exit_code\":%d}\n",
                   worker_status == EXIT_SUCCESS ? "stopped" : "failed",
                   (long)worker_pid, worker_status);
            fflush(stdout);
            return worker_status == EXIT_SUCCESS ? EXIT_SUCCESS : EXIT_FAILURE;
        }
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
    if (compaction) {
        uint64_t first_offset;
        uint64_t second_offset;
        size_t first_length;
        size_t second_length;

        if (fetch_request.fetched_length != message.descriptor.total_length ||
            fetch_request.checksum != message.descriptor.checksum ||
            userspace_checksum != message.descriptor.checksum ||
            slsm_compaction_validate(buffer, (size_t)fetch_request.fetched_length,
                                     &batch_header) != 0 ||
            message.descriptor.sst_id != batch_header.output_sst_id ||
            message.metadata.epoch != batch_header.epoch) {
            fprintf(stderr, "compaction batch validation failed after RDMA READ\n");
            ack.phase = SLSM_PHASE_FETCH;
            ack.generation = message.generation;
            ack.status = -EBADMSG;
            goto disconnect_send_ack;
        }
        first_offset = batch_header.inputs[0].offset;
        second_offset = batch_header.inputs[1].offset;
        first_length = (size_t)batch_header.inputs[0].length;
        second_length = (size_t)batch_header.inputs[1].length;
        compaction_first_id = batch_header.inputs[0].sst_id;
        compaction_second_id = batch_header.inputs[1].sst_id;
        compaction_output_id = batch_header.output_sst_id;
        if (posix_memalign(&output_buffer, 4096, SLSM_SST_MAX_SIZE) != 0 ||
            slsm_sst_validate((const uint8_t *)buffer + first_offset, first_length,
                              compaction_first_id, batch_header.epoch,
                              &input_headers[0]) != 0 ||
            slsm_sst_validate((const uint8_t *)buffer + second_offset, second_length,
                              compaction_second_id, batch_header.epoch,
                              &input_headers[1]) != 0 ||
            slsm_compaction_merge(buffer, (size_t)fetch_request.fetched_length,
                                  output_buffer, SLSM_SST_MAX_SIZE,
                                  &output_metadata, &output_header, &overlap_key,
                                  &overlap_value) != 0) {
            fprintf(stderr, "compaction merge failed after RDMA READ\n");
            ack.phase = SLSM_PHASE_FETCH;
            ack.generation = message.generation;
            ack.status = -EBADMSG;
            goto disconnect_send_ack;
        }
        output_length = sizeof(output_header) + (size_t)output_header.payload_bytes;
        if (output_metadata.level != 1 || output_header.sst_id != compaction_output_id ||
            output_metadata.epoch != batch_header.epoch ||
            output_metadata.min_key != message.metadata.min_key ||
            output_metadata.max_key != message.metadata.max_key ||
            slsm_sst_validate(output_buffer, output_length, compaction_output_id,
                              batch_header.epoch, NULL) != 0 ||
            slsm_sst_lookup(output_buffer, output_length, overlap_key, &lookup_value) != 0 ||
            lookup_value != overlap_value) {
            fprintf(stderr, "compaction output validation failed\n");
            ack.phase = SLSM_PHASE_FETCH;
            ack.generation = message.generation;
            ack.status = -EBADMSG;
            goto disconnect_send_ack;
        }
        input_metadata[0] = (struct slsm_sst_metadata){
            .version = SLSM_METADATA_VERSION,
            .level = 0,
            .epoch = batch_header.epoch,
            .min_key = input_headers[0].min_key,
            .max_key = input_headers[0].max_key,
        };
        input_metadata[1] = (struct slsm_sst_metadata){
            .version = SLSM_METADATA_VERSION,
            .level = 0,
            .epoch = batch_header.epoch,
            .min_key = input_headers[1].min_key,
            .max_key = input_headers[1].max_key,
        };
        make_local_descriptor(&message.descriptor, compaction_first_id, first_length,
                              (const uint8_t *)buffer + first_offset,
                              &input_descriptors[0]);
        make_local_descriptor(&message.descriptor, compaction_second_id, second_length,
                              (const uint8_t *)buffer + second_offset,
                              &input_descriptors[1]);
        make_local_descriptor(&message.descriptor, compaction_output_id, output_length,
                              output_buffer, &output_descriptor);
        manifest_status = slsm_manifest_publish(&manifest, &input_metadata[0],
                                                &input_descriptors[0]);
        if (manifest_status == 0) {
            compaction_first_active = 1;
            manifest_status = slsm_manifest_publish(&manifest, &input_metadata[1],
                                                    &input_descriptors[1]);
        }
        if (manifest_status == 0) {
            compaction_second_active = 1;
            manifest_status = slsm_manifest_publish(&manifest, &output_metadata,
                                                    &output_descriptor);
        }
        if (manifest_status != 0) {
            ack.phase = SLSM_PHASE_FETCH;
            ack.generation = message.generation;
            ack.status = manifest_status;
            goto disconnect_send_ack;
        }
        compaction_output_active = 1;
        ack.phase = SLSM_PHASE_FETCH;
        ack.generation = message.generation;
        ack.status = 0;
        ack.fetched_length = fetch_request.fetched_length;
        ack.checksum = userspace_checksum;
        ack.elapsed_us = fetch_request.elapsed_us;
        ack.manifest_version = manifest.version;
        ack.lookup_sst_id = compaction_output_id;
        if (slsm_write_full(control_fd, &ack, sizeof(ack)) != 0) {
            perror("write compaction FETCH ack");
            goto disconnect;
        }
        if (slsm_read_full(control_fd, &next, sizeof(next)) != 0) {
            perror("read compaction COMMIT");
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
            ack.status = -EPROTO;
            if (slsm_write_full(control_fd, &ack, sizeof(ack)) != 0)
                result = EXIT_FAILURE;
            goto disconnect;
        }
        manifest_status = slsm_manifest_commit(&manifest, compaction_output_id,
                                               batch_header.epoch);
        lookup_entry = slsm_manifest_lookup(&manifest, output_metadata.min_key,
                                            batch_header.epoch);
        if (manifest_status != 0 || lookup_entry == NULL ||
            lookup_entry->descriptor.sst_id != compaction_output_id) {
            ack.status = manifest_status != 0 ? manifest_status : -EBADMSG;
        } else {
            ack.status = 0;
            ack.manifest_version = manifest.version;
            ack.lookup_sst_id = lookup_entry->descriptor.sst_id;
            compaction_output_committed = 1;
        }
        if (slsm_write_full(control_fd, &ack, sizeof(ack)) != 0) {
            perror("write compaction COMMIT ack");
            goto disconnect;
        }
        if (ack.status != 0)
            goto disconnect;
        if (slsm_read_full(control_fd, &next, sizeof(next)) != 0) {
            perror("read compaction REVOKE");
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
            ack.status = -EPROTO;
            goto revoke_ack;
        }
        manifest_status = slsm_manifest_abort(&manifest, compaction_first_id);
        if (manifest_status == 0)
            compaction_first_active = 0;
        if (manifest_status == 0)
            manifest_status = slsm_manifest_abort(&manifest, compaction_second_id);
        if (manifest_status == 0)
            compaction_second_active = 0;
        lookup_entry = slsm_manifest_lookup(&manifest, output_metadata.min_key,
                                            batch_header.epoch);
        if (manifest_status != 0 || lookup_entry == NULL ||
            lookup_entry->descriptor.sst_id != compaction_output_id) {
            ack.status = manifest_status != 0 ? manifest_status : -EBADMSG;
        } else {
            ack.status = 0;
            ack.manifest_version = manifest.version;
            ack.lookup_sst_id = lookup_entry->descriptor.sst_id;
            compaction_output_active = 0;
        }
        goto revoke_ack;
    }
    if (fetch_request.fetched_length != message.descriptor.total_length ||
        fetch_request.checksum != message.descriptor.checksum ||
        userspace_checksum != message.descriptor.checksum ||
        slsm_sst_validate(buffer, (size_t)fetch_request.fetched_length,
                          message.descriptor.sst_id, message.metadata.epoch,
                          &sst_header) != 0 ||
        sst_header.min_key != message.metadata.min_key ||
        sst_header.max_key != message.metadata.max_key ||
        slsm_sst_lookup(buffer, (size_t)fetch_request.fetched_length,
                         message.metadata.min_key, &lookup_value) != 0 ||
        lookup_value != message.metadata.min_key + UINT64_C(7)) {
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
    manifest_active = 1;
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
    if (ack.status != 0)
        goto disconnect;

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
        goto revoke_ack;
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
        manifest_active = 0;
    }
revoke_ack:
    if (compaction) {
        int cleanup_status = cleanup_compaction_manifest(
            &manifest, compaction_first_id, compaction_second_id,
            compaction_output_id, &compaction_first_active,
            &compaction_second_active, &compaction_output_active,
            compaction_output_committed);

        if (cleanup_status != 0 && ack.status == 0) {
            ack.status = cleanup_status;
            result = EXIT_FAILURE;
        }
    }
    if (manifest_active) {
        int abort_status = slsm_manifest_abort(&manifest, message.descriptor.sst_id);

        if (abort_status != 0) {
            if (ack.status == 0)
                ack.status = abort_status;
            errno = -abort_status;
            perror("slsm_manifest_abort");
            result = EXIT_FAILURE;
        }
        manifest_active = 0;
    }
    if (connected) {
        if (ioctl(device_fd, SLSM_IOCTL_DISCONNECT, 0) != 0) {
            int disconnect_errno = errno;

            perror("SLSM_IOCTL_DISCONNECT");
            ack.status = ack.status == 0 ? -disconnect_errno : ack.status;
            result = EXIT_FAILURE;
        }
        connected = 0;
    }
    if (slsm_write_full(control_fd, &ack, sizeof(ack)) != 0) {
        perror("write REVOKE ack");
        goto disconnect;
    }
    if (ack.status == 0 && result == EXIT_FAILURE)
        result = EXIT_SUCCESS;

disconnect:
    if (compaction) {
        int cleanup_status = cleanup_compaction_manifest(
            &manifest, compaction_first_id, compaction_second_id,
            compaction_output_id, &compaction_first_active,
            &compaction_second_active, &compaction_output_active,
            compaction_output_committed);

        if (cleanup_status != 0)
            result = EXIT_FAILURE;
    }
    if (manifest_active) {
        int abort_status = slsm_manifest_abort(&manifest, message.descriptor.sst_id);

        if (abort_status != 0) {
            errno = -abort_status;
            perror("slsm_manifest_abort");
            result = EXIT_FAILURE;
        }
        manifest_active = 0;
    }
    if (connected && ioctl(device_fd, SLSM_IOCTL_DISCONNECT, 0) != 0) {
        perror("SLSM_IOCTL_DISCONNECT");
        result = EXIT_FAILURE;
    }
    connected = 0;
    goto print_result;

disconnect_send_ack:
    if (compaction) {
        int cleanup_status = cleanup_compaction_manifest(
            &manifest, compaction_first_id, compaction_second_id,
            compaction_output_id, &compaction_first_active,
            &compaction_second_active, &compaction_output_active,
            compaction_output_committed);

        if (cleanup_status != 0 && ack.status == 0) {
            ack.status = cleanup_status;
            result = EXIT_FAILURE;
        }
    }
    if (connected && ioctl(device_fd, SLSM_IOCTL_DISCONNECT, 0) != 0) {
        perror("SLSM_IOCTL_DISCONNECT");
        result = EXIT_FAILURE;
    }
    connected = 0;
send_ack:
    if (control_fd >= 0 && slsm_write_full(control_fd, &ack, sizeof(ack)) != 0) {
        perror("write result ack");
        result = EXIT_FAILURE;
    }
print_result:
    if (result == EXIT_SUCCESS) {
        if (compaction) {
            printf("{\"event\":\"stage5_result\",\"role\":\"sn\",\"status\":\"pass\","
                   "\"mode\":\"compaction\",\"lifecycle\":"
                   "\"publish-fetch-commit-revoke\",\"input_sst_ids\":[%" PRIu64
                   ",%" PRIu64 "],\"output_sst_id\":%" PRIu64
                   ",\"output_level\":%u,\"output_records\":%u,"
                   "\"overlap_key\":%" PRIu64 ",\"overlap_value\":%" PRIu64
                   ",\"bytes\":%" PRIu64 ",\"elapsed_us\":%" PRIu64
                   ",\"manifest_version\":%" PRIu64 "}\n",
                   compaction_first_id, compaction_second_id, compaction_output_id,
                   output_metadata.level, output_header.record_count, overlap_key,
                   overlap_value, fetch_request.fetched_length,
                   fetch_request.elapsed_us, manifest.version);
            goto out;
        }
        printf("{\"event\":\"stage4_result\",\"role\":\"sn\",\"status\":\"pass\","
               "\"lifecycle\":\"publish-fetch-commit-revoke\",\"generation\":%" PRIu64
               ",\"sst_id\":%" PRIu64 ",\"bytes\":%" PRIu64 ",\"chunks\":%u,"
               "\"checksum\":\"0x%016" PRIx64 "\",\"elapsed_us\":%" PRIu64
               ",\"records\":%u,\"lookup_key\":%" PRIu64
               ",\"lookup_value\":%" PRIu64 ",\"manifest_version\":%" PRIu64 "}\n",
               message.generation,
               message.descriptor.sst_id, fetch_request.fetched_length,
               message.descriptor.chunk_count, userspace_checksum, fetch_request.elapsed_us,
               sst_header.record_count, message.metadata.min_key, lookup_value,
               manifest.version);
    }
out:
    if (device_fd >= 0)
        close(device_fd);
    if (control_fd >= 0)
    close(control_fd);
    free(output_buffer);
    free(buffer);
    return result;
}
