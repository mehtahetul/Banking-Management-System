/* protocol.h - the messages sent between client and server.
 *
 * Every message is a fixed-size struct. Both sides send and receive exactly
 * sizeof(struct request) or sizeof(struct response) bytes (see netio.h).
 * Both programs run on the same machine with the same compiler, so the
 * struct layout is identical on both sides.
 *
 * Always memset(&msg, 0, sizeof(msg)) before filling a message.
 *
 * Request:  client -> server, one per action.
 * Response: server -> client. A reply is ONE or MORE response messages.
 *           For a list (history, loans, feedback) the server sends one
 *           response per record with more = 1, then a final empty response
 *           with more = 0 (the "end marker"). Every other reply is a single
 *           response with more = 0.
 *
 * The client never sends its own account id for customer actions. The
 * server remembers who logged in and uses that account.
 */
#ifndef PROTOCOL_H
#define PROTOCOL_H

#include "structs.h"

#define SERVER_PORT  8080

/* ---------- opcodes (request.opcode) ----------
 * Allowed roles are listed here for reference. The server checks them. */

/* all roles (OP_LOGIN and OP_EXIT need no login).
 * Opcode 5 was the test-only OP_ECHO; it is removed and not reused. */
#define OP_LOGIN                1
#define OP_LOGOUT               2
#define OP_EXIT                 3
#define OP_CHANGE_PASSWORD      4

/* customer */
#define OP_VIEW_BALANCE         10
#define OP_DEPOSIT              11
#define OP_WITHDRAW             12
#define OP_TRANSFER             13
#define OP_APPLY_LOAN           14
#define OP_ADD_FEEDBACK         15
#define OP_VIEW_HISTORY         16   /* list reply */

/* employee */
#define OP_ADD_CUSTOMER         20
#define OP_MODIFY_CUSTOMER      21
#define OP_PROCESS_LOAN         22   /* approve or reject */
#define OP_VIEW_ASSIGNED_LOANS  23   /* list reply */
#define OP_VIEW_PASSBOOK        24   /* list reply, any customer's account */

/* manager */
#define OP_SET_ACCOUNT_STATUS   30   /* activate / deactivate */
#define OP_ASSIGN_LOAN          31
#define OP_REVIEW_FEEDBACK      32   /* list reply */

/* administrator */
#define OP_ADD_EMPLOYEE         40
#define OP_MODIFY_USER          41   /* customer or employee */
#define OP_SET_ROLE             42

/* ---------- status codes (response.status) ---------- */
#define ST_OK                      0
#define ST_ERR_BAD_REQUEST         1   /* unknown opcode or invalid fields */
#define ST_ERR_NOT_LOGGED_IN       2
#define ST_ERR_PERMISSION          3   /* this role may not do that */
#define ST_ERR_BAD_CREDENTIALS     4   /* wrong password (unknown user is ST_ERR_NOT_FOUND) */
#define ST_ERR_ALREADY_LOGGED_IN   5   /* one session per user (Day 5) */
#define ST_ERR_NOT_FOUND           6   /* no such user, account or loan */
#define ST_ERR_INSUFFICIENT_FUNDS  7
#define ST_ERR_ACCOUNT_INACTIVE    8
#define ST_ERR_INVALID_AMOUNT      9
#define ST_ERR_DUPLICATE_USER      10  /* username already taken */
#define ST_ERR_INTERNAL            11  /* file or lock failure on the server */
#define ST_ERR_ACCOUNT_LOCKED      12  /* too many wrong passwords, wait for the lockout to end */

/* ---------- response kinds (response.kind): which union member is filled ---------- */
#define RESP_NONE      0   /* nothing besides status/message (also the end marker) */
#define RESP_TEXT      1   /* u.text (no server request uses it any more) */
#define RESP_LOGIN     2   /* u.login */
#define RESP_BALANCE   3   /* u.balance */
#define RESP_CREATED   4   /* u.created */
#define RESP_TXN       5   /* u.txn */
#define RESP_LOAN      6   /* u.loan */
#define RESP_FEEDBACK  7   /* u.feedback */

/* ---------- request payloads: one small struct per kind of request ---------- */

struct req_login {                 /* OP_LOGIN */
    char username[32];
    char password[128];
};

struct req_change_password {       /* OP_CHANGE_PASSWORD */
    char old_password[128];
    char new_password[128];
};

struct req_amount {                /* OP_DEPOSIT, OP_WITHDRAW, OP_APPLY_LOAN */
    long long amount;              /* paise */
};

struct req_transfer {              /* OP_TRANSFER (from the logged-in customer) */
    int       to_account;
    long long amount;              /* paise */
};

struct req_feedback {              /* OP_ADD_FEEDBACK */
    char message[256];
};

struct req_account {               /* OP_VIEW_PASSBOOK */
    int account_id;
};

struct req_new_user {              /* OP_ADD_CUSTOMER, OP_ADD_EMPLOYEE (role comes from the opcode) */
    char username[32];
    char password[128];
    char name[50];
    char phone[16];
    char email[50];
};

struct req_modify_user {           /* OP_MODIFY_CUSTOMER, OP_MODIFY_USER */
    int  user_id;
    char name[50];
    char phone[16];
    char email[50];
};

struct req_process_loan {          /* OP_PROCESS_LOAN */
    int loan_id;
    int approve;                   /* 1 = approve, 0 = reject */
};

struct req_set_status {            /* OP_SET_ACCOUNT_STATUS */
    int account_id;
    int status;                    /* ACC_ACTIVE or ACC_DEACTIVATED */
};

struct req_assign_loan {           /* OP_ASSIGN_LOAN */
    int loan_id;
    int employee_id;               /* user_id of an employee */
};

struct req_set_role {              /* OP_SET_ROLE */
    int user_id;
    int role;                      /* ROLE_* */
};

/* ---------- the request ----------
 * OP_LOGOUT, OP_EXIT, OP_VIEW_BALANCE, OP_VIEW_HISTORY,
 * OP_VIEW_ASSIGNED_LOANS and OP_REVIEW_FEEDBACK have no payload. */
struct request {
    int opcode;                    /* OP_* */
    union {
        struct req_login           login;
        struct req_change_password change_password;
        struct req_amount          amount;
        struct req_transfer        transfer;
        struct req_feedback        feedback;
        struct req_account         account;
        struct req_new_user        new_user;
        struct req_modify_user     modify_user;
        struct req_process_loan    process_loan;
        struct req_set_status      set_status;
        struct req_assign_loan     assign_loan;
        struct req_set_role        set_role;
    } u;
};

/* ---------- the response ---------- */
struct response {
    int  status;                   /* ST_OK or an ST_ERR_* code */
    int  kind;                     /* RESP_*: which member of u is filled */
    int  more;                     /* 1 = another response follows, 0 = last one */
    char message[128];             /* short text to show the user */
    union {
        char text[200];
        struct { int user_id; int role; int account_id; } login;
        struct { long long balance; } balance;     /* paise */
        struct { int user_id; int account_id; } created;
        struct Transaction txn;
        struct Loan        loan;
        struct Feedback    feedback;
    } u;
};

#endif
