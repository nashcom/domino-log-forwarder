# SimpleWAL - a write ahead log for C++ programs

`SimpleWAL` keeps records of any bytes safe on disk until a program has delivered them. A program appends a record, tries to send it, and if the receiver is down or the program stops, the record is still there and is delivered later. Records are delivered in the order they were written, at least once, and none is lost, also not after a crash.

It was written for `otelfwd`, which keeps the OTLP push requests which could not be delivered. It does not know what a record is, so any program which has data to deliver can use it. It is safe to use from any number of threads.

| File                | What it is                                                                                   |
| :------------------ | :------------------------------------------------------------------------------------------- |
| `simple_wal.hpp`    | The class `SimpleWAL`. The header describes the rules in full                                |
| `simple_wal.cpp`    | The implementation. It needs the C++ standard library and POSIX (Linux), and nothing else    |
| `wal_sample.cpp`    | A complete sample program, see [Quick start](#quick-start)                                   |
| `wal_unit_test.cpp` | The tests of the WAL, see [The tests](#the-tests)                                            |
| `makefile`          | Builds the sample and the tests. Also the shortest description of what another program needs |

## Quick start

You need `g++` and `make`. In this folder:

```bash
make && ./wal_sample
```

The sample plays a small forwarder. It shows records which wait while the receiver is down, their delivery after a restart, and a loop of its own with `Peek()` and `Ack()` while another thread appends. It ends with `The sample worked as described` and exit code 0.

To use the WAL in another program, copy the two files `simple_wal.hpp` and `simple_wal.cpp` (or this folder), and compile them with the program. A C++17 compiler and `-pthread` are all it needs:

```bash
g++ -std=c++17 -pthread -o myprogram myprogram.cpp simple_wal.cpp
```

## Using the WAL

```cpp
#include "simple_wal.hpp"

SimpleWAL Wal;

Wal.SetLogFunction ([] (const char *pszMessage) { MyLog (pszMessage); });   // optional, see below
Wal.SetMaxSize (256 * 1024 * 1024);                                          // optional: Append() says false if the file would get larger

if (false == Wal.Init ("/local/notesdata/app.wal"))                          // false: cannot be opened, or is in use
    return;

Wal.Append (pData, Len);                                                     // from any thread. false: the record was not stored

// The records are taken out in one of two ways. Either all of them, through a function:
Wal.Replay ([] (const std::vector<uint8_t>& Record)                          // in the order they were written
{
    return Send (Record);                                                    // true: accepted, false: stop, try again later
});

// Or one record at a time, in a loop of your own which must not wait:
std::vector<uint8_t> Record;

if (Wal.Peek (Record))                                                       // false: there is nothing to read
{
    if (Send (Record))
        Wal.Ack();                                                           // now it is done. Without an ack it is read again
}
```

The records are taken out in one of two ways:

* **`Replay()`** hands every record to a function of the program, until the function says stop. The way if the program has one place which sends.
* **`Peek()` and `Ack()`** read one record at a time, without a function and without waiting. The way for a program with a loop of its own, which must not block. The record stays in the WAL until it is acknowledged.

## The rules

| Rule          | What it means                                                                                                                                                                                                                                                                                                                                 |
| :------------ | :-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Threads       | `Init()` is called before the object is shared, and the object is destroyed after the threads are done. Every other function can be called from any thread at any time. One mutex protects everything, the settings too                                                                                                                       |
| No lock held  | The function of `Replay()` and the log function are never called with the mutex held. They can call the WAL. A slow receiver in the function does not stop `Append()`                                                                                                                                                                         |
| One consumer  | Only one thread takes records out at a time. A second `Replay()` returns `false` at once, and so does `Peek()` while a replay runs. Records which are appended during a replay are part of it and are not lost when it ends: the WAL is only emptied if nothing was appended                                                                  |
| Peek and Ack  | `Peek()` reads the oldest record, and it stays in the WAL: without `Ack()` the next `Peek()` returns the same one. `Ack()` saves the position, one small write of a file for every record, and empties the WAL after the last one. If the WAL was emptied or a replay moved the position in between, `Ack()` returns `false` and does nothing |
| Delivery      | At least once. After a crash, or if the position could not be saved, a record can be delivered again. None is lost. `Peek()` and `Replay()` repair a damaged WAL in the same way                                                                                                                                                              |
| Size limit    | `SetMaxSize (Bytes)`: a record which would make the file larger is refused as a whole, and `Append()` returns `false`. One warning until a record was stored again. The file only gets smaller when the WAL is emptied. `GetSize()` is the size. 0 is no limit, the default                                                                   |
| Files         | Only accessible for the owner (0600), and not inherited by programs which are started. One object can use a file: a second one, in this or another process, cannot open it (`flock`). The numbers in the files are in the byte order of the machine                                                                                           |
| Sync          | `SetSync (true)` calls `fsync` after every write. Off by default                                                                                                                                                                                                                                                                              |
| Messages      | Errors and warnings are one line of text with the level, for example `[Error] WAL: unreadable data at offset 8 (6 bytes) moved to ....corrupt`. With `SetLogLevel (1)` information too. No new line at the end                                                                                                                                |
| Where they go | With a function set by `SetLogFunction()` (it gets one string): to the function. Without one: `printf` to stdout, by default. `SetLogTarget (SimpleWAL::LOG_STDERR)` changes it for one object, `SimpleWAL::SetDefaultLogTarget()` for all objects which have no target of their own. `LOG_NONE` is silent                                    |

## The files

For a WAL with the path `/local/notesdata/app.wal` the WAL uses these files:

| File                               | Content                                                                                                                                               |
| :--------------------------------- | :---------------------------------------------------------------------------------------------------------------------------------------------------- |
| `/local/notesdata/app.wal`         | The records. A record is a length of 4 bytes and the data. A new record is added at the end                                                           |
| `/local/notesdata/app.wal.commit`  | The position of the first record which was not accepted yet, 8 bytes. It does not exist while nothing was accepted                                    |
| `/local/notesdata/app.wal.corrupt` | Data which cannot be read: a record which was cut off by a crash, or a damaged length. It is moved here, and the WAL continues with what is before it |

When every record was accepted, the WAL is emptied and the commit file is removed. A WAL which is empty when the object is destroyed leaves no files behind.

## Notes and limits

* **Linux only.** The WAL uses POSIX calls (`pread`, `flock` and others). A version for Windows would need a small layer for these.
* **One process, one object per file.** The `flock` keeps a second object out. It is not meant to be shared between processes.
* **The file only gets smaller when the WAL is emptied.** Records which were accepted stay in the file until every record is accepted (or `Clear()`). With a consumer which is slower than the producer, the file grows up to the size limit (`SetMaxSize`). Without a limit it grows until the disk is full.
* **`Ack()` is one small write of a file for every record.** For many records per second use `Replay()`, which saves the position once at the end.
* **The numbers in the files are in the byte order of the machine.** The files are not meant to be moved to another kind of machine.
* **A record is at most 4 GB** (a length of 4 bytes), and it is read completely into memory. A record is never larger than the WAL file.

## The tests

`wal_unit_test.cpp` is a separate program which only links the WAL (`simple_wal.cpp`). It needs no `otelfwd`, no libcurl, no Docker and no network. It works in a directory of its own, which it removes at the end, and takes a few seconds.

```bash
make test
```

`make test` compiles the test when a source changed, builds the sample, and runs the test. `make` ends with an error if a check failed. From the repository root, `make test` runs this test and the unit test of `otelfwd`. This test can also be started directly:

```bash
./wal_unit_test
```

| Option      | Description                                                                                                                                                                          |
| :---------- | :----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `-n COUNT`  | Number of records of the producer and consumer test and of the performance table. Default: 100000                                                                                    |
| `DIRECTORY` | The parent directory of the test files. Default: `/tmp`, which can be a RAM file system. The test makes a directory of its own in it and removes it. See [Performance](#performance) |

```bash
./wal_unit_test -n 1000000
```

### Reading the output

The output has headed sections. Every check is one line with its status:

```text
--------------------------------------------------------------------------------
Behaviour
--------------------------------------------------------------------------------

[PASS]  round trip: init
[PASS]  round trip: a new WAL has nothing pending
...
--------------------------------------------------------------------------------
Result
--------------------------------------------------------------------------------

[PASS]  247 of 247 checks passed, 0 failed
```

* `[PASS]` and `[FAIL]` mark every check. If something failed, a section **Failed checks** lists only those lines, before the section **Result**.
* The last section, **Result**, is `[PASS]` only if every check passed. The exit code of the program is 0 then, and 1 otherwise.
* **The test is quiet.** Many tests cause messages of the WAL on purpose (a record which was cut off, a damaged commit file, a full disk). They are not written between the checks, where they would look like failures. The messages are checked in the section **Messages of the WAL**, with a function which collects them. If a `[Error]` or `[Warning]` line is in the output, something is wrong.
* **`make tsan`** (also from the repository root) runs the same test with ThreadSanitizer. It finds data races between threads which no check can see. It is not part of `make test`, because it needs g++ with the sanitizer library (`libtsan`). The exit code is not 0 if it reports a race.

### What it tests

| Section                     | Names of the checks start with                                                                                                                                                  | What it covers                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                               |
| :-------------------------- | :------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ | :----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Threads                     | `[thread]`                                                                                                                                                                      | One WAL used by several threads. The function of a replay can call the WAL (no deadlock). An append does not wait for a slow replay, and a record which is appended during a replay is delivered once and not lost. Settings change while others append and log. Two replaying threads: only one function is active at a time                                                                                                                                                                                                                                |
| Messages of the WAL         | `messages:`, `targets:`, `lifecycle:`                                                                                                                                           | The messages of the WAL: one string with the level for a function of the application, no new line, information only with a log level above 0. Without a function: stdout by default, stderr or nothing with `SetLogTarget`, and `SetDefaultLogTarget` for all objects. The function can call the WAL. A second object cannot open a file which is in use                                                                                                                                                                                                     |
| Behaviour                   | the name of the test                                                                                                                                                            | Order of the records, partial replay and resuming it, no progress, empty records and a WAL which was not opened, restart, the destructor, `Clear()`, a 5 MB record, 10,000 records, four threads appending at once, one thread appending while another replays                                                                                                                                                                                                                                                                                               |
| Peek and Ack                | `peek:`, `peek restart:`, `peek append:`, `peek clear:`, `peek replay:`, `peek damaged:`, `[thread] peek:`                                                                      | Reading one record at a time without a function. The same record until it is acknowledged. What is acknowledged is not delivered again after a restart, what was read without an ack is. A record which is appended meanwhile is not lost and keeps the WAL from being emptied. `Clear()` or a `Replay()` between the read and the ack make the ack do nothing. No read during a replay. The same repairs as in a replay (a record which was cut off, a commit position behind the end). Several producers with one consumer: every record arrives, in order |
| Size limit                  | `size limit:`                                                                                                                                                                   | `SetMaxSize`: exactly as many records fit as the limit allows, also at the exact boundary. A record which does not fit is refused as a whole and nothing of it is left in the file. Any number of refused records make one warning, and again after the WAL was full again. The stored records are delivered. 0 is no limit                                                                                                                                                                                                                                  |
| Damaged files               | `commit at the end:`, `stale commit:`, `torn tail:`, `torn header:`, `damaged length:`, `misaligned commit offset`, `zero length header:`, `short commit file:`, `reopen after` | The replay must not get stuck: a commit position at the end, behind the end or inside a record, a record which was cut off by a crash, a partial header, a damaged length, a zero length, a commit file which is short, empty or too long. Nothing valid is thrown away. The repaired state survives a restart                                                                                                                                                                                                                                               |
| Failures while saving       | `append failure:`, `commit write failure:`, `truncate failure:`, `commit removal failure:`, `quarantine failure:`                                                               | A full disk in the middle of a record leaves nothing behind. The commit position cannot be written, the WAL cannot be truncated, the commit file cannot be removed, the unreadable part cannot be moved: the WAL never reports progress which is not saved, and no record disappears                                                                                                                                                                                                                                                                         |
| Files, settings and crashes | `file modes:`, `old file modes:`, `close on exec:`, `members:`, `sync option:`, `crash after append:`, `crash inside the replay:`                                               | Files are only accessible for their owner, also old ones. They are closed when another program is started (checked on the open descriptors, also during a replay). A new WAL is silent. The sync option. Real crashes: a child process ends without any cleanup                                                                                                                                                                                                                                                                                              |
| Performance                 | `performance:`                                                                                                                                                                  | Speed of appending, replaying and starting with a backlog, with a table and very low limits ([below](#performance))                                                                                                                                                                                                                                                                                                                                                                                                                                          |

The tests for failures use these techniques. None of them needs code for tests in the WAL:

* **A full disk** is simulated with the size limit of a file (`setrlimit`), which also works as root.
* **A directory with the name of the file** makes it impossible to create the `.corrupt` file.
* **Fault injection:** the makefile links the test with `-Wl,--wrap=ftruncate -Wl,--wrap=unlink`. The test then makes these two functions fail on request, and the calls of the WAL go through them. If the test is compiled by hand without these options, the checks for a failing `ftruncate` or `unlink` fail, because the failure is never injected.
* **Crashes are real:** a child process (`fork`) ends with `_exit()`, which runs no destructor. In one test it ends after appending, and the parent must find the records. In another it ends inside the replay, after a record was delivered and before the position was saved. The parent must get all records again.

### Performance

The last section prints a table:

```text
File system: tmpfs (memory, not a disk: fsync does nothing, speeds are much higher than on a disk)

Measurement                                            Records  Time (ms)    Records/s      MB/s
append, 100 byte records, one thread                    100000       61.5      1625380     169.0
replay everything, 100 byte records                     100000       43.5      2301332     239.3
append, 64 KB records (a push request)                    1000       13.1        76113    4988.5
replay everything, 64 KB records                          1000       14.3        69830    4576.6
start with a backlog: check of the commit position       50000       10.2      4888202     508.4
append with fsync, 100 byte records (no limit)            2000        1.3      1518900     158.0
```

* **The file system decides.** The line above the table names it. On a RAM file system (`tmpfs`) `fsync` does nothing and everything is much faster than on a disk. `/tmp` can be one. To measure the disk where the WAL of `otelfwd` lives, give its directory. The test makes its own subdirectory in it and removes it:

```bash
./wal_unit_test /local/notesdata
```

* **The numbers depend on the machine and its load.** They are for comparing two versions of the WAL on the same machine and the same file system, not absolute values. Run it a few times, and not while another test runs.
* **Start with a backlog** is the check which `Replay` does once when a program starts: it follows the records up to the commit position to make sure that it is the start of a record. The time grows with the number of records before the position.
* **The checks have very low limits** on purpose (20,000 records/s for small records, 200 records/s for 64 KB records, 50,000 records/s for the check at start). They only fail when something is dramatically wrong. The `fsync` row has no limit, because it depends on the disk.

### Adding a test

Write a function in `wal_unit_test.cpp` and call it from `main` in the fitting section. Use `Check (condition, "name")`, one call for every statement which must be true. The name is what appears in the output, so it should say what is expected. Look at the files on disk (`FileSize`, `FileExists`, `FileMode`) and do not trust the WAL to report about itself. For a bug, write the test first and see it fail, then fix the WAL and see it pass.

## In otelfwd

`otelfwd` sets a function which writes the messages of the WAL as lines of its console output, with the time and the process name (see [Log at start](../README.md#log-at-start)). The WAL of `otelfwd` is described in [Durable Log Delivery](../README.md#durable-log-delivery).
