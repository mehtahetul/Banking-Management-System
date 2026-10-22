# LEARNING.md

One entry per bug or big lesson. Keep it in your own words.

---

## Day 1

### Bug 1: Lost update (two processes, one record)

**What I did**
Wrote `tests/race.c`. It keeps one Account record in `data/race.dat` and runs two copies at the same time. There are two modes.
- `delay`: read the balance, `sleep(1)`, write balance + 100.
- `loop`: do "read, add 1, write" 100,000 times with no pause.

**What I expected**
- delay: 0 + 100 + 100 = 200
- loop: 100,000 + 100,000 = 200,000

**What actually happened**
- delay: balance was **100**. Both processes read 0 before either wrote, so one deposit was lost. It gave 100 every time.
- loop: balance was **135,200**, about 64,800 updates lost. The wrong number was different on every run.

**Why it happens**
"Read, add, write" is three separate steps, not one. Another process can slip in between them:

```
A reads 50, B reads 50, A writes 51, B writes 51   <- two increments, only +1
```

The second write overwrites the first, using an old value.

**Why delay is always 100 but loop changes every run**
The 1-second sleep forces both processes to read before either writes, so the result is the same each time. The loop has no sleep, so a collision depends on exact OS scheduling and timing, which is different on every run. This makes it hard to test by luck, and it is why a test that passes once proves nothing.

**Why it matters for the bank**
Two customers depositing at the same moment could make the bank lose money. The assignment asks for correct handling of race conditions and ACID properties.

**Fix (Day 2)**
Add `fcntl` record locks inside `storage.c`. Lock a record before reading it and release it after writing. Rerun the same `loop` test. The target is exactly 200,000 every run.

**One line for the viva**
> Without locking, read-modify-write is not atomic, so two processes can read the same old value and one update is lost.

---

### Other things learned on Day 1

- **Fixed-size records:** every struct has a fixed size, so record `n` is at byte `n * sizeof(struct)`. Finding a record is one calculation, with no searching. The cost is that long strings get cut off and some space is wasted.
- **Padding:** the compiler can add hidden bytes between struct fields. I `memset` each struct to zero before filling it, so the file contents are predictable.
- **Layers:** only the storage layer (`storage.c`) touches the data files. Other code asks it for records by id. This is why locks can be added on Day 2 without changing the callers.
- **Money:** stored as `long long` in paise, never float or double. `dump` shows it as rupees, for example 100050 as 1000.50.
- **Time:** stored as one number (seconds since 1970). `dump` formats it as `dd:mm:yyyy -- hh:mm:ss`. The stored value is unchanged, only the display changes.
- **Tools:** `seed` creates sample data, `dump` prints a data file in readable form, `race` runs the race demo.

---

## Day 2

### Fix 1: Record locks stop the lost update

**What I did**
Added `storage_lock` and `storage_unlock` to `storage.c`. They use `fcntl` with `F_SETLKW` to lock the bytes of one record (`id * rec_size`, length `rec_size`). Added a `lockloop` mode to `race.c`: lock, read, add 1, write, unlock, 100,000 times, in two processes at once.

**What I expected**
200,000 every time.

**What actually happened**
- Without locks (`loop`): 134,835 and 135,017.
- With locks (`lockloop`): **200,000** both times.

**Why it works**
Lock, read, add, write and unlock now happen as one unit. The other process waits at its `storage_lock` call until I unlock, so it can never read the old value while I am in the middle.

**One line for the viva**
> A lock makes read-modify-write behave like one step, because anyone else wanting the same record has to wait their turn.

---

### Bug 2: Two new records get the same id (append race)

**What I did**
Wrote `append nolock` in `race.c`. Each round it does `id = count`, then writes a new record at that id. Two processes add 1000 records each. `appendcheck 2000` counts the records.

**What I expected**
2,000 records.

**What actually happened**
- First run: **1,996** records, so 4 were lost.
- Second run: 2,000 records, which looked fine.

**Why it happens**
Both processes can read the same `count` before either writes, so both write at the same slot and one record overwrites the other. It is the same kind of bug as the lost update, but on "next id".

**Why the second run passed**
The two processes barely overlapped, because one was almost finished before the other got going. A collision needs both to run at the same moment. So a test without locks that passes once proves nothing, and the number lost is different every run (4 lost in my first run, 0 in my second).

**Fix**
`storage_append_lock` locks one sentinel byte at offset 1,000,000,000. A process holds it while it reads `count` and writes the new record, so only one process can pick the next id at a time. With the lock (`append lock`): 2,000 records and correct ids.

**Why the file does not become a billion bytes**
A lock is only a note in the kernel's table. It does not write anything to the file, so the file size stays the same. Locking bytes past the end is allowed.

**One line for the viva**
> Picking the next id is also a read-modify-write, so it needs its own lock. Without it two clients can get the same id and lose a record.

---

### Surprise 3: The kernel caught the deadlock

**What I did**
Wrote `deadlock A B`: lock record A, sleep 1 second, then lock record B. I ran `./race deadlock 0 1 & ./race deadlock 1 0 & wait`. The sleep makes sure both processes hold their first record before asking for the second.

**What I expected**
Both processes hang forever, each waiting for the other.

**What actually happened**
Nothing hung. One process (pid 6432) got `Resource deadlock avoided` on its second lock and exited. The other (pid 6431) then got record 1 and finished.

**Why it happens**
Linux checks for a cycle of waiting processes when a lock request would block. If it finds one, it refuses that request with the error `EDEADLK` instead of letting everyone wait forever.

**Why I still use a rule**
The kernel only catches some cases, and it only reports an error after the cycle already formed. The code would then have to back out and retry. The real fix is the **lower id first** rule: if everyone locks in the same order, a cycle cannot form. `ordered 0 1` with `ordered 1 0` showed it. Both finished, the second one just waited for record 0.

**One line for the viva**
> Linux can sometimes detect a deadlock and return `EDEADLK`, but we prevent it by always locking the lower account id first.

---

### Other things learned on Day 2

- **Advisory locks:** the kernel does not stop a process that ignores the locks. They only work if every code path asks for them. That is why only the storage layer touches the data files.
- **Shared and exclusive:** `F_RDLCK` allows many readers, `F_WRLCK` allows one writer. Use read locks for things like viewing a balance, write locks when changing a record.
- **`F_SETLKW`:** set the lock, and Wait if someone else holds it. The sleeping process uses no CPU. I retry the call if a signal interrupts the wait (`EINTR`).
- **Locks vanish when the process exits:** good for crash safety. Classic locks belong to the process, not the thread, which is why Day 3 uses OFD locks.
- **Test tool versus product:** `race` is one program with several modes chosen by a word after it (`./race lockloop`). Each experiment is reset (`init`), run with two copies at once (`&` and `wait`), then checked (`show`). The final product is a server and a menu client.
- **Reading and writing do not lock on their own:** the caller wraps them with `storage_lock` and `storage_unlock`. I chose this for flexibility, and the cost is that I must not forget to unlock.
