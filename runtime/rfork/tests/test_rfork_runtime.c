#include "serverlessx/rfork_runtime.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#define TEST_HANDLER UINT64_C(4294967295)
#define SOURCE_MACHINE UINT32_C(2)
#define TARGET_MACHINE UINT32_C(3)

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            fprintf(stderr,                                                    \
                    "FAIL %s:%d: %s\n",                                        \
                    __FILE__,                                                  \
                    __LINE__,                                                  \
                    #condition);                                               \
            return 0;                                                          \
        }                                                                      \
    } while (0)

struct mock_io {
    uint8_t role_token[SPD_RFORK_ROLE_TOKEN_SIZE];
    uint8_t child_token[SPD_RFORK_ROLE_TOKEN_SIZE];
    int switch_to_child_on_prepare;
    int open_result;
    int open_errno;
    long ioctl_result;
    int ioctl_errno;
    int close_result;
    int close_errno;
    int64_t pid;
    int64_t child_pid;
    int getpid_errno;
    int pread_errno;
    size_t pread_limit;
    int return_eof_after_first_pread;
    int pwrite_errno;
    size_t pwrite_limit;
    int pread_calls;
    int pwrite_calls;
    int open_calls;
    int ioctl_calls;
    int close_calls;
    int getpid_calls;
    int last_fd;
    unsigned long last_request;
    uintptr_t last_argument;
    struct spd_rfork_resume_remote_request resume_request;
};

static const uint8_t run_cookie[SPD_RFORK_RUN_COOKIE_SIZE] = {
    0x10U, 0x11U, 0x12U, 0x13U,
    0x20U, 0x21U, 0x22U, 0x23U,
    0x30U, 0x31U, 0x32U, 0x33U,
    0x40U, 0x41U, 0x42U, 0x43U
};

static void
copy_bytes(void *destination, const void *source, size_t length)
{
    uint8_t *dst = (uint8_t *)destination;
    const uint8_t *src = (const uint8_t *)source;
    size_t index;

    for (index = 0U; index < length; ++index) {
        dst[index] = src[index];
    }
}

static void
mock_init(struct mock_io *mock)
{
    size_t index;

    for (index = 0U; index < sizeof(*mock); ++index) {
        ((uint8_t *)mock)[index] = 0U;
    }
    mock->open_result = 7;
    mock->ioctl_result = 0L;
    mock->close_result = 0;
    mock->pid = 1001;
    mock->child_pid = 2002;
    mock->last_fd = -1;
}

static int
mock_open_device(void *opaque, const char *path, int *errno_out)
{
    struct mock_io *mock = (struct mock_io *)opaque;
    const char *expected = SPD_RFORK_DEVICE_PATH;
    size_t index = 0U;

    while (path[index] != '\0' || expected[index] != '\0') {
        if (path[index] != expected[index]) {
            *errno_out = EINVAL;
            return -1;
        }
        ++index;
    }
    ++mock->open_calls;
    *errno_out = mock->open_errno;
    return mock->open_result;
}

static long
mock_ioctl(
    void *opaque,
    int fd,
    unsigned long request,
    uintptr_t argument,
    int *errno_out)
{
    struct mock_io *mock = (struct mock_io *)opaque;

    ++mock->ioctl_calls;
    mock->last_fd = fd;
    mock->last_request = request;
    mock->last_argument = argument;
    if (request == SPD_RFORK_IOCTL_RESUME_REMOTE && argument != 0U) {
        copy_bytes(&mock->resume_request,
                   (const void *)argument,
                   sizeof(mock->resume_request));
    }
    if (request == SPD_RFORK_IOCTL_PREPARE &&
        mock->switch_to_child_on_prepare) {
        copy_bytes(mock->role_token,
                   mock->child_token,
                   SPD_RFORK_ROLE_TOKEN_SIZE);
        mock->pid = mock->child_pid;
    }
    *errno_out = mock->ioctl_errno;
    return mock->ioctl_result;
}

static ssize_t
mock_pread(
    void *opaque,
    int fd,
    void *buffer,
    size_t length,
    int64_t offset,
    int *errno_out)
{
    struct mock_io *mock = (struct mock_io *)opaque;
    size_t available;
    size_t amount;

    ++mock->pread_calls;
    if (fd != SPD_RFORK_ROLE_FD || offset < 0 ||
        (uint64_t)offset > SPD_RFORK_ROLE_TOKEN_SIZE) {
        *errno_out = EBADF;
        return -1;
    }
    if (mock->pread_errno != 0) {
        *errno_out = mock->pread_errno;
        return -1;
    }
    if (mock->return_eof_after_first_pread && mock->pread_calls > 1) {
        *errno_out = 0;
        return 0;
    }
    available = SPD_RFORK_ROLE_TOKEN_SIZE - (size_t)offset;
    amount = length < available ? length : available;
    if (mock->pread_limit != 0U && amount > mock->pread_limit) {
        amount = mock->pread_limit;
    }
    copy_bytes(buffer, &mock->role_token[(size_t)offset], amount);
    *errno_out = 0;
    return (ssize_t)amount;
}

static ssize_t
mock_pwrite(
    void *opaque,
    int fd,
    const void *buffer,
    size_t length,
    int64_t offset,
    int *errno_out)
{
    struct mock_io *mock = (struct mock_io *)opaque;
    size_t available;
    size_t amount;

    ++mock->pwrite_calls;
    if (fd != SPD_RFORK_ROLE_FD || offset < 0 ||
        (uint64_t)offset > SPD_RFORK_ROLE_TOKEN_SIZE) {
        *errno_out = EBADF;
        return -1;
    }
    if (mock->pwrite_errno != 0) {
        *errno_out = mock->pwrite_errno;
        return -1;
    }
    available = SPD_RFORK_ROLE_TOKEN_SIZE - (size_t)offset;
    amount = length < available ? length : available;
    if (mock->pwrite_limit != 0U && amount > mock->pwrite_limit) {
        amount = mock->pwrite_limit;
    }
    copy_bytes(&mock->role_token[(size_t)offset], buffer, amount);
    *errno_out = 0;
    return (ssize_t)amount;
}

static int
mock_close(void *opaque, int fd, int *errno_out)
{
    struct mock_io *mock = (struct mock_io *)opaque;

    ++mock->close_calls;
    mock->last_fd = fd;
    *errno_out = mock->close_errno;
    return mock->close_result;
}

static int64_t
mock_getpid(void *opaque, int *errno_out)
{
    struct mock_io *mock = (struct mock_io *)opaque;

    ++mock->getpid_calls;
    *errno_out = mock->getpid_errno;
    return mock->pid;
}

static struct spd_rfork_io_ops
mock_ops(struct mock_io *mock)
{
    struct spd_rfork_io_ops ops = {
        mock,
        mock_open_device,
        mock_ioctl,
        mock_pread,
        mock_pwrite,
        mock_close,
        mock_getpid
    };
    return ops;
}

static int
make_context_and_tokens(
    struct spd_rfork_context *context,
    struct mock_io *mock)
{
    CHECK(spd_rfork_context_init(context,
                                 TEST_HANDLER,
                                 SOURCE_MACHINE,
                                 TARGET_MACHINE,
                                 run_cookie) == SPD_RFORK_OK);
    CHECK(spd_rfork_role_token_encode(mock->role_token,
                                      SPD_RFORK_ROLE_PARENT,
                                      TEST_HANDLER,
                                      SOURCE_MACHINE,
                                      run_cookie) == SPD_RFORK_OK);
    CHECK(spd_rfork_role_token_encode(mock->child_token,
                                      SPD_RFORK_ROLE_REMOTE_CHILD,
                                      TEST_HANDLER,
                                      TARGET_MACHINE,
                                      run_cookie) == SPD_RFORK_OK);
    return 1;
}

static int
test_parent_prepare(void)
{
    struct spd_rfork_context context;
    struct spd_rfork_outcome outcome;
    struct mock_io mock;
    struct spd_rfork_io_ops ops;

    mock_init(&mock);
    CHECK(make_context_and_tokens(&context, &mock));
    ops = mock_ops(&mock);

    CHECK(spd_rfork_prepare(&context, &ops, &outcome) == SPD_RFORK_OK);
    CHECK(outcome.role == SPD_RFORK_ROLE_PARENT);
    CHECK(outcome.audit.role_before == SPD_RFORK_ROLE_PARENT);
    CHECK(outcome.audit.role_after == SPD_RFORK_ROLE_PARENT);
    CHECK(outcome.audit.handler_id == UINT32_MAX);
    CHECK(outcome.audit.pid_before == 1001);
    CHECK(outcome.audit.pid_after == 1001);
    CHECK(outcome.audit.ioctl_result == 0);
    CHECK(outcome.audit.ioctl_errno == 0);
    CHECK(outcome.audit.close_attempted == 0U);
    CHECK(outcome.audit.device_fd_safe_to_close == 1U);
    CHECK(mock.open_calls == 1);
    CHECK(mock.ioctl_calls == 1);
    CHECK(mock.last_request == SPD_RFORK_IOCTL_PREPARE);
    CHECK(mock.last_argument == (uintptr_t)UINT32_MAX);
    CHECK(mock.close_calls == 0);
    CHECK(spd_rfork_parent_release(&ops, &outcome) == SPD_RFORK_OK);
    CHECK(outcome.audit.close_attempted == 1U);
    CHECK(outcome.audit.close_succeeded == 1U);
    CHECK(outcome.audit.device_fd_safe_to_close == 0U);
    CHECK(mock.close_calls == 1);
    CHECK(spd_rfork_parent_release(&ops, &outcome) ==
          SPD_RFORK_ERR_DEVICE_OWNERSHIP);
    CHECK(mock.close_calls == 1);
    return 1;
}

static int
test_remote_child_detected_by_local_role_fd(void)
{
    struct spd_rfork_context context;
    struct spd_rfork_outcome outcome;
    struct mock_io mock;
    struct spd_rfork_io_ops ops;

    mock_init(&mock);
    CHECK(make_context_and_tokens(&context, &mock));
    mock.switch_to_child_on_prepare = 1;
    ops = mock_ops(&mock);

    CHECK(spd_rfork_prepare(&context, &ops, &outcome) == SPD_RFORK_OK);
    CHECK(outcome.role == SPD_RFORK_ROLE_REMOTE_CHILD);
    CHECK(outcome.audit.role_before == SPD_RFORK_ROLE_PARENT);
    CHECK(outcome.audit.role_after == SPD_RFORK_ROLE_REMOTE_CHILD);
    CHECK(outcome.audit.observed_token_machine_id == TARGET_MACHINE);
    CHECK(outcome.audit.pid_before == 1001);
    CHECK(outcome.audit.pid_after == 2002);
    CHECK(outcome.audit.ioctl_result == 0);
    CHECK(outcome.audit.device_fd_safe_to_close == 0U);
    CHECK(outcome.audit.close_attempted == 0U);
    CHECK(mock.close_calls == 0);
    return 1;
}

static int
test_open_failure_is_closed(void)
{
    struct spd_rfork_context context;
    struct spd_rfork_outcome outcome;
    struct mock_io mock;
    struct spd_rfork_io_ops ops;

    mock_init(&mock);
    CHECK(make_context_and_tokens(&context, &mock));
    mock.open_result = -1;
    mock.open_errno = ENOENT;
    ops = mock_ops(&mock);

    CHECK(spd_rfork_prepare(&context, &ops, &outcome) ==
          SPD_RFORK_ERR_DEVICE_OPEN);
    CHECK(outcome.role == SPD_RFORK_ROLE_INVALID);
    CHECK(outcome.audit.stage == SPD_RFORK_STAGE_DEVICE_OPEN);
    CHECK(outcome.audit.open_errno == ENOENT);
    CHECK(mock.ioctl_calls == 0);
    CHECK(mock.close_calls == 0);
    return 1;
}

static int
test_prepare_ioctl_failure_is_closed(void)
{
    struct spd_rfork_context context;
    struct spd_rfork_outcome outcome;
    struct mock_io mock;
    struct spd_rfork_io_ops ops;

    mock_init(&mock);
    CHECK(make_context_and_tokens(&context, &mock));
    mock.ioctl_result = -1;
    mock.ioctl_errno = EIO;
    ops = mock_ops(&mock);

    CHECK(spd_rfork_prepare(&context, &ops, &outcome) ==
          SPD_RFORK_ERR_IOCTL);
    CHECK(outcome.audit.ioctl_errno == EIO);
    CHECK(outcome.audit.close_attempted == 1U);
    CHECK(outcome.audit.close_succeeded == 1U);
    CHECK(mock.close_calls == 1);
    return 1;
}

static int
test_parent_close_failure_is_closed(void)
{
    struct spd_rfork_context context;
    struct spd_rfork_outcome outcome;
    struct mock_io mock;
    struct spd_rfork_io_ops ops;

    mock_init(&mock);
    CHECK(make_context_and_tokens(&context, &mock));
    ops = mock_ops(&mock);

    CHECK(spd_rfork_prepare(&context, &ops, &outcome) == SPD_RFORK_OK);
    CHECK(outcome.role == SPD_RFORK_ROLE_PARENT);
    CHECK(mock.close_calls == 0);
    mock.close_result = -1;
    mock.close_errno = EBADF;
    CHECK(spd_rfork_parent_release(&ops, &outcome) ==
          SPD_RFORK_ERR_DEVICE_CLOSE);
    CHECK(outcome.role == SPD_RFORK_ROLE_PARENT);
    CHECK(outcome.audit.stage == SPD_RFORK_STAGE_DEVICE_CLOSE);
    CHECK(outcome.audit.close_attempted == 1U);
    CHECK(outcome.audit.close_succeeded == 0U);
    CHECK(outcome.audit.close_errno == EBADF);
    return 1;
}

static int
test_device_fd_zero_is_valid(void)
{
    struct spd_rfork_context context;
    struct spd_rfork_outcome outcome;
    struct mock_io mock;
    struct spd_rfork_io_ops ops;

    mock_init(&mock);
    CHECK(make_context_and_tokens(&context, &mock));
    mock.open_result = 0;
    ops = mock_ops(&mock);

    CHECK(spd_rfork_prepare(&context, &ops, &outcome) == SPD_RFORK_OK);
    CHECK(outcome.audit.device_fd == 0);
    CHECK(mock.last_fd == 0);
    CHECK(mock.ioctl_calls == 1);
    CHECK(mock.close_calls == 0);
    CHECK(spd_rfork_parent_release(&ops, &outcome) == SPD_RFORK_OK);
    CHECK(mock.close_calls == 1);
    return 1;
}

static int
test_handler_overflow_rejected_before_truncation(void)
{
    struct spd_rfork_context context;
    uint8_t token[SPD_RFORK_ROLE_TOKEN_SIZE];

    CHECK(spd_rfork_context_init(&context,
                                 (uint64_t)UINT32_MAX + UINT64_C(1),
                                 SOURCE_MACHINE,
                                 TARGET_MACHINE,
                                 run_cookie) ==
          SPD_RFORK_ERR_HANDLER_OVERFLOW);
    CHECK(context.handler_id == 0U);
    CHECK(spd_rfork_role_token_encode(token,
                                      SPD_RFORK_ROLE_PARENT,
                                      (uint64_t)UINT32_MAX + UINT64_C(1),
                                      SOURCE_MACHINE,
                                      run_cookie) ==
          SPD_RFORK_ERR_HANDLER_OVERFLOW);
    return 1;
}

static int
test_resume_normal_return_is_anomaly(void)
{
    struct spd_rfork_context context;
    struct spd_rfork_outcome outcome;
    struct mock_io mock;
    struct spd_rfork_io_ops ops;

    mock_init(&mock);
    CHECK(make_context_and_tokens(&context, &mock));
    copy_bytes(mock.role_token,
               mock.child_token,
               SPD_RFORK_ROLE_TOKEN_SIZE);
    mock.pid = mock.child_pid;
    mock.ioctl_result = 0;
    ops = mock_ops(&mock);

    CHECK(spd_rfork_resume_remote_or_error(&context, &ops, &outcome) ==
          SPD_RFORK_ERR_RESUME_RETURNED);
    CHECK(outcome.audit.ioctl_returned == 1U);
    CHECK(outcome.audit.ioctl_result == 0);
    CHECK(outcome.audit.stage == SPD_RFORK_STAGE_IOCTL);
    CHECK(mock.last_request == SPD_RFORK_IOCTL_RESUME_REMOTE);
    CHECK(mock.resume_request.machine_id == SOURCE_MACHINE);
    CHECK(mock.resume_request.handler_id == UINT32_MAX);
    CHECK(mock.close_calls == 1);
    return 1;
}

static int
test_resume_ioctl_failure_is_closed(void)
{
    struct spd_rfork_context context;
    struct spd_rfork_outcome outcome;
    struct mock_io mock;
    struct spd_rfork_io_ops ops;

    mock_init(&mock);
    CHECK(make_context_and_tokens(&context, &mock));
    copy_bytes(mock.role_token,
               mock.child_token,
               SPD_RFORK_ROLE_TOKEN_SIZE);
    mock.ioctl_result = -1;
    mock.ioctl_errno = ENOTTY;
    ops = mock_ops(&mock);

    CHECK(spd_rfork_resume_remote_or_error(&context, &ops, &outcome) ==
          SPD_RFORK_ERR_IOCTL);
    CHECK(outcome.audit.ioctl_errno == ENOTTY);
    CHECK(outcome.audit.close_succeeded == 1U);
    return 1;
}

static int
test_role_read_failure_and_short_read_are_closed(void)
{
    struct spd_rfork_context context;
    struct spd_rfork_outcome outcome;
    struct mock_io mock;
    struct spd_rfork_io_ops ops;

    mock_init(&mock);
    CHECK(make_context_and_tokens(&context, &mock));
    mock.pread_errno = EBADF;
    ops = mock_ops(&mock);
    CHECK(spd_rfork_prepare(&context, &ops, &outcome) ==
          SPD_RFORK_ERR_ROLE_READ);
    CHECK(outcome.audit.role_read_errno == EBADF);
    CHECK(mock.open_calls == 0);

    mock_init(&mock);
    CHECK(make_context_and_tokens(&context, &mock));
    mock.pread_limit = 8U;
    mock.return_eof_after_first_pread = 1;
    ops = mock_ops(&mock);
    CHECK(spd_rfork_prepare(&context, &ops, &outcome) ==
          SPD_RFORK_ERR_ROLE_SHORT_READ);
    CHECK(mock.open_calls == 0);
    return 1;
}

static int
test_token_mismatch_is_closed(void)
{
    struct spd_rfork_context context;
    struct spd_rfork_outcome outcome;
    struct mock_io mock;
    struct spd_rfork_io_ops ops;
    uint8_t other_cookie[SPD_RFORK_RUN_COOKIE_SIZE];
    size_t index;

    mock_init(&mock);
    CHECK(make_context_and_tokens(&context, &mock));
    for (index = 0U; index < sizeof(other_cookie); ++index) {
        other_cookie[index] = (uint8_t)(index + 1U);
    }
    CHECK(spd_rfork_role_token_encode(mock.role_token,
                                      SPD_RFORK_ROLE_PARENT,
                                      TEST_HANDLER,
                                      SOURCE_MACHINE,
                                      other_cookie) == SPD_RFORK_OK);
    ops = mock_ops(&mock);
    CHECK(spd_rfork_prepare(&context, &ops, &outcome) ==
          SPD_RFORK_ERR_ROLE_MISMATCH);
    CHECK(mock.open_calls == 0);
    return 1;
}

static int
test_role_token_write_checks_partial_and_failure(void)
{
    struct mock_io mock;
    struct spd_rfork_io_ops ops;
    int call_errno = -1;

    mock_init(&mock);
    mock.pwrite_limit = 7U;
    ops = mock_ops(&mock);
    CHECK(spd_rfork_role_token_install(&ops,
                                       SPD_RFORK_ROLE_PARENT,
                                       TEST_HANDLER,
                                       SOURCE_MACHINE,
                                       run_cookie,
                                       &call_errno) == SPD_RFORK_OK);
    CHECK(call_errno == 0);
    CHECK(mock.pwrite_calls == 7);

    mock.pread_calls = 0;
    CHECK(spd_rfork_role_token_encode(mock.child_token,
                                      SPD_RFORK_ROLE_PARENT,
                                      TEST_HANDLER,
                                      SOURCE_MACHINE,
                                      run_cookie) == SPD_RFORK_OK);
    CHECK(spd_rfork_role_token_install(&ops,
                                       SPD_RFORK_ROLE_PARENT,
                                       TEST_HANDLER,
                                       SOURCE_MACHINE,
                                       run_cookie,
                                       &call_errno) == SPD_RFORK_OK);

    mock_init(&mock);
    mock.pwrite_errno = ENOSPC;
    ops = mock_ops(&mock);
    CHECK(spd_rfork_role_token_install(&ops,
                                       SPD_RFORK_ROLE_PARENT,
                                       TEST_HANDLER,
                                       SOURCE_MACHINE,
                                       run_cookie,
                                       &call_errno) ==
          SPD_RFORK_ERR_ROLE_WRITE);
    CHECK(call_errno == ENOSPC);
    return 1;
}

struct test_case {
    const char *name;
    int (*run)(void);
};

int
main(void)
{
    static const struct test_case tests[] = {
        {"parent_prepare", test_parent_prepare},
        {"remote_child_role_fd", test_remote_child_detected_by_local_role_fd},
        {"open_failure", test_open_failure_is_closed},
        {"prepare_ioctl_failure", test_prepare_ioctl_failure_is_closed},
        {"close_failure", test_parent_close_failure_is_closed},
        {"fd_zero", test_device_fd_zero_is_valid},
        {"handler_overflow", test_handler_overflow_rejected_before_truncation},
        {"resume_returned", test_resume_normal_return_is_anomaly},
        {"resume_ioctl_failure", test_resume_ioctl_failure_is_closed},
        {"role_read_failure", test_role_read_failure_and_short_read_are_closed},
        {"token_mismatch", test_token_mismatch_is_closed},
        {"role_token_write", test_role_token_write_checks_partial_and_failure}
    };
    size_t index;
    size_t passed = 0U;

    for (index = 0U; index < sizeof(tests) / sizeof(tests[0]); ++index) {
        if (!tests[index].run()) {
            fprintf(stderr, "not ok %zu - %s\n", index + 1U, tests[index].name);
            return EXIT_FAILURE;
        }
        ++passed;
        printf("ok %zu - %s\n", index + 1U, tests[index].name);
    }
    printf("1..%zu\n", passed);
    return EXIT_SUCCESS;
}
