/* pack.c - the packer: compress a module and append it to the emulator.
 *
 * This is a build-time tool and is not part of the emulator; see
 * src/unity_pack.c.  Everything expensive lives here on purpose, because the
 * decoder's size is a cost paid by every copy of the game and the encoder's is
 * paid by nobody.  Concretely, the dynamic-programming parse below is worth
 * 3.55% over taking the longest match at each step, and the decoder cannot
 * tell the difference: both produce the same token stream, this one just picks
 * better tokens.
 *
 * The parse is priced against a snapshot of the adaptive model and iterated,
 * because the prices depend on the parse that produced them.  Three subtleties
 * cost me a day each when this was a prototype, so they are worth naming:
 *
 *   - The price of the "is match" bit depends on whether the previous token was
 *     a match, so the DP has to carry that as state.  Pricing it at a fixed
 *     context makes the DP choose a parse that is worse than greedy.
 *   - One price snapshot is not enough.  The first DP shifts the token mix,
 *     which changes the prices, which changes the best parse.  Iterating and
 *     keeping the best round is what actually converges.
 *   - Match lengths reach 258, so nothing that holds one may be a byte.
 */

#include "pack.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define HBITS 18
#define HSIZE (1u << HBITS)

/* ---- the range encoder, mirroring src/unpack.c -------------------------- */

typedef struct {
    uint8_t  *out;
    uint32_t  cap, n;
    uint64_t  low;
    uint32_t  range;
    uint8_t   cache;
    int64_t   cachesize;
} Enc;

static void enc_init(Enc *e, uint8_t *out, uint32_t cap)
{
    e->out = out; e->cap = cap; e->n = 0;
    e->low = 0; e->range = 0xFFFFFFFFu;
    e->cache = 0; e->cachesize = 1;
}

static void enc_put(Enc *e, uint8_t b)
{
    if (e->n < e->cap) e->out[e->n] = b;
    e->n++;                                /* keep counting past the cap */
}

static void enc_shift(Enc *e)
{
    if ((uint32_t)e->low < 0xFF000000u || (e->low >> 32)) {
        uint8_t carry = (uint8_t)(e->low >> 32);
        do {
            enc_put(e, (uint8_t)(e->cache + carry));
            e->cache = 0xFF;
        } while (--e->cachesize);
        e->cache = (uint8_t)(e->low >> 24);
    }
    e->cachesize++;
    e->low = (e->low << 8) & 0xFFFFFFFFu;
}

static void enc_bit(Enc *e, uint16_t *p, int bit)
{
    uint32_t bound = (e->range >> PK_PBITS) * *p;
    if (!bit) {
        e->range = bound;
        *p = (uint16_t)(*p + ((PK_PTOP - *p) >> PK_PMOVE));
    } else {
        e->low += bound;
        e->range -= bound;
        *p = (uint16_t)(*p - (*p >> PK_PMOVE));
    }
    while (e->range < (1u << 24)) { e->range <<= 8; enc_shift(e); }
}

static void enc_tree(Enc *e, uint16_t *tree, int nbits, unsigned sym)
{
    unsigned node = 1;
    int i;
    for (i = nbits - 1; i >= 0; i--) {
        int b = (int)((sym >> i) & 1);
        enc_bit(e, tree + node, b);
        node = node + node + (unsigned)b;
    }
}

static void enc_direct(Enc *e, uint32_t v, unsigned n)
{
    while (n--) {
        e->range >>= 1;
        if ((v >> n) & 1) e->low += e->range;
        while (e->range < (1u << 24)) { e->range <<= 8; enc_shift(e); }
    }
}

static void enc_flush(Enc *e)
{
    int i;
    for (i = 0; i < 5; i++) enc_shift(e);
}

/* ---- prices, in 1/256 bit units ------------------------------------------ */

static unsigned costtab[PK_PTOP + 1];

static void costinit(void)
{
    unsigned i;
    for (i = 1; i < PK_PTOP; i++)
        costtab[i] = (unsigned)(-log2((double)i / PK_PTOP) * 256.0);
    costtab[0] = costtab[1];
    costtab[PK_PTOP] = 0;
}

static unsigned treeprice(const uint16_t *tree, int nbits, unsigned sym)
{
    unsigned node = 1, c = 0;
    int i;
    for (i = nbits - 1; i >= 0; i--) {
        int b = (int)((sym >> i) & 1);
        c += b ? costtab[PK_PTOP - tree[node]] : costtab[tree[node]];
        node = node + node + (unsigned)b;
    }
    return c;
}

/* ---- the match finder ---------------------------------------------------- */

static const uint8_t *pk_in;
static uint32_t pk_size;
static int32_t *head, *chain;
static uint32_t distfor[PK_MAXLEN + 1];    /* shortest distance per length */

static uint32_t hash4(const uint8_t *p)
{
    uint32_t v = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                 ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    return (v * 2654435761u) >> (32 - HBITS);
}

static void mf_reset(void)
{
    uint32_t i;
    for (i = 0; i < HSIZE; i++) head[i] = -1;
    for (i = 0; i < pk_size + 1; i++) chain[i] = -1;
}

static void mf_insert(uint32_t at)
{
    uint32_t h;
    if (at + 4 > pk_size) return;
    h = hash4(pk_in + at);
    chain[at] = head[h];
    head[h] = (int32_t)at;
}

/* Fills distfor[] and returns the longest match found at `at`. */
static uint32_t mf_find(uint32_t at, uint32_t depth)
{
    uint32_t best = 0;
    int32_t cur;

    if (at + PK_MINLEN > pk_size) return 0;
    memset(distfor, 0, sizeof distfor);
    cur = head[hash4(pk_in + at)];
    while (cur >= 0 && depth--) {
        uint32_t d = at - (uint32_t)cur, n = 0, max = pk_size - at;
        if (!d) break;
        if (max > PK_MAXLEN) max = PK_MAXLEN;
        while (n < max && pk_in[cur + n] == pk_in[at + n]) n++;
        if (n >= PK_MINLEN) {
            uint32_t L;
            for (L = PK_MINLEN; L <= n; L++)
                if (!distfor[L] || d < distfor[L]) distfor[L] = d;
            if (n > best) best = n;
            if (n == PK_MAXLEN) break;
        }
        cur = chain[cur];
    }
    return best;
}

/* ---- the parse ----------------------------------------------------------- */

/* One token.  A literal is len == 0. */
typedef struct { uint32_t len, dist; } Tok;

static uint16_t pk_probs[PK_NPROBS];
static unsigned pr_ismatch[2][2], pr_isrep[2][2];
static unsigned pr_len[256], pr_dslot[PK_SLOTS], pr_lit[PK_LITCTX][256];

static void resetprobs(void)
{
    uint32_t i;
    for (i = 0; i < PK_NPROBS; i++) pk_probs[i] = PK_PTOP / 2;
}

static void snapshot(void)
{
    unsigned i, j;
    for (i = 0; i < 2; i++) {
        pr_ismatch[i][0] = costtab[pk_probs[PK_ISMATCH + i]];
        pr_ismatch[i][1] = costtab[PK_PTOP - pk_probs[PK_ISMATCH + i]];
        pr_isrep[i][0] = costtab[pk_probs[PK_ISREP + i]];
        pr_isrep[i][1] = costtab[PK_PTOP - pk_probs[PK_ISREP + i]];
    }
    for (i = 0; i < 256; i++) pr_len[i] = treeprice(pk_probs + PK_LEN, 8, i);
    for (i = 0; i < PK_SLOTS; i++) pr_dslot[i] = treeprice(pk_probs + PK_DSLOT, 6, i);
    for (i = 0; i < PK_LITCTX; i++)
        for (j = 0; j < 256; j++)
            pr_lit[i][j] = treeprice(pk_probs + PK_LIT + i * 256, 8, j);
}

/* Encode a token list, adapting as the decoder will, and return its size. */
static uint32_t emit(const Tok *tok, uint32_t ntok, uint8_t *out, uint32_t cap)
{
    Enc e;
    uint32_t i, pos = 0, lastd = 0;
    int state = 0;

    resetprobs();
    enc_init(&e, out, cap);
    for (i = 0; i < ntok; i++) {
        if (!tok[i].len) {
            unsigned prev = pos ? pk_in[pos - 1] : 0;
            enc_bit(&e, pk_probs + PK_ISMATCH + state, 0);
            enc_tree(&e, pk_probs + PK_LIT + (prev >> 4) * 256, 8, pk_in[pos]);
            pos++;
            state = 0;
        } else {
            uint32_t d = tok[i].dist;
            enc_bit(&e, pk_probs + PK_ISMATCH + state, 1);
            enc_tree(&e, pk_probs + PK_LEN, 8, tok[i].len - PK_MINLEN);
            if (d == lastd) {
                enc_bit(&e, pk_probs + PK_ISREP + state, 1);
            } else {
                unsigned slot = pk_slot(d);
                enc_bit(&e, pk_probs + PK_ISREP + state, 0);
                enc_tree(&e, pk_probs + PK_DSLOT, 6, slot);
                if (slot) enc_direct(&e, d & ((1u << slot) - 1), slot);
                lastd = d;
            }
            pos += tok[i].len;
            state = 1;
        }
    }
    enc_flush(&e);
    return e.n;
}

/* Longest-match-first, to seed the prices. */
static uint32_t parse_greedy(Tok *tok, uint32_t depth)
{
    uint32_t at = 0, ntok = 0;
    mf_reset();
    while (at < pk_size) {
        uint32_t n = mf_find(at, depth);
        if (n >= PK_MINLEN) {
            uint32_t i;
            tok[ntok].len = n;
            tok[ntok].dist = distfor[n];
            ntok++;
            for (i = 0; i < n; i++) mf_insert(at + i);
            at += n;
        } else {
            tok[ntok].len = 0;
            tok[ntok].dist = 0;
            ntok++;
            mf_insert(at);
            at++;
        }
    }
    return ntok;
}

/* The dynamic-programming parse.  best[pos][state] is the cheapest way to
   reach `pos` having just emitted a literal (state 0) or a match (state 1);
   carrying that state is what makes the "is match" bit priced correctly. */
static uint32_t parse_optimal(Tok *tok, uint32_t depth,
                              uint32_t *cost, uint16_t *clen, uint32_t *cdist)
{
    uint32_t at, ntok, i, p;
    int cur;
    unsigned s;

    for (i = 0; i <= pk_size; i++) {
        cost[i * 2] = cost[i * 2 + 1] = 0xFFFFFFFFu;
        clen[i * 2] = clen[i * 2 + 1] = 0;
    }
    cost[0] = 0;
    mf_reset();

    for (at = 0; at < pk_size; at++) {
        uint32_t n = 0;
        int have = 0;
        for (s = 0; s < 2; s++) {
            uint32_t base = cost[at * 2 + s], L;
            if (base == 0xFFFFFFFFu) continue;
            {   /* a literal always available */
                uint32_t c = base + pr_ismatch[s][0] +
                             pr_lit[at ? (pk_in[at - 1] >> 4) : 0][pk_in[at]];
                if (c < cost[(at + 1) * 2]) {
                    cost[(at + 1) * 2] = c;
                    clen[(at + 1) * 2] = 0;
                    cdist[(at + 1) * 2] = s;      /* which state we came from */
                }
            }
            if (!have) { n = mf_find(at, depth); have = 1; }
            for (L = PK_MINLEN; L <= n; L++) {
                uint32_t d = distfor[L], sl = pk_slot(d);
                /* Priced as a fresh distance.  A token that turns out to repeat
                   the previous one is cheaper than this when it is emitted, so
                   the DP is pessimistic about matches, never optimistic. */
                uint32_t c = base + pr_ismatch[s][1] + pr_len[L - PK_MINLEN] +
                             pr_isrep[s][0] + pr_dslot[sl] + sl * 256;
                if (c < cost[(at + L) * 2 + 1]) {
                    cost[(at + L) * 2 + 1] = c;
                    clen[(at + L) * 2 + 1] = (uint16_t)L;
                    cdist[(at + L) * 2 + 1] = d | (s << 31);
                }
            }
        }
        if (!have) mf_find(at, depth);
        mf_insert(at);
    }

    /* Walk back from the cheaper end state, then reverse. */
    cur = cost[pk_size * 2] <= cost[pk_size * 2 + 1] ? 0 : 1;
    ntok = 0;
    p = pk_size;
    while (p) {
        uint32_t L = clen[p * 2 + (unsigned)cur];
        tok[ntok].len = L;
        tok[ntok].dist = L ? (cdist[p * 2 + (unsigned)cur] & 0x7FFFFFFFu) : 0;
        ntok++;
        cur = L ? (int)(cdist[p * 2 + (unsigned)cur] >> 31)
                : (int)cdist[p * 2 + (unsigned)cur];
        p -= L ? L : 1;
    }
    for (i = 0; i < ntok / 2; i++) {
        Tok t = tok[i];
        tok[i] = tok[ntok - 1 - i];
        tok[ntok - 1 - i] = t;
    }
    return ntok;
}

/* ---- the audio transform ------------------------------------------------- */

/* Delta-code the RIFF resources pk_in place, identified by magic rather than by
   walking the resource table: the packer then needs no NE parser, and a file
   with no RIFF pk_in it simply gets no ranges.  See pack.h for the measurement. */
static uint32_t find_riff(uint8_t *d, uint32_t n, PackInfo *pi)
{
    uint32_t at, found = 0;
    for (at = 0; at + 8 <= n && found < PACK_MAXDELTA; at++) {
        uint32_t sz;
        if (memcmp(d + at, "RIFF", 4) != 0) continue;
        sz = (uint32_t)d[at + 4] | ((uint32_t)d[at + 5] << 8) |
             ((uint32_t)d[at + 6] << 16) | ((uint32_t)d[at + 7] << 24);
        sz += 8;
        if (sz < 64 || sz > n - at) continue;
        pi->delta[found].off = at;
        pi->delta[found].len = sz;
        found++;
        at += sz - 1;
    }
    pi->ndelta = found;
    return found;
}

static void delta_apply(uint8_t *d, const PackInfo *pi)
{
    uint32_t i;
    for (i = 0; i < pi->ndelta; i++) {
        uint32_t o = pi->delta[i].off, n = pi->delta[i].len, j;
        uint8_t prev = 0;
        for (j = 0; j < n; j++) {
            uint8_t v = d[o + j];
            d[o + j] = (uint8_t)(v - prev);
            prev = v;
        }
    }
}

/* ---- driver -------------------------------------------------------------- */

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint8_t *slurp(const char *path, uint32_t *len)
{
    FILE *f = fopen(path, "rb");
    uint8_t *b;
    long n;
    if (!f) { fprintf(stderr, "pack: cannot open %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    b = malloc((size_t)n + 1);
    if (n && fread(b, 1, (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "pack: short read on %s\n", path);
        fclose(f); free(b); return NULL;
    }
    fclose(f);
    *len = (uint32_t)n;
    return b;
}

/* Compress `raw` into a freshly allocated payload plus its trailer, and prove
   it round-trips before returning.  A packer that cannot decode its own output
   must not produce any: everything downstream trusts this one check. */
uint8_t *pack_compress(const uint8_t *raw, uint32_t rawlen, uint32_t depth,
                       int rounds, uint32_t *outlen, int verbose)
{
    uint8_t *work, *best = NULL, *buf;
    uint32_t bestlen = 0xFFFFFFFFu, cap, i;
    uint32_t *cost, *cdist;
    uint16_t *clen;
    Tok *tok;
    PackInfo pi;
    uint32_t ntok;
    int r;

    memset(&pi, 0, sizeof pi);
    pi.rawlen = rawlen;
    pi.crc = pack_crc(raw, rawlen);

    work = malloc(rawlen ? rawlen : 1);
    memcpy(work, raw, rawlen);
    find_riff(work, rawlen, &pi);
    delta_apply(work, &pi);
    if (verbose && pi.ndelta)
        printf("  %u RIFF range%s delta-coded\n", pi.ndelta,
               pi.ndelta == 1 ? "" : "s");

    pk_in = work;
    pk_size = rawlen;
    costinit();
    head = malloc(HSIZE * sizeof *head);
    chain = malloc((rawlen + 1) * sizeof *chain);
    tok = malloc((rawlen + 1) * sizeof *tok);
    cost = malloc((rawlen + 1) * 2 * sizeof *cost);
    clen = malloc((rawlen + 1) * 2 * sizeof *clen);
    cdist = malloc((rawlen + 1) * 2 * sizeof *cdist);
    /* Worst case is a literal per byte - one "is match" bit plus an 8-bit
       tree, so 9 bits, so 12.5% over.  Measured on 3 MB of noise it is 1.4%,
       because the trees adapt to the fact that nothing is matching.  Allow
       double the theoretical worst anyway and check after every emit: emit()
       keeps counting past the cap, so an overrun is something we can see
       rather than a silent write past the buffer. */
    cap = rawlen + rawlen / 4 + 4096;
    buf = malloc(cap);

    ntok = parse_greedy(tok, depth);
    bestlen = emit(tok, ntok, buf, cap);
    if (bestlen > cap) {
        fprintf(stderr, "pack: output %u exceeds the %u-byte buffer\n",
                bestlen, cap);
        return NULL;
    }
    best = malloc(bestlen);
    memcpy(best, buf, bestlen);
    if (verbose) printf("  greedy parse  %9u bytes\n", bestlen);

    for (r = 0; r < rounds; r++) {
        uint32_t n;
        snapshot();                         /* price from the last parse */
        ntok = parse_optimal(tok, depth, cost, clen, cdist);
        n = emit(tok, ntok, buf, cap);
        if (n > cap) {
            fprintf(stderr, "pack: output %u exceeds the %u-byte buffer\n",
                    n, cap);
            return NULL;
        }
        if (verbose) printf("  round %d       %9u bytes\n", r, n);
        if (n < bestlen) {
            bestlen = n;
            free(best);
            best = malloc(n);
            memcpy(best, buf, n);
        }
    }

    free(buf); free(tok); free(cost); free(clen); free(cdist);
    free(head); free(chain); free(work);

    /* ---- the trailer ---- */
    pi.complen = bestlen;
    {
        uint32_t tlen = PACK_FIXED + pi.ndelta * 8;
        uint8_t *outp = malloc(bestlen + tlen);
        memcpy(outp, best, bestlen);
        memcpy(outp + bestlen, PACK_MAGIC, PACK_MAGLEN);
        wr32(outp + bestlen + PACK_MAGLEN, PACK_VERSION);
        wr32(outp + bestlen + PACK_MAGLEN + 4, pi.rawlen);
        wr32(outp + bestlen + PACK_MAGLEN + 8, pi.complen);
        wr32(outp + bestlen + PACK_MAGLEN + 12, pi.crc);
        wr32(outp + bestlen + PACK_MAGLEN + 16, pi.ndelta);
        for (i = 0; i < pi.ndelta; i++) {
            wr32(outp + bestlen + PACK_FIXED + i * 8, pi.delta[i].off);
            wr32(outp + bestlen + PACK_FIXED + i * 8 + 4, pi.delta[i].len);
        }
        free(best);

        /* ---- prove it ---- */
        {
            PackInfo chk;
            uint8_t *back = malloc(rawlen ? rawlen : 1);
            if (!pack_find(outp, bestlen + tlen, &chk)) {
                fprintf(stderr, "pack: cannot find the trailer just written\n");
                return NULL;
            }
            if (!pack_decode(outp, &chk, back)) {
                fprintf(stderr, "pack: own output does not decode\n");
                return NULL;
            }
            if (memcmp(back, raw, rawlen) != 0) {
                uint32_t j;
                for (j = 0; j < rawlen && back[j] == raw[j]; j++) { }
                fprintf(stderr, "pack: round trip differs at byte %u\n", j);
                return NULL;
            }
            free(back);
        }
        *outlen = bestlen + tlen;
        return outp;
    }
}
