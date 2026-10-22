/* threadrace.c - two THREADS in one process update the same record.
 *
 * Build and run from the project root (the folder that contains data/):
 *   gcc -Wall -Wextra -pthread -I common -I storage -o threadrace tests/threadrace.c storage/storage.c
 *   ./threadrace
 *
 * Each thread opens its own file descriptor, adds 1 to the balance of
 * record 0, ITERATIONS times, using lock -> read -> modify -> write -> unlock.
 * Expected final balance: NUM_THREADS * ITERATIONS.
 *
 * With classic fcntl locks (F_SETLKW) the result is too low, because the
 * kernel sees both threads as the same process and grants the lock to both.
 * With OFD locks (F_OFD_SETLKW) the result is exact every time.
 * Uses its own scratch file, so seeded data is never touched.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#include "structs.h"
#include "storage.h"

#define SCRATCH_FILE  DATA_DIR "/threadrace.dat"
#define NUM_THREADS   2
#define ITERATIONS    100000

static void *worker(void *arg)
{
    (void)arg;

    /* each thread opens its own fd */
    int fd = storage_open(SCRATCH_FILE);
    if (fd == -1) {
        perror("worker: open");
        return NULL;
    }

    for (int i = 0; i < ITERATIONS; i++) {
        struct Account a;

        storage_lock(fd, 0, sizeof(a), F_WRLCK);
        if (storage_read(fd, 0, &a, sizeof(a)) == -1) {
            perror("worker: read");
            storage_unlock(fd, 0, sizeof(a));
            break;
        }
        a.balance++;
        if (storage_write(fd, 0, &a, sizeof(a)) == -1) {
            perror("worker: write");
            storage_unlock(fd, 0, sizeof(a));
            break;
        }
        storage_unlock(fd, 0, sizeof(a));
    }

    storage_close(fd);
    return NULL;
}

int main(void)
{
    pthread_t threads[NUM_THREADS];
    struct Account a;
    int fd;

    /* fresh scratch file with one account, balance 0 */
    unlink(SCRATCH_FILE);
    fd = storage_open(SCRATCH_FILE);
    if (fd == -1) { perror("open scratch"); return 1; }
    memset(&a, 0, sizeof(a));
    a.account_id = 0;
    a.user_id = 0;
    a.balance = 0;
    a.status = ACC_ACTIVE;
    if (storage_write(fd, 0, &a, sizeof(a)) == -1) {
        perror("write scratch");
        return 1;
    }
    storage_close(fd);

    /* start the threads */
    for (int i = 0; i < NUM_THREADS; i++) {
        if (pthread_create(&threads[i], NULL, worker, NULL) != 0) {
            perror("pthread_create");
            return 1;
        }
    }
    for (int i = 0; i < NUM_THREADS; i++)
        pthread_join(threads[i], NULL);

    /* read the result */
    fd = storage_open(SCRATCH_FILE);
    if (fd == -1) { perror("reopen scratch"); return 1; }
    if (storage_read(fd, 0, &a, sizeof(a)) == -1) {
        perror("read result");
        return 1;
    }
    storage_close(fd);

    long long expected = (long long)NUM_THREADS * ITERATIONS;
    printf("expected: %lld\n", expected);
    printf("actual:   %lld\n", a.balance);
    if (a.balance == expected)
        printf("RESULT: exact\n");
    else
        printf("RESULT: lost %lld updates\n", expected - a.balance);

    return 0;
}
