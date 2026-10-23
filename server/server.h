/* server.h - shared by server.c, dispatcher.c and handlers.c */
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

/* One of these per connected client. It lives in that client's thread only,
 * so no other thread touches it and it needs no lock. */
struct session {
    int fd;               /* the client's socket */
    int logged_in;        /* 0 or 1 */
    int user_id;          /* valid when logged_in */
    int role;             /* ROLE_*, valid when logged_in */
    int account_id;       /* the customer's account, NO_ID for staff */
    int failed_logins;    /* unknown usernames tried on this connection so far */
};

/* What dispatch() and every handler return. */
#define KEEP_OPEN    0    /* reply sent, wait for the next request */
#define CLOSE_CONN   1    /* reply sent, now close this client's connection */
#define SEND_FAILED  (-1) /* could not send the reply (client gone), close */

/* dispatcher.c: look up the opcode, check login and role, call the handler. */
int dispatch(struct session *s, const struct request *req);

/* handlers.c */
int reply(struct session *s, int status, const char *message);  /* one simple response */
int handle_login(struct session *s, const struct request *req);
int handle_logout(struct session *s, const struct request *req);
int handle_exit(struct session *s, const struct request *req);
int handle_echo(struct session *s, const struct request *req);

#endif
