/* fpu.c - x87 emulation by delegation to the host x87.
 *
 * The guest's eight registers are kept as 80-bit values in host layout, so a
 * guest operation is: load the guest control word, FLD the operands, run the
 * real instruction, read the status word back, FSTP the result.  Rounding,
 * precision control, the condition codes and the transcendentals then all come
 * out exactly right for free.
 *
 * The guest stack is modelled explicitly (top-of-stack index plus tag word)
 * rather than mapped onto the host stack, because the guest can leave values
 * live across arbitrary amounts of host code.
 */

#include "fpu.h"
#include "sel.h"
#include "log.h"

#include <string.h>
#include <math.h>

/* x87 status word condition-code bits. */
#define SW_C0 0x0100u
#define SW_C1 0x0200u
#define SW_C2 0x0400u
#define SW_C3 0x4000u

/* Tag word: two bits per physical register, 3 = empty. */
#define TAG_VALID 0
#define TAG_ZERO  1
#define TAG_SPEC  2
#define TAG_EMPTY 3

static uint16_t host_cw_saved;

void fpu_host_enter(void)
{
    __asm__ volatile ("fnstcw %0" : "=m"(host_cw_saved));
}

void fpu_host_leave(void)
{
    __asm__ volatile ("fldcw %0" : : "m"(host_cw_saved));
}

void fpu_reset(Cpu *c)
{
    memset(c->st, 0, sizeof c->st);
    c->fpu_cw  = 0x037F;      /* round to nearest, extended precision, all masked */
    c->fpu_sw  = 0;
    c->fpu_tw  = 0xFFFF;      /* every register empty */
    c->fpu_top = 0;
}

/* ------------------------------------------------------------ stack plumbing */

static int phys(Cpu *c, int i) { return (c->fpu_top + i) & 7; }

static int tag_of(Cpu *c, int p) { return (c->fpu_tw >> (p * 2)) & 3; }

static void set_tag(Cpu *c, int p, int t)
{
    c->fpu_tw = (uint16_t)((c->fpu_tw & ~(3u << (p * 2))) | ((unsigned)t << (p * 2)));
}

/* Read ST(i) into a host long double. */
static long double ld_get(Cpu *c, int i)
{
    long double v;
    const uint8_t *src = c->st[phys(c, i)].b;
    __asm__ volatile ("fldt %1\n\tfstpt %0" : "=m"(v) : "m"(*src) : "st");
    return v;
}

/* Write a host long double into ST(i) and mark it valid. */
static void ld_set(Cpu *c, int i, long double v)
{
    int p = phys(c, i);
    uint8_t *dst = c->st[p].b;
    __asm__ volatile ("fldt %1\n\tfstpt %0" : "=m"(*dst) : "m"(v) : "st");
    set_tag(c, p, (v == 0.0L) ? TAG_ZERO : TAG_VALID);
}

static void fpu_push(Cpu *c, long double v)
{
    c->fpu_top = (uint8_t)((c->fpu_top - 1) & 7);
    if (tag_of(c, phys(c, 0)) != TAG_EMPTY) {
        /* Stack overflow.  All exceptions are masked in practice, and the
           hardware would push anyway with C1 set. */
        c->fpu_sw |= SW_C1 | 0x0041u;
    }
    ld_set(c, 0, v);
}

static void fpu_discard(Cpu *c)
{
    set_tag(c, phys(c, 0), TAG_EMPTY);
    c->fpu_top = (uint8_t)((c->fpu_top + 1) & 7);
}

/* Status word with the current top-of-stack encoded, as the guest expects. */
static uint16_t sw_value(Cpu *c)
{
    return (uint16_t)((c->fpu_sw & ~0x3800u) | ((unsigned)c->fpu_top << 11));
}

/* --------------------------------------------------------------- host bridge */

/* Run a two-operand host x87 operation under the guest control word.
   `code` selects the operation; both operands are host long doubles. */
enum { OP_ADD, OP_SUB, OP_SUBR, OP_MUL, OP_DIV, OP_DIVR };

static long double host_arith(Cpu *c, int code, long double a, long double b)
{
    long double r;
    uint16_t cw = c->fpu_cw, sw = 0;

    switch (code) {
    case OP_ADD:
        __asm__ volatile ("fldcw %3\n\tfldt %1\n\tfldt %2\n\tfaddp\n\t"
                          "fnstsw %0\n\tfstpt %1"
                          : "=a"(sw), "+m"(a) : "m"(b), "m"(cw) : "st");
        r = a; break;
    case OP_SUB:   /* a - b */
        __asm__ volatile ("fldcw %3\n\tfldt %2\n\tfldt %1\n\tfsubp\n\t"
                          "fnstsw %0\n\tfstpt %1"
                          : "=a"(sw), "+m"(a) : "m"(b), "m"(cw) : "st");
        r = a; break;
    case OP_SUBR:  /* b - a */
        __asm__ volatile ("fldcw %3\n\tfldt %1\n\tfldt %2\n\tfsubp\n\t"
                          "fnstsw %0\n\tfstpt %1"
                          : "=a"(sw), "+m"(a) : "m"(b), "m"(cw) : "st");
        r = a; break;
    case OP_MUL:
        __asm__ volatile ("fldcw %3\n\tfldt %1\n\tfldt %2\n\tfmulp\n\t"
                          "fnstsw %0\n\tfstpt %1"
                          : "=a"(sw), "+m"(a) : "m"(b), "m"(cw) : "st");
        r = a; break;
    case OP_DIV:   /* a / b */
        __asm__ volatile ("fldcw %3\n\tfldt %2\n\tfldt %1\n\tfdivp\n\t"
                          "fnstsw %0\n\tfstpt %1"
                          : "=a"(sw), "+m"(a) : "m"(b), "m"(cw) : "st");
        r = a; break;
    default:       /* OP_DIVR: b / a */
        __asm__ volatile ("fldcw %3\n\tfldt %1\n\tfldt %2\n\tfdivp\n\t"
                          "fnstsw %0\n\tfstpt %1"
                          : "=a"(sw), "+m"(a) : "m"(b), "m"(cw) : "st");
        r = a; break;
    }
    c->fpu_sw = (uint16_t)((c->fpu_sw & 0x3800u) | (sw & ~0x3800u));
    return r;
}

/* Compare and set C3/C2/C0 exactly as FCOM does. */
static void host_compare(Cpu *c, long double a, long double b)
{
    uint16_t sw = 0, cw = c->fpu_cw;

    __asm__ volatile ("fldcw %3\n\tfldt %2\n\tfldt %1\n\tfcompp\n\tfnstsw %0"
                      : "=a"(sw) : "m"(a), "m"(b), "m"(cw) : "st");
    c->fpu_sw = (uint16_t)((c->fpu_sw & ~(SW_C0 | SW_C1 | SW_C2 | SW_C3)) |
                           (sw & (SW_C0 | SW_C1 | SW_C2 | SW_C3)));
}

/* One-operand host operations that need the real instruction. */
enum { U_SQRT, U_RNDINT, U_2XM1, U_TAN, U_ATAN, U_YL2X, U_YL2XP1, U_PREM,
       U_SCALE, U_XTRACT, U_SIN, U_COS, U_ABS, U_CHS, U_TST, U_XAM };

static long double host_unary(Cpu *c, int code, long double a, long double b,
                              long double *second, int *pushed)
{
    uint16_t sw = 0, cw = c->fpu_cw;
    long double r = a;

    *pushed = 0;
    switch (code) {
    case U_SQRT:
        __asm__ volatile ("fldcw %2\n\tfldt %1\n\tfsqrt\n\tfnstsw %0\n\tfstpt %1"
                          : "=a"(sw), "+m"(r) : "m"(cw) : "st");
        break;
    case U_RNDINT:
        __asm__ volatile ("fldcw %2\n\tfldt %1\n\tfrndint\n\tfnstsw %0\n\tfstpt %1"
                          : "=a"(sw), "+m"(r) : "m"(cw) : "st");
        break;
    case U_2XM1:
        __asm__ volatile ("fldcw %2\n\tfldt %1\n\tf2xm1\n\tfnstsw %0\n\tfstpt %1"
                          : "=a"(sw), "+m"(r) : "m"(cw) : "st");
        break;
    case U_ABS:
        __asm__ volatile ("fldt %1\n\tfabs\n\tfnstsw %0\n\tfstpt %1"
                          : "=a"(sw), "+m"(r) : : "st");
        break;
    case U_CHS:
        __asm__ volatile ("fldt %1\n\tfchs\n\tfnstsw %0\n\tfstpt %1"
                          : "=a"(sw), "+m"(r) : : "st");
        break;
    case U_TAN:   /* FPTAN: replaces ST with tan(ST) then pushes 1.0 */
        __asm__ volatile ("fldcw %3\n\tfldt %1\n\tfptan\n\tfnstsw %0\n\t"
                          "fstpt %2\n\tfstpt %1"
                          : "=a"(sw), "+m"(r), "=m"(*second) : "m"(cw) : "st");
        *pushed = 1;
        break;
    case U_ATAN:  /* FPATAN: atan(ST(1)/ST(0)), pops one */
        __asm__ volatile ("fldcw %3\n\tfldt %2\n\tfldt %1\n\tfpatan\n\t"
                          "fnstsw %0\n\tfstpt %1"
                          : "=a"(sw), "+m"(r) : "m"(b), "m"(cw) : "st");
        break;
    case U_YL2X:  /* ST(1) * log2(ST(0)), pops one */
        __asm__ volatile ("fldcw %3\n\tfldt %2\n\tfldt %1\n\tfyl2x\n\t"
                          "fnstsw %0\n\tfstpt %1"
                          : "=a"(sw), "+m"(r) : "m"(b), "m"(cw) : "st");
        break;
    case U_YL2XP1:
        __asm__ volatile ("fldcw %3\n\tfldt %2\n\tfldt %1\n\tfyl2xp1\n\t"
                          "fnstsw %0\n\tfstpt %1"
                          : "=a"(sw), "+m"(r) : "m"(b), "m"(cw) : "st");
        break;
    case U_PREM:  /* ST(0) remainder ST(1) */
        __asm__ volatile ("fldcw %3\n\tfldt %2\n\tfldt %1\n\tfprem\n\t"
                          "fnstsw %0\n\tfstpt %1\n\tfstp %%st(0)"
                          : "=a"(sw), "+m"(r) : "m"(b), "m"(cw) : "st");
        break;
    case U_SCALE: /* ST(0) * 2^trunc(ST(1)) */
        __asm__ volatile ("fldcw %3\n\tfldt %2\n\tfldt %1\n\tfscale\n\t"
                          "fnstsw %0\n\tfstpt %1\n\tfstp %%st(0)"
                          : "=a"(sw), "+m"(r) : "m"(b), "m"(cw) : "st");
        break;
    case U_XTRACT:
        __asm__ volatile ("fldcw %3\n\tfldt %1\n\tfxtract\n\tfnstsw %0\n\t"
                          "fstpt %1\n\tfstpt %2"
                          : "=a"(sw), "+m"(r), "=m"(*second) : "m"(cw) : "st");
        *pushed = 1;
        break;
    case U_SIN:
        __asm__ volatile ("fldcw %2\n\tfldt %1\n\tfsin\n\tfnstsw %0\n\tfstpt %1"
                          : "=a"(sw), "+m"(r) : "m"(cw) : "st");
        break;
    case U_COS:
        __asm__ volatile ("fldcw %2\n\tfldt %1\n\tfcos\n\tfnstsw %0\n\tfstpt %1"
                          : "=a"(sw), "+m"(r) : "m"(cw) : "st");
        break;
    case U_TST:
        __asm__ volatile ("fldt %1\n\tftst\n\tfnstsw %0\n\tfstp %%st(0)"
                          : "=a"(sw) : "m"(r) : "st");
        break;
    default:      /* U_XAM */
        __asm__ volatile ("fldt %1\n\tfxam\n\tfnstsw %0\n\tfstp %%st(0)"
                          : "=a"(sw) : "m"(r) : "st");
        break;
    }
    c->fpu_sw = (uint16_t)((c->fpu_sw & 0x3800u) | (sw & ~0x3800u));
    return r;
}

/* ------------------------------------------------------- memory load and store */

static long double load_f32(uint16_t sel, uint16_t off)
{
    float f;
    uint32_t bits = sel_rd32(sel, off);
    memcpy(&f, &bits, 4);
    return (long double)f;
}

static long double load_f64(uint16_t sel, uint16_t off)
{
    double d;
    uint8_t b[8];
    int i;
    for (i = 0; i < 8; i++) b[i] = sel_rd8(sel, (uint16_t)(off + i));
    memcpy(&d, b, 8);
    return (long double)d;
}

static long double load_f80(uint16_t sel, uint16_t off)
{
    uint8_t b[10];
    long double v;
    int i;
    for (i = 0; i < 10; i++) b[i] = sel_rd8(sel, (uint16_t)(off + i));
    __asm__ volatile ("fldt %1\n\tfstpt %0" : "=m"(v) : "m"(*b) : "st");
    return v;
}

static void store_f32(uint16_t sel, uint16_t off, long double v, uint16_t cw)
{
    float f;
    uint32_t bits;
    __asm__ volatile ("fldcw %2\n\tfldt %1\n\tfstps %0"
                      : "=m"(f) : "m"(v), "m"(cw) : "st");
    memcpy(&bits, &f, 4);
    sel_wr32(sel, off, bits);
}

static void store_f64(uint16_t sel, uint16_t off, long double v, uint16_t cw)
{
    double d;
    uint8_t b[8];
    int i;
    __asm__ volatile ("fldcw %2\n\tfldt %1\n\tfstpl %0"
                      : "=m"(d) : "m"(v), "m"(cw) : "st");
    memcpy(b, &d, 8);
    for (i = 0; i < 8; i++) sel_wr8(sel, (uint16_t)(off + i), b[i]);
}

static void store_f80(uint16_t sel, uint16_t off, long double v)
{
    uint8_t b[10];
    int i;
    __asm__ volatile ("fldt %1\n\tfstpt %0" : "=m"(*b) : "m"(v) : "st");
    for (i = 0; i < 10; i++) sel_wr8(sel, (uint16_t)(off + i), b[i]);
}

/* Integer conversions go through the host too, so the guest rounding mode in the
   control word decides how FIST rounds - which is exactly what C code relies on
   after setting the control word for truncation. */
static int64_t to_int64(long double v, uint16_t cw)
{
    int64_t r;
    __asm__ volatile ("fldcw %2\n\tfldt %1\n\tfistpll %0"
                      : "=m"(r) : "m"(v), "m"(cw) : "st");
    return r;
}

/* ------------------------------------------------------------------ dispatch */

int fpu_exec(Cpu *c, uint8_t op, uint8_t modrm, int is_reg,
             uint16_t sel, uint16_t off)
{
    int reg = (modrm >> 3) & 7;
    int rm  = modrm & 7;
    uint16_t cw = c->fpu_cw;

    if (!is_reg) {
        /* ---- memory forms ---- */
        switch (op) {
        case 0xD8: {                               /* arithmetic with m32 */
            long double m = load_f32(sel, off);
            switch (reg) {
            case 0: ld_set(c, 0, host_arith(c, OP_ADD,  ld_get(c, 0), m)); return 1;
            case 1: ld_set(c, 0, host_arith(c, OP_MUL,  ld_get(c, 0), m)); return 1;
            case 2: host_compare(c, ld_get(c, 0), m); return 1;
            case 3: host_compare(c, ld_get(c, 0), m); fpu_discard(c); return 1;
            case 4: ld_set(c, 0, host_arith(c, OP_SUB,  ld_get(c, 0), m)); return 1;
            case 5: ld_set(c, 0, host_arith(c, OP_SUBR, ld_get(c, 0), m)); return 1;
            case 6: ld_set(c, 0, host_arith(c, OP_DIV,  ld_get(c, 0), m)); return 1;
            default:ld_set(c, 0, host_arith(c, OP_DIVR, ld_get(c, 0), m)); return 1;
            }
        }
        case 0xD9:
            switch (reg) {
            case 0: fpu_push(c, load_f32(sel, off)); return 1;      /* FLD m32   */
            case 2: store_f32(sel, off, ld_get(c, 0), cw); return 1; /* FST m32  */
            case 3: store_f32(sel, off, ld_get(c, 0), cw);
                    fpu_discard(c); return 1;                        /* FSTP m32 */
            case 4: /* FLDENV: restore the 14-byte 16-bit environment */
                c->fpu_cw = sel_rd16(sel, off);
                c->fpu_sw = sel_rd16(sel, (uint16_t)(off + 2));
                c->fpu_tw = sel_rd16(sel, (uint16_t)(off + 4));
                c->fpu_top = (uint8_t)((c->fpu_sw >> 11) & 7);
                return 1;
            case 5: c->fpu_cw = sel_rd16(sel, off); return 1;        /* FLDCW    */
            case 6: /* FSTENV */
                sel_wr16(sel, off, c->fpu_cw);
                sel_wr16(sel, (uint16_t)(off + 2), sw_value(c));
                sel_wr16(sel, (uint16_t)(off + 4), c->fpu_tw);
                sel_wr16(sel, (uint16_t)(off + 6), 0);
                sel_wr16(sel, (uint16_t)(off + 8), 0);
                sel_wr16(sel, (uint16_t)(off + 10), 0);
                sel_wr16(sel, (uint16_t)(off + 12), 0);
                c->fpu_cw |= 0x3Fu;                 /* FSTENV masks everything */
                return 1;
            case 7: sel_wr16(sel, off, c->fpu_cw); return 1;        /* FNSTCW   */
            default: return 0;
            }
        case 0xDA: {                               /* integer m32 arithmetic */
            long double m = (long double)(int32_t)sel_rd32(sel, off);
            switch (reg) {
            case 0: ld_set(c, 0, host_arith(c, OP_ADD,  ld_get(c, 0), m)); return 1;
            case 1: ld_set(c, 0, host_arith(c, OP_MUL,  ld_get(c, 0), m)); return 1;
            case 2: host_compare(c, ld_get(c, 0), m); return 1;
            case 3: host_compare(c, ld_get(c, 0), m); fpu_discard(c); return 1;
            case 4: ld_set(c, 0, host_arith(c, OP_SUB,  ld_get(c, 0), m)); return 1;
            case 5: ld_set(c, 0, host_arith(c, OP_SUBR, ld_get(c, 0), m)); return 1;
            case 6: ld_set(c, 0, host_arith(c, OP_DIV,  ld_get(c, 0), m)); return 1;
            default:ld_set(c, 0, host_arith(c, OP_DIVR, ld_get(c, 0), m)); return 1;
            }
        }
        case 0xDB:
            switch (reg) {
            case 0: fpu_push(c, (long double)(int32_t)sel_rd32(sel, off)); return 1;
            case 2: {                              /* FIST m32 */
                int64_t v = to_int64(ld_get(c, 0), cw);
                sel_wr32(sel, off, (uint32_t)(int32_t)v);
                return 1;
            }
            case 3: {                              /* FISTP m32 */
                int64_t v = to_int64(ld_get(c, 0), cw);
                sel_wr32(sel, off, (uint32_t)(int32_t)v);
                fpu_discard(c);
                return 1;
            }
            case 5: fpu_push(c, load_f80(sel, off)); return 1;      /* FLD m80  */
            case 7: store_f80(sel, off, ld_get(c, 0));
                    fpu_discard(c); return 1;                        /* FSTP m80 */
            default: return 0;
            }
        case 0xDC: {                               /* arithmetic with m64 */
            long double m = load_f64(sel, off);
            switch (reg) {
            case 0: ld_set(c, 0, host_arith(c, OP_ADD,  ld_get(c, 0), m)); return 1;
            case 1: ld_set(c, 0, host_arith(c, OP_MUL,  ld_get(c, 0), m)); return 1;
            case 2: host_compare(c, ld_get(c, 0), m); return 1;
            case 3: host_compare(c, ld_get(c, 0), m); fpu_discard(c); return 1;
            case 4: ld_set(c, 0, host_arith(c, OP_SUB,  ld_get(c, 0), m)); return 1;
            case 5: ld_set(c, 0, host_arith(c, OP_SUBR, ld_get(c, 0), m)); return 1;
            case 6: ld_set(c, 0, host_arith(c, OP_DIV,  ld_get(c, 0), m)); return 1;
            default:ld_set(c, 0, host_arith(c, OP_DIVR, ld_get(c, 0), m)); return 1;
            }
        }
        case 0xDD:
            switch (reg) {
            case 0: fpu_push(c, load_f64(sel, off)); return 1;      /* FLD m64  */
            case 2: store_f64(sel, off, ld_get(c, 0), cw); return 1; /* FST m64 */
            case 3: store_f64(sel, off, ld_get(c, 0), cw);
                    fpu_discard(c); return 1;                        /* FSTP m64 */
            case 7: sel_wr16(sel, off, sw_value(c)); return 1;       /* FNSTSW m16 */
            default: return 0;
            }
        case 0xDE: {                               /* integer m16 arithmetic */
            long double m = (long double)(int16_t)sel_rd16(sel, off);
            switch (reg) {
            case 0: ld_set(c, 0, host_arith(c, OP_ADD,  ld_get(c, 0), m)); return 1;
            case 1: ld_set(c, 0, host_arith(c, OP_MUL,  ld_get(c, 0), m)); return 1;
            case 2: host_compare(c, ld_get(c, 0), m); return 1;
            case 3: host_compare(c, ld_get(c, 0), m); fpu_discard(c); return 1;
            case 4: ld_set(c, 0, host_arith(c, OP_SUB,  ld_get(c, 0), m)); return 1;
            case 5: ld_set(c, 0, host_arith(c, OP_SUBR, ld_get(c, 0), m)); return 1;
            case 6: ld_set(c, 0, host_arith(c, OP_DIV,  ld_get(c, 0), m)); return 1;
            default:ld_set(c, 0, host_arith(c, OP_DIVR, ld_get(c, 0), m)); return 1;
            }
        }
        case 0xDF:
            switch (reg) {
            case 0: fpu_push(c, (long double)(int16_t)sel_rd16(sel, off)); return 1;
            case 2: {                              /* FIST m16 */
                int64_t v = to_int64(ld_get(c, 0), cw);
                sel_wr16(sel, off, (uint16_t)(int16_t)v);
                return 1;
            }
            case 3: {                              /* FISTP m16 */
                int64_t v = to_int64(ld_get(c, 0), cw);
                sel_wr16(sel, off, (uint16_t)(int16_t)v);
                fpu_discard(c);
                return 1;
            }
            case 5: {                              /* FILD m64 */
                uint8_t b[8];
                int64_t v;
                int i;
                for (i = 0; i < 8; i++) b[i] = sel_rd8(sel, (uint16_t)(off + i));
                memcpy(&v, b, 8);
                fpu_push(c, (long double)v);
                return 1;
            }
            case 7: {                              /* FISTP m64 */
                int64_t v = to_int64(ld_get(c, 0), cw);
                uint8_t b[8];
                int i;
                memcpy(b, &v, 8);
                for (i = 0; i < 8; i++) sel_wr8(sel, (uint16_t)(off + i), b[i]);
                fpu_discard(c);
                return 1;
            }
            default: return 0;
            }
        default:
            return 0;
        }
    }

    /* ---- register forms ---- */
    switch (op) {
    case 0xD8:                                     /* op ST(0), ST(i) */
        switch (reg) {
        case 0: ld_set(c, 0, host_arith(c, OP_ADD,  ld_get(c, 0), ld_get(c, rm))); return 1;
        case 1: ld_set(c, 0, host_arith(c, OP_MUL,  ld_get(c, 0), ld_get(c, rm))); return 1;
        case 2: host_compare(c, ld_get(c, 0), ld_get(c, rm)); return 1;
        case 3: host_compare(c, ld_get(c, 0), ld_get(c, rm)); fpu_discard(c); return 1;
        case 4: ld_set(c, 0, host_arith(c, OP_SUB,  ld_get(c, 0), ld_get(c, rm))); return 1;
        case 5: ld_set(c, 0, host_arith(c, OP_SUBR, ld_get(c, 0), ld_get(c, rm))); return 1;
        case 6: ld_set(c, 0, host_arith(c, OP_DIV,  ld_get(c, 0), ld_get(c, rm))); return 1;
        default:ld_set(c, 0, host_arith(c, OP_DIVR, ld_get(c, 0), ld_get(c, rm))); return 1;
        }
    case 0xD9:
        switch (reg) {
        case 0: fpu_push(c, ld_get(c, rm == 0 ? 0 : rm)); return 1;   /* FLD ST(i) */
        case 1: {                                                     /* FXCH      */
            long double a = ld_get(c, 0), b = ld_get(c, rm);
            ld_set(c, 0, b);
            ld_set(c, rm, a);
            return 1;
        }
        case 2: if (rm == 0) return 1; return 0;                      /* FNOP      */
        case 3: fpu_discard(c); return 1;                             /* FSTP ST(i)*/
        case 4: {
            int pushed;
            long double a = ld_get(c, 0), second = 0;
            switch (rm) {
            case 0: ld_set(c, 0, host_unary(c, U_CHS, a, 0, &second, &pushed)); return 1;
            case 1: ld_set(c, 0, host_unary(c, U_ABS, a, 0, &second, &pushed)); return 1;
            case 4: host_unary(c, U_TST, a, 0, &second, &pushed); return 1;
            case 5: host_unary(c, U_XAM, a, 0, &second, &pushed); return 1;
            default: return 0;
            }
        }
        case 5:                                                        /* constants */
            switch (rm) {
            case 0: fpu_push(c, 1.0L); return 1;                       /* FLD1     */
            case 1: fpu_push(c, 3.3219280948873623478703194294894L); return 1; /* L2T */
            case 2: fpu_push(c, 1.4426950408889634073599246810019L); return 1; /* L2E */
            case 3: fpu_push(c, 3.1415926535897932384626433832795L); return 1; /* PI  */
            case 4: fpu_push(c, 0.3010299956639811952137388947245L); return 1; /* LG2 */
            case 5: fpu_push(c, 0.6931471805599453094172321214582L); return 1; /* LN2 */
            case 6: fpu_push(c, 0.0L); return 1;                       /* FLDZ     */
            default: return 0;
            }
        case 6: {                                                      /* FPxx     */
            int pushed = 0;
            long double a = ld_get(c, 0), second = 0, r;
            switch (rm) {
            case 0: r = host_unary(c, U_2XM1, a, 0, &second, &pushed);
                    ld_set(c, 0, r); return 1;
            case 1: r = host_unary(c, U_YL2X, a, ld_get(c, 1), &second, &pushed);
                    fpu_discard(c); ld_set(c, 0, r); return 1;
            case 2: r = host_unary(c, U_TAN, a, 0, &second, &pushed);
                    ld_set(c, 0, r); fpu_push(c, second); return 1;
            case 3: r = host_unary(c, U_ATAN, a, ld_get(c, 1), &second, &pushed);
                    fpu_discard(c); ld_set(c, 0, r); return 1;
            case 4: r = host_unary(c, U_XTRACT, a, 0, &second, &pushed);
                    ld_set(c, 0, second); fpu_push(c, r); return 1;
            case 6: c->fpu_top = (uint8_t)((c->fpu_top - 1) & 7); return 1; /* FDECSTP */
            case 7: c->fpu_top = (uint8_t)((c->fpu_top + 1) & 7); return 1; /* FINCSTP */
            default: return 0;
            }
        }
        case 7: {
            int pushed = 0;
            long double a = ld_get(c, 0), second = 0, r;
            switch (rm) {
            case 0: r = host_unary(c, U_PREM, a, ld_get(c, 1), &second, &pushed);
                    ld_set(c, 0, r); return 1;
            case 1: r = host_unary(c, U_YL2XP1, a, ld_get(c, 1), &second, &pushed);
                    fpu_discard(c); ld_set(c, 0, r); return 1;
            case 2: r = host_unary(c, U_SQRT, a, 0, &second, &pushed);
                    ld_set(c, 0, r); return 1;
            case 4: r = host_unary(c, U_RNDINT, a, 0, &second, &pushed);
                    ld_set(c, 0, r); return 1;
            case 5: r = host_unary(c, U_SCALE, a, ld_get(c, 1), &second, &pushed);
                    ld_set(c, 0, r); return 1;
            case 6: r = host_unary(c, U_SIN, a, 0, &second, &pushed);
                    ld_set(c, 0, r); return 1;
            case 7: r = host_unary(c, U_COS, a, 0, &second, &pushed);
                    ld_set(c, 0, r); return 1;
            default: return 0;
            }
        }
        default: return 0;
        }
    case 0xDA:
        return 0;                                  /* FCMOVcc: 686, not emitted */
    case 0xDB:
        if (reg == 4 && rm == 2) { c->fpu_sw &= ~0x80FFu; return 1; }  /* FCLEX */
        if (reg == 4 && rm == 3) { fpu_reset(c); return 1; }           /* FINIT */
        return 0;
    case 0xDC:                                     /* op ST(i), ST(0) */
        switch (reg) {
        case 0: ld_set(c, rm, host_arith(c, OP_ADD,  ld_get(c, rm), ld_get(c, 0))); return 1;
        case 1: ld_set(c, rm, host_arith(c, OP_MUL,  ld_get(c, rm), ld_get(c, 0))); return 1;
        case 4: ld_set(c, rm, host_arith(c, OP_SUBR, ld_get(c, rm), ld_get(c, 0))); return 1;
        case 5: ld_set(c, rm, host_arith(c, OP_SUB,  ld_get(c, rm), ld_get(c, 0))); return 1;
        case 6: ld_set(c, rm, host_arith(c, OP_DIVR, ld_get(c, rm), ld_get(c, 0))); return 1;
        case 7: ld_set(c, rm, host_arith(c, OP_DIV,  ld_get(c, rm), ld_get(c, 0))); return 1;
        default: return 0;
        }
    case 0xDD:
        switch (reg) {
        case 0: set_tag(c, phys(c, rm), TAG_EMPTY); return 1;          /* FFREE   */
        case 2: ld_set(c, rm, ld_get(c, 0)); return 1;                 /* FST ST(i)*/
        case 3: ld_set(c, rm, ld_get(c, 0)); fpu_discard(c); return 1;  /* FSTP    */
        case 4: host_compare(c, ld_get(c, 0), ld_get(c, rm)); return 1; /* FUCOM  */
        case 5: host_compare(c, ld_get(c, 0), ld_get(c, rm));
                fpu_discard(c); return 1;                               /* FUCOMP */
        default: return 0;
        }
    case 0xDE:                                     /* op ST(i), ST(0) then pop */
        switch (reg) {
        case 0: ld_set(c, rm, host_arith(c, OP_ADD,  ld_get(c, rm), ld_get(c, 0)));
                fpu_discard(c); return 1;
        case 1: ld_set(c, rm, host_arith(c, OP_MUL,  ld_get(c, rm), ld_get(c, 0)));
                fpu_discard(c); return 1;
        case 3: if (rm == 1) {                     /* FCOMPP */
                    host_compare(c, ld_get(c, 0), ld_get(c, 1));
                    fpu_discard(c);
                    fpu_discard(c);
                    return 1;
                }
                return 0;
        case 4: ld_set(c, rm, host_arith(c, OP_SUBR, ld_get(c, rm), ld_get(c, 0)));
                fpu_discard(c); return 1;
        case 5: ld_set(c, rm, host_arith(c, OP_SUB,  ld_get(c, rm), ld_get(c, 0)));
                fpu_discard(c); return 1;
        case 6: ld_set(c, rm, host_arith(c, OP_DIVR, ld_get(c, rm), ld_get(c, 0)));
                fpu_discard(c); return 1;
        default:ld_set(c, rm, host_arith(c, OP_DIV,  ld_get(c, rm), ld_get(c, 0)));
                fpu_discard(c); return 1;
        }
    case 0xDF:
        if (reg == 4 && rm == 0) {                 /* FNSTSW AX */
            c->r32[R_AX] = (c->r32[R_AX] & 0xFFFF0000u) | sw_value(c);
            return 1;
        }
        return 0;
    default:
        return 0;
    }
}
