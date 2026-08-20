#include "slsm_common.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

int main(void)
{
    int sockets[2];
    char byte = 0;

    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    assert(slsm_read_full_timeout(sockets[0], &byte, sizeof(byte), 20) != 0);
    assert(errno == ETIMEDOUT);
    assert(write(sockets[1], "x", sizeof(byte)) == (ssize_t)sizeof(byte));
    assert(slsm_read_full_timeout(sockets[0], &byte, sizeof(byte), 20) == 0);
    assert(byte == 'x');
    close(sockets[0]);
    close(sockets[1]);
    puts("{\"event\":\"common_test\",\"status\":\"pass\",\"timeout_ms\":20}");
    return 0;
}
