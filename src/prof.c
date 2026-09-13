/* prof.c - see prof.h.  The whole file is nothing unless asked for. */

#include "prof.h"

#ifdef STARSVM_PROFILE

#include "sel.h"
#include "cpu.h"
#include "log.h"
#include "thunk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <x86intrin.h>

ImpEntry *imp_slot(unsigned index);

/* A 0x0F instruction counts at 256 + its second byte, so one array covers both
   opcode pages and the pair histogram can speak about them too. */
#define PF_OPS 512

/* More than the import table has; imp_slot answers NULL past its end. */
#define PF_IMPORTS 1024

#define PF_NATIVES 64

static uint64_t pf_op[PF_OPS];
static uint32_t pf_pair[PF_OPS][PF_OPS];      /* 1 MB, saturating */
static uint32_t pf_first[PF_OPS];             /* SEGPTR, for a real mnemonic */
static uint64_t pf_sel[SEL_SLOTS];
static uint64_t pf_total, pf_repins, pf_repelems;

/* Counting is deferred by one instruction so that prof_op2 can raise a 0x0F to
   its two-byte identity before anything is recorded against it. */
static unsigned pf_prev = PF_OPS;
static unsigned pf_pending;
static uint32_t pf_at;                        /* where the pending one is */
static int      pf_have;

/* Guest code as loaded.  A decode cache - or a native routine patched over a
   block - is silently wrong if the game rewrites its own code, and comparing at
   exit costs the hot path nothing, so this answers the question rather than
   assuming it. */
static uint8_t *pf_code[SEL_SLOTS];
static uint32_t pf_size[SEL_SLOTS];

/* Which NE segment a code selector is, for naming sites the way the docs do. */
static uint16_t pf_segno[SEL_SLOTS];

/* ---- per address ----------------------------------------------------------
   Indexed by the first byte of the instruction, prefixes included, which is the
   address prof_op is given and the one a patch would go at. */
static uint32_t *pf_hit[SEL_SLOTS];    /* executions of the instruction here  */
static uint8_t  *pf_lead[SEL_SLOTS];   /* ran directly after a transfer       */
static uint8_t  *pf_xfer[SEL_SLOTS];   /* is a control transfer               */
static uint32_t *pf_callt[SEL_SLOTS];  /* entered as the target of a CALL     */
static uint64_t *pf_repx[SEL_SLOTS];   /* REP elements moved from here        */

/* What the previous instruction was, decided when it was seen so that the next
   prof_op can classify the address it lands on.  A CALL also records where it
   would have fallen through to: the instruction after a call into the thunk
   selector is a return from an API, not the entry of a function, and this is
   how the two are told apart. */
static int      pf_prev_xfer, pf_prev_call;
static uint32_t pf_prev_fall;
static unsigned pf_cur_i;
static uint16_t pf_cur_ip;

/* ---- time -----------------------------------------------------------------
   Exclusive handler time needs the guest time nested inside a handler taken
   out, and only the guest time nested *directly* inside it: a callback's own
   callbacks are already inside its interval.  So nested runs are accumulated
   per nesting depth, and a handler at depth d reads only acc[d]. */
#define PF_DEPTH 40
static uint64_t pf_acc[PF_DEPTH];
static unsigned pf_depth;
static uint64_t pf_api_ticks[PF_IMPORTS], pf_api_calls[PF_IMPORTS];
static uint64_t pf_fpu_ticks, pf_fpu_n, pf_rep_ticks;
static uint64_t pf_tsc0;
static LARGE_INTEGER pf_qpc0;

static uint64_t pf_nat_calls[PF_NATIVES], pf_nat_instrs[PF_NATIVES];

static void pf_commit(void)
{
    if (!pf_have) return;
    pf_op[pf_pending]++;
    if (pf_prev < PF_OPS && pf_pair[pf_prev][pf_pending] != 0xFFFFFFFFu)
        pf_pair[pf_prev][pf_pending]++;
    pf_prev = pf_pending;
    pf_have = 0;
}

void prof_begin(NeModule *m)
{
    unsigned i;

    pf_tsc0 = __rdtsc();
    QueryPerformanceCounter(&pf_qpc0);

    for (i = 1; i <= m->cseg; i++) {
        NeSeg *s = ne_seg(m, i);
        if (s && s->sel) pf_segno[SEL_INDEX(s->sel)] = (uint16_t)i;
    }

    for (i = 1; i < SEL_SLOTS; i++) {
        uint32_t n;
        if (sel_tab[i].kind != SK_CODE) continue;
        n = sel_tab[i].limit + 1u;
        if (n > SEL_SLOT) n = SEL_SLOT;
        pf_size[i] = n;
        pf_code[i] = malloc(n);
        if (pf_code[i]) memcpy(pf_code[i], sel_arena + ((size_t)i << 16), n);
        pf_hit[i]   = calloc(n, sizeof *pf_hit[i]);
        pf_lead[i]  = calloc(n, 1);
        pf_xfer[i]  = calloc(n, 1);
        pf_callt[i] = calloc(n, sizeof *pf_callt[i]);
        pf_repx[i]  = calloc(n, sizeof *pf_repx[i]);
    }
}

static int pf_is_prefix(uint8_t b)
{
    switch (b) {
    case 0x26: case 0x2E: case 0x36: case 0x3E: case 0x64: case 0x65:
    case 0x66: case 0x67: case 0xF0: case 0xF2: case 0xF3: return 1;
    default: return 0;
    }
}

/* Decide what the instruction just started is, for the sake of the one after
   it.  Read from the arena rather than passed in: cpu.c has the ModRM byte and
   the second opcode byte at different points in its decode, and it is simpler
   to look than to thread them through. */
static void pf_classify(uint16_t cs, uint16_t ip, uint8_t op)
{
    unsigned i = SEL_INDEX(cs);
    int xfer = 0, call = 0;
    uint32_t fall = 0;
    uint16_t p = ip;
    int osz = 0;

    for (;;) {
        uint8_t b = sel_rd8(cs, p);
        if (!pf_is_prefix(b)) break;
        if (b == 0x66) osz = 1;
        p++;
    }

    if (op == 0x0F) {
        uint8_t op2 = sel_rd8(cs, (uint16_t)(p + 1));
        if (op2 >= 0x80 && op2 <= 0x8F) xfer = 1;          /* Jcc near */
    } else if ((op >= 0x70 && op <= 0x7F) || (op >= 0xE0 && op <= 0xE3) ||
               op == 0xE9 || op == 0xEB || op == 0xEA ||
               op == 0xC3 || op == 0xC2 || op == 0xCB || op == 0xCA ||
               op == 0xCF || op == 0xCD || op == 0xCC) {
        xfer = 1;
    } else if (op == 0xE8 || op == 0x9A) {
        xfer = call = 1;
        fall = SEGPTR(cs, p + (op == 0xE8 ? 3 : 5) + (osz ? 2 : 0));
    } else if (op == 0xFF) {
        uint8_t modrm = sel_rd8(cs, (uint16_t)(p + 1));
        int reg = (modrm >> 3) & 7;
        if (reg == 2 || reg == 3) xfer = call = 1;
        else if (reg == 4 || reg == 5) xfer = 1;
        if (call) {
            int mod = modrm >> 6, rm = modrm & 7, len = 2;
            if (mod == 0 && rm == 6) len += 2;
            else if (mod == 1) len += 1;
            else if (mod == 2) len += 2;
            fall = SEGPTR(cs, p + len);
        }
    }
    if (xfer && pf_xfer[i] && ip < pf_size[i]) pf_xfer[i][ip] = 1;
    pf_prev_xfer = xfer;
    pf_prev_call = call;
    pf_prev_fall = fall;
}

void prof_op(uint16_t cs, uint16_t ip, uint8_t op)
{
    unsigned i = SEL_INDEX(cs);
    uint32_t at = SEGPTR(cs, ip);

    pf_commit();
    pf_pending = op;
    pf_have = 1;
    pf_total++;
    pf_sel[i]++;
    pf_at = at;
    if (!pf_first[op]) pf_first[op] = at;

    pf_cur_i = i;
    pf_cur_ip = ip;
    if (pf_hit[i] && ip < pf_size[i]) {
        pf_hit[i][ip]++;
        if (pf_prev_xfer) pf_lead[i][ip] = 1;
        if (pf_prev_call && at != pf_prev_fall) pf_callt[i][ip]++;
    }
    pf_classify(cs, ip, op);
}

void prof_op2(uint8_t op2)
{
    pf_pending = 256u + op2;
    if (!pf_first[pf_pending]) pf_first[pf_pending] = pf_at;
}

void prof_rep(uint32_t elems, uint64_t ticks)
{
    pf_repins++;
    pf_repelems += elems;
    pf_rep_ticks += ticks;
    if (pf_repx[pf_cur_i] && pf_cur_ip < pf_size[pf_cur_i])
        pf_repx[pf_cur_i][pf_cur_ip] += elems;
}

uint64_t prof_tick(void) { return __rdtsc(); }

void prof_fpu(uint64_t ticks) { pf_fpu_ticks += ticks; pf_fpu_n++; }

uint64_t prof_api_begin(void)
{
    return __rdtsc() - pf_acc[pf_depth < PF_DEPTH ? pf_depth : PF_DEPTH - 1];
}

void prof_api_end(unsigned index, uint64_t token)
{
    uint64_t now = __rdtsc() - pf_acc[pf_depth < PF_DEPTH ? pf_depth : PF_DEPTH - 1];
    if (index >= PF_IMPORTS) return;
    pf_api_ticks[index] += now - token;
    pf_api_calls[index]++;
}

uint64_t prof_nest_begin(void)
{
    pf_depth++;
    return __rdtsc();
}

void prof_nest_end(uint64_t token)
{
    if (pf_depth) pf_depth--;
    pf_acc[pf_depth < PF_DEPTH ? pf_depth : PF_DEPTH - 1] += __rdtsc() - token;
}

void prof_native(unsigned site, uint32_t instrs)
{
    if (site >= PF_NATIVES) return;
    pf_nat_calls[site]++;
    pf_nat_instrs[site] += instrs;
}

/* ---- the report ---------------------------------------------------------- */

static double pf_pct(uint64_t n) { return pf_total ? 100.0 * (double)n / (double)pf_total : 0.0; }

/* An opcode is far more legible as the instruction it was, so each one is shown
   disassembled at the first address it ran - which is sound because the
   self-modifying-code check below says the code never changed. */
static void pf_name(unsigned k, char *out, int n)
{
    char line[160];
    uint32_t at = pf_first[k];

    if (!at) {
        snprintf(out, (size_t)n, "%s%02X", k >= 256 ? "0F " : "", k & 0xFF);
        return;
    }
    disasm(SEGPTR_SEL(at), SEGPTR_OFF(at), line, sizeof line);
    /* disasm.c writes "%04X:%04X " then eight two-character byte slots then a
       space, so the text begins at column 27.  Taken from the format rather
       than counted off a sample, having first counted it off a sample wrong. */
    snprintf(out, (size_t)n, "%s", strlen(line) > 27 ? line + 27 : line);
}

/* "seg10:490E", the form docs/copy-protection.md uses and a patch table keys
   on, since a selector is only what this run happened to hand out. */
static const char *pf_site(uint16_t sel, uint16_t off)
{
    static char buf[4][24];
    static unsigned k;
    char *s = buf[k++ & 3];
    unsigned segno = pf_segno[SEL_INDEX(sel)];
    if (segno) snprintf(s, sizeof buf[0], "seg%u:%04X", segno, off);
    else       snprintf(s, sizeof buf[0], "%04X:%04X", sel, off);
    return s;
}

static int pf_is_jcc(unsigned k)
{
    return (k >= 0x70 && k <= 0x7F) ||            /* short */
           (k >= 256 + 0x80 && k <= 256 + 0x8F);  /* near, 0x0F page */
}

/* Instructions whose only interesting output is flags, or which set them on the
   way past.  A Jcc directly after one of these is a fusable pair: the condition
   can be computed from the operands and the flags never materialised. */
static int pf_sets_flags(unsigned k)
{
    if (k >= 256) return 0;
    if (k <= 0x3D && (k & 7) <= 5) return 1;      /* the eight ALU groups   */
    if (k >= 0x40 && k <= 0x4F) return 1;         /* INC/DEC r16            */
    if (k >= 0x80 && k <= 0x83) return 1;         /* group 1, immediate     */
    if (k == 0x84 || k == 0x85) return 1;         /* TEST r/m,r             */
    if (k == 0xA8 || k == 0xA9) return 1;         /* TEST al/ax,imm         */
    if (k >= 0xC0 && k <= 0xC1) return 1;         /* shift by imm8          */
    if (k >= 0xD0 && k <= 0xD3) return 1;         /* shift                  */
    if (k == 0xF6 || k == 0xF7) return 1;         /* group 3: TEST, NEG, .. */
    if (k == 0xFE || k == 0xFF) return 1;         /* group 4/5: INC/DEC r/m */
    return 0;
}

static int pf_cmp64(const void *a, const void *b)
{
    uint64_t x = pf_op[*(const unsigned *)a], y = pf_op[*(const unsigned *)b];
    return x < y ? 1 : x > y ? -1 : 0;
}

/* ---- blocks and functions ------------------------------------------------ */

typedef struct {
    uint16_t sel, start, end;   /* end is the first byte of the last instruction */
    uint32_t n, count;          /* instructions; executions of the leader        */
    uint64_t weight, repx;      /* instructions executed; REP elements moved     */
    int      uneven;            /* hit counts differ inside: a leader was missed */
    uint16_t func;              /* nearest call target at or before start        */
} Blk;

typedef struct {
    uint16_t sel, ip, next;     /* next call target in the selector, or the end */
    uint32_t calls;
    uint64_t weight;
    uint32_t nblk;
} Fn;

static Blk *blks;  static size_t nblk;
static Fn  *fns;   static size_t nfn;

static int blk_cmp(const void *a, const void *b)
{
    const Blk *x = a, *y = b;
    return x->weight < y->weight ? 1 : x->weight > y->weight ? -1 : 0;
}

static int fn_cmp(const void *a, const void *b)
{
    const Fn *x = a, *y = b;
    return x->weight < y->weight ? 1 : x->weight > y->weight ? -1 : 0;
}

/* A block belongs to the greatest call target at or before it in its selector.
   MS C lays functions out in order, so that is the function, except for code
   only ever reached by a jump, which lands in whatever precedes it. */
static Fn *pf_func_of(uint16_t sel, uint16_t ip)
{
    size_t lo = 0, hi = nfn;
    Fn *best = NULL;
    /* fns is built in (sel, ip) order, so binary search for the last <= ip. */
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (fns[mid].sel < sel || (fns[mid].sel == sel && fns[mid].ip <= ip)) {
            if (fns[mid].sel == sel) best = &fns[mid];
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return best;
}

static void pf_collect(void)
{
    unsigned i;
    size_t cap = 0, fcap = 0;

    for (i = 1; i < SEL_SLOTS; i++) {
        uint32_t ip;
        if (!pf_callt[i]) continue;
        for (ip = 0; ip < pf_size[i]; ip++) {
            if (!pf_callt[i][ip]) continue;
            if (nfn == fcap) {
                fcap = fcap ? fcap * 2 : 1024;
                fns = realloc(fns, fcap * sizeof *fns);
            }
            fns[nfn].sel = SEL_MAKE(i);
            fns[nfn].ip = (uint16_t)ip;
            fns[nfn].next = (uint16_t)(pf_size[i] - 1);
            fns[nfn].calls = pf_callt[i][ip];
            fns[nfn].weight = 0;
            fns[nfn].nblk = 0;
            if (nfn && fns[nfn - 1].sel == fns[nfn].sel)
                fns[nfn - 1].next = (uint16_t)ip;
            nfn++;
        }
    }

    /* A block runs from a leader to the next leader or the next transfer,
       whichever is first, over the addresses that executed at all.  Operand
       bytes never execute and jump tables never do either, so the next executed
       address after an instruction is the next instruction. */
    for (i = 1; i < SEL_SLOTS; i++) {
        uint32_t ip = 0, n = pf_size[i];
        if (!pf_hit[i]) continue;
        while (ip < n) {
            Blk b;
            uint32_t p = ip;
            Fn *f;
            if (!pf_hit[i][ip]) { ip++; continue; }
            memset(&b, 0, sizeof b);
            b.sel = SEL_MAKE(i);
            b.start = (uint16_t)ip;
            b.count = pf_hit[i][ip];
            for (;;) {
                uint32_t q = p + 1;
                int isx = pf_xfer[i][p];
                b.n++;
                b.weight += pf_hit[i][p];
                b.repx += pf_repx[i][p];
                b.end = (uint16_t)p;
                if (pf_hit[i][p] != b.count) b.uneven = 1;
                while (q < n && !pf_hit[i][q]) q++;
                p = q;
                if (isx || q >= n || pf_lead[i][q]) break;
            }
            f = pf_func_of(b.sel, b.start);
            b.func = f ? f->ip : 0xFFFF;
            if (f) { f->weight += b.weight; f->nblk++; }
            if (nblk == cap) {
                cap = cap ? cap * 2 : 4096;
                blks = realloc(blks, cap * sizeof *blks);
            }
            blks[nblk++] = b;
            ip = p;
        }
    }
}

static void pf_disasm_range(uint16_t sel, uint16_t start, uint16_t end, int maxlines)
{
    char line[160];
    uint32_t off = start;
    int lines = 0;

    while (off <= end && lines < maxlines) {
        int len = disasm(sel, (uint16_t)off, line, sizeof line);
        log_msg("        %s\n", line);
        if (len <= 0) break;
        off += (uint32_t)len;
        lines++;
    }
    if (off <= end) log_msg("        ...\n");
}

static void pf_report_blocks(void)
{
    size_t k;
    uint64_t cum = 0, uneven = 0;
    static const int marks[5] = { 25, 50, 75, 90, 99 };
    int mi = 0;

    pf_collect();
    if (!nblk) return;
    qsort(blks, nblk, sizeof *blks, blk_cmp);
    for (k = 0; k < nblk; k++) if (blks[k].uneven) uneven++;

    log_msg("\n  basic blocks: %llu executed", (unsigned long long)nblk);
    if (uneven) log_msg(", %llu uneven (a leader was missed)", (unsigned long long)uneven);
    log_msg("\n");
    for (k = 0; k < nblk && mi < 5; k++) {
        cum += blks[k].weight;
        while (mi < 5 && pf_pct(cum) >= marks[mi]) {
            log_msg("    %2d%% of instructions in the top %6llu blocks\n",
                    marks[mi], (unsigned long long)(k + 1));
            mi++;
        }
    }

    log_msg("\n  top blocks                       in           n      count"
            "        weight      %%   cum%%    rep-elems\n");
    cum = 0;
    for (k = 0; k < nblk && k < 60; k++) {
        Blk *b = &blks[k];
        cum += b->weight;
        log_msg("  %-13s %04X:%04X-%04X  %-12s %3u %10lu %13llu  %5.2f  %6.2f  %11llu%s\n",
                pf_site(b->sel, b->start), b->sel, b->start, b->end,
                pf_site(b->sel, b->func), b->n, (unsigned long)b->count,
                (unsigned long long)b->weight, pf_pct(b->weight), pf_pct(cum),
                (unsigned long long)b->repx, b->uneven ? " uneven" : "");
    }

    log_msg("\n  the top 30 blocks, disassembled\n");
    for (k = 0; k < nblk && k < 30; k++) {
        Blk *b = &blks[k];
        log_msg("\n    #%u  %s  %04X:%04X-%04X  x%lu  (%5.2f%%)  in %s\n",
                (unsigned)k + 1, pf_site(b->sel, b->start), b->sel, b->start,
                b->end, (unsigned long)b->count, pf_pct(b->weight),
                pf_site(b->sel, b->func));
        pf_disasm_range(b->sel, b->start, b->end, 20);
    }
}

static void pf_report_funcs(void)
{
    size_t k;
    uint64_t cum = 0;
    static const int marks[4] = { 25, 50, 75, 90 };
    int mi = 0;

    if (!nfn) return;
    qsort(fns, nfn, sizeof *fns, fn_cmp);
    log_msg("\n  functions (call targets): %llu seen\n", (unsigned long long)nfn);
    for (k = 0; k < nfn && mi < 4; k++) {
        cum += fns[k].weight;
        while (mi < 4 && pf_pct(cum) >= marks[mi]) {
            log_msg("    %2d%% of instructions in the top %5llu functions\n",
                    marks[mi], (unsigned long long)(k + 1));
            mi++;
        }
    }
    log_msg("\n  top functions               bytes  blocks       calls"
            "        weight      %%   cum%%   instr/call\n");
    cum = 0;
    for (k = 0; k < nfn && k < 50; k++) {
        Fn *f = &fns[k];
        cum += f->weight;
        log_msg("  %-13s %04X:%04X    %6u  %6lu  %10lu %13llu  %5.2f  %6.2f  %10.1f\n",
                pf_site(f->sel, f->ip), f->sel, f->ip,
                (unsigned)(f->next - f->ip), (unsigned long)f->nblk,
                (unsigned long)f->calls, (unsigned long long)f->weight,
                pf_pct(f->weight), pf_pct(cum),
                f->calls ? (double)f->weight / (double)f->calls : 0.0);
    }
    log_msg("\n  the top 10 functions, disassembled\n");
    for (k = 0; k < nfn && k < 10; k++) {
        Fn *f = &fns[k];
        log_msg("\n    #%u  %s  %04X:%04X  %lu calls  %5.2f%%\n", (unsigned)k + 1,
                pf_site(f->sel, f->ip), f->sel, f->ip, (unsigned long)f->calls,
                pf_pct(f->weight));
        pf_disasm_range(f->sel, f->ip, (uint16_t)(f->next - 1), 400);
    }
}

static void pf_report_time(void)
{
    LARGE_INTEGER q1, qf;
    uint64_t t1 = __rdtsc(), total, host = 0;
    double hz, secs;
    unsigned idx[PF_IMPORTS], a, b;

    QueryPerformanceCounter(&q1);
    QueryPerformanceFrequency(&qf);
    secs = (double)(q1.QuadPart - pf_qpc0.QuadPart) / (double)qf.QuadPart;
    total = t1 - pf_tsc0;
    if (!total || secs <= 0.0) return;
    hz = (double)total / secs;

    for (a = 0; a < PF_IMPORTS; a++) { idx[a] = a; host += pf_api_ticks[a]; }
    /* Insertion sort by ticks: a few hundred entries, once. */
    for (a = 1; a < PF_IMPORTS; a++) {
        unsigned v = idx[a];
        for (b = a; b && pf_api_ticks[idx[b - 1]] < pf_api_ticks[v]; b--)
            idx[b] = idx[b - 1];
        idx[b] = v;
    }

    log_msg("\n  time: %.3f s from prof_begin to exit (%.2f GHz timestamp counter)\n",
            secs, hz / 1e9);
    log_msg("    host API handlers, exclusive  %8.3f s  %5.1f%%\n",
            (double)host / hz, 100.0 * (double)host / (double)total);
    log_msg("    x87 fpu_exec, %llu ops       %8.3f s  %5.1f%%  (%.0f ns each)\n",
            (unsigned long long)pf_fpu_n, (double)pf_fpu_ticks / hz,
            100.0 * (double)pf_fpu_ticks / (double)total,
            pf_fpu_n ? (double)pf_fpu_ticks / hz * 1e9 / (double)pf_fpu_n : 0.0);
    log_msg("    REP string loops              %8.3f s  %5.1f%%\n",
            (double)pf_rep_ticks / hz, 100.0 * (double)pf_rep_ticks / (double)total);
    log_msg("    everything else is the interpreter\n");

    log_msg("\n  top API handlers by exclusive time\n");
    for (a = 0; a < 25 && pf_api_ticks[idx[a]]; a++) {
        ImpEntry *e = imp_slot(idx[a]);
        log_msg("    %-24s %10llu calls  %8.3f s  %5.1f%%  %8.1f us/call\n",
                e ? e->name : "?", (unsigned long long)pf_api_calls[idx[a]],
                (double)pf_api_ticks[idx[a]] / hz,
                100.0 * (double)pf_api_ticks[idx[a]] / (double)total,
                pf_api_calls[idx[a]]
                    ? (double)pf_api_ticks[idx[a]] / hz * 1e6 / (double)pf_api_calls[idx[a]]
                    : 0.0);
    }
}

const char *native_site_name(unsigned site);

static void pf_report_natives(void)
{
    unsigned k;
    uint64_t calls = 0, instrs = 0;

    for (k = 0; k < PF_NATIVES; k++) { calls += pf_nat_calls[k]; instrs += pf_nat_instrs[k]; }
    if (!calls) return;
    log_msg("\n  native routines: %llu calls stood in for %llu guest instructions"
            " (%.1f%% on top of the %llu interpreted)\n",
            (unsigned long long)calls, (unsigned long long)instrs,
            100.0 * (double)instrs / (double)pf_total, (unsigned long long)pf_total);
    for (k = 0; k < PF_NATIVES; k++) {
        if (!pf_nat_calls[k]) continue;
        log_msg("    %-28s %12llu calls  %14llu instructions  %7.1f each\n",
                native_site_name(k), (unsigned long long)pf_nat_calls[k],
                (unsigned long long)pf_nat_instrs[k],
                (double)pf_nat_instrs[k] / (double)pf_nat_calls[k]);
    }
}

void prof_report(void)
{
    unsigned i, k, order[PF_OPS], npair = 0;
    uint64_t distinct = 0, cum;
    char nm[96], nm2[96];
    struct { uint32_t n; uint16_t a, b; } top[40];

    pf_commit();
    if (!pf_total) return;

    log_msg("\n=== profile: %llu instructions ===\n",
            (unsigned long long)pf_total);

    /* --- opcodes --- */
    for (k = 0; k < PF_OPS; k++) order[k] = k;
    qsort(order, PF_OPS, sizeof *order, pf_cmp64);
    log_msg("\n  top opcodes                                    count      %%    cum%%\n");
    cum = 0;
    for (i = 0; i < 30 && pf_op[order[i]]; i++) {
        cum += pf_op[order[i]];
        pf_name(order[i], nm, sizeof nm);
        log_msg("  %-40.40s %12llu  %5.2f  %6.2f\n", nm,
                (unsigned long long)pf_op[order[i]], pf_pct(pf_op[order[i]]),
                pf_pct(cum));
    }

    /* --- adjacent pairs: what fusing would be fusing --- */
    memset(top, 0, sizeof top);
    for (k = 0; k < PF_OPS; k++) {
        unsigned j;
        for (j = 0; j < PF_OPS; j++) {
            uint32_t n = pf_pair[k][j];
            unsigned s;
            if (!n || (npair == 40 && n <= top[39].n)) continue;
            s = npair < 40 ? npair++ : 39;
            while (s && top[s - 1].n < n) { top[s] = top[s - 1]; s--; }
            top[s].n = n; top[s].a = (uint16_t)k; top[s].b = (uint16_t)j;
        }
    }
    cum = 0;
    log_msg("\n  top adjacent pairs                                                    count      %%    cum%%\n");
    for (i = 0; i < npair; i++) {
        cum += top[i].n;
        pf_name(top[i].a, nm, sizeof nm);
        pf_name(top[i].b, nm2, sizeof nm2);
        log_msg("  %-30.30s ; %-30.30s %12lu  %5.2f  %6.2f\n", nm, nm2,
                (unsigned long)top[i].n, pf_pct(top[i].n), pf_pct(cum));
    }

    /* --- the classes the fusion decision turns on --- */
    {
        uint64_t jcc = 0, fusable = 0, top20 = 0;
        unsigned j;
        for (k = 0; k < PF_OPS; k++) {
            if (!pf_is_jcc(k)) continue;
            jcc += pf_op[k];
            for (j = 0; j < PF_OPS; j++)
                if (pf_sets_flags(j)) fusable += pf_pair[j][k];
        }
        for (i = 0; i < npair && i < 20; i++) top20 += top[i].n;
        log_msg("\n  conditional branches      %12llu  %5.2f%%\n"
                "  ...directly after a flag producer, so fusable"
                "  %12llu  %5.2f%%\n"
                "  top 20 adjacent pairs                   %5.2f%% of all instructions\n",
                (unsigned long long)jcc, pf_pct(jcc),
                (unsigned long long)fusable, pf_pct(fusable), pf_pct(top20));
    }

    /* --- how much distinct code runs at all --- */
    log_msg("\n  code executed, by selector\n");
    for (i = 1; i < SEL_SLOTS; i++) {
        uint32_t b, starts = 0;
        if (!pf_hit[i] || !pf_sel[i]) continue;
        for (b = 0; b < pf_size[i]; b++) starts += pf_hit[i][b] != 0;
        distinct += starts;
        log_msg("    %-6s sel %04X  %10llu instructions  %6u distinct starts of %u bytes\n",
                pf_segno[i] ? pf_site(SEL_MAKE(i), 0) : "", (unsigned)SEL_MAKE(i),
                (unsigned long long)pf_sel[i], starts, (unsigned)pf_size[i]);
    }
    log_msg("    %llu distinct instruction addresses in all\n",
            (unsigned long long)distinct);

    /* --- REP, which the opcode histogram undercounts by design --- */
    if (pf_repins)
        log_msg("\n  REP: %llu instructions moved %llu elements (%.1f each)\n",
                (unsigned long long)pf_repins, (unsigned long long)pf_repelems,
                (double)pf_repelems / (double)pf_repins);

    /* --- did the game rewrite its own code? --- */
    {
        int dirty = 0;
        for (i = 1; i < SEL_SLOTS; i++) {
            uint32_t off;
            if (!pf_code[i]) continue;
            if (!memcmp(pf_code[i], sel_arena + ((size_t)i << 16), pf_size[i]))
                continue;
            for (off = 0; off < pf_size[i]; off++)
                if (pf_code[i][off] != sel_arena[((size_t)i << 16) + off]) break;
            log_msg("\n  *** code selector %04X changed, first at offset %04X\n",
                    (unsigned)SEL_MAKE(i), (unsigned)off);
            dirty = 1;
        }
        if (!dirty)
            log_msg("\n  no code selector changed: guest code is immutable here\n");
    }

    pf_report_blocks();
    pf_report_funcs();
    pf_report_natives();
    pf_report_time();
}

#endif
