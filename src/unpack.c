/* unpack.c - decode a packed module.  See pack.h for the format.
 *
 * This is the whole of the compression that ships.  The encoder is a separate
 * program (src/pack.c) and none of its machinery - the match finder, the
 * dynamic-programming parse, the price model - is here, because a decoder
 * needs to know only what the bits mean, not how they were chosen.
 *
 * Every read of the input and every write to the output is bounded.  A packed
 * payload arrives in the same file as the emulator, so a truncated download or
 * a helpful archiver is the expected way for this to go wrong, and the right
 * answer is a sentence on the log rather than a fault inside the interpreter.
 */

#include "pack.h"
#include "log.h"

#include <string.h>

/* ---- CRC-32, the ordinary one -------------------------------------------- */

uint32_t pack_crc(const uint8_t *p, uint32_t n)
{
    static uint32_t crctab[256];
    uint32_t c = 0xFFFFFFFFu;
    uint32_t i;

    if (!crctab[1]) {                            /* built on first use */
        for (i = 0; i < 256; i++) {
            uint32_t v = i, k;
            for (k = 0; k < 8; k++) v = (v >> 1) ^ (0xEDB88320u & -(v & 1));
            crctab[i] = v;
        }
    }
    for (i = 0; i < n; i++) c = crctab[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/* ---- the container ------------------------------------------------------- */

static uint32_t pk_rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int pack_find(const uint8_t *file, uint32_t len, PackInfo *pi)
{
    uint32_t at;

    if (len < PACK_FIXED) return 0;
    /* Backwards, so the last trailer wins if a file somehow carries two: the
       one a later packer added is the one that describes this file.

       The magic is not proof on its own, and not only in theory: this very
       comparison is compiled from a string constant that lives in our own
       .rdata, so scanning our own image always finds at least one copy of it.
       Every structural test therefore comes before anything is believed or
       reported, and a candidate that fails one is simply not a trailer.  Only
       a trailer that is structurally sound but unreadable earns a complaint. */
    for (at = len - PACK_FIXED + 1; at-- > 0; ) {
        uint32_t ndelta, tlen, i, version;

        if (memcmp(file + at, PACK_MAGIC, PACK_MAGLEN) != 0) continue;
        version     = pk_rd32(file + at + PACK_MAGLEN);
        pi->rawlen  = pk_rd32(file + at + PACK_MAGLEN + 4);
        pi->complen = pk_rd32(file + at + PACK_MAGLEN + 8);
        pi->crc     = pk_rd32(file + at + PACK_MAGLEN + 12);
        ndelta      = pk_rd32(file + at + PACK_MAGLEN + 16);

        if (ndelta > PACK_MAXDELTA) continue;
        tlen = PACK_FIXED + ndelta * 8;
        if (at + tlen != len) continue;               /* must end the file */
        if (pi->complen > at) continue;
        if (!pi->rawlen || !pi->complen) continue;
        if (version != PACK_VERSION) {
            log_msg("pack: payload is version %u, this build reads %u\n",
                    version, (unsigned)PACK_VERSION);
            return 0;
        }

        pi->ndelta = ndelta;
        pi->off = at - pi->complen;
        for (i = 0; i < ndelta; i++) {
            const uint8_t *e = file + at + PACK_FIXED + i * 8;
            pi->delta[i].off = pk_rd32(e);
            pi->delta[i].len = pk_rd32(e + 4);
            if (pi->delta[i].off > pi->rawlen ||
                pi->delta[i].len > pi->rawlen - pi->delta[i].off) {
                log_msg("pack: trailer names a delta range outside the image\n");
                return 0;
            }
        }
        return 1;
    }
    return 0;
}

/* ---- the range decoder --------------------------------------------------- */

/* The encoder's mirror image, and the reason the two are written as a pair:
   every operation here has exactly one counterpart in src/pack.c, so a change
   to either without the other shows up immediately as a stream that does not
   round-trip.  The packer refuses to write output it cannot itself decode. */
typedef struct {
    const uint8_t *in, *end;
    uint32_t range, code;
    int      overrun;        /* ran past the payload: the stream is corrupt */
} Rc;

static uint8_t rc_byte(Rc *r)
{
    if (r->in >= r->end) { r->overrun = 1; return 0; }
    return *r->in++;
}

static void rc_init(Rc *r, const uint8_t *in, uint32_t n)
{
    int i;
    r->in = in;
    r->end = in + n;
    r->range = 0xFFFFFFFFu;
    r->code = 0;
    r->overrun = 0;
    rc_byte(r);              /* the encoder's first shift_low emits a zero */
    for (i = 0; i < 4; i++) r->code = (r->code << 8) | rc_byte(r);
}

static void rc_norm(Rc *r)
{
    while (r->range < (1u << 24)) {
        r->range <<= 8;
        r->code = (r->code << 8) | rc_byte(r);
    }
}

static int rc_bit(Rc *r, uint16_t *p)
{
    uint32_t bound = (r->range >> PK_PBITS) * *p;
    int bit;

    if (r->code < bound) {
        r->range = bound;
        *p = (uint16_t)(*p + ((PK_PTOP - *p) >> PK_PMOVE));
        bit = 0;
    } else {
        r->code -= bound;
        r->range -= bound;
        *p = (uint16_t)(*p - (*p >> PK_PMOVE));
        bit = 1;
    }
    rc_norm(r);
    return bit;
}

/* A bit-tree: nbits decisions down a binary tree of contexts, so each bit is
   predicted knowing the bits above it.  The walk ends at 1<<nbits + symbol. */
static unsigned rc_tree(Rc *r, uint16_t *probs, int nbits)
{
    unsigned node = 1;
    int n = nbits;
    while (n--) node = node + node + (unsigned)rc_bit(r, probs + node);
    return node - (1u << nbits);
}

/* Raw, equiprobable bits: the low half of a distance, where modelling buys
   nothing because the bits really are noise. */
static uint32_t rc_direct(Rc *r, unsigned n)
{
    uint32_t v = 0;
    while (n--) {
        r->range >>= 1;
        if (r->code >= r->range) { r->code -= r->range; v = (v << 1) | 1; }
        else                     { v = v << 1; }
        rc_norm(r);
    }
    return v;
}

/* ---- the LZ layer -------------------------------------------------------- */

int pack_decode(const uint8_t *file, const PackInfo *pi, uint8_t *out)
{
    static uint16_t probs[PK_NPROBS];
    Rc rc;
    uint32_t pos = 0, lastd = 0, i;
    int state = 0;

    for (i = 0; i < PK_NPROBS; i++) probs[i] = PK_PTOP / 2;
    rc_init(&rc, file + pi->off, pi->complen);

    while (pos < pi->rawlen) {
        if (!rc_bit(&rc, probs + PK_ISMATCH + state)) {
            unsigned prev = pos ? out[pos - 1] : 0;
            out[pos++] = (uint8_t)rc_tree(&rc, probs + PK_LIT + (prev >> 4) * 256, 8);
            state = 0;
        } else {
            uint32_t len = rc_tree(&rc, probs + PK_LEN, 8) + PK_MINLEN;
            uint32_t d;

            if (rc_bit(&rc, probs + PK_ISREP + state)) {
                d = lastd;
            } else {
                unsigned slot = rc_tree(&rc, probs + PK_DSLOT, 6);
                d = 1u << slot;
                if (slot) d |= rc_direct(&rc, slot);
                lastd = d;
            }
            if (!d || d > pos || len > pi->rawlen - pos) {
                log_msg("pack: corrupt stream at %u (distance %u, length %u)\n",
                        pos, d, len);
                return 0;
            }
            while (len--) { out[pos] = out[pos - d]; pos++; }
            state = 1;
        }
        if (rc.overrun) {
            log_msg("pack: payload ends early, %u of %u bytes decoded\n",
                    pos, pi->rawlen);
            return 0;
        }
    }

    /* The audio came through delta-coded; see pack.h for why that is the only
       transform here.  First byte of each range is already the true value. */
    for (i = 0; i < pi->ndelta; i++) {
        uint32_t o = pi->delta[i].off, n = pi->delta[i].len, j;
        for (j = 1; j < n; j++) out[o + j] = (uint8_t)(out[o + j] + out[o + j - 1]);
    }

    if (pack_crc(out, pi->rawlen) != pi->crc) {
        log_msg("pack: checksum mismatch - the payload is damaged\n");
        return 0;
    }
    return 1;
}
