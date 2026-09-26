/* ne_file.c - getting an NE image off disk, for the emulator.
 *
 * Kept apart from ne.c so that the loader proper works on bytes already in
 * memory and never touches a file: the library build (src/unity_lib.c) is
 * handed the module by its caller and has no file system to read one from.
 * Everything here is the emulator's - the wide paths, the module appended to
 * our own executable, the packed payload - and none of it is the library's.
 */
#include "ne.h"
#include "pack.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* The loader's host-side tables, which ne_close frees. */
void *ne_alloc(size_t n)
{
    return calloc(1, n);
}

static uint16_t nf_rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t nf_rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Read a whole file.  Returns the buffer and sets *len, or NULL. */
static uint8_t *slurp_file(const wchar_t *path, uint32_t *len)
{
    FILE *f = _wfopen(path, L"rb");
    uint8_t *buf;
    long n;

    if (!f) { log_msg("ne: cannot open %s\n", log_wide(path)); return NULL; }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0x40) { fclose(f); log_msg("ne: %s too small\n", log_wide(path)); return NULL; }
    buf = malloc((size_t)n);
    if (!buf) { fclose(f); log_msg("ne: out of memory\n"); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(buf);
        log_msg("ne: short read on %s\n", log_wide(path));
        return NULL;
    }
    fclose(f);
    *len = (uint32_t)n;
    return buf;
}

/* Does an NE module start at `at`?  Checked hard enough that a chance "MZ" in a
   symbol table cannot pass: the DOS header must point at an NE signature, and
   that header's own tables must lie inside the file. */
static int looks_like_ne(const uint8_t *d, uint32_t len, uint32_t at)
{
    uint32_t lfanew, cseg, segtab;

    /* e_lfanew is an arbitrary dword from whatever bytes were concatenated, so
       every bound is written as a subtraction from `len` rather than a sum
       against it: `at + lfanew + 0x40 > len` wraps for a hostile lfanew and
       admits an index far outside the buffer.  Each subtraction below is safe
       because the test before it has already established the room it needs. */
    if (at + 0x40 > len || d[at] != 0x4D || d[at + 1] != 0x5A) return 0;
    lfanew = nf_rd32(d + at + 0x3C);
    if (lfanew < 0x40 || lfanew > len - at - 0x40) return 0;
    if (d[at + lfanew] != 0x4E || d[at + lfanew + 1] != 0x45) return 0;

    cseg   = nf_rd16(d + at + lfanew + 0x1C);
    segtab = nf_rd16(d + at + lfanew + 0x22);
    if (cseg == 0 || cseg > 4096) return 0;
    if (segtab > len - at - lfanew) return 0;
    if (cseg * 8u > len - at - lfanew - segtab) return 0;
    return 1;
}

int ne_open(NeModule *m, const wchar_t *path)
{
    memset(m, 0, sizeof *m);
    _snwprintf(m->path, sizeof m->path / sizeof *m->path - 1, L"%ls", path);
    m->path[sizeof m->path / sizeof *m->path - 1] = 0;

    m->img = slurp_file(path, &m->imglen);
    if (!m->img) return 0;
    return ne_parse(m);
}

int ne_open_appended(NeModule *m, const wchar_t *path)
{
    uint8_t *whole;
    uint32_t len, at, found = 0;
    PackInfo pi;

    memset(m, 0, sizeof *m);
    whole = slurp_file(path, &len);
    if (!whole) return 0;

    _snwprintf(m->path, sizeof m->path / sizeof *m->path - 1, L"%ls", path);
    m->path[sizeof m->path / sizeof *m->path - 1] = 0;

    /* A packed payload first, since that is what the packer produces and what
       a release carries.  It is found by its own trailer rather than by the
       scan below, and once decoded there is no file offset to speak of - the
       image exists only in memory, which is why nothing downstream is allowed
       to want one.  See src/pack.h. */
    if (pack_find(whole, len, &pi)) {
        uint8_t *img = malloc(pi.rawlen);
        if (!img) {
            log_msg("ne: no room for a %u-byte module\n", pi.rawlen);
            free(whole);
            return 0;
        }
        if (!pack_decode(whole, &pi, img)) { free(img); free(whole); return 0; }
        free(whole);
        m->img    = img;
        m->imglen = pi.rawlen;
        if (!ne_parse(m)) { free(img); m->img = NULL; return 0; }
        log_msg("Unpacked the module appended to this executable:"
                " %u bytes from %u\n", pi.rawlen, pi.complen);
        return 1;
    }

    /* Otherwise a plainly appended one, which `cat` still produces and which
       stays supported: offset 0 is our own PE - it opens "MZ" too, but its
       e_lfanew points at "PE\0\0".  Anything after that which passes
       looks_like_ne is the payload; there is only ever one, and the scan is
       over a file already in the page cache, so it costs nothing worth
       measuring. */
    for (at = 1; at + 0x40 <= len; at++) {
        if (whole[at] == 0x4D && looks_like_ne(whole, len, at)) { found = at; break; }
    }
    if (!found) { free(whole); return 0; }

    /* Slide the payload to the front so every offset inside the module stays
       relative to the image and ne_close still has one pointer to free. */
    memmove(whole, whole + found, len - found);
    m->img    = whole;
    m->imglen = len - found;

    if (!ne_parse(m)) { free(whole); m->img = NULL; return 0; }
    log_msg("Running the module appended to this executable at offset %u\n",
            (unsigned)found);
    return 1;
}

void ne_close(NeModule *m)
{
    free(m->img);     m->img = NULL;
    free(m->seg);     m->seg = NULL;
    free(m->modname); m->modname = NULL;
}
