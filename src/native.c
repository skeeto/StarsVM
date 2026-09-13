/* native.c - see native.h.  The table of sites comes first, the routines it
   names come last, and between them is the machinery that installs,
   dispatches and verifies them. */

#include "native.h"
#include "sel.h"
#include "log.h"
#include "prof.h"
#include "thunk.h"
#include "task.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    unsigned    segno;
    uint16_t    off;
    const char *name;
    NativeFn    fn;
    uint8_t     sig[8];        /* what must be at the site for fn to be true */
    /* filled in by native_install */
    uint16_t    sel;
    uint8_t     orig;          /* the byte 0xD6 replaced */
    int         installed;
    uint64_t    hits;
} NativeSite;

/* ---- the sites ------------------------------------------------------------
   Each routine is documented in docs/natives.md with the disassembly it
   stands in for, where it stops, and what is live there.  The last entry is
   a sentinel. */

static int nat_rand_long(Cpu *c);
static int nat_rand_mod(Cpu *c);
static int nat_scan_loop(Cpu *c);
static int nat_habitability(Cpu *c);
static int nat_tech_check(Cpu *c);
static int nat_byte_at_3e(Cpu *c);
static int nat_sqrt(Cpu *c);
static int nat_ftol(Cpu *c);

static NativeSite sites[] = {
    { 37, 0x0DC2, "sqrt", nat_sqrt,
      { 0xBA, 0x58, 0x18, 0xE9, 0xC6, 0x0E, 0xBA, 0xA2 }, 0, 0, 0, 0 },
    { 37, 0x0E40, "ftol", nat_ftol,
      { 0x8C, 0xD8, 0x90, 0x45, 0x55, 0x8B, 0xEC, 0x1E }, 0, 0, 0, 0 },
    { 9, 0x1940, "rand_long", nat_rand_long,
      { 0xC8, 0x08, 0x00, 0x00, 0x57, 0x56, 0x66, 0xA1 }, 0, 0, 0, 0 },
    { 9, 0x1652, "rand_mod", nat_rand_mod,
      { 0xC8, 0x0C, 0x00, 0x00, 0x57, 0x56, 0x66, 0xA1 }, 0, 0, 0, 0 },
    { 8, 0x2A01, "scan_loop", nat_scan_loop,
      { 0x8B, 0x46, 0x08, 0x2B, 0x44, 0x02, 0x89, 0x46 }, 0, 0, 0, 0 },
    { 10, 0x490E, "habitability", nat_habitability,
      { 0xC8, 0x1A, 0x00, 0x00, 0x57, 0x56, 0x66, 0x2B }, 0, 0, 0, 0 },
    { 2, 0x5916, "tech_check", nat_tech_check,
      { 0xC8, 0x0A, 0x00, 0x00, 0x57, 0x56, 0x33, 0xF6 }, 0, 0, 0, 0 },
    { 29, 0x222C, "byte_at_3e", nat_byte_at_3e,
      { 0x55, 0x8B, 0xEC, 0x56, 0x8B, 0x5E, 0x06, 0x8B }, 0, 0, 0, 0 },
    { 0, 0, NULL, NULL, { 0 }, 0, 0, 0, 0 },
};

#define NSITES ((unsigned)(sizeof sites / sizeof sites[0]) - 1u)

uint64_t native_calls, native_instrs;

static int      disabled;
static unsigned verify_every;
static int      verifying;     /* a check in progress: do not nest another */

/* ---- lookup ---------------------------------------------------------------
   Every dispatch pays for this, so it is a small direct-mapped table over the
   packed address rather than a scan of the site list. */
#define NAT_HASH 256
static NativeSite *nat_map[NAT_HASH];

static unsigned nat_slot(uint32_t segptr)
{
    return (segptr ^ (segptr >> 16) ^ (segptr >> 5)) & (NAT_HASH - 1);
}

static NativeSite *nat_find(uint16_t sel, uint16_t off)
{
    unsigned h = nat_slot(SEGPTR(sel, off));
    NativeSite *s;
    while ((s = nat_map[h]) != NULL) {
        if (s->sel == sel && s->off == off) return s;
        h = (h + 1) & (NAT_HASH - 1);
    }
    return NULL;
}

/* ---- verification --------------------------------------------------------- */

/* The committed arena, slot by slot.  The live set is recorded with it, so a
   restore puts back exactly what was taken and a compare walks the same
   slots on both sides. */
typedef struct {
    uint8_t  *buf;
    uint16_t *slots;
    unsigned  n;
} Snap;

static void snap_take(Snap *s)
{
    unsigned i, n = 0;
    for (i = 1; i < SEL_SLOTS; i++) n += sel_live[i] != 0;
    s->n = n;
    s->slots = realloc(s->slots, n * sizeof *s->slots);
    s->buf = realloc(s->buf, (size_t)n * SEL_SLOT);
    n = 0;
    for (i = 1; i < SEL_SLOTS; i++) {
        if (!sel_live[i]) continue;
        s->slots[n] = (uint16_t)i;
        memcpy(s->buf + (size_t)n * SEL_SLOT, sel_arena + ((size_t)i << 16), SEL_SLOT);
        n++;
    }
}

static void snap_restore(const Snap *s)
{
    unsigned k;
    for (k = 0; k < s->n; k++)
        memcpy(sel_arena + ((size_t)s->slots[k] << 16),
               s->buf + (size_t)k * SEL_SLOT, SEL_SLOT);
}

/* First byte at which the arena now differs from the snapshot; 0 if none.
   The stack below SP at the stopping point is dead - nothing can read it
   without first pushing over it - so the words a callee's frame left there
   are not part of the state being compared.  That is what lets a routine
   stand in for a function that calls into the C runtime without also
   reproducing the runtime's every temporary.  Only the stack: DGROUP below
   the stack's floor is the program's data. */
static uint32_t snap_diff(const Snap *s, uint16_t ss, uint16_t sp)
{
    unsigned k;
    for (k = 0; k < s->n; k++) {
        const uint8_t *a = s->buf + (size_t)k * SEL_SLOT;
        const uint8_t *b = sel_arena + ((size_t)s->slots[k] << 16);
        unsigned off, lo = 0, hi = 0;
        if (!memcmp(a, b, SEL_SLOT)) continue;
        if (SEL_MAKE(s->slots[k]) == ss && sp > task.stacktop) {
            lo = task.stacktop;
            hi = sp;
        }
        for (off = 0; off < SEL_SLOT; off++)
            if (a[off] != b[off] && !(off >= lo && off < hi)) break;
        if (off == SEL_SLOT) continue;
        return SEGPTR(SEL_MAKE(s->slots[k]), off);
    }
    return 0;
}

/* The replay runs with the site unpatched, so the snapshot it is compared
   against must show the site as the guest sees it. */
static void snap_poke(Snap *s, uint16_t sel, uint16_t off, uint8_t v)
{
    unsigned k;
    for (k = 0; k < s->n; k++)
        if (s->slots[k] == SEL_INDEX(sel)) { s->buf[(size_t)k * SEL_SLOT + off] = v; return; }
}

/* The machine, less the bookkeeping: what a routine is answerable for. */
static int cpu_same(const Cpu *a, const Cpu *b)
{
    return !memcmp(a->r32, b->r32, sizeof a->r32) &&
           !memcmp(a->seg, b->seg, sizeof a->seg) &&
           a->eip == b->eip && a->eflags == b->eflags &&
           !memcmp(a->st, b->st, sizeof a->st) &&
           a->fpu_cw == b->fpu_cw && a->fpu_sw == b->fpu_sw &&
           a->fpu_tw == b->fpu_tw && a->fpu_top == b->fpu_top;
}

static void dump_cpu(const char *who, const Cpu *c)
{
    log_msg("    %-8s cs:ip %04X:%04X ax=%08X bx=%08X cx=%08X dx=%08X"
            " si=%08X di=%08X bp=%08X sp=%08X ds=%04X es=%04X ss=%04X fl=%04X\n",
            who, c->seg[S_CS], (unsigned)c->eip, c->r32[R_AX], c->r32[R_BX],
            c->r32[R_CX], c->r32[R_DX], c->r32[R_SI], c->r32[R_DI],
            c->r32[R_BP], c->r32[R_SP], c->seg[S_DS], c->seg[S_ES],
            c->seg[S_SS], (unsigned)(c->eflags & 0xFFFF));
}

/* The x87 registers by physical slot, as values, with the control, status
   and tag words and the top: a stale value in a popped slot is what a
   routine's last computation was. */
static void dump_x87(const char *who, const Cpu *c)
{
    int i;
    log_msg("    %-8s x87 cw=%04X sw=%04X tw=%04X top=%u:", who, c->fpu_cw,
            c->fpu_sw, c->fpu_tw, c->fpu_top);
    for (i = 0; i < 8; i++) {
        long double v;
        memcpy(&v, c->st[i].b, 10);
        /* As a double: this printf's long double is MSVCRT's, which is a
           double, so %Lg would read the ten bytes as something else. */
        log_msg(" %.10g", (double)v);
    }
    log_msg("\n");
}

static void nat_account(NativeSite *s, int n)
{
    s->hits++;
    native_calls++;
    native_instrs += (uint64_t)n;
    prof_native((unsigned)(s - sites), (uint32_t)n);
}

/* Run the routine, then the code it replaced from the same state, and demand
   that the interpreter pass through the routine's stopping point with the
   same machine.  A loop routine may stop at its own head after k iterations;
   the interpreter passes the head k times with other registers first, which
   is why the comparison is made after every step and memory is compared only
   once the registers agree.  The interpreter runs with the site unpatched, so
   the head runs as guest code; any other site it meets runs natively, which
   is what it would do in a real run.

   A mismatch is a bug in the routine and stops the machine: a routine that is
   wrong once is wrong, and a game that goes on from there is a game whose
   turns cannot be trusted. */
#define VERIFY_BUDGET 4000000u

static int native_checked(Cpu *c, NativeSite *s, uint8_t *orig)
{
    static Snap before, after;
    Cpu c0 = *c, c1, first;
    uint32_t stop, diff = 0, firstdiff = 0;
    unsigned steps;
    int n, r = CPU_RUNNING, matched = 0, seen = 0;

    /* The hook is called with the opcode byte already fetched; the routine
       does not care, but the replay must start at the site itself. */
    c0.eip = s->off;
    verifying = 1;
    snap_take(&before);
    n = s->fn(c);
    if (n <= 0) {
        verifying = 0;
        *orig = s->orig;
        return 0;
    }
    c1 = *c;
    snap_take(&after);
    snap_poke(&after, s->sel, s->off, s->orig);
    stop = SEGPTR(c1.seg[S_CS], (uint16_t)c1.eip);

    *c = c0;
    snap_restore(&before);
    sel_wr8(s->sel, s->off, s->orig);
    for (steps = 0; steps < VERIFY_BUDGET; steps++) {
        r = cpu_step(c);
        if (r != CPU_RUNNING) break;
        if (c->seg[S_CS] == thunk_selector()) break;    /* an API call: too far */
        if (SEGPTR(c->seg[S_CS], (uint16_t)c->eip) != stop) continue;
        if (!seen) {
            first = *c;
            firstdiff = snap_diff(&after, c1.seg[S_SS], reg16(&c1, R_SP));
            seen = 1;
        }
        if (!cpu_same(c, &c1)) continue;
        diff = snap_diff(&after, c1.seg[S_SS], reg16(&c1, R_SP));
        if (!diff) { matched = 1; break; }
        /* Same registers, different memory: keep going in case the guest is
           merely passing through with a store still to come. */
    }
    sel_wr8(s->sel, s->off, 0xD6);
    verifying = 0;

    if (matched) {
        c->icount = c1.icount;
        nat_account(s, n);
        return 1;
    }

    log_msg("*** native %s at %04X:%04X disagrees with the interpreter\n",
            s->name, s->sel, s->off);
    dump_cpu("entered", &c0);
    dump_cpu("native", &c1);
    if (seen) {
        static const char *rn[8] = { "eax","ecx","edx","ebx","esp","ebp","esi","edi" };
        static const char *sn[6] = { "es","cs","ss","ds","fs","gs" };
        int k;
        dump_cpu("guest", &first);
        log_msg("    at the stopping point the first time the guest reached it;"
                " differing:");
        for (k = 0; k < 8; k++)
            if (first.r32[k] != c1.r32[k]) log_msg(" %s", rn[k]);
        for (k = 0; k < 6; k++)
            if (first.seg[k] != c1.seg[k]) log_msg(" %s", sn[k]);
        if (first.eflags != c1.eflags) log_msg(" eflags");
        if (memcmp(first.st, c1.st, sizeof first.st) || first.fpu_cw != c1.fpu_cw ||
            first.fpu_sw != c1.fpu_sw || first.fpu_tw != c1.fpu_tw ||
            first.fpu_top != c1.fpu_top) log_msg(" x87");
        if (firstdiff) log_msg(" memory at %04X:%04X", SEGPTR_SEL(firstdiff), SEGPTR_OFF(firstdiff));
        log_msg("\n");
        dump_x87("entered", &c0);
        dump_x87("native", &c1);
        dump_x87("guest", &first);
    } else {
        dump_cpu("guest", c);
    }
    if (r != CPU_RUNNING)
        log_msg("    the guest stopped: %s\n", cpu_state_name(r));
    else if (c->seg[S_CS] == thunk_selector())
        log_msg("    the guest reached an API call after %u instructions"
                " without matching\n", steps);
    else if (diff)
        log_msg("    registers matched at the stopping point but memory differs"
                " first at %04X:%04X\n", SEGPTR_SEL(diff), SEGPTR_OFF(diff));
    else
        log_msg("    the guest ran %u instructions without reaching the stopping"
                " point in that state\n", steps);
    *c = c1;
    c->state = CPU_FAULT;
    c->bad_cs = s->sel;
    c->bad_ip = s->off;
    return 1;
}

/* ---- dispatch ------------------------------------------------------------- */

static int native_hook(Cpu *c, uint16_t ip0, uint8_t *orig)
{
    NativeSite *s = nat_find(c->seg[S_CS], ip0);
    int n;

    if (!s) return -1;
    if (verify_every && !verifying && (s->hits + 1) % verify_every == 0)
        return native_checked(c, s, orig);
    n = s->fn(c);
    if (n <= 0) {
        *orig = s->orig;
        return 0;
    }
    nat_account(s, n);
    return 1;
}

/* ---- installation --------------------------------------------------------- */

void native_disable(void) { disabled = 1; }
void native_verify(unsigned every) { verify_every = every; }

const char *native_site_name(unsigned i)
{
    return i < NSITES ? sites[i].name : "?";
}

const char *native_name_at(uint16_t sel, uint16_t off)
{
    NativeSite *s = nat_find(sel, off);
    return s ? s->name : NULL;
}

/* Prefix bytes.  A site whose first byte is one cannot be patched, because a
   declined dispatch re-enters the decoder at the opcode with the prefixes
   already consumed, and there would have been none. */
static int is_prefix(uint8_t b)
{
    switch (b) {
    case 0x26: case 0x2E: case 0x36: case 0x3E: case 0x64: case 0x65:
    case 0x66: case 0x67: case 0xF0: case 0xF2: case 0xF3: return 1;
    default: return 0;
    }
}

int native_install(NeModule *m)
{
    unsigned i, k, done = 0;

    if (disabled || !NSITES) return 0;

    for (i = 0; i < NSITES; i++) {
        NativeSite *s = &sites[i];
        NeSeg *seg = ne_seg(m, s->segno);
        uint8_t have[8];
        unsigned h;

        if (!seg || !seg->sel) {
            log_msg("native: %s: no segment %u\n", s->name, s->segno);
            continue;
        }
        for (k = 0; k < 8; k++) have[k] = sel_rd8(seg->sel, (uint16_t)(s->off + k));
        if (memcmp(have, s->sig, 8) != 0) {
            log_msg("native: %s: seg%u:%04X does not hold the expected code"
                    " (%02X %02X %02X %02X...), not patched\n",
                    s->name, s->segno, s->off, have[0], have[1], have[2], have[3]);
            continue;
        }
        if (is_prefix(have[0])) {
            log_msg("native: %s: seg%u:%04X begins with a prefix, not patched\n",
                    s->name, s->segno, s->off);
            continue;
        }
        if (nat_find(seg->sel, s->off)) {
            log_msg("native: %s: seg%u:%04X already taken\n", s->name, s->segno, s->off);
            continue;
        }
        s->sel = seg->sel;
        s->orig = have[0];
        s->installed = 1;
        sel_wr8(s->sel, s->off, 0xD6);
        h = nat_slot(SEGPTR(s->sel, s->off));
        while (nat_map[h]) h = (h + 1) & (NAT_HASH - 1);
        nat_map[h] = s;
        done++;
    }
    if (done) cpu_native = native_hook;
    log_msg("native: %u of %u routines patched in%s\n", done, NSITES,
            verify_every ? ", verifying" : "");
    return (int)done;
}

/* ======================================================================== */
/*                              the routines                                 */
/* ======================================================================== */

/* What every routine here does, and what the verifier holds it to: not the
   function's meaning but the interpreter's state at the stopping point.  That
   includes the dead stores a compiled function makes - the saved registers
   and locals below the stack pointer, a scratch register left holding the
   high half of a product - because the arena is compared whole.  Reproducing
   them costs a few stores and is what makes "equal state" a mechanical test
   rather than a judgement about what is live. */

/* ---- the random generator ---------------------------------------------------
   Two copies of L'Ecuyer's combined multiplicative congruential generator
   (CACM 31(6), 1988): s1 = 40014 s1 mod 2147483563, s2 = 40692 s2 mod
   2147483399, result s1 - s2.  The compiler's rendering of Schrage's method:

       q  = s1 / -53668                   idiv by the negative quotient
       s1 = low32(q * 2147483563 + s1 * 40014)
       if (s1 < 0) s1 += 2147483563

   which is 40014 (s1 mod 53668) - 12211 q exactly, since 2147483563 =
   53668 * 40014 + 12211 and the true value fits in 32 bits.  Both steps are
   the same code twice with the other constants. */

typedef struct { uint32_t s1, s2; } Seeds;

static uint32_t lecuyer(uint32_t s, int32_t negq, uint32_t m, uint32_t a,
                        int *instrs)
{
    int32_t q = (int32_t)s / negq;               /* idiv: truncates, as C does */
    uint32_t r = (uint32_t)q * m + s * a;        /* the low halves of the products */
    if ((int32_t)r < 0) { r += m; (*instrs)++; }  /* the fix-up is one more */
    return r;
}

/* seg9:1940.  long rand_long(void): step both seeds at DGROUP:2292/2296 and
   return s1 - s2 in DX:AX.  Stops after the retf.  Live there: DX:AX, the
   seeds, and the flags of the `sbb dx,bx` that formed the high word. */
static int nat_rand_long(Cpu *c)
{
    uint16_t ds = c->seg[S_DS], ss = c->seg[S_SS];
    uint16_t sp = reg16(c, R_SP), bp = (uint16_t)(sp - 2);
    uint32_t s1 = sel_rd32(ds, 0x2292), s2 = sel_rd32(ds, 0x2296);
    uint32_t n1, n2, ax, dx, cf;
    int instrs = 59;

    /* enter 8,0 / push di / push si: the frame, as the guest lays it out. */
    sel_wr16(ss, bp, reg16(c, R_BP));
    sel_wr16(ss, (uint16_t)(bp - 10), reg16(c, R_DI));
    sel_wr16(ss, (uint16_t)(bp - 12), reg16(c, R_SI));

    n1 = lecuyer(s1, -53668, 2147483563u, 40014u, &instrs);
    n2 = lecuyer(s2, -52774, 2147483399u, 40692u, &instrs);
    sel_wr32(ss, (uint16_t)(bp - 8), n1);        /* [bp-8], after its fix-up */
    sel_wr32(ss, (uint16_t)(bp - 4), n2);        /* [bp-4], likewise         */
    sel_wr32(ds, 0x2292, n1);
    sel_wr32(ds, 0x2296, n2);

    /* mov eax,[bp-8]; mov dx,[bp-6]; mov ecx,[bp-4]; mov bx,[bp-2];
       sub ax,cx; sbb dx,bx.  Only the low words of eax and ebx move. */
    ax = n1 & 0xFFFFu;
    dx = n1 >> 16;
    cf = ax < (n2 & 0xFFFFu);
    c->r32[R_AX] = (n1 & 0xFFFF0000u) | ((ax - (n2 & 0xFFFFu)) & 0xFFFFu);
    c->r32[R_CX] = n2;
    set_reg16(c, R_BX, (uint16_t)(n2 >> 16));
    c->r32[R_DX] = (dx - (n2 >> 16) - cf) & 0xFFFFu;
    cpu_flags_sub(c, dx, n2 >> 16, cf, dx - (n2 >> 16) - cf, 2);

    /* pop si; pop di; leave; retf: everything restored, then the far return. */
    c->eip = sel_rd16(ss, sp);
    c->seg[S_CS] = sel_rd16(ss, (uint16_t)(sp + 2));
    set_reg16(c, R_SP, (uint16_t)(sp + 4));
    return instrs;
}

/* seg9:1652.  int rand_mod(int n): the same generator on the seeds at
   DGROUP:12D8/12DC, returning (s1 - s2, made positive) mod n in DX:AX, or 0
   when n <= 0.  Stops after the retf.  The dead values it must still leave:
   ECX holds n sign-extended (or the multiplier 40692 when n <= 0), EDX the
   high word of the remainder (or of the last product), and the flags are
   those of the `shr edx,16` (or of the `xor ax,ax`). */
static int nat_rand_mod(Cpu *c)
{
    uint16_t ds = c->seg[S_DS], ss = c->seg[S_SS];
    uint16_t sp = reg16(c, R_SP), bp = (uint16_t)(sp - 2);
    uint32_t s1 = sel_rd32(ds, 0x12D8), s2 = sel_rd32(ds, 0x12DC);
    uint32_t n1, n2, r;
    int16_t n = (int16_t)sel_rd16(ss, (uint16_t)(bp + 6));
    int instrs = 58;

    sel_wr16(ss, bp, reg16(c, R_BP));
    sel_wr16(ss, (uint16_t)(bp - 14), reg16(c, R_DI));
    sel_wr16(ss, (uint16_t)(bp - 16), reg16(c, R_SI));

    n1 = lecuyer(s1, -53668, 2147483563u, 40014u, &instrs);
    n2 = lecuyer(s2, -52774, 2147483399u, 40692u, &instrs);
    r = n1 - n2;
    if ((int32_t)r < 1) { r += 2147483562u; instrs++; }
    sel_wr32(ss, (uint16_t)(bp - 8), n1);
    sel_wr32(ss, (uint16_t)(bp - 4), n2);
    sel_wr32(ss, (uint16_t)(bp - 12), r);
    sel_wr32(ds, 0x12D8, n1);
    sel_wr32(ds, 0x12DC, n2);

    if (n <= 0) {
        /* cmp [bp+6],0 / jg not taken / xor ax,ax: EAX still carries the
           high word of s2, EDX and ECX what the second step left in them. */
        c->r32[R_AX] = n2 & 0xFFFF0000u;
        c->r32[R_DX] = (s2 * 40692u) >> 16;
        c->r32[R_CX] = 40692u;
        cpu_flags_logic(c, 0, 2);
        instrs += 5;
    } else {
        uint32_t m = r % (uint32_t)(int32_t)n;   /* div ecx: unsigned */
        sel_wr32(ss, (uint16_t)(bp - 20), (uint32_t)(int32_t)n);   /* push eax */
        c->r32[R_AX] = m;
        c->r32[R_CX] = (uint32_t)(int32_t)n;
        cpu_flags_logic(c, 0, 4);                /* xor edx,edx: AF and the rest */
        c->r32[R_DX] = cpu_shift(c, 5, m, 16, 4);
        instrs += 12;
    }

    c->eip = sel_rd16(ss, sp);
    c->seg[S_CS] = sel_rd16(ss, (uint16_t)(sp + 2));
    set_reg16(c, R_SP, (uint16_t)(sp + 4));
    return instrs;
}

/* ---- the nearest-object scan ------------------------------------------------
   seg8:2A01, the body of a loop inside seg8:2988 that walks an array of
   (x, y) word pairs at DS:SI looking for the closest to (x0, y0), which are
   the caller's [bp+6] and [bp+8]; DI indexes the array and DS:[7A] bounds it,
   and [bp-12] holds the best squared distance so far.  Each iteration:

       dy = y0 - p.y             [bp-A]
       eax = (x0 - p.x)^2        [bp-4], via cwd/push/pop/imul ecx
       if (eax > best) skip      jg 2A72
       ... the rare best-so-far update at 2A25 ...
       si += 4; di++; if (di < count) loop

   This routine runs the iterations that skip, which is nearly all of them,
   and stops either at the loop's exit (2A7C) or back at its head with
   everything as the guest would have it there, leaving the update path to
   the interpreter.  It commits nothing for an iteration it does not take, so
   the head is re-entered with the machine untouched and the decline runs the
   original first instruction. */

static int nat_scan_loop(Cpu *c)
{
    uint16_t ds = c->seg[S_DS], ss = c->seg[S_SS];
    uint16_t bp = reg16(c, R_BP), sp = reg16(c, R_SP);
    uint16_t si = reg16(c, R_SI), di = reg16(c, R_DI);
    uint16_t x0 = sel_rd16(ss, (uint16_t)(bp + 6));
    uint16_t y0 = sel_rd16(ss, (uint16_t)(bp + 8));
    uint32_t best = sel_rd32(ss, (uint16_t)(bp - 0x12));
    uint16_t dy, ax, dx, count = 0;
    uint16_t ax_c = 0;                 /* of the last iteration committed */
    uint32_t eax, eax_c = 0;
    int k = 0;

    for (;;) {
        dy = (uint16_t)(y0 - sel_rd16(ds, (uint16_t)(si + 2)));
        ax = (uint16_t)(x0 - sel_rd16(ds, si));
        dx = (ax & 0x8000u) ? 0xFFFFu : 0;
        eax = (uint32_t)((int32_t)(int16_t)ax * (int32_t)(int16_t)ax);
        if (!((int32_t)eax > (int32_t)best)) break;      /* the update path */

        /* The iteration, including what it leaves behind: the locals, and
           the four words the push/pop dance stores below the stack pointer. */
        sel_wr16(ss, (uint16_t)(bp - 0xA), dy);
        sel_wr16(ss, (uint16_t)(sp - 2), dx);
        sel_wr16(ss, (uint16_t)(sp - 4), ax);
        sel_wr16(ss, (uint16_t)(sp - 6), dx);
        sel_wr16(ss, (uint16_t)(sp - 8), ax);
        sel_wr32(ss, (uint16_t)(bp - 4), eax);
        eax_c = eax;
        ax_c = ax;
        si = (uint16_t)(si + 4);
        di = (uint16_t)(di + 1);
        k++;
        count = sel_rd16(ds, 0x7A);
        if (!((int16_t)di < (int16_t)count)) {
            /* jl not taken: the loop is over. */
            c->r32[R_AX] = eax_c;
            c->r32[R_CX] = (uint32_t)(int32_t)(int16_t)ax_c;
            c->r32[R_DX] = 0;                    /* the product's high half */
            set_reg16(c, R_SI, si);
            set_reg16(c, R_DI, di);
            cpu_flags_sub(c, di, count, 0, (uint32_t)di - count, 2);
            c->eip = 0x2A7C;
            return 20 * k;
        }
    }
    if (!k) return 0;

    /* Back at the head after k iterations: the registers of the last one
       taken, not of the one just declined, and the last cmp di,[7A] still in
       the flags. */
    c->r32[R_AX] = eax_c;
    c->r32[R_CX] = (uint32_t)(int32_t)(int16_t)ax_c;
    c->r32[R_DX] = 0;
    set_reg16(c, R_SI, si);
    set_reg16(c, R_DI, di);
    cpu_flags_sub(c, di, count, 0, (uint32_t)di - count, 2);
    c->eip = 0x2A01;
    return 20 * k;
}

/* ---- the C runtime's sqrt and _ftol -----------------------------------------------
   Two library routines the game's arithmetic leans on: seg37:0DC2, sqrt, and
   seg37:0E40, the double-to-long conversion the compiler emits for a cast.
   Both are built from x87 instructions, and the only way to be bit-exact with
   the interpreter is to perform the same host operations in the same order
   under the same guest control word, which is what the fpu_* primitives are.
   Nothing here does floating-point arithmetic in C.

   Each is written as a body - the effects between the far call and the retf,
   less the call and return themselves - and a site routine around it, so the
   habitability tail below can run the bodies inline where the guest calls
   the functions.  Their frames are below the stack pointer once they return
   and so are dead; only the globals sqrt writes are reproduced.

   sqrt takes the runtime's generic path: a shared prologue that saves the
   argument at DS:1A14 and calls the classifier at 20C2, which stores the
   control word, loads its own (extended precision, round to nearest, the
   precision exceptions masked), FXAMs ST(0), and jumps through a table
   indexed by the class.  For a positive normal the handler is FSQRT; the
   epilogue then FNCLEXes, stores the result at DS:16A6, checks the status
   word for overflow, restores the control word and returns.  Any other class
   - zero, a negative, infinity, NaN, an empty stack - goes elsewhere, and the
   routine declines rather than follow it.  It also declines if the argument
   is so large that storing it or its root as a double could overflow, which
   is where the epilogue's check would send it down another path. */

/* A double in guest memory that is a positive normal. */
static int f64_is_plain_positive(uint16_t sel, uint16_t off)
{
    uint32_t hi = sel_rd32(sel, (uint16_t)(off + 4));
    unsigned exp = (hi >> 20) & 0x7FF;
    return !(hi & 0x80000000u) && exp != 0 && exp != 0x7FF;
}

/* ST(0) is a positive normal below 2^1000, and is present. */
static int st0_is_plain_positive(Cpu *c)
{
    uint8_t b[10];
    long double v;
    unsigned exp;

    if (((c->fpu_tw >> ((c->fpu_top & 7) * 2)) & 3) == 3) return 0;   /* empty */
    v = fpu_get(c, 0);
    memcpy(b, &v, 10);
    exp = ((unsigned)(b[9] & 0x7F) << 8) | b[8];
    return !(b[9] & 0x80) && exp != 0 && exp < 0x3FFF + 1000 && (b[7] & 0x80);
}

static int crt_sqrt_body(Cpu *c, uint16_t bp)
{
    uint16_t ds = c->seg[S_DS];
    uint16_t cw0 = c->fpu_cw;
    long double a = fpu_get(c, 0);
    int n = 79;

    /* 1C9B: with the runtime's flag at 1BC8 clear the argument is saved first
       at 1A14 (three more instructions); set, the classifier is called at
       once.  The flag is set once the runtime has found the coprocessor,
       which is the state a running game is in. */
    if (sel_rd8(ds, 0x1BC8) == 0) fpu_store_f64(c, ds, 0x1A14, a);
    else                          n = 76;
    c->fpu_cw = (uint16_t)(0x1300 | (cw0 & 0xFF) | 0x38);  /* 20C2 fldcw [bp-8] */
    fpu_xam(c, a);                                          /* fxam: normal, +  */
    fpu_set(c, 0, fpu_sqrt(c, a));                          /* 12E1 fsqrt        */
    sel_wr8(ds, 0x1A44, 1);                                 /* 1CA5              */
    fpu_clex(c);                                            /* 1CF5 fnclex       */
    fpu_store_f64(c, ds, 0x16A6, fpu_get(c, 0));            /* 1CF8 fst [16A6]   */
    c->fpu_cw = cw0;                                        /* 1D1D fldcw [bp-6] */

    /* What the registers hold on the way out: the class index 0 in AX, the
       table slot in BX, the sign bit (none) in CL and the caller's CH masked,
       the descriptor in DX; then the epilogue's dec bp with the CF of the
       cmp byte [1A44],0 before it. */
    set_reg16(c, R_AX, 0);
    set_reg16(c, R_BX, 0x1868);
    set_reg16(c, R_CX, (uint16_t)(reg16(c, R_CX) & 0x0400));
    set_reg16(c, R_DX, 0x1858);
    cpu_flags_sub(c, 1, 0, 0, 1, 1);
    cpu_dec(c, (uint16_t)(bp + 1), 2);
    return n;
}

static int crt_ftol_body(Cpu *c, uint16_t bp, int64_t *out)
{
    uint16_t cw0 = c->fpu_cw;
    int64_t v;

    cpu_flags_logic(c, ((cw0 >> 8) | 0x0C) & 0xFF, 1);     /* 0E5A or ah,0C     */
    c->fpu_cw = (uint16_t)(cw0 | 0x0C00);                  /* 0E61 fldcw: trunc */
    v = fpu_to_int(c, fpu_get(c, 0), 8);                    /* 0E65 fistp qword  */
    fpu_pop(c);
    c->fpu_cw = cw0;                                        /* 0E69 fldcw        */
    set_reg16(c, R_AX, (uint16_t)v);
    set_reg16(c, R_DX, (uint16_t)(v >> 16));
    cpu_dec(c, (uint16_t)(bp + 1), 2);                      /* 0E7B, CF from or  */
    *out = v;
    return 36;
}

static void far_return(Cpu *c)
{
    uint16_t ss = c->seg[S_SS], sp0 = reg16(c, R_SP);
    c->eip = sel_rd16(ss, sp0);
    c->seg[S_CS] = sel_rd16(ss, (uint16_t)(sp0 + 2));
    set_reg16(c, R_SP, (uint16_t)(sp0 + 4));
}

static int nat_sqrt(Cpu *c)
{
    int n;
    if (!st0_is_plain_positive(c)) return 0;
    n = crt_sqrt_body(c, reg16(c, R_BP));
    far_return(c);
    return n;
}

static int nat_ftol(Cpu *c)
{
    int64_t v;
    int n;
    if (((c->fpu_tw >> ((c->fpu_top & 7) * 2)) & 3) == 3) return 0;    /* empty */
    n = crt_ftol_body(c, reg16(c, R_BP), &v);
    far_return(c);
    return n;
}

/* ---- habitability -------------------------------------------------------------
   seg10:490E.  int hab(planet far *p, int race): the planet's value on each
   of three axes, es:p[0xC + i], against the race's centre, low and high for
   that axis, bytes at DS:59D2/59D5/59D8 + race * 0xC0 + i.  Per axis:

       high < 0                 immune: sum += 10000
       low <= value <= high     pct = 100 |value - centre| / half-width
                                sum += (100 - pct)^2
                                excess = 2 |value - centre| - half-width
                                if (excess > 0)
                                    factor = factor * (2 half-width - excess)
                                           / (2 half-width)      (32-bit, truncating)
       otherwise                red += min(distance outside, 15)

   then, red != 0: return -red; red == 0: the float tail, which takes the
   square root of a third of the sum, adds 0.9 so that truncating rounds it,
   and scales it by factor / 10000.  This routine does the integer part and,
   when it can, the tail; otherwise it stops at 4A4E for the interpreter to
   do the tail, or after the retf on the negative path.

   The registers are modelled as the code moves them, high halves included,
   because a 32-bit pop or imul in one axis leaves a value the next axis's
   16-bit moves do not clear, and that value is what the caller gets back in
   ECX or EDX - dead, but part of the state.  Likewise the locals and the
   push/pop scratch are written only on the paths that write them, so that a
   stack slot the guest never touched keeps whatever it held.  The idivs
   decline the whole call rather than fault: a zero half-width is the guest's
   problem to have the way it always did. */

#define SET16(r, v)  ((r) = ((r) & 0xFFFF0000u) | ((uint32_t)(v) & 0xFFFFu))
#define SEXT8(b)     ((uint16_t)(int16_t)(int8_t)(b))
#define CWD(ax)      (((ax) & 0x8000u) ? 0xFFFFu : 0u)

static int nat_habitability(Cpu *c)
{
    uint16_t ds = c->seg[S_DS], ss = c->seg[S_SS], es;
    uint16_t sp0 = reg16(c, R_SP), bp = (uint16_t)(sp0 - 2);
    uint16_t sp = (uint16_t)(bp - 0x1E);
    uint16_t si0 = reg16(c, R_SI), di0 = reg16(c, R_DI);
    uint16_t planet = sel_rd16(ss, (uint16_t)(bp + 6));
    uint16_t race = sel_rd16(ss, (uint16_t)(bp + 0xA));
    uint16_t race192 = (uint16_t)(race * 0xC0);
    uint32_t eax = 0, ebx = c->r32[R_BX], ecx = c->r32[R_CX], edx = c->r32[R_DX];
    uint16_t si = si0, di = di0;
    uint32_t sum = 0, red = 0, factor = 0x2710;
    uint16_t width = 0, excess = 0, hi18 = 0, center = 0, low = 0;
    int have_width = 0, have_hi18 = 0;
    /* The words the push/pop sequences leave below the stack pointer,
       scratch[k] at sp - 12 + 2k, and the lowest one ever written. */
    uint16_t scratch[6];
    int lowest = 6;
    int i, instrs = 12;

    es = sel_rd16(ss, (uint16_t)(bp + 8));
    SET16(eax, race192);

    for (i = 0; i < 3; i++) {
        uint16_t bx, pv, high, ax, dx, cx;

        bx = (uint16_t)(i + planet);
        pv = SEXT8(sel_rd8(es, (uint16_t)(bx + 0xC)));
        di = pv;
        bx = (uint16_t)(race192 + i);
        center = SEXT8(sel_rd8(ds, (uint16_t)(bx + 0x59D2)));
        low    = SEXT8(sel_rd8(ds, (uint16_t)(bx + 0x59D5)));
        high   = SEXT8(sel_rd8(ds, (uint16_t)(bx + 0x59D8)));
        SET16(ebx, bx);
        SET16(eax, high);
        si = high;
        instrs += 18;

        if ((int16_t)high < 0) {                          /* immune */
            sum += 0x2710;
            instrs += 2;
        } else if ((int16_t)low > (int16_t)pv || (int16_t)high < (int16_t)pv) {
            /* out of range: how far, capped at 15 */
            hi18 = high;
            have_hi18 = 1;
            instrs += ((int16_t)low > (int16_t)pv) ? 3 : 5;
            instrs += 2;                                  /* cmp [bp-8],di; jle */
            if ((int16_t)low <= (int16_t)pv) { ax = (uint16_t)(pv - high); instrs += 2; }
            else                             { ax = (uint16_t)(low - pv);  instrs += 3; }
            instrs += 2;                                  /* cmp ax,0F; jle */
            if ((int16_t)ax > 15) { ax = 15; instrs++; }
            dx = CWD(ax);
            SET16(eax, ax);
            SET16(edx, dx);
            red += (uint32_t)(int32_t)(int16_t)ax;
            instrs += 3;                                  /* cwd; add; adc */
        } else {                                          /* in range */
            uint16_t absd;
            int16_t divisor;
            int64_t wide;

            hi18 = high;
            have_hi18 = 1;
            ax = (uint16_t)(pv - center);
            dx = CWD(ax);
            absd = (uint16_t)((ax ^ dx) - dx);
            si = (uint16_t)(absd * 100);
            instrs += 5 + 8;
            if ((int16_t)center > (int16_t)pv) {          /* below the centre */
                cx = (uint16_t)(center - low);
                divisor = (int16_t)cx;
                if (!divisor) return 0;
                si = (uint16_t)((int16_t)si / divisor);   /* idiv cx of sext(ax) */
                ax = (uint16_t)(center - pv);
                instrs += 10;
            } else {
                cx = (uint16_t)(high - center);
                divisor = (int16_t)cx;
                if (!divisor) return 0;
                si = (uint16_t)((int16_t)si / divisor);
                ax = (uint16_t)(pv - center);
                instrs += 9;
            }
            width = cx;
            have_width = 1;
            SET16(ecx, cx);
            ax = (uint16_t)(ax + ax);
            ax = (uint16_t)(ax - cx);
            excess = ax;
            ax = (uint16_t)(100 - si);
            dx = CWD(ax);
            /* push dx; push ax; push dx; push ax; pop eax; pop ecx */
            scratch[2] = ax; scratch[3] = dx; scratch[4] = ax; scratch[5] = dx;
            if (lowest > 2) lowest = 2;
            eax = ecx = (uint32_t)(int32_t)(int16_t)ax;
            wide = (int64_t)(int32_t)eax * (int64_t)(int32_t)ecx;
            eax = (uint32_t)wide;
            edx = eax >> 16;                              /* mov edx,eax; shr edx,10 */
            sum += eax;                                   /* add/adc into [bp-C] */
            instrs += 19;
            if ((int16_t)excess > 0) {
                /* mov ax,[bp-2]; add ax,ax; cwd; push dx; push ax; push [bp-12];
                   sub ax,[bp-6]; cwd; push dx; push ax; pop eax; pop ecx;
                   imul ecx; pop ecx; cdq; idiv ecx; mov [bp-12],eax; mov dx,[bp-10] */
                uint16_t w2 = (uint16_t)(width + width);
                uint16_t d = (uint16_t)(w2 - excess);
                uint32_t prod;
                scratch[5] = CWD(w2);  scratch[4] = w2;
                scratch[3] = (uint16_t)(factor >> 16); scratch[2] = (uint16_t)factor;
                scratch[1] = CWD(d);   scratch[0] = d;
                lowest = 0;
                eax = (uint32_t)(int32_t)(int16_t)d;
                ecx = factor;
                wide = (int64_t)(int32_t)eax * (int64_t)(int32_t)ecx;
                prod = (uint32_t)wide;
                ecx = (uint32_t)(int32_t)(int16_t)w2;
                if (!(int32_t)ecx) return 0;
                if ((int32_t)prod == INT32_MIN && (int32_t)ecx == -1) return 0;
                eax = (uint32_t)((int32_t)prod / (int32_t)ecx);
                edx = (uint32_t)((int32_t)prod % (int32_t)ecx);
                factor = eax;
                SET16(edx, factor >> 16);
                instrs += 19;
            }
        }
        instrs += 3;                                      /* inc; cmp; jl */
    }

    /* Everything is decided; now the machine.  The frame first: the saved
       registers, then the locals as last written. */
    sel_wr16(ss, bp, reg16(c, R_BP));
    sel_wr16(ss, (uint16_t)(bp - 0x1C), di0);
    sel_wr16(ss, (uint16_t)(bp - 0x1E), si0);
    sel_wr32(ss, (uint16_t)(bp - 0xC), sum);
    sel_wr32(ss, (uint16_t)(bp - 0x16), red);
    sel_wr32(ss, (uint16_t)(bp - 0x12), factor);
    sel_wr16(ss, (uint16_t)(bp - 0xE), 3);
    sel_wr16(ss, (uint16_t)(bp - 0x1A), race192);
    sel_wr16(ss, (uint16_t)(bp - 0x4), center);
    sel_wr16(ss, (uint16_t)(bp - 0x8), low);
    if (have_hi18) sel_wr16(ss, (uint16_t)(bp - 0x18), hi18);
    if (have_width) {
        sel_wr16(ss, (uint16_t)(bp - 0x2), width);
        sel_wr16(ss, (uint16_t)(bp - 0x6), excess);
    }
    for (i = lowest; i < 6; i++)
        sel_wr16(ss, (uint16_t)(sp - 12 + 2 * i), scratch[i]);

    c->seg[S_ES] = es;
    c->r32[R_BX] = ebx;
    c->r32[R_CX] = ecx;
    c->r32[R_DX] = edx;

    /* mov ax,[bp-14]; or ax,[bp-16]; jz 4A4E */
    {
        uint16_t ax = (uint16_t)((red >> 16) | (red & 0xFFFFu));
        SET16(eax, ax);
        cpu_flags_logic(c, ax, 2);
        instrs += 3;
        if (ax == 0 && (int32_t)sum > 0 && f64_is_plain_positive(ds, 0x1D02)) {
            /* The float tail, 4A4E-4A8D, with the two runtime calls inline:
                 push 2710; push [bp-12]; fild [bp-C]; fmul [1D02]; call sqrt;
                 fadd [1D0A]; call _ftol; push dx; push ax; pop eax; pop ecx;
                 imul ecx; pop ecx; cdq; idiv ecx; mov edx,eax; shr edx,10;
                 mov [bp-C],ax; pop si; pop di; leave; retf
               A positive sum times a positive normal constant is the positive
               normal sqrt wants, so the condition above is the routine's whole
               precondition.  Everything pushed here is below the final stack
               pointer, and so is the frame: dead. */
            uint32_t prod, q;
            int64_t v;

            fpu_load(c, (long double)(int32_t)sum);
            fpu_set(c, 0, fpu_arith(c, FPU_MUL, fpu_get(c, 0), fpu_load_f64(ds, 0x1D02)));
            instrs += crt_sqrt_body(c, bp);
            fpu_set(c, 0, fpu_arith(c, FPU_ADD, fpu_get(c, 0), fpu_load_f64(ds, 0x1D0A)));
            instrs += crt_ftol_body(c, bp, &v);
            c->r32[R_AX] = (uint32_t)v;                    /* push dx; push ax; pop eax */
            c->r32[R_CX] = factor;                         /* pop ecx */
            cpu_mul(c, 4, factor, 1);                      /* imul ecx */
            prod = c->r32[R_AX];
            q = (uint32_t)((int32_t)prod / 10000);         /* pop ecx; cdq; idiv ecx */
            c->r32[R_AX] = q;
            c->r32[R_CX] = 0x2710;
            c->r32[R_DX] = cpu_shift(c, 5, q, 16, 4);      /* mov edx,eax; shr edx,10 */
            set_reg16(c, R_SI, si0);
            set_reg16(c, R_DI, di0);
            c->eip = sel_rd16(ss, sp0);
            c->seg[S_CS] = sel_rd16(ss, (uint16_t)(sp0 + 2));
            set_reg16(c, R_SP, (uint16_t)(sp0 + 4));
            return instrs + 25;
        }
        if (ax == 0) {
            c->r32[R_AX] = eax;
            set_reg16(c, R_SI, si);
            set_reg16(c, R_DI, di);
            set_reg16(c, R_BP, bp);
            set_reg16(c, R_SP, sp);
            c->eip = 0x4A4E;
            return instrs;
        }
        /* mov ax,[bp-16]; neg ax; pop si; pop di; leave; retf */
        ax = (uint16_t)(red & 0xFFFFu);
        cpu_flags_sub(c, 0, ax, 0, (uint32_t)0 - ax, 2);
        SET16(eax, (uint16_t)(0 - ax));
        c->r32[R_AX] = eax;
        c->eip = sel_rd16(ss, sp0);
        c->seg[S_CS] = sel_rd16(ss, (uint16_t)(sp0 + 2));
        set_reg16(c, R_SP, (uint16_t)(sp0 + 4));
        return instrs + 6;
    }
}

/* ---- the six-field check ------------------------------------------------------
   seg2:5916.  int check(char far *have): six bytes at ES:DI against six
   requirements at DS:59DC + race * C0 (the race from DS:14C), and a nibble
   at DS:59FB naming the one field allowed to be short:

       for (i = 0; i < 6; i++)
           if (req[i] < have[i]) {           signed bytes
               count++;
               if ((flags & 0xF) == i) {
                   if (req[i] - have[i] + 1 == 0) flag = 1;
                   else idx = i + 1;
               }
           }
       count == 0            -> 1
       count == 1 && flag    -> 2
       count == 1 && idx     -> have[idx-1] - req[idx-1] + 1
       otherwise             -> 99

   Stops after the retf.  BX comes back as count, or as DI + idx - 1 on the
   third path; CX as the last sign-extended byte moved into it; the flags are
   those of whichever compare or inc decided the return. */

static int nat_tech_check(Cpu *c)
{
    uint16_t ds = c->seg[S_DS], ss = c->seg[S_SS];
    uint16_t sp0 = reg16(c, R_SP), bp = (uint16_t)(sp0 - 2);
    uint16_t off = sel_rd16(ss, (uint16_t)(bp + 6));
    uint16_t es = sel_rd16(ss, (uint16_t)(bp + 8));
    uint16_t race192 = (uint16_t)(sel_rd16(ds, 0x14C) * 0xC0);
    uint16_t count = 0, flag = 0, idx = 0;
    uint16_t bx = 0, cx = reg16(c, R_CX), ax;
    uint8_t req = 0;
    int i, instrs = 11;

    for (i = 0; i < 6; i++) {
        uint8_t have;
        req = sel_rd8(ds, (uint16_t)(race192 + i + 0x59DC));
        bx = (uint16_t)(off + i);
        have = sel_rd8(es, bx);
        instrs += 7;
        if ((int8_t)req >= (int8_t)have) { instrs += 3; continue; }
        count++;
        instrs += 6;
        if ((sel_rd8(ds, (uint16_t)(race192 + 0x59FB)) & 0xF) != i) { instrs += 3; continue; }
        cx = SEXT8(have);
        ax = (uint16_t)(SEXT8(req) - cx + 1);
        instrs += 10 + 2 + 3;
        if (ax) idx = (uint16_t)(i + 1);
        else    flag = 1;
    }

    sel_wr16(ss, bp, reg16(c, R_BP));
    sel_wr16(ss, (uint16_t)(bp - 0xC), reg16(c, R_DI));
    sel_wr16(ss, (uint16_t)(bp - 0xE), reg16(c, R_SI));
    sel_wr16(ss, (uint16_t)(bp - 2), count);
    sel_wr16(ss, (uint16_t)(bp - 6), flag);
    sel_wr16(ss, (uint16_t)(bp - 4), idx);
    sel_wr16(ss, (uint16_t)(bp - 8), race192);
    sel_wr8(ss, (uint16_t)(bp - 0xA), req);
    c->seg[S_ES] = es;

    bx = count;
    instrs += 3;                                          /* mov bx; or; jnz */
    if (count == 0) {
        cpu_flags_logic(c, 0, 2);
        ax = 1;
        instrs += 5;
    } else if (count == 1 && flag) {
        cpu_flags_sub(c, flag, 0, 0, flag, 2);            /* cmp [bp-6],0 */
        ax = 2;
        instrs += 2 + 2 + 5;
    } else if (count == 1 && idx) {
        uint16_t r = (uint16_t)(race192 + idx);
        uint8_t  want = sel_rd8(ds, (uint16_t)(r + 0x59DB));
        uint8_t  have;
        bx = (uint16_t)(off + idx - 1);
        have = sel_rd8(es, bx);
        cx = SEXT8(want);
        ax = (uint16_t)(SEXT8(have) - cx);
        cpu_flags_sub(c, SEXT8(have), cx, 0, ax, 2);
        ax = (uint16_t)cpu_inc(c, ax, 2);
        instrs += 2 + 2 + 2 + 2 + 15;
    } else if (count == 1) {
        cpu_flags_sub(c, 0, 0, 0, 0, 2);                  /* cmp [bp-4],0 */
        ax = 0x63;
        instrs += 2 + 2 + 2 + 2 + 5;
    } else {
        cpu_flags_sub(c, count, 1, 0, (uint32_t)count - 1, 2);   /* cmp bx,1 */
        ax = 0x63;
        instrs += 2 + 2 + 5;
    }
    set_reg16(c, R_AX, ax);
    set_reg16(c, R_BX, bx);
    set_reg16(c, R_CX, cx);
    c->eip = sel_rd16(ss, sp0);
    c->seg[S_CS] = sel_rd16(ss, (uint16_t)(sp0 + 2));
    set_reg16(c, R_SP, (uint16_t)(sp0 + 4));
    return instrs;
}

/* ---- a byte accessor --------------------------------------------------------
   seg29:222C.  int f(int a, int b): return (signed char) DS:[a + b + 3E].
   Ten instructions, and over a million calls per ten turns, so the call
   costs more than the body.  Touches no flags at all. */

static int nat_byte_at_3e(Cpu *c)
{
    uint16_t ds = c->seg[S_DS], ss = c->seg[S_SS];
    uint16_t sp0 = reg16(c, R_SP), bp = (uint16_t)(sp0 - 2);
    uint16_t bx = sel_rd16(ss, (uint16_t)(bp + 6));
    uint16_t si = sel_rd16(ss, (uint16_t)(bp + 8));

    sel_wr16(ss, bp, reg16(c, R_BP));
    sel_wr16(ss, (uint16_t)(bp - 2), reg16(c, R_SI));
    set_reg16(c, R_AX, SEXT8(sel_rd8(ds, (uint16_t)(bx + si + 0x3E))));
    set_reg16(c, R_BX, bx);
    c->eip = sel_rd16(ss, sp0);
    c->seg[S_CS] = sel_rd16(ss, (uint16_t)(sp0 + 2));
    set_reg16(c, R_SP, (uint16_t)(sp0 + 4));
    return 10;
}

#undef SET16
#undef SEXT8
#undef CWD
