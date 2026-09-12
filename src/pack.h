/* pack.h - the container and codec shared by the packer and the emulator.
 *
 * Everything the two sides must agree on lives here and nowhere else, because
 * the failure mode for a disagreement is not a compile error: it is a stream
 * that decodes into plausible rubbish.  The probability-model layout is an
 * enum for the same reason - both sides index one array, so a field added on
 * one side cannot silently shift the other's contexts.
 *
 * The codec is LZ77 over a binary range coder.  It was chosen by measuring
 * against the game, and the measurements are worth recording because they are
 * unintuitive:
 *
 *   - The 2.2 MB of art is 8bpp *paletted*, so the usual image tricks are
 *     counterproductive.  Every PNG-style filter tried - sub, up, average,
 *     Paeth, xor-with-above - made the file LARGER, the best by 9%: arithmetic
 *     differences between palette indices are meaningless, and filtering
 *     destroys the long exact repeats the matcher lives on.
 *   - Splitting the file into homogeneous streams (art, code, audio, palettes)
 *     was worth 0.16%.  An x86 BCJ filter made it worse, this being 16-bit
 *     code.  A window over 1 MiB is worth nothing at all.
 *   - A per-pixel 2D context model reached 547 KB on the art where a plain LZ
 *     reached 523 KB, so the redundancy really is long repeats rather than
 *     local smoothness.
 *   - Delta-coding the 8-bit PCM audio was the one domain trick that paid:
 *     14 KB, for the ten-line loop at the end of the decoder.
 *
 * So the compression comes from the parse, not from knowing what the bytes
 * mean.  An optimal parse is worth 3.55% over a greedy one here and costs the
 * decoder nothing, which is exactly where the complexity belongs: in a tool
 * nobody ships.
 */
#ifndef PACK_H
#define PACK_H

#include <stdint.h>

/* ---- the container ------------------------------------------------------- */

/* A trailer at the very end of the file, with the payload immediately before
   it.  Found by searching backwards for the magic rather than by arithmetic on
   where our own PE ends - the property the plain appended-module scan had, and
   worth keeping: the linker's idea of where we stop moves when symbols are
   stripped, and something may yet be appended after us. */
#define PACK_MAGIC    "StarsVM\032"        /* 8 bytes, no terminator */
#define PACK_MAGLEN   8
#define PACK_VERSION  1
#define PACK_MAXDELTA 16

/* magic, version, rawlen, complen, crc, ndelta, then ndelta (off,len) pairs. */
#define PACK_FIXED    (PACK_MAGLEN + 5 * 4)

typedef struct {
    uint32_t rawlen;                       /* decompressed size, checked exactly */
    uint32_t complen;
    uint32_t crc;                          /* of the decompressed image */
    uint32_t ndelta;
    uint32_t off;                          /* payload offset within the file */
    struct { uint32_t off, len; } delta[PACK_MAXDELTA];
} PackInfo;

/* ---- the codec ---------------------------------------------------------- */

#define PK_PBITS  11                       /* probability precision */
#define PK_PTOP   (1u << PK_PBITS)
#define PK_PMOVE  5                        /* adaptation shift */
#define PK_MINLEN 3
#define PK_MAXLEN 258                      /* MINLEN + 255, one bit-tree wide */
#define PK_LITCTX 16                       /* literal context: prev byte >> 4 */
#define PK_SLOTS  64                       /* distance slots, a 6-bit tree */

/* One array of probabilities, indexed through these.  A bit-tree of n bits
   uses 1<<n slots and is addressed from node 1, which is why each block is its
   full power of two rather than the number of symbols. */
enum {
    PK_ISMATCH = 0,                        /* 2: context is the previous token */
    PK_ISREP   = PK_ISMATCH + 2,           /* 2: reuse the previous distance   */
    PK_LEN     = PK_ISREP + 2,             /* 256: length - PK_MINLEN          */
    PK_DSLOT   = PK_LEN + 256,             /* 64: floor(log2(distance))        */
    PK_LIT     = PK_DSLOT + PK_SLOTS,      /* 16 * 256                         */
    PK_NPROBS  = PK_LIT + PK_LITCTX * 256
};

/* Distance d >= 1 is coded as slot = floor(log2(d)) then the low `slot` bits,
   which is a bijection: d == (1 << slot) | low. */
static inline unsigned pk_slot(uint32_t d)
{
    unsigned s = 0;
    while ((1u << (s + 1)) <= d) s++;
    return s;
}

/* ---- what the emulator needs (src/unpack.c) ----------------------------- */

/* Look for a trailer at the end of `file`.  1 if it carries a payload; 0 if it
   does not, which is not an error - a plainly appended module is still valid. */
int pack_find(const uint8_t *file, uint32_t len, PackInfo *pi);

/* Decode into `out`, which must hold pi->rawlen bytes.  Verifies the length
   and the checksum, and refuses a stream that would read or write outside the
   buffer: a corrupt payload has to fail here with something to say, not by
   handing the interpreter rubbish to execute. */
int pack_decode(const uint8_t *file, const PackInfo *pi, uint8_t *out);

uint32_t pack_crc(const uint8_t *p, uint32_t n);

#endif
