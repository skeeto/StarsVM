#include "handle.h"
#include "log.h"

#include <string.h>
#include <windows.h>

/* A flat table plus a small hash for the reverse direction.  Handle 0 is
   reserved so that a null guest handle stays null. */

#define MAX_HANDLES 8192
#define HASH_SIZE   16384

static struct {
    void    *host;
    uint8_t  type;
    uint16_t hnext;         /* hash chain */
} tab[MAX_HANDLES];

static uint16_t hash_head[HASH_SIZE];

/* Handles start well above the system colour indices.  Several Win16 calls -
   FillRect, FrameRect, WNDCLASS.hbrBackground - accept either a real brush or
   the small integer COLOR_x + 1, and tell them apart by magnitude, exactly as
   Win32 still does.  Real Win16 GDI handles were never single digits, so that
   worked; handing out 1, 2, 3 would make a black brush indistinguishable from
   COLOR_ACTIVECAPTION + 1, and the star map comes up pale blue. */
#define FIRST_HANDLE 0x40
static unsigned next_free = FIRST_HANDLE;

/* Released entries, chained through hnext and handed out again before the
   table is grown.  Without this it was a bump allocator: h_release blanked an
   entry and nothing ever gave it out again, so every handle the guest was
   given and gave back cost one of the 8192 for the life of the process. */
static uint16_t free_head;
static unsigned live;

/* Allocations since the table was last swept, and how full it has to be before
   sweeping is worth the scan.  The interval matters: a sweep that reclaims
   nothing, because every window really is alive, must not then run again on
   the very next allocation. */
#define SWEEP_FIRST (MAX_HANDLES / 8)
#define SWEEP_EVERY 512
static unsigned since_sweep;

static unsigned hash_of(void *host)
{
    uintptr_t v = (uintptr_t)host;
    v ^= v >> 16;
    return (unsigned)(v & (HASH_SIZE - 1));
}

/* Reclaim the entries whose window is gone.
   A window reaches this table from a dozen places - GetDlgItem, GetFocus,
   GetParent, and every message that carries an HWND - and most of what those
   name are host controls, running the host's own class procedure, so nothing
   tells us when they die.  DestroyWindow releases the one window it is handed
   and not the children destroyed along with it; a dialog that ends through
   EndDialog releases none of them at all.  Measured on the New Game wizard:
   thirteen open-and-close cycles took 260 entries and gave back six.
   IsWindow settles it with no bookkeeping to keep anywhere else.  Only windows
   are swept - a stale HDC or HBRUSH cannot be asked whether it is still alive,
   and neither piles up this way, because the host reuses those values and the
   entry already in the table is found again instead of another being made. */
static unsigned sweep_dead_windows(void)
{
    unsigned h, n = 0;

    for (h = FIRST_HANDLE; h < next_free; h++)
        if (tab[h].type == H_WND && tab[h].host &&
            !IsWindow((HWND)tab[h].host)) {
            h_release((uint16_t)h);
            n++;
        }
    return n;
}

uint16_t h16(int type, void *host)
{
    unsigned b;
    uint16_t h;

    if (!host) return 0;

    b = hash_of(host);
    for (h = hash_head[b]; h; h = tab[h].hnext)
        if (tab[h].host == host && tab[h].type == type)
            return h;

    /* Nothing to hand back and the table is filling: see who has died. */
    since_sweep++;
    if (!free_head && next_free >= SWEEP_FIRST && since_sweep >= SWEEP_EVERY) {
        unsigned dead = sweep_dead_windows();
        since_sweep = 0;
        if (dead)
            log_msg("handle: swept %u dead windows, %u live\n", dead, live);
    }

    if (free_head) {
        h = free_head;
        free_head = tab[h].hnext;
    } else if (next_free < MAX_HANDLES) {
        h = (uint16_t)next_free++;
    } else {
        log_msg("handle: table full (%d entries, %u live)\n",
                MAX_HANDLES, live);
        return 0;
    }
    live++;
    tab[h].host = host;
    tab[h].type = (uint8_t)type;
    tab[h].hnext = hash_head[b];
    hash_head[b] = h;
    return h;
}

/* The logging version below is right for an argument, where a type mismatch is
   a real bug.  A procedure's RETURN value is different: the type is inferred
   from the message, the guest may be returning something else entirely, and a
   wrong guess is not worth a line per repaint. */
void *h32_quiet(int type, uint16_t handle)
{
    if (handle < FIRST_HANDLE || handle >= MAX_HANDLES) return NULL;
    if (tab[handle].type != (uint8_t)type) return NULL;
    return tab[handle].host;
}

void *h32(int type, uint16_t handle)
{
    if (handle < FIRST_HANDLE || handle >= MAX_HANDLES || !tab[handle].host)
        return NULL;
    if (type != H_NONE && tab[handle].type != (uint8_t)type) {
        /* A GDI object handle may be used wherever a generic one is expected. */
        if (!(type == H_GDIOBJ &&
              (tab[handle].type == H_BITMAP || tab[handle].type == H_BRUSH ||
               tab[handle].type == H_PEN    || tab[handle].type == H_FONT  ||
               tab[handle].type == H_RGN    || tab[handle].type == H_PALETTE))) {
            log_msg("handle: %04X is type %u, not %u\n",
                    handle, tab[handle].type, type);
            return NULL;
        }
    }
    return tab[handle].host;
}

void h_release(uint16_t handle)
{
    unsigned b;
    uint16_t *link;

    if (handle < FIRST_HANDLE || handle >= MAX_HANDLES || !tab[handle].host)
        return;
    b = hash_of(tab[handle].host);
    for (link = &hash_head[b]; *link; link = &tab[*link].hnext) {
        if (*link == handle) { *link = tab[handle].hnext; break; }
    }
    memset(&tab[handle], 0, sizeof tab[handle]);
    tab[handle].hnext = free_head;
    free_head = handle;
    live--;
}
