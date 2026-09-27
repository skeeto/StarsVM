/* fpu.h - x87 emulation for the guest.
 *
 * Stars! contains 751 hardware x87 sites covering 86 distinct instruction forms,
 * including the transcendentals the MS C library uses (F2XM1, FYL2X, FPTAN,
 * FPATAN, FPREM, FSQRT, FRNDINT, FSCALE).  fpu.c decodes them and keeps the
 * guest's registers, stack and status word; the arithmetic is a backend's,
 * through the interface in x80.h.  x87hw.c hands each operation to the
 * host's real x87 with the guest control word loaded, which is bit-exact
 * and needs an x86; x80.c does it all in integers, anywhere.  See fpusel.h.
 */
#ifndef FPU_H
#define FPU_H

#include "cpu.h"
#include "x80.h"

void fpu_reset(Cpu *c);

/* Execute one escape instruction.  `modrm` has already been fetched and, for a
   memory form, `sel:off` is the resolved operand address.  Returns 0 if the
   encoding is not implemented, which stops the machine rather than guessing. */
int  fpu_exec(Cpu *c, uint8_t op, uint8_t modrm, int is_reg,
              uint16_t sel, uint16_t off);

/* Save and restore whatever of the host the backend changes around guest
   execution: the host's own x87 control word, for x87hw.c, so that MinGW's
   default 53-bit precision does not leak into guest arithmetic nor the
   guest's into the host's. */
void fpu_host_enter(void);
void fpu_host_leave(void);

/* For an FPU=dual build, which runs both backends and compares them: whose
   results the guest continues with, "hw" or "soft" (0 if that is not one,
   or this is not such a build), and a summary for the log at exit, whose
   value is how many operations differed. */
int                fpu_follow(const char *which);
unsigned long long fpu_report(void);

/* Instructions, one call each, for a native routine (native.h) that stands
   in for guest code containing x87 instructions.  Each is the path fpu_exec
   takes for that instruction, so a routine built from them is bit-exact with
   the interpreter by construction: the same operation under the same guest
   control word, with the same effect on the stack, the tag word and the
   status word.  They work on the stack as the instructions do, so no value
   ever leaves it for C to hold, and nothing here says how the arithmetic is
   carried out.  An `op` is x80.h's: X80_ADD, X80_MUL and so on. */
void    fpu_fild(Cpu *c, int32_t v);                /* FILD m32           */
void    fpu_arith_m64(Cpu *c, int op, uint16_t sel, uint16_t off); /* DC /op */
void    fpu_sqrt(Cpu *c);                           /* FSQRT              */
void    fpu_xam(Cpu *c);                            /* FXAM               */
void    fpu_store_f64(Cpu *c, uint16_t sel, uint16_t off); /* FST m64     */
int64_t fpu_fistp64(Cpu *c);             /* FISTP m64, less the memory    */
void    fpu_clex(Cpu *c);                           /* FNCLEX             */

#endif
