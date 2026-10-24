/* ui.c - reading the keyboard, printing replies, and talking to the server. */
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "client.h"
#include "netio.h"

static int server_fd = -1;

void ui_set_server(int fd)
{
    server_fd = fd;
}

const char *role_name(int role)
{
    switch (role) {
    case ROLE_CUSTOMER: return "customer";
    case ROLE_EMPLOYEE: return "employee";
    case ROLE_MANAGER:  return "manager";
    case ROLE_ADMIN:    return "administrator";
    default:            return "?";
    }
}

/* Copy text into a fixed-size field, always ending with a zero byte. */
void copy_field(char *dst, size_t dst_size, const char *src)
{
    memset(dst, 0, dst_size);
    strncpy(dst, src, dst_size - 1);
}

/* paise -> "1000.00" */
void format_money(long long paise, char *buf, size_t size)
{
    long long a = (paise < 0) ? -paise : paise;

    snprintf(buf, size, "%s%lld.%02lld", (paise < 0) ? "-" : "", a / 100, a % 100);
}

/* ---------- keyboard ---------- */

/* Print a prompt and read one line (without the newline). A line longer than
 * the buffer is cut, and the rest of it is thrown away. 0, or -1 at end of input. */
int read_line(const char *prompt, char *buf, size_t size)
{
    size_t len;

    printf("%s", prompt);
    fflush(stdout);
    if (fgets(buf, (int)size, stdin) == NULL)
        return -1;
    len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n') {
        buf[len - 1] = '\0';
    } else {
        int c;

        while ((c = getchar()) != '\n' && c != EOF)
            ;
    }
    return 0;
}

/* Ask until the user types a whole number. -1 only at end of input. */
int read_int(const char *prompt, int *out)
{
    char line[32];

    for (;;) {
        char *end;
        long v;

        if (read_line(prompt, line, sizeof(line)) == -1)
            return -1;
        errno = 0;
        v = strtol(line, &end, 10);
        if (end == line || *end != '\0' || errno == ERANGE || v < INT_MIN || v > INT_MAX) {
            printf("  ! Please enter a whole number.\n");
            continue;
        }
        *out = (int)v;
        return 0;
    }
}

/* Turn text like "50", "50.5" or "50.25" (rupees) into paise, using only
 * whole numbers (never float/double for money).
 * Returns 0 and fills *paise, or -1 if the text is not a valid amount.
 * Whether the amount is allowed (above zero, enough funds) is the server's job. */
static int parse_rupees(const char *text, long long *paise)
{
    int decimals = -1, digits = 0;     /* decimals -1 = no '.' seen yet */
    long long value = 0;

    for (; *text; text++) {
        int d;

        if (*text == '.') {
            if (decimals != -1)
                return -1;             /* second '.' */
            decimals = 0;
            continue;
        }
        if (*text < '0' || *text > '9')
            return -1;                 /* not a digit */
        if (decimals == 2)
            return -1;                 /* more than 2 decimals */
        d = *text - '0';
        if (value > (LLONG_MAX - d) / 10)
            return -1;                 /* too big for a long long */
        value = value * 10 + d;
        digits++;
        if (decimals != -1)
            decimals++;
    }
    if (digits == 0)
        return -1;
    if (decimals == -1)
        decimals = 0;
    while (decimals < 2) {             /* rupees -> paise */
        if (value > LLONG_MAX / 10)
            return -1;
        value *= 10;
        decimals++;
    }
    *paise = value;
    return 0;
}

/* Ask until the user types a valid amount in rupees. -1 only at end of input. */
int read_money(const char *prompt, long long *paise)
{
    char line[64];

    for (;;) {
        if (read_line(prompt, line, sizeof(line)) == -1)
            return -1;
        if (parse_rupees(line, paise) == 0)
            return 0;
        printf("  ! Please enter an amount like 50 or 50.25.\n");
    }
}

/* Read a password, showing one * for each character typed.
 * To get each key as it is pressed, the terminal is switched to "one key at a
 * time, no echo" mode and ALWAYS switched back before returning.
 * If input is not a terminal (a script or a pipe), a plain line is read.
 * 0, or -1 at end of input (Ctrl+D) or Ctrl+C. */
int read_password(const char *prompt, char *buf, size_t size)
{
    struct termios old, raw;
    size_t len = 0;
    int c, result = 0;

    if (!isatty(STDIN_FILENO))
        return read_line(prompt, buf, size);

    printf("%s", prompt);
    fflush(stdout);

    if (tcgetattr(STDIN_FILENO, &old) == -1)
        return read_line("", buf, size);
    raw = old;
    raw.c_lflag &= ~(tcflag_t)(ICANON | ECHO | ISIG);   /* key by key, no echo, Ctrl+C arrives as a key */
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
        return read_line("", buf, size);

    for (;;) {
        c = getchar();
        if (c == EOF || c == 3 || c == 4) {              /* end of input, Ctrl+C, Ctrl+D */
            result = -1;
            break;
        }
        if (c == '\n' || c == '\r')
            break;
        if (c == 127 || c == 8) {                        /* backspace */
            if (len > 0) {
                len--;
                printf("\b \b");
                fflush(stdout);
            }
            continue;
        }
        if (c >= 32 && c < 127 && len < size - 1) {      /* a normal printable key */
            buf[len++] = (char)c;
            putchar('*');
            fflush(stdout);
        }
    }
    buf[len] = '\0';

    tcsetattr(STDIN_FILENO, TCSAFLUSH, &old);            /* switch the terminal back */
    printf("\n");
    return result;
}

/* ---------- showing replies ---------- */

static void show_txn(const struct Transaction *t)
{
    static const char *type_name[] = { "deposit", "withdraw", "transfer out", "transfer in" };
    char amount[40], after[40], when[40], other[32];
    time_t tt = (time_t)t->timestamp;
    struct tm *tm = localtime(&tt);

    format_money(t->amount, amount, sizeof(amount));
    format_money(t->balance_after, after, sizeof(after));
    if (tm != NULL)
        strftime(when, sizeof(when), "%d-%m-%Y %H:%M:%S", tm);
    else
        snprintf(when, sizeof(when), "?");
    if (t->other_account_id == NO_ID)
        other[0] = '\0';
    else
        snprintf(other, sizeof(other), "%s account %d",
                 t->type == TXN_TRANSFER_OUT ? "to" : "from", t->other_account_id);

    printf("  #%-4d %-12s Rs %9s  balance Rs %9s  %-16s %s\n",
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
        strftime(when, sizeof(when), "%d-%m-%Y %H:%M:%S", tm);
    else
        snprintf(when, sizeof(when), "?");
    if (l->assigned_to == NO_ID)
        snprintf(assigned, sizeof(assigned), "nobody");
    else
        snprintf(assigned, sizeof(assigned), "user %d", l->assigned_to);

    printf("  Loan #%-3d account %-3d Rs %9s  %-9s  assigned to %-8s applied %s\n",
           l->loan_id, l->account_id, amount,
           (l->status >= 0 && l->status <= 3) ? status_name[l->status] : "?",
           assigned, when);
}

/* Print one response of a reply: an error as "! text", a list row as a row,
 * anything else as its message. */
static void show_response(const struct response *r)
{
    char money[40];

    if (r->status != ST_OK) {
        printf("  ! %s\n", r->message);
        return;
    }
    switch (r->kind) {
    case RESP_BALANCE:
        format_money(r->u.balance.balance, money, sizeof(money));
        printf("  %s: Rs %s\n", r->message, money);
        break;
    case RESP_CREATED:
        if (r->u.created.account_id == NO_ID)
            printf("  %s (user id %d)\n", r->message, r->u.created.user_id);
        else
            printf("  %s (user id %d, account id %d)\n", r->message,
                   r->u.created.user_id, r->u.created.account_id);
        break;
    case RESP_TXN:
        show_txn(&r->u.txn);
        break;
    case RESP_LOAN:
        show_loan(&r->u.loan);
        break;
    case RESP_FEEDBACK:
        printf("  #%d (user %d): %s\n", r->u.feedback.feedback_id,
               r->u.feedback.user_id, r->u.feedback.message);
        break;
    default:
        printf("  %s\n", r->message);
        break;
    }
}

/* Send one request, read and show every response of the reply (a list is
 * several responses, until more == 0). *last gets the final response if it is
 * not NULL. Returns 0, or -1 if the connection is lost. */
int call_server(const struct request *req, struct response *last)
{
    struct response r;

    if (write_full(server_fd, req, sizeof(*req)) == -1) {
        printf("  ! Lost the connection to the server.\n");
        return -1;
    }
    do {
        if (read_full(server_fd, &r, sizeof(r)) != 1) {
            printf("  ! Lost the connection to the server.\n");
            return -1;
        }
        show_response(&r);
    } while (r.more);

    if (last != NULL)
        *last = r;
    return 0;
}
