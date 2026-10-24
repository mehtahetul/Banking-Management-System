/* handlers_employee.c - what a bank employee can do (Day 5).
 *
 * Lock orders used here (never reverse them):
 *   add customer:  append lock of users.dat, then append lock of accounts.dat.
 *                  While holding the users append lock it takes a READ lock on
 *                  one user record at a time (each released at once).
 *   modify/process: one record write lock, nothing else held.
 * Nothing here waits for the session semaphore. */
#include <stdio.h>
#include <string.h>

#include "server.h"
#include "storage.h"
#include "password.h"

/* ----- add a new customer (a user record plus an account record) ----- */

/* Work done while BOTH append locks are held.
 *
 * The account is written FIRST, then the user. If the second write fails we
 * are left with an account nobody uses (harmless, balance 0). The other order
 * could leave a user pointing at an account that does not exist yet, and the
 * next customer added would be given that same account id, so two customers
 * would share one account. */
static int add_customer_locked(int fd_usr, int fd_acc, const char *username,
                               const char *hash, const char *name,
                               const char *phone, const char *email,
                               int *user_id, int *account_id, char *msg)
{
    struct User u;
    struct Account a;
    int count, id, rc, uid, aid;

    count = storage_count(fd_usr, sizeof(u));
    if (count == -1)
        return fail(msg, ST_ERR_INTERNAL, "Server error reading the users file");

    /* the username must be unique. Nobody else can add a user while we hold
     * the append lock, so no duplicate can sneak in after this scan. */
    for (id = 0; id < count; id++) {
        if (storage_lock(fd_usr, id, sizeof(u), F_RDLCK) == -1)
            return fail(msg, ST_ERR_INTERNAL, "Server error locking a user");
        rc = storage_read(fd_usr, id, &u, sizeof(u));
        storage_unlock(fd_usr, id, sizeof(u));
        if (rc == -1)
            return fail(msg, ST_ERR_INTERNAL, "Server error reading a user");
        if (strncmp(u.username, username, sizeof(u.username)) == 0)
            return fail(msg, ST_ERR_DUPLICATE_USER, "That username is already taken");
    }

    uid = count;
    aid = storage_count(fd_acc, sizeof(a));
    if (aid == -1)
        return fail(msg, ST_ERR_INTERNAL, "Server error reading the accounts file");

    /* the new account: balance 0, active */
    memset(&a, 0, sizeof(a));
    a.account_id = aid;
    a.user_id = uid;
    a.balance = 0;
    a.status = ACC_ACTIVE;
    if (storage_write(fd_acc, aid, &a, sizeof(a)) == -1)
        return fail(msg, ST_ERR_INTERNAL, "Server error writing the account");

    /* the new user */
    memset(&u, 0, sizeof(u));
    u.user_id = uid;
    strncpy(u.username, username, sizeof(u.username) - 1);
    strncpy(u.password, hash, sizeof(u.password) - 1);
    u.role = ROLE_CUSTOMER;
    strncpy(u.name, name, sizeof(u.name) - 1);
    strncpy(u.phone, phone, sizeof(u.phone) - 1);
    strncpy(u.email, email, sizeof(u.email) - 1);
    u.account_id = aid;
    u.logged_in = 0;
    u.failed_attempts = 0;
    u.last_failed_time = 0;
    if (storage_write(fd_usr, uid, &u, sizeof(u)) == -1)
        return fail(msg, ST_ERR_INTERNAL, "Server error writing the user");

    *user_id = uid;
    *account_id = aid;
    return ST_OK;
}

static int do_add_customer(const char *username, const char *hash,
                           const char *name, const char *phone, const char *email,
                           int *user_id, int *account_id, char *msg)
{
    int fd_usr = storage_open(USERS_FILE);
    int fd_acc = storage_open(ACCOUNTS_FILE);
    int st;

    if (fd_usr == -1 || fd_acc == -1) {
        close_two(fd_usr, fd_acc);
        return fail(msg, ST_ERR_INTERNAL, "Server error opening the data files");
    }
    if (storage_append_lock(fd_usr) == -1) {
        close_two(fd_usr, fd_acc);
        return fail(msg, ST_ERR_INTERNAL, "Server error locking the users file");
    }
    if (storage_append_lock(fd_acc) == -1) {
        storage_append_unlock(fd_usr);
        close_two(fd_usr, fd_acc);
        return fail(msg, ST_ERR_INTERNAL, "Server error locking the accounts file");
    }

    st = add_customer_locked(fd_usr, fd_acc, username, hash, name, phone, email,
                             user_id, account_id, msg);

    storage_append_unlock(fd_acc);
    storage_append_unlock(fd_usr);
    close_two(fd_usr, fd_acc);
    return st;
}

int handle_add_customer(struct session *s, const struct request *req)
{
    char username[32];
    char password[128];
    char name[50];
    char phone[16];
    char email[50];
    char hash[HASH_SIZE];
    char msg[128];
    struct response r;
    int user_id = NO_ID, account_id = NO_ID, st;

    /* copy into buffers that are certain to end with a zero byte */
    memset(username, 0, sizeof(username));
    memset(password, 0, sizeof(password));
    memset(name, 0, sizeof(name));
    memset(phone, 0, sizeof(phone));
    memset(email, 0, sizeof(email));
    strncpy(username, req->u.new_user.username, sizeof(username) - 1);
    strncpy(password, req->u.new_user.password, sizeof(password) - 1);
    strncpy(name, req->u.new_user.name, sizeof(name) - 1);
    strncpy(phone, req->u.new_user.phone, sizeof(phone) - 1);
    strncpy(email, req->u.new_user.email, sizeof(email) - 1);

    /* cheap checks first, before any hashing or locking */
    if (username[0] == '\0')
        return reply(s, ST_ERR_BAD_REQUEST, "Username cannot be empty");
    if (strlen(password) < MIN_PASSWORD_LENGTH) {
        snprintf(msg, sizeof(msg), "Password must be at least %d characters",
                 MIN_PASSWORD_LENGTH);
        return reply(s, ST_ERR_BAD_REQUEST, msg);
    }

    /* hash BEFORE any lock: hashing is slow on purpose */
    if (hash_password(password, hash, sizeof(hash)) == -1)
        return reply(s, ST_ERR_INTERNAL, "Server error while hashing the password");

    st = do_add_customer(username, hash, name, phone, email, &user_id, &account_id, msg);
    if (st != ST_OK)
        return reply(s, st, msg);

    printf("[fd %d] employee %d added customer '%s' (user %d, account %d)\n",
           s->fd, s->user_id, username, user_id, account_id);

    memset(&r, 0, sizeof(r));
    r.status = ST_OK;
    r.kind = RESP_CREATED;
    r.more = 0;
    strncpy(r.message, "Customer added", sizeof(r.message) - 1);
    r.u.created.user_id = user_id;
    r.u.created.account_id = account_id;
    return send_response(s, &r);
}

/* ----- modify a customer's name, phone and email ----- */

/* Work done while the user record is write-locked. An empty field means
 * "keep the current value". */
static int modify_customer_locked(int fd, int user_id, const char *name,
                                  const char *phone, const char *email, char *msg)
{
    struct User u;

    if (storage_read(fd, user_id, &u, sizeof(u)) == -1)
        return fail(msg, ST_ERR_INTERNAL, "Server error reading the user");
    if (u.role != ROLE_CUSTOMER)
        return fail(msg, ST_ERR_PERMISSION, "That user is not a customer");

    if (name[0] != '\0') {
        memset(u.name, 0, sizeof(u.name));
        strncpy(u.name, name, sizeof(u.name) - 1);
    }
    if (phone[0] != '\0') {
        memset(u.phone, 0, sizeof(u.phone));
        strncpy(u.phone, phone, sizeof(u.phone) - 1);
    }
    if (email[0] != '\0') {
        memset(u.email, 0, sizeof(u.email));
        strncpy(u.email, email, sizeof(u.email) - 1);
    }

    if (storage_write(fd, user_id, &u, sizeof(u)) == -1)
        return fail(msg, ST_ERR_INTERNAL, "Server error writing the user");
    return ST_OK;
}

static int do_modify_customer(int user_id, const char *name, const char *phone,
                              const char *email, char *msg)
{
    int fd = storage_open(USERS_FILE);
    int count, st;

    if (fd == -1)
        return fail(msg, ST_ERR_INTERNAL, "Server error opening the users file");
    count = storage_count(fd, sizeof(struct User));
    if (count == -1) {
        storage_close(fd);
        return fail(msg, ST_ERR_INTERNAL, "Server error reading the users file");
    }
    if (user_id < 0 || user_id >= count) {
        storage_close(fd);
        return fail(msg, ST_ERR_NOT_FOUND, "No such user");
    }
    if (storage_lock(fd, user_id, sizeof(struct User), F_WRLCK) == -1) {
        storage_close(fd);
        return fail(msg, ST_ERR_INTERNAL, "Server error locking the user");
    }

    st = modify_customer_locked(fd, user_id, name, phone, email, msg);

    storage_unlock(fd, user_id, sizeof(struct User));
    storage_close(fd);
    return st;
}

int handle_modify_customer(struct session *s, const struct request *req)
{
    char name[50];
    char phone[16];
    char email[50];
    char msg[128];
    int user_id = req->u.modify_user.user_id;
    int st;

    memset(name, 0, sizeof(name));
    memset(phone, 0, sizeof(phone));
    memset(email, 0, sizeof(email));
    strncpy(name, req->u.modify_user.name, sizeof(name) - 1);
    strncpy(phone, req->u.modify_user.phone, sizeof(phone) - 1);
    strncpy(email, req->u.modify_user.email, sizeof(email) - 1);

    if (name[0] == '\0' && phone[0] == '\0' && email[0] == '\0')
        return reply(s, ST_ERR_BAD_REQUEST, "Nothing to change");

    st = do_modify_customer(user_id, name, phone, email, msg);
    if (st != ST_OK)
        return reply(s, st, msg);

    printf("[fd %d] employee %d modified customer %d\n", s->fd, s->user_id, user_id);
    return reply(s, ST_OK, "Customer details updated");
}

/* ----- approve or reject a loan ----- */

/* Work done while the loan record is write-locked. Approving only changes
 * the status; it does NOT credit the balance. */
static int process_loan_locked(int fd, int loan_id, int employee_id,
                               int approve, char *msg)
{
    struct Loan l;

    if (storage_read(fd, loan_id, &l, sizeof(l)) == -1)
        return fail(msg, ST_ERR_INTERNAL, "Server error reading the loan");
    if (l.assigned_to != employee_id)
        return fail(msg, ST_ERR_PERMISSION, "This loan is not assigned to you");
    if (l.status != LOAN_ASSIGNED)
        return fail(msg, ST_ERR_BAD_REQUEST, "This loan has already been processed");

    l.status = approve ? LOAN_APPROVED : LOAN_REJECTED;
    if (storage_write(fd, loan_id, &l, sizeof(l)) == -1)
        return fail(msg, ST_ERR_INTERNAL, "Server error writing the loan");
    return ST_OK;
}

static int do_process_loan(int loan_id, int employee_id, int approve, char *msg)
{
    int fd = storage_open(LOANS_FILE);
    int count, st;

    if (fd == -1)
        return fail(msg, ST_ERR_INTERNAL, "Server error opening the loans file");
    count = storage_count(fd, sizeof(struct Loan));
    if (count == -1) {
        storage_close(fd);
        return fail(msg, ST_ERR_INTERNAL, "Server error reading the loans file");
    }
    if (loan_id < 0 || loan_id >= count) {
        storage_close(fd);
        return fail(msg, ST_ERR_NOT_FOUND, "No such loan");
    }
    if (storage_lock(fd, loan_id, sizeof(struct Loan), F_WRLCK) == -1) {
        storage_close(fd);
        return fail(msg, ST_ERR_INTERNAL, "Server error locking the loan");
    }

    st = process_loan_locked(fd, loan_id, employee_id, approve, msg);

    storage_unlock(fd, loan_id, sizeof(struct Loan));
    storage_close(fd);
    return st;
}

int handle_process_loan(struct session *s, const struct request *req)
{
    int loan_id = req->u.process_loan.loan_id;
    int approve = req->u.process_loan.approve;
    char msg[128];
    int st;

    if (approve != 0 && approve != 1)
        return reply(s, ST_ERR_BAD_REQUEST, "Approve must be 1 (approve) or 0 (reject)");

    st = do_process_loan(loan_id, s->user_id, approve, msg);
    if (st != ST_OK)
        return reply(s, st, msg);

    printf("[fd %d] employee %d %s loan %d\n", s->fd, s->user_id,
           approve ? "approved" : "rejected", loan_id);
    snprintf(msg, sizeof(msg), "Loan %d %s", loan_id, approve ? "approved" : "rejected");
    return reply(s, ST_OK, msg);
}

/* ----- view the loans assigned to this employee (list reply) ----- */

int handle_view_assigned_loans(struct session *s, const struct request *req)
{
    struct response r;
    struct Loan l;
    char msg[128];
    int fd, count, id, rc, entries = 0;

    (void)req;
    fd = storage_open(LOANS_FILE);
    if (fd == -1)
        return reply(s, ST_ERR_INTERNAL, "Server error opening the loans file");
    count = storage_count(fd, sizeof(l));
    if (count == -1) {
        storage_close(fd);
        return reply(s, ST_ERR_INTERNAL, "Server error reading the loans file");
    }

    /* oldest first (loan id 0 upwards): the order to work through them.
     * Every loan assigned to this employee is shown, whatever its status. */
    for (id = 0; id < count; id++) {
        if (storage_lock(fd, id, sizeof(l), F_RDLCK) == -1) {
            storage_close(fd);
            return reply(s, ST_ERR_INTERNAL, "Server error locking a loan");
        }
        rc = storage_read(fd, id, &l, sizeof(l));
        storage_unlock(fd, id, sizeof(l));
        if (rc == -1) {
            storage_close(fd);
            return reply(s, ST_ERR_INTERNAL, "Server error reading a loan");
        }
        if (l.assigned_to != s->user_id)
            continue;

        memset(&r, 0, sizeof(r));
        r.status = ST_OK;
        r.kind = RESP_LOAN;
        r.more = 1;
        strncpy(r.message, "Loan", sizeof(r.message) - 1);
        r.u.loan = l;
        if (send_response(s, &r) == SEND_FAILED) {
            storage_close(fd);
            return SEND_FAILED;
        }
        entries++;
    }
    storage_close(fd);

    snprintf(msg, sizeof(msg), "End of assigned loans (%d entries)", entries);
    return reply(s, ST_OK, msg);
}

/* ----- passbook: one customer's history, chosen by account id ----- */

int handle_view_passbook(struct session *s, const struct request *req)
{
    int account_id = req->u.account.account_id;
    int fd, count;

    fd = storage_open(ACCOUNTS_FILE);
    if (fd == -1)
        return reply(s, ST_ERR_INTERNAL, "Server error opening the accounts file");
    count = storage_count(fd, sizeof(struct Account));
    storage_close(fd);
    if (count == -1)
        return reply(s, ST_ERR_INTERNAL, "Server error reading the accounts file");
    if (account_id < 0 || account_id >= count)
        return reply(s, ST_ERR_NOT_FOUND, "No such account");

    return send_account_history(s, account_id);
}
