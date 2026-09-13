/* cpu.h - 16-bit x86 interpreter with protected-mode segmentation.
 *
 * Register and segment indices follow the hardware encodings so ModRM decoding
 * is a direct table lookup:
 *
 *     r32[]  0=EAX 1=ECX 2=EDX 3=EBX 4=ESP 5=EBP 6=ESI 7=EDI
 *     seg[]  0=ES  1=CS  2=SS  3=DS  4=FS  5=GS
 *
 * Flags live in an EFLAGS-shaped word, computed eagerly.  That costs a little
 * speed and buys exact PUSHF/POPF and a trivial comparison against the host CPU
 * in the differential fuzzer, which is the better trade for this project.
 */
#ifndef CPU_H
#define CPU_H

#include <stdint.h>

enum { R_AX, R_CX, R_DX, R_BX, R_SP, R_BP, R_SI, R_DI };
enum { S_ES, S_CS, S_SS, S_DS, S_FS, S_GS };

/* EFLAGS bits we model. */
#define F_CF 0x00000001u
#define F_RS 0x00000002u   /* reserved, reads as 1 */
#define F_PF 0x00000004u
#define F_AF 0x00000010u
#define F_ZF 0x00000040u
#define F_SF 0x00000080u
#define F_TF 0x00000100u
#define F_IF 0x00000200u
#define F_DF 0x00000400u
#define F_OF 0x00000800u

#define F_STATUS (F_CF | F_PF | F_AF | F_ZF | F_SF | F_OF)
#define F_ALL    (F_STATUS | F_TF | F_IF | F_DF)

/* Reasons cpu_run stops. */
enum {
    CPU_RUNNING = 0,
    CPU_HALT,           /* guest asked to stop                  */
    CPU_RETURN,         /* reached the call16 return address    */
    CPU_BADOP,          /* undecodable instruction              */
    CPU_NOAPI,          /* an imported API has no implementation */
    CPU_FAULT,          /* bad selector or division error       */
    CPU_STEPS           /* step budget exhausted                */
};

typedef struct {
    uint8_t  b[10];     /* 80-bit extended precision, host x87 layout */
} F80;

typedef struct {
    uint32_t r32[8];
    uint16_t seg[6];
    uint32_t eip;
    uint32_t eflags;

    /* x87.  Values are kept in host 80-bit form and handed to the host FPU. */
    F80      st[8];
    uint16_t fpu_cw, fpu_sw, fpu_tw;
    uint8_t  fpu_top;

    int      seg_override;  /* pending segment prefix, -1 when none */

    /* Where CS lands in the arena, and the selector that was true for.  Guest
       code is fetched through this instead of through sel_ptr.  Checked rather
       than invalidated: every path that can change CS changes seg[S_CS], and
       a selector's base is a pure function of its value, so a stale base is
       not expressible.  Copying a Cpu carries both, which is what makes
       call16_wndproc's save and restore correct for free. */
    uint8_t *cs_base;
    uint16_t cs_cached;

    int      state;         /* one of the CPU_* codes            */
    uint32_t bad_cs, bad_ip;
    uint8_t  bad_op, bad_op2;

    uint64_t icount;
} Cpu;

extern Cpu cpu;

void cpu_reset(Cpu *c);

/* Execute one instruction.  Returns a CPU_* code; CPU_RUNNING means continue. */
int  cpu_step(Cpu *c);

/* Execute until something stops it, or `max` instructions have run
   (max == 0 means no limit). */
int  cpu_run(Cpu *c, uint64_t max);

/* Latch a stop that has to escape through host frames.  A fatal condition
   inside a callback - an unimplemented API reached from a window procedure,
   say - surfaces deep inside Win32's own code (DispatchMessage, a modal dialog
   loop), which has no way to carry it back out.  Latching it means cpu_step
   refuses to run another guest instruction and hands the same reason to every
   enclosing cpu_run, so exactly one report comes out at the top. */
void cpu_stop_latch(Cpu *c, int reason);
int  cpu_stop_latched(void);

/* Stack helpers shared with the thunk layer. */
void     cpu_push16(Cpu *c, uint16_t v);
uint16_t cpu_pop16(Cpu *c);

/* Opcode 0xD6 marks a site where a native routine stands in for guest code
   (see native.h).  cpu_step hands it here with the address of the instruction;
   the hook answers 1 when it ran the routine and left eip where it stopped,
   0 when it declined - having put the byte 0xD6 replaced into *orig, so the
   instruction can be decoded as it was - and -1 when the address is not a
   site at all, which is then the undecodable instruction it always was.  A
   pointer rather than a call so that cpu.c, and the fuzzer built from it, need
   know nothing about native.c. */
extern int (*cpu_native)(Cpu *c, uint16_t ip0, uint8_t *orig);

/* The interpreter's own flag arithmetic, for a native routine that has to
   leave eflags exactly as the instructions it stands in for would have -
   including the bits real hardware leaves undefined and this interpreter
   defines, because --verify-native compares the whole register.  `res` is the
   unmasked result and `size` is in bytes.  cpu_shift is do_shift: op is the
   group-2 reg field (4 SHL, 5 SHR, 7 SAR, ...). */
void     cpu_flags_add(Cpu *c, uint32_t a, uint32_t b, uint32_t carry,
                       uint32_t res, int size);
void     cpu_flags_sub(Cpu *c, uint32_t a, uint32_t b, uint32_t borrow,
                       uint32_t res, int size);
void     cpu_flags_logic(Cpu *c, uint32_t res, int size);
uint32_t cpu_shift(Cpu *c, int op, uint32_t v, unsigned count, int size);
uint32_t cpu_inc(Cpu *c, uint32_t a, int size);     /* CF untouched */
uint32_t cpu_dec(Cpu *c, uint32_t a, int size);
/* MUL/IMUL r/m: the accumulator in, the product in DX:AX (EDX:EAX). */
void     cpu_mul(Cpu *c, int size, uint32_t src, int signed_op);

/* Convenience accessors. */
static inline uint16_t reg16(Cpu *c, int r)            { return (uint16_t)c->r32[r]; }
static inline void set_reg16(Cpu *c, int r, uint16_t v){ c->r32[r] = (c->r32[r] & 0xFFFF0000u) | v; }
static inline uint16_t cpu_ip(Cpu *c)                  { return (uint16_t)c->eip; }

const char *cpu_state_name(int state);

/* Decode one instruction for logging.  Writes at most `len` bytes and returns
   the instruction length. */
int  disasm(uint16_t sel, uint16_t off, char *out, int len);

#endif
