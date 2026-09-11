#include "handle.h"
#include "log.h"

#include <string.h>

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

static unsigned hash_of(void *host)
{
    uintptr_t v = (uintptr_t)host;
    v ^= v >> 16;
    return (unsigned)(v & (HASH_SIZE - 1));
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

    if (next_free >= MAX_HANDLES) {
        log_msg("handle: table full (%d entries)\n", MAX_HANDLES);
        return 0;
    }
    h = (uint16_t)next_free++;
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
}
