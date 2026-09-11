/* handle.h - the 16 <-> 32 bit handle map.
 *
 * A Win32 handle does not fit in 16 bits, so every handle the guest sees is an
 * index we assign.  This is needed just as much in the 32-bit build as in a
 * 64-bit one, which is why an x64 build costs no extra work here.
 *
 * Handles are typed so that a mix-up (passing an HDC where an HBRUSH belongs)
 * is caught at the boundary rather than producing a confusing GDI failure.
 */
#ifndef HANDLE_H
#define HANDLE_H

#include <stdint.h>

enum {
    H_NONE = 0,
    H_WND, H_DC, H_BITMAP, H_BRUSH, H_PEN, H_FONT, H_RGN, H_PALETTE,
    H_MENU, H_CURSOR, H_ICON, H_ACCEL, H_FILE, H_GDIOBJ, H_MIXSESSION
};

/* Map a host handle to a 16-bit one, reusing the existing entry if the same
   handle has been seen before, so guest code comparing handles for equality
   behaves. */
uint16_t h16(int type, void *host);

/* Recover the host handle.  Returns NULL if the handle is unknown or of the
   wrong type; `type` may be H_NONE to accept any GDI object. */
void *h32(int type, uint16_t handle);

/* Handles below this are not handles at all: the Win16 calls that take a brush
   also take COLOR_x + 1, and tell the two apart by size. */
#define H_FIRST 0x40

/* The same lookup without the type-mismatch complaint, for a value whose type
   was guessed rather than declared - a window procedure's return, say. */
void *h32_quiet(int type, uint16_t handle);

/* Forget a mapping, for DestroyWindow, DeleteObject and friends. */
void h_release(uint16_t handle);

/* Convenience wrappers, purely for readability at the call sites. */
#define HWND_16(h)   h16(H_WND, (void *)(h))
#define HWND_32(h)   ((HWND)h32(H_WND, (h)))
#define HDC_16(h)    h16(H_DC, (void *)(h))
#define HDC_32(h)    ((HDC)h32(H_DC, (h)))
#define HMENU_16(h)  h16(H_MENU, (void *)(h))
#define HMENU_32(h)  ((HMENU)h32(H_MENU, (h)))

#endif
