/* handlers.c - one function per request. Only this layer (through the
 * storage layer) decides what a request means. Sockets are only written to
 * through write_full. */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "server.h"
#include "storage.h"
#include "netio.h"

/* ---------- helpers ---------- */

static int send_response(struct session *s, const struct response *r)
{
    return (write_full(s->fd, r, sizeof(*r)) == 0) ? KEEP_OPEN : SEND_FAILED;
}

/* Send one simple response: a status and a short text, nothing else. */
int reply(struct session *s, int status, const char *message)
{
    struct response r;

    memset(&r, 0, sizeof(r));
    r.status = status;
    r.kind = RESP_NONE;
    r.more = 0;
    strncpy(r.message, message, sizeof(r.message) - 1);
    return send_response(s, &r);
}

/* Look for a user by username in users.dat. The file is opened for this one
 * request only (so this thread has its own fd and its own locks), and each
 * record is read under a read lock.
 * Returns 1 and fills *out if found, 0 if not found, -1 on file error. */
static int find_user(const char *username, struct User *out)
{
    int fd = storage_open(USERS_FILE);
    int count, id, found = 0;

    if (fd == -1)
        return -1;
    count = storage_count(fd, sizeof(struct User));
    if (count == -1) {
        storage_close(fd);
        return -1;
    }

    for (id = 0; id < count && !found; id++) {
        struct User u;
        int rc;

        if (storage_lock(fd, id, sizeof(u), F_RDLCK) == -1) {
            storage_close(fd);
            return -1;
        }
        rc = storage_read(fd, id, &u, sizeof(u));
        storage_unlock(fd, id, sizeof(u));
        if (rc == -1) {
            storage_close(fd);
            return -1;
        }
        if (strncmp(u.username, username, sizeof(u.username)) == 0) {
            *out = u;
            found = 1;
        }
    }

    storage_close(fd);
    return found;
}

/* Results of check_password(). */
#define LOGIN_OK              0
#define LOGIN_WRONG_PASSWORD  1
#define LOGIN_LOCKED          2
#define LOGIN_ERROR           (-1)

/* Check a password for a user that exists, and keep that user's failed-attempt
 * count in users.dat. The whole read-check-update runs under ONE write lock on
 * the user's record, so two connections failing at the same moment cannot
 * lose a count (same idea as the balance update).
 *
 * Rules:
 *  - 30 minutes after the last wrong password the count starts again.
 *  - With MAX_LOGIN_ATTEMPTS wrong passwords inside that time the user is
 *    locked: even the correct password is refused.
 *  - A correct password sets the count back to 0.
 *
 * Fills *out (fresh copy of the record) on LOGIN_OK, *attempts on
 * LOGIN_WRONG_PASSWORD, *minutes_left on LOGIN_LOCKED. */
static int check_password(int user_id, const char *password, struct User *out,
                          int *attempts, int *minutes_left)
{
    struct User u;
    long long now;
    int changed = 0;
    int result;
    int fd = storage_open(USERS_FILE);

    if (fd == -1)
        return LOGIN_ERROR;
    if (storage_lock(fd, user_id, sizeof(u), F_WRLCK) == -1) {
        storage_close(fd);
        return LOGIN_ERROR;
    }
    if (storage_read(fd, user_id, &u, sizeof(u)) == -1) {
        storage_unlock(fd, user_id, sizeof(u));
        storage_close(fd);
        return LOGIN_ERROR;
    }

    now = (long long)time(NULL);

    /* old failures expire: the count starts again after LOCKOUT_SECONDS */
    if (u.failed_attempts > 0 && now - u.last_failed_time >= LOCKOUT_SECONDS) {
        u.failed_attempts = 0;
        u.last_failed_time = 0;
        changed = 1;
    }

    if (u.failed_attempts >= MAX_LOGIN_ATTEMPTS) {
        /* still locked: do not even look at the password */
        long long left = u.last_failed_time + LOCKOUT_SECONDS - now;
        *minutes_left = (int)((left + 59) / 60);          /* round up */
        result = LOGIN_LOCKED;
    } else if (strncmp(u.password, password, sizeof(u.password)) == 0) {
        /* Day 3: plain text comparison. Day 4 replaces this with crypt(). */
        if (u.failed_attempts != 0) {
            u.failed_attempts = 0;
            u.last_failed_time = 0;
            changed = 1;
        }
        *out = u;
        result = LOGIN_OK;
    } else {
        u.failed_attempts++;
        u.last_failed_time = now;
        changed = 1;
        *attempts = u.failed_attempts;
        result = LOGIN_WRONG_PASSWORD;
    }

    if (changed && storage_write(fd, user_id, &u, sizeof(u)) == -1)
        result = LOGIN_ERROR;

    storage_unlock(fd, user_id, sizeof(u));
    storage_close(fd);
    return result;
}

/* ---------- handlers ---------- */

int handle_login(struct session *s, const struct request *req)
{
    char username[32];
    char password[128];
    char msg[128];
    struct User u;        /* from the name search */
    struct User fresh;    /* re-read under the write lock */
    struct response r;
    int found, rc;
    int attempts = 0, minutes_left = 0;

    if (s->logged_in)
        return reply(s, ST_ERR_ALREADY_LOGGED_IN, "Already logged in on this connection");

    /* copy into buffers that are certain to end with a zero byte */
    memset(username, 0, sizeof(username));
    memset(password, 0, sizeof(password));
    strncpy(username, req->u.login.username, sizeof(username) - 1);
    strncpy(password, req->u.login.password, sizeof(password) - 1);

    found = find_user(username, &u);
    if (found == -1)
        return reply(s, ST_ERR_INTERNAL, "Server error while reading users");

    if (found == 0) {
        /* There is no record to keep a count in, so unknown names are only
         * limited per connection. */
        s->failed_logins++;
        printf("[fd %d] unknown user '%s' (%d of %d on this connection)\n",
               s->fd, username, s->failed_logins, MAX_LOGIN_ATTEMPTS);
        if (s->failed_logins >= MAX_LOGIN_ATTEMPTS) {
            if (reply(s, ST_ERR_NOT_FOUND,
                      "No such user exists. Too many failed attempts, disconnecting.") == SEND_FAILED)
                return SEND_FAILED;
            return CLOSE_CONN;
        }
        snprintf(msg, sizeof(msg), "No such user exists (attempt %d of %d)",
                 s->failed_logins, MAX_LOGIN_ATTEMPTS);
        return reply(s, ST_ERR_NOT_FOUND, msg);
    }

    rc = check_password(u.user_id, password, &fresh, &attempts, &minutes_left);

    if (rc == LOGIN_ERROR)
        return reply(s, ST_ERR_INTERNAL, "Server error while checking the login");

    if (rc == LOGIN_LOCKED) {
        printf("[fd %d] login refused: '%s' is locked (%d min left)\n",
               s->fd, username, minutes_left);
        snprintf(msg, sizeof(msg), "Account locked. Try again in %d minute(s).", minutes_left);
        return reply(s, ST_ERR_ACCOUNT_LOCKED, msg);
    }

    if (rc == LOGIN_WRONG_PASSWORD) {
        printf("[fd %d] wrong password for '%s' (%d of %d)\n",
               s->fd, username, attempts, MAX_LOGIN_ATTEMPTS);
        if (attempts >= MAX_LOGIN_ATTEMPTS) {
            snprintf(msg, sizeof(msg),
                     "Wrong password. Too many failed attempts: account locked for %d minutes.",
                     LOCKOUT_SECONDS / 60);
            if (reply(s, ST_ERR_ACCOUNT_LOCKED, msg) == SEND_FAILED)
                return SEND_FAILED;
            return CLOSE_CONN;
        }
        snprintf(msg, sizeof(msg), "Wrong password (attempt %d of %d)",
                 attempts, MAX_LOGIN_ATTEMPTS);
        return reply(s, ST_ERR_BAD_CREDENTIALS, msg);
    }

    /* LOGIN_OK */
    u = fresh;
    s->logged_in = 1;
    s->user_id = u.user_id;
    s->role = u.role;
    s->account_id = u.account_id;
    s->failed_logins = 0;
    printf("[fd %d] login ok: '%s' (user %d, role %d)\n",
           s->fd, username, s->user_id, s->role);

    memset(&r, 0, sizeof(r));
    r.status = ST_OK;
    r.kind = RESP_LOGIN;
    r.more = 0;
    strncpy(r.message, "Login successful", sizeof(r.message) - 1);
    r.u.login.user_id = s->user_id;
    r.u.login.role = s->role;
    r.u.login.account_id = s->account_id;
    return send_response(s, &r);
}

int handle_logout(struct session *s, const struct request *req)
{
    (void)req;
    printf("[fd %d] logout: user %d\n", s->fd, s->user_id);
    s->logged_in = 0;
    s->user_id = NO_ID;
    s->role = -1;
    s->account_id = NO_ID;
    return reply(s, ST_OK, "Logged out");
}

int handle_exit(struct session *s, const struct request *req)
{
    (void)req;
    if (reply(s, ST_OK, "Goodbye") == SEND_FAILED)
        return SEND_FAILED;
    return CLOSE_CONN;
}

/* Test request: send the text back. Needs no login. */
int handle_echo(struct session *s, const struct request *req)
{
    struct response r;

    memset(&r, 0, sizeof(r));
    r.status = ST_OK;
    r.kind = RESP_TEXT;
    r.more = 0;
    strncpy(r.message, "Echo", sizeof(r.message) - 1);
    strncpy(r.u.text, req->u.echo.text, sizeof(r.u.text) - 1);
    return send_response(s, &r);
}
