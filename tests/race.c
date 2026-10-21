/* race.c - shows the "lost update" problem when two processes change one record.
 *
 * It uses its own file, data/race.dat, holding ONE Account record.
 *
 * Usage (from the bank/ folder):
 *     ./race init       create race.dat with balance 0
 *     ./race show       print the current balance
 *     ./race delay      read balance, wait 1 second, write balance + 100
 *     ./race loop       do "read, add 1, write" 100000 times
 *
 * Run two copies at the same time with:
 *     ./race delay & ./race delay & wait
 *     ./race loop  & ./race loop  & wait
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "structs.h"
#include "storage.h"

#define RACE_FILE   DATA_DIR "/race.dat"
#define LOOP_COUNT  100000

static int do_init(void)
{
    struct Account a;
    int fd;

    unlink(RACE_FILE);
    fd = storage_open(RACE_FILE);
    if (fd == -1) { perror("open"); return 1; }

    memset(&a, 0, sizeof(a));
    a.account_id = 0;
    a.user_id = 0;
    a.balance = 0;
    a.status = ACC_ACTIVE;
    storage_write(fd, 0, &a, sizeof(a));
    storage_close(fd);
    printf("race.dat created, balance = 0\n");
    return 0;
}

static int do_show(void)
{
    struct Account a;
    int fd = storage_open(RACE_FILE);

    if (fd == -1) { perror("open"); return 1; }
    if (storage_read(fd, 0, &a, sizeof(a)) == -1) { perror("read"); return 1; }
    printf("balance = %lld\n", a.balance);
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

int main(int argc, char *argv[])
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s init|show|delay|loop\n", argv[0]);
        return 1;
    }
    if (strcmp(argv[1], "init") == 0)  return do_init();
    if (strcmp(argv[1], "show") == 0)  return do_show();
    if (strcmp(argv[1], "delay") == 0) return do_delay();
    if (strcmp(argv[1], "loop") == 0)  return do_loop();

    fprintf(stderr, "unknown mode '%s'\n", argv[1]);
    return 1;
}
