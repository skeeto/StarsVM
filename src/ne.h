/* ne.h - New Executable (Win16) parsing and loading. */
#ifndef NE_H
#define NE_H

#include <stdint.h>
#include <wchar.h>

/* ne_flags */
#define NEF_SINGLEDATA   0x0001
#define NEF_MULTIPLEDATA 0x0002
#define NEF_SELFLOAD     0x0800
#define NEF_LINKERROR    0x2000
#define NEF_LIBMODULE    0x8000

/* segment flags */
#define NES_DATA         0x0001
#define NES_ITERATED     0x0008
#define NES_MOVEABLE     0x0010
#define NES_SHAREABLE    0x0020
#define NES_PRELOAD      0x0040
#define NES_RELOCINFO    0x0100
#define NES_DISCARDABLE  0x1000
#define NES_32BIT        0x2000

/* relocation address types */
#define NER_LOBYTE       0
#define NER_SELECTOR     2
#define NER_FARADDR      3
#define NER_OFFSET       5
#define NER_FARADDR48    11
#define NER_OFFSET32     13

/* relocation target types (low 2 bits of the type byte) */
#define NERT_INTERNAL    0
#define NERT_ORDINAL     1
#define NERT_NAME        2
#define NERT_OSFIXUP     3
#define NERT_ADDITIVE    4    /* flag bit */

typedef struct {
    uint16_t sector;      /* file offset >> align shift */
    uint16_t length;      /* bytes in file (0 means 64K) */
    uint16_t flags;
    uint16_t minalloc;    /* 0 means 64K */
    uint16_t sel;         /* selector assigned at load time */
    uint32_t filepos;
    uint32_t size;        /* allocated size */
} NeSeg;

typedef struct {
    uint8_t  *img;        /* the NE image, which may be a slice of a larger file */
    uint32_t  imglen;
    uint32_t  hdr;        /* offset of the NE header within img */

    uint16_t  flags, autodata, heap, stack, align;
    uint16_t  cseg, cmod, cmovent, expver;
    uint16_t  enttab, cbenttab, segtab, rsrctab, restab, modtab, imptab;
    uint32_t  nrestab;
    uint32_t  csip, sssp;

    NeSeg    *seg;        /* cseg entries, 1-based access via ne_seg() */
    char    (*modname)[16];
    int       nmod;

    uint16_t  dgroup_sel;
    wchar_t   path[512];  /* full path of the file the image came from */
    char      name[16];   /* resident module name */
} NeModule;

static inline NeSeg *ne_seg(NeModule *m, unsigned n)   /* n is 1-based */
{
    return (n >= 1 && n <= m->cseg) ? &m->seg[n - 1] : 0;
}

/* Resolve an (module index, ordinal) import to a far pointer.  Supplied by the
   thunk layer; the loader calls it while applying relocations. */
typedef uint32_t (*NeImportFn)(const char *module, uint16_t ordinal, void *user);

int  ne_open(NeModule *m, const wchar_t *path);

/* Open an NE module that has been appended to another file - the emulator's own
   executable, so that `cat StarsVM.exe stars.exe > Stars-x86.exe` is a
   single self-contained program.  Finds the image by signature rather than by
   arithmetic on our own size: nothing then depends on the toolchain's idea of
   where our binary ends, which is not its file size (mingw leaves the COFF
   symbol table past the last section).  Returns 0 if there is nothing appended.

   Every offset inside the module stays relative to the image, so a payload at
   an arbitrary byte offset needs no alignment, and nothing downstream knows or
   cares where in the file it was found. */
int  ne_open_appended(NeModule *m, const wchar_t *path);

void ne_close(NeModule *m);
int  ne_load(NeModule *m, NeImportFn resolve, void *user);
void ne_dump(NeModule *m, int verbose);

/* Resource access. */
typedef struct {
    uint32_t off;      /* file offset of the data */
    uint32_t len;      /* byte length */
    uint16_t flags;
    uint16_t id;       /* 0x8000|n for numeric, else offset of a Pascal string */
} NeResource;

int  ne_find_resource(NeModule *m, uint32_t type, uint32_t name, NeResource *out);
const char *ne_resource_type_name(NeModule *m, uint16_t tid, char *buf, int len);

/* Entry table: resolve an export ordinal to a far pointer (0 if absent). */
uint32_t ne_entry_point(NeModule *m, uint16_t ordinal);
/* Look up an export by name (case-insensitive); 0 if absent. */
uint16_t ne_ordinal_by_name(NeModule *m, const char *name);

#endif
