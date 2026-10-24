/* testclient.c - throwaway test client. NOT the real menu client.
 *
 * Opens ONE connection to the server on this machine, then runs the words
 * you give it, in order, on that connection:
 *
 *   login <user> <pass>   send OP_LOGIN
 *   logout                send OP_LOGOUT
 *   chpass <old> <new>    send OP_CHANGE_PASSWORD
 *   balance               send OP_VIEW_BALANCE
 *   deposit <rupees>      send OP_DEPOSIT   (rupees: 50 or 50.5 or 50.25)
 *   withdraw <rupees>     send OP_WITHDRAW
 *   transfer <to> <rupees>  send OP_TRANSFER to account number <to>
 *   loan <rupees>         send OP_APPLY_LOAN
 *   feedback <text>       send OP_ADD_FEEDBACK (put the text in quotes)
 *   history               send OP_VIEW_HISTORY (newest first)
 *
 * Employee (Day 5):
 *   addcustomer <user> <pass> <name> <phone> <email>   send OP_ADD_CUSTOMER
 *   modify <user_id> <name> <phone> <email>            send OP_MODIFY_CUSTOMER
 *                         use "" for a field you do not want to change
 *   process <loan_id> approve|reject                   send OP_PROCESS_LOAN
 *   assigned              send OP_VIEW_ASSIGNED_LOANS
 *   passbook <account_id> send OP_VIEW_PASSBOOK (newest first)
 *
 * Manager (Day 5):
 *   setstatus <account_id> active|deactivated          send OP_SET_ACCOUNT_STATUS
 *   assign <loan_id> <employee_user_id>                send OP_ASSIGN_LOAN
 *   feedbacks             send OP_REVIEW_FEEDBACK (newest first)
 *
 * Administrator (Day 5):
 *   addemployee <user> <pass> <name> <phone> <email>   send OP_ADD_EMPLOYEE
 *   modifyuser <user_id> <name> <phone> <email>        send OP_MODIFY_USER
 *   setrole <user_id> customer|employee|manager|admin  send OP_SET_ROLE
 *                         (the server only accepts employee or manager)
 *
 * Other:
 *   raw <opcode>          send a request with this opcode and an empty payload
 *   exit                  send OP_EXIT
 *   sleep <seconds>       wait on the client side (connection stays open)
 *
 * Example:  ./testclient login hetul hetul123 balance logout exit
 *
 * Build from the project root:  make testclient
 */
#include <arpa/inet.h>
#include <limits.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "protocol.h"
#include "netio.h"

static const char *role_name(int role)
{
    switch (role) {
    case ROLE_CUSTOMER: return "customer";
    case ROLE_EMPLOYEE: return "employee";
    case ROLE_MANAGER:  return "manager";
    case ROLE_ADMIN:    return "admin";
    default:            return "?";
    }
}

/* Role word -> ROLE_* value, or -1 if the word is not a role. */
static int parse_role(const char *word)
{
    if (strcmp(word, "customer") == 0) return ROLE_CUSTOMER;
    if (strcmp(word, "employee") == 0) return ROLE_EMPLOYEE;
    if (strcmp(word, "manager") == 0)  return ROLE_MANAGER;
    if (strcmp(word, "admin") == 0)    return ROLE_ADMIN;
    return -1;
}

/* Turn text like "50", "50.5" or "50.25" (rupees) into paise, using only
 * whole numbers (never float/double for money). A leading '-' is accepted
 * on purpose, so you can check that the SERVER refuses negative amounts.
 * Returns 0 and fills *paise, or -1 if the text is not a valid amount. */
static int parse_rupees(const char *text, long long *paise)
{
    int negative = 0, decimals = -1, digits = 0;   /* decimals -1 = no '.' seen yet */
    long long value = 0;

    if (*text == '-') {
        negative = 1;
        text++;
    }
    for (; *text; text++) {
        int d;

        if (*text == '.') {
            if (decimals != -1)
                return -1;                         /* second '.' */
            decimals = 0;
            continue;
        }
        if (*text < '0' || *text > '9')
            return -1;                             /* not a digit */
        if (decimals == 2)
            return -1;                             /* more than 2 decimals */
        d = *text - '0';
        if (value > (LLONG_MAX - d) / 10)
            return -1;                             /* too big for a long long */
        value = value * 10 + d;
        digits++;
        if (decimals != -1)
            decimals++;
    }
    if (digits == 0)
        return -1;
    if (decimals == -1)
        decimals = 0;
    while (decimals < 2) {                         /* rupees -> paise */
        if (value > LLONG_MAX / 10)
            return -1;
        value *= 10;
        decimals++;
    }
    *paise = negative ? -value : value;
    return 0;
}

/* Parse an amount word, or print why not. Returns 0 or -1. */
static int get_amount(const char *text, long long *paise)
{
    if (parse_rupees(text, paise) == -1) {
        fprintf(stderr, "bad amount '%s' (use something like 50 or 50.25)\n", text);
        return -1;
    }
    return 0;
}

/* paise -> "1000.00" */
static void format_money(long long paise, char *buf, size_t size)
{
    long long a = (paise < 0) ? -paise : paise;

    snprintf(buf, size, "%s%lld.%02lld", (paise < 0) ? "-" : "", a / 100, a % 100);
}

static void show_txn(const struct Transaction *t)
{
    static const char *type_name[] = { "deposit", "withdraw", "transfer out", "transfer in" };
    char amount[40], after[40], when[40], other[16];
    time_t tt = (time_t)t->timestamp;
    struct tm *tm = localtime(&tt);

    format_money(t->amount, amount, sizeof(amount));
    format_money(t->balance_after, after, sizeof(after));
    if (tm != NULL)
        strftime(when, sizeof(when), "%d:%m:%Y -- %H:%M:%S", tm);
    else
        snprintf(when, sizeof(when), "?");
    if (t->other_account_id == NO_ID)
        snprintf(other, sizeof(other), "-");
    else
        snprintf(other, sizeof(other), "%d", t->other_account_id);

    printf("     #%d  %-12s %12s  balance after %12s  other account %s  %s\n",
           t->txn_id,
           (t->type >= 0 && t->type <= 3) ? type_name[t->type] : "?",
           amount, after, other, when);
}

static void show_loan(const struct Loan *l)
{
    static const char *status_name[] = { "applied", "assigned", "approved", "rejected" };
    char amount[40], when[40], assigned[16];
    time_t tt = (time_t)l->applied_time;
    struct tm *tm = localtime(&tt);

    format_money(l->amount, amount, sizeof(amount));
    if (tm != NULL)
        strftime(when, sizeof(when), "%d:%m:%Y -- %H:%M:%S", tm);
    else
        snprintf(when, sizeof(when), "?");
    if (l->assigned_to == NO_ID)
        snprintf(assigned, sizeof(assigned), "-");
    else
        snprintf(assigned, sizeof(assigned), "%d", l->assigned_to);

    printf("     loan #%d  account %d  amount %12s  %-9s  assigned to %s  applied %s\n",
           l->loan_id, l->account_id, amount,
           (l->status >= 0 && l->status <= 3) ? status_name[l->status] : "?",
           assigned, when);
}

static void show(const struct response *r)
{
    printf("  <- [status %d] %s\n", r->status, r->message);
    if (r->kind == RESP_LOGIN)
        printf("     user_id=%d role=%s account=%d\n",
               r->u.login.user_id, role_name(r->u.login.role), r->u.login.account_id);
    else if (r->kind == RESP_TEXT)
        printf("     text: %s\n", r->u.text);
    else if (r->kind == RESP_BALANCE) {
        char money[40];

        format_money(r->u.balance.balance, money, sizeof(money));
        printf("     balance: %s rupees (%lld paise)\n", money, r->u.balance.balance);
    } else if (r->kind == RESP_CREATED)
        printf("     created: user_id=%d account_id=%d\n",
               r->u.created.user_id, r->u.created.account_id);
    else if (r->kind == RESP_TXN)
        show_txn(&r->u.txn);
    else if (r->kind == RESP_LOAN)
        show_loan(&r->u.loan);
    else if (r->kind == RESP_FEEDBACK)
        printf("     feedback #%d from user %d: %s\n",
               r->u.feedback.feedback_id, r->u.feedback.user_id, r->u.feedback.message);
}

/* Send one request and print every response of the reply.
 * Returns 0 if the connection is still usable, -1 if not. */
static int send_and_show(int fd, const struct request *req)
{
    struct response r;

    if (write_full(fd, req, sizeof(*req)) == -1) {
        printf("  !! could not send: the server has closed the connection\n");
        return -1;
    }
    do {
        int rc = read_full(fd, &r, sizeof(r));

        if (rc == 0) {
            printf("  !! server closed the connection\n");
            return -1;
        }
        if (rc == -1) {
            printf("  !! connection error\n");
            return -1;
        }
        show(&r);
    } while (r.more);
    return 0;
}

static void usage(void)
{
    fprintf(stderr,
        "usage: testclient <word> ...\n"
        "  login <user> <pass> | logout | chpass <old> <new> | raw <opcode> | exit | sleep <seconds>\n"
        "  balance | deposit <rupees> | withdraw <rupees> | transfer <to_account> <rupees>\n"
        "  loan <rupees> | feedback <text> | history\n"
        "  addcustomer <user> <pass> <name> <phone> <email> | modify <user_id> <name> <phone> <email>\n"
        "  process <loan_id> approve|reject | assigned | passbook <account_id>\n"
        "  setstatus <account_id> active|deactivated | assign <loan_id> <employee_id> | feedbacks\n"
        "  addemployee <user> <pass> <name> <phone> <email> | modifyuser <user_id> <name> <phone> <email>\n"
        "  setrole <user_id> customer|employee|manager|admin\n");
}

int main(int argc, char *argv[])
{
    struct sockaddr_in addr;
    int fd, i = 1;

    if (argc < 2) {
        usage();
        return 1;
    }

    /* do not die if the server closes the connection while we write */
    signal(SIGPIPE, SIG_IGN);

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) {
        perror("socket");
        return 1;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(SERVER_PORT);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        perror("connect");
        return 1;
    }
    printf("connected to 127.0.0.1:%d\n", SERVER_PORT);

    while (i < argc) {
        struct request req;
        const char *word = argv[i];

        memset(&req, 0, sizeof(req));

        if (strcmp(word, "login") == 0 && i + 2 < argc) {
            req.opcode = OP_LOGIN;
            strncpy(req.u.login.username, argv[i + 1], sizeof(req.u.login.username) - 1);
            strncpy(req.u.login.password, argv[i + 2], sizeof(req.u.login.password) - 1);
            printf("-> login %s\n", argv[i + 1]);
            i += 3;
        } else if (strcmp(word, "logout") == 0) {
            req.opcode = OP_LOGOUT;
            printf("-> logout\n");
            i += 1;
        } else if (strcmp(word, "chpass") == 0 && i + 2 < argc) {
            req.opcode = OP_CHANGE_PASSWORD;
            strncpy(req.u.change_password.old_password, argv[i + 1],
                    sizeof(req.u.change_password.old_password) - 1);
            strncpy(req.u.change_password.new_password, argv[i + 2],
                    sizeof(req.u.change_password.new_password) - 1);
            printf("-> chpass (old and new password sent)\n");
            i += 3;
        } else if (strcmp(word, "balance") == 0) {
            req.opcode = OP_VIEW_BALANCE;
            printf("-> balance\n");
            i += 1;
        } else if (strcmp(word, "deposit") == 0 && i + 1 < argc) {
            req.opcode = OP_DEPOSIT;
            if (get_amount(argv[i + 1], &req.u.amount.amount) == -1) {
                close(fd);
                return 1;
            }
            printf("-> deposit %s\n", argv[i + 1]);
            i += 2;
        } else if (strcmp(word, "withdraw") == 0 && i + 1 < argc) {
            req.opcode = OP_WITHDRAW;
            if (get_amount(argv[i + 1], &req.u.amount.amount) == -1) {
                close(fd);
                return 1;
            }
            printf("-> withdraw %s\n", argv[i + 1]);
            i += 2;
        } else if (strcmp(word, "transfer") == 0 && i + 2 < argc) {
            req.opcode = OP_TRANSFER;
            req.u.transfer.to_account = atoi(argv[i + 1]);
            if (get_amount(argv[i + 2], &req.u.transfer.amount) == -1) {
                close(fd);
                return 1;
            }
            printf("-> transfer %s to account %s\n", argv[i + 2], argv[i + 1]);
            i += 3;
        } else if (strcmp(word, "loan") == 0 && i + 1 < argc) {
            req.opcode = OP_APPLY_LOAN;
            if (get_amount(argv[i + 1], &req.u.amount.amount) == -1) {
                close(fd);
                return 1;
            }
            printf("-> loan %s\n", argv[i + 1]);
            i += 2;
        } else if (strcmp(word, "feedback") == 0 && i + 1 < argc) {
            req.opcode = OP_ADD_FEEDBACK;
            strncpy(req.u.feedback.message, argv[i + 1], sizeof(req.u.feedback.message) - 1);
            printf("-> feedback\n");
            i += 2;
        } else if (strcmp(word, "history") == 0) {
            req.opcode = OP_VIEW_HISTORY;
            printf("-> history\n");
            i += 1;
        } else if (strcmp(word, "addcustomer") == 0 && i + 5 < argc) {
            req.opcode = OP_ADD_CUSTOMER;
            strncpy(req.u.new_user.username, argv[i + 1], sizeof(req.u.new_user.username) - 1);
            strncpy(req.u.new_user.password, argv[i + 2], sizeof(req.u.new_user.password) - 1);
            strncpy(req.u.new_user.name, argv[i + 3], sizeof(req.u.new_user.name) - 1);
            strncpy(req.u.new_user.phone, argv[i + 4], sizeof(req.u.new_user.phone) - 1);
            strncpy(req.u.new_user.email, argv[i + 5], sizeof(req.u.new_user.email) - 1);
            printf("-> addcustomer %s\n", argv[i + 1]);
            i += 6;
        } else if (strcmp(word, "modify") == 0 && i + 4 < argc) {
            req.opcode = OP_MODIFY_CUSTOMER;
            req.u.modify_user.user_id = atoi(argv[i + 1]);
            strncpy(req.u.modify_user.name, argv[i + 2], sizeof(req.u.modify_user.name) - 1);
            strncpy(req.u.modify_user.phone, argv[i + 3], sizeof(req.u.modify_user.phone) - 1);
            strncpy(req.u.modify_user.email, argv[i + 4], sizeof(req.u.modify_user.email) - 1);
            printf("-> modify user %s\n", argv[i + 1]);
            i += 5;
        } else if (strcmp(word, "process") == 0 && i + 2 < argc) {
            req.opcode = OP_PROCESS_LOAN;
            req.u.process_loan.loan_id = atoi(argv[i + 1]);
            if (strcmp(argv[i + 2], "approve") == 0)
                req.u.process_loan.approve = 1;
            else if (strcmp(argv[i + 2], "reject") == 0)
                req.u.process_loan.approve = 0;
            else {
                fprintf(stderr, "process: say 'approve' or 'reject', not '%s'\n", argv[i + 2]);
                close(fd);
                return 1;
            }
            printf("-> process loan %s: %s\n", argv[i + 1], argv[i + 2]);
            i += 3;
        } else if (strcmp(word, "assigned") == 0) {
            req.opcode = OP_VIEW_ASSIGNED_LOANS;
            printf("-> assigned\n");
            i += 1;
        } else if (strcmp(word, "passbook") == 0 && i + 1 < argc) {
            req.opcode = OP_VIEW_PASSBOOK;
            req.u.account.account_id = atoi(argv[i + 1]);
            printf("-> passbook of account %s\n", argv[i + 1]);
            i += 2;
        } else if (strcmp(word, "setstatus") == 0 && i + 2 < argc) {
            req.opcode = OP_SET_ACCOUNT_STATUS;
            req.u.set_status.account_id = atoi(argv[i + 1]);
            if (strcmp(argv[i + 2], "active") == 0)
                req.u.set_status.status = ACC_ACTIVE;
            else if (strcmp(argv[i + 2], "deactivated") == 0)
                req.u.set_status.status = ACC_DEACTIVATED;
            else {
                fprintf(stderr, "setstatus: say 'active' or 'deactivated', not '%s'\n", argv[i + 2]);
                close(fd);
                return 1;
            }
            printf("-> setstatus account %s: %s\n", argv[i + 1], argv[i + 2]);
            i += 3;
        } else if (strcmp(word, "assign") == 0 && i + 2 < argc) {
            req.opcode = OP_ASSIGN_LOAN;
            req.u.assign_loan.loan_id = atoi(argv[i + 1]);
            req.u.assign_loan.employee_id = atoi(argv[i + 2]);
            printf("-> assign loan %s to user %s\n", argv[i + 1], argv[i + 2]);
            i += 3;
        } else if (strcmp(word, "feedbacks") == 0) {
            req.opcode = OP_REVIEW_FEEDBACK;
            printf("-> feedbacks\n");
            i += 1;
        } else if (strcmp(word, "addemployee") == 0 && i + 5 < argc) {
            req.opcode = OP_ADD_EMPLOYEE;
            strncpy(req.u.new_user.username, argv[i + 1], sizeof(req.u.new_user.username) - 1);
            strncpy(req.u.new_user.password, argv[i + 2], sizeof(req.u.new_user.password) - 1);
            strncpy(req.u.new_user.name, argv[i + 3], sizeof(req.u.new_user.name) - 1);
            strncpy(req.u.new_user.phone, argv[i + 4], sizeof(req.u.new_user.phone) - 1);
            strncpy(req.u.new_user.email, argv[i + 5], sizeof(req.u.new_user.email) - 1);
            printf("-> addemployee %s\n", argv[i + 1]);
            i += 6;
        } else if (strcmp(word, "modifyuser") == 0 && i + 4 < argc) {
            req.opcode = OP_MODIFY_USER;
            req.u.modify_user.user_id = atoi(argv[i + 1]);
            strncpy(req.u.modify_user.name, argv[i + 2], sizeof(req.u.modify_user.name) - 1);
            strncpy(req.u.modify_user.phone, argv[i + 3], sizeof(req.u.modify_user.phone) - 1);
            strncpy(req.u.modify_user.email, argv[i + 4], sizeof(req.u.modify_user.email) - 1);
            printf("-> modifyuser %s\n", argv[i + 1]);
            i += 5;
        } else if (strcmp(word, "setrole") == 0 && i + 2 < argc) {
            int role = parse_role(argv[i + 2]);

            if (role == -1) {
                fprintf(stderr, "setrole: say customer, employee, manager or admin, not '%s'\n",
                        argv[i + 2]);
                close(fd);
                return 1;
            }
            req.opcode = OP_SET_ROLE;
            req.u.set_role.user_id = atoi(argv[i + 1]);
            req.u.set_role.role = role;
            printf("-> setrole user %s: %s\n", argv[i + 1], argv[i + 2]);
            i += 3;
        } else if (strcmp(word, "exit") == 0) {
            req.opcode = OP_EXIT;
            printf("-> exit\n");
            i += 1;
        } else if (strcmp(word, "raw") == 0 && i + 1 < argc) {
            req.opcode = atoi(argv[i + 1]);
            printf("-> raw opcode %d\n", req.opcode);
            i += 2;
        } else if (strcmp(word, "sleep") == 0 && i + 1 < argc) {
            printf("-- sleeping %s seconds (connection stays open)\n", argv[i + 1]);
            sleep((unsigned)atoi(argv[i + 1]));
            i += 2;
            continue;
        } else {
            usage();
            close(fd);
            return 1;
        }

        if (send_and_show(fd, &req) == -1)
            break;
    }

    close(fd);
    return 0;
}
