/* x80ops.h - every x87 operation of x80.h as a row of a table, for tests.
 *
 * The fuzzer's --x80 mode runs each case through x80.c and x87hw.c and
 * compares; x80test runs the same cases through x80.c alone, on any host,
 * and compares a hash of the outputs with one the fuzzer recorded from
 * silicon.  Both draw their cases here, so a stream is the same stream
 * wherever it is drawn, and a block that hashes differently under x80test
 * can be found case by case with the fuzzer.
 *
 * Portable C without floating point, like x80gen.h, which it builds on.
 */
#ifndef X80OPS_H
#define X80OPS_H

#include "x80gen.h"

#include <string.h>

enum {
    XO_ARITH, XO_ARITH32, XO_ARITH64, XO_CMP, XO_UCMP, XO_CMP32, XO_CMP64,
    XO_SQRT, XO_RNDINT, XO_ABS, XO_CHS, XO_TST, XO_XAM, XO_PREM, XO_SCALE,
    XO_XTRACT, XO_CONST, XO_FILD, XO_FLD32, XO_FLD64, XO_FST32, XO_FST64,
    XO_FIST,
    /* The transcendentals, which the x87 does not round correctly and so
       which agree with it approximately: see x80ops_approx. */
    XO_F2XM1, XO_YL2X, XO_YL2XP1, XO_PATAN, XO_SIN, XO_COS, XO_PTAN,
};

static const struct x80op { const char *name; int kind, arg; } x80ops[] = {
    { "add",    XO_ARITH,   X80_ADD  }, { "sub",    XO_ARITH,   X80_SUB  },
    { "subr",   XO_ARITH,   X80_SUBR }, { "mul",    XO_ARITH,   X80_MUL  },
    { "div",    XO_ARITH,   X80_DIV  }, { "divr",   XO_ARITH,   X80_DIVR },
    { "add32",  XO_ARITH32, X80_ADD  }, { "sub32",  XO_ARITH32, X80_SUB  },
    { "subr32", XO_ARITH32, X80_SUBR }, { "mul32",  XO_ARITH32, X80_MUL  },
    { "div32",  XO_ARITH32, X80_DIV  }, { "divr32", XO_ARITH32, X80_DIVR },
    { "add64",  XO_ARITH64, X80_ADD  }, { "sub64",  XO_ARITH64, X80_SUB  },
    { "subr64", XO_ARITH64, X80_SUBR }, { "mul64",  XO_ARITH64, X80_MUL  },
    { "div64",  XO_ARITH64, X80_DIV  }, { "divr64", XO_ARITH64, X80_DIVR },
    { "fcom",   XO_CMP,     0 },        { "fucom",  XO_UCMP,    0 },
    { "fcom32", XO_CMP32,   0 },        { "fcom64", XO_CMP64,   0 },
    { "sqrt",   XO_SQRT,    0 },        { "rndint", XO_RNDINT,  0 },
    { "abs",    XO_ABS,     0 },        { "chs",    XO_CHS,     0 },
    { "tst",    XO_TST,     0 },        { "xam",    XO_XAM,     0 },
    { "prem",   XO_PREM,    0 },        { "scale",  XO_SCALE,   0 },
    { "xtract", XO_XTRACT,  0 },        { "const",  XO_CONST,   0 },
    { "fild",   XO_FILD,    0 },        { "fld32",  XO_FLD32,   0 },
    { "fld64",  XO_FLD64,   0 },        { "fst32",  XO_FST32,   0 },
    { "fst64",  XO_FST64,   0 },        { "fist16", XO_FIST,    2 },
    { "fist32", XO_FIST,    4 },        { "fist64", XO_FIST,    8 },
    { "f2xm1",  XO_F2XM1,   0 },        { "yl2x",   XO_YL2X,    0 },
    { "yl2xp1", XO_YL2XP1,  0 },        { "patan",  XO_PATAN,   0 },
    { "sin",    XO_SIN,     0 },        { "cos",    XO_COS,     0 },
    { "ptan",   XO_PTAN,    0 },
};
#define X80_NOPS (sizeof x80ops / sizeof *x80ops)

/* One case: a control word, two register operands, a memory operand or
   integer, and which constant. */
struct x80case {
    uint16_t cw;
    X80      a, b;
    uint64_t m;
    int      which;
};

/* What an operation made of one: its effect on the status word, its
   results, and an integer or float result as bits. */
struct x80out {
    X80Env   env;
    X80      r, s;
    uint64_t v;
};

/* Operation k's stream: its own, so that adding an operation to the table
   does not move the cases of the others. */
static inline X80Rng x80ops_stream(uint64_t seed, unsigned k)
{
    X80Rng g;
    g.s = seed ^ 0x9E3779B97F4A7C15u * (k + 1);
    return g;
}

/* The case a case seed stands for.  Every precision and rounding control,
   reserved PC 01 included, with the exception masks always on. */
static inline void x80case_gen(unsigned k, uint64_t seed, struct x80case *c)
{
    X80Rng g;
    int kind = x80ops[k].kind;

    g.s = seed;
    c->cw = (uint16_t)(0x007F | x80gen_below(&g, 4) << 8);
    c->cw |= (uint16_t)(x80gen_below(&g, 4) << 10);
    x80gen_value(&g, &c->a);
    if (x80gen_below(&g, 2)) x80gen_near(&g, &c->a, &c->b);
    else                     x80gen_value(&g, &c->b);
    c->which = (int)x80gen_below(&g, 7);
    c->m = 0;
    switch (kind) {
    case XO_ARITH32: case XO_CMP32: case XO_FLD32:
        c->m = x80gen_ieee(&g, &c->a, 0);
        break;
    case XO_ARITH64: case XO_CMP64: case XO_FLD64:
        c->m = x80gen_ieee(&g, &c->a, 1);
        break;
    case XO_FILD:
        c->m = (uint64_t)x80gen_int(&g, &c->a, 2u << x80gen_below(&g, 3));
        break;
    }
}

/* Run case c of operation k through a backend given as its prefix: the
   fuzzer names both, x80test only x80. */
#define X80OPS_RUN(P, k, c, o)                                               \
    do {                                                                     \
        X80Env *e_ = &(o)->env;                                              \
        int arg_ = x80ops[k].arg;                                            \
        memset((o), 0, sizeof *(o));                                         \
        e_->cw = (c)->cw;                                                    \
        switch (x80ops[k].kind) {                                            \
        case XO_ARITH:   P##arith(e_, arg_, &(c)->a, &(c)->b, &(o)->r); break; \
        case XO_ARITH32: P##arith_f32(e_, arg_, &(c)->a, (uint32_t)(c)->m, &(o)->r); break; \
        case XO_ARITH64: P##arith_f64(e_, arg_, &(c)->a, (c)->m, &(o)->r); break; \
        case XO_CMP:     P##compare(e_, &(c)->a, &(c)->b); break;            \
        case XO_UCMP:    P##ucompare(e_, &(c)->a, &(c)->b); break;           \
        case XO_CMP32:   P##compare_f32(e_, &(c)->a, (uint32_t)(c)->m); break; \
        case XO_CMP64:   P##compare_f64(e_, &(c)->a, (c)->m); break;         \
        case XO_SQRT:    P##sqrt(e_, &(c)->a, &(o)->r); break;               \
        case XO_RNDINT:  P##rndint(e_, &(c)->a, &(o)->r); break;             \
        case XO_ABS:     P##abs(e_, &(c)->a, &(o)->r); break;                \
        case XO_CHS:     P##chs(e_, &(c)->a, &(o)->r); break;                \
        case XO_TST:     P##tst(e_, &(c)->a); break;                         \
        case XO_XAM:     P##xam(e_, &(c)->a); break;                         \
        case XO_PREM:    P##prem(e_, &(c)->a, &(c)->b, &(o)->r); break;      \
        case XO_SCALE:   P##scale(e_, &(c)->a, &(c)->b, &(o)->r); break;     \
        case XO_XTRACT:  P##xtract(e_, &(c)->a, &(o)->r, &(o)->s); break;    \
        case XO_CONST:   P##constant(e_, (c)->which, &(o)->r); break;        \
        case XO_FILD:    P##from_int(e_, (int64_t)(c)->m, &(o)->r); break;   \
        case XO_FLD32:   P##from_f32(e_, (uint32_t)(c)->m, &(o)->r); break;  \
        case XO_FLD64:   P##from_f64(e_, (c)->m, &(o)->r); break;            \
        case XO_FST32:   (o)->v = P##to_f32(e_, &(c)->a); break;             \
        case XO_FST64:   (o)->v = P##to_f64(e_, &(c)->a); break;             \
        case XO_F2XM1:   P##f2xm1(e_, &(c)->a, &(o)->r); break;              \
        case XO_YL2X:    P##yl2x(e_, &(c)->a, &(c)->b, &(o)->r); break;      \
        case XO_YL2XP1:  P##yl2xp1(e_, &(c)->a, &(c)->b, &(o)->r); break;    \
        case XO_PATAN:   P##patan(e_, &(c)->a, &(c)->b, &(o)->r); break;     \
        case XO_SIN:     P##sin(e_, &(c)->a, &(o)->r); break;                \
        case XO_COS:     P##cos(e_, &(c)->a, &(o)->r); break;                \
        case XO_PTAN:    (o)->v = (uint64_t)P##ptan(e_, &(c)->a, &(o)->r, &(o)->s); \
                         break;                                              \
        case XO_FIST:    (o)->v = (uint64_t)P##to_int(e_, &(c)->a, (unsigned)arg_); \
                         break;                                              \
        }                                                                    \
        /* Only what the operation defines: the rest of the status word is  \
           the exception flags. */                                           \
        e_->sw &= (uint16_t)(0x80FFu | e_->cc);                              \
    } while (0)

static inline int x80ops_approx(unsigned k) { return x80ops[k].kind >= XO_F2XM1; }

/* How far apart two register results are, in units in the last place,
   capped: the difference of their encodings read as one number, exponent
   above significand, which is continuous across the denormal boundary.
   Only for finite values of the same sign within one exponent of each
   other; anything else is the cap. */
static inline uint64_t x80_ulps(const X80 *a, const X80 *b, uint64_t cap)
{
    int ea = a->se & 0x7FFF, eb = b->se & 0x7FFF, lo;
    uint64_t ka, kb;

    if ((a->se ^ b->se) & 0x8000 || ea == 0x7FFF || eb == 0x7FFF) return cap;
    if (ea > eb + 1 || eb > ea + 1) return cap;
    lo = ea < eb ? ea : eb;
    /* Counted from the smaller exponent's start: the fraction, plus one
       whole binade for the larger. */
    ka = (a->m & 0x7FFFFFFFFFFFFFFFu) + (ea > lo ? 0x8000000000000000u : 0);
    kb = (b->m & 0x7FFFFFFFFFFFFFFFu) + (eb > lo ? 0x8000000000000000u : 0);
    if (lo == 0) {           /* a denormal and the least binade: m alone */
        ka = a->m;
        kb = b->m;
    }
    ka = ka > kb ? ka - kb : kb - ka;
    return ka < cap ? ka : cap;
}

static inline int x80out_same(const struct x80out *h, const struct x80out *s)
{
    return h->env.sw == s->env.sw && h->env.cc == s->env.cc &&
           h->r.m == s->r.m && h->r.se == s->r.se &&
           h->s.m == s->s.m && h->s.se == s->s.se && h->v == s->v;
}

/* FNV-1a over what the outputs are, byte by byte in a fixed order, so that
   the hash is the same on every host. */
static inline uint64_t x80out_hash(uint64_t h, const struct x80out *o)
{
    uint64_t w[7];
    int i, j;

    w[0] = o->env.sw;
    w[1] = o->env.cc;
    w[2] = o->r.se;
    w[3] = o->r.m;
    w[4] = o->s.se;
    w[5] = o->s.m;
    w[6] = o->v;
    for (i = 0; i < 7; i++)
        for (j = 0; j < 8; j++) {
            h ^= (uint8_t)(w[i] >> (8 * j));
            h *= 0x100000001B3u;
        }
    return h;
}

#define X80OUT_HASH0 0xCBF29CE484222325u

/* Cases are hashed in blocks of this many, so a difference can be narrowed
   to a block before the fuzzer finds the case.  x80test checks the first
   X80TEST_QUICK blocks of each operation's stream from X80TEST_SEED, and
   with --heavy the rest up to X80TEST_HEAVY; the fuzzer's --emit records
   what they should hash to. */
#define X80OPS_BLOCK  4096
#define X80TEST_SEED  0x5EED0F87u
#define X80TEST_QUICK 16
#define X80TEST_HEAVY 256

#endif
