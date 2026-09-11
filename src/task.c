#include "task.h"
#include "sel.h"
#include "heap.h"
#include "log.h"
#include "thunk.h"

#include <stdio.h>
#include <string.h>
#include <windows.h>

Task task;

static void put_str(uint16_t sel, uint16_t off, const char *s)
{
    while (*s) sel_wr8(sel, off++, (uint8_t)*s++);
    sel_wr8(sel, off, 0);
}

static void build_psp(Task *t)
{
    uint16_t sel = t->psp_sel;
    unsigned n = (unsigned)strlen(t->cmdline);
    unsigned i;

    if (n > 126) n = 126;

    sel_wr16(sel, PSP_INT20, 0x20CD);            /* int 20h */

    /* The ancient `call PSP:0005` convention reaches DOS through the same entry
       as KERNEL.102, so point it at that thunk. */
    sel_wr8(sel, PSP_DISPATCH, 0x9A);            /* call far */
    {
        uint32_t dos3 = thunk_resolve("KERNEL", 102, NULL);
        sel_wr16(sel, PSP_DISPATCH + 1, SEGPTR_OFF(dos3));
        sel_wr16(sel, PSP_DISPATCH + 3, SEGPTR_SEL(dos3));
    }

    for (i = 0; i < 20; i++) sel_wr8(sel, (uint16_t)(0x18 + i), 0xFF);
    sel_wr16(sel, PSP_PARENT, 0);
    sel_wr16(sel, PSP_ENVIRON, t->env_sel);
    sel_wr16(sel, 0x32, 20);                      /* handle table size */
    sel_wr16(sel, 0x34, 0);                       /* handle table pointer */
    sel_wr16(sel, 0x36, sel);

    sel_wr8(sel, PSP_CMDLINE, (uint8_t)n);
    for (i = 0; i < n; i++)
        sel_wr8(sel, (uint16_t)(PSP_CMDLINE + 1 + i), (uint8_t)t->cmdline[i]);
    sel_wr8(sel, (uint16_t)(PSP_CMDLINE + 1 + n), 0x0D);
}

static void build_environment(Task *t)
{
    /* A DOS-style environment: NUL-terminated NAME=VALUE strings, an empty
       string to end the block, then a word 1 and the program path. */
    uint16_t off = 0;
    static const char *vars[] = { "COMSPEC=C:\\WINDOWS\\SYSTEM32\\CMD.EXE", NULL };
    int i;

    for (i = 0; vars[i]; i++) {
        put_str(t->env_sel, off, vars[i]);
        off = (uint16_t)(off + strlen(vars[i]) + 1);
    }
    sel_wr8(t->env_sel, off++, 0);
    sel_wr16(t->env_sel, off, 1);
    off = (uint16_t)(off + 2);
    put_str(t->env_sel, off, t->exepath);
}

int task_start(NeModule *m, Cpu *c, const char *cmdline, int ncmdshow)
{
    Task *t = &task;
    NeSeg *cs = ne_seg(m, (unsigned)(m->csip >> 16));
    NeSeg *dg = ne_seg(m, m->autodata);
    uint16_t sp, heap_base;
    char *slash;

    if (!cs || !dg) {
        log_msg("task: module has no entry or data segment\n");
        return 0;
    }

    memset(t, 0, sizeof *t);
    t->mod = m;
    t->hinstance = dg->sel;
    t->hmodule = dg->sel;
    t->ncmdshow = ncmdshow;
    snprintf(t->cmdline, sizeof t->cmdline, "%s", cmdline ? cmdline : "");

    /* Wide first, because that is the path that has to actually open.  The
       narrow copies below are for the guest, which has no other way to see a
       filename; a character the code page cannot spell survives in exepathw and
       is lost in exepath, and only the latter is ever merely shown. */
    if (!GetFullPathNameW(m->path, sizeof t->exepathw / sizeof *t->exepathw,
                          t->exepathw, NULL))
        _snwprintf(t->exepathw, sizeof t->exepathw / sizeof *t->exepathw - 1,
                   L"%ls", m->path);
    t->exepathw[sizeof t->exepathw / sizeof *t->exepathw - 1] = 0;
    wcscpy(t->exedirw, t->exepathw);
    { wchar_t *w = wcsrchr(t->exedirw, L'\\'); if (w) *w = 0; }

    WideCharToMultiByte(CP_ACP, 0, t->exepathw, -1,
                        t->exepath, sizeof t->exepath, NULL, NULL);
    t->exepath[sizeof t->exepath - 1] = 0;
    memcpy(t->exedir, t->exepath, sizeof t->exedir);
    t->exedir[sizeof t->exedir - 1] = 0;
    slash = strrchr(t->exedir, '\\');
    if (slash) *slash = 0;

    t->psp_sel = sel_alloc(PSP_SIZE, SK_DATA);
    t->env_sel = sel_alloc(0x800, SK_DATA);
    if (!t->psp_sel || !t->env_sel) {
        log_msg("task: cannot allocate the PSP or environment\n");
        return 0;
    }
    build_environment(t);
    build_psp(t);

    /* DGROUP layout, bottom to top: instance data and static data, then the
       stack growing down from `sp`, then the local heap at the very top. */
    sp = (uint16_t)(m->sssp & 0xFFFF);
    if (sp == 0) {
        uint32_t top = dg->size - m->heap;
        sp = (uint16_t)(top > 0xFFFEu ? 0xFFFEu : top);
    }
    sp &= (uint16_t)~1u;

    heap_base = sp;
    if (m->heap && (uint32_t)heap_base + m->heap <= dg->size)
        lmem_init(dg->sel, heap_base, m->heap);
    else if (m->heap)
        log_msg("task: no room for a %u-byte local heap above sp=%04X\n",
                m->heap, sp);

    /* Instance data.  The stack limit is what InitTask reports in CX. */
    sel_wr16(dg->sel, ID_NULL, 0);
    sel_wr16(dg->sel, ID_HEAP, lmem_base());
    sel_wr16(dg->sel, ID_ATOMTABLE, 0);
    t->stacktop = (uint16_t)(sp > m->stack ? sp - m->stack : 0);
    sel_wr16(dg->sel, ID_STACKTOP, t->stacktop);
    sel_wr16(dg->sel, ID_STACKMIN, sp);
    sel_wr16(dg->sel, ID_STACKBOTTOM, sp);

    cpu_reset(c);
    c->seg[S_CS] = cs->sel;
    c->eip       = m->csip & 0xFFFF;
    c->seg[S_SS] = dg->sel;
    c->seg[S_DS] = dg->sel;
    c->seg[S_ES] = t->psp_sel;
    set_reg16(c, R_SP, sp);

    /* Registers at a Win16 entry point: ax 0, bx stack size, cx heap size,
       si hPrevInstance, di hInstance, bp 0. */
    set_reg16(c, R_AX, 0);
    set_reg16(c, R_BX, m->stack);
    set_reg16(c, R_CX, m->heap);
    set_reg16(c, R_DX, 0);
    set_reg16(c, R_SI, 0);
    set_reg16(c, R_DI, dg->sel);
    set_reg16(c, R_BP, 0);
    return 1;
}
