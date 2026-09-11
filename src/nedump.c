/* nedump.c - human-readable dump of an NE module, used by --dump.
   This is diagnostic only; nothing here is on the execution path. */

#include "ne.h"
#include "log.h"

#include <stdio.h>
#include <string.h>

static uint16_t dmp_rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static const char *seg_flag_str(uint16_t f, char *buf, size_t n)
{
    snprintf(buf, n, "%s%s%s%s%s%s%s%s",
             (f & NES_DATA)        ? "DATA "        : "CODE ",
             (f & NES_MOVEABLE)    ? "MOVEABLE "    : "",
             (f & NES_SHAREABLE)   ? "SHAREABLE "   : "",
             (f & NES_PRELOAD)     ? "PRELOAD "     : "",
             (f & NES_RELOCINFO)   ? "RELOC "       : "",
             (f & NES_DISCARDABLE) ? "DISCARDABLE " : "",
             (f & NES_ITERATED)    ? "ITERATED "    : "",
             (f & NES_32BIT)       ? "32BIT "       : "");
    return buf;
}

static void dump_relocs(NeModule *m, unsigned segno, int verbose)
{
    NeSeg *s = ne_seg(m, segno);
    uint32_t pos;
    unsigned count, i;
    const uint8_t *rec;
    unsigned nint = 0, nord = 0, nname = 0, nos = 0, nadd = 0;

    if (!(s->flags & NES_RELOCINFO)) return;
    pos = s->filepos + (s->length ? s->length : 0x10000u);
    if (pos + 2 > m->imglen) return;
    count = dmp_rd16(m->img + pos);
    rec = m->img + pos + 2;
    if (pos + 2 + (size_t)count * 8 > m->imglen) return;

    for (i = 0; i < count; i++, rec += 8) {
        unsigned tbyte = rec[1], rtype = tbyte & 3;
        if (tbyte & NERT_ADDITIVE) nadd++;
        switch (rtype) {
        case NERT_INTERNAL: nint++;  break;
        case NERT_ORDINAL:  nord++;  break;
        case NERT_NAME:     nname++; break;
        default:            nos++;   break;
        }
        if (verbose > 1) {
            uint16_t off = dmp_rd16(rec + 2), t1 = dmp_rd16(rec + 4), t2 = dmp_rd16(rec + 6);
            const char *mod = (rtype == NERT_ORDINAL && t1 >= 1 && t1 <= m->cmod)
                              ? m->modname[t1 - 1] : "";
            log_msg("      at %04X atype=%-2u rtype=%u%s t1=%04X t2=%04X %s%s\n",
                    off, rec[0] & 0x7F, rtype,
                    (tbyte & NERT_ADDITIVE) ? "+add" : "    ", t1, t2,
                    mod, (rtype == NERT_ORDINAL) ? "" : "");
        }
    }
    log_msg("    relocs: %u total (internal %u, ordinal %u, name %u, osfixup %u; additive %u)\n",
            count, nint, nord, nname, nos, nadd);
}

static void dump_entries(NeModule *m)
{
    const uint8_t *p   = m->img + m->hdr + m->enttab;
    const uint8_t *end = p + m->cbenttab;
    uint16_t ord = 1;
    unsigned total = 0;

    log_msg("\nEntry table (%u bytes, %u moveable entries declared)\n",
            m->cbenttab, m->cmovent);
    while (p + 2 <= end) {
        unsigned count = p[0], type = p[1], i;
        if (count == 0) break;
        p += 2;
        if (type == 0) {
            log_msg("  gap of %u ordinals (%u..%u)\n", count, ord, ord + count - 1);
            ord = (uint16_t)(ord + count);
            continue;
        }
        log_msg("  bundle: %u entries, type %s\n", count,
                type == 0xFF ? "MOVEABLE" : (type == 0xFE ? "ABSOLUTE" : "FIXED"));
        for (i = 0; i < count; i++) {
            uint8_t fl, sg;
            uint16_t off;
            char name[64];

            if (type == 0xFF) {
                if (p + 6 > end) return;
                fl = p[0]; sg = p[3]; off = dmp_rd16(p + 4); p += 6;
            } else {
                if (p + 3 > end) return;
                fl = p[0]; sg = (uint8_t)type; off = dmp_rd16(p + 1); p += 3;
            }
            name[0] = 0;
            {   /* find this ordinal in the resident name table */
                const uint8_t *r = m->img + m->hdr + m->restab;
                int first = 1;
                while (*r) {
                    unsigned len = *r;
                    if (!first && dmp_rd16(r + 1 + len) == ord) {
                        if (len > sizeof name - 1) len = sizeof name - 1;
                        memcpy(name, r + 1, len);
                        name[len] = 0;
                        break;
                    }
                    first = 0;
                    r += 1 + len + 2;
                }
            }
            log_msg("    ord %3u  seg %2u:%04X  flags %02X  %s\n",
                    ord, sg, off, fl, name);
            ord++;
            total++;
        }
    }
    log_msg("  %u entries total\n", total);
}

static void dump_resources(NeModule *m)
{
    const uint8_t *rt;
    unsigned shift;
    const uint8_t *p;
    uint32_t grand = 0;

    if (!m->rsrctab) { log_msg("\nNo resource table\n"); return; }
    rt = m->img + m->hdr + m->rsrctab;
    shift = dmp_rd16(rt);
    p = rt + 2;

    log_msg("\nResource table (alignment shift %u)\n", shift);
    for (;;) {
        uint16_t tid = dmp_rd16(p);
        uint16_t cnt;
        unsigned i;
        uint32_t bytes = 0;
        char tname[64];

        if (tid == 0) break;
        cnt = dmp_rd16(p + 2);
        p += 8;
        for (i = 0; i < cnt; i++)
            bytes += (uint32_t)dmp_rd16(p + i * 12 + 2) << shift;
        grand += bytes;
        log_msg("  type %-10s count %3u  %8u bytes\n",
                ne_resource_type_name(m, tid, tname, sizeof tname), cnt, bytes);
        p += (size_t)cnt * 12;
    }
    log_msg("  %u bytes of resources total\n", grand);
}

void ne_dump(NeModule *m, int verbose)
{
    unsigned i;
    char buf[128];

    log_msg("%s: NE module \"%s\" (%u bytes, header at %08X)\n",
            log_wide(m->path), m->name, m->imglen, m->hdr);
    log_msg("  flags      %04X  %s%s%s%s\n", m->flags,
            (m->flags & NEF_SINGLEDATA)   ? "SINGLEDATA "   : "",
            (m->flags & NEF_MULTIPLEDATA) ? "MULTIPLEDATA " : "",
            (m->flags & NEF_SELFLOAD)     ? "SELFLOAD "     : "",
            (m->flags & NEF_LIBMODULE)    ? "LIBMODULE "    : "");
    log_msg("  expver     %u.%u\n", m->expver >> 8, m->expver & 0xFF);
    log_msg("  align      %u (sector = %u bytes)\n", m->align, 1u << m->align);
    log_msg("  segments   %u    module refs %u\n", m->cseg, m->cmod);
    log_msg("  autodata   %u\n", m->autodata);
    log_msg("  heap/stack %04X / %04X\n", m->heap, m->stack);
    log_msg("  cs:ip      %04X:%04X\n", (unsigned)(m->csip >> 16),
            (unsigned)(m->csip & 0xFFFF));
    log_msg("  ss:sp      %04X:%04X\n", (unsigned)(m->sssp >> 16),
            (unsigned)(m->sssp & 0xFFFF));

    log_msg("\nModule references:");
    for (i = 0; i < m->cmod; i++) log_msg(" %s", m->modname[i]);
    log_msg("\n");

    log_msg("\nSegment table\n");
    log_msg("   #   filepos    len  minall   size   sel  flags\n");
    for (i = 1; i <= m->cseg; i++) {
        NeSeg *s = ne_seg(m, i);
        log_msg("  %2u  %8X  %5X   %5X  %5X  %04X  %s\n",
                i, s->filepos, s->length, s->minalloc, s->size, s->sel,
                seg_flag_str(s->flags, buf, sizeof buf));
        if (verbose) dump_relocs(m, i, verbose);
    }

    dump_entries(m);
    dump_resources(m);
}
