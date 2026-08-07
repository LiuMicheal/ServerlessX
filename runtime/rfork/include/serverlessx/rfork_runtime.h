#ifndef SERVERLESSPD_RFORK_RUNTIME_H
#define SERVERLESSPD_RFORK_RUNTIME_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * This ABI is intentionally small.  It is the userspace cut between the
 * Mitosis ioctl ABI and the PhOS post-rfork rebind path.
 */
#define SPD_RFORK_ABI_VERSION UINT32_C(1)
#define SPD_RFORK_ROLE_FD 198
#define SPD_RFORK_ROLE_TOKEN_SIZE 48U
#define SPD_RFORK_RUN_COOKIE_SIZE 16U
#define SPD_RFORK_DEVICE_PATH "/dev/mitosis-syscalls"

/* Current Mitosis C-client ioctl command numbers. */
#define SPD_RFORK_IOCTL_PREPARE 4UL
#define SPD_RFORK_IOCTL_RESUME_REMOTE 6UL

enum spd_rfork_role {
    SPD_RFORK_ROLE_INVALID = 0,
    SPD_RFORK_ROLE_PARENT = 1,
    SPD_RFORK_ROLE_REMOTE_CHILD = 2
};

enum spd_rfork_operation {
    SPD_RFORK_OPERATION_NONE = 0,
    SPD_RFORK_OPERATION_PREPARE = 1,
    SPD_RFORK_OPERATION_RESUME_REMOTE = 2
};

enum spd_rfork_stage {
    SPD_RFORK_STAGE_NONE = 0,
    SPD_RFORK_STAGE_CONFIG = 1,
    SPD_RFORK_STAGE_ROLE_BEFORE = 2,
    SPD_RFORK_STAGE_PID_BEFORE = 3,
    SPD_RFORK_STAGE_DEVICE_OPEN = 4,
    SPD_RFORK_STAGE_IOCTL = 5,
    SPD_RFORK_STAGE_ROLE_AFTER = 6,
    SPD_RFORK_STAGE_PID_AFTER = 7,
    SPD_RFORK_STAGE_DEVICE_CLOSE = 8,
    SPD_RFORK_STAGE_COMPLETE = 9
};

enum spd_rfork_status {
    SPD_RFORK_OK = 0,
    SPD_RFORK_ERR_INVALID_ARGUMENT = -1,
    SPD_RFORK_ERR_HANDLER_OVERFLOW = -2,
    SPD_RFORK_ERR_COOKIE_INVALID = -3,
    SPD_RFORK_ERR_IO_OPS_INVALID = -4,
    SPD_RFORK_ERR_ROLE_READ = -5,
    SPD_RFORK_ERR_ROLE_SHORT_READ = -6,
    SPD_RFORK_ERR_ROLE_WRITE = -7,
    SPD_RFORK_ERR_ROLE_SHORT_WRITE = -8,
    SPD_RFORK_ERR_ROLE_TOKEN = -9,
    SPD_RFORK_ERR_ROLE_MISMATCH = -10,
    SPD_RFORK_ERR_PID = -11,
    SPD_RFORK_ERR_DEVICE_OPEN = -12,
    SPD_RFORK_ERR_IOCTL = -13,
    SPD_RFORK_ERR_DEVICE_CLOSE = -14,
    /*
     * A successful ResumeRemote changes the launcher's saved user registers
     * and therefore never reaches the instruction after the ioctl.  Any
     * ordinary ioctl return, including zero, is an anomaly.
     */
    SPD_RFORK_ERR_RESUME_RETURNED = -15,
    SPD_RFORK_ERR_DEVICE_OWNERSHIP = -16
};

struct spd_rfork_context {
    uint32_t abi_version;
    uint32_t handler_id;
    uint32_t source_machine_id;
    uint32_t target_machine_id;
    uint8_t run_cookie[SPD_RFORK_RUN_COOKIE_SIZE];
};

/*
 * Wire-compatible with Mitosis resume_remote_req_t.  Static assertions in the
 * implementation guard its sanitized two-u32 layout.
 */
struct spd_rfork_resume_remote_request {
    uint32_t machine_id;
    uint32_t handler_id;
};

/*
 * Every callback returns the syscall-like result and writes the errno value
 * associated with a negative result to errno_out.  A successful callback must
 * set errno_out to zero.  Tests can inject this table without a real device.
 */
struct spd_rfork_io_ops {
    void *opaque;
    int (*open_device)(void *opaque, const char *path, int *errno_out);
    long (*ioctl_call)(void *opaque,
                       int fd,
                       unsigned long request,
                       uintptr_t argument,
                       int *errno_out);
    ssize_t (*pread_call)(void *opaque,
                          int fd,
                          void *buffer,
                          size_t length,
                          int64_t offset,
                          int *errno_out);
    ssize_t (*pwrite_call)(void *opaque,
                           int fd,
                           const void *buffer,
                           size_t length,
                           int64_t offset,
                           int *errno_out);
    int (*close_fd)(void *opaque, int fd, int *errno_out);
    int64_t (*get_process_id)(void *opaque, int *errno_out);
};

/*
 * Machine-readable audit fields.  No field is inferred from log text.  A
 * caller may encode this structure as JSON after PhOS transport rebind.
 */
struct spd_rfork_audit {
    uint32_t abi_version;
    enum spd_rfork_operation operation;
    enum spd_rfork_stage stage;
    enum spd_rfork_status status;
    enum spd_rfork_role role_before;
    enum spd_rfork_role role_after;
    uint32_t handler_id;
    uint32_t source_machine_id;
    uint32_t target_machine_id;
    uint32_t observed_token_machine_id;
    uint32_t observed_token_checksum;
    uint8_t run_cookie[SPD_RFORK_RUN_COOKIE_SIZE];
    int32_t role_fd;
    int32_t device_fd;
    int64_t pid_before;
    int64_t pid_after;
    int32_t pid_errno;
    int64_t ioctl_result;
    int32_t role_read_errno;
    int32_t open_errno;
    int32_t ioctl_errno;
    int32_t close_result;
    int32_t close_errno;
    uint8_t close_attempted;
    uint8_t close_succeeded;
    /*
     * False in the resumed child: device_fd was copied as an integer in the
     * parent's memory and must not be acted on in the child's FD table.
     */
    uint8_t device_fd_safe_to_close;
    uint8_t ioctl_returned;
};

struct spd_rfork_outcome {
    enum spd_rfork_status status;
    enum spd_rfork_role role;
    struct spd_rfork_audit audit;
};

/*
 * Initialize a context.  handler_id is deliberately u64 at this boundary so
 * callers cannot silently truncate an artifact/CLI value into the kernel u32
 * ABI.
 */
enum spd_rfork_status spd_rfork_context_init(
    struct spd_rfork_context *context,
    uint64_t handler_id,
    uint32_t source_machine_id,
    uint32_t target_machine_id,
    const uint8_t run_cookie[SPD_RFORK_RUN_COOKIE_SIZE]);

/*
 * Encode the token that each launcher installs at SPD_RFORK_ROLE_FD before
 * entering the runtime.  The returned bytes are a fixed little-endian wire
 * format, not a native C struct.
 */
enum spd_rfork_status spd_rfork_role_token_encode(
    uint8_t output[SPD_RFORK_ROLE_TOKEN_SIZE],
    enum spd_rfork_role role,
    uint64_t handler_id,
    uint32_t machine_id,
    const uint8_t run_cookie[SPD_RFORK_RUN_COOKIE_SIZE]);

/*
 * Encode and install a token in the already-reserved role FD with checked raw
 * pwrite64 operations.  errno_out is always set (zero on success).
 */
enum spd_rfork_status spd_rfork_role_token_install(
    const struct spd_rfork_io_ops *ops,
    enum spd_rfork_role role,
    uint64_t handler_id,
    uint32_t machine_id,
    const uint8_t run_cookie[SPD_RFORK_RUN_COOKIE_SIZE],
    int *errno_out);

/*
 * Parent entry point and remote-child continuation point.  On success,
 * outcome->role is determined by the fixed role FD, not by ioctl return value
 * or PID.  A successful parent outcome retains ownership of audit.device_fd:
 * it keeps the kernel handler and parent image alive until the child has
 * stopped all remote access.  Release it only with spd_rfork_parent_release().
 * The remote-child branch must immediately enter PhOS rebind.
 */
enum spd_rfork_status spd_rfork_prepare(
    const struct spd_rfork_context *context,
    const struct spd_rfork_io_ops *ops,
    struct spd_rfork_outcome *outcome);

/*
 * Release the parent-owned Mitosis FD after the remote child has completed.
 * Calling this for a child, for a failed prepare, or more than once is an
 * ownership error.  The resumed child must never close the copied integer.
 */
enum spd_rfork_status spd_rfork_parent_release(
    const struct spd_rfork_io_ops *ops,
    struct spd_rfork_outcome *parent_outcome);

/*
 * Empty-child-launcher entry point.  This function only returns an error.
 * SPD_RFORK_ERR_RESUME_RETURNED means the ResumeRemote ioctl returned normally
 * instead of transferring control to the parent's prepare continuation.
 */
enum spd_rfork_status spd_rfork_resume_remote_or_error(
    const struct spd_rfork_context *context,
    const struct spd_rfork_io_ops *ops,
    struct spd_rfork_outcome *outcome);

/*
 * Linux target implementation.  Its callbacks issue raw kernel syscalls, in
 * particular pread64 for the role token, and do not depend on stdio state.
 */
const struct spd_rfork_io_ops *spd_rfork_default_io_ops(void);

const char *spd_rfork_status_name(enum spd_rfork_status status);
const char *spd_rfork_role_name(enum spd_rfork_role role);

#ifdef __cplusplus
}
#endif

#endif
