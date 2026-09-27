/* x80gen.h - x87 operands worth testing, for the fuzzer and x80test.
 *
 * Uniformly random bits make poor x87 operands: nearly every one is a normal
 * number of middling exponent, and the cases an implementation gets wrong -
 * a rounding tie, an exact cancellation, a denormal, a NaN payload, an
 * integer boundary, an argument at the edge of some instruction's special
 * handling - come up about never.  So these generators draw from classes,
 * each chosen to hit one of those on purpose, and keep plain noise as one
 * class among many.  They are portable C with no floating point, so that
 * the same streams can be drawn on any host.
 *
 * The generator is splitmix64: one word of state, any value a valid seed.
 */
#ifndef X80GEN_H
#define X80GEN_H

#include "x80.h"

typedef struct { uint64_t s; } X80Rng;

static inline uint64_t x80gen_u64(X80Rng *g)
{
    uint64_t z = (g->s += 0x9E3779B97F4A7C15u);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9u;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBu;
    return z ^ (z >> 31);
}

/* Uniform in [0, n), n > 0. */
static inline uint32_t x80gen_below(X80Rng *g, uint32_t n)
{
    return (uint32_t)(((x80gen_u64(g) >> 32) * n) >> 32);
}

static inline void x80gen_set(X80 *v, uint16_t se, uint64_t m)
{
    v->se = se;
    v->m = m;
}

/* An integer part and a 64-bit binary fraction, as a normal X80, truncated
   to 64 significant bits. */
static inline void x80gen_fixed(X80 *v, uint64_t ip, uint64_t fp, int neg)
{
    uint16_t s = neg ? 0x8000u : 0;
    int e;

    if (!ip && !fp) { x80gen_set(v, s, 0); return; }
    if (ip) {
        for (e = 63; !(ip >> e); e--) {}
        v->m = e == 63 ? ip : ip << (63 - e) | fp >> (e + 1);
        v->se = (uint16_t)(s | (0x3FFF + e));
    } else {
        for (e = 63; !(fp >> e); e--) {}
        v->m = fp << (63 - e);
        v->se = (uint16_t)(s | (0x3FFF - 64 + e));
    }
}

/* k * pi/2, from pi/2 as the 128-bit C90FDAA22168C234C4C6628B80DC1CD1 *
   2^-127, truncated: arguments near the points where FSIN, FCOS and FPTAN
   have their zeros and poles, which is where an argument reduction shows
   how much of pi it carries. */
static inline void x80gen_kpi2(X80 *v, uint32_t k)
{
    const uint64_t hi = 0xC90FDAA22168C234u, lo = 0xC4C6628B80DC1CD1u;
    uint64_t p0 = (lo & 0xFFFFFFFFu) * k, p1 = (lo >> 32) * k;
    uint64_t p2 = (hi & 0xFFFFFFFFu) * k, p3 = (hi >> 32) * k;
    uint64_t t, w2, w3, top;
    int b;

    if (!k) { x80gen_set(v, 0, 0); return; }
    /* (hi:lo) * k is t:w3:w2:w1:w0 in 32-bit words; w1 and w0 are below
       anything a 64-bit significand can hold. */
    t = (p0 >> 32) + (p1 & 0xFFFFFFFFu);
    t = (t >> 32) + (p1 >> 32) + (p2 & 0xFFFFFFFFu); w2 = t & 0xFFFFFFFFu;
    t = (t >> 32) + (p2 >> 32) + (p3 & 0xFFFFFFFFu); w3 = t & 0xFFFFFFFFu;
    t = (t >> 32) + (p3 >> 32);
    top = w3 << 32 | w2;                      /* bits 64-127: pi/2 when k = 1 */
    if (!t) { x80gen_set(v, 0x3FFF, top); return; }
    for (b = 31; !(t >> b); b--) {}
    x80gen_set(v, (uint16_t)(0x3FFF + b + 1), t << (63 - b) | top >> (b + 1));
}

/* One of the encodings with a name: signed zeros, the denormal edges, the
   normal extremes, infinities, NaNs quiet and signalling with payloads,
   the indefinite, and the four encodings the 387 onwards calls invalid -
   pseudo-denormal, unnormal, pseudo-infinity, pseudo-NaN. */
static inline void x80gen_special(X80Rng *g, X80 *v)
{
    static const struct { uint16_t e; uint64_t m; } t[] = {
        { 0x0000, 0x0000000000000000u },    /* zero              */
        { 0x0000, 0x0000000000000001u },    /* least denormal    */
        { 0x0000, 0x7FFFFFFFFFFFFFFFu },    /* greatest denormal */
        { 0x0000, 0x8000000000000000u },    /* pseudo-denormal   */
        { 0x0001, 0x8000000000000000u },    /* least normal      */
        { 0x3FFF, 0x8000000000000000u },    /* 1                 */
        { 0x4000, 0x8000000000000000u },    /* 2                 */
        { 0x3FFE, 0x8000000000000000u },    /* 0.5               */
        { 0x4000, 0xC90FDAA22168C235u },    /* pi, rounded up    */
        { 0x4000, 0xC90FDAA22168C234u },    /* pi, truncated     */
        { 0x7FFE, 0xFFFFFFFFFFFFFFFFu },    /* greatest normal   */
        { 0x7FFF, 0x8000000000000000u },    /* infinity          */
        { 0x7FFF, 0xC000000000000000u },    /* QNaN, indefinite  */
        { 0x7FFF, 0xFFFFFFFFFFFFFFFFu },    /* QNaN, all payload */
        { 0x7FFF, 0xA000000000000000u },    /* SNaN              */
        { 0x7FFF, 0x8000000000000001u },    /* SNaN, least       */
        { 0x7FFF, 0x0000000000000000u },    /* pseudo-infinity   */
        { 0x7FFF, 0x4000000000000000u },    /* pseudo-NaN        */
        { 0x3FFF, 0x4000000000000000u },    /* unnormal          */
        { 0x4005, 0xFA00000000000000u },    /* 125               */
        { 0x400C, 0x9C40000000000000u },    /* 10000             */
    };
    unsigned k = x80gen_below(g, sizeof t / sizeof *t);
    uint64_t r = x80gen_u64(g);

    x80gen_set(v, (uint16_t)(t[k].e | (r & 1 ? 0x8000u : 0)), t[k].m);
    /* A random payload now and then, for the NaN-choosing rules. */
    if (t[k].e == 0x7FFF && t[k].m >> 62 && (r & 6) == 6)
        v->m = (t[k].m & 0xC000000000000000u) | (x80gen_u64(g) >> 2) | 1;
}

/* A normal with the given biased exponent and a random significand. */
static inline void x80gen_normal(X80Rng *g, X80 *v, int exp, int neg)
{
    if (exp < 1) exp = 1;
    if (exp > 0x7FFE) exp = 0x7FFE;
    x80gen_set(v, (uint16_t)((neg ? 0x8000 : 0) | exp),
               x80gen_u64(g) | 0x8000000000000000u);
}

/* An interesting value. */
static inline void x80gen_value(X80Rng *g, X80 *v)
{
    uint64_t r = x80gen_u64(g);
    int neg = (int)(r & 1), d = (int)((r >> 8) & 0xFF) - 128;

    switch (x80gen_below(g, 16)) {
    case 0: case 1:                               /* named encodings */
        x80gen_special(g, v);
        break;
    case 2: case 3: case 4:                       /* everyday numbers */
        x80gen_normal(g, v, 0x3FFF + d / 2, neg);
        break;
    case 5:                                       /* any exponent at all */
        x80gen_normal(g, v, 1 + (int)x80gen_below(g, 0x7FFE), neg);
        break;
    case 6: {                                     /* integer edges, +- halves */
        static const uint64_t n[] = {
            0, 1, 2, 3, 32767, 32768, 65535, 65536, 0x7FFFFFFFu, 0x80000000u,
            0xFFFFFFFFu, 0x100000000u, 0x20000000000000u, 0x7FFFFFFFFFFFFFFFu,
            0x8000000000000000u, 0xFFFFFFFFFFFFFFFFu,
        };
        static const uint64_t f[] = {
            0, 0, 0x8000000000000000u, 0x8000000000000001u,
            0x7FFFFFFFFFFFFFFFu, 0x0000000000000001u, 0xFFFFFFFFFFFFFFFFu,
        };
        uint64_t ip = n[x80gen_below(g, sizeof n / sizeof *n)];
        if (r & 2 && ip) ip--;
        x80gen_fixed(v, ip, f[x80gen_below(g, sizeof f / sizeof *f)], neg);
        break;
    }
    case 7: {                                     /* ties when narrowed */
        static const unsigned low[] = { 40, 11, 1, 2 }; /* to 24, 53, 63, 62 */
        unsigned k = low[x80gen_below(g, 4)];
        uint64_t half = 1ull << (k - 1), m;
        x80gen_normal(g, v, 0x3FFF + d / 4, neg);
        m = v->m & ~((half << 1) - 1);
        switch (x80gen_below(g, 3)) {
        case 0:  m |= half;     break;            /* exactly half */
        case 1:  m |= half - 1; break;            /* just under   */
        default: m |= half + 1; break;            /* just over    */
        }
        v->m = m | 0x8000000000000000u;
        break;
    }
    case 8: {                                     /* where formats run out */
        static const int edge[] = {
            0x3FFF - 126, 0x3FFF - 149, 0x3FFF + 127, 0x3FFF + 128,
            0x3FFF - 1022, 0x3FFF - 1074, 0x3FFF + 1023, 0x3FFF + 1024,
            1, 2, 64, 0x7FFE, 0x7FFD,
        };
        int e = edge[x80gen_below(g, sizeof edge / sizeof *edge)];
        x80gen_normal(g, v, e + (int)x80gen_below(g, 5) - 2, neg);
        if (r & 2) v->m = (r & 4) ? 0xFFFFFFFFFFFFFFFFu : 0x8000000000000000u;
        break;
    }
    case 9:                                       /* denormal, any bits */
        x80gen_set(v, (uint16_t)(neg ? 0x8000 : 0), x80gen_u64(g) >> (r >> 58));
        break;
    case 10: {                                    /* pow: log2 of a base */
        static const uint64_t b[] = {               /* 3/4 7/8 4/3 8/7 */
            0xC000000000000000u, 0xE000000000000000u,
            0xAAAAAAAAAAAAAAABu, 0x9249249249249249u,
        };
        if (r & 2) {
            unsigned k = x80gen_below(g, 4);
            x80gen_set(v, (uint16_t)(k < 2 ? 0x3FFE : 0x3FFF), b[k]);
        } else {
            x80gen_normal(g, v, 0x3FFE + (int)(r >> 3 & 1), 0);
        }
        break;
    }
    case 11:                                      /* pow: 2^x - 1 */
        if (r & 2) x80gen_normal(g, v, 0x3FFE - (int)x80gen_below(g, 8), neg);
        else       x80gen_normal(g, v, 0x3FFF - 67 + (int)x80gen_below(g, 7) - 3, neg);
        break;
    case 12:                                      /* trig: near k pi/2 */
        x80gen_kpi2(v, (uint32_t)(x80gen_u64(g) >> (40 + (r >> 60))));
        if (v->m) v->m += (uint64_t)(int64_t)(int)(x80gen_below(g, 9) - 4);
        v->se |= (uint16_t)(neg ? 0x8000 : 0);
        break;
    case 13:                                      /* trig: the thresholds */
        x80gen_normal(g, v, (r & 2 ? 0x3FBA : 0x3FFF + 63) +
                            (int)x80gen_below(g, 5) - 2, neg);
        break;
    case 14:                                      /* invalid encodings */
        x80gen_set(v, (uint16_t)(x80gen_u64(g) | (r & 2 ? 0x7FFF : 0)),
                   x80gen_u64(g) & 0x7FFFFFFFFFFFFFFFu);
        break;
    default:                                      /* noise */
        x80gen_set(v, (uint16_t)x80gen_u64(g), x80gen_u64(g));
        break;
    }
}

/* A value related to `a`, as a second operand: the same, its negation, a
   power of two away, a few units in the last place away, the same exponent
   or one nearby with other bits, `a` cut to a float's or a double's
   precision, or at an exponent distance where some instruction changes how
   it works.  These make the exact cancellations, the ties in sums and
   quotients, and the unordered and equal compares that independent values
   almost never do. */
static inline void x80gen_near(X80Rng *g, const X80 *a, X80 *v)
{
    uint64_t r = x80gen_u64(g);
    int e = a->se & 0x7FFF, k = (int)x80gen_below(g, 141) - 70;

    *v = *a;
    switch (x80gen_below(g, 11)) {
    case 0:  break;                                            /* equal    */
    case 1:  v->se ^= 0x8000; break;                           /* negated  */
    case 2:  e += k; if (e >= 1 && e <= 0x7FFE)                /* * 2^k    */
                 v->se = (uint16_t)((a->se & 0x8000) | e);
             break;
    case 3:  v->m += (uint64_t)(int64_t)(int)(x80gen_below(g, 7) - 3); break;
    case 4:  v->m = x80gen_u64(g) | 0x8000000000000000u; break; /* same exp */
    case 5:  x80gen_normal(g, v, e + k / 4, (int)(r & 1)); break;
    case 6:  v->m &= 0xFFFFFF0000000000u; break;               /* to float */
    case 7:  v->m &= 0xFFFFFFFFFFFFF800u; break;               /* to double*/
    case 8:  v->m ^= x80gen_u64(g) >> (r >> 58 | 32); break;   /* low bits */
    case 9: {                                                  /* the gaps */
        static const int gap[] = { 41, 42, 63, 64, 65, 66, 67, 68 };
        int d = gap[x80gen_below(g, sizeof gap / sizeof *gap)];
        x80gen_normal(g, v, r & 2 ? e + d : e - d, (int)(r & 1));
        break;
    }
    default: x80gen_value(g, v); break;
    }
}

/* A float or a double as its bits: named ones, everyday ones, noise, and
   `a` cut to the format, give or take a unit, so that memory operands meet
   the register values they are compared with and subtracted from. */
static inline uint64_t x80gen_ieee(X80Rng *g, const X80 *a, int dbl)
{
    static const uint64_t f[] = {
        0x00000000u, 0x00000001u, 0x007FFFFFu, 0x00800000u, 0x3F800000u,
        0x7F7FFFFFu, 0x7F800000u, 0x7FC00000u, 0x7FFFFFFFu, 0x7F800001u,
        0x7FA00000u, 0x7FBFFFFFu, 0x3F000000u, 0x4B000000u, 0x4F000000u,
    };
    static const uint64_t d[] = {
        0x0000000000000000u, 0x0000000000000001u, 0x000FFFFFFFFFFFFFu,
        0x0010000000000000u, 0x3FF0000000000000u, 0x7FEFFFFFFFFFFFFFu,
        0x7FF0000000000000u, 0x7FF8000000000000u, 0x7FFFFFFFFFFFFFFFu,
        0x7FF0000000000001u, 0x7FF4000000000000u, 0x7FF7FFFFFFFFFFFFu,
        0x3FE0000000000000u, 0x4330000000000000u, 0x43E0000000000000u,
    };
    int mb = dbl ? 52 : 23, bias = dbl ? 1023 : 127, emax = dbl ? 2047 : 255;
    uint64_t sign = 1ull << (dbl ? 63 : 31), r = x80gen_u64(g), bits;
    int e;

    switch (x80gen_below(g, 5)) {
    case 0:                                             /* named */
        bits = dbl ? d[x80gen_below(g, sizeof d / sizeof *d)]
                   : f[x80gen_below(g, sizeof f / sizeof *f)];
        return bits | (r & 1 ? sign : 0);
    case 1:                                             /* everyday */
        e = bias + (int)x80gen_below(g, 61) - 30;
        return (r & 1 ? sign : 0) | (uint64_t)e << mb |
               (x80gen_u64(g) & ((1ull << mb) - 1));
    case 2:                                             /* noise */
        return dbl ? x80gen_u64(g) : (uint32_t)x80gen_u64(g);
    default:                                            /* near a */
        e = (a->se & 0x7FFF) - 0x3FFF + bias;
        if (e < 1 || e >= emax || !(a->m >> 63)) return x80gen_ieee(g, a, dbl);
        bits = (a->se & 0x8000 ? sign : 0) | (uint64_t)e << mb |
               (a->m << 1 >> (64 - mb));
        return bits + (uint64_t)(int64_t)(int)(x80gen_below(g, 5) - 2);
    }
}

/* An integer of `width` bytes, sign-extended: the edges of every width,
   small ones, `a` truncated, and noise. */
static inline int64_t x80gen_int(X80Rng *g, const X80 *a, unsigned width)
{
    static const int64_t n[] = {
        0, 1, -1, 2, 32767, -32768, 32766, -32767, 2147483647,
        -2147483647 - 1, 2147483646, -2147483647,
        9223372036854775807, -9223372036854775807 - 1,
    };
    int64_t v;
    int e = (a->se & 0x7FFF) - 0x3FFF;

    switch (x80gen_below(g, 4)) {
    case 0:  v = n[x80gen_below(g, sizeof n / sizeof *n)]; break;
    case 1:  v = (int64_t)x80gen_below(g, 201) - 100; break;
    case 2:  v = e >= 0 && e < 63 ? (int64_t)(a->m >> (63 - e)) : 0;
             if (a->se & 0x8000) v = -v;
             break;
    default: v = (int64_t)x80gen_u64(g); break;
    }
    if (width == 2) return (int16_t)v;
    if (width == 4) return (int32_t)v;
    return v;
}

#endif
