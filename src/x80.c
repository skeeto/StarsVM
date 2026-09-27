/* x80.c - the x87's exact operations, in integers.  See x80.h.
 *
 * Every operation here has one correct answer, the one IEEE 754 and the x87
 * manual define: the exact result, rounded once to the destination under the
 * control word.  So an operation works out its exact result as a sign, an
 * exponent and a significand of up to 128 bits plus a sticky bit, and hands
 * it to round_pack, the one place rounding happens.  What is particular to
 * the x87 rather than to IEEE is which exceptions an operation raises and in
 * what order, which NaN it returns, how it treats the encodings the 387
 * stopped supporting, and what it does with C1; those are written out where
 * they arise, and were all checked against silicon by the fuzzer's --x80
 * mode, which runs these functions and x87hw.c's side by side.
 *
 * Nothing here uses floating point, and nothing depends on the host but the
 * integer widths: unsigned __int128 where the compiler has it, 32-bit
 * halves where it does not (or with -DX80_NO_INT128, which must give the
 * same results).
 */
#include "x80.h"

#if defined(__SIZEOF_INT128__) && !defined(X80_NO_INT128)
#  define X80_INT128 1
#endif

/* Status word bits. */
#define IE 0x0001u
#define DE 0x0002u
#define ZE 0x0004u
#define OE 0x0008u
#define UE 0x0010u
#define PE 0x0020u
#define C0 0x0100u
#define C1 0x0200u
#define C2 0x0400u
#define C3 0x4000u

/* ---------------------------------------------------------- integer helpers */

static inline int clz64(uint64_t x) { return __builtin_clzll(x); }

/* a * b as hi:lo. */
static inline void mul64(uint64_t a, uint64_t b, uint64_t *hi, uint64_t *lo)
{
#ifdef X80_INT128
    unsigned __int128 p = (unsigned __int128)a * b;
    *hi = (uint64_t)(p >> 64);
    *lo = (uint64_t)p;
#else
    uint64_t a0 = a & 0xFFFFFFFFu, a1 = a >> 32;
    uint64_t b0 = b & 0xFFFFFFFFu, b1 = b >> 32;
    uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    uint64_t mid = (p00 >> 32) + (p01 & 0xFFFFFFFFu) + (p10 & 0xFFFFFFFFu);
    *lo = mid << 32 | (p00 & 0xFFFFFFFFu);
    *hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
#endif
}

/* hi:lo / d, for hi < d, which makes the quotient fit in 64 bits; the
   remainder through *r.  Without __int128 this is Knuth's algorithm D in
   32-bit digits, as Hacker's Delight gives it (divlu). */
static inline uint64_t div128(uint64_t hi, uint64_t lo, uint64_t d, uint64_t *r)
{
#ifdef X80_INT128
    unsigned __int128 n = (unsigned __int128)hi << 64 | lo;
    uint64_t q = (uint64_t)(n / d);
    *r = lo - q * d;
    return q;
#else
    const uint64_t b = 1ull << 32;
    uint64_t vn1, vn0, un32, un21, un10, un1, un0, q1, q0, rhat;
    int s = clz64(d);

    d <<= s;
    vn1 = d >> 32;
    vn0 = d & 0xFFFFFFFFu;
    un32 = s ? hi << s | lo >> (64 - s) : hi;
    un10 = lo << s;
    un1 = un10 >> 32;
    un0 = un10 & 0xFFFFFFFFu;

    q1 = un32 / vn1;
    rhat = un32 - q1 * vn1;
    while (q1 >= b || q1 * vn0 > (rhat << 32 | un1)) {
        q1--;
        rhat += vn1;
        if (rhat >= b) break;
    }
    un21 = (un32 << 32 | un1) - q1 * d;

    q0 = un21 / vn1;
    rhat = un21 - q0 * vn1;
    while (q0 >= b || q0 * vn0 > (rhat << 32 | un0)) {
        q0--;
        rhat += vn1;
        if (rhat >= b) break;
    }
    *r = ((un21 << 32 | un0) - q0 * d) >> s;
    return q1 << 32 | q0;
#endif
}

/* floor(sqrt(x)) for any 64-bit x, a bit at a time. */
static uint32_t isqrt64(uint64_t x)
{
    uint64_t r = 0, bit = 1ull << 62;

    while (bit > x) bit >>= 2;
    while (bit) {
        if (x >= r + bit) {
            x -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return (uint32_t)r;
}

/* floor(sqrt(hi:lo)) for hi >= 2^62, which is 64 bits with the top one set,
   and whether it was exact, less than half a unit short, or more.  The root
   of the high word gives the top 32 bits and one division the rest, which
   can be out by a unit or two either way and is put right against the exact
   remainder.  The next bit below the root is set exactly when the
   remainder exceeds the root, since (r + 1/2)^2 = r^2 + r + 1/4, and the
   root of an integer is never exactly half-way, so that bit and whether
   the remainder is zero are all the rounding needs. */
static uint64_t isqrt128(uint64_t hi, uint64_t lo, int *guard, int *sticky)
{
    uint64_t r1 = isqrt64(hi), rem1 = hi - r1 * r1, q, qr, r, sh, sl, rh, rl;

    /* (rem1 * 2^32 + lo / 2^32) / (2 r1): rem1 <= 2 r1 < 2^34, so the
       dividend is at most 66 bits and its high word below the divisor.  The
       quotient can come out a little over 32 bits; capped, the fix-up below
       still reaches the root. */
    q = div128(rem1 >> 32, rem1 << 32 | lo >> 32, r1 << 1, &qr);
    (void)qr;
    if (q > 0xFFFFFFFFu) q = 0xFFFFFFFFu;
    r = r1 << 32 | q;

    /* r*r <= hi:lo < (r+1)*(r+1), without ever forming (r+1)^2, which need
       not fit: the remainder hi:lo - r*r is at most 2r, a 65-bit number, and
       r + 1 fits when the remainder reaches 2r + 1. */
    for (;;) {
        mul64(r, r, &sh, &sl);
        if (sh > hi || (sh == hi && sl > lo)) { r--; continue; }
        rl = lo - sl;
        rh = hi - sh - (lo < sl);
        if (rh > r >> 63 || (rh == r >> 63 && rl >= (r << 1 | 1))) { r++; continue; }
        *sticky = rh || rl;
        *guard = rh || rl > r;
        return r;
    }
}

/* --------------------------------------------------------------- unpacking */

enum { K_ZERO, K_FIN, K_INF, K_QNAN, K_SNAN, K_BAD };

/* An operand, classified.  A finite one is m * 2^(e - 63) with m's top bit
   set, whatever encoding it came from; `den` says it came as a denormal (or
   the 387's pseudo-denormal), which is what the denormal exception is about.
   A NaN keeps its significand in m, for choosing between two of them. */
typedef struct {
    int      k, neg, den;
    int32_t  e;
    uint64_t m;
} Un;

static void unpack(const X80 *x, Un *u)
{
    int be = x->se & 0x7FFF;

    u->neg = x->se >> 15;
    u->den = 0;
    u->e = 0;
    u->m = x->m;
    if (be == 0) {
        if (!x->m) { u->k = K_ZERO; return; }
        /* A denormal is m * 2^(-16382 - 63), J clear; a pseudo-denormal is
           the same with J set, and the 387 onwards reads it that way. */
        u->k = K_FIN;
        u->den = 1;
        u->e = -16382 - clz64(x->m);
        u->m = x->m << clz64(x->m);
    } else if (!(x->m >> 63)) {
        u->k = K_BAD;                   /* unnormal, pseudo-inf, pseudo-NaN */
    } else if (be == 0x7FFF) {
        u->k = !(x->m << 1) ? K_INF : x->m >> 62 & 1 ? K_QNAN : K_SNAN;
    } else {
        u->k = K_FIN;
        u->e = be - 16383;
    }
}

/* A float or a double, as its bits, by the same rules: a signalling NaN
   stays signalling until an operation sees it. */
static void unpack_ieee(uint64_t bits, int dbl, Un *u)
{
    int mb = dbl ? 52 : 23, emax = dbl ? 2047 : 255, bias = dbl ? 1023 : 127;
    int be = (int)(bits >> mb) & emax;
    uint64_t f = bits & ((1ull << mb) - 1);

    u->neg = (int)(bits >> (mb + (dbl ? 11 : 8))) & 1;
    u->den = 0;
    u->e = 0;
    u->m = f << (63 - mb) | 1ull << 63;
    if (be == 0) {
        if (!f) { u->k = K_ZERO; u->m = 0; return; }
        u->k = K_FIN;
        u->den = 1;
        u->m = f << clz64(f);
        u->e = 1 - bias - mb + 63 - clz64(f);
    } else if (be == emax) {
        u->k = !f ? K_INF : f >> (mb - 1) ? K_QNAN : K_SNAN;
    } else {
        u->k = K_FIN;
        u->e = be - bias;
    }
}

static void set(X80 *r, int neg, uint16_t e, uint64_t m)
{
    r->se = (uint16_t)((neg ? 0x8000u : 0) | e);
    r->m = m;
}

static void indefinite(X80 *r) { set(r, 1, 0x7FFF, 0xC000000000000000u); }
static void inf(X80 *r, int neg) { set(r, neg, 0x7FFF, 0x8000000000000000u); }
static void zero(X80 *r, int neg) { set(r, neg, 0, 0); }

/* The NaN a one-operand operation returns for a NaN operand: the same,
   quiet, with IE if it was signalling. */
static void nan1(X80Env *env, const Un *a, X80 *r)
{
    if (a->k == K_SNAN) env->sw |= IE;
    set(r, a->neg, 0x7FFF, a->m | 0x4000000000000000u);
}

/* The NaN a two-operand operation returns when either operand is one.  A
   signalling NaN raises IE.  Of two, a quiet one beats a signalling one,
   and otherwise the larger significand wins; with equal significands, the
   positive one, which the fuzzer measured. */
static void nan2(X80Env *env, const Un *a, const Un *b, X80 *r)
{
    const Un *w;

    if (a->k == K_SNAN || b->k == K_SNAN) env->sw |= IE;
    if (a->k < K_QNAN)      w = b;
    else if (b->k < K_QNAN) w = a;
    else if (a->k != b->k)  w = a->k == K_QNAN ? a : b;
    else if (a->m != b->m)  w = b->m > a->m ? b : a;
    else                    w = a->neg ? b : a;
    set(r, w->neg, 0x7FFF, w->m | 0x4000000000000000u);
}

static int is_nan(const Un *u) { return u->k == K_QNAN || u->k == K_SNAN; }

/* ---------------------------------------------------------------- rounding */

/* Where a result goes: p bits of precision, normal exponents emin to emax
   (emax being the bias too), and whether it is a register, whose significand
   keeps its integer bit, or a float or a double, whose does not.  A denormal
   keeps what fits of p bits at 2^emin, and that holds for a register too: a
   precision control of 24 or 53 bits denormalises at 2^(emin - 23) or
   2^(emin - 52), not at the 64-bit format's own 2^(emin - 63), which the
   fuzzer measured. */
typedef struct { int p, reg; int32_t emin, emax; } Fmt;

static const Fmt FMT_F32 = { 24, 0, -126, 127 };
static const Fmt FMT_F64 = { 53, 0, -1022, 1023 };
static const Fmt FMT_R64 = { 64, 1, -16382, 16383 };

/* The register format under the control word's precision control.  PC 01
   is reserved, and the x87 rounds to the full 64 bits under it, which the
   fuzzer measured too. */
static Fmt fmt_reg(uint16_t cw)
{
    static const int p[4] = { 24, 64, 53, 64 };
    Fmt f = FMT_R64;
    f.p = p[cw >> 8 & 3];
    return f;
}

/* (hi:lo), with `sticky` for anything nonzero below lo, shifted right by
   s >= 64: the bits kept, the first one dropped, and whether anything below
   that was nonzero. */
static uint64_t shr_round(uint64_t hi, uint64_t lo, int sticky, int32_t s,
                          int *g, int *st)
{
    if (s == 64) {
        *g = (int)(lo >> 63);
        *st = (lo << 1) || sticky;
        return hi;
    }
    if (s < 128) {
        int k = (int)s - 64;                              /* 1..63 */
        *g = (int)(hi >> (k - 1) & 1);
        *st = (hi & ((1ull << (k - 1)) - 1)) || lo || sticky;
        return hi >> k;
    }
    if (s == 128) {
        *g = (int)(hi >> 63);
        *st = (hi << 1) || lo || sticky;
        return 0;
    }
    *g = 0;
    *st = hi || lo || sticky;
    return 0;
}

/* Whether rounding control rounds kept up, given the dropped bits. */
static int round_up(uint16_t cw, int neg, uint64_t kept, int g, int st)
{
    switch (cw >> 10 & 3) {
    case 0:  return g && (st || (kept & 1));             /* nearest, even */
    case 1:  return neg && (g || st);                    /* down          */
    case 2:  return !neg && (g || st);                   /* up            */
    default: return 0;                                   /* toward zero   */
    }
}

/* The result of an overflow: infinity, or the largest finite value when the
   rounding direction is toward zero from where the result lies. */
static int overflow_to_inf(uint16_t cw, int neg)
{
    switch (cw >> 10 & 3) {
    case 0:  return 1;
    case 1:  return neg;
    case 2:  return !neg;
    default: return 0;
    }
}

/* Round (hi:lo) * 2^(e - 127), hi's top bit set, plus sticky, to fmt under
   the control word, raise what that raises, and set C1 if the magnitude went
   up.  The result is a sign, a biased exponent and a significand in the
   destination's own layout: for a register the 64-bit significand with J,
   for a float or double the fraction below its implicit bit, which is how
   a denormal's is laid out too. */
typedef struct { int neg; int32_t be; uint64_t m; } Packed;

static void round_pack(X80Env *env, const Fmt *f, int neg, int32_t e,
                       uint64_t hi, uint64_t lo, int sticky, Packed *out)
{
    uint16_t cw = env->cw;
    uint64_t top = 1ull << (f->p - 1), kept;
    int32_t s = 128 - f->p;                  /* bits dropped at precision p */
    int g, st, up;

    kept = shr_round(hi, lo, sticky, s, &g, &st);
    up = round_up(cw, neg, kept, g, st);
    out->neg = neg;

    /* Tiny after rounding with the exponent unbounded, which is how the x86
       decides it: below 2^emin even once rounded to p bits.  Only a value
       just under 2^emin can round its way out. */
    if (e >= f->emin || (e == f->emin - 1 && up && kept == (top << 1) - 1)) {
        if (up && ++kept == top << 1) { kept = top; e++; }
        if (e > f->emax) {
            env->sw |= OE | PE;
            if (overflow_to_inf(cw, neg)) {
                env->sw |= C1;
                out->be = 2 * f->emax + 1;             /* all ones */
                out->m = f->reg ? 1ull << 63 : 0;
            } else {
                out->be = 2 * f->emax;                 /* the largest */
                out->m = f->reg ? ~0ull << (64 - f->p) : top - 1;
            }
            return;
        }
        if (g || st) env->sw |= PE;
        if (up) env->sw |= C1;
        out->be = e + f->emax;
        out->m = f->reg ? kept << (64 - f->p) : kept - top;
        return;
    }

    /* Denormal: rounded at 2^(emin - (p - 1)) instead, which is emin - e
       bits further up.  Underflow is flagged only when that loses something. */
    kept = shr_round(hi, lo, sticky, s + f->emin - e, &g, &st);
    up = round_up(cw, neg, kept, g, st);
    if (up) kept++;
    if (g || st) env->sw |= PE | UE;
    if (up) env->sw |= C1;
    /* kept counts units of 2^(emin - (p - 1)), so reaching `top` is 2^emin:
       rounding up into the least normal, which is encoded as one. */
    out->be = kept >= top;
    out->m = f->reg ? kept << (64 - f->p) : kept & (top - 1);
}

static void to_reg(const Packed *p, X80 *r)
{
    set(r, p->neg, (uint16_t)p->be, p->m);
}

static uint64_t to_ieee(const Packed *p, int dbl)
{
    int mb = dbl ? 52 : 23;
    return (uint64_t)p->neg << (mb + (dbl ? 11 : 8)) |
           (uint64_t)p->be << mb | p->m;
}

/* Round an exact result into a register. */
static void round_reg(X80Env *env, const Fmt *f, int neg, int32_t e,
                      uint64_t hi, uint64_t lo, int sticky, X80 *r)
{
    Packed p;
    round_pack(env, f, neg, e, hi, lo, sticky, &p);
    to_reg(&p, r);
}

/* ------------------------------------------------------------- arithmetic */

/* a + b, with b's sign already flipped for a subtraction.  Both finite or
   zero.  Exact until round_reg: the smaller operand is aligned into a
   128-bit field, anything below that kept as a sticky bit, and 128 bits is
   room enough that a cancellation after an alignment of more than a bit or
   two cannot reach the part that was jammed. */
static void add_fin(X80Env *env, const Fmt *f, const Un *a, const Un *b, X80 *r)
{
    const Un *x = a, *y = b;
    uint64_t xh, xl, yh, yl, h, l;
    int32_t d, e;
    int sticky = 0, neg;

    if (a->k == K_ZERO && b->k == K_ZERO) {
        /* Zeros of opposite sign sum to +0, but to -0 rounding down. */
        zero(r, a->neg == b->neg ? a->neg : (env->cw >> 10 & 3) == 1);
        return;
    }
    if (b->k == K_ZERO) { round_reg(env, f, a->neg, a->e, a->m, 0, 0, r); return; }
    if (a->k == K_ZERO) { round_reg(env, f, b->neg, b->e, b->m, 0, 0, r); return; }

    /* x the larger in magnitude. */
    if (b->e > a->e || (b->e == a->e && b->m > a->m)) { x = b; y = a; }
    d = x->e - y->e;
    xh = x->m;
    xl = 0;
    if (d == 0)        { yh = y->m; yl = 0; }
    else if (d < 64)   { yh = y->m >> d; yl = y->m << (64 - d); }
    else if (d == 64)  { yh = 0; yl = y->m; }
    else if (d < 128)  { yh = 0; yl = y->m >> (d - 64); sticky = (y->m << (128 - d)) != 0; }
    else               { yh = 0; yl = 0; sticky = 1; }
    e = x->e;
    neg = x->neg;

    if (x->neg == y->neg) {
        l = xl + yl;
        h = xh + yh + (l < xl);
        if (h < xh || (h == xh && yh | (l < xl))) {
            /* A carry out of the top: one bit right, into the sticky. */
            if (h < xh || (h == xh && (yh || l < xl))) {
                sticky |= (int)(l & 1);
                l = l >> 1 | h << 63;
                h = h >> 1 | 1ull << 63;
                e++;
            }
        }
    } else {
        /* Taking away a jammed bit: x - (y + eps) is (x - y - 1) plus a
           fraction in (0, 1), which is still sticky. */
        uint64_t bl = yl + (uint64_t)sticky, bh = yh + (bl < yl);
        l = xl - bl;
        h = xh - bh - (xl < bl);
        if (!h && !l) {
            /* Exact cancellation: +0, or -0 rounding down. */
            zero(r, (env->cw >> 10 & 3) == 1);
            return;
        }
        if (!h) { h = l; l = 0; e -= 64; }
        if (!(h >> 63)) {
            int z = clz64(h);
            h = h << z | l >> (64 - z);
            l <<= z;
            e -= z;
        }
    }
    round_reg(env, f, neg, e, h, l, sticky, r);
}

static void mul_fin(X80Env *env, const Fmt *f, const Un *a, const Un *b, X80 *r)
{
    uint64_t h, l;
    int32_t e = a->e + b->e;

    mul64(a->m, b->m, &h, &l);
    if (h >> 63) e++;
    else { h = h << 1 | l >> 63; l <<= 1; }
    round_reg(env, f, a->neg ^ b->neg, e, h, l, 0, r);
}

static void div_fin(X80Env *env, const Fmt *f, const Un *a, const Un *b, X80 *r)
{
    uint64_t qh, ql, rem;
    int32_t e = a->e - b->e;

    /* A 128-bit quotient with its top bit set, and the remainder for the
       sticky bit: a->m * 2^127 / b->m when a's significand is the larger,
       a->m * 2^128 / b->m when it is not. */
    if (a->m >= b->m) {
        qh = div128(a->m >> 1, a->m << 63, b->m, &rem);
    } else {
        qh = div128(a->m, 0, b->m, &rem);
        e--;
    }
    ql = div128(rem, 0, b->m, &rem);
    round_reg(env, f, a->neg ^ b->neg, e, qh, ql, rem != 0, r);
}

/* The two-operand arithmetic on unpacked operands: the checks every one of
   them makes, in the order the x87 makes them, and then the operation. */
static void arith_un(X80Env *env, int op, const Un *a, const Un *b, X80 *r)
{
    Fmt f = fmt_reg(env->cw);
    const Un *x = a, *y = b;
    Un nb;

    env->cc = C1;
    if (op == X80_SUBR || op == X80_DIVR) { x = b; y = a; }

    if (a->k == K_BAD || b->k == K_BAD) { env->sw |= IE; indefinite(r); return; }
    if (is_nan(a) || is_nan(b)) { nan2(env, a, b, r); return; }

    switch (op) {
    case X80_ADD: case X80_SUB: case X80_SUBR:
        nb = *y;
        if (op != X80_ADD) nb.neg ^= 1;
        if (x->k == K_INF || nb.k == K_INF) {
            if (x->k == K_INF && nb.k == K_INF && x->neg != nb.neg) {
                env->sw |= IE;
                indefinite(r);
                return;
            }
            if (a->den || b->den) env->sw |= DE;
            inf(r, x->k == K_INF ? x->neg : nb.neg);
            return;
        }
        if (a->den || b->den) env->sw |= DE;
        add_fin(env, &f, x, &nb, r);
        return;

    case X80_MUL:
        if ((a->k == K_ZERO && b->k == K_INF) || (a->k == K_INF && b->k == K_ZERO)) {
            env->sw |= IE;
            indefinite(r);
            return;
        }
        if (a->den || b->den) env->sw |= DE;
        if (a->k == K_INF || b->k == K_INF) { inf(r, a->neg ^ b->neg); return; }
        if (a->k == K_ZERO || b->k == K_ZERO) { zero(r, a->neg ^ b->neg); return; }
        mul_fin(env, &f, a, b, r);
        return;

    default:                                            /* x / y */
        if ((x->k == K_ZERO && y->k == K_ZERO) || (x->k == K_INF && y->k == K_INF)) {
            env->sw |= IE;
            indefinite(r);
            return;
        }
        /* Divide-by-zero outranks the denormal operand, and with both masked
           only the higher is reported. */
        if (y->k == K_ZERO && x->k != K_INF) {
            env->sw |= ZE;
            inf(r, x->neg ^ y->neg);
            return;
        }
        if (a->den || b->den) env->sw |= DE;
        if (x->k == K_INF)  { inf(r, x->neg ^ y->neg); return; }
        if (y->k == K_INF)  { zero(r, x->neg ^ y->neg); return; }
        if (x->k == K_ZERO) { zero(r, x->neg ^ y->neg); return; }
        div_fin(env, &f, x, y, r);
        return;
    }
}

void x80_arith(X80Env *env, int op, const X80 *a, const X80 *b, X80 *r)
{
    Un ua, ub;
    unpack(a, &ua);
    unpack(b, &ub);
    arith_un(env, op, &ua, &ub, r);
}

void x80_arith_f32(X80Env *env, int op, const X80 *a, uint32_t m, X80 *r)
{
    Un ua, ub;
    unpack(a, &ua);
    unpack_ieee(m, 0, &ub);
    arith_un(env, op, &ua, &ub, r);
}

void x80_arith_f64(X80Env *env, int op, const X80 *a, uint64_t m, X80 *r)
{
    Un ua, ub;
    unpack(a, &ua);
    unpack_ieee(m, 1, &ub);
    arith_un(env, op, &ua, &ub, r);
}

/* ---------------------------------------------------------------- compares */

/* C3 C2 C0 for a against b: 000 greater, 001 less, 100 equal, 111
   unordered; C1 clear.  FCOM raises IE for any NaN, FUCOM only for a
   signalling one; both for the unsupported encodings. */
static void compare_un(X80Env *env, const Un *a, const Un *b, int unordered)
{
    int c;

    env->cc = C0 | C1 | C2 | C3;
    if (a->k == K_BAD || b->k == K_BAD || is_nan(a) || is_nan(b)) {
        if (a->k == K_BAD || b->k == K_BAD || a->k == K_SNAN || b->k == K_SNAN ||
            !unordered)
            env->sw |= IE;
        env->sw |= C0 | C2 | C3;
        return;
    }
    if (a->den || b->den) env->sw |= DE;
    if (a->k == K_ZERO && b->k == K_ZERO) c = 0;
    else if (a->k == K_ZERO) c = b->neg ? 1 : -1;
    else if (b->k == K_ZERO) c = a->neg ? -1 : 1;
    else if (a->neg != b->neg) c = a->neg ? -1 : 1;
    else {
        /* Same sign: by magnitude, infinity above everything. */
        int32_t ea = a->k == K_INF ? 0x7FFFFFFF : a->e;
        int32_t eb = b->k == K_INF ? 0x7FFFFFFF : b->e;
        uint64_t ma = a->k == K_INF ? 0 : a->m, mb = b->k == K_INF ? 0 : b->m;
        c = ea != eb ? (ea > eb ? 1 : -1) : ma != mb ? (ma > mb ? 1 : -1) : 0;
        if (a->neg) c = -c;
    }
    if (c < 0) env->sw |= C0;
    else if (!c) env->sw |= C3;
}

void x80_compare(X80Env *env, const X80 *a, const X80 *b)
{
    Un ua, ub;
    unpack(a, &ua);
    unpack(b, &ub);
    compare_un(env, &ua, &ub, 0);
}

void x80_ucompare(X80Env *env, const X80 *a, const X80 *b)
{
    Un ua, ub;
    unpack(a, &ua);
    unpack(b, &ub);
    compare_un(env, &ua, &ub, 1);
}

void x80_compare_f32(X80Env *env, const X80 *a, uint32_t m)
{
    Un ua, ub;
    unpack(a, &ua);
    unpack_ieee(m, 0, &ub);
    compare_un(env, &ua, &ub, 0);
}

void x80_compare_f64(X80Env *env, const X80 *a, uint64_t m)
{
    Un ua, ub;
    unpack(a, &ua);
    unpack_ieee(m, 1, &ub);
    compare_un(env, &ua, &ub, 0);
}

void x80_tst(X80Env *env, const X80 *a)
{
    Un ua, z;
    unpack(a, &ua);
    z.k = K_ZERO;
    z.neg = z.den = 0;
    z.e = 0;
    z.m = 0;
    compare_un(env, &ua, &z, 0);
}

/* FXAM's classes, C3 C2 C0: 000 unsupported, 001 NaN, 010 normal, 011
   infinity, 100 zero, 110 denormal; C1 the sign. */
void x80_xam(X80Env *env, const X80 *a)
{
    int be = a->se & 0x7FFF;
    uint16_t c;

    env->cc = C0 | C1 | C2 | C3;
    if (be == 0)                c = a->m ? C3 | C2 : C3;
    else if (!(a->m >> 63))     c = 0;
    else if (be == 0x7FFF)      c = a->m << 1 ? C0 : C2 | C0;
    else                        c = C2;
    env->sw |= (uint16_t)(c | (a->se & 0x8000 ? C1 : 0));
}

/* ------------------------------------------------------------ one operand */

void x80_abs(X80Env *env, const X80 *a, X80 *r)
{
    env->cc = C1;
    *r = *a;
    r->se &= 0x7FFF;
}

void x80_chs(X80Env *env, const X80 *a, X80 *r)
{
    env->cc = C1;
    *r = *a;
    r->se ^= 0x8000;
}

void x80_sqrt(X80Env *env, const X80 *a, X80 *r)
{
    Fmt f = fmt_reg(env->cw);
    Un u;
    uint64_t rt;
    int g, st;
    int32_t e;

    env->cc = C1;
    unpack(a, &u);
    switch (u.k) {
    case K_BAD:  env->sw |= IE; indefinite(r); return;
    case K_QNAN: case K_SNAN: nan1(env, &u, r); return;
    case K_ZERO: *r = *a; return;
    case K_INF:
        if (u.neg) { env->sw |= IE; indefinite(r); }
        else       *r = *a;
        return;
    }
    if (u.neg) { env->sw |= IE; indefinite(r); return; }   /* outranks DE */
    if (u.den) env->sw |= DE;
    /* m * 2^(e - 63): with e odd the root of m * 2^64, with e even the root
       of m * 2^63, so that what is left of the exponent halves exactly; the
       root's top bit then stands for 2^floor(e / 2). */
    if (u.e & 1) rt = isqrt128(u.m, 0, &g, &st);
    else         rt = isqrt128(u.m >> 1, u.m << 63, &g, &st);
    e = (u.e - (u.e & 1)) / 2;
    round_reg(env, &f, 0, e, rt, g ? 1ull << 63 : 0, st, r);
}

/* Round to an integer by the rounding control, at full precision whatever
   the precision control says. */
void x80_rndint(X80Env *env, const X80 *a, X80 *r)
{
    Un u;
    int g, st, up;
    int32_t s;
    uint64_t kept;

    env->cc = C1;
    unpack(a, &u);
    switch (u.k) {
    case K_BAD:  env->sw |= IE; indefinite(r); return;
    case K_QNAN: case K_SNAN: nan1(env, &u, r); return;
    case K_ZERO: case K_INF: *r = *a; return;
    }
    if (u.den) env->sw |= DE;
    if (u.e >= 63) { *r = *a; return; }         /* already an integer */
    /* The integer part is m >> (63 - e), for e < 0 nothing at all. */
    s = 64 + 63 - u.e;
    kept = shr_round(u.m, 0, 0, s, &g, &st);
    if (!g && !st) { set(r, u.neg, (uint16_t)(u.e + 16383), u.m); return; }
    env->sw |= PE;
    up = round_up(env->cw, u.neg, kept, g, st);
    if (up) { kept++; env->sw |= C1; }
    if (!kept) { zero(r, u.neg); return; }
    {
        int z = clz64(kept);
        set(r, u.neg, (uint16_t)(16383 + 63 - z), kept << z);
    }
}

/* ST(0) * 2^trunc(ST(1)).  The scale is truncated toward zero, and one
   beyond any exponent there is saturates to one that overflows or
   underflows everything. */
void x80_scale(X80Env *env, const X80 *a, const X80 *b, X80 *r)
{
    Un ua, ub;
    int32_t n;

    env->cc = C1;
    unpack(a, &ua);
    unpack(b, &ub);
    if (ua.k == K_BAD || ub.k == K_BAD) { env->sw |= IE; indefinite(r); return; }
    if (is_nan(&ua) || is_nan(&ub)) { nan2(env, &ua, &ub, r); return; }
    if (ub.k == K_INF) {
        if ((ua.k == K_ZERO && !ub.neg) || (ua.k == K_INF && ub.neg)) {
            env->sw |= IE;
            indefinite(r);
            return;
        }
        if (ua.den) env->sw |= DE;
        if (ua.k == K_ZERO || ua.k == K_INF) { *r = *a; return; }
        if (ub.neg) zero(r, ua.neg);
        else        inf(r, ua.neg);
        return;
    }
    if (ua.den || ub.den) env->sw |= DE;
    if (ua.k == K_ZERO || ua.k == K_INF) { *r = *a; return; }
    if (ub.k == K_ZERO || ub.e < 0) n = 0;
    else if (ub.e > 30) n = ub.neg ? -0x10000 : 0x10000;
    else {
        n = (int32_t)(ub.m >> (63 - ub.e));
        if (n > 0x10000) n = 0x10000;
        if (ub.neg) n = -n;
    }
    round_reg(env, &FMT_R64, ua.neg, ua.e + n, ua.m, 0, 0, r);
}

/* ST(0) into its exponent, left in ST(0) as a value, and its significand,
   pushed, with the exponent zero. */
void x80_xtract(X80Env *env, const X80 *a, X80 *exp, X80 *sig)
{
    Un u;
    X80Env z;

    env->cc = C1;
    unpack(a, &u);
    switch (u.k) {
    case K_BAD:  env->sw |= IE; indefinite(exp); indefinite(sig); return;
    case K_QNAN: case K_SNAN: nan1(env, &u, sig); *exp = *sig; return;
    case K_INF:  *sig = *a; inf(exp, 0); return;
    case K_ZERO: env->sw |= ZE; *sig = *a; inf(exp, 1); return;
    }
    if (u.den) env->sw |= DE;
    set(sig, u.neg, 16383, u.m);
    z = *env;
    x80_from_int(&z, u.e, exp);
}

/* The partial remainder: ST(0) - q * ST(1), q the quotient truncated.  For
   an exponent difference under 64 it is complete, and the low three bits of
   q go in C0, C3 and C1; for more, it reduces by a power of two of the
   quotient only, sets C2, and leaves the rest for the next FPREM. */
void x80_prem(X80Env *env, const X80 *a, const X80 *b, X80 *r)
{
    Un ua, ub;
    int32_t d, n;
    uint64_t rem, q = 0;

    env->cc = C0 | C1 | C2 | C3;
    unpack(a, &ua);
    unpack(b, &ub);
    if (ua.k == K_BAD || ub.k == K_BAD || is_nan(&ua) || is_nan(&ub) ||
        ua.k == K_INF || ub.k == K_ZERO) {
        env->cc = C1 | C2;
        if (ua.k == K_BAD || ub.k == K_BAD) { env->sw |= IE; indefinite(r); }
        else if (is_nan(&ua) || is_nan(&ub)) nan2(env, &ua, &ub, r);
        else { env->sw |= IE; indefinite(r); }
        return;
    }
    if (ua.den || ub.den) env->sw |= DE;
    if (ua.k == K_ZERO) { *r = *a; return; }

    /* A dividend smaller than the divisor is the remainder, but re-encoded
       as the x87 would any result, so a pseudo-denormal comes back as the
       least normal it stands for. */
    d = ua.e - ub.e;
    if (d < 0 || ub.k == K_INF) {
        round_reg(env, &FMT_R64, ua.neg, ua.e, ua.m, 0, 0, r);
        return;
    }
    rem = ua.m;
    if (d >= 64) {
        /* Reduce by the top N bits of the quotient only.  Which N is
           implementation-dependent; this one reduces the exponent
           difference to a multiple of 32 below it. */
        n = 32 + (d % 32);
        d -= n;
    } else {
        n = d;
        d = 0;
    }
    /* Long division, a bit at a time: rem stays below 2 * b.m. */
    {
        int32_t i;
        int carry = 0;
        for (i = 0; i <= n; i++) {
            q <<= 1;
            if (carry || rem >= ub.m) { rem -= ub.m; q |= 1; }
            if (i < n) { carry = (int)(rem >> 63); rem <<= 1; }
        }
    }
    if (!d) {
        env->sw |= (uint16_t)((q & 1 ? C1 : 0) | (q & 2 ? C3 : 0) | (q & 4 ? C0 : 0));
    } else {
        env->sw |= C2;
    }
    if (!rem) { zero(r, ua.neg); return; }
    /* rem * 2^(b.e + d - 63), normalised. */
    {
        int z = clz64(rem);
        round_reg(env, &FMT_R64, ua.neg, ub.e + d - z, rem << z, 0, 0, r);
    }
}

/* ------------------------------------------------------------- conversions */

/* The seven constants, as the x87 holds them: 66 bits, rounded to 64 by
   the rounding control.  The two below the truncated significand are the
   next bits of each, which is all the rounding looks at. */
void x80_constant(X80Env *env, int which, X80 *r)
{
    static const struct { uint16_t e; uint64_t m; unsigned low; } k[] = {
        { 0x3FFF, 0x8000000000000000u, 0 },         /* 1        */
        { 0x4000, 0xD49A784BCD1B8AFEu, 1 },         /* log2 10  */
        { 0x3FFF, 0xB8AA3B295C17F0BBu, 3 },         /* log2 e   */
        { 0x4000, 0xC90FDAA22168C234u, 3 },         /* pi       */
        { 0x3FFD, 0x9A209A84FBCFF798u, 3 },         /* log10 2  */
        { 0x3FFE, 0xB17217F7D1CF79ABu, 3 },         /* ln 2     */
        { 0x0000, 0x0000000000000000u, 0 },         /* 0        */
    };
    unsigned low = k[which].low;
    int up;

    env->cc = C1;
    /* No PE, and C1 clear, whatever the rounding: the x87 treats these as
       exact.  Round to nearest takes the two extra bits as they are. */
    switch (env->cw >> 10 & 3) {
    case 0:  up = low >= 2 && (low > 2 || (k[which].m & 1)); break;
    case 2:  up = low != 0; break;
    default: up = 0; break;
    }
    set(r, 0, k[which].e, k[which].m + (uint64_t)up);
}

void x80_from_int(X80Env *env, int64_t v, X80 *r)
{
    uint64_t m = v < 0 ? 0 - (uint64_t)v : (uint64_t)v;
    int z;

    env->cc = C1;
    if (!m) { zero(r, 0); return; }
    z = clz64(m);
    set(r, v < 0, (uint16_t)(16383 + 63 - z), m << z);
}

/* Loading a float or a double: exact, but a signalling NaN raises IE and
   arrives quiet, and a denormal raises DE and arrives normalised. */
static void from_ieee(X80Env *env, uint64_t bits, int dbl, X80 *r)
{
    Un u;

    env->cc = C1;
    unpack_ieee(bits, dbl, &u);
    switch (u.k) {
    case K_ZERO: zero(r, u.neg); return;
    case K_INF:  inf(r, u.neg); return;
    case K_QNAN: case K_SNAN: nan1(env, &u, r); return;
    }
    if (u.den) env->sw |= DE;
    set(r, u.neg, (uint16_t)(u.e + 16383), u.m);
}

void x80_from_f32(X80Env *env, uint32_t bits, X80 *r) { from_ieee(env, bits, 0, r); }
void x80_from_f64(X80Env *env, uint64_t bits, X80 *r) { from_ieee(env, bits, 1, r); }

/* Storing as a float or a double: rounded to the format, whatever the
   precision control, and a denormal is not reported as one.  A NaN keeps the top of its payload and is quieted;
   an unsupported encoding stores the format's indefinite. */
static uint64_t to_ieee_bits(X80Env *env, const X80 *a, int dbl)
{
    const Fmt *f = dbl ? &FMT_F64 : &FMT_F32;
    int mb = dbl ? 52 : 23, sh = dbl ? 63 : 31;
    uint64_t expall = dbl ? 0x7FFu : 0xFFu, sign;
    Un u;
    Packed p;

    env->cc = C1;
    unpack(a, &u);
    sign = (uint64_t)u.neg << sh;
    switch (u.k) {
    case K_BAD:
        env->sw |= IE;
        return 1ull << sh | expall << mb | 1ull << (mb - 1);
    case K_ZERO:
        return sign;
    case K_INF:
        return sign | expall << mb;
    case K_QNAN: case K_SNAN:
        if (u.k == K_SNAN) env->sw |= IE;
        return sign | expall << mb | (u.m << 1 >> (64 - mb)) | 1ull << (mb - 1);
    }
    round_pack(env, f, u.neg, u.e, u.m, 0, 0, &p);
    return to_ieee(&p, dbl);
}

uint32_t x80_to_f32(X80Env *env, const X80 *a) { return (uint32_t)to_ieee_bits(env, a, 0); }
uint64_t x80_to_f64(X80Env *env, const X80 *a) { return to_ieee_bits(env, a, 1); }

/* FIST: rounded to an integer by the rounding control, then checked against
   the width.  A denormal is not reported as one.  Out of range, or a NaN, an infinity or an unsupported
   encoding, it is IE alone and the width's integer indefinite, the most
   negative value.  The result comes back sign-extended. */
int64_t x80_to_int(X80Env *env, const X80 *a, unsigned width)
{
    Un u;
    uint64_t kept, lim = 1ull << (8 * width - 1);
    int g, st, up;
    int64_t indef = -(int64_t)(lim - 1) - 1;

    env->cc = C1;
    unpack(a, &u);
    if (u.k != K_FIN && u.k != K_ZERO) { env->sw |= IE; return indef; }
    if (u.k == K_ZERO) return 0;
    if (u.e > 63) { env->sw |= IE; return indef; }
    kept = shr_round(u.m, 0, 0, 64 + 63 - u.e, &g, &st);
    up = round_up(env->cw, u.neg, kept, g, st);
    if (up && !++kept) { env->sw |= IE; return indef; }  /* 2^64 */
    if (kept > lim || (kept == lim && !u.neg)) { env->sw |= IE; return indef; }
    if (g || st) env->sw |= PE;
    if (up) env->sw |= C1;
    return u.neg ? -(int64_t)(kept - 1) - 1 : (int64_t)kept;
}
