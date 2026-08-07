// Copyright 2026 ServerlessPD Artifact Authors
// SPDX-License-Identifier: Apache-2.0

#include "serverlessx/spd_gdr.h"

#include <cuda.h>
#include <cuda_runtime.h>

#include <signal.h>
#include <time.h>
#include <errno.h>

#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr size_t kRequestedBytes = 2ull * 1024ull * 1024ull;
constexpr int kCudaDevice = 0;
constexpr unsigned int kVmmSupportedAttribute = 102;
constexpr unsigned int kDmaBufSupportedAttribute = 124;
constexpr unsigned int kVmmGdrSupportedAttribute = 110;

volatile sig_atomic_t stop_requested = 0;

struct VmmAllocation {
    CUdeviceptr address = 0;
    size_t bytes = 0;
    size_t granularity = 0;
    CUmemGenericAllocationHandle allocation = 0;
};

[[noreturn]] void fail(const std::string &message) {
    throw std::runtime_error(message);
}

void cuda_driver_check(CUresult result, const char *operation) {
    if (result == CUDA_SUCCESS) return;
    const char *name = "unknown";
    const char *description = "unknown";
    (void)cuGetErrorName(result, &name);
    (void)cuGetErrorString(result, &description);
    fail(std::string(operation) + " failed: " + name + " (" + description + ")");
}

void cuda_runtime_check(cudaError_t result, const char *operation) {
    if (result != cudaSuccess) {
        fail(std::string(operation) + " failed: " + cudaGetErrorString(result));
    }
}

uint64_t monotonic_raw_ns() {
    struct timespec value {};
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &value) != 0) {
        fail(std::string("clock_gettime failed: ") + std::strerror(errno));
    }
    return static_cast<uint64_t>(value.tv_sec) * 1000000000ull +
           static_cast<uint64_t>(value.tv_nsec);
}

uint64_t fnv1a64(const uint8_t *data, size_t bytes) {
    uint64_t value = 1469598103934665603ull;
    for (size_t index = 0; index < bytes; ++index) {
        value ^= data[index];
        value *= 1099511628211ull;
    }
    return value;
}

uint8_t pattern_byte(size_t index) {
    return static_cast<uint8_t>((index * 131ull + 17ull) & 0xffull);
}

std::vector<uint8_t> expected_pattern(size_t bytes) {
    std::vector<uint8_t> value(bytes);
    for (size_t index = 0; index < bytes; ++index) {
        value[index] = pattern_byte(index);
    }
    return value;
}

VmmAllocation make_vmm_allocation() {
    cuda_driver_check(cuInit(0), "cuInit");
    CUdevice device = 0;
    cuda_driver_check(cuDeviceGet(&device, kCudaDevice), "cuDeviceGet");

    size_t granularity = 0;
    CUmemAllocationProp query_prop{};
    query_prop.type = CU_MEM_ALLOCATION_TYPE_PINNED;
    query_prop.location.type = CU_MEM_LOCATION_TYPE_DEVICE;
    query_prop.location.id = kCudaDevice;
    cuda_driver_check(cuMemGetAllocationGranularity(
                          &granularity, &query_prop,
                          CU_MEM_ALLOC_GRANULARITY_MINIMUM),
                      "cuMemGetAllocationGranularity");
    const size_t bytes =
        ((kRequestedBytes + granularity - 1) / granularity) * granularity;

    CUmemAllocationProp prop = query_prop;
    prop.allocFlags.gpuDirectRDMACapable = 1;
    CUmemGenericAllocationHandle allocation = 0;
    cuda_driver_check(cuMemCreate(&allocation, bytes, &prop, 0), "cuMemCreate");

    CUdeviceptr address = 0;
    try {
        cuda_driver_check(cuMemAddressReserve(&address, bytes, granularity, 0, 0),
                          "cuMemAddressReserve");
        cuda_driver_check(cuMemMap(address, bytes, 0, allocation, 0), "cuMemMap");
        CUmemAccessDesc access{};
        access.location.type = CU_MEM_LOCATION_TYPE_DEVICE;
        access.location.id = kCudaDevice;
        access.flags = CU_MEM_ACCESS_FLAGS_PROT_READWRITE;
        cuda_driver_check(cuMemSetAccess(address, bytes, &access, 1),
                          "cuMemSetAccess");
        return {address, bytes, granularity, allocation};
    } catch (...) {
        if (address != 0) (void)cuMemAddressFree(address, bytes);
        (void)cuMemRelease(allocation);
        throw;
    }
}

void destroy_vmm_allocation(VmmAllocation *value) {
    if (value == nullptr) return;
    if (value->address != 0) {
        (void)cuMemUnmap(value->address, value->bytes);
        (void)cuMemAddressFree(value->address, value->bytes);
    }
    if (value->allocation != 0) (void)cuMemRelease(value->allocation);
    *value = {};
}

struct VmmCapabilities {
    int vmm = 0;
    int dma_buf = 0;
    int vmm_gdr = 0;
};

VmmCapabilities query_vmm_capabilities() {
    cuda_driver_check(cuInit(0), "cuInit");
    CUdevice device = 0;
    cuda_driver_check(cuDeviceGet(&device, kCudaDevice), "cuDeviceGet");
    VmmCapabilities value{};
    cuda_driver_check(cuDeviceGetAttribute(
                          &value.vmm,
                          static_cast<CUdevice_attribute>(
                              kVmmSupportedAttribute), device),
                      "cuDeviceGetAttribute(VMM_SUPPORTED)");
    cuda_driver_check(cuDeviceGetAttribute(
                          &value.dma_buf,
                          static_cast<CUdevice_attribute>(
                              kDmaBufSupportedAttribute), device),
                      "cuDeviceGetAttribute(DMA_BUF_SUPPORTED)");
    cuda_driver_check(cuDeviceGetAttribute(
                          &value.vmm_gdr,
                          static_cast<CUdevice_attribute>(
                              kVmmGdrSupportedAttribute), device),
                      "cuDeviceGetAttribute(GPU_DIRECT_RDMA_WITH_CUDA_VMM)");
    if (value.vmm != 1) fail("CUDA VMM is not supported");
    return value;
}

void on_term(int) { stop_requested = 1; }

void print_json_string(const char *value) {
    std::fputc('"', stdout);
    for (const char *cursor = value; *cursor != '\0'; ++cursor) {
        if (*cursor == '"' || *cursor == '\\') std::fputc('\\', stdout);
        std::fputc(*cursor, stdout);
    }
    std::fputc('"', stdout);
}

void print_failure(const std::string &message) {
    std::fprintf(stderr, "{\"event\":\"failure\",\"status\":\"fail\",\"error\":");
    print_json_string(message.c_str());
    std::fprintf(stderr, "}\n");
}

int run_source(const char *local_server, size_t bytes, uint64_t timeout_ms) {
    (void)timeout_ms;
    const auto capabilities = query_vmm_capabilities();
    const auto expected = expected_pattern(bytes);
    VmmAllocation vmm = make_vmm_allocation();
    void *staging = nullptr;
    void *gdr = nullptr;
    try {
        cuda_runtime_check(cudaMalloc(&staging, bytes), "cudaMalloc source staging");
        cuda_runtime_check(cudaMemcpy(reinterpret_cast<void *>(vmm.address),
                                      expected.data(), bytes,
                                      cudaMemcpyHostToDevice),
                           "cudaMemcpy source H2D to VMM");
        cuda_runtime_check(cudaDeviceSynchronize(), "source VMM H2D synchronize");

        cuda_runtime_check(cudaDeviceSynchronize(), "source copy pre-sync");
        const uint64_t copy_start = monotonic_raw_ns();
        cuda_runtime_check(cudaMemcpy(staging, reinterpret_cast<void *>(vmm.address),
                                      bytes, cudaMemcpyDeviceToDevice),
                           "cudaMemcpy VMM to legacy staging");
        cuda_runtime_check(cudaDeviceSynchronize(), "source staging synchronize");
        const uint64_t vmm_to_staging_ns = monotonic_raw_ns() - copy_start;
        const uint64_t checksum = fnv1a64(expected.data(), expected.size());

        gdr = spd_gdr_open(local_server, reinterpret_cast<uint64_t>(staging),
                           bytes, kCudaDevice);
        if (gdr == nullptr) {
            fail(std::string("spd_gdr_open source failed: ") + spd_gdr_last_error());
        }
        std::printf(
            "{\"event\":\"source_ready\",\"status\":\"pass\","
            "\"role\":\"source\",\"bytes\":%zu,"
            "\"vmm_bytes\":%zu,\"staging_bytes\":%zu,"
            "\"vmm_to_staging_ns\":%" PRIu64 ","
            "\"expected_checksum\":%" PRIu64 ","
            "\"vmm_supported\":%d,\"dma_buf_supported\":%d,"
            "\"vmm_gdr_supported\":%d,\"registration\":"
            "\"legacy_cudaMalloc_once\"}\n",
            bytes, vmm.bytes, bytes, vmm_to_staging_ns, checksum,
            capabilities.vmm, capabilities.dma_buf, capabilities.vmm_gdr);
        std::fflush(stdout);

        struct sigaction action{};
        action.sa_handler = on_term;
        sigemptyset(&action.sa_mask);
        sigaction(SIGTERM, &action, nullptr);
        sigaction(SIGINT, &action, nullptr);
        while (!stop_requested) {
            struct timespec delay{0, 10000000};
            while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {
            }
        }
        const int close_status = spd_gdr_close(gdr);
        gdr = nullptr;
        if (close_status != 0) {
            fail(std::string("spd_gdr_close source failed: ") + spd_gdr_last_error());
        }
        cuda_runtime_check(cudaFree(staging), "cudaFree source staging");
        staging = nullptr;
        destroy_vmm_allocation(&vmm);
        std::printf("{\"event\":\"source_released\",\"status\":\"pass\"}\n");
        return EXIT_SUCCESS;
    } catch (...) {
        if (gdr != nullptr) (void)spd_gdr_close(gdr);
        if (staging != nullptr) (void)cudaFree(staging);
        destroy_vmm_allocation(&vmm);
        throw;
    }
}

int run_reader(const char *local_server, const char *source_server,
               size_t bytes, uint64_t timeout_ms, uint64_t expected_checksum) {
    const auto capabilities = query_vmm_capabilities();
    const auto expected = expected_pattern(bytes);
    VmmAllocation vmm = make_vmm_allocation();
    void *staging = nullptr;
    void *gdr = nullptr;
    try {
        cuda_runtime_check(cudaMalloc(&staging, bytes), "cudaMalloc reader staging");
        cuda_runtime_check(cudaMemset(staging, 0xa5, bytes),
                           "cudaMemset reader staging sentinel");
        cuda_runtime_check(cudaDeviceSynchronize(), "reader staging sentinel sync");
        gdr = spd_gdr_open(local_server, reinterpret_cast<uint64_t>(staging),
                           bytes, kCudaDevice);
        if (gdr == nullptr) {
            fail(std::string("spd_gdr_open reader failed: ") + spd_gdr_last_error());
        }
        std::printf(
            "{\"event\":\"reader_ready\",\"status\":\"pass\","
            "\"role\":\"reader\",\"bytes\":%zu,"
            "\"vmm_bytes\":%zu,\"staging_bytes\":%zu,"
            "\"vmm_supported\":%d,\"dma_buf_supported\":%d,"
            "\"vmm_gdr_supported\":%d,\"registration\":"
            "\"legacy_cudaMalloc_once\"}\n",
            bytes, vmm.bytes, bytes, capabilities.vmm, capabilities.dma_buf,
            capabilities.vmm_gdr);
        std::fflush(stdout);

        uint64_t metadata_ns = 0;
        uint64_t rdma_ns = 0;
        uint64_t transferred = 0;
        const uint64_t total_start = monotonic_raw_ns();
        if (spd_gdr_read(gdr, source_server, timeout_ms, &metadata_ns, &rdma_ns,
                         &transferred) != 0) {
            fail(std::string("spd_gdr_read failed: ") + spd_gdr_last_error());
        }
        const uint64_t rdma_return_ns = monotonic_raw_ns() - total_start;
        if (transferred != bytes) fail("RDMA completion byte count mismatch");

        cuda_runtime_check(cudaDeviceSynchronize(), "reader copy pre-sync");
        const uint64_t copy_start = monotonic_raw_ns();
        cuda_runtime_check(cudaMemcpy(reinterpret_cast<void *>(vmm.address), staging,
                                      bytes, cudaMemcpyDeviceToDevice),
                           "cudaMemcpy legacy staging to VMM");
        cuda_runtime_check(cudaDeviceSynchronize(), "reader VMM copy synchronize");
        const uint64_t staging_to_vmm_ns = monotonic_raw_ns() - copy_start;
        const uint64_t total_ns = monotonic_raw_ns() - total_start;

        std::vector<uint8_t> observed(bytes);
        cuda_runtime_check(cudaDeviceSynchronize(), "reader checksum pre-sync");
        const uint64_t d2h_start = monotonic_raw_ns();
        cuda_runtime_check(cudaMemcpy(observed.data(),
                                      reinterpret_cast<void *>(vmm.address), bytes,
                                      cudaMemcpyDeviceToHost),
                           "cudaMemcpy reader VMM D2H checksum");
        cuda_runtime_check(cudaDeviceSynchronize(), "reader checksum synchronize");
        const uint64_t checksum_d2h_ns = monotonic_raw_ns() - d2h_start;
        const uint64_t observed_checksum = fnv1a64(observed.data(), observed.size());
        size_t mismatches = 0;
        for (size_t index = 0; index < bytes; ++index) {
            if (observed[index] != expected[index]) ++mismatches;
        }
        if (expected_checksum != observed_checksum || mismatches != 0) {
            fail("VMM checksum mismatch after RDMA staging");
        }
        std::printf(
            "{\"event\":\"transfer_complete\",\"status\":\"pass\","
            "\"role\":\"reader\",\"bytes\":%zu,"
            "\"transferred_bytes\":%zu,\"metadata_exchange_ns\":%" PRIu64 ","
            "\"rdma_completion_ns\":%" PRIu64 ","
            "\"rdma_return_ns\":%" PRIu64 ","
            "\"staging_to_vmm_ns\":%" PRIu64 ","
            "\"total_ns\":%" PRIu64 ",\"checksum_d2h_ns\":%" PRIu64 ","
            "\"expected_checksum\":%" PRIu64 ","
            "\"observed_checksum\":%" PRIu64 ",\"mismatches\":%zu,"
            "\"vmm_supported\":%d,\"dma_buf_supported\":%d,"
            "\"vmm_gdr_supported\":%d}\n",
            bytes, transferred, metadata_ns, rdma_ns, rdma_return_ns,
            staging_to_vmm_ns, total_ns, checksum_d2h_ns, expected_checksum,
            observed_checksum, mismatches, capabilities.vmm, capabilities.dma_buf,
            capabilities.vmm_gdr);
        std::fflush(stdout);

        const int close_status = spd_gdr_close(gdr);
        gdr = nullptr;
        if (close_status != 0) {
            fail(std::string("spd_gdr_close reader failed: ") + spd_gdr_last_error());
        }
        cuda_runtime_check(cudaFree(staging), "cudaFree reader staging");
        staging = nullptr;
        destroy_vmm_allocation(&vmm);
        return EXIT_SUCCESS;
    } catch (...) {
        if (gdr != nullptr) (void)spd_gdr_close(gdr);
        if (staging != nullptr) (void)cudaFree(staging);
        destroy_vmm_allocation(&vmm);
        throw;
    }
}

}  // namespace

int main(int argc, char **argv) {
    try {
        if (argc < 5) {
            fail("usage: --role source|reader --local SERVER --source SERVER "
                 "--bytes BYTES --timeout-ms MS [--expected-checksum VALUE]");
        }
        std::string role;
        std::string local_server;
        std::string source_server;
        size_t bytes = kRequestedBytes;
        uint64_t timeout_ms = 30000;
        uint64_t expected_checksum = 0;
        for (int index = 1; index < argc; ++index) {
            const std::string argument = argv[index];
            auto next = [&](const char *name) -> const char * {
                if (index + 1 >= argc) fail(std::string("missing value for ") + name);
                return argv[++index];
            };
            if (argument == "--role") role = next("--role");
            else if (argument == "--local") local_server = next("--local");
            else if (argument == "--source") source_server = next("--source");
            else if (argument == "--mode" || argument == "--local_server_name") (void)next(argument.c_str());
            else if (argument.rfind("--mode=", 0) == 0 ||
                     argument.rfind("--local_server_name=", 0) == 0) {
                // Compatibility markers used by the scoped process cleanup.
            }
            else if (argument == "--segment_id") (void)next(argument.c_str());
            else if (argument == "--bytes") bytes = std::strtoull(next("--bytes"), nullptr, 10);
            else if (argument == "--timeout-ms") timeout_ms = std::strtoull(next("--timeout-ms"), nullptr, 10);
            else if (argument == "--expected-checksum") expected_checksum = std::strtoull(next("--expected-checksum"), nullptr, 10);
            else fail("unknown argument: " + argument);
        }
        if (role != "source" && role != "reader") fail("--role must be source or reader");
        if (local_server.empty()) fail("--local is required");
        if (role == "reader" && (source_server.empty() || expected_checksum == 0)) {
            fail("reader requires --source and --expected-checksum");
        }
        if (bytes == 0 || bytes > (1ull << 30)) fail("bytes is out of range");
        if (timeout_ms == 0 || timeout_ms > 600000) fail("timeout-ms is out of range");
        if (role == "source") return run_source(local_server.c_str(), bytes, timeout_ms);
        return run_reader(local_server.c_str(), source_server.c_str(), bytes,
                          timeout_ms, expected_checksum);
    } catch (const std::exception &error) {
        print_failure(error.what());
        return EXIT_FAILURE;
    }
}
