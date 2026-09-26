/* lib.h - what the library build's own files share, and nothing public.
 *
 * The public interface is stars.h.  This is the glue between libstars.c, which
 * owns the caller's arena and the run, and fs_mem.c and headless.c, which
 * allocate from it and can end the run.
 */
#ifndef LIB_H
#define LIB_H

#include <stddef.h>
#include <stdint.h>

/* Zeroed memory from the arena of the call in progress, aligned for anything.
   Never returns NULL: running out ends the call with STARS_ENOMEM. */
void *lib_alloc(ptrdiff_t size);

/* Grow the allocation at `p`, `old` bytes long, to `size` bytes, moving it
   only when it is not the arena's last.  The new bytes are zero. */
void *lib_grow(void *p, ptrdiff_t old, ptrdiff_t size);

/* End the call in progress with a STARS_E* code.  Does not return. */
void  lib_fail(int err);

/* ---- fs_mem.c: the directory the guest sees ------------------------------- */

void memfs_reset(void);

/* Put a file in the root, by name, over the caller's bytes.  They are
   borrowed, never written: the first write to the file copies it. */
void memfs_add(const char *name, const uint8_t *data, ptrdiff_t len);

/* Walk the directory after the run: the i'th entry, 0 when there are no more.
   A name is as the directory holds it - uppercase, root-relative, with a
   backslash before any directory - and `written` says whether the guest
   created or wrote it. */
int   memfs_entry(int i, const char **name, const uint8_t **data,
                  ptrdiff_t *len, int *isdir, int *written);

/* ---- headless.c: the window system -------------------------------------- */

void headless_reset(void);
void headless_register(void);

/* How many message boxes the guest raised, for telling a run that reported a
   problem from one that did not. */
extern int headless_msgboxes;

#endif
