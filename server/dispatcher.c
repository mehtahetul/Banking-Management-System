/* dispatcher.c - opcode -> handler, with the login and role checks.
 * Every request goes through dispatch(), so the checks cannot be forgotten. */
#include <stddef.h>

#include "server.h"
#include "storage.h"

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

/* Read the logged-in user's role from users.dat (under a read lock released
 * at once) and store it in the session. This makes a role change take effect
 * on the user's very next request, even if they were already logged in.
 * Returns 0, or -1 on a file error. */
static int refresh_role(struct session *s)
{
    struct User u;
    int fd = storage_open(USERS_FILE);
    int rc;

    if (fd == -1)
        return -1;
    if (storage_lock(fd, s->user_id, sizeof(u), F_RDLCK) == -1) {
        storage_close(fd);
        return -1;
    }
    rc = storage_read(fd, s->user_id, &u, sizeof(u));
    storage_unlock(fd, s->user_id, sizeof(u));
    storage_close(fd);

    if (rc == -1)
        return -1;
    s->role = u.role;
    return 0;
}

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

        /* Only requests limited to certain roles depend on the role, so only
         * those re-read it. Logout and change password (open to everyone)
         * skip the extra file work and cannot fail because of it. */
        if (rule->roles != EVERYONE && refresh_role(s) == -1)
            return reply(s, ST_ERR_INTERNAL, "Server error while checking your role");

        if (!(rule->roles & ROLE_BIT(s->role)))
            return reply(s, ST_ERR_PERMISSION, "Not allowed for your role");
    }

    switch (req->opcode) {
    case OP_LOGIN:   return handle_login(s, req);
    case OP_LOGOUT:  return handle_logout(s, req);
    case OP_EXIT:    return handle_exit(s, req);
    case OP_CHANGE_PASSWORD: return handle_change_password(s, req);
    case OP_ECHO:    return handle_echo(s, req);
    /* customer */
    case OP_VIEW_BALANCE: return handle_view_balance(s, req);
    case OP_DEPOSIT:      return handle_deposit(s, req);
    case OP_WITHDRAW:     return handle_withdraw(s, req);
    case OP_TRANSFER:     return handle_transfer(s, req);
    case OP_APPLY_LOAN:   return handle_apply_loan(s, req);
    case OP_ADD_FEEDBACK: return handle_add_feedback(s, req);
    case OP_VIEW_HISTORY: return handle_view_history(s, req);
    /* employee */
    case OP_ADD_CUSTOMER:        return handle_add_customer(s, req);
    case OP_MODIFY_CUSTOMER:     return handle_modify_customer(s, req);
    case OP_PROCESS_LOAN:        return handle_process_loan(s, req);
    case OP_VIEW_ASSIGNED_LOANS: return handle_view_assigned_loans(s, req);
    case OP_VIEW_PASSBOOK:       return handle_view_passbook(s, req);
    /* manager */
    case OP_SET_ACCOUNT_STATUS:  return handle_set_account_status(s, req);
    case OP_ASSIGN_LOAN:         return handle_assign_loan(s, req);
    case OP_REVIEW_FEEDBACK:     return handle_review_feedback(s, req);
    /* administrator */
    case OP_ADD_EMPLOYEE:        return handle_add_employee(s, req);
    case OP_MODIFY_USER:         return handle_modify_user(s, req);
    case OP_SET_ROLE:            return handle_set_role(s, req);
    default:         return reply(s, ST_ERR_BAD_REQUEST, "Not implemented yet");
    }
}
