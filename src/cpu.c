/* cpu.c - 16-bit x86 interpreter.
 *
 * Covers 8086/80186/80286 plus the 386 additions a 16-bit MS C compiler emits:
 * the 0x66/0x67 size prefixes, MOVZX/MOVSX, SETcc, IMUL r,r/m, SHLD/SHRD, the
 * bit instructions, and the near forms of Jcc.  x87 lives in fpu.c.
 *
 * Anything not understood stops the machine with CPU_BADOP.  That is deliberate:
 * an interpreter that silently mis-executes an unknown encoding makes every
 * later bug unfalsifiable.
 */

#include "cpu.h"
#include "sel.h"
#include "log.h"
#include "fpu.h"
#include "thunk.h"

#include <string.h>

Cpu cpu;

/* ------------------------------------------------------------------- helpers */

static const uint8_t parity8[256] = {
#define P2(n) n, n ^ F_PF, n ^ F_PF, n
#define P4(n) P2(n), P2(n ^ F_PF), P2(n ^ F_PF), P2(n)
#define P6(n) P4(n), P4(n ^ F_PF), P4(n ^ F_PF), P4(n)
    P6(F_PF), P6(0), P6(0), P6(F_PF)
#undef P6
#undef P4
#undef P2
};

static uint32_t mask_of(int size)
{
    return size == 1 ? 0xFFu : size == 2 ? 0xFFFFu : 0xFFFFFFFFu;
}

static uint32_t sign_of(int size)
{
    return size == 1 ? 0x80u : size == 2 ? 0x8000u : 0x80000000u;
}

static uint32_t sext(uint32_t v, int size)
{
    if (size == 1) return (uint32_t)(int32_t)(int8_t)v;
    if (size == 2) return (uint32_t)(int32_t)(int16_t)v;
    return v;
}

const char *cpu_state_name(int state)
{
    switch (state) {
    case CPU_RUNNING: return "running";
    case CPU_HALT:    return "halted";
    case CPU_RETURN:  return "returned to host";
    case CPU_BADOP:   return "undecodable instruction";
    case CPU_NOAPI:   return "unimplemented API";
    case CPU_FAULT:   return "fault";
    case CPU_STEPS:   return "step limit";
    default:          return "?";
    }
}

void cpu_reset(Cpu *c)
{
    memset(c, 0, sizeof *c);
    c->eflags = F_RS | F_IF;
    fpu_reset(c);
}

/* --------------------------------------------------------- register accessors */

static uint32_t get_reg(Cpu *c, int r, int size)
{
    if (size == 1)
        return (r < 4) ? (c->r32[r] & 0xFFu) : ((c->r32[r - 4] >> 8) & 0xFFu);
    if (size == 2) return c->r32[r] & 0xFFFFu;
    return c->r32[r];
}

static void set_reg(Cpu *c, int r, int size, uint32_t v)
{
    if (size == 1) {
        if (r < 4) c->r32[r] = (c->r32[r] & 0xFFFFFF00u) | (v & 0xFFu);
        else       c->r32[r - 4] = (c->r32[r - 4] & 0xFFFF00FFu) | ((v & 0xFFu) << 8);
    } else if (size == 2) {
        c->r32[r] = (c->r32[r] & 0xFFFF0000u) | (v & 0xFFFFu);
    } else {
        c->r32[r] = v;
    }
}

/* ---------------------------------------------------------------- code fetch */

static uint16_t ip_of(Cpu *c) { return (uint16_t)c->eip; }

static void ip_set(Cpu *c, uint16_t v) { c->eip = v; }

static uint8_t fetch8(Cpu *c)
{
    uint8_t v = sel_rd8(c->seg[S_CS], ip_of(c));
    c->eip = (uint16_t)(c->eip + 1);
    return v;
}

static uint16_t fetch16(Cpu *c)
{
    uint16_t v = sel_rd16(c->seg[S_CS], ip_of(c));
    c->eip = (uint16_t)(c->eip + 2);
    return v;
}

static uint32_t fetch32(Cpu *c)
{
    uint32_t v = sel_rd32(c->seg[S_CS], ip_of(c));
    c->eip = (uint16_t)(c->eip + 4);
    return v;
}

/* -------------------------------------------------------------------- stack */

void cpu_push16(Cpu *c, uint16_t v)
{
    uint16_t sp = (uint16_t)(reg16(c, R_SP) - 2);
    set_reg16(c, R_SP, sp);
    sel_wr16(c->seg[S_SS], sp, v);
}

uint16_t cpu_pop16(Cpu *c)
{
    uint16_t sp = reg16(c, R_SP);
    uint16_t v = sel_rd16(c->seg[S_SS], sp);
    set_reg16(c, R_SP, (uint16_t)(sp + 2));
    return v;
}

static void push32(Cpu *c, uint32_t v)
{
    uint16_t sp = (uint16_t)(reg16(c, R_SP) - 4);
    set_reg16(c, R_SP, sp);
    sel_wr32(c->seg[S_SS], sp, v);
}

static uint32_t pop32(Cpu *c)
{
    uint16_t sp = reg16(c, R_SP);
    uint32_t v = sel_rd32(c->seg[S_SS], sp);
    set_reg16(c, R_SP, (uint16_t)(sp + 4));
    return v;
}

static void push_size(Cpu *c, int size, uint32_t v)
{
    if (size == 4) push32(c, v); else cpu_push16(c, (uint16_t)v);
}

static uint32_t pop_size(Cpu *c, int size)
{
    return (size == 4) ? pop32(c) : cpu_pop16(c);
}

/* -------------------------------------------------------------------- flags */

static void set_zsp(Cpu *c, uint32_t res, int size)
{
    uint32_t m = mask_of(size);
    res &= m;
    c->eflags &= ~(F_ZF | F_SF | F_PF);
    if (res == 0)               c->eflags |= F_ZF;
    if (res & sign_of(size))    c->eflags |= F_SF;
    c->eflags |= parity8[res & 0xFF];
}

static void flags_add(Cpu *c, uint32_t a, uint32_t b, uint32_t carry,
                      uint32_t res, int size)
{
    uint32_t m = mask_of(size), s = sign_of(size);
    uint32_t r = res & m;

    c->eflags &= ~(F_CF | F_OF | F_AF);
    /* Carry out of the top bit, computed without relying on wider arithmetic. */
    if (r < (a & m) || (carry && r == (a & m))) c->eflags |= F_CF;
    if (((a ^ ~b) & (a ^ r) & s) != 0)          c->eflags |= F_OF;
    if (((a ^ b ^ r) & 0x10u) != 0)             c->eflags |= F_AF;
    set_zsp(c, r, size);
}

static void flags_sub(Cpu *c, uint32_t a, uint32_t b, uint32_t borrow,
                      uint32_t res, int size)
{
    uint32_t m = mask_of(size), s = sign_of(size);
    uint32_t r = res & m;

    c->eflags &= ~(F_CF | F_OF | F_AF);
    if ((a & m) < (b & m) || (borrow && (a & m) == (b & m))) c->eflags |= F_CF;
    if (((a ^ b) & (a ^ r) & s) != 0)                        c->eflags |= F_OF;
    if (((a ^ b ^ r) & 0x10u) != 0)                          c->eflags |= F_AF;
    set_zsp(c, r, size);
}

static void flags_logic(Cpu *c, uint32_t res, int size)
{
    c->eflags &= ~(F_CF | F_OF | F_AF);
    set_zsp(c, res, size);
}

/* --------------------------------------------------------------- ModRM decode */

typedef struct {
    int      is_reg;    /* operand is a register, not memory */
    int      reg;       /* the reg field of the ModRM byte   */
    int      rm;        /* register number when is_reg       */
    uint16_t sel;       /* segment selector when memory      */
    uint16_t off;       /* offset when memory                */
} Ea;

/* A 26/2E/36/3E/64/65 prefix selects the segment for the next memory operand.
   It lives in the Cpu rather than a file static so that nested interpreter runs
   (a guest callback reached through an API handler) cannot disturb each other. */
static uint16_t seg_for(Cpu *c, int dflt)
{
    return c->seg[c->seg_override >= 0 ? c->seg_override : dflt];
}

static void decode_ea(Cpu *c, uint8_t modrm, int addr32, Ea *ea)
{
    int mod = modrm >> 6, rm = modrm & 7;

    ea->reg = (modrm >> 3) & 7;
    if (mod == 3) {
        ea->is_reg = 1;
        ea->rm = rm;
        return;
    }
    ea->is_reg = 0;

    if (addr32) {
        /* 32-bit addressing.  Compilers of this era emit it only rarely, but
           the 0x67 prefix does appear, so decode it properly. */
        uint32_t base = 0, index = 0;
        int scale = 0, dflt = S_DS, have_base = 1;

        if (rm == 4) {                       /* SIB */
            uint8_t sib = fetch8(c);
            int b = sib & 7, x = (sib >> 3) & 7;
            scale = (sib >> 6) & 3;
            if (x != 4) index = c->r32[x] << scale;
            if (b == 5 && mod == 0) { base = fetch32(c); have_base = 0; }
            else { base = c->r32[b]; if (b == 4 || b == 5) dflt = S_SS; }
        } else if (rm == 5 && mod == 0) {
            base = fetch32(c);
            have_base = 0;
        } else {
            base = c->r32[rm];
            if (rm == 5) dflt = S_SS;
        }
        if (mod == 1) base += sext(fetch8(c), 1);
        else if (mod == 2) base += fetch32(c);
        (void)have_base;
        ea->off = (uint16_t)(base + index);
        ea->sel = seg_for(c, dflt);
        return;
    }

    {   /* 16-bit addressing */
        uint16_t off = 0;
        int dflt = S_DS;

        switch (rm) {
        case 0: off = (uint16_t)(reg16(c, R_BX) + reg16(c, R_SI)); break;
        case 1: off = (uint16_t)(reg16(c, R_BX) + reg16(c, R_DI)); break;
        case 2: off = (uint16_t)(reg16(c, R_BP) + reg16(c, R_SI)); dflt = S_SS; break;
        case 3: off = (uint16_t)(reg16(c, R_BP) + reg16(c, R_DI)); dflt = S_SS; break;
        case 4: off = reg16(c, R_SI); break;
        case 5: off = reg16(c, R_DI); break;
        case 6:
            if (mod == 0) off = fetch16(c);
            else { off = reg16(c, R_BP); dflt = S_SS; }
            break;
        case 7: off = reg16(c, R_BX); break;
        }
        if (mod == 1) off = (uint16_t)(off + (int16_t)(int8_t)fetch8(c));
        else if (mod == 2) off = (uint16_t)(off + fetch16(c));
        ea->off = off;
        ea->sel = seg_for(c, dflt);
    }
}

static uint32_t ea_read(Cpu *c, const Ea *ea, int size)
{
    if (ea->is_reg) return get_reg(c, ea->rm, size);
    if (size == 1) return sel_rd8(ea->sel, ea->off);
    if (size == 2) return sel_rd16(ea->sel, ea->off);
    return sel_rd32(ea->sel, ea->off);
}

static void ea_write(Cpu *c, const Ea *ea, int size, uint32_t v)
{
    if (ea->is_reg) { set_reg(c, ea->rm, size, v); return; }
    if (size == 1) sel_wr8(ea->sel, ea->off, (uint8_t)v);
    else if (size == 2) sel_wr16(ea->sel, ea->off, (uint16_t)v);
    else sel_wr32(ea->sel, ea->off, v);
}

/* ------------------------------------------------------------------ ALU core */

enum { ALU_ADD, ALU_OR, ALU_ADC, ALU_SBB, ALU_AND, ALU_SUB, ALU_XOR, ALU_CMP };

static uint32_t alu(Cpu *c, int op, uint32_t a, uint32_t b, int size, int *store)
{
    uint32_t cf = (c->eflags & F_CF) ? 1u : 0u;
    uint32_t r;

    *store = 1;
    switch (op) {
    case ALU_ADD: r = a + b;      flags_add(c, a, b, 0,  r, size); break;
    case ALU_ADC: r = a + b + cf; flags_add(c, a, b, cf, r, size); break;
    case ALU_SUB: r = a - b;      flags_sub(c, a, b, 0,  r, size); break;
    case ALU_SBB: r = a - b - cf; flags_sub(c, a, b, cf, r, size); break;
    case ALU_CMP: r = a - b;      flags_sub(c, a, b, 0,  r, size); *store = 0; break;
    case ALU_OR:  r = a | b;      flags_logic(c, r, size); break;
    case ALU_AND: r = a & b;      flags_logic(c, r, size); break;
    default:      r = a ^ b;      flags_logic(c, r, size); break;
    }
    return r & mask_of(size);
}

/* INC and DEC leave CF alone; that is the classic emulator bug, so it is worth
   being explicit about. */
static uint32_t do_inc(Cpu *c, uint32_t a, int size)
{
    uint32_t r = (a + 1) & mask_of(size);
    uint32_t keep = c->eflags & F_CF;
    c->eflags &= ~(F_OF | F_AF);
    if (r == sign_of(size))      c->eflags |= F_OF;
    if ((r & 0x0Fu) == 0)        c->eflags |= F_AF;
    set_zsp(c, r, size);
    c->eflags = (c->eflags & ~F_CF) | keep;
    return r;
}

static uint32_t do_dec(Cpu *c, uint32_t a, int size)
{
    uint32_t r = (a - 1) & mask_of(size);
    uint32_t keep = c->eflags & F_CF;
    c->eflags &= ~(F_OF | F_AF);
    if (r == sign_of(size) - 1)  c->eflags |= F_OF;
    if ((r & 0x0Fu) == 0x0Fu)    c->eflags |= F_AF;
    set_zsp(c, r, size);
    c->eflags = (c->eflags & ~F_CF) | keep;
    return r;
}

/* --------------------------------------------------------- shifts and rotates */

static uint32_t do_shift(Cpu *c, int op, uint32_t v, unsigned count, int size)
{
    uint32_t m = mask_of(size), s = sign_of(size);
    unsigned bits = size * 8;
    uint32_t cf;

    count &= 31;                     /* the 386 masks the count to 5 bits */
    v &= m;
    if (count == 0) return v;        /* flags untouched */

    switch (op) {
    case 0:                          /* ROL */
        count %= bits;
        if (count) v = ((v << count) | (v >> (bits - count))) & m;
        cf = v & 1u;
        c->eflags = (c->eflags & ~(F_CF | F_OF)) | (cf ? F_CF : 0);
        if (((v & s) != 0) != (cf != 0)) c->eflags |= F_OF;
        return v;
    case 1:                          /* ROR */
        count %= bits;
        if (count) v = ((v >> count) | (v << (bits - count))) & m;
        c->eflags &= ~(F_CF | F_OF);
        if (v & s) c->eflags |= F_CF;
        if (((v & s) != 0) != ((v & (s >> 1)) != 0)) c->eflags |= F_OF;
        return v;
    case 2: {                        /* RCL */
        unsigned i;
        for (i = 0; i < count % (bits + 1); i++) {
            uint32_t hi = (v & s) != 0;
            v = ((v << 1) | ((c->eflags & F_CF) ? 1u : 0u)) & m;
            c->eflags = (c->eflags & ~F_CF) | (hi ? F_CF : 0);
        }
        c->eflags &= ~F_OF;
        if (((v & s) != 0) != ((c->eflags & F_CF) != 0)) c->eflags |= F_OF;
        return v;
    }
    case 3: {                        /* RCR */
        unsigned i;
        c->eflags &= ~F_OF;
        if (((v & s) != 0) != ((c->eflags & F_CF) != 0)) c->eflags |= F_OF;
        for (i = 0; i < count % (bits + 1); i++) {
            uint32_t lo = v & 1u;
            v = (v >> 1) | ((c->eflags & F_CF) ? s : 0);
            c->eflags = (c->eflags & ~F_CF) | (lo ? F_CF : 0);
        }
        return v & m;
    }
    case 4:                          /* SHL */
    case 6: {                        /* SAL, same encoding space */
        uint32_t r;
        cf = (count <= bits) ? ((v >> (bits - count)) & 1u) : 0u;
        r = (count >= bits) ? 0u : (v << count) & m;
        c->eflags &= ~(F_CF | F_OF);
        if (cf) c->eflags |= F_CF;
        if (((r & s) != 0) != (cf != 0)) c->eflags |= F_OF;
        set_zsp(c, r, size);
        return r;
    }
    case 5: {                        /* SHR */
        uint32_t r;
        cf = (count <= bits) ? ((v >> (count - 1)) & 1u) : 0u;
        r = (count >= bits) ? 0u : (v >> count);
        c->eflags &= ~(F_CF | F_OF);
        if (cf) c->eflags |= F_CF;
        if (v & s) c->eflags |= F_OF;      /* OF = original sign bit */
        set_zsp(c, r, size);
        return r & m;
    }
    default: {                       /* SAR */
        int32_t sv = (int32_t)sext(v, size);
        uint32_t r;
        unsigned n = count >= bits ? bits - 1 : count;
        cf = ((uint32_t)(sv >> (int)(count > bits ? bits - 1 : count - 1))) & 1u;
        r = (uint32_t)(sv >> (int)n) & m;
        c->eflags &= ~(F_CF | F_OF);
        if (cf) c->eflags |= F_CF;
        set_zsp(c, r, size);
        return r;
    }
    }
}

/* ------------------------------------------------------------ multiply/divide */

static void do_mul(Cpu *c, int size, uint32_t src, int signed_op)
{
    c->eflags &= ~(F_CF | F_OF);
    if (size == 1) {
        uint16_t r = signed_op
            ? (uint16_t)((int16_t)(int8_t)get_reg(c, R_AX, 1) * (int16_t)(int8_t)src)
            : (uint16_t)((uint8_t)get_reg(c, R_AX, 1) * (uint8_t)src);
        set_reg(c, R_AX, 2, r);
        if (signed_op ? ((int16_t)r != (int8_t)r) : ((r >> 8) != 0))
            c->eflags |= F_CF | F_OF;
        set_zsp(c, r & 0xFF, 1);
    } else if (size == 2) {
        uint32_t r = signed_op
            ? (uint32_t)((int32_t)(int16_t)get_reg(c, R_AX, 2) * (int32_t)(int16_t)src)
            : (uint32_t)((uint16_t)get_reg(c, R_AX, 2) * (uint16_t)src);
        set_reg(c, R_AX, 2, r & 0xFFFF);
        set_reg(c, R_DX, 2, r >> 16);
        if (signed_op ? ((int32_t)r != (int16_t)r) : ((r >> 16) != 0))
            c->eflags |= F_CF | F_OF;
        set_zsp(c, r & 0xFFFF, 2);
    } else {
        uint64_t r = signed_op
            ? (uint64_t)((int64_t)(int32_t)c->r32[R_AX] * (int64_t)(int32_t)src)
            : (uint64_t)c->r32[R_AX] * (uint64_t)src;
        c->r32[R_AX] = (uint32_t)r;
        c->r32[R_DX] = (uint32_t)(r >> 32);
        if (signed_op ? ((int64_t)r != (int32_t)r) : ((r >> 32) != 0))
            c->eflags |= F_CF | F_OF;
        set_zsp(c, (uint32_t)r, 4);
    }
}

static int do_div(Cpu *c, int size, uint32_t src, int signed_op)
{
    /* In the 8-bit encoding, register index 4 is AH. */
    if (src == 0) return 0;
    if (size == 1) {
        if (signed_op) {
            int16_t n = (int16_t)get_reg(c, R_AX, 2);
            int16_t d = (int8_t)src, q;
            q = (int16_t)(n / d);
            if (q > 127 || q < -128) return 0;
            set_reg(c, R_AX, 1, (uint32_t)q & 0xFF);
            set_reg(c, 4, 1, (uint32_t)(n % d) & 0xFF);
        } else {
            uint16_t n = (uint16_t)get_reg(c, R_AX, 2);
            uint8_t d = (uint8_t)src;
            uint16_t q = (uint16_t)(n / d);
            if (q > 0xFF) return 0;
            set_reg(c, R_AX, 1, q & 0xFF);
            set_reg(c, 4, 1, (uint32_t)(n % d) & 0xFF);
        }
        return 1;
    }
    if (size == 2) {
        uint32_t n = ((uint32_t)get_reg(c, R_DX, 2) << 16) | get_reg(c, R_AX, 2);
        if (signed_op) {
            int32_t sn = (int32_t)n, d = (int16_t)src, q;
            if (d == 0) return 0;
            q = sn / d;
            if (q > 32767 || q < -32768) return 0;
            set_reg(c, R_AX, 2, (uint32_t)q & 0xFFFF);
            set_reg(c, R_DX, 2, (uint32_t)(sn % d) & 0xFFFF);
        } else {
            uint32_t d = (uint16_t)src, q = n / d;
            if (q > 0xFFFF) return 0;
            set_reg(c, R_AX, 2, q & 0xFFFF);
            set_reg(c, R_DX, 2, (n % d) & 0xFFFF);
        }
        return 1;
    }
    {
        uint64_t n = ((uint64_t)c->r32[R_DX] << 32) | c->r32[R_AX];
        if (signed_op) {
            int64_t sn = (int64_t)n, d = (int32_t)src, q;
            if (d == 0) return 0;
            q = sn / d;
            if (q > 2147483647LL || q < -2147483648LL) return 0;
            c->r32[R_AX] = (uint32_t)q;
            c->r32[R_DX] = (uint32_t)(sn % d);
        } else {
            uint64_t d = src, q = n / d;
            if (q > 0xFFFFFFFFull) return 0;
            c->r32[R_AX] = (uint32_t)q;
            c->r32[R_DX] = (uint32_t)(n % d);
        }
        return 1;
    }
}

/* ------------------------------------------------------------------ conditions */

static int cond(Cpu *c, int cc)
{
    uint32_t f = c->eflags;
    int r;

    switch (cc >> 1) {
    case 0: r = (f & F_OF) != 0; break;                           /* O  */
    case 1: r = (f & F_CF) != 0; break;                           /* B  */
    case 2: r = (f & F_ZF) != 0; break;                           /* Z  */
    case 3: r = (f & (F_CF | F_ZF)) != 0; break;                  /* BE */
    case 4: r = (f & F_SF) != 0; break;                           /* S  */
    case 5: r = (f & F_PF) != 0; break;                           /* P  */
    case 6: r = ((f & F_SF) != 0) != ((f & F_OF) != 0); break;     /* L  */
    default: r = (((f & F_SF) != 0) != ((f & F_OF) != 0)) ||
                 ((f & F_ZF) != 0); break;                        /* LE */
    }
    return (cc & 1) ? !r : r;
}

/* ---------------------------------------------------------------- string ops */

static void str_step(Cpu *c, int reg, int size, int addr32)
{
    int32_t d = (c->eflags & F_DF) ? -size : size;
    if (addr32) c->r32[reg] = (uint32_t)(c->r32[reg] + d);
    else set_reg16(c, reg, (uint16_t)(reg16(c, reg) + d));
}

/* ------------------------------------------------------------------- far jumps */

static void far_jump(Cpu *c, uint16_t sel, uint16_t off)
{
    c->seg[S_CS] = sel;
    ip_set(c, off);
}

/* =============================================================== interpreter */

/* See cpu_stop_latch in cpu.h.  These live outside Cpu on purpose: call16
   restores the whole register file on the way home, so a latch stored in the
   struct would be thrown away by the very unwind it is meant to survive. */
static int      latched;
static uint16_t latched_cs, latched_ip;
static uint8_t  latched_op, latched_op2;

void cpu_stop_latch(Cpu *c, int reason)
{
    if (latched) return;                /* keep the first, most informative one */
    latched     = reason;
    latched_cs  = (uint16_t)c->bad_cs;
    latched_ip  = (uint16_t)c->bad_ip;
    latched_op  = c->bad_op;
    latched_op2 = c->bad_op2;
}

int cpu_stop_latched(void) { return latched; }

int cpu_step(Cpu *c)
{
    uint8_t op;
    int osize = 2, asize = 0;      /* operand size in bytes; asize: 32-bit addr */
    int rep = 0;                   /* 0 none, 0xF3 repe/rep, 0xF2 repne        */

    if (latched) {
        c->state  = latched;
        c->bad_cs = latched_cs;
        c->bad_ip = latched_ip;
        c->bad_op = latched_op;
        c->bad_op2 = latched_op2;
        return latched;
    }

    c->seg_override = -1;
    c->icount++;

    /* A call that lands in the thunk selector is an imported API. */
    if (c->seg[S_CS] == thunk_selector()) {
        thunk_dispatch(c, ip_of(c));
        return c->state;
    }
    if (c->seg[S_CS] == call16_ret_selector()) {
        c->state = CPU_RETURN;
        return CPU_RETURN;
    }

    /* Prefixes. */
    for (;;) {
        op = fetch8(c);
        switch (op) {
        case 0x66: osize = (osize == 2) ? 4 : 2; continue;
        case 0x67: asize = !asize; continue;
        case 0x26: c->seg_override = S_ES; continue;
        case 0x2E: c->seg_override = S_CS; continue;
        case 0x36: c->seg_override = S_SS; continue;
        case 0x3E: c->seg_override = S_DS; continue;
        case 0x64: c->seg_override = S_FS; continue;
        case 0x65: c->seg_override = S_GS; continue;
        case 0xF0: continue;                       /* LOCK: no effect for us */
        case 0xF2: rep = 0xF2; continue;
        case 0xF3: rep = 0xF3; continue;
        default: break;
        }
        break;
    }

    switch (op) {

    /* ---- ALU: r/m, r and r, r/m ------------------------------------------ */
    case 0x00: case 0x08: case 0x10: case 0x18:
    case 0x20: case 0x28: case 0x30: case 0x38:   /* op r/m8, r8 */
    case 0x01: case 0x09: case 0x11: case 0x19:
    case 0x21: case 0x29: case 0x31: case 0x39:   /* op r/m, r   */
    case 0x02: case 0x0A: case 0x12: case 0x1A:
    case 0x22: case 0x2A: case 0x32: case 0x3A:   /* op r8, r/m8 */
    case 0x03: case 0x0B: case 0x13: case 0x1B:
    case 0x23: case 0x2B: case 0x33: case 0x3B: { /* op r, r/m   */
        int aluop = (op >> 3) & 7;
        int size = (op & 1) ? osize : 1;
        int to_reg = (op & 2) != 0;
        Ea ea;
        uint32_t a, b, r;
        int store;

        decode_ea(c, fetch8(c), asize, &ea);
        if (to_reg) {
            a = get_reg(c, ea.reg, size);
            b = ea_read(c, &ea, size);
            r = alu(c, aluop, a, b, size, &store);
            if (store) set_reg(c, ea.reg, size, r);
        } else {
            a = ea_read(c, &ea, size);
            b = get_reg(c, ea.reg, size);
            r = alu(c, aluop, a, b, size, &store);
            if (store) ea_write(c, &ea, size, r);
        }
        break;
    }

    /* ---- ALU: accumulator, immediate ------------------------------------- */
    case 0x04: case 0x0C: case 0x14: case 0x1C:
    case 0x24: case 0x2C: case 0x34: case 0x3C:
    case 0x05: case 0x0D: case 0x15: case 0x1D:
    case 0x25: case 0x2D: case 0x35: case 0x3D: {
        int aluop = (op >> 3) & 7;
        int size = (op & 1) ? osize : 1;
        uint32_t imm = (size == 1) ? fetch8(c) : (size == 2 ? fetch16(c) : fetch32(c));
        uint32_t r;
        int store;

        r = alu(c, aluop, get_reg(c, R_AX, size), imm, size, &store);
        if (store) set_reg(c, R_AX, size, r);
        break;
    }

    /* ---- group 1: op r/m, imm ------------------------------------------- */
    case 0x80: case 0x81: case 0x83: {
        int size = (op == 0x80) ? 1 : osize;
        Ea ea;
        uint32_t imm, a, r;
        int store;

        decode_ea(c, fetch8(c), asize, &ea);
        if (op == 0x80) imm = fetch8(c);
        else if (op == 0x83) imm = sext(fetch8(c), 1) & mask_of(size);
        else imm = (size == 2) ? fetch16(c) : fetch32(c);
        a = ea_read(c, &ea, size);
        r = alu(c, ea.reg, a, imm, size, &store);
        if (store) ea_write(c, &ea, size, r);
        break;
    }

    /* ---- TEST ----------------------------------------------------------- */
    case 0x84: case 0x85: {
        int size = (op & 1) ? osize : 1;
        Ea ea;
        decode_ea(c, fetch8(c), asize, &ea);
        flags_logic(c, ea_read(c, &ea, size) & get_reg(c, ea.reg, size), size);
        break;
    }
    case 0xA8: case 0xA9: {
        int size = (op & 1) ? osize : 1;
        uint32_t imm = (size == 1) ? fetch8(c) : (size == 2 ? fetch16(c) : fetch32(c));
        flags_logic(c, get_reg(c, R_AX, size) & imm, size);
        break;
    }
    case 0xF6: case 0xF7: {
        int size = (op & 1) ? osize : 1;
        Ea ea;
        uint32_t v;

        decode_ea(c, fetch8(c), asize, &ea);
        v = ea_read(c, &ea, size);
        switch (ea.reg) {
        case 0: case 1: {                       /* TEST r/m, imm */
            uint32_t imm = (size == 1) ? fetch8(c)
                         : (size == 2) ? fetch16(c) : fetch32(c);
            flags_logic(c, v & imm, size);
            break;
        }
        case 2:                                  /* NOT */
            ea_write(c, &ea, size, ~v & mask_of(size));
            break;
        case 3: {                                /* NEG */
            uint32_t r = (0u - v) & mask_of(size);
            flags_sub(c, 0, v, 0, r, size);
            ea_write(c, &ea, size, r);
            break;
        }
        case 4: do_mul(c, size, v, 0); break;    /* MUL   */
        case 5: do_mul(c, size, v, 1); break;    /* IMUL  */
        case 6:
            if (!do_div(c, size, v, 0)) { c->state = CPU_FAULT; return CPU_FAULT; }
            break;
        default:
            if (!do_div(c, size, v, 1)) { c->state = CPU_FAULT; return CPU_FAULT; }
            break;
        }
        break;
    }

    /* ---- MOV ------------------------------------------------------------ */
    case 0x88: case 0x89: {
        int size = (op & 1) ? osize : 1;
        Ea ea;
        decode_ea(c, fetch8(c), asize, &ea);
        ea_write(c, &ea, size, get_reg(c, ea.reg, size));
        break;
    }
    case 0x8A: case 0x8B: {
        int size = (op & 1) ? osize : 1;
        Ea ea;
        decode_ea(c, fetch8(c), asize, &ea);
        set_reg(c, ea.reg, size, ea_read(c, &ea, size));
        break;
    }
    case 0xC6: case 0xC7: {
        int size = (op & 1) ? osize : 1;
        Ea ea;
        uint32_t imm;
        decode_ea(c, fetch8(c), asize, &ea);
        imm = (size == 1) ? fetch8(c) : (size == 2 ? fetch16(c) : fetch32(c));
        ea_write(c, &ea, size, imm);
        break;
    }
    case 0xB0: case 0xB1: case 0xB2: case 0xB3:
    case 0xB4: case 0xB5: case 0xB6: case 0xB7:
        set_reg(c, op & 7, 1, fetch8(c));
        break;
    case 0xB8: case 0xB9: case 0xBA: case 0xBB:
    case 0xBC: case 0xBD: case 0xBE: case 0xBF:
        set_reg(c, op & 7, osize, (osize == 2) ? fetch16(c) : fetch32(c));
        break;
    case 0xA0: case 0xA1: {
        int size = (op & 1) ? osize : 1;
        uint16_t off = asize ? (uint16_t)fetch32(c) : fetch16(c);
        uint16_t sel = seg_for(c, S_DS);
        set_reg(c, R_AX, size,
                size == 1 ? sel_rd8(sel, off)
                          : (size == 2 ? sel_rd16(sel, off) : sel_rd32(sel, off)));
        break;
    }
    case 0xA2: case 0xA3: {
        int size = (op & 1) ? osize : 1;
        uint16_t off = asize ? (uint16_t)fetch32(c) : fetch16(c);
        uint16_t sel = seg_for(c, S_DS);
        uint32_t v = get_reg(c, R_AX, size);
        if (size == 1) sel_wr8(sel, off, (uint8_t)v);
        else if (size == 2) sel_wr16(sel, off, (uint16_t)v);
        else sel_wr32(sel, off, v);
        break;
    }

    /* ---- segment registers, LEA, LES/LDS -------------------------------- */
    case 0x8C: {                                  /* MOV r/m16, Sreg */
        Ea ea;
        decode_ea(c, fetch8(c), asize, &ea);
        ea_write(c, &ea, 2, c->seg[ea.reg]);
        break;
    }
    case 0x8E: {                                  /* MOV Sreg, r/m16 */
        Ea ea;
        decode_ea(c, fetch8(c), asize, &ea);
        if (ea.reg == S_CS) { c->state = CPU_BADOP; break; }
        c->seg[ea.reg] = (uint16_t)ea_read(c, &ea, 2);
        break;
    }
    case 0x8D: {                                  /* LEA */
        Ea ea;
        decode_ea(c, fetch8(c), asize, &ea);
        if (ea.is_reg) { c->state = CPU_BADOP; break; }
        set_reg(c, ea.reg, osize, ea.off);
        break;
    }
    case 0xC4: case 0xC5: {                       /* LES / LDS */
        Ea ea;
        decode_ea(c, fetch8(c), asize, &ea);
        if (ea.is_reg) { c->state = CPU_BADOP; break; }
        set_reg(c, ea.reg, 2, sel_rd16(ea.sel, ea.off));
        c->seg[(op == 0xC4) ? S_ES : S_DS] =
            sel_rd16(ea.sel, (uint16_t)(ea.off + 2));
        break;
    }

    /* ---- push / pop ----------------------------------------------------- */
    case 0x50: case 0x51: case 0x52: case 0x53:
    case 0x54: case 0x55: case 0x56: case 0x57:
        push_size(c, osize, get_reg(c, op & 7, osize));
        break;
    case 0x58: case 0x59: case 0x5A: case 0x5B:
    case 0x5C: case 0x5D: case 0x5E: case 0x5F:
        set_reg(c, op & 7, osize, pop_size(c, osize));
        break;
    case 0x06: cpu_push16(c, c->seg[S_ES]); break;
    case 0x0E: cpu_push16(c, c->seg[S_CS]); break;
    case 0x16: cpu_push16(c, c->seg[S_SS]); break;
    case 0x1E: cpu_push16(c, c->seg[S_DS]); break;
    case 0x07: c->seg[S_ES] = cpu_pop16(c); break;
    case 0x17: c->seg[S_SS] = cpu_pop16(c); break;
    case 0x1F: c->seg[S_DS] = cpu_pop16(c); break;
    case 0x68: push_size(c, osize, (osize == 2) ? fetch16(c) : fetch32(c)); break;
    case 0x6A: push_size(c, osize, sext(fetch8(c), 1) & mask_of(osize)); break;
    case 0x8F: {                                  /* POP r/m */
        Ea ea;
        uint32_t v;
        decode_ea(c, fetch8(c), asize, &ea);
        v = pop_size(c, osize);
        ea_write(c, &ea, osize, v);
        break;
    }
    case 0x60: {                                  /* PUSHA */
        uint16_t sp = reg16(c, R_SP);
        int i;
        for (i = 0; i < 8; i++)
            push_size(c, osize, (i == R_SP) ? sp : get_reg(c, i, osize));
        break;
    }
    case 0x61: {                                  /* POPA */
        int i;
        for (i = 7; i >= 0; i--) {
            uint32_t v = pop_size(c, osize);
            if (i != R_SP) set_reg(c, i, osize, v);
        }
        break;
    }
    case 0x9C:                                    /* PUSHF */
        push_size(c, osize, c->eflags & (F_ALL | F_RS));
        break;
    case 0x9D: {                                  /* POPF */
        uint32_t v = pop_size(c, osize);
        c->eflags = (v & F_ALL) | F_RS;
        break;
    }
    case 0x9E:                                    /* SAHF */
        c->eflags = (c->eflags & ~(F_SF | F_ZF | F_AF | F_PF | F_CF)) | F_RS |
                    (get_reg(c, 4, 1) & (F_SF | F_ZF | F_AF | F_PF | F_CF));
        break;
    case 0x9F:                                    /* LAHF */
        set_reg(c, 4, 1, (c->eflags & 0xFF) | 0x02u);
        break;

    /* ---- exchange -------------------------------------------------------- */
    case 0x86: case 0x87: {
        int size = (op & 1) ? osize : 1;
        Ea ea;
        uint32_t a, b;
        decode_ea(c, fetch8(c), asize, &ea);
        a = ea_read(c, &ea, size);
        b = get_reg(c, ea.reg, size);
        ea_write(c, &ea, size, b);
        set_reg(c, ea.reg, size, a);
        break;
    }
    case 0x91: case 0x92: case 0x93:
    case 0x94: case 0x95: case 0x96: case 0x97: {
        uint32_t a = get_reg(c, R_AX, osize);
        uint32_t b = get_reg(c, op & 7, osize);
        set_reg(c, R_AX, osize, b);
        set_reg(c, op & 7, osize, a);
        break;
    }
    case 0x90: break;                             /* NOP */

    /* ---- inc / dec ------------------------------------------------------- */
    case 0x40: case 0x41: case 0x42: case 0x43:
    case 0x44: case 0x45: case 0x46: case 0x47:
        set_reg(c, op & 7, osize, do_inc(c, get_reg(c, op & 7, osize), osize));
        break;
    case 0x48: case 0x49: case 0x4A: case 0x4B:
    case 0x4C: case 0x4D: case 0x4E: case 0x4F:
        set_reg(c, op & 7, osize, do_dec(c, get_reg(c, op & 7, osize), osize));
        break;
    case 0xFE: case 0xFF: {
        int size = (op == 0xFE) ? 1 : osize;
        Ea ea;
        uint32_t v;

        decode_ea(c, fetch8(c), asize, &ea);
        if (op == 0xFE && ea.reg > 1) { c->state = CPU_BADOP; break; }
        v = ea_read(c, &ea, size);
        switch (ea.reg) {
        case 0: ea_write(c, &ea, size, do_inc(c, v, size)); break;
        case 1: ea_write(c, &ea, size, do_dec(c, v, size)); break;
        case 2:                                   /* CALL near r/m */
            push_size(c, osize, ip_of(c));
            ip_set(c, (uint16_t)v);
            break;
        case 3: {                                 /* CALL far m16:16 */
            uint16_t off, sel;
            if (ea.is_reg) { c->state = CPU_BADOP; break; }
            off = sel_rd16(ea.sel, ea.off);
            sel = sel_rd16(ea.sel, (uint16_t)(ea.off + 2));
            cpu_push16(c, c->seg[S_CS]);
            cpu_push16(c, ip_of(c));
            far_jump(c, sel, off);
            break;
        }
        case 4:                                   /* JMP near r/m */
            ip_set(c, (uint16_t)v);
            break;
        case 5: {                                 /* JMP far m16:16 */
            uint16_t off, sel;
            if (ea.is_reg) { c->state = CPU_BADOP; break; }
            off = sel_rd16(ea.sel, ea.off);
            sel = sel_rd16(ea.sel, (uint16_t)(ea.off + 2));
            far_jump(c, sel, off);
            break;
        }
        case 6: push_size(c, osize, v); break;    /* PUSH r/m */
        default: c->state = CPU_BADOP; break;
        }
        break;
    }

    /* ---- shifts and rotates --------------------------------------------- */
    case 0xC0: case 0xC1: case 0xD0: case 0xD1:
    case 0xD2: case 0xD3: {
        int size = (op & 1) ? osize : 1;
        Ea ea;
        unsigned count;

        decode_ea(c, fetch8(c), asize, &ea);
        if (op == 0xC0 || op == 0xC1) count = fetch8(c);
        else if (op == 0xD0 || op == 0xD1) count = 1;
        else count = get_reg(c, R_CX, 1);
        ea_write(c, &ea, size,
                 do_shift(c, ea.reg, ea_read(c, &ea, size), count, size));
        break;
    }

    /* ---- control transfer ----------------------------------------------- */
    case 0xE8: {                                  /* CALL near rel */
        int32_t d = (osize == 2) ? (int16_t)fetch16(c) : (int32_t)fetch32(c);
        push_size(c, osize, ip_of(c));
        ip_set(c, (uint16_t)(ip_of(c) + d));
        break;
    }
    case 0xE9: {                                  /* JMP near rel */
        int32_t d = (osize == 2) ? (int16_t)fetch16(c) : (int32_t)fetch32(c);
        ip_set(c, (uint16_t)(ip_of(c) + d));
        break;
    }
    case 0xEB: {                                  /* JMP short */
        int8_t d = (int8_t)fetch8(c);
        ip_set(c, (uint16_t)(ip_of(c) + d));
        break;
    }
    case 0x9A: {                                  /* CALL far imm */
        uint16_t off = fetch16(c), sel = fetch16(c);
        cpu_push16(c, c->seg[S_CS]);
        cpu_push16(c, ip_of(c));
        far_jump(c, sel, off);
        break;
    }
    case 0xEA: {                                  /* JMP far imm */
        uint16_t off = fetch16(c), sel = fetch16(c);
        far_jump(c, sel, off);
        break;
    }
    case 0xC3: ip_set(c, cpu_pop16(c)); break;    /* RET near */
    case 0xC2: {                                  /* RET near imm */
        uint16_t n = fetch16(c);
        ip_set(c, cpu_pop16(c));
        set_reg16(c, R_SP, (uint16_t)(reg16(c, R_SP) + n));
        break;
    }
    case 0xCB: {                                  /* RETF */
        uint16_t off = cpu_pop16(c), sel = cpu_pop16(c);
        far_jump(c, sel, off);
        break;
    }
    case 0xCA: {                                  /* RETF imm */
        uint16_t n = fetch16(c);
        uint16_t off = cpu_pop16(c), sel = cpu_pop16(c);
        set_reg16(c, R_SP, (uint16_t)(reg16(c, R_SP) + n));
        far_jump(c, sel, off);
        break;
    }
    case 0xCF: {                                  /* IRET */
        uint16_t off = cpu_pop16(c), sel = cpu_pop16(c), fl = cpu_pop16(c);
        c->eflags = (fl & F_ALL) | F_RS;
        far_jump(c, sel, off);
        break;
    }
    case 0x70: case 0x71: case 0x72: case 0x73:
    case 0x74: case 0x75: case 0x76: case 0x77:
    case 0x78: case 0x79: case 0x7A: case 0x7B:
    case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
        int8_t d = (int8_t)fetch8(c);
        if (cond(c, op & 15)) ip_set(c, (uint16_t)(ip_of(c) + d));
        break;
    }
    case 0xE0: case 0xE1: case 0xE2: {            /* LOOPNE / LOOPE / LOOP */
        int8_t d = (int8_t)fetch8(c);
        uint16_t cx = (uint16_t)(reg16(c, R_CX) - 1);
        int take;
        set_reg16(c, R_CX, cx);
        if (op == 0xE2) take = cx != 0;
        else if (op == 0xE1) take = cx != 0 && (c->eflags & F_ZF);
        else take = cx != 0 && !(c->eflags & F_ZF);
        if (take) ip_set(c, (uint16_t)(ip_of(c) + d));
        break;
    }
    case 0xE3: {                                  /* JCXZ */
        int8_t d = (int8_t)fetch8(c);
        if (reg16(c, R_CX) == 0) ip_set(c, (uint16_t)(ip_of(c) + d));
        break;
    }
    case 0xC8: {                                  /* ENTER */
        uint16_t alloc = fetch16(c);
        uint8_t level = fetch8(c);
        uint16_t fp;
        cpu_push16(c, reg16(c, R_BP));
        fp = reg16(c, R_SP);
        while (level-- > 1) {
            set_reg16(c, R_BP, (uint16_t)(reg16(c, R_BP) - 2));
            cpu_push16(c, sel_rd16(c->seg[S_SS], reg16(c, R_BP)));
        }
        set_reg16(c, R_BP, fp);
        set_reg16(c, R_SP, (uint16_t)(reg16(c, R_SP) - alloc));
        break;
    }
    case 0xC9:                                    /* LEAVE */
        set_reg16(c, R_SP, reg16(c, R_BP));
        set_reg16(c, R_BP, cpu_pop16(c));
        break;

    /* ---- flag and misc --------------------------------------------------- */
    case 0xF5: c->eflags ^= F_CF; break;          /* CMC */
    case 0xF8: c->eflags &= ~F_CF; break;         /* CLC */
    case 0xF9: c->eflags |= F_CF; break;          /* STC */
    case 0xFA: c->eflags &= ~F_IF; break;         /* CLI */
    case 0xFB: c->eflags |= F_IF; break;          /* STI */
    case 0xFC: c->eflags &= ~F_DF; break;         /* CLD */
    case 0xFD: c->eflags |= F_DF; break;          /* STD */
    case 0x98:                                    /* CBW / CWDE */
        if (osize == 2) set_reg(c, R_AX, 2, sext(get_reg(c, R_AX, 1), 1) & 0xFFFF);
        else c->r32[R_AX] = sext(get_reg(c, R_AX, 2), 2);
        break;
    case 0x99:                                    /* CWD / CDQ */
        if (osize == 2)
            set_reg(c, R_DX, 2, (get_reg(c, R_AX, 2) & 0x8000u) ? 0xFFFFu : 0);
        else
            c->r32[R_DX] = (c->r32[R_AX] & 0x80000000u) ? 0xFFFFFFFFu : 0;
        break;
    case 0xD7: {                                  /* XLAT */
        uint16_t sel = seg_for(c, S_DS);
        set_reg(c, R_AX, 1,
                sel_rd8(sel, (uint16_t)(reg16(c, R_BX) + get_reg(c, R_AX, 1))));
        break;
    }
    /* The decimal adjusts follow the manual's pseudocode literally.  A 16-bit C
       compiler never emits them, but they are cheap to get right and the
       differential fuzzer reports every deviation, so approximating them would
       just be permanent noise. */
    case 0x27: case 0x2F: {                       /* DAA / DAS */
        int das = (op == 0x2F);
        uint32_t al = get_reg(c, R_AX, 1), old_al = al;
        int old_cf = (c->eflags & F_CF) != 0;
        int old_af = (c->eflags & F_AF) != 0;
        int cf = old_cf;

        c->eflags &= ~(F_CF | F_AF);
        if ((al & 0x0F) > 9 || old_af) {
            cf = old_cf || (das ? (al < 6) : (al + 6 > 0xFF));
            al = (das ? al - 6 : al + 6) & 0xFF;
            c->eflags |= F_AF;
        }
        if (old_al > 0x99 || old_cf) {
            al = (das ? al - 0x60 : al + 0x60) & 0xFF;
            cf = 1;
        } else if (!das) {
            cf = 0;         /* DAA clears CF here; DAS leaves it as it was */
        }
        set_reg(c, R_AX, 1, al);
        set_zsp(c, al, 1);
        if (cf) c->eflags |= F_CF;
        break;
    }
    case 0x37: case 0x3F: {                       /* AAA / AAS */
        uint32_t ax = get_reg(c, R_AX, 2);

        if ((ax & 0x0F) > 9 || (c->eflags & F_AF)) {
            /* The adjustment is on the whole of AX, so a carry or borrow out of
               AL reaches AH, and only then is AH stepped.  Doing this on AL and
               AH separately loses that propagation. */
            ax = (op == 0x37) ? ax + 6 : ax - 6;
            ax = (op == 0x37) ? ax + 0x100 : ax - 0x100;
            c->eflags |= F_CF | F_AF;
        } else {
            c->eflags &= ~(F_CF | F_AF);
        }
        set_reg(c, R_AX, 2, ax & 0xFFFFu);
        set_reg(c, R_AX, 1, ax & 0x0F);
        break;
    }
    case 0xD4: {                                  /* AAM */
        uint8_t base = fetch8(c);
        uint32_t al = get_reg(c, R_AX, 1);
        if (base == 0) { c->state = CPU_FAULT; return CPU_FAULT; }
        set_reg(c, 4, 1, al / base);
        set_reg(c, R_AX, 1, al % base);
        set_zsp(c, al % base, 1);
        break;
    }
    case 0xD5: {                                  /* AAD */
        uint8_t base = fetch8(c);
        uint32_t v = get_reg(c, R_AX, 1) + get_reg(c, 4, 1) * base;
        set_reg(c, R_AX, 1, v & 0xFF);
        set_reg(c, 4, 1, 0);
        set_zsp(c, v & 0xFF, 1);
        break;
    }
    case 0x62: {                                  /* BOUND */
        Ea ea;
        int32_t v, lo, hi;
        decode_ea(c, fetch8(c), asize, &ea);
        if (ea.is_reg) { c->state = CPU_BADOP; break; }
        v = (int32_t)sext(get_reg(c, ea.reg, osize), osize);
        lo = (int32_t)sext(ea_read(c, &ea, osize), osize);
        {
            Ea hiea = ea;
            hiea.off = (uint16_t)(ea.off + osize);
            hi = (int32_t)sext(ea_read(c, &hiea, osize), osize);
        }
        if (v < lo || v > hi) { c->state = CPU_FAULT; return CPU_FAULT; }
        break;
    }
    case 0x69: case 0x6B: {                       /* IMUL r, r/m, imm */
        Ea ea;
        uint32_t src, imm;
        int64_t r;
        decode_ea(c, fetch8(c), asize, &ea);
        src = ea_read(c, &ea, osize);
        imm = (op == 0x6B) ? sext(fetch8(c), 1)
                           : (osize == 2 ? sext(fetch16(c), 2) : fetch32(c));
        r = (int64_t)(int32_t)sext(src, osize) * (int64_t)(int32_t)imm;
        set_reg(c, ea.reg, osize, (uint32_t)r & mask_of(osize));
        c->eflags &= ~(F_CF | F_OF);
        if (osize == 2 ? (r != (int16_t)r) : (r != (int32_t)r))
            c->eflags |= F_CF | F_OF;
        set_zsp(c, (uint32_t)r, osize);
        break;
    }

    /* ---- string operations ---------------------------------------------- */
    case 0xA4: case 0xA5:                         /* MOVS */
    case 0xAA: case 0xAB:                         /* STOS */
    case 0xAC: case 0xAD:                         /* LODS */
    case 0xA6: case 0xA7:                         /* CMPS */
    case 0xAE: case 0xAF: {                       /* SCAS */
        int size = (op & 1) ? osize : 1;
        uint16_t src_sel = seg_for(c, S_DS);
        uint16_t dst_sel = c->seg[S_ES];          /* ES is not overridable */
        uint32_t count = rep ? (asize ? c->r32[R_CX] : reg16(c, R_CX)) : 1;

        if (rep && count == 0) break;
        for (;;) {
            uint16_t si = asize ? (uint16_t)c->r32[R_SI] : reg16(c, R_SI);
            uint16_t di = asize ? (uint16_t)c->r32[R_DI] : reg16(c, R_DI);

            switch (op & 0xFE) {
            case 0xA4: {                          /* MOVS */
                uint32_t v = (size == 1) ? sel_rd8(src_sel, si)
                           : (size == 2) ? sel_rd16(src_sel, si)
                                         : sel_rd32(src_sel, si);
                if (size == 1) sel_wr8(dst_sel, di, (uint8_t)v);
                else if (size == 2) sel_wr16(dst_sel, di, (uint16_t)v);
                else sel_wr32(dst_sel, di, v);
                str_step(c, R_SI, size, asize);
                str_step(c, R_DI, size, asize);
                break;
            }
            case 0xAA: {                          /* STOS */
                uint32_t v = get_reg(c, R_AX, size);
                if (size == 1) sel_wr8(dst_sel, di, (uint8_t)v);
                else if (size == 2) sel_wr16(dst_sel, di, (uint16_t)v);
                else sel_wr32(dst_sel, di, v);
                str_step(c, R_DI, size, asize);
                break;
            }
            case 0xAC: {                          /* LODS */
                uint32_t v = (size == 1) ? sel_rd8(src_sel, si)
                           : (size == 2) ? sel_rd16(src_sel, si)
                                         : sel_rd32(src_sel, si);
                set_reg(c, R_AX, size, v);
                str_step(c, R_SI, size, asize);
                break;
            }
            case 0xA6: {                          /* CMPS */
                uint32_t a = (size == 1) ? sel_rd8(src_sel, si)
                           : (size == 2) ? sel_rd16(src_sel, si)
                                         : sel_rd32(src_sel, si);
                uint32_t b = (size == 1) ? sel_rd8(dst_sel, di)
                           : (size == 2) ? sel_rd16(dst_sel, di)
                                         : sel_rd32(dst_sel, di);
                flags_sub(c, a, b, 0, a - b, size);
                str_step(c, R_SI, size, asize);
                str_step(c, R_DI, size, asize);
                break;
            }
            default: {                            /* SCAS */
                uint32_t a = get_reg(c, R_AX, size);
                uint32_t b = (size == 1) ? sel_rd8(dst_sel, di)
                           : (size == 2) ? sel_rd16(dst_sel, di)
                                         : sel_rd32(dst_sel, di);
                flags_sub(c, a, b, 0, a - b, size);
                str_step(c, R_DI, size, asize);
                break;
            }
            }

            if (!rep) break;
            count--;
            if (asize) c->r32[R_CX] = count; else set_reg16(c, R_CX, (uint16_t)count);
            if (count == 0) break;
            /* CMPS and SCAS also stop on the zero flag. */
            if ((op & 0xFE) == 0xA6 || (op & 0xFE) == 0xAE) {
                int zf = (c->eflags & F_ZF) != 0;
                if (rep == 0xF3 && !zf) break;
                if (rep == 0xF2 && zf) break;
            }
        }
        break;
    }

    /* ---- interrupts ------------------------------------------------------ */
    case 0xCD: {
        uint8_t vec = fetch8(c);
        /* INT 3Fh is the NE moveable-entry thunk.  Nothing in Stars! reaches its
           own entry table, so seeing one means an assumption broke. */
        log_msg("*** INT %02Xh at %04X:%04X - not implemented\n",
                vec, c->seg[S_CS], (unsigned)(ip_of(c) - 2));
        c->state = CPU_BADOP;
        c->bad_op = 0xCD;
        c->bad_op2 = vec;
        break;
    }
    case 0xCC: c->state = CPU_HALT; break;        /* INT3 */
    case 0xF4: c->state = CPU_HALT; break;        /* HLT  */

    /* ---- x87 ------------------------------------------------------------- */
    case 0xD8: case 0xD9: case 0xDA: case 0xDB:
    case 0xDC: case 0xDD: case 0xDE: case 0xDF: {
        uint8_t modrm = fetch8(c);
        Ea ea = { 0, 0, 0, 0, 0 };
        if ((modrm >> 6) == 3) {
            ea.is_reg = 1;
            ea.reg = (modrm >> 3) & 7;
            ea.rm = modrm & 7;
        } else {
            /* Re-decode with the ModRM byte we already consumed. */
            decode_ea(c, modrm, asize, &ea);
        }
        if (!fpu_exec(c, op, modrm, ea.is_reg, ea.sel, ea.off)) {
            c->state = CPU_BADOP;
            c->bad_op = op;
            c->bad_op2 = modrm;
        }
        break;
    }
    case 0x9B: break;                             /* FWAIT */

    /* ---- two-byte opcodes ------------------------------------------------ */
    case 0x0F: {
        uint8_t op2 = fetch8(c);
        switch (op2) {
        case 0x80: case 0x81: case 0x82: case 0x83:
        case 0x84: case 0x85: case 0x86: case 0x87:
        case 0x88: case 0x89: case 0x8A: case 0x8B:
        case 0x8C: case 0x8D: case 0x8E: case 0x8F: {
            int32_t d = (osize == 2) ? (int16_t)fetch16(c) : (int32_t)fetch32(c);
            if (cond(c, op2 & 15)) ip_set(c, (uint16_t)(ip_of(c) + d));
            break;
        }
        case 0x90: case 0x91: case 0x92: case 0x93:
        case 0x94: case 0x95: case 0x96: case 0x97:
        case 0x98: case 0x99: case 0x9A: case 0x9B:
        case 0x9C: case 0x9D: case 0x9E: case 0x9F: {
            Ea ea;
            decode_ea(c, fetch8(c), asize, &ea);
            ea_write(c, &ea, 1, cond(c, op2 & 15) ? 1u : 0u);
            break;
        }
        case 0xB6: case 0xB7: {                   /* MOVZX */
            Ea ea;
            int ssize = (op2 == 0xB6) ? 1 : 2;
            decode_ea(c, fetch8(c), asize, &ea);
            set_reg(c, ea.reg, osize, ea_read(c, &ea, ssize) & mask_of(ssize));
            break;
        }
        case 0xBE: case 0xBF: {                   /* MOVSX */
            Ea ea;
            int ssize = (op2 == 0xBE) ? 1 : 2;
            decode_ea(c, fetch8(c), asize, &ea);
            set_reg(c, ea.reg, osize,
                    sext(ea_read(c, &ea, ssize), ssize) & mask_of(osize));
            break;
        }
        case 0xAF: {                              /* IMUL r, r/m */
            Ea ea;
            int64_t r;
            decode_ea(c, fetch8(c), asize, &ea);
            r = (int64_t)(int32_t)sext(get_reg(c, ea.reg, osize), osize) *
                (int64_t)(int32_t)sext(ea_read(c, &ea, osize), osize);
            set_reg(c, ea.reg, osize, (uint32_t)r & mask_of(osize));
            c->eflags &= ~(F_CF | F_OF);
            if (osize == 2 ? (r != (int16_t)r) : (r != (int32_t)r))
                c->eflags |= F_CF | F_OF;
            set_zsp(c, (uint32_t)r, osize);
            break;
        }
        case 0xA4: case 0xA5: case 0xAC: case 0xAD: {   /* SHLD / SHRD */
            Ea ea;
            unsigned n;
            uint32_t dst, src, r;
            int left = (op2 < 0xAC);
            unsigned bits = (unsigned)osize * 8;

            decode_ea(c, fetch8(c), asize, &ea);
            n = (op2 == 0xA4 || op2 == 0xAC) ? fetch8(c) : get_reg(c, R_CX, 1);
            n &= 31;
            if (n == 0) break;
            dst = ea_read(c, &ea, osize);
            src = get_reg(c, ea.reg, osize);
            if (n >= bits) { /* undefined on real hardware; keep it defined here */
                r = src;
                c->eflags &= ~F_CF;
            } else if (left) {
                r = ((dst << n) | (src >> (bits - n))) & mask_of(osize);
                c->eflags = (c->eflags & ~F_CF) |
                            (((dst >> (bits - n)) & 1u) ? F_CF : 0);
            } else {
                r = ((dst >> n) | (src << (bits - n))) & mask_of(osize);
                c->eflags = (c->eflags & ~F_CF) |
                            (((dst >> (n - 1)) & 1u) ? F_CF : 0);
            }
            ea_write(c, &ea, osize, r);
            set_zsp(c, r, osize);
            break;
        }
        case 0xA3: case 0xAB: case 0xB3: case 0xBB: {   /* BT/BTS/BTR/BTC */
            Ea ea;
            uint32_t v, bit, n;
            decode_ea(c, fetch8(c), asize, &ea);
            n = get_reg(c, ea.reg, osize) & (osize * 8 - 1);
            v = ea_read(c, &ea, osize);
            bit = 1u << n;
            c->eflags = (c->eflags & ~F_CF) | ((v & bit) ? F_CF : 0);
            if (op2 == 0xAB) v |= bit;
            else if (op2 == 0xB3) v &= ~bit;
            else if (op2 == 0xBB) v ^= bit;
            if (op2 != 0xA3) ea_write(c, &ea, osize, v);
            break;
        }
        case 0xBA: {                              /* BT/BTS/BTR/BTC r/m, imm8 */
            Ea ea;
            uint32_t v, bit;
            unsigned n;
            decode_ea(c, fetch8(c), asize, &ea);
            n = fetch8(c) & (unsigned)(osize * 8 - 1);
            v = ea_read(c, &ea, osize);
            bit = 1u << n;
            c->eflags = (c->eflags & ~F_CF) | ((v & bit) ? F_CF : 0);
            if (ea.reg == 5) v |= bit;
            else if (ea.reg == 6) v &= ~bit;
            else if (ea.reg == 7) v ^= bit;
            else if (ea.reg != 4) { c->state = CPU_BADOP; break; }
            if (ea.reg != 4) ea_write(c, &ea, osize, v);
            break;
        }
        case 0xBC: case 0xBD: {                   /* BSF / BSR */
            Ea ea;
            uint32_t v;
            int i, bits;
            decode_ea(c, fetch8(c), asize, &ea);
            v = ea_read(c, &ea, osize);
            bits = osize * 8;
            if (v == 0) { c->eflags |= F_ZF; break; }
            c->eflags &= ~F_ZF;
            if (op2 == 0xBC) { for (i = 0; i < bits; i++) if (v & (1u << i)) break; }
            else             { for (i = bits - 1; i >= 0; i--) if (v & (1u << i)) break; }
            set_reg(c, ea.reg, osize, (uint32_t)i);
            break;
        }
        case 0xA0: cpu_push16(c, c->seg[S_FS]); break;
        case 0xA1: c->seg[S_FS] = cpu_pop16(c); break;
        case 0xA8: cpu_push16(c, c->seg[S_GS]); break;
        case 0xA9: c->seg[S_GS] = cpu_pop16(c); break;
        case 0xB2: case 0xB4: case 0xB5: {        /* LSS / LFS / LGS */
            Ea ea;
            int sr = (op2 == 0xB2) ? S_SS : (op2 == 0xB4 ? S_FS : S_GS);
            decode_ea(c, fetch8(c), asize, &ea);
            if (ea.is_reg) { c->state = CPU_BADOP; break; }
            set_reg(c, ea.reg, 2, sel_rd16(ea.sel, ea.off));
            c->seg[sr] = sel_rd16(ea.sel, (uint16_t)(ea.off + 2));
            break;
        }
        default:
            c->state = CPU_BADOP;
            c->bad_op = 0x0F;
            c->bad_op2 = op2;
            break;
        }
        break;
    }

    default:
        c->state = CPU_BADOP;
        c->bad_op = op;
        c->bad_op2 = 0;
        break;
    }

    if (c->state == CPU_BADOP || c->state == CPU_FAULT) {
        c->bad_cs = c->seg[S_CS];
        c->bad_ip = ip_of(c);
        if (!c->bad_op) c->bad_op = op;
    }
    if (sel_fault && c->state == CPU_RUNNING) {
        c->state = CPU_FAULT;
        c->bad_cs = c->seg[S_CS];
        c->bad_ip = ip_of(c);
    }
    return c->state;
}

int cpu_run(Cpu *c, uint64_t max)
{
    uint64_t n = 0;

    c->state = CPU_RUNNING;
    for (;;) {
        int r = cpu_step(c);
        if (r != CPU_RUNNING) return r;
        if (max && ++n >= max) {
            c->state = CPU_STEPS;
            return CPU_STEPS;
        }
    }
}
