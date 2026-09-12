/* thunk.c - dispatching guest calls out to host handlers, and host calls back
   into guest code. */

#include "thunk.h"
#include "sel.h"
#include "log.h"
#include "fpu.h"

#include <stdio.h>
#include <string.h>

ImpEntry *imp_slot(unsigned index);
unsigned  imp_index_for_offset(uint16_t off);

/* Keep going after an unimplemented API (returning zero) instead of stopping.
   Useful for surveying what the game calls; wrong answers, but a long log. */
int thunk_survey;

/* The last import that had no handler, so the stop can name it even where the
   log went nowhere. */
static char last_missing[64];

const char *thunk_last_missing(void)
{
    return last_missing[0] ? last_missing : NULL;
}

static uint16_t ret_sel;
static int      depth;

int call16_depth(void) { return depth; }

/* How often to note that a callback has not come back yet.  Large enough that
   a slow but honest one stays quiet for a good while, small enough that a stuck
   one gets named within seconds of interpreted time. */
#define CALL16_NOTE 1000000000ull

uint16_t call16_ret_selector(void) { return ret_sel; }

int call16_init(void)
{
    ret_sel = sel_alloc(0x10000u, SK_CODE);
    if (!ret_sel) {
        log_msg("thunk: cannot allocate the return selector\n");
        return 0;
    }
    return 1;
}

/* --------------------------------------------------------- guest -> host ---- */

static void log_call(const ImpEntry *e, const uint16_t *words, unsigned nwords)
{
    char buf[256];
    int n = 0;
    unsigned i;

    n += snprintf(buf + n, sizeof buf - n, "%s.%u %s(",
                  e->module, e->ordinal, e->name);
    /* Print in declaration order, i.e. from the top of the block downwards. */
    for (i = nwords; i-- > 0 && n < (int)sizeof buf - 8; )
        n += snprintf(buf + n, sizeof buf - n, "%04X%s", words[i], i ? " " : "");
    snprintf(buf + n, sizeof buf - n, ")");
    log_msg("%s\n", buf);
}

void thunk_dispatch(Cpu *c, uint16_t off)
{
    unsigned index = imp_index_for_offset(off);
    ImpEntry *e = imp_slot(index);
    uint16_t ss = c->seg[S_SS];
    uint16_t sp = reg16(c, R_SP);
    uint16_t ret_ip, ret_cs;
    const uint16_t *words;
    uint32_t result = 0;
    Args a;

    if (!e) {
        log_msg("*** call into thunk selector at offset %04X, which is not a slot\n",
                off);
        c->state = CPU_BADOP;
        return;
    }

    ret_ip = sel_rd16(ss, sp);
    ret_cs = sel_rd16(ss, (uint16_t)(sp + 2));
    words  = (const uint16_t *)sel_ptr(ss, (uint16_t)(sp + 4));

    if (log_verbose) log_call(e, words, e->pop / 2u);

    if (!e->fn) {
        snprintf(last_missing, sizeof last_missing, "%s.%u %s",
                 e->module, e->ordinal, e->name);
        log_msg("*** %s.%u %s is not implemented (called from %04X:%04X)\n",
                e->module, e->ordinal, e->name, ret_cs, ret_ip);
        if (!log_verbose) log_call(e, words, e->pop / 2u);
        if (!thunk_survey) {
            c->state = CPU_NOAPI;
            c->bad_cs = ret_cs;
            c->bad_ip = ret_ip;
            return;
        }
    } else {
        a.top = words + e->pop / 2u;
        result = e->fn(c, &a);
    }

    /* A register-convention entry (InitTask, DOS3Call, the FP dispatcher) takes
       no stack arguments and returns through the whole register file, so its
       handler has already set what it needs; overwriting AX and DX here would
       destroy the result. */
    if (!(e->flags & IMP_REGISTER)) {
        set_reg16(c, R_AX, (uint16_t)result);
        if (e->flags & IMP_RET32) set_reg16(c, R_DX, (uint16_t)(result >> 16));
    }

    /* Do the RETF the real entry point would have done.  A cdecl entry leaves
       the arguments for the caller to remove. */
    sp = (uint16_t)(sp + 4 + ((e->flags & IMP_CDECL) ? 0 : e->pop));
    set_reg16(c, R_SP, sp);
    c->seg[S_CS] = ret_cs;
    c->eip = ret_ip;
}

/* --------------------------------------------------------- host -> guest ---- */

uint32_t call16_wndproc(uint32_t proc, uint16_t ax,
                        const uint16_t *args, unsigned nbytes,
                        void *extra, unsigned extralen)
{
    Cpu *c = &cpu;
    Cpu saved;
    uint32_t result;
    unsigned i;
    uint16_t sp;
    uint16_t extra_sel = 0, extra_off = 0;
    int r;

    if (!proc) return 0;
    if (depth >= 32) {
        log_msg("*** call16 nested %d deep; refusing to go further\n", depth);
        c->bad_cs = SEGPTR_SEL(proc);
        c->bad_ip = SEGPTR_OFF(proc);
        cpu_stop_latch(c, CPU_FAULT);
        return 0;
    }

    saved = *c;

    /* Guest stack check: leave room for the frame plus headroom for whatever the
       callee does.  DGROUP holds only ne_stack bytes, and deep callback chains
       are how that gets exhausted. */
    sp = reg16(c, R_SP);
    if (sp < nbytes + extralen + 0x200) {
        log_msg("*** guest stack exhausted (sp=%04X, need %u) at call16 depth %d\n",
                sp, nbytes + extralen + 0x200, depth);
        c->bad_cs = SEGPTR_SEL(proc);
        c->bad_ip = SEGPTR_OFF(proc);
        cpu_stop_latch(c, CPU_FAULT);
        return 0;
    }

    /* Optionally place a struct on the guest stack and point the last four
       argument bytes at it.  Win16 code frequently treats such an lParam as a
       near pointer, which only works when it really lives in SS. */
    if (extra && extralen) {
        sp = (uint16_t)(sp - extralen);
        set_reg16(c, R_SP, sp);
        memcpy(sel_ptr(c->seg[S_SS], sp), extra, extralen);
        extra_sel = c->seg[S_SS];
        extra_off = sp;
    }

    /* args[0] is the last declared argument, so pushing from the top down leaves
       args[0] at the lowest address - the layout the callee expects. */
    for (i = nbytes / 2u; i-- > 0; ) {
        uint16_t w = args[i];
        if (extra && extralen && i < 2) {
            /* Rewrite the far pointer to point at the copy we just made. */
            w = (i == 0) ? sp : c->seg[S_SS];
        }
        cpu_push16(c, w);
    }

    depth++;
    cpu_push16(c, ret_sel);
    cpu_push16(c, (uint16_t)depth);

    c->seg[S_CS] = SEGPTR_SEL(proc);
    c->eip       = SEGPTR_OFF(proc);
    c->seg[S_DS] = c->seg[S_SS];
    c->seg[S_ES] = c->seg[S_SS];
    set_reg16(c, R_AX, ax ? ax : c->seg[S_SS]);
    set_reg16(c, R_BP, (uint16_t)(reg16(c, R_SP) + 2));

    /* No ceiling on a callback's instructions, and this is the second design.
       The first gave every callback 200 million and treated exceeding it as
       fatal, which cannot work: no instruction count distinguishes a callback
       that will never return from one merely doing a great deal of work, both
       being "has not come back yet", and the game does plenty of the latter.
       Generating a turn on a large map with many players runs well past 200
       million inside a single WM_COMMAND, so the budget ended games in
       progress - much the worse of the two failures.

       What it was for was making a callback that cannot return visible rather
       than silent, and a periodic note does that while ending nothing.

       --steps cannot be honoured here either, which was the other thing tried:
       a stop latched inside a callback unwinds only if control returns to an
       enclosing cpu_run, and when the callback came from a Win32 modal loop it
       does not - the loop goes on pumping messages whose callbacks all refuse
       to run, leaving the program alive with nothing reported.  So --steps
       bounds the outer loop only, and its help text says so. */
    {
        uint64_t ran = 0;
        for (;;) {
            r = cpu_run(c, CALL16_NOTE);
            if (r != CPU_STEPS) break;
            ran += CALL16_NOTE;
            log_msg("*** guest callback %04X:%04X has not returned after %llu"
                    " instructions, still going\n",
                    SEGPTR_SEL(proc), SEGPTR_OFF(proc),
                    (unsigned long long)ran);
        }
    }
    depth--;

    /* Read the struct back before the CPU state is restored; the bytes live in
       the arena and are still there, but the selector and offset are only known
       here. */
    if (extra && extralen)
        memcpy(extra, sel_ptr(extra_sel, extra_off), extralen);

    if (r != CPU_RETURN) {
        if (!cpu_stop_latched())
            log_msg("*** guest callback %04X:%04X stopped: %s at %04X:%04X"
                    " (op %02X %02X)\n",
                    SEGPTR_SEL(proc), SEGPTR_OFF(proc), cpu_state_name(r),
                    c->bad_cs, c->bad_ip, c->bad_op, c->bad_op2);
        /* The host frames between here and the interpreter - DispatchMessage, a
           modal dialog loop - have no way to carry a failure back out, and
           returning 0 to Win32 as though nothing happened is exactly how the
           serial-number dialog vanished without trace.  Latch it instead, so no
           further guest instruction runs and one report comes out at the top. */
        if (r == CPU_NOAPI || r == CPU_BADOP || r == CPU_FAULT)
            cpu_stop_latch(c, r);
        result = 0;
    } else {
        result = (uint32_t)reg16(c, R_AX) | ((uint32_t)reg16(c, R_DX) << 16);
    }

    /* Restore everything except the result.  Like Wine, we do not trust the
       callee to have balanced the stack.
       icount is not part of the machine being restored: it counts what the run
       has executed, and rolling it back discarded every instruction a callback
       ever ran.  Since the game does nearly all of its work inside callbacks -
       turn generation entirely so - the count has always reported a tiny
       fraction of a GUI run.  Ten generated turns and one generated turn came
       out 48 instructions apart. */
    {
        uint32_t ax_ = c->r32[R_AX], dx_ = c->r32[R_DX];
        uint64_t ran = c->icount;

        /* The FPU carries forward rather than being restored.  Its registers,
           control, status and tag words are task-global on real Win16 - a
           callee that changes the control word has changed it for everyone,
           and nothing in the calling convention makes any of it caller-saved.
           They were rolled back only because they share this struct with the
           machine registers, which is the same accident that hid icount.
           Measured before changing it: over twenty generated turns and a GUI
           session the control word never moved across a callback, and the tag
           word read FFFF - every register empty, as the convention requires -
           at every single boundary.  So this costs nothing today.  What it buys
           is that a callback which does leave something behind now keeps it,
           and one that leaves the stack unbalanced shows up as itself rather
           than being quietly tidied away. */
        saved.fpu_cw  = c->fpu_cw;
        saved.fpu_sw  = c->fpu_sw;
        saved.fpu_tw  = c->fpu_tw;
        saved.fpu_top = c->fpu_top;
        memcpy(saved.st, c->st, sizeof saved.st);

        *c = saved;
        c->r32[R_AX] = ax_;
        c->r32[R_DX] = dx_;
        c->icount = ran;
        c->state = CPU_RUNNING;
    }
    return result;
}

uint32_t call16(uint32_t proc, const uint16_t *args, unsigned nbytes)
{
    return call16_wndproc(proc, 0, args, nbytes, NULL, 0);
}
