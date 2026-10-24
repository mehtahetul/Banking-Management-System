/* sessions.c - the semaphore and the logged_in flag.
 *
 * ONE binary semaphore (value 1 = free, 0 = taken) guards the step
 * "look at logged_in, then set it". Without it, two connections could both
 * see 0 and both set 1. */
#include <errno.h>
#include <stdio.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/types.h>

#include "sessions.h"
#include "structs.h"
#include "storage.h"

/* Linux wants us to define this ourselves to use semctl(SETVAL). */
union semun {
    int val;
    struct semid_ds *buf;
    unsigned short *array;
};

static int sem_id = -1;

/* Add delta to the semaphore: -1 = wait (P), +1 = release (V).
 * semop sleeps while the value would go below 0. */
static int sem_change(int delta)
{
    struct sembuf op;

    op.sem_num = 0;
    op.sem_op = delta;
    op.sem_flg = 0;
    while (semop(sem_id, &op, 1) == -1) {
        if (errno != EINTR)
            return -1;
    }
    return 0;
}

/* Set logged_in to 'value' for one user. Takes the semaphore, then the
 * record's write lock. If value is 1 and the flag is already 1, change
 * nothing and return SESS_ALREADY. */
static int set_flag(int user_id, int value)
{
    struct User u;
    int fd, result = SESS_ERROR;

    if (sem_change(-1) == -1)
        return SESS_ERROR;

    fd = storage_open(USERS_FILE);
    if (fd != -1) {
        if (storage_lock(fd, user_id, sizeof(u), F_WRLCK) == 0) {
            if (storage_read(fd, user_id, &u, sizeof(u)) == 0) {
                if (value == 1 && u.logged_in) {
                    result = SESS_ALREADY;
                } else {
                    u.logged_in = value;
                    if (storage_write(fd, user_id, &u, sizeof(u)) == 0)
                        result = SESS_OK;
                }
            }
            storage_unlock(fd, user_id, sizeof(u));
        }
        storage_close(fd);
    }

    sem_change(1);
    return result;
}

int sessions_login(int user_id)
{
    return set_flag(user_id, 1);
}

int sessions_logout(int user_id)
{
    return (set_flag(user_id, 0) == SESS_OK) ? 0 : -1;
}

/* At start nobody is connected, so every flag must be 0. A server that
 * crashed leaves old 1s behind, which would lock those users out. */
static int reset_all_flags(void)
{
    struct User u;
    int fd = storage_open(USERS_FILE);
    int count, id;

    if (fd == -1)
        return -1;
    count = storage_count(fd, sizeof(u));
    if (count == -1) {
        storage_close(fd);
        return -1;
    }
    for (id = 0; id < count; id++) {
        if (storage_lock(fd, id, sizeof(u), F_WRLCK) == -1) {
            storage_close(fd);
            return -1;
        }
        if (storage_read(fd, id, &u, sizeof(u)) == 0 && u.logged_in) {
            u.logged_in = 0;
            storage_write(fd, id, &u, sizeof(u));
        }
        storage_unlock(fd, id, sizeof(u));
    }
    storage_close(fd);
    return 0;
}

int sessions_init(void)
{
    union semun arg;
    key_t key;
    int old;

    /* the key comes from the users file, so the file must exist */
    key = ftok(USERS_FILE, 'B');
    if (key == -1) {
        perror("ftok (does data/users.dat exist? run ./seed first)");
        return -1;
    }

    /* remove a semaphore left behind by an earlier run */
    old = semget(key, 1, 0);
    if (old != -1)
        semctl(old, 0, IPC_RMID);

    sem_id = semget(key, 1, IPC_CREAT | IPC_EXCL | 0600);
    if (sem_id == -1) {
        perror("semget");
        return -1;
    }
    arg.val = 1;                       /* 1 = free */
    if (semctl(sem_id, 0, SETVAL, arg) == -1) {
        perror("semctl SETVAL");
        return -1;
    }
    return reset_all_flags();
}

void sessions_cleanup(void)
{
    if (sem_id != -1)
        semctl(sem_id, 0, IPC_RMID);
}
