#include <stdio.h>
#include <string.h>
#include "password.h"

static int failures = 0;

static void check(const char *what, int ok)
{
    printf("%-45s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok)
        failures++;
}

int main(void)
{
    char h1[HASH_SIZE], h2[HASH_SIZE];

    check("hash_password succeeds (1st)", hash_password("admin123", h1, sizeof(h1)) == 0);
    check("hash_password succeeds (2nd)", hash_password("admin123", h2, sizeof(h2)) == 0);
    printf("  sample hash: %s  (length %zu)\n", h1, strlen(h1));

    check("hash starts with $6$", strncmp(h1, "$6$", 3) == 0);
    check("fits in 128 bytes", strlen(h1) < HASH_SIZE);
    check("same password, different salt -> different hash", strcmp(h1, h2) != 0);
    check("correct password verifies (hash 1)", verify_password("admin123", h1) == 1);
    check("correct password verifies (hash 2)", verify_password("admin123", h2) == 1);
    check("wrong password rejected", verify_password("admin124", h1) == 0);
    check("empty password rejected", verify_password("", h1) == 0);
    check("old plain-text value is not a valid hash", verify_password("admin123", "admin123") == 0);
    check("NULL inputs handled", verify_password(NULL, h1) == 0 && verify_password("x", NULL) == 0);
    check("small output buffer refused", hash_password("x", h1, 10) == -1);

    printf("\n%s\n", failures == 0 ? "ALL PASSED" : "SOME FAILED");
    return failures != 0;
}
