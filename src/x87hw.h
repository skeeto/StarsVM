/* x87hw.h - x87 operations on X80s, carried out by this machine's own x87.
 *
 * Each function loads the guest's control word and its operands into the
 * host x87, runs the one real instruction, and hands back the result and the
 * status word.  On an x86 that makes every result exactly the silicon's,
 * transcendentals included, which is what the fuzzer compares against and
 * what the goldens were made with.  It is also the only code in the emulator
 * that needs an x86 to run.
 *
 * Operands are ST(0) first: `a` is ST(0) and `b` ST(1) for the two-operand
 * instructions (FPATAN is atan(b/a), FYL2X is b*log2(a), FSCALE a*2^b), and
 * arith follows x80.h's operation codes.  `r` never aliases an operand.  The
 * _f32 and _f64 forms take a float or double operand `m` as its bits, the
 * way the memory forms of the instructions do.
 */
#ifndef X87HW_H
#define X87HW_H

#include "x80.h"

/* Keep the host's own control word out of guest execution and back again, so
   that neither sees the other's precision or rounding. */
void x87hw_host_enter(void);
void x87hw_host_leave(void);

void     x87hw_arith(X80Env *e, int op, const X80 *a, const X80 *b, X80 *r);
void     x87hw_arith_f32(X80Env *e, int op, const X80 *a, uint32_t m, X80 *r);
void     x87hw_arith_f64(X80Env *e, int op, const X80 *a, uint64_t m, X80 *r);
void     x87hw_compare(X80Env *e, const X80 *a, const X80 *b);    /* FCOM  */
void     x87hw_ucompare(X80Env *e, const X80 *a, const X80 *b);   /* FUCOM */
void     x87hw_compare_f32(X80Env *e, const X80 *a, uint32_t m);
void     x87hw_compare_f64(X80Env *e, const X80 *a, uint64_t m);
void     x87hw_sqrt(X80Env *e, const X80 *a, X80 *r);
void     x87hw_rndint(X80Env *e, const X80 *a, X80 *r);
void     x87hw_f2xm1(X80Env *e, const X80 *a, X80 *r);
void     x87hw_sin(X80Env *e, const X80 *a, X80 *r);
void     x87hw_cos(X80Env *e, const X80 *a, X80 *r);
void     x87hw_abs(X80Env *e, const X80 *a, X80 *r);
void     x87hw_chs(X80Env *e, const X80 *a, X80 *r);
void     x87hw_tst(X80Env *e, const X80 *a);
void     x87hw_xam(X80Env *e, const X80 *a);
int      x87hw_ptan(X80Env *e, const X80 *a, X80 *r, X80 *one); /* pushed? */
void     x87hw_patan(X80Env *e, const X80 *a, const X80 *b, X80 *r);
void     x87hw_yl2x(X80Env *e, const X80 *a, const X80 *b, X80 *r);
void     x87hw_yl2xp1(X80Env *e, const X80 *a, const X80 *b, X80 *r);
void     x87hw_prem(X80Env *e, const X80 *a, const X80 *b, X80 *r);
void     x87hw_scale(X80Env *e, const X80 *a, const X80 *b, X80 *r);
void     x87hw_xtract(X80Env *e, const X80 *a, X80 *exp, X80 *sig);
void     x87hw_constant(X80Env *e, int which, X80 *r);
void     x87hw_from_int(X80Env *e, int64_t v, X80 *r);
void     x87hw_from_f32(X80Env *e, uint32_t bits, X80 *r);
void     x87hw_from_f64(X80Env *e, uint64_t bits, X80 *r);
uint32_t x87hw_to_f32(X80Env *e, const X80 *a);
uint64_t x87hw_to_f64(X80Env *e, const X80 *a);
int64_t  x87hw_to_int(X80Env *e, const X80 *a, unsigned width);

#endif
