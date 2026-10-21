/* storage.h - the ONLY layer that touches data files.
 *
 * Day 1 version: plain open/lseek/read/write, NO locking yet.
 * Day 2 will add fcntl record locks on top of these same functions.
 *
 * All functions work on "record numbers" (ids). Record n lives at byte
 * n * rec_size. Every function takes rec_size so one set of functions
 * works for every struct (use sizeof(struct User), etc).
 *
 * Return values: 0 or a positive number on success, -1 on error.
 */
#ifndef STORAGE_H
#define STORAGE_H

#include <stddef.h>

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

#endif
