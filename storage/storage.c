/* storage.c - record read/write using plain system calls (Day 1, no locking). */
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "storage.h"

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
