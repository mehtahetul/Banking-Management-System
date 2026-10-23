/* dispatcher.c - opcode -> handler, with the login and role checks.
 * Every request goes through dispatch(), so the checks cannot be forgotten. */
#include <stddef.h>

#include "server.h"

#define ROLE_BIT(r)  (1 << (r))
#define EVERYONE     (ROLE_BIT(ROLE_CUSTOMER) | ROLE_BIT(ROLE_EMPLOYEE) | \
                      ROLE_BIT(ROLE_MANAGER)  | ROLE_BIT(ROLE_ADMIN))
#define CUSTOMER     ROLE_BIT(ROLE_CUSTOMER)
#define EMPLOYEE     ROLE_BIT(ROLE_EMPLOYEE)
#define MANAGER      ROLE_BIT(ROLE_MANAGER)
#define ADMIN        ROLE_BIT(ROLE_ADMIN)

struct rule {
    int opcode;
    int needs_login;   /* 0 = allowed before login */
    int roles;         /* which roles may use it (checked when needs_login) */
};

/* The permission table. Adding a feature later = one line here plus a handler. */
static const struct rule rules[] = {
    /* all roles */
    { OP_LOGIN,               0, EVERYONE },
    { OP_LOGOUT,              1, EVERYONE },
    { OP_EXIT,                0, EVERYONE },
    { OP_CHANGE_PASSWORD,     1, EVERYONE },
    { OP_ECHO,                0, EVERYONE },   /* test only, remove at the end */
    /* customer */
    { OP_VIEW_BALANCE,        1, CUSTOMER },
    { OP_DEPOSIT,             1, CUSTOMER },
    { OP_WITHDRAW,            1, CUSTOMER },
    { OP_TRANSFER,            1, CUSTOMER },
    { OP_APPLY_LOAN,          1, CUSTOMER },
    { OP_ADD_FEEDBACK,        1, CUSTOMER },
    { OP_VIEW_HISTORY,        1, CUSTOMER },
    /* employee */
    { OP_ADD_CUSTOMER,        1, EMPLOYEE },
    { OP_MODIFY_CUSTOMER,     1, EMPLOYEE },
    { OP_PROCESS_LOAN,        1, EMPLOYEE },
    { OP_VIEW_ASSIGNED_LOANS, 1, EMPLOYEE },
    { OP_VIEW_PASSBOOK,       1, EMPLOYEE },
    /* manager */
    { OP_SET_ACCOUNT_STATUS,  1, MANAGER },
    { OP_ASSIGN_LOAN,         1, MANAGER },
    { OP_REVIEW_FEEDBACK,     1, MANAGER },
    /* administrator */
    { OP_ADD_EMPLOYEE,        1, ADMIN },
    { OP_MODIFY_USER,         1, ADMIN },
    { OP_SET_ROLE,            1, ADMIN },
};

int dispatch(struct session *s, const struct request *req)
{
    const struct rule *rule = NULL;
    size_t i;

    for (i = 0; i < sizeof(rules) / sizeof(rules[0]); i++) {
        if (rules[i].opcode == req->opcode) {
            rule = &rules[i];
            break;
        }
    }
    if (rule == NULL)
        return reply(s, ST_ERR_BAD_REQUEST, "Unknown request");

    if (rule->needs_login) {
        if (!s->logged_in)
            return reply(s, ST_ERR_NOT_LOGGED_IN, "Please log in first");
        if (!(rule->roles & ROLE_BIT(s->role)))
            return reply(s, ST_ERR_PERMISSION, "Not allowed for your role");
    }

    switch (req->opcode) {
    case OP_LOGIN:   return handle_login(s, req);
    case OP_LOGOUT:  return handle_logout(s, req);
    case OP_EXIT:    return handle_exit(s, req);
    case OP_ECHO:    return handle_echo(s, req);
    default:         return reply(s, ST_ERR_BAD_REQUEST, "Not implemented yet");
    }
}
