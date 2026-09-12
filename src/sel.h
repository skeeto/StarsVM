/* sel.h - Win16 selector arena.
 *
 * Win16 runs in protected mode: a segment register holds an LDT selector, not a
 * paragraph address.  We model the LDT with a flat reserved arena in which the
 * selector index directly picks a 64 KB slot:
 *
 *     index = sel >> 3          host = arena + (index << 16) + off
 *
 * Real Win16 selectors are LDT/RPL-3, so sel = (index << 3) | 7.  Adding 8 to a
 * selector therefore advances exactly 64 KB, which is what __AHINCR == 8 and
 * __AHSHIFT == 3 mean.  Guest huge-pointer arithmetic then needs no special case
 * and a >64 KB block is just a run of consecutive selectors.
 */
#ifndef SEL_H
#define SEL_H

#include <stdint.h>
#include <stddef.h>

#define SEL_SLOTS   4096u              /* selector indices we can hand out      */
#define SEL_SLOT    0x10000u           /* address space reserved per index      */
#define SEL_ARENA   ((size_t)SEL_SLOTS * SEL_SLOT)

#define SEL_MAKE(i)  (uint16_t)(((i) << 3) | 7)
#define SEL_INDEX(s) (unsigned)((s) >> 3)

enum {                                  /* sel_desc.kind */
    SK_FREE = 0,
    SK_CODE,
    SK_DATA
};

typedef struct {
    uint32_t limit;     /* byte limit, i.e. size-1 (may exceed 64K for huge blocks) */
    uint8_t  kind;
    uint8_t  count;     /* selectors in this block (1 unless huge)                  */
    uint8_t  head;      /* nonzero if this index starts the block                   */
    uint8_t  pad;
} SelDesc;

extern uint8_t *sel_arena;
extern SelDesc  sel_tab[SEL_SLOTS];

/* Nonzero where sel_tab[i].kind is not SK_FREE.  It says nothing sel_tab does
   not, and exists only so the check in sel_ptr - which every guest read and
   write pays - touches one byte in a 4 KB array rather than one field of an
   8-byte descriptor in a 32 KB one.  Around 45 selectors are live at a time, so
   in this form they share a single cache line.  Maintained by sel_alloc and
   sel_free, which are the only two places kind changes. */
extern uint8_t  sel_live[SEL_SLOTS];

int      sel_init(void);
void     sel_shutdown(void);

/* Allocate `count` consecutive indices covering `size` bytes; commits memory.
   Returns the first selector, or 0 on failure. */
uint16_t sel_alloc(uint32_t size, int kind);
void     sel_free(uint16_t sel);

/* Fault reporting: set by sel_bad() when a translation is out of range. */
extern int sel_fault;
void sel_report_fault(uint16_t sel, uint16_t off, const char *what);

/* Translate a far pointer.  Never returns NULL: an invalid selector resolves to
   a quarantined guard page slot so a bad access is loud but contained. */
/* Out of line and out of the way: having the report inside sel_ptr was what
   gave the fast path a stack frame.  Index 0 is never allocated, so returning
   the guard slot keeps a bad access contained. */
uint8_t *sel_bad(uint16_t sel, uint16_t off);

static inline uint8_t *sel_ptr(uint16_t sel, uint16_t off)
{
    unsigned i = SEL_INDEX(sel);
    if (i >= SEL_SLOTS || !sel_live[i]) return sel_bad(sel, off);
    return sel_arena + ((size_t)i << 16) + off;
}

static inline uint8_t  sel_rd8 (uint16_t s, uint16_t o) { return *sel_ptr(s,o); }
static inline uint16_t sel_rd16(uint16_t s, uint16_t o) { uint8_t *p = sel_ptr(s,o); return (uint16_t)(p[0] | (p[1]<<8)); }
static inline uint32_t sel_rd32(uint16_t s, uint16_t o) { uint8_t *p = sel_ptr(s,o); return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24); }
static inline void sel_wr8 (uint16_t s, uint16_t o, uint8_t  v) { *sel_ptr(s,o) = v; }
static inline void sel_wr16(uint16_t s, uint16_t o, uint16_t v) { uint8_t *p = sel_ptr(s,o); p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
static inline void sel_wr32(uint16_t s, uint16_t o, uint32_t v) { uint8_t *p = sel_ptr(s,o); p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }

/* A guest far pointer packed as Win16 does: selector in the high word. */
#define SEGPTR(sel, off)   (((uint32_t)(sel) << 16) | (uint16_t)(off))
#define SEGPTR_SEL(p)      ((uint16_t)((p) >> 16))
#define SEGPTR_OFF(p)      ((uint16_t)(p))

/* Flat pointer for a packed far pointer; NULL far pointer stays NULL. */
static inline void *sel_map(uint32_t segptr)
{
    if (!segptr) return NULL;
    return sel_ptr(SEGPTR_SEL(segptr), SEGPTR_OFF(segptr));
}

#endif
