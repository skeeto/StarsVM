/* fuzz.c - differential test of the interpreter against the host CPU.
 *
 * This is a program of its own, built by unity_fuzz.c and not part of the
 * emulator: the trampoline below asks for a writable-executable page, which is
 * not a thing a game should be seen doing, and none of this has anything to say
 * about Stars! anyway.  `make fuzz` builds and runs it.
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
 * Coverage is the ALU, shifts and rotates, inc/dec, mul/div, the bit
 * instructions, MOVZX/MOVSX, SETcc, SHLD/SHRD and the decimal adjusts, in
 * register and immediate forms - that is where flag bugs live - and the ALU
 * again against memory.  String operations and control transfer are not covered
 * here; they are exercised by running the game and by the targeted tests in
 * tools/.
 *
 * A memory operand is the one case where the two sides cannot run the same
 * bytes: a 16-bit ModRM means something else to a host whose address size is 32
 * or 64 bits.  So the guest gets a real addressing form - [bx+si+disp],
 * [bp+disp], a direct offset - and the fuzzer works out the effective address
 * *itself*, independently of decode_ea, which is exactly what makes it a test
 * of decode_ea; the oracle is then pointed at the mirror of that byte by
 * absolute address.  Two windows are mirrored rather than one, because DS and
 * SS have to be able to differ before a BP-relative form can be caught
 * choosing the wrong default segment.  Whatever either side wrote is compared
 * afterwards, so stores are covered as well as loads.
 *
 * The x87 register forms are covered too, and the oracle there is a different
 * shape.  fpu.c does not implement x87 arithmetic - it hands the operands to
 * the host's real FPU and stores the result back - so comparing results is
 * nearly tautological, and that is the good news: both sides are the same
 * silicon, so rounding, precision and the transcendentals agree by
 * construction, with no tolerance to invent.  What is genuinely under test is
 * everything around the arithmetic: operand order (fsub against fsubr is the
 * classic bug), the register stack and TOP, the tag word, the condition codes,
 * the control word reaching the host, and the decode of eight escape opcodes.
 * FRSTOR and FNSAVE in the trampoline move the whole x87 state at once, which
 * is the only way to set TOP and the tag word arbitrarily.
 *
 * Three things are deliberately outside that comparison, because fpu.c does not
 * model them and a failure would say nothing new:
 *
 *  - the exception flags, SF, ES and B.  Nothing issues fnclex on the host and
 *    fpu.c assigns the host status word rather than or-ing it, so those bits
 *    are the host's own accumulated noise.
 *  - the tag word beyond empty against live.  TAG_SPEC is never written, so
 *    NaNs, infinities and denormals are all tagged valid.
 *  - stack underflow.  fpu_discard never checks the tag, so popping an empty
 *    register silently rotates TOP; the generated state keeps four registers
 *    live and four empty so no single instruction can reach either end.
 *
 * Only encodings fpu.c implements are generated: an unimplemented one stops the
 * interpreter, which would be reported as a failure rather than as the gap it
 * is.  Absent, if ever wanted: FPREM1, FSINCOS, FUCOMPP, FSAVE/FRSTOR,
 * FBLD/FBSTP, FISTTP, DC /2 /3, DD /1 /6 /7, DE /2, and all of DF but E0.
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

/* An FNSAVE image, and where the parts we care about sit in it.  Registers are
   stored ST(0) first - top-relative, not physical - while the tag word is in
   physical order.  That asymmetry is the hardware's, and honouring it is what
   makes the TOP mapping testable rather than cancelled out. */
/* The mirrored window.  Wide enough for an 80-bit operand and some room to
   move it around in, small enough to compare in full every round. */
#define MEM_WIN    64
#define MEM_AT     0x200u          /* where it sits in each guest selector */

#define FPU_IMG    108
#define FPU_O_CW     0
#define FPU_O_SW     4
#define FPU_O_TW     8
#define FPU_O_ST    28

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
    /* FNSAVE/FRSTOR images.  108 bytes is the 32-bit protected-mode layout,
       which is what the trampoline runs in either mode: control word at 0,
       status at 4, tag at 8, then four words of instruction and operand
       pointers we do not model, then the eight registers at 28.  Only the
       named fields are ever compared. */
    uint8_t  in_fpu[FPU_IMG];
    uint8_t  out_fpu[FPU_IMG];
    /* The oracle's side of a memory operand.  [0] mirrors the guest's DS
       window and [1] its SS window; the payload is aimed at whichever the
       addressing form is supposed to pick. */
    uint8_t  mem[2][MEM_WIN];
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

/* One instruction against the mirrored window, built into a payload buffer
   rather than emitted into the trampoline.  A 32-bit host can name the address
   outright; a 64-bit host cannot, so it goes through R15 like the rest of the
   state block.  That needs REX.B, which the payload is otherwise free of - but
   the invariant that matters is that nothing in the payload *writes* R15, and
   naming it as a base does not. */
static unsigned host_mem_insn(uint8_t *out, const uint8_t *opb, unsigned opn,
                              int reg, unsigned which, unsigned off,
                              const uint8_t *imm, unsigned immn)
{
    size_t at = offsetof(struct fzstate, mem) + which * MEM_WIN + off;
    unsigned n = 0, i;

    if (long_mode) out[n++] = 0x41;               /* REX.B: the base is R15 */
    for (i = 0; i < opn; i++) out[n++] = opb[i];
    if (long_mode) out[n++] = (uint8_t)(0x80 | (reg << 3) | 7);  /* [r15+d32] */
    else           out[n++] = (uint8_t)(0x05 | (reg << 3));      /* [disp32]  */
    {
        uint32_t d = long_mode ? (uint32_t)at
                               : (uint32_t)(uintptr_t)&fz + (uint32_t)at;
        out[n++] = (uint8_t)d;
        out[n++] = (uint8_t)(d >> 8);
        out[n++] = (uint8_t)(d >> 16);
        out[n++] = (uint8_t)(d >> 24);
    }
    for (i = 0; i < immn; i++) out[n++] = imm[i];
    return n;
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
    /* The whole x87 state in one instruction.  Before the flags, because
       FRSTOR does not touch EFLAGS but the order reads better this way. */
    eref(0xDD, 4, offsetof(struct fzstate, in_fpu));      /* frstor [in_fpu] */
    eref(0xFF, 6, offsetof(struct fzstate, in_flags));   /* push [in_flags] */
    e8(0x9D);                                            /* popf            */

    tramp_insn = emit;
    tramp_insn_max = insn_max;
    for (i = 0; i < insn_max; i++) e8(0x90);      /* room for the payload   */

    e8(0x9C);                                            /* pushf           */
    eref(0x8F, 0, offsetof(struct fzstate, out_flags));  /* pop [out_flags] */
    /* FNSAVE also reinitialises the FPU, which is the tidy thing to leave
       behind: the emulator's own host x87 state is not ours to disturb. */
    eref(0xDD, 6, offsetof(struct fzstate, out_fpu));     /* fnsave [out_fpu] */
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

/* ---- x87 ----------------------------------------------------------------
 *
 * The generated state is described logically - ST(0) through ST(7) and a TOP -
 * and both sides are built from that one description.  The logical-to-physical
 * mapping is spelled out here rather than borrowed from fpu.c's phys(), so a
 * bug in phys() cannot cancel itself out by being used on both sides.
 *
 * Four registers are live and four are empty, whatever TOP is.  That is enough
 * for any single instruction to push once or pop twice without reaching an end
 * of the stack: overflow is only approximated by fpu.c and underflow is not
 * modelled at all, so neither belongs in a comparison yet.
 */
#define FZ_LIVE 4

struct fpstate {
    uint8_t  st[8][10];         /* ST(0) first, not physical */
    uint8_t  tag[8];            /* ditto, TAG_* values       */
    unsigned top;
    uint16_t cw;
};

/* An interesting 80-bit value.  Extremes and special encodings far more often
   than noise, for the same reason rnd_value prefers them. */
static void f80_make(uint8_t *b)
{
    static const struct { uint16_t se; uint64_t m; } pat[] = {
        { 0x0000, 0x0000000000000000ull },   /* +0            */
        { 0x8000, 0x0000000000000000ull },   /* -0            */
        { 0x3FFF, 0x8000000000000000ull },   /* +1            */
        { 0xBFFF, 0x8000000000000000ull },   /* -1            */
        { 0x4000, 0x8000000000000000ull },   /* +2            */
        { 0x3FFE, 0x8000000000000000ull },   /* +0.5          */
        { 0x4000, 0xC90FDAA22168C235ull },   /* pi            */
        { 0x7FFF, 0x8000000000000000ull },   /* +inf          */
        { 0xFFFF, 0x8000000000000000ull },   /* -inf          */
        { 0x7FFF, 0xC000000000000000ull },   /* QNaN          */
        { 0x7FFF, 0xA000000000000000ull },   /* SNaN          */
        { 0x0000, 0x0000000000000001ull },   /* denormal      */
        { 0x7FFE, 0xFFFFFFFFFFFFFFFFull },   /* max normal    */
        { 0x0001, 0x8000000000000000ull },   /* min normal    */
        { 0x4005, 0xFA00000000000000ull },   /* 125           */
        { 0x400C, 0x9C40000000000000ull },   /* 10000         */
    };
    uint64_t m;
    uint16_t se;
    int i;

    if (rnd_below(8) == 0) {                  /* sometimes just noise */
        se = (uint16_t)rnd();
        m  = ((uint64_t)rnd() << 32) | rnd();
    } else {
        unsigned k = rnd_below(sizeof pat / sizeof *pat);
        se = pat[k].se;
        m  = pat[k].m;
    }
    for (i = 0; i < 8; i++) b[i] = (uint8_t)(m >> (i * 8));
    b[8] = (uint8_t)se;
    b[9] = (uint8_t)(se >> 8);
}

static void fp_gen(struct fpstate *s)
{
    unsigned i;

    s->top = rnd_below(8);
    /* Rounding and precision vary; the six exception masks never come off.
       An unmasked control word does not fail a round - fpu.c hands it to the
       real host FPU, which then faults inside this process. */
    s->cw = (uint16_t)(0x007Fu | (rnd_below(4) << 8) | (rnd_below(4) << 10));
    for (i = 0; i < 8; i++) {
        if (i < FZ_LIVE) {
            f80_make(s->st[i]);
            s->tag[i] = 0;                    /* TAG_VALID; fpu.c retags */
        } else {
            memset(s->st[i], 0, 10);
            s->tag[i] = 3;                    /* TAG_EMPTY */
        }
    }
}

/* The FNSAVE image the oracle is seeded from. */
static void fp_to_image(const struct fpstate *s, uint8_t *img)
{
    unsigned i;

    memset(img, 0, FPU_IMG);
    img[FPU_O_CW] = (uint8_t)s->cw;
    img[FPU_O_CW + 1] = (uint8_t)(s->cw >> 8);
    img[FPU_O_SW] = 0;
    img[FPU_O_SW + 1] = (uint8_t)(s->top << 3);        /* TOP is bits 11-13 */
    {
        uint16_t tw = 0;
        for (i = 0; i < 8; i++)
            tw |= (uint16_t)((unsigned)s->tag[i] << (((s->top + i) & 7) * 2));
        img[FPU_O_TW] = (uint8_t)tw;
        img[FPU_O_TW + 1] = (uint8_t)(tw >> 8);
    }
    for (i = 0; i < 8; i++)
        memcpy(img + FPU_O_ST + i * 10, s->st[i], 10);
}

/* The same state, into the interpreter. */
static void fp_to_cpu(const struct fpstate *s, Cpu *c)
{
    unsigned i;

    c->fpu_cw = s->cw;
    c->fpu_sw = 0;
    c->fpu_top = (uint8_t)s->top;
    c->fpu_tw = 0;
    for (i = 0; i < 8; i++) {
        unsigned p = (s->top + i) & 7;
        memcpy(c->st[p].b, s->st[i], 10);
        c->fpu_tw = (uint16_t)(c->fpu_tw | ((unsigned)s->tag[i] << (p * 2)));
    }
}

/* The condition codes, which is what the guest branches on and what the host
   genuinely decides.  The exception flags are deliberately absent: nothing
   issues fnclex on the host, and fpu.c assigns the host status word rather than
   or-ing it, so those bits are the host's own accumulated noise rather than
   anything the guest computed. */
#define FP_CC 0x4700u

static uint16_t img16(const uint8_t *img, unsigned off)
{
    return (uint16_t)(img[off] | ((unsigned)img[off + 1] << 8));
}

/* Compare the oracle's FNSAVE image against the interpreter, both read as
   ST(0)-first so that TOP is part of what is being checked rather than part of
   how it is read. */
static int fp_compare(const uint8_t *img, Cpu *c, const char *what)
{
    unsigned htop = (img16(img, FPU_O_SW) >> 11) & 7;
    unsigned gtop = c->fpu_top;
    uint16_t htw = img16(img, FPU_O_TW), hcw = img16(img, FPU_O_CW);
    uint16_t hsw = img16(img, FPU_O_SW);
    uint16_t gsw = (uint16_t)((c->fpu_sw & ~0x3800u) | (gtop << 11));
    int bad = 0, i;

    if (htop != gtop) {
        log_msg("fuzz: %s top: host %u, emu %u\n", what, htop, gtop);
        bad = 1;
    }
    if (hcw != c->fpu_cw) {
        log_msg("fuzz: %s cw: host %04X, emu %04X\n", what, hcw, c->fpu_cw);
        bad = 1;
    }
    if ((hsw ^ gsw) & FP_CC) {
        log_msg("fuzz: %s condition codes: host %04X, emu %04X\n",
                what, hsw & FP_CC, gsw & FP_CC);
        bad = 1;
    }
    for (i = 0; i < 8; i++) {
        unsigned hp = (htop + (unsigned)i) & 7, gp = (gtop + (unsigned)i) & 7;
        int hempty = ((htw >> (hp * 2)) & 3) == 3;
        int gempty = ((c->fpu_tw >> (gp * 2)) & 3) == 3;
        /* Only empty against non-empty: fpu.c never writes TAG_SPEC, so the
           valid/zero/special distinction is not one it can be held to yet. */
        if (hempty != gempty) {
            log_msg("fuzz: %s st(%d) %s on the host, %s in the emu\n", what, i,
                    hempty ? "empty" : "live", gempty ? "empty" : "live");
            bad = 1;
            continue;
        }
        if (hempty) continue;
        if (memcmp(img + FPU_O_ST + (unsigned)i * 10, c->st[gp].b, 10)) {
            int k;
            log_msg("fuzz: %s st(%d): host", what, i);
            for (k = 9; k >= 0; k--) log_msg(" %02X", img[FPU_O_ST + i * 10 + k]);
            log_msg(", emu");
            for (k = 9; k >= 0; k--) log_msg(" %02X", c->st[gp].b[k]);
            log_msg("\n");
            bad = 1;
        }
    }
    return bad;
}

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
    /* A memory operand, when mem_len is nonzero: the guest bytes already carry
       its ModRM and displacement, and these say what the oracle should aim at.
       mem_seg is which window the addressing form ought to choose - 0 for DS,
       1 for SS - so picking the other one shows up as a difference. */
    unsigned mem_len;    /* bytes of the operand, 0 when there is none */
    unsigned mem_float;  /* 4 or 8 when the operand is converted from a float */
    unsigned mem_off;    /* where in the window it lands */
    unsigned mem_seg;
    uint8_t  mem_op[2];  /* opcode bytes for the oracle's encoding */
    unsigned mem_opn;
    int      mem_reg;    /* ModRM reg field for the oracle */
    uint8_t  mem_imm[4];
    unsigned mem_immn;
    int      fpu;        /* an escape opcode: set up and compare x87 state,
                            and emit no operand-size prefix on either side,
                            since D8-DF mean the same in every mode */
    const char *what;
};

/* A 16-bit addressing form aimed at the mirrored window: writes the ModRM byte
   and a 16-bit displacement, and records which segment the form is supposed to
   pick.  The displacement is whatever makes the effective address land in the
   window given the registers already drawn - working that out here, rather than
   asking decode_ea, is what makes this a test of decode_ea. */
static unsigned mem_modrm(struct form *f, int reg, unsigned esize, uint8_t *out)
{
    int rmf = (int)rnd_below(8);
    int mod = 2;                                   /* disp16 */
    unsigned off = rnd_below(MEM_WIN - esize + 1u);
    uint16_t base = 0;
    uint16_t bx = (uint16_t)fz.in_r[R_BX], bp = (uint16_t)fz.in_r[R_BP];
    uint16_t si = (uint16_t)fz.in_r[R_SI], di = (uint16_t)fz.in_r[R_DI];
    uint16_t disp;

    f->mem_seg = 0;                                /* DS unless BP is in it */
    switch (rmf) {
    case 0: base = (uint16_t)(bx + si); break;
    case 1: base = (uint16_t)(bx + di); break;
    case 2: base = (uint16_t)(bp + si); f->mem_seg = 1; break;
    case 3: base = (uint16_t)(bp + di); f->mem_seg = 1; break;
    case 4: base = si; break;
    case 5: base = di; break;
    case 6:
        /* mod 0 with rm 6 is the direct form, the one case here that does not
           mean BP - so it is DS, not SS. */
        if (rnd_below(2)) mod = 0;
        else { base = bp; f->mem_seg = 1; }
        break;
    default: base = bx; break;
    }

    disp = (uint16_t)(MEM_AT + off - base);
    out[0] = (uint8_t)((mod << 6) | (reg << 3) | rmf);
    out[1] = (uint8_t)disp;
    out[2] = (uint8_t)(disp >> 8);
    f->mem_off = off;
    f->mem_len = esize;
    f->mem_reg = reg;
    return 3;
}

/* Make a signalling NaN in a float memory operand quiet.
 *
 * This is the one operand shape where fpu.c cannot agree with the hardware, and
 * the reason is structural rather than a mistake.  Hardware loads the operand
 * and operates in a single instruction, so an SNaN in memory meeting a QNaN in
 * ST(0) takes the SNaN-versus-QNaN rule, where the QNaN wins.  fpu.c widens the
 * operand to 80 bits first, which quietens it, so by the time the division
 * happens both are quiet and the larger-significand rule picks the other one.
 *
 * It takes a NaN in ST(0) *and* a signalling NaN in memory to be observable -
 * against any ordinary value the two agree bit for bit, because quietening
 * early and quietening late produce the same bits - and nothing a compiler
 * emits produces either.  Fixing it would mean giving host_arith memory-operand
 * forms so the host does load-and-operate itself, which is a restructuring of
 * the path carrying about 5% of turn generation for a case that cannot arise.
 * So it is recorded here and kept out of the generated operands instead.
 */
static void mem_quieten(uint8_t *p, unsigned n)
{
    if (n == 4) {
        uint32_t v = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                     ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        if ((v & 0x7F800000u) == 0x7F800000u && (v & 0x007FFFFFu))
            p[2] |= 0x40;                          /* the quiet bit */
    } else if (n == 8) {
        uint32_t hi = (uint32_t)p[4] | ((uint32_t)p[5] << 8) |
                      ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);
        uint32_t lo = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                      ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        if ((hi & 0x7FF00000u) == 0x7FF00000u && ((hi & 0x000FFFFFu) || lo))
            p[6] |= 0x08;
    }
}

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

    switch (rnd_below(21)) {
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
    case 18: {                                    /* ALU against memory */
        int aluop = (int)rnd_below(8);
        int to_reg = (int)rnd_below(2);
        /* The oracle needs REX.B to reach R15 in long mode, and any REX
           prefix turns the 8-bit registers 4-7 from AH/CH/DH/BH into
           SPL/BPL/SIL/DIL.  The guest means the former, so on a 64-bit host the
           8-bit register operand stays in AL/CL/DL/BL; the 32-bit build, which
           needs no REX, still covers the high-byte registers. */
        int mreg = size8 ? (int)rnd_below(long_mode ? 4 : 8) : rnd_reg();
        unsigned esize = size8 ? 1u : (f->want32 ? 4u : 2u);

        f->bytes[0] = (uint8_t)((aluop << 3) | (to_reg << 1) | (size8 ? 0 : 1));
        f->len = 1 + mem_modrm(f, mreg, esize, f->bytes + 1);
        f->mem_op[0] = f->bytes[0];
        f->mem_opn = 1;
        f->what = "alu r/m,r memory";
        break;
    }

    case 19: {                                    /* x87 against memory */
        /* Every memory escape fpu.c implements, less four that cannot be
           compared: FLDCW and FLDENV would take a control word out of random
           bytes and hand it to the real host FPU, where an unmasked exception
           faults inside this process rather than failing a round; FSTENV writes
           four words of instruction and operand pointers that fpu.c does not
           model; and FNSTSW m16 writes the exception flags, which are the
           host's own accumulated noise rather than anything the guest did. */
        /* The last column is the width the operand is converted *from* when it
           is a float, which is what mem_quieten needs; integer and 80-bit
           operands convert exactly and are left alone. */
        static const struct { uint8_t op, reg, size, flt; } mf[] = {
            { 0xD8, 0, 4, 4 }, { 0xD8, 1, 4, 4 }, { 0xD8, 2, 4, 4 },
            { 0xD8, 3, 4, 4 }, { 0xD8, 4, 4, 4 }, { 0xD8, 5, 4, 4 },
            { 0xD8, 6, 4, 4 }, { 0xD8, 7, 4, 4 },
            { 0xD9, 0, 4, 4 }, { 0xD9, 2, 4, 4 }, { 0xD9, 3, 4, 4 },
            { 0xD9, 7, 2, 0 },
            { 0xDA, 0, 4, 0 }, { 0xDA, 1, 4, 0 }, { 0xDA, 2, 4, 0 },
            { 0xDA, 3, 4, 0 }, { 0xDA, 4, 4, 0 }, { 0xDA, 5, 4, 0 },
            { 0xDA, 6, 4, 0 }, { 0xDA, 7, 4, 0 },
            { 0xDB, 0, 4, 0 }, { 0xDB, 2, 4, 0 }, { 0xDB, 3, 4, 0 },
            { 0xDB, 5,10, 0 }, { 0xDB, 7,10, 0 },
            { 0xDC, 0, 8, 8 }, { 0xDC, 1, 8, 8 }, { 0xDC, 2, 8, 8 },
            { 0xDC, 3, 8, 8 }, { 0xDC, 4, 8, 8 }, { 0xDC, 5, 8, 8 },
            { 0xDC, 6, 8, 8 }, { 0xDC, 7, 8, 8 },
            { 0xDD, 0, 8, 8 }, { 0xDD, 2, 8, 8 }, { 0xDD, 3, 8, 8 },
            { 0xDE, 0, 2, 0 }, { 0xDE, 1, 2, 0 }, { 0xDE, 2, 2, 0 },
            { 0xDE, 3, 2, 0 }, { 0xDE, 4, 2, 0 }, { 0xDE, 5, 2, 0 },
            { 0xDE, 6, 2, 0 }, { 0xDE, 7, 2, 0 },
            { 0xDF, 0, 2, 0 }, { 0xDF, 2, 2, 0 }, { 0xDF, 3, 2, 0 },
            { 0xDF, 5, 8, 0 }, { 0xDF, 7, 8, 0 },
        };
        unsigned k = rnd_below(sizeof mf / sizeof *mf);

        f->fpu = 1;
        f->want32 = 0;
        f->bytes[0] = mf[k].op;
        f->len = 1 + mem_modrm(f, mf[k].reg, mf[k].size, f->bytes + 1);
        f->mem_op[0] = mf[k].op;
        f->mem_opn = 1;
        f->mem_float = mf[k].flt;
        f->what = "x87 memory";
        break;
    }

    case 17: {                                    /* x87, register forms */
        /* Every register-form escape fpu.c implements, and only those: an
           encoding it does not implement stops the interpreter, which the round
           loop would report as a failure rather than as the gap it is.  The
           gaps are listed in the header comment.
           The second byte of a `withst` entry has the register number or-ed in;
           only the live ones, because reading an empty register is the
           unmodelled stack-underflow case where fpu.c returns the stored bytes
           and the hardware returns the indefinite QNaN. */
        static const uint8_t withst[][2] = {
            { 0xD8, 0xC0 }, { 0xD8, 0xC8 }, { 0xD8, 0xD0 }, { 0xD8, 0xD8 },
            { 0xD8, 0xE0 }, { 0xD8, 0xE8 }, { 0xD8, 0xF0 }, { 0xD8, 0xF8 },
            { 0xDC, 0xC0 }, { 0xDC, 0xC8 }, { 0xDC, 0xE0 }, { 0xDC, 0xE8 },
            { 0xDC, 0xF0 }, { 0xDC, 0xF8 },
            { 0xDE, 0xC0 }, { 0xDE, 0xC8 }, { 0xDE, 0xE0 }, { 0xDE, 0xE8 },
            { 0xDE, 0xF0 }, { 0xDE, 0xF8 },
            { 0xD9, 0xC0 }, { 0xD9, 0xC8 }, { 0xD9, 0xD8 },
            { 0xDD, 0xC0 }, { 0xDD, 0xD0 }, { 0xDD, 0xD8 },
            { 0xDD, 0xE0 }, { 0xDD, 0xE8 },
        };
        static const uint8_t fixed[][2] = {
            { 0xD9, 0xD0 },                                    /* FNOP      */
            { 0xD9, 0xE0 }, { 0xD9, 0xE1 },                    /* FCHS FABS */
            { 0xD9, 0xE4 }, { 0xD9, 0xE5 },                    /* FTST FXAM */
            { 0xD9, 0xE8 }, { 0xD9, 0xE9 }, { 0xD9, 0xEA },
            { 0xD9, 0xEB }, { 0xD9, 0xEC }, { 0xD9, 0xED },
            { 0xD9, 0xEE },                                    /* constants */
            { 0xD9, 0xF0 }, { 0xD9, 0xF1 }, { 0xD9, 0xF2 },
            { 0xD9, 0xF3 }, { 0xD9, 0xF4 },
            { 0xD9, 0xF6 }, { 0xD9, 0xF7 },                    /* FDEC/FINCSTP */
            { 0xD9, 0xF8 }, { 0xD9, 0xF9 }, { 0xD9, 0xFA },
            { 0xD9, 0xFC }, { 0xD9, 0xFD }, { 0xD9, 0xFE },
            { 0xD9, 0xFF },
            { 0xDE, 0xD9 },                                    /* FCOMPP    */
            { 0xDF, 0xE0 },                                    /* FNSTSW AX */
        };
        f->fpu = 1;
        f->want32 = 0;
        f->len = 2;
        if (rnd_below(2)) {
            unsigned k = rnd_below(sizeof withst / sizeof *withst);
            f->bytes[0] = withst[k][0];
            f->bytes[1] = (uint8_t)(withst[k][1] | rnd_below(FZ_LIVE));
            f->what = "x87 st(i)";
        } else {
            unsigned k = rnd_below(sizeof fixed / sizeof *fixed);
            f->bytes[0] = fixed[k][0];
            f->bytes[1] = fixed[k][1];
            f->what = "x87 unary/const";
        }
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
#define MAX_FORMS 40
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

static int fuzz_run(long rounds, unsigned seed)
{
    uint16_t code_sel, ds_sel, ss_sel;
    Cpu *c = &cpu;
    long i, tested = 0, skipped = 0, unrunnable = 0, failed = 0;

    if (long_mode) {
        unsigned a, b, cx, d;
        lahf_ok = __get_cpuid(0x80000001u, &a, &b, &cx, &d) && (cx & 1);
    }

    if (seed) rng_state = seed;
    if (!tramp_build(16)) return 1;
    code_sel = sel_alloc(0x10000u, SK_CODE);
    /* Two data selectors, not one: DS and SS have to be able to differ, or a
       BP-relative form choosing the wrong default segment would land on the
       right bytes anyway and the mistake would never show. */
    ds_sel = sel_alloc(0x10000u, SK_DATA);
    ss_sel = sel_alloc(0x10000u, SK_DATA);
    if (!code_sel || !ds_sel || !ss_sel) {
        log_msg("fuzz: no selector\n");
        return 1;
    }

    log_msg("fuzz: %ld rounds, seed %08X\n", rounds, rng_state);

    for (i = 0; i < rounds; i++) {
        struct form f;
        struct fpstate fs;
        uint8_t guest[16], host[16];
        unsigned gl = 0, hl = 0, k;
        uint32_t seed_here = rng_state;
        uint32_t gflags, hflags, diff;
        int bad = 0;

        /* Before gen(), because a memory form has to work out the
           displacement that lands the effective address in the window, and
           that depends on what the base and index registers hold. */
        for (k = 0; k < 8; k++) fz.in_r[k] = rnd_value();
        fz.in_r[4] = 0;                            /* the stack is the host's */

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
            /* D8-DF mean the same in every mode, so an escape needs no prefix
               on either side - and would be told nothing by one. */
            if (f.fpu)         { /* neither */ }
            else if (f.want32) guest[gl++] = 0x66;
            else               host[hl++]  = 0x66;
            for (k = 0; k < f.len; k++) guest[gl++] = f.bytes[k];
            if (f.mem_len)
                hl += host_mem_insn(host + hl, f.mem_op, f.mem_opn, f.mem_reg,
                                    f.mem_seg, f.mem_off, f.mem_imm, f.mem_immn);
            else
                for (k = 0; k < hn; k++) host[hl++] = hb[k];
        }

        /* TF clear, IF set, and only the flags we compare are seeded, so a
           mismatch is never about a bit we do not model. */
        if (f.cl_mod)
            fz.in_r[1] = (fz.in_r[1] & ~0xFFu) | rnd_below(f.cl_mod);
        fz.in_flags = (rnd() & CMP_FLAGS) | F_RS | F_IF;

        /* Run natively. */
        memset(fz.out_r, 0, sizeof fz.out_r);
        fz.out_flags = 0;
        if (f.fpu) { fp_gen(&fs); fp_to_image(&fs, fz.in_fpu); }
        memset(fz.out_fpu, 0, sizeof fz.out_fpu);
        /* Both windows get the same bytes on both sides, then each side works
           on its own copy: the oracle on fz.mem, the guest on its selectors.
           Whatever either wrote is compared afterwards. */
        if (f.mem_len) {
            for (k = 0; k < MEM_WIN; k++) {
                fz.mem[0][k] = (uint8_t)rnd();
                fz.mem[1][k] = (uint8_t)rnd();
            }
            if (f.mem_float)
                mem_quieten(fz.mem[f.mem_seg] + f.mem_off, f.mem_float);
            for (k = 0; k < MEM_WIN; k++) {
                sel_wr8(ds_sel, (uint16_t)(MEM_AT + k), fz.mem[0][k]);
                sel_wr8(ss_sel, (uint16_t)(MEM_AT + k), fz.mem[1][k]);
            }
        }
        tramp_run(host, hl);
        hflags = (uint32_t)fz.out_flags;

        /* Run in the interpreter. */
        cpu_reset(c);
        if (f.fpu) fp_to_cpu(&fs, c);
        for (k = 0; k < 8; k++) c->r32[k] = fz.in_r[k];
        c->eflags = (uint32_t)fz.in_flags;
        c->seg[S_CS] = code_sel;
        c->seg[S_DS] = ds_sel;
        c->seg[S_ES] = ds_sel;
        c->seg[S_SS] = ss_sel;
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
        if (f.fpu && fp_compare(fz.out_fpu, c, f.what)) bad = 1;
        if (f.mem_len) {
            static const char *wn[2] = { "ds", "ss" };
            unsigned w;
            for (w = 0; w < 2; w++) {
                uint16_t s = w ? ss_sel : ds_sel;
                for (k = 0; k < MEM_WIN; k++) {
                    uint8_t want = fz.mem[w][k];
                    uint8_t got = sel_rd8(s, (uint16_t)(MEM_AT + k));
                    if (want == got) continue;
                    log_msg("fuzz: %s %s window +%02X: host %02X, emu %02X\n",
                            f.what, wn[w], k, want, got);
                    bad = 1;
                    break;
                }
            }
        }
        diff = (hflags ^ gflags) & f.mask;
        if (diff) {
            log_msg("fuzz: %s flags differ (%s): host %04X emu %04X\n",
                    f.what, fname(diff), hflags & CMP_FLAGS, gflags & CMP_FLAGS);
            bad = 1;
        }
        if (bad) {
            if (f.mem_len) {
                log_msg("       operand %s+%02X:", f.mem_seg ? "ss" : "ds",
                        f.mem_off);
                for (k = 0; k < f.mem_len; k++)
                    log_msg(" %02X", fz.mem[f.mem_seg][f.mem_off + k]);
                log_msg("\n");
            }
            if (f.fpu) {
                log_msg("       st in:");
                for (k = 0; k < FZ_LIVE; k++) {
                    int q;
                    log_msg(" ");
                    for (q = 9; q >= 0; q--) log_msg("%02X", fs.st[k][q]);
                }
                log_msg(" top %u cw %04X\n", fs.top, fs.cw);
            }
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


/* ------------------------------------------------------------------ the driver */

/* Being a console program, the report needs no --console: GetStdHandle succeeds,
   so log_open finds stdout already there.  The name comes from argv[0] for the
   same reason the emulator's does - nothing in this tree writes its own name
   down. */
static const char *prog = "fuzz";

static const char driver_usage[] =
    "\n"
    "Generates a random instruction and a random register state, runs each both\n"
    "in the interpreter and natively on this CPU, and compares registers and\n"
    "flags.  A mismatch prints the seed that produced it, so it can be replayed\n"
    "on its own.\n"
    "\n"
    "  --rounds N   instructions to test (default 200000)\n"
    "  --seed N     start the generator here instead of at its fixed default,\n"
    "               which is what a seed printed by a failure is for\n"
    "  --log FILE   also write the report to FILE\n"
    "  --help       this text\n";

int main(int argc, char **argv)
{
    long rounds = 200000;
    unsigned seed = 0;
    const char *logfile = NULL;
    int i, rc;

    if (argv[0] && argv[0][0]) {
        const char *p;
        prog = argv[0];
        for (p = argv[0]; *p; p++)
            if (*p == '\\' || *p == '/' || *p == ':') prog = p + 1;
    }

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--rounds") && i + 1 < argc) {
            rounds = strtol(argv[++i], NULL, 0);
        } else if (!strcmp(a, "--seed") && i + 1 < argc) {
            seed = (unsigned)strtoul(argv[++i], NULL, 0);
        } else if (!strcmp(a, "--log") && i + 1 < argc) {
            logfile = argv[++i];
        } else if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            printf("usage: %s [--rounds N] [--seed N] [--log FILE]\n", prog);
            fputs(driver_usage, stdout);
            return 0;
        } else {
            fprintf(stderr, "%s: unknown option %s\n", prog, a);
            return 2;
        }
    }

    /* Both of these are things the emulator's main() happens to have done long
       before it reaches the fuzzer, so doing without them here is not an
       option: log_msg writes nowhere until log_open has gone looking for a
       handle, and sel_alloc has no arena to carve up until sel_init. */
    log_open(logfile);
    if (!sel_init()) { log_close(); return 1; }

    rc = fuzz_run(rounds, seed);
    log_close();
    return rc;
}
