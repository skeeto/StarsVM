/* msg16.h - sending a message from guest code out to a real Win32 window.
 *
 * This is the mirror image of winproc.c, which carries messages the other way.
 * Keeping the two directions in separate files keeps each table readable, and
 * the pair has to stay consistent: a message the guest sends to its own window
 * makes the round trip 16 -> 32 -> 16, so anything this file rewrites, winproc.c
 * must rewrite back.
 */
#ifndef MSG16_H
#define MSG16_H

#include <stdint.h>
#include <windows.h>

/* Renumber a Win16 message for the window it is going to.  Win16 crammed every
   control message into the WM_USER range, so BM_GETCHECK and EM_GETSEL are both
   0x0400 and the class is the only thing that tells them apart. */
UINT msg16_to_32_for(HWND hwnd, uint16_t msg16);

/* Send/post a message the guest asked to send, marshaling whatever it carries
   and mapping the result back into 16-bit form. */
uint32_t msg16_send(HWND hwnd, uint16_t msg16, uint16_t wp, uint32_t lp);
uint32_t msg16_post(HWND hwnd, uint16_t msg16, uint16_t wp, uint32_t lp);

#endif
