/* structs.h - all record layouts and constants for the Banking Management System.
 *
 * Rule: id = record number in its file. Record n starts at byte n * sizeof(struct).
 * Money is always long long, in paise. Never float/double.
 */
#ifndef STRUCTS_H
#define STRUCTS_H

/* ---------- data file paths ---------- */
#define DATA_DIR       "data"
#define USERS_FILE     DATA_DIR "/users.dat"
#define ACCOUNTS_FILE  DATA_DIR "/accounts.dat"
#define TXNS_FILE      DATA_DIR "/txns.dat"
#define LOANS_FILE     DATA_DIR "/loans.dat"
#define FEEDBACK_FILE  DATA_DIR "/feedback.dat"

/* ---------- roles (User.role) ---------- */
#define ROLE_CUSTOMER  0
#define ROLE_EMPLOYEE  1
#define ROLE_MANAGER   2
#define ROLE_ADMIN     3

/* ---------- account status (Account.status) ---------- */
#define ACC_DEACTIVATED 0
#define ACC_ACTIVE      1

/* ---------- transaction types (Transaction.type) ---------- */
#define TXN_DEPOSIT       0
#define TXN_WITHDRAW      1
#define TXN_TRANSFER_OUT  2
#define TXN_TRANSFER_IN   3

/* ---------- loan status (Loan.status) ---------- */
#define LOAN_APPLIED   0
#define LOAN_ASSIGNED  1
#define LOAN_APPROVED  2
#define LOAN_REJECTED  3

/* ---------- "no value" marker for id fields ---------- */
#define NO_ID  (-1)

/* ---------- records ----------
 * IMPORTANT: always memset(&rec, 0, sizeof(rec)) before filling a record.
 * The compiler may add hidden padding bytes between fields; zeroing first
 * keeps those bytes clean so the file contents are predictable.
 */

struct User {                 /* users.dat */
    int  user_id;
    char username[32];        /* unique */
    char password[128];       /* plain text for now, crypt() hash from Day 4 */
    int  role;                /* ROLE_* */
    char name[50];
    char phone[16];
    char email[50];
    int  account_id;          /* customer's account, NO_ID for staff */
    int  logged_in;           /* 0/1, one-session-per-user flag */
    int  failed_attempts;     /* wrong passwords since the last reset (login lockout) */
    long long last_failed_time; /* time of the last wrong password, 0 if none */
};

struct Account {              /* accounts.dat */
    int       account_id;
    int       user_id;        /* owner */
    long long balance;        /* paise */
    int       status;         /* ACC_* */
};

struct Transaction {          /* txns.dat */
    int       txn_id;
    int       account_id;        /* whose passbook this entry belongs to */
    int       type;              /* TXN_* */
    long long amount;            /* paise */
    long long balance_after;     /* balance after this entry */
    int       other_account_id;  /* other side of a transfer, NO_ID otherwise */
    long long timestamp;         /* plain integer time */
};

struct Loan {                 /* loans.dat */
    int       loan_id;
    int       account_id;     /* applicant's account */
    long long amount;         /* paise */
    int       status;         /* LOAN_* */
    int       assigned_to;    /* employee's user_id, NO_ID until assigned */
    long long applied_time;
};

struct Feedback {             /* feedback.dat */
    int  feedback_id;
    int  user_id;             /* customer who wrote it */
    char message[256];
};

#endif
