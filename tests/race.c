/* race.c - lost-update demo (Day 1) and its fixes with locks (Day 2).
 *
 * Files used (all in data/):
 *     race.dat    TWO Account records (id 0 and id 1)
 *     append.dat  records added by the append test
 *
 * Usage (from the bank/ folder):
 *     ./race init                    create race.dat (balances 0) and clear append.dat
 *     ./race show                    print both balances
 *
 *   Day 1 (no locks - shows the bug):
 *     ./race delay                   read, wait 1 second, write balance + 100
 *     ./race loop                    "read, add 1, write" 100000 times
 *
 *   Day 2 (with locks):
 *     ./race lockloop               same as loop, but each round holds a record lock
 *     ./race append nolock|lock      add 1000 new records to append.dat
 *     ./race appendcheck N           check append.dat holds exactly N records, ids correct
 *     ./race deadlock A B            lock record A, wait 1 sec, then lock record B
 *     ./race ordered A B             same, but always locks the LOWER id first
 *
 * Run two copies at the same time with:
 *     ./race loop     & ./race loop     & wait
 *     ./race lockloop & ./race lockloop & wait
 *     ./race append nolock & ./race append nolock & wait
 *     ./race deadlock 0 1 & ./race deadlock 1 0 & wait
 *     ./race ordered  0 1 & ./race ordered  1 0 & wait
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "structs.h"
#include "storage.h"

#define RACE_FILE    DATA_DIR "/race.dat"
#define APPEND_FILE  DATA_DIR "/append.dat"
#define LOOP_COUNT   100000
#define APPEND_COUNT 1000

static int do_init(void)
{
    struct Account a;
    int fd;

    unlink(RACE_FILE);
    unlink(APPEND_FILE);
    fd = storage_open(RACE_FILE);
    if (fd == -1) { perror("open"); return 1; }

    /* two accounts, so the deadlock demo has two records to fight over */
    for (int id = 0; id < 2; id++) {
        memset(&a, 0, sizeof(a));
        a.account_id = id;
        a.user_id = id;
        a.balance = 0;
        a.status = ACC_ACTIVE;
        storage_write(fd, id, &a, sizeof(a));
    }
    storage_close(fd);
    printf("race.dat created (2 accounts, balance 0), append.dat cleared\n");
    return 0;
}

static int do_show(void)
{
    struct Account a;
    int fd = storage_open(RACE_FILE);

    if (fd == -1) { perror("open"); return 1; }
    for (int id = 0; id < 2; id++) {
        if (storage_read(fd, id, &a, sizeof(a)) == -1) { perror("read"); return 1; }
        printf("account %d balance = %lld\n", id, a.balance);
    }
    storage_close(fd);
    return 0;
}

/* Version 1: a deliberate pause between read and write.
 * Two copies started together both read the SAME old balance,
 * so one deposit is lost. */
static int do_delay(void)
{
    struct Account a;
    int fd = storage_open(RACE_FILE);

    if (fd == -1) { perror("open"); return 1; }

    storage_read(fd, 0, &a, sizeof(a));
    printf("[pid %d] read balance = %lld\n", (int)getpid(), a.balance);

    sleep(1);                         /* the other process reads during this pause */

    a.balance += 100;                 /* deposit of 100 */
    storage_write(fd, 0, &a, sizeof(a));
    printf("[pid %d] wrote balance = %lld\n", (int)getpid(), a.balance);

    storage_close(fd);
    return 0;
}

/* Version 2: no pause at all, just many quick read-add-write rounds.
 * The processes still collide sometimes, so the total comes out too low. */
static int do_loop(void)
{
    struct Account a;
    int fd = storage_open(RACE_FILE);

    if (fd == -1) { perror("open"); return 1; }

    for (int i = 0; i < LOOP_COUNT; i++) {
        storage_read(fd, 0, &a, sizeof(a));
        a.balance += 1;
        storage_write(fd, 0, &a, sizeof(a));
    }

    printf("[pid %d] finished %d rounds\n", (int)getpid(), LOOP_COUNT);
    storage_close(fd);
    return 0;
}

/* Day 2 fix for the loop: the same read-add-write, but the record is locked
 * for the whole round, so nobody else can sneak in between. */
static int do_lockloop(void)
{
    struct Account a;
    int fd = storage_open(RACE_FILE);

    if (fd == -1) { perror("open"); return 1; }

    for (int i = 0; i < LOOP_COUNT; i++) {
        if (storage_lock(fd, 0, sizeof(a), F_WRLCK) == -1) { perror("lock"); return 1; }
        storage_read(fd, 0, &a, sizeof(a));
        a.balance += 1;
        storage_write(fd, 0, &a, sizeof(a));
        storage_unlock(fd, 0, sizeof(a));
    }

    printf("[pid %d] finished %d locked rounds\n", (int)getpid(), LOOP_COUNT);
    storage_close(fd);
    return 0;
}

/* Append test: each round picks "next id = count" and writes a new record.
 * Without the append lock, two processes can pick the same id, so one
 * record overwrites the other and the file ends up too short. */
static int do_append(const char *mode)
{
    struct Account a;
    int use_lock;
    int fd;

    if (strcmp(mode, "lock") == 0)        use_lock = 1;
    else if (strcmp(mode, "nolock") == 0) use_lock = 0;
    else { fprintf(stderr, "append needs 'lock' or 'nolock'\n"); return 1; }

    fd = storage_open(APPEND_FILE);
    if (fd == -1) { perror("open"); return 1; }

    for (int i = 0; i < APPEND_COUNT; i++) {
        int id;

        if (use_lock && storage_append_lock(fd) == -1) { perror("append lock"); return 1; }

        id = storage_count(fd, sizeof(a));          /* the "next id" */
        memset(&a, 0, sizeof(a));
        a.account_id = id;
        a.user_id = (int)getpid();
        a.status = ACC_ACTIVE;
        storage_write(fd, id, &a, sizeof(a));

        if (use_lock) storage_append_unlock(fd);
    }

    printf("[pid %d] appended %d records (%s)\n", (int)getpid(), APPEND_COUNT, mode);
    storage_close(fd);
    return 0;
}

/* Check the append result: exactly N records, and record n has account_id n. */
static int do_appendcheck(const char *expected_str)
{
    struct Account a;
    int expected = atoi(expected_str);
    int fd = storage_open(APPEND_FILE);
    int count, bad = 0;

    if (fd == -1) { perror("open"); return 1; }
    count = storage_count(fd, sizeof(a));
    for (int id = 0; id < count; id++) {
        if (storage_read(fd, id, &a, sizeof(a)) == -1 || a.account_id != id)
            bad++;
    }
    printf("records in file = %d (expected %d), records with wrong id = %d\n",
           count, expected, bad);
    printf("%s\n", (count == expected && bad == 0) ? "RESULT: OK" : "RESULT: LOST RECORDS");
    storage_close(fd);
    return 0;
}

/* Lock record `first`, pause, then lock record `second`.
 * If another process does it in the opposite order, both wait on each other. */
static int lock_two(int first, int second)
{
    struct Account a;
    int fd = storage_open(RACE_FILE);

    if (fd == -1) { perror("open"); return 1; }

    printf("[pid %d] locking record %d ...\n", (int)getpid(), first);
    if (storage_lock(fd, first, sizeof(a), F_WRLCK) == -1) { perror("lock first"); return 1; }
    printf("[pid %d] got record %d\n", (int)getpid(), first);

    sleep(1);                         /* time for the other process to take its first lock */

    printf("[pid %d] locking record %d ...\n", (int)getpid(), second);
    if (storage_lock(fd, second, sizeof(a), F_WRLCK) == -1) {
        printf("[pid %d] could not lock record %d: %s\n",
               (int)getpid(), second, strerror(errno));
        storage_close(fd);            /* closing releases our locks */
        return 1;
    }
    printf("[pid %d] got record %d - holding both, done\n", (int)getpid(), second);

    storage_unlock(fd, second, sizeof(a));
    storage_unlock(fd, first, sizeof(a));
    storage_close(fd);
    return 0;
}

static int do_deadlock(int a, int b)
{
    return lock_two(a, b);            /* order exactly as the user typed it */
}

static int do_ordered(int a, int b)
{
    if (a < b) return lock_two(a, b); /* lower id always first: no deadlock possible */
    return lock_two(b, a);
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s init|show|delay|loop|lockloop|append|appendcheck|deadlock|ordered\n", argv[0]);
        return 1;
    }
    if (strcmp(argv[1], "init") == 0)      return do_init();
    if (strcmp(argv[1], "show") == 0)      return do_show();
    if (strcmp(argv[1], "delay") == 0)     return do_delay();
    if (strcmp(argv[1], "loop") == 0)      return do_loop();
    if (strcmp(argv[1], "lockloop") == 0)  return do_lockloop();

    if (strcmp(argv[1], "append") == 0 && argc == 3)
        return do_append(argv[2]);
    if (strcmp(argv[1], "appendcheck") == 0 && argc == 3)
        return do_appendcheck(argv[2]);
    if (strcmp(argv[1], "deadlock") == 0 && argc == 4)
        return do_deadlock(atoi(argv[2]), atoi(argv[3]));
    if (strcmp(argv[1], "ordered") == 0 && argc == 4)
        return do_ordered(atoi(argv[2]), atoi(argv[3]));

    fprintf(stderr, "unknown mode or wrong number of arguments\n");
    return 1;
}
