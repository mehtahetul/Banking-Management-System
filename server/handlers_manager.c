/* handlers_manager.c - what the manager can do (Day 5).
 *
 * Lock use here (nothing is ever held together, so no new lock order):
 *   set account status: write lock on one account record
 *   assign loan:        a short read lock on one user record (released),
 *                       THEN a write lock on one loan record
 *   review feedback:    one read lock at a time, released at once
 * Nothing here waits for the session semaphore. */
#include <stdio.h>
#include <string.h>

#include "server.h"
#include "storage.h"

/* ----- activate / deactivate an account ----- */

/* Work done while the account record is write-locked. */
static int set_status_locked(int fd, int account_id, int status, char *msg)
{
    struct Account a;

    if (storage_read(fd, account_id, &a, sizeof(a)) == -1)
        return fail(msg, ST_ERR_INTERNAL, "Server error reading the account");
    if (a.status == status)
        return fail(msg, ST_ERR_BAD_REQUEST,
                    status == ACC_ACTIVE ? "The account is already active"
                                         : "The account is already deactivated");

    a.status = status;
    if (storage_write(fd, account_id, &a, sizeof(a)) == -1)
        return fail(msg, ST_ERR_INTERNAL, "Server error writing the account");
    return ST_OK;
}

static int do_set_status(int account_id, int status, char *msg)
{
    int fd = storage_open(ACCOUNTS_FILE);
    int count, st;

    if (fd == -1)
        return fail(msg, ST_ERR_INTERNAL, "Server error opening the accounts file");
    count = storage_count(fd, sizeof(struct Account));
    if (count == -1) {
        storage_close(fd);
        return fail(msg, ST_ERR_INTERNAL, "Server error reading the accounts file");
    }
    if (account_id < 0 || account_id >= count) {
        storage_close(fd);
        return fail(msg, ST_ERR_NOT_FOUND, "No such account");
    }
    if (storage_lock(fd, account_id, sizeof(struct Account), F_WRLCK) == -1) {
        storage_close(fd);
        return fail(msg, ST_ERR_INTERNAL, "Server error locking the account");
    }

    st = set_status_locked(fd, account_id, status, msg);

    storage_unlock(fd, account_id, sizeof(struct Account));
    storage_close(fd);
    return st;
}

int handle_set_account_status(struct session *s, const struct request *req)
{
    int account_id = req->u.set_status.account_id;
    int status = req->u.set_status.status;
    char msg[128];
    int st;

    if (status != ACC_ACTIVE && status != ACC_DEACTIVATED)
        return reply(s, ST_ERR_BAD_REQUEST, "Status must be active or deactivated");

    st = do_set_status(account_id, status, msg);
    if (st != ST_OK)
        return reply(s, st, msg);

    printf("[fd %d] manager %d %s account %d\n", s->fd, s->user_id,
           status == ACC_ACTIVE ? "activated" : "deactivated", account_id);
    snprintf(msg, sizeof(msg), "Account %d %s", account_id,
             status == ACC_ACTIVE ? "activated" : "deactivated");
    return reply(s, ST_OK, msg);
}

/* ----- assign a loan to an employee ----- */

/* Is this user id a real employee? Returns ST_OK, or an error status with the
 * text in msg. The user record is read under a read lock that is released
 * before this function returns. */
static int check_employee(int user_id, char *msg)
{
    struct User u;
    int fd = storage_open(USERS_FILE);
    int count, rc;

    if (fd == -1)
        return fail(msg, ST_ERR_INTERNAL, "Server error opening the users file");
    count = storage_count(fd, sizeof(u));
    if (count == -1) {
        storage_close(fd);
        return fail(msg, ST_ERR_INTERNAL, "Server error reading the users file");
    }
    if (user_id < 0 || user_id >= count) {
        storage_close(fd);
        return fail(msg, ST_ERR_NOT_FOUND, "No such employee");
    }
    if (storage_lock(fd, user_id, sizeof(u), F_RDLCK) == -1) {
        storage_close(fd);
        return fail(msg, ST_ERR_INTERNAL, "Server error locking the user");
    }
    rc = storage_read(fd, user_id, &u, sizeof(u));
    storage_unlock(fd, user_id, sizeof(u));
    storage_close(fd);

    if (rc == -1)
        return fail(msg, ST_ERR_INTERNAL, "Server error reading the user");
    if (u.role != ROLE_EMPLOYEE)
        return fail(msg, ST_ERR_BAD_REQUEST, "That user is not an employee");
    return ST_OK;
}

/* Work done while the loan record is write-locked. Only a loan that is still
 * "applied" (not assigned yet) can be assigned. */
static int assign_loan_locked(int fd, int loan_id, int employee_id, char *msg)
{
    struct Loan l;

    if (storage_read(fd, loan_id, &l, sizeof(l)) == -1)
        return fail(msg, ST_ERR_INTERNAL, "Server error reading the loan");
    if (l.status != LOAN_APPLIED)
        return fail(msg, ST_ERR_BAD_REQUEST,
                    "This loan has already been assigned or processed");

    l.assigned_to = employee_id;
    l.status = LOAN_ASSIGNED;
    if (storage_write(fd, loan_id, &l, sizeof(l)) == -1)
        return fail(msg, ST_ERR_INTERNAL, "Server error writing the loan");
    return ST_OK;
}

static int do_assign_loan(int loan_id, int employee_id, char *msg)
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

    st = assign_loan_locked(fd, loan_id, employee_id, msg);

    storage_unlock(fd, loan_id, sizeof(struct Loan));
    storage_close(fd);
    return st;
}

int handle_assign_loan(struct session *s, const struct request *req)
{
    int loan_id = req->u.assign_loan.loan_id;
    int employee_id = req->u.assign_loan.employee_id;
    char msg[128];
    int st;

    /* check the employee first (short read lock, released), then lock the
     * loan: two locks are never held at the same time */
    st = check_employee(employee_id, msg);
    if (st != ST_OK)
        return reply(s, st, msg);

    st = do_assign_loan(loan_id, employee_id, msg);
    if (st != ST_OK)
        return reply(s, st, msg);

    printf("[fd %d] manager %d assigned loan %d to employee %d\n",
           s->fd, s->user_id, loan_id, employee_id);
    snprintf(msg, sizeof(msg), "Loan %d assigned to employee %d", loan_id, employee_id);
    return reply(s, ST_OK, msg);
}

/* ----- review customer feedback (list reply, newest first) ----- */

int handle_review_feedback(struct session *s, const struct request *req)
{
    struct response r;
    struct Feedback f;
    char msg[128];
    int fd, count, id, rc, entries = 0;

    (void)req;
    fd = storage_open(FEEDBACK_FILE);
    if (fd == -1)
        return reply(s, ST_ERR_INTERNAL, "Server error opening the feedback file");
    count = storage_count(fd, sizeof(f));
    if (count == -1) {
        storage_close(fd);
        return reply(s, ST_ERR_INTERNAL, "Server error reading the feedback file");
    }

    /* last record first = newest first. Each record is read under a read
     * lock released at once; no lock is held while sending. */
    for (id = count - 1; id >= 0; id--) {
        if (storage_lock(fd, id, sizeof(f), F_RDLCK) == -1) {
            storage_close(fd);
            return reply(s, ST_ERR_INTERNAL, "Server error locking the feedback");
        }
        rc = storage_read(fd, id, &f, sizeof(f));
        storage_unlock(fd, id, sizeof(f));
        if (rc == -1) {
            storage_close(fd);
            return reply(s, ST_ERR_INTERNAL, "Server error reading the feedback");
        }

        memset(&r, 0, sizeof(r));
        r.status = ST_OK;
        r.kind = RESP_FEEDBACK;
        r.more = 1;
        strncpy(r.message, "Feedback", sizeof(r.message) - 1);
        r.u.feedback = f;
        if (send_response(s, &r) == SEND_FAILED) {
            storage_close(fd);
            return SEND_FAILED;
        }
        entries++;
    }
    storage_close(fd);

    snprintf(msg, sizeof(msg), "End of feedback (%d entries)", entries);
    return reply(s, ST_OK, msg);
}
