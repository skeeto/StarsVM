/* x80dual.c - both x87 backends at once, compared.  See x80dual.h.
 *
 * An FPU=dual build runs every x87 operation on x87hw.c and on x80.c.  The
 * exact operations must agree in every bit of the result and the status
 * word; the first twenty that do not are logged with the guest's CS:IP (just
 * past the instruction) and the operands, and all of them are counted, by
 * operation, in the summary at exit.  --x87-follow says whose result the
 * guest continues with: x87hw's by default, so that a difference does not
 * send the run somewhere the goldens have never been; x80.c's with "soft",
 * which is what an FPU=soft build will do and so what its goldens will be.
 *
 * x80.c has no transcendentals yet, so those are x87hw's alone, and counted.
 */
#include "x80dual.h"
#include "x87hw.h"
#include "log.h"

#include <string.h>

enum {
    D_ARITH, D_ARITH32, D_ARITH64, D_CMP, D_UCMP, D_CMP32, D_CMP64, D_SQRT,
    D_RNDINT, D_ABS, D_CHS, D_TST, D_XAM, D_PREM, D_SCALE, D_XTRACT, D_CONST,
    D_FILD, D_FLD32, D_FLD64, D_FST32, D_FST64, D_FIST, D_N
};

static const char *const dname[D_N] = {
    "arith", "arith m32", "arith m64", "fcom", "fucom", "fcom m32",
    "fcom m64", "fsqrt", "frndint", "fabs", "fchs", "ftst", "fxam", "fprem",
    "fscale", "fxtract", "constant", "fild", "fld m32", "fld m64", "fst m32",
    "fst m64", "fist",
};

static unsigned long long nops[D_N], ndiff[D_N], ntrans, nlogged;
static int follow_soft;
static uint16_t at_cs;
static uint32_t at_ip;

void x80dual_note(uint16_t cs, uint32_t ip)
{
    at_cs = cs;
    at_ip = ip;
}

int x80dual_follow(const char *which)
{
    if (!strcmp(which, "hw"))   { follow_soft = 0; return 1; }
    if (!strcmp(which, "soft")) { follow_soft = 1; return 1; }
    return 0;
}

static void dual_show(const char *who, const X80Env *e, const X80 *r, int nr,
                 uint64_t v)
{
    int i;
    log_msg("      %s:", who);
    for (i = 0; i < nr; i++)
        log_msg(" %04X:%016llX", r[i].se, (unsigned long long)r[i].m);
    if (!nr) log_msg(" %016llX", (unsigned long long)v);
    log_msg("  sw %04X cc %04X\n", e->sw, e->cc);
}

/* What the two backends made of one operation: counted, and logged while
   there have been few enough differences to read. */
static void dual_check(int d, uint16_t cw, const X80 *a, const X80 *b, uint64_t m,
                  const X80Env *h, const X80Env *s, const X80 *rh,
                  const X80 *rs, int nr, uint64_t vh, uint64_t vs)
{
    int i, same = h->cc == s->cc && h->sw == s->sw && vh == vs;

    nops[d]++;
    for (i = 0; i < nr; i++)
        same &= rh[i].m == rs[i].m && rh[i].se == rs[i].se;
    if (same) return;
    ndiff[d]++;
    if (nlogged++ >= 20) return;
    log_msg("*** x87 backends differ: %s at %04X:%04X, cw %04X", dname[d],
            at_cs, (unsigned)at_ip, cw);
    if (a) log_msg(", a %04X:%016llX", a->se, (unsigned long long)a->m);
    if (b) log_msg(", b %04X:%016llX", b->se, (unsigned long long)b->m);
    if (!b && m) log_msg(", m %016llX", (unsigned long long)m);
    log_msg("\n");
    dual_show("x87hw", h, rh, nr, vh);
    dual_show("x80  ", s, rs, nr, vs);
}

unsigned long long x80dual_report(void)
{
    unsigned long long n = 0, bad = 0;
    int d;

    for (d = 0; d < D_N; d++) {
        n += nops[d];
        bad += ndiff[d];
    }
    log_msg("x87: dual backends, following %s: %llu exact operations, "
            "%llu differed; %llu transcendental, x87hw.c's only\n",
            follow_soft ? "x80.c" : "x87hw.c", n, bad, ntrans);
    for (d = 0; d < D_N; d++)
        if (ndiff[d])
            log_msg("    %-10s %llu of %llu differed\n", dname[d], ndiff[d], nops[d]);
    return bad;
}

/* Run an operation on both, check, and hand on the followed side's. */
#define DUAL_BOTH(d, a, b, m, nr, call_h, call_s)                                 \
    X80Env h = *e, s = *e;                                                   \
    call_h;                                                                  \
    call_s;                                                                  \
    dual_check(d, e->cw, a, b, m, &h, &s, rh, rs, nr, vh, vs);                   \
    *e = follow_soft ? s : h

void dual_arith(X80Env *e, int op, const X80 *a, const X80 *b, X80 *r)
{
    X80 rh[1], rs[1];
    uint64_t vh = 0, vs = 0;
    DUAL_BOTH(D_ARITH, a, b, 0, 1, x87hw_arith(&h, op, a, b, rh), x80_arith(&s, op, a, b, rs));
    *r = follow_soft ? rs[0] : rh[0];
}

void dual_arith_f32(X80Env *e, int op, const X80 *a, uint32_t m, X80 *r)
{
    X80 rh[1], rs[1];
    uint64_t vh = 0, vs = 0;
    DUAL_BOTH(D_ARITH32, a, NULL, m, 1, x87hw_arith_f32(&h, op, a, m, rh),
         x80_arith_f32(&s, op, a, m, rs));
    *r = follow_soft ? rs[0] : rh[0];
}

void dual_arith_f64(X80Env *e, int op, const X80 *a, uint64_t m, X80 *r)
{
    X80 rh[1], rs[1];
    uint64_t vh = 0, vs = 0;
    DUAL_BOTH(D_ARITH64, a, NULL, m, 1, x87hw_arith_f64(&h, op, a, m, rh),
         x80_arith_f64(&s, op, a, m, rs));
    *r = follow_soft ? rs[0] : rh[0];
}

void dual_compare(X80Env *e, const X80 *a, const X80 *b)
{
    X80 *rh = NULL, *rs = NULL;
    uint64_t vh = 0, vs = 0;
    DUAL_BOTH(D_CMP, a, b, 0, 0, x87hw_compare(&h, a, b), x80_compare(&s, a, b));
}

void dual_ucompare(X80Env *e, const X80 *a, const X80 *b)
{
    X80 *rh = NULL, *rs = NULL;
    uint64_t vh = 0, vs = 0;
    DUAL_BOTH(D_UCMP, a, b, 0, 0, x87hw_ucompare(&h, a, b), x80_ucompare(&s, a, b));
}

void dual_compare_f32(X80Env *e, const X80 *a, uint32_t m)
{
    X80 *rh = NULL, *rs = NULL;
    uint64_t vh = 0, vs = 0;
    DUAL_BOTH(D_CMP32, a, NULL, m, 0, x87hw_compare_f32(&h, a, m), x80_compare_f32(&s, a, m));
}

void dual_compare_f64(X80Env *e, const X80 *a, uint64_t m)
{
    X80 *rh = NULL, *rs = NULL;
    uint64_t vh = 0, vs = 0;
    DUAL_BOTH(D_CMP64, a, NULL, m, 0, x87hw_compare_f64(&h, a, m), x80_compare_f64(&s, a, m));
}

/* One operand in, one result out. */
#define DUAL_UNARY(fn, d)                                                         \
    void dual_##fn(X80Env *e, const X80 *a, X80 *r)                          \
    {                                                                        \
        X80 rh[1], rs[1];                                                    \
        uint64_t vh = 0, vs = 0;                                             \
        DUAL_BOTH(d, a, NULL, 0, 1, x87hw_##fn(&h, a, rh), x80_##fn(&s, a, rs));  \
        *r = follow_soft ? rs[0] : rh[0];                                    \
    }

DUAL_UNARY(sqrt, D_SQRT)
DUAL_UNARY(rndint, D_RNDINT)
DUAL_UNARY(abs, D_ABS)
DUAL_UNARY(chs, D_CHS)

void dual_tst(X80Env *e, const X80 *a)
{
    X80 *rh = NULL, *rs = NULL;
    uint64_t vh = 0, vs = 0;
    DUAL_BOTH(D_TST, a, NULL, 0, 0, x87hw_tst(&h, a), x80_tst(&s, a));
}

void dual_xam(X80Env *e, const X80 *a)
{
    X80 *rh = NULL, *rs = NULL;
    uint64_t vh = 0, vs = 0;
    DUAL_BOTH(D_XAM, a, NULL, 0, 0, x87hw_xam(&h, a), x80_xam(&s, a));
}

void dual_prem(X80Env *e, const X80 *a, const X80 *b, X80 *r)
{
    X80 rh[1], rs[1];
    uint64_t vh = 0, vs = 0;
    DUAL_BOTH(D_PREM, a, b, 0, 1, x87hw_prem(&h, a, b, rh), x80_prem(&s, a, b, rs));
    *r = follow_soft ? rs[0] : rh[0];
}

void dual_scale(X80Env *e, const X80 *a, const X80 *b, X80 *r)
{
    X80 rh[1], rs[1];
    uint64_t vh = 0, vs = 0;
    DUAL_BOTH(D_SCALE, a, b, 0, 1, x87hw_scale(&h, a, b, rh), x80_scale(&s, a, b, rs));
    *r = follow_soft ? rs[0] : rh[0];
}

void dual_xtract(X80Env *e, const X80 *a, X80 *exp, X80 *sig)
{
    X80 rh[2], rs[2];
    uint64_t vh = 0, vs = 0;
    DUAL_BOTH(D_XTRACT, a, NULL, 0, 2, x87hw_xtract(&h, a, &rh[0], &rh[1]),
         x80_xtract(&s, a, &rs[0], &rs[1]));
    *exp = follow_soft ? rs[0] : rh[0];
    *sig = follow_soft ? rs[1] : rh[1];
}

void dual_constant(X80Env *e, int which, X80 *r)
{
    X80 rh[1], rs[1];
    uint64_t vh = 0, vs = 0;
    DUAL_BOTH(D_CONST, NULL, NULL, (uint64_t)which, 1, x87hw_constant(&h, which, rh),
         x80_constant(&s, which, rs));
    *r = follow_soft ? rs[0] : rh[0];
}

void dual_from_int(X80Env *e, int64_t v, X80 *r)
{
    X80 rh[1], rs[1];
    uint64_t vh = 0, vs = 0;
    DUAL_BOTH(D_FILD, NULL, NULL, (uint64_t)v, 1, x87hw_from_int(&h, v, rh),
         x80_from_int(&s, v, rs));
    *r = follow_soft ? rs[0] : rh[0];
}

void dual_from_f32(X80Env *e, uint32_t bits, X80 *r)
{
    X80 rh[1], rs[1];
    uint64_t vh = 0, vs = 0;
    DUAL_BOTH(D_FLD32, NULL, NULL, bits, 1, x87hw_from_f32(&h, bits, rh),
         x80_from_f32(&s, bits, rs));
    *r = follow_soft ? rs[0] : rh[0];
}

void dual_from_f64(X80Env *e, uint64_t bits, X80 *r)
{
    X80 rh[1], rs[1];
    uint64_t vh = 0, vs = 0;
    DUAL_BOTH(D_FLD64, NULL, NULL, bits, 1, x87hw_from_f64(&h, bits, rh),
         x80_from_f64(&s, bits, rs));
    *r = follow_soft ? rs[0] : rh[0];
}

uint32_t dual_to_f32(X80Env *e, const X80 *a)
{
    X80 *rh = NULL, *rs = NULL;
    uint64_t vh, vs;
    DUAL_BOTH(D_FST32, a, NULL, 0, 0, vh = x87hw_to_f32(&h, a), vs = x80_to_f32(&s, a));
    return (uint32_t)(follow_soft ? vs : vh);
}

uint64_t dual_to_f64(X80Env *e, const X80 *a)
{
    X80 *rh = NULL, *rs = NULL;
    uint64_t vh, vs;
    DUAL_BOTH(D_FST64, a, NULL, 0, 0, vh = x87hw_to_f64(&h, a), vs = x80_to_f64(&s, a));
    return follow_soft ? vs : vh;
}

int64_t dual_to_int(X80Env *e, const X80 *a, unsigned width)
{
    X80 *rh = NULL, *rs = NULL;
    uint64_t vh, vs;
    DUAL_BOTH(D_FIST, a, NULL, width, 0, vh = (uint64_t)x87hw_to_int(&h, a, width),
         vs = (uint64_t)x80_to_int(&s, a, width));
    return (int64_t)(follow_soft ? vs : vh);
}

/* ------------------------------------------------ x87hw's alone, for now */

void dual_f2xm1(X80Env *e, const X80 *a, X80 *r) { ntrans++; x87hw_f2xm1(e, a, r); }
void dual_sin(X80Env *e, const X80 *a, X80 *r)   { ntrans++; x87hw_sin(e, a, r); }
void dual_cos(X80Env *e, const X80 *a, X80 *r)   { ntrans++; x87hw_cos(e, a, r); }

int dual_ptan(X80Env *e, const X80 *a, X80 *r, X80 *one)
{
    ntrans++;
    return x87hw_ptan(e, a, r, one);
}

void dual_patan(X80Env *e, const X80 *a, const X80 *b, X80 *r)
{
    ntrans++;
    x87hw_patan(e, a, b, r);
}

void dual_yl2x(X80Env *e, const X80 *a, const X80 *b, X80 *r)
{
    ntrans++;
    x87hw_yl2x(e, a, b, r);
}

void dual_yl2xp1(X80Env *e, const X80 *a, const X80 *b, X80 *r)
{
    ntrans++;
    x87hw_yl2xp1(e, a, b, r);
}

void dual_host_enter(void) { x87hw_host_enter(); }
void dual_host_leave(void) { x87hw_host_leave(); }

#undef DUAL_BOTH
#undef DUAL_UNARY
