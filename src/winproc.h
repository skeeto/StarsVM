/* winproc.h - the window-procedure bridge between Win32 and guest code. */
#ifndef WINPROC_H
#define WINPROC_H

#include <stdint.h>
#include <windows.h>

LRESULT CALLBACK winproc_bridge(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
LRESULT winproc_default(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

/* The wheel messages and metrics postdate the Windows version these headers
   are written against, and each is nothing but a number. */
#ifndef WM_MOUSEHWHEEL
#define WM_MOUSEHWHEEL 0x020E
#endif
#ifndef SPI_GETWHEELSCROLLCHARS
#define SPI_GETWHEELSCROLLCHARS 0x006C
#endif

/* Answer a wheel turn the only ways a Win16 window can be scrolled: through a
   stock control's own class procedure, or as WM_VSCROLL or WM_HSCROLL. */
LRESULT winproc_wheel(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

/* Around a call that passes on a message whose lParam is a struct the guest
   holds a 16-bit copy of, at guest_lp: widen what the guest wrote into its
   copy into the 32-bit struct before the call, and rebuild the copy from the
   struct after it. */
void    winproc_widen_struct(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                             uint32_t guest_lp);
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

/* A message being dispatched to a guest procedure: what USER32 sent, and what
   the guest was handed for it, so that a forward of it (msg16_call,
   msg16_default) can tell whether the guest changed it. */
struct inflight {
    HWND     hwnd;
    UINT     msg16;
    uint16_t wp16;          /* what the guest procedure was handed      */
    uint32_t lp16;
    int      onstack;       /* lParam, a struct copied onto its stack   */
    UINT     msg32;         /* what USER32 sent                         */
    WPARAM   wp32;
    LPARAM   lp32;
};

/* The innermost message in flight to this window with this Win16 number, or
   NULL. */
const struct inflight *winproc_inflight(HWND hwnd, UINT msg16);

/* The scroll flag of an EM_SETSEL the guest is sending or dispatching to an
   edit it has subclassed, for its procedure to be handed; hwnd NULL when the
   send or the dispatch is over. */
void     winproc_sending_setsel(HWND hwnd, WPARAM start, LPARAM end,
                                uint16_t flag);

/* Renumber a message between Win32 and Win16.  Only a stock control's own
   messages differ.  Going to Win32 only the class of the window it is going
   to can say which a number is; coming back the number says it alone. */
uint32_t msg32_to_16(uint32_t msg);
uint32_t msg16_to_32(HWND hwnd, uint32_t msg);

/* A message in Win16's form as far as its parameters are values, which is
   as far as a posted one needs; the guest's message loop fills in its MSG
   this way.  msg16_unpack (msg16.h) is the other way. */
void     msg16_pack(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                    uint16_t *msg16, uint16_t *wp16, uint32_t *lp16);

/* Whether a Win32 message is one Win16 had, so that the guest can know what
   it means.  Those that are not are answered on this side and never reach
   it. */
int      msg_win16(UINT msg);

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
