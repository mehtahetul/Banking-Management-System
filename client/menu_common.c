/* menu_common.c - actions that every role has, and a helper for simple requests. */
#include <stdio.h>
#include <string.h>

#include "client.h"

/* A request with no payload (view balance, history, feedback list, ...). */
int send_simple(int opcode)
{
    struct request req;

    memset(&req, 0, sizeof(req));
    req.opcode = opcode;
    if (call_server(&req, NULL) == -1)
        return MENU_LOST;
    return MENU_STAY;
}

/* Ask for the old password and the new one (twice), then let the server decide.
 * Checking that the two new passwords match is only for the user's comfort;
 * the server still checks the length and the old password. */
int action_change_password(void)
{
    char old_pw[128], new_pw[128], again[128];
    struct request req;

    if (read_password("Old password: ", old_pw, sizeof(old_pw)) == -1)
        return MENU_EXIT;
    if (read_password("New password: ", new_pw, sizeof(new_pw)) == -1)
        return MENU_EXIT;
    if (read_password("New password again: ", again, sizeof(again)) == -1)
        return MENU_EXIT;
    if (strcmp(new_pw, again) != 0) {
        printf("  ! The two new passwords do not match.\n");
        return MENU_STAY;
    }

    memset(&req, 0, sizeof(req));
    req.opcode = OP_CHANGE_PASSWORD;
    copy_field(req.u.change_password.old_password,
               sizeof(req.u.change_password.old_password), old_pw);
    copy_field(req.u.change_password.new_password,
               sizeof(req.u.change_password.new_password), new_pw);
    if (call_server(&req, NULL) == -1)
        return MENU_LOST;
    return MENU_STAY;
}

int action_logout(void)
{
    struct request req;

    memset(&req, 0, sizeof(req));
    req.opcode = OP_LOGOUT;
    if (call_server(&req, NULL) == -1)
        return MENU_LOST;
    return MENU_LOGGED_OUT;
}

int action_exit(void)
{
    struct request req;

    memset(&req, 0, sizeof(req));
    req.opcode = OP_EXIT;
    call_server(&req, NULL);        /* if the connection is already gone we leave anyway */
    return MENU_EXIT;
}
