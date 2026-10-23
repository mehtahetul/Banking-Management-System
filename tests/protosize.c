/* protosize.c - checks that protocol.h compiles and prints the message sizes.
 *
 * Build and run from the project root:
 *   gcc -Wall -Wextra -I common -I storage -o protosize tests/protosize.c
 *   ./protosize
 */
#include <stdio.h>

#include "protocol.h"

int main(void)
{
    printf("struct request  = %zu bytes\n", sizeof(struct request));
    printf("struct response = %zu bytes\n", sizeof(struct response));
    printf("server port     = %d\n", SERVER_PORT);
    return 0;
}
