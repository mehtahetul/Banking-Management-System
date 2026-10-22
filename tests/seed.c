/* seed.c - creates sample records so dump and login have something to work with.
 * Run from the bank/ folder:  ./seed
 * It wipes and recreates the data files each time.
 *
 * Users:   0 admin, 1 man, 2 emp, 3 hetul (account 0), 4 cust (account 1)
 * Money is in paise: 100000 paise = 1000.00 rupees.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "structs.h"
#include "storage.h"

static void add_user(int fd, int id, const char *username, const char *password,
                     int role, const char *name, int account_id)
{
    struct User u;

    memset(&u, 0, sizeof(u));              /* zero everything first */
    u.user_id = id;
    strncpy(u.username, username, sizeof(u.username) - 1);
    strncpy(u.password, password, sizeof(u.password) - 1);
    u.role = role;
    strncpy(u.name, name, sizeof(u.name) - 1);
    strncpy(u.phone, "9999900000", sizeof(u.phone) - 1);
    strncpy(u.email, "test@example.com", sizeof(u.email) - 1);
    u.account_id = account_id;
    u.logged_in = 0;

    if (storage_write(fd, id, &u, sizeof(u)) == -1)
        perror("write user");
}

static void add_account(int fd, int id, int user_id, long long balance)
{
    struct Account a;

    memset(&a, 0, sizeof(a));
    a.account_id = id;
    a.user_id = user_id;
    a.balance = balance;
    a.status = ACC_ACTIVE;

    if (storage_write(fd, id, &a, sizeof(a)) == -1)
        perror("write account");
}

static void add_deposit(int fd, int id, int account_id, long long amount)
{
    struct Transaction t;

    memset(&t, 0, sizeof(t));
    t.txn_id = id;
    t.account_id = account_id;
    t.type = TXN_DEPOSIT;
    t.amount = amount;
    t.balance_after = amount;              /* first entry, so balance equals amount */
    t.other_account_id = NO_ID;
    t.timestamp = (long long)time(NULL);

    if (storage_write(fd, id, &t, sizeof(t)) == -1)
        perror("write txn");
}

int main(void)
{
    int fd;

    /* start clean */
    unlink(USERS_FILE);
    unlink(ACCOUNTS_FILE);
    unlink(TXNS_FILE);
    unlink(LOANS_FILE);
    unlink(FEEDBACK_FILE);

    /* users */
    fd = storage_open(USERS_FILE);
    if (fd == -1) { perror("open users"); return 1; }
    add_user(fd, 0, "admin", "admin123", ROLE_ADMIN,    "Admin One",    NO_ID);
    add_user(fd, 1, "man",   "man123",   ROLE_MANAGER,  "Manager One",  NO_ID);
    add_user(fd, 2, "emp",   "emp123",   ROLE_EMPLOYEE, "Employee One", NO_ID);
    add_user(fd, 3, "hetul", "hetul123", ROLE_CUSTOMER, "Hetul",        0);
    add_user(fd, 4, "cust",  "cust123",  ROLE_CUSTOMER, "Customer One", 1);
    storage_close(fd);

    /* accounts: 0 -> hetul (1000.00), 1 -> cust (500.00) */
    fd = storage_open(ACCOUNTS_FILE);
    if (fd == -1) { perror("open accounts"); return 1; }
    add_account(fd, 0, 3, 100000);
    add_account(fd, 1, 4, 50000);
    storage_close(fd);

    /* one opening deposit per account, so each passbook matches its balance */
    fd = storage_open(TXNS_FILE);
    if (fd == -1) { perror("open txns"); return 1; }
    add_deposit(fd, 0, 0, 100000);
    add_deposit(fd, 1, 1, 50000);
    storage_close(fd);

    /* one loan application from hetul's account, not yet assigned */
    fd = storage_open(LOANS_FILE);
    if (fd == -1) { perror("open loans"); return 1; }
    struct Loan l;
    memset(&l, 0, sizeof(l));
    l.loan_id = 0;
    l.account_id = 0;
    l.amount = 500000;
    l.status = LOAN_APPLIED;
    l.assigned_to = NO_ID;
    l.applied_time = (long long)time(NULL);
    storage_write(fd, 0, &l, sizeof(l));
    storage_close(fd);

    /* one feedback entry from hetul */
    fd = storage_open(FEEDBACK_FILE);
    if (fd == -1) { perror("open feedback"); return 1; }
    struct Feedback f;
    memset(&f, 0, sizeof(f));
    f.feedback_id = 0;
    f.user_id = 3;
    strncpy(f.message, "Great service!", sizeof(f.message) - 1);
    storage_write(fd, 0, &f, sizeof(f));
    storage_close(fd);

    printf("Seeded sample data in %s/\n", DATA_DIR);
    return 0;
}
