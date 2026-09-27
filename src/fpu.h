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

/* Instructions, one call each, for a native routine (native.h) that stands
   in for guest code containing x87 instructions.  Each is the path fpu_exec
   takes for that instruction, so a routine built from them is bit-exact with
   the interpreter by construction: the same operation under the same guest
   control word, with the same effect on the stack, the tag word and the
   status word.  They work on the stack as the instructions do, so no value
   ever leaves it for C to hold, and nothing here says how the arithmetic is
   carried out. */
enum { FPU_ADD, FPU_SUB, FPU_SUBR, FPU_MUL, FPU_DIV, FPU_DIVR };
void    fpu_fild(Cpu *c, int32_t v);                /* FILD m32           */
void    fpu_arith_m64(Cpu *c, int op, uint16_t sel, uint16_t off); /* DC /r */
void    fpu_sqrt(Cpu *c);                           /* FSQRT              */
void    fpu_xam(Cpu *c);                            /* FXAM               */
void    fpu_store_f64(Cpu *c, uint16_t sel, uint16_t off); /* FST m64     */
int64_t fpu_fistp64(Cpu *c);             /* FISTP m64, less the memory    */
void    fpu_clex(Cpu *c);                           /* FNCLEX             */

#endif
