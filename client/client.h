/* client.h - shared by all the client files. The client is "dumb": it shows
 * menus, sends a request, prints the reply. All real checks are on the server. */
#ifndef CLIENT_H
#define CLIENT_H

#include <stddef.h>
#include "protocol.h"

/* What a menu or an action returns. */
#define MENU_STAY        0   /* stay in this menu */
#define MENU_LOGGED_OUT  1   /* the user logged out: back to the login screen */
#define MENU_EXIT        2   /* leave the program */
#define MENU_LOST        3   /* the server connection is gone: leave the program */

/* ---------- ui.c: keyboard, screen and talking to the server ---------- */
void ui_set_server(int fd);                         /* which socket call_server uses */
int call_server(const struct request *req, struct response *last);  /* 0, or -1 if the connection is lost */
int read_line(const char *prompt, char *buf, size_t size);          /* 0, or -1 at end of input */
int read_int(const char *prompt, int *out);                         /* repeats until valid; -1 at end of input */
int read_money(const char *prompt, long long *paise);               /* rupees like 50 or 50.25 -> paise */
int read_password(const char *prompt, char *buf, size_t size);      /* shows * per key; -1 at end of input */
void copy_field(char *dst, size_t dst_size, const char *src);       /* zero dst, copy at most dst_size-1 chars */
void format_money(long long paise, char *buf, size_t size);
const char *role_name(int role);

/* ---------- menu_common.c: things every role has ---------- */
int send_simple(int opcode);          /* a request with no payload; MENU_STAY or MENU_LOST */
int action_change_password(void);
int action_logout(void);
int action_exit(void);

/* ---------- one menu per role; each returns a MENU_* code ---------- */
int menu_customer(void);
int menu_employee(void);
int menu_manager(void);
int menu_admin(void);

#endif
