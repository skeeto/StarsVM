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
#include "harness.h"

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

/* Win16 numbered each stock control's messages from WM_USER, so BM_GETCHECK,
   EM_GETSEL and CB_GETEDITSEL are all 0x0400 and only the window's class says
   which one is meant; Win32 gave each class a block of its own below WM_USER.
   So on the way out a number is renumbered only for a window of the class it
   belongs to.  To any other window a number from WM_USER up is its own
   business - the game posts its frame WM_USER+100 - and it has to arrive as
   it was sent, whichever way it goes round.

   On the way back the blocks do not overlap, so the number alone says which
   message it is, and it becomes the Win16 number whatever the window.  That
   is how Windows 3.1 had it: USER sends BM_SETCHECK to whatever dialog item
   CheckDlgButton names, and EM_SETSEL and BM_SETSTYLE to whatever control
   answers WM_GETDLGCODE for them, and a control of a program's own class got
   the same WM_USER numbers a button or an edit did.  Wine's 16-bit USER goes
   by number this way too (WINPROC_CallProc32ATo16).  It cannot catch a
   number of the guest's own, which goes out from WM_USER up whatever it is.

   The blocks run as far as Win16's did (Wine's winuser16.h has them all).
   Win32 went on adding to the end of each - BM_CLICK, EM_SETMARGINS,
   LB_GETLISTBOXINFO - and those have no Win16 number to be given.  The
   static's pair has a block of its own, which Win32 put in the numbers left
   over after the combo box's. */
static const struct ctlblock {
    const char *cls;            /* the class, as RealGetWindowClass names it */
    uint16_t    msg32, msg16;   /* the first message's two numbers           */
    uint16_t    count;
} ctlblocks[] = {
    { "Edit",      0x00B0, 0x0400, 35 },  /* EM_GETSEL .. EM_GETPASSWORDCHAR  */
    { "ScrollBar", 0x00E0, 0x0400,  5 },  /* SBM_SETPOS .. SBM_ENABLE_ARROWS  */
    { "Button",    0x00F0, 0x0400,  5 },  /* BM_GETCHECK .. BM_SETSTYLE       */
    { "ComboBox",  0x0140, 0x0400, 25 },  /* CB_GETEDITSEL ..
                                             CB_FINDSTRINGEXACT               */
    { "Static",    0x0170, 0x0400,  2 },  /* STM_SETICON, STM_GETICON         */
    { "ListBox",   0x0180, 0x0401, 37 },  /* LB_ADDSTRING .. LB_CARETOFF      */
    { "ComboLBox", 0x0180, 0x0401, 37 },  /* a combo box's list, by its name  */
};

static int ctl_has(const struct ctlblock *b, uint32_t msg, int win16)
{
    return msg - (win16 ? b->msg16 : b->msg32) < b->count;
}

/* The block the Win16 `msg` is in for `hwnd`: NULL unless the window is a
   stock control and the number is one of its class's.  RealGetWindowClass
   names the class whose procedure the window really runs, so a control the
   guest has subclassed is still what it was, and so is a class of its own
   that it built on one.  Windows 11 calls a combo box's list a ListBox that
   way too; the ComboLBox row is for a host that calls it by its own name.
   The class is looked up only for a number some block has, which leaves out
   nearly everything. */
static const struct ctlblock *ctl_block(HWND hwnd, uint32_t msg)
{
    const size_t n = sizeof ctlblocks / sizeof ctlblocks[0];
    char cls[16];
    size_t i;

    for (i = 0; i < n && !ctl_has(&ctlblocks[i], msg, 1); i++)
        ;
    if (i == n || !hwnd || !RealGetWindowClassA(hwnd, cls, sizeof cls))
        return NULL;
    for (i = 0; i < n; i++)
        if (!_stricmp(cls, ctlblocks[i].cls))
            return ctl_has(&ctlblocks[i], msg, 1) ? &ctlblocks[i] : NULL;
    return NULL;
}

uint32_t msg32_to_16(uint32_t msg)
{
    size_t i;

    for (i = 0; i < sizeof ctlblocks / sizeof ctlblocks[0]; i++)
        if (ctl_has(&ctlblocks[i], msg, 0))
            return msg - ctlblocks[i].msg32 + ctlblocks[i].msg16;
    return msg;
}

uint32_t msg16_to_32(HWND hwnd, uint32_t msg)
{
    const struct ctlblock *b = ctl_block(hwnd, msg);
    return b ? msg - b->msg16 + b->msg32 : msg;
}

/* Whether the guest can know what a message means, which below WM_USER is
   whether Windows 3.1 had it.  Win32 has been adding system messages ever
   since, and sends them to every window: WM_GETICON to draw its caption, the
   WM_UAH* messages carrying host pointers to draw a themed menu bar, the
   compositor's notifications and registered messages through the queue.  To
   the guest each is a number it has never heard of, handed to DefWindowProc
   at the cost of a window procedure call or a turn of its message loop.  And
   some come from other threads whenever those get round to it - the shell
   asks a new top-level window for its icon too - so whether they arrive
   before the game exits is down to timing, which made a --fixed-clock run's
   instruction count vary, 32 at a time.

   The ranges are Windows 3.1's own numbers, undocumented ones included (as
   Schulman et al., Undocumented Windows, 1992, lists them), and MMSYSTEM's,
   whose MM_MCINOTIFY the game's frame window handles; so nothing the guest
   could have been sent under Win16 is lost.  A stock control's messages
   count as far as Win16 took its class's block, at any window, as they have
   a Win16 number there (see msg32_to_16); Win32 went on adding to each block
   (BM_CLICK, LB_GETLISTBOXINFO) and put WM_INPUT at the end of the
   buttons', and none of that is anything Win16 sent.
   From WM_USER up to the registered range a number is private to its window
   class or application, which is the guest's business; a registered message
   is the host's, since the guest registers none. */
int msg_win16(UINT msg)
{
    static const struct { uint16_t lo, hi; } known[] = {
        { 0x0000, 0x0024 },     /* WM_NULL .. WM_GETMINMAXINFO           */
        { 0x0026, 0x0039 },     /* WM_PAINTICON .. WM_COMPAREITEM, the
                                   hot keys and WM_FILESYSCHANGE among them */
        { 0x0041, 0x0048 },     /* WM_COMPACTING .. WM_POWER             */
        { 0x0081, 0x0089 },     /* WM_NCCREATE .. WM_SYNCTASK            */
        { 0x00A0, 0x00A9 },     /* the non-client mouse                  */
        { 0x0100, 0x0108 },     /* the keyboard                          */
        { 0x0110, 0x0118 },     /* WM_INITDIALOG .. WM_SYSTIMER          */
        { 0x011F, 0x0121 },     /* WM_MENUSELECT .. WM_ENTERIDLE         */
        { 0x0131, 0x0138 },     /* WM_LBTRACKPOINT, and Win32's
                                   WM_CTLCOLOR split seven ways          */
        { 0x0200, 0x0209 },     /* the mouse                             */
        { 0x0210, 0x0212 },     /* WM_PARENTNOTIFY, the menu loop        */
        { 0x0220, 0x0234 },     /* MDI, drag and drop, size and move     */
        { 0x0300, 0x0311 },     /* the clipboard and the palette         */
        { 0x0380, 0x039F },     /* pen windows, coalescing               */
        { 0x03A0, 0x03DF },     /* MMSYSTEM: joystick, MCI, wave, MIDI   */
        { 0x03E0, 0x03E8 },     /* DDE                                   */
    };
    size_t i;

    if (msg >= WM_USER) return msg < 0xC000;
    /* A stock control's own, which is when it has a Win16 number. */
    if (msg32_to_16(msg) != msg) return 1;
    for (i = 0; i < sizeof known / sizeof known[0]; i++)
        if (msg >= known[i].lo && msg <= known[i].hi) return 1;
    return 0;
}

/* ---- the message currently being dispatched -------------------------------- */

/* When guest code forwards a message to DefWindowProc, or to the procedure it
   subclassed, the parameters it hands back are the 16-bit ones we gave it - and
   for anything carrying a pointer (WM_NCCREATE and WM_CREATE above all) those
   are segmented addresses that real USER32 cannot dereference, or a host
   pointer cut to 32 bits.  Converting back is not possible in general, but it
   is not necessary either when the guest passes on what it was given: the
   original 32-bit parameters are right here.  So keep a small stack of
   in-flight messages, with what the guest was handed for each, and msg16.c
   reuses the originals when that is what comes back.
   It is a stack rather than a single slot because a window procedure can be
   reentered while another message is still being dispatched. */
#define MAX_INFLIGHT 32
static struct inflight inflight[MAX_INFLIGHT];
static int inflight_depth;

static int inflight_push(const struct inflight *f)
{
    /* Returning whether it pushed matters: an unconditional pop after a refused
       push walks the top down into a live outer frame, and winproc_inflight
       would then hand DefWindowProc some other message's parameters. */
    if (inflight_depth >= MAX_INFLIGHT) return 0;
    inflight[inflight_depth++] = *f;
    return 1;
}

static void inflight_pop(void)
{
    if (inflight_depth > 0) inflight_depth--;
}

const struct inflight *winproc_inflight(HWND hwnd, UINT msg16)
{
    int i;
    for (i = inflight_depth - 1; i >= 0; i--)
        if (inflight[i].hwnd == hwnd && inflight[i].msg16 == msg16)
            return &inflight[i];
    return NULL;
}

/* ---- an EM_SETSEL on its way out -------------------------------------------- */

/* Win16's EM_SETSEL carries a flag that Win32's has no room for: 0 to scroll a
   multiline control's caret into view, 1 not to.  When the guest sends one to
   an edit it has subclassed, its own procedure has to be handed the flag it
   sent, so that whether the scroll happens is up to that procedure - pass the
   message on as it came and it scrolls, change the flag or keep the message
   and it does not - as it was in Win16, where the scroll was part of the
   edit's own EM_SETSEL.  The only way there is through USER32, so msg16.c's
   send, and DispatchMessage of the guest's own MSG, leave the flag here for as
   long as that takes, and msg_to_16 takes it for the first EM_SETSEL through
   with the same window and ends.  The queue cannot carry it: a posted one is
   handed the flag Win32's EM_SETSEL amounts to. */
static struct {
    HWND     hwnd;
    WPARAM   start;
    LPARAM   end;
    uint16_t flag;
} setsel_out;

void winproc_sending_setsel(HWND hwnd, WPARAM start, LPARAM end, uint16_t flag)
{
    setsel_out.hwnd  = hwnd;
    setsel_out.start = start;
    setsel_out.end   = end;
    setsel_out.flag  = flag;
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
 *
 * Four messages carrying a string are left to arrive that way, with a host
 * pointer for lParam, because nothing in the game reads one.  WM_SETTEXT and
 * WM_GETTEXT come whenever it sets or reads the text of a window of its own,
 * those calls going out through USER32, and WM_DEVMODECHANGE when a printer's
 * settings are changed; every window procedure in the game passes all three
 * to DefWindowProc or CallWindowProc, and every dialog procedure returns
 * FALSE.  WM_WININICHANGE - WM_SETTINGCHANGE, which Windows broadcasts for as
 * little as a switch between light and dark - the frame window does handle,
 * but as it handles WM_SYSCOLORCHANGE: it remakes its brushes, and never looks
 * at the section lParam names.
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

static void msg_to_16_values(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                             struct xlat *x);

static void msg_to_16(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                      uint16_t hinst, uint8_t *extra, struct xlat *x)
{
    uint8_t *p = extra;

    msg_to_16_values(hwnd, msg, wp, lp, x);

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

    case WM_COMPAREITEM: {
        /* COMPAREITEMSTRUCT16, 18 bytes: Win16 had no dwLocaleId.  Nothing
           flows back; the answer is the result. */
        const COMPAREITEMSTRUCT *ci = (const COMPAREITEMSTRUCT *)lp;
        *(uint16_t *)(p +  0) = (uint16_t)ci->CtlType;
        *(uint16_t *)(p +  2) = (uint16_t)ci->CtlID;
        *(uint16_t *)(p +  4) = HWND_16(ci->hwndItem);
        *(uint16_t *)(p +  6) = (uint16_t)ci->itemID1;
        *(uint32_t *)(p +  8) = (uint32_t)ci->itemData1;
        *(uint16_t *)(p + 12) = (uint16_t)ci->itemID2;
        *(uint32_t *)(p + 14) = (uint32_t)ci->itemData2;
        x->extralen = 18;
        break;
    }

    default:
        break;
    }
}

/* The rewrites that need nothing but the message's own three values: the
   renumbering, a handle's 16-bit number, the pairs whose parameters Win32
   packs differently.  They are all a posted message can need, as nothing
   posted carries a pointer - Win32 refuses one, and msg16_post sends what it
   cannot post - so they are what the guest's message loop does to the MSG it
   hands the guest, through msg16_pack.  msg16_unpack, in msg16.c, undoes
   each of them, for the MSG the guest hands back. */
static void msg_to_16_values(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                             struct xlat *x)
{
    x->msg16 = (UINT)msg32_to_16(msg);
    x->wp16 = (uint16_t)wp;
    x->lp16 = (uint32_t)lp;
    x->extralen = 0;
    x->back = BACK_NONE;
    x->ret_handle = H_NONE;

    switch (msg) {

    /* ---- messages that merely repack their two parameters ------------------ */

    case WM_COMMAND:
    case WM_ACTIVATE:
        /* Win16 puts the control handle in lParam's LOW word and the notify
           code in its HIGH word; Win32 puts the notify code in wParam's high
           word and the handle in lParam.  This is how every menu pick and
           every button click arrives.  WM_ACTIVATE has the same shape: the
           other window, the one losing activation or gaining it, where the
           control would be, and in the high word whether this one is
           minimized. */
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

    case LB_SETSEL:
        /* The index back into lParam's low word, where msg16.c found it and
           sign-extended it: MAKELPARAM(-1, 0) for every item, as Win16
           code writes it, not Win32's -1. */
        x->lp16 = LOWORD(lp);
        break;

    case EM_LINESCROLL:
        /* Both counts back into the one lParam, lines in the low word and
           columns in the high, which is where msg16.c found them. */
        x->wp16 = 0;
        x->lp16 = (uint32_t)MAKELONG(LOWORD(lp), LOWORD(wp));
        break;

    case EM_SETSEL:
        /* Both ends back into the one lParam, start in the low word and end
           in the high, as msg16.c found them; USER32's dialog manager sends
           this one itself, EM_SETSEL(0, -1), to an edit control tabbed into.
           Win16's wParam is a multiline control's scroll flag, 0 to scroll
           the caret into view and 1 not to; a single-line control ignores
           it (KB Q102641, for the 3.1 SDK).  Win32's EM_SETSEL never
           scrolls a multiline control, so one is told 1, which is what is
           about to happen, and a single-line one 0, which is what Win16 code
           passes.  Wine passes 0 to both, as its own EM_SETSEL scrolls
           either kind.  A control of another class gets it too, from the
           dialog manager, when it answers DLGC_HASSETSEL; its style's
           ES_MULTILINE bit means something else, and it is told 0.  When
           the guest sent this one or dispatched it itself, it gets the flag
           it used: see winproc_sending_setsel. */
        x->lp16 = (uint32_t)MAKELONG(LOWORD(wp), LOWORD(lp));
        if (setsel_out.hwnd == hwnd && setsel_out.start == wp &&
            setsel_out.end == lp) {
            x->wp16 = setsel_out.flag;
            setsel_out.hwnd = NULL;
            break;
        }
        x->wp16 = msg16_to_32(hwnd, x->msg16) == EM_SETSEL
               && (GetWindowLongA(hwnd, GWL_STYLE) & ES_MULTILINE);
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

void msg16_pack(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                uint16_t *msg16, uint16_t *wp16, uint32_t *lp16)
{
    struct xlat x;

    msg_to_16_values(hwnd, msg, wp, lp, &x);
    *msg16 = (uint16_t)x.msg16;
    *wp16  = x.wp16;
    *lp16  = x.lp16;
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
    struct inflight f;
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

    memcpy(before, extra, sizeof before);

    /* Record the ORIGINAL 32-bit parameters beside the translated ones: they
       are what a forward is restored from, and the translated pair would hand
       USER32 a 16-bit handle.  The translated pair is for telling whether the
       guest forwards what it was handed.  A struct's far pointer is made by
       call16_wndproc, on a stack we do not see from here, so onstack stands
       in for it. */
    f.hwnd    = hwnd;
    f.msg16   = x.msg16;
    f.wp16    = x.wp16;
    f.lp16    = x.lp16;
    f.onstack = x.extralen != 0;
    f.msg32   = msg;
    f.wp32    = wp;
    f.lp32    = lp;
    pushed = inflight_push(&f);
    r = call16_wndproc(proc16, hinst, args, sizeof args,
                       x.extralen ? extra : NULL, x.extralen);
    if (pushed) inflight_pop();

    /* Copy back only if the guest wrote something: itself, or by passing the
       message on, which rebuilds its copy from what DefWindowProc or the
       control made of the struct (winproc_refresh_struct).  A copy it left
       alone would only narrow the struct's values to 16 bits. */
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

/* ---- the wheel ------------------------------------------------------------ */

/* The wheel is two years younger than Win16, so no window here has a handler
   for one and none ever will.  Left at that, a turn of the wheel does nothing
   anywhere in the game; answered here, in terms of what the window under the
   pointer already knows how to do, it does what every other program does.
   There are only two shapes to answer:

     - a stock control the guest has subclassed, which is every list in the
       game.  The scrolling is in its class procedure, and DefWindowProc is not
       that procedure - answering there is what left the lists sitting still
       while an untouched combo box in the same dialog scrolled fine.

     - a window of the guest's own, which scrolls by acting on WM_VSCROLL or
       WM_HSCROLL.  Turning a notch into line messages is what the wheel
       drivers of the day did for Win16 programs, and it is all the game needs
       to be told.

   Sideways is either held shift, which is the convention every browser taught,
   or a tilt wheel saying so in its own message.  The scanner is the one that
   wanted it: zoomed in far enough it grows both scroll bars, and the game has
   never offered another way to pan. */

struct wheel_bar { HWND found; int horz; };

/* The scroll bar a guest window scrolls with.  Win16 code either puts one in
   the non-client area or spends a SCROLLBAR control on it; this game does both
   - the scanner and the panes' lists take the first, the report windows the
   second - and the same message drives either, distinguished by the control
   handle in lParam. */
static BOOL CALLBACK wheel_child_bar(HWND h, LPARAM lp)
{
    struct wheel_bar *want = (struct wheel_bar *)lp;
    char cls[16];
    int is_vert, lo, hi;

    if (!IsWindowVisible(h) || !IsWindowEnabled(h)) return TRUE;
    if (!GetClassNameA(h, cls, sizeof cls) || _stricmp(cls, "scrollbar"))
        return TRUE;
    is_vert = (GetWindowLongA(h, GWL_STYLE) & SBS_VERT) != 0;
    if (is_vert == want->horz) return TRUE;              /* the other axis */
    if (!GetScrollRange(h, SB_CTL, &lo, &hi) || lo >= hi) return TRUE;
    want->found = h;
    return FALSE;
}

LRESULT winproc_wheel(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    /* A wheel that reports finer than a notch - a trackpad - is worth nothing
       to a program that scrolls in whole lines, so the remainder is carried to
       the next message rather than thrown away.  One carry per axis. */
    static HWND last;
    static int  carry[2];

    WNDPROC cls_proc = (WNDPROC)(uintptr_t)GetClassLongPtrA(hwnd, GCLP_WNDPROC);
    struct wheel_bar want;
    UINT per_notch = 3, scroll;
    int delta, lo, hi, notches, code, count;

    if (cls_proc && cls_proc != winproc_bridge)
        return CallWindowProcA(cls_proc, hwnd, msg, wp, lp);

    want.found = NULL;
    want.horz = (msg == WM_MOUSEHWHEEL) ||
                (GET_KEYSTATE_WPARAM(wp) & MK_SHIFT) != 0;

    if (!GetScrollRange(hwnd, want.horz ? SB_HORZ : SB_VERT, &lo, &hi) ||
        lo >= hi) {
        EnumChildWindows(hwnd, wheel_child_bar, (LPARAM)&want);
        /* Nothing here scrolls that way.  DefWindowProc offers the wheel to the
           parent, which is how a pane gets a turn at one its child could not
           use. */
        if (!want.found) return DefWindowProcA(hwnd, msg, wp, lp);
    }

    /* A tilt wheel's delta runs the other way from a turned one: tilting right
       is a scroll right, where turning away is a scroll up. */
    delta = GET_WHEEL_DELTA_WPARAM(wp);
    if (msg == WM_MOUSEHWHEEL) delta = -delta;

    if (hwnd != last) { last = hwnd; carry[0] = carry[1] = 0; }
    carry[want.horz] += delta;
    notches = carry[want.horz] / WHEEL_DELTA;  /* toward zero, which is right */
    if (!notches) return 0;
    carry[want.horz] -= notches * WHEEL_DELTA;

    if (!SystemParametersInfoA(want.horz ? SPI_GETWHEELSCROLLCHARS
                                         : SPI_GETWHEELSCROLLLINES,
                               0, &per_notch, 0))
        per_notch = 3;

    /* SB_LINEUP and SB_LINELEFT are the same number, as are their opposites and
       the page pair: which axis is meant is carried by the message, not by the
       code, so one set serves both. */
    count = notches < 0 ? -notches : notches;
    if (per_notch == WHEEL_PAGESCROLL) {
        code = notches > 0 ? SB_PAGEUP : SB_PAGEDOWN;
    } else {
        code = notches > 0 ? SB_LINEUP : SB_LINEDOWN;
        count *= (int)per_notch;
        if (count > 64) count = 64;           /* a silly setting is still a cap */
    }

    scroll = want.horz ? WM_HSCROLL : WM_VSCROLL;
    while (count-- > 0)
        SendMessageA(hwnd, scroll, MAKEWPARAM(code, 0), (LPARAM)want.found);
    SendMessageA(hwnd, scroll, MAKEWPARAM(SB_ENDSCROLL, 0), (LPARAM)want.found);
    return 0;
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

    /* Nothing can run once a stop is latched, so let USER32 finish the teardown
       on its own rather than calling a guest that cannot execute. */
    if (cpu_stop_latched()) return DefWindowProcA(hwnd, msg, wp, lp);

    /* Win16 has no wheel, so the guest procedure is not offered one: it is
       answered on this side, where the delta is still 32 bits wide. */
    if (msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL)
        return winproc_wheel(hwnd, msg, wp, lp);

    /* Nor any other message Win16 never had.  All the guest could do with one
       is pass it down to its default, so the default gets it directly: the
       control's own procedure when the guest has subclassed a stock control,
       and otherwise DefWindowProc, which is where every class of the game's
       sends what it does not handle. */
    if (!msg_win16(msg)) {
        WNDPROC cls = (WNDPROC)(uintptr_t)GetClassLongPtrA(hwnd, GCLP_WNDPROC);
        if (cls && cls != winproc_bridge)
            return CallWindowProcA(cls, hwnd, msg, wp, lp);
        return DefWindowProcA(hwnd, msg, wp, lp);
    }

    /* No harness pump here.  Every window message is dispatched from the guest's
   own loop, which reaches GetMessage and pumps there, so pumping again inside
   the bridge only injects command execution into the middle of the guest's
   window setup - which is enough to wedge the tutorial's game creation.  The
   modal-dialog case, where the guest's loop is not running, is covered by
   dlgproc_bridge. */
    if (msg == WM_COMMAND)
        harness_event("cmd", (uintptr_t)hwnd, (long)LOWORD(wp), (long)HIWORD(wp));

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

/* A message whose lParam is a struct the guest was handed a 16-bit copy of
   goes on, when the guest passes it to DefWindowProc or to a control's
   procedure, with USER32's own 32-bit struct (msg16.c's pass_original).
   Before that call, widen into the struct whatever the guest wrote into its
   copy - PLANETWNDPROC and MESSAGEWNDPROC set ptMinTrackSize and then call
   DefWindowProc - as winproc_call16 does when the guest returns.  A copy that
   is still what the struct narrows to leaves it alone, so values wider than
   16 bits go on as they came.  `guest_lp` is the far pointer the guest passed
   on, which is exactly where its copy lives. */
void winproc_widen_struct(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                          uint32_t guest_lp)
{
    uint8_t now[64], copy[64];
    struct xlat x;

    if (!guest_lp) return;
    memset(now, 0, sizeof now);
    msg_to_16(hwnd, msg, wp, lp, 0, now, &x);
    if (x.back == BACK_NONE || !x.extralen) return;
    g_read(guest_lp, copy, x.extralen);
    if (memcmp(now, copy, x.extralen) != 0)
        msg_copy_back(&x, copy, msg, wp, lp);
}

/* After the call, DefWindowProc or the control has written its answer into the
   32-bit struct through the original pointer, and the guest is holding a
   16-bit copy that is now out of date.  Guest code routinely reads the copy
   after passing it on - "let the default size the client area, then take
   another few pixels off the top" is the standard shape of a WM_NCCALCSIZE
   handler - so rebuild the copy in place.  Left stale, the copy with the few
   pixels off would go over the answer when the guest returns: a client area
   covering the whole window less those pixels, with no room for its border,
   caption, menu bar or scroll bar. */
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
