# Banking Management System: Viva Notes (easy version)

Read this the night before the viva. Every answer says *what we did*, *why*, and *where in the code*.

---

## 1. The project in 30 seconds

A bank where many clients talk to one server at the same time.

- **Client** (`bankclient`): shows a menu, sends a request, prints the reply. It does no real checking.
- **Server** (`bankserver`): one **thread per client**. It checks every rule.
- **Data**: five binary files (`users.dat`, `accounts.dat`, `txns.dat`, `loans.dat`, `feedback.dat`). Each record has a fixed size, and its **id is its record number**, so record `n` starts at byte `n x record size`.
- **Four roles**: customer, employee, manager, administrator.

How a request travels:

```
client  --TCP-->  server thread  -->  dispatcher (login + role check)
                                  -->  handler (one file per role)
                                  -->  storage layer (read/write + locks)  -->  .dat files
```

Only the **storage layer** touches the files. Only the **server layer** touches sockets.

---

## 2. ACID (the part the PDF names)

| Letter | Plain meaning | What we do | Where |
|---|---|---|---|
| **A**tomicity | A transaction happens fully or not at all | A transfer write-locks **both** accounts, writes both history rows under one append lock, writes both balances, then unlocks. Nobody else can see it half-done while the locks are held. | `handlers_customer.c` (`do_transfer`, `transfer_locked`) |
| **C**onsistency | Data always follows the rules | The server checks every rule: amount above 0, balance never below 0, no overflow, no transfer to yourself, deactivated accounts refused, usernames unique. Money is whole **paise** (`long long`), never float. Each history row stores `balance_after`, written while the account is locked, so it always matches. | `handlers_customer.c`, `handlers_employee.c`, `structs.h` |
| **I**solation | Clients do not disturb each other | One lock **per record** (`fcntl` OFD locks). `F_WRLCK` to change, `F_RDLCK` to read. Transfers lock the lower account id first. | `storage.c` (`storage_lock`, `storage_unlock`, `storage_append_lock`) |
| **D**urability | Finished work survives | Every change is written to the file with `write()` before the reply is sent, so it survives a client crash or a server restart. | `storage.c` |

### Be honest about the limits (examiners respect this)

- **Atomicity has a gap on a crash.** If the server is killed with `kill -9` or the power fails *in the middle* of a transfer, one account can be updated and the other not.
  - *How to fix it:* write a **redo log** (what the transfer will do) before changing anything, mark it done at the end, and finish any unfinished entry when the server starts. We did not build this because it is not in the problem statement.
- **Durability has a gap on a power cut.** We do not call `fsync`, so a very recent write can still be in the operating system's memory if the whole machine loses power.
- A history row is written **before** the balance. If a crash falls between the two, you get a row without a balance change. The reverse order would be worse (a balance change with no record).

---

## 3. Concurrency cheat sheet

| Problem | Our answer | Why |
|---|---|---|
| Two clients change one balance at once (**lost update**) | Lock the record, read, change, write, unlock | Without it, the 2-process test gave **135,200** instead of **200,000**. With it, exactly 200,000 every run. |
| Which lock type? | **OFD locks** (`F_OFD_SETLKW`), one `open()` per thread | Classic `fcntl` locks belong to the *process*, so threads of one process never block each other. OFD locks belong to the *open file*, so each thread is its own owner. Test: 146,330 with classic, 200,000 with OFD. |
| Two threads adding a record get the same id | **Append lock**: lock one byte at offset 1,000,000,000 | That byte is never a real record, and locking it does not grow the file. |
| **Deadlock** in a transfer (A to B while B to A) | Always lock the **lower account id first** | OFD locks have no kernel deadlock detection, so this rule is the only protection. Lock order is: accounts, then the append lock, never reversed. |
| Same user logging in twice | `logged_in` flag in the user record, guarded by a **System V semaphore** | Flag cleared on logout, on exit, and in the thread's cleanup (so a killed client cannot leave a stuck session). Lock order: semaphore first, then the record lock. |
| A client dies mid-write to a socket | `signal(SIGPIPE, SIG_IGN)` once in `main` | Otherwise `SIGPIPE` would kill the whole server. |
| Too many open files | Data files opened **per request**, closed after | Idle clients hold no descriptors. |
| Guessing passwords | 5 wrong passwords lock the user for 30 minutes, on every connection | Count lives in the user record, updated under that record's write lock. |
| Role changed while the user is logged in | Dispatcher re-reads the role before every role-restricted request | The change takes effect on the very next request. |

Passwords are stored as **SHA-512 hashes with a random salt** (`crypt_r`, thread-safe). The hash is computed *before* taking a record lock, because hashing is slow on purpose.

---

## 4. System calls and where they are used

The PDF says "use system calls instead of library functions wherever possible".

| Area | Calls |
|---|---|
| Files | `open`, `read`, `write`, `lseek`, `close`, `fstat` (in `storage.c`) |
| File locking | `fcntl` with `F_OFD_SETLKW` |
| Sockets | `socket`, `setsockopt` (`SO_REUSEADDR`), `bind`, `listen`, `accept`, `connect`, `read`, `write` |
| Threads | `pthread_create` (detached, one per client) |
| Semaphore / IPC | `ftok`, `semget`, `semop`, `semctl` |
| Signals | `SIGPIPE` ignored; a handler for `SIGINT`/`SIGTERM` stops the server and removes the semaphore |
| Terminal | `termios` (`tcgetattr`, `tcsetattr`) for the `*` password display |
| Randomness | reading `/dev/urandom` for the salt |

**If asked "where is `fork` / pipe?":** the server deliberately has **no `fork`**. Mixing `fork` into a multi-threaded server is unsafe (only the calling thread survives in the child), and we chose threads. Our IPC is the System V **semaphore** and the **sockets**. The race demos run as separate processes.

---

## 5. Likely viva questions, short answers

1. **Why record locks, not whole-file locks?** Customers on different accounts never wait for each other. A transfer needs two specific records locked anyway.
2. **How do you prevent deadlock?** Lower account id first, always. Then the append lock, never before the accounts.
3. **Why threads and not fork per client?** Cheaper, shares memory, easy to give each client its own session struct. And `fork` inside a threaded server is unsafe.
4. **Why OFD locks?** Classic locks are per process, so they fail across threads. OFD locks are per open file, so each thread (with its own `open`) is a separate owner.
5. **What happens if the server crashes mid-transfer?** One account may be updated and not the other. Fix: a redo log written first and finished at restart (not built).
6. **Why integers for money?** Floats cannot store 0.10 exactly, so errors build up. We use `long long` paise.
7. **Why fixed-size records, and the cost?** Finding record `n` is one `lseek` (no search). Cost: long strings are cut, space is wasted, and changing a struct needs a reseed.
8. **What does "advisory lock" mean?** The kernel does not stop a program that ignores the lock. That is why *only* the storage layer touches the files.
9. **Why do `read_full` / `write_full` exist?** TCP has no message boundaries. A `read()` can return fewer bytes than asked, so we loop until the whole fixed-size message has arrived.
10. **Why a semaphore if the record lock already protects the flag?** The record lock already makes the update safe. The semaphore is the course's synchronisation tool for the one-session rule. It adds a fixed order and a single place where login state changes, and costs a little extra waiting.
11. **Why is "already logged in" checked after the password?** Only someone with the right password learns that the user is online. A refusal for this reason does not count as a wrong password.
12. **Why hash before taking the record lock?** Hashing is slow. Holding the lock during it would make others wait.
13. **Why send replies only after unlocking?** A slow client must never keep a record locked.
14. **Why does the client never send its own account id?** The server remembers who logged in. A client cannot ask for someone else's account.
15. **Why does approving a loan not change the balance?** The problem statement only asks for approve/reject. Disbursing money would need its own audited path.

---

## 6. Quick demo script (5 minutes)

1. `./seed` then `./bankserver`.
2. `./bankclient` as `hetul` / `hetul123`: balance, deposit, transfer, history.
3. As `emp` / `emp123`: add a customer, process loan 1, passbook.
4. As `man` / `man123`: deactivate an account, assign loan 0 to employee 2, review feedback.
5. As `admin` / `admin123`: add an employee, manage roles.
6. Try the same user in two terminals: second login refused.
7. Five wrong passwords: account locked.
8. Show the race demo: `./race loop` run in two terminals at the same time gives a wrong total, `./race lockloop` in two terminals gives exactly 200,000. (Run `./race init` first; use `./race show` to see the result.)

*Seed data is demo data only. Reseed with the server stopped.*
