/* winproc.c - the window-procedure bridge.
 *
 * Every class the guest registers is registered with real Win32 using OUR C
 * procedure.  When Win32 dispatches a message, we translate it and call the
 * guest's 16-bit procedure through call16.  DefWindowProc and all the
 * non-client behaviour then come from USER32 for free.
 *
 * Two contracts matter and are easy to get wrong:
 *
 *  - A 16-bit window procedure expects AX = hInstance and DS = ES = SS on
 *    entry, and returns its result in DX:AX, not AX.
 *
 *  - For WM_CREATE, WM_NCCREATE, WM_DRAWITEM and WM_COMPAREITEM the struct in
 *    lParam must sit on the 16-bit stack, because Win16 code routinely casts
 *    such an lParam to a near pointer - which only works if it really is in SS.
 */

#include "winproc.h"
#include "thunk.h"
#include "handle.h"
#include "task.h"
#include "sel.h"
#include "log.h"
#include "gmem.h"

extern int trace_paint;   /* --trace-paint */

#include <stdio.h>
#include <string.h>
#include <windows.h>

/* ---- guest procedure registry -------------------------------------------- */

/* A 16-bit procedure the guest gave us, and the reverse: a host procedure we
   need to hand back to the guest (for subclassing).  Host procedures live in a
   reserved selector so that CallWindowProc16 can tell the two apart by
   selector alone. */
#define MAX_HOSTPROC 256
static uint16_t hostproc_sel;
static WNDPROC  hostproc[MAX_HOSTPROC];
static int      hostproc_count;

uint32_t winproc_from_host(WNDPROC p)
{
    int i;
    if (!p) return 0;
    if (!hostproc_sel) {
        hostproc_sel = sel_alloc(0x1000, SK_CODE);
        if (!hostproc_sel) return 0;
    }
    for (i = 0; i < hostproc_count; i++)
        if (hostproc[i] == p) return SEGPTR(hostproc_sel, (uint16_t)(i * 4));
    if (hostproc_count >= MAX_HOSTPROC) {
        log_msg("winproc: too many host procedures\n");
        return 0;
    }
    hostproc[hostproc_count] = p;
    return SEGPTR(hostproc_sel, (uint16_t)(hostproc_count++ * 4));
}

WNDPROC winproc_to_host(uint32_t proc16)
{
    unsigned i;
    if (!proc16 || SEGPTR_SEL(proc16) != hostproc_sel) return NULL;
    i = SEGPTR_OFF(proc16) / 4;
    return (i < (unsigned)hostproc_count) ? hostproc[i] : NULL;
}

/* ---- per-window state ----------------------------------------------------- */

#define MAX_WINDOWS 512
static struct {
    HWND     hwnd;
    uint32_t proc16;          /* the guest procedure, 0 if the window is ours */
    WNDPROC  prev_host;       /* the real procedure when the guest subclassed */
    uint16_t hinstance;
} wins[MAX_WINDOWS];

static int win_slot(HWND h)
{
    int i, free_slot = -1;
    for (i = 0; i < MAX_WINDOWS; i++) {
        if (wins[i].hwnd == h) return i;
        if (!wins[i].hwnd && free_slot < 0) free_slot = i;
    }
    return free_slot;
}

void winproc_set(HWND hwnd, uint32_t proc16, uint16_t hinst)
{
    int i = win_slot(hwnd);
    if (i < 0) { log_msg("winproc: window table full\n"); return; }
    wins[i].hwnd = hwnd;
    wins[i].proc16 = proc16;
    wins[i].hinstance = hinst;
}

uint32_t winproc_get(HWND hwnd)
{
    int i = win_slot(hwnd);
    return (i >= 0 && wins[i].hwnd == hwnd) ? wins[i].proc16 : 0;
}

void winproc_forget(HWND hwnd)
{
    int i = win_slot(hwnd);
    if (i >= 0 && wins[i].hwnd == hwnd) memset(&wins[i], 0, sizeof wins[i]);
}

/* ---- registered classes --------------------------------------------------- */

/* RegisterClass happens long before any window exists, so the guest procedure
   is remembered per class name and picked up by CreateWindow. */
#define MAX_CLASSES 64
static struct {
    char     name[64];
    char     menu[64];
    uint32_t proc16;
    uint16_t hinstance;
} classes[MAX_CLASSES];
static int class_count;

void class_add(const char *name, uint32_t proc16, uint16_t hinst,
               const char *menu)
{
    int i;
    for (i = 0; i < class_count; i++)
        if (_stricmp(classes[i].name, name) == 0) {
            classes[i].proc16 = proc16;
            classes[i].hinstance = hinst;
            snprintf(classes[i].menu, sizeof classes[0].menu, "%s",
                     menu ? menu : "");
            return;
        }
    if (class_count >= MAX_CLASSES) {
        log_msg("winproc: too many window classes\n");
        return;
    }
    snprintf(classes[class_count].name, sizeof classes[0].name, "%s", name);
    snprintf(classes[class_count].menu, sizeof classes[0].menu, "%s",
             menu ? menu : "");
    classes[class_count].proc16 = proc16;
    classes[class_count].hinstance = hinst;
    class_count++;
}

const char *class_menu(const char *name)
{
    int i;
    for (i = 0; i < class_count; i++)
        if (_stricmp(classes[i].name, name) == 0)
            return classes[i].menu[0] ? classes[i].menu : NULL;
    return NULL;
}

uint32_t class_proc(const char *name)
{
    int i;
    for (i = 0; i < class_count; i++)
        if (_stricmp(classes[i].name, name) == 0) return classes[i].proc16;
    return 0;
}

/* ---- the class being created --------------------------------------------- */

/* CreateWindow passes the guest procedure through the class, but a window is
   registered before its HWND exists, so the procedure for the window currently
   being created is stashed here and picked up by the first message. */
static uint32_t pending_proc;
static uint16_t pending_inst;

void winproc_set_pending(uint32_t proc16, uint16_t hinst)
{
    pending_proc = proc16;
    pending_inst = hinst;
}

/* ---- message translation -------------------------------------------------- */

/* Win16 control messages live in the WM_USER range; Win32 gave the same
   messages dedicated numbers.  These are the fixed offsets between them. */
uint32_t msg32_to_16(uint32_t msg)
{
    if (msg >= 0x00F0 && msg <= 0x00FF) return msg + 0x0310;   /* BM_*  */
    if (msg >= 0x00B0 && msg <= 0x00DF) return msg + 0x0350;   /* EM_*  */
    if (msg >= 0x0180 && msg <= 0x01FF) return msg + 0x0281;   /* LB_*  */
    if (msg >= 0x0140 && msg <= 0x017F) return msg + 0x02C0;   /* CB_*  */
    if (msg >= 0x00E0 && msg <= 0x00EF) return msg + 0x0320;   /* SBM_* */
    return msg;
}

uint32_t msg16_to_32(uint32_t msg)
{
    if (msg >= 0x0400 && msg <= 0x040F) return msg - 0x0310;
    if (msg >= 0x0400 && msg <= 0x042F) return msg - 0x0350;
    return msg;
}

/* ---- the message currently being dispatched -------------------------------- */

/* When guest code forwards a message to DefWindowProc, the parameters it hands
   back are the 16-bit ones we gave it - and for anything carrying a pointer
   (WM_NCCREATE and WM_CREATE above all) those are segmented addresses that real
   USER32 cannot dereference.  Converting back is not possible in general, but it
   is never necessary either: the original 32-bit parameters are right here.  So
   keep a small stack of in-flight messages and reuse the originals.
   It is a stack rather than a single slot because a window procedure can be
   reentered while another message is still being dispatched. */
#define MAX_INFLIGHT 32
static struct {
    HWND   hwnd;
    UINT   msg16;
    UINT   msg32;
    WPARAM wp32;
    LPARAM lp32;
} inflight[MAX_INFLIGHT];
static int inflight_depth;

static int inflight_push(HWND hwnd, UINT msg16, UINT msg32,
                         WPARAM wp32, LPARAM lp32)
{
    /* Returning whether it pushed matters: an unconditional pop after a refused
       push walks the top down into a live outer frame, and winproc_original
       would then hand DefWindowProc some other message's parameters. */
    if (inflight_depth >= MAX_INFLIGHT) return 0;
    inflight[inflight_depth].hwnd  = hwnd;
    inflight[inflight_depth].msg16 = msg16;
    inflight[inflight_depth].msg32 = msg32;
    inflight[inflight_depth].wp32  = wp32;
    inflight[inflight_depth].lp32  = lp32;
    inflight_depth++;
    return 1;
}

static void inflight_pop(void)
{
    if (inflight_depth > 0) inflight_depth--;
}

/* Find the innermost in-flight message matching this window and 16-bit message
   number, and recover its original 32-bit parameters. */
int winproc_original(HWND hwnd, UINT msg16, UINT *msg32, WPARAM *wp, LPARAM *lp)
{
    int i;
    for (i = inflight_depth - 1; i >= 0; i--)
        if (inflight[i].hwnd == hwnd && inflight[i].msg16 == msg16) {
            *msg32 = inflight[i].msg32;
            *wp = inflight[i].wp32;
            *lp = inflight[i].lp32;
            return 1;
        }
    return 0;
}

/* ---- the bridge ----------------------------------------------------------- */

/* Translate one message from Win32 form into Win16 form.
 *
 * `extra`/`extralen` optionally receive a struct to be copied onto the guest
 * stack, with the resulting far pointer becoming lParam - Win16 code routinely
 * casts such an lParam to a near pointer, which only works if it really is in
 * SS.  `back` records what has to be copied out again afterwards.
 *
 * The default is to pass wParam and lParam through unchanged.  That is right
 * far more often than it looks: mouse messages pack a POINT into lParam
 * identically in both worlds, and any message the guest merely forwards to
 * DefWindowProc is restored from the in-flight stack anyway.  What must be
 * handled here is every message the guest actually READS.
 */
enum { BACK_NONE = 0, BACK_MEASUREITEM, BACK_MINMAX, BACK_WINDOWPOS, BACK_NCCALC };

struct xlat {
    UINT     msg16;
    uint16_t wp16;
    uint32_t lp16;
    unsigned extralen;
    int      back;          /* what to copy out of the guest struct afterwards */
    int      ret_handle;    /* H_* type when the return value is a handle      */
};

static void msg_to_16(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                      uint16_t hinst, uint8_t *extra, struct xlat *x)
{
    uint8_t *p = extra;

    x->msg16 = (UINT)msg32_to_16(msg);
    x->wp16 = (uint16_t)wp;
    x->lp16 = (uint32_t)lp;
    x->extralen = 0;
    x->back = BACK_NONE;
    x->ret_handle = H_NONE;

    switch (msg) {

    /* ---- structs the guest reads through lParam ---------------------------- */

    case WM_NCCREATE:
    case WM_CREATE: {
        const CREATESTRUCTA *cs = (const CREATESTRUCTA *)lp;
        /* CREATESTRUCT16: lpCreateParams, hInstance, hMenu, hwndParent,
           cy, cx, y, x, style, lpszName, lpszClass, dwExStyle.  The
           coordinates run in the opposite order from Win32. */
        memset(extra, 0, 64);
        *(uint32_t *)(p +  0) = (uint32_t)(uintptr_t)cs->lpCreateParams;
        *(uint16_t *)(p +  4) = hinst;
        *(uint16_t *)(p +  6) = HMENU_16(cs->hMenu);
        *(uint16_t *)(p +  8) = HWND_16(cs->hwndParent);
        *(int16_t  *)(p + 10) = (int16_t)cs->cy;
        *(int16_t  *)(p + 12) = (int16_t)cs->cx;
        *(int16_t  *)(p + 14) = (int16_t)cs->y;
        *(int16_t  *)(p + 16) = (int16_t)cs->x;
        *(uint32_t *)(p + 18) = (uint32_t)cs->style;
        *(uint32_t *)(p + 22) = 0;                        /* lpszName  */
        *(uint32_t *)(p + 26) = 0;                        /* lpszClass */
        /* Hide the extended styles this shim added for itself.  Win16 had no
           dwExStyle worth speaking of and certainly no WS_EX_APPWINDOW, so the
           guest must not find one here. */
        *(uint32_t *)(p + 30) = (uint32_t)cs->dwExStyle
                              & ~(uint32_t)(WS_EX_APPWINDOW | WS_EX_TOOLWINDOW);
        x->extralen = 34;
        break;
    }

    case WM_GETMINMAXINFO: {
        /* Five POINT16 in place of five POINT.  This is the first message an
           overlapped window ever receives, so it fires during the guest's very
           first CreateWindow. */
        const MINMAXINFO *mm = (const MINMAXINFO *)lp;
        const POINT *src[5] = { &mm->ptReserved, &mm->ptMaxSize,
                                &mm->ptMaxPosition, &mm->ptMinTrackSize,
                                &mm->ptMaxTrackSize };
        int k;
        for (k = 0; k < 5; k++) {
            *(int16_t *)(p + k * 4 + 0) = (int16_t)src[k]->x;
            *(int16_t *)(p + k * 4 + 2) = (int16_t)src[k]->y;
        }
        x->extralen = 20;
        x->back = BACK_MINMAX;
        break;
    }

    case WM_WINDOWPOSCHANGING:
    case WM_WINDOWPOSCHANGED: {
        const WINDOWPOS *wpos = (const WINDOWPOS *)lp;
        *(uint16_t *)(p +  0) = HWND_16(wpos->hwnd);
        *(uint16_t *)(p +  2) = HWND_16(wpos->hwndInsertAfter);
        *(int16_t  *)(p +  4) = (int16_t)wpos->x;
        *(int16_t  *)(p +  6) = (int16_t)wpos->y;
        *(int16_t  *)(p +  8) = (int16_t)wpos->cx;
        *(int16_t  *)(p + 10) = (int16_t)wpos->cy;
        *(uint16_t *)(p + 12) = (uint16_t)wpos->flags;
        x->extralen = 14;
        if (msg == WM_WINDOWPOSCHANGING) x->back = BACK_WINDOWPOS;
        break;
    }

    case WM_NCCALCSIZE: {
        /* With wParam set lParam is NCCALCSIZE_PARAMS; without it, a bare
           RECT.  Only rgrc[0] is ever written back. */
        const RECT *r = wp ? &((const NCCALCSIZE_PARAMS *)lp)->rgrc[0]
                           : (const RECT *)lp;
        int n = wp ? 3 : 1, k;
        for (k = 0; k < n; k++) {
            const RECT *rr = wp ? &((const NCCALCSIZE_PARAMS *)lp)->rgrc[k] : r;
            *(int16_t *)(p + k * 8 + 0) = (int16_t)rr->left;
            *(int16_t *)(p + k * 8 + 2) = (int16_t)rr->top;
            *(int16_t *)(p + k * 8 + 4) = (int16_t)rr->right;
            *(int16_t *)(p + k * 8 + 6) = (int16_t)rr->bottom;
        }
        x->extralen = (unsigned)(n * 8);
        if (wp) *(uint32_t *)(p + 24) = 0;       /* lppos: not handed over */
        if (wp) x->extralen = 28;
        x->back = BACK_NCCALC;
        break;
    }

    case WM_MEASUREITEM: {
        if (trace_paint) {
            const MEASUREITEMSTRUCT *q = (const MEASUREITEMSTRUCT *)lp;
            log_msg("  [paint] WM_MEASUREITEM in  ctl=%u id=%u item=%u %ux%u\n",
                    (unsigned)q->CtlType, (unsigned)q->CtlID, (unsigned)q->itemID,
                    (unsigned)q->itemWidth, (unsigned)q->itemHeight);
        }
        /* MEASUREITEMSTRUCT16, 14 bytes.  itemWidth and itemHeight are the
           whole point of the message and must be copied back out. */
        const MEASUREITEMSTRUCT *mi = (const MEASUREITEMSTRUCT *)lp;
        *(uint16_t *)(p +  0) = (uint16_t)mi->CtlType;
        *(uint16_t *)(p +  2) = (uint16_t)mi->CtlID;
        *(uint16_t *)(p +  4) = (uint16_t)mi->itemID;
        *(uint16_t *)(p +  6) = (uint16_t)mi->itemWidth;
        *(uint16_t *)(p +  8) = (uint16_t)mi->itemHeight;
        *(uint32_t *)(p + 10) = (uint32_t)mi->itemData;
        x->extralen = 14;
        x->back = BACK_MEASUREITEM;
        break;
    }

    case WM_DRAWITEM: {
        /* DRAWITEMSTRUCT16, 26 bytes.  hwndItem is an HMENU rather than an
           HWND when the item is a menu item. */
        const DRAWITEMSTRUCT *di = (const DRAWITEMSTRUCT *)lp;
        *(uint16_t *)(p +  0) = (uint16_t)di->CtlType;
        *(uint16_t *)(p +  2) = (uint16_t)di->CtlID;
        *(uint16_t *)(p +  4) = (uint16_t)di->itemID;
        *(uint16_t *)(p +  6) = (uint16_t)di->itemAction;
        *(uint16_t *)(p +  8) = (uint16_t)di->itemState;
        *(uint16_t *)(p + 10) = (di->CtlType == ODT_MENU)
                              ? HMENU_16((HMENU)di->hwndItem)
                              : HWND_16(di->hwndItem);
        *(uint16_t *)(p + 12) = HDC_16(di->hDC);
        *(int16_t  *)(p + 14) = (int16_t)di->rcItem.left;
        *(int16_t  *)(p + 16) = (int16_t)di->rcItem.top;
        *(int16_t  *)(p + 18) = (int16_t)di->rcItem.right;
        *(int16_t  *)(p + 20) = (int16_t)di->rcItem.bottom;
        *(uint32_t *)(p + 22) = (uint32_t)di->itemData;
        x->extralen = 26;
        break;
    }

    case WM_DELETEITEM: {
        const DELETEITEMSTRUCT *di = (const DELETEITEMSTRUCT *)lp;
        *(uint16_t *)(p + 0) = (uint16_t)di->CtlType;
        *(uint16_t *)(p + 2) = (uint16_t)di->CtlID;
        *(uint16_t *)(p + 4) = (uint16_t)di->itemID;
        *(uint16_t *)(p + 6) = HWND_16(di->hwndItem);
        *(uint32_t *)(p + 8) = (uint32_t)di->itemData;
        x->extralen = 12;
        break;
    }

    /* ---- messages that merely repack their two parameters ------------------ */

    case WM_COMMAND:
        /* Win16 puts the control handle in lParam's LOW word and the notify
           code in its HIGH word; Win32 puts the notify code in wParam's high
           word and the handle in lParam.  This is how every menu pick and
           every button click arrives. */
        x->wp16 = LOWORD(wp);
        x->lp16 = (uint32_t)MAKELONG(HWND_16((HWND)lp), HIWORD(wp));
        break;

    case WM_HSCROLL:
    case WM_VSCROLL:
        /* Here the handle is in the HIGH word of lParam, and the position -
           which is what a thumb drag carries - is in the high word of wParam. */
        x->wp16 = LOWORD(wp);
        x->lp16 = (uint32_t)MAKELONG(HIWORD(wp), HWND_16((HWND)lp));
        break;

    case WM_MENUSELECT:
        /* Not the WM_COMMAND layout, despite the resemblance: Win16 puts the
           flags in the LOW half of lParam and the menu in the HIGH half.  And
           where Win32 reports a submenu by its position index, Win16 named the
           submenu itself, so resolve it. */
        x->wp16 = LOWORD(wp);
        if ((HIWORD(wp) & MF_POPUP) && lp) {
            HMENU sub = GetSubMenu((HMENU)lp, LOWORD(wp));
            if (sub) x->wp16 = HMENU_16(sub);
        }
        x->lp16 = (uint32_t)MAKELONG(HIWORD(wp), HMENU_16((HMENU)lp));
        break;

    case WM_MENUCHAR:
        x->wp16 = LOWORD(wp);
        x->lp16 = (uint32_t)MAKELONG(HIWORD(wp), HMENU_16((HMENU)lp));
        break;

    case WM_ACTIVATE:
        /* Unlike WM_COMMAND, the handle goes in the HIGH word here. */
        x->wp16 = LOWORD(wp);
        x->lp16 = (uint32_t)MAKELONG(HIWORD(wp) ? 1 : 0, HWND_16((HWND)lp));
        break;

    case WM_PARENTNOTIFY:
        x->wp16 = LOWORD(wp);
        if (LOWORD(wp) == WM_CREATE || LOWORD(wp) == WM_DESTROY)
            x->lp16 = (uint32_t)MAKELONG(HWND_16((HWND)lp), HIWORD(wp));
        break;

    case WM_VKEYTOITEM:
    case WM_CHARTOITEM:
        x->wp16 = LOWORD(wp);
        x->lp16 = (uint32_t)MAKELONG(HWND_16((HWND)lp), HIWORD(wp));
        break;

    case WM_ACTIVATEAPP:
        /* Win16 passes a task handle here; a thread id means nothing to the
           guest, so hand it zero rather than a plausible-looking lie. */
        x->lp16 = 0;
        break;

    /* ---- seven Win32 messages collapsing into one Win16 message ------------ */

    case WM_CTLCOLORMSGBOX:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSCROLLBAR:
    case WM_CTLCOLORSTATIC:
        x->msg16 = 0x0019;                        /* WM_CTLCOLOR */
        x->wp16 = HDC_16((HDC)wp);
        x->lp16 = (uint32_t)MAKELONG(HWND_16((HWND)lp),
                                     msg - WM_CTLCOLORMSGBOX);
        x->ret_handle = H_BRUSH;                  /* the guest returns a brush */
        break;

    /* ---- messages whose wParam is a handle --------------------------------- */

    case WM_SETCURSOR:
    case WM_MOUSEACTIVATE:
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_INITDIALOG:
        x->wp16 = HWND_16((HWND)wp);
        break;

    case WM_INITMENU:
    case WM_INITMENUPOPUP:
        /* This is how the game greys and checks its menu items before a popup
           opens; a truncated HMENU would leave every item in its design-time
           state. */
        x->wp16 = HMENU_16((HMENU)wp);
        break;

    case WM_SETFONT:
        x->wp16 = h16(H_FONT, (void *)wp);
        break;

    case WM_ERASEBKGND:
    case WM_ICONERASEBKGND:
        x->wp16 = HDC_16((HDC)wp);
        break;

    case WM_PAINT:
        x->wp16 = HDC_16((HDC)wp);                /* normally 0 in Win32 */
        break;

    case WM_ENTERIDLE:
        /* wParam says whether it is a dialog or a menu; lParam is the window,
           and a 32-bit one truncates to nonsense. */
        x->lp16 = HWND_16((HWND)lp);
        break;

    case WM_QUERYDRAGICON:
        x->ret_handle = H_ICON;
        break;

    case WM_GETDLGCODE:
        /* Win16 never passed the MSG through, and guest code was written
           against a lParam that is always zero. */
        x->wp16 = 0;
        x->lp16 = 0;
        break;

    default:
        break;
    }
}

/* Copy the parts of a guest-side struct that the message contract says flow
   back out, now that the guest has had a chance to write them. */
static void msg_copy_back(const struct xlat *x, const uint8_t *g,
                          UINT msg, WPARAM wp, LPARAM lp)
{
    switch (x->back) {
    case BACK_MEASUREITEM: {
        MEASUREITEMSTRUCT *mi = (MEASUREITEMSTRUCT *)lp;
        mi->itemWidth  = *(const uint16_t *)(g + 6);
        mi->itemHeight = *(const uint16_t *)(g + 8);
        if (trace_paint)
            log_msg("  [paint] WM_MEASUREITEM out %ux%u\n",
                    (unsigned)mi->itemWidth, (unsigned)mi->itemHeight);
        break;
    }
    case BACK_MINMAX: {
        MINMAXINFO *mm = (MINMAXINFO *)lp;
        POINT *dst[5] = { &mm->ptReserved, &mm->ptMaxSize, &mm->ptMaxPosition,
                          &mm->ptMinTrackSize, &mm->ptMaxTrackSize };
        int k;
        for (k = 0; k < 5; k++) {
            dst[k]->x = *(const int16_t *)(g + k * 4 + 0);
            dst[k]->y = *(const int16_t *)(g + k * 4 + 2);
        }
        break;
    }
    case BACK_WINDOWPOS: {
        WINDOWPOS *wpos = (WINDOWPOS *)lp;
        wpos->x     = *(const int16_t *)(g +  4);
        wpos->y     = *(const int16_t *)(g +  6);
        wpos->cx    = *(const int16_t *)(g +  8);
        wpos->cy    = *(const int16_t *)(g + 10);
        wpos->flags = *(const uint16_t *)(g + 12);
        break;
    }
    case BACK_NCCALC: {
        RECT *r = wp ? &((NCCALCSIZE_PARAMS *)lp)->rgrc[0] : (RECT *)lp;
        r->left   = *(const int16_t *)(g + 0);
        r->top    = *(const int16_t *)(g + 2);
        r->right  = *(const int16_t *)(g + 4);
        r->bottom = *(const int16_t *)(g + 6);
        break;
    }
    default:
        break;
    }
    (void)msg;
}

/* Translate one message and run a guest procedure with it.  Both bridges - the
   window-procedure one below and the dialog-procedure one in dlg.c - go through
   here, so the translation table has exactly one caller shape. */
uint32_t winproc_call16(HWND hwnd, uint32_t proc16, uint16_t hinst,
                        UINT msg, WPARAM wp, LPARAM lp, int *ret_handle)
{
    uint16_t args[5];
    uint8_t  extra[64], before[64];
    struct xlat x;
    uint32_t r;
    int pushed;

    memset(extra, 0, sizeof extra);
    msg_to_16(hwnd, msg, wp, lp, hinst, extra, &x);

    /* Pascal order: args[0] is the last declared argument. */
    args[4] = HWND_16(hwnd);
    args[3] = (uint16_t)x.msg16;
    args[2] = x.wp16;
    args[1] = (uint16_t)(x.lp16 >> 16);
    args[0] = (uint16_t)x.lp16;

    /* Record the ORIGINAL 32-bit parameters, not the translated ones: this is
       what a forward to DefWindowProc is restored from, and recording the
       translated pair would hand USER32 a 16-bit handle. */
    memcpy(before, extra, sizeof before);

    pushed = inflight_push(hwnd, x.msg16, msg, wp, lp);
    r = call16_wndproc(proc16, hinst, args, sizeof args,
                       x.extralen ? extra : NULL, x.extralen);
    if (pushed) inflight_pop();

    /* Copy back only if the guest wrote something.  A window procedure that
       simply forwards WM_NCCALCSIZE to DefWindowProc leaves its own copy
       untouched, while USER32 has already adjusted the real struct through the
       original pointer - copying the stale 16-bit copy over that undoes the
       adjustment, and the window ends up with a client area covering its whole
       frame, no caption and no menu bar. */
    if (x.back != BACK_NONE && memcmp(before, extra, x.extralen) != 0)
        msg_copy_back(&x, extra, msg, wp, lp);

    if (ret_handle) *ret_handle = x.ret_handle;
    return r;
}

/* A handle-valued result, but only when it really is one: a procedure that
   simply returns TRUE for WM_CTLCOLOR is saying "handled", not naming brush
   number 1, and running that through the handle map is a type error every
   time the control repaints. */
/* The H_* type of a message's result, or H_NONE.  u_DefWindowProc needs this:
   when the guest forwards and USER32 answers with a real handle, that handle
   has to be mapped down before it goes back to 16-bit code. */
int winproc_ret_handle_type(UINT msg)
{
    switch (msg) {
    case WM_CTLCOLORMSGBOX: case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORBTN:    case WM_CTLCOLORDLG:  case WM_CTLCOLORSCROLLBAR:
    case WM_CTLCOLORSTATIC:
        return H_BRUSH;
    case WM_QUERYDRAGICON:
        return H_ICON;
    default:
        return H_NONE;
    }
}

LRESULT winproc_ret_handle(int type, uint32_t r)
{
    if ((uint16_t)r < H_FIRST) return (LRESULT)r;
    return (LRESULT)(uintptr_t)h32_quiet(type, (uint16_t)r);
}

LRESULT CALLBACK winproc_bridge(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    uint32_t proc16 = winproc_get(hwnd);
    uint16_t hinst;
    uint32_t r;
    int ret_handle = H_NONE;
    int i;

    if (!proc16) {
        /* The first message for a window created by the guest: adopt it. */
        if (pending_proc) {
            winproc_set(hwnd, pending_proc, pending_inst);
            proc16 = pending_proc;
            pending_proc = 0;
        } else {
            return DefWindowProcA(hwnd, msg, wp, lp);
        }
    }
    i = win_slot(hwnd);
    hinst = (i >= 0) ? wins[i].hinstance : task.hinstance;

    /* Win16 has no wheel, and delivering one with a truncated delta would be
       worse than not delivering it. */
    if (msg == WM_MOUSEWHEEL) return DefWindowProcA(hwnd, msg, wp, lp);

    /* Nothing can run once a stop is latched, so let USER32 finish the teardown
       on its own rather than calling a guest that cannot execute. */
    if (cpu_stop_latched()) return DefWindowProcA(hwnd, msg, wp, lp);

    r = winproc_call16(hwnd, proc16, hinst, msg, wp, lp, &ret_handle);

    if (msg == WM_NCCREATE && r == 0) return FALSE;
    if (ret_handle != H_NONE) return winproc_ret_handle(ret_handle, r);
    return (LRESULT)r;
}

/* DefWindowProc for the guest: hand the message straight to the real one. */
LRESULT winproc_default(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* The other half of that: DefWindowProc has just written its answer into the
   32-bit struct through the original pointer, and the guest is holding a 16-bit
   copy that is now out of date.  Guest code routinely reads the copy after
   forwarding - "let DefWindowProc size the client area, then take another few
   pixels off the top" is the standard shape of a WM_NCCALCSIZE handler - so
   rebuild the copy in place.  `guest_lp` is the far pointer the guest passed
   back to us, which is exactly where its copy lives. */
void winproc_refresh_struct(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                            uint32_t guest_lp)
{
    uint8_t extra[64];
    struct xlat x;

    if (!guest_lp) return;
    memset(extra, 0, sizeof extra);
    msg_to_16(hwnd, msg, wp, lp, 0, extra, &x);
    if (x.back == BACK_NONE || !x.extralen) return;
    g_write(guest_lp, extra, x.extralen);
}
