/* handlers_auth.c - login, logout, exit, change password, echo.
 * These work for every role. */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "server.h"
#include "storage.h"
#include "password.h"
#include "sessions.h"

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
 * The password is checked with verify_password() against the stored hash.
 * If new_hash is not NULL (change password), then when the password is
 * correct the new hash is written into the record in the SAME locked step, so
 * checking the old password and storing the new one cannot be split apart.
 * Login passes NULL. The slow hashing of the new password is done by the
 * caller BEFORE this function, so the record lock is held as briefly as
 * possible.
 *
 * Fills *out (fresh copy of the record) on LOGIN_OK, *attempts on
 * LOGIN_WRONG_PASSWORD, *minutes_left on LOGIN_LOCKED. */
static int check_password(int user_id, const char *password, struct User *out,
                          int *attempts, int *minutes_left, const char *new_hash)
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
    } else if (verify_password(password, u.password)) {
        /* correct password */
        if (u.failed_attempts != 0) {
            u.failed_attempts = 0;
            u.last_failed_time = 0;
            changed = 1;
        }
        if (new_hash != NULL) {
            /* change password: store the new hash in this same locked step */
            memset(u.password, 0, sizeof(u.password));
            strncpy(u.password, new_hash, sizeof(u.password) - 1);
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

    rc = check_password(u.user_id, password, &fresh, &attempts, &minutes_left, NULL);

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

    /* LOGIN_OK: the password was right. Now claim the session flag.
     * This comes AFTER the password check, and being refused here does not
     * count as a failed attempt. */
    rc = sessions_login(fresh.user_id);
    if (rc == SESS_ALREADY) {
        printf("[fd %d] login refused: '%s' is already logged in\n", s->fd, username);
        return reply(s, ST_ERR_ALREADY_LOGGED_IN,
                     "This user is already logged in on another connection");
    }
    if (rc == SESS_ERROR)
        return reply(s, ST_ERR_INTERNAL, "Server error while starting the session");

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
    if (s->logged_in && sessions_logout(s->user_id) == -1)
        printf("[fd %d] could not clear the logged_in flag of user %d\n", s->fd, s->user_id);
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

/* Called by the client thread when the connection ends for ANY reason
 * (exit, client closed, client killed, lost connection). If the user never
 * logged out, clear the flag here so the session cannot get stuck. */
void session_end(struct session *s)
{
    if (!s->logged_in)
        return;
    if (sessions_logout(s->user_id) == -1)
        printf("[fd %d] could not clear the logged_in flag of user %d\n", s->fd, s->user_id);
    s->logged_in = 0;
}

/* Change the logged-in user's own password.
 * Rules: new password at least MIN_PASSWORD_LENGTH characters, and different
 * from the old one. The old password must be right; a wrong one counts towards
 * the same 5-strike lockout as a wrong login (and the 5th closes the
 * connection). After a successful change the user stays logged in. */
int handle_change_password(struct session *s, const struct request *req)
{
    char old_pw[128];
    char new_pw[128];
    char new_hash[HASH_SIZE];
    char msg[128];
    struct User fresh;
    int rc, attempts = 0, minutes_left = 0;

    /* copy into buffers that are certain to end with a zero byte */
    memset(old_pw, 0, sizeof(old_pw));
    memset(new_pw, 0, sizeof(new_pw));
    strncpy(old_pw, req->u.change_password.old_password, sizeof(old_pw) - 1);
    strncpy(new_pw, req->u.change_password.new_password, sizeof(new_pw) - 1);

    /* cheap checks first, before any hashing or locking */
    if (strlen(new_pw) < MIN_PASSWORD_LENGTH) {
        snprintf(msg, sizeof(msg), "New password must be at least %d characters",
                 MIN_PASSWORD_LENGTH);
        return reply(s, ST_ERR_BAD_REQUEST, msg);
    }
    if (strcmp(old_pw, new_pw) == 0)
        return reply(s, ST_ERR_BAD_REQUEST, "New password must be different from the old one");

    /* hash the new password now (slow), so the record lock stays short */
    if (hash_password(new_pw, new_hash, sizeof(new_hash)) == -1)
        return reply(s, ST_ERR_INTERNAL, "Server error while hashing the new password");

    rc = check_password(s->user_id, old_pw, &fresh, &attempts, &minutes_left, new_hash);

    if (rc == LOGIN_ERROR)
        return reply(s, ST_ERR_INTERNAL, "Server error while changing the password");

    if (rc == LOGIN_LOCKED) {
        printf("[fd %d] change password refused: user %d is locked (%d min left)\n",
               s->fd, s->user_id, minutes_left);
        snprintf(msg, sizeof(msg), "Account locked. Try again in %d minute(s).", minutes_left);
        return reply(s, ST_ERR_ACCOUNT_LOCKED, msg);
    }

    if (rc == LOGIN_WRONG_PASSWORD) {
        printf("[fd %d] wrong old password for user %d (%d of %d)\n",
               s->fd, s->user_id, attempts, MAX_LOGIN_ATTEMPTS);
        if (attempts >= MAX_LOGIN_ATTEMPTS) {
            snprintf(msg, sizeof(msg),
                     "Wrong password. Too many failed attempts: account locked for %d minutes.",
                     LOCKOUT_SECONDS / 60);
            if (reply(s, ST_ERR_ACCOUNT_LOCKED, msg) == SEND_FAILED)
                return SEND_FAILED;
            return CLOSE_CONN;
        }
        snprintf(msg, sizeof(msg), "Wrong old password (attempt %d of %d)",
                 attempts, MAX_LOGIN_ATTEMPTS);
        return reply(s, ST_ERR_BAD_CREDENTIALS, msg);
    }

    /* LOGIN_OK: the new hash is already stored. The session stays logged in. */
    printf("[fd %d] password changed: user %d\n", s->fd, s->user_id);
    return reply(s, ST_OK, "Password changed");
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
