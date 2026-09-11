/* api_user.c - the USER entry points Stars! imports.
 *
 * The leverage here is that DefWindowProc, the message loop and all non-client
 * behaviour come from real USER32.  What this file provides is the translation:
 * handle mapping in both directions, 16-bit struct layouts, and the bridge that
 * lets a real Win32 window procedure end up running guest code.
 */

#include "thunk.h"
#include "task.h"
#include "sel.h"
#include "log.h"
#include "handle.h"
#include "winproc.h"
#include "msg16.h"
#include "dlg.h"
#include "resobj.h"

#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <limits.h>

extern int  trace_paint;              /* --trace-paint */
static HWND painting;                 /* window with an open BeginPaint */
static void updc(const char *what);

/* USER.5 InitApp - in real Windows this creates the task's message queue.
   Returning success is all a single-task emulator needs. */
static uint32_t u_InitApp(Cpu *c, Args *a)
{
    (void)c;
    arg_word(a);                       /* hInstance */
    return 1;
}

static uint32_t u_GetTickCount(Cpu *c, Args *a)
{
    (void)c; (void)a;
    return GetTickCount();
}

static uint32_t u_MessageBeep(Cpu *c, Args *a)
{
    (void)c;
    MessageBeep(arg_word(a));
    return 1;
}

static uint32_t u_GetSystemMetrics(Cpu *c, Args *a)
{
    (void)c;
    return (uint32_t)GetSystemMetrics(arg_sword(a));
}

static uint32_t u_GetSysColor(Cpu *c, Args *a)
{
    (void)c;
    return GetSysColor(arg_word(a));
}

static uint32_t u_PostQuitMessage(Cpu *c, Args *a)
{
    (void)c;
    PostQuitMessage(arg_sword(a));
    return 0;
}

/* ---- strings and rects ---------------------------------------------------- */

static char *gstr(uint32_t segptr, char *buf, size_t n)
{
    uint16_t sel = SEGPTR_SEL(segptr), off = SEGPTR_OFF(segptr);
    size_t i = 0;
    if (!segptr) { buf[0] = 0; return buf; }
    while (i + 1 < n) {
        uint8_t ch = sel_rd8(sel, (uint16_t)(off + i));
        if (!ch) break;
        buf[i++] = (char)ch;
    }
    buf[i] = 0;
    return buf;
}

/* A resource name is either a string or MAKEINTRESOURCE, which in Win16 is a
   far pointer whose selector is zero. */
static const char *gres(uint32_t segptr, char *buf, size_t n)
{
    if (!segptr) return NULL;
    if (SEGPTR_SEL(segptr) == 0) return MAKEINTRESOURCEA(SEGPTR_OFF(segptr));
    return gstr(segptr, buf, n);
}

static void put_rect16(uint32_t p, const RECT *r)
{
    uint16_t sel = SEGPTR_SEL(p), off = SEGPTR_OFF(p);
    if (!p) return;
    sel_wr16(sel, off,              (uint16_t)(int16_t)r->left);
    sel_wr16(sel, (uint16_t)(off+2),(uint16_t)(int16_t)r->top);
    sel_wr16(sel, (uint16_t)(off+4),(uint16_t)(int16_t)r->right);
    sel_wr16(sel, (uint16_t)(off+6),(uint16_t)(int16_t)r->bottom);
}



static void get_rect16(uint32_t p, RECT *r)
{
    uint16_t sel = SEGPTR_SEL(p), off = SEGPTR_OFF(p);
    if (!p) { memset(r, 0, sizeof *r); return; }
    r->left   = (int16_t)sel_rd16(sel, off);
    r->top    = (int16_t)sel_rd16(sel, (uint16_t)(off + 2));
    r->right  = (int16_t)sel_rd16(sel, (uint16_t)(off + 4));
    r->bottom = (int16_t)sel_rd16(sel, (uint16_t)(off + 6));
}

/* gres hands back either a pointer into `buf` or a MAKEINTRESOURCE ordinal,
   which is not a string at all.  The resource table wants a name either way, and
   spells an ordinal "#123". */
static const char *res_name(uint32_t segptr, char *buf, size_t n)
{
    const char *r = gres(segptr, buf, n);
    if (!r) return NULL;
    if (IS_INTRESOURCE(r)) {
        snprintf(buf, n, "#%u", (unsigned)(uintptr_t)r);
        return buf;
    }
    return r;
}

/* ---- classes and windows -------------------------------------------------- */

/* WNDCLASS16 is 26 bytes:
     0 style(W) 2 lpfnWndProc(D) 6 cbClsExtra(W) 8 cbWndExtra(W)
     10 hInstance(W) 12 hIcon(W) 14 hCursor(W) 16 hbrBackground(W)
     18 lpszMenuName(D) 22 lpszClassName(D) */
static uint32_t u_RegisterClass(Cpu *c, Args *a)
{
    uint32_t p = arg_long(a);
    uint16_t sel = SEGPTR_SEL(p), off = SEGPTR_OFF(p);
    WNDCLASSEXA wc;
    char cls[128], menu[128];
    ATOM at;

    (void)c;
    memset(&wc, 0, sizeof wc);
    wc.cbSize       = sizeof wc;
    wc.style        = sel_rd16(sel, off);
    wc.cbClsExtra   = (int16_t)sel_rd16(sel, (uint16_t)(off + 6));
    wc.cbWndExtra   = (int16_t)sel_rd16(sel, (uint16_t)(off + 8));
    wc.hInstance    = GetModuleHandleA(NULL);
    wc.hIcon        = (HICON)h32(H_ICON,   sel_rd16(sel, (uint16_t)(off + 12)));
    /* Stars! registers every class with hIcon == 0 and never calls LoadIcon, so
       left alone its windows carry the generic Windows icon in their caption,
       task bar button and Alt-Tab entry.  Fall back to the module's own icon -
       which is what the shell would have supplied on Win16 - and give Win32 a
       properly rendered small icon too rather than letting it squash the 32x32
       one.  WNDCLASSEX exists precisely for that second field. */
    if (!wc.hIcon) {
        wc.hIcon   = icon_app16(GetSystemMetrics(SM_CXICON),
                                GetSystemMetrics(SM_CYICON));
        wc.hIconSm = icon_app16(GetSystemMetrics(SM_CXSMICON),
                                GetSystemMetrics(SM_CYSMICON));
    }
    wc.hCursor      = (HCURSOR)h32(H_CURSOR, sel_rd16(sel, (uint16_t)(off + 14)));
    {
        /* Either a brush handle or COLOR_x + 1, which Win32 understands here
           unchanged. */
        uint16_t bg = sel_rd16(sel, (uint16_t)(off + 16));
        wc.hbrBackground = (bg && bg < H_FIRST) ? (HBRUSH)(uintptr_t)bg
                                                : (HBRUSH)h32(H_BRUSH, bg);
    }
    wc.lpfnWndProc  = winproc_bridge;
    wc.lpszClassName = gstr(sel_rd32(sel, (uint16_t)(off + 22)), cls, sizeof cls);
    {
        /* lpszMenuName names a resource in the GUEST's module.  Handing it to
           RegisterClassA would have Win32 look for it in ours, where it does not
           exist, and the window would silently come up with no menu bar at all.
           Remember it and build the menu ourselves at CreateWindow time. */
        uint32_t mn = sel_rd32(sel, (uint16_t)(off + 18));
        menu[0] = 0;
        if (mn) {
            const char *r = res_name(mn, menu, sizeof menu);
            if (r && r != menu) snprintf(menu, sizeof menu, "%s", r);
        }
        wc.lpszMenuName = NULL;
    }

    /* Win16 classes do not have the CS_ bits Win32 added, and a background
       brush handle of 1..n means COLOR_x + 1 in both worlds. */
    if (log_verbose)
        log_msg("RegisterClass %s menu=%s proc=%04X:%04X\n", cls,
                menu[0] ? menu : "(none)",
                (unsigned)(sel_rd32(sel, (uint16_t)(off + 2)) >> 16),
                (unsigned)(sel_rd32(sel, (uint16_t)(off + 2)) & 0xFFFF));
    at = RegisterClassExA(&wc);
    if (!at) {
        log_msg("RegisterClass(%s) failed: %lu\n", cls, GetLastError());
        return 0;
    }
    class_add(cls, sel_rd32(sel, (uint16_t)(off + 2)),
              sel_rd16(sel, (uint16_t)(off + 10)), menu);
    return at;
}

static uint32_t u_CreateWindow(Cpu *c, Args *a)
{
    char cls[128], name[256];
    uint32_t clsp  = arg_long(a);
    uint32_t namep = arg_long(a);
    uint32_t style = arg_long(a);
    int16_t  x  = arg_sword(a), y  = arg_sword(a);
    int16_t  w  = arg_sword(a), h  = arg_sword(a);
    uint16_t parent = arg_word(a);
    uint16_t menu   = arg_word(a);
    uint16_t inst   = arg_word(a);
    uint32_t param  = arg_long(a);
    HWND hwnd;
    uint32_t proc16;

    HMENU hmenu;
    DWORD exstyle = 0;

    (void)c;
    gstr(clsp, cls, sizeof cls);
    gstr(namep, name, sizeof name);

    proc16 = class_proc(cls);
    winproc_set_pending(proc16, inst ? inst : task.hinstance);

    /* A listbox without LBS_NOINTEGRALHEIGHT shrinks itself to a whole number
       of items.  Win32 measures that against the CLIENT height, so a bordered
       listbox asked for five 16-pixel rows (80) comes back as four (66) - and
       this game recomputes its layout on every paint, asks for 80 again, is cut
       to 66 again, and invalidates its parent each time round.  That is an
       infinite repaint: the process burns a core, and because a window that is
       always dirty always wins the paint queue, every other window starves and
       is never drawn at all - which is why the turn-summary panel was missing.
       Win16 evidently measured against the window height instead; Wine's own
       listbox carries a comment doubting which one native used.  Honouring the
       size the guest asked for is both the faithful reading and the one that
       terminates. */
    if (!_stricmp(cls, "listbox")) style |= 0x0100;   /* LBS_NOINTEGRALHEIGHT */

    /* Taskbar presence.  Windows gives a taskbar button to a top-level window
       that nothing owns; this game owns its splash screen off the main frame,
       and keeps the frame hidden until a game is loaded, so between those two
       facts the application has no taskbar button at all while the splash is up
       and it is easy to lose behind other windows.  WS_EX_APPWINDOW overrides
       that.  The game's transient windows - tooltips and the popup overlay -
       are all WS_CHILD, so "not a child" is most of the line to draw.  An
       unowned top-level window already gets a button from the default rule, so
       leave those exactly as they shipped and override only where it is needed.

       The remaining requirement is a caption or a visible-on-creation popup.
       Measured: starsframe 0x00CF0000 and starsreport 0x80CF0000 are captioned,
       the splash starstitle 0x90000000 has no caption but is created visible,
       and every panel is WS_CHILD.  A tooltip or transient overlay - created
       hidden, captionless, and positioned before it is shown - fails both
       halves, so it cannot acquire a taskbar button even if it turns out to be
       a top-level window. */
    if (!(style & WS_CHILD) && parent &&
        ((style & WS_CAPTION) == WS_CAPTION || (style & WS_VISIBLE)))
        exstyle |= WS_EX_APPWINDOW;

    /* The same argument is a control id on a child window and a real menu
       handle on a top-level one; and when a top-level window passes none, the
       menu comes from the class. */
    if (style & WS_CHILD)   hmenu = (HMENU)(uintptr_t)menu;
    else if (menu)          hmenu = HMENU_32(menu);
    else                    hmenu = menu_load16(class_menu(cls));

    if (log_verbose)
        log_msg("CreateWindow %-14s style=%08X ex=%08X parent=%04X %dx%d\n",
                cls, (unsigned)style, (unsigned)exstyle, parent, w, h);
    hwnd = CreateWindowExA(exstyle, cls, name, style,
                           x == (int16_t)0x8000 ? CW_USEDEFAULT : x,
                           y == (int16_t)0x8000 ? CW_USEDEFAULT : y,
                           w == (int16_t)0x8000 ? CW_USEDEFAULT : w,
                           h == (int16_t)0x8000 ? CW_USEDEFAULT : h,
                           HWND_32(parent), hmenu,
                           GetModuleHandleA(NULL),
                           (LPVOID)(uintptr_t)param);
    winproc_set_pending(0, 0);
    if (!hwnd) {
        log_msg("CreateWindow(%s,%s) failed: %lu\n", cls, name, GetLastError());
        return 0;
    }
    if (proc16) winproc_set(hwnd, proc16, inst ? inst : task.hinstance);
    return HWND_16(hwnd);
}

static uint32_t u_ShowWindow(Cpu *c, Args *a)
{
    uint16_t hwnd = arg_word(a);
    int16_t  cmd  = arg_sword(a);
    (void)c;
    { uint32_t r = (uint32_t)ShowWindow(HWND_32(hwnd), cmd); updc("ShowWindow"); return r; }
}

static uint32_t u_UpdateWindow(Cpu *c, Args *a)
{
    (void)c;
    return (uint32_t)UpdateWindow(HWND_32(arg_word(a)));
}

static uint32_t u_DestroyWindow(Cpu *c, Args *a)
{
    uint16_t hwnd = arg_word(a);
    HWND h = HWND_32(hwnd);
    BOOL r;
    (void)c;
    r = DestroyWindow(h);
    winproc_forget(h);
    h_release(hwnd);
    return (uint32_t)r;
}

static uint32_t u_DefWindowProc(Cpu *c, Args *a)
{
    uint16_t hwnd16 = arg_word(a);
    uint16_t msg    = arg_word(a);
    uint16_t wp     = arg_word(a);
    uint32_t lp     = arg_long(a);
    HWND hwnd = HWND_32(hwnd16);
    UINT msg32;
    WPARAM wp32;
    LPARAM lp32;

    (void)c;
    /* If this is the message we are currently dispatching, hand USER32 back the
       parameters it gave us.  Anything carrying a pointer - WM_NCCREATE and
       WM_CREATE especially - would otherwise arrive as a segmented address that
       USER32 cannot dereference. */
    if (winproc_original(hwnd, msg, &msg32, &wp32, &lp32)) {
        LRESULT r = winproc_default(hwnd, msg32, wp32, lp32);
        int type = winproc_ret_handle_type(msg32);
        winproc_refresh_struct(hwnd, msg32, wp32, lp32, lp);
        /* DefWindowProc answers WM_CTLCOLOR* with a real HBRUSH.  Handed back
           raw, the guest returns its low 16 bits to winproc_bridge, which maps
           that through the handle table and gets NULL - or, once the table has
           grown past that index, somebody else's brush.  Map it here. */
        if (type != H_NONE) return h16(type, (void *)(uintptr_t)r);
        return (uint32_t)r;
    }

    return (uint32_t)winproc_default(hwnd, (UINT)msg16_to_32(msg), wp, (LPARAM)lp);
}

static uint32_t u_GetClientRect(Cpu *c, Args *a)
{
    uint16_t hwnd = arg_word(a);
    uint32_t p = arg_long(a);
    RECT r;
    (void)c;
    if (!GetClientRect(HWND_32(hwnd), &r)) return 0;
    put_rect16(p, &r);
    return 1;
}

static uint32_t u_GetWindowRect(Cpu *c, Args *a)
{
    uint16_t hwnd = arg_word(a);
    uint32_t p = arg_long(a);
    RECT r;
    (void)c;
    if (!GetWindowRect(HWND_32(hwnd), &r)) return 0;
    put_rect16(p, &r);
    return 1;
}

/* ---- the message loop ------------------------------------------------------ */

/* MSG16 is 18 bytes: hwnd, message, wParam, lParam, time, pt. */
static void put_msg16(uint32_t p, const MSG *m)
{
    uint16_t sel = SEGPTR_SEL(p), off = SEGPTR_OFF(p);
    sel_wr16(sel, off,               HWND_16(m->hwnd));
    sel_wr16(sel, (uint16_t)(off+2), (uint16_t)msg32_to_16(m->message));
    sel_wr16(sel, (uint16_t)(off+4), (uint16_t)m->wParam);
    sel_wr32(sel, (uint16_t)(off+6), (uint32_t)m->lParam);
    sel_wr32(sel, (uint16_t)(off+10),m->time);
    sel_wr16(sel, (uint16_t)(off+14),(uint16_t)m->pt.x);
    sel_wr16(sel, (uint16_t)(off+16),(uint16_t)m->pt.y);
}

static void get_msg16(uint32_t p, MSG *m)
{
    uint16_t sel = SEGPTR_SEL(p), off = SEGPTR_OFF(p);
    memset(m, 0, sizeof *m);
    m->hwnd    = HWND_32(sel_rd16(sel, off));
    m->message = msg16_to_32(sel_rd16(sel, (uint16_t)(off + 2)));
    m->wParam  = sel_rd16(sel, (uint16_t)(off + 4));
    m->lParam  = (LPARAM)sel_rd32(sel, (uint16_t)(off + 6));
    m->time    = sel_rd32(sel, (uint16_t)(off + 10));
    m->pt.x    = (int16_t)sel_rd16(sel, (uint16_t)(off + 14));
    m->pt.y    = (int16_t)sel_rd16(sel, (uint16_t)(off + 16));
}

static uint32_t u_GetMessage(Cpu *c, Args *a)
{
    uint32_t p = arg_long(a);
    uint16_t hwnd = arg_word(a);
    uint16_t first = arg_word(a), last = arg_word(a);
    MSG m;
    BOOL r;

    (void)c;
    r = GetMessageA(&m, HWND_32(hwnd), first, last);
    if (r == -1) return 0;
    put_msg16(p, &m);
    return r ? 1u : 0u;
}

static uint32_t u_PeekMessage(Cpu *c, Args *a)
{
    uint32_t p = arg_long(a);
    uint16_t hwnd = arg_word(a);
    uint16_t first = arg_word(a), last = arg_word(a);
    uint16_t flags = arg_word(a);
    MSG m;

    (void)c;
    if (!PeekMessageA(&m, HWND_32(hwnd), first, last, flags)) return 0;
    put_msg16(p, &m);
    return 1;
}

static uint32_t u_TranslateMessage(Cpu *c, Args *a)
{
    MSG m;
    (void)c;
    get_msg16(arg_long(a), &m);
    return (uint32_t)TranslateMessage(&m);
}

static uint32_t u_DispatchMessage(Cpu *c, Args *a)
{
    MSG m;
    (void)c;
    get_msg16(arg_long(a), &m);
    return (uint32_t)DispatchMessageA(&m);
}

/* Everything the guest sends goes out through real USER32, even when the target
   is one of its own windows.  Calling the guest procedure directly would be
   faster and was the first thing this did, but it skips DefDlgProc, skips the
   in-flight record a forward to DefWindowProc needs, and quietly hands a
   16-bit handle to whatever the message reaches.  One translation table per
   direction, always used, is the only version that stays correct. */
static uint32_t u_SendMessage(Cpu *c, Args *a)
{
    uint16_t hwnd = arg_word(a);
    uint16_t msg  = arg_word(a);
    uint16_t wp   = arg_word(a);
    uint32_t lp   = arg_long(a);
    (void)c;
    { uint32_t r = msg16_send(HWND_32(hwnd), msg, wp, lp); updc("SendMessage"); return r; }
}

static uint32_t u_PostMessage(Cpu *c, Args *a)
{
    uint16_t hwnd = arg_word(a);
    uint16_t msg  = arg_word(a);
    uint16_t wp   = arg_word(a);
    uint32_t lp   = arg_long(a);
    (void)c;
    return msg16_post(HWND_32(hwnd), msg, wp, lp);
}

/* ---- odds and ends the startup path needs --------------------------------- */

static uint32_t u_LoadCursor(Cpu *c, Args *a)
{
    char buf[128];
    uint16_t inst = arg_word(a);
    uint32_t name = arg_long(a);
    const char *rn;
    HCURSOR cur = NULL;

    (void)c;
    /* A non-null instance means the cursor is one of the eleven in the game's
       own resources - ScannerCur, OpenGrabCur and the rest.  Only a null
       instance means a system cursor. */
    if (inst) {
        char nbuf[128];
        rn = res_name(name, nbuf, sizeof nbuf);
        if (rn) cur = (HCURSOR)icon_load16(rn, 0);
    }
    if (!cur) cur = LoadCursorA(NULL, gres(name, buf, sizeof buf));
    if (!cur) cur = LoadCursorA(NULL, IDC_ARROW);
    return h16(H_CURSOR, cur);
}

static uint32_t u_LoadIcon(Cpu *c, Args *a)
{
    char buf[128];
    uint16_t inst = arg_word(a);
    uint32_t name = arg_long(a);
    const char *rn;
    HICON ic = NULL;

    (void)c;
    if (inst) {
        char nbuf[128];
        rn = res_name(name, nbuf, sizeof nbuf);
        if (rn) ic = icon_load16(rn, 1);
    }
    if (!ic) ic = LoadIconA(NULL, gres(name, buf, sizeof buf));
    if (!ic) ic = LoadIconA(NULL, IDI_APPLICATION);
    return h16(H_ICON, ic);
}

/* A cursor or icon that came from LoadCursor/LoadIcon with a null instance is
   a shared system object and must not be destroyed; DestroyIcon on one fails
   harmlessly, but the handle mapping has to be dropped either way. */
static uint32_t u_DestroyIcon(Cpu *c, Args *a)
{
    uint16_t h = arg_word(a);
    HICON ic = (HICON)h32(H_ICON, h);
    (void)c;
    if (!ic) return 0;
    DestroyIcon(ic);
    h_release(h);
    return 1;
}

static uint32_t u_DestroyCursor(Cpu *c, Args *a)
{
    uint16_t h = arg_word(a);
    HCURSOR cu = (HCURSOR)h32(H_CURSOR, h);
    (void)c;
    if (!cu) return 0;
    DestroyCursor(cu);
    h_release(h);
    return 1;
}

static uint32_t u_GetFocus(Cpu *c, Args *a)
{
    (void)c; (void)a;
    return HWND_16(GetFocus());
}

static uint32_t u_MessageBox(Cpu *c, Args *a)
{
    char text[512], caption[256];
    uint16_t hwnd = arg_word(a);
    uint32_t t = arg_long(a), cap = arg_long(a);
    uint16_t type = arg_word(a);

    (void)c;
    gstr(t, text, sizeof text);
    gstr(cap, caption, sizeof caption);
    log_msg("MessageBox: \"%s\" / \"%s\"\n", caption, text);
    return (uint32_t)MessageBoxA(HWND_32(hwnd), text,
                                 caption[0] ? caption : "Stars!", type);
}

/* ---- window state and geometry -------------------------------------------- */

static uint32_t u_IsIconic(Cpu *c, Args *a)
{ (void)c; return (uint32_t)IsIconic(HWND_32(arg_word(a))); }

static uint32_t u_IsZoomed(Cpu *c, Args *a)
{ (void)c; return (uint32_t)IsZoomed(HWND_32(arg_word(a))); }

static uint32_t u_IsWindowVisible(Cpu *c, Args *a)
{ (void)c; return (uint32_t)IsWindowVisible(HWND_32(arg_word(a))); }

static uint32_t u_GetParent(Cpu *c, Args *a)
{ (void)c; return HWND_16(GetParent(HWND_32(arg_word(a)))); }

static uint32_t u_GetActiveWindow(Cpu *c, Args *a)
{ (void)c; (void)a; return HWND_16(GetActiveWindow()); }

static uint32_t u_GetWindow(Cpu *c, Args *a)
{
    uint16_t hwnd = arg_word(a);
    uint16_t rel = arg_word(a);
    (void)c;
    return HWND_16(GetWindow(HWND_32(hwnd), rel));
}

static uint32_t u_EnableWindow(Cpu *c, Args *a)
{
    uint16_t hwnd = arg_word(a);
    uint16_t on = arg_word(a);
    (void)c;
    { uint32_t r = (uint32_t)EnableWindow(HWND_32(hwnd), on); updc("EnableWindow"); return r; }
}

static uint32_t u_SetFocus(Cpu *c, Args *a)
{ (void)c; return HWND_16(SetFocus(HWND_32(arg_word(a)))); }

static uint32_t u_SetCapture(Cpu *c, Args *a)
{ (void)c; return HWND_16(SetCapture(HWND_32(arg_word(a)))); }

static uint32_t u_ReleaseCapture(Cpu *c, Args *a)
{ (void)c; (void)a; return (uint32_t)ReleaseCapture(); }

static uint32_t u_MoveWindow(Cpu *c, Args *a)
{
    uint16_t hwnd = arg_word(a);
    int16_t x = arg_sword(a), y = arg_sword(a);
    int16_t w = arg_sword(a), h = arg_sword(a);
    uint16_t repaint = arg_word(a);
    (void)c;
    { uint32_t r = (uint32_t)MoveWindow(HWND_32(hwnd), x, y, w, h, repaint); updc("MoveWindow"); return r; }
}

static uint32_t u_SetWindowPos(Cpu *c, Args *a)
{
    uint16_t hwnd = arg_word(a);
    uint16_t after = arg_word(a);
    int16_t x = arg_sword(a), y = arg_sword(a);
    int16_t w = arg_sword(a), h = arg_sword(a);
    uint16_t flags = arg_word(a);
    HWND ins;
    (void)c;
    /* The special HWND_TOP/BOTTOM/TOPMOST values are small integers, not
       handles, so they must not go through the handle map. */
    ins = (after <= 1 || after == 0xFFFF || after == 0xFFFE)
        ? (HWND)(INT_PTR)(int16_t)after : HWND_32(after);
    { HWND hw = HWND_32(hwnd);
      uint32_t r = (uint32_t)SetWindowPos(hw, ins, x, y, w, h, flags);
      if (trace_paint && painting) {
          RECT got; POINT tl;
          GetWindowRect(hw, &got);
          tl.x = got.left; tl.y = got.top;
          ScreenToClient(GetParent(hw), &tl);
          log_msg("  [paint]   SetWindowPos hwnd=%04X asked %d,%d %dx%d flags=%04X"
                  " -> now %d,%d %ldx%ld\n", hwnd, x, y, w, h, flags,
                  (int)tl.x, (int)tl.y, got.right - got.left, got.bottom - got.top);
      }
      updc("SetWindowPos"); return r; }
}

static uint32_t u_GetWindowText(Cpu *c, Args *a)
{
    uint16_t hwnd = arg_word(a);
    uint32_t buf = arg_long(a);
    int16_t max = arg_sword(a);
    char tmp[512];
    int n;

    (void)c;
    n = GetWindowTextA(HWND_32(hwnd), tmp, sizeof tmp);
    if (max > 0) {
        uint16_t sel = SEGPTR_SEL(buf), off = SEGPTR_OFF(buf);
        int i;
        for (i = 0; i < n && i < max - 1; i++)
            sel_wr8(sel, (uint16_t)(off + i), (uint8_t)tmp[i]);
        sel_wr8(sel, (uint16_t)(off + i), 0);
        n = i;
    }
    return (uint32_t)n;
}

static uint32_t u_SetWindowText(Cpu *c, Args *a)
{
    uint16_t hwnd = arg_word(a);
    uint32_t text = arg_long(a);
    char buf[512];
    (void)c;
    gstr(text, buf, sizeof buf);
    return (uint32_t)SetWindowTextA(HWND_32(hwnd), buf);
}

static uint32_t u_GetWindowPlacement(Cpu *c, Args *a)
{
    uint16_t hwnd = arg_word(a);
    uint32_t p = arg_long(a);
    uint16_t sel = SEGPTR_SEL(p), off = SEGPTR_OFF(p);
    WINDOWPLACEMENT wp;

    (void)c;
    wp.length = sizeof wp;
    if (!GetWindowPlacement(HWND_32(hwnd), &wp)) return 0;
    /* WINDOWPLACEMENT16 is 22 bytes: length, flags, showCmd, ptMinPosition,
       ptMaxPosition, rcNormalPosition. */
    sel_wr16(sel, off, 22);
    sel_wr16(sel, (uint16_t)(off + 2), (uint16_t)wp.flags);
    sel_wr16(sel, (uint16_t)(off + 4), (uint16_t)wp.showCmd);
    sel_wr16(sel, (uint16_t)(off + 6), (uint16_t)wp.ptMinPosition.x);
    sel_wr16(sel, (uint16_t)(off + 8), (uint16_t)wp.ptMinPosition.y);
    sel_wr16(sel, (uint16_t)(off + 10),(uint16_t)wp.ptMaxPosition.x);
    sel_wr16(sel, (uint16_t)(off + 12),(uint16_t)wp.ptMaxPosition.y);
    sel_wr16(sel, (uint16_t)(off + 14),(uint16_t)wp.rcNormalPosition.left);
    sel_wr16(sel, (uint16_t)(off + 16),(uint16_t)wp.rcNormalPosition.top);
    sel_wr16(sel, (uint16_t)(off + 18),(uint16_t)wp.rcNormalPosition.right);
    sel_wr16(sel, (uint16_t)(off + 20),(uint16_t)wp.rcNormalPosition.bottom);
    return 1;
}

/* ---- coordinates and rectangles ------------------------------------------- */

static void put_point16(uint32_t p, const POINT *pt)
{
    uint16_t sel = SEGPTR_SEL(p), off = SEGPTR_OFF(p);
    if (!p) return;
    sel_wr16(sel, off, (uint16_t)(int16_t)pt->x);
    sel_wr16(sel, (uint16_t)(off + 2), (uint16_t)(int16_t)pt->y);
}

static void get_point16(uint32_t p, POINT *pt)
{
    uint16_t sel = SEGPTR_SEL(p), off = SEGPTR_OFF(p);
    pt->x = (int16_t)sel_rd16(sel, off);
    pt->y = (int16_t)sel_rd16(sel, (uint16_t)(off + 2));
}

static uint32_t u_GetCursorPos(Cpu *c, Args *a)
{
    uint32_t p = arg_long(a);
    POINT pt;
    (void)c;
    if (!GetCursorPos(&pt)) return 0;
    put_point16(p, &pt);
    return 1;
}

static uint32_t u_ClientToScreen(Cpu *c, Args *a)
{
    uint16_t hwnd = arg_word(a);
    uint32_t p = arg_long(a);
    POINT pt;
    (void)c;
    get_point16(p, &pt);
    if (!ClientToScreen(HWND_32(hwnd), &pt)) return 0;
    put_point16(p, &pt);
    return 1;
}

static uint32_t u_ScreenToClient(Cpu *c, Args *a)
{
    uint16_t hwnd = arg_word(a);
    uint32_t p = arg_long(a);
    POINT pt;
    (void)c;
    get_point16(p, &pt);
    if (!ScreenToClient(HWND_32(hwnd), &pt)) return 0;
    put_point16(p, &pt);
    return 1;
}

static uint32_t u_MapWindowPoints(Cpu *c, Args *a)
{
    uint16_t from = arg_word(a), to = arg_word(a);
    uint32_t p = arg_long(a);
    uint16_t count = arg_word(a);
    uint16_t sel = SEGPTR_SEL(p), off = SEGPTR_OFF(p);
    unsigned i;

    (void)c;
    for (i = 0; i < count; i++) {
        POINT pt;
        uint32_t q = SEGPTR(sel, (uint16_t)(off + i * 4));
        get_point16(q, &pt);
        MapWindowPoints(HWND_32(from), HWND_32(to), &pt, 1);
        put_point16(q, &pt);
    }
    return 0;
}

static uint32_t u_WindowFromPoint(Cpu *c, Args *a)
{
    uint32_t packed = arg_long(a);
    POINT pt;
    (void)c;
    pt.x = (int16_t)(packed & 0xFFFF);
    pt.y = (int16_t)(packed >> 16);
    return HWND_16(WindowFromPoint(pt));
}

static uint32_t u_SetRect(Cpu *c, Args *a)
{
    uint32_t p = arg_long(a);
    int16_t l = arg_sword(a), t = arg_sword(a);
    int16_t r = arg_sword(a), b = arg_sword(a);
    RECT rc;
    (void)c;
    rc.left = l; rc.top = t; rc.right = r; rc.bottom = b;
    put_rect16(p, &rc);
    return 1;
}

static uint32_t u_CopyRect(Cpu *c, Args *a)
{
    uint32_t dst = arg_long(a), src = arg_long(a);
    RECT r;
    (void)c;
    get_rect16(src, &r);
    put_rect16(dst, &r);
    return 1;
}

static uint32_t u_EqualRect(Cpu *c, Args *a)
{
    uint32_t p1 = arg_long(a), p2 = arg_long(a);
    RECT a1, a2;
    (void)c;
    get_rect16(p1, &a1);
    get_rect16(p2, &a2);
    return (uint32_t)EqualRect(&a1, &a2);
}

static uint32_t u_PtInRect(Cpu *c, Args *a)
{
    uint32_t p = arg_long(a);
    uint32_t packed = arg_long(a);
    RECT r;
    POINT pt;
    (void)c;
    get_rect16(p, &r);
    pt.x = (int16_t)(packed & 0xFFFF);
    pt.y = (int16_t)(packed >> 16);
    return (uint32_t)PtInRect(&r, pt);
}

static uint32_t u_OffsetRect(Cpu *c, Args *a)
{
    uint32_t p = arg_long(a);
    int16_t dx = arg_sword(a), dy = arg_sword(a);
    RECT r;
    (void)c;
    get_rect16(p, &r);
    OffsetRect(&r, dx, dy);
    put_rect16(p, &r);
    return 1;
}

static uint32_t u_InflateRect(Cpu *c, Args *a)
{
    uint32_t p = arg_long(a);
    int16_t dx = arg_sword(a), dy = arg_sword(a);
    RECT r;
    (void)c;
    get_rect16(p, &r);
    InflateRect(&r, dx, dy);
    put_rect16(p, &r);
    return 1;
}

static uint32_t u_IntersectRect(Cpu *c, Args *a)
{
    uint32_t dst = arg_long(a), s1 = arg_long(a), s2 = arg_long(a);
    RECT r, a1, a2;
    BOOL ok;
    (void)c;
    get_rect16(s1, &a1);
    get_rect16(s2, &a2);
    ok = IntersectRect(&r, &a1, &a2);
    put_rect16(dst, &r);
    return (uint32_t)ok;
}

/* A brush argument that is a small integer means COLOR_x + 1, in Win16 as in
   Win32.  Handles start above that range so the two can never be confused. */
static HBRUSH brush16(uint16_t brush)
{
    if (brush && brush < H_FIRST) return GetSysColorBrush(brush - 1);
    return (HBRUSH)h32(H_BRUSH, brush);
}

static uint32_t u_FillRect(Cpu *c, Args *a)
{
    uint16_t hdc = arg_word(a);
    uint32_t p = arg_long(a);
    uint16_t brush = arg_word(a);
    RECT r;
    (void)c;
    get_rect16(p, &r);
    return (uint32_t)FillRect(HDC_32(hdc), &r, brush16(brush));
}

static uint32_t u_FrameRect(Cpu *c, Args *a)
{
    uint16_t hdc = arg_word(a);
    uint32_t p = arg_long(a);
    uint16_t brush = arg_word(a);
    RECT r;
    (void)c;
    get_rect16(p, &r);
    return (uint32_t)FrameRect(HDC_32(hdc), &r, brush16(brush));
}

static uint32_t u_DrawText(Cpu *c, Args *a)
{
    uint16_t hdc = arg_word(a);
    uint32_t textp = arg_long(a);
    int16_t len = arg_sword(a);
    uint32_t p = arg_long(a);
    uint16_t fmt = arg_word(a);
    char buf[1024];
    RECT r;
    int n;

    (void)c;
    gstr(textp, buf, sizeof buf);
    get_rect16(p, &r);
    n = DrawTextA(HDC_32(hdc), buf, len < 0 ? -1 : len, &r, fmt);
    if (fmt & DT_CALCRECT) put_rect16(p, &r);
    return (uint32_t)n;
}

static uint32_t u_DrawIcon(Cpu *c, Args *a)
{
    uint16_t hdc = arg_word(a);
    int16_t x = arg_sword(a), y = arg_sword(a);
    uint16_t icon = arg_word(a);
    (void)c;
    return (uint32_t)DrawIcon(HDC_32(hdc), x, y, (HICON)h32(H_ICON, icon));
}

/* ---- input ---------------------------------------------------------------- */

static uint32_t u_SetCursor(Cpu *c, Args *a)
{ (void)c; return h16(H_CURSOR, SetCursor((HCURSOR)h32(H_CURSOR, arg_word(a)))); }

static uint32_t u_GetKeyState(Cpu *c, Args *a)
{ (void)c; return (uint32_t)(uint16_t)GetKeyState(arg_sword(a)); }

static uint32_t u_GetAsyncKeyState(Cpu *c, Args *a)
{ (void)c; return (uint32_t)(uint16_t)GetAsyncKeyState(arg_sword(a)); }

static uint32_t u_FlashWindow(Cpu *c, Args *a)
{
    uint16_t hwnd = arg_word(a);
    uint16_t inv = arg_word(a);
    (void)c;
    return (uint32_t)FlashWindow(HWND_32(hwnd), inv);
}

static uint32_t u_ExitWindows(Cpu *c, Args *a)
{
    (void)a;
    /* Refuse to shut the machine down on a 1995 game's behalf; just stop. */
    log_msg("ExitWindows requested by the guest; stopping the emulator instead\n");
    c->state = CPU_HALT;
    return 0;
}

/* ---- timers --------------------------------------------------------------- */

static uint32_t u_SetTimer(Cpu *c, Args *a)
{
    uint16_t hwnd = arg_word(a);
    uint16_t id = arg_word(a);
    uint16_t elapse = arg_word(a);
    uint32_t proc = arg_long(a);
    (void)c;
    /* A timer proc would need a callback thunk; the game passes NULL and reads
       WM_TIMER from its own loop, so say so loudly if that ever changes. */
    if (proc) log_msg("SetTimer with a timer procedure is not supported yet\n");
    return (uint32_t)SetTimer(HWND_32(hwnd), id, elapse, NULL);
}

static uint32_t u_KillTimer(Cpu *c, Args *a)
{
    uint16_t hwnd = arg_word(a);
    uint16_t id = arg_word(a);
    (void)c;
    return (uint32_t)KillTimer(HWND_32(hwnd), id);
}

/* ---- scrolling ------------------------------------------------------------ */

static uint32_t u_SetScrollPos(Cpu *c, Args *a)
{
    uint16_t hwnd = arg_word(a);
    uint16_t bar = arg_word(a);
    int16_t pos = arg_sword(a);
    uint16_t redraw = arg_word(a);
    (void)c;
    return (uint32_t)(int16_t)SetScrollPos(HWND_32(hwnd), bar, pos, redraw);
}

static uint32_t u_GetScrollPos(Cpu *c, Args *a)
{
    uint16_t hwnd = arg_word(a);
    uint16_t bar = arg_word(a);
    (void)c;
    return (uint32_t)(int16_t)GetScrollPos(HWND_32(hwnd), bar);
}

static uint32_t u_SetScrollRange(Cpu *c, Args *a)
{
    uint16_t hwnd = arg_word(a);
    uint16_t bar = arg_word(a);
    int16_t lo = arg_sword(a), hi = arg_sword(a);
    uint16_t redraw = arg_word(a);
    (void)c;
    return (uint32_t)SetScrollRange(HWND_32(hwnd), bar, lo, hi, redraw);
}

static uint32_t u_ScrollWindow(Cpu *c, Args *a)
{
    uint16_t hwnd = arg_word(a);
    int16_t dx = arg_sword(a), dy = arg_sword(a);
    uint32_t rp = arg_long(a), cp = arg_long(a);
    RECT r, cr;
    (void)c;
    if (rp) get_rect16(rp, &r);
    if (cp) get_rect16(cp, &cr);
    ScrollWindow(HWND_32(hwnd), dx, dy, rp ? &r : NULL, cp ? &cr : NULL);
    return 1;
}

/* ---- window words and subclassing ---------------------------------------- */

/* GWL_WNDPROC has to round-trip: the guest may read the current procedure,
   install its own, and later call the previous one.  A host procedure is handed
   back wrapped in a reserved selector so CallWindowProc can tell them apart. */
static uint32_t u_GetWindowLong(Cpu *c, Args *a)
{
    uint16_t hwnd16 = arg_word(a);
    int16_t off = arg_sword(a);
    HWND hwnd = HWND_32(hwnd16);

    (void)c;
    if (off == GWL_WNDPROC) {
        uint32_t p16 = winproc_get(hwnd);
        if (p16) return p16;
        return winproc_from_host((WNDPROC)GetWindowLongPtrA(hwnd, GWLP_WNDPROC));
    }
    if (off == DWLP_DLGPROC && dlg_proc_get(hwnd))
        return dlg_proc_get(hwnd);
    /* Win16 windows have 4-byte extra words at non-negative offsets. */
    return (uint32_t)GetWindowLongA(hwnd, off);
}

static uint32_t u_SetWindowLong(Cpu *c, Args *a)
{
    uint16_t hwnd16 = arg_word(a);
    int16_t off = arg_sword(a);
    uint32_t val = arg_long(a);
    HWND hwnd = HWND_32(hwnd16);

    (void)c;
    if (off == GWL_WNDPROC) {
        uint32_t prev = winproc_get(hwnd);
        WNDPROC host_prev = NULL;
        WNDPROC host_new = winproc_to_host(val);

        if (!prev)
            host_prev = (WNDPROC)GetWindowLongPtrA(hwnd, GWLP_WNDPROC);

        /* Un-subclassing: the guest is handing back a value we invented for a
           real Win32 procedure.  Treating it as guest code would run the
           interpreter into the empty selector those wrappers live in, which
           burns the callback budget and then kills the process. */
        if (host_new) {
            winproc_forget(hwnd);
            SetWindowLongPtrA(hwnd, GWLP_WNDPROC, (LONG_PTR)host_new);
            return prev ? prev : winproc_from_host(host_prev);
        }

        /* Install our bridge and remember the guest procedure, so the window
           now runs guest code even though it is a stock control. */
        winproc_set(hwnd, val, task.hinstance);
        SetWindowLongPtrA(hwnd, GWLP_WNDPROC, (LONG_PTR)winproc_bridge);
        return prev ? prev : winproc_from_host(host_prev);
    }
    if (off == DWLP_DLGPROC && dlg_proc_get(hwnd)) {
        uint32_t prev = dlg_proc_get(hwnd);
        dlg_proc_set(hwnd, val, task.hinstance);
        return prev;
    }
    return (uint32_t)SetWindowLongA(hwnd, off, (LONG)val);
}

static uint32_t u_CallWindowProc(Cpu *c, Args *a)
{
    uint32_t proc = arg_long(a);
    uint16_t hwnd16 = arg_word(a);
    uint16_t msg = arg_word(a);
    uint16_t wp = arg_word(a);
    uint32_t lp = arg_long(a);
    HWND hwnd = HWND_32(hwnd16);
    WNDPROC host = winproc_to_host(proc);

    (void)c;
    if (host) {
        /* A wrapped host procedure: use the original 32-bit parameters when this
           is the message we are dispatching. */
        UINT msg32;
        WPARAM wp32;
        LPARAM lp32;
        if (winproc_original(hwnd, msg, &msg32, &wp32, &lp32)) {
            LRESULT r = CallWindowProcA(host, hwnd, msg32, wp32, lp32);
            int type = winproc_ret_handle_type(msg32);
            if (type != H_NONE) return h16(type, (void *)(uintptr_t)r);
            return (uint32_t)r;
        }
        return (uint32_t)CallWindowProcA(host, hwnd, (UINT)msg16_to_32(msg),
                                         wp, (LPARAM)lp);
    }
    {   /* A guest procedure: 16 to 16 needs no translation. */
        uint16_t args[5];
        args[4] = hwnd16; args[3] = msg; args[2] = wp;
        args[1] = (uint16_t)(lp >> 16); args[0] = (uint16_t)lp;
        return call16_wndproc(proc, task.hinstance, args, sizeof args, NULL, 0);
    }
}

/* ---- menus ---------------------------------------------------------------- */

static uint32_t u_GetMenu(Cpu *c, Args *a)
{ (void)c; return HMENU_16(GetMenu(HWND_32(arg_word(a)))); }

static uint32_t u_GetSubMenu(Cpu *c, Args *a)
{
    uint16_t menu = arg_word(a);
    int16_t pos = arg_sword(a);
    (void)c;
    return HMENU_16(GetSubMenu(HMENU_32(menu), pos));
}

static uint32_t u_GetMenuItemCount(Cpu *c, Args *a)
{ (void)c; return (uint32_t)GetMenuItemCount(HMENU_32(arg_word(a))); }

static uint32_t u_DrawMenuBar(Cpu *c, Args *a)
{ (void)c; return (uint32_t)DrawMenuBar(HWND_32(arg_word(a))); }

static uint32_t u_DestroyMenu(Cpu *c, Args *a)
{
    uint16_t m = arg_word(a);
    BOOL r;
    (void)c;
    r = DestroyMenu(HMENU_32(m));
    h_release(m);
    return (uint32_t)r;
}

static uint32_t u_CheckMenuItem(Cpu *c, Args *a)
{
    uint16_t menu = arg_word(a);
    uint16_t id = arg_word(a);
    uint16_t flags = arg_word(a);
    (void)c;
    return (uint32_t)CheckMenuItem(HMENU_32(menu), id, flags);
}

static uint32_t u_EnableMenuItem(Cpu *c, Args *a)
{
    uint16_t menu = arg_word(a);
    uint16_t id = arg_word(a);
    uint16_t flags = arg_word(a);
    (void)c;
    return (uint32_t)EnableMenuItem(HMENU_32(menu), id, flags);
}

static uint32_t u_CreatePopupMenu(Cpu *c, Args *a)
{ (void)c; (void)a; return HMENU_16(CreatePopupMenu()); }

/* AppendMenu(hMenu, wFlags, wIDNewItem, lpNewItem) - and the two arguments are
   not interchangeable.  wIDNewItem is the command id, EXCEPT under MF_POPUP,
   where it is the handle of the drop-down menu; lpNewItem is always the label,
   a far pointer to a string, or a bitmap handle under MF_BITMAP whose high word
   is the caller's data segment and so carries nothing.

   Taking the submenu from lpNewItem instead is silent and specific: the item
   still appears with the right text, because Win32 copies the label, but its
   submenu handle is nonsense and the cascade never opens.  In Stars! that is
   the Advanced New Game Wizard's player list, where Predefined Race, Custom
   Race and Computer Player are all submenus - so every way of ADDING a player
   did nothing, and only "No Player", the one plain command item on the menu,
   worked. */
static uint32_t menu_item(Cpu *c, uint16_t menu, uint16_t flags, uint16_t id,
                          uint32_t data, uint16_t pos, int insert)
{
    char buf[256];
    UINT_PTR item = id;
    const void *str = NULL;

    (void)c;
    if (flags & MF_POPUP) item = (UINT_PTR)HMENU_32(id);
    if (flags & MF_BITMAP) str = (const void *)h32(H_BITMAP, (uint16_t)data);
    else if (!(flags & (MF_SEPARATOR | MF_OWNERDRAW)))
        str = gstr(data, buf, sizeof buf);

    if (insert)
        return (uint32_t)InsertMenuA(HMENU_32(menu), pos, flags, item, str);
    return (uint32_t)AppendMenuA(HMENU_32(menu), flags, item, str);
}

static uint32_t u_AppendMenu(Cpu *c, Args *a)
{
    uint16_t menu = arg_word(a);
    uint16_t flags = arg_word(a);
    uint16_t id = arg_word(a);
    uint32_t data = arg_long(a);
    return menu_item(c, menu, flags, id, data, 0, 0);
}

static uint32_t u_InsertMenu(Cpu *c, Args *a)
{
    uint16_t menu = arg_word(a);
    uint16_t pos = arg_word(a);
    uint16_t flags = arg_word(a);
    uint16_t id = arg_word(a);
    uint32_t data = arg_long(a);
    return menu_item(c, menu, flags, id, data, pos, 1);
}

static uint32_t u_DeleteMenu(Cpu *c, Args *a)
{
    uint16_t menu = arg_word(a);
    uint16_t pos = arg_word(a);
    uint16_t flags = arg_word(a);
    (void)c;
    return (uint32_t)DeleteMenu(HMENU_32(menu), pos, flags);
}

static uint32_t u_TrackPopupMenu(Cpu *c, Args *a)
{
    uint16_t menu = arg_word(a);
    uint16_t flags = arg_word(a);
    int16_t x = arg_sword(a), y = arg_sword(a);
    int16_t reserved = arg_sword(a);
    uint16_t hwnd = arg_word(a);
    uint32_t rp = arg_long(a);
    RECT r;
    (void)c; (void)reserved;
    if (rp) get_rect16(rp, &r);
    return (uint32_t)TrackPopupMenu(HMENU_32(menu), flags, x, y, 0,
                                    HWND_32(hwnd), rp ? &r : NULL);
}

/* ---- help ---------------------------------------------------------------- */

static uint32_t u_WinHelp(Cpu *c, Args *a)
{
    char buf[MAX_PATH];
    uint16_t hwnd = arg_word(a);
    uint32_t file = arg_long(a);
    uint16_t cmd = arg_word(a);
    uint32_t data = arg_long(a);
    static int warned;

    (void)c; (void)hwnd; (void)data;
    gstr(file, buf, sizeof buf);
    /* Modern Windows has no WinHlp32, so there is nothing to forward to. */
    if (!warned) {
        warned = 1;
        log_msg("WinHelp(%s, cmd %u): stubbed, modern Windows has no help viewer\n",
                buf, cmd);
    }
    return 0;
}

/* ---- wsprintf -------------------------------------------------------------- */

/* USER.420 wsprintf is the only cdecl import: the caller pushes right to left
   and cleans up afterwards, so its arguments read upward from the lowest
   address.  Sizes follow the 16-bit convention - %d, %x, %u and %c take a word
   unless an `l` modifier widens them, and %s is a far pointer.  385 call sites,
   so nearly every string the game puts on screen comes through here. */
static uint32_t u_wsprintf(Cpu *c, Args *a)
{
    uint32_t outp = arg_long_up(a);
    uint32_t fmtp = arg_long_up(a);
    char fmt[512], out[2048], spec[64];
    size_t fi = 0, oi = 0;

    (void)c;
    gstr(fmtp, fmt, sizeof fmt);

    while (fmt[fi] && oi + 1 < sizeof out) {
        size_t si = 0;
        int is_long = 0;

        if (fmt[fi] != '%') { out[oi++] = fmt[fi++]; continue; }
        if (fmt[fi + 1] == '%') { out[oi++] = '%'; fi += 2; continue; }

        /* Copy the conversion through to the host, noting the width modifier
           and stopping at the conversion character. */
        spec[si++] = fmt[fi++];
        while (fmt[fi] && si + 4 < sizeof spec &&
               strchr("-+ #0123456789.*", fmt[fi]))
            spec[si++] = fmt[fi++];
        while (fmt[fi] == 'l' || fmt[fi] == 'h' || fmt[fi] == 'F' ||
               fmt[fi] == 'N' || fmt[fi] == 'w') {
            if (fmt[fi] == 'l') is_long = 1;
            fi++;                              /* not passed to the host */
        }
        if (!fmt[fi]) break;

        switch (fmt[fi]) {
        case 'd': case 'i': {
            long v = is_long ? (long)(int32_t)arg_long_up(a)
                             : (long)arg_sword_up(a);
            spec[si++] = 'l';
            spec[si++] = fmt[fi++];
            spec[si] = 0;
            oi += (size_t)snprintf(out + oi, sizeof out - oi, spec, v);
            break;
        }
        case 'u': case 'x': case 'X': case 'o': {
            unsigned long v = is_long ? (unsigned long)arg_long_up(a)
                                      : (unsigned long)arg_word_up(a);
            spec[si++] = 'l';
            spec[si++] = fmt[fi++];
            spec[si] = 0;
            oi += (size_t)snprintf(out + oi, sizeof out - oi, spec, v);
            break;
        }
        case 'c': {
            uint16_t v = arg_word_up(a);
            spec[si++] = 'c';
            spec[si] = 0;
            fi++;
            oi += (size_t)snprintf(out + oi, sizeof out - oi, spec, (int)(v & 0xFF));
            break;
        }
        case 's': case 'S': {
            char sbuf[512];
            uint32_t sp = arg_long_up(a);
            gstr(sp, sbuf, sizeof sbuf);
            spec[si++] = 's';
            spec[si] = 0;
            fi++;
            oi += (size_t)snprintf(out + oi, sizeof out - oi, spec, sbuf);
            break;
        }
        default:
            /* Something we do not recognise: emit it literally rather than
               silently consuming an argument we cannot size. */
            out[oi++] = fmt[fi++];
            break;
        }
        if (oi >= sizeof out) { oi = sizeof out - 1; break; }
    }
    out[oi] = 0;

    /* The caller's buffer has no declared size; Win16 wsprintf has the same
       hazard, and callers size for 1024. */
    {
        uint16_t sel = SEGPTR_SEL(outp), off = SEGPTR_OFF(outp);
        size_t k;
        for (k = 0; k < oi; k++)
            sel_wr8(sel, (uint16_t)(off + k), (uint8_t)out[k]);
        sel_wr8(sel, (uint16_t)(off + k), 0);
    }
    return (uint32_t)oi;
}

/* ---- palettes -------------------------------------------------------------- */

/* On a 32-bit desktop these are largely inert, but they must still succeed and
   RealizePalette has to report a plausible count: code that checks it will take
   a failure path otherwise. */
static uint32_t u_SelectPalette(Cpu *c, Args *a)
{
    uint16_t hdc = arg_word(a);
    uint16_t pal = arg_word(a);
    uint16_t background = arg_word(a);
    (void)c;
    return h16(H_PALETTE, SelectPalette(HDC_32(hdc),
                                        (HPALETTE)h32(H_PALETTE, pal),
                                        background));
}

static uint32_t u_RealizePalette(Cpu *c, Args *a)
{
    (void)c;
    return (uint32_t)RealizePalette(HDC_32(arg_word(a)));
}

/* ---- device contexts and painting ----------------------------------------- */

static uint32_t u_GetDC(Cpu *c, Args *a)
{
    (void)c;
    return HDC_16(GetDC(HWND_32(arg_word(a))));
}

/* Win16 code routinely keeps drawing into a DC after releasing it, so the
   handle mapping is deliberately left in place rather than released here. */
static uint32_t u_ReleaseDC(Cpu *c, Args *a)
{
    uint16_t hwnd = arg_word(a);
    uint16_t hdc = arg_word(a);
    (void)c;
    return (uint32_t)ReleaseDC(HWND_32(hwnd), HDC_32(hdc));
}

/* PAINTSTRUCT16, 32 bytes: hdc, fErase, rcPaint, fRestore, fIncUpdate,
   rgbReserved[16].  Only hdc is read back by EndPaint. */
/* --trace-paint: report the update region around every call that can touch it.
   A window that is dirty again the moment it finishes painting will repaint
   forever, and because such a window always wins the paint queue it starves
   every other window in the process - a failure that is invisible in a normal
   API trace and obvious in this one. */
int trace_paint;

/* BeginPaint fills in more of PAINTSTRUCT than the four documented fields: the
   trailing rgbReserved bytes are, as the header says, "used internally by the
   system", and EndPaint reads them back.  The guest only ever sees the 16-bit
   PAINTSTRUCT16, so the Win32 one has to be kept on this side and handed to
   EndPaint verbatim.  Handing it a freshly zeroed struct instead makes EndPaint
   re-invalidate the part of the window its child controls occupy, so the
   window is dirty again the instant it finishes painting: WM_PAINT fires
   forever, the process burns most of a core, and - because a window that is
   always dirty always wins the paint queue - every other window in the app
   starves and is never drawn at all. */
#define MAX_PAINTS 16
static struct {
    HWND        hwnd;
    PAINTSTRUCT ps;
    int         used;
} paints[MAX_PAINTS];

static void paint_save(HWND h, const PAINTSTRUCT *ps)
{
    int i;
    for (i = 0; i < MAX_PAINTS; i++) {
        if (!paints[i].used) {
            paints[i].used = 1;
            paints[i].hwnd = h;
            paints[i].ps = *ps;
            return;
        }
    }
    log_msg("paint: more than %d nested BeginPaint calls\n", MAX_PAINTS);
}

/* The innermost saved paint for this window; a window procedure can be
   re-entered while it is painting, so this is a stack, not a slot. */
static int paint_take(HWND h, PAINTSTRUCT *out)
{
    int i;
    for (i = MAX_PAINTS; i-- > 0; ) {
        if (paints[i].used && paints[i].hwnd == h) {
            *out = paints[i].ps;
            paints[i].used = 0;
            return 1;
        }
    }
    return 0;
}

/* Report the painting window's update region after `what`, so whichever call
   re-dirties it names itself. */
static void updc(const char *what)
{
    RECT r;
    if (!trace_paint || !painting) return;
    if (GetUpdateRect(painting, &r, FALSE))
        log_msg("  [paint]   after %-20s update=%ld,%ld-%ld,%ld\n",
                what, r.left, r.top, r.right, r.bottom);
}

static void upd(const char *what, HWND h)
{
    RECT r;
    if (!trace_paint) return;
    if (GetUpdateRect(h, &r, FALSE))
        log_msg("  [paint] %-22s hwnd=%p update=%ld,%ld-%ld,%ld\n",
                what, (void *)h, r.left, r.top, r.right, r.bottom);
    else
        log_msg("  [paint] %-22s hwnd=%p update=CLEAN\n", what, (void *)h);
}

static uint32_t u_BeginPaint(Cpu *c, Args *a)
{
    uint16_t hwnd = arg_word(a);
    uint32_t p = arg_long(a);
    uint16_t sel = SEGPTR_SEL(p), off = SEGPTR_OFF(p);
    PAINTSTRUCT ps;
    HDC dc;
    uint16_t hdc16;

    (void)c;
    upd("BeginPaint before", HWND_32(hwnd));
    dc = BeginPaint(HWND_32(hwnd), &ps);
    upd("BeginPaint after", HWND_32(hwnd));
    if (!dc) return 0;
    paint_save(HWND_32(hwnd), &ps);
    painting = HWND_32(hwnd);
    hdc16 = HDC_16(dc);
    sel_wr16(sel, off, hdc16);
    sel_wr16(sel, (uint16_t)(off + 2), (uint16_t)ps.fErase);
    sel_wr16(sel, (uint16_t)(off + 4), (uint16_t)(int16_t)ps.rcPaint.left);
    sel_wr16(sel, (uint16_t)(off + 6), (uint16_t)(int16_t)ps.rcPaint.top);
    sel_wr16(sel, (uint16_t)(off + 8), (uint16_t)(int16_t)ps.rcPaint.right);
    sel_wr16(sel, (uint16_t)(off + 10),(uint16_t)(int16_t)ps.rcPaint.bottom);
    sel_wr16(sel, (uint16_t)(off + 12),(uint16_t)ps.fRestore);
    sel_wr16(sel, (uint16_t)(off + 14),(uint16_t)ps.fIncUpdate);
    return hdc16;
}

static uint32_t u_EndPaint(Cpu *c, Args *a)
{
    uint16_t hwnd = arg_word(a);
    uint32_t p = arg_long(a);
    PAINTSTRUCT ps;

    (void)c;
    /* Prefer the struct BeginPaint actually produced; fall back to rebuilding
       one from the guest's copy only if we somehow never saw the BeginPaint. */
    if (!paint_take(HWND_32(hwnd), &ps)) {
        memset(&ps, 0, sizeof ps);
        ps.hdc = HDC_32(sel_rd16(SEGPTR_SEL(p), SEGPTR_OFF(p)));
    }
    {
        BOOL r;
        updc("(just before EndPaint)");
        r = EndPaint(HWND_32(hwnd), &ps);
        upd("EndPaint after", HWND_32(hwnd));
        painting = NULL;
        return (uint32_t)r;
    }
}

static uint32_t u_InvalidateRect(Cpu *c, Args *a)
{
    uint16_t hwnd = arg_word(a);
    uint32_t p = arg_long(a);
    uint16_t erase = arg_word(a);
    RECT r;

    (void)c;
    if (p) get_rect16(p, &r);
    return (uint32_t)InvalidateRect(HWND_32(hwnd), p ? &r : NULL, erase);
}

static uint32_t u_ValidateRect(Cpu *c, Args *a)
{
    uint16_t hwnd = arg_word(a);
    uint32_t p = arg_long(a);
    RECT r;

    (void)c;
    if (p) get_rect16(p, &r);
    return (uint32_t)ValidateRect(HWND_32(hwnd), p ? &r : NULL);
}

void api_user_register(void)
{
    api_bind("USER",   7, u_ExitWindows);
    api_bind("USER",  10, u_SetTimer);
    api_bind("USER",  12, u_KillTimer);
    api_bind("USER",  17, u_GetCursorPos);
    api_bind("USER",  18, u_SetCapture);
    api_bind("USER",  19, u_ReleaseCapture);
    api_bind("USER",  22, u_SetFocus);
    api_bind("USER",  28, u_ClientToScreen);
    api_bind("USER",  29, u_ScreenToClient);
    api_bind("USER",  30, u_WindowFromPoint);
    api_bind("USER",  31, u_IsIconic);
    api_bind("USER",  34, u_EnableWindow);
    api_bind("USER",  36, u_GetWindowText);
    api_bind("USER",  37, u_SetWindowText);
    api_bind("USER",  46, u_GetParent);
    api_bind("USER",  49, u_IsWindowVisible);
    api_bind("USER",  56, u_MoveWindow);
    api_bind("USER",  60, u_GetActiveWindow);
    api_bind("USER",  61, u_ScrollWindow);
    api_bind("USER",  62, u_SetScrollPos);
    api_bind("USER",  63, u_GetScrollPos);
    api_bind("USER",  64, u_SetScrollRange);
    api_bind("USER",  69, u_SetCursor);
    api_bind("USER",  72, u_SetRect);
    api_bind("USER",  74, u_CopyRect);
    api_bind("USER",  76, u_PtInRect);
    api_bind("USER",  77, u_OffsetRect);
    api_bind("USER",  78, u_InflateRect);
    api_bind("USER",  79, u_IntersectRect);
    api_bind("USER",  81, u_FillRect);
    api_bind("USER",  83, u_FrameRect);
    api_bind("USER",  84, u_DrawIcon);
    api_bind("USER",  85, u_DrawText);
    api_bind("USER", 105, u_FlashWindow);
    api_bind("USER", 106, u_GetKeyState);
    api_bind("USER", 122, u_CallWindowProc);
    api_bind("USER", 135, u_GetWindowLong);
    api_bind("USER", 136, u_SetWindowLong);
    api_bind("USER", 152, u_DestroyMenu);
    api_bind("USER", 154, u_CheckMenuItem);
    api_bind("USER", 155, u_EnableMenuItem);
    api_bind("USER", 157, u_GetMenu);
    api_bind("USER", 159, u_GetSubMenu);
    api_bind("USER", 160, u_DrawMenuBar);
    api_bind("USER", 171, u_WinHelp);
    api_bind("USER", 232, u_SetWindowPos);
    api_bind("USER", 244, u_EqualRect);
    api_bind("USER", 249, u_GetAsyncKeyState);
    api_bind("USER", 258, u_MapWindowPoints);
    api_bind("USER", 262, u_GetWindow);
    api_bind("USER", 263, u_GetMenuItemCount);
    api_bind("USER", 272, u_IsZoomed);
    api_bind("USER", 370, u_GetWindowPlacement);
    api_bind("USER", 410, u_InsertMenu);
    api_bind("USER", 411, u_AppendMenu);
    api_bind("USER", 413, u_DeleteMenu);
    api_bind("USER", 415, u_CreatePopupMenu);
    api_bind("USER", 416, u_TrackPopupMenu);
    api_bind("USER", 282, u_SelectPalette);
    api_bind("USER", 283, u_RealizePalette);
    api_bind("USER",  39, u_BeginPaint);
    api_bind("USER",  40, u_EndPaint);
    api_bind("USER",  66, u_GetDC);
    api_bind("USER",  68, u_ReleaseDC);
    api_bind("USER", 125, u_InvalidateRect);
    api_bind("USER", 127, u_ValidateRect);
    api_bind("USER",   1, u_MessageBox);
    api_bind("USER",  23, u_GetFocus);
    api_bind("USER",  33, u_GetClientRect);
    api_bind("USER",  32, u_GetWindowRect);
    api_bind("USER",  41, u_CreateWindow);
    api_bind("USER",  42, u_ShowWindow);
    api_bind("USER",  53, u_DestroyWindow);
    api_bind("USER",  57, u_RegisterClass);
    api_bind("USER", 107, u_DefWindowProc);
    api_bind("USER", 108, u_GetMessage);
    api_bind("USER", 109, u_PeekMessage);
    api_bind("USER", 110, u_PostMessage);
    api_bind("USER", 111, u_SendMessage);
    api_bind("USER", 113, u_TranslateMessage);
    api_bind("USER", 114, u_DispatchMessage);
    api_bind("USER", 124, u_UpdateWindow);
    api_bind("USER", 173, u_LoadCursor);
    api_bind("USER", 174, u_LoadIcon);
    api_bind("USER",   5, u_InitApp);
    api_bind("USER",   6, u_PostQuitMessage);
    api_bind("USER",  13, u_GetTickCount);
    api_bind("USER",  15, u_GetTickCount);      /* GetCurrentTime is the same */
    api_bind("USER", 104, u_MessageBeep);
    api_bind("USER", 179, u_GetSystemMetrics);
    api_bind("USER", 180, u_GetSysColor);
    api_bind("USER", 420, u_wsprintf);
    api_bind("USER", 457, u_DestroyIcon);
    api_bind("USER", 458, u_DestroyCursor);
}
