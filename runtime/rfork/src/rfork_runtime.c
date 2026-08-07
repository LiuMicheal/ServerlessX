#define _GNU_SOURCE

#include "serverlessx/rfork_runtime.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/syscall.h>

#if !defined(__linux__)
#error "rfork-runtime requires Linux"
#endif

#if !defined(__x86_64__)
#error "the raw-syscall backend currently supports the x86_64 artifact target"
#endif

#define SPD_ROLE_MAGIC_0 ((uint8_t)'S')
#define SPD_ROLE_MAGIC_1 ((uint8_t)'P')
#define SPD_ROLE_MAGIC_2 ((uint8_t)'D')
#define SPD_ROLE_MAGIC_3 ((uint8_t)'R')
#define SPD_ROLE_MAGIC_4 ((uint8_t)'F')
#define SPD_ROLE_MAGIC_5 ((uint8_t)'O')
#define SPD_ROLE_MAGIC_6 ((uint8_t)'R')
#define SPD_ROLE_MAGIC_7 ((uint8_t)'K')

#define SPD_TOKEN_ABI_OFFSET 8U
#define SPD_TOKEN_SIZE_OFFSET 10U
#define SPD_TOKEN_ROLE_OFFSET 12U
#define SPD_TOKEN_HANDLER_OFFSET 16U
#define SPD_TOKEN_MACHINE_OFFSET 20U
#define SPD_TOKEN_COOKIE_OFFSET 24U
#define SPD_TOKEN_CHECKSUM_OFFSET 40U
#define SPD_TOKEN_RESERVED_OFFSET 44U
#define SPD_MAX_EINTR_RETRIES 8U

_Static_assert(sizeof(struct spd_rfork_resume_remote_request) == 8U,
               "Mitosis resume request must remain two u32 values");
_Static_assert(SPD_TOKEN_RESERVED_OFFSET + 4U == SPD_RFORK_ROLE_TOKEN_SIZE,
               "role-token offsets must cover the fixed token");

struct spd_decoded_token {
    enum spd_rfork_role role;
    uint32_t handler_id;
    uint32_t machine_id;
    uint8_t run_cookie[SPD_RFORK_RUN_COOKIE_SIZE];
    uint32_t checksum;
};

static int spd_ops_are_valid(const struct spd_rfork_io_ops *ops);

static void
spd_bytes_zero(void *destination, size_t length)
{
    uint8_t *bytes = (uint8_t *)destination;
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static void
spd_bytes_copy(void *destination, const void *source, size_t length)
{
    uint8_t *dst = (uint8_t *)destination;
    const uint8_t *src = (const uint8_t *)source;
    size_t index;

    for (index = 0U; index < length; ++index) {
        dst[index] = src[index];
    }
}

static int
spd_bytes_equal(const void *left, const void *right, size_t length)
{
    const uint8_t *lhs = (const uint8_t *)left;
    const uint8_t *rhs = (const uint8_t *)right;
    size_t index;
    uint8_t difference = 0U;

    for (index = 0U; index < length; ++index) {
        difference = (uint8_t)(difference | (uint8_t)(lhs[index] ^ rhs[index]));
    }
    return difference == 0U;
}

static int
spd_cookie_is_nonzero(const uint8_t cookie[SPD_RFORK_RUN_COOKIE_SIZE])
{
    size_t index;
    uint8_t accumulated = 0U;

    if (cookie == NULL) {
        return 0;
    }
    for (index = 0U; index < SPD_RFORK_RUN_COOKIE_SIZE; ++index) {
        accumulated = (uint8_t)(accumulated | cookie[index]);
    }
    return accumulated != 0U;
}

static void
spd_store_u16_le(uint8_t *destination, uint16_t value)
{
    destination[0] = (uint8_t)(value & UINT16_C(0xff));
    destination[1] = (uint8_t)((value >> 8U) & UINT16_C(0xff));
}

static void
spd_store_u32_le(uint8_t *destination, uint32_t value)
{
    destination[0] = (uint8_t)(value & UINT32_C(0xff));
    destination[1] = (uint8_t)((value >> 8U) & UINT32_C(0xff));
    destination[2] = (uint8_t)((value >> 16U) & UINT32_C(0xff));
    destination[3] = (uint8_t)((value >> 24U) & UINT32_C(0xff));
}

static uint16_t
spd_load_u16_le(const uint8_t *source)
{
    return (uint16_t)((uint16_t)source[0] |
                      (uint16_t)((uint16_t)source[1] << 8U));
}

static uint32_t
spd_load_u32_le(const uint8_t *source)
{
    return (uint32_t)source[0] |
           ((uint32_t)source[1] << 8U) |
           ((uint32_t)source[2] << 16U) |
           ((uint32_t)source[3] << 24U);
}

static uint32_t
spd_fnv1a32(const uint8_t *bytes, size_t length)
{
    uint32_t hash = UINT32_C(2166136261);
    size_t index;

    for (index = 0U; index < length; ++index) {
        hash ^= (uint32_t)bytes[index];
        hash *= UINT32_C(16777619);
    }
    return hash;
}

static int
spd_role_is_supported(enum spd_rfork_role role)
{
    return role == SPD_RFORK_ROLE_PARENT ||
           role == SPD_RFORK_ROLE_REMOTE_CHILD;
}

enum spd_rfork_status
spd_rfork_context_init(
    struct spd_rfork_context *context,
    uint64_t handler_id,
    uint32_t source_machine_id,
    uint32_t target_machine_id,
    const uint8_t run_cookie[SPD_RFORK_RUN_COOKIE_SIZE])
{
    if (context == NULL || run_cookie == NULL) {
        return SPD_RFORK_ERR_INVALID_ARGUMENT;
    }

    spd_bytes_zero(context, sizeof(*context));
    context->abi_version = SPD_RFORK_ABI_VERSION;

    if (handler_id > (uint64_t)UINT32_MAX) {
        return SPD_RFORK_ERR_HANDLER_OVERFLOW;
    }
    if (!spd_cookie_is_nonzero(run_cookie)) {
        return SPD_RFORK_ERR_COOKIE_INVALID;
    }

    context->handler_id = (uint32_t)handler_id;
    context->source_machine_id = source_machine_id;
    context->target_machine_id = target_machine_id;
    spd_bytes_copy(context->run_cookie, run_cookie, SPD_RFORK_RUN_COOKIE_SIZE);
    return SPD_RFORK_OK;
}

enum spd_rfork_status
spd_rfork_role_token_encode(
    uint8_t output[SPD_RFORK_ROLE_TOKEN_SIZE],
    enum spd_rfork_role role,
    uint64_t handler_id,
    uint32_t machine_id,
    const uint8_t run_cookie[SPD_RFORK_RUN_COOKIE_SIZE])
{
    uint32_t checksum;

    if (output == NULL || run_cookie == NULL || !spd_role_is_supported(role)) {
        return SPD_RFORK_ERR_INVALID_ARGUMENT;
    }
    spd_bytes_zero(output, SPD_RFORK_ROLE_TOKEN_SIZE);
    if (handler_id > (uint64_t)UINT32_MAX) {
        return SPD_RFORK_ERR_HANDLER_OVERFLOW;
    }
    if (!spd_cookie_is_nonzero(run_cookie)) {
        return SPD_RFORK_ERR_COOKIE_INVALID;
    }

    output[0] = SPD_ROLE_MAGIC_0;
    output[1] = SPD_ROLE_MAGIC_1;
    output[2] = SPD_ROLE_MAGIC_2;
    output[3] = SPD_ROLE_MAGIC_3;
    output[4] = SPD_ROLE_MAGIC_4;
    output[5] = SPD_ROLE_MAGIC_5;
    output[6] = SPD_ROLE_MAGIC_6;
    output[7] = SPD_ROLE_MAGIC_7;
    spd_store_u16_le(&output[SPD_TOKEN_ABI_OFFSET],
                     (uint16_t)SPD_RFORK_ABI_VERSION);
    spd_store_u16_le(&output[SPD_TOKEN_SIZE_OFFSET],
                     (uint16_t)SPD_RFORK_ROLE_TOKEN_SIZE);
    spd_store_u32_le(&output[SPD_TOKEN_ROLE_OFFSET], (uint32_t)role);
    spd_store_u32_le(&output[SPD_TOKEN_HANDLER_OFFSET], (uint32_t)handler_id);
    spd_store_u32_le(&output[SPD_TOKEN_MACHINE_OFFSET], machine_id);
    spd_bytes_copy(&output[SPD_TOKEN_COOKIE_OFFSET],
                   run_cookie,
                   SPD_RFORK_RUN_COOKIE_SIZE);
    checksum = spd_fnv1a32(output, SPD_TOKEN_CHECKSUM_OFFSET);
    spd_store_u32_le(&output[SPD_TOKEN_CHECKSUM_OFFSET], checksum);
    return SPD_RFORK_OK;
}

enum spd_rfork_status
spd_rfork_role_token_install(
    const struct spd_rfork_io_ops *requested_ops,
    enum spd_rfork_role role,
    uint64_t handler_id,
    uint32_t machine_id,
    const uint8_t run_cookie[SPD_RFORK_RUN_COOKIE_SIZE],
    int *errno_out)
{
    const struct spd_rfork_io_ops *ops = requested_ops;
    uint8_t wire[SPD_RFORK_ROLE_TOKEN_SIZE];
    enum spd_rfork_status status;
    size_t completed = 0U;
    unsigned int interrupted = 0U;

    if (errno_out == NULL) {
        return SPD_RFORK_ERR_INVALID_ARGUMENT;
    }
    *errno_out = 0;
    if (ops == NULL) {
        ops = spd_rfork_default_io_ops();
    }
    if (!spd_ops_are_valid(ops)) {
        return SPD_RFORK_ERR_IO_OPS_INVALID;
    }
    status = spd_rfork_role_token_encode(
        wire, role, handler_id, machine_id, run_cookie);
    if (status != SPD_RFORK_OK) {
        return status;
    }

    while (completed < sizeof(wire)) {
        int call_errno = 0;
        ssize_t result = ops->pwrite_call(
            ops->opaque,
            SPD_RFORK_ROLE_FD,
            &wire[completed],
            sizeof(wire) - completed,
            (int64_t)completed,
            &call_errno);

        if (result < 0) {
            if (call_errno == EINTR &&
                interrupted < SPD_MAX_EINTR_RETRIES) {
                ++interrupted;
                continue;
            }
            *errno_out = call_errno > 0 ? call_errno : EPROTO;
            return SPD_RFORK_ERR_ROLE_WRITE;
        }
        if (call_errno != 0 || result == 0 ||
            (size_t)result > sizeof(wire) - completed) {
            *errno_out = call_errno != 0 ? call_errno : EPROTO;
            return SPD_RFORK_ERR_ROLE_SHORT_WRITE;
        }
        completed += (size_t)result;
    }
    return SPD_RFORK_OK;
}

static enum spd_rfork_status
spd_decode_role_token(
    const uint8_t input[SPD_RFORK_ROLE_TOKEN_SIZE],
    struct spd_decoded_token *decoded)
{
    static const uint8_t magic[8] = {
        SPD_ROLE_MAGIC_0,
        SPD_ROLE_MAGIC_1,
        SPD_ROLE_MAGIC_2,
        SPD_ROLE_MAGIC_3,
        SPD_ROLE_MAGIC_4,
        SPD_ROLE_MAGIC_5,
        SPD_ROLE_MAGIC_6,
        SPD_ROLE_MAGIC_7
    };
    uint32_t checksum;
    uint32_t role;

    if (!spd_bytes_equal(input, magic, sizeof(magic)) ||
        spd_load_u16_le(&input[SPD_TOKEN_ABI_OFFSET]) !=
            (uint16_t)SPD_RFORK_ABI_VERSION ||
        spd_load_u16_le(&input[SPD_TOKEN_SIZE_OFFSET]) !=
            (uint16_t)SPD_RFORK_ROLE_TOKEN_SIZE ||
        spd_load_u32_le(&input[SPD_TOKEN_RESERVED_OFFSET]) != 0U) {
        return SPD_RFORK_ERR_ROLE_TOKEN;
    }

    checksum = spd_load_u32_le(&input[SPD_TOKEN_CHECKSUM_OFFSET]);
    if (checksum != spd_fnv1a32(input, SPD_TOKEN_CHECKSUM_OFFSET)) {
        return SPD_RFORK_ERR_ROLE_TOKEN;
    }

    role = spd_load_u32_le(&input[SPD_TOKEN_ROLE_OFFSET]);
    if (role != (uint32_t)SPD_RFORK_ROLE_PARENT &&
        role != (uint32_t)SPD_RFORK_ROLE_REMOTE_CHILD) {
        return SPD_RFORK_ERR_ROLE_TOKEN;
    }

    decoded->role = (enum spd_rfork_role)role;
    decoded->handler_id = spd_load_u32_le(&input[SPD_TOKEN_HANDLER_OFFSET]);
    decoded->machine_id = spd_load_u32_le(&input[SPD_TOKEN_MACHINE_OFFSET]);
    spd_bytes_copy(decoded->run_cookie,
                   &input[SPD_TOKEN_COOKIE_OFFSET],
                   SPD_RFORK_RUN_COOKIE_SIZE);
    decoded->checksum = checksum;
    return SPD_RFORK_OK;
}

static int
spd_ops_are_valid(const struct spd_rfork_io_ops *ops)
{
    return ops != NULL &&
           ops->open_device != NULL &&
           ops->ioctl_call != NULL &&
           ops->pread_call != NULL &&
           ops->pwrite_call != NULL &&
           ops->close_fd != NULL &&
           ops->get_process_id != NULL;
}

static void
spd_audit_init(
    struct spd_rfork_outcome *outcome,
    const struct spd_rfork_context *context,
    enum spd_rfork_operation operation)
{
    spd_bytes_zero(outcome, sizeof(*outcome));
    outcome->status = SPD_RFORK_ERR_INVALID_ARGUMENT;
    outcome->role = SPD_RFORK_ROLE_INVALID;
    outcome->audit.abi_version = SPD_RFORK_ABI_VERSION;
    outcome->audit.operation = operation;
    outcome->audit.stage = SPD_RFORK_STAGE_CONFIG;
    outcome->audit.status = SPD_RFORK_ERR_INVALID_ARGUMENT;
    outcome->audit.role_before = SPD_RFORK_ROLE_INVALID;
    outcome->audit.role_after = SPD_RFORK_ROLE_INVALID;
    outcome->audit.role_fd = SPD_RFORK_ROLE_FD;
    outcome->audit.device_fd = -1;
    outcome->audit.pid_before = -1;
    outcome->audit.pid_after = -1;
    outcome->audit.ioctl_result = -1;
    outcome->audit.close_result = -1;
    if (context != NULL) {
        outcome->audit.handler_id = context->handler_id;
        outcome->audit.source_machine_id = context->source_machine_id;
        outcome->audit.target_machine_id = context->target_machine_id;
        spd_bytes_copy(outcome->audit.run_cookie,
                       context->run_cookie,
                       SPD_RFORK_RUN_COOKIE_SIZE);
    }
}

static enum spd_rfork_status
spd_finish(
    struct spd_rfork_outcome *outcome,
    enum spd_rfork_status status,
    enum spd_rfork_stage stage)
{
    outcome->status = status;
    outcome->audit.status = status;
    outcome->audit.stage = stage;
    if (status != SPD_RFORK_OK) {
        outcome->role = SPD_RFORK_ROLE_INVALID;
    }
    return status;
}

static enum spd_rfork_status
spd_validate_context(const struct spd_rfork_context *context)
{
    if (context == NULL ||
        context->abi_version != SPD_RFORK_ABI_VERSION) {
        return SPD_RFORK_ERR_INVALID_ARGUMENT;
    }
    if (!spd_cookie_is_nonzero(context->run_cookie)) {
        return SPD_RFORK_ERR_COOKIE_INVALID;
    }
    return SPD_RFORK_OK;
}

static enum spd_rfork_status
spd_read_role_token(
    const struct spd_rfork_io_ops *ops,
    struct spd_decoded_token *decoded,
    struct spd_rfork_audit *audit,
    enum spd_rfork_stage stage)
{
    uint8_t wire[SPD_RFORK_ROLE_TOKEN_SIZE];
    size_t completed = 0U;
    unsigned int interrupted = 0U;

    spd_bytes_zero(wire, sizeof(wire));
    while (completed < sizeof(wire)) {
        int call_errno = 0;
        ssize_t result = ops->pread_call(
            ops->opaque,
            SPD_RFORK_ROLE_FD,
            &wire[completed],
            sizeof(wire) - completed,
            (int64_t)completed,
            &call_errno);

        if (result < 0) {
            if (call_errno == EINTR &&
                interrupted < SPD_MAX_EINTR_RETRIES) {
                ++interrupted;
                continue;
            }
            audit->role_read_errno = call_errno > 0 ? call_errno : EPROTO;
            audit->stage = stage;
            return SPD_RFORK_ERR_ROLE_READ;
        }
        if (call_errno != 0 || result == 0 ||
            (size_t)result > sizeof(wire) - completed) {
            audit->role_read_errno = call_errno != 0 ? call_errno : EPROTO;
            audit->stage = stage;
            return SPD_RFORK_ERR_ROLE_SHORT_READ;
        }
        completed += (size_t)result;
    }

    audit->role_read_errno = 0;
    return spd_decode_role_token(wire, decoded);
}

static enum spd_rfork_status
spd_validate_observed_role(
    const struct spd_rfork_context *context,
    const struct spd_decoded_token *token,
    enum spd_rfork_role required_role,
    int allow_parent_or_child)
{
    uint32_t required_machine;

    if (token->handler_id != context->handler_id ||
        !spd_bytes_equal(token->run_cookie,
                         context->run_cookie,
                         SPD_RFORK_RUN_COOKIE_SIZE)) {
        return SPD_RFORK_ERR_ROLE_MISMATCH;
    }

    if (allow_parent_or_child) {
        if (token->role == SPD_RFORK_ROLE_PARENT) {
            required_machine = context->source_machine_id;
        } else if (token->role == SPD_RFORK_ROLE_REMOTE_CHILD) {
            required_machine = context->target_machine_id;
        } else {
            return SPD_RFORK_ERR_ROLE_MISMATCH;
        }
    } else {
        if (token->role != required_role) {
            return SPD_RFORK_ERR_ROLE_MISMATCH;
        }
        required_machine =
            required_role == SPD_RFORK_ROLE_PARENT
                ? context->source_machine_id
                : context->target_machine_id;
    }

    return token->machine_id == required_machine
               ? SPD_RFORK_OK
               : SPD_RFORK_ERR_ROLE_MISMATCH;
}

static enum spd_rfork_status
spd_capture_pid(
    const struct spd_rfork_io_ops *ops,
    int64_t *destination,
    int *pid_errno)
{
    int call_errno = 0;
    int64_t result = ops->get_process_id(ops->opaque, &call_errno);

    if (result <= 0 || call_errno != 0) {
        *pid_errno = call_errno > 0 ? call_errno : EPROTO;
        return SPD_RFORK_ERR_PID;
    }
    *destination = result;
    *pid_errno = 0;
    return SPD_RFORK_OK;
}

static enum spd_rfork_status
spd_close_owned_device(
    const struct spd_rfork_io_ops *ops,
    struct spd_rfork_audit *audit)
{
    int call_errno = 0;
    int result;

    audit->close_attempted = 1U;
    result = ops->close_fd(ops->opaque, audit->device_fd, &call_errno);
    audit->close_result = result;
    audit->close_errno = result < 0
                             ? (call_errno > 0 ? call_errno : EPROTO)
                             : call_errno;
    if (result != 0 || call_errno != 0) {
        audit->close_succeeded = 0U;
        if (audit->close_errno == 0) {
            audit->close_errno = EPROTO;
        }
        return SPD_RFORK_ERR_DEVICE_CLOSE;
    }
    audit->close_succeeded = 1U;
    return SPD_RFORK_OK;
}

enum spd_rfork_status
spd_rfork_prepare(
    const struct spd_rfork_context *context,
    const struct spd_rfork_io_ops *requested_ops,
    struct spd_rfork_outcome *outcome)
{
    const struct spd_rfork_io_ops *ops = requested_ops;
    struct spd_decoded_token token;
    enum spd_rfork_status status;
    int call_errno = 0;
    int pid_errno = 0;
    long ioctl_result;

    if (outcome == NULL) {
        return SPD_RFORK_ERR_INVALID_ARGUMENT;
    }
    spd_audit_init(outcome, context, SPD_RFORK_OPERATION_PREPARE);

    status = spd_validate_context(context);
    if (status != SPD_RFORK_OK) {
        return spd_finish(outcome, status, SPD_RFORK_STAGE_CONFIG);
    }
    if (ops == NULL) {
        ops = spd_rfork_default_io_ops();
    }
    if (!spd_ops_are_valid(ops)) {
        return spd_finish(outcome,
                          SPD_RFORK_ERR_IO_OPS_INVALID,
                          SPD_RFORK_STAGE_CONFIG);
    }

    spd_bytes_zero(&token, sizeof(token));
    status = spd_read_role_token(
        ops, &token, &outcome->audit, SPD_RFORK_STAGE_ROLE_BEFORE);
    if (status != SPD_RFORK_OK) {
        return spd_finish(outcome, status, SPD_RFORK_STAGE_ROLE_BEFORE);
    }
    outcome->audit.role_before = token.role;
    outcome->audit.observed_token_machine_id = token.machine_id;
    outcome->audit.observed_token_checksum = token.checksum;
    status = spd_validate_observed_role(
        context, &token, SPD_RFORK_ROLE_PARENT, 0);
    if (status != SPD_RFORK_OK) {
        return spd_finish(outcome, status, SPD_RFORK_STAGE_ROLE_BEFORE);
    }

    status = spd_capture_pid(
        ops, &outcome->audit.pid_before, &pid_errno);
    if (status != SPD_RFORK_OK) {
        outcome->audit.pid_errno = pid_errno;
        return spd_finish(outcome, status, SPD_RFORK_STAGE_PID_BEFORE);
    }

    outcome->audit.device_fd =
        ops->open_device(ops->opaque, SPD_RFORK_DEVICE_PATH, &call_errno);
    outcome->audit.open_errno =
        outcome->audit.device_fd < 0
            ? (call_errno > 0 ? call_errno : EPROTO)
            : call_errno;
    if (outcome->audit.device_fd < 0 || call_errno != 0) {
        return spd_finish(outcome,
                          SPD_RFORK_ERR_DEVICE_OPEN,
                          SPD_RFORK_STAGE_DEVICE_OPEN);
    }
    outcome->audit.device_fd_safe_to_close = 1U;

    call_errno = 0;
    ioctl_result = ops->ioctl_call(
        ops->opaque,
        outcome->audit.device_fd,
        SPD_RFORK_IOCTL_PREPARE,
        (uintptr_t)context->handler_id,
        &call_errno);
    outcome->audit.ioctl_returned = 1U;
    outcome->audit.ioctl_result = ioctl_result;
    outcome->audit.ioctl_errno =
        ioctl_result != 0 ? (call_errno > 0 ? call_errno : EPROTO) : call_errno;
    if (ioctl_result != 0 || call_errno != 0) {
        enum spd_rfork_status close_status =
            spd_close_owned_device(ops, &outcome->audit);
        (void)close_status;
        return spd_finish(outcome,
                          SPD_RFORK_ERR_IOCTL,
                          SPD_RFORK_STAGE_IOCTL);
    }

    /*
     * A successful remote resume returns here using the parent's restored
     * registers and memory, but the target launcher's FD table.  Re-read the
     * fixed FD before touching device_fd again.
     */
    spd_bytes_zero(&token, sizeof(token));
    status = spd_read_role_token(
        ops, &token, &outcome->audit, SPD_RFORK_STAGE_ROLE_AFTER);
    if (status != SPD_RFORK_OK) {
        /*
         * The role is unknown, so device_fd may be a copied integer in a
         * target-local FD table.  Fail closed without close(2).
         */
        outcome->audit.device_fd_safe_to_close = 0U;
        return spd_finish(outcome, status, SPD_RFORK_STAGE_ROLE_AFTER);
    }
    outcome->audit.role_after = token.role;
    outcome->audit.observed_token_machine_id = token.machine_id;
    outcome->audit.observed_token_checksum = token.checksum;
    status = spd_validate_observed_role(context, &token,
                                        SPD_RFORK_ROLE_INVALID, 1);
    if (status != SPD_RFORK_OK) {
        outcome->audit.device_fd_safe_to_close = 0U;
        return spd_finish(outcome, status, SPD_RFORK_STAGE_ROLE_AFTER);
    }

    status = spd_capture_pid(ops, &outcome->audit.pid_after, &pid_errno);
    if (status != SPD_RFORK_OK) {
        outcome->audit.pid_errno = pid_errno;
        if (token.role == SPD_RFORK_ROLE_PARENT) {
            enum spd_rfork_status close_status =
                spd_close_owned_device(ops, &outcome->audit);
            (void)close_status;
        } else {
            outcome->audit.device_fd_safe_to_close = 0U;
        }
        return spd_finish(outcome, status, SPD_RFORK_STAGE_PID_AFTER);
    }

    outcome->role = token.role;
    if (token.role == SPD_RFORK_ROLE_PARENT) {
        /*
         * The file release hook unregisters the prepared handler.  Keep this
         * descriptor open until the remote child has stopped faulting parent
         * pages; the orchestrator releases it through the explicit API.
         */
        outcome->audit.device_fd_safe_to_close = 1U;
    } else {
        outcome->audit.device_fd_safe_to_close = 0U;
    }

    return spd_finish(outcome, SPD_RFORK_OK, SPD_RFORK_STAGE_COMPLETE);
}

enum spd_rfork_status
spd_rfork_parent_release(
    const struct spd_rfork_io_ops *requested_ops,
    struct spd_rfork_outcome *parent_outcome)
{
    const struct spd_rfork_io_ops *ops = requested_ops;
    enum spd_rfork_status status;

    if (parent_outcome == NULL) {
        return SPD_RFORK_ERR_INVALID_ARGUMENT;
    }
    if (ops == NULL) {
        ops = spd_rfork_default_io_ops();
    }
    if (!spd_ops_are_valid(ops)) {
        return SPD_RFORK_ERR_IO_OPS_INVALID;
    }
    if (parent_outcome->status != SPD_RFORK_OK ||
        parent_outcome->role != SPD_RFORK_ROLE_PARENT ||
        parent_outcome->audit.device_fd < 0 ||
        parent_outcome->audit.device_fd_safe_to_close != 1U ||
        parent_outcome->audit.close_attempted != 0U) {
        return SPD_RFORK_ERR_DEVICE_OWNERSHIP;
    }

    status = spd_close_owned_device(ops, &parent_outcome->audit);
    parent_outcome->audit.device_fd_safe_to_close = 0U;
    if (status != SPD_RFORK_OK) {
        parent_outcome->status = status;
        parent_outcome->audit.status = status;
        parent_outcome->audit.stage = SPD_RFORK_STAGE_DEVICE_CLOSE;
        return status;
    }
    return SPD_RFORK_OK;
}

enum spd_rfork_status
spd_rfork_resume_remote_or_error(
    const struct spd_rfork_context *context,
    const struct spd_rfork_io_ops *requested_ops,
    struct spd_rfork_outcome *outcome)
{
    const struct spd_rfork_io_ops *ops = requested_ops;
    struct spd_decoded_token token;
    struct spd_rfork_resume_remote_request request;
    enum spd_rfork_status status;
    int call_errno = 0;
    int pid_errno = 0;
    long ioctl_result;

    if (outcome == NULL) {
        return SPD_RFORK_ERR_INVALID_ARGUMENT;
    }
    spd_audit_init(outcome, context, SPD_RFORK_OPERATION_RESUME_REMOTE);

    status = spd_validate_context(context);
    if (status != SPD_RFORK_OK) {
        return spd_finish(outcome, status, SPD_RFORK_STAGE_CONFIG);
    }
    if (ops == NULL) {
        ops = spd_rfork_default_io_ops();
    }
    if (!spd_ops_are_valid(ops)) {
        return spd_finish(outcome,
                          SPD_RFORK_ERR_IO_OPS_INVALID,
                          SPD_RFORK_STAGE_CONFIG);
    }

    spd_bytes_zero(&token, sizeof(token));
    status = spd_read_role_token(
        ops, &token, &outcome->audit, SPD_RFORK_STAGE_ROLE_BEFORE);
    if (status != SPD_RFORK_OK) {
        return spd_finish(outcome, status, SPD_RFORK_STAGE_ROLE_BEFORE);
    }
    outcome->audit.role_before = token.role;
    outcome->audit.observed_token_machine_id = token.machine_id;
    outcome->audit.observed_token_checksum = token.checksum;
    status = spd_validate_observed_role(
        context, &token, SPD_RFORK_ROLE_REMOTE_CHILD, 0);
    if (status != SPD_RFORK_OK) {
        return spd_finish(outcome, status, SPD_RFORK_STAGE_ROLE_BEFORE);
    }

    status = spd_capture_pid(
        ops, &outcome->audit.pid_before, &pid_errno);
    if (status != SPD_RFORK_OK) {
        outcome->audit.pid_errno = pid_errno;
        return spd_finish(outcome, status, SPD_RFORK_STAGE_PID_BEFORE);
    }

    outcome->audit.device_fd =
        ops->open_device(ops->opaque, SPD_RFORK_DEVICE_PATH, &call_errno);
    outcome->audit.open_errno =
        outcome->audit.device_fd < 0
            ? (call_errno > 0 ? call_errno : EPROTO)
            : call_errno;
    if (outcome->audit.device_fd < 0 || call_errno != 0) {
        return spd_finish(outcome,
                          SPD_RFORK_ERR_DEVICE_OPEN,
                          SPD_RFORK_STAGE_DEVICE_OPEN);
    }
    outcome->audit.device_fd_safe_to_close = 1U;

    request.machine_id = context->source_machine_id;
    request.handler_id = context->handler_id;
    call_errno = 0;
    ioctl_result = ops->ioctl_call(
        ops->opaque,
        outcome->audit.device_fd,
        SPD_RFORK_IOCTL_RESUME_REMOTE,
        (uintptr_t)&request,
        &call_errno);

    /*
     * Reaching this assignment is itself evidence of an abnormal return.
     * On success the kernel restores the parent's prepare-side user context.
     */
    outcome->audit.ioctl_returned = 1U;
    outcome->audit.ioctl_result = ioctl_result;
    outcome->audit.ioctl_errno =
        ioctl_result < 0 ? (call_errno > 0 ? call_errno : EPROTO) : call_errno;
    status = (ioctl_result < 0 || call_errno != 0)
                 ? SPD_RFORK_ERR_IOCTL
                 : SPD_RFORK_ERR_RESUME_RETURNED;

    {
        enum spd_rfork_status close_status =
            spd_close_owned_device(ops, &outcome->audit);
        if (close_status != SPD_RFORK_OK) {
            return spd_finish(outcome,
                              SPD_RFORK_ERR_DEVICE_CLOSE,
                              SPD_RFORK_STAGE_DEVICE_CLOSE);
        }
    }
    return spd_finish(outcome, status, SPD_RFORK_STAGE_IOCTL);
}

static long
spd_raw_syscall4(long number, long arg1, long arg2, long arg3, long arg4)
{
    register long syscall_number __asm__("rax") = number;
    register long first __asm__("rdi") = arg1;
    register long second __asm__("rsi") = arg2;
    register long third __asm__("rdx") = arg3;
    register long fourth __asm__("r10") = arg4;

    __asm__ volatile("syscall"
                     : "+r"(syscall_number)
                     : "r"(first), "r"(second), "r"(third), "r"(fourth)
                     : "rcx", "r11", "memory");
    return syscall_number;
}

static long
spd_translate_raw_result(long result, int *errno_out)
{
    if (result < 0 && result >= -4095L) {
        *errno_out = (int)-result;
        return -1L;
    }
    *errno_out = 0;
    return result;
}

static int
spd_default_open_device(void *opaque, const char *path, int *errno_out)
{
    long result;
    (void)opaque;
    result = spd_raw_syscall4(
        SYS_openat, AT_FDCWD, (long)(uintptr_t)path, O_RDWR | O_CLOEXEC, 0L);
    return (int)spd_translate_raw_result(result, errno_out);
}

static long
spd_default_ioctl(
    void *opaque,
    int fd,
    unsigned long request,
    uintptr_t argument,
    int *errno_out)
{
    long result;
    (void)opaque;
    result = spd_raw_syscall4(
        SYS_ioctl, (long)fd, (long)request, (long)argument, 0L);
    return spd_translate_raw_result(result, errno_out);
}

static ssize_t
spd_default_pread(
    void *opaque,
    int fd,
    void *buffer,
    size_t length,
    int64_t offset,
    int *errno_out)
{
    long result;
    (void)opaque;
    result = spd_raw_syscall4(SYS_pread64,
                              (long)fd,
                              (long)(uintptr_t)buffer,
                              (long)length,
                              (long)offset);
    return (ssize_t)spd_translate_raw_result(result, errno_out);
}

static ssize_t
spd_default_pwrite(
    void *opaque,
    int fd,
    const void *buffer,
    size_t length,
    int64_t offset,
    int *errno_out)
{
    long result;
    (void)opaque;
    result = spd_raw_syscall4(SYS_pwrite64,
                              (long)fd,
                              (long)(uintptr_t)buffer,
                              (long)length,
                              (long)offset);
    return (ssize_t)spd_translate_raw_result(result, errno_out);
}

static int
spd_default_close(void *opaque, int fd, int *errno_out)
{
    long result;
    (void)opaque;
    result = spd_raw_syscall4(SYS_close, (long)fd, 0L, 0L, 0L);
    return (int)spd_translate_raw_result(result, errno_out);
}

static int64_t
spd_default_getpid(void *opaque, int *errno_out)
{
    long result;
    (void)opaque;
    result = spd_raw_syscall4(SYS_getpid, 0L, 0L, 0L, 0L);
    return (int64_t)spd_translate_raw_result(result, errno_out);
}

const struct spd_rfork_io_ops *
spd_rfork_default_io_ops(void)
{
    static const struct spd_rfork_io_ops operations = {
        NULL,
        spd_default_open_device,
        spd_default_ioctl,
        spd_default_pread,
        spd_default_pwrite,
        spd_default_close,
        spd_default_getpid
    };
    return &operations;
}

const char *
spd_rfork_status_name(enum spd_rfork_status status)
{
    switch (status) {
    case SPD_RFORK_OK:
        return "ok";
    case SPD_RFORK_ERR_INVALID_ARGUMENT:
        return "invalid_argument";
    case SPD_RFORK_ERR_HANDLER_OVERFLOW:
        return "handler_overflow";
    case SPD_RFORK_ERR_COOKIE_INVALID:
        return "cookie_invalid";
    case SPD_RFORK_ERR_IO_OPS_INVALID:
        return "io_ops_invalid";
    case SPD_RFORK_ERR_ROLE_READ:
        return "role_read";
    case SPD_RFORK_ERR_ROLE_SHORT_READ:
        return "role_short_read";
    case SPD_RFORK_ERR_ROLE_WRITE:
        return "role_write";
    case SPD_RFORK_ERR_ROLE_SHORT_WRITE:
        return "role_short_write";
    case SPD_RFORK_ERR_ROLE_TOKEN:
        return "role_token";
    case SPD_RFORK_ERR_ROLE_MISMATCH:
        return "role_mismatch";
    case SPD_RFORK_ERR_PID:
        return "pid";
    case SPD_RFORK_ERR_DEVICE_OPEN:
        return "device_open";
    case SPD_RFORK_ERR_IOCTL:
        return "ioctl";
    case SPD_RFORK_ERR_DEVICE_CLOSE:
        return "device_close";
    case SPD_RFORK_ERR_RESUME_RETURNED:
        return "resume_returned";
    case SPD_RFORK_ERR_DEVICE_OWNERSHIP:
        return "device_ownership";
    default:
        return "unknown";
    }
}

const char *
spd_rfork_role_name(enum spd_rfork_role role)
{
    switch (role) {
    case SPD_RFORK_ROLE_PARENT:
        return "parent";
    case SPD_RFORK_ROLE_REMOTE_CHILD:
        return "remote_child";
    case SPD_RFORK_ROLE_INVALID:
    default:
        return "invalid";
    }
}
