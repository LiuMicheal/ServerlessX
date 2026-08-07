// Copyright 2026 ServerlessPD Artifact Authors
// SPDX-License-Identifier: Apache-2.0

// Minimal CUDA VMM -> DMA-BUF -> ibv_reg_dmabuf_mr -> RC RDMA Write probe.
// The two endpoints run the same binary.  The client owns the source VMM
// region, the server owns the destination VMM region, and the server checks
// the bytes after the client's RDMA completion notification.  No legacy
// cudaMalloc buffer is used by the data path.

#include <cuda.h>
#include <infiniband/verbs.h>

#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sched.h>
#include <time.h>

#include <algorithm>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr size_t kRequestedBytes = 2ull * 1024ull * 1024ull;
constexpr unsigned int kVmmSupportedAttribute = 102;
constexpr unsigned int kVmmGdrSupportedAttribute = 110;
constexpr unsigned int kDmaBufSupportedAttribute = 124;
constexpr unsigned int kDmaBufFdHandleType = 1;
constexpr uint32_t kWireMagic = 0x53504447u;  // "SPDG"
constexpr uint16_t kWireVersion = 1;
constexpr int kDefaultCudaDevice = 0;
constexpr int kDefaultIbPort = 1;
constexpr int kDefaultGidIndex = 3;

using get_handle_for_range_fn = CUresult (*)(
    void *, CUdeviceptr, size_t, unsigned int, unsigned long long);
using reg_dmabuf_mr_fn = ibv_mr *(*)(ibv_pd *, uint64_t, size_t, uint64_t,
                                     int, int);
using flush_gdr_writes_fn = CUresult (*)(int, int);

constexpr int kFlushTargetCurrentContext = 0;
constexpr int kFlushScopeOwner = 100;

[[noreturn]] void fail(const std::string &message) {
    throw std::runtime_error(message);
}

void cuda_check(CUresult result, const char *operation) {
    if (result == CUDA_SUCCESS) return;
    const char *name = "unknown";
    const char *description = "unknown";
    (void)cuGetErrorName(result, &name);
    (void)cuGetErrorString(result, &description);
    fail(std::string(operation) + " failed: " + name + " (" + description + ")");
}

void verbs_check(bool condition, const char *operation) {
    if (!condition) fail(std::string(operation) + " failed: " + std::strerror(errno));
}

uint64_t monotonic_ns() {
    timespec value{};
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &value) != 0) {
        fail(std::string("clock_gettime failed: ") + std::strerror(errno));
    }
    return static_cast<uint64_t>(value.tv_sec) * 1000000000ull +
           static_cast<uint64_t>(value.tv_nsec);
}

uint64_t htonll(uint64_t value) {
    static const int one = 1;
    if (*reinterpret_cast<const char *>(&one) == 1) {
        return (static_cast<uint64_t>(htonl(static_cast<uint32_t>(value))) << 32) |
               htonl(static_cast<uint32_t>(value >> 32));
    }
    return value;
}

uint64_t ntohll(uint64_t value) { return htonll(value); }

void write_all(int fd, const void *buffer, size_t bytes) {
    const auto *cursor = static_cast<const uint8_t *>(buffer);
    while (bytes != 0) {
        const ssize_t written = send(fd, cursor, bytes, MSG_NOSIGNAL);
        if (written < 0) {
            if (errno == EINTR) continue;
            fail(std::string("send failed: ") + std::strerror(errno));
        }
        if (written == 0) fail("send returned zero");
        cursor += written;
        bytes -= static_cast<size_t>(written);
    }
}

void read_all(int fd, void *buffer, size_t bytes) {
    auto *cursor = static_cast<uint8_t *>(buffer);
    while (bytes != 0) {
        const ssize_t received = recv(fd, cursor, bytes, 0);
        if (received < 0) {
            if (errno == EINTR) continue;
            fail(std::string("recv failed: ") + std::strerror(errno));
        }
        if (received == 0) fail("peer closed control connection");
        cursor += received;
        bytes -= static_cast<size_t>(received);
    }
}

uint8_t pattern_byte(size_t index) {
    return static_cast<uint8_t>((index * 131ull + 17ull) & 0xffull);
}

uint64_t fnv1a64(const uint8_t *data, size_t bytes) {
    uint64_t value = 1469598103934665603ull;
    for (size_t index = 0; index < bytes; ++index) {
        value ^= data[index];
        value *= 1099511628211ull;
    }
    return value;
}

std::vector<uint8_t> make_pattern(size_t bytes) {
    std::vector<uint8_t> pattern(bytes);
    for (size_t index = 0; index < bytes; ++index) pattern[index] = pattern_byte(index);
    return pattern;
}

struct VmmAllocation {
    CUdeviceptr address = 0;
    size_t bytes = 0;
    size_t granularity = 0;
    CUmemGenericAllocationHandle handle = 0;
    int dmabuf_fd = -1;
};

struct Capabilities {
    int vmm = 0;
    int dma_buf = 0;
    int vmm_gdr = 0;
};

Capabilities query_capabilities(int device_id) {
    cuda_check(cuInit(0), "cuInit");
    CUdevice device = 0;
    cuda_check(cuDeviceGet(&device, device_id), "cuDeviceGet");
    Capabilities value{};
    cuda_check(cuDeviceGetAttribute(
                   &value.vmm,
                   static_cast<CUdevice_attribute>(kVmmSupportedAttribute), device),
               "cuDeviceGetAttribute(VMM_SUPPORTED)");
    cuda_check(cuDeviceGetAttribute(
                   &value.dma_buf,
                   static_cast<CUdevice_attribute>(kDmaBufSupportedAttribute), device),
               "cuDeviceGetAttribute(DMA_BUF_SUPPORTED)");
    cuda_check(cuDeviceGetAttribute(
                   &value.vmm_gdr,
                   static_cast<CUdevice_attribute>(kVmmGdrSupportedAttribute), device),
               "cuDeviceGetAttribute(GPU_DIRECT_RDMA_WITH_CUDA_VMM)");
    if (value.vmm != 1 || value.dma_buf != 1 || value.vmm_gdr != 1) {
        fail("CUDA VMM DMA-BUF GDR capabilities are incomplete");
    }
    return value;
}

VmmAllocation make_vmm(int device_id, size_t requested_bytes) {
    cuda_check(cuInit(0), "cuInit");
    CUdevice device = 0;
    cuda_check(cuDeviceGet(&device, device_id), "cuDeviceGet");
    size_t granularity = 0;
    CUmemAllocationProp query{};
    query.type = CU_MEM_ALLOCATION_TYPE_PINNED;
    query.location.type = CU_MEM_LOCATION_TYPE_DEVICE;
    query.location.id = device_id;
    cuda_check(cuMemGetAllocationGranularity(
                   &granularity, &query, CU_MEM_ALLOC_GRANULARITY_MINIMUM),
               "cuMemGetAllocationGranularity");
    const size_t bytes =
        ((requested_bytes + granularity - 1) / granularity) * granularity;
    CUmemAllocationProp prop = query;
    prop.allocFlags.gpuDirectRDMACapable = 1;
    CUmemGenericAllocationHandle handle = 0;
    cuda_check(cuMemCreate(&handle, bytes, &prop, 0), "cuMemCreate");
    CUdeviceptr address = 0;
    try {
        cuda_check(cuMemAddressReserve(&address, bytes, granularity, 0, 0),
                   "cuMemAddressReserve");
        cuda_check(cuMemMap(address, bytes, 0, handle, 0), "cuMemMap");
        CUmemAccessDesc access{};
        access.location.type = CU_MEM_LOCATION_TYPE_DEVICE;
        access.location.id = device_id;
        access.flags = CU_MEM_ACCESS_FLAGS_PROT_READWRITE;
        cuda_check(cuMemSetAccess(address, bytes, &access, 1), "cuMemSetAccess");
        const auto get_handle = reinterpret_cast<get_handle_for_range_fn>(
            dlsym(RTLD_DEFAULT, "cuMemGetHandleForAddressRange"));
        if (get_handle == nullptr) fail("cuMemGetHandleForAddressRange is absent");
        int dmabuf_fd = -1;
        cuda_check(get_handle(&dmabuf_fd, address, bytes, kDmaBufFdHandleType, 0),
                   "cuMemGetHandleForAddressRange(DMA_BUF_FD)");
        return {address, bytes, granularity, handle, dmabuf_fd};
    } catch (...) {
        if (address != 0) (void)cuMemAddressFree(address, bytes);
        (void)cuMemRelease(handle);
        throw;
    }
}

void destroy_vmm(VmmAllocation *value) {
    if (value == nullptr) return;
    if (value->dmabuf_fd >= 0) close(value->dmabuf_fd);
    if (value->address != 0) {
        (void)cuMemUnmap(value->address, value->bytes);
        (void)cuMemAddressFree(value->address, value->bytes);
    }
    if (value->handle != 0) (void)cuMemRelease(value->handle);
    *value = {};
}

struct WireInfo {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    uint32_t qpn;
    uint32_t psn;
    uint32_t rkey;
    uint32_t bytes;
    uint64_t address;
    uint8_t gid[16];
};

WireInfo encode_wire(uint32_t qpn, uint32_t psn, uint32_t rkey, size_t bytes,
                     uint64_t address, const ibv_gid &gid) {
    WireInfo value{};
    value.magic = htonl(kWireMagic);
    value.version = htons(kWireVersion);
    value.qpn = htonl(qpn);
    value.psn = htonl(psn);
    value.rkey = htonl(rkey);
    value.bytes = htonl(static_cast<uint32_t>(bytes));
    value.address = htonll(address);
    std::memcpy(value.gid, gid.raw, sizeof(value.gid));
    return value;
}

WireInfo decode_wire(const WireInfo &wire) {
    if (ntohl(wire.magic) != kWireMagic || ntohs(wire.version) != kWireVersion) {
        fail("invalid peer wire header");
    }
    return wire;
}

int open_control_server(uint16_t port) {
    const int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) fail(std::string("socket failed: ") + std::strerror(errno));
    int yes = 1;
    (void)setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);
    if (bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 ||
        listen(listener, 1) != 0) {
        const int saved = errno;
        close(listener);
        fail(std::string("bind/listen failed: ") + std::strerror(saved));
    }
    const int connection = accept(listener, nullptr, nullptr);
    const int saved = errno;
    close(listener);
    if (connection < 0) fail(std::string("accept failed: ") + std::strerror(saved));
    return connection;
}

int open_control_client(const char *peer, uint16_t port) {
    const int connection = socket(AF_INET, SOCK_STREAM, 0);
    if (connection < 0) fail(std::string("socket failed: ") + std::strerror(errno));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (inet_pton(AF_INET, peer, &address.sin_addr) != 1) {
        close(connection);
        fail(std::string("peer is not an IPv4 address: ") + peer);
    }
    if (connect(connection, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
        const int saved = errno;
        close(connection);
        fail(std::string("connect failed: ") + std::strerror(saved));
    }
    return connection;
}

struct RdmaResources {
    ibv_context *context = nullptr;
    ibv_pd *pd = nullptr;
    ibv_cq *cq = nullptr;
    ibv_qp *qp = nullptr;
    ibv_mr *mr = nullptr;
    ibv_device **device_list = nullptr;
};

void destroy_rdma(RdmaResources *resources) {
    if (resources == nullptr) return;
    if (resources->qp != nullptr) (void)ibv_destroy_qp(resources->qp);
    if (resources->cq != nullptr) (void)ibv_destroy_cq(resources->cq);
    if (resources->mr != nullptr) (void)ibv_dereg_mr(resources->mr);
    if (resources->pd != nullptr) (void)ibv_dealloc_pd(resources->pd);
    if (resources->context != nullptr) (void)ibv_close_device(resources->context);
    if (resources->device_list != nullptr) ibv_free_device_list(resources->device_list);
    *resources = {};
}

RdmaResources make_rdma(const char *requested_device, VmmAllocation *vmm,
                        bool destination) {
    RdmaResources resources{};
    int count = 0;
    resources.device_list = ibv_get_device_list(&count);
    if (resources.device_list == nullptr) fail("ibv_get_device_list failed");
    ibv_device *selected = nullptr;
    for (int index = 0; index < count; ++index) {
        if (requested_device == nullptr ||
            std::strcmp(ibv_get_device_name(resources.device_list[index]),
                        requested_device) == 0) {
            selected = resources.device_list[index];
            break;
        }
    }
    if (selected == nullptr) fail("requested RDMA device was not found");
    resources.context = ibv_open_device(selected);
    if (resources.context == nullptr) fail("ibv_open_device failed");
    resources.pd = ibv_alloc_pd(resources.context);
    if (resources.pd == nullptr) fail("ibv_alloc_pd failed");
    resources.cq = ibv_create_cq(resources.context, 2, nullptr, nullptr, 0);
    if (resources.cq == nullptr) fail("ibv_create_cq failed");
    ibv_qp_init_attr init{};
    init.send_cq = resources.cq;
    init.recv_cq = resources.cq;
    init.qp_type = IBV_QPT_RC;
    init.cap.max_send_wr = 2;
    init.cap.max_recv_wr = 1;
    init.cap.max_send_sge = 1;
    init.cap.max_recv_sge = 1;
    resources.qp = ibv_create_qp(resources.pd, &init);
    if (resources.qp == nullptr) fail("ibv_create_qp failed");
    const int access = IBV_ACCESS_LOCAL_WRITE |
                       (destination ? IBV_ACCESS_REMOTE_WRITE : 0);
    const auto register_dmabuf = reinterpret_cast<reg_dmabuf_mr_fn>(
        dlsym(RTLD_DEFAULT, "ibv_reg_dmabuf_mr"));
    if (register_dmabuf == nullptr) fail("ibv_reg_dmabuf_mr is absent");
    resources.mr = register_dmabuf(resources.pd, 0, vmm->bytes,
                                    static_cast<uint64_t>(vmm->address),
                                    vmm->dmabuf_fd, access);
    if (resources.mr == nullptr) {
        fail(std::string("ibv_reg_dmabuf_mr failed: ") + std::strerror(errno));
    }
    return resources;
}

ibv_gid query_gid(ibv_context *context, int port, int gid_index) {
    ibv_gid gid{};
    verbs_check(ibv_query_gid(context, port, gid_index, &gid) == 0,
                "ibv_query_gid");
    return gid;
}

void transition_qp(ibv_qp *qp, int port, int gid_index, const ibv_gid &peer_gid,
                  uint32_t peer_qpn, uint32_t peer_psn, uint32_t local_psn,
                  bool allow_remote_write) {
    ibv_qp_attr init{};
    init.qp_state = IBV_QPS_INIT;
    init.pkey_index = 0;
    init.port_num = static_cast<uint8_t>(port);
    init.qp_access_flags = IBV_ACCESS_LOCAL_WRITE |
                           (allow_remote_write ? IBV_ACCESS_REMOTE_WRITE : 0);
    verbs_check(ibv_modify_qp(qp, &init,
                               IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT |
                                   IBV_QP_ACCESS_FLAGS) == 0,
                "ibv_modify_qp(INIT)");

    ibv_qp_attr ready{};
    ready.qp_state = IBV_QPS_RTR;
    ready.path_mtu = IBV_MTU_4096;
    ready.dest_qp_num = peer_qpn;
    ready.rq_psn = peer_psn;
    ready.max_dest_rd_atomic = 1;
    ready.min_rnr_timer = 12;
    ready.ah_attr.is_global = 1;
    ready.ah_attr.dlid = 0;
    ready.ah_attr.sl = 0;
    ready.ah_attr.src_path_bits = 0;
    ready.ah_attr.port_num = static_cast<uint8_t>(port);
    ready.ah_attr.grh.dgid = peer_gid;
    ready.ah_attr.grh.sgid_index = static_cast<uint8_t>(gid_index);
    ready.ah_attr.grh.hop_limit = 1;
    verbs_check(ibv_modify_qp(qp, &ready,
                               IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
                                   IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
                                   IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER) == 0,
                "ibv_modify_qp(RTR)");

    ibv_qp_attr running{};
    running.qp_state = IBV_QPS_RTS;
    running.sq_psn = local_psn;
    running.timeout = 14;
    running.retry_cnt = 7;
    running.rnr_retry = 7;
    running.max_rd_atomic = 1;
    verbs_check(ibv_modify_qp(qp, &running,
                               IBV_QP_STATE | IBV_QP_SQ_PSN | IBV_QP_TIMEOUT |
                                   IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY |
                                   IBV_QP_MAX_QP_RD_ATOMIC) == 0,
                "ibv_modify_qp(RTS)");
}

ibv_wc poll_completion(ibv_cq *cq) {
    ibv_wc completion{};
    for (;;) {
        const int count = ibv_poll_cq(cq, 1, &completion);
        if (count < 0) fail("ibv_poll_cq failed");
        if (count == 1) return completion;
        sched_yield();
    }
}

struct Options {
    std::string role;
    std::string peer;
    std::string device = "mlx5_0";
    int cuda_device = kDefaultCudaDevice;
    int ib_port = kDefaultIbPort;
    int gid_index = kDefaultGidIndex;
    uint16_t control_port = 23140;
    size_t bytes = kRequestedBytes;
};

Options parse_options(int argc, char **argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        auto next = [&](const char *name) -> const char * {
            if (index + 1 >= argc) fail(std::string("missing value for ") + name);
            return argv[++index];
        };
        if (argument == "--role") options.role = next("--role");
        else if (argument == "--peer") options.peer = next("--peer");
        else if (argument == "--device") options.device = next("--device");
        else if (argument == "--cuda-device") options.cuda_device = std::atoi(next("--cuda-device"));
        else if (argument == "--ib-port") options.ib_port = std::atoi(next("--ib-port"));
        else if (argument == "--gid-index") options.gid_index = std::atoi(next("--gid-index"));
        else if (argument == "--control-port") options.control_port = static_cast<uint16_t>(std::atoi(next("--control-port")));
        else if (argument == "--bytes") options.bytes = std::strtoull(next("--bytes"), nullptr, 10);
        else fail("unknown argument: " + argument);
    }
    if (options.role != "server" && options.role != "client") {
        fail("usage: --role server|client [--peer IPV4] [--control-port PORT]");
    }
    if (options.role == "client" && options.peer.empty()) fail("client requires --peer");
    if (options.bytes == 0 || options.bytes > (1ull << 30)) fail("bytes out of range");
    return options;
}

int run(const Options &options) {
    cuda_check(cuInit(0), "cuInit");
    CUdevice cuda_device = 0;
    cuda_check(cuDeviceGet(&cuda_device, options.cuda_device), "cuDeviceGet");
    CUcontext cuda_context = nullptr;
    cuda_check(cuDevicePrimaryCtxRetain(&cuda_context, cuda_device),
               "cuDevicePrimaryCtxRetain");
    cuda_check(cuCtxSetCurrent(cuda_context), "cuCtxSetCurrent");
    const Capabilities capabilities = query_capabilities(options.cuda_device);
    const std::vector<uint8_t> expected = make_pattern(options.bytes);
    VmmAllocation vmm = make_vmm(options.cuda_device, options.bytes);
    RdmaResources rdma{};
    int control = -1;
    try {
        const bool server = options.role == "server";
        if (!server) {
            cuda_check(cuMemcpyHtoD(vmm.address, expected.data(), expected.size()),
                       "cuMemcpyHtoD source VMM");
            cuda_check(cuCtxSynchronize(), "source CUDA synchronize");
        } else {
            std::vector<uint8_t> sentinel(options.bytes, 0xa5);
            cuda_check(cuMemcpyHtoD(vmm.address, sentinel.data(), sentinel.size()),
                       "cuMemcpyHtoD destination sentinel");
            cuda_check(cuCtxSynchronize(), "destination CUDA synchronize");
        }
        rdma = make_rdma(options.device.c_str(), &vmm, server);
        const ibv_gid local_gid = query_gid(rdma.context, options.ib_port,
                                             options.gid_index);
        std::random_device random;
        const uint32_t local_psn = random() & 0x00ffffffu;
        const WireInfo local = encode_wire(rdma.qp->qp_num, local_psn,
                                            rdma.mr->rkey, vmm.bytes,
                                            static_cast<uint64_t>(vmm.address), local_gid);
        control = server ? open_control_server(options.control_port)
                         : open_control_client(options.peer.c_str(), options.control_port);
        write_all(control, &local, sizeof(local));
        WireInfo raw_peer{};
        read_all(control, &raw_peer, sizeof(raw_peer));
        const WireInfo peer = decode_wire(raw_peer);
        if (ntohl(peer.bytes) != vmm.bytes) fail("peer VMM region size differs");
        ibv_gid peer_gid{};
        std::memcpy(peer_gid.raw, peer.gid, sizeof(peer_gid.raw));
        transition_qp(rdma.qp, options.ib_port, options.gid_index, peer_gid,
                      ntohl(peer.qpn), ntohl(peer.psn), local_psn, server);

        std::printf(
            "{\"event\":\"vmm_dmabuf_ready\",\"status\":\"pass\","
            "\"role\":\"%s\",\"bytes\":%zu,\"granularity\":%zu,"
            "\"cuda_address\":\"0x%" PRIx64 "\",\"dmabuf\":true,"
            "\"ibv_reg_dmabuf_mr\":true,\"vmm\":%d,\"dma_buf\":%d,"
            "\"vmm_gdr\":%d,\"qpn\":%u}\n",
            server ? "server" : "client", vmm.bytes, vmm.granularity,
            static_cast<uint64_t>(vmm.address), capabilities.vmm,
            capabilities.dma_buf, capabilities.vmm_gdr, rdma.qp->qp_num);
        std::fflush(stdout);

        if (server) {
            uint8_t done = 0;
            read_all(control, &done, sizeof(done));
            if (done != 1) fail("client did not acknowledge RDMA completion");
            const auto flush_gdr_writes = reinterpret_cast<flush_gdr_writes_fn>(
                dlsym(RTLD_DEFAULT, "cuFlushGPUDirectRDMAWrites"));
            if (flush_gdr_writes == nullptr) {
                fail("cuFlushGPUDirectRDMAWrites is absent");
            }
            cuda_check(flush_gdr_writes(kFlushTargetCurrentContext,
                                        kFlushScopeOwner),
                       "cuFlushGPUDirectRDMAWrites");
            cuda_check(cuCtxSynchronize(), "destination CUDA synchronize after RDMA");
            std::vector<uint8_t> observed(options.bytes);
            const uint64_t copy_start = monotonic_ns();
            cuda_check(cuMemcpyDtoH(observed.data(), vmm.address, observed.size()),
                       "cuMemcpyDtoH destination VMM");
            cuda_check(cuCtxSynchronize(), "destination checksum synchronize");
            const uint64_t copy_ns = monotonic_ns() - copy_start;
            const uint64_t expected_checksum = fnv1a64(expected.data(), expected.size());
            const uint64_t observed_checksum = fnv1a64(observed.data(), observed.size());
            size_t mismatches = 0;
            for (size_t index = 0; index < observed.size(); ++index) {
                if (observed[index] != expected[index]) ++mismatches;
            }
            if (observed_checksum != expected_checksum || mismatches != 0) {
                fail("destination VMM checksum mismatch");
            }
            std::printf(
                "{\"event\":\"checksum\",\"status\":\"pass\","
                "\"role\":\"server\",\"bytes\":%zu,"
                "\"expected_checksum\":%" PRIu64 ","
                "\"observed_checksum\":%" PRIu64 ",\"mismatches\":%zu,"
                "\"destination_d2h_ns\":%" PRIu64 ",\"gdr_flush\":true}\n",
                observed.size(), expected_checksum, observed_checksum, mismatches, copy_ns);
        } else {
            ibv_sge sge{};
            sge.addr = static_cast<uint64_t>(vmm.address);
            sge.length = static_cast<uint32_t>(vmm.bytes);
            sge.lkey = rdma.mr->lkey;
            ibv_send_wr wr{};
            wr.wr_id = 1;
            wr.sg_list = &sge;
            wr.num_sge = 1;
            wr.opcode = IBV_WR_RDMA_WRITE;
            wr.send_flags = IBV_SEND_SIGNALED;
            wr.wr.rdma.remote_addr = ntohll(peer.address);
            wr.wr.rdma.rkey = ntohl(peer.rkey);
            ibv_send_wr *bad = nullptr;
            const uint64_t start = monotonic_ns();
            verbs_check(ibv_post_send(rdma.qp, &wr, &bad) == 0, "ibv_post_send");
            const ibv_wc completion = poll_completion(rdma.cq);
            const uint64_t completion_ns = monotonic_ns() - start;
            if (completion.status != IBV_WC_SUCCESS) {
                fail(std::string("RDMA completion failed status=") +
                     std::to_string(completion.status) + " vendor_err=" +
                     std::to_string(completion.vendor_err));
            }
            const uint8_t done = 1;
            write_all(control, &done, sizeof(done));
            std::printf(
                "{\"event\":\"rdma_write\",\"status\":\"pass\","
                "\"role\":\"client\",\"bytes\":%zu,"
                "\"rdma_completion_ns\":%" PRIu64 ","
                "\"source_checksum\":%" PRIu64 "}\n",
                vmm.bytes, completion_ns, fnv1a64(expected.data(), expected.size()));
        }
        std::fflush(stdout);
        close(control);
        destroy_rdma(&rdma);
        destroy_vmm(&vmm);
        cuda_check(cuCtxSetCurrent(nullptr), "cuCtxSetCurrent(nullptr)");
        cuda_check(cuDevicePrimaryCtxRelease(cuda_device),
                   "cuDevicePrimaryCtxRelease");
        return EXIT_SUCCESS;
    } catch (...) {
        if (control >= 0) close(control);
        destroy_rdma(&rdma);
        destroy_vmm(&vmm);
        (void)cuCtxSetCurrent(nullptr);
        (void)cuDevicePrimaryCtxRelease(cuda_device);
        throw;
    }
}

}  // namespace

int main(int argc, char **argv) {
    try {
        return run(parse_options(argc, argv));
    } catch (const std::exception &error) {
        std::fprintf(stderr, "{\"event\":\"failure\",\"status\":\"fail\",\"error\":\"%s\"}\n",
                     error.what());
        return EXIT_FAILURE;
    }
}
