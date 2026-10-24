# Banking Management System

A multi-client banking server written in C for Linux, built for the **CS-513 System Software** mini project. Many clients connect over TCP at the same time. The server keeps all data in binary files and uses record locks, a semaphore and threads to stay correct under concurrency.

## Features

| Role | What they can do |
|---|---|
| **Customer** | View balance, deposit, withdraw, transfer funds, apply for a loan, add feedback, view transaction history, change password |
| **Employee** | Add customers, modify customer details, view assigned loans, approve or reject loans, view any customer's passbook, change password |
| **Manager** | Activate or deactivate accounts, assign loans to employees, review feedback, change password |
| **Administrator** | Add employees, modify customer or employee details, manage roles (employee and manager), change password |

Every role also has login, logout and exit. Each user can have only **one active session** at a time.

## How it works

```
bankclient (menu per role)  --TCP :8080-->  bankserver (one thread per client)
                                               |
                                      dispatcher: login + role check
                                               |
                                      handlers (one file per role)
                                               |
                                      storage layer: read/write + locks
                                               |
                                      data/*.dat  (users, accounts, txns, loans, feedback)
```

- **Fixed-size records.** Each file holds one struct type. A record's id is its position in the file.
- **Fixed-size messages.** Client and server exchange `struct request` and `struct response`.
- **Money** is stored as whole paise (`long long`), never floating point.
- **The server decides.** The client only checks formats; every rule is enforced on the server.

### Concurrency and ACID

- **Isolation:** one `fcntl` **OFD record lock** per record. Each thread opens its own descriptor, so threads of one process block each other correctly.
- **Deadlock avoidance:** transfers lock the lower account id first. The append lock is always taken after the account locks.
- **New ids:** an append lock on a sentinel byte, so two writers never get the same id.
- **Atomicity:** a transfer holds both account locks while it writes both history rows and both balances.
- **Consistency:** the server refuses zero or negative amounts, overdrafts, overflow, self-transfers, and operations on deactivated accounts.
- **One session per user:** a `logged_in` flag guarded by a System V semaphore, cleared even if a client dies.
- **Security:** passwords are stored as salted SHA-512 hashes (`crypt_r`). Five wrong passwords lock a user for 30 minutes.

## Requirements

- Linux with `gcc`, GNU `make` 3.82 or newer, and the `crypt` library (on Ubuntu: `sudo apt install build-essential libcrypt-dev`)
- Developed and tested on Ubuntu (ARM64) under UTM

Keep the project on the machine's own disk. File locking can misbehave on shared or network folders.

## Build and run

```bash
make                  # builds bankserver, bankclient, testclient, seed, dump and the test tools
./seed                # creates data/ with demo users (server must NOT be running)
./bankserver          # terminal 1: listens on port 8080
./bankclient          # terminal 2 (and more): connects to 127.0.0.1:8080
```

Stop the server with Ctrl+C (it removes its semaphore). `make clean` deletes the built programs.

## Demo logins (created by `./seed`)

| Username | Password | Role |
|---|---|---|
| `admin` | `admin123` | Administrator |
| `man` | `man123` | Manager |
| `emp` | `emp123` | Employee |
| `hetul` | `hetul123` | Customer (account 0, 1000.00) |
| `cust` | `cust123` | Customer (account 1, 500.00) |

These are demo values created by the seed program, not real credentials.

## Project layout

```
common/    structs.h, protocol.h, netio.c (read_full / write_full), password.c (hashing)
storage/   storage.c: record read/write and locking (the only code that touches files)
server/    server.c (sockets, threads), dispatcher.c, sessions.c (semaphore), handlers_*.c
client/    main.c, ui.c, one menu file per role
tests/     seed, dump, race, threadrace, protosize, test_password, testclient
docs/      class diagram (class-diagram.drawio), viva notes
Makefile
```

## Tests and demos

| Tool | What it shows |
|---|---|
| `./race` | Two processes updating one balance: without locks the total is wrong (about 135,000 instead of 200,000); with locks it is exactly 200,000 |
| `./threadrace` | Two threads: classic `fcntl` locks fail, OFD locks give exactly 200,000 |
| `./testclient login hetul hetul123 balance logout exit` | Scripted requests, useful for several clients at once |
| `./test_password` | Checks password hashing and verification |
| `./dump users` | Prints any data file (`users`, `accounts`, `txns`, `loans`, `feedback`) |

Ten clients each making ten 1-rupee deposits on one account ended with the exact expected balance.

## Known limitations

- A server crash (`kill -9` or power loss) in the middle of a transfer can leave one account updated and the other not. A redo log would fix this; it is not implemented.
- Files are written with `write()` but not `fsync`ed, so a power cut can lose the last few writes.
- Ids for users and loans are typed by hand (use `./dump` to look them up).
- No idle timeout and no limit on the number of clients.
- Unknown usernames are only rate-limited per connection, so they can be probed by reconnecting.
- The client connects to a fixed address, `127.0.0.1:8080`.
