/* api_dos.c - KERNEL.102 DOS3Call, i.e. INT 21h, and the KERNEL file API.
 *
 * Stars! reaches DOS directly for directory enumeration and file work: 22 call
 * sites, and the game's save files (<game>.hst, .m1, .x1, .xy, backup\) are
 * found by walking directories with FindFirst/FindNext.
 *
 * This file is the guest's side of it - registers, DTAs, OFSTRUCTs and the DOS
 * handle table - and fs.h is the other side, which is real files for the
 * emulator and a directory in memory for the library.
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
#include "fs.h"
#include "hostclock.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* OpenFile's style bits, which have the same values in Win16 and Win32. */
#define OF_READ       0x0000
#define OF_WRITE      0x0001
#define OF_READWRITE  0x0002
#define OF_CREATE     0x1000
#define OF_EXIST      0x4000

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

/* A DOS handle is either a file, which fs.h holds, or a window onto the module
   image.  The second kind exists for AccessResource, which hands the game a
   handle to resource data that it then reads with _lread: the module is
   already in memory, so reopening the file it came from was always a detour,
   and once that file can hold the module compressed there is no byte range in
   it to reopen at all.

   Sequential and read-only is the whole contract.  The game imports _lread
   and _lclose and no seek of any kind, so an image window needs no more than
   a position, and the operations that cannot apply to one say so rather than
   pretending. */
enum { FD_FREE, FD_FILE, FD_MEM };
static struct {
    int            kind;
    int            fh;          /* FD_FILE: the fs.h handle */
    const uint8_t *mem;         /* FD_MEM: the span          */
    uint32_t       len, pos;
} fds[MAX_FILES];

static int fd_slot(void)
{
    int i;
    for (i = 5; i < MAX_FILES; i++)   /* 0..4 are the standard handles */
        if (fds[i].kind == FD_FREE) return i;
    return -1;
}

/* A DOS handle for an fs.h handle, closing the latter if there is no room. */
static int fd_file(int fh)
{
    int fd = fd_slot();
    if (fd < 0) { fs_close(fh); return -DOSERR_TOOMANYFILES; }
    fds[fd].kind = FD_FILE;
    fds[fd].fh = fh;
    return fd;
}

static int fd_ok(int fd)
{
    return fd >= 0 && fd < MAX_FILES && fds[fd].kind != FD_FREE;
}

/* 0, or -error when the last of the writes could not be made. */
static int fd_close(int fd)
{
    int r = fds[fd].kind == FD_FILE ? fs_close(fds[fd].fh) : 0;
    memset(&fds[fd], 0, sizeof fds[fd]);
    return r;
}

/* Running off the end of an image window is a short read, as for a file. */
static long fd_read(int fd, void *dst, uint32_t want)
{
    if (fds[fd].kind == FD_MEM) {
        uint32_t n = fds[fd].len - fds[fd].pos;
        if (n > want) n = want;
        memcpy(dst, fds[fd].mem + fds[fd].pos, n);
        fds[fd].pos += n;
        return (long)n;
    }
    return fs_read(fds[fd].fh, dst, want);
}

/* Every handle, flushed and closed: the program is over. */
void dos_shutdown(void)
{
    int i;
    for (i = 5; i < MAX_FILES; i++)
        if (fds[i].kind != FD_FREE) fd_close(i);
    fs_close_all();
}

void dos_reset(void)
{
    memset(fds, 0, sizeof fds);
    dta_ptr = 0;
}

/* A DOS handle onto a span of the module image, for AccessResource. */
uint16_t dos_open_mem(const uint8_t *mem, uint32_t len)
{
    int fd = fd_slot();
    if (fd < 0) return 0xFFFF;
    fds[fd].kind = FD_MEM;
    fds[fd].mem = mem;
    fds[fd].len = len;
    fds[fd].pos = 0;
    return (uint16_t)fd;
}

/* ---- helpers ------------------------------------------------------------- */

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

static void dos_fail(Cpu *c, int err)
{
    set_reg16(c, R_AX, (uint16_t)err);
    c->eflags |= F_CF;
}

static void dos_ok(Cpu *c)
{
    c->eflags &= ~F_CF;
}

/* ---- the Win16 file API, on the same handle table ------------------------- */

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
    char path[FS_PATH], full[FS_PATH];
    uint32_t namep = arg_long(a);
    uint32_t ofs   = arg_long(a);
    uint16_t style = arg_word(a);
    unsigned attr;
    int fh, fd;

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
        if (fs_getattr(full, &attr) < 0)
            snprintf(full, sizeof full, "%.*s", (int)sizeof full - 1, path);
    } else {
        snprintf(full, sizeof full, "%.*s", (int)sizeof full - 1, path);
    }

    if (log_verbose)
        log_msg("OpenFile \"%s\" style %04X (as \"%s\")\n", path, style, full);
    if (style & OF_EXIST) {
        fill_ofstruct(ofs, full, 0);
        return fs_getattr(full, &attr) < 0 ? 0xFFFF : 0;
    }

    fh = fs_open(full,
                 (style & OF_READWRITE) ? FS_RDWR :
                 (style & OF_WRITE)     ? FS_WRITE : FS_READ,
                 (style & OF_CREATE)    ? FS_CREATE : FS_EXISTING);
    if (fh < 0) {
        if (log_verbose)
            log_msg("OpenFile: \"%s\" failed, DOS error %d\n", full, -fh);
        fill_ofstruct(ofs, full, 2);
        return 0xFFFF;
    }
    fd = fd_file(fh);
    if (fd < 0) { fill_ofstruct(ofs, full, 4); return 0xFFFF; }
    fill_ofstruct(ofs, full, 0);
    return (uint32_t)fd;
}

static uint32_t k_lclose(Cpu *c, Args *a)
{
    int fd = arg_word(a);
    (void)c;
    if (!fd_ok(fd)) return 0xFFFF;
    return fd_close(fd) < 0 ? 0xFFFF : 0;
}

static uint32_t k_lread(Cpu *c, Args *a)
{
    int fd = arg_word(a);
    uint32_t buf = arg_long(a);
    uint16_t want = arg_word(a);
    long got;

    (void)c;
    if (!fd_ok(fd)) return 0xFFFF;
    got = fd_read(fd, sel_ptr(SEGPTR_SEL(buf), SEGPTR_OFF(buf)), want);
    return (got < 0) ? 0xFFFF : (uint32_t)got;
}

static uint32_t k_lwrite(Cpu *c, Args *a)
{
    int fd = arg_word(a);
    uint32_t buf = arg_long(a);
    uint16_t want = arg_word(a);
    long put;

    (void)c;
    if (!fd_ok(fd) || fds[fd].kind != FD_FILE)  /* an image window is read-only */
        return 0xFFFF;
    put = fs_write(fds[fd].fh, sel_ptr(SEGPTR_SEL(buf), SEGPTR_OFF(buf)), want);
    return (put < 0) ? 0xFFFF : (uint32_t)put;
}

/* ---- directory searches -------------------------------------------------- */

static uint32_t current_dta(Cpu *c)
{
    (void)c;
    return dta_ptr ? dta_ptr : SEGPTR(task.psp_sel, PSP_CMDLINE);
}

static void put_dta_result(uint32_t dta, const FsEntry *e)
{
    uint16_t sel = SEGPTR_SEL(dta), off = SEGPTR_OFF(dta);
    unsigned i;

    sel_wr8 (sel, (uint16_t)(off + DTA_FILEATTR), e->attr);
    sel_wr16(sel, (uint16_t)(off + DTA_TIME), e->time);
    sel_wr16(sel, (uint16_t)(off + DTA_DATE), e->date);
    sel_wr32(sel, (uint16_t)(off + DTA_SIZE), e->size);
    for (i = 0; i < 12 && e->name[i]; i++)
        sel_wr8(sel, (uint16_t)(off + DTA_NAME + i), (uint8_t)e->name[i]);
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
    char path[FS_PATH], path2[FS_PATH];
    int r;

    (void)a;
    dos_ok(c);

    if (log_verbose) {
        switch (ah) {
        case 0x39: case 0x3A: case 0x3B: case 0x3C: case 0x3D: case 0x41:
        case 0x43: case 0x4E: case 0x56: case 0x5B:
            guest_path(SEGPTR(c->seg[S_DS], reg16(c, R_DX)), path, sizeof path);
            log_msg("INT 21h AH=%02X AL=%02X CX=%04X \"%s\"\n",
                    ah, al, reg16(c, R_CX), path);
        }
    }

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
        HostTime t;
        host_localtime(&t);
        set_reg16(c, R_CX, t.year);
        set_reg16(c, R_DX, (uint16_t)((t.month << 8) | t.day));
        set_reg16(c, R_AX, (uint16_t)((reg16(c, R_AX) & 0xFF00) | t.dow));
        return 0;
    }
    case 0x2C: {                                 /* get time */
        HostTime t;
        host_localtime(&t);
        set_reg16(c, R_CX, (uint16_t)((t.hour << 8) | t.minute));
        set_reg16(c, R_DX, (uint16_t)((t.second << 8) | (t.ms / 10)));
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
        uint32_t spc, bps, freec, totalc;
        /* DL, not AL: 0 means the default drive, then 1 = A.  Reading AL
           here asked about a drive named by whatever was left in it, so this
           handler had never once succeeded. */
        if (fs_disk_space((unsigned)(reg16(c, R_DX) & 0xFF),
                          &spc, &bps, &freec, &totalc) < 0) {
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
        if ((r = fs_mkdir(path)) < 0) dos_fail(c, -r);
        return 0;

    case 0x3A:                                   /* rmdir */
        guest_path(SEGPTR(c->seg[S_DS], reg16(c, R_DX)), path, sizeof path);
        if ((r = fs_rmdir(path)) < 0) dos_fail(c, -r);
        return 0;

    case 0x3B:                                   /* chdir */
        guest_path(SEGPTR(c->seg[S_DS], reg16(c, R_DX)), path, sizeof path);
        if ((r = fs_chdir(path)) < 0) dos_fail(c, -r);
        return 0;

    case 0x3C: case 0x5B:                        /* create / create new */
    case 0x3D: {                                 /* open */
        guest_path(SEGPTR(c->seg[S_DS], reg16(c, R_DX)), path, sizeof path);
        if (ah == 0x3D)
            r = fs_open(path, (al & 3) == 0 ? FS_READ :
                              (al & 3) == 1 ? FS_WRITE : FS_RDWR, FS_EXISTING);
        else
            r = fs_open(path, FS_RDWR, ah == 0x5B ? FS_CREATE_NEW : FS_CREATE);
        if (r >= 0) r = fd_file(r);
        if (r < 0) { dos_fail(c, -r); return 0; }
        set_reg16(c, R_AX, (uint16_t)r);
        return 0;
    }

    case 0x3E: {                                 /* close */
        int fd = reg16(c, R_BX);
        if (!fd_ok(fd)) { dos_fail(c, DOSERR_BADHANDLE); return 0; }
        if ((r = fd_close(fd)) < 0) dos_fail(c, -r);
        return 0;
    }

    case 0x3F: {                                 /* read */
        int fd = reg16(c, R_BX);
        uint16_t sel = c->seg[S_DS], off = reg16(c, R_DX);
        uint32_t want = reg16(c, R_CX);
        uint32_t limit;
        long got;

        if (!fd_ok(fd)) { dos_fail(c, DOSERR_BADHANDLE); return 0; }
        /* Clamp to what the destination selector can actually hold; Win16 code
           is known to pass counts larger than the buffer. */
        limit = sel_tab[SEL_INDEX(sel)].limit;
        if (off + want > limit + 1) want = limit + 1 - off;
        got = fd_read(fd, sel_ptr(sel, off), want);
        if (got < 0) { dos_fail(c, (int)-got); return 0; }
        set_reg16(c, R_AX, (uint16_t)got);
        return 0;
    }

    case 0x40: {                                 /* write */
        int fd = reg16(c, R_BX);
        uint16_t sel = c->seg[S_DS], off = reg16(c, R_DX);
        uint32_t want = reg16(c, R_CX);
        long put;

        if (!fd_ok(fd) || fds[fd].kind != FD_FILE) {
            dos_fail(c, DOSERR_BADHANDLE);
            return 0;
        }
        put = fs_write(fds[fd].fh, sel_ptr(sel, off), want);
        if (put < 0) { dos_fail(c, (int)-put); return 0; }
        set_reg16(c, R_AX, (uint16_t)put);
        return 0;
    }

    case 0x41:                                   /* unlink */
        guest_path(SEGPTR(c->seg[S_DS], reg16(c, R_DX)), path, sizeof path);
        if ((r = fs_unlink(path)) < 0) dos_fail(c, -r);
        return 0;

    case 0x42: {                                 /* lseek */
        int fd = reg16(c, R_BX);
        int32_t off = (int32_t)(((uint32_t)reg16(c, R_CX) << 16) | reg16(c, R_DX));
        uint32_t pos;

        if (!fd_ok(fd) || fds[fd].kind != FD_FILE) {
            dos_fail(c, DOSERR_BADHANDLE);
            return 0;
        }
        r = fs_seek(fds[fd].fh, off,
                    al == 1 ? FS_CUR : al == 2 ? FS_END : FS_SET, &pos);
        if (r < 0) { dos_fail(c, -r); return 0; }
        set_reg16(c, R_AX, (uint16_t)pos);
        set_reg16(c, R_DX, (uint16_t)(pos >> 16));
        return 0;
    }

    case 0x43: {                                 /* get or set attributes */
        unsigned attr;
        guest_path(SEGPTR(c->seg[S_DS], reg16(c, R_DX)), path, sizeof path);
        if (al == 0) {
            if ((r = fs_getattr(path, &attr)) < 0) { dos_fail(c, -r); return 0; }
            set_reg16(c, R_CX, (uint16_t)attr);
        } else if ((r = fs_setattr(path, reg16(c, R_CX))) < 0) {
            dos_fail(c, -r);
        }
        return 0;
    }

    case 0x44:                                   /* IOCTL */
        if (al == 0) { set_reg16(c, R_DX, 0x0080); return 0; }  /* a plain file */
        dos_fail(c, DOSERR_FUNC);
        return 0;

    case 0x47: {                                 /* get the current directory */
        char cwd[FS_PATH];
        uint16_t sel = c->seg[S_DS], off = reg16(c, R_SI);
        unsigned i;

        if ((r = fs_getcwd(cwd, sizeof cwd)) < 0) { dos_fail(c, -r); return 0; }
        for (i = 0; cwd[i] && i < 63; i++)
            sel_wr8(sel, (uint16_t)(off + i), (uint8_t)cwd[i]);
        sel_wr8(sel, (uint16_t)(off + i), 0);
        set_reg16(c, R_AX, 0x0100);
        return 0;
    }

    case 0x4E: {                                 /* find first */
        uint32_t dta = current_dta(c);
        uint16_t cx = reg16(c, R_CX);
        FsEntry e;

        guest_path(SEGPTR(c->seg[S_DS], reg16(c, R_DX)), path, sizeof path);

        /* An attribute of exactly the volume-label bit is how DOS is asked
           for a volume label, and it is how the game reads the one its
           machine fingerprint hashes.  A directory search never reports a
           label, so that one has to be answered elsewhere.  Only the bare bit
           is redirected: with other bits set the caller wants files as well,
           and those searches keep the ordinary path. */
        if (cx == DOSATTR_LABEL) {
            char label[64];
            if ((r = fs_volume_label(path, label, sizeof label)) < 0) {
                dos_fail(c, -r);
                return 0;
            }
            put_dta_label(dta, label);
            set_reg16(c, R_AX, 0);
            return 0;
        }

        if ((r = fs_find_first(path, &e)) < 0) { dos_fail(c, -r); return 0; }
        sel_wr32(SEGPTR_SEL(dta), (uint16_t)(SEGPTR_OFF(dta) + DTA_COOKIE),
                 (uint32_t)(r + 1));
        sel_wr8(SEGPTR_SEL(dta), (uint16_t)(SEGPTR_OFF(dta) + DTA_ATTR),
                (uint8_t)cx);
        put_dta_result(dta, &e);
        set_reg16(c, R_AX, 0);
        return 0;
    }

    case 0x4F: {                                 /* find next */
        uint32_t dta = current_dta(c);
        uint32_t cookie = sel_rd32(SEGPTR_SEL(dta),
                                   (uint16_t)(SEGPTR_OFF(dta) + DTA_COOKIE));
        FsEntry e;

        if (!cookie || (r = fs_find_next((int)cookie - 1, &e)) < 0) {
            dos_fail(c, DOSERR_NOMOREFILES);
            return 0;
        }
        put_dta_result(dta, &e);
        set_reg16(c, R_AX, 0);
        return 0;
    }

    case 0x56:                                   /* rename */
        guest_path(SEGPTR(c->seg[S_DS], reg16(c, R_DX)), path, sizeof path);
        guest_path(SEGPTR(c->seg[S_ES], reg16(c, R_DI)), path2, sizeof path2);
        if ((r = fs_rename(path, path2)) < 0) dos_fail(c, -r);
        return 0;

    case 0x4C:                                   /* terminate */
        log_msg("DOS3Call: the guest exited with code %u\n", al);
        task.exitcode = (int)al;
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
