#define _GNU_SOURCE
#include "phos_gpu_rfork_bridge.h"

#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define SPD_GPU_RFORK_BRIDGE_ABI_VERSION UINT32_C(1)
#define PHOS_OK 0
#define PHOS_REBOUND_CHILD_SESSION UINT32_C(2)
#define PHOS_REPLAY_NONE UINT32_C(0)
#define SPD_GPU_RFORK_CLONE_TICKET_CAPACITY 128U
#define SPD_GPU_RFORK_ENV_VALUE_CAPACITY 256U

#define SPD_TARGET_GDR_MODE_ENV \
    "SPD_GPU_RFORK_TARGET_POS_GDR_IMAGE_MODE"
#define SPD_TARGET_GDR_LOCAL_ENV \
    "SPD_GPU_RFORK_TARGET_POS_GDR_IMAGE_LOCAL_ENDPOINT"
#define SPD_TARGET_GDR_REMOTE_ENV \
    "SPD_GPU_RFORK_TARGET_POS_GDR_IMAGE_REMOTE_ENDPOINT"
#define SPD_TARGET_GDR_DIRECT_LOCAL_ENV \
    "SPD_GPU_RFORK_TARGET_POS_GDR_IMAGE_DIRECT_LOCAL_ENDPOINT"
#define SPD_TARGET_GDR_DIRECT_REMOTE_ENV \
    "SPD_GPU_RFORK_TARGET_POS_GDR_IMAGE_DIRECT_REMOTE_ENDPOINT"

static struct spd_rfork_outcome parent_outcome;
static int parent_outcome_valid;
static int remote_child_attached;

struct SpdGpuRforkChildInput {
    char clone_ticket[SPD_GPU_RFORK_CLONE_TICKET_CAPACITY];
    const void *image_address;
    uint64_t image_mapping_bytes;
    uint64_t image_bytes;
    uint32_t image_record_count;
    uint32_t reserved;
    uint8_t image_sha256[32];
    char target_network_config[PATH_MAX];
    char target_pos_config[PATH_MAX];
    char target_gdr_mode[SPD_GPU_RFORK_ENV_VALUE_CAPACITY];
    char target_gdr_local[SPD_GPU_RFORK_ENV_VALUE_CAPACITY];
    char target_gdr_remote[SPD_GPU_RFORK_ENV_VALUE_CAPACITY];
    char target_gdr_direct_local[SPD_GPU_RFORK_ENV_VALUE_CAPACITY];
    char target_gdr_direct_remote[SPD_GPU_RFORK_ENV_VALUE_CAPACITY];
    uint64_t source_phos_uuid;
    void *cuda_allocation;
    uint64_t cuda_allocation_bytes;
    uint64_t source_digest;
};

static struct SpdGpuRforkChildInput child_input;
static struct SpdGpuRforkBridgeResult child_result;

typedef int (*SpdCudaMemcpyFn)(void *, const void *, size_t, int);
typedef int (*SpdCudaMemsetFn)(void *, int, size_t);
typedef int (*SpdCudaDeviceSynchronizeFn)(void);
typedef int (*SpdCudaFreeFn)(void *);
typedef int (*SpdPhosShutdownSessionFn)(void);

static long
raw_syscall6(long number, long arg1, long arg2, long arg3,
             long arg4, long arg5, long arg6)
{
#if defined(__x86_64__)
    register long r10 __asm__("r10") = arg4;
    register long r8 __asm__("r8") = arg5;
    register long r9 __asm__("r9") = arg6;
    long result;
    __asm__ volatile(
        "syscall"
        : "=a"(result)
        : "a"(number), "D"(arg1), "S"(arg2), "d"(arg3),
          "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory");
    return result;
#else
#error "The current Mitosis canary supports x86-64 only"
#endif
}

static long
raw_syscall3(long number, long arg1, long arg2, long arg3)
{
    return raw_syscall6(number, arg1, arg2, arg3, 0, 0, 0);
}

static long
raw_syscall1(long number, long arg1)
{
    return raw_syscall6(number, arg1, 0, 0, 0, 0, 0);
}

static long
raw_syscall0(long number)
{
    return raw_syscall6(number, 0, 0, 0, 0, 0, 0);
}

static void
raw_write_all(int fd, const char *data, size_t bytes)
{
    while (bytes != 0U) {
        long written = raw_syscall3(
            SYS_write, fd, (long)(uintptr_t)data, (long)bytes);
        if (written <= 0) return;
        data += (size_t)written;
        bytes -= (size_t)written;
    }
}

static void
raw_write_literal(const char *text)
{
    size_t bytes = 0;
    while (text[bytes] != '\0') ++bytes;
    raw_write_all(STDOUT_FILENO, text, bytes);
}

__attribute__((noreturn)) static void
raw_exit_group(int status)
{
    (void)raw_syscall1(SYS_exit_group, status);
    for (;;) {}
}

static void *
resolve_default(const char *name)
{
    void *address;
    (void)dlerror();
    address = dlsym(RTLD_DEFAULT, name);
    if (address == NULL || dlerror() != NULL) return NULL;
    return address;
}

static SpdPhosCloneAttachMemoryFn
resolve_clone_attach(void)
{
    SpdPhosCloneAttachMemoryFn function = NULL;
    void *address = resolve_default(
        "phos_post_rfork_child_clone_attach_memory_v2");
    if (address == NULL) return NULL;
    memcpy(&function, &address, sizeof(function));
    return function;
}

static SpdPhosQuerySessionFn
resolve_query_session(void)
{
    SpdPhosQuerySessionFn function = NULL;
    void *address = resolve_default("phos_query_session_identity_v1");
    if (address == NULL) return NULL;
    memcpy(&function, &address, sizeof(function));
    return function;
}

static uint64_t
fnv1a64(const char *text)
{
    const unsigned char *cursor = (const unsigned char *)text;
    uint64_t value = UINT64_C(0xcbf29ce484222325);
    while (*cursor != '\0') {
        value ^= *cursor++;
        value *= UINT64_C(0x100000001b3);
    }
    return value;
}

static uint64_t
fnv1a64_bytes(const unsigned char *data, size_t bytes)
{
    uint64_t value = UINT64_C(0xcbf29ce484222325);
    size_t index;
    for (index = 0; index < bytes; ++index) {
        value ^= data[index];
        value *= UINT64_C(0x100000001b3);
    }
    return value;
}

static int
copy_child_string(char *destination, size_t capacity, const char *source)
{
    size_t bytes;
    if (destination == NULL || capacity == 0U || source == NULL) return EINVAL;
    bytes = strnlen(source, capacity);
    if (bytes == 0U || bytes == capacity) return ENAMETOOLONG;
    memcpy(destination, source, bytes + 1U);
    return 0;
}

static int
copy_child_environment(
    char *destination, size_t capacity, const char *environment_name)
{
    const char *value;
    if (environment_name == NULL) return EINVAL;
    value = getenv(environment_name);
    return copy_child_string(destination, capacity, value);
}

static int
install_target_environment(void)
{
    if (setenv("NETWORK_CONFIG", child_input.target_network_config, 1) != 0 ||
        setenv("POS_CONFIG", child_input.target_pos_config, 1) != 0 ||
        setenv("POS_GDR_IMAGE_MODE", child_input.target_gdr_mode, 1) != 0 ||
        setenv("POS_GDR_IMAGE_LOCAL_ENDPOINT", child_input.target_gdr_local, 1) != 0 ||
        setenv("POS_GDR_IMAGE_REMOTE_ENDPOINT", child_input.target_gdr_remote, 1) != 0 ||
        setenv("POS_GDR_IMAGE_DIRECT_LOCAL_ENDPOINT",
               child_input.target_gdr_direct_local, 1) != 0 ||
        setenv("POS_GDR_IMAGE_DIRECT_REMOTE_ENDPOINT",
               child_input.target_gdr_direct_remote, 1) != 0) {
        return errno != 0 ? errno : EINVAL;
    }
    return 0;
}

static int
monotonic_ns(uint64_t *out)
{
    struct timespec value;
    long status;
    if (out == NULL) return EINVAL;
    status = raw_syscall3(
        SYS_clock_gettime, CLOCK_MONOTONIC, (long)(uintptr_t)&value, 0);
    if (status != 0 || value.tv_sec < 0 || value.tv_nsec < 0) {
        return errno != 0 ? errno : EINVAL;
    }
    *out = (uint64_t)value.tv_sec * UINT64_C(1000000000) +
           (uint64_t)value.tv_nsec;
    return 0;
}

__attribute__((noreturn)) static void
child_fatal_exit(void)
{
    static const char message[] =
        "phos_gpu_rfork_bridge: child clone-attach failed\n";
    if (remote_child_attached) {
        SpdPhosShutdownSessionFn shutdown = NULL;
        void *address = resolve_default("phos_shutdown_session_v1");
        if (address != NULL) {
            memcpy(&shutdown, &address, sizeof(shutdown));
            (void)shutdown();
        }
    }
    raw_write_all(STDERR_FILENO, message, sizeof(message) - 1U);
    raw_exit_group(120);
}

static void
append_hex64(char **cursor, uint64_t value)
{
    static const char hex[] = "0123456789abcdef";
    int shift;
    for (shift = 60; shift >= 0; shift -= 4) {
        *(*cursor)++ = hex[(value >> (unsigned int)shift) & UINT64_C(0xf)];
    }
}

static void
emit_digest_event(uint64_t source, uint64_t restored, uint64_t mutated)
{
    static char buffer[512];
    static const char prefix[] =
        "{\"schema\":\"serverlesspd.gpu-rfork-canary-event.v1\","
        "\"event\":\"child_gpu_verified\",\"status\":\"pass\","
        "\"bytes\":2097152,\"source_digest_fnv1a64\":\"";
    static const char middle1[] = "\",\"restored_digest_fnv1a64\":\"";
    static const char middle2[] = "\",\"mutated_digest_fnv1a64\":\"";
    static const char suffix[] = "\"}\n";
    char *cursor = buffer;
    size_t index;
    for (index = 0; index < sizeof(prefix) - 1U; ++index) *cursor++ = prefix[index];
    append_hex64(&cursor, source);
    for (index = 0; index < sizeof(middle1) - 1U; ++index) *cursor++ = middle1[index];
    append_hex64(&cursor, restored);
    for (index = 0; index < sizeof(middle2) - 1U; ++index) *cursor++ = middle2[index];
    append_hex64(&cursor, mutated);
    for (index = 0; index < sizeof(suffix) - 1U; ++index) *cursor++ = suffix[index];
    raw_write_all(STDOUT_FILENO, buffer, (size_t)(cursor - buffer));
}

static int
install_child_input(
    const char *clone_ticket, const void *image_address,
    uint64_t image_mapping_bytes, uint64_t image_bytes,
    uint32_t image_record_count, const uint8_t image_sha256[32],
    const char *target_network_config, const char *target_pos_config,
    uint64_t source_phos_uuid, void *cuda_allocation,
    uint64_t cuda_allocation_bytes, uint64_t source_digest)
{
    if (clone_ticket == NULL || *clone_ticket == '\0' ||
        image_address == NULL || image_mapping_bytes < image_bytes ||
        image_bytes == 0 || image_record_count == 0 || image_sha256 == NULL ||
        target_network_config == NULL || target_pos_config == NULL ||
        cuda_allocation == NULL || cuda_allocation_bytes != UINT64_C(2097152)) {
        return EINVAL;
    }
    memset(&child_input, 0, sizeof(child_input));
    if (copy_child_string(
            child_input.clone_ticket, sizeof(child_input.clone_ticket),
            clone_ticket) != 0 ||
        copy_child_string(
            child_input.target_network_config,
            sizeof(child_input.target_network_config),
            target_network_config) != 0 ||
        copy_child_string(
            child_input.target_pos_config,
            sizeof(child_input.target_pos_config), target_pos_config) != 0 ||
        copy_child_environment(
            child_input.target_gdr_mode, sizeof(child_input.target_gdr_mode),
            SPD_TARGET_GDR_MODE_ENV) != 0 ||
        copy_child_environment(
            child_input.target_gdr_local, sizeof(child_input.target_gdr_local),
            SPD_TARGET_GDR_LOCAL_ENV) != 0 ||
        copy_child_environment(
            child_input.target_gdr_remote,
            sizeof(child_input.target_gdr_remote),
            SPD_TARGET_GDR_REMOTE_ENV) != 0 ||
        copy_child_environment(
            child_input.target_gdr_direct_local,
            sizeof(child_input.target_gdr_direct_local),
            SPD_TARGET_GDR_DIRECT_LOCAL_ENV) != 0 ||
        copy_child_environment(
            child_input.target_gdr_direct_remote,
            sizeof(child_input.target_gdr_direct_remote),
            SPD_TARGET_GDR_DIRECT_REMOTE_ENV) != 0) {
        memset(&child_input, 0, sizeof(child_input));
        return ENAMETOOLONG;
    }
    child_input.image_address = image_address;
    child_input.image_mapping_bytes = image_mapping_bytes;
    child_input.image_bytes = image_bytes;
    child_input.image_record_count = image_record_count;
    memcpy(child_input.image_sha256, image_sha256, 32U);
    child_input.source_phos_uuid = source_phos_uuid;
    child_input.cuda_allocation = cuda_allocation;
    child_input.cuda_allocation_bytes = cuda_allocation_bytes;
    child_input.source_digest = source_digest;
    return 0;
}

__attribute__((visibility("default"), noinline, noreturn)) void
spd_gpu_rfork_child_run(void)
{
    SpdPhosCloneAttachMemoryFn clone_attach = resolve_clone_attach();
    SpdPhosQuerySessionFn query_session = resolve_query_session();
    SpdCudaMemcpyFn cuda_memcpy = NULL;
    SpdCudaMemsetFn cuda_memset = NULL;
    SpdCudaDeviceSynchronizeFn cuda_synchronize = NULL;
    SpdCudaFreeFn cuda_free = NULL;
    SpdPhosShutdownSessionFn phos_shutdown = NULL;
    uint64_t attach_started_ns;
    uint64_t attach_finished_ns;
    uint64_t restored_digest;
    uint64_t mutated_digest;
    unsigned char *host_buffer;
    long mapping;
    long child_pid;
    int status;
    void *address;
    size_t index;

    if (clone_attach == NULL || query_session == NULL ||
        child_input.clone_ticket[0] == '\0' || child_input.image_address == NULL ||
        child_input.cuda_allocation == NULL ||
        install_target_environment() != 0 ||
        monotonic_ns(&attach_started_ns) != 0) {
        child_fatal_exit();
    }
    memset(&child_result, 0, sizeof(child_result));
    child_result.abi_version = SPD_GPU_RFORK_BRIDGE_ABI_VERSION;
    status = clone_attach(
        child_input.clone_ticket, child_input.image_address,
        child_input.image_bytes, child_input.image_sha256,
        child_input.target_network_config, child_input.target_pos_config,
        &child_result.child, sizeof(child_result.child));
    if (status != PHOS_OK) child_fatal_exit();

    child_pid = raw_syscall0(SYS_getpid);
    if (child_pid <= 0 || child_result.child.abi_version != 2U ||
        child_result.child.actual_pid != (uint32_t)child_pid ||
        child_result.child.remoting_id < 0 ||
        child_result.child.replay_required != PHOS_REPLAY_NONE ||
        child_result.child.source_phos_uuid != child_input.source_phos_uuid ||
        child_result.child.clone_ticket_hash != fnv1a64(child_input.clone_ticket) ||
        child_result.child.image_bytes != child_input.image_bytes ||
        child_result.child.record_count != child_input.image_record_count ||
        child_result.child.reserved != 0U ||
        memcmp(child_result.child.sha256, child_input.image_sha256, 32U) != 0) {
        child_fatal_exit();
    }
    status = query_session(&child_result.session, sizeof(child_result.session));
    if (status != PHOS_OK || child_result.session.abi_version != 1U ||
        child_result.session.actual_pid != (uint32_t)child_pid ||
        child_result.session.remoting_id != child_result.child.remoting_id ||
        child_result.session.session_kind != PHOS_REBOUND_CHILD_SESSION ||
        child_result.session.phos_uuid != child_result.child.target_phos_uuid ||
        monotonic_ns(&attach_finished_ns) != 0 ||
        attach_finished_ns < attach_started_ns) {
        child_fatal_exit();
    }
    remote_child_attached = 1;
    child_result.clone_attach_ns = attach_finished_ns - attach_started_ns;
    raw_write_literal(
        "{\"schema\":\"serverlesspd.gpu-rfork-canary-event.v1\","
        "\"event\":\"child_clone_attached\",\"status\":\"pass\","
        "\"role\":\"remote_child\"}\n");

    if (raw_syscall6(
            SYS_munmap, (long)(uintptr_t)child_input.image_address,
            (long)child_input.image_mapping_bytes, 0, 0, 0, 0) != 0) {
        child_fatal_exit();
    }
    mapping = raw_syscall6(
        SYS_mmap, 0, (long)child_input.cuda_allocation_bytes,
        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if ((unsigned long)mapping >= (unsigned long)-4095L) child_fatal_exit();
    host_buffer = (unsigned char *)(uintptr_t)mapping;

#define RESOLVE_TYPED(target, symbol_name) do { \
        address = resolve_default(symbol_name); \
        if (address == NULL) child_fatal_exit(); \
        memcpy(&(target), &address, sizeof(target)); \
    } while (0)
    RESOLVE_TYPED(cuda_memcpy, "cudaMemcpy");
    RESOLVE_TYPED(cuda_memset, "cudaMemset");
    RESOLVE_TYPED(cuda_synchronize, "cudaDeviceSynchronize");
    RESOLVE_TYPED(cuda_free, "cudaFree");
    RESOLVE_TYPED(phos_shutdown, "phos_shutdown_session_v1");
#undef RESOLVE_TYPED

    status = cuda_memcpy(
        host_buffer, child_input.cuda_allocation,
        (size_t)child_input.cuda_allocation_bytes, 2);
    if (status != 0) child_fatal_exit();
    restored_digest = fnv1a64_bytes(
        host_buffer, (size_t)child_input.cuda_allocation_bytes);
    if (restored_digest != child_input.source_digest) child_fatal_exit();
    status = cuda_memset(
        child_input.cuda_allocation, 0xa5,
        (size_t)child_input.cuda_allocation_bytes);
    if (status != 0 || cuda_synchronize() != 0) child_fatal_exit();
    status = cuda_memcpy(
        host_buffer, child_input.cuda_allocation,
        (size_t)child_input.cuda_allocation_bytes, 2);
    if (status != 0) child_fatal_exit();
    for (index = 0; index < (size_t)child_input.cuda_allocation_bytes; ++index) {
        if (host_buffer[index] != 0xa5U) child_fatal_exit();
    }
    mutated_digest = fnv1a64_bytes(
        host_buffer, (size_t)child_input.cuda_allocation_bytes);
    emit_digest_event(child_input.source_digest, restored_digest, mutated_digest);

    if (cuda_free(child_input.cuda_allocation) != 0 || phos_shutdown() != PHOS_OK ||
        raw_syscall6(SYS_munmap, mapping,
                     (long)child_input.cuda_allocation_bytes, 0, 0, 0, 0) != 0) {
        child_fatal_exit();
    }
    remote_child_attached = 0;
    raw_write_literal(
        "{\"schema\":\"serverlesspd.gpu-rfork-canary-event.v1\","
        "\"event\":\"canary_complete\",\"status\":\"pass\","
        "\"role\":\"remote_child\"}\n");
    raw_exit_group(0);
}

static void
clear_result(struct SpdGpuRforkBridgeResult *result)
{
    if (result == NULL) return;
    memset(result, 0, sizeof(*result));
    result->abi_version = SPD_GPU_RFORK_BRIDGE_ABI_VERSION;
    result->status = SPD_GPU_RFORK_BRIDGE_INVALID_ARGUMENT;
}

__attribute__((visibility("default"), noinline)) int
spd_gpu_rfork_prepare_attach(
    uint64_t handler_id,
    uint32_t source_machine_id,
    uint32_t target_machine_id,
    const uint8_t run_cookie[SPD_RFORK_RUN_COOKIE_SIZE],
    const char *clone_ticket,
    const void *image_address,
    uint64_t image_mapping_bytes,
    uint64_t image_bytes,
    uint32_t image_record_count,
    const uint8_t image_sha256[32],
    const char *target_network_config,
    const char *target_pos_config,
    uint64_t source_phos_uuid,
    void *cuda_allocation,
    uint64_t cuda_allocation_bytes,
    uint64_t source_digest,
    struct SpdGpuRforkBridgeResult *result)
{
    struct spd_rfork_context context;
    struct spd_rfork_outcome outcome;
    enum spd_rfork_status rfork_status;

    clear_result(result);
    if (result == NULL || run_cookie == NULL ||
        clone_ticket == NULL || *clone_ticket == '\0' ||
        image_address == NULL || image_mapping_bytes < image_bytes ||
        image_bytes == 0 || image_record_count == 0 ||
        image_sha256 == NULL || target_network_config == NULL ||
        target_pos_config == NULL || cuda_allocation == NULL ||
        cuda_allocation_bytes != UINT64_C(2097152) ||
        install_child_input(
            clone_ticket, image_address, image_mapping_bytes, image_bytes,
            image_record_count, image_sha256, target_network_config,
            target_pos_config, source_phos_uuid, cuda_allocation,
            cuda_allocation_bytes, source_digest) != 0) {
        return SPD_GPU_RFORK_BRIDGE_INVALID_ARGUMENT;
    }
    rfork_status = spd_rfork_context_init(
        &context, handler_id, source_machine_id, target_machine_id, run_cookie);
    if (rfork_status != SPD_RFORK_OK) {
        result->status = SPD_GPU_RFORK_BRIDGE_RFORK_ERROR;
        result->native_status = rfork_status;
        return result->status;
    }

    memset(&outcome, 0, sizeof(outcome));
    rfork_status = spd_rfork_prepare(
        &context, spd_rfork_default_io_ops(), &outcome);
    if (rfork_status != SPD_RFORK_OK) {
        result->status = SPD_GPU_RFORK_BRIDGE_RFORK_ERROR;
        result->native_status = rfork_status;
        return result->status;
    }

    if (outcome.role == SPD_RFORK_ROLE_REMOTE_CHILD) {
        spd_gpu_rfork_child_run();
    }

    if (outcome.role != SPD_RFORK_ROLE_PARENT) {
        result->status = SPD_GPU_RFORK_BRIDGE_RFORK_ERROR;
        result->native_status = SPD_RFORK_ERR_ROLE_MISMATCH;
        return result->status;
    }

    parent_outcome = outcome;
    parent_outcome_valid = 1;
    result->handler_id = outcome.audit.handler_id;
    result->role = SPD_RFORK_ROLE_PARENT;
    result->native_status = SPD_RFORK_OK;
    result->status = SPD_GPU_RFORK_BRIDGE_OK;
    return SPD_GPU_RFORK_BRIDGE_OK;
}

__attribute__((visibility("default"), noinline)) int
spd_gpu_rfork_parent_release_bridge(void)
{
    enum spd_rfork_status status;
    if (!parent_outcome_valid) return SPD_GPU_RFORK_BRIDGE_OWNERSHIP_ERROR;
    status = spd_rfork_parent_release(
        spd_rfork_default_io_ops(), &parent_outcome);
    if (status != SPD_RFORK_OK) return status;
    parent_outcome_valid = 0;
    memset(&parent_outcome, 0, sizeof(parent_outcome));
    return SPD_GPU_RFORK_BRIDGE_OK;
}
