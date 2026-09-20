
/* Unit test of the FileReader module (file_reader.cpp). A separate program which only links the reader: no otelfwd, no network.

   Build and run:  cd filereader && make test        (or: make file_reader_unit_test && ./file_reader_unit_test. From the repository root: make test)
   Options:        DIRECTORY     the parent directory of the test files (default /tmp). The test makes a directory of its own in it and
                                 removes it at the end

   The files are real files in a private temporary directory: the tests write, append, rename, truncate and delete them like a program
   which writes a log does. Time is a parameter of the reader, so no test waits. Every check prints [PASS] or [FAIL]. The end of
   the output has a section "Failed checks" (only if there are any) and a section "Result" with the overall status. The exit code
   is 0 if every check passed and 1 if any check failed.

   Groups of tests, in the order of the output:

   - Lines:             complete lines, a line which is not finished, line endings, binary data, a file which grows, a file which is not there
   - Commit and restart: the position is saved with Commit() and only then, a restart continues there, lines which were not committed
                        are read again, a line which was not finished is read whole
   - Where to start:    the beginning, the end, and that a state file wins
   - Rotation:          the old file is read to its end, also what is written late, then the new file. Old generations are ignored
   - Truncation:        the file is emptied and written again, also to a larger size, a line which was not finished is dropped
   - Changed while stopped: the file was replaced, emptied or written again while the reader was not running
   - Damaged state file: every kind of damage is reported, and the reader starts as without a state file
   - Long lines:        cut at the maximum, the rest dropped, the boundary, the default of 1 MB
   - Incomplete last line: the time limit
   - The state file:    mode, atomic write, a place which cannot be written
   - Larger data:       many lines written in pieces which cut the lines anywhere. Files which are not leaked

   The name of a check starts with what it tests, for example "rotation:" or "truncate:" */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <sys/stat.h>

#include <string>
#include <vector>

#include "file_reader.hpp"


static int g_Total  = 0;
static int g_Failed = 0;
static std::string g_Dir;
static std::vector<std::string> g_FailedNames;
static std::vector<std::string> g_Messages;

static const time_t NOW = 1000;


static void Check (bool bCondition, const char *pszName)
{
    printf ("[%s]  %s\n", bCondition ? "PASS" : "FAIL", pszName);

    g_Total++;

    if (false == bCondition)
    {
        g_Failed++;
        g_FailedNames.push_back (pszName);
    }
}


static void Group (const char *pszTitle)
{
    std::string Line (80, '-');

    printf ("\n%s\n%s\n%s\n\n", Line.c_str(), pszTitle, Line.c_str());
}


/* --- Helpers. The tests do not trust the reader to tell about itself: they look at the files --- */

static std::string Path (const char *pszName)
{
    return g_Dir + "/" + pszName;
}


static void AppendFile (const std::string& File, const std::string& Data)
{
    int fd = ::open (File.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);

    if (fd < 0)
    {
        perror ("AppendFile");
        exit (2);
    }

    if (static_cast<size_t> (::write (fd, Data.data(), Data.size())) != Data.size())
    {
        perror ("AppendFile write");
        exit (2);
    }

    ::close (fd);
}


/* The file has this content and nothing else. It is emptied, not replaced: the inode stays (as with copytruncate) */
static void RewriteFile (const std::string& File, const std::string& Data)
{
    if (0 != ::truncate (File.c_str(), 0))
    {
        int fd = ::open (File.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);

        if (fd >= 0)
            ::close (fd);
    }

    AppendFile (File, Data);
}


/* Another file at the same path. The new file is made first and renamed over the old one, so that it has an inode of its own: a file
   system reuses the inode of a file which was just deleted */
static void ReplaceFile (const std::string& File, const std::string& Data)
{
    std::string Temp = File + ".new";

    ::unlink (Temp.c_str());
    AppendFile (Temp, Data);

    if (0 != ::rename (Temp.c_str(), File.c_str()))
    {
        perror ("ReplaceFile rename");
        exit (2);
    }
}


static std::string ReadWholeFile (const std::string& File)
{
    std::string Result;
    char        szBuffer[4096];
    int         fd = ::open (File.c_str(), O_RDONLY);

    if (fd < 0)
        return "";

    for (;;)
    {
        ssize_t Read = ::read (fd, szBuffer, sizeof (szBuffer));

        if (Read <= 0)
            break;

        Result.append (szBuffer, static_cast<size_t> (Read));
    }

    ::close (fd);
    return Result;
}


static long FileSize (const std::string& File)
{
    struct stat StatBuf;

    if (0 != ::stat (File.c_str(), &StatBuf))
        return -1;

    return static_cast<long> (StatBuf.st_size);
}


static unsigned long long InodeOf (const std::string& File)
{
    struct stat StatBuf;

    if (0 != ::stat (File.c_str(), &StatBuf))
        return 0;

    return static_cast<unsigned long long> (StatBuf.st_ino);
}


/* The value of a name=value line of the state file, "" if there is none */
static std::string StateValue (const std::string& StateFile, const char *pszName)
{
    std::string Text   = ReadWholeFile (StateFile);
    std::string Prefix = std::string (pszName) + "=";
    size_t      Pos    = 0;

    while (Pos < Text.size())
    {
        size_t End = Text.find ('\n', Pos);

        if (std::string::npos == End)
            End = Text.size();

        if (0 == Text.compare (Pos, Prefix.size(), Prefix))
            return Text.substr (Pos + Prefix.size(), End - Pos - Prefix.size());

        Pos = End + 1;
    }

    return "";
}


static void OnMessage (const char *pszMessage)
{
    g_Messages.push_back (pszMessage);
}


static void NewReader (FileReader& Reader)
{
    Reader.SetLogFunction (OnMessage);
}


static size_t CountMessages (const char *pszPart = "")
{
    size_t Count = 0;

    for (const std::string& Message : g_Messages)
    {
        if (std::string::npos != Message.find (pszPart))
            Count++;
    }

    return Count;
}


/* Reads until there is no line. Some tests call it more than once: the reader waits one call before it leaves a file which was replaced */
static void ReadAll (FileReader& Reader, std::vector<FileReader::Line>& Lines, time_t tNow = NOW)
{
    FileReader::Line Line;

    while (Reader.ReadLine (Line, tNow))
        Lines.push_back (Line);
}


static std::string Join (const std::vector<FileReader::Line>& Lines)
{
    std::string Result;

    for (size_t i = 0; i < Lines.size(); i++)
    {
        if (i > 0)
            Result += "|";

        Result += Lines[i].Text;
    }

    return Result;
}


static std::string ReadJoined (FileReader& Reader, int Rounds = 1)
{
    std::vector<FileReader::Line> Lines;

    for (int i = 0; i < Rounds; i++)
        ReadAll (Reader, Lines);

    return Join (Lines);
}


/* A fresh file name and state file name for a test */
struct Files
{
    std::string File;
    std::string State;

    explicit Files (const char *pszName)
    {
        File  = Path (pszName);
        State = File + ".state";

        ::unlink (File.c_str());
        ::unlink (State.c_str());
    }
};


static size_t CountOpenFiles()
{
    size_t Count = 0;
    DIR   *pDir  = ::opendir ("/proc/self/fd");

    if (NULL == pDir)
        return 0;

    while (struct dirent *pEntry = ::readdir (pDir))
    {
        if ('.' != pEntry->d_name[0])
            Count++;
    }

    ::closedir (pDir);
    return Count;
}


/* --- Lines --- */

static void TestBasicLines()
{
    Files F ("basic.log");
    FileReader Reader;
    std::vector<FileReader::Line> Lines;

    NewReader (Reader);
    g_Messages.clear();

    AppendFile (F.File, "one\ntwo\nthree\n");

    Check (Reader.Open (F.File, F.State), "lines: Open accepts the paths");
    ReadAll (Reader, Lines);

    Check ("one|two|three" == Join (Lines), "lines: the complete lines come in the order of the file");
    Check ( (3 == Lines.size()) && (4 == Lines[0].EndOffset) && (8 == Lines[1].EndOffset) && (14 == Lines[2].EndOffset), "lines: the EndOffset is the position behind the line, with its new line");
    Check ( (3 == Lines.size()) && (1 == Lines[0].Generation) && (1 == Lines[2].Generation), "lines: the first file is generation 1");
    Check ( (3 == Lines.size()) && (false == Lines[0].bTruncated), "lines: a normal line is not marked as cut");
    Check (true == Reader.IsFileOpen(), "lines: the file is open");

    FileReader::Line Line;

    Check (false == Reader.ReadLine (Line, NOW), "lines: at the end of the file there is no line, and no waiting");
    Check (0 == g_Messages.size(), "lines: no message for an ordinary file");

    FileReader Empty;

    Check (false == Empty.Open ("", F.State), "lines: Open refuses an empty path");
    Check (false == Empty.Open (F.File, ""), "lines: Open refuses an empty state path");
    Check (false == Empty.ReadLine (Line, NOW), "lines: a reader which was not opened has no line");
}


static void TestIncompleteLine()
{
    Files F ("incomplete.log");
    FileReader Reader;
    std::vector<FileReader::Line> Lines;

    NewReader (Reader);
    Reader.Open (F.File, F.State);

    AppendFile (F.File, "abc");
    ReadAll (Reader, Lines);
    Check (0 == Lines.size(), "incomplete line: a line without its new line is not returned");

    AppendFile (F.File, "def");
    ReadAll (Reader, Lines);
    Check (0 == Lines.size(), "incomplete line: still not, when more of it was written");

    AppendFile (F.File, "\nnext\n");
    ReadAll (Reader, Lines);
    Check ("abcdef|next" == Join (Lines), "incomplete line: the line is returned whole when its new line arrives");
    Check ( (2 == Lines.size()) && (7 == Lines[0].EndOffset) && (12 == Lines[1].EndOffset), "incomplete line: the EndOffset counts the bytes of all the pieces");

    /* A line which is written one byte at a time */
    Files G ("bytes.log");
    FileReader Slow;
    std::vector<FileReader::Line> Result;
    std::string Text = "written byte by byte\n";
    size_t Returned = 0;

    NewReader (Slow);
    Slow.Open (G.File, G.State);

    for (size_t i = 0; i < Text.size(); i++)
    {
        AppendFile (G.File, Text.substr (i, 1));
        ReadAll (Slow, Result);

        if (i + 1 < Text.size())
            Returned += Result.size();
    }

    Check ( (0 == Returned) && (1 == Result.size()) && ("written byte by byte" == Result[0].Text), "incomplete line: one byte at a time is one line, returned once, at the end");
}


static void TestLineEndings()
{
    Files F ("endings.log");
    FileReader Reader;
    std::vector<FileReader::Line> Lines;

    NewReader (Reader);

    AppendFile (F.File, "a\r\nb\n\n\r\nc\n");
    Reader.Open (F.File, F.State);
    ReadAll (Reader, Lines);

    Check (5 == Lines.size(), "line endings: an empty line is a line");
    Check ( (5 == Lines.size()) && ("a" == Lines[0].Text) && ("b" == Lines[1].Text) && ("" == Lines[2].Text) && ("" == Lines[3].Text) && ("c" == Lines[4].Text),
            "line endings: \\r\\n and \\n both end a line, the carriage return is not part of it");
    Check ( (5 == Lines.size()) && (3 == Lines[0].EndOffset) && (10 == Lines[4].EndOffset), "line endings: the EndOffset counts the carriage return");

    Files G ("binary.log");
    FileReader Binary;
    std::vector<FileReader::Line> Result;

    NewReader (Binary);
    AppendFile (G.File, std::string ("x\0y\n\xff\xfe\n", 7));
    Binary.Open (G.File, G.State);
    ReadAll (Binary, Result);

    Check ( (2 == Result.size()) && (std::string ("x\0y", 3) == Result[0].Text) && ("\xff\xfe" == Result[1].Text), "binary data: a zero byte and bytes above 127 are kept as they are");
}


static void TestGrowingAndMissingFile()
{
    Files F ("growing.log");
    FileReader Reader;
    std::vector<FileReader::Line> Lines;

    NewReader (Reader);
    g_Messages.clear();

    /* The file is not there */
    Reader.Open (F.File, F.State);
    ReadAll (Reader, Lines);
    ReadAll (Reader, Lines);
    Check ( (0 == Lines.size()) && (false == Reader.IsFileOpen()), "missing file: no line, and no file is open");
    Check (0 == g_Messages.size(), "missing file: it is no message: a log which is written later is normal");
    Check (0 != ::access (F.State.c_str(), F_OK), "missing file: no state file yet");

    AppendFile (F.File, "first\n");
    ReadAll (Reader, Lines);
    Check ("first" == Join (Lines), "missing file: the file is read from its beginning when it appears");
    Check (1 == Reader.GetGeneration(), "missing file: it is generation 1");

    AppendFile (F.File, "second\nthird\n");
    ReadAll (Reader, Lines);
    Check ("first|second|third" == Join (Lines), "growing file: what is appended later is read");

    /* The file is a directory */
    Files D ("dir.log");
    FileReader OnDir;
    FileReader::Line Line;

    NewReader (OnDir);
    g_Messages.clear();
    ::mkdir (D.File.c_str(), 0755);
    OnDir.Open (D.File, D.State);

    Check (false == OnDir.ReadLine (Line, NOW), "not a file: a directory has no lines");
    OnDir.ReadLine (Line, NOW);
    Check (1 == CountMessages ("not a regular file"), "not a file: one message, not one for every call");
    ::rmdir (D.File.c_str());
}


/* --- Commit and restart --- */

static void TestCommitAndRestart()
{
    Files F ("restart.log");
    std::vector<FileReader::Line> Lines;

    g_Messages.clear();
    AppendFile (F.File, "l1\nl2\nl3\nl4\nl5\n");

    {
        FileReader Reader;

        NewReader (Reader);
        Reader.Open (F.File, F.State);
        ReadAll (Reader, Lines);

        Check (5 == Lines.size(), "restart: the first run reads all five lines");
        Check ("0" == StateValue (F.State, "offset"), "restart: nothing is committed: the state file has the position 0");
        Check (Reader.Commit (Lines[1].Generation, Lines[1].EndOffset), "restart: Commit returns true");
        Check ("6" == StateValue (F.State, "offset"), "restart: the position is behind the committed line");
        Check (6 == Reader.GetCommitted(), "restart: GetCommitted is the position");
    }

    std::vector<FileReader::Line> Again;
    FileReader Second;

    NewReader (Second);
    Second.Open (F.File, F.State);
    ReadAll (Second, Again);

    Check ("l3|l4|l5" == Join (Again), "restart: the lines which were read and not committed are read again, and only those");
    Check ( (3 == Again.size()) && (9 == Again[0].EndOffset), "restart: the positions go on where they were");
    Check (0 == CountMessages(), "restart: the same file is no message");

    Second.Commit (Again[2].Generation, Again[2].EndOffset);

    std::vector<FileReader::Line> Third;
    FileReader Reader3;

    NewReader (Reader3);
    Reader3.Open (F.File, F.State);
    ReadAll (Reader3, Third);
    Check (0 == Third.size(), "restart: when everything was committed nothing is read again");

    AppendFile (F.File, "l6\n");
    ReadAll (Reader3, Third);
    Check ( ("l6" == Join (Third)) && (1 == Third.size()) && (18 == Third[0].EndOffset), "restart: a line which is written later is read");

    /* A line which was not finished at the time of the restart */
    Files G ("restart_partial.log");
    std::vector<FileReader::Line> First;

    AppendFile (G.File, "aaa\nbb");

    {
        FileReader Reader;

        NewReader (Reader);
        Reader.Open (G.File, G.State);
        ReadAll (Reader, First);
        Check ("aaa" == Join (First), "restart with a partial line: the complete line is read");
        Reader.Commit (First[0].Generation, First[0].EndOffset);
    }

    AppendFile (G.File, "b\ncc\n");

    std::vector<FileReader::Line> Rest;
    FileReader Reader2;

    NewReader (Reader2);
    Reader2.Open (G.File, G.State);
    ReadAll (Reader2, Rest);
    Check ("bbb|cc" == Join (Rest), "restart with a partial line: the line which was not finished is read whole, no piece of it is lost");
}


static void TestCommitRules()
{
    Files F ("commit.log");
    FileReader Reader;
    std::vector<FileReader::Line> Lines;

    NewReader (Reader);
    g_Messages.clear();

    AppendFile (F.File, "l1\nl2\nl3\n");
    Reader.Open (F.File, F.State);

    ReadAll (Reader, Lines);

    Check (false == Reader.Commit (1, 999), "commit rules: a position behind the lines which were read is refused");
    Check (1 == CountMessages ("behind the lines"), "commit rules: with a message");
    Check ("0" == StateValue (F.State, "offset"), "commit rules: and nothing is saved");

    Check (true == Reader.Commit (2, 6), "commit rules: another generation is ignored, and it is not an error");
    Check ("0" == StateValue (F.State, "offset"), "commit rules: and it saves nothing");

    Check (true == Reader.Commit (1, 6), "commit rules: a position which was read is saved");
    Check ("6" == StateValue (F.State, "offset"), "commit rules: the state file has it");

    Check (true == Reader.Commit (1, 3), "commit rules: an earlier position is ignored");
    Check ("6" == StateValue (F.State, "offset"), "commit rules: the position does not go back");

    Check (true == Reader.Commit (1, 6), "commit rules: the same position is ignored");
    Check (true == Reader.Commit (1, 9), "commit rules: the end");
    Check ("9" == StateValue (F.State, "offset"), "commit rules: the state file has the end");
    Check (StateValue (F.State, "inode") == std::to_string (InodeOf (F.File)), "commit rules: the state file has the inode of the file");
}


/* --- Where to start --- */

static void TestStart()
{
    /* The beginning is the default */
    Files A ("start_begin.log");
    FileReader Begin;

    NewReader (Begin);
    AppendFile (A.File, "a\nb\n");
    Begin.Open (A.File, A.State);
    Check ("a|b" == ReadJoined (Begin), "start: without a state file the file is read from the beginning");

    /* The end: only lines which are written later */
    Files B ("start_end.log");
    FileReader End;
    std::vector<FileReader::Line> Lines;

    NewReader (End);
    g_Messages.clear();
    AppendFile (B.File, "old1\nold2\n");
    End.SetStartAtEnd (true);
    End.Open (B.File, B.State);
    ReadAll (End, Lines);
    Check (0 == Lines.size(), "start at the end: the lines which are there are not read");
    Check ("10" == StateValue (B.State, "offset"), "start at the end: the position is saved at once");

    AppendFile (B.File, "new1\n");
    ReadAll (End, Lines);
    Check ("new1" == Join (Lines), "start at the end: what is written later is read");

    /* A restart before the first commit: the position of the state file is the old end, not the new one */
    AppendFile (B.File, "new2\n");

    FileReader Restarted;
    std::vector<FileReader::Line> After;

    NewReader (Restarted);
    Restarted.SetStartAtEnd (true);
    Restarted.Open (B.File, B.State);
    ReadAll (Restarted, After);
    Check ("new1|new2" == Join (After), "start at the end: a restart before the first commit does not lose the lines in between");
    Check (0 == CountMessages(), "start at the end: no message");

    /* Only without a state file. Here the state file wins */
    Files C ("start_state.log");
    FileReader First;
    std::vector<FileReader::Line> Read;

    NewReader (First);
    AppendFile (C.File, "x\ny\nz\n");
    First.Open (C.File, C.State);
    ReadAll (First, Read);
    First.Commit (Read[0].Generation, Read[0].EndOffset);

    FileReader Second;
    NewReader (Second);
    Second.SetStartAtEnd (true);
    Second.Open (C.File, C.State);
    Check ("y|z" == ReadJoined (Second), "start at the end: a state file wins");
}


/* --- Rotation --- */

static void TestRotation()
{
    Files F ("rotate.log");
    FileReader Reader;
    std::vector<FileReader::Line> Lines;

    NewReader (Reader);
    g_Messages.clear();

    AppendFile (F.File, "a1\na2\na3\n");
    Reader.Open (F.File, F.State);
    ReadAll (Reader, Lines);
    Reader.Commit (Lines[2].Generation, Lines[2].EndOffset);

    unsigned long long OldInode = InodeOf (F.File);

    /* logrotate: the file is renamed, a new one is created. Lines were written to the old file which were not read yet. The last one is
       not finished */
    AppendFile (F.File, "a4\na5");
    Check (0 == ::rename (F.File.c_str(), (F.File + ".1").c_str()), "rotation: (test) the file is renamed");
    AppendFile (F.File, "b1\nb2\n");
    Check (InodeOf (F.File) != OldInode, "rotation: (test) the new file has another inode");

    std::vector<FileReader::Line> Rest;

    ReadAll (Reader, Rest);
    Check ("a4" == Join (Rest), "rotation: the old file is read on to its end first");
    Check (1 == Reader.GetGeneration(), "rotation: the reader waits one call before it lets go of the old file");

    ReadAll (Reader, Rest);
    Check ("a4|a5|b1|b2" == Join (Rest), "rotation: then the last line of the old file, which had no new line, and the new file");
    Check ( (4 == Rest.size()) && (1 == Rest[0].Generation) && (1 == Rest[1].Generation) && (2 == Rest[2].Generation) && (2 == Rest[3].Generation), "rotation: the lines of the new file are generation 2");
    Check ( (4 == Rest.size()) && (3 == Rest[2].EndOffset) && (6 == Rest[3].EndOffset), "rotation: the positions of the new file start at 0");
    Check (StateValue (F.State, "inode") == std::to_string (InodeOf (F.File)), "rotation: the state file has the new file");
    Check ("0" == StateValue (F.State, "offset"), "rotation: at its beginning");

    Check (true == Reader.Commit (1, 12), "rotation: a Commit for the old file is ignored");
    Check ("0" == StateValue (F.State, "offset"), "rotation: it does not touch the state file of the new file");

    Reader.Commit (2, 6);

    FileReader Restarted;
    std::vector<FileReader::Line> After;

    NewReader (Restarted);
    Restarted.Open (F.File, F.State);
    AppendFile (F.File, "b3\n");
    ReadAll (Restarted, After);
    Check ("b3" == Join (After), "rotation: after a restart the reader continues in the new file");
    Check (0 == CountMessages(), "rotation: no message for a rotation, it is normal");
}


static void TestRotationLateWrites()
{
    Files F ("rotate_late.log");
    FileReader Reader;
    std::vector<FileReader::Line> Lines;

    NewReader (Reader);
    AppendFile (F.File, "a1\n");
    Reader.Open (F.File, F.State);
    ReadAll (Reader, Lines);

    ::rename (F.File.c_str(), (F.File + ".1").c_str());
    AppendFile (F.File, "b1\n");

    /* The reader has seen the new file. The program which writes the log is not finished with the old one */
    ReadAll (Reader, Lines);
    AppendFile (F.File + ".1", "late\n");
    ReadAll (Reader, Lines);
    ReadAll (Reader, Lines);

    Check ("a1|late|b1" == Join (Lines), "rotation: what is written to the old file in the wait is not lost");

    /* The old file is read to its end first, however many lines there are */
    Files G ("rotate_many.log");
    FileReader Many;
    std::vector<FileReader::Line> Result;
    std::string Text;

    NewReader (Many);
    AppendFile (G.File, "start\n");
    Many.Open (G.File, G.State);
    ReadAll (Many, Result);

    for (int i = 0; i < 20000; i++)
        Text += "old line number " + std::to_string (i) + "\n";

    AppendFile (G.File, Text);
    ::rename (G.File.c_str(), (G.File + ".1").c_str());
    AppendFile (G.File, "new\n");

    ReadAll (Many, Result);
    ReadAll (Many, Result);
    Check ( (20002 == Result.size()) && ("start" == Result[0].Text) && ("old line number 19999" == Result[20000].Text) && ("new" == Result[20001].Text),
            "rotation: 20000 lines of the old file which were not read yet come before the new file");
}


static void TestDeletedFile()
{
    Files F ("deleted.log");
    FileReader Reader;
    std::vector<FileReader::Line> Lines;

    NewReader (Reader);
    g_Messages.clear();
    AppendFile (F.File, "a\n");
    Reader.Open (F.File, F.State);
    ReadAll (Reader, Lines);

    ::unlink (F.File.c_str());
    ReadAll (Reader, Lines);
    ReadAll (Reader, Lines);
    Check ( (1 == Lines.size()) && (0 == g_Messages.size()), "deleted file: the reader waits for a new file, no message");

    AppendFile (F.File, "b\nc\n");
    ReadAll (Reader, Lines);
    ReadAll (Reader, Lines);
    Check ("a|b|c" == Join (Lines), "deleted file: the new file is read from its beginning");
    Check ( (3 == Lines.size()) && (2 == Lines[2].Generation), "deleted file: it is generation 2");
}


/* --- Truncation --- */

static void TestTruncate()
{
    Files F ("truncate.log");
    FileReader Reader;
    std::vector<FileReader::Line> Lines;

    NewReader (Reader);
    g_Messages.clear();
    AppendFile (F.File, "line one\nline two\nline three\n");
    Reader.Open (F.File, F.State);
    ReadAll (Reader, Lines);
    Reader.Commit (Lines[2].Generation, Lines[2].EndOffset);

    /* copytruncate: the same file is emptied and written again */
    RewriteFile (F.File, "n1\n");

    std::vector<FileReader::Line> After;

    ReadAll (Reader, After);
    Check ("n1" == Join (After), "truncate: the file is read again from the start");
    Check (1 == CountMessages ("was truncated"), "truncate: with one message");
    Check ( (1 == After.size()) && (2 == After[0].Generation) && (3 == After[0].EndOffset), "truncate: the Generation changes and the position starts at 0");
    Check ("0" == StateValue (F.State, "offset"), "truncate: the state file starts again");
    Check (true == Reader.Commit (1, 29), "truncate: a Commit for the lines before is ignored");
    Check ("0" == StateValue (F.State, "offset"), "truncate: and does not move the position");

    /* A line which was not finished is dropped */
    Files G ("truncate_partial.log");
    FileReader Partial;
    std::vector<FileReader::Line> Result;

    NewReader (Partial);
    AppendFile (G.File, "first\nabc");
    Partial.Open (G.File, G.State);
    ReadAll (Partial, Result);
    RewriteFile (G.File, "x\n");
    ReadAll (Partial, Result);
    Check ("first|x" == Join (Result), "truncate: a line which was not finished is dropped, it is not glued to the new content");

    /* Written again to a larger size: only the start of the file shows it */
    Files H ("truncate_larger.log");
    FileReader Larger;
    std::vector<FileReader::Line> Big;

    NewReader (Larger);
    g_Messages.clear();
    AppendFile (H.File, std::string (100, 'A') + "\n");
    Larger.Open (H.File, H.State);
    ReadAll (Larger, Big);
    RewriteFile (H.File, std::string (300, 'B') + "\n");
    ReadAll (Larger, Big);
    Check ( (2 == Big.size()) && (std::string (300, 'B') == Big[1].Text) && (2 == Big[1].Generation), "truncate: a file which was written again and is larger than before is found by its start");
    Check (1 == CountMessages ("start has changed"), "truncate: with a message");

    /* A file which is emptied and stays empty */
    Files I ("truncate_empty.log");
    FileReader Empty;
    std::vector<FileReader::Line> Small;

    NewReader (Empty);
    AppendFile (I.File, "a\nb\n");
    Empty.Open (I.File, I.State);
    ReadAll (Empty, Small);
    RewriteFile (I.File, "");
    ReadAll (Empty, Small);
    AppendFile (I.File, "c\n");
    ReadAll (Empty, Small);
    Check ("a|b|c" == Join (Small), "truncate: an emptied file which gets lines again is read from the start");
}


/* --- Changed while stopped --- */

static void TestChangedWhileStopped()
{
    Files F ("stopped.log");
    std::vector<FileReader::Line> Lines;

    g_Messages.clear();
    AppendFile (F.File, "first line of the file\nsecond line\nthird\n");

    {
        FileReader Reader;

        NewReader (Reader);
        Reader.Open (F.File, F.State);
        ReadAll (Reader, Lines);
        Reader.Commit (Lines[2].Generation, Lines[2].EndOffset);
    }

    /* The file is not the same inode: replaced while stopped */
    ReplaceFile (F.File, "new1\nnew2\n");

    {
        FileReader Reader;
        std::vector<FileReader::Line> Result;

        NewReader (Reader);
        Reader.Open (F.File, F.State);
        ReadAll (Reader, Result);
        Check ("new1|new2" == Join (Result), "changed while stopped: another file: it is read from the start");
        Check (1 == CountMessages ("another file"), "changed while stopped: another file: with a message");
    }

    /* The same inode, but shorter than the position */
    Files G ("stopped_short.log");
    std::vector<FileReader::Line> First;

    g_Messages.clear();
    AppendFile (G.File, "first line of the file\nsecond line\nthird\n");

    {
        FileReader Reader;

        NewReader (Reader);
        Reader.Open (G.File, G.State);
        ReadAll (Reader, First);
        Reader.Commit (First[2].Generation, First[2].EndOffset);
    }

    RewriteFile (G.File, "short\n");

    {
        FileReader Reader;
        std::vector<FileReader::Line> Result;

        NewReader (Reader);
        Reader.Open (G.File, G.State);
        ReadAll (Reader, Result);
        Check ("short" == Join (Result), "changed while stopped: shorter than the position: it is read from the start");
        Check (1 == CountMessages ("shorter than the saved position"), "changed while stopped: shorter: with a message");
    }

    /* The same inode, longer than the position, but other content at the start */
    Files H ("stopped_start.log");
    std::vector<FileReader::Line> Third;

    g_Messages.clear();
    AppendFile (H.File, "first line of the file\nsecond line\nthird\n");

    {
        FileReader Reader;

        NewReader (Reader);
        Reader.Open (H.File, H.State);
        ReadAll (Reader, Third);
        Reader.Commit (Third[2].Generation, Third[2].EndOffset);
    }

    RewriteFile (H.File, "another first line here\nanother second line\nanother third\nand more\n");

    {
        FileReader Reader;
        std::vector<FileReader::Line> Result;

        NewReader (Reader);
        Reader.Open (H.File, H.State);
        ReadAll (Reader, Result);
        Check ( (4 == Result.size()) && ("another first line here" == Result[0].Text), "changed while stopped: other start: it is read from the start");
        Check (1 == CountMessages ("start of"), "changed while stopped: other start: with a message");
    }

    /* Only appended to: the normal case is no message */
    Files J ("stopped_appended.log");
    std::vector<FileReader::Line> Fourth;

    g_Messages.clear();
    AppendFile (J.File, "a\nb\n");

    {
        FileReader Reader;

        NewReader (Reader);
        Reader.Open (J.File, J.State);
        ReadAll (Reader, Fourth);
        Reader.Commit (Fourth[1].Generation, Fourth[1].EndOffset);
    }

    AppendFile (J.File, "c\n");

    {
        FileReader Reader;

        NewReader (Reader);
        Reader.Open (J.File, J.State);
        Check ("c" == ReadJoined (Reader), "changed while stopped: a file which only grew is read on from the position");
        Check (0 == g_Messages.size(), "changed while stopped: and it is no message");
    }
}


/* --- Damaged state file --- */

static void TestDamagedState()
{
    struct Case
    {
        const char *pszName;
        std::string Content;
    };

    const std::vector<Case> Cases =
    {
        { "text",            "this is not a state file\n" },
        { "not a number",    "version=1\ndevice=1\ninode=1\noffset=abc\nheadlen=0\nheadhash=0\n" },
        { "negative",        "version=1\ndevice=1\ninode=1\noffset=-5\nheadlen=0\nheadhash=0\n" },
        { "empty value",     "version=1\ndevice=1\ninode=1\noffset=\nheadlen=0\nheadhash=0\n" },
        { "number too big",  "version=1\ndevice=1\ninode=1\noffset=99999999999999999999999\nheadlen=0\nheadhash=0\n" },
        { "other version",   "version=2\ndevice=1\ninode=1\noffset=0\nheadlen=0\nheadhash=0\n" },
        { "key missing",     "version=1\ndevice=1\ninode=1\nheadlen=0\nheadhash=0\n" },
        { "head too long",   "version=1\ndevice=1\ninode=1\noffset=10\nheadlen=999\nheadhash=0\n" },
        { "head behind offset", "version=1\ndevice=1\ninode=1\noffset=2\nheadlen=5\nheadhash=0\n" },
        { "too large",       std::string (5000, 'x') },
    };

    for (const Case& Test : Cases)
    {
        Files F ("damaged.log");
        FileReader Reader;
        std::string Name = std::string ("damaged state file (") + Test.pszName + "): ";

        NewReader (Reader);
        g_Messages.clear();
        AppendFile (F.File, "a\nb\n");
        AppendFile (F.State, Test.Content);

        Reader.Open (F.File, F.State);

        std::string Text = ReadJoined (Reader);

        Check ("a|b" == Text, (Name + "the file is read from the beginning").c_str());
        Check (1 == CountMessages ("ignored"), (Name + "one message").c_str());
        Check ("1" == StateValue (F.State, "version"), (Name + "the state file is written again").c_str());
    }

    /* An empty state file is the same as damage: it is what a crash leaves without the flush */
    Files E ("damaged_empty.log");
    FileReader Empty;

    NewReader (Empty);
    g_Messages.clear();
    AppendFile (E.File, "a\n");
    AppendFile (E.State, "");
    Empty.Open (E.File, E.State);
    Check ("a" == ReadJoined (Empty), "damaged state file (empty): the file is read from the beginning");
    Check (1 == CountMessages ("incomplete"), "damaged state file (empty): one message");

    /* The state file can not be read, as it is a directory */
    Files D ("damaged_dir.log");
    FileReader OnDir;

    NewReader (OnDir);
    g_Messages.clear();
    AppendFile (D.File, "a\n");
    ::mkdir (D.State.c_str(), 0755);
    OnDir.Open (D.File, D.State);
    Check ("a" == ReadJoined (OnDir), "state file which cannot be read: the file is read from the beginning");
    Check (CountMessages ("ignored") >= 1, "state file which cannot be read: a message");
    ::rmdir (D.State.c_str());

    /* Start at the end applies to a damaged state file too: there is no position */
    Files S ("damaged_end.log");
    FileReader AtEnd;

    NewReader (AtEnd);
    AppendFile (S.File, "old\n");
    AppendFile (S.State, "garbage\n");
    AtEnd.SetStartAtEnd (true);
    AtEnd.Open (S.File, S.State);
    Check ("" == ReadJoined (AtEnd), "damaged state file: with start at the end the lines which are there are not read");
    AppendFile (S.File, "new\n");
    Check ("new" == ReadJoined (AtEnd), "damaged state file: with start at the end the new lines are read");
}


/* --- Long lines --- */

static void TestLongLines()
{
    Files F ("long.log");
    FileReader Reader;
    std::vector<FileReader::Line> Lines;

    NewReader (Reader);
    Reader.SetMaxLineLength (10);

    AppendFile (F.File, "0123456789\n01234567890\nabc\n");
    Reader.Open (F.File, F.State);
    ReadAll (Reader, Lines);

    Check ( (3 == Lines.size()) && ("0123456789" == Lines[0].Text) && (false == Lines[0].bTruncated), "long line: a line of exactly the maximum is not cut");
    Check ( (3 == Lines.size()) && ("0123456789" == Lines[1].Text) && (Lines[1].bTruncated), "long line: a line of one more is cut, and marked");
    Check ( (3 == Lines.size()) && (23 == Lines[1].EndOffset), "long line: the position is behind its new line");
    Check ( (3 == Lines.size()) && ("abc" == Lines[2].Text) && (false == Lines[2].bTruncated), "long line: the next line is fine");

    /* A long line which is still being written: what comes after the maximum is dropped up to the new line */
    Files G ("long_open.log");
    FileReader Open;
    std::vector<FileReader::Line> Result;

    NewReader (Open);
    Open.SetMaxLineLength (10);
    Open.Open (G.File, G.State);
    AppendFile (G.File, "AAAAAAAAAAAAAAAAAAAA");
    ReadAll (Open, Result);

    Check ( (1 == Result.size()) && ("AAAAAAAAAA" == Result[0].Text) && Result[0].bTruncated, "long line: without a new line it is cut as soon as it is too long");
    Check ( (1 == Result.size()) && (10 == Result[0].EndOffset), "long line: the position is behind the part which was returned");

    AppendFile (G.File, "BBBB\nok\n");
    ReadAll (Open, Result);
    Check ( (2 == Result.size()) && ("ok" == Result[1].Text), "long line: the rest of the line is dropped, and the next line is fine");

    AppendFile (G.File, "0123456789");
    ReadAll (Open, Result);
    Check (2 == Result.size(), "long line: exactly the maximum without a new line waits for it");
    AppendFile (G.File, "\n");
    ReadAll (Open, Result);
    Check ( (3 == Result.size()) && ("0123456789" == Result[2].Text) && (false == Result[2].bTruncated), "long line: and is not cut");

    /* The default is 1 MB */
    Files H ("long_default.log");
    FileReader Default;
    std::vector<FileReader::Line> Big;

    NewReader (Default);
    AppendFile (H.File, std::string (1024 * 1024 + 10, 'x') + "\nnext\n");
    Default.Open (H.File, H.State);
    ReadAll (Default, Big);
    Check ( (2 == Big.size()) && (1024 * 1024 == Big[0].Text.size()) && Big[0].bTruncated && ("next" == Big[1].Text), "long line: the default maximum is 1 MB");

    Files I ("long_zero.log");
    FileReader Zero;
    std::vector<FileReader::Line> Small;

    NewReader (Zero);
    Zero.SetMaxLineLength (0);
    AppendFile (I.File, std::string (5000, 'y') + "\n");
    Zero.Open (I.File, I.State);
    ReadAll (Zero, Small);
    Check ( (1 == Small.size()) && (5000 == Small[0].Text.size()) && (false == Small[0].bTruncated), "long line: the maximum 0 means the default");
}


/* --- Incomplete last line --- */

static void TestIncompleteTimeout()
{
    Files F ("timeout.log");
    FileReader Reader;
    FileReader::Line Line;

    NewReader (Reader);
    Reader.SetIncompleteLineSeconds (5);
    Reader.Open (F.File, F.State);
    AppendFile (F.File, "abc");

    Check (false == Reader.ReadLine (Line, 100), "incomplete timeout: the line is not returned at once");
    Check (false == Reader.ReadLine (Line, 104), "incomplete timeout: nor before the time is over");
    Check (true == Reader.ReadLine (Line, 105), "incomplete timeout: it is returned when it did not grow for the time");
    Check ( ("abc" == Line.Text) && (3 == Line.EndOffset) && (false == Line.bTruncated), "incomplete timeout: as it is, with the position behind it");
    Check (false == Reader.ReadLine (Line, 200), "incomplete timeout: only once");

    AppendFile (F.File, "def\n");
    Check ( Reader.ReadLine (Line, 201) && ("def" == Line.Text), "incomplete timeout: what comes after is a line of its own");

    /* A line which grows starts the time again */
    AppendFile (F.File, "xy");
    Check (false == Reader.ReadLine (Line, 300), "incomplete timeout: a new line which is not finished");
    AppendFile (F.File, "z");
    Check (false == Reader.ReadLine (Line, 303), "incomplete timeout: it grew");
    Check (false == Reader.ReadLine (Line, 307), "incomplete timeout: the time starts again when it grows");
    Check ( Reader.ReadLine (Line, 308) && ("xyz" == Line.Text), "incomplete timeout: and the line is returned when it did not grow for the time");

    /* Not set: it waits */
    Files G ("timeout_off.log");
    FileReader Wait;

    NewReader (Wait);
    Wait.Open (G.File, G.State);
    AppendFile (G.File, "waiting");
    Check ( (false == Wait.ReadLine (Line, 1)) && (false == Wait.ReadLine (Line, 2000000000)), "incomplete timeout: without the setting the line waits for its new line, however long it takes");

    /* A rotation ends the file: its last line is returned, with or without the setting */
    Files H ("timeout_rotate.log");
    FileReader Rotate;
    std::vector<FileReader::Line> Lines;

    NewReader (Rotate);
    AppendFile (H.File, "a\nlast");
    Rotate.Open (H.File, H.State);
    ReadAll (Rotate, Lines);
    ::rename (H.File.c_str(), (H.File + ".1").c_str());
    AppendFile (H.File, "b\n");
    ReadAll (Rotate, Lines);
    ReadAll (Rotate, Lines);
    Check ("a|last|b" == Join (Lines), "incomplete timeout: a file which was rotated gives its last line, it will not be finished");
}


/* --- The state file --- */

static void TestStateFile()
{
    Files F ("statefile.log");
    FileReader Reader;
    std::vector<FileReader::Line> Lines;
    struct stat StatBuf;

    NewReader (Reader);
    ::umask (022);
    AppendFile (F.File, "one\ntwo\n");
    Reader.Open (F.File, F.State);
    ReadAll (Reader, Lines);
    Reader.Commit (Lines[1].Generation, Lines[1].EndOffset);

    Check ( (0 == ::stat (F.State.c_str(), &StatBuf)) && (0600 == (StatBuf.st_mode & 0777)), "state file: only the owner can access it (0600)");
    Check (0 != ::access ((F.State + ".tmp").c_str(), F_OK), "state file: no temporary file is left");
    Check ( ("1" == StateValue (F.State, "version")) && ("8" == StateValue (F.State, "offset")) && ("" != StateValue (F.State, "device")) && ("" != StateValue (F.State, "headhash")),
            "state file: it has the version, the file, the position and the fingerprint");
    Check ("8" == StateValue (F.State, "headlen"), "state file: the fingerprint is as long as the committed part of a small file");

    /* The fingerprint stops at 256 bytes */
    Files G ("statefile_big.log");
    FileReader Big;
    std::vector<FileReader::Line> Result;

    NewReader (Big);
    AppendFile (G.File, std::string (1000, 'q') + "\n");
    Big.Open (G.File, G.State);
    ReadAll (Big, Result);
    Big.Commit (Result[0].Generation, Result[0].EndOffset);
    Check ("256" == StateValue (G.State, "headlen"), "state file: the fingerprint is 256 bytes at most");

    /* A place which cannot be written: the reader goes on, and says it once */
    Files H ("statefile_nodir.log");
    FileReader Blocked;
    std::vector<FileReader::Line> Read;
    std::string NoState = g_Dir + "/does_not_exist/state";

    NewReader (Blocked);
    g_Messages.clear();
    AppendFile (H.File, "a\nb\nc\n");
    Blocked.Open (H.File, NoState);
    ReadAll (Blocked, Read);

    Check ("a|b|c" == Join (Read), "state file which cannot be written: the lines are still read");
    Check (1 == CountMessages ("cannot save"), "state file which cannot be written: one message");
    Check (false == Blocked.Commit (Read[0].Generation, Read[0].EndOffset), "state file which cannot be written: Commit says so");
    Check (false == Blocked.Commit (Read[1].Generation, Read[1].EndOffset), "state file which cannot be written: every time");
    Check (1 == CountMessages ("cannot save"), "state file which cannot be written: but the message is not repeated");
    Check (0 == Blocked.GetCommitted(), "state file which cannot be written: the position was not moved");

    /* The place is there again: it works, and the position is the one of the last commit */
    ::mkdir ((g_Dir + "/does_not_exist").c_str(), 0755);
    Check (true == Blocked.Commit (Read[1].Generation, Read[1].EndOffset), "state file which cannot be written: it works again when the place is there");
    Check ("4" == StateValue (NoState, "offset"), "state file which cannot be written: with the position of the commit");
    ::unlink (NoState.c_str());
    ::rmdir ((g_Dir + "/does_not_exist").c_str());
}


/* --- Larger data --- */

static void TestLargerData()
{
    Files F ("large.log");
    FileReader Reader;
    std::vector<FileReader::Line> Lines;
    std::string Text;
    const int Count = 200000;

    NewReader (Reader);
    g_Messages.clear();
    Reader.Open (F.File, F.State);

    for (int i = 0; i < Count; i++)
        Text += "line " + std::to_string (i) + " " + std::string (static_cast<size_t> (i % 97), 'x') + "\n";

    /* The pieces cut the lines anywhere, and are not related to the buffer of the reader (64 KB) */
    for (size_t Pos = 0; Pos < Text.size(); Pos += 7919)
    {
        AppendFile (F.File, Text.substr (Pos, 7919));

        if (0 == ((Pos / 7919) % 3))
            ReadAll (Reader, Lines);
    }

    ReadAll (Reader, Lines);

    bool bSame = (static_cast<size_t> (Count) == Lines.size());

    for (int i = 0; bSame && (i < Count); i++)
    {
        std::string Expected = "line " + std::to_string (i) + " " + std::string (static_cast<size_t> (i % 97), 'x');

        if ( (Lines[static_cast<size_t> (i)].Text != Expected) || (Lines[static_cast<size_t> (i)].bTruncated) )
            bSame = false;
    }

    Check (bSame, "large data: 200000 lines written in odd pieces arrive in order and complete, none twice");
    Check ( (false == Lines.empty()) && (static_cast<uint64_t> (FileSize (F.File)) == Lines.back().EndOffset), "large data: the last position is the size of the file");
    Check (0 == g_Messages.size(), "large data: no message");

    /* The same file in one piece, from a state file which was written in the middle */
    Reader.Commit (Lines[100000].Generation, Lines[100000].EndOffset);

    FileReader Restarted;
    std::vector<FileReader::Line> Rest;

    NewReader (Restarted);
    Restarted.Open (F.File, F.State);
    ReadAll (Restarted, Rest);
    Check ( (static_cast<size_t> (Count - 100001) == Rest.size()) && (Rest[0].Text == Lines[100001].Text), "large data: a restart in the middle of a large file continues at the right line");
}


static void TestFileLeaks()
{
    Files F ("leak.log");
    size_t Before = CountOpenFiles();

    for (int i = 0; i < 30; i++)
    {
        FileReader Reader;
        std::vector<FileReader::Line> Lines;

        NewReader (Reader);
        AppendFile (F.File, "a\n");
        Reader.Open (F.File, F.State);
        ReadAll (Reader, Lines);
        ::rename (F.File.c_str(), (F.File + ".1").c_str());
        AppendFile (F.File, "b\n");
        ReadAll (Reader, Lines);
        ReadAll (Reader, Lines);
        ReadAll (Reader, Lines);
        ::unlink ((F.File + ".1").c_str());
        ::unlink (F.File.c_str());
    }

    Check (CountOpenFiles() == Before, "files: no file is left open after readers which read, rotated and were destroyed");

    /* Open again with other paths: the file of the first one is closed */
    Files A ("leak_a.log");
    Files B ("leak_b.log");
    FileReader Reader;
    std::vector<FileReader::Line> Lines;

    NewReader (Reader);
    AppendFile (A.File, "a\n");
    AppendFile (B.File, "b\n");
    Reader.Open (A.File, A.State);
    ReadAll (Reader, Lines);

    size_t One = CountOpenFiles();

    Reader.Open (B.File, B.State);
    Check (CountOpenFiles() + 1 == One, "files: Open again lets go of the file of the first Open");
    ReadAll (Reader, Lines);
    Check ("a|b" == Join (Lines), "files: and reads the other file (generation 1 again)");
    Check (1 == Reader.GetGeneration(), "files: the generation starts again with Open");

    Reader.Close();
    Check (false == Reader.IsFileOpen(), "files: Close lets go of the file");

    FileReader::Line Line;
    Check (false == Reader.ReadLine (Line, NOW), "files: a closed reader has no line");
}


int main (int argc, char *argv[])
{
    std::string Parent = "/tmp";

    ::signal (SIGPIPE, SIG_IGN);
    ::umask (022);
    setvbuf (stdout, NULL, _IOLBF, 0);

    if (argc > 1)
        Parent = argv[1];

    std::string Template = Parent + "/file_reader_unit_test_XXXXXX";
    std::vector<char> TemplateBuffer (Template.begin(), Template.end());
    TemplateBuffer.push_back ('\0');

    char *pszDir = ::mkdtemp (TemplateBuffer.data());

    if (NULL == pszDir)
    {
        perror ("Cannot create the working directory");
        return 2;
    }

    g_Dir = pszDir;

    Group ("FileReader unit test");
    printf ("Working directory: %s\n", g_Dir.c_str());

    Group ("Lines");
    TestBasicLines();
    TestIncompleteLine();
    TestLineEndings();
    TestGrowingAndMissingFile();

    Group ("Commit and restart");
    TestCommitAndRestart();
    TestCommitRules();

    Group ("Where to start");
    TestStart();

    Group ("Rotation");
    TestRotation();
    TestRotationLateWrites();
    TestDeletedFile();

    Group ("Truncation");
    TestTruncate();

    Group ("Changed while the reader was stopped");
    TestChangedWhileStopped();

    Group ("Damaged state file");
    TestDamagedState();

    Group ("Long lines");
    TestLongLines();

    Group ("Incomplete last line");
    TestIncompleteTimeout();

    Group ("The state file");
    TestStateFile();

    Group ("Larger data, and files which are not leaked");
    TestLargerData();
    TestFileLeaks();

    /* The directory was created by this test and only contains its files */
    std::string Command = "rm -rf '" + g_Dir + "'";

    if (0 != ::system (Command.c_str()))
        fprintf (stderr, "could not remove %s\n", g_Dir.c_str());

    if (false == g_FailedNames.empty())
    {
        Group ("Failed checks");

        for (const std::string& Name : g_FailedNames)
            printf ("[FAIL]  %s\n", Name.c_str());
    }

    Group ("Result");
    printf ("%s  %d of %d checks passed, %d failed\n\n", g_Failed ? "[FAIL]" : "[PASS]", g_Total - g_Failed, g_Total, g_Failed);

    return g_Failed ? 1 : 0;
}
