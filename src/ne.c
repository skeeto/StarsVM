#include "ne.h"
#include "sel.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
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
    lfanew = rd32(d + at + 0x3C);
    if (lfanew < 0x40 || lfanew > len - at - 0x40) return 0;
    if (d[at + lfanew] != 0x4E || d[at + lfanew + 1] != 0x45) return 0;

    cseg   = rd16(d + at + lfanew + 0x1C);
    segtab = rd16(d + at + lfanew + 0x22);
    if (cseg == 0 || cseg > 4096) return 0;
    if (segtab > len - at - lfanew) return 0;
    if (cseg * 8u > len - at - lfanew - segtab) return 0;
    return 1;
}

static int ne_parse(NeModule *m)
{
    uint32_t lfanew;
    const uint8_t *h;
    unsigned i;

    if (m->img[0] != 0x4D || m->img[1] != 0x5A) {
        log_msg("ne: not an MZ file\n");
        return 0;
    }
    lfanew = rd32(m->img + 0x3C);
    if (lfanew + 0x40 > m->imglen) { log_msg("ne: bad e_lfanew\n"); return 0; }
    h = m->img + lfanew;
    if (h[0] != 0x4E || h[1] != 0x45) { log_msg("ne: not an NE file\n"); return 0; }
    m->hdr = lfanew;

    m->enttab   = rd16(h + 0x04);
    m->cbenttab = rd16(h + 0x06);
    m->flags    = rd16(h + 0x0C);
    m->autodata = rd16(h + 0x0E);
    m->heap     = rd16(h + 0x10);
    m->stack    = rd16(h + 0x12);
    m->csip     = rd32(h + 0x14);
    m->sssp     = rd32(h + 0x18);
    m->cseg     = rd16(h + 0x1C);
    m->cmod     = rd16(h + 0x1E);
    m->segtab   = rd16(h + 0x22);
    m->rsrctab  = rd16(h + 0x24);
    m->restab   = rd16(h + 0x26);
    m->modtab   = rd16(h + 0x28);
    m->imptab   = rd16(h + 0x2A);
    m->nrestab  = rd32(h + 0x2C);
    m->cmovent  = rd16(h + 0x30);
    m->align    = rd16(h + 0x32);
    m->expver   = rd16(h + 0x3E);
    if (m->align == 0) m->align = 9;

    /* Segment table: 8 bytes per entry, offset relative to the NE header. */
    m->seg = calloc(m->cseg ? m->cseg : 1, sizeof *m->seg);
    if (!m->seg) { log_msg("ne: out of memory\n"); return 0; }
    for (i = 0; i < m->cseg; i++) {
        const uint8_t *s = m->img + m->hdr + m->segtab + i * 8;
        NeSeg *g = &m->seg[i];
        uint32_t min;

        g->sector   = rd16(s + 0);
        g->length   = rd16(s + 2);
        g->flags    = rd16(s + 4);
        g->minalloc = rd16(s + 6);
        g->filepos  = (uint32_t)g->sector << m->align;
        g->size     = g->length ? g->length : 0x10000u;
        min = g->minalloc ? g->minalloc : 0x10000u;
        if (min > g->size) g->size = min;
    }

    /* Module reference table indexes the imported names table, whose entries
       are Pascal strings. */
    m->nmod = m->cmod;
    m->modname = calloc(m->cmod ? m->cmod : 1, sizeof *m->modname);
    if (!m->modname) { log_msg("ne: out of memory\n"); return 0; }
    for (i = 0; i < m->cmod; i++) {
        uint16_t off = rd16(m->img + m->hdr + m->modtab + i * 2);
        const uint8_t *p = m->img + m->hdr + m->imptab + off;
        unsigned len = p[0];
        if (len > sizeof m->modname[0] - 1) len = sizeof m->modname[0] - 1;
        memcpy(m->modname[i], p + 1, len);
        m->modname[i][len] = 0;
    }

    /* Resident name table: the first record is the module name. */
    {
        const uint8_t *p = m->img + m->hdr + m->restab;
        unsigned len = p[0];
        if (len > sizeof m->name - 1) len = sizeof m->name - 1;
        memcpy(m->name, p + 1, len);
        m->name[len] = 0;
    }
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

    memset(m, 0, sizeof *m);
    whole = slurp_file(path, &len);
    if (!whole) return 0;

    /* Offset 0 is our own PE - it opens "MZ" too, but its e_lfanew points at
       "PE\0\0".  Anything after that which passes looks_like_ne is the payload;
       there is only ever one, and the scan is over a file already in the page
       cache, so it costs nothing worth measuring. */
    for (at = 1; at + 0x40 <= len; at++) {
        if (whole[at] == 0x4D && looks_like_ne(whole, len, at)) { found = at; break; }
    }
    if (!found) { free(whole); return 0; }

    /* Slide the payload to the front so every offset inside the module stays
       relative to the image and ne_close still has one pointer to free. */
    memmove(whole, whole + found, len - found);
    m->img    = whole;
    m->imglen = len - found;
    _snwprintf(m->path, sizeof m->path / sizeof *m->path - 1, L"%ls", path);
    m->path[sizeof m->path / sizeof *m->path - 1] = 0;

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

/* ---------------------------------------------------------------- entry table */

/* Bundle stream: [BYTE count][BYTE type] followed by count entries.
     type 0    - a gap of `count` unused ordinals
     type 0xFF - moveable, 6 bytes: [flags][CD 3F][segno][off:2]
     otherwise - fixed in segment `type`, 3 bytes: [flags][off:2] */
static int entry_lookup(NeModule *m, uint16_t ordinal, uint8_t *segno,
                        uint16_t *off, uint8_t *flags)
{
    const uint8_t *p   = m->img + m->hdr + m->enttab;
    const uint8_t *end = p + m->cbenttab;
    uint16_t ord = 1;

    while (p + 2 <= end) {
        unsigned count = p[0], type = p[1], i;
        if (count == 0) break;
        p += 2;
        if (type == 0) { ord = (uint16_t)(ord + count); continue; }
        for (i = 0; i < count; i++) {
            uint8_t  fl, sg;
            uint16_t o;
            if (type == 0xFF) {
                if (p + 6 > end) return 0;
                fl = p[0]; sg = p[3]; o = rd16(p + 4); p += 6;
            } else {
                if (p + 3 > end) return 0;
                fl = p[0]; sg = (uint8_t)type; o = rd16(p + 1); p += 3;
            }
            if (ord == ordinal) {
                *segno = sg;
                *off = o;
                if (flags) *flags = fl;
                return 1;
            }
            ord++;
        }
    }
    return 0;
}

uint32_t ne_entry_point(NeModule *m, uint16_t ordinal)
{
    uint8_t  segno, flags;
    uint16_t off;
    NeSeg   *s;

    if (!entry_lookup(m, ordinal, &segno, &off, &flags)) return 0;
    if (segno == 0xFE) return SEGPTR(0xFFFF, off);   /* absolute value / equate */
    s = ne_seg(m, segno);
    if (!s || !s->sel) return 0;
    return SEGPTR(s->sel, off);
}

uint16_t ne_ordinal_by_name(NeModule *m, const char *name)
{
    const uint8_t *p = m->img + m->hdr + m->restab;
    size_t want = strlen(name);
    int first = 1;

    while (*p) {
        unsigned len = *p;
        uint16_t ord = rd16(p + 1 + len);
        if (!first && len == want &&
            _strnicmp((const char *)p + 1, name, len) == 0)
            return ord;
        first = 0;
        p += 1 + len + 2;
    }
    return 0;
}

/* ------------------------------------------------------------------ resources */

const char *ne_resource_type_name(NeModule *m, uint16_t tid, char *buf, int len)
{
    if (tid & 0x8000) {
        snprintf(buf, (size_t)len, "#%u", tid & 0x7FFF);
    } else {
        const uint8_t *p = m->img + m->hdr + m->rsrctab + tid;
        int n = p[0];
        if (n > len - 1) n = len - 1;
        memcpy(buf, p + 1, (size_t)n);
        buf[n] = 0;
    }
    return buf;
}

/* type and name are 0x8000|n for a numeric id, otherwise a byte offset into the
   resource table naming a Pascal string. */
int ne_find_resource(NeModule *m, uint32_t type, uint32_t name, NeResource *out)
{
    const uint8_t *rt;
    unsigned shift;
    const uint8_t *p;

    if (!m->rsrctab) return 0;
    rt = m->img + m->hdr + m->rsrctab;
    shift = rd16(rt);
    p = rt + 2;

    for (;;) {
        uint16_t tid = rd16(p);
        uint16_t cnt;
        unsigned i;

        if (tid == 0) break;
        cnt = rd16(p + 2);
        p += 8;
        if (tid == (uint16_t)type) {
            for (i = 0; i < cnt; i++) {
                const uint8_t *e = p + i * 12;
                if (rd16(e + 6) == (uint16_t)name) {
                    out->off   = (uint32_t)rd16(e + 0) << shift;
                    out->len   = (uint32_t)rd16(e + 2) << shift;
                    out->flags = rd16(e + 4);
                    out->id    = rd16(e + 6);
                    return 1;
                }
            }
        }
        p += (size_t)cnt * 12;
    }
    return 0;
}

/* ---------------------------------------------------------------- relocations */

struct relctx {
    NeModule  *m;
    NeImportFn resolve;
    void      *user;
    int        errors;
};

static uint32_t resolve_target(struct relctx *c, unsigned rtype,
                               uint16_t t1, uint16_t t2)
{
    NeModule *m = c->m;

    switch (rtype) {
    case NERT_INTERNAL:
        if ((t1 & 0xFF) == 0xFF)          /* moveable: t2 is our own ordinal */
            return ne_entry_point(m, t2);
        else {
            NeSeg *s = ne_seg(m, t1);
            if (!s || !s->sel) {
                log_msg("ne: internal reference to bad segment %u\n", t1);
                c->errors++;
                return 0;
            }
            return SEGPTR(s->sel, t2);
        }

    case NERT_ORDINAL: {
        const char *mod = (t1 >= 1 && t1 <= m->cmod) ? m->modname[t1 - 1] : "?";
        return c->resolve(mod, t2, c->user);
    }

    case NERT_NAME: {
        const char *mod = (t1 >= 1 && t1 <= m->cmod) ? m->modname[t1 - 1] : "?";
        const uint8_t *p = m->img + m->hdr + m->imptab + t2;
        char nm[64];
        unsigned len = p[0];
        if (len > sizeof nm - 1) len = sizeof nm - 1;
        memcpy(nm, p + 1, len);
        nm[len] = 0;
        log_msg("ne: import by name %s.%s is not supported\n", mod, nm);
        c->errors++;
        return 0;
    }

    default:
        return 0;
    }
}

static void patch_at(uint16_t sel, uint16_t off, unsigned atype, uint32_t addr)
{
    switch (atype) {
    case NER_LOBYTE:
        sel_wr8(sel, off, (uint8_t)addr);
        break;
    case NER_OFFSET:
        sel_wr16(sel, off, SEGPTR_OFF(addr));
        break;
    case NER_SELECTOR:
        sel_wr16(sel, off, SEGPTR_SEL(addr));
        break;
    case NER_FARADDR:
        sel_wr16(sel, off, SEGPTR_OFF(addr));
        sel_wr16(sel, (uint16_t)(off + 2), SEGPTR_SEL(addr));
        break;
    default:
        break;
    }
}

static int apply_relocations(struct relctx *c, unsigned segno)
{
    NeModule *m = c->m;
    NeSeg *s = ne_seg(m, segno);
    uint32_t pos;
    unsigned count, i;
    const uint8_t *rec;

    if (!(s->flags & NES_RELOCINFO)) return 1;
    pos = s->filepos + (s->length ? s->length : 0x10000u);
    if (pos + 2 > m->imglen) {
        log_msg("ne: segment %u relocation table past end of file\n", segno);
        return 0;
    }
    count = rd16(m->img + pos);
    rec = m->img + pos + 2;
    if (pos + 2 + (size_t)count * 8 > m->imglen) {
        log_msg("ne: segment %u relocation table truncated\n", segno);
        return 0;
    }

    for (i = 0; i < count; i++, rec += 8) {
        unsigned atype = rec[0] & 0x7F;   /* the high bit is sometimes set; ignore it */
        unsigned tbyte = rec[1];
        unsigned rtype = tbyte & 3;
        int additive   = (tbyte & NERT_ADDITIVE) != 0;
        uint16_t off   = rd16(rec + 2);
        uint16_t t1    = rd16(rec + 4);
        uint16_t t2    = rd16(rec + 6);
        uint32_t addr;

        /* Floating-point loader fixups.  The bytes already in the file are real
           x87 instructions, so there is nothing to patch: we emulate a real FPU
           rather than the WIN87EM software emulator. */
        if (rtype == NERT_OSFIXUP) continue;

        addr = resolve_target(c, rtype, t1, t2);

        if (additive) {
            /* Additive records patch exactly one site and never chain. */
            switch (atype) {
            case NER_LOBYTE:
                sel_wr8(s->sel, off,
                        (uint8_t)(sel_rd8(s->sel, off) + (uint8_t)addr));
                break;
            case NER_OFFSET:
                sel_wr16(s->sel, off,
                         (uint16_t)(sel_rd16(s->sel, off) + SEGPTR_OFF(addr)));
                break;
            case NER_FARADDR:
                sel_wr16(s->sel, off,
                         (uint16_t)(sel_rd16(s->sel, off) + SEGPTR_OFF(addr)));
                sel_wr16(s->sel, (uint16_t)(off + 2), SEGPTR_SEL(addr));
                break;
            case NER_SELECTOR:
                /* Some linkers emit additive selector records with offset 0. */
                if (sel_rd16(s->sel, off) == 0)
                    sel_wr16(s->sel, off, SEGPTR_SEL(addr));
                break;
            default:
                log_msg("ne: segment %u unsupported additive address type %u\n",
                        segno, atype);
                c->errors++;
                break;
            }
            continue;
        }

        /* Non-additive records name only the FIRST site.  The word stored there
           is the offset of the next site, and 0xFFFF ends the chain, so the link
           has to be read before it is overwritten. */
        {
            uint16_t cur = off;
            unsigned guard = 0;
            while (cur != 0xFFFF) {
                uint16_t next = sel_rd16(s->sel, cur);
                patch_at(s->sel, cur, atype, addr);
                if (next == cur) break;              /* self-referential link */
                if (next >= s->size) break;          /* link outside the segment */
                if (++guard > 0x10000) {
                    log_msg("ne: segment %u runaway fixup chain at %04X\n",
                            segno, off);
                    c->errors++;
                    break;
                }
                cur = next;
            }
        }
    }
    return 1;
}

/* --------------------------------------------------------------------- loading */

/* Windows rewrites the prolog of exported functions so they see the right DS.
   Stars! does not use that prolog form, but the check costs ten lines and keeps
   the loader correct for other modules. */
static void fixup_prologs(NeModule *m, unsigned segno)
{
    const uint8_t *p   = m->img + m->hdr + m->enttab;
    const uint8_t *end = p + m->cbenttab;
    NeSeg *dg = ne_seg(m, m->autodata);
    NeSeg *s  = ne_seg(m, segno);

    if ((s->flags & NES_DATA) || !m->autodata || !dg || !dg->sel) return;

    while (p + 2 <= end) {
        unsigned count = p[0], type = p[1], i;
        if (count == 0) break;
        p += 2;
        if (type == 0) continue;
        for (i = 0; i < count; i++) {
            uint8_t  fl, sg;
            uint16_t off, w;

            if (type == 0xFF) {
                if (p + 6 > end) return;
                fl = p[0]; sg = p[3]; off = rd16(p + 4); p += 6;
            } else {
                if (p + 3 > end) return;
                fl = p[0]; sg = (uint8_t)type; off = rd16(p + 1); p += 3;
            }
            if (sg != segno) continue;
            if (sel_rd8(s->sel, (uint16_t)(off + 2)) != 0x90) continue;

            w = sel_rd16(s->sel, off);
            if (w == 0x581E) {                    /* push ds; pop ax */
                sel_wr16(s->sel, off, 0x8CD8);    /* -> mov ax, ds   */
                w = 0x8CD8;
            }
            if (w != 0x8CD8) continue;
            if (fl & 2) {                         /* shared data: load DGROUP */
                sel_wr8(s->sel, off, 0xB8);
                sel_wr16(s->sel, (uint16_t)(off + 1), dg->sel);
            } else if ((m->flags & NEF_MULTIPLEDATA) && (fl & 1)) {
                sel_wr16(s->sel, off, 0x9090);    /* keep the caller DS */
            }
        }
    }
}

int ne_load(NeModule *m, NeImportFn resolve, void *user)
{
    struct relctx c;
    unsigned i;

    /* 1. Every segment needs a selector before any relocation runs, because
          relocations reference other segments. */
    for (i = 1; i <= m->cseg; i++) {
        NeSeg *s = ne_seg(m, i);
        uint32_t size = s->size;

        if (i == m->autodata) {
            /* DGROUP carries the stack and the local heap above its data. */
            size += m->stack + m->heap;
            if (size > 0x10000u) {
                log_msg("ne: DGROUP wants %u bytes, clamping to 64K\n", size);
                size = 0x10000u;
            }
        }
        s->sel = sel_alloc(size, (s->flags & NES_DATA) ? SK_DATA : SK_CODE);
        if (!s->sel) {
            log_msg("ne: no selector available for segment %u\n", i);
            return 0;
        }
        s->size = size;
        if (i == m->autodata) m->dgroup_sel = s->sel;
    }

    /* 2. Read the segment bodies. */
    for (i = 1; i <= m->cseg; i++) {
        NeSeg *s = ne_seg(m, i);
        uint32_t len = s->length ? s->length : 0x10000u;

        if (!s->sector) continue;                /* BSS-only segment */
        if (s->filepos + len > m->imglen) {
            log_msg("ne: segment %u data past end of file\n", i);
            return 0;
        }
        if (s->flags & NES_ITERATED) {
            /* Run-length form: [WORD repeat][WORD len][len bytes], repeated. */
            const uint8_t *src = m->img + s->filepos, *end = src + len;
            uint32_t dst = 0;
            while (src + 4 <= end) {
                unsigned rep = rd16(src), n = rd16(src + 2);
                src += 4;
                while (rep--) {
                    if (dst + n > s->size) break;
                    memcpy(sel_ptr(s->sel, 0) + dst, src, n);
                    dst += n;
                }
                src += n;
            }
        } else {
            if (len > s->size) len = s->size;
            memcpy(sel_ptr(s->sel, 0), m->img + s->filepos, len);
        }
    }

    /* 3. Relocate. */
    c.m = m;
    c.resolve = resolve;
    c.user = user;
    c.errors = 0;
    for (i = 1; i <= m->cseg; i++)
        if (!apply_relocations(&c, i)) return 0;

    /* 4. Exported-function prolog fixups. */
    for (i = 1; i <= m->cseg; i++)
        fixup_prologs(m, i);

    if (c.errors)
        log_msg("ne: %d relocation problem(s)\n", c.errors);
    return c.errors == 0;
}
