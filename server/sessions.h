/* sessions.h - one session per user: a semaphore plus the logged_in flag.
 *
 * Lock order (never reverse it):
 *   1. the semaphore
 *   2. the user's record lock
 * Nothing that holds a record lock may wait for the semaphore. */
#ifndef SESSIONS_H
#define SESSIONS_H

#define SESS_OK       0
#define SESS_ALREADY  1     /* the user is already logged in somewhere */
#define SESS_ERROR  (-1)

/* Call once at server start, before accepting clients. Removes an old
 * semaphore left by an earlier run, creates a fresh one, and sets every
 * logged_in flag to 0. Returns 0, or -1 on error. */
int sessions_init(void);

/* Remove the semaphore. Called when the server is stopped. */
void sessions_cleanup(void);

/* Try to mark the user as logged in. SESS_OK, SESS_ALREADY or SESS_ERROR. */
int sessions_login(int user_id);

/* Mark the user as logged out. 0, or -1 on error. */
int sessions_logout(int user_id);

#endif
