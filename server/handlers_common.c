/* handlers_common.c - small helpers used by every handlers_*.c file.
 * Sockets are only written to through write_full (here, in send_response). */
#include <stdio.h>
#include <string.h>

#include "server.h"
#include "storage.h"
#include "netio.h"

/* Send one response message. KEEP_OPEN if it went out, SEND_FAILED if not. */
int send_response(struct session *s, const struct response *r)
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

/* Put a text into msg (128 bytes) and return the status. Saves repeating
 * snprintf in every error branch. */
int fail(char *msg, int status, const char *text)
{
    snprintf(msg, 128, "%s", text);
    return status;
}

void close_two(int a, int b)
{
    if (a != -1)
        storage_close(a);
    if (b != -1)
        storage_close(b);
}

/* Send a RESP_BALANCE reply carrying a balance (in paise). */
int balance_reply(struct session *s, const char *message, long long balance)
{
    struct response r;

    memset(&r, 0, sizeof(r));
    r.status = ST_OK;
    r.kind = RESP_BALANCE;
    r.more = 0;
    strncpy(r.message, message, sizeof(r.message) - 1);
    r.u.balance.balance = balance;
    return send_response(s, &r);
}

/* Look for a user by username in users.dat. The file is opened for this one
 * request only (so this thread has its own fd and its own locks), and each
 * record is read under a read lock.
 * Returns 1 and fills *out if found, 0 if not found, -1 on file error. */
int find_user(const char *username, struct User *out)
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

/* Send every history row of one account as a list reply, newest first
 * (more = 1 for each row), then the end marker (more = 0). Used by the
 * customer's own history and by the employee's passbook.
 * The caller has already checked that the account id is valid. */
int send_account_history(struct session *s, int account_id)
{
    struct response r;
    struct Transaction t;
    char msg[128];
    int fd, count, id, rc, entries = 0;

    fd = storage_open(TXNS_FILE);
    if (fd == -1)
        return reply(s, ST_ERR_INTERNAL, "Server error opening the history file");
    count = storage_count(fd, sizeof(t));
    if (count == -1) {
        storage_close(fd);
        return reply(s, ST_ERR_INTERNAL, "Server error reading the history file");
    }

    /* last record first = newest first. Each record is read under a read
     * lock (and unlocked at once), so a record that is being written right now
     * is never read half-finished. No lock is held while sending. */
    for (id = count - 1; id >= 0; id--) {
        if (storage_lock(fd, id, sizeof(t), F_RDLCK) == -1) {
            storage_close(fd);
            return reply(s, ST_ERR_INTERNAL, "Server error locking the history");
        }
        rc = storage_read(fd, id, &t, sizeof(t));
        storage_unlock(fd, id, sizeof(t));
        if (rc == -1) {
            storage_close(fd);
            return reply(s, ST_ERR_INTERNAL, "Server error reading the history");
        }
        if (t.account_id != account_id)
            continue;

        memset(&r, 0, sizeof(r));
        r.status = ST_OK;
        r.kind = RESP_TXN;
        r.more = 1;
        strncpy(r.message, "Transaction", sizeof(r.message) - 1);
        r.u.txn = t;
        if (send_response(s, &r) == SEND_FAILED) {
            storage_close(fd);
            return SEND_FAILED;
        }
        entries++;
    }
    storage_close(fd);

    /* the end marker: more = 0 */
    snprintf(msg, sizeof(msg), "End of history (%d entries)", entries);
    return reply(s, ST_OK, msg);
}
