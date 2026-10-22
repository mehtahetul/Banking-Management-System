/* storage.c - record read/write using plain system calls, plus fcntl locks. */

/* Day 3: F_OFD_SETLKW is a GNU extension. This define MUST come before the
 * first #include, otherwise <fcntl.h> will not declare it. */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "storage.h"

/* The append lock locks ONE byte at this offset. No real record will ever
 * live this far into a file, so the byte is used only as a "someone is
 * appending" flag. (Locking bytes beyond the end of a file is allowed.) */
#define APPEND_LOCK_OFFSET  1000000000L

int storage_open(const char *path)
{
    /* O_RDWR: read and write. O_CREAT: create if missing. 0644: rw-r--r-- */
    return open(path, O_RDWR | O_CREAT, 0644);
}

int storage_close(int fd)
{
    return close(fd);
}

int storage_count(int fd, size_t rec_size)
{
    struct stat st;

    if (fstat(fd, &st) == -1)
        return -1;
    return (int)(st.st_size / (off_t)rec_size);
}

int storage_read(int fd, int id, void *buf, size_t rec_size)
{
    int count = storage_count(fd, rec_size);

    if (count == -1 || id < 0 || id >= count)
        return -1;

    /* jump to the start of record `id`, then read exactly one record */
    if (lseek(fd, (off_t)id * (off_t)rec_size, SEEK_SET) == (off_t)-1)
        return -1;
    if (read(fd, buf, rec_size) != (ssize_t)rec_size)
        return -1;
    return 0;
}

int storage_write(int fd, int id, const void *buf, size_t rec_size)
{
    int count = storage_count(fd, rec_size);

    /* allowed: overwrite an existing record, or append exactly at the end.
     * Not allowed: leaving a gap in the file. */
    if (count == -1 || id < 0 || id > count)
        return -1;

    if (lseek(fd, (off_t)id * (off_t)rec_size, SEEK_SET) == (off_t)-1)
        return -1;
    if (write(fd, buf, rec_size) != (ssize_t)rec_size)
        return -1;
    return 0;
}

/* ---------- Day 2: locking (changed to OFD locks on Day 3) ---------- */

/* Shared helper: apply one lock request on bytes [start, start+len).
 *
 * F_OFD_SETLKW = "set an Open File Description lock, and Wait if someone
 * else holds it". The lock belongs to the open file description (the thing
 * open() creates), NOT to the process. Each client thread opens its own fd,
 * so each thread is its own lock owner and threads block each other.
 *
 * (The classic F_SETLKW lock belonged to the whole process, so threads of
 * one process never blocked each other. threadrace showed the lost updates.)
 */
static int lock_range(int fd, short type, off_t start, off_t len)
{
    struct flock fl;

    memset(&fl, 0, sizeof(fl));
    fl.l_type   = type;        /* F_RDLCK, F_WRLCK or F_UNLCK */
    fl.l_whence = SEEK_SET;    /* l_start counts from the start of the file */
    fl.l_start  = start;
    fl.l_len    = len;
    fl.l_pid    = 0;           /* OFD locks REQUIRE this to be 0 */

    /* A signal can interrupt the wait; if so, just wait again. */
    while (fcntl(fd, F_OFD_SETLKW, &fl) == -1) {
        if (errno != EINTR)
            return -1;
    }
    return 0;
}

int storage_lock(int fd, int id, size_t rec_size, int type)
{
    if (id < 0 || (type != F_RDLCK && type != F_WRLCK))
        return -1;
    return lock_range(fd, (short)type, (off_t)id * (off_t)rec_size, (off_t)rec_size);
}

int storage_unlock(int fd, int id, size_t rec_size)
{
    if (id < 0)
        return -1;
    return lock_range(fd, F_UNLCK, (off_t)id * (off_t)rec_size, (off_t)rec_size);
}

int storage_append_lock(int fd)
{
    return lock_range(fd, F_WRLCK, (off_t)APPEND_LOCK_OFFSET, 1);
}

int storage_append_unlock(int fd)
{
    return lock_range(fd, F_UNLCK, (off_t)APPEND_LOCK_OFFSET, 1);
}
