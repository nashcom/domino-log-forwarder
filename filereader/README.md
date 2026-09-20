# FileReader

A small C++ class which follows a text file that grows, like `tail -F`, and hands out its lines one at a time. Unlike `tail` it works with **lines**, not bytes, and it **remembers how far the lines were delivered**, so a restart continues where the last run stopped.

It is meant for programs which forward the lines of a log file to somewhere else (a receiver, a queue, a WAL). It has no other dependency than the C++ standard library and POSIX (Linux). No threads, no libcurl. Copy this folder, or the two files `file_reader.hpp` and `file_reader.cpp`.

`otelfwd` uses it for its [file input](../README.md#file-input). It is a module of its own, and it can be tested on its own.

## Use

```cpp
#include "file_reader.hpp"

FileReader Reader;

Reader.Open ("/var/log/app.log", "/var/lib/myprog/app.log.state");     // the file does not have to exist yet

for (;;)
{
    FileReader::Line Line;
    uint64_t         Generation = 0, EndOffset = 0;
    int              Count = 0;

    while (Reader.ReadLine (Line, time (NULL)))         // never waits
    {
        Send (Line.Text);                                // or collect a batch
        Generation = Line.Generation;
        EndOffset  = Line.EndOffset;
        Count++;
    }

    if (Count > 0 && DeliveryWasAccepted())
        Reader.Commit (Generation, EndOffset);          // only now the position is saved

    sleep_for (250ms);
}
```

`ReadLine()` returns `false` when there is no complete line now, and the program asks again later. A polling loop with a short sleep is all it needs.

## The rules

| Rule | What it means |
| :--- | :------------ |
| Only complete lines | A line is returned when its new line has arrived. What the writer has written of an unfinished line stays in a buffer. The read is done with `pread()` into the reader's own buffer, so half a line is never handed out, and the position is a plain count of the bytes which were consumed as complete lines |
| At least once | The position is saved by `Commit()`, not when a line is read. After a crash or restart the lines which were read and not committed are read again. No line is lost |
| One commit per batch | The state file is written and flushed to disk for every `Commit()` which moves the position. Commit once for a batch of lines, after the receiver accepted them or they are safe in a WAL |
| Line ending | A line is returned without `\n`, and without a `\r` before it |
| Missing file | The file does not have to exist. The reader waits for it and reads it from the beginning when it appears. A file which is not there is no message |

## What happens to the file

| The file | The reader |
| :------- | :--------- |
| Grows | Reads what is new |
| Is rotated (renamed, and a new file is created, as logrotate does without `copytruncate`) | Reads the old file to its end, including a last line without a new line, then the new file from the beginning. It waits one call of `ReadLine()` before it lets go of the old file, for a writer which is still busy with it. Lines which a program writes to the old file after that are lost |
| Is truncated (`copytruncate`, or emptied) | Starts again at the beginning. A line which was not finished is dropped. A message is written |
| Is written again with the same first bytes and a larger size | Cannot be told from a file which only grew. That is the limit of every reader which does not own the file |
| Is deleted | Keeps reading what is left of it, and waits for a new file |

Every file gets a **generation** number, and so does a restart from the beginning. A `Commit()` for an older generation is ignored: the state file already belongs to the new file.

## The state file

The state file is a few lines of text. It holds the identity of the file (device and inode), the committed position, and a **fingerprint**: the first bytes of the file (up to 256), hashed. It is written to a file of its own, flushed to disk, and renamed, so a reader never sees a half written state file. Its mode is 0600.

At the start the reader continues at the saved position only if the file is still the same: same inode, at least as long as the position, and the same start. Otherwise the file is read from the beginning, and a message says why:

| Situation | What the reader does |
| :-------- | :------------------- |
| No state file (first start) | Starts at the beginning, or at the end with `SetStartAtEnd (true)`. The position is saved at once, so a restart before the first commit does not lose the lines in between |
| The file was replaced while the reader was stopped (another inode) | Reads the new file from the beginning. The end of the old file, which is now `app.log.1`, cannot be found again, and can be missed |
| The file is shorter than the position | Reads it from the beginning |
| The start of the file has changed (the inode was reused, or the file was emptied and written again) | Reads it from the beginning |
| The state file is damaged, incomplete, of another version, or cannot be read | A message, and it starts as without a state file |

## Settings

| Function | Meaning |
| :------- | :------ |
| `SetMaxLineLength (Bytes)` | The longest line which is returned as a whole. 1 MB by default. A longer line is returned cut to that length, with `bTruncated` set, and the rest of it is dropped up to the next new line. The `EndOffset` of a line which was cut while it was still being written stops right behind the part which was returned. A restart at that position would return the rest as a line |
| `SetStartAtEnd (bool)` | Where to start without a state file: the beginning (default) or the end |
| `SetIncompleteLineSeconds (Seconds)` | A last line without a new line which did not grow for that many seconds is returned as it is. The rest of it, when it comes, is a line of its own. 0 (default) waits for the new line. The time comes from the parameter of `ReadLine()`, so the program decides what the time is |
| `SetLogFunction (Function)` | Messages go to the function (`void (const char *)`). By default they are written to stdout |

## Messages

One line each, with the level: `[Warning] FileReader: /var/log/app.log was truncated. Reading it again from the start`. A message for an error which repeats (the state file cannot be written) is given once, and again after it went away.

## What it does not do

* **Several files, and patterns like `*.log`.** One reader is one file. A program which follows many files uses one reader for each.
* **Lines which belong together** (a stack trace over several lines). Every line is a line. That is a matter for the program which uses the reader.
* **Windows.** The identity of a file is the device and the inode, and the reader uses POSIX calls.
* **A file which is on a file system without stable inodes** (some network file systems).
* **Threads.** One object is for one thread: the thread which reads is the thread which commits.

## Files

| File | Content |
| :--- | :------ |
| `file_reader.hpp` | The class, with the description of the rules |
| `file_reader.cpp` | The implementation |
| `file_reader_unit_test.cpp` | The unit test. Real files in a private temporary directory: it writes, appends, renames, truncates and deletes them like a program which writes a log. It does not wait: time is a parameter |
| `makefile` | `make test` builds and runs the unit test |

## Test

```bash
make test
```

The test covers complete and incomplete lines (also written one byte at a time), line endings and binary data, a file which does not exist yet, commit and restart (also with a line which was not finished at the restart), where to start, rotation (also lines written late, and 20000 lines which were not read yet), truncation (also to a larger size, and of a line which was not finished), a file which was changed while the reader was stopped, ten kinds of damaged state file, long lines (at the boundary, still being written, and the default of 1 MB), the time limit for an incomplete line, the state file (mode, no temporary file left, a place which cannot be written), 200000 lines written in odd pieces, and that no file is left open.
