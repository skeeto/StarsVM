/* resobj.c - Win32 objects built out of Win16 resources.
 *
 * Menus, icons and cursors cannot be handed to the guest as raw bytes the way
 * FindResource/LockResource hands over everything else: the guest expects a
 * handle, and only USER32 can make one.  So these are read out of the NE image
 * and rebuilt through the Win32 calls.
 *
 * The menu is rebuilt by walking the template and calling AppendMenu rather
 * than by converting the template byte for byte.  Converting is possible, but
 * the two formats differ in exactly the places that are easy to get wrong -
 * MF_END is 0x0080, which Win32 reads as MF_HILITE, so a template copied
 * verbatim comes up with every last item of every popup highlighted.
 */

#include "resobj.h"
#include "res.h"
#include "log.h"

#include <stdio.h>
#include <string.h>
#include <windows.h>

/* ---- menus ---------------------------------------------------------------- */

#define MF16_GRAYED     0x0001
#define MF16_DISABLED   0x0002
#define MF16_CHECKED    0x0008
#define MF16_POPUP      0x0010
#define MF16_END        0x0080

static const uint8_t *menu_text(char *out, size_t n,
                                const uint8_t *p, const uint8_t *end)
{
    size_t i = 0;
    while (p < end && *p && i + 1 < n) out[i++] = (char)*p++;
    while (p < end && *p) p++;               /* anything that did not fit */
    out[i] = 0;
    if (p < end) p++;                        /* the terminator */
    return p;
}

/* Fill `h` from the item list at `p`, stopping after the item flagged MF_END.
   Returns where the next sibling list continues. */
static const uint8_t *menu_walk(HMENU h, const uint8_t *p, const uint8_t *end,
                                int depth)
{
    if (depth > 8) return end;               /* a malformed template */

    while (p + 2 <= end) {
        uint16_t flags = (uint16_t)(p[0] | (p[1] << 8));
        char text[128];
        p += 2;

        if (flags & MF16_POPUP) {
            HMENU sub = CreatePopupMenu();
            p = menu_text(text, sizeof text, p, end);
            p = menu_walk(sub, p, end, depth + 1);
            AppendMenuA(h, (UINT)((flags & ~MF16_END) | MF_POPUP | MF_STRING),
                        (UINT_PTR)sub, text);
        } else {
            uint16_t id;
            if (p + 2 > end) break;
            id = (uint16_t)(p[0] | (p[1] << 8));
            p += 2;
            p = menu_text(text, sizeof text, p, end);
            /* An item with no id and no text is how the template spells a
               separator; Win16 did not have a flag for it. */
            if (!id && !text[0])
                AppendMenuA(h, MF_SEPARATOR, 0, NULL);
            else
                AppendMenuA(h, (UINT)((flags & ~MF16_END) | MF_STRING), id, text);
        }

        if (flags & MF16_END) break;
    }
    return p;
}

HMENU menu_load16(const char *name)
{
    const uint8_t *res;
    uint32_t len = 0;
    unsigned hdrsize;
    HMENU h;

    if (!name || !*name) return NULL;
    res = res_locate_name(RT16_MENU, name, &len);
    if (!res) {
        log_msg("*** no RT_MENU resource named %s\n", name);
        return NULL;
    }
    if (len < 4) return NULL;

    /* WORD version (0), WORD cbHeaderSize, then the items. */
    hdrsize = (unsigned)(res[2] | (res[3] << 8));
    if (4 + hdrsize > len) return NULL;

    h = CreateMenu();
    if (!h) return NULL;
    menu_walk(h, res + 4 + hdrsize, res + len, 0);
    return h;
}

/* ---- icons and cursors ----------------------------------------------------- */

/* The RT_GROUP_ICON / RT_GROUP_CURSOR directory has the same layout in an NE as
   in a PE, so USER32's own chooser can pick the best entry for the size we
   want, and CreateIconFromResourceEx can take the chosen image unchanged.  A
   cursor image carries its hotspot in the first two words, which is exactly
   where CreateIconFromResourceEx looks for it. */
static HICON from_group(const uint8_t *dir, int is_icon, int cx, int cy)
{
    const uint8_t *data;
    uint32_t len = 0;
    int id;

    id = LookupIconIdFromDirectoryEx((PBYTE)dir, is_icon, cx, cy, LR_DEFAULTCOLOR);
    if (!id) return NULL;

    data = res_locate_id(is_icon ? RT16_ICON : RT16_CURSOR, (uint16_t)id, &len);
    if (!data) return NULL;

    return CreateIconFromResourceEx((PBYTE)data, len, is_icon,
                                    0x00030000, cx, cy, LR_DEFAULTCOLOR);
}

HICON icon_load16(const char *name, int is_icon)
{
    const uint8_t *dir;
    uint32_t len = 0;
    int cx = GetSystemMetrics(is_icon ? SM_CXICON : SM_CXCURSOR);
    int cy = GetSystemMetrics(is_icon ? SM_CYICON : SM_CYCURSOR);

    dir = res_locate_name(is_icon ? RT16_GROUP_ICON : RT16_GROUP_CURSOR,
                          name, &len);
    if (!dir || len < 6) return NULL;
    return from_group(dir, is_icon, cx, cy);
}

HICON icon_load16_sized(const char *name, int cx, int cy)
{
    const uint8_t *dir;
    uint32_t len = 0;

    dir = res_locate_name(RT16_GROUP_ICON, name, &len);
    if (!dir || len < 6) return NULL;
    return from_group(dir, 1, cx, cy);
}

/* The application icon, for windows whose class does not name one.  Stars!
   never calls LoadIcon at all, so on Win16 its windows showed the module icon
   the shell had already associated with the executable; the equivalent here is
   to reach into the game's own resources for it.  "StarsIco" is the module's
   first RT_GROUP_ICON, and the one the shell picks. */
HICON icon_app16(int cx, int cy)
{
    static HICON big, small_;
    int want_big = (cx >= GetSystemMetrics(SM_CXICON));
    HICON *slot = want_big ? &big : &small_;

    if (!*slot) *slot = icon_load16_sized("StarsIco", cx, cy);
    return *slot;
}
