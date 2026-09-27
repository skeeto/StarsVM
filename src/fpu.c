/* fpu.c - the guest's x87: its registers, stack, tags and status word.
 *
 * The guest's eight registers are kept as the ten bytes of each one's memory
 * image, and the stack is modelled explicitly (top-of-stack index plus tag
 * word) rather than mapped onto anything of the host's, because the guest
 * can leave values live across arbitrary amounts of host code.
 *
 * The arithmetic is not done here.  Each operation goes to a backend through
 * FX(), as X80s (x80.h) with the guest's control word, and comes back as a
 * result and an effect on the status word, which is committed by the rule
 * x80.h gives.  Which backend is decided when the emulator is built.
 */

#include "fpu.h"
#include "sel.h"
#include "log.h"

#include <string.h>

/* The backend: x87hw.c hands each operation to this machine's own x87, and
   x80dual.c (FPU=dual) runs both it and x80.c's integer x87 and compares.
   NOTE tells the dual build where the operations come from. */
#if defined(STARSVM_FPU_DUAL)
#  include "x80dual.h"
#  define FX(name) dual_##name
#  define NOTE(c)  x80dual_note((c)->seg[S_CS], (c)->eip)
#else
#  include "x87hw.h"
#  define FX(name) x87hw_##name
#  define NOTE(c)  ((void)(c))
#endif

/* x87 status word condition-code bits. */
#define SW_C0 0x0100u
#define SW_C1 0x0200u
#define SW_C2 0x0400u
#define SW_C3 0x4000u

/* Tag word: two bits per physical register, 3 = empty. */
#define TAG_VALID 0
#define TAG_ZERO  1
#define TAG_SPEC  2
#define TAG_EMPTY 3

void fpu_host_enter(void) { FX(host_enter)(); }
void fpu_host_leave(void) { FX(host_leave)(); }

int fpu_follow(const char *which)
{
#if defined(STARSVM_FPU_DUAL)
    return x80dual_follow(which);
#else
    (void)which;
    return 0;
#endif
}

unsigned long long fpu_report(void)
{
#if defined(STARSVM_FPU_DUAL)
    return x80dual_report();
#else
    return 0;
#endif
}

void fpu_reset(Cpu *c)
{
    memset(c->st, 0, sizeof c->st);
    c->fpu_cw  = 0x037F;      /* round to nearest, extended precision, all masked */
    c->fpu_sw  = 0;
    c->fpu_tw  = 0xFFFF;      /* every register empty */
    c->fpu_top = 0;
}

/* ------------------------------------------------------------ stack plumbing */

static int phys(Cpu *c, int i) { return (c->fpu_top + i) & 7; }

static int tag_of(Cpu *c, int p) { return (c->fpu_tw >> (p * 2)) & 3; }

static void set_tag(Cpu *c, int p, int t)
{
    c->fpu_tw = (uint16_t)((c->fpu_tw & ~(3u << (p * 2))) | ((unsigned)t << (p * 2)));
}

static void st_get(Cpu *c, int i, X80 *v)
{
    x80_get(v, c->st[phys(c, i)].b);
}

/* Write ST(i) and tag it: zero for +0 and -0 (a denormal has the zero
   exponent but a significand, so it is correctly not zero), valid for
   everything else. */
static void st_set(Cpu *c, int i, const X80 *v)
{
    int p = phys(c, i);
    x80_put(c->st[p].b, v);
    set_tag(c, p, !(v->se & 0x7FFFu) && !v->m ? TAG_ZERO : TAG_VALID);
}

/* Push, leaving C1 as the instruction left it: the loads have cleared it
   already, and FPTAN's is the rounding of the tangent under the 1.0 it
   pushes. */
static void fpu_push(Cpu *c, const X80 *v)
{
    c->fpu_top = (uint8_t)((c->fpu_top - 1) & 7);
    if (tag_of(c, phys(c, 0)) != TAG_EMPTY) {
        /* Stack overflow.  All exceptions are masked in practice, and the
           hardware would push anyway with C1 set. */
        c->fpu_sw |= SW_C1 | 0x0041u;
    }
    st_set(c, 0, v);
}

/* For the instructions that only move values - FLD ST(i), FLD m80, FST and
   FSTP to a register or m80, FXCH, FINCSTP, FDECSTP - and so define C1 as
   clear and nothing else. */
static void c1_clear(Cpu *c)
{
    c->fpu_sw &= ~SW_C1;
}

static void fpu_discard(Cpu *c)
{
    set_tag(c, phys(c, 0), TAG_EMPTY);
    c->fpu_top = (uint8_t)((c->fpu_top + 1) & 7);
}

/* A control word as the x87 keeps it: the reserved bits 7 and 13-15 read
   back clear and bit 6 set, whatever was loaded, and bit 12, the 287's
   infinity control, is kept though nothing looks at it.  Measured by the
   fuzzer, which compares the control word after FLDCW. */
static uint16_t cw_loaded(uint16_t v)
{
    return (uint16_t)((v & 0x1F3Fu) | 0x0040u);
}

/* Status word with the current top-of-stack encoded, as the guest expects. */
static uint16_t sw_value(Cpu *c)
{
    return (uint16_t)((c->fpu_sw & ~0x3800u) | ((unsigned)c->fpu_top << 11));
}

/* What an operation needs of the machine, and how what it reports goes back:
   see x80.h. */
static X80Env env(Cpu *c)
{
    X80Env e;
    e.cw = c->fpu_cw;
    e.sw = e.cc = 0;
    return e;
}

static void commit(Cpu *c, const X80Env *e)
{
    c->fpu_sw = (uint16_t)((c->fpu_sw & ~e->cc) | e->sw);
}

/* ------------------------------------------------------------ memory operands */

/* One translation instead of eight or ten.  An access that runs off the end of
   a selector keeps the byte path, where the offset wraps inside the selector as
   16-bit addressing does. */
static void fpu_mem_read(uint16_t sel, uint16_t off, uint8_t *dst, unsigned n)
{
    unsigned i;

    if ((uint32_t)off + n <= 0x10000u) {
        memcpy(dst, sel_ptr(sel, off), n);
        return;
    }
    for (i = 0; i < n; i++) dst[i] = sel_rd8(sel, (uint16_t)(off + i));
}

static void fpu_mem_write(uint16_t sel, uint16_t off, const uint8_t *src,
                          unsigned n)
{
    unsigned i;

    if ((uint32_t)off + n <= 0x10000u) {
        memcpy(sel_ptr(sel, off), src, n);
        return;
    }
    for (i = 0; i < n; i++) sel_wr8(sel, (uint16_t)(off + i), src[i]);
}

static uint64_t rd64(uint16_t sel, uint16_t off)
{
    uint8_t b[8];
    uint64_t v = 0;
    int i;

    fpu_mem_read(sel, off, b, 8);
    for (i = 7; i >= 0; i--) v = v << 8 | b[i];
    return v;
}

static void wr64(uint16_t sel, uint16_t off, uint64_t v)
{
    uint8_t b[8];
    int i;

    for (i = 0; i < 8; i++) b[i] = (uint8_t)(v >> (8 * i));
    fpu_mem_write(sel, off, b, 8);
}

/* ---------------------------------------------------------------- operations */

/* ST(dst) = a op b, where a is ST(dst)'s own value. */
static void arith(Cpu *c, int op, int dst, const X80 *a, const X80 *b)
{
    X80Env e = env(c);
    X80 r;

    FX(arith)(&e, op, a, b, &r);
    commit(c, &e);
    st_set(c, dst, &r);
}

/* FCOM, or with `unordered` FUCOM, which differs in not raising IE for a
   quiet NaN. */
static void compare(Cpu *c, const X80 *a, const X80 *b, int unordered)
{
    X80Env e = env(c);

    if (unordered) FX(ucompare)(&e, a, b);
    else           FX(compare)(&e, a, b);
    commit(c, &e);
}

/* A D8, DA, DC or DE memory form: ST(0) against the operand, by the ModRM reg
   field, which is x80.h's operation code for everything but the compares.
   A float or double goes to the backend as the bits in memory, since the
   instruction loads and operates in one step and the difference shows (see
   x87hw.c); an integer converts exactly and raises nothing, so it is widened
   first. */
static void arith_mem(Cpu *c, uint8_t op, int reg, uint16_t sel, uint16_t off)
{
    X80Env e = env(c);
    X80 a, m, r;
    int cmp = reg == 2 || reg == 3;

    st_get(c, 0, &a);
    if (op == 0xD8) {
        uint32_t v = sel_rd32(sel, off);
        if (cmp) FX(compare_f32)(&e, &a, v);
        else     FX(arith_f32)(&e, reg, &a, v, &r);
    } else if (op == 0xDC) {
        uint64_t v = rd64(sel, off);
        if (cmp) FX(compare_f64)(&e, &a, v);
        else     FX(arith_f64)(&e, reg, &a, v, &r);
    } else {
        X80Env w = env(c);
        FX(from_int)(&w, op == 0xDA ? (int32_t)sel_rd32(sel, off)
                                    : (int16_t)sel_rd16(sel, off), &m);
        if (cmp) FX(compare)(&e, &a, &m);
        else     FX(arith)(&e, reg, &a, &m, &r);
    }
    commit(c, &e);
    if (!cmp)          st_set(c, 0, &r);
    else if (reg == 3) fpu_discard(c);
}

static void fild(Cpu *c, int64_t v)
{
    X80Env e = env(c);
    X80 r;

    FX(from_int)(&e, v, &r);
    commit(c, &e);
    fpu_push(c, &r);
}

/* FIST(P) at the given width in bytes, less the store. */
static int64_t fist(Cpu *c, unsigned width)
{
    X80Env e = env(c);
    X80 a;
    int64_t v;

    st_get(c, 0, &a);
    v = FX(to_int)(&e, &a, width);
    commit(c, &e);
    return v;
}

static void store_f32(Cpu *c, uint16_t sel, uint16_t off)
{
    X80Env e = env(c);
    X80 a;
    uint32_t bits;

    st_get(c, 0, &a);
    bits = FX(to_f32)(&e, &a);
    commit(c, &e);
    sel_wr32(sel, off, bits);
}

static void store_f64(Cpu *c, uint16_t sel, uint16_t off)
{
    X80Env e = env(c);
    X80 a;
    uint64_t bits;

    st_get(c, 0, &a);
    bits = FX(to_f64)(&e, &a);
    commit(c, &e);
    wr64(sel, off, bits);
}

/* The D9 E0..FF group, less the stack-pointer moves, by its second byte: one
   operation on ST(0), or on ST(0) and ST(1), and what it does to the stack.
   The two-operand ones that pop leave their result where ST(1) was. */
static int special(Cpu *c, uint8_t modrm)
{
    X80Env e = env(c);
    X80 a, b, r, s;

    st_get(c, 0, &a);
    st_get(c, 1, &b);
    switch (modrm) {
    case 0xE0: FX(chs)(&e, &a, &r);        break;             /* FCHS    */
    case 0xE1: FX(abs)(&e, &a, &r);        break;             /* FABS    */
    case 0xE4: FX(tst)(&e, &a);            commit(c, &e); return 1; /* FTST */
    case 0xE5: FX(xam)(&e, &a);            commit(c, &e); return 1; /* FXAM */
    case 0xF0: FX(f2xm1)(&e, &a, &r);      break;             /* F2XM1   */
    case 0xF1: FX(yl2x)(&e, &a, &b, &r);   fpu_discard(c); break;   /* FYL2X */
    case 0xF2:                                                /* FPTAN   */
        if (FX(ptan)(&e, &a, &r, &s)) {
            commit(c, &e);
            st_set(c, 0, &r);
            fpu_push(c, &s);
            return 1;
        }
        break;                              /* out of range: no push */
    case 0xF3: FX(patan)(&e, &a, &b, &r);  fpu_discard(c); break;   /* FPATAN */
    case 0xF4:                                                /* FXTRACT */
        FX(xtract)(&e, &a, &r, &s);
        commit(c, &e);
        st_set(c, 0, &r);
        fpu_push(c, &s);
        return 1;
    case 0xF8: FX(prem)(&e, &a, &b, &r);   break;             /* FPREM   */
    case 0xF9: FX(yl2xp1)(&e, &a, &b, &r); fpu_discard(c); break;   /* FYL2XP1 */
    case 0xFA: FX(sqrt)(&e, &a, &r);       break;             /* FSQRT   */
    case 0xFC: FX(rndint)(&e, &a, &r);     break;             /* FRNDINT */
    case 0xFD: FX(scale)(&e, &a, &b, &r);  break;             /* FSCALE  */
    case 0xFE: FX(sin)(&e, &a, &r);        break;             /* FSIN    */
    case 0xFF: FX(cos)(&e, &a, &r);        break;             /* FCOS    */
    default:   return 0;
    }
    commit(c, &e);
    st_set(c, 0, &r);
    return 1;
}

/* ------------------------------------------------------------------ dispatch */

int fpu_exec(Cpu *c, uint8_t op, uint8_t modrm, int is_reg,
             uint16_t sel, uint16_t off)
{
    int reg = (modrm >> 3) & 7;
    int rm  = modrm & 7;
    X80 a, b;

    NOTE(c);
    if (!is_reg) {
        /* ---- memory forms ---- */
        switch (op) {
        case 0xD8: case 0xDA: case 0xDC: case 0xDE:
            arith_mem(c, op, reg, sel, off);
            return 1;
        case 0xD9:
            switch (reg) {
            case 0: {                                                /* FLD m32  */
                X80Env e = env(c);
                FX(from_f32)(&e, sel_rd32(sel, off), &a);
                commit(c, &e);
                fpu_push(c, &a);
                return 1;
            }
            case 2: store_f32(c, sel, off); return 1;               /* FST m32  */
            case 3: store_f32(c, sel, off); fpu_discard(c); return 1; /* FSTP m32 */
            case 4: /* FLDENV: restore the 14-byte 16-bit environment */
                c->fpu_cw = cw_loaded(sel_rd16(sel, off));
                c->fpu_sw = sel_rd16(sel, (uint16_t)(off + 2));
                c->fpu_tw = sel_rd16(sel, (uint16_t)(off + 4));
                c->fpu_top = (uint8_t)((c->fpu_sw >> 11) & 7);
                return 1;
            case 5: c->fpu_cw = cw_loaded(sel_rd16(sel, off)); return 1; /* FLDCW */
            case 6: /* FSTENV */
                sel_wr16(sel, off, c->fpu_cw);
                sel_wr16(sel, (uint16_t)(off + 2), sw_value(c));
                sel_wr16(sel, (uint16_t)(off + 4), c->fpu_tw);
                sel_wr16(sel, (uint16_t)(off + 6), 0);
                sel_wr16(sel, (uint16_t)(off + 8), 0);
                sel_wr16(sel, (uint16_t)(off + 10), 0);
                sel_wr16(sel, (uint16_t)(off + 12), 0);
                c->fpu_cw |= 0x3Fu;                 /* FSTENV masks everything */
                return 1;
            case 7: sel_wr16(sel, off, c->fpu_cw); return 1;        /* FNSTCW   */
            default: return 0;
            }
        case 0xDB:
            switch (reg) {
            case 0: fild(c, (int32_t)sel_rd32(sel, off)); return 1; /* FILD m32 */
            case 2: sel_wr32(sel, off, (uint32_t)fist(c, 4)); return 1;   /* FIST  */
            case 3: sel_wr32(sel, off, (uint32_t)fist(c, 4));
                    fpu_discard(c); return 1;                        /* FISTP m32 */
            case 5: {                                                /* FLD m80  */
                uint8_t m[10];
                fpu_mem_read(sel, off, m, 10);
                x80_get(&a, m);
                c1_clear(c);
                fpu_push(c, &a);
                return 1;
            }
            case 7: {                                                /* FSTP m80 */
                uint8_t m[10];
                st_get(c, 0, &a);
                x80_put(m, &a);
                fpu_mem_write(sel, off, m, 10);
                c1_clear(c);
                fpu_discard(c);
                return 1;
            }
            default: return 0;
            }
        case 0xDD:
            switch (reg) {
            case 0: {                                                /* FLD m64  */
                X80Env e = env(c);
                FX(from_f64)(&e, rd64(sel, off), &a);
                commit(c, &e);
                fpu_push(c, &a);
                return 1;
            }
            case 2: store_f64(c, sel, off); return 1;               /* FST m64  */
            case 3: store_f64(c, sel, off); fpu_discard(c); return 1; /* FSTP m64 */
            case 7: sel_wr16(sel, off, sw_value(c)); return 1;       /* FNSTSW m16 */
            default: return 0;
            }
        case 0xDF:
            switch (reg) {
            case 0: fild(c, (int16_t)sel_rd16(sel, off)); return 1; /* FILD m16 */
            case 2: sel_wr16(sel, off, (uint16_t)fist(c, 2)); return 1;   /* FIST  */
            case 3: sel_wr16(sel, off, (uint16_t)fist(c, 2));
                    fpu_discard(c); return 1;                        /* FISTP m16 */
            case 5: fild(c, (int64_t)rd64(sel, off)); return 1;     /* FILD m64 */
            case 7: wr64(sel, off, (uint64_t)fist(c, 8));
                    fpu_discard(c); return 1;                        /* FISTP m64 */
            default: return 0;
            }
        default:
            return 0;
        }
    }

    /* ---- register forms ---- */
    switch (op) {
    case 0xD8:                                     /* op ST(0), ST(i) */
        st_get(c, 0, &a);
        st_get(c, rm, &b);
        if (reg == 2 || reg == 3) {
            compare(c, &a, &b, 0);
            if (reg == 3) fpu_discard(c);
        } else {
            arith(c, reg, 0, &a, &b);
        }
        return 1;
    case 0xD9:
        switch (reg) {
        case 0: st_get(c, rm, &a); c1_clear(c);
                fpu_push(c, &a); return 1;                            /* FLD ST(i) */
        case 1:                                                       /* FXCH      */
            st_get(c, 0, &a);
            st_get(c, rm, &b);
            st_set(c, 0, &b);
            st_set(c, rm, &a);
            c1_clear(c);
            return 1;
        case 2: if (rm == 0) return 1; return 0;                      /* FNOP      */
        /* Copy then pop, which is what DD /3 below has always done.  This
           form only popped, so `fstp st(2)` discarded ST(0) and left ST(2)
           alone. */
        case 3: st_get(c, 0, &a); st_set(c, rm, &a); c1_clear(c);
                fpu_discard(c); return 1;                             /* FSTP ST(i)*/
        case 5: {                                                     /* constants */
            /* FLD1 L2T L2E PI LG2 LN2 Z, in encoding order. */
            X80Env e = env(c);
            if (rm > 6) return 0;
            FX(constant)(&e, rm, &a);
            commit(c, &e);
            fpu_push(c, &a);
            return 1;
        }
        case 6:
            if (rm == 6 || rm == 7) {                                 /* FDECSTP, FINCSTP */
                c->fpu_top = (uint8_t)((c->fpu_top + (rm == 6 ? 7 : 1)) & 7);
                c1_clear(c);
                return 1;
            }
            return special(c, modrm);
        default:
            return special(c, modrm);
        }
    case 0xDA:
        return 0;                                  /* FCMOVcc: 686, not emitted */
    case 0xDB:
        if (reg == 4 && rm == 2) { c->fpu_sw &= ~0x80FFu; return 1; }  /* FCLEX */
        if (reg == 4 && rm == 3) { fpu_reset(c); return 1; }           /* FINIT */
        return 0;
    case 0xDC:                                     /* op ST(i), ST(0) */
    case 0xDE:                                     /* the same, then pop */
        /* The reversed forms swap SUB with SUBR and DIV with DIVR, since the
           destination is now the first operand: DC E8+i is ST(i) - ST(0). */
        if (op == 0xDE && reg == 3 && rm == 1) {   /* FCOMPP */
            st_get(c, 0, &a);
            st_get(c, 1, &b);
            compare(c, &a, &b, 0);
            fpu_discard(c);
            fpu_discard(c);
            return 1;
        }
        if (reg == 2 || reg == 3) return 0;
        st_get(c, rm, &a);
        st_get(c, 0, &b);
        arith(c, reg < 4 ? reg : reg ^ 1, rm, &a, &b);
        if (op == 0xDE) fpu_discard(c);
        return 1;
    case 0xDD:
        switch (reg) {
        case 0: set_tag(c, phys(c, rm), TAG_EMPTY);                    /* FFREE   */
                c1_clear(c);        /* undefined in the manual; measured */
                return 1;
        case 2: st_get(c, 0, &a); st_set(c, rm, &a);
                c1_clear(c); return 1;                                  /* FST ST(i)*/
        case 3: st_get(c, 0, &a); st_set(c, rm, &a); c1_clear(c);
                fpu_discard(c); return 1;                               /* FSTP    */
        case 4: case 5:                                                 /* FUCOM(P)*/
            st_get(c, 0, &a);
            st_get(c, rm, &b);
            compare(c, &a, &b, 1);
            if (reg == 5) fpu_discard(c);
            return 1;
        default: return 0;
        }
    case 0xDF:
        if (reg == 4 && rm == 0) {                 /* FNSTSW AX */
            c->r32[R_AX] = (c->r32[R_AX] & 0xFFFF0000u) | sw_value(c);
            return 1;
        }
        return 0;
    default:
        return 0;
    }
}

/* ------------------------------------------------------- for native routines */

/* See fpu.h.  Each is the path fpu_exec takes for the instruction it names. */
void fpu_fild(Cpu *c, int32_t v) { NOTE(c); fild(c, v); }

void fpu_arith_m64(Cpu *c, int op, uint16_t sel, uint16_t off)
{
    NOTE(c);
    arith_mem(c, 0xDC, op, sel, off);
}

void fpu_sqrt(Cpu *c) { NOTE(c); special(c, 0xFA); }
void fpu_xam(Cpu *c)  { NOTE(c); special(c, 0xE5); }

void fpu_store_f64(Cpu *c, uint16_t sel, uint16_t off)
{
    NOTE(c);
    store_f64(c, sel, off);
}

int64_t fpu_fistp64(Cpu *c)
{
    int64_t v;
    NOTE(c);
    v = fist(c, 8);
    fpu_discard(c);
    return v;
}

void fpu_clex(Cpu *c) { c->fpu_sw &= ~0x80FFu; }
