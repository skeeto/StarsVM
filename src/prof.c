/* prof.c - see prof.h.  The whole file is nothing unless asked for. */

#include "prof.h"

#ifdef STARSVM_PROFILE

#include "sel.h"
#include "cpu.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A 0x0F instruction counts at 256 + its second byte, so one array covers both
   opcode pages and the pair histogram can speak about them too. */
#define PF_OPS 512

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

/* One bit per code byte that has started an instruction.  The population of
   this is what sizes a decode cache and predicts its hit rate before there is
   one to measure. */
static uint8_t *pf_seen[SEL_SLOTS];
static uint32_t pf_size[SEL_SLOTS];

/* Guest code as loaded.  A decode cache is silently wrong if the game rewrites
   its own code, and comparing at exit costs the hot path nothing, so this
   answers the question rather than assuming it. */
static uint8_t *pf_code[SEL_SLOTS];

static void pf_commit(void)
{
    if (!pf_have) return;
    pf_op[pf_pending]++;
    if (pf_prev < PF_OPS && pf_pair[pf_prev][pf_pending] != 0xFFFFFFFFu)
        pf_pair[pf_prev][pf_pending]++;
    pf_prev = pf_pending;
    pf_have = 0;
}

void prof_begin(void)
{
    unsigned i;

    for (i = 1; i < SEL_SLOTS; i++) {
        uint32_t n;
        if (sel_tab[i].kind != SK_CODE) continue;
        n = sel_tab[i].limit + 1u;
        if (n > SEL_SLOT) n = SEL_SLOT;
        pf_size[i] = n;
        pf_seen[i] = calloc((n + 7u) / 8u, 1);
        pf_code[i] = malloc(n);
        if (pf_code[i]) memcpy(pf_code[i], sel_arena + ((size_t)i << 16), n);
    }
}

void prof_op(uint16_t cs, uint16_t ip, uint8_t op)
{
    unsigned i = SEL_INDEX(cs);

    pf_commit();
    pf_pending = op;
    pf_have = 1;
    pf_total++;
    pf_sel[i]++;
    pf_at = ((uint32_t)cs << 16) | ip;
    if (!pf_first[op]) pf_first[op] = pf_at;
    if (pf_seen[i] && ip < pf_size[i])
        pf_seen[i][ip >> 3] |= (uint8_t)(1u << (ip & 7));
}

void prof_op2(uint8_t op2)
{
    pf_pending = 256u + op2;
    if (!pf_first[pf_pending]) pf_first[pf_pending] = pf_at;
}

void prof_rep(uint32_t elems)
{
    pf_repins++;
    pf_repelems += elems;
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
    disasm((uint16_t)(at >> 16), (uint16_t)at, line, sizeof line);
    /* disasm.c writes "%04X:%04X " then eight two-character byte slots then a
       space, so the text begins at column 27.  Taken from the format rather
       than counted off a sample, having first counted it off a sample wrong. */
    snprintf(out, (size_t)n, "%s", strlen(line) > 27 ? line + 27 : line);
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
        uint32_t b, bits = 0;
        if (!pf_seen[i] || !pf_sel[i]) continue;
        for (b = 0; b < (pf_size[i] + 7u) / 8u; b++) {
            uint8_t v = pf_seen[i][b];
            while (v) { bits += v & 1u; v >>= 1; }
        }
        distinct += bits;
        log_msg("    sel %04X  %10llu instructions  %6u distinct starts of %u bytes\n",
                (unsigned)SEL_MAKE(i), (unsigned long long)pf_sel[i], bits,
                (unsigned)pf_size[i]);
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
}

#endif
