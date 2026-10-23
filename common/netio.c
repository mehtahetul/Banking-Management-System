/* netio.c - read and write an exact number of bytes. See netio.h. */
#include <errno.h>
#include <unistd.h>

#include "netio.h"

int read_full(int fd, void *buf, size_t n)
{
    char  *p = buf;
    size_t got = 0;

    while (got < n) {
        ssize_t r = read(fd, p + got, n - got);

        if (r == -1) {
            if (errno == EINTR)        /* a signal interrupted us: try again */
                continue;
            return -1;
        }
        if (r == 0)                    /* the other side closed the connection */
            return (got == 0) ? 0 : -1;
        got += (size_t)r;
    }
    return 1;
}

int write_full(int fd, const void *buf, size_t n)
{
    const char *p = buf;
    size_t sent = 0;

    while (sent < n) {
        ssize_t w = write(fd, p + sent, n - sent);

        if (w == -1) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        sent += (size_t)w;
    }
    return 0;
}
