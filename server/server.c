/* server.c - the socket and thread part of the server.
 * accept loop -> one detached thread per client -> read request -> dispatch.
 *
 * Run from the project root (the folder that contains data/):  ./bankserver
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "server.h"
#include "netio.h"
#include "sessions.h"

/* Runs in its own thread, one per connected client. */
static void *client_thread(void *arg)
{
    struct session *s = arg;
    struct request req;

    for (;;) {
        int rc = read_full(s->fd, &req, sizeof(req));

        if (rc == 0) {
            printf("[fd %d] client disconnected\n", s->fd);
            break;
        }
        if (rc == -1) {
            printf("[fd %d] connection lost\n", s->fd);
            break;
        }
        if (dispatch(s, &req) != KEEP_OPEN) {
            printf("[fd %d] closing connection\n", s->fd);
            break;
        }
    }

    session_end(s);        /* clear the logged_in flag if still set */
    close(s->fd);
    free(s);
    return NULL;
}

/* Ctrl+C or kill: remove the semaphore, then stop. */
static void stop_server(int sig)
{
    (void)sig;
    sessions_cleanup();
    _exit(0);
}

int main(void)
{
    int listen_fd;
    int yes = 1;
    struct sockaddr_in addr;

    /* Writing to a client that already disconnected would raise SIGPIPE and
     * kill the whole server. Ignore it: write() then just returns -1. */
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, stop_server);
    signal(SIGTERM, stop_server);

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd == -1) {
        perror("socket");
        return 1;
    }
    /* let us restart the server at once, without "address already in use" */
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) == -1) {
        perror("setsockopt");
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);   /* all network interfaces */
    addr.sin_port = htons(SERVER_PORT);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        perror("bind");
        return 1;
    }
    if (listen(listen_fd, SOMAXCONN) == -1) {
        perror("listen");
        return 1;
    }

    /* Only now, when we know no other server owns the port: create the
     * semaphore (removing an old one) and reset every logged_in flag. If this
     * ran before bind, a second server started by mistake would delete the
     * running server's semaphore and then fail at bind. */
    if (sessions_init() == -1)
        return 1;

    printf("Bank server listening on port %d\n", SERVER_PORT);

    for (;;) {
        struct sockaddr_in client_addr;
        socklen_t len = sizeof(client_addr);
        char ip[INET_ADDRSTRLEN];
        struct session *s;
        pthread_t tid;
        int rc;
        int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &len);

        if (client_fd == -1) {
            if (errno == EINTR)
                continue;
            perror("accept");
            if (errno == EMFILE || errno == ENFILE)
                sleep(1);                       /* out of descriptors: do not spin */
            continue;
        }

        inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));
        printf("[fd %d] connected from %s:%d\n", client_fd, ip, ntohs(client_addr.sin_port));

        /* the session goes on the heap: the thread owns it and frees it */
        s = calloc(1, sizeof(*s));
        if (s == NULL) {
            close(client_fd);
            continue;
        }
        s->fd = client_fd;
        s->user_id = NO_ID;
        s->role = -1;
        s->account_id = NO_ID;

        rc = pthread_create(&tid, NULL, client_thread, s);
        if (rc != 0) {
            fprintf(stderr, "pthread_create: %s\n", strerror(rc));
            close(client_fd);
            free(s);
            continue;
        }
        pthread_detach(tid);    /* nobody joins it; it cleans up when it ends */
    }
}
