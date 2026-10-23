#define _GNU_SOURCE            /* crypt_r is a GNU function */
#include <crypt.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include "password.h"

#define SALT_CHARS 16
/* "$6$" + 16 salt characters + "$" + terminating zero */
#define SALT_BUF   (3 + SALT_CHARS + 1 + 1)

/* The only characters crypt accepts in a salt. Exactly 64 of them,
   so (random byte & 63) picks one with no bias. */
static const char salt_alphabet[] =
    "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

/* Fill 'salt' with "$6$" + random characters + "$". Returns 0 or -1. */
static int make_salt(char *salt)
{
    unsigned char rnd[SALT_CHARS];
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0)
        return -1;

    /* read() may return fewer bytes than asked, so loop until full */
    size_t got = 0;
    while (got < sizeof(rnd)) {
        ssize_t n = read(fd, rnd + got, sizeof(rnd) - got);
        if (n <= 0) {
            close(fd);
            return -1;
        }
        got += (size_t)n;
    }
    close(fd);

    salt[0] = '$';
    salt[1] = '6';
    salt[2] = '$';
    for (int i = 0; i < SALT_CHARS; i++)
        salt[3 + i] = salt_alphabet[rnd[i] & 63];
    salt[3 + SALT_CHARS] = '$';
    salt[4 + SALT_CHARS] = '\0';
    return 0;
}

int hash_password(const char *plain, char *out, size_t out_size)
{
    char salt[SALT_BUF];
    struct crypt_data data;          /* our own buffer, so no sharing between threads */

    if (plain == NULL || out == NULL || out_size < HASH_SIZE)
        return -1;
    if (make_salt(salt) < 0)
        return -1;

    memset(&data, 0, sizeof(data));  /* crypt_r needs this zeroed before use */
    char *result = crypt_r(plain, salt, &data);

    /* crypt reports failure with NULL or a string starting with '*' */
    if (result == NULL || result[0] == '*')
        return -1;
    if (strlen(result) >= out_size)
        return -1;

    strcpy(out, result);
    return 0;
}

int verify_password(const char *plain, const char *stored)
{
    struct crypt_data data;

    if (plain == NULL || stored == NULL)
        return 0;

    memset(&data, 0, sizeof(data));
    /* the stored hash begins with its own salt, so it can be used as the salt */
    char *result = crypt_r(plain, stored, &data);
    if (result == NULL || result[0] == '*')
        return 0;

    return strcmp(result, stored) == 0;
}
