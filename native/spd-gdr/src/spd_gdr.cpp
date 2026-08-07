// Copyright 2026 ServerlessPD Artifact Authors
// SPDX-License-Identifier: Apache-2.0

#include "serverlessx/spd_gdr.h"

#include <cuda.h>
#include <cuda_runtime.h>
#ifndef SPD_GDR_DIRECT_ONLY
#include <glog/logging.h>
#endif
#include <arpa/inet.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <immintrin.h>
#include <infiniband/verbs.h>
#include <poll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>

#ifndef SPD_GDR_DIRECT_ONLY
#include "transfer_engine.h"
#endif

namespace {

constexpr uint32_t kAbiVersion = 1;
constexpr uint64_t kContextMagic = 0x5350444744523031ull;
constexpr const char *kProtocol = "rdma";
constexpr const char *kMemoryLocation = "cuda:0";
constexpr const char *kRequiredDevice = "mlx5_0";
constexpr uint64_t kMaximumBytes = 4ull << 30;
constexpr int kNativeIbPort = 1;
constexpr int kNativeGidIndex = 3;
constexpr unsigned int kDmaBufFdHandleType = 1;
constexpr unsigned int kVmmGdrSupportedAttribute = 110;
constexpr unsigned int kDmaBufSupportedAttribute = 124;
constexpr int kFlushTargetCurrentContext = 0;
constexpr int kFlushScopeOwner = 100;
constexpr uint32_t kNativeWireMagic = 0x53504444u;
constexpr uint32_t kNativeDoneMagic = 0x5350444fu;
constexpr uint16_t kNativeWireVersion = 1;
constexpr uint64_t kDefaultControlTimeoutMs = 300000;
constexpr uint64_t kMaximumControlTimeoutMs = 600000;

thread_local std::string last_error;
#ifndef SPD_GDR_DIRECT_ONLY
std::once_flag logging_once;
#endif
std::atomic<uintptr_t> next_handle_id{1};
std::atomic<uint32_t> next_psn{1};
std::mutex contexts_mutex;

enum class Backend {
    kMooncake,
    kDirectDmaBuf,
};

using get_handle_for_range_fn = CUresult (*)(void *, CUdeviceptr, size_t,
                                             unsigned int, unsigned long long);
using reg_dmabuf_mr_fn = ibv_mr *(*)(ibv_pd *, uint64_t, size_t, uint64_t, int,
                                     int);
using flush_gdr_writes_fn = CUresult (*)(int, int);

struct NativeResources {
    ibv_device **device_list = nullptr;
    ibv_context *verbs = nullptr;
    ibv_pd *pd = nullptr;
    ibv_cq *cq = nullptr;
    ibv_qp *qp = nullptr;
    ibv_mr *mr = nullptr;
    int dmabuf_fd = -1;
    sockaddr_in endpoint{};
    ibv_gid gid{};
    uint32_t psn = 0;
    int role = 0;
    int vmm_gdr_supported = 0;
    int dmabuf_supported = 0;
    uint64_t control_timeout_ms = kDefaultControlTimeoutMs;
    std::atomic<bool> stop{false};
    std::atomic<int> listener_fd{-1};
    std::atomic<int> control_fd{-1};
    std::thread control_thread;
    std::mutex thread_mutex;
    std::string thread_error;
    bool one_target_served = false;
    bool target_used = false;
};

struct Context {
    ~Context();

    uint64_t magic = kContextMagic;
#ifndef SPD_GDR_DIRECT_ONLY
    std::unique_ptr<mooncake::TransferEngine> engine;
#endif
    Backend backend = Backend::kMooncake;
    std::unique_ptr<NativeResources> native;
    void *buffer = nullptr;
    size_t bytes = 0;
    int gpu_id = -1;
    bool registered = false;
    bool transfer_active = false;
    bool poisoned = false;
    bool closing = false;
    std::mutex mutex;
};

std::unordered_map<uintptr_t, std::shared_ptr<Context>> contexts;

[[noreturn]] void fail(const std::string &message) {
    throw std::runtime_error(message);
}

void cuda_check(cudaError_t result, const char *operation) {
    if (result != cudaSuccess) {
        fail(std::string(operation) + ": " + cudaGetErrorString(result));
    }
}

uint64_t monotonic_raw_ns() {
    struct timespec value{};
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &value) != 0) {
        fail(std::string("clock_gettime failed: ") + std::strerror(errno));
    }
    return static_cast<uint64_t>(value.tv_sec) * 1000000000ull +
           static_cast<uint64_t>(value.tv_nsec);
}

void cu_check(CUresult result, const char *operation) {
    if (result == CUDA_SUCCESS) return;
    const char *name = "unknown";
    const char *description = "unknown";
    (void)cuGetErrorName(result, &name);
    (void)cuGetErrorString(result, &description);
    fail(std::string(operation) + ": " + name + " (" + description + ")");
}

void verbs_check(bool condition, const char *operation) {
    if (!condition) {
        fail(std::string(operation) + ": " + std::strerror(errno));
    }
}

uint64_t htonll(uint64_t value) {
    static const int one = 1;
    if (*reinterpret_cast<const char *>(&one) == 1) {
        return (static_cast<uint64_t>(htonl(static_cast<uint32_t>(value)))
                << 32) |
               htonl(static_cast<uint32_t>(value >> 32));
    }
    return value;
}

uint64_t ntohll(uint64_t value) { return htonll(value); }

uint64_t control_timeout_ms() {
    const char *raw = std::getenv("SPD_GDR_DIRECT_CONTROL_TIMEOUT_MS");
    if (raw == nullptr || raw[0] == '\0') return kDefaultControlTimeoutMs;
    char *end = nullptr;
    errno = 0;
    const unsigned long long value = std::strtoull(raw, &end, 10);
    if (errno != 0 || end == raw || *end != '\0' || value == 0 ||
        value > kMaximumControlTimeoutMs) {
        fail("SPD_GDR_DIRECT_CONTROL_TIMEOUT_MS must be in [1, 600000]");
    }
    return static_cast<uint64_t>(value);
}

sockaddr_in parse_ipv4_endpoint(const char *endpoint, const char *name) {
    if (endpoint == nullptr || endpoint[0] == '\0') {
        fail(std::string(name) + " is empty");
    }
    const std::string value(endpoint);
    const size_t separator = value.rfind(':');
    if (separator == std::string::npos || separator == 0 ||
        separator + 1 == value.size() || value.find(':') != separator) {
        fail(std::string(name) + " must be IPV4:PORT");
    }
    const std::string ip = value.substr(0, separator);
    const std::string port = value.substr(separator + 1);
    char *end = nullptr;
    errno = 0;
    const unsigned long parsed_port = std::strtoul(port.c_str(), &end, 10);
    if (errno != 0 || end == port.c_str() || *end != '\0' || parsed_port == 0 ||
        parsed_port > 65535) {
        fail(std::string(name) + " has an invalid TCP port");
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(parsed_port));
    if (inet_pton(AF_INET, ip.c_str(), &address.sin_addr) != 1) {
        fail(std::string(name) + " has an invalid IPv4 address");
    }
    return address;
}

void set_nonblocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        fail(std::string("fcntl(O_NONBLOCK): ") + std::strerror(errno));
    }
    const int descriptor_flags = fcntl(fd, F_GETFD, 0);
    if (descriptor_flags < 0 ||
        fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC) != 0) {
        fail(std::string("fcntl(FD_CLOEXEC): ") + std::strerror(errno));
    }
}

uint64_t deadline_after_ms(uint64_t timeout_ms) {
    return monotonic_raw_ns() + timeout_ms * 1000000ull;
}

bool wait_socket(int fd, short events, uint64_t deadline_ns,
                 const std::atomic<bool> *stop) {
    while (true) {
        if (stop != nullptr && stop->load(std::memory_order_acquire)) {
            return false;
        }
        const uint64_t now = monotonic_raw_ns();
        if (now >= deadline_ns) fail("native control operation timed out");
        uint64_t remaining_ms = (deadline_ns - now + 999999ull) / 1000000ull;
        if (stop != nullptr && remaining_ms > 100) remaining_ms = 100;
        pollfd descriptor{};
        descriptor.fd = fd;
        descriptor.events = events;
        const int result = poll(&descriptor, 1, static_cast<int>(remaining_ms));
        if (result > 0) return true;
        if (result == 0) continue;
        if (errno == EINTR) continue;
        fail(std::string("poll: ") + std::strerror(errno));
    }
}

bool write_all(int fd, const void *buffer, size_t bytes, uint64_t deadline_ns,
               const std::atomic<bool> *stop = nullptr) {
    const auto *cursor = static_cast<const uint8_t *>(buffer);
    while (bytes != 0) {
        const ssize_t written = send(fd, cursor, bytes, MSG_NOSIGNAL);
        if (written > 0) {
            cursor += written;
            bytes -= static_cast<size_t>(written);
            continue;
        }
        if (written == 0) fail("send returned zero");
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (!wait_socket(fd, POLLOUT, deadline_ns, stop)) return false;
            continue;
        }
        fail(std::string("send: ") + std::strerror(errno));
    }
    return true;
}

bool read_all(int fd, void *buffer, size_t bytes, uint64_t deadline_ns,
              const std::atomic<bool> *stop = nullptr) {
    auto *cursor = static_cast<uint8_t *>(buffer);
    while (bytes != 0) {
        const ssize_t received = recv(fd, cursor, bytes, 0);
        if (received > 0) {
            cursor += received;
            bytes -= static_cast<size_t>(received);
            continue;
        }
        if (received == 0) fail("peer closed native control connection");
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (!wait_socket(fd, POLLIN, deadline_ns, stop)) return false;
            continue;
        }
        fail(std::string("recv: ") + std::strerror(errno));
    }
    return true;
}

int open_control_listener(const sockaddr_in &local) {
    const int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) fail(std::string("socket: ") + std::strerror(errno));
    try {
        const int yes = 1;
        (void)setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        set_nonblocking(listener);
        verbs_check(bind(listener, reinterpret_cast<const sockaddr *>(&local),
                         sizeof(local)) == 0,
                    "bind native source endpoint");
        verbs_check(listen(listener, 1) == 0, "listen native source endpoint");
        return listener;
    } catch (...) {
        (void)close(listener);
        throw;
    }
}

int accept_one(int listener, uint64_t deadline_ns,
               const std::atomic<bool> *stop) {
    while (true) {
        if (!wait_socket(listener, POLLIN, deadline_ns, stop)) return -1;
        const int connection = accept(listener, nullptr, nullptr);
        if (connection >= 0) {
            try {
                set_nonblocking(connection);
                return connection;
            } catch (...) {
                (void)close(connection);
                throw;
            }
        }
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
            continue;
        }
        fail(std::string("accept: ") + std::strerror(errno));
    }
}

int open_control_client(const sockaddr_in &local, const sockaddr_in &peer,
                        uint64_t deadline_ns) {
    const int connection = socket(AF_INET, SOCK_STREAM, 0);
    if (connection < 0) fail(std::string("socket: ") + std::strerror(errno));
    try {
        const int yes = 1;
        (void)setsockopt(connection, SOL_SOCKET, SO_REUSEADDR, &yes,
                         sizeof(yes));
        set_nonblocking(connection);
        verbs_check(bind(connection, reinterpret_cast<const sockaddr *>(&local),
                         sizeof(local)) == 0,
                    "bind native target endpoint");
        if (connect(connection, reinterpret_cast<const sockaddr *>(&peer),
                    sizeof(peer)) != 0) {
            if (errno != EINPROGRESS) {
                fail(std::string("connect: ") + std::strerror(errno));
            }
            (void)wait_socket(connection, POLLOUT, deadline_ns, nullptr);
            int socket_error = 0;
            socklen_t length = sizeof(socket_error);
            if (getsockopt(connection, SOL_SOCKET, SO_ERROR, &socket_error,
                           &length) != 0) {
                fail(std::string("getsockopt(SO_ERROR): ") +
                     std::strerror(errno));
            }
            if (socket_error != 0) {
                fail(std::string("connect: ") + std::strerror(socket_error));
            }
        }
        return connection;
    } catch (...) {
        (void)close(connection);
        throw;
    }
}

#ifndef SPD_GDR_DIRECT_ONLY
std::string read_text_file(const char *path) {
    std::ifstream input(path);
    if (!input) fail(std::string("cannot read ") + path);
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

void validate_platform() {
    const std::string peer_direct = read_text_file(
        "/sys/module/nvidia_peermem/parameters/peerdirect_support");
    if (peer_direct.find('1') == std::string::npos) {
        fail("nvidia_peermem peerdirect_support must equal 1");
    }
    const std::string rdma_state =
        read_text_file("/sys/class/infiniband/mlx5_0/ports/1/state");
    if (rdma_state.find("ACTIVE") == std::string::npos) {
        fail("mlx5_0 port 1 is not ACTIVE");
    }
}
#endif

void validate_pointer(void *pointer, size_t bytes, int gpu_id,
                      bool require_sync_memops) {
    if (pointer == nullptr) fail("CUDA buffer pointer is null");
    if (bytes == 0 || bytes > kMaximumBytes) {
        fail("CUDA buffer size must be in [1, 4 GiB]");
    }
    if (gpu_id != 0) fail("this artifact requires CUDA device 0");
    int current_device = -1;
    cuda_check(cudaGetDevice(&current_device), "cudaGetDevice");
    if (current_device != gpu_id) {
        fail("current CUDA context belongs to another device");
    }
    cudaPointerAttributes attributes{};
    cuda_check(cudaPointerGetAttributes(&attributes, pointer),
               "cudaPointerGetAttributes");
    if (attributes.type != cudaMemoryTypeDevice ||
        attributes.device != gpu_id) {
        fail("caller pointer is not a device-0 CUDA allocation");
    }
    CUdeviceptr allocation_base = 0;
    size_t allocation_bytes = 0;
    const CUresult range = cuMemGetAddressRange(
        &allocation_base, &allocation_bytes,
        static_cast<CUdeviceptr>(reinterpret_cast<uintptr_t>(pointer)));
    const uint64_t pointer_value = reinterpret_cast<uintptr_t>(pointer);
    if (pointer_value > std::numeric_limits<uintptr_t>::max() - (bytes - 1)) {
        fail("requested CUDA range overflows the address space");
    }
    if (range == CUDA_SUCCESS) {
        const uint64_t allocation_value =
            static_cast<uint64_t>(allocation_base);
        if (pointer_value < allocation_value ||
            pointer_value - allocation_value > allocation_bytes ||
            bytes > allocation_bytes - (pointer_value - allocation_value)) {
            fail("requested CUDA range exceeds the driver allocation");
        }
    } else {
        // PhoenixOS restores memory with CUDA VMM.  cuMemGetAddressRange is
        // specified for legacy allocations and may reject a valid VMM range,
        // so prove that both ends resolve to the same retained allocation.
        CUmemGenericAllocationHandle first = 0;
        CUmemGenericAllocationHandle last = 0;
        const CUresult first_result = cuMemRetainAllocationHandle(
            &first, reinterpret_cast<void *>(pointer_value));
        const CUresult last_result = cuMemRetainAllocationHandle(
            &last, reinterpret_cast<void *>(pointer_value + bytes - 1));
        const bool first_retained = first_result == CUDA_SUCCESS;
        const bool last_retained = last_result == CUDA_SUCCESS;
        const bool one_allocation =
            first_retained && last_retained && first == last;
        if (first_retained) {
            (void)cuMemRelease(first);
        }
        if (last_retained) {
            (void)cuMemRelease(last);
        }
        if (!one_allocation) {
            fail("CUDA pointer is neither one legacy nor one VMM allocation");
        }
    }
    if (require_sync_memops) {
        unsigned int sync_memops = 1;
        if (cuPointerSetAttribute(&sync_memops,
                                  CU_POINTER_ATTRIBUTE_SYNC_MEMOPS,
                                  static_cast<CUdeviceptr>(pointer_value)) !=
            CUDA_SUCCESS) {
            fail("cuPointerSetAttribute(SYNC_MEMOPS) failed");
        }
        sync_memops = 0;
        if (cuPointerGetAttribute(&sync_memops,
                                  CU_POINTER_ATTRIBUTE_SYNC_MEMOPS,
                                  static_cast<CUdeviceptr>(pointer_value)) !=
                CUDA_SUCCESS ||
            sync_memops != 1) {
            fail("CUDA allocation does not enforce SYNC_MEMOPS");
        }
    }
}

struct NativeWireInfo {
    uint32_t magic;
    uint16_t version;
    uint16_t role;
    uint32_t qpn;
    uint32_t psn;
    uint32_t rkey;
    uint32_t reserved;
    uint64_t address;
    uint64_t bytes;
    uint8_t gid[16];
};

static_assert(sizeof(NativeWireInfo) == 56,
              "native wire metadata must have a stable size");

struct NativePeer {
    int role = 0;
    uint32_t qpn = 0;
    uint32_t psn = 0;
    uint32_t rkey = 0;
    uint64_t address = 0;
    uint64_t bytes = 0;
    ibv_gid gid{};
};

NativeWireInfo encode_native_wire(const Context &context) {
    const NativeResources &native = *context.native;
    NativeWireInfo wire{};
    wire.magic = htonl(kNativeWireMagic);
    wire.version = htons(kNativeWireVersion);
    wire.role = htons(static_cast<uint16_t>(native.role));
    wire.qpn = htonl(native.qp->qp_num);
    wire.psn = htonl(native.psn);
    wire.rkey = htonl(native.mr->rkey);
    wire.address = htonll(
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(context.buffer)));
    wire.bytes = htonll(context.bytes);
    std::memcpy(wire.gid, native.gid.raw, sizeof(wire.gid));
    return wire;
}

NativePeer decode_native_wire(const NativeWireInfo &wire, int expected_role,
                              size_t expected_bytes) {
    if (ntohl(wire.magic) != kNativeWireMagic ||
        ntohs(wire.version) != kNativeWireVersion || wire.reserved != 0) {
        fail("native peer metadata has an invalid header");
    }
    NativePeer peer{};
    peer.role = ntohs(wire.role);
    peer.qpn = ntohl(wire.qpn);
    peer.psn = ntohl(wire.psn);
    peer.rkey = ntohl(wire.rkey);
    peer.address = ntohll(wire.address);
    peer.bytes = ntohll(wire.bytes);
    std::memcpy(peer.gid.raw, wire.gid, sizeof(peer.gid.raw));
    if (peer.role != expected_role || peer.qpn == 0 || peer.rkey == 0 ||
        peer.bytes != expected_bytes || peer.address == 0) {
        fail("native peer metadata does not describe the expected DMA-BUF");
    }
    return peer;
}

void transition_native_qp(NativeResources *native, const NativePeer &peer) {
    const bool source = native->role == SPD_GDR_DMABUF_SOURCE_V1;
    ibv_qp_attr init{};
    init.qp_state = IBV_QPS_INIT;
    init.pkey_index = 0;
    init.port_num = kNativeIbPort;
    init.qp_access_flags =
        IBV_ACCESS_LOCAL_WRITE | (source ? IBV_ACCESS_REMOTE_READ : 0);
    verbs_check(ibv_modify_qp(native->qp, &init,
                              IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT |
                                  IBV_QP_ACCESS_FLAGS) == 0,
                "ibv_modify_qp(INIT)");

    ibv_qp_attr ready{};
    ready.qp_state = IBV_QPS_RTR;
    ready.path_mtu = IBV_MTU_4096;
    ready.dest_qp_num = peer.qpn;
    ready.rq_psn = peer.psn;
    ready.max_dest_rd_atomic = 1;
    ready.min_rnr_timer = 12;
    ready.ah_attr.is_global = 1;
    ready.ah_attr.dlid = 0;
    ready.ah_attr.sl = 0;
    ready.ah_attr.src_path_bits = 0;
    ready.ah_attr.port_num = kNativeIbPort;
    ready.ah_attr.grh.dgid = peer.gid;
    ready.ah_attr.grh.sgid_index = kNativeGidIndex;
    ready.ah_attr.grh.hop_limit = 1;
    verbs_check(ibv_modify_qp(native->qp, &ready,
                              IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
                                  IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
                                  IBV_QP_MAX_DEST_RD_ATOMIC |
                                  IBV_QP_MIN_RNR_TIMER) == 0,
                "ibv_modify_qp(RTR)");

    ibv_qp_attr running{};
    running.qp_state = IBV_QPS_RTS;
    running.sq_psn = native->psn;
    running.timeout = 14;
    running.retry_cnt = 7;
    running.rnr_retry = 7;
    running.max_rd_atomic = 1;
    verbs_check(ibv_modify_qp(native->qp, &running,
                              IBV_QP_STATE | IBV_QP_SQ_PSN | IBV_QP_TIMEOUT |
                                  IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY |
                                  IBV_QP_MAX_QP_RD_ATOMIC) == 0,
                "ibv_modify_qp(RTS)");
}

void make_native_resources(Context *context, const sockaddr_in &endpoint,
                           int role) {
    context->native = std::make_unique<NativeResources>();
    NativeResources *native = context->native.get();
    native->endpoint = endpoint;
    native->role = role;
    native->control_timeout_ms = control_timeout_ms();

    CUdevice cuda_device = 0;
    cu_check(cuDeviceGet(&cuda_device, context->gpu_id), "cuDeviceGet");
    cu_check(cuDeviceGetAttribute(
                 &native->vmm_gdr_supported,
                 static_cast<CUdevice_attribute>(kVmmGdrSupportedAttribute),
                 cuda_device),
             "cuDeviceGetAttribute(GPU_DIRECT_RDMA_WITH_CUDA_VMM)");
    cu_check(cuDeviceGetAttribute(
                 &native->dmabuf_supported,
                 static_cast<CUdevice_attribute>(kDmaBufSupportedAttribute),
                 cuda_device),
             "cuDeviceGetAttribute(DMA_BUF_SUPPORTED)");
    if (native->vmm_gdr_supported != 1 || native->dmabuf_supported != 1) {
        fail("CUDA VMM DMA-BUF GDR capabilities are incomplete");
    }

    int device_count = 0;
    native->device_list = ibv_get_device_list(&device_count);
    if (native->device_list == nullptr) fail("ibv_get_device_list failed");
    ibv_device *selected = nullptr;
    for (int index = 0; index < device_count; ++index) {
        if (std::strcmp(ibv_get_device_name(native->device_list[index]),
                        kRequiredDevice) == 0) {
            selected = native->device_list[index];
            break;
        }
    }
    if (selected == nullptr) fail("mlx5_0 was not found");
    native->verbs = ibv_open_device(selected);
    if (native->verbs == nullptr) fail("ibv_open_device(mlx5_0) failed");
    ibv_port_attr port{};
    verbs_check(ibv_query_port(native->verbs, kNativeIbPort, &port) == 0,
                "ibv_query_port(mlx5_0,1)");
    if (port.state != IBV_PORT_ACTIVE || port.active_mtu < IBV_MTU_4096) {
        fail("mlx5_0 port 1 is not ACTIVE at MTU 4096");
    }
    verbs_check(ibv_query_gid(native->verbs, kNativeIbPort, kNativeGidIndex,
                              &native->gid) == 0,
                "ibv_query_gid(mlx5_0,1,3)");
    native->pd = ibv_alloc_pd(native->verbs);
    if (native->pd == nullptr) fail("ibv_alloc_pd failed");
    native->cq = ibv_create_cq(native->verbs, 4, nullptr, nullptr, 0);
    if (native->cq == nullptr) fail("ibv_create_cq failed");
    ibv_qp_init_attr qp_init{};
    qp_init.send_cq = native->cq;
    qp_init.recv_cq = native->cq;
    qp_init.qp_type = IBV_QPT_RC;
    qp_init.cap.max_send_wr = 2;
    qp_init.cap.max_recv_wr = 1;
    qp_init.cap.max_send_sge = 1;
    qp_init.cap.max_recv_sge = 1;
    native->qp = ibv_create_qp(native->pd, &qp_init);
    if (native->qp == nullptr) fail("ibv_create_qp failed");

    const auto get_handle = reinterpret_cast<get_handle_for_range_fn>(
        dlsym(RTLD_DEFAULT, "cuMemGetHandleForAddressRange"));
    if (get_handle == nullptr) {
        fail("cuMemGetHandleForAddressRange is absent");
    }
    cu_check(get_handle(&native->dmabuf_fd,
                        static_cast<CUdeviceptr>(
                            reinterpret_cast<uintptr_t>(context->buffer)),
                        context->bytes, kDmaBufFdHandleType, 0),
             "cuMemGetHandleForAddressRange(DMA_BUF_FD)");
    const auto register_dmabuf = reinterpret_cast<reg_dmabuf_mr_fn>(
        dlsym(RTLD_DEFAULT, "ibv_reg_dmabuf_mr"));
    if (register_dmabuf == nullptr) fail("ibv_reg_dmabuf_mr is absent");
    const int access = role == SPD_GDR_DMABUF_SOURCE_V1
                           ? IBV_ACCESS_REMOTE_READ
                           : IBV_ACCESS_LOCAL_WRITE;
    native->mr = register_dmabuf(
        native->pd, 0, context->bytes,
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(context->buffer)),
        native->dmabuf_fd, access);
    if (native->mr == nullptr) {
        fail(std::string("ibv_reg_dmabuf_mr: ") + std::strerror(errno));
    }
    const uint32_t serial = next_psn.fetch_add(0x101u);
    native->psn = static_cast<uint32_t>(
        (monotonic_raw_ns() ^ native->qp->qp_num ^ serial) & 0x00ffffffu);
    if (native->psn == 0) native->psn = 1;
}

void stop_native_thread(Context *context) {
    if (context == nullptr || !context->native) return;
    NativeResources *native = context->native.get();
    native->stop.store(true, std::memory_order_release);
    const int control = native->control_fd.load(std::memory_order_acquire);
    if (control >= 0) (void)shutdown(control, SHUT_RDWR);
    const int listener = native->listener_fd.load(std::memory_order_acquire);
    if (listener >= 0) (void)shutdown(listener, SHUT_RDWR);
    if (native->control_thread.joinable()) {
        native->control_thread.join();
    }
}

void close_native_socket(std::atomic<int> *slot) {
    const int fd = slot->exchange(-1, std::memory_order_acq_rel);
    if (fd >= 0) (void)close(fd);
}

void cleanup_native_best_effort(Context *context) {
    if (context == nullptr || !context->native) return;
    stop_native_thread(context);
    NativeResources *native = context->native.get();
    close_native_socket(&native->control_fd);
    close_native_socket(&native->listener_fd);
    if (native->qp != nullptr) {
        (void)ibv_destroy_qp(native->qp);
        native->qp = nullptr;
    }
    if (native->cq != nullptr) {
        (void)ibv_destroy_cq(native->cq);
        native->cq = nullptr;
    }
    if (native->mr != nullptr) {
        (void)ibv_dereg_mr(native->mr);
        native->mr = nullptr;
    }
    if (native->dmabuf_fd >= 0) {
        (void)close(native->dmabuf_fd);
        native->dmabuf_fd = -1;
    }
    if (native->pd != nullptr) {
        (void)ibv_dealloc_pd(native->pd);
        native->pd = nullptr;
    }
    if (native->verbs != nullptr) {
        (void)ibv_close_device(native->verbs);
        native->verbs = nullptr;
    }
    if (native->device_list != nullptr) {
        ibv_free_device_list(native->device_list);
        native->device_list = nullptr;
    }
}

Context::~Context() { cleanup_native_best_effort(this); }

std::string cleanup_native_strict(Context *context) {
    stop_native_thread(context);
    NativeResources *native = context->native.get();
    close_native_socket(&native->control_fd);
    close_native_socket(&native->listener_fd);
    if (native->qp != nullptr) {
        if (ibv_destroy_qp(native->qp) != 0) return "ibv_destroy_qp failed";
        native->qp = nullptr;
    }
    if (native->cq != nullptr) {
        if (ibv_destroy_cq(native->cq) != 0) return "ibv_destroy_cq failed";
        native->cq = nullptr;
    }
    if (native->mr != nullptr) {
        if (ibv_dereg_mr(native->mr) != 0) return "ibv_dereg_mr failed";
        native->mr = nullptr;
    }
    if (native->dmabuf_fd >= 0) {
        if (close(native->dmabuf_fd) != 0) return "close(DMA-BUF) failed";
        native->dmabuf_fd = -1;
    }
    if (native->pd != nullptr) {
        if (ibv_dealloc_pd(native->pd) != 0) return "ibv_dealloc_pd failed";
        native->pd = nullptr;
    }
    if (native->verbs != nullptr) {
        if (ibv_close_device(native->verbs) != 0) {
            return "ibv_close_device failed";
        }
        native->verbs = nullptr;
    }
    if (native->device_list != nullptr) {
        ibv_free_device_list(native->device_list);
        native->device_list = nullptr;
    }
    return {};
}

void set_native_thread_error(NativeResources *native,
                             const std::string &message) {
    std::lock_guard<std::mutex> lock(native->thread_mutex);
    native->thread_error = message;
}

void serve_native_source(Context *context) noexcept {
    NativeResources *native = context->native.get();
    try {
        const uint64_t deadline = deadline_after_ms(native->control_timeout_ms);
        const int listener =
            native->listener_fd.load(std::memory_order_acquire);
        const int control = accept_one(listener, deadline, &native->stop);
        if (control < 0) return;
        native->control_fd.store(control, std::memory_order_release);
        close_native_socket(&native->listener_fd);
        const NativeWireInfo local = encode_native_wire(*context);
        NativeWireInfo raw_peer{};
        if (!write_all(control, &local, sizeof(local), deadline,
                       &native->stop) ||
            !read_all(control, &raw_peer, sizeof(raw_peer), deadline,
                      &native->stop)) {
            close_native_socket(&native->control_fd);
            return;
        }
        const NativePeer peer = decode_native_wire(
            raw_peer, SPD_GDR_DMABUF_TARGET_V1, context->bytes);
        transition_native_qp(native, peer);
        uint32_t raw_done = 0;
        if (!read_all(control, &raw_done, sizeof(raw_done), deadline,
                      &native->stop)) {
            close_native_socket(&native->control_fd);
            return;
        }
        if (ntohl(raw_done) != kNativeDoneMagic) {
            fail("native target sent an invalid completion acknowledgement");
        }
        {
            std::lock_guard<std::mutex> lock(native->thread_mutex);
            native->one_target_served = true;
        }
        close_native_socket(&native->control_fd);
    } catch (const std::exception &error) {
        close_native_socket(&native->control_fd);
        close_native_socket(&native->listener_fd);
        if (!native->stop.load(std::memory_order_acquire)) {
            set_native_thread_error(native, error.what());
        }
    }
}

#ifndef SPD_GDR_DIRECT_ONLY

std::unique_ptr<mooncake::TransferEngine> make_engine(
    const std::string &local_server_name) {
    if (local_server_name.empty()) fail("local server name is empty");
    const std::string topology =
        R"({"cpu:0":[["mlx5_0"],[]],"cpu:1":[["mlx5_0"],[]],"cuda:0":[["mlx5_0"],[]]})";
    auto engine = std::make_unique<mooncake::TransferEngine>(false);
    const auto local = mooncake::parseHostNameWithPort(local_server_name);
    if (engine->init("P2PHANDSHAKE", local_server_name, local.first,
                     local.second) != 0) {
        fail("TransferEngine::init failed");
    }
    void *transport_args[2] = {const_cast<char *>(topology.c_str()), nullptr};
    if (engine->installTransport(kProtocol, transport_args) == nullptr) {
        fail("installTransport(rdma) failed");
    }
    return engine;
}

bool descriptor_uses_required_device(
    const mooncake::TransferMetadata::SegmentDesc &descriptor) {
    for (const auto &device : descriptor.devices) {
        if (device.name == kRequiredDevice) return true;
    }
    return false;
}
#endif

std::shared_ptr<Context> checked_context(void *handle) {
    if (handle == nullptr) fail("GDR context is null");
    const auto handle_id = reinterpret_cast<uintptr_t>(handle);
    std::lock_guard<std::mutex> lock(contexts_mutex);
    const auto found = contexts.find(handle_id);
    if (found == contexts.end()) {
        fail("GDR context is invalid or closed");
    }
    return found->second;
}

void set_error(const std::exception &error) { last_error = error.what(); }

int direct_read(void *handle, const char *source_server_name,
                uint64_t timeout_ms, uint64_t *metadata_exchange_ns,
                uint64_t *submit_to_completion_ns,
                uint64_t *transferred_bytes) {
    std::shared_ptr<Context> context;
    int control = -1;
    try {
        context = checked_context(handle);
        std::unique_lock<std::mutex> lock(context->mutex);
        if (context->magic != kContextMagic ||
            context->backend != Backend::kDirectDmaBuf || !context->native ||
            !context->registered || context->closing) {
            fail("direct DMA-BUF context is invalid or closed");
        }
        if (context->poisoned) fail("GDR context is poisoned");
        if (context->transfer_active) fail("GDR transfer is already active");
        NativeResources *native = context->native.get();
        if (native->role != SPD_GDR_DMABUF_TARGET_V1) {
            fail("only a direct DMA-BUF target may initiate RDMA READ");
        }
        if (native->target_used) fail("direct DMA-BUF target is one-shot");
        if (timeout_ms == 0 || timeout_ms > kMaximumControlTimeoutMs) {
            fail("completion timeout must be in [1, 600000] ms");
        }
        if (metadata_exchange_ns == nullptr ||
            submit_to_completion_ns == nullptr ||
            transferred_bytes == nullptr) {
            fail("transfer output pointer is null");
        }
        if (source_server_name == nullptr || source_server_name[0] == '\0') {
            fail("source server name is empty");
        }
        native->target_used = true;
        const sockaddr_in peer_endpoint =
            parse_ipv4_endpoint(source_server_name, "source server name");
        const uint64_t metadata_start = monotonic_raw_ns();
        const uint64_t metadata_deadline = deadline_after_ms(timeout_ms);
        control = open_control_client(native->endpoint, peer_endpoint,
                                      metadata_deadline);
        const NativeWireInfo local = encode_native_wire(*context);
        NativeWireInfo raw_peer{};
        (void)write_all(control, &local, sizeof(local), metadata_deadline);
        (void)read_all(control, &raw_peer, sizeof(raw_peer), metadata_deadline);
        const NativePeer peer = decode_native_wire(
            raw_peer, SPD_GDR_DMABUF_SOURCE_V1, context->bytes);
        transition_native_qp(native, peer);
        *metadata_exchange_ns = monotonic_raw_ns() - metadata_start;

        ibv_sge sge{};
        sge.addr =
            static_cast<uint64_t>(reinterpret_cast<uintptr_t>(context->buffer));
        sge.length = static_cast<uint32_t>(context->bytes);
        sge.lkey = native->mr->lkey;
        ibv_send_wr request{};
        request.wr_id = reinterpret_cast<uintptr_t>(handle);
        request.sg_list = &sge;
        request.num_sge = 1;
        request.opcode = IBV_WR_RDMA_READ;
        request.send_flags = IBV_SEND_SIGNALED;
        request.wr.rdma.remote_addr = peer.address;
        request.wr.rdma.rkey = peer.rkey;
        ibv_send_wr *bad_request = nullptr;
        const uint64_t transfer_start = monotonic_raw_ns();
        context->transfer_active = true;
        if (ibv_post_send(native->qp, &request, &bad_request) != 0) {
            context->transfer_active = false;
            fail(std::string("ibv_post_send(RDMA_READ): ") +
                 std::strerror(errno));
        }
        const uint64_t timeout_ns = timeout_ms * 1000000ull;
        ibv_wc completion{};
        while (true) {
            const int count = ibv_poll_cq(native->cq, 1, &completion);
            if (count < 0) {
                context->poisoned = true;
                fail("ibv_poll_cq failed with an unretired RDMA READ");
            }
            if (count == 1) break;
            if (monotonic_raw_ns() - transfer_start >= timeout_ns) {
                context->poisoned = true;
                fail("native RDMA READ completion timed out");
            }
            _mm_pause();
        }
        *submit_to_completion_ns = monotonic_raw_ns() - transfer_start;
        context->transfer_active = false;
        if (completion.status != IBV_WC_SUCCESS ||
            completion.opcode != IBV_WC_RDMA_READ) {
            fail("native RDMA READ completed with status " +
                 std::to_string(completion.status) + " vendor_err " +
                 std::to_string(completion.vendor_err));
        }

        const auto flush_gdr_writes = reinterpret_cast<flush_gdr_writes_fn>(
            dlsym(RTLD_DEFAULT, "cuFlushGPUDirectRDMAWrites"));
        if (flush_gdr_writes == nullptr) {
            fail("cuFlushGPUDirectRDMAWrites is absent");
        }
        cu_check(flush_gdr_writes(kFlushTargetCurrentContext, kFlushScopeOwner),
                 "cuFlushGPUDirectRDMAWrites");
        cu_check(cuCtxSynchronize(), "cuCtxSynchronize after RDMA READ");
        *transferred_bytes = context->bytes;
        const uint32_t done = htonl(kNativeDoneMagic);
        const uint64_t acknowledgement_deadline = deadline_after_ms(timeout_ms);
        (void)write_all(control, &done, sizeof(done), acknowledgement_deadline);
        (void)close(control);
        control = -1;
        return 0;
    } catch (const std::exception &error) {
        if (control >= 0) (void)close(control);
        set_error(error);
        return context != nullptr && context->poisoned ? -2 : -1;
    }
}

}  // namespace

extern "C" uint32_t spd_gdr_abi_version() { return kAbiVersion; }

extern "C" const char *spd_gdr_last_error() { return last_error.c_str(); }

extern "C" void *spd_gdr_open(const char *local_server_name,
                              uint64_t device_pointer, uint64_t bytes,
                              int gpu_id) {
    last_error.clear();
#ifdef SPD_GDR_DIRECT_ONLY
    (void)local_server_name;
    (void)device_pointer;
    (void)bytes;
    (void)gpu_id;
    last_error =
        "Mooncake staging backend is absent from this direct-only build";
    return nullptr;
#else
    try {
        std::call_once(logging_once, [] {
            google::InitGoogleLogging("libspdgdr");
            FLAGS_logtostderr = 1;
        });
        if (local_server_name == nullptr) fail("local server name is null");
        if (bytes > std::numeric_limits<size_t>::max()) {
            fail("CUDA buffer size does not fit size_t");
        }
        validate_platform();
        auto context = std::make_shared<Context>();
        context->buffer =
            reinterpret_cast<void *>(static_cast<uintptr_t>(device_pointer));
        context->bytes = static_cast<size_t>(bytes);
        context->gpu_id = gpu_id;
        validate_pointer(context->buffer, context->bytes, context->gpu_id,
                         true);
        context->engine = make_engine(local_server_name);
        if (context->engine->registerLocalMemory(
                context->buffer, context->bytes, kMemoryLocation) != 0) {
            fail("registerLocalMemory failed");
        }
        context->registered = true;
        uintptr_t handle_id = next_handle_id.fetch_add(1);
        if (handle_id == 0) fail("GDR handle ID space exhausted");
        {
            std::lock_guard<std::mutex> lock(contexts_mutex);
            if (!contexts.emplace(handle_id, context).second) {
                fail("GDR handle ID collision");
            }
        }
        return reinterpret_cast<void *>(handle_id);
    } catch (const std::exception &error) {
        set_error(error);
        return nullptr;
    }
#endif
}

extern "C" void *spd_gdr_open_dmabuf_v1(const char *local_endpoint,
                                        uint64_t device_pointer, uint64_t bytes,
                                        int gpu_id, int role) {
    last_error.clear();
    std::shared_ptr<Context> context;
    try {
        if (role != SPD_GDR_DMABUF_SOURCE_V1 &&
            role != SPD_GDR_DMABUF_TARGET_V1) {
            fail("direct DMA-BUF role must be source=1 or target=2");
        }
        if (bytes > std::numeric_limits<size_t>::max() ||
            bytes > std::numeric_limits<uint32_t>::max()) {
            fail("direct DMA-BUF size must fit one RDMA SGE");
        }
        const sockaddr_in endpoint =
            parse_ipv4_endpoint(local_endpoint, "local endpoint");
        context = std::make_shared<Context>();
        context->backend = Backend::kDirectDmaBuf;
        context->buffer =
            reinterpret_cast<void *>(static_cast<uintptr_t>(device_pointer));
        context->bytes = static_cast<size_t>(bytes);
        context->gpu_id = gpu_id;
        validate_pointer(context->buffer, context->bytes, context->gpu_id,
                         false);
        make_native_resources(context.get(), endpoint, role);
        if (role == SPD_GDR_DMABUF_SOURCE_V1) {
            const int listener = open_control_listener(endpoint);
            context->native->listener_fd.store(listener,
                                               std::memory_order_release);
            context->native->control_thread =
                std::thread(serve_native_source, context.get());
        }
        context->registered = true;
        const uintptr_t handle_id = next_handle_id.fetch_add(1);
        if (handle_id == 0) fail("GDR handle ID space exhausted");
        {
            std::lock_guard<std::mutex> lock(contexts_mutex);
            if (!contexts.emplace(handle_id, context).second) {
                fail("GDR handle ID collision");
            }
        }
        return reinterpret_cast<void *>(handle_id);
    } catch (const std::exception &error) {
        cleanup_native_best_effort(context.get());
        set_error(error);
        return nullptr;
    }
}

#ifndef SPD_GDR_DIRECT_ONLY
int transfer_one(void *handle, const char *target_server_name,
                 uint64_t local_offset, uint64_t remote_offset,
                 uint64_t transfer_bytes, uint64_t timeout_ms,
                 uint64_t *metadata_exchange_ns,
                 uint64_t *submit_to_completion_ns, uint64_t *transferred_bytes,
                 mooncake::TransferRequest::OpCode opcode,
                 const char *operation_name) {
    last_error.clear();
    std::shared_ptr<Context> context;
    try {
        context = checked_context(handle);
        std::unique_lock<std::mutex> lock(context->mutex);
        if (context->magic != kContextMagic || !context->engine ||
            !context->registered) {
            fail("GDR context is invalid or closed");
        }
        if (context->poisoned) fail("GDR context is poisoned");
        if (context->transfer_active) fail("GDR transfer is already active");
        if (target_server_name == nullptr || target_server_name[0] == '\0') {
            fail("target server name is empty");
        }
        if (timeout_ms == 0 || timeout_ms > 600000) {
            fail("completion timeout must be in [1, 600000] ms");
        }
        if (metadata_exchange_ns == nullptr ||
            submit_to_completion_ns == nullptr ||
            transferred_bytes == nullptr) {
            fail("transfer output pointer is null");
        }
        if (transfer_bytes == 0) transfer_bytes = context->bytes;
        if (transfer_bytes == 0 || local_offset > context->bytes ||
            transfer_bytes > context->bytes - local_offset ||
            remote_offset > context->bytes ||
            transfer_bytes > context->bytes - remote_offset) {
            fail("transfer range exceeds the registered staging slab");
        }

        const uint64_t metadata_start = monotonic_raw_ns();
        const mooncake::SegmentID segment_id =
            context->engine->openSegment(target_server_name);
        const auto descriptor =
            context->engine->getMetadata()->getSegmentDescByID(segment_id);
        *metadata_exchange_ns = monotonic_raw_ns() - metadata_start;
        if (!descriptor || descriptor->protocol != kProtocol ||
            !descriptor_uses_required_device(*descriptor) ||
            descriptor->buffers.size() != 1 ||
            descriptor->buffers[0].name != kMemoryLocation ||
            descriptor->buffers[0].length != context->bytes ||
            descriptor->buffers[0].rkey.empty()) {
            fail("target is not one exact CUDA RDMA buffer on mlx5_0");
        }

        const mooncake::BatchID batch = context->engine->allocateBatchID(1);
        if (batch == mooncake::Transport::INVALID_BATCH_ID) {
            fail("allocateBatchID failed");
        }
        mooncake::TransferRequest request{};
        request.opcode = opcode;
        request.source = static_cast<char *>(context->buffer) + local_offset;
        request.target_id = segment_id;
        if (descriptor->buffers[0].addr >
            std::numeric_limits<uint64_t>::max() - remote_offset) {
            fail("remote staging offset overflows the address space");
        }
        request.target_offset = descriptor->buffers[0].addr + remote_offset;
        request.length = transfer_bytes;

        context->transfer_active = true;
        const uint64_t transfer_start = monotonic_raw_ns();
        const mooncake::Status submitted =
            context->engine->submitTransfer(batch, {request});
        if (!submitted.ok()) {
            const mooncake::Status freed = context->engine->freeBatchID(batch);
            context->transfer_active = false;
            if (!freed.ok()) context->poisoned = true;
            fail("submitTransfer failed: " + submitted.ToString());
        }
        const uint64_t timeout_ns = timeout_ms * 1000000ull;
        mooncake::TransferStatus status{};
        while (true) {
            const mooncake::Status queried =
                context->engine->getTransferStatus(batch, 0, status);
            if (!queried.ok()) {
                const mooncake::Status freed =
                    context->engine->freeBatchID(batch);
                context->transfer_active = false;
                if (!freed.ok()) context->poisoned = true;
                fail("getTransferStatus failed: " + queried.ToString());
            }
            // clang-format off
      if (status.s == mooncake::TransferStatusEnum::COMPLETED &&
                status.transferred_bytes == transfer_bytes) {
                // clang-format on
                break;
            }
            // Mooncake v0.3 publishes each 64 KiB slice's byte count before
            // its completion count.  A status snapshot can therefore report
            // COMPLETED while the byte counter is still one slice behind.
            if (status.s == mooncake::TransferStatusEnum::COMPLETED &&
                status.transferred_bytes > transfer_bytes) {
                const mooncake::Status freed =
                    context->engine->freeBatchID(batch);
                context->transfer_active = false;
                if (!freed.ok()) context->poisoned = true;
                fail(std::string("RDMA ") + operation_name +
                     " completion byte count exceeds the requested range");
            }
            if (status.s != mooncake::TransferStatusEnum::WAITING &&
                status.s != mooncake::TransferStatusEnum::PENDING &&
                status.s != mooncake::TransferStatusEnum::COMPLETED) {
                const mooncake::Status freed =
                    context->engine->freeBatchID(batch);
                context->transfer_active = false;
                if (!freed.ok()) context->poisoned = true;
                fail(std::string("RDMA ") + operation_name +
                     " entered a non-completable status");
            }
            if (monotonic_raw_ns() - transfer_start >= timeout_ns) {
                const mooncake::Status freed =
                    context->engine->freeBatchID(batch);
                context->transfer_active = false;
                if (!freed.ok()) context->poisoned = true;
                fail(std::string("RDMA ") + operation_name +
                     " completion or byte accounting timed out");
            }
            _mm_pause();
        }
        *submit_to_completion_ns = monotonic_raw_ns() - transfer_start;
        *transferred_bytes = status.transferred_bytes;
        const mooncake::Status freed = context->engine->freeBatchID(batch);
        context->transfer_active = false;
        if (!freed.ok()) {
            context->poisoned = true;
            fail("freeBatchID failed: " + freed.ToString());
        }
        if (status.transferred_bytes != transfer_bytes) {
            fail("completion byte count differs from source buffer size");
        }
        return 0;
    } catch (const std::exception &error) {
        set_error(error);
        return context != nullptr && context->poisoned ? -2 : -1;
    }
}
#endif

extern "C" int spd_gdr_transfer(void *handle, const char *target_server_name,
                                uint64_t timeout_ms,
                                uint64_t *metadata_exchange_ns,
                                uint64_t *submit_to_completion_ns,
                                uint64_t *transferred_bytes) {
#ifdef SPD_GDR_DIRECT_ONLY
    (void)handle;
    (void)target_server_name;
    (void)timeout_ms;
    (void)metadata_exchange_ns;
    (void)submit_to_completion_ns;
    (void)transferred_bytes;
    last_error =
        "Mooncake staging backend is absent from this direct-only build";
    return -1;
#else
    return transfer_one(handle, target_server_name, 0, 0, 0, timeout_ms,
                        metadata_exchange_ns, submit_to_completion_ns,
                        transferred_bytes, mooncake::TransferRequest::WRITE,
                        "WRITE");
#endif
}

extern "C" int spd_gdr_read(void *handle, const char *source_server_name,
                            uint64_t timeout_ms, uint64_t *metadata_exchange_ns,
                            uint64_t *submit_to_completion_ns,
                            uint64_t *transferred_bytes) {
    last_error.clear();
    try {
        const std::shared_ptr<Context> context = checked_context(handle);
        if (context->backend == Backend::kDirectDmaBuf) {
            return direct_read(handle, source_server_name, timeout_ms,
                               metadata_exchange_ns, submit_to_completion_ns,
                               transferred_bytes);
        }
    } catch (const std::exception &error) {
        set_error(error);
        return -1;
    }
#ifdef SPD_GDR_DIRECT_ONLY
    last_error =
        "Mooncake staging backend is absent from this direct-only build";
    return -1;
#else
    return transfer_one(handle, source_server_name, 0, 0, 0, timeout_ms,
                        metadata_exchange_ns, submit_to_completion_ns,
                        transferred_bytes, mooncake::TransferRequest::READ,
                        "READ");
#endif
}

extern "C" int spd_gdr_read_at_v1(void *handle, const char *source_server_name,
                                  uint64_t local_offset, uint64_t remote_offset,
                                  uint64_t bytes, uint64_t timeout_ms,
                                  uint64_t *metadata_exchange_ns,
                                  uint64_t *submit_to_completion_ns,
                                  uint64_t *transferred_bytes) {
#ifdef SPD_GDR_DIRECT_ONLY
    (void)handle;
    (void)source_server_name;
    (void)local_offset;
    (void)remote_offset;
    (void)bytes;
    (void)timeout_ms;
    (void)metadata_exchange_ns;
    (void)submit_to_completion_ns;
    (void)transferred_bytes;
    last_error =
        "Mooncake staging backend is absent from this direct-only build";
    return -1;
#else
    return transfer_one(handle, source_server_name, local_offset, remote_offset,
                        bytes, timeout_ms, metadata_exchange_ns,
                        submit_to_completion_ns, transferred_bytes,
                        mooncake::TransferRequest::READ, "READ");
#endif
}

extern "C" int spd_gdr_close(void *handle) {
    last_error.clear();
    if (handle == nullptr) return 0;
    std::shared_ptr<Context> context;
    try {
        context = checked_context(handle);
        std::unique_lock<std::mutex> lock(context->mutex);
#ifdef SPD_GDR_DIRECT_ONLY
        if (context->magic != kContextMagic ||
            context->backend != Backend::kDirectDmaBuf || !context->native ||
            !context->registered) {
            fail("GDR context is invalid or closed");
        }
#else
        if (context->backend == Backend::kDirectDmaBuf) {
            if (context->magic != kContextMagic || !context->native ||
                !context->registered) {
                fail("GDR context is invalid or closed");
            }
        } else if (context->magic != kContextMagic || !context->engine ||
                   !context->registered) {
            fail("GDR context is invalid or closed");
        }
#endif
        if (context->backend == Backend::kDirectDmaBuf) {
            if (context->poisoned || context->transfer_active) {
                last_error =
                    "GDR context is poisoned or has an unretired transfer";
                return -2;
            }
            context->closing = true;
            lock.unlock();
            stop_native_thread(context.get());
            lock.lock();
            std::string control_error;
            {
                std::lock_guard<std::mutex> thread_lock(
                    context->native->thread_mutex);
                control_error = context->native->thread_error;
            }
            const std::string cleanup_error =
                cleanup_native_strict(context.get());
            if (!cleanup_error.empty()) {
                context->poisoned = true;
                last_error = cleanup_error;
                return -2;
            }
            context->registered = false;
            context->native.reset();
            context->magic = 0;
            {
                std::lock_guard<std::mutex> contexts_lock(contexts_mutex);
                contexts.erase(reinterpret_cast<uintptr_t>(handle));
            }
            if (!control_error.empty()) {
                last_error = "native source control failed: " + control_error;
                return -1;
            }
            return 0;
        }
#ifndef SPD_GDR_DIRECT_ONLY
        if (context->magic != kContextMagic || !context->engine ||
            !context->registered) {
            fail("GDR context is invalid or closed");
        }
        if (context->poisoned || context->transfer_active) {
            last_error = "GDR context is poisoned or has an unretired transfer";
            return -2;
        }
        int result = 0;
        if (context->registered && context->engine) {
            result = context->engine->unregisterLocalMemory(context->buffer);
            if (result != 0) {
                last_error = "unregisterLocalMemory failed";
                context->poisoned = true;
                return -2;
            }
        }
        context->registered = false;
        context->engine.reset();
        context->magic = 0;
        {
            std::lock_guard<std::mutex> contexts_lock(contexts_mutex);
            contexts.erase(reinterpret_cast<uintptr_t>(handle));
        }
        return 0;
#else
        fail("Mooncake staging backend is absent from this direct-only build");
#endif
    } catch (const std::exception &error) {
        set_error(error);
        if (context != nullptr) {
            context->poisoned = true;
            return -2;
        }
        return -1;
    }
}
