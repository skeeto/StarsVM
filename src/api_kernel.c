/* api_kernel.c - the KERNEL entry points Stars! imports. */

#include "thunk.h"
#include "task.h"
#include "heap.h"
#include "sel.h"
#include "log.h"

#include <stdio.h>
#include <string.h>
#include <windows.h>

/* Copy a NUL-terminated guest string out to host memory. */
static char *guest_str(uint32_t segptr, char *buf, size_t n)
{
    uint16_t sel = SEGPTR_SEL(segptr), off = SEGPTR_OFF(segptr);
    size_t i = 0;

    if (!segptr) { buf[0] = 0; return buf; }
    while (i + 1 < n) {
        uint8_t ch = sel_rd8(sel, (uint16_t)(off + i));
        if (!ch) break;
        buf[i++] = (char)ch;
    }
    buf[i] = 0;
    return buf;
}

static void put_guest_str(uint32_t segptr, const char *s, unsigned max)
{
    uint16_t sel = SEGPTR_SEL(segptr), off = SEGPTR_OFF(segptr);
    unsigned i = 0;

    if (!segptr || !max) return;
    for (; s[i] && i + 1 < max; i++)
        sel_wr8(sel, (uint16_t)(off + i), (uint8_t)s[i]);
    sel_wr8(sel, (uint16_t)(off + i), 0);
}

/* ------------------------------------------------------------- task startup */

/* KERNEL.91 InitTask - register convention.
   Returns: AX 1 on success, BX the PSP-relative offset of the command line,
   CX the stack limit, DX nCmdShow, ES:BX the command line, SI hPrevInstance,
   DI hInstance.  It also pushes a zero word to terminate the BP chain. */
static uint32_t k_InitTask(Cpu *c, Args *a)
{
    uint16_t bx = 0x80;
    unsigned i;

    (void)a;

    /* BX points at the first non-blank character of the command line, or at the
       length byte itself when there are no arguments. */
    for (i = 1; i <= 126; i++) {
        uint8_t ch = sel_rd8(task.psp_sel, (uint16_t)(PSP_CMDLINE + i));
        if (ch == 0x0D) break;
        if (ch != ' ' && ch != '\t') { bx = (uint16_t)(PSP_CMDLINE + i); break; }
    }

    set_reg16(c, R_AX, 1);
    set_reg16(c, R_BX, bx);
    set_reg16(c, R_CX, task.stacktop);
    set_reg16(c, R_DX, (uint16_t)task.ncmdshow);
    set_reg16(c, R_SI, 0);                       /* hPrevInstance */
    set_reg16(c, R_DI, task.hinstance);
    c->seg[S_ES] = task.psp_sel;

    cpu_push16(c, 0);                            /* root of the BP chain */
    return 0;
}

/* KERNEL.3 GetVersion - a Windows 3.10 app expects 3.10, with the DOS version
   in the high word.  The bytes are swapped relative to the obvious order, which
   is why the startup code does `xchg al,ah` right after calling this. */
static uint32_t k_GetVersion(Cpu *c, Args *a)
{
    (void)c; (void)a;
    return 0x0A03u | (0x0616u << 16);            /* Windows 3.10, DOS 6.22 */
}

static uint32_t k_WaitEvent(Cpu *c, Args *a)
{
    (void)c;
    arg_word(a);                                 /* task handle, ignored */
    return 0;                                    /* no event was pending */
}

static uint32_t k_Yield(Cpu *c, Args *a)
{
    (void)c; (void)a;
    return 1;
}

static uint32_t k_GetModuleFileName(Cpu *c, Args *a)
{
    uint16_t hmod = arg_word(a);
    uint32_t buf  = arg_long(a);
    int16_t  size = arg_sword(a);

    (void)c; (void)hmod;
    put_guest_str(buf, task.exepath, (unsigned)(size > 0 ? size : 0));
    return (uint32_t)strlen(task.exepath);
}

static uint32_t k_GetDOSEnvironment(Cpu *c, Args *a)
{
    (void)c; (void)a;
    return SEGPTR(task.env_sel, 0);
}

static uint32_t k_GetDriveType(Cpu *c, Args *a)
{
    uint16_t drive = arg_word(a);
    char root[4];
    UINT t;

    (void)c;
    root[0] = (char)('A' + drive);
    root[1] = ':';
    root[2] = '\\';
    root[3] = 0;
    t = GetDriveTypeA(root);
    /* Win16 numbered these the way Win32 later kept them - removable 2,
       fixed 3, remote 4 - and differed in only two places: MSCDEX reached a
       CD-ROM through the network redirector, so one reports remote, and a
       root that is not there is merely unknown.  Both are load-bearing.  The
       game's machine fingerprint (see docs/copy-protection.md) runs only for
       a drive reporting 3, which on Win16 is an ordinary hard disk; an
       off-by-one here reports every hard disk as removable instead, and
       leaves the fingerprint reading a compile-time constant. */
    if (t == DRIVE_CDROM) t = DRIVE_REMOTE;
    else if (t == DRIVE_NO_ROOT_DIR) t = DRIVE_UNKNOWN;
    return t;
}

static uint32_t k_FatalExit(Cpu *c, Args *a)
{
    int16_t code = arg_sword(a);
    log_msg("*** the guest called FatalExit(%d)\n", code);
    c->state = CPU_HALT;
    return 0;
}

static uint32_t k_FatalAppExit(Cpu *c, Args *a)
{
    char buf[256];
    uint16_t flags = arg_word(a);
    uint32_t text = arg_long(a);

    (void)flags;
    log_msg("*** the guest called FatalAppExit: %s\n",
            guest_str(text, buf, sizeof buf));
    c->state = CPU_HALT;
    return 0;
}

/* --------------------------------------------------------------- string ops */

static uint32_t k_lstrlen(Cpu *c, Args *a)
{
    uint32_t s = arg_long(a);
    uint16_t sel = SEGPTR_SEL(s), off = SEGPTR_OFF(s);
    uint32_t n = 0;

    (void)c;
    if (!s) return 0;
    while (sel_rd8(sel, (uint16_t)(off + n))) n++;
    return n;
}

static uint32_t k_lstrcpy(Cpu *c, Args *a)
{
    uint32_t dst = arg_long(a), src = arg_long(a);
    uint16_t ds = SEGPTR_SEL(dst), doff = SEGPTR_OFF(dst);
    uint16_t ss = SEGPTR_SEL(src), soff = SEGPTR_OFF(src);
    uint32_t i = 0;

    (void)c;
    if (!dst || !src) return 0;
    for (;;) {
        uint8_t ch = sel_rd8(ss, (uint16_t)(soff + i));
        sel_wr8(ds, (uint16_t)(doff + i), ch);
        if (!ch) break;
        i++;
    }
    return dst;
}

static uint32_t k_lstrcat(Cpu *c, Args *a)
{
    uint32_t dst = arg_long(a), src = arg_long(a);
    uint16_t ds = SEGPTR_SEL(dst), doff = SEGPTR_OFF(dst);
    uint16_t ss = SEGPTR_SEL(src), soff = SEGPTR_OFF(src);
    uint32_t n = 0, i = 0;

    (void)c;
    if (!dst || !src) return 0;
    while (sel_rd8(ds, (uint16_t)(doff + n))) n++;
    for (;;) {
        uint8_t ch = sel_rd8(ss, (uint16_t)(soff + i));
        sel_wr8(ds, (uint16_t)(doff + n + i), ch);
        if (!ch) break;
        i++;
    }
    return dst;
}

/* -------------------------------------------------------------------- heaps */

static uint32_t k_GlobalAlloc(Cpu *c, Args *a)
{
    uint16_t flags = arg_word(a);
    uint32_t bytes = arg_long(a);
    (void)c;
    return gmem_alloc(flags, bytes);
}

static uint32_t k_GlobalReAlloc(Cpu *c, Args *a)
{
    uint16_t h = arg_word(a);
    uint32_t bytes = arg_long(a);
    uint16_t flags = arg_word(a);
    (void)c;
    return gmem_realloc(h, bytes, flags);
}

static uint32_t k_GlobalFree(Cpu *c, Args *a)
{
    (void)c;
    return gmem_free(arg_word(a));
}

static uint32_t k_GlobalLock(Cpu *c, Args *a)
{
    uint16_t h = arg_word(a);
    uint16_t sel;

    if (h == 0xFFFF) sel = c->seg[S_DS];         /* 0xFFFF means "current DS" */
    else { gmem_lock(h); sel = gmem_sel(h); }
    if (!sel) return 0;
    /* Real KERNEL also leaves the selector in CX, and some code relies on it. */
    set_reg16(c, R_CX, sel);
    return SEGPTR(sel, 0);
}

static uint32_t k_GlobalUnlock(Cpu *c, Args *a)
{
    (void)c;
    return (uint32_t)gmem_unlock(arg_word(a));
}

static uint32_t k_GlobalSize(Cpu *c, Args *a)
{
    (void)c;
    return gmem_size(arg_word(a));
}

static uint32_t k_LockSegment(Cpu *c, Args *a)
{
    uint16_t h = arg_word(a);
    if (h == 0xFFFF) h = c->seg[S_DS];
    /* Nothing is ever moved or discarded here, so this only tracks the count. */
    gmem_lock(h);
    return h;
}

static uint32_t k_UnlockSegment(Cpu *c, Args *a)
{
    uint16_t h = arg_word(a);
    if (h == 0xFFFF) h = c->seg[S_DS];
    gmem_unlock(h);
    return h;
}

static uint32_t k_LocalAlloc(Cpu *c, Args *a)
{
    uint16_t flags = arg_word(a);
    uint16_t bytes = arg_word(a);
    return lmem_alloc(c->seg[S_DS], flags, bytes);
}

static uint32_t k_LocalReAlloc(Cpu *c, Args *a)
{
    uint16_t h = arg_word(a);
    uint16_t bytes = arg_word(a);
    uint16_t flags = arg_word(a);
    return lmem_realloc(c->seg[S_DS], h, bytes, flags);
}

static uint32_t k_LocalFree(Cpu *c, Args *a)
{
    return lmem_free(c->seg[S_DS], arg_word(a));
}

static uint32_t k_LocalSize(Cpu *c, Args *a)
{
    return lmem_size(c->seg[S_DS], arg_word(a));
}

/* ------------------------------------------------------- instance thunks --- */

/* MakeProcInstance builds a six-byte stub that loads AX with the instance's
   data selector and jumps to the real procedure.  Returning the input unchanged
   would usually work here (there is only ever one instance), but synthesizing
   the real thunk costs nothing and keeps AX correct for code that depends on it. */
static uint16_t thunk_seg;
static uint16_t thunk_next;

static uint32_t k_MakeProcInstance(Cpu *c, Args *a)
{
    uint32_t proc = arg_long(a);
    uint16_t inst = arg_word(a);
    uint16_t off;

    (void)c;
    if (!proc) return 0;
    if (!thunk_seg) {
        thunk_seg = sel_alloc(0x1000, SK_CODE);
        if (!thunk_seg) return proc;
    }
    if (thunk_next + 6 > 0x1000) {
        log_msg("MakeProcInstance: out of thunk slots\n");
        return proc;
    }
    off = thunk_next;
    thunk_next = (uint16_t)(thunk_next + 6);

    sel_wr8 (thunk_seg, off, 0xB8);                       /* mov ax, inst   */
    sel_wr16(thunk_seg, (uint16_t)(off + 1), inst ? inst : task.hinstance);
    sel_wr8 (thunk_seg, (uint16_t)(off + 3), 0xEA);       /* jmp far proc   */
    sel_wr16(thunk_seg, (uint16_t)(off + 4), SEGPTR_OFF(proc));
    sel_wr16(thunk_seg, (uint16_t)(off + 6), SEGPTR_SEL(proc));
    thunk_next = (uint16_t)(off + 8);
    return SEGPTR(thunk_seg, off);
}

static uint32_t k_FreeProcInstance(Cpu *c, Args *a)
{
    (void)c;
    arg_long(a);
    return 1;              /* the slots are never reused; there are few enough */
}

/* ------------------------------------------------------------------- wiring */

void api_kernel_register(void)
{
    api_bind("KERNEL",   1, k_FatalExit);
    api_bind("KERNEL",   3, k_GetVersion);
    api_bind("KERNEL",   5, k_LocalAlloc);
    api_bind("KERNEL",   6, k_LocalReAlloc);
    api_bind("KERNEL",   7, k_LocalFree);
    api_bind("KERNEL",  10, k_LocalSize);
    api_bind("KERNEL",  15, k_GlobalAlloc);
    api_bind("KERNEL",  16, k_GlobalReAlloc);
    api_bind("KERNEL",  17, k_GlobalFree);
    api_bind("KERNEL",  18, k_GlobalLock);
    api_bind("KERNEL",  19, k_GlobalUnlock);
    api_bind("KERNEL",  20, k_GlobalSize);
    api_bind("KERNEL",  23, k_LockSegment);
    api_bind("KERNEL",  24, k_UnlockSegment);
    api_bind("KERNEL",  29, k_Yield);
    api_bind("KERNEL",  30, k_WaitEvent);
    api_bind("KERNEL",  49, k_GetModuleFileName);
    api_bind("KERNEL",  51, k_MakeProcInstance);
    api_bind("KERNEL",  52, k_FreeProcInstance);
    api_bind("KERNEL",  88, k_lstrcpy);
    api_bind("KERNEL",  89, k_lstrcat);
    api_bind("KERNEL",  90, k_lstrlen);
    api_bind("KERNEL",  91, k_InitTask);
    api_bind("KERNEL", 131, k_GetDOSEnvironment);
    api_bind("KERNEL", 136, k_GetDriveType);
    api_bind("KERNEL", 137, k_FatalAppExit);
}
