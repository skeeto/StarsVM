/* thunk.h - the Win16/Win32 call boundary, in both directions.
 *
 * Guest -> host: every imported (module, ordinal) gets an 8-byte slot in one
 * reserved "thunk" selector, and the NE loader points each import fixup at it.
 * The interpreter checks for that selector at the top of its loop, so a call is
 * caught however it arrives - 9A, EA, FF /3, FF /5, or a jump through a
 * MakeProcInstance thunk - without ever decoding an instruction there.
 *
 * Host -> guest: call16() builds a Pascal frame on the guest stack, pushes a
 * return address in a second reserved selector whose offset encodes the current
 * nesting depth, and runs the interpreter until control comes back to exactly
 * that address.  Distinct depths mean reentrancy is unambiguous.
 */
#ifndef THUNK_H
#define THUNK_H

#include <stdint.h>
#include "cpu.h"

/* Import entry flags. */
#define IMP_RET32     0x01   /* returns a DWORD in DX:AX rather than a word in AX */
#define IMP_CDECL     0x02   /* caller cleans the stack                           */
#define IMP_REGISTER  0x04   /* arguments and results are the whole register file */
#define IMP_EQUATE    0x08   /* resolves to a constant, not to code               */

/* Reading arguments.  The Pascal convention pushes left to right, so the first
   declared argument ends up at the HIGHEST address.  An Args cursor starts just
   past it and walks down, which means a handler consumes arguments in plain
   declaration order:

       static uint32_t user_SetWindowText(Cpu *c, Args *a)
       {
           uint16_t hwnd = arg_word(a);      // first declared
           uint32_t text = arg_long(a);      // second declared
           ...
       }

   Getting this backwards is the classic Win16 shim bug, so the cursor exists
   specifically to make the natural reading the correct one. */
typedef struct {
    const uint16_t *top;    /* one past the argument word not yet consumed */
} Args;

static inline uint16_t arg_word(Args *a)
{
    a->top -= 1;
    return a->top[0];
}

static inline int16_t arg_sword(Args *a)
{
    return (int16_t)arg_word(a);
}

static inline uint32_t arg_long(Args *a)
{
    a->top -= 2;
    return (uint32_t)a->top[0] | ((uint32_t)a->top[1] << 16);
}

/* cdecl is the other way round: the caller pushes right to left and cleans up,
   so the first declared argument sits at the LOWEST address and the cursor
   walks up.  Reading a cdecl entry with the descending accessors above would
   walk off the bottom of the argument block into the return address. */
static inline uint16_t arg_word_up(Args *a)
{
    uint16_t v = a->top[0];
    a->top += 1;
    return v;
}

static inline int16_t arg_sword_up(Args *a)
{
    return (int16_t)arg_word_up(a);
}

static inline uint32_t arg_long_up(Args *a)
{
    uint32_t v = (uint32_t)a->top[0] | ((uint32_t)a->top[1] << 16);
    a->top += 2;
    return v;
}

/* An API handler.  Return the 32-bit result; it lands in DX:AX, or in AX alone
   when the entry is not IMP_RET32. */
typedef uint32_t (*ApiFn)(Cpu *c, Args *a);

typedef struct {
    const char *module;
    uint16_t    ordinal;
    const char *name;
    uint16_t    pop;
    uint16_t    flags;
    uint32_t    value;    /* IMP_EQUATE only */
    ApiFn       fn;
} ImpEntry;

int      thunk_init(void);
uint16_t thunk_selector(void);

/* The selector whose slots are call16 return addresses.  The interpreter
   recognises it to unwind one callback frame. */
uint16_t call16_ret_selector(void);

/* Resolve an import to a far pointer for the NE loader.  Equates come back as
   0xFFFF:value, which is how a constant reaches an OFFSET-type fixup. */
uint32_t thunk_resolve(const char *module, uint16_t ordinal, void *user);

/* Bind a handler.  Called by the api_* modules at startup.  Returns 0 if the
   module/ordinal is not one this binary imports. */
int      api_bind(const char *module, uint16_t ordinal, ApiFn fn);

/* Dispatch: the interpreter calls this when CS is the thunk selector.
   Performs the call and the RETF, leaving the CPU ready to continue. */
void     thunk_dispatch(Cpu *c, uint16_t off);

/* Report which imports were never given a handler. */
void     thunk_report_unbound(void);

/* The last import reached with no handler, or NULL. */
const char *thunk_last_missing(void);

/* --------------------------------------------------------- host -> guest ---- */

/* Push `nbytes` of argument words (already in Pascal order: args[0] is the
   deepest/leftmost) and call the 16-bit procedure at `proc`.  Returns DX:AX. */
uint32_t call16(uint32_t proc, const uint16_t *args, unsigned nbytes);

/* As call16, but also sets AX on entry (window procedures want hInstance there)
   and optionally copies `extra` bytes onto the guest stack, rewriting the last
   4 argument bytes to point at the copy.  That is the WM_CREATE/WM_DRAWITEM
   accommodation: Win16 code routinely treats such an lParam as a near pointer,
   which only works when the struct really is in SS.

   `extra` is copied back out of the guest stack before returning, so whatever
   the callee wrote into the struct is visible to the caller.  It is the
   caller's own buffer on purpose: a window procedure re-enters constantly - a
   forward to DefWindowProc alone brings back WM_NCCALCSIZE and WM_WINDOWPOS* -
   and a single shared snapshot would be overwritten by the inner call before
   the outer one ever read it. */
uint32_t call16_wndproc(uint32_t proc, uint16_t ax,
                        const uint16_t *args, unsigned nbytes,
                        void *extra, unsigned extralen);

/* True while any call16 frame is active. */
int      call16_depth(void);

#endif
