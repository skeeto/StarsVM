/* heap.h - the Win16 global and local heaps.
 *
 * Global heap: a block is one or more consecutive selectors from the arena.
 * Following the real convention, a FIXED block's handle IS its selector, and a
 * MOVEABLE block's handle is the selector minus one; `handle | 7` recovers the
 * selector either way.  Keeping that convention costs nothing and means guest
 * code doing arithmetic on handles behaves as it expects.
 *
 * Local heap: a near allocator living inside DGROUP.  Stars! imports LocalAlloc,
 * LocalReAlloc, LocalFree and LocalSize but none of LocalLock, LocalUnlock,
 * LocalHandle or LocalFlags, so it only ever uses FIXED blocks - which means a
 * handle is simply a near pointer and no handle table is needed.
 */
#ifndef HEAP_H
#define HEAP_H

#include <stdint.h>

/* GMEM_/LMEM_ flags that matter to us. */
#define MEM_MOVEABLE    0x0002
#define MEM_ZEROINIT    0x0040
#define MEM_MODIFY      0x0080
#define MEM_DISCARDABLE 0x0100

/* ---- global heap ---- */
uint16_t gmem_alloc(uint16_t flags, uint32_t bytes);
uint16_t gmem_realloc(uint16_t handle, uint32_t bytes, uint16_t flags);
uint16_t gmem_free(uint16_t handle);
uint32_t gmem_size(uint16_t handle);
uint16_t gmem_sel(uint16_t handle);
int      gmem_lock(uint16_t handle);      /* returns the new lock count */
int      gmem_unlock(uint16_t handle);

/* ---- local heap, inside one data segment ---- */
void     lmem_init(uint16_t sel, uint16_t base, uint16_t size);
uint16_t lmem_alloc(uint16_t sel, uint16_t flags, uint16_t bytes);
uint16_t lmem_realloc(uint16_t sel, uint16_t handle, uint16_t bytes, uint16_t flags);
uint16_t lmem_free(uint16_t sel, uint16_t handle);
uint16_t lmem_size(uint16_t sel, uint16_t handle);
uint16_t lmem_base(void);

#endif
