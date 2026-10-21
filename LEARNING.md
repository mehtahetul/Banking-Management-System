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
