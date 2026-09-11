/* gmem.h - reading and writing guest memory through a far pointer.
 *
 * Every one of these takes a SEGPTR, because that is what a Win16 argument is.
 * They are here rather than duplicated per api_* file so that the newer code
 * has one place to fix if a convention turns out to be wrong.
 */
#ifndef GMEM_H
#define GMEM_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "sel.h"

/* Copy a NUL-terminated guest string into `buf`, always terminating. */
static inline char *g_str(uint32_t segptr, char *buf, size_t n)
{
    uint16_t sel = SEGPTR_SEL(segptr), off = SEGPTR_OFF(segptr);
    size_t i = 0;

    if (!segptr || !n) { if (n) buf[0] = 0; return buf; }
    while (i + 1 < n) {
        uint8_t ch = sel_rd8(sel, (uint16_t)(off + i));
        if (!ch) break;
        buf[i++] = (char)ch;
    }
    buf[i] = 0;
    return buf;
}

/* Write a NUL-terminated string into the guest, truncating to `n` bytes
   including the terminator.  Returns the number of characters written, which is
   what the Win16 functions that do this report. */
static inline unsigned g_puts(uint32_t segptr, const char *s, size_t n)
{
    uint16_t sel = SEGPTR_SEL(segptr), off = SEGPTR_OFF(segptr);
    size_t i = 0;

    if (!segptr || !n) return 0;
    while (s[i] && i + 1 < n) {
        sel_wr8(sel, (uint16_t)(off + i), (uint8_t)s[i]);
        i++;
    }
    sel_wr8(sel, (uint16_t)(off + i), 0);
    return (unsigned)i;
}

/* Copy raw bytes in either direction. */
static inline void g_read(uint32_t segptr, void *dst, size_t n)
{
    uint16_t sel = SEGPTR_SEL(segptr), off = SEGPTR_OFF(segptr);
    uint8_t *d = (uint8_t *)dst;
    size_t i;
    for (i = 0; i < n; i++) d[i] = sel_rd8(sel, (uint16_t)(off + i));
}

static inline void g_write(uint32_t segptr, const void *src, size_t n)
{
    uint16_t sel = SEGPTR_SEL(segptr), off = SEGPTR_OFF(segptr);
    const uint8_t *s = (const uint8_t *)src;
    size_t i;
    for (i = 0; i < n; i++) sel_wr8(sel, (uint16_t)(off + i), s[i]);
}

#endif
