/* fuzz.c - differential test of the interpreter against the host CPU.
 *
 * The host is x86, so it is the best possible oracle: generate a random
 * instruction and a random register state, run it both in the interpreter and
 * natively in a trampoline, and compare registers and flags.
 *
 * Three details make this honest rather than approximate:
 *
 *  - A 16-bit instruction means something different where the default operand
 *    size is 32 bits, which is true of the host's code segment in either mode,
 *    so each generated instruction is emitted twice: the guest gets a 0x66
 *    prefix exactly when the host does not.  The core bytes are the same, but
 *    for the one exception below.
 *
 *  - Some flags are architecturally undefined for some instructions (AF after
 *    a logical op, OF after a multi-bit shift, everything but CF/OF after MUL).
 *    Each generated form carries the mask of flags that are actually defined,
 *    and only those are compared.  Comparing undefined flags would produce
 *    failures that mean nothing.
 *
 *  - The trampoline runs in whatever mode the host was built for, and
 *    64-bit mode is not a superset of 32-bit: the one-byte INC/DEC reg
 *    encodings became the REX prefixes, and the six decimal adjusts were
 *    dropped outright.  A form 64-bit mode spells differently carries an
 *    alternate encoding for the oracle to run, so the guest still sees the
 *    byte a 16-bit compiler would have emitted; a form 64-bit mode cannot
 *    run at all is skipped and counted, never quietly passed.
 *
 * Coverage is register and immediate forms: the ALU, shifts and rotates,
 * inc/dec, mul/div, the bit instructions, MOVZX/MOVSX, SETcc, SHLD/SHRD and the
 * decimal adjusts.  That is where flag bugs live.  Memory operands, string
 * operations and control transfer are not covered here - they are exercised by
 * running the game and by the targeted tests in tools/.
 */

#include "cpu.h"
#include "sel.h"
#include "log.h"

#include <cpuid.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

/* Flags we can meaningfully compare.  TF must stay clear (it would single-step
   the host) and IF cannot be changed from user mode. */
#define CMP_FLAGS (F_CF | F_PF | F_AF | F_ZF | F_SF | F_OF | F_DF)

/* The whole oracle state in one block, because the 64-bit trampoline reaches
   all of it through a single base register - see eref() - and one base means
   one address to materialise rather than one per access.  The flag words are
   64 bits wide because `pushfq` and `popfq` have no narrower form; the 32-bit
   trampoline touches only their low halves, which on a little-endian host is
   the same dword.  saved_sp is host-sized for the same reason. */
struct fzstate {
    uint32_t in_r[8];
    uint64_t in_flags;
    uint32_t out_r[8];
    uint64_t out_flags;
    uint64_t saved_sp;
};
static struct fzstate fz;

#define FZ_IN_R(k)  (offsetof(struct fzstate, in_r)  + 4u * (unsigned)(k))
#define FZ_OUT_R(k) (offsetof(struct fzstate, out_r) + 4u * (unsigned)(k))

/* Which mode the trampoline is emitted for.  A constant either way, so the
   branches below fold away, but written as a value rather than an #if so that
   both encoders are compiled - and warned about - in both builds. */
static const int long_mode = sizeof(void *) == 8;

/* LAHF and SAHF were left out of the first 64-bit implementations and came
   back as a feature bit.  Without it they are #UD, which would take the
   process down rather than report a mismatch, so they have to be generated
   conditionally.  In 32-bit mode they are unconditional. */
static int lahf_ok = 1;

typedef void (*Tramp)(void);
static uint8_t *tramp_code;
static uint8_t *tramp_insn;      /* where the instruction bytes go */
static unsigned tramp_insn_max;
static uint8_t  tramp_ref[0x1000];   /* pristine copy, for the integrity check */
static unsigned tramp_len;
static long     tramp_calls;

/* ------------------------------------------------------------ code emission */

static uint8_t *emit;

static void e8(uint8_t v)   { *emit++ = v; }
static void e32(uint32_t v) { memcpy(emit, &v, 4); emit += 4; }
static void e64(uint64_t v) { memcpy(emit, &v, 8); emit += 8; }

/* One access to the state block.  A 32-bit host can name it absolutely, with
   the address sitting in the instruction as a bare disp32.  A 64-bit host
   cannot: that same encoding means RIP-relative there, and the distance from
   a VirtualAlloc'd page to a static is not guaranteed to fit in 2 GB anyway.
   So the block is reached through R15 instead, which is safe from the payload
   for a structural reason - naming R8-R15 requires a REX prefix, and no
   generated payload carries one.  Keeping that true is why the one-byte
   INC/DEC forms have to be re-encoded for the oracle; see gen(). */
static void eref(uint8_t op, int reg, size_t off)
{
    if (long_mode) {
        e8(0x41);                                 /* REX.B: the base is R15  */
        e8(op);
        e8((uint8_t)(0x80 | (reg << 3) | 7));     /* mod=10, r/m=111, disp32 */
        e32((uint32_t)off);
    } else {
        e8(op);
        e8((uint8_t)(0x05 | (reg << 3)));         /* mod=00, r/m=101, disp32 */
        e32((uint32_t)(uintptr_t)&fz + (uint32_t)off);
    }
}

/* The stack pointer is the one register whose full width matters, so on a
   64-bit host it needs REX.W where the others do not. */
static void eref_sp(uint8_t op, size_t off)
{
    if (long_mode) {
        e8(0x49); e8(op); e8(0xA7); e32((uint32_t)off);   /* [r15+disp32] */
    } else {
        eref(op, 4, off);
    }
}

/* Build the trampoline once.  Layout:
     save the host's callee-saved registers, stash the stack pointer
     load the guest state from fz.in_r (the stack pointer excluded)
     push fz.in_flags; popf
     <instruction bytes>
     pushf; pop fz.out_flags          (mov does not disturb flags, so this is first)
     store registers to fz.out_r
     restore the host's registers, ret                                        */
static int tramp_build(unsigned insn_max)
{
    /* The guest registers, in trampoline order.  Index 4 - the stack pointer -
       is absent: the host's own stack lives there, so it is neither loaded nor
       stored, and rnd_reg() never generates it as an operand either. */
    static const int gpr[7] = { 0, 1, 2, 3, 5, 6, 7 };
    unsigned i;

    tramp_code = VirtualAlloc(NULL, 0x1000, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!tramp_code) {
        log_msg("fuzz: cannot allocate an executable trampoline\n");
        return 0;
    }
    emit = tramp_code;

    /* bx/bp/si/di are callee-saved in both ABIs, and the payload writes all of
       them.  R15 joins them on a 64-bit host, where it also carries the base. */
    e8(0x53); e8(0x55); e8(0x56); e8(0x57);       /* push bx/bp/si/di       */
    if (long_mode) {
        e8(0x41); e8(0x57);                       /* push r15               */
        e8(0x49); e8(0xBF);                       /* movabs r15, &fz        */
        e64((uint64_t)(uintptr_t)&fz);
    }
    eref_sp(0x89, offsetof(struct fzstate, saved_sp));

    for (i = 0; i < 7; i++) eref(0x8B, gpr[i], FZ_IN_R(gpr[i]));
    eref(0xFF, 6, offsetof(struct fzstate, in_flags));   /* push [in_flags] */
    e8(0x9D);                                            /* popf            */

    tramp_insn = emit;
    tramp_insn_max = insn_max;
    for (i = 0; i < insn_max; i++) e8(0x90);      /* room for the payload   */

    e8(0x9C);                                            /* pushf           */
    eref(0x8F, 0, offsetof(struct fzstate, out_flags));  /* pop [out_flags] */
    for (i = 0; i < 7; i++) eref(0x89, gpr[i], FZ_OUT_R(gpr[i]));
    /* The payload may have been STD, and both ABIs guarantee DF is clear on
       entry to and return from a function.  Leaving it set makes the compiler's
       `rep movs` run backwards - which showed up as memcpy corrupting the three
       bytes below this very slot.  Flags have already been captured above, so
       clearing DF here costs nothing. */
    e8(0xFC);                                     /* cld                    */
    eref_sp(0x8B, offsetof(struct fzstate, saved_sp));
    if (long_mode) { e8(0x41); e8(0x5F); }        /* pop r15                */
    e8(0x5F); e8(0x5E); e8(0x5D); e8(0x5B);       /* pop di/si/bp/bx        */
    e8(0xC3);                                     /* ret                    */

    tramp_len = (unsigned)(emit - tramp_code);
    memcpy(tramp_ref, tramp_code, tramp_len);
    log_msg("fuzz: %d-bit trampoline at %p, %u bytes, payload slot at +%02X\n",
            long_mode ? 64 : 32, (void *)tramp_code, tramp_len,
            (unsigned)(tramp_insn - tramp_code));
    return 1;
}


/* The trampoline is self-modifying by design (the payload slot), so anything
   else changing is memory corruption and worth catching immediately rather than
   debugging as a mysterious crash. */
static int tramp_check(void)
{
    unsigned i;
    for (i = 0; i < tramp_len; i++) {
        if (i >= (unsigned)(tramp_insn - tramp_code) &&
            i <  (unsigned)(tramp_insn - tramp_code) + tramp_insn_max)
            continue;                              /* the payload slot */
        if (tramp_code[i] != tramp_ref[i]) {
            log_msg("fuzz: trampoline corrupted at +%02X (%02X, expected %02X)"
                    " after %ld calls\n",
                    i, tramp_code[i], tramp_ref[i], tramp_calls);
            return 0;
        }
    }
    return 1;
}

static void tramp_run(const uint8_t *insn, unsigned len)
{
    unsigned i;
    if (len > tramp_insn_max) {
        log_msg("fuzz: payload of %u bytes exceeds the %u-byte slot\n",
                len, tramp_insn_max);
        return;
    }
    memcpy(tramp_insn, insn, len);
    for (i = len; i < tramp_insn_max; i++) tramp_insn[i] = 0x90;
    tramp_calls++;
    ((Tramp)tramp_code)();
    if (!tramp_check()) {
        log_msg("fuzz: aborting, the oracle is no longer trustworthy\n");
        exit(2);
    }
}

/* ----------------------------------------------------------------- generator */

static uint32_t rng_state = 0x2C1A3F5Bu;

static uint32_t rnd(void)
{
    /* xorshift32: deterministic, so a failure can be replayed from its seed. */
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return rng_state = x;
}

static uint32_t rnd_below(uint32_t n) { return rnd() % n; }

/* An interesting operand value: extremes far more often than uniform noise,
   because that is where carry, overflow and the nibble flags actually change. */
static uint32_t rnd_value(void)
{
    switch (rnd_below(8)) {
    case 0: return 0;
    case 1: return 1;
    case 2: return 0xFFFFFFFFu;
    case 3: return 0x80000000u;
    case 4: return 0x7FFFFFFFu;
    case 5: return 0x8000u | (rnd() & 0xFF);
    case 6: return 0xFF00u | (rnd() & 0xFF);
    default: return rnd();
    }
}

/* A register number usable as a 16/32-bit operand.  SP is excluded: the host
   trampoline runs on the real stack, so an instruction that wrote SP would
   destroy it. */
static int rnd_reg(void) { int r = (int)rnd_below(7); return r >= 4 ? r + 1 : r; }

struct form {
    uint8_t  bytes[8];
    unsigned len;
    uint8_t  alt[8];     /* what the oracle runs where this host spells it
                            differently; alt_len 0 means `bytes` serves both */
    unsigned alt_len;
    const char *why_not;  /* non-NULL: this host cannot run it at all, and why */
    int      want32;     /* guest operand size is 32 bits */
    uint32_t mask;       /* flags that are architecturally defined */
    unsigned cl_mod;     /* if set, force CL into [0, cl_mod) before running */
    const char *what;
};

/* modrm for "register, register", avoiding SP when the size is 16/32. */
static uint8_t modrm_rr(int reg, int rm) { return (uint8_t)(0xC0 | (reg << 3) | rm); }

static void gen(struct form *f)
{
    int size8 = (int)rnd_below(3) == 0;
    int reg, rm;

    memset(f, 0, sizeof *f);
    f->mask = CMP_FLAGS;
    f->want32 = !size8 && (int)rnd_below(4) == 0;

    reg = size8 ? (int)rnd_below(8) : rnd_reg();
    rm  = size8 ? (int)rnd_below(8) : rnd_reg();

    switch (rnd_below(18)) {
    case 0: {                                     /* ALU r/m,r and r,r/m */
        int aluop = (int)rnd_below(8);
        int dir = (int)rnd_below(2);
        f->bytes[0] = (uint8_t)((aluop << 3) | (dir << 1) | (size8 ? 0 : 1));
        f->bytes[1] = modrm_rr(reg, rm);
        f->len = 2;
        f->what = "alu r/m,r";
        break;
    }
    case 1: {                                     /* ALU acc, imm */
        int aluop = (int)rnd_below(8);
        f->bytes[0] = (uint8_t)((aluop << 3) | 4 | (size8 ? 0 : 1));
        if (size8) { f->bytes[1] = (uint8_t)rnd(); f->len = 2; }
        else if (f->want32) { f->len = 5; memcpy(f->bytes + 1, (uint32_t[]){ rnd_value() }, 4); }
        else { uint16_t v = (uint16_t)rnd_value(); memcpy(f->bytes + 1, &v, 2); f->len = 3; }
        f->what = "alu acc,imm";
        break;
    }
    case 2: {                                     /* group 1: op r/m, imm */
        int aluop = (int)rnd_below(8);
        int form = size8 ? 0 : (int)rnd_below(2);  /* 81 or 83 */
        f->bytes[0] = size8 ? 0x80 : (form ? 0x83 : 0x81);
        f->bytes[1] = modrm_rr(aluop, rm);
        if (f->bytes[0] == 0x80 || f->bytes[0] == 0x83) {
            f->bytes[2] = (uint8_t)rnd();
            f->len = 3;
        } else if (f->want32) {
            uint32_t v = rnd_value();
            memcpy(f->bytes + 2, &v, 4);
            f->len = 6;
        } else {
            uint16_t v = (uint16_t)rnd_value();
            memcpy(f->bytes + 2, &v, 2);
            f->len = 4;
        }
        f->what = "alu r/m,imm";
        break;
    }
    case 3:                                       /* TEST r/m, r */
        f->bytes[0] = size8 ? 0x84 : 0x85;
        f->bytes[1] = modrm_rr(reg, rm);
        f->len = 2;
        f->what = "test";
        break;
    case 4:                                       /* NOT / NEG */
        f->bytes[0] = size8 ? 0xF6 : 0xF7;
        f->bytes[1] = modrm_rr(2 + (int)rnd_below(2), rm);
        f->len = 2;
        f->what = "not/neg";
        break;
    case 5: {                                     /* INC / DEC */
        int dec = (int)rnd_below(2);
        if (size8) {
            f->bytes[0] = 0xFE;
            f->bytes[1] = modrm_rr(dec, rm);
            f->len = 2;
        } else if (rnd_below(2)) {
            /* The one-byte 8086 form, which is what a 16-bit compiler emits and
               so the one that matters.  Those same bytes are the REX prefixes in
               64-bit mode, so there the oracle is handed the group-5 encoding of
               the identical operation instead. */
            f->bytes[0] = (uint8_t)((dec ? 0x48 : 0x40) + rm);
            f->len = 1;
            if (long_mode) {
                f->alt[0] = 0xFF;
                f->alt[1] = modrm_rr(dec, rm);
                f->alt_len = 2;
            }
        } else {
            f->bytes[0] = 0xFF;                   /* group 5, the long form */
            f->bytes[1] = modrm_rr(dec, rm);
            f->len = 2;
        }
        f->what = "inc/dec";
        break;
    }
    case 6: {                                     /* MUL / IMUL */
        f->bytes[0] = size8 ? 0xF6 : 0xF7;
        f->bytes[1] = modrm_rr(4 + (int)rnd_below(2), rm);
        f->len = 2;
        /* Only CF and OF are defined; SF/ZF/AF/PF are not. */
        f->mask = F_CF | F_OF | F_DF;
        f->what = "mul/imul";
        break;
    }
    case 7: {                                     /* shifts and rotates */
        int sub = (int)rnd_below(8);
        int by = (int)rnd_below(3);
        unsigned bits = size8 ? 8u : (f->want32 ? 32u : 16u);
        if (sub == 6) sub = 4;                    /* 6 is an alias for SHL */
        f->bytes[0] = (uint8_t)((by == 0 ? 0xC0 : by == 1 ? 0xD0 : 0xD2) +
                                (size8 ? 0 : 1));
        f->bytes[1] = modrm_rr(sub, rm);
        if (by == 0) { f->bytes[2] = (uint8_t)rnd_below(bits); f->len = 3; }
        else f->len = 2;
        if (by == 2) f->cl_mod = bits;
        /* OF is only defined for a one-bit shift, AF never; rotates leave
           SF/ZF/PF alone, which the comparison covers since we require them to
           be preserved. */
        f->mask = F_CF | F_DF;
        if (sub >= 4) f->mask |= F_SF | F_ZF | F_PF;
        if (by == 1) f->mask |= F_OF;
        f->what = "shift/rotate";
        break;
    }
    case 8: {                                     /* MOVZX / MOVSX */
        int wide = (int)rnd_below(2);              /* 16-bit source, not 8-bit */
        f->bytes[0] = 0x0F;
        f->bytes[1] = (uint8_t)((rnd_below(2) ? 0xB6 : 0xBE) + wide);
        /* An 8-bit source may be any of the eight registers (index 4 is AH), but
           a 16-bit source must not be SP: the host trampoline runs on the real
           stack, so its SP would never match the guest's. */
        f->bytes[2] = modrm_rr(rnd_reg(), wide ? rnd_reg() : (int)rnd_below(8));
        f->len = 3;
        f->want32 = (int)rnd_below(2);
        f->what = "movzx/movsx";
        break;
    }
    case 9:                                       /* SETcc */
        f->bytes[0] = 0x0F;
        f->bytes[1] = (uint8_t)(0x90 + rnd_below(16));
        f->bytes[2] = modrm_rr(0, (int)rnd_below(8));
        f->len = 3;
        f->want32 = 0;
        f->what = "setcc";
        break;
    case 10:                                      /* IMUL r, r/m */
        f->bytes[0] = 0x0F;
        f->bytes[1] = 0xAF;
        f->bytes[2] = modrm_rr(rnd_reg(), rnd_reg());
        f->len = 3;
        f->want32 = (int)rnd_below(2);
        f->mask = F_CF | F_OF | F_DF;
        f->what = "imul r,r/m";
        break;
    case 11: {                                    /* IMUL r, r/m, imm */
        int b8 = (int)rnd_below(2);
        f->bytes[0] = (uint8_t)(b8 ? 0x6B : 0x69);
        f->bytes[1] = modrm_rr(rnd_reg(), rnd_reg());
        if (b8) { f->bytes[2] = (uint8_t)rnd(); f->len = 3; }
        else if (f->want32) { uint32_t v = rnd_value(); memcpy(f->bytes + 2, &v, 4); f->len = 6; }
        else { uint16_t v = (uint16_t)rnd_value(); memcpy(f->bytes + 2, &v, 2); f->len = 4; }
        f->mask = F_CF | F_OF | F_DF;
        f->what = "imul r,r/m,imm";
        break;
    }
    case 12: {                                    /* SHLD / SHRD */
        int by_cl = (int)rnd_below(2);
        unsigned bits = f->want32 ? 32u : 16u;
        f->bytes[0] = 0x0F;
        f->bytes[1] = (uint8_t)((rnd_below(2) ? 0xA4 : 0xAC) + (by_cl ? 1 : 0));
        f->bytes[2] = modrm_rr(rnd_reg(), rnd_reg());
        if (by_cl) { f->len = 3; f->cl_mod = bits; }
        else { f->bytes[3] = (uint8_t)rnd_below(bits); f->len = 4; }
        f->mask = F_CF | F_SF | F_ZF | F_PF | F_DF;
        f->what = "shld/shrd";
        break;
    }
    case 13: {                                    /* BT / BTS / BTR / BTC */
        int imm = (int)rnd_below(2);
        f->bytes[0] = 0x0F;
        if (imm) {
            f->bytes[1] = 0xBA;
            f->bytes[2] = modrm_rr(4 + (int)rnd_below(4), rnd_reg());
            f->bytes[3] = (uint8_t)rnd_below(f->want32 ? 32u : 16u);
            f->len = 4;
        } else {
            static const uint8_t o[4] = { 0xA3, 0xAB, 0xB3, 0xBB };
            f->bytes[1] = o[rnd_below(4)];
            f->bytes[2] = modrm_rr(rnd_reg(), rnd_reg());
            f->len = 3;
        }
        f->mask = F_CF | F_DF;
        f->what = "bt/bts/btr/btc";
        break;
    }
    case 14:                                      /* BSF / BSR */
        f->bytes[0] = 0x0F;
        f->bytes[1] = (uint8_t)(0xBC + rnd_below(2));
        f->bytes[2] = modrm_rr(rnd_reg(), rnd_reg());
        f->len = 3;
        f->mask = F_ZF | F_DF;
        f->what = "bsf/bsr";
        break;
    case 15:                                      /* XCHG */
        if (size8) {
            f->bytes[0] = 0x86;
            f->bytes[1] = modrm_rr(reg, rm);
            f->len = 2;
        } else if (rnd_below(2)) {
            f->bytes[0] = 0x87;
            f->bytes[1] = modrm_rr(reg, rm);
            f->len = 2;
        } else {
            f->bytes[0] = (uint8_t)(0x90 + rm);
            f->len = 1;
            if (rm == 0) f->bytes[0] = 0x90;
        }
        f->what = "xchg";
        break;
    case 16: {                                    /* sign and flag helpers */
        static const uint8_t o[] = { 0x98, 0x99, 0x9E, 0x9F,
                                     0xF5, 0xF8, 0xF9, 0xFC, 0xFD };
        f->bytes[0] = o[rnd_below(sizeof o)];
        f->len = 1;
        if (!lahf_ok && (f->bytes[0] == 0x9E || f->bytes[0] == 0x9F))
            f->why_not = "lahf/sahf: absent on this CPU in 64-bit mode";
        f->what = "cbw/cwd/sahf/lahf/flags";
        break;
    }
    default: {                                    /* decimal adjust */
        static const uint8_t o[] = { 0x27, 0x2F, 0x37, 0x3F, 0xD4, 0xD5 };
        f->bytes[0] = o[rnd_below(sizeof o)];
        f->len = 1;
        if (f->bytes[0] >= 0xD4) {
            f->bytes[1] = (uint8_t)(1 + rnd_below(255));
            f->len = 2;
        }
        f->want32 = 0;
        /* All six were removed in 64-bit mode, so there is no oracle for them
           on an x64 host - they decode as #UD, which would end the process
           rather than report a mismatch. */
        if (long_mode)
            f->why_not = "daa/das/aaa/aas/aam/aad: removed in 64-bit mode";
        /* Per the manual: DAA/DAS define CF, AF, SF, ZF and PF; AAA/AAS define
           only CF and AF; AAM/AAD define only SF, ZF and PF.  OF is undefined
           for all six. */
        if (f->bytes[0] == 0x27 || f->bytes[0] == 0x2F)
            f->mask = F_CF | F_AF | F_SF | F_ZF | F_PF | F_DF;
        else if (f->bytes[0] == 0x37 || f->bytes[0] == 0x3F)
            f->mask = F_CF | F_AF | F_DF;
        else
            f->mask = F_SF | F_ZF | F_PF | F_DF;
        f->what = "daa/das/aaa/aas/aam/aad";
        break;
    }
    }
}

/* Would this instruction divide by zero or overflow?  Skip those: the host
   would raise an exception where the interpreter reports a fault. */
static int is_divide(const struct form *f)
{
    if (f->len >= 2 && (f->bytes[0] == 0xF6 || f->bytes[0] == 0xF7)) {
        int sub = (f->bytes[1] >> 3) & 7;
        return sub == 6 || sub == 7;
    }
    return 0;
}

/* ------------------------------------------------------------------ the test */

static const char *fname(uint32_t f)
{
    static char b[64];
    snprintf(b, sizeof b, "%s%s%s%s%s%s%s",
             (f & F_CF) ? "cf " : "", (f & F_PF) ? "pf " : "",
             (f & F_AF) ? "af " : "", (f & F_ZF) ? "zf " : "",
             (f & F_SF) ? "sf " : "", (f & F_OF) ? "of " : "",
             (f & F_DF) ? "df " : "");
    return b;
}

/* Coverage, so a run that silently stopped generating something shows up, and
   the same table shape for what this host could not be asked to run.  Bucketing
   is by pointer: every key is a string literal from gen(), so identity is the
   cheap and exact test. */
#define MAX_FORMS 24
static long form_count[MAX_FORMS];
static const char *form_name[MAX_FORMS];
static long skip_count[MAX_FORMS];
static const char *skip_name[MAX_FORMS];

static void tally(const char **names, long *counts, const char *key)
{
    int s;
    for (s = 0; s < MAX_FORMS; s++) {
        if (!names[s]) names[s] = key;
        if (names[s] == key) { counts[s]++; return; }
    }
}

int fuzz_main(long rounds, unsigned seed)
{
    uint16_t code_sel;
    Cpu *c = &cpu;
    long i, tested = 0, skipped = 0, unrunnable = 0, failed = 0;

    if (long_mode) {
        unsigned a, b, cx, d;
        lahf_ok = __get_cpuid(0x80000001u, &a, &b, &cx, &d) && (cx & 1);
    }

    if (seed) rng_state = seed;
    if (!tramp_build(16)) return 1;
    code_sel = sel_alloc(0x10000u, SK_CODE);
    if (!code_sel) { log_msg("fuzz: no code selector\n"); return 1; }

    log_msg("fuzz: %ld rounds, seed %08X\n", rounds, rng_state);

    for (i = 0; i < rounds; i++) {
        struct form f;
        uint8_t guest[12], host[12];
        unsigned gl = 0, hl = 0, k;
        uint32_t seed_here = rng_state;
        uint32_t gflags, hflags, diff;
        int bad = 0;

        gen(&f);
        if (is_divide(&f)) { skipped++; continue; }
        if (f.why_not) {
            tally(skip_name, skip_count, f.why_not);
            unrunnable++;
            continue;
        }
        tally(form_name, form_count, f.what);

        /* Same core bytes; the guest is USE16 and the host USE32, which is also
           what 64-bit mode defaults to, so exactly one of them needs the
           operand-size prefix.  Where this host spells the instruction
           differently the oracle runs f.alt instead: the same operation on the
           same operands, so it still says what the guest ought to have done. */
        {
            const uint8_t *hb = f.alt_len ? f.alt : f.bytes;
            unsigned hn = f.alt_len ? f.alt_len : f.len;
            if (f.want32) guest[gl++] = 0x66;
            else          host[hl++]  = 0x66;
            for (k = 0; k < f.len; k++) guest[gl++] = f.bytes[k];
            for (k = 0; k < hn;    k++) host[hl++]  = hb[k];
        }

        /* Random input state.  TF clear, IF set, and only the flags we compare
           are seeded so a mismatch is never about a bit we do not model. */
        for (k = 0; k < 8; k++) fz.in_r[k] = rnd_value();
        fz.in_r[4] = 0;                            /* the stack is the host's */
        if (f.cl_mod)
            fz.in_r[1] = (fz.in_r[1] & ~0xFFu) | rnd_below(f.cl_mod);
        fz.in_flags = (rnd() & CMP_FLAGS) | F_RS | F_IF;

        /* Run natively. */
        memset(fz.out_r, 0, sizeof fz.out_r);
        fz.out_flags = 0;
        tramp_run(host, hl);
        hflags = (uint32_t)fz.out_flags;

        /* Run in the interpreter. */
        cpu_reset(c);
        for (k = 0; k < 8; k++) c->r32[k] = fz.in_r[k];
        c->eflags = (uint32_t)fz.in_flags;
        c->seg[S_CS] = code_sel;
        c->seg[S_DS] = code_sel;
        c->seg[S_ES] = code_sel;
        c->seg[S_SS] = code_sel;
        c->eip = 0;
        for (k = 0; k < gl; k++) sel_wr8(code_sel, (uint16_t)k, guest[k]);
        sel_wr8(code_sel, (uint16_t)gl, 0xF4);     /* HLT, so a length error shows */
        c->state = CPU_RUNNING;
        cpu_step(c);
        gflags = c->eflags;

        if (c->state == CPU_BADOP) {
            log_msg("fuzz: interpreter rejected %s (seed %08X):", f.what, seed_here);
            for (k = 0; k < gl; k++) log_msg(" %02X", guest[k]);
            log_msg("\n");
            failed++;
            continue;
        }

        /* Instruction length must agree, or the trace would desync. */
        if ((uint16_t)c->eip != gl) {
            log_msg("fuzz: length mismatch on %s (seed %08X): consumed %u of %u\n",
                    f.what, seed_here, (unsigned)(uint16_t)c->eip, gl);
            bad = 1;
        }

        for (k = 0; k < 8; k++) {
            uint32_t want = fz.out_r[k], got = c->r32[k];
            if (k == 4) continue;                  /* esp is not compared */
            /* In 16-bit mode the upper half of a 32-bit register is untouched by
               a 16-bit operation, and the host preserves it identically, so a
               full 32-bit comparison is correct. */
            if (want != got) {
                static const char *rn[8] = { "eax","ecx","edx","ebx","esp",
                                             "ebp","esi","edi" };
                log_msg("fuzz: %s %s: host %08X, emu %08X\n",
                        f.what, rn[k], want, got);
                bad = 1;
            }
        }
        diff = (hflags ^ gflags) & f.mask;
        if (diff) {
            log_msg("fuzz: %s flags differ (%s): host %04X emu %04X\n",
                    f.what, fname(diff), hflags & CMP_FLAGS, gflags & CMP_FLAGS);
            bad = 1;
        }
        if (bad) {
            log_msg("       seed %08X bytes:", seed_here);
            for (k = 0; k < gl; k++) log_msg(" %02X", guest[k]);
            if (f.alt_len) {                       /* the oracle ran other bytes */
                log_msg("  oracle:");
                for (k = 0; k < hl; k++) log_msg(" %02X", host[k]);
            }
            log_msg("  in: ");
            for (k = 0; k < 8; k++) log_msg("%08X ", fz.in_r[k]);
            log_msg("fl=%04X\n", (uint32_t)fz.in_flags & CMP_FLAGS);
            failed++;
            if (failed >= 25) {
                log_msg("fuzz: stopping after 25 failures\n");
                break;
            }
        }
        tested++;
    }

    {
        int s;
        log_msg("fuzz: coverage by form\n");
        for (s = 0; s < MAX_FORMS && form_name[s]; s++)
            log_msg("  %-26s %ld\n", form_name[s], form_count[s]);
        if (skip_name[0]) {
            log_msg("fuzz: no oracle on this host, so untested\n");
            for (s = 0; s < MAX_FORMS && skip_name[s]; s++)
                log_msg("  %-44s %ld\n", skip_name[s], skip_count[s]);
        }
    }
    log_msg("fuzz: %ld tested, %ld divides skipped, %ld unrunnable, %ld failed\n",
            tested, skipped, unrunnable, failed);
    return failed != 0;
}
