#ifndef PASSWORD_H
#define PASSWORD_H

#include <stddef.h>

/* A SHA-512 hash from crypt is about 106 characters, so 128 bytes
   (the size of User.password) is enough. */
#define HASH_SIZE 128

/* Make a salted SHA-512 hash of 'plain' and copy it into 'out'.
   out_size must be at least HASH_SIZE.
   Returns 0 on success, -1 on failure. */
int hash_password(const char *plain, char *out, size_t out_size);

/* Check a typed password against a stored hash.
   Returns 1 if it matches, 0 if not (or if anything goes wrong). */
int verify_password(const char *plain, const char *stored);

#endif
