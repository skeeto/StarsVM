/* x80dual.h - both x87 backends at once, for FPU=dual builds.
 *
 * Every operation runs on x87hw.c and on x80.c, the two results are
 * compared, and one of them goes on to the guest: see x80dual.c.  The
 * functions have x80.h's signatures under the prefix dual_, so that fpu.c's
 * FX() can name them like either backend's.
 */
#ifndef X80DUAL_H
#define X80DUAL_H

#include "x80.h"

/* Where the operations that follow come from, for the report. */
void x80dual_note(uint16_t cs, uint32_t ip);

/* Which backend's results the guest continues with: "hw" (the default) or
   "soft".  0 for anything else. */
int  x80dual_follow(const char *which);

/* Log how many operations ran and how many differed, by operation; the
   number that differed. */
unsigned long long x80dual_report(void);

void     dual_arith(X80Env *e, int op, const X80 *a, const X80 *b, X80 *r);
void     dual_arith_f32(X80Env *e, int op, const X80 *a, uint32_t m, X80 *r);
void     dual_arith_f64(X80Env *e, int op, const X80 *a, uint64_t m, X80 *r);
void     dual_compare(X80Env *e, const X80 *a, const X80 *b);
void     dual_ucompare(X80Env *e, const X80 *a, const X80 *b);
void     dual_compare_f32(X80Env *e, const X80 *a, uint32_t m);
void     dual_compare_f64(X80Env *e, const X80 *a, uint64_t m);
void     dual_sqrt(X80Env *e, const X80 *a, X80 *r);
void     dual_rndint(X80Env *e, const X80 *a, X80 *r);
void     dual_abs(X80Env *e, const X80 *a, X80 *r);
void     dual_chs(X80Env *e, const X80 *a, X80 *r);
void     dual_tst(X80Env *e, const X80 *a);
void     dual_xam(X80Env *e, const X80 *a);
void     dual_prem(X80Env *e, const X80 *a, const X80 *b, X80 *r);
void     dual_scale(X80Env *e, const X80 *a, const X80 *b, X80 *r);
void     dual_xtract(X80Env *e, const X80 *a, X80 *exp, X80 *sig);
void     dual_constant(X80Env *e, int which, X80 *r);
void     dual_from_int(X80Env *e, int64_t v, X80 *r);
void     dual_from_f32(X80Env *e, uint32_t bits, X80 *r);
void     dual_from_f64(X80Env *e, uint64_t bits, X80 *r);
uint32_t dual_to_f32(X80Env *e, const X80 *a);
uint64_t dual_to_f64(X80Env *e, const X80 *a);
int64_t  dual_to_int(X80Env *e, const X80 *a, unsigned width);

/* The transcendentals, compared approximately: see x80dual.c. */
void     dual_f2xm1(X80Env *e, const X80 *a, X80 *r);
void     dual_sin(X80Env *e, const X80 *a, X80 *r);
void     dual_cos(X80Env *e, const X80 *a, X80 *r);
int      dual_ptan(X80Env *e, const X80 *a, X80 *r, X80 *one);
void     dual_patan(X80Env *e, const X80 *a, const X80 *b, X80 *r);
void     dual_yl2x(X80Env *e, const X80 *a, const X80 *b, X80 *r);
void     dual_yl2xp1(X80Env *e, const X80 *a, const X80 *b, X80 *r);

void     dual_host_enter(void);
void     dual_host_leave(void);

#endif
