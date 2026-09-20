/* winproc.h - the window-procedure bridge between Win32 and guest code. */
#ifndef WINPROC_H
#define WINPROC_H

#include <stdint.h>
#include <windows.h>

LRESULT CALLBACK winproc_bridge(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
LRESULT winproc_default(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

/* Answer a wheel turn the only ways a Win16 window can be scrolled: through a
   stock control's own class procedure, or as WM_VSCROLL. */
LRESULT winproc_wheel(HWND hwnd, WPARAM wp, LPARAM lp);

/* Rebuild the guest's 16-bit copy of a struct DefWindowProc just wrote to. */
void    winproc_refresh_struct(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                               uint32_t guest_lp);

void     winproc_set(HWND hwnd, uint32_t proc16, uint16_t hinst);
uint32_t winproc_get(HWND hwnd);
void     winproc_forget(HWND hwnd);
void     winproc_set_pending(uint32_t proc16, uint16_t hinst);

/* Wrapping a real Win32 procedure so the guest can hold and call it, which is
   what subclassing a standard control requires. */
uint32_t winproc_from_host(WNDPROC p);
WNDPROC  winproc_to_host(uint32_t proc16);

void        class_add(const char *name, uint32_t proc16, uint16_t hinst,
                      const char *menu);
uint32_t    class_proc(const char *name);
/* The class's lpszMenuName, as a name in the guest's resources. */
const char *class_menu(const char *name);

/* Recover the original 32-bit parameters of the message currently being
   dispatched to guest code, so a forward to DefWindowProc can use them. */
int      winproc_original(HWND hwnd, UINT msg16, UINT *msg32,
                          WPARAM *wp, LPARAM *lp);

uint32_t msg32_to_16(uint32_t msg);
uint32_t msg16_to_32(uint32_t msg);

/* Translate a message into Win16 form and run `proc16` with it.  Shared by the
   window-procedure bridge and the dialog-procedure bridge; *ret_handle comes
   back as the H_* type when the guest's result is a handle. */
uint32_t winproc_call16(HWND hwnd, uint32_t proc16, uint16_t hinst,
                        UINT msg, WPARAM wp, LPARAM lp, int *ret_handle);

/* Map a guest procedure result that is meant to be a handle - but only when
   the value is large enough to be one. */
LRESULT winproc_ret_handle(int type, uint32_t r);

/* The H_* type of a message's result, or H_NONE. */
int     winproc_ret_handle_type(UINT msg);

#endif
