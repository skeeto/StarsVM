/* api_res.c - resources: the NE resource table, as KERNEL hands it out.
 *
 * Two paths matter for Stars!:
 *
 *  - The ordinary one: FindResource, LoadResource, LockResource.  A resource is
 *    loaded into a global block and the guest gets a far pointer to it.
 *
 *  - The one for large bitmaps: several of the 38 RT_BITMAP resources are over
 *    64 KB (the largest is 640x480x8bpp at 308,288 bytes), and for those the
 *    game uses AccessResource + _lread, so AccessResource has to hand back a
 *    real DOS file handle positioned at the resource data.
 *
 * Turning resource data into Win32 objects - bitmaps and accelerator tables -
 * is the emulator's alone, and lives in api_res_gui.c.
 */

#include "thunk.h"
#include "heap.h"
#include "task.h"
#include "sel.h"
#include "log.h"
#include "dos.h"
#include "res.h"
#include "port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/* A live resource: HRSRC16 is an index here, which keeps the handle opaque and
   lets us remember where the data came from. */
#define MAX_RES 256
static struct {
    int      used;
    uint32_t type, name;      /* as passed to FindResource */
    uint32_t off, len;        /* file offset and byte length */
    uint16_t hglobal;         /* global block once loaded    */
    int      usage;
} rsrc[MAX_RES];

void api_res_reset(void)
{
    memset(rsrc, 0, sizeof rsrc);
}

/* Resolve a guest resource type/name argument.  A Win16 MAKEINTRESOURCE is a
   far pointer with a zero selector, so the offset is the numeric id; anything
   else is a string, which has to be matched against the resource table's own
   Pascal strings. */
/* The same lookup, given a name already in host memory - which is where a menu
   name ends up, since RegisterClass hands one over long before CreateWindow
   needs it. */
static uint32_t res_key_str(NeModule *m, const char *buf, int is_type)
{
    const uint8_t *rt;
    const uint8_t *p;
    unsigned i;

    /* "#123" is the decimal form of an ordinal. */
    if (buf[0] == '#') return 0x8000u | (unsigned)atoi(buf + 1);

    /* Otherwise find the matching Pascal string in the resource table and
       return its byte offset, which is how the table refers to it. */
    rt = m->img + m->hdr + m->rsrctab;
    p = rt + 2;
    for (;;) {
        uint16_t tid = ((uint16_t)p[1] << 8) | p[0];
        uint16_t cnt;
        if (tid == 0) break;
        cnt = (uint16_t)(((uint16_t)p[3] << 8) | p[2]);
        if (!(tid & 0x8000) && is_type) {
            const uint8_t *s = rt + tid;
            if (s[0] == strlen(buf) && ascii_ncasecmp((const char *)s + 1, buf, s[0]) == 0)
                return tid;
        }
        if (!is_type) {
            for (i = 0; i < cnt; i++) {
                const uint8_t *e = p + 8 + i * 12;
                uint16_t nid = (uint16_t)(((uint16_t)e[7] << 8) | e[6]);
                if (!(nid & 0x8000)) {
                    const uint8_t *s = rt + nid;
                    if (s[0] == strlen(buf) &&
                        ascii_ncasecmp((const char *)s + 1, buf, s[0]) == 0)
                        return nid;
                }
            }
        }
        p += 8 + (size_t)cnt * 12;
    }
    return 0;
}

uint32_t res_key(NeModule *m, uint32_t segptr, int is_type)
{
    char buf[64];
    unsigned i;

    if (!segptr) return 0;
    if (SEGPTR_SEL(segptr) == 0)
        return 0x8000u | (SEGPTR_OFF(segptr) & 0x7FFF);

    for (i = 0; i + 1 < sizeof buf; i++) {
        uint8_t ch = sel_rd8(SEGPTR_SEL(segptr), (uint16_t)(SEGPTR_OFF(segptr) + i));
        if (!ch) break;
        buf[i] = (char)ch;
    }
    buf[i] = 0;
    return res_key_str(m, buf, is_type);
}

/* Reading a resource directly out of the file image.  Dialogs, menus and icons
   are turned into Win32 objects here rather than handed to the guest, so they
   want a host pointer and a length, not an HRSRC. */
const uint8_t *res_locate(uint16_t type_id, uint32_t name_segptr, uint32_t *len)
{
    NeModule *m = task.mod;
    NeResource nr;
    uint32_t name;

    if (!m) return NULL;
    name = res_key(m, name_segptr, 0);
    if (!name || !ne_find_resource(m, type_id, name, &nr)) return NULL;
    if (nr.off + nr.len > m->imglen) return NULL;
    if (len) *len = nr.len;
    return m->img + nr.off;
}

const uint8_t *res_locate_name(uint16_t type_id, const char *name, uint32_t *len)
{
    NeModule *m = task.mod;
    NeResource nr;
    uint32_t key;

    if (!m || !name) return NULL;
    key = res_key_str(m, name, 0);
    if (!key || !ne_find_resource(m, type_id, key, &nr)) return NULL;
    if (nr.off + nr.len > m->imglen) return NULL;
    if (len) *len = nr.len;
    return m->img + nr.off;
}

/* Types are usually the numeric RT16_x above, but a program may invent its own:
   Stars! keeps its sound effects under a type literally named "WAVE".  The
   resource table refers to such a type by the offset of its name string, so the
   lookup has to happen before any of the res_locate_* calls can be used. */
uint16_t res_type_key(const char *type_name)
{
    NeModule *m = task.mod;
    if (!m || !type_name) return 0;
    return (uint16_t)res_key_str(m, type_name, 1);
}

const uint8_t *res_locate_id(uint16_t type_id, uint16_t id, uint32_t *len)
{
    NeModule *m = task.mod;
    NeResource nr;

    if (!m) return NULL;
    if (!ne_find_resource(m, type_id, (uint32_t)(0x8000u | id), &nr)) return NULL;
    if (nr.off + nr.len > m->imglen) return NULL;
    if (len) *len = nr.len;
    return m->img + nr.off;
}

/* ---- FindResource / LoadResource / LockResource --------------------------- */

static uint32_t r_FindResource(Cpu *c, Args *a)
{
    uint16_t hinst = arg_word(a);
    uint32_t namep = arg_long(a);
    uint32_t typep = arg_long(a);
    NeModule *m = task.mod;
    uint32_t type, name;
    NeResource nr;
    int i;

    (void)c; (void)hinst;
    type = res_key(m, typep, 1);
    name = res_key(m, namep, 0);
    if (!type || !name) return 0;

    if (!ne_find_resource(m, type, name, &nr)) {
        if (log_verbose)
            log_msg("FindResource: no resource type %04X name %04X\n", type, name);
        return 0;
    }
    /* Reuse an existing entry for the same resource so handles compare equal. */
    for (i = 1; i < MAX_RES; i++)
        if (rsrc[i].used && rsrc[i].type == type && rsrc[i].name == name)
            return (uint32_t)i;
    for (i = 1; i < MAX_RES; i++) if (!rsrc[i].used) break;
    if (i == MAX_RES) { log_msg("FindResource: table full\n"); return 0; }

    rsrc[i].used = 1;
    rsrc[i].type = type;
    rsrc[i].name = name;
    rsrc[i].off  = nr.off;
    rsrc[i].len  = nr.len;
    rsrc[i].hglobal = 0;
    rsrc[i].usage = 0;
    return (uint32_t)i;
}

static uint32_t r_SizeofResource(Cpu *c, Args *a)
{
    uint16_t hinst = arg_word(a);
    uint16_t h = arg_word(a);
    (void)c; (void)hinst;
    if (!h || h >= MAX_RES || !rsrc[h].used) return 0;
    return rsrc[h].len;
}

static uint32_t r_LoadResource(Cpu *c, Args *a)
{
    uint16_t hinst = arg_word(a);
    uint16_t h = arg_word(a);
    NeModule *m = task.mod;

    (void)c; (void)hinst;
    if (!h || h >= MAX_RES || !rsrc[h].used) return 0;
    if (rsrc[h].hglobal) { rsrc[h].usage++; return rsrc[h].hglobal; }

    rsrc[h].hglobal = gmem_alloc(MEM_MOVEABLE, rsrc[h].len);
    if (!rsrc[h].hglobal) return 0;
    if (rsrc[h].off + rsrc[h].len > m->imglen) {
        log_msg("LoadResource: resource %u runs past the end of the file\n", h);
        return 0;
    }
    /* The block may span several selectors for a resource over 64 KB; the arena
       lays those out consecutively, so one copy covers it. */
    memcpy(sel_ptr(gmem_sel(rsrc[h].hglobal), 0),
           m->img + rsrc[h].off, rsrc[h].len);
    rsrc[h].usage = 1;
    return rsrc[h].hglobal;
}

static uint32_t r_LockResource(Cpu *c, Args *a)
{
    uint16_t hmem = arg_word(a);
    uint16_t sel = gmem_sel(hmem);
    (void)c;
    if (!sel) return 0;
    gmem_lock(hmem);
    set_reg16(c, R_CX, sel);
    return SEGPTR(sel, 0);
}

static uint32_t r_FreeResource(Cpu *c, Args *a)
{
    uint16_t hmem = arg_word(a);
    int i;
    (void)c;
    for (i = 1; i < MAX_RES; i++)
        if (rsrc[i].used && rsrc[i].hglobal == hmem) {
            if (--rsrc[i].usage > 0) return 0;
            gmem_free(hmem);
            rsrc[i].hglobal = 0;
            return 0;                    /* FreeResource returns FALSE on success */
        }
    gmem_free(hmem);
    return 0;
}

static uint32_t r_AllocResource(Cpu *c, Args *a)
{
    uint16_t hinst = arg_word(a);
    uint16_t h = arg_word(a);
    uint32_t size = arg_long(a);
    (void)c; (void)hinst;
    if (!h || h >= MAX_RES || !rsrc[h].used) return 0;
    if (size < rsrc[h].len) size = rsrc[h].len;
    return gmem_alloc(MEM_MOVEABLE, size);
}

/* AccessResource hands back a DOS handle already positioned at the data, which
   the game reads with _lread for the bitmaps that exceed 64 KB.

   The handle is a window onto the module image, not a reopened file.  This
   used to be the one place a resource offset escaped as a file offset, and so
   the one place that had to know how many bytes of us came first when the
   module was appended to our own executable.  Reading it out of the image we
   are already holding is shorter, and it is the only thing that can work once
   that executable can carry the module compressed, where there is no byte
   range to seek to at all. */
static uint32_t r_AccessResource(Cpu *c, Args *a)
{
    uint16_t hinst = arg_word(a);
    uint16_t h = arg_word(a);
    (void)c; (void)hinst;
    if (!h || h >= MAX_RES || !rsrc[h].used) return 0xFFFF;
    return dos_open_mem(task.mod->img + rsrc[h].off, rsrc[h].len);
}

void api_res_register(void)
{
    api_bind("KERNEL",  60, r_FindResource);
    api_bind("KERNEL",  61, r_LoadResource);
    api_bind("KERNEL",  62, r_LockResource);
    api_bind("KERNEL",  63, r_FreeResource);
    api_bind("KERNEL",  64, r_AccessResource);
    api_bind("KERNEL",  65, r_SizeofResource);
    api_bind("KERNEL",  66, r_AllocResource);
}
