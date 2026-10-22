/* storage.h - the ONLY layer that touches data files.
 *
 * Day 1: plain open/lseek/read/write.
 * Day 2: adds fcntl record locks (storage_lock / storage_unlock) and an
 *        "append lock" that stops two writers from picking the same new id.
 *
 * All functions work on "record numbers" (ids). Record n lives at byte
 * n * rec_size. Every function takes rec_size so one set of functions
 * works for every struct (use sizeof(struct User), etc).
 *
 * Return values: 0 or a positive number on success, -1 on error.
 *
 * HOW TO USE THE LOCKS (the caller does this around read-modify-write):
 *
 *     storage_lock(fd, id, sizeof(rec), F_WRLCK);   // wait until it is ours
 *     storage_read(fd, id, &rec, sizeof(rec));
 *     rec.balance += 100;
 *     storage_write(fd, id, &rec, sizeof(rec));
 *     storage_unlock(fd, id, sizeof(rec));
 *
 * Locks are ADVISORY: they only work if every process asks for them.
 */
#ifndef STORAGE_H
#define STORAGE_H

#include <stddef.h>
#include <fcntl.h>      /* for F_RDLCK and F_WRLCK, used by the caller */

/* Open (and create if missing) a data file. Returns a file descriptor, or -1. */
int storage_open(const char *path);

/* Close a file descriptor. */
int storage_close(int fd);

/* How many records are in the file? (file size / rec_size). -1 on error. */
int storage_count(int fd, size_t rec_size);

/* Read record number `id` into buf. -1 if id does not exist or on error. */
int storage_read(int fd, int id, void *buf, size_t rec_size);

/* Write buf as record number `id` (overwrites, or appends if id == count).
 * The caller sets the id field inside the struct before calling. */
int storage_write(int fd, int id, const void *buf, size_t rec_size);

/* ---------- Day 2: locking ---------- */

/* Lock the bytes of record `id`. type is F_RDLCK (shared, many readers) or
 * F_WRLCK (exclusive, one writer). BLOCKS (waits) until the lock is granted.
 * Returns 0 on success, -1 on error. errno == EDEADLK means the kernel
 * noticed a deadlock and refused the lock. */
int storage_lock(int fd, int id, size_t rec_size, int type);

/* Release the lock on record `id`. */
int storage_unlock(int fd, int id, size_t rec_size);

/* Take / release the append lock. Hold it while you do:
 *     count = storage_count(...);  fill the new record with id = count;
 *     storage_write(fd, count, ...);
 * so two clients can never pick the same new id. It locks one "sentinel"
 * byte far beyond any real record (see APPEND_LOCK_OFFSET in storage.c). */
int storage_append_lock(int fd);
int storage_append_unlock(int fd);

#endif
