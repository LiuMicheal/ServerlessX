// One-allocation PhOS + Mitosis GPU remote-fork canary.
//
// The source creates one 2 MiB CUDA allocation through the runtime API.  The
// configured PhOS server backs that allocation with direct-GDR-capable VMM,
// prepares an in-memory image, and enters the checked rfork userspace boundary.
// The resumed child performs PhOS clone-attach before its first CUDA call, then
// verifies the restored bytes and a target-local mutation.  PhOS itself is
// supplied by the operator through LD_PRELOAD; this file only consumes its
// public C ABI.

#include "phos_gpu_rfork_bridge.h"

#include <cuda_runtime_api.h>
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <link.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr size_t kCanaryBytes = 2ull * 1024ull * 1024ull;
constexpr size_t kMaxMetadataBytes = 128ull * 1024ull * 1024ull;
constexpr uint32_t kPhosOk = 0;
constexpr uint32_t kPhosParentSession = 1;
constexpr uint32_t kPhosMetaAnonymousVma = 1u << 0;
constexpr uint32_t kPhosMetaSha256 = 1u << 1;
bool g_rfork_continuation = false;

enum class Mode { kInvalid, kSource, kLauncher, kSelfTest };

struct Options {
    Mode mode = Mode::kInvalid;
    std::string run_id;
    uint64_t handler_id = 0;
    uint32_t source_machine_id = 0;
    uint32_t target_machine_id = 1;
    std::string run_cookie;
    std::string role_token;
    std::string clone_ticket;
    std::string source_network_config;
    std::string source_pos_config;
    std::string target_network_config;
    std::string target_pos_config;
    std::string release_gate;
    double timeout_sec = 300.0;
};

struct PhosSupervisorIdentityV1 {
    uint32_t abi_version;
    uint32_t actual_pid;
    int32_t remoting_id;
    uint32_t session_kind;
    uint64_t phos_uuid;
    uint32_t cuda_device_initialized;
    uint32_t reserved;
};

struct PhosTemplatePrepareMemoryResultV1 {
    uint32_t abi_version;
    uint32_t owner_pid;
    uint64_t required_bytes;
    uint64_t bytes_written;
    uint32_t record_count;
    uint32_t reserved;
    uint8_t sha256[32];
};

static_assert(sizeof(PhosSupervisorIdentityV1) == 32);
static_assert(sizeof(PhosSessionIdentityV1) == 24);
static_assert(sizeof(PhosTemplatePrepareMemoryResultV1) == 64);
static_assert(sizeof(PhosCloneMemoryChildIdentityV2) == 88);

using PhosPrepareSupervisorFn = int (*)(
    const char *, const char *, PhosSupervisorIdentityV1 *, size_t);
using PhosTemplatePrepareMemoryFn = int (*)(
    void *, uint64_t, PhosTemplatePrepareMemoryResultV1 *, size_t);
using PhosShutdownSessionFn = int (*)();

struct PhosApi {
    PhosPrepareSupervisorFn prepare_supervisor = nullptr;
    SpdPhosQuerySessionFn query_session = nullptr;
    PhosTemplatePrepareMemoryFn prepare_memory = nullptr;
    SpdPhosCloneAttachMemoryFn clone_attach_memory = nullptr;
    PhosShutdownSessionFn shutdown_session = nullptr;
};

using CudaMallocFn = cudaError_t (*)(void **, size_t);
using CudaFreeFn = cudaError_t (*)(void *);
using CudaMemcpyFn = cudaError_t (*)(void *, const void *, size_t,
                                     cudaMemcpyKind);
using CudaMemsetFn = cudaError_t (*)(void *, int, size_t);
using CudaDeviceSynchronizeFn = cudaError_t (*)();

struct CudaRuntimeApi {
    CudaMallocFn malloc = nullptr;
    CudaFreeFn free = nullptr;
    CudaMemcpyFn memcpy = nullptr;
    CudaMemsetFn memset = nullptr;
    CudaDeviceSynchronizeFn synchronize = nullptr;
};

struct MetadataImage {
    void *address = nullptr;
    size_t mapping_bytes = 0;
    uint64_t image_bytes = 0;
    uint32_t record_count = 0;
    uint8_t sha256[32] = {};
};

struct CudaAllocation {
    void *address = nullptr;
    size_t bytes = 0;
    uint64_t source_digest = 0;
};

struct PrefaultAudit {
    uint32_t required_mask = 0;
    uint32_t matched_mask = 0;
    uint64_t objects = 0;
    uint64_t segments = 0;
    uint64_t pages = 0;
    uint32_t checksum = 2166136261u;
    size_t page_size = 0;
    bool failed = false;
};

constexpr uint32_t kElfMain = 1u << 0;
constexpr uint32_t kElfClient = 1u << 1;
constexpr uint32_t kElfPos = 1u << 2;
constexpr uint32_t kElfLibc = 1u << 3;
constexpr uint32_t kElfLoader = 1u << 4;
constexpr uint32_t kElfLibstdcxx = 1u << 5;
constexpr uint32_t kElfRequired = kElfMain | kElfClient | kElfPos | kElfLibc |
                                  kElfLoader | kElfLibstdcxx;

[[noreturn]] void fail(const std::string &message) {
    throw std::runtime_error(message);
}

[[noreturn]] void raw_exit_group(int status) {
    (void)syscall(SYS_exit_group, status);
    _exit(status);
}

std::string json_escape(const std::string &value) {
    std::string output;
    output.reserve(value.size());
    static constexpr char hex[] = "0123456789abcdef";
    for (unsigned char byte : value) {
        switch (byte) {
            case '"': output += "\\\""; break;
            case '\\': output += "\\\\"; break;
            case '\b': output += "\\b"; break;
            case '\f': output += "\\f"; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            default:
                if (byte < 0x20) {
                    output += "\\u00";
                    output += hex[byte >> 4];
                    output += hex[byte & 0xf];
                } else {
                    output.push_back(static_cast<char>(byte));
                }
        }
    }
    return output;
}

void emit_failure(const char *stage, const std::string &message) {
    std::printf(
        "{\"schema\":\"serverlesspd.gpu-rfork-canary-event.v1\","
        "\"event\":\"failure\",\"stage\":\"%s\",\"status\":\"fail\","
        "\"message\":\"%s\"}\n",
        stage, json_escape(message).c_str());
    std::fflush(stdout);
}

void cuda_check(cudaError_t result, const char *operation) {
    if (result == cudaSuccess) return;
    fail(std::string(operation) + " failed: cuda status " +
         std::to_string(static_cast<int>(result)));
}

uint64_t monotonic_ns() {
    timespec value{};
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) {
        fail(std::string("clock_gettime failed: ") + std::strerror(errno));
    }
    return static_cast<uint64_t>(value.tv_sec) * 1000000000ull +
           static_cast<uint64_t>(value.tv_nsec);
}

uint64_t fnv1a64(const uint8_t *data, size_t bytes) {
    uint64_t value = 0xcbf29ce484222325ull;
    for (size_t index = 0; index < bytes; ++index) {
        value ^= data[index];
        value *= 0x100000001b3ull;
    }
    return value;
}

std::string hex_bytes(const uint8_t *data, size_t bytes) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string output(bytes * 2, '0');
    for (size_t index = 0; index < bytes; ++index) {
        output[index * 2] = hex[data[index] >> 4];
        output[index * 2 + 1] = hex[data[index] & 0xf];
    }
    return output;
}

bool safe_id(const std::string &value) {
    if (value.empty() || value.size() > 128) return false;
    for (unsigned char byte : value) {
        if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
            (byte >= '0' && byte <= '9') || byte == '.' || byte == '_' ||
            byte == ':' || byte == '-') {
            continue;
        }
        return false;
    }
    return true;
}

bool absolute_path(const std::string &value) {
    return !value.empty() && value.front() == '/';
}

bool contains_tunable(const char *tunables, const char *required) {
    if (tunables == nullptr || required == nullptr) return false;
    const size_t required_size = std::strlen(required);
    const char *cursor = tunables;
    while (*cursor != '\0') {
        const char *end = std::strchr(cursor, ':');
        const size_t size = end == nullptr
                                ? std::strlen(cursor)
                                : static_cast<size_t>(end - cursor);
        if (size == required_size &&
            std::memcmp(cursor, required, required_size) == 0) {
            return true;
        }
        if (end == nullptr) break;
        cursor = end + 1;
    }
    return false;
}

bool any_nonzero(const uint8_t *data, size_t bytes) {
    uint8_t accumulated = 0;
    for (size_t index = 0; index < bytes; ++index) accumulated |= data[index];
    return accumulated != 0;
}

uint64_t parse_u64(const char *text, const char *name) {
    if (text == nullptr || *text == '\0' || *text == '-') {
        fail(std::string("invalid ") + name);
    }
    errno = 0;
    char *end = nullptr;
    const unsigned long long value = std::strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') {
        fail(std::string("invalid ") + name);
    }
    return static_cast<uint64_t>(value);
}

double parse_timeout(const char *text) {
    if (text == nullptr || *text == '\0') fail("invalid timeout");
    errno = 0;
    char *end = nullptr;
    const double value = std::strtod(text, &end);
    if (errno != 0 || end == text || *end != '\0' || !(value > 0.0) ||
        value > 3600.0) {
        fail("timeout must be in (0, 3600]");
    }
    return value;
}

std::vector<uint8_t> parse_cookie(const std::string &text) {
    if (text.size() != SPD_RFORK_RUN_COOKIE_SIZE * 2) {
        fail("run cookie must contain 32 lowercase hex characters");
    }
    std::vector<uint8_t> output(SPD_RFORK_RUN_COOKIE_SIZE);
    for (size_t index = 0; index < output.size(); ++index) {
        const char high = text[index * 2];
        const char low = text[index * 2 + 1];
        auto nibble = [](char value) -> int {
            if (value >= '0' && value <= '9') return value - '0';
            if (value >= 'a' && value <= 'f') return value - 'a' + 10;
            return -1;
        };
        const int high_value = nibble(high);
        const int low_value = nibble(low);
        if (high_value < 0 || low_value < 0) {
            fail("run cookie must contain 32 lowercase hex characters");
        }
        output[index] = static_cast<uint8_t>((high_value << 4) | low_value);
    }
    return output;
}

void usage(FILE *stream) {
    std::fprintf(
        stream,
        "usage:\n"
        "  phos_gpu_rfork_canary --self-test\n"
        "  phos_gpu_rfork_canary --mode launcher COMMON\n"
        "  phos_gpu_rfork_canary --mode source COMMON SOURCE\n\n"
        "COMMON: --run-id ID --handler-id N --source-machine-id N "
        "--target-machine-id N --run-cookie HEX32 --role-token ABS\n"
        "SOURCE: --clone-ticket ID --source-network-config ABS "
        "--source-pos-config ABS --target-network-config ABS "
        "--target-pos-config ABS --release-gate ABS [--timeout-sec N]\n");
}

Options parse_options(int argc, char **argv) {
    Options options;
    if (argc == 2 && std::strcmp(argv[1], "--self-test") == 0) {
        options.mode = Mode::kSelfTest;
        return options;
    }
    auto value_after = [&](int *index, const char *option) -> const char * {
        if (*index + 1 >= argc) fail(std::string("missing value for ") + option);
        ++*index;
        return argv[*index];
    };
    for (int index = 1; index < argc; ++index) {
        const char *option = argv[index];
        if (std::strcmp(option, "--help") == 0) {
            usage(stdout);
            std::exit(0);
        } else if (std::strcmp(option, "--mode") == 0) {
            const char *value = value_after(&index, option);
            if (std::strcmp(value, "source") == 0) options.mode = Mode::kSource;
            else if (std::strcmp(value, "launcher") == 0) options.mode = Mode::kLauncher;
            else fail("mode must be source or launcher");
        } else if (std::strcmp(option, "--run-id") == 0) {
            options.run_id = value_after(&index, option);
        } else if (std::strcmp(option, "--handler-id") == 0) {
            options.handler_id = parse_u64(value_after(&index, option), "handler id");
        } else if (std::strcmp(option, "--source-machine-id") == 0) {
            const uint64_t value = parse_u64(value_after(&index, option), "source machine id");
            if (value > UINT32_MAX) fail("source machine id exceeds uint32");
            options.source_machine_id = static_cast<uint32_t>(value);
        } else if (std::strcmp(option, "--target-machine-id") == 0) {
            const uint64_t value = parse_u64(value_after(&index, option), "target machine id");
            if (value > UINT32_MAX) fail("target machine id exceeds uint32");
            options.target_machine_id = static_cast<uint32_t>(value);
        } else if (std::strcmp(option, "--run-cookie") == 0) {
            options.run_cookie = value_after(&index, option);
        } else if (std::strcmp(option, "--role-token") == 0) {
            options.role_token = value_after(&index, option);
        } else if (std::strcmp(option, "--clone-ticket") == 0) {
            options.clone_ticket = value_after(&index, option);
        } else if (std::strcmp(option, "--source-network-config") == 0) {
            options.source_network_config = value_after(&index, option);
        } else if (std::strcmp(option, "--source-pos-config") == 0) {
            options.source_pos_config = value_after(&index, option);
        } else if (std::strcmp(option, "--target-network-config") == 0) {
            options.target_network_config = value_after(&index, option);
        } else if (std::strcmp(option, "--target-pos-config") == 0) {
            options.target_pos_config = value_after(&index, option);
        } else if (std::strcmp(option, "--release-gate") == 0) {
            options.release_gate = value_after(&index, option);
        } else if (std::strcmp(option, "--timeout-sec") == 0) {
            options.timeout_sec = parse_timeout(value_after(&index, option));
        } else {
            fail(std::string("unknown option: ") + option);
        }
    }
    return options;
}

void validate_options(const Options &options) {
    if (options.mode != Mode::kSource && options.mode != Mode::kLauncher) {
        fail("--mode is required");
    }
    if (!safe_id(options.run_id)) fail("run id is not a safe identifier");
    if (options.handler_id == 0 || options.handler_id > UINT32_MAX) {
        fail("handler id must be a nonzero uint32");
    }
    (void)parse_cookie(options.run_cookie);
    if (!absolute_path(options.role_token)) fail("role token path must be absolute");
    if (options.mode == Mode::kSource) {
        if (!contains_tunable(std::getenv("GLIBC_TUNABLES"),
                              "glibc.pthread.rseq=0")) {
            fail("source must be exec'd with GLIBC_TUNABLES=glibc.pthread.rseq=0");
        }
        if (!safe_id(options.clone_ticket)) fail("clone ticket is not a safe identifier");
        for (const auto *path : {&options.source_network_config,
                                 &options.source_pos_config,
                                 &options.target_network_config,
                                 &options.target_pos_config,
                                 &options.release_gate}) {
            if (!absolute_path(*path)) fail("all source config/gate paths must be absolute");
        }
        struct stat gate_status {};
        if (stat(options.release_gate.c_str(), &gate_status) == 0) {
            fail("release gate already exists");
        }
        if (errno != ENOENT) {
            fail(std::string("cannot inspect release gate: ") + std::strerror(errno));
        }
    }
}

template <typename Function>
Function resolve_symbol(const char *name) {
    dlerror();
    void *address = dlsym(RTLD_DEFAULT, name);
    const char *error = dlerror();
    if (address == nullptr || error != nullptr) {
        fail(std::string("required PhOS symbol is unavailable: ") + name);
    }
    static_assert(sizeof(Function) == sizeof(address));
    Function function;
    std::memcpy(&function, &address, sizeof(function));
    return function;
}

PhosApi resolve_phos_api() {
    PhosApi api;
    api.prepare_supervisor =
        resolve_symbol<PhosPrepareSupervisorFn>("phos_prepare_supervisor_v1");
    api.query_session =
        resolve_symbol<SpdPhosQuerySessionFn>("phos_query_session_identity_v1");
    api.prepare_memory = resolve_symbol<PhosTemplatePrepareMemoryFn>(
        "phos_template_prepare_memory_v1");
    api.clone_attach_memory = resolve_symbol<SpdPhosCloneAttachMemoryFn>(
        "phos_post_rfork_child_clone_attach_memory_v2");
    api.shutdown_session =
        resolve_symbol<PhosShutdownSessionFn>("phos_shutdown_session_v1");
    return api;
}

CudaRuntimeApi resolve_cuda_runtime_api() {
    CudaRuntimeApi api;
    api.malloc = resolve_symbol<CudaMallocFn>("cudaMalloc");
    api.free = resolve_symbol<CudaFreeFn>("cudaFree");
    api.memcpy = resolve_symbol<CudaMemcpyFn>("cudaMemcpy");
    api.memset = resolve_symbol<CudaMemsetFn>("cudaMemset");
    api.synchronize =
        resolve_symbol<CudaDeviceSynchronizeFn>("cudaDeviceSynchronize");
    return api;
}

void install_role_token(const Options &options, enum spd_rfork_role role,
                        uint32_t machine_id) {
    int fd = open(options.role_token.c_str(),
                  O_CREAT | O_EXCL | O_NOFOLLOW | O_RDWR, 0600);
    if (fd < 0) fail(std::string("open role token failed: ") + std::strerror(errno));
    if (ftruncate(fd, SPD_RFORK_ROLE_TOKEN_SIZE) != 0) {
        const int saved = errno;
        close(fd);
        fail(std::string("ftruncate role token failed: ") + std::strerror(saved));
    }
    if (fd != SPD_RFORK_ROLE_FD) {
        if (dup2(fd, SPD_RFORK_ROLE_FD) < 0) {
            const int saved = errno;
            close(fd);
            fail(std::string("dup2 role token failed: ") + std::strerror(saved));
        }
        close(fd);
    }
    const std::vector<uint8_t> cookie = parse_cookie(options.run_cookie);
    int call_errno = 0;
    const enum spd_rfork_status status = spd_rfork_role_token_install(
        nullptr, role, options.handler_id, machine_id, cookie.data(), &call_errno);
    if (status != SPD_RFORK_OK) {
        fail(std::string("role token install failed: ") +
             spd_rfork_status_name(status) + " errno=" +
             std::to_string(call_errno));
    }
}

struct spd_rfork_context make_rfork_context(const Options &options) {
    const std::vector<uint8_t> cookie = parse_cookie(options.run_cookie);
    struct spd_rfork_context context {};
    const enum spd_rfork_status status = spd_rfork_context_init(
        &context, options.handler_id, options.source_machine_id,
        options.target_machine_id, cookie.data());
    if (status != SPD_RFORK_OK) {
        fail(std::string("rfork context failed: ") + spd_rfork_status_name(status));
    }
    return context;
}

size_t current_task_count() {
    DIR *directory = opendir("/proc/self/task");
    if (directory == nullptr) {
        fail(std::string("opendir /proc/self/task failed: ") + std::strerror(errno));
    }
    size_t count = 0;
    errno = 0;
    while (dirent *entry = readdir(directory)) {
        if (entry->d_name[0] < '0' || entry->d_name[0] > '9') continue;
        bool digits = true;
        for (const char *cursor = entry->d_name; *cursor != '\0'; ++cursor) {
            if (*cursor < '0' || *cursor > '9') digits = false;
        }
        if (digits) ++count;
    }
    const int saved = errno;
    closedir(directory);
    if (saved != 0) fail(std::string("readdir task failed: ") + std::strerror(saved));
    return count;
}

const char *base_name(const char *path) {
    if (path == nullptr) return "";
    const char *slash = std::strrchr(path, '/');
    return slash == nullptr ? path : slash + 1;
}

bool starts_with(const char *text, const char *prefix) {
    return std::strncmp(text, prefix, std::strlen(prefix)) == 0;
}

uint32_t classify_object(const dl_phdr_info *info) {
    const char *name = base_name(info->dlpi_name);
    if (info->dlpi_addr == 0 || *name == '\0') return kElfMain;
    if ((starts_with(name, "libclient") || starts_with(name, "libxpuclient")) &&
        std::strstr(name, ".so") != nullptr) {
        return kElfClient;
    }
    if (starts_with(name, "libpos.so")) return kElfPos;
    if (starts_with(name, "libc.so")) return kElfLibc;
    if (starts_with(name, "ld-linux")) return kElfLoader;
    if (starts_with(name, "libstdc++.so")) return kElfLibstdcxx;
    return 0;
}

int prefault_callback(dl_phdr_info *info, size_t, void *opaque) {
    auto *audit = static_cast<PrefaultAudit *>(opaque);
    const uint32_t object_class = classify_object(info);
    if (object_class == 0) return 0;
    audit->matched_mask |= object_class;
    ++audit->objects;
    for (ElfW(Half) index = 0; index < info->dlpi_phnum; ++index) {
        const ElfW(Phdr) &header = info->dlpi_phdr[index];
        if (header.p_type != PT_LOAD || (header.p_flags & PF_R) == 0 ||
            header.p_memsz == 0) {
            continue;
        }
        const uintptr_t start = static_cast<uintptr_t>(info->dlpi_addr) +
                                static_cast<uintptr_t>(header.p_vaddr);
        if (start < static_cast<uintptr_t>(info->dlpi_addr) ||
            header.p_memsz > std::numeric_limits<uintptr_t>::max() - start) {
            audit->failed = true;
            return 1;
        }
        const uintptr_t end = start + static_cast<uintptr_t>(header.p_memsz);
        uintptr_t page = start - (start % audit->page_size);
        ++audit->segments;
        while (page < end) {
            const uintptr_t address = std::max(page, start);
            const volatile uint8_t byte =
                *reinterpret_cast<const volatile uint8_t *>(address);
            audit->checksum ^= byte;
            audit->checksum *= 16777619u;
            ++audit->pages;
            if (page > std::numeric_limits<uintptr_t>::max() - audit->page_size) {
                audit->failed = true;
                return 1;
            }
            page += audit->page_size;
        }
    }
    return 0;
}

PrefaultAudit prefault_required_objects() {
    const long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) fail("sysconf(_SC_PAGESIZE) failed");
    PrefaultAudit audit;
    audit.required_mask = kElfRequired;
    audit.page_size = static_cast<size_t>(page_size);
    const int result = dl_iterate_phdr(prefault_callback, &audit);
    if (result != 0 || audit.failed) fail("required ELF prefault failed");
    if (audit.matched_mask != audit.required_mask) {
        fail("required ELF prefault did not find the complete runtime set");
    }
    return audit;
}

std::vector<uint8_t> make_pattern(size_t bytes) {
    std::vector<uint8_t> output(bytes);
    for (size_t index = 0; index < bytes; ++index) {
        output[index] = static_cast<uint8_t>((index * 131ull + 17ull) & 0xffull);
    }
    return output;
}

CudaAllocation make_cuda_allocation(const CudaRuntimeApi &cuda) {
    CudaAllocation value;
    value.bytes = kCanaryBytes;
    cuda_check(cuda.malloc(&value.address, value.bytes), "cudaMalloc");
    const std::vector<uint8_t> pattern = make_pattern(value.bytes);
    cuda_check(cuda.memcpy(value.address, pattern.data(), pattern.size(),
                           cudaMemcpyHostToDevice),
               "cudaMemcpy(HtoD pattern)");
    cuda_check(cuda.synchronize(), "cudaDeviceSynchronize(pattern)");
    std::vector<uint8_t> observed(value.bytes);
    cuda_check(cuda.memcpy(observed.data(), value.address, value.bytes,
                           cudaMemcpyDeviceToHost),
               "cudaMemcpy(DtoH source)");
    if (observed != pattern) fail("source CUDA pattern verification failed");
    value.source_digest = fnv1a64(observed.data(), observed.size());
    return value;
}

void destroy_cuda_allocation(const CudaRuntimeApi &cuda, CudaAllocation *value) {
    if (value == nullptr) return;
    if (value->address != nullptr) cuda_check(cuda.free(value->address), "cudaFree");
    *value = {};
}

MetadataImage prepare_metadata_image(const PhosApi &api) {
    PhosTemplatePrepareMemoryResultV1 query{};
    int status = api.prepare_memory(nullptr, 0, &query, sizeof(query));
    if (status != static_cast<int>(kPhosOk) || query.abi_version != 1 ||
        query.owner_pid != static_cast<uint32_t>(getpid()) ||
        query.required_bytes == 0 || query.required_bytes > kMaxMetadataBytes ||
        query.bytes_written != 0 || query.record_count == 0 || query.reserved != 0 ||
        !any_nonzero(query.sha256, sizeof(query.sha256))) {
        fail("PhOS metadata size query failed its ABI gate");
    }
    const long page_size_raw = sysconf(_SC_PAGESIZE);
    if (page_size_raw <= 0) fail("metadata page-size query failed");
    const uint64_t page_size = static_cast<uint64_t>(page_size_raw);
    if (query.required_bytes > UINT64_MAX - (page_size - 1)) {
        fail("PhOS metadata size overflow");
    }
    const uint64_t rounded =
        ((query.required_bytes + page_size - 1) / page_size) * page_size;
    if (rounded > SIZE_MAX) fail("PhOS metadata mapping exceeds size_t");
    void *mapping = mmap(nullptr, static_cast<size_t>(rounded),
                         PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) {
        fail(std::string("metadata mmap failed: ") + std::strerror(errno));
    }

    PhosTemplatePrepareMemoryResultV1 filled{};
    status = api.prepare_memory(mapping, rounded, &filled, sizeof(filled));
    const bool valid =
        status == static_cast<int>(kPhosOk) && filled.abi_version == 1 &&
        filled.owner_pid == query.owner_pid &&
        filled.required_bytes == query.required_bytes &&
        filled.bytes_written == query.required_bytes &&
        filled.record_count == query.record_count && filled.reserved == 0 &&
        std::memcmp(filled.sha256, query.sha256, sizeof(query.sha256)) == 0;
    if (!valid) {
        munmap(mapping, static_cast<size_t>(rounded));
        fail("PhOS metadata fill failed its ABI gate");
    }
    if (mprotect(mapping, static_cast<size_t>(rounded), PROT_READ) != 0) {
        const int saved = errno;
        munmap(mapping, static_cast<size_t>(rounded));
        fail(std::string("metadata mprotect failed: ") + std::strerror(saved));
    }
    MetadataImage image;
    image.address = mapping;
    image.mapping_bytes = static_cast<size_t>(rounded);
    image.image_bytes = filled.required_bytes;
    image.record_count = filled.record_count;
    std::memcpy(image.sha256, filled.sha256, sizeof(image.sha256));
    return image;
}

uint64_t wait_for_gate(const std::string &path, double timeout_sec) {
    const uint64_t started = monotonic_ns();
    const uint64_t timeout_ns = static_cast<uint64_t>(timeout_sec * 1e9);
    for (;;) {
        struct stat value {};
        if (stat(path.c_str(), &value) == 0) return monotonic_ns() - started;
        if (errno != ENOENT) {
            fail(std::string("release-gate stat failed: ") + std::strerror(errno));
        }
        if (monotonic_ns() - started >= timeout_ns) {
            fail("timed out waiting for release gate");
        }
        timespec pause{0, 20 * 1000 * 1000};
        while (nanosleep(&pause, &pause) != 0 && errno == EINTR) {}
    }
}

int run_self_test() {
    const char hello[] = "hello";
    if (fnv1a64(reinterpret_cast<const uint8_t *>(hello), 5) !=
        0xa430d84680aabd0bull) {
        fail("FNV-1a self-test failed");
    }
    const std::vector<uint8_t> pattern = make_pattern(4);
    if (pattern != std::vector<uint8_t>({17, 148, 23, 154})) {
        fail("pattern self-test failed");
    }
    std::printf(
        "{\"schema\":\"serverlesspd.gpu-rfork-canary-event.v1\","
        "\"event\":\"self_test\",\"status\":\"pass\",\"bytes\":%zu}\n",
        kCanaryBytes);
    return 0;
}

int run_launcher(const Options &options) {
    install_role_token(options, SPD_RFORK_ROLE_REMOTE_CHILD,
                       options.target_machine_id);
    struct spd_rfork_context context = make_rfork_context(options);
    struct spd_rfork_outcome outcome {};
    const enum spd_rfork_status status = spd_rfork_resume_remote_or_error(
        &context, nullptr, &outcome);
    fail(std::string("ResumeRemote returned: ") + spd_rfork_status_name(status) +
         " ioctl_errno=" + std::to_string(outcome.audit.ioctl_errno));
}

int run_source(const Options &options) {
    install_role_token(options, SPD_RFORK_ROLE_PARENT,
                       options.source_machine_id);
    const PhosApi phos = resolve_phos_api();
    PhosSupervisorIdentityV1 supervisor{};
    const int supervisor_status = phos.prepare_supervisor(
        options.source_network_config.c_str(), options.source_pos_config.c_str(),
        &supervisor, sizeof(supervisor));
    if (supervisor_status != static_cast<int>(kPhosOk) ||
        supervisor.abi_version != 1 ||
        supervisor.actual_pid != static_cast<uint32_t>(getpid()) ||
        supervisor.remoting_id < 0 ||
        supervisor.session_kind != kPhosParentSession ||
        supervisor.cuda_device_initialized != 0 || supervisor.reserved != 0) {
        fail("PhOS supervisor preparation failed its identity gate");
    }

    const CudaRuntimeApi cuda = resolve_cuda_runtime_api();
    CudaAllocation allocation = make_cuda_allocation(cuda);
    PhosSessionIdentityV1 source_session{};
    const int source_query_status =
        phos.query_session(&source_session, sizeof(source_session));
    if (source_query_status != static_cast<int>(kPhosOk) ||
        source_session.abi_version != 1 ||
        source_session.actual_pid != static_cast<uint32_t>(getpid()) ||
        source_session.remoting_id != supervisor.remoting_id ||
        source_session.session_kind != kPhosParentSession ||
        source_session.phos_uuid != supervisor.phos_uuid) {
        fail("PhOS source session query failed");
    }
    std::printf(
        "{\"schema\":\"serverlesspd.gpu-rfork-canary-event.v1\","
        "\"event\":\"source_gpu_ready\",\"status\":\"pass\","
        "\"run_id\":\"%s\",\"pid\":%ld,\"bytes\":%zu,"
        "\"allocation_count\":1,\"address\":\"0x%" PRIx64 "\","
        "\"source_digest_fnv1a64\":\"%016" PRIx64 "\"}\n",
        options.run_id.c_str(), static_cast<long>(getpid()), allocation.bytes,
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(allocation.address)),
        allocation.source_digest);
    std::fflush(stdout);

    MetadataImage image = prepare_metadata_image(phos);
    std::printf(
        "{\"schema\":\"serverlesspd.gpu-rfork-canary-event.v1\","
        "\"event\":\"source_image_ready\",\"status\":\"pass\","
        "\"run_id\":\"%s\",\"image_bytes\":%" PRIu64 ","
        "\"record_count\":%u,\"flags\":%u,\"sha256\":\"%s\"}\n",
        options.run_id.c_str(), image.image_bytes, image.record_count,
        kPhosMetaAnonymousVma | kPhosMetaSha256,
        hex_bytes(image.sha256, sizeof(image.sha256)).c_str());

    const PrefaultAudit prefault = prefault_required_objects();
    const size_t tasks = current_task_count();
    if (tasks != 1) {
        fail("source must have exactly one task at the remote-fork cut");
    }
    std::printf(
        "{\"schema\":\"serverlesspd.gpu-rfork-canary-event.v1\","
        "\"event\":\"fork_cut_ready\",\"status\":\"pass\","
        "\"run_id\":\"%s\",\"task_count\":%zu,"
        "\"prefault_required_mask\":%u,\"prefault_matched_mask\":%u,"
        "\"prefault_objects\":%" PRIu64 ",\"prefault_segments\":%" PRIu64 ","
        "\"prefault_pages\":%" PRIu64 ",\"prefault_checksum\":%u}\n",
        options.run_id.c_str(), tasks, prefault.required_mask,
        prefault.matched_mask, prefault.objects, prefault.segments,
        prefault.pages, prefault.checksum);
    std::fflush(stdout);

    const std::vector<uint8_t> cookie = parse_cookie(options.run_cookie);
    SpdGpuRforkBridgeResult bridge{};
    const int bridge_status = spd_gpu_rfork_prepare_attach(
        options.handler_id, options.source_machine_id, options.target_machine_id,
        cookie.data(), options.clone_ticket.c_str(), image.address,
        image.mapping_bytes, image.image_bytes, image.record_count, image.sha256,
        options.target_network_config.c_str(), options.target_pos_config.c_str(),
        source_session.phos_uuid, allocation.address, allocation.bytes,
        allocation.source_digest, &bridge);
    g_rfork_continuation = true;
    if (bridge_status != SPD_GPU_RFORK_BRIDGE_OK) {
        fail("rfork/PhOS bridge failed: status=" +
             std::to_string(bridge_status) + " native_status=" +
             std::to_string(bridge.native_status));
    }

    if (bridge.role != SPD_RFORK_ROLE_PARENT) fail("rfork returned an invalid role");
    std::printf(
        "{\"schema\":\"serverlesspd.gpu-rfork-canary-event.v1\","
        "\"event\":\"parent_prepared\",\"status\":\"pass\","
        "\"run_id\":\"%s\",\"role\":\"parent\",\"pid\":%ld,"
        "\"handler_id\":%u}\n",
        options.run_id.c_str(), static_cast<long>(getpid()),
        bridge.handler_id);
    std::fflush(stdout);
    const uint64_t release_wait_ns =
        wait_for_gate(options.release_gate, options.timeout_sec);
    const int release_status = spd_gpu_rfork_parent_release_bridge();
    if (release_status != SPD_GPU_RFORK_BRIDGE_OK) {
        fail("parent release failed: status=" + std::to_string(release_status));
    }
    if (munmap(image.address, image.mapping_bytes) != 0) {
        fail(std::string("parent metadata munmap failed: ") + std::strerror(errno));
    }
    destroy_cuda_allocation(cuda, &allocation);
    const int shutdown_status = phos.shutdown_session();
    if (shutdown_status != static_cast<int>(kPhosOk)) {
        fail("parent PhOS shutdown failed");
    }
    std::printf(
        "{\"schema\":\"serverlesspd.gpu-rfork-canary-event.v1\","
        "\"event\":\"canary_complete\",\"status\":\"pass\","
        "\"run_id\":\"%s\",\"role\":\"parent\","
        "\"release_wait_ns\":%" PRIu64 "}\n",
        options.run_id.c_str(), release_wait_ns);
    std::fflush(stdout);
    raw_exit_group(0);
}

}  // namespace

int main(int argc, char **argv) {
    const char *stage = "arguments";
    try {
        const Options options = parse_options(argc, argv);
        if (options.mode == Mode::kSelfTest) return run_self_test();
        validate_options(options);
        if (options.mode == Mode::kLauncher) {
            stage = "resume_remote";
            return run_launcher(options);
        }
        stage = "source_remote_fork";
        return run_source(options);
    } catch (const std::exception &error) {
        emit_failure(stage, error.what());
        if (g_rfork_continuation) raw_exit_group(1);
        return 1;
    }
}
