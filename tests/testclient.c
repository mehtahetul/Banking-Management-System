/* testclient.c - throwaway test client. NOT the real menu client.
 *
 * Opens ONE connection to the server on this machine, then runs the words
 * you give it, in order, on that connection:
 *
 *   login <user> <pass>   send OP_LOGIN
 *   logout                send OP_LOGOUT
 *   echo <text>           send OP_ECHO
 *   raw <opcode>          send a request with this opcode and an empty payload
 *   exit                  send OP_EXIT
 *   sleep <seconds>       wait on the client side (connection stays open)
 *
 * Example:  ./testclient login hetul hetul123 echo hi logout exit
 *
 * Build from the project root:
 *   gcc -Wall -Wextra -I common -o testclient tests/testclient.c common/netio.c
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "protocol.h"
#include "netio.h"

static const char *role_name(int role)
{
    switch (role) {
    case ROLE_CUSTOMER: return "customer";
    case ROLE_EMPLOYEE: return "employee";
    case ROLE_MANAGER:  return "manager";
    case ROLE_ADMIN:    return "admin";
    default:            return "?";
    }
}

static void show(const struct response *r)
{
    printf("  <- [status %d] %s\n", r->status, r->message);
    if (r->kind == RESP_LOGIN)
        printf("     user_id=%d role=%s account=%d\n",
               r->u.login.user_id, role_name(r->u.login.role), r->u.login.account_id);
    else if (r->kind == RESP_TEXT)
        printf("     text: %s\n", r->u.text);
}

/* Send one request and print every response of the reply.
 * Returns 0 if the connection is still usable, -1 if not. */
static int send_and_show(int fd, const struct request *req)
{
    struct response r;

    if (write_full(fd, req, sizeof(*req)) == -1) {
        printf("  !! could not send: the server has closed the connection\n");
        return -1;
    }
    do {
        int rc = read_full(fd, &r, sizeof(r));

        if (rc == 0) {
            printf("  !! server closed the connection\n");
            return -1;
        }
        if (rc == -1) {
            printf("  !! connection error\n");
            return -1;
        }
        show(&r);
    } while (r.more);
    return 0;
}

static void usage(void)
{
    fprintf(stderr,
        "usage: testclient <word> ...\n"
        "  login <user> <pass> | logout | echo <text> | raw <opcode> | exit | sleep <seconds>\n");
}

int main(int argc, char *argv[])
{
    struct sockaddr_in addr;
    int fd, i = 1;

    if (argc < 2) {
        usage();
        return 1;
    }

    /* do not die if the server closes the connection while we write */
    signal(SIGPIPE, SIG_IGN);

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) {
        perror("socket");
        return 1;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(SERVER_PORT);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        perror("connect");
        return 1;
    }
    printf("connected to 127.0.0.1:%d\n", SERVER_PORT);

    while (i < argc) {
        struct request req;
        const char *word = argv[i];

        memset(&req, 0, sizeof(req));

        if (strcmp(word, "login") == 0 && i + 2 < argc) {
            req.opcode = OP_LOGIN;
            strncpy(req.u.login.username, argv[i + 1], sizeof(req.u.login.username) - 1);
            strncpy(req.u.login.password, argv[i + 2], sizeof(req.u.login.password) - 1);
            printf("-> login %s\n", argv[i + 1]);
            i += 3;
        } else if (strcmp(word, "logout") == 0) {
            req.opcode = OP_LOGOUT;
            printf("-> logout\n");
            i += 1;
        } else if (strcmp(word, "exit") == 0) {
            req.opcode = OP_EXIT;
            printf("-> exit\n");
            i += 1;
        } else if (strcmp(word, "echo") == 0 && i + 1 < argc) {
            req.opcode = OP_ECHO;
            strncpy(req.u.echo.text, argv[i + 1], sizeof(req.u.echo.text) - 1);
            printf("-> echo %s\n", argv[i + 1]);
            i += 2;
        } else if (strcmp(word, "raw") == 0 && i + 1 < argc) {
            req.opcode = atoi(argv[i + 1]);
            printf("-> raw opcode %d\n", req.opcode);
            i += 2;
        } else if (strcmp(word, "sleep") == 0 && i + 1 < argc) {
            printf("-- sleeping %s seconds (connection stays open)\n", argv[i + 1]);
            sleep((unsigned)atoi(argv[i + 1]));
            i += 2;
            continue;
        } else {
            usage();
            close(fd);
            return 1;
        }

        if (send_and_show(fd, &req) == -1)
            break;
    }

    close(fd);
    return 0;
}
