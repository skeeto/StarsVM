/* fpu.h - x87 emulation for the guest.
 *
 * Stars! contains 751 hardware x87 sites covering 86 distinct instruction forms,
 * including the transcendentals the MS C library uses (F2XM1, FYL2X, FPTAN,
 * FPATAN, FPREM, FSQRT, FRNDINT, FSCALE).  Writing a soft-float x87 with correct
 * 80-bit semantics for all of that would be the largest and least interesting
 * part of this project - so instead each guest instruction is handed to the
 * host's real x87 with the guest control word loaded, which is bit-exact and
 * small.  This is also why the 32-bit x86 host is the natural target.
 */
#ifndef FPU_H
#define FPU_H

#include "cpu.h"

void fpu_reset(Cpu *c);

/* Execute one escape instruction.  `modrm` has already been fetched and, for a
   memory form, `sel:off` is the resolved operand address.  Returns 0 if the
   encoding is not implemented, which stops the machine rather than guessing. */
int  fpu_exec(Cpu *c, uint8_t op, uint8_t modrm, int is_reg,
              uint16_t sel, uint16_t off);

/* Save and restore the host control word around guest execution, so MinGW's
   default 53-bit precision does not leak into guest arithmetic. */
void fpu_host_enter(void);
void fpu_host_leave(void);

/* The primitives fpu_exec is made of, for a native routine (native.h) that
   stands in for guest code containing x87 instructions.  Going through these
   rather than through C arithmetic is what keeps such a routine bit-exact
   with the interpreter: the same host instruction under the same guest
   control word, with the same effect on the tag word and status word.
   ST(i) is relative to the guest's top of stack, as in the instructions. */
long double fpu_get(Cpu *c, int i);                 /* ld_get             */
void        fpu_set(Cpu *c, int i, long double v);  /* ld_set             */
void        fpu_load(Cpu *c, long double v);        /* push: FLD          */
void        fpu_pop(Cpu *c);                        /* FSTP's pop         */
enum { FPU_ADD, FPU_SUB, FPU_SUBR, FPU_MUL, FPU_DIV, FPU_DIVR };
long double fpu_arith(Cpu *c, int op, long double a, long double b);
long double fpu_sqrt(Cpu *c, long double a);        /* FSQRT              */
void        fpu_xam(Cpu *c, long double a);         /* FXAM: sets C3..C0  */
int64_t     fpu_to_int(Cpu *c, long double v, unsigned width);  /* FIST(P) */
uint16_t    fpu_status(Cpu *c);                     /* FNSTSW's value     */
long double fpu_load_f64(uint16_t sel, uint16_t off);
void        fpu_store_f64(Cpu *c, uint16_t sel, uint16_t off, long double v); /* FST m64 */
void        fpu_clex(Cpu *c);                       /* FNCLEX             */

#endif
