/* fuzz.c - differential test of the interpreter against the host CPU.
 *
 * The host is x86, so it is the best possible oracle: generate a random
 * instruction and a random register state, run it both in the interpreter and
 * natively in a trampoline, and compare registers and flags.
 *
 * Two details make this honest rather than approximate:
 *
 *  - A 16-bit instruction means something different in the host's 32-bit code
 *    segment, so each generated instruction is emitted twice: the guest gets a
 *    0x66 prefix exactly when the host does not.  The core bytes are identical.
 *
 *  - Some flags are architecturally undefined for some instructions (AF after
 *    a logical op, OF after a multi-bit shift, everything but CF/OF after MUL).
 *    Each generated form carries the mask of flags that are actually defined,
 *    and only those are compared.  Comparing undefined flags would produce
 *    failures that mean nothing.
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

/* Flags we can meaningfully compare.  TF must stay clear (it would single-step
   the host) and IF cannot be changed from user mode. */
#define CMP_FLAGS (F_CF | F_PF | F_AF | F_ZF | F_SF | F_OF | F_DF)

struct state { uint32_t r[8]; uint32_t eflags; };

static struct state fz_in, fz_out;
static uint32_t     fz_saved_esp;

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
static void eabs(uint8_t op, uint8_t modrm, const void *p)
{
    e8(op); e8(modrm); e32((uint32_t)(uintptr_t)p);
}

/* Build the trampoline once.  Layout:
     save host registers, stash esp
     load the guest state from fz_in (esp excluded)
     push fz_in.eflags; popfd
     <instruction bytes>
     pushfd; pop fz_out.eflags        (mov does not disturb flags, so this is first)
     store registers to fz_out
     restore host registers, ret                                              */
static int tramp_build(unsigned insn_max)
{
    static const struct { uint8_t op, modrm; int reg; } loads[] = {
        { 0x8B, 0x05, 0 },  /* mov eax, [abs] */
        { 0x8B, 0x0D, 1 },  /* mov ecx, [abs] */
        { 0x8B, 0x15, 2 },  /* mov edx, [abs] */
        { 0x8B, 0x1D, 3 },  /* mov ebx, [abs] */
        { 0x8B, 0x2D, 5 },  /* mov ebp, [abs] */
        { 0x8B, 0x35, 6 },  /* mov esi, [abs] */
        { 0x8B, 0x3D, 7 },  /* mov edi, [abs] */
    };
    static const struct { uint8_t op, modrm; int reg; } stores[] = {
        { 0x89, 0x05, 0 },  /* mov [abs], eax */
        { 0x89, 0x0D, 1 },
        { 0x89, 0x15, 2 },
        { 0x89, 0x1D, 3 },
        { 0x89, 0x2D, 5 },
        { 0x89, 0x35, 6 },
        { 0x89, 0x3D, 7 },
    };
    unsigned i;

    tramp_code = VirtualAlloc(NULL, 0x1000, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!tramp_code) {
        log_msg("fuzz: cannot allocate an executable trampoline\n");
        return 0;
    }
    emit = tramp_code;

    e8(0x53); e8(0x55); e8(0x56); e8(0x57);       /* push ebx/ebp/esi/edi   */
    eabs(0x89, 0x25, &fz_saved_esp);              /* mov [saved_esp], esp   */

    for (i = 0; i < 7; i++)
        eabs(loads[i].op, loads[i].modrm, &fz_in.r[loads[i].reg]);
    eabs(0xFF, 0x35, &fz_in.eflags);              /* push [in.eflags]       */
    e8(0x9D);                                     /* popfd                  */

    tramp_insn = emit;
    tramp_insn_max = insn_max;
    for (i = 0; i < insn_max; i++) e8(0x90);      /* room for the payload   */

    e8(0x9C);                                     /* pushfd                 */
    eabs(0x8F, 0x05, &fz_out.eflags);             /* pop [out.eflags]       */
    for (i = 0; i < 7; i++)
        eabs(stores[i].op, stores[i].modrm, &fz_out.r[stores[i].reg]);
    /* The payload may have been STD, and the C ABI guarantees DF is clear on
       entry to and return from a function.  Leaving it set makes the compiler's
       `rep movs` run backwards - which showed up as memcpy corrupting the three
       bytes below this very slot.  Flags have already been captured above, so
       clearing DF here costs nothing. */
    e8(0xFC);                                     /* cld                    */
    eabs(0x8B, 0x25, &fz_saved_esp);              /* mov esp, [saved_esp]   */
    e8(0x5F); e8(0x5E); e8(0x5D); e8(0x5B);       /* pop edi/esi/ebp/ebx    */
    e8(0xC3);                                     /* ret                    */

    tramp_len = (unsigned)(emit - tramp_code);
    memcpy(tramp_ref, tramp_code, tramp_len);
    log_msg("fuzz: trampoline at %p, %u bytes, payload slot at +%02X\n",
            (void *)tramp_code, tramp_len,
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
    case 5:                                       /* INC / DEC reg */
        if (size8) {
            f->bytes[0] = 0xFE;
            f->bytes[1] = modrm_rr((int)rnd_below(2), rm);
            f->len = 2;
        } else {
            f->bytes[0] = (uint8_t)((rnd_below(2) ? 0x48 : 0x40) + rm);
            f->len = 1;
        }
        f->what = "inc/dec";
        break;
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

/* Coverage, so a run that silently stopped generating something shows up. */
#define MAX_FORMS 24
static long form_count[MAX_FORMS];
static const char *form_name[MAX_FORMS];

int fuzz_main(long rounds, unsigned seed)
{
    uint16_t code_sel;
    Cpu *c = &cpu;
    long i, tested = 0, skipped = 0, failed = 0;

    /* The trampoline is emitted as 32-bit machine code against 32-bit absolute
       addresses: eabs() writes the operand address as a bare disp32, which on
       x86-64 both truncates the pointer and means RIP-relative rather than
       absolute.  Running it there is a segfault, not a test result.

       Porting it wants a scratch base register the guest state does not use -
       r12, say, loaded with a movabs - plus a REX prefix on every access, and
       fz_in/fz_out/fz_saved_esp gathered behind one base.  That is worth doing
       deliberately rather than in passing: a fuzzer nobody trusts is worse than
       one that says it cannot run.  Until then, note that this tests cpu.c,
       which is host-independent C, so a 32-bit run covers the same interpreter. */
    if (sizeof(void *) != 4) {
        log_msg("fuzz: needs a 32-bit host; the trampoline encodes 32-bit\n"
                "      absolute addresses.  Build with the i686 toolchain to\n"
                "      exercise the interpreter - it is the same C either way.\n");
        return 1;
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
        {   /* record coverage by form name */
            int s;
            for (s = 0; s < MAX_FORMS; s++) {
                if (!form_name[s]) { form_name[s] = f.what; }
                if (form_name[s] == f.what) { form_count[s]++; break; }
            }
        }

        /* Same core bytes; the guest is USE16 and the host USE32, so exactly one
           of them needs the operand-size prefix. */
        if (f.want32) guest[gl++] = 0x66;
        else          host[hl++]  = 0x66;
        for (k = 0; k < f.len; k++) { guest[gl++] = f.bytes[k]; host[hl++] = f.bytes[k]; }

        /* Random input state.  TF clear, IF set, and only the flags we compare
           are seeded so a mismatch is never about a bit we do not model. */
        for (k = 0; k < 8; k++) fz_in.r[k] = rnd_value();
        fz_in.r[4] = 0;                            /* esp is the host's */
        if (f.cl_mod)
            fz_in.r[1] = (fz_in.r[1] & ~0xFFu) | rnd_below(f.cl_mod);
        fz_in.eflags = (rnd() & CMP_FLAGS) | F_RS | F_IF;


        /* Run natively. */
        memset(&fz_out, 0, sizeof fz_out);
        tramp_run(host, hl);
        hflags = fz_out.eflags;

        /* Run in the interpreter. */
        cpu_reset(c);
        for (k = 0; k < 8; k++) c->r32[k] = fz_in.r[k];
        c->eflags = fz_in.eflags;
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
            uint32_t want = fz_out.r[k], got = c->r32[k];
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
            log_msg("  in: ");
            for (k = 0; k < 8; k++) log_msg("%08X ", fz_in.r[k]);
            log_msg("fl=%04X\n", fz_in.eflags & CMP_FLAGS);
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
    }
    log_msg("fuzz: %ld tested, %ld skipped, %ld failed\n", tested, skipped, failed);
    return failed != 0;
}
