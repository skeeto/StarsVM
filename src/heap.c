#include "heap.h"
#include "sel.h"
#include "log.h"

#include <string.h>

/* ------------------------------------------------------------- global heap */

/* One entry per selector index we have handed out as a global block. */
static struct {
    uint32_t size;
    uint8_t  used;
    uint8_t  lock;
    uint8_t  moveable;
} gmem[SEL_SLOTS];

uint16_t gmem_sel(uint16_t handle)
{
    return handle ? (uint16_t)(handle | 7) : 0;
}

uint16_t gmem_alloc(uint16_t flags, uint32_t bytes)
{
    uint16_t sel;
    unsigned i;

    if (bytes == 0) bytes = 1;
    sel = sel_alloc(bytes, SK_DATA);
    if (!sel) {
        log_msg("gmem: out of selectors for %u bytes\n", bytes);
        return 0;
    }
    i = SEL_INDEX(sel);
    gmem[i].size = bytes;
    gmem[i].used = 1;
    gmem[i].lock = 0;
    gmem[i].moveable = (flags & MEM_MOVEABLE) != 0;
    /* sel_alloc already zeroes, so GMEM_ZEROINIT needs nothing extra. */
    return gmem[i].moveable ? (uint16_t)(sel - 1) : sel;
}

uint16_t gmem_realloc(uint16_t handle, uint32_t bytes, uint16_t flags)
{
    uint16_t sel = gmem_sel(handle);
    unsigned i = SEL_INDEX(sel);
    uint16_t nh;
    uint32_t keep;

    if (!handle || i >= SEL_SLOTS || !gmem[i].used) return 0;
    if (flags & MEM_MODIFY) {              /* only changing the flags */
        gmem[i].moveable = (flags & MEM_MOVEABLE) != 0;
        return handle;
    }
    if (bytes == 0) bytes = 1;
    if (bytes <= gmem[i].size) {
        gmem[i].size = bytes;              /* shrink in place */
        return handle;
    }
    nh = gmem_alloc(flags, bytes);
    if (!nh) return 0;
    keep = gmem[i].size;
    memcpy(sel_ptr(gmem_sel(nh), 0), sel_ptr(sel, 0), keep);
    gmem_free(handle);
    return nh;
}

uint16_t gmem_free(uint16_t handle)
{
    uint16_t sel = gmem_sel(handle);
    unsigned i = SEL_INDEX(sel);

    if (!handle || i >= SEL_SLOTS || !gmem[i].used) return handle;  /* failure */
    memset(&gmem[i], 0, sizeof gmem[i]);
    sel_free(sel);
    return 0;
}

uint32_t gmem_size(uint16_t handle)
{
    unsigned i = SEL_INDEX(gmem_sel(handle));
    return (handle && i < SEL_SLOTS && gmem[i].used) ? gmem[i].size : 0;
}

int gmem_lock(uint16_t handle)
{
    unsigned i = SEL_INDEX(gmem_sel(handle));
    if (!handle || i >= SEL_SLOTS || !gmem[i].used) return 0;
    if (gmem[i].lock < 0xFF) gmem[i].lock++;
    return gmem[i].lock;
}

int gmem_unlock(uint16_t handle)
{
    unsigned i = SEL_INDEX(gmem_sel(handle));
    if (!handle || i >= SEL_SLOTS || !gmem[i].used) return 0;
    if (gmem[i].lock) gmem[i].lock--;
    return gmem[i].lock;
}

/* -------------------------------------------------------------- local heap */

/* Block layout in guest memory, 4-byte header:
       WORD size   total bytes including this header
       WORD used   1 if allocated
   A handle is the offset of the payload, i.e. header offset + 4. */

#define LB_HDR 4

static uint16_t heap_sel, heap_base, heap_end;

uint16_t lmem_base(void) { return heap_base; }

void lmem_init(uint16_t sel, uint16_t base, uint16_t size)
{
    heap_sel  = sel;
    heap_base = (uint16_t)((base + 3) & ~3u);
    heap_end  = (uint16_t)(heap_base + size);

    /* One free block covering everything. */
    sel_wr16(sel, heap_base, (uint16_t)(heap_end - heap_base));
    sel_wr16(sel, (uint16_t)(heap_base + 2), 0);
}

static uint16_t blk_size(uint16_t off) { return sel_rd16(heap_sel, off); }
static uint16_t blk_used(uint16_t off) { return sel_rd16(heap_sel, (uint16_t)(off + 2)); }
static void blk_set(uint16_t off, uint16_t size, uint16_t used)
{
    sel_wr16(heap_sel, off, size);
    sel_wr16(heap_sel, (uint16_t)(off + 2), used);
}

/* Merge a free block with any free blocks that follow it. */
static void blk_coalesce(uint16_t off)
{
    uint16_t size = blk_size(off);
    while (off + size < heap_end) {
        uint16_t nxt = (uint16_t)(off + size);
        if (blk_used(nxt)) break;
        size = (uint16_t)(size + blk_size(nxt));
        blk_set(off, size, 0);
    }
}

uint16_t lmem_alloc(uint16_t sel, uint16_t flags, uint16_t bytes)
{
    uint16_t off, need;

    if (sel != heap_sel || !heap_base) return 0;
    need = (uint16_t)((bytes + LB_HDR + 3) & ~3u);
    if (need < LB_HDR + 4) need = LB_HDR + 4;

    for (off = heap_base; off < heap_end; off = (uint16_t)(off + blk_size(off))) {
        uint16_t size = blk_size(off);
        if (size == 0) break;                      /* corrupt; stop rather than spin */
        if (blk_used(off)) continue;
        blk_coalesce(off);
        size = blk_size(off);
        if (size < need) continue;
        if (size >= need + LB_HDR + 4) {           /* split off the remainder */
            blk_set((uint16_t)(off + need), (uint16_t)(size - need), 0);
            blk_set(off, need, 1);
        } else {
            blk_set(off, size, 1);
        }
        if (flags & MEM_ZEROINIT)
            memset(sel_ptr(sel, (uint16_t)(off + LB_HDR)), 0,
                   blk_size(off) - LB_HDR);
        return (uint16_t)(off + LB_HDR);
    }
    log_msg("lmem: out of local heap (wanted %u bytes)\n", bytes);
    return 0;
}

uint16_t lmem_size(uint16_t sel, uint16_t handle)
{
    uint16_t off;
    if (sel != heap_sel || handle < heap_base + LB_HDR) return 0;
    off = (uint16_t)(handle - LB_HDR);
    return (uint16_t)(blk_size(off) - LB_HDR);
}

uint16_t lmem_free(uint16_t sel, uint16_t handle)
{
    uint16_t off;
    if (sel != heap_sel || handle < heap_base + LB_HDR) return handle;
    off = (uint16_t)(handle - LB_HDR);
    blk_set(off, blk_size(off), 0);
    blk_coalesce(off);
    return 0;
}

uint16_t lmem_realloc(uint16_t sel, uint16_t handle, uint16_t bytes, uint16_t flags)
{
    uint16_t old, nh, off;

    if (!handle) return lmem_alloc(sel, flags, bytes);
    if (sel != heap_sel) return 0;
    old = lmem_size(sel, handle);
    off = (uint16_t)(handle - LB_HDR);

    if (bytes <= old) {
        /* Shrinking: give the tail back if there is enough for a real block. */
        uint16_t need = (uint16_t)((bytes + LB_HDR + 3) & ~3u);
        uint16_t size = blk_size(off);
        if (need >= LB_HDR + 4 && size >= need + LB_HDR + 4) {
            blk_set((uint16_t)(off + need), (uint16_t)(size - need), 0);
            blk_set(off, need, 1);
            blk_coalesce((uint16_t)(off + need));
        }
        return handle;
    }

    /* Try to grow into an adjacent free block before moving. */
    {
        uint16_t size = blk_size(off);
        uint16_t need = (uint16_t)((bytes + LB_HDR + 3) & ~3u);
        uint16_t nxt = (uint16_t)(off + size);
        if (nxt < heap_end && !blk_used(nxt)) {
            uint16_t merged;
            blk_coalesce(nxt);
            merged = (uint16_t)(size + blk_size(nxt));
            if (merged >= need) {
                if (merged >= need + LB_HDR + 4) {
                    blk_set((uint16_t)(off + need), (uint16_t)(merged - need), 0);
                    blk_set(off, need, 1);
                } else {
                    blk_set(off, merged, 1);
                }
                if (flags & MEM_ZEROINIT)
                    memset(sel_ptr(sel, (uint16_t)(handle + old)), 0,
                           (size_t)(blk_size(off) - LB_HDR - old));
                return handle;
            }
        }
    }

    nh = lmem_alloc(sel, flags, bytes);
    if (!nh) return 0;
    memcpy(sel_ptr(sel, nh), sel_ptr(sel, handle), old);
    lmem_free(sel, handle);
    return nh;
}
