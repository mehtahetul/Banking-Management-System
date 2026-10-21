/* seed.c - creates a few sample records so dump has something to show.
 * Run from the bank/ folder:  ./seed
 * It wipes and recreates the data files each time.
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

int main(void)
{
    int fd;

    /* start clean */
    unlink(USERS_FILE);
    unlink(ACCOUNTS_FILE);
    unlink(TXNS_FILE);
    unlink(LOANS_FILE);
    unlink(FEEDBACK_FILE);

    /* users: 0 admin, 1 manager, 2 employee, 3 customer (has account 0) */
    fd = storage_open(USERS_FILE);
    if (fd == -1) { perror("open users"); return 1; }
    add_user(fd, 0, "admin",    "admin123",  ROLE_ADMIN,    "Admin One",    NO_ID);
    add_user(fd, 1, "manager",  "manager123", ROLE_MANAGER,  "Manager One",  NO_ID);
    add_user(fd, 2, "employee", "emp123",    ROLE_EMPLOYEE, "Employee One", NO_ID);
    add_user(fd, 3, "hetul",    "cust123",   ROLE_CUSTOMER, "Hetul",        0);
    storage_close(fd);

    /* account 0 belongs to user 3, balance 1000.00 rupees = 100000 paise */
    fd = storage_open(ACCOUNTS_FILE);
    if (fd == -1) { perror("open accounts"); return 1; }
    struct Account a;
    memset(&a, 0, sizeof(a));
    a.account_id = 0;
    a.user_id = 3;
    a.balance = 100000;
    a.status = ACC_ACTIVE;
    storage_write(fd, 0, &a, sizeof(a));
    storage_close(fd);

    /* one deposit transaction */
    fd = storage_open(TXNS_FILE);
    if (fd == -1) { perror("open txns"); return 1; }
    struct Transaction t;
    memset(&t, 0, sizeof(t));
    t.txn_id = 0;
    t.account_id = 0;
    t.type = TXN_DEPOSIT;
    t.amount = 100000;
    t.balance_after = 100000;
    t.other_account_id = NO_ID;
    t.timestamp = (long long)time(NULL);
    storage_write(fd, 0, &t, sizeof(t));
    storage_close(fd);

    /* one loan application, not yet assigned */
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

    /* one feedback entry */
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
