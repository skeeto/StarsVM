/* x80tx.c - the x87's transcendental instructions, in integers.
 *
 * x80.c includes this at its end, so that it shares that file's unpacking,
 * NaN rules and rounding rather than exporting them; it is a file of its
 * own only because it is long.
 *
 * Unlike the rest of x80.c these have no one right answer to match.  The
 * x87 computes them to a little more than 64 bits and rounds that, and the
 * last bit it gives is not always the correctly rounded one.  So each is
 * computed here to about 2^-120 in a 128-bit software float, and rounded
 * once, correctly - and then, where the x87 is known to do something
 * particular that a correctly rounded result would not, that is done
 * instead.  Those rules are gathered in the block marked "measured", and
 * each is a hypothesis the fuzzer's --x80 mode keeps testing: on random
 * arguments these agree with the x87 in most but not all cases, and the
 * fuzzer counts how often, and by how many units when they do not.
 *
 * Speed is not a concern: the game runs a few hundred of these a turn.
 */

/* ------------------------------------------------------- 128-bit floats */

/* (hi:lo) * 2^(e - 127), hi's top bit set; zero is hi = lo = 0. */
typedef struct { uint64_t hi, lo; int32_t e; int neg; } XW;

static int xw_iszero(const XW *a) { return !a->hi && !a->lo; }

static XW xw(uint64_t hi, uint64_t lo, int32_t e, int neg)
{
    XW r;
    r.hi = hi;
    r.lo = lo;
    r.e = e;
    r.neg = neg;
    return r;
}

static void xw_norm(XW *a)
{
    int z;
    if (!a->hi) {
        if (!a->lo) return;
        a->hi = a->lo;
        a->lo = 0;
        a->e -= 64;
    }
    z = clz64(a->hi);
    if (z) {
        a->hi = a->hi << z | a->lo >> (64 - z);
        a->lo <<= z;
        a->e -= z;
    }
}

static XW xw_from_un(const Un *u) { return xw(u->m, 0, u->e, u->neg); }

static XW xw_from_int(int64_t v)
{
    XW r = xw(v < 0 ? 0 - (uint64_t)v : (uint64_t)v, 0, 63, v < 0);
    xw_norm(&r);
    return r;
}

static XW xw_neg(XW a) { a.neg ^= 1; return a; }

/* a * b, the low half of the 256-bit product dropped. */
static XW xw_mul(const XW *a, const XW *b)
{
    uint64_t p3h, p3l, p2h, p2l, p1h, p1l, p0h, p0l, w1, w2, w3, c1, c2;

    if (xw_iszero(a) || xw_iszero(b)) return xw(0, 0, 0, a->neg ^ b->neg);
    mul64(a->hi, b->hi, &p3h, &p3l);
    mul64(a->hi, b->lo, &p2h, &p2l);
    mul64(a->lo, b->hi, &p1h, &p1l);
    mul64(a->lo, b->lo, &p0h, &p0l);
    (void)p0l;
    w1 = p0h + p1l;
    c1 = w1 < p0h;
    w1 += p2l;
    c1 += w1 < p2l;
    w2 = p3l + p1h;
    c2 = w2 < p3l;
    w2 += p2h;
    c2 += w2 < p2h;
    w2 += c1;
    c2 += w2 < c1;
    w3 = p3h + c2;
    if (w3 >> 63) return xw(w3, w2, a->e + b->e + 1, a->neg ^ b->neg);
    return xw(w3 << 1 | w2 >> 63, w2 << 1 | w1 >> 63, a->e + b->e, a->neg ^ b->neg);
}

/* a + b, bits aligned out of the bottom dropped. */
static XW xw_add(XW a, XW b)
{
    XW t;
    int32_t d;
    uint64_t bh, bl;

    if (xw_iszero(&b)) return a;
    if (xw_iszero(&a)) return b;
    if (b.e > a.e || (b.e == a.e && (b.hi > a.hi || (b.hi == a.hi && b.lo > a.lo)))) {
        t = a; a = b; b = t;
    }
    d = a.e - b.e;
    if (d >= 128) return a;
    if (d >= 64) { bl = b.hi >> (d - 64); bh = 0; }
    else if (d)  { bl = b.lo >> d | b.hi << (64 - d); bh = b.hi >> d; }
    else         { bl = b.lo; bh = b.hi; }
    if (a.neg == b.neg) {
        uint64_t l = a.lo + bl, h = a.hi + bh + (l < a.lo);
        int carry = h < a.hi || (h == a.hi && (bh || l < a.lo));
        if (carry) {
            l = l >> 1 | h << 63;
            h = h >> 1 | 1ull << 63;
            a.e++;
        }
        a.hi = h;
        a.lo = l;
    } else {
        uint64_t l = a.lo - bl, h = a.hi - bh - (a.lo < bl);
        a.hi = h;
        a.lo = l;
        if (!h && !l) return xw(0, 0, 0, 0);
        xw_norm(&a);
    }
    return a;
}

static XW xw_sub(XW a, XW b) { return xw_add(a, xw_neg(b)); }

/* a / b, a bit at a time: 128 bits of quotient, truncated. */
static XW xw_div(const XW *a, const XW *b)
{
    uint64_t rh = a->hi, rl = a->lo, qh = 0, ql = 0;
    int32_t e = a->e - b->e;
    int carry = 0, i;

    if (xw_iszero(a)) return xw(0, 0, 0, a->neg ^ b->neg);
    if (rh < b->hi || (rh == b->hi && rl < b->lo)) {
        carry = (int)(rh >> 63);
        rh = rh << 1 | rl >> 63;
        rl <<= 1;
        e--;
    }
    for (i = 0; i < 128; i++) {
        qh = qh << 1 | ql >> 63;
        ql <<= 1;
        if (carry || rh > b->hi || (rh == b->hi && rl >= b->lo)) {
            uint64_t t = rl - b->lo;
            rh = rh - b->hi - (rl < b->lo);
            rl = t;
            ql |= 1;
        }
        carry = (int)(rh >> 63);
        rh = rh << 1 | rl >> 63;
        rl <<= 1;
    }
    return xw(qh, ql, e, a->neg ^ b->neg);
}

/* Round into a register at full precision - the precision control does not
   apply to these instructions - with the sticky bit set, since every result
   that reaches here is irrational and so inexact: PE always. */
static void xw_round(X80Env *env, const XW *x, X80 *r)
{
    if (xw_iszero(x)) { zero(r, x->neg); env->sw |= PE; return; }
    round_reg(env, &FMT_R64, x->neg, x->e, x->hi, x->lo, 1, r);
}

/* A constant from its 128-bit significand and exponent. */
typedef struct { uint64_t hi, lo; int32_t e; } XK;

static XW xk(const XK *k, int neg) { return xw(k->hi, k->lo, k->e, neg); }

/* --------------------------------------------------------------- tables */

/* Each rounded to nearest at 128 bits, by exact rational arithmetic: the
   series coefficients directly, the rest by series to 400 bits. */

static const XK K_PI     = { 0xC90FDAA22168C234u, 0xC4C6628B80DC1CD1u,  1 };  /* pi      */
static const XK K_PI_2   = { 0xC90FDAA22168C234u, 0xC4C6628B80DC1CD1u,  0 };  /* pi/2    */
static const XK K_PI_4   = { 0xC90FDAA22168C234u, 0xC4C6628B80DC1CD1u, -1 };  /* pi/4    */
static const XK K_3PI_4  = { 0x96CBE3F9990E91A7u, 0x9394C9E8A0A5159Du,  1 };  /* 3pi/4   */
static const XK K_LN2    = { 0xB17217F7D1CF79ABu, 0xC9E3B39803F2F6AFu, -1 };  /* ln 2    */
static const XK K_LOG2E  = { 0xB8AA3B295C17F0BBu, 0xBE87FED0691D3E89u,  0 };  /* log2 e  */
static const XK K_LN8164 = { 0xF1383B7157972F4Fu, 0x543FFF0FF4F0AAEEu, -3 };  /* ln 81/64 */
static const XK K_LN5164 = { 0xE881BF932AF3DAC0u, 0xC524848E3443E040u, -3 };  /* -ln 51/64 */

/* atan(j/8), j = 0..8. */
static const XK ATAN8[9] = {
    { 0, 0, 0 },
    { 0xFEADD4D5617B6E32u, 0xC897989F3E888EF8u, -4 },
    { 0xFADBAFC96406EB15u, 0x6DC79EF5F7A217E6u, -3 },
    { 0xB7B0CA0F26F78473u, 0x8AA32122DCFE4483u, -2 },
    { 0xED63382B0DDA7B45u, 0x6FE445ECBC3A8D03u, -2 },
    { 0x8F005D5EF7F59F9Bu, 0x5C835E1665C43748u, -1 },
    { 0xA4BC7D1934F70924u, 0x19A87F2A457DAC9Fu, -1 },
    { 0xB8053E2BC2319E73u, 0xCB2DA55210A4443Du, -1 },
    { 0xC90FDAA22168C234u, 0xC4C6628B80DC1CD1u, -1 },
};

/* 1/n!, n = 0..33. */
static const XK FACT_INV[34] = {
    { 0x8000000000000000u, 0x0000000000000000u,    0 },
    { 0x8000000000000000u, 0x0000000000000000u,    0 },
    { 0x8000000000000000u, 0x0000000000000000u,   -1 },
    { 0xAAAAAAAAAAAAAAAAu, 0xAAAAAAAAAAAAAAABu,   -3 },
    { 0xAAAAAAAAAAAAAAAAu, 0xAAAAAAAAAAAAAAABu,   -5 },
    { 0x8888888888888888u, 0x8888888888888889u,   -7 },
    { 0xB60B60B60B60B60Bu, 0x60B60B60B60B60B6u,  -10 },
    { 0xD00D00D00D00D00Du, 0x00D00D00D00D00D0u,  -13 },
    { 0xD00D00D00D00D00Du, 0x00D00D00D00D00D0u,  -16 },
    { 0xB8EF1D2AB6399C7Du, 0x560E4472800B8EF2u,  -19 },
    { 0x93F27DBBC4FAE397u, 0x780B69F5333C725Bu,  -22 },
    { 0xD7322B3FAA271C7Fu, 0x3A3F25C1BEE38F10u,  -26 },
    { 0x8F76C77FC6C4BDAAu, 0x26D4C3D67F425F60u,  -29 },
    { 0xB092309D43684BE5u, 0x1C198E91D7B4269Eu,  -33 },
    { 0xC9CBA54603E4E905u, 0xD6F8A2EFD1F27546u,  -37 },
    { 0xD73F9F399DC0F88Eu, 0xC32B58774657F48Fu,  -41 },
    { 0xD73F9F399DC0F88Eu, 0xC32B58774657F48Fu,  -45 },
    { 0xCA963B81856A5359u, 0x3028CBBB8D7FF53Cu,  -49 },
    { 0xB413C31DCBECBBDDu, 0x8024435161554BC3u,  -53 },
    { 0x97A4DA340A0AB926u, 0x50F61DBDCB3A5ABFu,  -57 },
    { 0xF2A15D201011283Du, 0x4E5695FC785D5DFFu,  -62 },
    { 0xB8DC77B6E7AB8C5Fu, 0x78A37E77372290C2u,  -66 },
    { 0x8671CB6DBFC294A2u, 0x86485BF99C763ABCu,  -70 },
    { 0xBB0DA098B1C0CECBu, 0xDC3826EBFB13CC27u,  -75 },
    { 0xF96780CB97ABBE65u, 0x25A033E54EC51034u,  -80 },
    { 0x9F9E66E8B2FD46A7u, 0x22520CBBB7885C4Au,  -84 },
    { 0xC4742FE35272CD1Cu, 0x790285D3580A4A34u,  -89 },
    { 0xE8D58E16E6751905u, 0x4D0C78AEA13B9A50u,  -94 },
    { 0x850C5131A842E9B9u, 0xE2E28E1AA546A152u,  -98 },
    { 0x92CFCC5A1AC56BD5u, 0xF1873BB378948EB3u, -103 },
    { 0x9C9962823EB07306u, 0x56F6A614C4E2BA59u, -108 },
    { 0xA1A6973C1FADE217u, 0x0F7237D35FE1C89Eu, -113 },
    { 0xA1A6973C1FADE217u, 0x0F7237D35FE1C89Eu, -118 },
    { 0x9CC092A6E86A8DA9u, 0xC166FFD4BA113EA8u, -123 },
};

/* 1/(2k+1), k = 0..21. */
static const XK ODD_INV[22] = {
    { 0x8000000000000000u, 0x0000000000000000u,  0 },
    { 0xAAAAAAAAAAAAAAAAu, 0xAAAAAAAAAAAAAAABu, -2 },
    { 0xCCCCCCCCCCCCCCCCu, 0xCCCCCCCCCCCCCCCDu, -3 },
    { 0x9249249249249249u, 0x2492492492492492u, -3 },
    { 0xE38E38E38E38E38Eu, 0x38E38E38E38E38E4u, -4 },
    { 0xBA2E8BA2E8BA2E8Bu, 0xA2E8BA2E8BA2E8BAu, -4 },
    { 0x9D89D89D89D89D89u, 0xD89D89D89D89D89Eu, -4 },
    { 0x8888888888888888u, 0x8888888888888889u, -4 },
    { 0xF0F0F0F0F0F0F0F0u, 0xF0F0F0F0F0F0F0F1u, -5 },
    { 0xD79435E50D79435Eu, 0x50D79435E50D7943u, -5 },
    { 0xC30C30C30C30C30Cu, 0x30C30C30C30C30C3u, -5 },
    { 0xB21642C8590B2164u, 0x2C8590B21642C859u, -5 },
    { 0xA3D70A3D70A3D70Au, 0x3D70A3D70A3D70A4u, -5 },
    { 0x97B425ED097B425Eu, 0xD097B425ED097B42u, -5 },
    { 0x8D3DCB08D3DCB08Du, 0x3DCB08D3DCB08D3Eu, -5 },
    { 0x8421084210842108u, 0x4210842108421084u, -5 },
    { 0xF83E0F83E0F83E0Fu, 0x83E0F83E0F83E0F8u, -6 },
    { 0xEA0EA0EA0EA0EA0Eu, 0xA0EA0EA0EA0EA0EAu, -6 },
    { 0xDD67C8A60DD67C8Au, 0x60DD67C8A60DD67Du, -6 },
    { 0xD20D20D20D20D20Du, 0x20D20D20D20D20D2u, -6 },
    { 0xC7CE0C7CE0C7CE0Cu, 0x7CE0C7CE0C7CE0C8u, -6 },
    { 0xBE82FA0BE82FA0BEu, 0x82FA0BE82FA0BE83u, -6 },
};

/* ---------------------------------------------------------------- kernels */

/* sum over k < n of c[base + stride*k] * v^k, by Horner. */
static XW horner(const XK *c, int base, int stride, int n, const XW *v)
{
    XW acc = xk(&c[base + stride * (n - 1)], 0);
    int k;

    for (k = n - 2; k >= 0; k--) {
        acc = xw_mul(&acc, v);
        acc = xw_add(acc, xk(&c[base + stride * k], 0));
    }
    return acc;
}

/* e^u - 1 for |u| < 0.7: u * sum u^k/(k+1)!, 31 terms. */
static XW expm1_k(const XW *u)
{
    XW p = horner(FACT_INV, 1, 1, 31, u);
    return xw_mul(u, &p);
}

/* atanh-style log: ln((1+s)/(1-s)) = 2 s sum s^2k/(2k+1), for |s| < 0.07. */
static XW ln_s(const XW *s)
{
    XW w = xw_mul(s, s), p = horner(ODD_INV, 0, 1, 20, &w), r = xw_mul(s, &p);
    r.e++;
    return r;
}

/* atan t = t sum (-t^2)^k/(2k+1), for |t| <= 1/16 or so. */
static XW atan_k(const XW *t)
{
    XW w = xw_mul(t, t), p;
    w.neg ^= 1;
    p = horner(ODD_INV, 0, 1, 22, &w);
    return xw_mul(t, &p);
}

/* sin r and cos r for |r| <= pi/4. */
static XW sin_k(const XW *r)
{
    XW w = xw_mul(r, r), p;
    w.neg ^= 1;
    p = horner(FACT_INV, 1, 2, 16, &w);
    return xw_mul(r, &p);
}

static XW cos_k(const XW *r)
{
    XW w = xw_mul(r, r);
    w.neg ^= 1;
    return horner(FACT_INV, 0, 2, 17, &w);
}

/* log2 x for a finite x > 0 given as a 128-bit float, to about 2^-120:
   x = m 2^k with m brought into [sqrt(1/2), sqrt(2)), then m r - 1 = u for
   r one of 1, 81/64 and 51/64, whichever puts |u| below 0.128, so that
   ln m = ln(1+u) - ln r with s = u/(2+u) small.  For a 64-bit m the
   product m r is exact; for more it loses only what is below 2^-128. */
static XW log2_k(const XW *x)
{
    XW mm = xw(x->hi, x->lo, 0, 0), um, lnm;
    int32_t k = x->e;
    int which;                           /* 0: r = 1, 1: 81/64, 2: 51/64 */

    if (x->hi >= 0xB504F333F9DE6484u) {  /* m >= sqrt 2: m/2 instead */
        mm.e = -1;
        k++;
    }
    /* The thresholds 2^(-1/6) and 2^(1/6), on m in [sqrt(1/2), sqrt(2)). */
    if (mm.e < 0 && x->hi < 0xE411F03A76094A35u)       which = 1;
    else if (mm.e == 0 && x->hi >= 0x8FACD61E3EB5FEB5u) which = 2;
    else                                                which = 0;
    if (which == 1) {
        XW r = xw(0xA200000000000000u, 0, 0, 0);          /* 81/64 */
        um = xw_mul(&mm, &r);
    } else if (which == 2) {
        XW r = xw(0xCC00000000000000u, 0, -1, 0);         /* 51/64 */
        um = xw_mul(&mm, &r);
    } else {
        um = mm;
    }
    um = xw_sub(um, xw(1ull << 63, 0, 0, 0));
    if (xw_iszero(&um)) {
        lnm = xw(0, 0, 0, 0);
    } else {
        XW den = xw_add(xw(1ull << 63, 0, 1, 0), um), sv = xw_div(&um, &den);
        lnm = ln_s(&sv);
    }
    if (which == 1)      lnm = xw_sub(lnm, xk(&K_LN8164, 0));
    else if (which == 2) lnm = xw_add(lnm, xk(&K_LN5164, 0));
    {
        XW l2 = xk(&K_LOG2E, 0), kk = xw_from_int(k);
        lnm = xw_mul(&lnm, &l2);
        return xw_add(kk, lnm);
    }
}

/* ======================================================================
   Measured on the i9-12900 (Alder Lake) this was written on.  These are
   how that x87 differs from correct rounding, found by the fuzzer; an AMD
   part or a later Intel one may differ, and x80test checks this model,
   not the host it runs on.
   ====================================================================== */

/* F2XM1 uses ln 2 cut to 67 bits.  Below 2^-67 the result is x times that,
   rounded once.  Below 1/4 x ln 2 is cut to 67 bits too before the
   exponential is taken of it, which takes agreement there from 90% to
   99.8%; from 1/4 up the correctly rounded result agrees best, at 97%. */
#define TX_F2XM1_TINY (-68)
#define TX_F2XM1_CUT  (-3)

/* FYL2X is y times log2 x cut to 67 bits toward zero, rounded once. */
#define TX_LOG_BITS 67

/* FPATAN with |x| more than 2^41 above |y| is y/x cut to 67 bits. */
#define TX_ATAN_GAP 41

/* FSIN, FCOS and FPTAN reduce by a 66-bit pi, exactly, and so agree with
   the x87 just where a correct reduction would not: near multiples of pi. */
static const uint64_t TX_PINT_HI = 0x3u, TX_PINT_LO = 0x243F6A8885A308D3u;
static const uint64_t TX_RINV = 0xA2F9836E4E44152Au;     /* 2^129 / Pint */

/* Where the x87 reports a result as inexact whether or not the last
   rounding lost anything - it computed the thing to more bits than it
   shows - it reports a tiny one as underflowing too. */
static void force_pe(X80Env *env, const X80 *r)
{
    env->sw |= PE;
    if (!(r->se & 0x7FFF) && r->m) env->sw |= UE;
}

/* The exact product of a 64-bit y and an XW cut to `bits` bits toward
   zero, rounded once into a register, inexact whatever the rounding says. */
static void mul_cut(X80Env *env, int neg, const Un *y, const XW *l, int bits,
                    X80 *r)
{
    uint64_t lh = l->hi, ll = l->lo, ph, pl, qh, ql, w2, w1, w0;
    int32_t e;

    /* Keep the top `bits` of the 128-bit significand. */
    if (bits <= 64) { lh &= ~0ull << (64 - bits); ll = 0; }
    else            ll &= ~0ull << (128 - bits);
    /* y.m * (lh:ll) as w2:w1:w0. */
    mul64(y->m, lh, &ph, &pl);
    mul64(y->m, ll, &qh, &ql);
    w0 = ql;
    w1 = pl + qh;
    w2 = ph + (w1 < pl);
    /* y 2^(ey - 63) times l 2^(el - 127): the product's top bit is at
       2^(ey + el + 1) if w2's top bit is set. */
    e = y->e + l->e;
    if (w2 >> 63) e++;
    else { w2 = w2 << 1 | w1 >> 63; w1 = w1 << 1 | w0 >> 63; w0 <<= 1; }
    round_reg(env, &FMT_R64, neg, e, w2, w1, w0 != 0, r);
    force_pe(env, r);
}

/* y times log2 of an exact power of two, 2^k.  For k > 0 the logarithm is
   k and the product is rounded, inexact regardless; for k < 0 the x87
   behaves as if it were a 67-bit unit short of k in magnitude, so that the
   result is just below |y k|. */
static void mul_pow2(X80Env *env, const Un *y, int32_t k, X80 *r)
{
    XW kk = xw_from_int(k);

    if (k < 0) {
        XW unit = xw(1ull << 63, 0, kk.e - (TX_LOG_BITS - 1), 0);
        kk = xw_add(kk, unit);                          /* toward zero */
    }
    mul_cut(env, y->neg ^ kk.neg, y, &kk, 128, r);
}

/* ------------------------------------------------------------------ F2XM1 */

void x80_f2xm1(X80Env *env, const X80 *a, X80 *r)
{
    Un u;
    XW x, t;

    env->cc = C1;
    unpack(a, &u);
    switch (u.k) {
    case K_BAD:  env->sw |= IE; indefinite(r); return;
    case K_QNAN: case K_SNAN: nan1(env, &u, r); return;
    case K_ZERO: *r = *a; return;
    case K_INF:
        if (u.neg) set(r, 1, 0x3FFF, 1ull << 63);           /* -1 */
        else       *r = *a;
        return;
    }
    if (u.den) env->sw |= DE;
    if (u.e >= 0) {
        /* 1 and -1 give 1 and -1/2, exactly but with PE; anything larger
           is outside the instruction's domain and comes back unchanged. */
        env->sw |= PE;
        if (u.e == 0 && u.m == 1ull << 63)
            set(r, u.neg, (uint16_t)(u.neg ? 0x3FFE : 0x3FFF), 1ull << 63);
        else
            *r = *a;
        return;
    }
    if (u.e < TX_F2XM1_TINY) {
        XW c67 = xk(&K_LN2, 0);
        mul_cut(env, u.neg, &u, &c67, 67, r);
        return;
    }
    x = xw_from_un(&u);
    {
        XW ln2 = xk(&K_LN2, 0);
        if (u.e <= TX_F2XM1_CUT) {
            ln2.lo &= ~0ull << (128 - 67);
            t = xw_mul(&x, &ln2);
            t.lo &= ~0ull << (128 - 67);
        } else {
            t = xw_mul(&x, &ln2);
        }
    }
    t = expm1_k(&t);
    xw_round(env, &t, r);
}

/* ------------------------------------------------------------------ FYL2X */

void x80_yl2x(X80Env *env, const X80 *a, const X80 *b, X80 *r)
{
    Un x, y;
    int lt1;
    XW l;

    env->cc = C1;
    unpack(a, &x);
    unpack(b, &y);
    if (x.k == K_BAD || y.k == K_BAD) { env->sw |= IE; indefinite(r); return; }
    if (is_nan(&x) || is_nan(&y)) { nan2(env, &x, &y, r); return; }
    if (x.neg && x.k != K_ZERO) { env->sw |= IE; indefinite(r); return; }
    if (x.k == K_ZERO) {
        /* log2 0: divide-by-zero, which outranks a denormal y. */
        if (y.k == K_ZERO) { env->sw |= IE; indefinite(r); return; }
        if (y.k != K_INF) env->sw |= ZE;
        inf(r, !y.neg);
        return;
    }
    if (x.k == K_INF) {
        if (y.k == K_ZERO) { env->sw |= IE; indefinite(r); return; }
        if (y.den) env->sw |= DE;
        inf(r, y.neg);
        return;
    }
    /* x finite and positive. */
    if (x.e == 0 && x.m == 1ull << 63) {                     /* x = 1 */
        if (y.k == K_INF) { env->sw |= IE; indefinite(r); return; }
        if (x.den || y.den) env->sw |= DE;
        zero(r, y.neg);
        return;
    }
    if (x.den || y.den) env->sw |= DE;
    lt1 = x.e < 0;
    if (y.k == K_INF)  { inf(r, y.neg ^ lt1); return; }
    if (y.k == K_ZERO) { zero(r, y.neg ^ lt1); return; }

    if (x.m == 1ull << 63) {                    /* a power of two */
        mul_pow2(env, &y, x.e, r);
        return;
    }
    {
        XW xx = xw_from_un(&x);
        l = log2_k(&xx);
    }
    mul_cut(env, y.neg ^ l.neg, &y, &l, TX_LOG_BITS, r);
}

/* ---------------------------------------------------------------- FYL2XP1 */

/* y log2(1 + x), correctly rounded.  The game never runs it. */
void x80_yl2xp1(X80Env *env, const X80 *a, const X80 *b, X80 *r)
{
    Un x, y;
    XW l, xx;

    env->cc = C1;
    unpack(a, &x);
    unpack(b, &y);
    if (x.k == K_BAD || y.k == K_BAD) { env->sw |= IE; indefinite(r); return; }
    if (is_nan(&x) || is_nan(&y)) { nan2(env, &x, &y, r); return; }
    if (x.k == K_ZERO) {
        if (y.k == K_INF) { env->sw |= IE; indefinite(r); return; }
        if (y.den) env->sw |= DE;
        zero(r, x.neg ^ y.neg);
        return;
    }
    if (x.k == K_INF) {
        if (x.neg || y.k == K_ZERO) { env->sw |= IE; indefinite(r); return; }
        if (y.den) env->sw |= DE;
        inf(r, y.neg);
        return;
    }
    if (x.den || y.den) env->sw |= DE;
    if (y.k == K_INF)  { inf(r, y.neg ^ x.neg); return; }
    if (y.k == K_ZERO) { zero(r, y.neg ^ x.neg); return; }
    /* The manual defines this for |x| < 1 - sqrt(2)/2 and leaves the rest;
       this x87 gives x back, inexact, for x <= -1 (once y's infinities and
       zeros have had their say), and a logarithm for anything above. */
    if (x.neg && x.e >= 0) {
        *r = *a;
        env->sw |= PE;
        return;
    }
    xx = xw_from_un(&x);
    if (x.e < -2) {
        /* |x| < 1/4: ln(1+x) = ln((1+s)/(1-s)) with s = x/(2+x), which
           keeps its relative accuracy however small x is. */
        XW two = xw(1ull << 63, 0, 1, 0), den = xw_add(two, xx), sv;
        XW l2 = xk(&K_LOG2E, 0);
        sv = xw_div(&xx, &den);
        l = ln_s(&sv);
        l = xw_mul(&l, &l2);
    } else {
        /* Otherwise 1 + x is formed and its logarithm taken - by FYL2X's
           rule when it is a power of two, as the x87 does. */
        XW onex = xw_add(xw(1ull << 63, 0, 0, 0), xx);
        if (onex.hi == 1ull << 63 && !onex.lo) {
            mul_pow2(env, &y, onex.e, r);
            return;
        }
        l = log2_k(&onex);
    }
    {
        XW yy = xw_from_un(&y);
        l = xw_mul(&l, &yy);
    }
    xw_round(env, &l, r);
}

/* ----------------------------------------------------------------- FPATAN */

/* A constant of pi, rounded under the control word, with PE. */
static void pi_const(X80Env *env, const XK *k, int neg, X80 *r)
{
    XW c = xk(k, neg);
    xw_round(env, &c, r);
}

/* atan(y/x) in the right quadrant: a is ST(0), x; b is ST(1), y. */
void x80_patan(X80Env *env, const X80 *a, const X80 *b, X80 *r)
{
    Un x, y;
    XW ax, ay, t, th;
    int swap = 0;

    env->cc = C1;
    unpack(a, &x);
    unpack(b, &y);
    if (x.k == K_BAD || y.k == K_BAD) { env->sw |= IE; indefinite(r); return; }
    if (is_nan(&x) || is_nan(&y)) { nan2(env, &x, &y, r); return; }
    if (x.den || y.den) env->sw |= DE;

    if (y.k == K_ZERO) {
        /* +-0 against positive x (or +0) is +-0; against negative x (or
           -0), +-pi. */
        if (!x.neg) zero(r, y.neg);
        else        pi_const(env, &K_PI, y.neg, r);
        return;
    }
    if (y.k == K_INF) {
        if (x.k == K_INF) pi_const(env, x.neg ? &K_3PI_4 : &K_PI_4, y.neg, r);
        else              pi_const(env, &K_PI_2, y.neg, r);
        return;
    }
    if (x.k == K_ZERO) { pi_const(env, &K_PI_2, y.neg, r); return; }
    if (x.k == K_INF) {
        if (!x.neg) zero(r, y.neg);
        else        pi_const(env, &K_PI, y.neg, r);
        return;
    }

    /* Both finite and nonzero.  The tiny-ratio rule first. */
    if (!x.neg && x.e - y.e >= TX_ATAN_GAP) {
        XW q = xw_div(&(XW){ y.m, 0, y.e, 0 }, &(XW){ x.m, 0, x.e, 0 });
        q.neg = y.neg;
        /* cut to 67 bits toward zero, then round once */
        q.lo &= ~0ull << (128 - 67);
        round_reg(env, &FMT_R64, q.neg, q.e, q.hi, q.lo, 0, r);
        force_pe(env, r);
        return;
    }

    ax = xw(x.m, 0, x.e, 0);
    ay = xw(y.m, 0, y.e, 0);
    if (ay.e > ax.e || (ay.e == ax.e && ay.hi > ax.hi)) {
        XW tmp = ax; ax = ay; ay = tmp;
        swap = 1;
    }
    /* t = ay/ax <= 1; reduce by the nearest j/8. */
    {
        int32_t d = ax.e - ay.e;
        unsigned j = 0;
        if (d <= 4) {
            /* 8t, from the top bits, rounded to the nearest integer. */
            uint64_t num = ay.hi >> (40 + d), den = ax.hi >> 40;
            j = (unsigned)((num * 16 / den + 1) / 2);
            if (j > 8) j = 8;
        }
        if (j) {
            XW jj = xw_from_int((int64_t)j), eight = xw_from_int(8);
            XW n1 = xw_mul(&ay, &eight), n2 = xw_mul(&ax, &jj);
            XW d1 = xw_mul(&ax, &eight), d2 = xw_mul(&ay, &jj);
            XW num = xw_sub(n1, n2), den = xw_add(d1, d2), tt;
            tt = xw_div(&num, &den);
            th = atan_k(&tt);
            th = xw_add(xk(&ATAN8[j], 0), th);
        } else {
            t = xw_div(&ay, &ax);
            th = atan_k(&t);
        }
    }
    if (swap) th = xw_sub(xk(&K_PI_2, 0), th);
    if (x.neg) th = xw_sub(xk(&K_PI, 0), th);
    th.neg ^= y.neg;
    xw_round(env, &th, r);
}

/* ------------------------------------------------------ FSIN FCOS FPTAN */

/* Reduce |x| by the 66-bit pi to r in [-pi/4, pi/4] and a quadrant. */
static XW reduce(const Un *x, unsigned *quad)
{
    uint64_t mx = x->m, nh, nl, qh, ql, q, rh, rl, ph, pl;
    int32_t ex = x->e;
    int64_t rneg;
    XW r;

    if (ex < -1 || (ex == -1 && mx <= 0xC90FDAA22168C234u)) {
        *quad = 0;
        return xw(mx, 0, ex, 0);
    }
    /* N = |x| in units of 2^-65, below 2^128. */
    nh = ex + 2 >= 64 ? mx << (ex + 2 - 64) : (ex + 2 ? mx >> (64 - (ex + 2)) : 0);
    nl = ex + 2 >= 64 ? 0 : mx << (ex + 2);
    if (ex + 2 == 0) { nh = 0; nl = mx; }
    /* q from the reciprocal, then R = N - q Pint and a correction. */
    mul64(mx, TX_RINV, &qh, &ql);
    q = ex == -1 ? 0 : qh >> (63 - ex);
    /* q * Pint as 128 bits: Pint = 3:243F6A8885A308D3 */
    mul64(q, TX_PINT_LO, &ph, &pl);
    ph += q * TX_PINT_HI;
    rl = nl - pl;
    rh = nh - ph - (nl < pl);
    /* R as a signed 128-bit number; bring 2R into [-Pint, Pint). */
    for (;;) {
        /* 2R >= Pint ? */
        int neg = (int64_t)rh < 0;
        uint64_t th = rh << 1 | rl >> 63, tl = rl << 1;
        if (!neg && (th > TX_PINT_HI || (th == TX_PINT_HI && tl >= TX_PINT_LO))) {
            uint64_t t = rl - TX_PINT_LO;
            rh = rh - TX_PINT_HI - (rl < TX_PINT_LO);
            rl = t;
            q++;
            continue;
        }
        if (neg) {
            /* 2R < -Pint, that is -2R > Pint */
            uint64_t mh = ~rh, ml = ~rl + 1;
            if (!ml) mh++;
            th = mh << 1 | ml >> 63;
            tl = ml << 1;
            if (th > TX_PINT_HI || (th == TX_PINT_HI && tl > TX_PINT_LO)) {
                uint64_t t = rl + TX_PINT_LO;
                rh = rh + TX_PINT_HI + (t < rl);
                rl = t;
                q--;
                continue;
            }
        }
        break;
    }
    *quad = (unsigned)(q & 3);
    rneg = (int64_t)rh < 0;
    if (rneg) {
        rh = ~rh;
        rl = ~rl + 1;
        if (!rl) rh++;
    }
    r = xw(rh, rl, 127 - 65, (int)rneg);
    xw_norm(&r);
    return r;
}

/* The work common to the three: specials, the edges, the reduction.  0
   when *r is the answer already; otherwise S and C are sin and cos of the
   reduced argument, with the quadrant. */
enum { T_SIN, T_COS, T_TAN };

static int trig(X80Env *env, int which, const X80 *a, X80 *r, Un *u,
                XW *s, XW *c, unsigned *quad)
{
    int be = a->se & 0x7FFF;
    XW rr;

    env->cc = C1 | C2;
    unpack(a, u);
    switch (u->k) {
    case K_BAD:  env->sw |= IE; indefinite(r); return 0;
    case K_QNAN: case K_SNAN: nan1(env, u, r); return 0;
    case K_INF:  env->sw |= IE; indefinite(r); return 0;
    case K_ZERO:
        if (which == T_COS) set(r, 0, 0x3FFF, 1ull << 63);
        else                *r = *a;
        return 0;
    }
    if (be >= 0x403E) {                      /* |x| >= 2^63: out of range */
        env->sw |= C2;
        *r = *a;
        return 0;
    }
    if (u->den) env->sw |= DE;
    if (be <= 0x3FBA) {
        /* Tiny: sin and tan are x and cos is 1, with PE and C1 clear - the
           value of x, that is, re-encoded, so a pseudo-denormal comes back
           the least normal, and underflowing if it is a true denormal. */
        if (which == T_COS) {
            set(r, 0, 0x3FFF, 1ull << 63);
            env->sw |= PE;
        } else {
            round_reg(env, &FMT_R64, u->neg, u->e, u->m, 0, 0, r);
            force_pe(env, r);
        }
        return 0;
    }
    rr = reduce(u, quad);
    *s = sin_k(&rr);
    *c = cos_k(&rr);
    return 1;
}

void x80_sin(X80Env *env, const X80 *a, X80 *r)
{
    Un u;
    XW s, c, v;
    unsigned q;

    if (!trig(env, T_SIN, a, r, &u, &s, &c, &q)) return;
    switch (q) {
    case 0:  v = s; break;
    case 1:  v = c; break;
    case 2:  v = xw_neg(s); break;
    default: v = xw_neg(c); break;
    }
    v.neg ^= u.neg;
    xw_round(env, &v, r);
}

void x80_cos(X80Env *env, const X80 *a, X80 *r)
{
    Un u;
    XW s, c, v;
    unsigned q;

    if (!trig(env, T_COS, a, r, &u, &s, &c, &q)) return;
    switch (q) {
    case 0:  v = c; break;
    case 1:  v = xw_neg(s); break;
    case 2:  v = xw_neg(c); break;
    default: v = s; break;
    }
    xw_round(env, &v, r);
}

/* tan, and 1.0 pushed after it - except out of range, where the stack is
   left as it was.  The return value says whether to push. */
int x80_ptan(X80Env *env, const X80 *a, X80 *r, X80 *one)
{
    Un u;
    XW s, c, v;
    unsigned q;

    if (!trig(env, T_TAN, a, r, &u, &s, &c, &q)) {
        if (env->sw & C2) return 0;                /* no push, *one untouched */
        /* An infinity, a NaN or an unsupported encoding: the result is
           pushed in place of the 1.0 too. */
        if (u.k == K_INF || u.k == K_BAD || is_nan(&u)) *one = *r;
        else set(one, 0, 0x3FFF, 1ull << 63);
        return 1;
    }
    set(one, 0, 0x3FFF, 1ull << 63);
    v = q & 1 ? xw_div(&c, &s) : xw_div(&s, &c);
    if (q & 1) v.neg ^= 1;
    v.neg ^= u.neg;
    xw_round(env, &v, r);
    return 1;
}
