/* handlers_admin.c - what the administrator can do (Day 5).
 *
 * Lock use (no handler here ever holds two locks at the same time):
 *   add employee:  append lock of users.dat, with a read lock on ONE user
 *                  record at a time inside it (each released at once)
 *   modify user:   write lock on one user record
 *   set role:      loans are scanned first (one read lock at a time), THEN
 *                  one write lock on the user record
 * Nothing here waits for the session semaphore. */
#include <stdio.h>
#include <string.h>

#include "server.h"
#include "storage.h"
#include "password.h"

/* ----- add a new employee (a user record, no account) ----- */

/* Work done while the append lock of users.dat is held.
 * This repeats the username scan from handlers_employee.c: both handlers
 * are small, and sharing one function would mean editing that file too. */
static int add_employee_locked(int fd, const char *username, const char *hash,
                               const char *name, const char *phone, const char *email,
                               int *user_id, char *msg)
{
    struct User u;
    int count, id, rc, uid;

    count = storage_count(fd, sizeof(u));
    if (count == -1)
        return fail(msg, ST_ERR_INTERNAL, "Server error reading the users file");

    /* the username must be unique. Nobody else can add a user while we hold
     * the append lock, so no duplicate can sneak in after this scan. */
    for (id = 0; id < count; id++) {
        if (storage_lock(fd, id, sizeof(u), F_RDLCK) == -1)
            return fail(msg, ST_ERR_INTERNAL, "Server error locking a user");
        rc = storage_read(fd, id, &u, sizeof(u));
        storage_unlock(fd, id, sizeof(u));
        if (rc == -1)
            return fail(msg, ST_ERR_INTERNAL, "Server error reading a user");
        if (strncmp(u.username, username, sizeof(u.username)) == 0)
            return fail(msg, ST_ERR_DUPLICATE_USER, "That username is already taken");
    }

    uid = count;
    memset(&u, 0, sizeof(u));
    u.user_id = uid;
    strncpy(u.username, username, sizeof(u.username) - 1);
    strncpy(u.password, hash, sizeof(u.password) - 1);
    u.role = ROLE_EMPLOYEE;
    strncpy(u.name, name, sizeof(u.name) - 1);
    strncpy(u.phone, phone, sizeof(u.phone) - 1);
    strncpy(u.email, email, sizeof(u.email) - 1);
    u.account_id = NO_ID;               /* staff have no account */
    u.logged_in = 0;
    u.failed_attempts = 0;
    u.last_failed_time = 0;
    if (storage_write(fd, uid, &u, sizeof(u)) == -1)
        return fail(msg, ST_ERR_INTERNAL, "Server error writing the user");

    *user_id = uid;
    return ST_OK;
}

static int do_add_employee(const char *username, const char *hash, const char *name,
                           const char *phone, const char *email, int *user_id, char *msg)
{
    int fd = storage_open(USERS_FILE);
    int st;

    if (fd == -1)
        return fail(msg, ST_ERR_INTERNAL, "Server error opening the users file");
    if (storage_append_lock(fd) == -1) {
        storage_close(fd);
        return fail(msg, ST_ERR_INTERNAL, "Server error locking the users file");
    }

    st = add_employee_locked(fd, username, hash, name, phone, email, user_id, msg);

    storage_append_unlock(fd);
    storage_close(fd);
    return st;
}

int handle_add_employee(struct session *s, const struct request *req)
{
    char username[32];
    char password[128];
    char name[50];
    char phone[16];
    char email[50];
    char hash[HASH_SIZE];
    char msg[128];
    struct response r;
    int user_id = NO_ID, st;

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

    st = do_add_employee(username, hash, name, phone, email, &user_id, msg);
    if (st != ST_OK)
        return reply(s, st, msg);

    printf("[fd %d] admin %d added employee '%s' (user %d)\n",
           s->fd, s->user_id, username, user_id);

    memset(&r, 0, sizeof(r));
    r.status = ST_OK;
    r.kind = RESP_CREATED;
    r.more = 0;
    strncpy(r.message, "Employee added", sizeof(r.message) - 1);
    r.u.created.user_id = user_id;
    r.u.created.account_id = NO_ID;
    return send_response(s, &r);
}

/* ----- modify a customer's or employee's name, phone and email ----- */

/* Work done while the user record is write-locked. An empty field means
 * "keep the current value". Managers and admins cannot be changed here. */
static int modify_user_locked(int fd, int user_id, const char *name,
                              const char *phone, const char *email, char *msg)
{
    struct User u;

    if (storage_read(fd, user_id, &u, sizeof(u)) == -1)
        return fail(msg, ST_ERR_INTERNAL, "Server error reading the user");
    if (u.role != ROLE_CUSTOMER && u.role != ROLE_EMPLOYEE)
        return fail(msg, ST_ERR_PERMISSION,
                    "Only customers and employees can be modified here");

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

static int do_modify_user(int user_id, const char *name, const char *phone,
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

    st = modify_user_locked(fd, user_id, name, phone, email, msg);

    storage_unlock(fd, user_id, sizeof(struct User));
    storage_close(fd);
    return st;
}

int handle_modify_user(struct session *s, const struct request *req)
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

    st = do_modify_user(user_id, name, phone, email, msg);
    if (st != ST_OK)
        return reply(s, st, msg);

    printf("[fd %d] admin %d modified user %d\n", s->fd, s->user_id, user_id);
    return reply(s, ST_OK, "User details updated");
}

/* ----- change a user's role (employee <-> manager only) ----- */

/* How many loans are assigned to this user and still waiting for a decision?
 * Each loan is read under a read lock released at once. -1 on error. */
static int count_open_loans(int user_id)
{
    struct Loan l;
    int fd = storage_open(LOANS_FILE);
    int count, id, rc, open = 0;

    if (fd == -1)
        return -1;
    count = storage_count(fd, sizeof(l));
    if (count == -1) {
        storage_close(fd);
        return -1;
    }
    for (id = 0; id < count; id++) {
        if (storage_lock(fd, id, sizeof(l), F_RDLCK) == -1) {
            storage_close(fd);
            return -1;
        }
        rc = storage_read(fd, id, &l, sizeof(l));
        storage_unlock(fd, id, sizeof(l));
        if (rc == -1) {
            storage_close(fd);
            return -1;
        }
        if (l.assigned_to == user_id && l.status == LOAN_ASSIGNED)
            open++;
    }
    storage_close(fd);
    return open;
}

/* Work done while the user record is write-locked. */
static int set_role_locked(int fd, int user_id, int new_role, char *msg)
{
    struct User u;

    if (storage_read(fd, user_id, &u, sizeof(u)) == -1)
        return fail(msg, ST_ERR_INTERNAL, "Server error reading the user");
    if (u.role != ROLE_EMPLOYEE && u.role != ROLE_MANAGER)
        return fail(msg, ST_ERR_PERMISSION,
                    "Only the role of an employee or a manager can be changed");
    if (u.role == new_role)
        return fail(msg, ST_ERR_BAD_REQUEST,
                    new_role == ROLE_MANAGER ? "That user is already a manager"
                                             : "That user is already an employee");

    u.role = new_role;
    if (storage_write(fd, user_id, &u, sizeof(u)) == -1)
        return fail(msg, ST_ERR_INTERNAL, "Server error writing the user");
    return ST_OK;
}

static int do_set_role(int user_id, int new_role, char *msg)
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

    st = set_role_locked(fd, user_id, new_role, msg);

    storage_unlock(fd, user_id, sizeof(struct User));
    storage_close(fd);
    return st;
}

int handle_set_role(struct session *s, const struct request *req)
{
    int user_id = req->u.set_role.user_id;
    int new_role = req->u.set_role.role;
    char msg[128];
    int open, st;

    if (new_role != ROLE_EMPLOYEE && new_role != ROLE_MANAGER)
        return reply(s, ST_ERR_BAD_REQUEST, "The new role must be employee or manager");
    if (user_id == s->user_id)
        return reply(s, ST_ERR_BAD_REQUEST, "You cannot change your own role");

    /* A manager cannot process loans, so an employee who still has loans to
     * decide must not become a manager. The loans are counted BEFORE the user
     * record is locked, so two locks are never held together. A loan assigned
     * in the tiny gap after this check could still get stuck. */
    if (new_role == ROLE_MANAGER) {
        open = count_open_loans(user_id);
        if (open == -1)
            return reply(s, ST_ERR_INTERNAL, "Server error while checking the loans");
        if (open > 0) {
            snprintf(msg, sizeof(msg),
                     "This employee still has %d loan(s) to process", open);
            return reply(s, ST_ERR_BAD_REQUEST, msg);
        }
    }

    st = do_set_role(user_id, new_role, msg);
    if (st != ST_OK)
        return reply(s, st, msg);

    printf("[fd %d] admin %d changed user %d to %s\n", s->fd, s->user_id, user_id,
           new_role == ROLE_MANAGER ? "manager" : "employee");
    snprintf(msg, sizeof(msg), "User %d is now a %s", user_id,
             new_role == ROLE_MANAGER ? "manager" : "employee");
    return reply(s, ST_OK, msg);
}
