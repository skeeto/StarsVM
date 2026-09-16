/* harness.h - the LLM harness's hooks into the rest of the shim.
 *
 * The harness is a dev-only control channel: a named pipe the emulator serves
 * so an agent can read the game's windows, controls and menus and drive them by
 * id.  None of it belongs in a release build, so this header follows the same
 * shape as prof.h: the hooks are empty inline functions unless STARSVM_HARNESS
 * is defined, and src/unity_harness.c is what defines it and includes unity.c.
 * The call sites in api_user.c, winproc.c and dlg.c therefore carry no #ifdef
 * of their own, and StarsVM.exe is byte-for-byte what it was.
 */
#ifndef HARNESS_H
#define HARNESS_H

#include <stdint.h>

#ifdef STARSVM_HARNESS

/* Begin serving the pipe at `name` (NULL picks the default).  Called once from
   main before the interpreter starts. */
void harness_start(const char *name);

/* Execute at most one pending request.  Called from the message-loop safe
   points, where the host is about to run guest code, so every command runs on
   the game's own thread with no locks.  Cheap when nothing is pending. */
void harness_pump(void);

/* Whether a client is connected, so a caller can report it. */
int harness_active(void);

/* Record one high-level UI event for the journal: a command was sent, a dialog
   opened or closed.  Called from the two window-procedure bridges, so it is on
   the game's thread and needs no locking. */
void harness_event(const char *kind, uintptr_t hwnd, long a, long b);

/* Text drawn to a window, so the game's own-drawn panes can be read as text
   instead of by eye.  harness_text_begin resets a window's text when its paint
   starts; harness_text appends one drawn string, attributed to the window the
   DC belongs to.  The DC and window are void * so the header stays free of
   windows.h. */
void harness_text_begin(unsigned hwnd16);
void harness_text(void *hdc, int x, int y, const char *s, int len);
/* The same, for a call site that has only the guest's own DC handle.  Taking
   the 16-bit handle rather than HDC_32 of it keeps the release build exactly
   as it was: see the note at u_DrawText. */
void harness_text16(unsigned hdc16, int x, int y, const char *s, int len);
void harness_draw(void *hdc, const char *kind, int x, int y, int w, int h);

/* Emulated input state.  The harness drives the game by injecting messages into
   its queue, but a game that asks GetCursorPos or GetKeyState would otherwise
   read the host's real mouse and keyboard and ignore the injected position and
   modifiers.  So once the harness has injected input it owns both: the cursor
   in screen coordinates, and the down/up state of each virtual key.  The
   getters return 0 / 0 when the harness has not taken over, leaving the host's
   own values in force for a normal interactive session. */
int harness_input_active(void);
int harness_input_cursor(int *x, int *y);
int harness_input_key(int vk);

/* Popup menus.  The game shows them with the host's TrackPopupMenu, whose own
   modal loop reads the host mouse and keyboard and so never sees the messages
   the harness injects.  So while the harness is driving, TrackPopupMenu does
   not show a menu: it records the menu's items for the agent to read and waits
   for the agent's pick, then returns that item's id (or 0 to cancel). */
void harness_menu_record(void *menu);
int  harness_menu_wait(void);

/* Answer a MessageBox without blocking.  A host MessageBox runs its own modal
   loop that never reaches our bridges, so showing one would wedge the harness
   for as long as it is up; this records the text and returns the default
   result instead.  Only called when the harness is active. */
int harness_msgbox(const char *caption, const char *text, unsigned type);

#else

static inline void harness_start(const char *name) { (void)name; }
static inline void harness_pump(void) {}
static inline int  harness_active(void) { return 0; }
static inline void harness_event(const char *kind, uintptr_t hwnd, long a, long b)
{ (void)kind; (void)hwnd; (void)a; (void)b; }
static inline void harness_text_begin(unsigned hwnd16) { (void)hwnd16; }
static inline void harness_text(void *hdc, int x, int y, const char *s, int len)
{ (void)hdc; (void)x; (void)y; (void)s; (void)len; }
static inline void harness_text16(unsigned hdc16, int x, int y, const char *s, int len)
{ (void)hdc16; (void)x; (void)y; (void)s; (void)len; }
static inline void harness_draw(void *hdc, const char *kind, int x, int y, int w, int h)
{ (void)hdc; (void)kind; (void)x; (void)y; (void)w; (void)h; }
static inline int harness_input_active(void) { return 0; }
static inline void harness_menu_record(void *menu) { (void)menu; }
static inline int harness_menu_wait(void) { return 0; }
static inline int harness_input_cursor(int *x, int *y)
{ (void)x; (void)y; return 0; }
static inline int harness_input_key(int vk) { (void)vk; return 0; }
static inline int harness_msgbox(const char *caption, const char *text, unsigned type)
{ (void)caption; (void)text; (void)type; return 0; }

#endif
#endif