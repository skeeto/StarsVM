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
 * tables, msg16_pack and msg16_unpack, so what holds for a send holds for a
 * post.
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
