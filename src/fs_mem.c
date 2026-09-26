/* fs_mem.c - the guest's files as a directory in memory, for the library.
 * See fs.h for the interface and lib.h for how the library fills and reads it.
 *
 * One drive, C:, whose root is where the game is and where it starts.  Names
 * are case-insensitive, so they are kept in uppercase, and are root-relative:
 * "C:\GAME.HST", "\game.hst" and "game.hst" are one file.
 *
 * Directories exist only if the game made them, which in practice means the
 * backup directory turn generation keeps beside the host file.  A path through
 * a directory that does not exist names the file of that name in the root
 * instead of failing.  That is what lets a .def written for some real
 * directory tree - "c:\stars\play\game.r1" - find the race files the caller
 * supplied, and put the new game where the library will collect it.
 *
 * The caller's input files are never copied up front.  A file borrows the
 * caller's bytes until the guest first writes to it, and only then gets bytes
 * of its own from the arena.
 */

#include "fs.h"
#include "lib.h"
#include "stars.h"

#include <string.h>

#define MAX_ENTRIES 512
#define NAME_MAX    128
#define MAX_OPEN    64
#define MAX_FINDS   16

/* Every file carries this: midnight, the first of January 2026, the same
   date the pinned clock reports.  It is not read from that clock, because
   each reading of it advances it, and a directory listing must not change what
   the guest sees next. */
#define FILE_DATE (uint16_t)(((2026 - 1980) << 9) | (1 << 5) | 1)
#define FILE_TIME 0

typedef struct {
    char           name[NAME_MAX];  /* "" when the slot is free */
    int            isdir;
    int            written;
    uint8_t        attr;
    const uint8_t *borrowed;        /* the caller's, until the first write */
    uint8_t       *data;            /* ours, after it                      */
    ptrdiff_t      len, cap;
} Ent;

static Ent *ents;          /* MAX_ENTRIES, from the arena of the run */
static int  nents;         /* high-water mark of used slots          */

static struct {
    int      ent;          /* -1 when the handle is free */
    int      access;
    uint32_t pos;
} opens[MAX_OPEN];

static struct {
    int  used;
    char dir[NAME_MAX];    /* "" for the root, else "BACKUP" */
    char pat[NAME_MAX];
    int  next;             /* the entry to look at next      */
} finds[MAX_FINDS];

void memfs_reset(void)
{
    int i;
    ents = lib_alloc((ptrdiff_t)sizeof *ents * MAX_ENTRIES);
    nents = 0;
    for (i = 0; i < MAX_OPEN; i++) opens[i].ent = -1;
    memset(finds, 0, sizeof finds);
}

/* ---- names --------------------------------------------------------------- */

/* The canonical form of a guest path: no drive, no leading backslash, "." and
   ".." resolved, uppercase, backslashes only.  0 if it will not fit. */
static int canon(const char *in, char *out)
{
    size_t n = 0;

    if (in[0] && in[1] == ':') in += 2;
    while (*in) {
        const char *seg = in;
        size_t len;
        while (*in && *in != '\\' && *in != '/') in++;
        len = (size_t)(in - seg);
        if (*in) in++;
        if (!len || (len == 1 && seg[0] == '.')) continue;
        if (len == 2 && seg[0] == '.' && seg[1] == '.') {
            while (n && out[n - 1] != '\\') n--;
            if (n) n--;                         /* the separator before it */
            continue;
        }
        if (n + (n ? 1 : 0) + len + 1 > NAME_MAX) return 0;
        if (n) out[n++] = '\\';
        while (len--) {
            char c = *seg++;
            out[n++] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
        }
    }
    out[n] = 0;
    return 1;
}

static int find_ent(const char *name)
{
    int i;
    for (i = 0; i < nents; i++)
        if (ents[i].name[0] && !strcmp(ents[i].name, name)) return i;
    return -1;
}

static int is_dir(const char *name)
{
    int i = find_ent(name);
    return i >= 0 && ents[i].isdir;
}

/* The directory's own name, or "" for the root. */
static void parent_of(const char *name, char *dir)
{
    const char *slash = strrchr(name, '\\');
    size_t n = slash ? (size_t)(slash - name) : 0;
    memcpy(dir, name, n);
    dir[n] = 0;
}

static const char *base_of(const char *name)
{
    const char *slash = strrchr(name, '\\');
    return slash ? slash + 1 : name;
}

/* A guest path as the name of an entry, following the rule at the top: a
   directory the game did not make is not there, so the file is in the root. */
static int mem_resolve(const char *path, char *out)
{
    char dir[NAME_MAX];
    if (!canon(path, out)) return 0;
    parent_of(out, dir);
    if (dir[0] && !is_dir(dir)) memmove(out, base_of(out), strlen(base_of(out)) + 1);
    return 1;
}

static int new_ent(const char *name, int isdir)
{
    int i;
    for (i = 0; i < nents; i++) if (!ents[i].name[0]) break;
    if (i == nents) {
        if (nents == MAX_ENTRIES) return -1;
        nents++;
    }
    memset(&ents[i], 0, sizeof ents[i]);
    strcpy(ents[i].name, name);
    ents[i].isdir = isdir;
    ents[i].attr = isdir ? DOSATTR_DIR : DOSATTR_ARCHIVE;
    return i;
}

void memfs_add(const char *name, const uint8_t *data, ptrdiff_t len)
{
    char full[NAME_MAX], n[NAME_MAX];
    int i;
    if (!canon(name, full)) return;
    strcpy(n, base_of(full));             /* the root has every input */
    i = find_ent(n);
    if (i < 0) i = new_ent(n, 0);
    if (i < 0) lib_fail(STARS_ENOMEM);
    ents[i].borrowed = data;
    ents[i].data = NULL;
    ents[i].len = len;
    ents[i].cap = 0;
    ents[i].written = 0;
}

int memfs_entry(int i, const char **name, const uint8_t **data, ptrdiff_t *len,
                int *isdir, int *written)
{
    for (; i < nents; i++) {
        if (!ents[i].name[0]) continue;
        *name = ents[i].name;
        *data = ents[i].data ? ents[i].data : ents[i].borrowed;
        *len = ents[i].len;
        *isdir = ents[i].isdir;
        *written = ents[i].written;
        return i + 1;
    }
    return 0;
}

/* ---- contents ------------------------------------------------------------ */

static const uint8_t *ent_bytes(const Ent *e)
{
    return e->data ? e->data : e->borrowed;
}

/* Room for `need` bytes of our own, copying the caller's on the first write. */
static void ent_reserve(Ent *e, ptrdiff_t need)
{
    if (e->borrowed) {
        ptrdiff_t cap = e->len > need ? e->len : need;
        uint8_t *d = lib_alloc(cap ? cap : 1);
        memcpy(d, e->borrowed, (size_t)e->len);
        e->borrowed = NULL;
        e->data = d;
        e->cap = cap;
    }
    if (need > e->cap) {
        ptrdiff_t cap = e->cap ? e->cap : 4096;
        while (cap < need) cap *= 2;
        e->data = e->data ? lib_grow(e->data, e->cap, cap) : lib_alloc(cap);
        e->cap = cap;
    }
}

static void ent_truncate(Ent *e)
{
    e->borrowed = NULL;
    e->len = 0;               /* keep any bytes of our own for reuse */
    e->written = 1;
}

/* ---- handles ------------------------------------------------------------- */

int fs_open(const char *path, int access, int disposition)
{
    char name[NAME_MAX];
    int h, i;

    if (!mem_resolve(path, name) || !name[0])
        return disposition == FS_EXISTING ? -DOSERR_FILENOTFOUND : -DOSERR_ACCESS;
    i = find_ent(name);
    if (i >= 0 && ents[i].isdir) return -DOSERR_ACCESS;
    switch (disposition) {
    case FS_EXISTING:
        if (i < 0) return -DOSERR_FILENOTFOUND;
        break;
    case FS_CREATE_NEW:
        if (i >= 0) return -DOSERR_ACCESS;
        /* fall through */
    case FS_CREATE:
        if (i < 0 && (i = new_ent(name, 0)) < 0) return -DOSERR_ACCESS;
        ent_truncate(&ents[i]);
        break;
    }
    for (h = 0; h < MAX_OPEN; h++) if (opens[h].ent < 0) break;
    if (h == MAX_OPEN) return -DOSERR_TOOMANYFILES;
    opens[h].ent = i;
    opens[h].access = access;
    opens[h].pos = 0;
    return h;
}

static Ent *open_ent(int h)
{
    if (h < 0 || h >= MAX_OPEN || opens[h].ent < 0) return NULL;
    return &ents[opens[h].ent];
}

int fs_close(int h)
{
    if (!open_ent(h)) return -DOSERR_BADHANDLE;
    opens[h].ent = -1;
    return 0;
}

long fs_read(int h, void *dst, uint32_t n)
{
    Ent *e = open_ent(h);
    ptrdiff_t left;

    if (!e) return -DOSERR_BADHANDLE;
    if (opens[h].access == FS_WRITE) return -DOSERR_ACCESS;
    left = e->len - (ptrdiff_t)opens[h].pos;
    if (left <= 0) return 0;
    if ((ptrdiff_t)n > left) n = (uint32_t)left;
    memcpy(dst, ent_bytes(e) + opens[h].pos, n);
    opens[h].pos += n;
    return (long)n;
}

long fs_write(int h, const void *src, uint32_t n)
{
    Ent *e = open_ent(h);
    ptrdiff_t end;

    if (!e) return -DOSERR_BADHANDLE;
    if (opens[h].access == FS_READ) return -DOSERR_ACCESS;
    e->written = 1;
    if (!n) return 0;
    end = (ptrdiff_t)opens[h].pos + n;
    ent_reserve(e, end);
    if ((ptrdiff_t)opens[h].pos > e->len)       /* a gap left by a seek */
        memset(e->data + e->len, 0, (size_t)(opens[h].pos - e->len));
    memcpy(e->data + opens[h].pos, src, n);
    if (end > e->len) e->len = end;
    opens[h].pos = (uint32_t)end;
    return (long)n;
}

int fs_seek(int h, int32_t off, int origin, uint32_t *pos)
{
    Ent *e = open_ent(h);
    int64_t base, to;

    if (!e) return -DOSERR_BADHANDLE;
    base = origin == FS_CUR ? (int64_t)opens[h].pos
         : origin == FS_END ? (int64_t)e->len : 0;
    to = base + off;
    if (to < 0 || to > 0x7FFFFFFF) return -DOSERR_ACCESS;
    opens[h].pos = (uint32_t)to;
    *pos = opens[h].pos;
    return 0;
}

void fs_close_all(void)
{
    int h;
    for (h = 0; h < MAX_OPEN; h++) opens[h].ent = -1;
}

/* ---- by path ------------------------------------------------------------- */

int fs_getattr(const char *path, unsigned *attr)
{
    char name[NAME_MAX];
    int i;
    if (!mem_resolve(path, name)) return -DOSERR_FILENOTFOUND;
    if (!name[0]) { *attr = DOSATTR_DIR; return 0; }     /* the root */
    if ((i = find_ent(name)) < 0) return -DOSERR_FILENOTFOUND;
    *attr = ents[i].attr;
    return 0;
}

int fs_setattr(const char *path, unsigned attr)
{
    char name[NAME_MAX];
    int i;
    if (!mem_resolve(path, name) || (i = find_ent(name)) < 0) return -DOSERR_FILENOTFOUND;
    ents[i].attr = (uint8_t)((attr & ~DOSATTR_DIR) | (ents[i].isdir ? DOSATTR_DIR : 0));
    return 0;
}

int fs_unlink(const char *path)
{
    char name[NAME_MAX];
    int i;
    if (!mem_resolve(path, name) || (i = find_ent(name)) < 0 || ents[i].isdir)
        return -DOSERR_FILENOTFOUND;
    ents[i].name[0] = 0;
    return 0;
}

int fs_rename(const char *from, const char *to)
{
    char a[NAME_MAX], b[NAME_MAX];
    int i;
    if (!mem_resolve(from, a) || !mem_resolve(to, b)) return -DOSERR_ACCESS;
    if ((i = find_ent(a)) < 0) return -DOSERR_ACCESS;
    if (!strcmp(a, b)) return 0;
    if (find_ent(b) >= 0) return -DOSERR_ACCESS;
    strcpy(ents[i].name, b);
    return 0;
}

/* Failures say "path not found" whatever the reason, as the emulator's
   CreateDirectory-based one does, so the guest cannot tell the two apart. */
int fs_mkdir(const char *path)
{
    char name[NAME_MAX];
    if (!mem_resolve(path, name) || !name[0] || find_ent(name) >= 0)
        return -DOSERR_PATHNOTFOUND;
    return new_ent(name, 1) < 0 ? -DOSERR_PATHNOTFOUND : 0;
}

int fs_rmdir(const char *path)
{
    char name[NAME_MAX];
    int i, k;
    if (!canon(path, name) || (i = find_ent(name)) < 0 || !ents[i].isdir)
        return -DOSERR_PATHNOTFOUND;
    for (k = 0; k < nents; k++) {
        char dir[NAME_MAX];
        if (!ents[k].name[0]) continue;
        parent_of(ents[k].name, dir);
        if (!strcmp(dir, name)) return -DOSERR_ACCESS;     /* not empty */
    }
    ents[i].name[0] = 0;
    return 0;
}

/* There is no directory to be in but the root. */
int fs_chdir(const char *path)
{
    char name[NAME_MAX];
    if (!canon(path, name) || name[0]) return -DOSERR_PATHNOTFOUND;
    return 0;
}

int fs_getcwd(char *buf, size_t n)
{
    if (n) buf[0] = 0;
    return 0;
}

/* ---- searches ------------------------------------------------------------ */

/* DOS wildcards over one uppercase name: `*` any run, `?` any one character.
   "*.*" also matches a name with no dot, as it did under DOS. */
static int wild(const char *p, const char *s)
{
    for (; *p; p++, s++) {
        if (*p == '*') {
            for (;;) {
                if (wild(p + 1, s)) return 1;
                if (!*s) return 0;
                s++;
            }
        }
        if (!*s) return !strcmp(p, ".*");
        if (*p != '?' && *p != *s) return 0;
    }
    return !*s;
}

static int find_from(int slot, FsEntry *e)
{
    int i;
    for (i = finds[slot].next; i < nents; i++) {
        char dir[NAME_MAX];
        const char *base;
        if (!ents[i].name[0]) continue;
        parent_of(ents[i].name, dir);
        base = base_of(ents[i].name);
        if (strcmp(dir, finds[slot].dir) || strlen(base) > 12) continue;
        if (!wild(finds[slot].pat, base)) continue;
        finds[slot].next = i + 1;
        strcpy(e->name, base);
        e->attr = ents[i].attr;
        e->time = FILE_TIME;
        e->date = FILE_DATE;
        e->size = ents[i].isdir ? 0 : (uint32_t)ents[i].len;
        return 1;
    }
    return 0;
}

int fs_find_first(const char *pattern, FsEntry *e)
{
    char name[NAME_MAX];
    int slot;

    for (slot = 0; slot < MAX_FINDS; slot++) if (!finds[slot].used) break;
    if (slot == MAX_FINDS) return -DOSERR_TOOMANYFILES;
    if (!mem_resolve(pattern, name)) return -DOSERR_FILENOTFOUND;
    parent_of(name, finds[slot].dir);
    strcpy(finds[slot].pat, base_of(name));
    finds[slot].next = 0;
    if (!find_from(slot, e)) return -DOSERR_FILENOTFOUND;
    finds[slot].used = 1;
    return slot;
}

int fs_find_next(int slot, FsEntry *e)
{
    if (slot < 0 || slot >= MAX_FINDS || !finds[slot].used) return -DOSERR_NOMOREFILES;
    if (!find_from(slot, e)) {
        finds[slot].used = 0;
        return -DOSERR_NOMOREFILES;
    }
    return 0;
}

/* ---- the drive ----------------------------------------------------------- */

/* No label: the game's fingerprint falls back to its own constants, which it
   does already, since GetDriveType reports no fixed disks. */
int fs_volume_label(const char *path, char *label, size_t n)
{
    (void)path; (void)label; (void)n;
    return -DOSERR_FILENOTFOUND;
}

/* A plausible disk with plenty of room: 8 x 512-byte sectors per cluster, and
   65535 clusters of them free out of as many. */
int fs_disk_space(unsigned drive, uint32_t *spc, uint32_t *bps,
                  uint32_t *freec, uint32_t *totalc)
{
    (void)drive;
    *spc = 8; *bps = 512; *freec = 0xFFFF; *totalc = 0xFFFF;
    return 0;
}
