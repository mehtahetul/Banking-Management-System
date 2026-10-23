/* handlers.c - one function per request. Only this layer (through the
 * storage layer) decides what a request means. Sockets are only written to
 * through write_full. */
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "server.h"
#include "storage.h"
#include "netio.h"
#include "password.h"

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
 * Day 4: the password is checked with verify_password() against the stored
 * hash. If new_hash is not NULL (change password), then when the password is
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

/* ---------- Day 4: customer handlers ----------
 *
 * Pattern for every one of them:
 *   - the account is always the LOGGED-IN customer's (s->account_id), never
 *     something the client sends (except the receiving account of a transfer)
 *   - a small "do_" function opens the files, takes the locks, calls a
 *     "_locked" function that does the real work, then unlocks and closes
 *   - the reply is sent only AFTER everything is unlocked, so a slow client
 *     can never keep a record locked
 *
 * Lock order (never reverse it, or two clients can wait for each other forever):
 *   1. account records, LOWER account id first
 *   2. the append lock of txns.dat
 */

/* Put a text into msg (128 bytes) and return the status. Saves repeating
 * snprintf in every error branch. */
static int fail(char *msg, int status, const char *text)
{
    snprintf(msg, 128, "%s", text);
    return status;
}

static void close_two(int a, int b)
{
    if (a != -1)
        storage_close(a);
    if (b != -1)
        storage_close(b);
}

/* Send a RESP_BALANCE reply carrying a balance (in paise). */
static int balance_reply(struct session *s, const char *message, long long balance)
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

/* Add one record at the end of txns.dat.
 * The CALLER must already hold the append lock on fd. */
static int add_txn(int fd, int account_id, int type, long long amount,
                   long long balance_after, int other_account_id)
{
    struct Transaction t;
    int id = storage_count(fd, sizeof(t));

    if (id == -1)
        return -1;
    memset(&t, 0, sizeof(t));
    t.txn_id = id;
    t.account_id = account_id;
    t.type = type;
    t.amount = amount;
    t.balance_after = balance_after;
    t.other_account_id = other_account_id;
    t.timestamp = (long long)time(NULL);
    return storage_write(fd, id, &t, sizeof(t));
}

/* ----- view balance ----- */

int handle_view_balance(struct session *s, const struct request *req)
{
    struct Account a;
    int fd, rc;

    (void)req;
    if (s->account_id == NO_ID)
        return reply(s, ST_ERR_NOT_FOUND, "You have no account");

    fd = storage_open(ACCOUNTS_FILE);
    if (fd == -1)
        return reply(s, ST_ERR_INTERNAL, "Server error opening the accounts file");
    if (storage_lock(fd, s->account_id, sizeof(a), F_RDLCK) == -1) {
        storage_close(fd);
        return reply(s, ST_ERR_INTERNAL, "Server error locking the account");
    }
    rc = storage_read(fd, s->account_id, &a, sizeof(a));
    storage_unlock(fd, s->account_id, sizeof(a));
    storage_close(fd);

    if (rc == -1)
        return reply(s, ST_ERR_INTERNAL, "Server error reading the account");
    return balance_reply(s, "Current balance", a.balance);
}

/* ----- deposit and withdraw ----- */

/* Work done while the account record is write-locked. */
static int money_locked(int fd_acc, int fd_txn, int account_id, int type,
                        long long amount, long long *new_balance, char *msg)
{
    struct Account a;
    long long nb;
    int rc;

    if (storage_read(fd_acc, account_id, &a, sizeof(a)) == -1)
        return fail(msg, ST_ERR_INTERNAL, "Server error reading the account");
    if (a.status != ACC_ACTIVE)
        return fail(msg, ST_ERR_ACCOUNT_INACTIVE, "Your account is deactivated");

    if (type == TXN_DEPOSIT) {
        if (a.balance > LLONG_MAX - amount)        /* would overflow */
            return fail(msg, ST_ERR_INVALID_AMOUNT, "Amount too large for this account");
        nb = a.balance + amount;
    } else {
        if (a.balance < amount)
            return fail(msg, ST_ERR_INSUFFICIENT_FUNDS, "Insufficient funds");
        nb = a.balance - amount;
    }

    /* History row first, then the balance. If the history row cannot be
     * written, the balance is still untouched. (The crash-in-between case is
     * handled on Day 6.) */
    if (storage_append_lock(fd_txn) == -1)
        return fail(msg, ST_ERR_INTERNAL, "Server error locking the history file");
    rc = add_txn(fd_txn, account_id, type, amount, nb, NO_ID);
    storage_append_unlock(fd_txn);
    if (rc == -1)
        return fail(msg, ST_ERR_INTERNAL, "Server error writing the history");

    a.balance = nb;
    if (storage_write(fd_acc, account_id, &a, sizeof(a)) == -1)
        return fail(msg, ST_ERR_INTERNAL, "Server error writing the account");

    *new_balance = nb;
    return ST_OK;
}

static int do_money_op(int account_id, int type, long long amount,
                       long long *new_balance, char *msg)
{
    int fd_acc = storage_open(ACCOUNTS_FILE);
    int fd_txn = storage_open(TXNS_FILE);
    int st;

    if (fd_acc == -1 || fd_txn == -1) {
        close_two(fd_acc, fd_txn);
        return fail(msg, ST_ERR_INTERNAL, "Server error opening the data files");
    }
    if (storage_lock(fd_acc, account_id, sizeof(struct Account), F_WRLCK) == -1) {
        close_two(fd_acc, fd_txn);
        return fail(msg, ST_ERR_INTERNAL, "Server error locking the account");
    }

    st = money_locked(fd_acc, fd_txn, account_id, type, amount, new_balance, msg);

    storage_unlock(fd_acc, account_id, sizeof(struct Account));
    close_two(fd_acc, fd_txn);
    return st;
}

/* Shared by deposit and withdraw. */
static int money_handler(struct session *s, const struct request *req, int type)
{
    long long amount = req->u.amount.amount;
    long long new_balance = 0;
    char msg[128];
    int st;

    if (s->account_id == NO_ID)
        return reply(s, ST_ERR_NOT_FOUND, "You have no account");
    if (amount <= 0)
        return reply(s, ST_ERR_INVALID_AMOUNT, "Amount must be greater than zero");

    st = do_money_op(s->account_id, type, amount, &new_balance, msg);
    if (st != ST_OK)
        return reply(s, st, msg);
    return balance_reply(s, type == TXN_DEPOSIT ? "Deposit successful"
                                                : "Withdrawal successful", new_balance);
}

int handle_deposit(struct session *s, const struct request *req)
{
    return money_handler(s, req, TXN_DEPOSIT);
}

int handle_withdraw(struct session *s, const struct request *req)
{
    return money_handler(s, req, TXN_WITHDRAW);
}

/* ----- transfer ----- */

/* Work done while BOTH account records are write-locked. */
static int transfer_locked(int fd_acc, int fd_txn, int from, int to,
                           long long amount, long long *from_balance, char *msg)
{
    struct Account a_from, a_to;
    int rc1, rc2;

    if (storage_read(fd_acc, from, &a_from, sizeof(a_from)) == -1 ||
        storage_read(fd_acc, to, &a_to, sizeof(a_to)) == -1)
        return fail(msg, ST_ERR_INTERNAL, "Server error reading the accounts");

    if (a_from.status != ACC_ACTIVE)
        return fail(msg, ST_ERR_ACCOUNT_INACTIVE, "Your account is deactivated");
    if (a_to.status != ACC_ACTIVE)
        return fail(msg, ST_ERR_ACCOUNT_INACTIVE, "The receiving account is deactivated");
    if (a_from.balance < amount)
        return fail(msg, ST_ERR_INSUFFICIENT_FUNDS, "Insufficient funds");
    if (a_to.balance > LLONG_MAX - amount)
        return fail(msg, ST_ERR_INVALID_AMOUNT, "Amount too large for the receiving account");

    a_from.balance -= amount;
    a_to.balance += amount;

    /* both history rows are added under ONE append lock, so they get
     * neighbouring ids */
    if (storage_append_lock(fd_txn) == -1)
        return fail(msg, ST_ERR_INTERNAL, "Server error locking the history file");
    rc1 = add_txn(fd_txn, from, TXN_TRANSFER_OUT, amount, a_from.balance, to);
    rc2 = (rc1 == 0) ? add_txn(fd_txn, to, TXN_TRANSFER_IN, amount, a_to.balance, from) : -1;
    storage_append_unlock(fd_txn);
    if (rc1 == -1 || rc2 == -1)
        return fail(msg, ST_ERR_INTERNAL, "Server error writing the history");

    if (storage_write(fd_acc, from, &a_from, sizeof(a_from)) == -1 ||
        storage_write(fd_acc, to, &a_to, sizeof(a_to)) == -1)
        return fail(msg, ST_ERR_INTERNAL, "Server error writing the accounts");

    *from_balance = a_from.balance;
    return ST_OK;
}

static int do_transfer(int from, int to, long long amount,
                       long long *from_balance, char *msg)
{
    int fd_acc = storage_open(ACCOUNTS_FILE);
    int fd_txn = storage_open(TXNS_FILE);
    int lo = (from < to) ? from : to;      /* always lock the LOWER id first */
    int hi = (from < to) ? to : from;
    int count, st;

    if (fd_acc == -1 || fd_txn == -1) {
        close_two(fd_acc, fd_txn);
        return fail(msg, ST_ERR_INTERNAL, "Server error opening the data files");
    }

    /* accounts are never deleted, so an id below the count always exists */
    count = storage_count(fd_acc, sizeof(struct Account));
    if (count == -1) {
        close_two(fd_acc, fd_txn);
        return fail(msg, ST_ERR_INTERNAL, "Server error reading the accounts file");
    }
    if (to < 0 || to >= count) {
        close_two(fd_acc, fd_txn);
        return fail(msg, ST_ERR_NOT_FOUND, "Receiving account not found");
    }

    if (storage_lock(fd_acc, lo, sizeof(struct Account), F_WRLCK) == -1) {
        close_two(fd_acc, fd_txn);
        return fail(msg, ST_ERR_INTERNAL, "Server error locking the account");
    }
    if (storage_lock(fd_acc, hi, sizeof(struct Account), F_WRLCK) == -1) {
        storage_unlock(fd_acc, lo, sizeof(struct Account));
        close_two(fd_acc, fd_txn);
        return fail(msg, ST_ERR_INTERNAL, "Server error locking the account");
    }

    st = transfer_locked(fd_acc, fd_txn, from, to, amount, from_balance, msg);

    storage_unlock(fd_acc, hi, sizeof(struct Account));
    storage_unlock(fd_acc, lo, sizeof(struct Account));
    close_two(fd_acc, fd_txn);
    return st;
}

int handle_transfer(struct session *s, const struct request *req)
{
    int to = req->u.transfer.to_account;
    long long amount = req->u.transfer.amount;
    long long from_balance = 0;
    char msg[128];
    int st;

    if (s->account_id == NO_ID)
        return reply(s, ST_ERR_NOT_FOUND, "You have no account");
    if (amount <= 0)
        return reply(s, ST_ERR_INVALID_AMOUNT, "Amount must be greater than zero");
    if (to == s->account_id)
        return reply(s, ST_ERR_BAD_REQUEST, "Cannot transfer to your own account");

    st = do_transfer(s->account_id, to, amount, &from_balance, msg);
    if (st != ST_OK)
        return reply(s, st, msg);
    return balance_reply(s, "Transfer successful", from_balance);
}

/* ----- apply for a loan ----- */

/* Check the account is active, then add one loan record under the append lock. */
static int do_apply_loan(int account_id, long long amount, int *loan_id, char *msg)
{
    struct Account a;
    struct Loan l;
    int fd_acc = storage_open(ACCOUNTS_FILE);
    int fd_loan = storage_open(LOANS_FILE);
    int rc, id;

    if (fd_acc == -1 || fd_loan == -1) {
        close_two(fd_acc, fd_loan);
        return fail(msg, ST_ERR_INTERNAL, "Server error opening the data files");
    }

    if (storage_lock(fd_acc, account_id, sizeof(a), F_RDLCK) == -1) {
        close_two(fd_acc, fd_loan);
        return fail(msg, ST_ERR_INTERNAL, "Server error locking the account");
    }
    rc = storage_read(fd_acc, account_id, &a, sizeof(a));
    storage_unlock(fd_acc, account_id, sizeof(a));
    if (rc == -1) {
        close_two(fd_acc, fd_loan);
        return fail(msg, ST_ERR_INTERNAL, "Server error reading the account");
    }
    if (a.status != ACC_ACTIVE) {
        close_two(fd_acc, fd_loan);
        return fail(msg, ST_ERR_ACCOUNT_INACTIVE, "Your account is deactivated");
    }

    if (storage_append_lock(fd_loan) == -1) {
        close_two(fd_acc, fd_loan);
        return fail(msg, ST_ERR_INTERNAL, "Server error locking the loans file");
    }
    id = storage_count(fd_loan, sizeof(l));
    rc = -1;
    if (id != -1) {
        memset(&l, 0, sizeof(l));
        l.loan_id = id;
        l.account_id = account_id;
        l.amount = amount;
        l.status = LOAN_APPLIED;
        l.assigned_to = NO_ID;
        l.applied_time = (long long)time(NULL);
        rc = storage_write(fd_loan, id, &l, sizeof(l));
    }
    storage_append_unlock(fd_loan);
    close_two(fd_acc, fd_loan);

    if (rc == -1)
        return fail(msg, ST_ERR_INTERNAL, "Server error writing the loan");
    *loan_id = id;
    return ST_OK;
}

int handle_apply_loan(struct session *s, const struct request *req)
{
    long long amount = req->u.amount.amount;
    int loan_id = -1;
    char msg[128];
    int st;

    if (s->account_id == NO_ID)
        return reply(s, ST_ERR_NOT_FOUND, "You have no account");
    if (amount <= 0)
        return reply(s, ST_ERR_INVALID_AMOUNT, "Amount must be greater than zero");

    st = do_apply_loan(s->account_id, amount, &loan_id, msg);
    if (st != ST_OK)
        return reply(s, st, msg);
    snprintf(msg, sizeof(msg), "Loan application %d submitted", loan_id);
    return reply(s, ST_OK, msg);
}

/* ----- add feedback ----- */

int handle_add_feedback(struct session *s, const struct request *req)
{
    struct Feedback f;
    char text[256];
    int fd, id, rc = -1;

    /* copy into a buffer that is certain to end with a zero byte */
    memset(text, 0, sizeof(text));
    strncpy(text, req->u.feedback.message, sizeof(text) - 1);
    if (text[0] == '\0')
        return reply(s, ST_ERR_BAD_REQUEST, "Feedback cannot be empty");

    fd = storage_open(FEEDBACK_FILE);
    if (fd == -1)
        return reply(s, ST_ERR_INTERNAL, "Server error opening the feedback file");

    if (storage_append_lock(fd) == -1) {
        storage_close(fd);
        return reply(s, ST_ERR_INTERNAL, "Server error locking the feedback file");
    }
    id = storage_count(fd, sizeof(f));
    if (id != -1) {
        memset(&f, 0, sizeof(f));
        f.feedback_id = id;
        f.user_id = s->user_id;
        strncpy(f.message, text, sizeof(f.message) - 1);
        rc = storage_write(fd, id, &f, sizeof(f));
    }
    storage_append_unlock(fd);
    storage_close(fd);

    if (rc == -1)
        return reply(s, ST_ERR_INTERNAL, "Server error writing the feedback");
    return reply(s, ST_OK, "Thank you for your feedback");
}

/* ----- view transaction history (list reply, newest first) ----- */

int handle_view_history(struct session *s, const struct request *req)
{
    struct response r;
    struct Transaction t;
    char msg[128];
    int fd, count, id, rc, entries = 0;

    (void)req;
    if (s->account_id == NO_ID)
        return reply(s, ST_ERR_NOT_FOUND, "You have no account");

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
        if (t.account_id != s->account_id)
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
