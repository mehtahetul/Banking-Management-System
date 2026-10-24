/* handlers_customer.c - the customer's banking operations (Day 4).
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
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "server.h"
#include "storage.h"

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
    (void)req;
    if (s->account_id == NO_ID)
        return reply(s, ST_ERR_NOT_FOUND, "You have no account");
    return send_account_history(s, s->account_id);
}
