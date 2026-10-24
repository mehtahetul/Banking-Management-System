/* main.c - connects to the server, shows the login screen, and then runs the
 * menu that belongs to the role the server reports.
 *
 * Run from anywhere:  ./bankclient     (the server must already be running)
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "client.h"

/* Ask for a username and password and log in. If it works, run the menu for
 * the user's role. Returns a MENU_* code. */
static int login_and_run(void)
{
    struct request req;
    struct response last;
    char username[32], password[128];
    int rc;

    if (read_line("Username: ", username, sizeof(username)) == -1)
        return MENU_EXIT;
    if (read_password("Password: ", password, sizeof(password)) == -1)
        return MENU_EXIT;

    memset(&req, 0, sizeof(req));
    req.opcode = OP_LOGIN;
    copy_field(req.u.login.username, sizeof(req.u.login.username), username);
    copy_field(req.u.login.password, sizeof(req.u.login.password), password);

    if (call_server(&req, &last) == -1)
        return MENU_LOST;
    if (last.status != ST_OK || last.kind != RESP_LOGIN)
        return MENU_STAY;                  /* the server already said why */

    printf("  Welcome, %s (%s).\n", username, role_name(last.u.login.role));
    switch (last.u.login.role) {
    case ROLE_CUSTOMER: rc = menu_customer(); break;
    case ROLE_EMPLOYEE: rc = menu_employee(); break;
    case ROLE_MANAGER:  rc = menu_manager();  break;
    case ROLE_ADMIN:    rc = menu_admin();    break;
    default:
        printf("  ! Unknown role from the server.\n");
        rc = action_logout();
        break;
    }
    return rc;
}

int main(void)
{
    struct sockaddr_in addr;
    int fd;

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
        perror("connect (is the server running?)");
        return 1;
    }
    ui_set_server(fd);

    for (;;) {
        int choice, rc;

        printf("\n=== Banking Management System ===\n"
               " 1. Login\n"
               " 0. Exit\n");
        if (read_int("Choice: ", &choice) == -1)
            break;
        if (choice == 0) {
            action_exit();
            break;
        }
        if (choice != 1) {
            printf("  ! Unknown option.\n");
            continue;
        }

        rc = login_and_run();
        if (rc == MENU_EXIT || rc == MENU_LOST)
            break;
        /* MENU_LOGGED_OUT or MENU_STAY: back to the login screen */
    }

    close(fd);
    printf("Goodbye.\n");
    return 0;
}
