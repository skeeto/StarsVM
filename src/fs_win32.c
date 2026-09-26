/* fs_win32.c - the guest's files as real files, for the emulator.  See fs.h.
 *
 * A file here is buffered.  Win16's _lread was a DOS call and nothing more -
 * HFILE is the DOS handle number - and the game reads its files the way that
 * API invites: a record's length word, its type word, then the record, one
 * call each, 36,000 reads and 11,000 writes per generated turn, all but a
 * hundred of them directly after another on the same handle and none of them
 * seeking.  Each was a ReadFile or WriteFile, a microsecond or two apiece,
 * which by the time the interpreter had been made fast was an eighth of a
 * turn.  So a handle carries one buffer that is either read-ahead - the next
 * 16 KB of the file, served out in pieces - or write-behind - what the game
 * has written and the file has not yet seen.  It is one or the other: a read
 * flushes the writes, a write hands back to the file whatever was read ahead
 * and not consumed, so the file's own position is always what the guest would
 * believe it to be except by the amount the buffer accounts for, and a seek
 * corrects for that amount.
 *
 * Two handles can be the same file - the game closes a file before opening it
 * again, but nothing says it must - so a handle remembers which file it is,
 * and a read through one flushes the writes pending on any other handle to the
 * same file first.  Flushing at close and at exit is the rest of it.  What is
 * lost is only the error from a write that fails, which now surfaces at the
 * flush rather than at the call.
 */

#include "fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define MAX_FILES 64
#define FILE_BUF  16384u

typedef struct {
    HANDLE   h;                   /* NULL when the slot is free          */
    uint8_t *buf;                 /* FILE_BUF bytes, on first use        */
    uint32_t rpos, rlen;          /* read-ahead: buf[rpos..rlen) unread  */
    uint32_t wlen;                /* write-behind: buf[0..wlen) unwritten */
    DWORD    vol, ixhi, ixlo;     /* which file this is                  */
} File;
static File files[MAX_FILES];

static File *file_get(int h)
{
    if (h < 0 || h >= MAX_FILES || !files[h].h) return NULL;
    return &files[h];
}

/* Hand the file what the guest has written.  0, or -1 with the buffer
   discarded: there is nothing a second attempt would do differently. */
static int file_flush(File *f)
{
    DWORD put = 0;
    int ok;
    if (!f->wlen) return 0;
    ok = WriteFile(f->h, f->buf, f->wlen, &put, NULL) && put == f->wlen;
    f->wlen = 0;
    return ok ? 0 : -1;
}

static int file_same(const File *a, const File *b)
{
    return a->h && b->h && a->vol == b->vol && a->ixhi == b->ixhi && a->ixlo == b->ixlo;
}

/* Before reading through `f`, whatever another handle to the same file has
   not yet written must be there to read. */
static int file_flush_aliases(File *f)
{
    int i, r = 0;
    for (i = 0; i < MAX_FILES; i++)
        if (&files[i] != f && files[i].wlen && file_same(&files[i], f))
            if (file_flush(&files[i]) < 0) r = -1;
    return r;
}

/* Before opening or creating anything: which file it will turn out to be is
   not known until it is open, and by then a creation has already truncated
   it, so a buffered write to it from an older handle would land afterwards
   and put back what the creation removed.  Opens are rare and a handle is
   dirty only mid-write, so this costs nothing measurable. */
static void file_flush_all(void)
{
    int i;
    for (i = 0; i < MAX_FILES; i++)
        if (files[i].wlen) file_flush(&files[i]);
}

/* Give back to the file what was read ahead of the guest, so that its
   position is where the guest thinks it is. */
static void file_unread(File *f)
{
    if (f->rlen > f->rpos) {
        LONG back = -(LONG)(f->rlen - f->rpos);
        SetFilePointer(f->h, back, NULL, FILE_CURRENT);
    }
    f->rpos = f->rlen = 0;
}

static int file_buffer(File *f)
{
    if (!f->buf) f->buf = malloc(FILE_BUF);
    return f->buf != NULL;
}

int fs_open(const char *path, int access, int disposition)
{
    BY_HANDLE_FILE_INFORMATION info;
    DWORD acc, disp;
    HANDLE h;
    int i;

    file_flush_all();
    acc = access == FS_READ  ? GENERIC_READ
        : access == FS_WRITE ? GENERIC_WRITE
                             : (GENERIC_READ | GENERIC_WRITE);
    disp = disposition == FS_CREATE     ? CREATE_ALWAYS
         : disposition == FS_CREATE_NEW ? CREATE_NEW
                                        : OPEN_EXISTING;
    /* Shared both ways: the only other handles on these files are the
       game's own, and it may hold two on one file. */
    h = CreateFileA(path, acc, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, disp,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return disposition == FS_EXISTING ? -DOSERR_FILENOTFOUND : -DOSERR_ACCESS;

    for (i = 0; i < MAX_FILES; i++) if (!files[i].h) break;
    if (i == MAX_FILES) { CloseHandle(h); return -DOSERR_TOOMANYFILES; }
    files[i].h = h;
    files[i].rpos = files[i].rlen = files[i].wlen = 0;
    if (GetFileInformationByHandle(h, &info)) {
        files[i].vol  = info.dwVolumeSerialNumber;
        files[i].ixhi = info.nFileIndexHigh;
        files[i].ixlo = info.nFileIndexLow;
    } else {
        files[i].vol = files[i].ixhi = files[i].ixlo = 0;
    }
    return i;
}

int fs_close(int h)
{
    File *f = file_get(h);
    int r;
    if (!f) return -DOSERR_BADHANDLE;
    r = file_flush(f);
    CloseHandle(f->h);
    free(f->buf);
    memset(f, 0, sizeof *f);
    return r < 0 ? -DOSERR_ACCESS : 0;
}

long fs_read(int h, void *dst, uint32_t want)
{
    File *f = file_get(h);
    uint8_t *out = dst;
    uint32_t done = 0;
    DWORD got = 0;

    if (!f) return -DOSERR_BADHANDLE;
    if (f->wlen && file_flush(f) < 0) return -DOSERR_ACCESS;
    if (file_flush_aliases(f) < 0) return -DOSERR_ACCESS;
    if (!file_buffer(f)) {
        if (want && !ReadFile(f->h, dst, want, &got, NULL)) return -DOSERR_ACCESS;
        return (long)got;
    }
    while (done < want) {
        uint32_t n = f->rlen - f->rpos;
        if (n) {
            if (n > want - done) n = want - done;
            memcpy(out + done, f->buf + f->rpos, n);
            f->rpos += n;
            done += n;
            continue;
        }
        if (want - done >= FILE_BUF) {
            /* Bigger than the buffer: straight into the destination. */
            if (!ReadFile(f->h, out + done, want - done, &got, NULL))
                return -DOSERR_ACCESS;
            done += got;
            break;
        }
        if (!ReadFile(f->h, f->buf, FILE_BUF, &got, NULL)) return -DOSERR_ACCESS;
        if (!got) break;                       /* the end of the file */
        f->rpos = 0;
        f->rlen = got;
    }
    return (long)done;
}

long fs_write(int h, const void *src, uint32_t n)
{
    File *f = file_get(h);
    DWORD put = 0;

    if (!f) return -DOSERR_BADHANDLE;
    if (f->rlen) file_unread(f);
    if (!n) return 0;
    if (!file_buffer(f)) {
        if (!WriteFile(f->h, src, n, &put, NULL)) return -DOSERR_ACCESS;
        return (long)put;
    }
    if (f->wlen + n > FILE_BUF && file_flush(f) < 0) return -DOSERR_ACCESS;
    if (n >= FILE_BUF) {
        if (!WriteFile(f->h, src, n, &put, NULL)) return -DOSERR_ACCESS;
        return (long)put;
    }
    memcpy(f->buf + f->wlen, src, n);
    f->wlen += n;
    return (long)n;
}

/* The buffer's worth of read-ahead is the difference between the file's
   position and the guest's; writes go out first. */
int fs_seek(int h, int32_t off, int origin, uint32_t *pos)
{
    File *f = file_get(h);
    DWORD method = origin == FS_CUR ? FILE_CURRENT
                 : origin == FS_END ? FILE_END : FILE_BEGIN;
    DWORD r;

    if (!f) return -DOSERR_BADHANDLE;
    if (f->wlen && file_flush(f) < 0) return -DOSERR_ACCESS;
    if (f->rlen) {
        if (method == FILE_CURRENT) off -= (int32_t)(f->rlen - f->rpos);
        f->rpos = f->rlen = 0;
    }
    r = SetFilePointer(f->h, off, NULL, method);
    if (r == INVALID_SET_FILE_POINTER) return -DOSERR_ACCESS;
    *pos = r;
    return 0;
}

/* DOS closed a program's handles when it exited, and the buffered writes of a
   file the game left open would otherwise never reach it. */
void fs_close_all(void)
{
    int i;
    for (i = 0; i < MAX_FILES; i++)
        if (files[i].h) fs_close(i);
}

/* ---- by path ------------------------------------------------------------- */

int fs_getattr(const char *path, unsigned *out)
{
    DWORD attr = GetFileAttributesA(path);
    if (attr == INVALID_FILE_ATTRIBUTES) return -DOSERR_FILENOTFOUND;
    *out = (attr & FILE_ATTRIBUTE_READONLY  ? DOSATTR_RDONLY  : 0) |
           (attr & FILE_ATTRIBUTE_HIDDEN    ? DOSATTR_HIDDEN  : 0) |
           (attr & FILE_ATTRIBUTE_SYSTEM    ? DOSATTR_SYSTEM  : 0) |
           (attr & FILE_ATTRIBUTE_DIRECTORY ? DOSATTR_DIR     : 0) |
           (attr & FILE_ATTRIBUTE_ARCHIVE   ? DOSATTR_ARCHIVE : 0);
    return 0;
}

int fs_setattr(const char *path, unsigned dos)
{
    DWORD attr = (dos & DOSATTR_RDONLY) ? FILE_ATTRIBUTE_READONLY : 0;
    if (dos & DOSATTR_HIDDEN)  attr |= FILE_ATTRIBUTE_HIDDEN;
    if (dos & DOSATTR_SYSTEM)  attr |= FILE_ATTRIBUTE_SYSTEM;
    if (dos & DOSATTR_ARCHIVE) attr |= FILE_ATTRIBUTE_ARCHIVE;
    if (!attr) attr = FILE_ATTRIBUTE_NORMAL;
    return SetFileAttributesA(path, attr) ? 0 : -DOSERR_FILENOTFOUND;
}

int fs_unlink(const char *path)
{
    return DeleteFileA(path) ? 0 : -DOSERR_FILENOTFOUND;
}

int fs_rename(const char *from, const char *to)
{
    return MoveFileA(from, to) ? 0 : -DOSERR_ACCESS;
}

int fs_mkdir(const char *path)
{
    return CreateDirectoryA(path, NULL) ? 0 : -DOSERR_PATHNOTFOUND;
}

int fs_rmdir(const char *path)
{
    return RemoveDirectoryA(path) ? 0 : -DOSERR_PATHNOTFOUND;
}

int fs_chdir(const char *path)
{
    return SetCurrentDirectoryA(path) ? 0 : -DOSERR_PATHNOTFOUND;
}

int fs_getcwd(char *buf, size_t n)
{
    char cwd[MAX_PATH];
    const char *p;

    if (!GetCurrentDirectoryA(sizeof cwd, cwd)) return -DOSERR_PATHNOTFOUND;
    /* DOS reports the path without the drive or the leading backslash. */
    p = cwd;
    if (p[0] && p[1] == ':') p += 2;
    if (*p == '\\') p++;
    snprintf(buf, n, "%s", p);
    return 0;
}

/* ---- directory searches -------------------------------------------------- */

#define MAX_FINDS 16
static HANDLE finds[MAX_FINDS];

static void put_entry(FsEntry *e, const WIN32_FIND_DATAA *fd)
{
    FILETIME lft;
    WORD date = 0, time = 0;
    const char *name = fd->cAlternateFileName[0] ? fd->cAlternateFileName
                                                 : fd->cFileName;
    unsigned a = 0;

    if (fd->dwFileAttributes & FILE_ATTRIBUTE_READONLY)  a |= DOSATTR_RDONLY;
    if (fd->dwFileAttributes & FILE_ATTRIBUTE_HIDDEN)    a |= DOSATTR_HIDDEN;
    if (fd->dwFileAttributes & FILE_ATTRIBUTE_SYSTEM)    a |= DOSATTR_SYSTEM;
    if (fd->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) a |= DOSATTR_DIR;
    if (fd->dwFileAttributes & FILE_ATTRIBUTE_ARCHIVE)   a |= DOSATTR_ARCHIVE;
    if (FileTimeToLocalFileTime(&fd->ftLastWriteTime, &lft))
        FileTimeToDosDateTime(&lft, &date, &time);

    snprintf(e->name, sizeof e->name, "%s", name);
    e->attr = (uint8_t)a;
    e->time = time;
    e->date = date;
    e->size = fd->nFileSizeLow;
}

int fs_find_first(const char *pattern, FsEntry *e)
{
    WIN32_FIND_DATAA fd;
    HANDLE h;
    int slot;

    for (slot = 0; slot < MAX_FINDS; slot++) if (!finds[slot]) break;
    if (slot == MAX_FINDS) return -DOSERR_TOOMANYFILES;
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return -DOSERR_FILENOTFOUND;
    finds[slot] = h;
    put_entry(e, &fd);
    return slot;
}

int fs_find_next(int slot, FsEntry *e)
{
    WIN32_FIND_DATAA fd;

    if (slot < 0 || slot >= MAX_FINDS || !finds[slot]) return -DOSERR_NOMOREFILES;
    if (!FindNextFileA(finds[slot], &fd)) {
        FindClose(finds[slot]);
        finds[slot] = NULL;
        return -DOSERR_NOMOREFILES;
    }
    put_entry(e, &fd);
    return 0;
}

/* GetVolumeInformation is the only route Win32 offers to the name; see
   api_dos.c for why the game wants it. */
int fs_volume_label(const char *path, char *label, size_t n)
{
    char root[4], buf[MAX_PATH];
    const char *rootp = NULL;

    if (path[0] && path[1] == ':') {
        root[0] = path[0];
        root[1] = ':';
        root[2] = '\\';
        root[3] = 0;
        rootp = root;
    }
    if (!GetVolumeInformationA(rootp, buf, sizeof buf, NULL, NULL, NULL, NULL, 0)
        || !buf[0])
        return -DOSERR_FILENOTFOUND;
    snprintf(label, n, "%s", buf);
    return 0;
}

int fs_disk_space(unsigned drive, uint32_t *spc, uint32_t *bps,
                  uint32_t *freec, uint32_t *totalc)
{
    DWORD a = 0, b = 0, f = 0, t = 0;
    char root[4];

    root[0] = (char)('A' + (drive ? drive - 1 : 2));
    root[1] = ':'; root[2] = '\\'; root[3] = 0;
    if (!GetDiskFreeSpaceA(root, &a, &b, &f, &t)) return -DOSERR_FUNC;
    *spc = a; *bps = b; *freec = f; *totalc = t;
    return 0;
}
