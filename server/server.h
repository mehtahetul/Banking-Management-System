/* server.h - shared by server.c, dispatcher.c and all the handlers_*.c files */
#ifndef SERVER_H
#define SERVER_H

#include "structs.h"
#include "protocol.h"

/* Login limits.
 * - A real user: after MAX_LOGIN_ATTEMPTS wrong passwords, logins for that
 *   user are refused on EVERY connection for LOCKOUT_SECONDS. The count is
 *   kept in the User record (failed_attempts, last_failed_time).
 * - An unknown username: counted per connection only; MAX_LOGIN_ATTEMPTS
 *   misses close the connection.
 * LOCKOUT_SECONDS can be overridden when compiling (-DLOCKOUT_SECONDS=5) to
 * test the unlock without waiting 30 minutes. */
#define MAX_LOGIN_ATTEMPTS  5
#ifndef LOCKOUT_SECONDS
#define LOCKOUT_SECONDS     (30 * 60)
#endif

/* Shortest password allowed (change password, add customer, add employee). */
#define MIN_PASSWORD_LENGTH 3

/* One of these per connected client. It lives in that client's thread only,
 * so no other thread touches it and it needs no lock. */
struct session {
    int fd;               /* the client's socket */
    int logged_in;        /* 0 or 1 */
    int user_id;          /* valid when logged_in */
    int role;             /* ROLE_*, valid when logged_in. Refreshed from the
                             stored record before every role-restricted request */
    int account_id;       /* the customer's account, NO_ID for staff */
    int failed_logins;    /* unknown usernames tried on this connection so far */
};

/* What dispatch() and every handler return. */
#define KEEP_OPEN    0    /* reply sent, wait for the next request */
#define CLOSE_CONN   1    /* reply sent, now close this client's connection */
#define SEND_FAILED  (-1) /* could not send the reply (client gone), close */

/* dispatcher.c: look up the opcode, check login and role, call the handler. */
int dispatch(struct session *s, const struct request *req);

/* ---------- handlers_common.c: helpers shared by every handler file ---------- */
int send_response(struct session *s, const struct response *r);   /* one response message */
int reply(struct session *s, int status, const char *message);    /* a status and a short text */
int fail(char *msg, int status, const char *text);                /* copy text into msg (128 bytes), return status */
void close_two(int a, int b);                                     /* storage_close both, skipping -1 */
int balance_reply(struct session *s, const char *message, long long balance);
int find_user(const char *username, struct User *out);            /* 1 found, 0 not found, -1 error */
int send_account_history(struct session *s, int account_id);      /* list reply, newest first */

/* ---------- handlers_auth.c ---------- */
int handle_login(struct session *s, const struct request *req);
int handle_logout(struct session *s, const struct request *req);
int handle_exit(struct session *s, const struct request *req);
int handle_change_password(struct session *s, const struct request *req);
void session_end(struct session *s);   /* clear the logged_in flag if still set */

/* ---------- handlers_customer.c ---------- */
int handle_view_balance(struct session *s, const struct request *req);
int handle_deposit(struct session *s, const struct request *req);
int handle_withdraw(struct session *s, const struct request *req);
int handle_transfer(struct session *s, const struct request *req);
int handle_apply_loan(struct session *s, const struct request *req);
int handle_add_feedback(struct session *s, const struct request *req);
int handle_view_history(struct session *s, const struct request *req);

/* ---------- handlers_employee.c ---------- */
int handle_add_customer(struct session *s, const struct request *req);
int handle_modify_customer(struct session *s, const struct request *req);
int handle_process_loan(struct session *s, const struct request *req);
int handle_view_assigned_loans(struct session *s, const struct request *req);
int handle_view_passbook(struct session *s, const struct request *req);

/* ---------- handlers_manager.c ---------- */
int handle_set_account_status(struct session *s, const struct request *req);
int handle_assign_loan(struct session *s, const struct request *req);
int handle_review_feedback(struct session *s, const struct request *req);

/* ---------- handlers_admin.c ---------- */
int handle_add_employee(struct session *s, const struct request *req);
int handle_modify_user(struct session *s, const struct request *req);
int handle_set_role(struct session *s, const struct request *req);

#endif
