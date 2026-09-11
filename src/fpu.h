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

#endif
