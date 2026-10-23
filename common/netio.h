/* netio.h - read and write an exact number of bytes on a socket (or pipe).
 *
 * Why: read() on a socket may return FEWER bytes than asked for, because the
 * data can arrive in pieces. write() can also write less than asked. These
 * two helpers keep going until the whole message has been transferred.
 *
 * Used by both the client and the server.
 */
#ifndef NETIO_H
#define NETIO_H

#include <stddef.h>

/* Read exactly n bytes into buf.
 * Returns  1  all n bytes were read.
 *          0  the other side closed the connection before sending anything
 *             (a normal disconnect).
 *         -1  error, or the connection closed in the middle of a message. */
int read_full(int fd, void *buf, size_t n);

/* Write exactly n bytes from buf.
 * Returns  0  all n bytes were written.
 *         -1  error (for example the other side has gone away). */
int write_full(int fd, const void *buf, size_t n);

#endif
