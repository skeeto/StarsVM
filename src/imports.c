/* imports.c - the fixed import table for stars.exe and its resolver.
 *
 * Every imported (module, ordinal) owns one 8-byte slot in a reserved selector.
 * The NE loader points each import fixup at that slot, and the interpreter
 * recognises the selector rather than decoding anything there, so a call is
 * caught no matter how it arrives.
 */

#include "thunk.h"
#include "sel.h"
#include "log.h"

#include <string.h>

#define MODULE(m)
#define IMP(m, o, n, p, f)  { #m, o, n, p, f, 0, 0 },
#define EQU(m, o, n, v)     { #m, o, n, 0, IMP_EQUATE, v, 0 },

static ImpEntry imports[] = {
#include "imports.inc"
};

#undef MODULE
#undef IMP
#undef EQU

#define NIMPORTS ((int)(sizeof imports / sizeof imports[0]))

#define THUNK_STRIDE 8

static uint16_t thunk_sel;

uint16_t thunk_selector(void) { return thunk_sel; }

int thunk_init(void)
{
    uint32_t need = (uint32_t)NIMPORTS * THUNK_STRIDE;

    if (need > 0x10000u) {
        log_msg("thunk: %d imports do not fit in one selector\n", NIMPORTS);
        return 0;
    }
    thunk_sel = sel_alloc(0x10000u, SK_CODE);
    if (!thunk_sel) {
        log_msg("thunk: cannot allocate the thunk selector\n");
        return 0;
    }
    /* The slots are never executed as instructions, but fill them with a byte
       that would fault loudly if the dispatch check were ever bypassed. */
    memset(sel_ptr(thunk_sel, 0), 0xF1, 0x10000u);
    return 1;
}

static ImpEntry *lookup(const char *module, uint16_t ordinal)
{
    int i;
    for (i = 0; i < NIMPORTS; i++)
        if (imports[i].ordinal == ordinal &&
            _stricmp(imports[i].module, module) == 0)
            return &imports[i];
    return NULL;
}

int api_bind(const char *module, uint16_t ordinal, ApiFn fn)
{
    ImpEntry *e = lookup(module, ordinal);
    if (!e) {
        log_msg("api_bind: %s.%u is not imported by this binary\n",
                module, ordinal);
        return 0;
    }
    e->fn = fn;
    return 1;
}

ImpEntry *imp_slot(unsigned index)
{
    return (index < (unsigned)NIMPORTS) ? &imports[index] : NULL;
}

unsigned imp_index_for_offset(uint16_t off)
{
    return (unsigned)(off / THUNK_STRIDE);
}

uint32_t thunk_resolve(const char *module, uint16_t ordinal, void *user)
{
    ImpEntry *e = lookup(module, ordinal);
    (void)user;

    if (!e) {
        /* An import we did not know about.  Rather than guessing a stack
           adjustment later, say so now: the binary is not what we think. */
        log_msg("*** unknown import %s.%u - the import table needs regenerating\n",
                module, ordinal);
        return 0;
    }
    if (e->flags & IMP_EQUATE) {
        /* Equates resolve to a value, not an address.  Handing back
           0xFFFF:value is how a constant reaches an OFFSET-type fixup. */
        return SEGPTR(0xFFFF, (uint16_t)e->value);
    }
    return SEGPTR(thunk_sel, (uint16_t)((e - imports) * THUNK_STRIDE));
}

void thunk_report_unbound(void)
{
    int i, bound = 0, equ = 0, missing = 0;

    for (i = 0; i < NIMPORTS; i++) {
        if (imports[i].flags & IMP_EQUATE) equ++;
        else if (imports[i].fn)            bound++;
        else                               missing++;
    }
    log_msg("imports: %d implemented, %d equates, %d still stubbed (%d total)\n",
            bound, equ, missing, NIMPORTS);
}

void imp_dump_table(void)
{
    int i;
    log_msg("Import table (%d entries, thunk selector %04X)\n",
            NIMPORTS, thunk_sel);
    for (i = 0; i < NIMPORTS; i++) {
        ImpEntry *e = &imports[i];
        if (e->flags & IMP_EQUATE)
            log_msg("  %04X  %-9s %-4u %-26s = %08X\n",
                    (unsigned)(i * THUNK_STRIDE), e->module, e->ordinal,
                    e->name, e->value);
        else
            log_msg("  %04X  %-9s %-4u %-26s pop %-2u %s%s%s\n",
                    (unsigned)(i * THUNK_STRIDE), e->module, e->ordinal,
                    e->name, e->pop,
                    (e->flags & IMP_RET32)    ? "dx:ax " : "ax ",
                    (e->flags & IMP_CDECL)    ? "cdecl " : "",
                    (e->flags & IMP_REGISTER) ? "regs "  : "");
    }
}
