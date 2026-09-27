/* x87hw.c - the x87 backend that is an x87.  See x87hw.h.
 *
 * Every operand goes through memory: an X80's first ten bytes are the
 * register's memory image on a little-endian machine (asserted below), so
 * FLDT and FSTPT move them in and out directly, and the status word comes
 * back through FNSTSW AX.  The host stack is left as it was found by every
 * function, so nothing here depends on what the compiler keeps there.
 *
 * Each operation starts with FNCLEX, so the exception flags it reports are
 * its own.  Nothing else on the host clears them, and before this every
 * operation handed back everything the host had accumulated since the
 * process started - and assigned it over the guest's own, so a guest FCLEX
 * lasted only until the next operation.
 *
 * That status word is an early-clobber output, "=&a", everywhere: FNSTSW
 * writes AX before the result is stored, and without the & the compiler is
 * free to address the result through EAX - which it did, on both Windows
 * builds, and the store went wherever the status word pointed.
 */
#include "x87hw.h"

#include <stddef.h>

#if !defined(__i386__) && !defined(__x86_64__)
#  error "x87hw.c needs an x86: build with FPU=soft"
#endif

_Static_assert(offsetof(X80, m) == 0 && offsetof(X80, se) == 8,
               "an X80 must begin with the ten-byte memory image");

/* The host's status word after one operation, as x80.h's merge wants it: the
   exception flags, SF, ES and B only ever set, so they accumulate in the
   guest's as they would on an x87; the condition codes the host's. */
static void took(X80Env *e, uint16_t sw)
{
    e->cc = 0x4700u;
    e->sw = (uint16_t)(sw & 0xC7FFu);
}

/* The same for an operation that defines no condition code. */
static void raised(X80Env *e, uint16_t sw)
{
    e->cc = 0;
    e->sw = (uint16_t)(sw & 0x80FFu);
}

static uint16_t host_cw_saved;

void x87hw_host_enter(void)
{
    __asm__ volatile ("fnstcw %0" : "=m"(host_cw_saved));
}

void x87hw_host_leave(void)
{
    __asm__ volatile ("fldcw %0" : : "m"(host_cw_saved));
}

/* a and b onto the host stack, in the order given by operand number (%3 is
   a, %4 is b) so that the second is ST(0); then the instruction, which pops
   one of them, and the result stored. */
#define ARITH(first, second, insn)                                           \
    __asm__ volatile ("fldcw %2\n\tfnclex\n\t"                               \
                      "fldt %" first "\n\tfldt %" second "\n\t" insn "\n\t"  \
                      "fnstsw %0\n\tfstpt %1"                                \
                      : "=&a"(sw), "=m"(*r) : "m"(cw), "m"(*a), "m"(*b)      \
                      : "st", "st(1)")

void x87hw_arith(X80Env *e, int op, const X80 *a, const X80 *b, X80 *r)
{
    uint16_t cw = e->cw, sw = 0;

    switch (op) {
    case X80_ADD:  ARITH("3", "4", "faddp"); break;
    case X80_SUB:  ARITH("4", "3", "fsubp"); break;          /* a - b */
    case X80_SUBR: ARITH("3", "4", "fsubp"); break;          /* b - a */
    case X80_MUL:  ARITH("3", "4", "fmulp"); break;
    case X80_DIV:  ARITH("4", "3", "fdivp"); break;          /* a / b */
    default:       ARITH("3", "4", "fdivp"); break;          /* b / a */
    }
    took(e, sw);
}

/* a against a float or double in memory, which is not the same as widening
   it first: a signalling NaN stays one until the operation sees it, and a
   denormal operand is a denormal operand even though the 80-bit format could
   hold it as a normal.  Both show in the exception flags. */
#define MEMOP(insn)                                                          \
    __asm__ volatile ("fldcw %2\n\tfnclex\n\tfldt %3\n\t" insn " %4\n\t"    \
                      "fnstsw %0\n\tfstpt %1"                                \
                      : "=&a"(sw), "=m"(*r) : "m"(cw), "m"(*a), "m"(m)       \
                      : "st")

void x87hw_arith_f32(X80Env *e, int op, const X80 *a, uint32_t m, X80 *r)
{
    uint16_t cw = e->cw, sw = 0;

    switch (op) {
    case X80_ADD:  MEMOP("fadds");  break;
    case X80_SUB:  MEMOP("fsubs");  break;
    case X80_SUBR: MEMOP("fsubrs"); break;
    case X80_MUL:  MEMOP("fmuls");  break;
    case X80_DIV:  MEMOP("fdivs");  break;
    default:       MEMOP("fdivrs"); break;
    }
    took(e, sw);
}

void x87hw_arith_f64(X80Env *e, int op, const X80 *a, uint64_t m, X80 *r)
{
    uint16_t cw = e->cw, sw = 0;

    switch (op) {
    case X80_ADD:  MEMOP("faddl");  break;
    case X80_SUB:  MEMOP("fsubl");  break;
    case X80_SUBR: MEMOP("fsubrl"); break;
    case X80_MUL:  MEMOP("fmull");  break;
    case X80_DIV:  MEMOP("fdivl");  break;
    default:       MEMOP("fdivrl"); break;
    }
    took(e, sw);
}

/* FCOM: C3, C2 and C0 say how a compares with b, and C1 comes back clear.
   FUCOM differs only in not raising IE for a quiet NaN. */
#define COMPARE(insn)                                                        \
    __asm__ volatile ("fldcw %3\n\tfnclex\n\tfldt %2\n\tfldt %1\n\t"        \
                      insn "\n\tfnstsw %0"                                   \
                      : "=&a"(sw) : "m"(*a), "m"(*b), "m"(cw)                \
                      : "st", "st(1)")

void x87hw_compare(X80Env *e, const X80 *a, const X80 *b)
{
    uint16_t sw = 0, cw = e->cw;
    COMPARE("fcompp");
    took(e, sw);
}

void x87hw_ucompare(X80Env *e, const X80 *a, const X80 *b)
{
    uint16_t sw = 0, cw = e->cw;
    COMPARE("fucompp");
    took(e, sw);
}

#define MEMCOMPARE(insn)                                                     \
    __asm__ volatile ("fldcw %3\n\tfnclex\n\tfldt %1\n\t" insn " %2\n\t"    \
                      "fnstsw %0"                                            \
                      : "=&a"(sw) : "m"(*a), "m"(m), "m"(cw) : "st")

void x87hw_compare_f32(X80Env *e, const X80 *a, uint32_t m)
{
    uint16_t sw = 0, cw = e->cw;
    MEMCOMPARE("fcomps");
    took(e, sw);
}

void x87hw_compare_f64(X80Env *e, const X80 *a, uint64_t m)
{
    uint16_t sw = 0, cw = e->cw;
    MEMCOMPARE("fcompl");
    took(e, sw);
}

/* One operand in, one result out, under the guest control word. */
#define UNARY(insn)                                                          \
    uint16_t sw = 0, cw = e->cw;                                             \
    __asm__ volatile ("fldcw %2\n\tfnclex\n\tfldt %3\n\t" insn "\n\t"       \
                      "fnstsw %0\n\tfstpt %1"                                \
                      : "=&a"(sw), "=m"(*r) : "m"(cw), "m"(*a) : "st");      \
    took(e, sw)

void x87hw_sqrt(X80Env *e, const X80 *a, X80 *r)   { UNARY("fsqrt"); }
void x87hw_rndint(X80Env *e, const X80 *a, X80 *r) { UNARY("frndint"); }
void x87hw_f2xm1(X80Env *e, const X80 *a, X80 *r)  { UNARY("f2xm1"); }
void x87hw_sin(X80Env *e, const X80 *a, X80 *r)    { UNARY("fsin"); }
void x87hw_cos(X80Env *e, const X80 *a, X80 *r)    { UNARY("fcos"); }
void x87hw_abs(X80Env *e, const X80 *a, X80 *r)    { UNARY("fabs"); }
void x87hw_chs(X80Env *e, const X80 *a, X80 *r)    { UNARY("fchs"); }

/* One operand in and only the status word out. */
#define EXAMINE(insn)                                                        \
    uint16_t sw = 0, cw = e->cw;                                             \
    __asm__ volatile ("fldcw %2\n\tfnclex\n\tfldt %1\n\t" insn "\n\t"       \
                      "fnstsw %0\n\tfstp %%st(0)"                            \
                      : "=&a"(sw) : "m"(*a), "m"(cw) : "st");                \
    took(e, sw)

void x87hw_tst(X80Env *e, const X80 *a) { EXAMINE("ftst"); }
void x87hw_xam(X80Env *e, const X80 *a) { EXAMINE("fxam"); }

/* FPTAN replaces ST with tan(ST) and pushes 1.0 - but only if the argument is
   in range.  Outside it, C2 comes back set and the stack is left exactly as it
   was, so the second store is conditional and the caller is told whether the
   push happened.  Out of range, r is the argument itself. */
int x87hw_ptan(X80Env *e, const X80 *a, X80 *r, X80 *one)
{
    uint16_t sw = 0, cw = e->cw;

    __asm__ volatile ("fldcw %3\n\tfnclex\n\tfldt %4\n\tfptan\n\t"
                      "fnstsw %%ax\n\ttestb $0x04, %%ah\n\tjnz 1f\n\t"
                      "fstpt %2\n\t1:\tfstpt %1"
                      : "=&a"(sw), "=m"(*r), "=m"(*one) : "m"(cw), "m"(*a)
                      : "st", "st(1)", "cc");
    took(e, sw);
    return (sw & 0x0400u) == 0;
}

/* Two operands in, ST(1) loaded first, and one result out: the instruction
   pops one and FSTPT the other. */
#define BINARY(insn)                                                         \
    uint16_t sw = 0, cw = e->cw;                                             \
    __asm__ volatile ("fldcw %2\n\tfnclex\n\tfldt %4\n\tfldt %3\n\t"        \
                      insn "\n\tfnstsw %0\n\tfstpt %1"                       \
                      : "=&a"(sw), "=m"(*r) : "m"(cw), "m"(*a), "m"(*b)      \
                      : "st", "st(1)");                                      \
    took(e, sw)

void x87hw_patan(X80Env *e, const X80 *a, const X80 *b, X80 *r)  { BINARY("fpatan"); }
void x87hw_yl2x(X80Env *e, const X80 *a, const X80 *b, X80 *r)   { BINARY("fyl2x"); }
void x87hw_yl2xp1(X80Env *e, const X80 *a, const X80 *b, X80 *r) { BINARY("fyl2xp1"); }

/* The same, for the two that leave ST(1) where it was: one more pop. */
#define BINARY_KEEP(insn)                                                    \
    uint16_t sw = 0, cw = e->cw;                                             \
    __asm__ volatile ("fldcw %2\n\tfnclex\n\tfldt %4\n\tfldt %3\n\t"        \
                      insn "\n\tfnstsw %0\n\tfstpt %1\n\tfstp %%st(0)"       \
                      : "=&a"(sw), "=m"(*r) : "m"(cw), "m"(*a), "m"(*b)      \
                      : "st", "st(1)");                                      \
    took(e, sw)

void x87hw_prem(X80Env *e, const X80 *a, const X80 *b, X80 *r)  { BINARY_KEEP("fprem"); }
void x87hw_scale(X80Env *e, const X80 *a, const X80 *b, X80 *r) { BINARY_KEEP("fscale"); }

void x87hw_xtract(X80Env *e, const X80 *a, X80 *exp, X80 *sig)
{
    uint16_t sw = 0, cw = e->cw;

    __asm__ volatile ("fldcw %3\n\tfnclex\n\tfldt %4\n\tfxtract\n\t"
                      "fnstsw %0\n\tfstpt %1\n\tfstpt %2"
                      : "=&a"(sw), "=m"(*sig), "=m"(*exp) : "m"(cw), "m"(*a)
                      : "st", "st(1)");
    took(e, sw);
}

/* The constants from the host's own instructions rather than from C literals.
   Two reasons, and the fuzzer found both at once: the exact 64-bit significands
   are the hardware's to define, and the hardware rounds them by the current
   rounding mode, which a literal written once cannot do.  Five of the seven
   differed in the last byte.  None raises anything. */
void x87hw_constant(X80Env *e, int which, X80 *r)
{
    uint16_t cw = e->cw;

    switch (which) {
    case X80_ONE: __asm__ volatile ("fldcw %1\n\tfld1\n\tfstpt %0"
                                    : "=m"(*r) : "m"(cw) : "st"); break;
    case X80_L2T: __asm__ volatile ("fldcw %1\n\tfldl2t\n\tfstpt %0"
                                    : "=m"(*r) : "m"(cw) : "st"); break;
    case X80_L2E: __asm__ volatile ("fldcw %1\n\tfldl2e\n\tfstpt %0"
                                    : "=m"(*r) : "m"(cw) : "st"); break;
    case X80_PI:  __asm__ volatile ("fldcw %1\n\tfldpi\n\tfstpt %0"
                                    : "=m"(*r) : "m"(cw) : "st"); break;
    case X80_LG2: __asm__ volatile ("fldcw %1\n\tfldlg2\n\tfstpt %0"
                                    : "=m"(*r) : "m"(cw) : "st"); break;
    case X80_LN2: __asm__ volatile ("fldcw %1\n\tfldln2\n\tfstpt %0"
                                    : "=m"(*r) : "m"(cw) : "st"); break;
    default:      __asm__ volatile ("fldcw %1\n\tfldz\n\tfstpt %0"
                                    : "=m"(*r) : "m"(cw) : "st"); break;
    }
}

/* An integer converts exactly and raises nothing. */
void x87hw_from_int(X80Env *e, int64_t v, X80 *r)
{
    (void)e;
    __asm__ volatile ("fildll %1\n\tfstpt %0" : "=m"(*r) : "m"(v) : "st");
}

/* A float or double converts exactly too, but loading one raises IE for a
   signalling NaN, which arrives quieted, and DE for a denormal. */
void x87hw_from_f32(X80Env *e, uint32_t bits, X80 *r)
{
    uint16_t sw = 0;
    __asm__ volatile ("fnclex\n\tflds %2\n\tfnstsw %0\n\tfstpt %1"
                      : "=&a"(sw), "=m"(*r) : "m"(bits) : "st");
    raised(e, sw);
}

void x87hw_from_f64(X80Env *e, uint64_t bits, X80 *r)
{
    uint16_t sw = 0;
    __asm__ volatile ("fnclex\n\tfldl %2\n\tfnstsw %0\n\tfstpt %1"
                      : "=&a"(sw), "=m"(*r) : "m"(bits) : "st");
    raised(e, sw);
}

/* Narrowing a value to store it rounds, and rounding is something the status
   word reports: C1 says whether the result went up. */
uint32_t x87hw_to_f32(X80Env *e, const X80 *a)
{
    uint32_t bits;
    uint16_t sw, cw = e->cw;

    __asm__ volatile ("fldcw %3\n\tfnclex\n\tfldt %2\n\tfstps %1\n\tfnstsw %0"
                      : "=&a"(sw), "=m"(bits) : "m"(*a), "m"(cw) : "st");
    took(e, sw);
    return bits;
}

uint64_t x87hw_to_f64(X80Env *e, const X80 *a)
{
    uint64_t bits;
    uint16_t sw, cw = e->cw;

    __asm__ volatile ("fldcw %3\n\tfnclex\n\tfldt %2\n\tfstpl %1\n\tfnstsw %0"
                      : "=&a"(sw), "=m"(bits) : "m"(*a), "m"(cw) : "st");
    took(e, sw);
    return bits;
}

/* The guest rounding mode in the control word decides how FIST rounds, which
   is exactly what C code relies on after setting it for truncation. */
int64_t x87hw_to_int(X80Env *e, const X80 *a, unsigned width)
{
    int64_t r;
    uint16_t sw, cw = e->cw;

    __asm__ volatile ("fldcw %3\n\tfnclex\n\tfldt %2\n\tfistpll %1\n\tfnstsw %0"
                      : "=&a"(sw), "=m"(r) : "m"(*a), "m"(cw) : "st");
    took(e, sw);

    /* A value that does not fit the destination is not truncated to it: the
       hardware writes the integer indefinite for that width and raises IE.
       Casting the 64-bit result down wrote 0 instead, which is a plausible
       number and therefore the worst kind of wrong.  A conversion that
       already failed comes back as the 64-bit indefinite, which is out of
       range for the narrower widths and so lands in the same place. */
    if (width == 8) return r;
    if (width == 4) {
        if (r < -2147483647LL - 1 || r > 2147483647LL) {
            e->sw |= 0x0001u;                              /* IE */
            return (int64_t)(int32_t)0x80000000u;
        }
        return r;
    }
    if (r < -32768LL || r > 32767LL) {
        e->sw |= 0x0001u;
        return (int64_t)(int16_t)0x8000u;
    }
    return r;
}
