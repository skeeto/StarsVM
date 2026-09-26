/* fs.h - the files behind the guest's DOS calls.
 *
 * api_dos.c speaks INT 21h and the KERNEL file API to the guest and keeps the
 * DOS handle table; this is what it speaks to underneath.  There are two
 * implementations, and a build links exactly one:
 *
 *   fs_win32.c  the emulator's: real files through Win32, with the read-ahead
 *               and write-behind buffering the game's tiny reads want
 *   fs_mem.c    the library's: a directory held in memory, which is the whole
 *               of the library's contact with anything like a disk
 *
 * Paths are the guest's own bytes, exactly as it passed them - drive letters,
 * backslashes and all - so each implementation decides what they name.
 * Failures are DOS error codes, returned negated where the result is
 * otherwise a count or a handle.
 */
#ifndef FS_H
#define FS_H

#include <stddef.h>
#include <stdint.h>

/* DOS error codes. */
#define DOSERR_FUNC          1
#define DOSERR_FILENOTFOUND  2
#define DOSERR_PATHNOTFOUND  3
#define DOSERR_TOOMANYFILES  4
#define DOSERR_ACCESS        5
#define DOSERR_BADHANDLE     6
#define DOSERR_NOMOREFILES  18

/* DOS file attributes. */
#define DOSATTR_RDONLY  0x01
#define DOSATTR_HIDDEN  0x02
#define DOSATTR_SYSTEM  0x04
#define DOSATTR_LABEL   0x08
#define DOSATTR_DIR     0x10
#define DOSATTR_ARCHIVE 0x20

/* The longest path the guest can hand over and have honoured. */
#define FS_PATH 260

enum { FS_READ, FS_WRITE, FS_RDWR };                 /* fs_open access */
enum { FS_EXISTING, FS_CREATE, FS_CREATE_NEW };      /* fs_open disposition */
enum { FS_SET, FS_CUR, FS_END };                     /* fs_seek origin */

/* Open or create.  A handle, which is small and non-negative, or -error. */
int  fs_open(const char *path, int access, int disposition);
/* 0, or -error if the last of the writes could not be made. */
int  fs_close(int h);
long fs_read(int h, void *dst, uint32_t n);          /* bytes, or -error */
long fs_write(int h, const void *src, uint32_t n);   /* bytes, or -error */
/* The new position in *pos; 0 or -error. */
int  fs_seek(int h, int32_t off, int origin, uint32_t *pos);
/* Flush and close everything: the program is over. */
void fs_close_all(void);

int  fs_getattr(const char *path, unsigned *attr);   /* 0 or -error */
int  fs_setattr(const char *path, unsigned attr);
int  fs_unlink(const char *path);
int  fs_rename(const char *from, const char *to);
int  fs_mkdir(const char *path);
int  fs_rmdir(const char *path);
int  fs_chdir(const char *path);
/* The current directory as DOS reports it: no drive, no leading backslash. */
int  fs_getcwd(char *buf, size_t n);

/* One directory entry, in the form a DTA carries it. */
typedef struct {
    char     name[13];     /* 8.3, NUL-terminated */
    uint8_t  attr;         /* DOSATTR_* */
    uint16_t time, date;   /* DOS packed */
    uint32_t size;
} FsEntry;

/* A search: a slot, which is small and non-negative, with the first match in
   *e; or -error.  fs_find_next gives 0 and the next match, or -error once
   there are none left, at which point the slot is already closed. */
int  fs_find_first(const char *pattern, FsEntry *e);
int  fs_find_next(int slot, FsEntry *e);

/* The volume label of the drive `path` starts with, or of the current drive;
   0 or -error.  The game folds it into its machine fingerprint. */
int  fs_volume_label(const char *path, char *label, size_t n);

/* Drive geometry for AH=36h, `drive` 0 for the current one, then 1 = A:. */
int  fs_disk_space(unsigned drive, uint32_t *sectors_per_cluster,
                   uint32_t *bytes_per_sector, uint32_t *free_clusters,
                   uint32_t *total_clusters);

#endif
