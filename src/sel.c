#include "sel.h"
#include "log.h"

#include <windows.h>
#include <string.h>

uint8_t *sel_arena;
SelDesc  sel_tab[SEL_SLOTS];
uint8_t  sel_live[SEL_SLOTS];   /* index 0 stays zero, which is the null check */
int      sel_fault;

static unsigned sel_next = 1;   /* index 0 stays permanently invalid */

int sel_init(void)
{
    sel_arena = VirtualAlloc(NULL, SEL_ARENA, MEM_RESERVE, PAGE_NOACCESS);
    if (!sel_arena) {
        log_msg("sel: cannot reserve %u MB arena (error %lu)\n",
                (unsigned)(SEL_ARENA >> 20), GetLastError());
        return 0;
    }
    /* Commit slot 0 read-only-ish so a stray translation lands somewhere real
       instead of crashing the host; it reads as zero and writes are discarded
       into a scratch page. */
    if (!VirtualAlloc(sel_arena, SEL_SLOT, MEM_COMMIT, PAGE_READWRITE)) {
        log_msg("sel: cannot commit guard slot\n");
        return 0;
    }
    memset(sel_tab, 0, sizeof sel_tab);
    sel_next = 1;
    return 1;
}

void sel_shutdown(void)
{
    if (sel_arena) {
        VirtualFree(sel_arena, 0, MEM_RELEASE);
        sel_arena = NULL;
    }
}

uint16_t sel_alloc(uint32_t size, int kind)
{
    unsigned count, i, start;

    if (size == 0) size = 1;
    count = (unsigned)((size + 0xFFFFu) >> 16);
    if (count == 0) count = 1;

    /* First fit over the index space. */
    for (start = sel_next; start + count <= SEL_SLOTS; start++) {
        for (i = 0; i < count; i++)
            if (sel_tab[start + i].kind != SK_FREE) break;
        if (i == count) break;
    }
    if (start + count > SEL_SLOTS) {
        log_msg("sel: out of selectors (wanted %u for %u bytes)\n", count, size);
        return 0;
    }

    if (!VirtualAlloc(sel_arena + ((size_t)start << 16),
                      (size_t)count * SEL_SLOT, MEM_COMMIT, PAGE_READWRITE)) {
        log_msg("sel: commit failed for %u slots at %u (error %lu)\n",
                count, start, GetLastError());
        return 0;
    }
    memset(sel_arena + ((size_t)start << 16), 0, (size_t)count * SEL_SLOT);

    /* Each selector in a huge block carries the limit of the remainder, which is
       what real Windows does and what huge-pointer code expects. */
    for (i = 0; i < count; i++) {
        SelDesc *d = &sel_tab[start + i];
        uint32_t remain = size - (uint32_t)i * SEL_SLOT;
        d->limit = remain - 1;
        d->kind  = (uint8_t)kind;
        d->count = (uint8_t)count;
        d->head  = (uint8_t)(i == 0);
        sel_live[start + i] = 1;
    }
    if (start + count > sel_next) sel_next = start + count;
    return SEL_MAKE(start);
}

void sel_free(uint16_t sel)
{
    unsigned i = SEL_INDEX(sel), n, k;
    if (i == 0 || i >= SEL_SLOTS || sel_tab[i].kind == SK_FREE) return;
    n = sel_tab[i].count ? sel_tab[i].count : 1;
    VirtualFree(sel_arena + ((size_t)i << 16), (size_t)n * SEL_SLOT, MEM_DECOMMIT);
    for (k = 0; k < n && i + k < SEL_SLOTS; k++) {
        memset(&sel_tab[i + k], 0, sizeof sel_tab[0]);
        sel_live[i + k] = 0;
    }
    if (i < sel_next) sel_next = i;
}

uint8_t *sel_bad(uint16_t sel, uint16_t off)
{
    sel_report_fault(sel, off, "translate");
    return sel_arena;
}

void sel_report_fault(uint16_t sel, uint16_t off, const char *what)
{
    static int shown;
    sel_fault = 1;
    if (shown < 32) {
        shown++;
        log_msg("*** bad selector %04X:%04X (%s)\n", sel, off, what);
    }
}
