/* dump.c - prints every record of one data file in readable form.
 * Usage (from the bank/ folder):
 *     ./dump users | accounts | txns | loans | feedback
 * Fields are separated by " | ". Passwords are not printed.
 * Times are shown as dd:mm:yyyy -- hh:mm:ss in the computer's local time.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "structs.h"
#include "storage.h"

static const char *role_name(int r)
{
    switch (r) {
    case ROLE_CUSTOMER: return "customer";
    case ROLE_EMPLOYEE: return "employee";
    case ROLE_MANAGER:  return "manager";
    case ROLE_ADMIN:    return "admin";
    default:            return "?";
    }
}

static const char *txn_name(int t)
{
    switch (t) {
    case TXN_DEPOSIT:      return "deposit";
    case TXN_WITHDRAW:     return "withdraw";
    case TXN_TRANSFER_OUT: return "transfer-out";
    case TXN_TRANSFER_IN:  return "transfer-in";
    default:               return "?";
    }
}

static const char *loan_name(int s)
{
    switch (s) {
    case LOAN_APPLIED:  return "applied";
    case LOAN_ASSIGNED: return "assigned";
    case LOAN_APPROVED: return "approved";
    case LOAN_REJECTED: return "rejected";
    default:            return "?";
    }
}

/* print a paise amount as rupees.paise, e.g. 100050 -> 1000.50 */
static void print_money(long long paise)
{
    printf("%lld.%02lld", paise / 100, paise % 100);
}

/* print a timestamp (seconds since 1970) as dd:mm:yyyy -- hh:mm:ss,
 * e.g. 21:10:2026 -- 14:05:09 */
static void print_time(long long stamp)
{
    time_t t = (time_t)stamp;
    struct tm tm;

    if (localtime_r(&t, &tm) == NULL) {
        printf("??:??:???? -- ??:??:??");
        return;
    }
    /* tm_mon counts from 0 (January = 0); tm_year counts years since 1900 */
    printf("%02d:%02d:%04d -- %02d:%02d:%02d",
           tm.tm_mday, tm.tm_mon + 1, tm.tm_year + 1900,
           tm.tm_hour, tm.tm_min, tm.tm_sec);
}

static void dump_users(int fd)
{
    int n = storage_count(fd, sizeof(struct User));
    struct User u;

    printf("users.dat: %d record(s), %zu bytes each\n", n, sizeof(struct User));
    for (int i = 0; i < n; i++) {
        if (storage_read(fd, i, &u, sizeof(u)) == -1) { perror("read"); return; }
        printf("  [%d] | user=%s | role=%s | name=%s | account=%d | logged_in=%d\n",
               u.user_id, u.username, role_name(u.role), u.name,
               u.account_id, u.logged_in);
    }
}

static void dump_accounts(int fd)
{
    int n = storage_count(fd, sizeof(struct Account));
    struct Account a;

    printf("accounts.dat: %d record(s), %zu bytes each\n", n, sizeof(struct Account));
    for (int i = 0; i < n; i++) {
        if (storage_read(fd, i, &a, sizeof(a)) == -1) { perror("read"); return; }
        printf("  [%d] | owner=%d | balance=", a.account_id, a.user_id);
        print_money(a.balance);
        printf(" | status=%s\n", a.status == ACC_ACTIVE ? "active" : "deactivated");
    }
}

static void dump_txns(int fd)
{
    int n = storage_count(fd, sizeof(struct Transaction));
    struct Transaction t;

    printf("txns.dat: %d record(s), %zu bytes each\n", n, sizeof(struct Transaction));
    for (int i = 0; i < n; i++) {
        if (storage_read(fd, i, &t, sizeof(t)) == -1) { perror("read"); return; }
        printf("  [%d] | account=%d | type=%s | amount=", t.txn_id, t.account_id,
               txn_name(t.type));
        print_money(t.amount);
        printf(" | balance_after=");
        print_money(t.balance_after);
        printf(" | other=%d | time=", t.other_account_id);
        print_time(t.timestamp);
        printf("\n");
    }
}

static void dump_loans(int fd)
{
    int n = storage_count(fd, sizeof(struct Loan));
    struct Loan l;

    printf("loans.dat: %d record(s), %zu bytes each\n", n, sizeof(struct Loan));
    for (int i = 0; i < n; i++) {
        if (storage_read(fd, i, &l, sizeof(l)) == -1) { perror("read"); return; }
        printf("  [%d] | account=%d | amount=", l.loan_id, l.account_id);
        print_money(l.amount);
        printf(" | status=%s | assigned_to=%d | time=", loan_name(l.status),
               l.assigned_to);
        print_time(l.applied_time);
        printf("\n");
    }
}

static void dump_feedback(int fd)
{
    int n = storage_count(fd, sizeof(struct Feedback));
    struct Feedback f;

    printf("feedback.dat: %d record(s), %zu bytes each\n", n, sizeof(struct Feedback));
    for (int i = 0; i < n; i++) {
        if (storage_read(fd, i, &f, sizeof(f)) == -1) { perror("read"); return; }
        printf("  [%d] | user=%d | message=%s\n", f.feedback_id, f.user_id, f.message);
    }
}

int main(int argc, char *argv[])
{
    const char *path;
    void (*fn)(int);
    int fd;

    if (argc != 2) {
        fprintf(stderr, "usage: %s users|accounts|txns|loans|feedback\n", argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "users") == 0)         { path = USERS_FILE;    fn = dump_users; }
    else if (strcmp(argv[1], "accounts") == 0) { path = ACCOUNTS_FILE; fn = dump_accounts; }
    else if (strcmp(argv[1], "txns") == 0)     { path = TXNS_FILE;     fn = dump_txns; }
    else if (strcmp(argv[1], "loans") == 0)    { path = LOANS_FILE;    fn = dump_loans; }
    else if (strcmp(argv[1], "feedback") == 0) { path = FEEDBACK_FILE; fn = dump_feedback; }
    else {
        fprintf(stderr, "unknown file '%s'\n", argv[1]);
        return 1;
    }

    fd = storage_open(path);
    if (fd == -1) {
        perror(path);
        return 1;
    }
    fn(fd);
    storage_close(fd);
    return 0;
}
