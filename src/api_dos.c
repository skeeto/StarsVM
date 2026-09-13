/* api_dos.c - KERNEL.102 DOS3Call, i.e. INT 21h over Win32.
 *
 * Stars! reaches DOS directly for directory enumeration and file work: 22 call
 * sites, and the game's save files (<game>.hst, .m1, .x1, .xy, backup\) are
 * found by walking directories with FindFirst/FindNext.
 *
 * Contract: AH selects the function, the carry flag reports failure, and AX
 * carries the DOS error code when CF is set.  Carry is cleared on entry so a
 * handler only has to act on failure.
 */

#include "thunk.h"
#include "task.h"
#include "sel.h"
#include "log.h"
#include "dos.h"
#include "hostclock.h"

#include <stdio.h>
#include <string.h>
#include <windows.h>

/* DOS error codes we hand back. */
#define DOSERR_FUNC        1
#define DOSERR_FILENOTFOUND 2
#define DOSERR_PATHNOTFOUND 3
#define DOSERR_TOOMANYFILES 4
#define DOSERR_ACCESS      5
#define DOSERR_BADHANDLE   6
#define DOSERR_NOMOREFILES 18

/* DOS file attributes. */
#define DOSATTR_RDONLY  0x01
#define DOSATTR_HIDDEN  0x02
#define DOSATTR_SYSTEM  0x04
#define DOSATTR_LABEL   0x08
#define DOSATTR_DIR     0x10
#define DOSATTR_ARCHIVE 0x20

/* The Disk Transfer Area, 43 bytes.  The first 21 are DOS-private state that
   FindNext consumes; only 0x15..0x2A is the application-visible result.  The
   4 bytes at 0x11 are reserved on real DOS, so we keep our search cookie there
   rather than inventing a field. */
#define DTA_DRIVE     0x00
#define DTA_MASK      0x01   /* 11 bytes */
#define DTA_ATTR      0x0C
#define DTA_COOKIE    0x11   /* our find handle index */
#define DTA_FILEATTR  0x15
#define DTA_TIME      0x16
#define DTA_DATE      0x18
#define DTA_SIZE      0x1A   /* DWORD */
#define DTA_NAME      0x1E   /* 13 bytes, "NAME.EXT" */

static uint32_t dta_ptr = 0;          /* SEGPTR; defaults to PSP:0080 */

/* ---- open file handles --------------------------------------------------- */

#define MAX_FILES 64

/* A handle is either a real file or a window onto the module image.  The
   second kind exists for AccessResource, which hands the game a handle to
   resource data that it then reads with _lread: the module is already in
   memory, so reopening the file it came from was always a detour, and once
   that file can hold the module compressed there is no byte range in it to
   reopen at all.

   Sequential and read-only is the whole contract.  The game imports _lread
   and _lclose and no seek of any kind, so an image window needs no more than
   a position, and the operations that cannot apply to one say so rather than
   pretending. */
typedef struct {
    HANDLE         h;      /* a real file; NULL for an image window     */
    const uint8_t *mem;    /* an image window; NULL for a real file     */
    uint32_t       len, pos;
} File;
static File files[MAX_FILES];

static int file_slot(void)
{
    int i;
    for (i = 5; i < MAX_FILES; i++)   /* 0..4 are the standard handles */
        if (!files[i].h && !files[i].mem) return i;
    return -1;
}

static int file_alloc(HANDLE h)
{
    int fd = file_slot();
    if (fd < 0) { CloseHandle(h); return -1; }
    files[fd].h = h;
    return fd;
}

static File *file_get(int fd)
{
    if (fd < 0 || fd >= MAX_FILES) return NULL;
    return (files[fd].h || files[fd].mem) ? &files[fd] : NULL;
}

static void file_release(int fd)
{
    if (files[fd].h) CloseHandle(files[fd].h);
    files[fd].h = NULL;
    files[fd].mem = NULL;
    files[fd].len = files[fd].pos = 0;
}

/* Read from either kind of handle.  -1 on failure, which for an image window
   cannot happen: running off the end is a short read, exactly as it would be
   for a file. */
static long file_read(File *f, void *dst, uint32_t want)
{
    DWORD got = 0;
    if (f->mem) {
        uint32_t n = f->len - f->pos;
        if (n > want) n = want;
        memcpy(dst, f->mem + f->pos, n);
        f->pos += n;
        return (long)n;
    }
    if (want && !ReadFile(f->h, dst, want, &got, NULL)) return -1;
    return (long)got;
}

static char *guest_path(uint32_t segptr, char *buf, size_t n);

/* A DOS handle onto a span of the module image, for AccessResource. */
uint16_t dos_open_mem(const uint8_t *mem, uint32_t len)
{
    int fd = file_slot();
    if (fd < 0) return 0xFFFF;
    files[fd].mem = mem;
    files[fd].len = len;
    files[fd].pos = 0;
    return (uint16_t)fd;
}

/* ---- the Win16 file API, on the same handle table ------------------------- */

/* OF_READ/OF_WRITE/OF_READWRITE/OF_CREATE/OF_EXIST come from windows.h and
   have the same values in Win16, so there is nothing to translate. */

/* OFSTRUCT, which OpenFile fills in: cBytes, fFixedDisk, nErrCode,
   Reserved[4], szPathName[128]. */
static void fill_ofstruct(uint32_t p, const char *path, uint16_t err)
{
    uint16_t sel = SEGPTR_SEL(p), off = SEGPTR_OFF(p);
    unsigned i;

    if (!p) return;
    sel_wr8 (sel, off, 0x88);                     /* cBytes */
    sel_wr8 (sel, (uint16_t)(off + 1), 1);        /* fFixedDisk */
    sel_wr16(sel, (uint16_t)(off + 2), err);
    for (i = 0; i < 4; i++) sel_wr8(sel, (uint16_t)(off + 4 + i), 0);
    for (i = 0; i < 127 && path[i]; i++)
        sel_wr8(sel, (uint16_t)(off + 8 + i), (uint8_t)path[i]);
    sel_wr8(sel, (uint16_t)(off + 8 + i), 0);
}

static uint32_t k_OpenFile(Cpu *c, Args *a)
{
    char path[MAX_PATH], full[MAX_PATH];
    uint32_t namep = arg_long(a);
    uint32_t ofs   = arg_long(a);
    uint16_t style = arg_word(a);
    HANDLE h;
    DWORD access, disp;
    int fd;

    (void)c;
    guest_path(namep, path, sizeof path);
    /* A bare or relative name is looked for beside the module first and then
       in the current directory - the first two of the six places OpenFile
       documents, and the two that matter, since the game's files are either
       beside stars.exe or where the game was started.  The rule covers
       creating a file as well as opening one, so that a turn file the game
       rewrites lands where it found the last one; a name that exists in
       neither place is created where the game was started, as it would have
       been under Windows.  Until this, only the module's directory was tried,
       and a game started from its own directory with the module elsewhere
       opened its host file (a DOS open, which is relative to the current
       directory) and then could not find the .xy beside it. */
    if (path[1] != ':' && path[0] != '\\') {
        size_t dl = strlen(task.exedir);
        if (dl > sizeof full / 2) dl = sizeof full / 2;
        snprintf(full, sizeof full, "%.*s\\%.*s",
                 (int)dl, task.exedir,
                 (int)(sizeof full - dl - 2), path);
        if (GetFileAttributesA(full) == INVALID_FILE_ATTRIBUTES)
            snprintf(full, sizeof full, "%.*s", (int)sizeof full - 1, path);
    } else {
        snprintf(full, sizeof full, "%.*s", (int)sizeof full - 1, path);
    }

    if (style & OF_EXIST) {
        DWORD attr = GetFileAttributesA(full);
        fill_ofstruct(ofs, full, 0);
        return (attr == INVALID_FILE_ATTRIBUTES) ? 0xFFFF : 0;
    }

    access = (style & OF_READWRITE) ? (GENERIC_READ | GENERIC_WRITE)
           : (style & OF_WRITE)     ? GENERIC_WRITE
                                    : GENERIC_READ;
    disp = (style & OF_CREATE) ? CREATE_ALWAYS : OPEN_EXISTING;
    h = CreateFileA(full, access, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    disp, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        fill_ofstruct(ofs, full, 2);
        return 0xFFFF;
    }
    fd = file_alloc(h);
    if (fd < 0) { fill_ofstruct(ofs, full, 4); return 0xFFFF; }
    fill_ofstruct(ofs, full, 0);
    return (uint32_t)fd;
}

static uint32_t k_lclose(Cpu *c, Args *a)
{
    int fd = arg_word(a);
    (void)c;
    if (!file_get(fd)) return 0xFFFF;
    file_release(fd);
    return 0;
}

static uint32_t k_lread(Cpu *c, Args *a)
{
    int fd = arg_word(a);
    uint32_t buf = arg_long(a);
    uint16_t want = arg_word(a);
    File *f = file_get(fd);
    long got;

    (void)c;
    if (!f) return 0xFFFF;
    got = file_read(f, sel_ptr(SEGPTR_SEL(buf), SEGPTR_OFF(buf)), want);
    return (got < 0) ? 0xFFFF : (uint32_t)got;
}

static uint32_t k_lwrite(Cpu *c, Args *a)
{
    int fd = arg_word(a);
    uint32_t buf = arg_long(a);
    uint16_t want = arg_word(a);
    File *f = file_get(fd);
    DWORD put = 0;

    (void)c;
    if (!f || !f->h) return 0xFFFF;          /* an image window is read-only */
    if (want && !WriteFile(f->h, sel_ptr(SEGPTR_SEL(buf), SEGPTR_OFF(buf)),
                           want, &put, NULL))
        return 0xFFFF;
    return put;
}

/* ---- directory searches -------------------------------------------------- */

#define MAX_FINDS 16
static struct {
    HANDLE h;
    int    used;
    char   dir[MAX_PATH];
} finds[MAX_FINDS];

/* ---- helpers ------------------------------------------------------------- */

static void dos_fail(Cpu *c, uint16_t err)
{
    set_reg16(c, R_AX, err);
    c->eflags |= F_CF;
}

static void dos_ok(Cpu *c)
{
    c->eflags &= ~F_CF;
}

static char *guest_path(uint32_t segptr, char *buf, size_t n)
{
    uint16_t sel = SEGPTR_SEL(segptr), off = SEGPTR_OFF(segptr);
    size_t i = 0;

    while (i + 1 < n) {
        uint8_t ch = sel_rd8(sel, (uint16_t)(off + i));
        if (!ch) break;
        buf[i++] = (char)ch;
    }
    buf[i] = 0;
    return buf;
}

static uint32_t current_dta(Cpu *c)
{
    (void)c;
    return dta_ptr ? dta_ptr : SEGPTR(task.psp_sel, PSP_CMDLINE);
}

static void put_dta_result(uint32_t dta, const WIN32_FIND_DATAA *fd)
{
    uint16_t sel = SEGPTR_SEL(dta), off = SEGPTR_OFF(dta);
    FILETIME lft;
    WORD date = 0, time = 0;
    uint8_t attr = 0;
    unsigned i;
    const char *name = fd->cAlternateFileName[0] ? fd->cAlternateFileName
                                                 : fd->cFileName;

    if (fd->dwFileAttributes & FILE_ATTRIBUTE_READONLY)  attr |= DOSATTR_RDONLY;
    if (fd->dwFileAttributes & FILE_ATTRIBUTE_HIDDEN)    attr |= DOSATTR_HIDDEN;
    if (fd->dwFileAttributes & FILE_ATTRIBUTE_SYSTEM)    attr |= DOSATTR_SYSTEM;
    if (fd->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) attr |= DOSATTR_DIR;
    if (fd->dwFileAttributes & FILE_ATTRIBUTE_ARCHIVE)   attr |= DOSATTR_ARCHIVE;

    if (FileTimeToLocalFileTime(&fd->ftLastWriteTime, &lft))
        FileTimeToDosDateTime(&lft, &date, &time);

    sel_wr8 (sel, (uint16_t)(off + DTA_FILEATTR), attr);
    sel_wr16(sel, (uint16_t)(off + DTA_TIME), time);
    sel_wr16(sel, (uint16_t)(off + DTA_DATE), date);
    sel_wr32(sel, (uint16_t)(off + DTA_SIZE), fd->nFileSizeLow);
    for (i = 0; i < 12 && name[i]; i++)
        sel_wr8(sel, (uint16_t)(off + DTA_NAME + i), (uint8_t)name[i]);
    sel_wr8(sel, (uint16_t)(off + DTA_NAME + i), 0);
}

/* DOS answers a search for the volume-label attribute with the label itself,
   reported as though it were a directory entry.  Win32 has no such entry to
   hand back: GetVolumeInformation is the only route to the name, and the
   label's FAT timestamp is not reachable through Win32 at all, so the date
   and time fields stay zero.  The game folds both halves into its machine
   fingerprint, so the name half varies per machine here and the timestamp
   half does not - see docs/copy-protection.md. */
static void put_dta_label(uint32_t dta, const char *label)
{
    uint16_t sel = SEGPTR_SEL(dta), off = SEGPTR_OFF(dta);
    unsigned i;

    sel_wr8 (sel, (uint16_t)(off + DTA_FILEATTR), DOSATTR_LABEL);
    sel_wr16(sel, (uint16_t)(off + DTA_TIME), 0);
    sel_wr16(sel, (uint16_t)(off + DTA_DATE), 0);
    sel_wr32(sel, (uint16_t)(off + DTA_SIZE), 0);
    for (i = 0; i < 12 && label[i]; i++)
        sel_wr8(sel, (uint16_t)(off + DTA_NAME + i), (uint8_t)label[i]);
    sel_wr8(sel, (uint16_t)(off + DTA_NAME + i), 0);
    /* No find handle is allocated: a label search has exactly one result.
       Clear the cookie so a find-next behind it fails cleanly rather than
       walking whatever search used this DTA last. */
    sel_wr32(sel, (uint16_t)(off + DTA_COOKIE), 0);
}

/* ---- the dispatcher ------------------------------------------------------ */

static uint32_t dos3call(Cpu *c, Args *a)
{
    unsigned ah = (unsigned)((reg16(c, R_AX) >> 8) & 0xFF);
    unsigned al = (unsigned)(reg16(c, R_AX) & 0xFF);
    char path[MAX_PATH], path2[MAX_PATH];

    (void)a;
    dos_ok(c);

    switch (ah) {
    case 0x19:                                   /* get default drive */
        set_reg16(c, R_AX, (uint16_t)((reg16(c, R_AX) & 0xFF00) | 2));  /* C: */
        return 0;

    case 0x1A:                                   /* set the DTA to DS:DX */
        dta_ptr = SEGPTR(c->seg[S_DS], reg16(c, R_DX));
        return 0;

    case 0x25:                                   /* set an interrupt vector */
        return 0;                                /* nothing to hook here */

    case 0x2A: {                                 /* get date */
        SYSTEMTIME st;
        host_localtime(&st);
        set_reg16(c, R_CX, st.wYear);
        set_reg16(c, R_DX, (uint16_t)((st.wMonth << 8) | st.wDay));
        set_reg16(c, R_AX, (uint16_t)((reg16(c, R_AX) & 0xFF00) | st.wDayOfWeek));
        return 0;
    }
    case 0x2C: {                                 /* get time */
        SYSTEMTIME st;
        host_localtime(&st);
        set_reg16(c, R_CX, (uint16_t)((st.wHour << 8) | st.wMinute));
        set_reg16(c, R_DX, (uint16_t)((st.wSecond << 8) | (st.wMilliseconds / 10)));
        return 0;
    }
    case 0x2F:                                   /* get the DTA into ES:BX */
        c->seg[S_ES] = SEGPTR_SEL(current_dta(c));
        set_reg16(c, R_BX, SEGPTR_OFF(current_dta(c)));
        return 0;

    case 0x30:                                   /* get the DOS version */
        set_reg16(c, R_AX, 0x1606);              /* AL=6 AH=22, i.e. 6.22 */
        set_reg16(c, R_BX, 0);
        set_reg16(c, R_CX, 0);
        return 0;

    case 0x35:                                   /* get an interrupt vector */
        c->seg[S_ES] = 0;
        set_reg16(c, R_BX, 0);
        return 0;

    case 0x36: {                                 /* get free disk space */
        DWORD spc = 0, bps = 0, freec = 0, totalc = 0;
        unsigned dl = (unsigned)(reg16(c, R_DX) & 0xFF);
        char root[4];
        /* DL, not AL: 0 means the default drive, then 1 = A.  Reading AL
           here asked Win32 about a drive named by whatever was left in it,
           so this handler had never once succeeded. */
        root[0] = (char)('A' + (dl ? dl - 1 : 2));
        root[1] = ':'; root[2] = '\\'; root[3] = 0;
        if (!GetDiskFreeSpaceA(root, &spc, &bps, &freec, &totalc)) {
            set_reg16(c, R_AX, 0xFFFF);
            return 0;
        }
        if (totalc > 0xFFFF) { freec = freec / 16; totalc = totalc / 16; spc *= 16; }
        set_reg16(c, R_AX, (uint16_t)spc);
        set_reg16(c, R_BX, (uint16_t)(freec > 0xFFFF ? 0xFFFF : freec));
        set_reg16(c, R_CX, (uint16_t)bps);
        set_reg16(c, R_DX, (uint16_t)(totalc > 0xFFFF ? 0xFFFF : totalc));
        return 0;
    }

    case 0x39:                                   /* mkdir */
        guest_path(SEGPTR(c->seg[S_DS], reg16(c, R_DX)), path, sizeof path);
        if (!CreateDirectoryA(path, NULL)) dos_fail(c, DOSERR_PATHNOTFOUND);
        return 0;

    case 0x3A:                                   /* rmdir */
        guest_path(SEGPTR(c->seg[S_DS], reg16(c, R_DX)), path, sizeof path);
        if (!RemoveDirectoryA(path)) dos_fail(c, DOSERR_PATHNOTFOUND);
        return 0;

    case 0x3B:                                   /* chdir */
        guest_path(SEGPTR(c->seg[S_DS], reg16(c, R_DX)), path, sizeof path);
        if (!SetCurrentDirectoryA(path)) dos_fail(c, DOSERR_PATHNOTFOUND);
        return 0;

    case 0x3C: case 0x5B: {                      /* create / create new */
        HANDLE h;
        int fd;
        guest_path(SEGPTR(c->seg[S_DS], reg16(c, R_DX)), path, sizeof path);
        h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_READ, NULL,
                        (ah == 0x5B) ? CREATE_NEW : CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE) { dos_fail(c, DOSERR_ACCESS); return 0; }
        fd = file_alloc(h);
        if (fd < 0) { dos_fail(c, DOSERR_TOOMANYFILES); return 0; }
        set_reg16(c, R_AX, (uint16_t)fd);
        return 0;
    }

    case 0x3D: {                                 /* open */
        HANDLE h;
        int fd;
        DWORD access = (al & 3) == 0 ? GENERIC_READ
                     : (al & 3) == 1 ? GENERIC_WRITE
                                     : (GENERIC_READ | GENERIC_WRITE);
        guest_path(SEGPTR(c->seg[S_DS], reg16(c, R_DX)), path, sizeof path);
        h = CreateFileA(path, access, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE) {
            dos_fail(c, DOSERR_FILENOTFOUND);
            return 0;
        }
        fd = file_alloc(h);
        if (fd < 0) { dos_fail(c, DOSERR_TOOMANYFILES); return 0; }
        set_reg16(c, R_AX, (uint16_t)fd);
        return 0;
    }

    case 0x3E: {                                 /* close */
        int fd = reg16(c, R_BX);
        if (!file_get(fd)) { dos_fail(c, DOSERR_BADHANDLE); return 0; }
        file_release(fd);
        return 0;
    }

    case 0x3F: {                                 /* read */
        int fd = reg16(c, R_BX);
        File *f = file_get(fd);
        uint16_t sel = c->seg[S_DS], off = reg16(c, R_DX);
        uint32_t want = reg16(c, R_CX);
        long got;
        uint32_t limit;

        if (!f) { dos_fail(c, DOSERR_BADHANDLE); return 0; }
        /* Clamp to what the destination selector can actually hold; Win16 code
           is known to pass counts larger than the buffer. */
        limit = sel_tab[SEL_INDEX(sel)].limit;
        if (off + want > limit + 1) want = limit + 1 - off;
        got = file_read(f, sel_ptr(sel, off), want);
        if (got < 0) {
            dos_fail(c, DOSERR_ACCESS);
            return 0;
        }
        set_reg16(c, R_AX, (uint16_t)got);
        return 0;
    }

    case 0x40: {                                 /* write */
        int fd = reg16(c, R_BX);
        File *f = file_get(fd);
        uint16_t sel = c->seg[S_DS], off = reg16(c, R_DX);
        uint32_t want = reg16(c, R_CX);
        DWORD put = 0;

        if (!f || !f->h) { dos_fail(c, DOSERR_BADHANDLE); return 0; }
        if (want && !WriteFile(f->h, sel_ptr(sel, off), want, &put, NULL)) {
            dos_fail(c, DOSERR_ACCESS);
            return 0;
        }
        set_reg16(c, R_AX, (uint16_t)put);
        return 0;
    }

    case 0x41:                                   /* unlink */
        guest_path(SEGPTR(c->seg[S_DS], reg16(c, R_DX)), path, sizeof path);
        if (!DeleteFileA(path)) dos_fail(c, DOSERR_FILENOTFOUND);
        return 0;

    case 0x42: {                                 /* lseek */
        int fd = reg16(c, R_BX);
        File *f = file_get(fd);
        LONG lo = (LONG)(((uint32_t)reg16(c, R_CX) << 16) | reg16(c, R_DX));
        DWORD method = (al == 1) ? FILE_CURRENT : (al == 2) ? FILE_END : FILE_BEGIN;
        DWORD pos;

        if (!f || !f->h) { dos_fail(c, DOSERR_BADHANDLE); return 0; }
        pos = SetFilePointer(f->h, lo, NULL, method);
        if (pos == INVALID_SET_FILE_POINTER) { dos_fail(c, DOSERR_ACCESS); return 0; }
        set_reg16(c, R_AX, (uint16_t)pos);
        set_reg16(c, R_DX, (uint16_t)(pos >> 16));
        return 0;
    }

    case 0x43: {                                 /* get or set attributes */
        DWORD attr;
        guest_path(SEGPTR(c->seg[S_DS], reg16(c, R_DX)), path, sizeof path);
        if (al == 0) {
            attr = GetFileAttributesA(path);
            if (attr == INVALID_FILE_ATTRIBUTES) {
                dos_fail(c, DOSERR_FILENOTFOUND);
                return 0;
            }
            set_reg16(c, R_CX, (uint16_t)
                ((attr & FILE_ATTRIBUTE_READONLY  ? DOSATTR_RDONLY  : 0) |
                 (attr & FILE_ATTRIBUTE_HIDDEN    ? DOSATTR_HIDDEN  : 0) |
                 (attr & FILE_ATTRIBUTE_SYSTEM    ? DOSATTR_SYSTEM  : 0) |
                 (attr & FILE_ATTRIBUTE_DIRECTORY ? DOSATTR_DIR     : 0) |
                 (attr & FILE_ATTRIBUTE_ARCHIVE   ? DOSATTR_ARCHIVE : 0)));
        } else {
            uint16_t cx = reg16(c, R_CX);
            attr = (cx & DOSATTR_RDONLY)  ? FILE_ATTRIBUTE_READONLY : 0;
            if (cx & DOSATTR_HIDDEN)  attr |= FILE_ATTRIBUTE_HIDDEN;
            if (cx & DOSATTR_SYSTEM)  attr |= FILE_ATTRIBUTE_SYSTEM;
            if (cx & DOSATTR_ARCHIVE) attr |= FILE_ATTRIBUTE_ARCHIVE;
            if (!attr) attr = FILE_ATTRIBUTE_NORMAL;
            if (!SetFileAttributesA(path, attr)) dos_fail(c, DOSERR_FILENOTFOUND);
        }
        return 0;
    }

    case 0x44:                                   /* IOCTL */
        if (al == 0) { set_reg16(c, R_DX, 0x0080); return 0; }  /* a plain file */
        dos_fail(c, DOSERR_FUNC);
        return 0;

    case 0x47: {                                 /* get the current directory */
        char cwd[MAX_PATH];
        uint16_t sel = c->seg[S_DS], off = reg16(c, R_SI);
        unsigned i;
        const char *p;

        if (!GetCurrentDirectoryA(sizeof cwd, cwd)) {
            dos_fail(c, DOSERR_PATHNOTFOUND);
            return 0;
        }
        /* DOS reports the path without the drive or the leading backslash. */
        p = cwd;
        if (p[0] && p[1] == ':') p += 2;
        if (*p == '\\') p++;
        for (i = 0; p[i] && i < 63; i++)
            sel_wr8(sel, (uint16_t)(off + i), (uint8_t)p[i]);
        sel_wr8(sel, (uint16_t)(off + i), 0);
        set_reg16(c, R_AX, 0x0100);
        return 0;
    }

    case 0x4E: {                                 /* find first */
        uint32_t dta = current_dta(c);
        uint16_t cx = reg16(c, R_CX);
        WIN32_FIND_DATAA fd;
        HANDLE h;
        int slot;

        guest_path(SEGPTR(c->seg[S_DS], reg16(c, R_DX)), path, sizeof path);

        /* An attribute of exactly the volume-label bit is how DOS is asked
           for a volume label, and it is how the game reads the one its
           machine fingerprint hashes.  FindFirstFile never reports a label,
           so that search has to be answered elsewhere.  Only the bare bit is
           redirected: with other bits set the caller wants files as well, and
           those searches keep the ordinary path. */
        if (cx == DOSATTR_LABEL) {
            char vol[4], label[MAX_PATH];
            const char *rootp = NULL;

            if (path[0] && path[1] == ':') {
                vol[0] = path[0];
                vol[1] = ':';
                vol[2] = '\\';
                vol[3] = 0;
                rootp = vol;
            }
            if (!GetVolumeInformationA(rootp, label, sizeof label,
                                       NULL, NULL, NULL, NULL, 0)
                || !label[0]) {
                dos_fail(c, DOSERR_FILENOTFOUND);
                return 0;
            }
            put_dta_label(dta, label);
            set_reg16(c, R_AX, 0);
            return 0;
        }

        for (slot = 0; slot < MAX_FINDS; slot++) if (!finds[slot].used) break;
        if (slot == MAX_FINDS) { dos_fail(c, DOSERR_TOOMANYFILES); return 0; }

        h = FindFirstFileA(path, &fd);
        if (h == INVALID_HANDLE_VALUE) { dos_fail(c, DOSERR_FILENOTFOUND); return 0; }
        finds[slot].h = h;
        finds[slot].used = 1;
        sel_wr32(SEGPTR_SEL(dta), (uint16_t)(SEGPTR_OFF(dta) + DTA_COOKIE),
                 (uint32_t)(slot + 1));
        sel_wr8(SEGPTR_SEL(dta), (uint16_t)(SEGPTR_OFF(dta) + DTA_ATTR),
                (uint8_t)reg16(c, R_CX));
        put_dta_result(dta, &fd);
        set_reg16(c, R_AX, 0);
        return 0;
    }

    case 0x4F: {                                 /* find next */
        uint32_t dta = current_dta(c);
        WIN32_FIND_DATAA fd;
        uint32_t cookie = sel_rd32(SEGPTR_SEL(dta),
                                   (uint16_t)(SEGPTR_OFF(dta) + DTA_COOKIE));
        int slot = (int)cookie - 1;

        if (slot < 0 || slot >= MAX_FINDS || !finds[slot].used) {
            dos_fail(c, DOSERR_NOMOREFILES);
            return 0;
        }
        if (!FindNextFileA(finds[slot].h, &fd)) {
            FindClose(finds[slot].h);
            finds[slot].used = 0;
            dos_fail(c, DOSERR_NOMOREFILES);
            return 0;
        }
        put_dta_result(dta, &fd);
        set_reg16(c, R_AX, 0);
        return 0;
    }

    case 0x56:                                   /* rename */
        guest_path(SEGPTR(c->seg[S_DS], reg16(c, R_DX)), path, sizeof path);
        guest_path(SEGPTR(c->seg[S_ES], reg16(c, R_DI)), path2, sizeof path2);
        if (!MoveFileA(path, path2)) dos_fail(c, DOSERR_ACCESS);
        return 0;

    case 0x4C:                                   /* terminate */
        log_msg("DOS3Call: the guest exited with code %u\n", al);
        c->state = CPU_HALT;
        return 0;

    default:
        log_msg("*** DOS3Call: INT 21h AH=%02X is not implemented "
                "(AL=%02X BX=%04X CX=%04X DX=%04X)\n",
                ah, al, reg16(c, R_BX), reg16(c, R_CX), reg16(c, R_DX));
        dos_fail(c, DOSERR_FUNC);
        return 0;
    }
}

void api_dos_register(void)
{
    api_bind("KERNEL", 102, dos3call);
    api_bind("KERNEL",  74, k_OpenFile);
    api_bind("KERNEL",  81, k_lclose);
    api_bind("KERNEL",  82, k_lread);
    api_bind("KERNEL",  86, k_lwrite);
}
