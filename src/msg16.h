/* msg16.h - sending a message from guest code out to a real Win32 window.
 *
 * This is the mirror image of winproc.c, which carries messages the other way.
 * Keeping the two directions in separate files keeps each table readable, and
 * the pair has to stay consistent: a message the guest sends to its own window
 * makes the round trip 16 -> 32 -> 16, so anything this file rewrites, winproc.c
 * must rewrite back.  The renumbering is winproc.c's, both ways, from one table
 * (msg16_to_32), so that much cannot come apart.
 *
 * A posted message makes the same round trip through the guest's own message
 * loop: GetMessage hands the guest a MSG in Win16's form, and the guest hands
 * it back to DispatchMessage.  The loop uses the value-only halves of the two
 * tables, msg16_pack and msg16_unpack, so a post is translated as a send is.
 * What the queue does to a message on the way through is another matter:
 *
 *  - It cannot carry EM_SETSEL's Win16 scroll flag, which Win32's EM_SETSEL
 *    has no room for.  A send, and a DispatchMessage of the guest's own MSG,
 *    take it through to a guest subclass of the edit (winproc_sending_setsel);
 *    a posted one is handed the flag Win32's EM_SETSEL amounts to, and its
 *    scroll is a second message posted after it.
 *  - PostMessageA converts a character message's wParam from the code page
 *    and keeps only the character: the high word WM_CHARTOITEM and
 *    WM_MENUCHAR carry, the caret's index and the menu's flags, is lost, and
 *    on a 32-bit build WM_CHAR and its kin keep only their low byte.
 *    SendMessageA does the same to a Unicode window, and the guest's
 *    dialogs are Unicode windows (dlg.c).
 */
#ifndef MSG16_H
#define MSG16_H

#include <stdint.h>
#include <windows.h>

/* Send/post a message the guest asked to send, marshaling whatever it carries
   and mapping the result back into 16-bit form. */
uint32_t msg16_send(HWND hwnd, uint16_t msg16, uint16_t wp, uint32_t lp);
uint32_t msg16_post(HWND hwnd, uint16_t msg16, uint16_t wp, uint32_t lp);

/* The same for a message the guest hands a real window procedure with
   CallWindowProc, calling `proc` rather than sending, or hands DefWindowProc.
   The message being dispatched to the guest, passed on as it was handed over,
   gets USER32's own parameters back instead. */
uint32_t msg16_call(WNDPROC proc, HWND hwnd, uint16_t msg16, uint16_t wp,
                    uint32_t lp);
uint32_t msg16_default(HWND hwnd, uint16_t msg16, uint16_t wp, uint32_t lp);

/* A message in Win32's form as far as its parameters are values, which is as
   far as a posted one needs: nothing that carries a pointer is posted.  The
   guest's message loop takes back its MSG this way.  msg16_pack (winproc.h)
   is the other way. */
struct msg32 {
    UINT   msg;
    WPARAM wp;
    LPARAM lp;
    UINT   after;           /* a message without parameters to follow, or 0 */
};
void msg16_unpack(HWND hwnd, uint16_t msg16, uint16_t wp, uint32_t lp,
                  struct msg32 *m);

#endif
