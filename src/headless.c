/* headless.c - USER, GDI and the rest, with no window system underneath.
 *
 * The library's stand-in for api_user.c, api_gdi.c, dlg.c, audio.c,
 * api_misc.c and api_profile.c, which all put the game in front of Win32.
 * Here nothing is ever drawn and nobody is ever asked anything, yet the game
 * still has to be able to run: even its batch modes are GUI programs.  Turn
 * generation happens inside a WM_COMMAND the game posts to its own main
 * window, and the dumps build the whole main window, lists and all, before
 * they write a byte.
 *
 * So the window system here is real where the game can tell the difference
 * and a stub where it cannot:
 *
 *   - Windows exist: a table of them, with the class's window procedure,
 *     parent, id, style, rectangle, text and extra bytes, created with
 *     WM_NCCREATE, WM_CREATE, WM_SIZE and WM_MOVE and destroyed with
 *     WM_DESTROY, as Windows does.
 *   - Messages are real: SendMessage calls the window procedure, PostMessage
 *     queues, GetMessage and DispatchMessage deliver.  What never arrives is
 *     anything Windows would have made up by itself - painting, activation,
 *     the mouse, timers - because nothing here happens by itself.
 *   - A game that is left waiting - its queue empty and nothing posted - is
 *     told so: the call ends with STARS_EHUNG instead of polling a directory
 *     for a turn that is not coming.
 *   - The predefined controls keep just enough state to answer about what
 *     they were told: a list's count and selection, a button's check.
 *   - GDI objects are handles with a kind and a size, and drawing does
 *     nothing.  Text measures as if every character were half as wide as the
 *     font is high.
 *   - A message box is written to the log and answered with its most
 *     cautious button.  A dialog box ends the call as STARS_EGAME: nobody is
 *     there to fill it in, and none of these modes should raise one.  The one
 *     they could, the password prompt, is headed off by password.c.
 *   - Stars.ini is empty and stays empty.  What the game asks it gets the
 *     defaults for, with the same preset serial the emulator supplies.
 */

#include "lib.h"
#include "stars.h"
#include "thunk.h"
#include "task.h"
#include "sel.h"
#include "gmem.h"
#include "log.h"
#include "res.h"
#include "port.h"
#include "profile.h"

#include <stdio.h>
#include <string.h>

int headless_msgboxes;

/* ---- messages the window system itself sends ----------------------------- */

#define WM_CREATE         0x0001
#define WM_DESTROY        0x0002
#define WM_MOVE           0x0003
#define WM_SIZE           0x0005
#define WM_SETTEXT        0x000C
#define WM_GETTEXT        0x000D
#define WM_GETTEXTLENGTH  0x000E
#define WM_CLOSE          0x0010
#define WM_QUERYENDSESSION 0x0011
#define WM_QUIT           0x0012
#define WM_QUERYOPEN      0x0013
#define WM_ERASEBKGND     0x0014
#define WM_SHOWWINDOW     0x0018
#define WM_SETFONT        0x0030
#define WM_GETFONT        0x0031
#define WM_NCCREATE       0x0081
#define WM_NCDESTROY      0x0082
#define WM_NCHITTEST      0x0084
#define WM_SYSCOMMAND     0x0112
#define WM_TIMER          0x0113

/* ---- handles --------------------------------------------------------------- */

/* Windows and objects share one 16-bit handle space, in two ranges that
   cannot meet and that stay clear of the small integers Win16 lets stand for
   system colours and stock ids. */
#define MAX_WINS   256
#define WIN_BASE   0x6000u
#define MAX_OBJS   2048
#define OBJ_BASE   0x2000u
#define HANDLE_STEP 4u

/* ---- GDI and USER objects --------------------------------------------------- */

enum { OB_FREE, OB_PEN, OB_BRUSH, OB_FONT, OB_BITMAP, OB_RGN, OB_PALETTE,
       OB_DC, OB_MENU, OB_CURSOR, OB_ICON, OB_ACCEL };

typedef struct {
    uint8_t  kind;
    int16_t  w, h;           /* OB_BITMAP */
    uint8_t  bpp;            /* OB_BITMAP */
    int16_t  height;         /* OB_FONT: cell height, positive */
    int16_t  weight;         /* OB_FONT */
    uint16_t font;           /* OB_DC: the font selected into it */
    uint16_t pen, brush, bitmap, palette;   /* OB_DC */
    uint16_t sub[16];        /* OB_MENU: submenus, made on first request */
} Obj;

static Obj      objs[MAX_OBJS];
static uint16_t stock[20];

static Obj *obj_get(uint16_t h)
{
    unsigned i;
    if (h < OBJ_BASE || (h - OBJ_BASE) % HANDLE_STEP) return NULL;
    i = (h - OBJ_BASE) / HANDLE_STEP;
    return (i < MAX_OBJS && objs[i].kind != OB_FREE) ? &objs[i] : NULL;
}

static uint16_t obj_new(int kind)
{
    unsigned i;
    for (i = 0; i < MAX_OBJS; i++) {
        if (objs[i].kind == OB_FREE) {
            memset(&objs[i], 0, sizeof objs[i]);
            objs[i].kind = (uint8_t)kind;
            return (uint16_t)(OBJ_BASE + i * HANDLE_STEP);
        }
    }
    log_msg("headless: out of object handles\n");
    return 0;
}

static void obj_free(uint16_t h)
{
    Obj *o = obj_get(h);
    unsigned i;
    if (!o) return;
    for (i = 0; i < sizeof stock / sizeof *stock; i++)
        if (stock[i] == h) return;              /* stock objects stay */
    o->kind = OB_FREE;
}

/* GetStockObject's ids: 0-5 brushes, 6-8 pens, 10-17 fonts, 15 the palette. */
static uint16_t stock_object(unsigned n)
{
    int kind;
    if (n >= sizeof stock / sizeof *stock) return 0;
    if (stock[n]) return stock[n];
    kind = n <= 5 ? OB_BRUSH : n <= 8 ? OB_PEN : n == 15 ? OB_PALETTE : OB_FONT;
    stock[n] = obj_new(kind);
    if (kind == OB_FONT) {
        Obj *o = obj_get(stock[n]);
        o->height = 16;
        o->weight = 400;
    }
    return stock[n];
}

/* The font a DC measures text with: its own, or the system font. */
static const Obj *dc_font(uint16_t dc)
{
    Obj *o = obj_get(dc);
    const Obj *f = o && o->kind == OB_DC ? obj_get(o->font) : NULL;
    if (f && f->kind == OB_FONT) return f;
    return obj_get(stock_object(13));           /* SYSTEM_FONT */
}

static uint16_t dc_new(void)
{
    uint16_t h = obj_new(OB_DC);
    Obj *o = obj_get(h);
    if (o) {
        o->font    = stock_object(13);          /* SYSTEM_FONT */
        o->pen     = stock_object(7);           /* BLACK_PEN   */
        o->brush   = stock_object(0);           /* WHITE_BRUSH */
        o->palette = stock_object(15);          /* DEFAULT_PALETTE */
    }
    return h;
}

/* ---- window classes ------------------------------------------------------- */

#define MAX_CLASSES 32

typedef struct {
    char     name[64];
    uint32_t proc;
    uint16_t style, wndextra, hinst, menu_named;
} Class;

static Class classes[MAX_CLASSES];
static int   nclasses;

/* The predefined classes, which have no window procedure of the guest's. */
enum { SC_NONE, SC_BUTTON, SC_EDIT, SC_STATIC, SC_LISTBOX, SC_COMBOBOX,
       SC_SCROLLBAR };

static int sysclass_of(const char *name)
{
    static const char *const names[] = {
        "", "button", "edit", "static", "listbox", "combobox", "scrollbar"
    };
    int i;
    for (i = 1; i < (int)(sizeof names / sizeof *names); i++)
        if (!ascii_casecmp(name, names[i])) return i;
    return SC_NONE;
}

static Class *class_find(const char *name)
{
    int i;
    for (i = 0; i < nclasses; i++)
        if (!ascii_casecmp(classes[i].name, name)) return &classes[i];
    return NULL;
}

/* ---- windows ----------------------------------------------------------------- */

#define MAX_EXTRA 64
#define MAX_ITEMS 256

typedef struct {
    int      used;
    uint32_t proc;           /* the guest's, or defproc for a predefined class */
    int      sysclass;
    uint16_t parent, id, menu, hinst, font;
    uint32_t style, exstyle;
    int16_t  x, y, w, h;
    int      visible, enabled;
    char     text[128];
    uint8_t  extra[MAX_EXTRA];
    int      check;                           /* a button's */
    int      count, cursel;                   /* a list's   */
    uint32_t itemdata[MAX_ITEMS];
    int16_t  scroll[3][3];                    /* bar: pos, min, max */
} Win;

static Win      wins[MAX_WINS];
static uint16_t focus, capture, active;

/* The address that stands for "the predefined control's own procedure": the
   thunk that is DefWindowProc.  It is what GetWindowLong hands back for a
   control, so a game that subclasses one and chains to the old procedure -
   through CallWindowProc or with a plain far call - arrives in defwndproc. */
static uint32_t defproc;

static Win *win_get(uint16_t h)
{
    unsigned i;
    if (h < WIN_BASE || (h - WIN_BASE) % HANDLE_STEP) return NULL;
    i = (h - WIN_BASE) / HANDLE_STEP;
    return (i < MAX_WINS && wins[i].used) ? &wins[i] : NULL;
}

static uint16_t win_handle(const Win *w)
{
    return (uint16_t)(WIN_BASE + (unsigned)(w - wins) * HANDLE_STEP);
}

/* The window's top-left corner in screen coordinates. */
static void win_origin(uint16_t h, int *x, int *y)
{
    Win *w;
    *x = *y = 0;
    for (w = win_get(h); w; w = win_get(w->parent)) {
        *x += w->x;
        *y += w->y;
    }
}

static uint32_t defwndproc(uint16_t hwnd, uint16_t msg, uint16_t wp, uint32_t lp);

static uint32_t call_proc(uint32_t proc, uint16_t hwnd, uint16_t msg,
                          uint16_t wp, uint32_t lp, void *extra, unsigned extralen)
{
    uint16_t args[5];
    if (!proc || proc == defproc) return defwndproc(hwnd, msg, wp, lp);
    args[4] = hwnd;
    args[3] = msg;
    args[2] = wp;
    args[1] = (uint16_t)(lp >> 16);
    args[0] = (uint16_t)lp;
    return call16_wndproc(proc, 0, args, sizeof args, extra, extralen);
}

static uint32_t send(uint16_t hwnd, uint16_t msg, uint16_t wp, uint32_t lp)
{
    Win *w = win_get(hwnd);
    if (!w) return 0;
    return call_proc(w->proc, hwnd, msg, wp, lp, NULL, 0);
}

static void destroy(uint16_t hwnd)
{
    Win *w = win_get(hwnd);
    int i;
    if (!w) return;
    send(hwnd, WM_DESTROY, 0, 0);
    for (i = 0; i < MAX_WINS; i++)
        if (wins[i].used && wins[i].parent == hwnd) destroy(win_handle(&wins[i]));
    send(hwnd, WM_NCDESTROY, 0, 0);
    if ((w = win_get(hwnd)) != NULL) w->used = 0;
    if (focus == hwnd) focus = 0;
    if (capture == hwnd) capture = 0;
    if (active == hwnd) active = 0;
}

/* The move and size a Windows window would be told about, in the order
   Windows tells it. */
static void place(uint16_t hwnd, int x, int y, int cx, int cy, int move, int size)
{
    Win *w = win_get(hwnd);
    if (!w) return;
    move = move && (w->x != x || w->y != y);
    size = size && (w->w != cx || w->h != cy);
    if (move) { w->x = (int16_t)x; w->y = (int16_t)y; }
    if (size) { w->w = (int16_t)cx; w->h = (int16_t)cy; }
    if (size)
        send(hwnd, WM_SIZE, 0,
             (uint32_t)(uint16_t)cx | ((uint32_t)(uint16_t)cy << 16));
    if (move)
        send(hwnd, WM_MOVE, 0,
             (uint32_t)(uint16_t)x | ((uint32_t)(uint16_t)y << 16));
}

static int show(uint16_t hwnd, int visible)
{
    Win *w = win_get(hwnd);
    int was;
    if (!w) return 0;
    was = w->visible;
    if (!was != !visible) {
        w->visible = visible;
        send(hwnd, WM_SHOWWINDOW, (uint16_t)!!visible, 0);
        if (visible && !w->parent) active = hwnd;
    }
    return was;
}

/* ---- the predefined controls ----------------------------------------------- */

static uint32_t list_proc(Win *w, int combo, uint16_t msg, uint16_t wp, uint32_t lp)
{
    enum { ADD, INSERT, DELETE, RESET, SETSEL, GETSEL, COUNT, GETDATA, SETDATA };
    static const uint16_t lb[] = { 0x401, 0x402, 0x403, 0x405, 0x407, 0x409,
                                   0x40C, 0x41A, 0x41B };
    static const uint16_t cb[] = { 0x403, 0x40A, 0x404, 0x40B, 0x40E, 0x407,
                                   0x406, 0x410, 0x411 };
    const uint16_t *m = combo ? cb : lb;
    int i, op = -1;

    for (i = 0; i < 9; i++) if (m[i] == msg) op = i;
    switch (op) {
    case ADD:
        if (w->count >= MAX_ITEMS) return 0xFFFF;
        return (uint32_t)w->count++;
    case INSERT: {
        int at = (int16_t)wp < 0 || wp > w->count ? w->count : wp;
        if (w->count >= MAX_ITEMS) return 0xFFFF;
        memmove(&w->itemdata[at + 1], &w->itemdata[at],
                (size_t)(w->count - at) * sizeof *w->itemdata);
        w->itemdata[at] = 0;
        w->count++;
        return (uint32_t)at;
    }
    case DELETE:
        if (wp >= w->count) return 0xFFFF;
        memmove(&w->itemdata[wp], &w->itemdata[wp + 1],
                (size_t)(w->count - wp - 1) * sizeof *w->itemdata);
        w->count--;
        if (w->cursel >= w->count) w->cursel = -1;
        return (uint32_t)w->count;
    case RESET:
        w->count = 0;
        w->cursel = -1;
        memset(w->itemdata, 0, sizeof w->itemdata);
        return 0;
    case SETSEL:
        w->cursel = (int16_t)wp >= 0 && wp < w->count ? wp : -1;
        return w->cursel < 0 ? 0xFFFF : (uint32_t)w->cursel;
    case GETSEL:
        return w->cursel < 0 ? 0xFFFF : (uint32_t)w->cursel;
    case COUNT:
        return (uint32_t)w->count;
    case GETDATA:
        return wp < w->count ? w->itemdata[wp] : 0xFFFFFFFFu;
    case SETDATA:
        if (wp >= w->count) return 0xFFFF;
        w->itemdata[wp] = lp;
        return 0;
    }
    (void)lp;
    return 0;
}

/* DefWindowProc, and the whole of a predefined control's own procedure. */
static uint32_t defwndproc(uint16_t hwnd, uint16_t msg, uint16_t wp, uint32_t lp)
{
    Win *w = win_get(hwnd);

    if (!w) return 0;
    switch (msg) {
    case WM_NCCREATE:        return 1;
    case WM_ERASEBKGND:      return 1;
    case WM_QUERYENDSESSION: return 1;
    case WM_QUERYOPEN:       return 1;
    case WM_NCHITTEST:       return 1;                   /* HTCLIENT */
    case WM_CLOSE:           destroy(hwnd); return 0;
    case WM_SETTEXT:
        g_str(lp, w->text, sizeof w->text);
        return 1;
    case WM_GETTEXT:
        return g_puts(lp, w->text, wp);
    case WM_GETTEXTLENGTH:
        return (uint32_t)strlen(w->text);
    case WM_SETFONT:
        w->font = wp;
        return 0;
    case WM_GETFONT:
        return w->font;
    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == 0xF060) send(hwnd, WM_CLOSE, 0, 0);   /* SC_CLOSE */
        return 0;
    }
    if (msg >= 0x400 && msg < 0x500) {
        switch (w->sysclass) {
        case SC_BUTTON:
            if (msg == 0x400) return (uint32_t)w->check;          /* BM_GETCHECK */
            if (msg == 0x401) { w->check = wp; return 0; }        /* BM_SETCHECK */
            return 0;
        case SC_LISTBOX:  return list_proc(w, 0, msg, wp, lp);
        case SC_COMBOBOX: return list_proc(w, 1, msg, wp, lp);
        }
    }
    return 0;
}

/* ---- window API ---------------------------------------------------------------- */

static uint32_t u_InitApp(Cpu *c, Args *a)
{
    (void)c; (void)a;
    return 1;
}

static uint32_t u_RegisterClass(Cpu *c, Args *a)
{
    uint32_t p = arg_long(a);
    uint16_t sel = SEGPTR_SEL(p), off = SEGPTR_OFF(p);
    Class *k;
    char name[64];

    (void)c;
    g_str(sel_rd32(sel, (uint16_t)(off + 22)), name, sizeof name);
    if (class_find(name)) return 0;
    if (nclasses == MAX_CLASSES) {
        log_msg("headless: too many window classes (%s)\n", name);
        return 0;
    }
    k = &classes[nclasses++];
    memset(k, 0, sizeof *k);
    snprintf(k->name, sizeof k->name, "%s", name);
    k->style      = sel_rd16(sel, off);
    k->proc       = sel_rd32(sel, (uint16_t)(off + 2));
    k->wndextra   = sel_rd16(sel, (uint16_t)(off + 8));
    k->hinst      = sel_rd16(sel, (uint16_t)(off + 10));
    k->menu_named = sel_rd32(sel, (uint16_t)(off + 18)) != 0;
    if (k->wndextra > MAX_EXTRA)
        log_msg("headless: class %s wants %u extra bytes, keeping %u\n",
                name, k->wndextra, MAX_EXTRA);
    return 0xC000u + (unsigned)nclasses;        /* an atom */
}

static uint32_t u_CreateWindow(Cpu *c, Args *a)
{
    uint32_t clsp   = arg_long(a);
    uint32_t namep  = arg_long(a);
    uint32_t style  = arg_long(a);
    int16_t  x = arg_sword(a), y = arg_sword(a);
    int16_t  cx = arg_sword(a), cy = arg_sword(a);
    uint16_t parent = arg_word(a);
    uint16_t menu   = arg_word(a);
    uint16_t hinst  = arg_word(a);
    uint32_t param  = arg_long(a);
    char cls[64];
    uint8_t cs[34];
    Class *k;
    Win *w;
    uint16_t hwnd;
    int i;

    (void)c;
    g_str(clsp, cls, sizeof cls);
    for (i = 0; i < MAX_WINS; i++) if (!wins[i].used) break;
    if (i == MAX_WINS) {
        log_msg("headless: out of window handles creating a %s\n", cls);
        return 0;
    }
    w = &wins[i];
    memset(w, 0, sizeof *w);
    w->used = 1;
    w->cursel = -1;
    w->enabled = 1;
    w->parent = parent;
    w->style = style;
    w->hinst = hinst;
    /* CW_USEDEFAULT, which only means something for a top-level window. */
    if (x == (int16_t)0x8000) { x = 0; y = 0; }
    if (cx == (int16_t)0x8000) { cx = 640; cy = 480; }
    w->x = x; w->y = y; w->w = cx; w->h = cy;
    if (style & 0x40000000u) w->id = menu;              /* WS_CHILD */
    else w->menu = menu;
    g_str(namep, w->text, sizeof w->text);
    if (!defproc) defproc = thunk_resolve("USER", 107, NULL);

    k = class_find(cls);
    if (k) {
        w->proc = k->proc;
        if (!w->menu && k->menu_named) w->menu = obj_new(OB_MENU);
    } else if ((w->sysclass = sysclass_of(cls)) != SC_NONE) {
        w->proc = defproc;
    } else {
        log_msg("headless: CreateWindow of unknown class %s\n", cls);
        w->used = 0;
        return 0;
    }
    hwnd = win_handle(w);

    /* CREATESTRUCT16: lpCreateParams, hInstance, hMenu, hwndParent, cy, cx,
       y, x, style, lpszName, lpszClass, dwExStyle. */
    memset(cs, 0, sizeof cs);
    cs[0] = (uint8_t)param;         cs[1] = (uint8_t)(param >> 8);
    cs[2] = (uint8_t)(param >> 16); cs[3] = (uint8_t)(param >> 24);
    cs[4] = (uint8_t)hinst;         cs[5] = (uint8_t)(hinst >> 8);
    cs[6] = (uint8_t)menu;          cs[7] = (uint8_t)(menu >> 8);
    cs[8] = (uint8_t)parent;        cs[9] = (uint8_t)(parent >> 8);
    cs[10] = (uint8_t)cy;           cs[11] = (uint8_t)((uint16_t)cy >> 8);
    cs[12] = (uint8_t)cx;           cs[13] = (uint8_t)((uint16_t)cx >> 8);
    cs[14] = (uint8_t)y;            cs[15] = (uint8_t)((uint16_t)y >> 8);
    cs[16] = (uint8_t)x;            cs[17] = (uint8_t)((uint16_t)x >> 8);
    cs[18] = (uint8_t)style;        cs[19] = (uint8_t)(style >> 8);
    cs[20] = (uint8_t)(style >> 16); cs[21] = (uint8_t)(style >> 24);
    {
        uint8_t copy[sizeof cs];
        memcpy(copy, cs, sizeof cs);
        if (!call_proc(w->proc, hwnd, WM_NCCREATE, 0, 0, copy, sizeof copy)) {
            if ((w = win_get(hwnd)) != NULL) w->used = 0;
            return 0;
        }
        memcpy(copy, cs, sizeof cs);
        if ((w = win_get(hwnd)) == NULL) return 0;
        if ((int16_t)call_proc(w->proc, hwnd, WM_CREATE, 0, 0, copy,
                               sizeof copy) == -1) {
            destroy(hwnd);
            return 0;
        }
    }
    if (!win_get(hwnd)) return 0;
    send(hwnd, WM_SIZE, 0, (uint32_t)(uint16_t)cx | ((uint32_t)(uint16_t)cy << 16));
    send(hwnd, WM_MOVE, 0, (uint32_t)(uint16_t)x | ((uint32_t)(uint16_t)y << 16));
    if (style & 0x10000000u) show(hwnd, 1);             /* WS_VISIBLE */
    return win_get(hwnd) ? hwnd : 0;
}

static uint32_t u_DestroyWindow(Cpu *c, Args *a)
{
    uint16_t hwnd = arg_word(a);
    (void)c;
    if (!win_get(hwnd)) return 0;
    destroy(hwnd);
    return 1;
}

static uint32_t u_ShowWindow(Cpu *c, Args *a)
{
    uint16_t hwnd = arg_word(a);
    uint16_t cmd  = arg_word(a);
    (void)c;
    return (uint32_t)show(hwnd, cmd != 0);              /* SW_HIDE is 0 */
}

static uint32_t u_MoveWindow(Cpu *c, Args *a)
{
    uint16_t hwnd = arg_word(a);
    int16_t x = arg_sword(a), y = arg_sword(a), cx = arg_sword(a), cy = arg_sword(a);
    (void)c;
    place(hwnd, x, y, cx, cy, 1, 1);
    return 1;
}

static uint32_t u_SetWindowPos(Cpu *c, Args *a)
{
    uint16_t hwnd = arg_word(a);
    uint16_t after = arg_word(a);
    int16_t x = arg_sword(a), y = arg_sword(a), cx = arg_sword(a), cy = arg_sword(a);
    uint16_t flags = arg_word(a);
    (void)c; (void)after;
    place(hwnd, x, y, cx, cy, !(flags & 0x0002), !(flags & 0x0001));
    if (flags & 0x0040) show(hwnd, 1);                  /* SWP_SHOWWINDOW */
    if (flags & 0x0080) show(hwnd, 0);                  /* SWP_HIDEWINDOW */
    return 1;
}

static void rect_out(uint32_t p, int l, int t, int r, int b)
{
    uint16_t sel = SEGPTR_SEL(p), off = SEGPTR_OFF(p);
    if (!p) return;
    sel_wr16(sel, off, (uint16_t)l);
    sel_wr16(sel, (uint16_t)(off + 2), (uint16_t)t);
    sel_wr16(sel, (uint16_t)(off + 4), (uint16_t)r);
    sel_wr16(sel, (uint16_t)(off + 6), (uint16_t)b);
}

static uint32_t u_GetClientRect(Cpu *c, Args *a)
{
    uint16_t hwnd = arg_word(a);
    uint32_t p = arg_long(a);
    Win *w = win_get(hwnd);
    (void)c;
    rect_out(p, 0, 0, w ? w->w : 0, w ? w->h : 0);
    return 0;
}

static uint32_t u_GetWindowRect(Cpu *c, Args *a)
{
    uint16_t hwnd = arg_word(a);
    uint32_t p = arg_long(a);
    Win *w = win_get(hwnd);
    int x, y;
    (void)c;
    win_origin(hwnd, &x, &y);
    rect_out(p, x, y, x + (w ? w->w : 0), y + (w ? w->h : 0));
    return 0;
}

static uint32_t u_GetWindowPlacement(Cpu *c, Args *a)
{
    uint16_t hwnd = arg_word(a);
    uint32_t p = arg_long(a);
    Win *w = win_get(hwnd);
    uint8_t wp[22];
    (void)c;
    if (!w || !p) return 0;
    /* WINDOWPLACEMENT16: length, flags, showCmd, ptMinPosition,
       ptMaxPosition, rcNormalPosition. */
    memset(wp, 0, sizeof wp);
    wp[0] = sizeof wp;
    wp[4] = w->visible ? 1 : 0;
    g_write(p, wp, sizeof wp);
    rect_out(p + 14, w->x, w->y, w->x + w->w, w->y + w->h);
    return 1;
}

static uint32_t u_IsIconic(Cpu *c, Args *a)        { (void)c; (void)a; return 0; }
static uint32_t u_IsZoomed(Cpu *c, Args *a)        { (void)c; (void)a; return 0; }

static uint32_t u_IsWindowVisible(Cpu *c, Args *a)
{
    Win *w = win_get(arg_word(a));
    (void)c;
    return w && w->visible;
}

static uint32_t u_EnableWindow(Cpu *c, Args *a)
{
    Win *w = win_get(arg_word(a));
    uint16_t on = arg_word(a);
    int was;
    (void)c;
    if (!w) return 0;
    was = !w->enabled;
    w->enabled = on != 0;
    return (uint32_t)was;                       /* nonzero: was disabled */
}

static uint32_t u_GetParent(Cpu *c, Args *a)
{
    Win *w = win_get(arg_word(a));
    (void)c;
    return w ? w->parent : 0;
}

static uint32_t u_GetWindow(Cpu *c, Args *a)
{
    uint16_t hwnd = arg_word(a);
    uint16_t cmd = arg_word(a);
    Win *w = win_get(hwnd);
    int i, start;
    uint16_t parent;

    (void)c;
    if (!w) return 0;
    switch (cmd) {
    case 4:                                             /* GW_OWNER */
        return 0;
    case 5:                                             /* GW_CHILD */
        for (i = 0; i < MAX_WINS; i++)
            if (wins[i].used && wins[i].parent == hwnd) return win_handle(&wins[i]);
        return 0;
    case 2:                                             /* GW_HWNDNEXT */
        parent = w->parent;
        start = (int)(w - wins) + 1;
        for (i = start; i < MAX_WINS; i++)
            if (wins[i].used && wins[i].parent == parent) return win_handle(&wins[i]);
        return 0;
    case 3:                                             /* GW_HWNDPREV */
        parent = w->parent;
        for (i = (int)(w - wins) - 1; i >= 0; i--)
            if (wins[i].used && wins[i].parent == parent) return win_handle(&wins[i]);
        return 0;
    case 0: case 1: {                                   /* first, last */
        uint16_t found = 0;
        parent = w->parent;
        for (i = 0; i < MAX_WINS; i++)
            if (wins[i].used && wins[i].parent == parent) {
                found = win_handle(&wins[i]);
                if (cmd == 0) break;
            }
        return found;
    }
    }
    return 0;
}

static uint32_t u_GetWindowText(Cpu *c, Args *a)
{
    uint16_t hwnd = arg_word(a);
    uint32_t p = arg_long(a);
    uint16_t max = arg_word(a);
    (void)c;
    return send(hwnd, WM_GETTEXT, max, p);
}

static uint32_t u_SetWindowText(Cpu *c, Args *a)
{
    uint16_t hwnd = arg_word(a);
    uint32_t p = arg_long(a);
    (void)c;
    send(hwnd, WM_SETTEXT, 0, p);
    return 0;
}

/* Window words: offsets from 0 are the class's extra bytes, and negative ones
   are the window's own fields. */
static uint32_t u_GetWindowLong(Cpu *c, Args *a)
{
    Win *w = win_get(arg_word(a));
    int16_t i = arg_sword(a);
    (void)c;
    if (!w) return 0;
    switch (i) {
    case -4:  return w->proc;                           /* GWL_WNDPROC   */
    case -6:  return w->hinst;                          /* GWW_HINSTANCE */
    case -8:  return w->parent;                         /* GWW_HWNDPARENT */
    case -12: return w->id;                             /* GWW_ID        */
    case -16: return w->style;                          /* GWL_STYLE     */
    case -20: return w->exstyle;                        /* GWL_EXSTYLE   */
    }
    if (i >= 0 && i + 4 <= MAX_EXTRA)
        return (uint32_t)w->extra[i] | ((uint32_t)w->extra[i + 1] << 8) |
               ((uint32_t)w->extra[i + 2] << 16) | ((uint32_t)w->extra[i + 3] << 24);
    return 0;
}

static uint32_t u_SetWindowLong(Cpu *c, Args *a)
{
    Win *w = win_get(arg_word(a));
    int16_t i = arg_sword(a);
    uint32_t v = arg_long(a), old;
    (void)c;
    if (!w) return 0;
    switch (i) {
    case -4:  old = w->proc;    w->proc = v;              return old;
    case -16: old = w->style;   w->style = v;             return old;
    case -20: old = w->exstyle; w->exstyle = v;           return old;
    case -12: old = w->id;      w->id = (uint16_t)v;      return old;
    }
    if (i >= 0 && i + 4 <= MAX_EXTRA) {
        old = (uint32_t)w->extra[i] | ((uint32_t)w->extra[i + 1] << 8) |
              ((uint32_t)w->extra[i + 2] << 16) | ((uint32_t)w->extra[i + 3] << 24);
        w->extra[i]     = (uint8_t)v;
        w->extra[i + 1] = (uint8_t)(v >> 8);
        w->extra[i + 2] = (uint8_t)(v >> 16);
        w->extra[i + 3] = (uint8_t)(v >> 24);
        return old;
    }
    return 0;
}

static uint32_t u_SetFocus(Cpu *c, Args *a)
{
    uint16_t hwnd = arg_word(a), old = focus;
    (void)c;
    focus = hwnd;
    return old;
}

static uint32_t u_GetFocus(Cpu *c, Args *a)        { (void)c; (void)a; return focus; }
static uint32_t u_GetActiveWindow(Cpu *c, Args *a) { (void)c; (void)a; return active; }

static uint32_t u_SetCapture(Cpu *c, Args *a)
{
    uint16_t hwnd = arg_word(a), old = capture;
    (void)c;
    capture = hwnd;
    return old;
}

static uint32_t u_ReleaseCapture(Cpu *c, Args *a)
{
    (void)c; (void)a;
    capture = 0;
    return 0;
}

static uint32_t u_GetCursorPos(Cpu *c, Args *a)
{
    uint32_t p = arg_long(a);
    (void)c;
    if (p) { sel_wr16(SEGPTR_SEL(p), SEGPTR_OFF(p), 0);
             sel_wr16(SEGPTR_SEL(p), (uint16_t)(SEGPTR_OFF(p) + 2), 0); }
    return 0;
}

static void point_shift(uint32_t p, int dx, int dy)
{
    uint16_t sel = SEGPTR_SEL(p), off = SEGPTR_OFF(p);
    if (!p) return;
    sel_wr16(sel, off, (uint16_t)(sel_rd16(sel, off) + dx));
    sel_wr16(sel, (uint16_t)(off + 2), (uint16_t)(sel_rd16(sel, (uint16_t)(off + 2)) + dy));
}

static uint32_t u_ClientToScreen(Cpu *c, Args *a)
{
    uint16_t hwnd = arg_word(a);
    uint32_t p = arg_long(a);
    int x, y;
    (void)c;
    win_origin(hwnd, &x, &y);
    point_shift(p, x, y);
    return 0;
}

static uint32_t u_ScreenToClient(Cpu *c, Args *a)
{
    uint16_t hwnd = arg_word(a);
    uint32_t p = arg_long(a);
    int x, y;
    (void)c;
    win_origin(hwnd, &x, &y);
    point_shift(p, -x, -y);
    return 0;
}

static uint32_t u_MapWindowPoints(Cpu *c, Args *a)
{
    uint16_t from = arg_word(a), to = arg_word(a);
    uint32_t p = arg_long(a);
    uint16_t n = arg_word(a), i;
    int fx, fy, tx, ty;
    (void)c;
    win_origin(from, &fx, &fy);
    win_origin(to, &tx, &ty);
    for (i = 0; i < n; i++) point_shift(p + i * 4u, fx - tx, fy - ty);
    return 0;
}

static uint32_t u_WindowFromPoint(Cpu *c, Args *a) { (void)c; (void)a; return 0; }

/* Scroll bars: SB_HORZ, SB_VERT, SB_CTL. */
static uint32_t u_SetScrollPos(Cpu *c, Args *a)
{
    Win *w = win_get(arg_word(a));
    uint16_t bar = arg_word(a);
    int16_t pos = arg_sword(a), old;
    (void)c;
    if (!w || bar > 2) return 0;
    old = w->scroll[bar][0];
    w->scroll[bar][0] = pos;
    return (uint16_t)old;
}

static uint32_t u_GetScrollPos(Cpu *c, Args *a)
{
    Win *w = win_get(arg_word(a));
    uint16_t bar = arg_word(a);
    (void)c;
    return (w && bar <= 2) ? (uint16_t)w->scroll[bar][0] : 0;
}

static uint32_t u_SetScrollRange(Cpu *c, Args *a)
{
    Win *w = win_get(arg_word(a));
    uint16_t bar = arg_word(a);
    int16_t lo = arg_sword(a), hi = arg_sword(a);
    (void)c;
    if (w && bar <= 2) { w->scroll[bar][1] = lo; w->scroll[bar][2] = hi; }
    return 0;
}

static uint32_t u_ScrollWindow(Cpu *c, Args *a) { (void)c; (void)a; return 0; }

static uint32_t u_DefWindowProc(Cpu *c, Args *a)
{
    uint16_t hwnd = arg_word(a), msg = arg_word(a), wp = arg_word(a);
    uint32_t lp = arg_long(a);
    (void)c;
    return defwndproc(hwnd, msg, wp, lp);
}

static uint32_t u_CallWindowProc(Cpu *c, Args *a)
{
    uint32_t proc = arg_long(a);
    uint16_t hwnd = arg_word(a), msg = arg_word(a), wp = arg_word(a);
    uint32_t lp = arg_long(a);
    (void)c;
    return call_proc(proc, hwnd, msg, wp, lp, NULL, 0);
}

static uint32_t u_SendMessage(Cpu *c, Args *a)
{
    uint16_t hwnd = arg_word(a), msg = arg_word(a), wp = arg_word(a);
    uint32_t lp = arg_long(a);
    (void)c;
    return send(hwnd, msg, wp, lp);
}

/* ---- the queue ------------------------------------------------------------------ */

#define QLEN 256

static struct { uint16_t hwnd, msg, wp; uint32_t lp; } queue[QLEN];
static unsigned qhead, qcount;
static int      quit_posted;
static uint16_t quit_code;

static uint32_t u_PostMessage(Cpu *c, Args *a)
{
    uint16_t hwnd = arg_word(a), msg = arg_word(a), wp = arg_word(a);
    uint32_t lp = arg_long(a);
    unsigned at;
    (void)c;
    if (qcount == QLEN) {
        log_msg("headless: message queue full, dropping %04X\n", msg);
        return 0;
    }
    at = (qhead + qcount++) % QLEN;
    queue[at].hwnd = hwnd;
    queue[at].msg = msg;
    queue[at].wp = wp;
    queue[at].lp = lp;
    return 1;
}

static uint32_t u_PostQuitMessage(Cpu *c, Args *a)
{
    (void)c;
    quit_code = arg_word(a);
    quit_posted = 1;
    return 0;
}

/* MSG16: hwnd, message, wParam, lParam, time, pt.  The time is zero rather
   than a reading of the clock, which would move it. */
static void msg_out(uint32_t p, uint16_t hwnd, uint16_t msg, uint16_t wp, uint32_t lp)
{
    uint16_t sel = SEGPTR_SEL(p), off = SEGPTR_OFF(p);
    sel_wr16(sel, off, hwnd);
    sel_wr16(sel, (uint16_t)(off + 2), msg);
    sel_wr16(sel, (uint16_t)(off + 4), wp);
    sel_wr32(sel, (uint16_t)(off + 6), lp);
    sel_wr32(sel, (uint16_t)(off + 10), 0);
    sel_wr32(sel, (uint16_t)(off + 14), 0);
}

/* The front of the queue into *p: 1 for a message, 0 for WM_QUIT, -1 for
   nothing at all. */
static int next_msg(uint32_t p, int remove)
{
    if (qcount) {
        msg_out(p, queue[qhead].hwnd, queue[qhead].msg, queue[qhead].wp,
                queue[qhead].lp);
        if (remove) { qhead = (qhead + 1) % QLEN; qcount--; }
        return 1;
    }
    if (quit_posted) {
        msg_out(p, 0, WM_QUIT, quit_code, 0);
        if (remove) quit_posted = 0;
        return 0;
    }
    return -1;
}

static uint32_t u_GetMessage(Cpu *c, Args *a)
{
    uint32_t p = arg_long(a);
    int r;
    (void)c;
    if ((r = next_msg(p, 1)) >= 0) return (uint32_t)r;
    /* Under Windows this is where the game would sleep until something
       happened, and here nothing is going to. */
    log_msg("headless: the game is waiting for input that will never come\n");
    lib_fail(STARS_EHUNG);
    return 0;
}

static uint32_t u_PeekMessage(Cpu *c, Args *a)
{
    uint32_t p = arg_long(a);
    uint16_t hwnd = arg_word(a), lo = arg_word(a), hi = arg_word(a);
    uint16_t flags = arg_word(a);
    (void)c; (void)hwnd; (void)lo; (void)hi;
    return next_msg(p, flags & 1) >= 0;                 /* PM_REMOVE */
}

static uint32_t u_TranslateMessage(Cpu *c, Args *a)     { (void)c; (void)a; return 0; }
static uint32_t u_TranslateAccelerator(Cpu *c, Args *a) { (void)c; (void)a; return 0; }

static uint32_t u_DispatchMessage(Cpu *c, Args *a)
{
    uint32_t p = arg_long(a);
    uint16_t sel = SEGPTR_SEL(p), off = SEGPTR_OFF(p);
    uint16_t hwnd = sel_rd16(sel, off);
    uint16_t msg  = sel_rd16(sel, (uint16_t)(off + 2));
    uint16_t wp   = sel_rd16(sel, (uint16_t)(off + 4));
    uint32_t lp   = sel_rd32(sel, (uint16_t)(off + 6));
    (void)c;
    if (msg == WM_TIMER && lp) return call_proc(lp, hwnd, msg, wp, 0, NULL, 0);
    return send(hwnd, msg, wp, lp);
}

/* Timers are accepted and never fire: nothing here waits. */
static uint32_t u_SetTimer(Cpu *c, Args *a)
{
    uint16_t hwnd = arg_word(a), id = arg_word(a);
    (void)c; (void)hwnd;
    return id ? id : 1;
}

static uint32_t u_KillTimer(Cpu *c, Args *a) { (void)c; (void)a; return 1; }

/* ---- asking the user ------------------------------------------------------------ */

static uint32_t u_MessageBox(Cpu *c, Args *a)
{
    uint16_t hwnd = arg_word(a);
    uint32_t textp = arg_long(a), capp = arg_long(a);
    uint16_t type = arg_word(a);
    char text[512], cap[128];
    static const uint16_t answer[] = {
        1,          /* MB_OK: IDOK                         */
        2,          /* MB_OKCANCEL: IDCANCEL               */
        3,          /* MB_ABORTRETRYIGNORE: IDABORT        */
        2,          /* MB_YESNOCANCEL: IDCANCEL            */
        7,          /* MB_YESNO: IDNO                      */
        2,          /* MB_RETRYCANCEL: IDCANCEL            */
    };
    (void)c; (void)hwnd;
    g_str(textp, text, sizeof text);
    g_str(capp, cap, sizeof cap);
    log_msg("MessageBox: \"%s\" / \"%s\"\n", cap, text);
    headless_msgboxes++;
    return (type & 0xF) < sizeof answer / sizeof *answer ? answer[type & 0xF] : 1;
}

static uint32_t u_DialogBox(Cpu *c, Args *a)
{
    uint16_t hinst = arg_word(a);
    uint32_t tmpl = arg_long(a);
    char name[64];
    (void)c; (void)hinst;
    if (SEGPTR_SEL(tmpl) == 0 && SEGPTR_OFF(tmpl) == 140) {
        log_msg("The game asked for a password.\n");
        lib_fail(STARS_EGAME);
    }
    if (SEGPTR_SEL(tmpl) == 0) snprintf(name, sizeof name, "#%u", SEGPTR_OFF(tmpl));
    else g_str(tmpl, name, sizeof name);
    log_msg("The game opened dialog %s, which has nobody to answer it.\n", name);
    lib_fail(STARS_EGAME);
    return 0;
}

static uint32_t u_CreateDialog(Cpu *c, Args *a)
{
    return u_DialogBox(c, a);
}

static uint32_t u_EndDialog(Cpu *c, Args *a) { (void)c; (void)a; return 1; }

static uint32_t u_GetDlgItem(Cpu *c, Args *a)
{
    uint16_t hwnd = arg_word(a), id = arg_word(a);
    int i;
    (void)c;
    for (i = 0; i < MAX_WINS; i++)
        if (wins[i].used && wins[i].parent == hwnd && wins[i].id == id)
            return win_handle(&wins[i]);
    return 0;
}

static uint16_t dlg_item(uint16_t hwnd, uint16_t id)
{
    int i;
    for (i = 0; i < MAX_WINS; i++)
        if (wins[i].used && wins[i].parent == hwnd && wins[i].id == id)
            return win_handle(&wins[i]);
    return 0;
}

static uint32_t u_SetDlgItemText(Cpu *c, Args *a)
{
    uint16_t hwnd = arg_word(a), id = arg_word(a);
    uint32_t p = arg_long(a);
    (void)c;
    send(dlg_item(hwnd, id), WM_SETTEXT, 0, p);
    return 0;
}

static uint32_t u_GetDlgItemText(Cpu *c, Args *a)
{
    uint16_t hwnd = arg_word(a), id = arg_word(a);
    uint32_t p = arg_long(a);
    uint16_t max = arg_word(a);
    (void)c;
    return send(dlg_item(hwnd, id), WM_GETTEXT, max, p);
}

static uint32_t u_CheckDlgButton(Cpu *c, Args *a)
{
    uint16_t hwnd = arg_word(a), id = arg_word(a), check = arg_word(a);
    (void)c;
    send(dlg_item(hwnd, id), 0x401, check, 0);         /* BM_SETCHECK */
    return 0;
}

static uint32_t u_IsDlgButtonChecked(Cpu *c, Args *a)
{
    uint16_t hwnd = arg_word(a), id = arg_word(a);
    (void)c;
    return send(dlg_item(hwnd, id), 0x400, 0, 0);      /* BM_GETCHECK */
}

static uint32_t u_CheckRadioButton(Cpu *c, Args *a)
{
    uint16_t hwnd = arg_word(a), first = arg_word(a), last = arg_word(a);
    uint16_t pick = arg_word(a), id;
    (void)c;
    for (id = first; id <= last && id >= first; id++)
        send(dlg_item(hwnd, id), 0x401, id == pick, 0);
    return 0;
}

static uint32_t u_SendDlgItemMessage(Cpu *c, Args *a)
{
    uint16_t hwnd = arg_word(a), id = arg_word(a), msg = arg_word(a);
    uint16_t wp = arg_word(a);
    uint32_t lp = arg_long(a);
    (void)c;
    return send(dlg_item(hwnd, id), msg, wp, lp);
}

static uint32_t u_MessageBeep(Cpu *c, Args *a)  { (void)c; (void)a; return 1; }
static uint32_t u_FlashWindow(Cpu *c, Args *a)  { (void)c; (void)a; return 0; }
static uint32_t u_WinHelp(Cpu *c, Args *a)      { (void)c; (void)a; return 1; }
static uint32_t u_GetKeyState(Cpu *c, Args *a)  { (void)c; (void)a; return 0; }

/* -x: the game asks Windows to shut down, which here is the game ending. */
static uint32_t u_ExitWindows(Cpu *c, Args *a)
{
    (void)a;
    c->state = CPU_HALT;
    return 0;
}

/* ---- menus ------------------------------------------------------------------------ */

static uint32_t u_GetMenu(Cpu *c, Args *a)
{
    Win *w = win_get(arg_word(a));
    (void)c;
    return w ? w->menu : 0;
}

static uint32_t u_GetSubMenu(Cpu *c, Args *a)
{
    uint16_t menu = arg_word(a);
    int16_t pos = arg_sword(a);
    Obj *o = obj_get(menu);
    (void)c;
    if (!o || o->kind != OB_MENU || pos < 0 || pos >= 16) return 0;
    if (!o->sub[pos]) {
        uint16_t h = obj_new(OB_MENU);
        if ((o = obj_get(menu)) != NULL) o->sub[pos] = h;
        return h;
    }
    return o->sub[pos];
}

static uint32_t u_CreatePopupMenu(Cpu *c, Args *a)
{
    (void)c; (void)a;
    return obj_new(OB_MENU);
}

static uint32_t u_DestroyMenu(Cpu *c, Args *a)
{
    (void)c;
    obj_free(arg_word(a));
    return 1;
}

static uint32_t u_MenuItemCount(Cpu *c, Args *a)   { (void)c; (void)a; return 0; }
static uint32_t u_MenuState(Cpu *c, Args *a)       { (void)c; (void)a; return 0; }
static uint32_t u_MenuOk(Cpu *c, Args *a)          { (void)c; (void)a; return 1; }
static uint32_t u_TrackPopupMenu(Cpu *c, Args *a)  { (void)c; (void)a; return 0; }

/* ---- cursors, icons, bitmaps, accelerators ---------------------------------------- */

static uint32_t u_LoadCursor(Cpu *c, Args *a) { (void)c; (void)a; return obj_new(OB_CURSOR); }
static uint32_t u_LoadIcon(Cpu *c, Args *a)   { (void)c; (void)a; return obj_new(OB_ICON); }

static uint32_t u_DestroyObject(Cpu *c, Args *a)
{
    (void)c;
    obj_free(arg_word(a));
    return 1;
}

static uint32_t u_SetCursor(Cpu *c, Args *a)
{
    static uint16_t current;
    uint16_t h = arg_word(a), old = current;
    (void)c;
    current = h;
    return old;
}

/* A bitmap is its size, which is all anything here could ask of it: read from
   the resource's own header, which is a BITMAPINFOHEADER (40 bytes) or the
   older BITMAPCOREHEADER (12). */
static uint32_t u_LoadBitmap(Cpu *c, Args *a)
{
    uint16_t hinst = arg_word(a);
    uint32_t namep = arg_long(a);
    NeResource nr;
    uint32_t name;
    const uint8_t *d;
    uint16_t h;
    Obj *o;

    (void)c; (void)hinst;
    name = res_key(task.mod, namep, 0);
    if (!name || !ne_find_resource(task.mod, RT16_BITMAP, name, &nr) ||
        nr.len < 16 || nr.off + nr.len > task.mod->imglen)
        return 0;
    d = task.mod->img + nr.off;
    if (!(h = obj_new(OB_BITMAP))) return 0;
    o = obj_get(h);
    if (d[0] == 12) {
        o->w = (int16_t)(d[4] | d[5] << 8);
        o->h = (int16_t)(d[6] | d[7] << 8);
        o->bpp = d[10];
    } else {
        o->w = (int16_t)(d[4] | d[5] << 8);
        o->h = (int16_t)(d[8] | d[9] << 8);
        o->bpp = d[14];
    }
    return h;
}

static uint32_t u_LoadAccelerators(Cpu *c, Args *a)
{
    (void)c; (void)a;
    return obj_new(OB_ACCEL);
}

static uint32_t u_DrawIcon(Cpu *c, Args *a) { (void)c; (void)a; return 1; }

/* ---- the screen ---------------------------------------------------------------------- */

/* A 1920x1080 screen of the sort the emulator usually finds itself on. */
static uint32_t u_GetSystemMetrics(Cpu *c, Args *a)
{
    static const int16_t m[] = {
        1920, 1080, 17, 17, 23, 1, 1, 3, 3, 17,     /*  0-9  */
        17, 32, 32, 32, 32, 20, 1920, 1017, 0, 1,   /* 10-19 */
        17, 17, 0, 0, 0, 0, 0, 0, 136, 39,          /* 20-29 */
        18, 18, 4, 4, 136, 39,                      /* 30-35 */
    };
    int16_t i = arg_sword(a);
    (void)c;
    return (i >= 0 && i < (int)(sizeof m / sizeof *m)) ? (uint16_t)m[i] : 0;
}

static uint32_t u_GetSysColor(Cpu *c, Args *a)
{
    (void)c; (void)a;
    return 0x00C0C0C0u;
}

/* ---- painting, which never happens ------------------------------------------------- */

static uint32_t u_GetDC(Cpu *c, Args *a) { (void)c; (void)a; return dc_new(); }

static uint32_t u_ReleaseDC(Cpu *c, Args *a)
{
    uint16_t hwnd = arg_word(a), dc = arg_word(a);
    (void)c; (void)hwnd;
    obj_free(dc);
    return 1;
}

static uint32_t u_BeginPaint(Cpu *c, Args *a)
{
    uint16_t hwnd = arg_word(a);
    uint32_t p = arg_long(a);
    Win *w = win_get(hwnd);
    uint16_t dc = dc_new();
    uint8_t ps[32];
    (void)c;
    /* PAINTSTRUCT16: hdc, fErase, rcPaint, fRestore, fIncUpdate, reserved. */
    memset(ps, 0, sizeof ps);
    ps[0] = (uint8_t)dc;
    ps[1] = (uint8_t)(dc >> 8);
    if (p) {
        g_write(p, ps, sizeof ps);
        rect_out(p + 4, 0, 0, w ? w->w : 0, w ? w->h : 0);
    }
    return dc;
}

static uint32_t u_EndPaint(Cpu *c, Args *a)
{
    uint16_t hwnd = arg_word(a);
    uint32_t p = arg_long(a);
    (void)c; (void)hwnd;
    if (p) obj_free(sel_rd16(SEGPTR_SEL(p), SEGPTR_OFF(p)));
    return 1;
}

static uint32_t u_Nothing(Cpu *c, Args *a) { (void)c; (void)a; return 0; }
static uint32_t u_True(Cpu *c, Args *a)    { (void)c; (void)a; return 1; }

/* Text is measured, not drawn: DT_CALCRECT is the one use of DrawText whose
   answer the game keeps. */
static uint32_t u_DrawText(Cpu *c, Args *a)
{
    uint16_t dc = arg_word(a);
    uint32_t sp = arg_long(a);
    int16_t n = arg_sword(a);
    uint32_t rp = arg_long(a);
    uint16_t fmt = arg_word(a);
    const Obj *f = dc_font(dc);
    char buf[512];
    int h = f ? f->height : 16;
    (void)c;
    if (n < 0) n = (int16_t)strlen(g_str(sp, buf, sizeof buf));
    if ((fmt & 0x400) && rp) {                          /* DT_CALCRECT */
        uint16_t sel = SEGPTR_SEL(rp), off = SEGPTR_OFF(rp);
        int16_t l = (int16_t)sel_rd16(sel, off), t = (int16_t)sel_rd16(sel, (uint16_t)(off + 2));
        sel_wr16(sel, (uint16_t)(off + 4), (uint16_t)(l + n * (h / 2)));
        sel_wr16(sel, (uint16_t)(off + 6), (uint16_t)(t + h));
    }
    return (uint32_t)h;
}

/* ---- GDI ------------------------------------------------------------------------------- */

static uint32_t g_SetBkColor(Cpu *c, Args *a)   { (void)c; (void)a; return 0x00FFFFFFu; }
static uint32_t g_SetTextColor(Cpu *c, Args *a) { (void)c; (void)a; return 0; }
static uint32_t g_GetBkColor(Cpu *c, Args *a)   { (void)c; (void)a; return 0x00FFFFFFu; }
static uint32_t g_SetBkMode(Cpu *c, Args *a)    { (void)c; (void)a; return 2; }   /* OPAQUE */
static uint32_t g_SetROP2(Cpu *c, Args *a)      { (void)c; (void)a; return 13; }  /* R2_COPYPEN */
static uint32_t g_Region(Cpu *c, Args *a)       { (void)c; (void)a; return 2; }   /* SIMPLEREGION */
static uint32_t g_Lines(Cpu *c, Args *a)
{
    /* StretchDIBits: the scan lines copied, which is the height it was given.
       Arguments: hdc, x, y, cx, cy, xs, ys, cxs, cys, bits, info, usage, rop. */
    uint16_t dc = arg_word(a);
    int16_t x = arg_sword(a), y = arg_sword(a), cx = arg_sword(a), cy = arg_sword(a);
    int16_t xs = arg_sword(a), ys = arg_sword(a), cxs = arg_sword(a), cys = arg_sword(a);
    (void)c; (void)dc; (void)x; (void)y; (void)cx; (void)cy; (void)xs; (void)ys; (void)cxs;
    return (uint16_t)cys;
}

static uint32_t g_CreateObject(int kind)
{
    return obj_new(kind);
}

static uint32_t g_CreatePen(Cpu *c, Args *a)            { (void)c; (void)a; return g_CreateObject(OB_PEN); }
static uint32_t g_CreateBrush(Cpu *c, Args *a)          { (void)c; (void)a; return g_CreateObject(OB_BRUSH); }
static uint32_t g_CreateRectRgn(Cpu *c, Args *a)        { (void)c; (void)a; return g_CreateObject(OB_RGN); }
static uint32_t g_CreatePalette(Cpu *c, Args *a)        { (void)c; (void)a; return g_CreateObject(OB_PALETTE); }
static uint32_t g_CreateCompatibleDC(Cpu *c, Args *a)   { (void)c; (void)a; return dc_new(); }

static uint32_t g_CreateCompatibleBitmap(Cpu *c, Args *a)
{
    uint16_t dc = arg_word(a);
    int16_t w = arg_sword(a), h = arg_sword(a);
    uint16_t bm = obj_new(OB_BITMAP);
    Obj *o = obj_get(bm);
    (void)c; (void)dc;
    if (o) { o->w = w; o->h = h; o->bpp = 32; }
    return bm;
}

/* LOGFONT16 begins lfHeight, lfWidth, lfEscapement, lfOrientation, lfWeight. */
static uint32_t g_CreateFontIndirect(Cpu *c, Args *a)
{
    uint32_t p = arg_long(a);
    uint16_t f = obj_new(OB_FONT);
    Obj *o = obj_get(f);
    int16_t h;
    (void)c;
    if (!o) return 0;
    h = p ? (int16_t)sel_rd16(SEGPTR_SEL(p), SEGPTR_OFF(p)) : 0;
    o->height = (int16_t)(h < 0 ? -h : h ? h : 16);
    o->weight = p ? (int16_t)sel_rd16(SEGPTR_SEL(p), (uint16_t)(SEGPTR_OFF(p) + 8)) : 400;
    return f;
}

static uint32_t g_DeleteObject(Cpu *c, Args *a)
{
    (void)c;
    obj_free(arg_word(a));
    return 1;
}

static uint32_t g_GetStockObject(Cpu *c, Args *a)
{
    (void)c;
    return stock_object(arg_word(a));
}

static uint32_t g_SelectObject(Cpu *c, Args *a)
{
    uint16_t dc = arg_word(a), h = arg_word(a), old;
    Obj *d = obj_get(dc), *o = obj_get(h);
    uint16_t *slot;
    (void)c;
    if (!d || d->kind != OB_DC || !o) return 0;
    switch (o->kind) {
    case OB_FONT:   slot = &d->font;   break;
    case OB_PEN:    slot = &d->pen;    break;
    case OB_BRUSH:  slot = &d->brush;  break;
    case OB_BITMAP: slot = &d->bitmap; break;
    case OB_RGN:    return 2;                           /* SIMPLEREGION */
    default:        return 0;
    }
    old = *slot;
    *slot = h;
    if (!old && o->kind == OB_BITMAP) old = obj_new(OB_BITMAP);   /* 1x1 default */
    return old;
}

static uint32_t g_SelectPalette(Cpu *c, Args *a)
{
    uint16_t dc = arg_word(a), h = arg_word(a), old;
    Obj *d = obj_get(dc);
    (void)c;
    if (!d || d->kind != OB_DC) return 0;
    old = d->palette;
    d->palette = h;
    return old;
}

static uint32_t g_DeleteDC(Cpu *c, Args *a)
{
    (void)c;
    obj_free(arg_word(a));
    return 1;
}

static uint32_t g_GetDeviceCaps(Cpu *c, Args *a)
{
    uint16_t dc = arg_word(a);
    int16_t i = arg_sword(a);
    (void)c; (void)dc;
    switch (i) {
    case 2:  return 0x0300;             /* DRIVERVERSION */
    case 4:  return 508;                /* HORZSIZE, mm  */
    case 6:  return 286;                /* VERTSIZE, mm  */
    case 8:  return 1920;               /* HORZRES       */
    case 10: return 1080;               /* VERTRES       */
    case 12: return 32;                 /* BITSPIXEL     */
    case 14: return 1;                  /* PLANES        */
    case 24: return 0xFFFF;             /* NUMCOLORS: -1, more than a palette */
    case 38: return 0x7E99;             /* RASTERCAPS    */
    case 40: return 36;                 /* ASPECTX       */
    case 42: return 36;                 /* ASPECTY       */
    case 44: return 51;                 /* ASPECTXY      */
    case 88: return 96;                 /* LOGPIXELSX    */
    case 90: return 96;                 /* LOGPIXELSY    */
    }
    return 0;
}

static uint32_t g_GetTextMetrics(Cpu *c, Args *a)
{
    uint16_t dc = arg_word(a);
    uint32_t p = arg_long(a);
    const Obj *f = dc_font(dc);
    int h = f ? f->height : 16;
    int asc = (h * 3 + 2) / 4;
    uint8_t tm[31];
    (void)c;
    if (!p) return 0;
    /* TEXTMETRIC16: eight words of height and width, then nine bytes of
       style and character range, then three words. */
    memset(tm, 0, sizeof tm);
#define PUT16(o, v) (tm[o] = (uint8_t)(v), tm[(o) + 1] = (uint8_t)((unsigned)(v) >> 8))
    PUT16(0,  h);                       /* tmHeight           */
    PUT16(2,  asc);                     /* tmAscent           */
    PUT16(4,  h - asc);                 /* tmDescent          */
    PUT16(6,  h / 8);                   /* tmInternalLeading  */
    PUT16(8,  0);                       /* tmExternalLeading  */
    PUT16(10, h / 2);                   /* tmAveCharWidth     */
    PUT16(12, h);                       /* tmMaxCharWidth     */
    PUT16(14, f ? f->weight : 400);     /* tmWeight           */
    tm[19] = 0x20;                      /* tmFirstChar        */
    tm[20] = 0xFF;                      /* tmLastChar         */
    tm[21] = 0x1F;                      /* tmDefaultChar      */
    tm[22] = 0x20;                      /* tmBreakChar        */
    tm[23] = 0x22;                      /* tmPitchAndFamily   */
    PUT16(27, 96);                      /* tmDigitizedAspectX */
    PUT16(29, 96);                      /* tmDigitizedAspectY */
#undef PUT16
    g_write(p, tm, sizeof tm);
    return 1;
}

static uint32_t g_GetTextExtent(Cpu *c, Args *a)
{
    uint16_t dc = arg_word(a);
    uint32_t sp = arg_long(a);
    int16_t n = arg_sword(a);
    const Obj *f = dc_font(dc);
    int h = f ? f->height : 16;
    (void)c; (void)sp;
    if (n < 0) n = 0;
    return (uint32_t)(uint16_t)(n * (h / 2)) | ((uint32_t)(uint16_t)h << 16);
}

/* BITMAP16: bmType, bmWidth, bmHeight, bmWidthBytes, bmPlanes, bmBitsPixel,
   bmBits.  Anything else is described as zeros, which is at least a size. */
static uint32_t g_GetObject(Cpu *c, Args *a)
{
    uint16_t h = arg_word(a);
    int16_t size = arg_sword(a);
    uint32_t p = arg_long(a);
    const Obj *o = obj_get(h);
    uint8_t buf[50];
    int n = 0;
    (void)c;
    if (!o || !p || size <= 0) return 0;
    memset(buf, 0, sizeof buf);
    switch (o->kind) {
    case OB_BITMAP: {
        unsigned stride = (((unsigned)o->w * o->bpp + 15) / 16) * 2;
        buf[2] = (uint8_t)o->w; buf[3] = (uint8_t)((uint16_t)o->w >> 8);
        buf[4] = (uint8_t)o->h; buf[5] = (uint8_t)((uint16_t)o->h >> 8);
        buf[6] = (uint8_t)stride; buf[7] = (uint8_t)(stride >> 8);
        buf[8] = 1;
        buf[9] = o->bpp;
        n = 14;
        break;
    }
    case OB_FONT:
        buf[0] = (uint8_t)o->height; buf[1] = (uint8_t)((uint16_t)o->height >> 8);
        buf[8] = (uint8_t)o->weight; buf[9] = (uint8_t)((uint16_t)o->weight >> 8);
        n = 50;
        break;
    case OB_PEN:   n = 10; break;
    case OB_BRUSH: n = 8;  break;
    default:       return 0;
    }
    if (n > size) n = size;
    g_write(p, buf, (size_t)n);
    return (uint32_t)n;
}

static uint32_t g_MoveTo(Cpu *c, Args *a)      { (void)c; (void)a; return 0; }
static uint32_t g_SetPixel(Cpu *c, Args *a)    { (void)c; (void)a; return 0; }
static uint32_t g_Escape(Cpu *c, Args *a)      { (void)c; (void)a; return 0; }

/* ---- COMMDLG and WAVEMIX: nobody to ask, nothing to hear ---------------------------- */

static uint32_t cd_Cancelled(Cpu *c, Args *a) { (void)c; (void)a; return 0; }

/* WaveMixConfigureInit returning no session is how the game learns there is no
   sound, after which it never calls another WaveMix entry.  The rest answer
   as they would with no session, should one ever be reached. */
static uint32_t wm_NoSession(Cpu *c, Args *a) { (void)c; (void)a; return 0; }
static uint32_t wm_Fail(Cpu *c, Args *a)      { (void)c; (void)a; return 1; }

/* ---- Stars.ini, which is not there ------------------------------------------------- */

static uint32_t p_GetPrivateProfileString(Cpu *c, Args *a)
{
    uint32_t secp = arg_long(a), keyp = arg_long(a), defp = arg_long(a);
    uint32_t bufp = arg_long(a);
    uint16_t size = arg_word(a);
    char key[64], value[256];
    (void)c; (void)secp;
    g_str(keyp, key, sizeof key);
    g_str(defp, value, sizeof value);
    if (keyp && !strcmp(key, "GlobalSettings"))
        snprintf(value, sizeof value, "%s", GLOBAL_SETTINGS_PRESET);
    if (!keyp) value[0] = 0;          /* a section's keys: there are none */
    return g_puts(bufp, value, size);
}

static uint32_t p_GetPrivateProfileInt(Cpu *c, Args *a)
{
    uint32_t secp = arg_long(a), keyp = arg_long(a);
    int16_t def = arg_sword(a);
    char sec[64], key[64];
    (void)c;
    g_str(secp, sec, sizeof sec);
    g_str(keyp, key, sizeof key);
    if (!ascii_casecmp(sec, "Windows") && !ascii_casecmp(key, "Layout"))
        return LAYOUT_LARGE;
    return (uint16_t)def;
}

static uint32_t p_WritePrivateProfileString(Cpu *c, Args *a)
{
    (void)c; (void)a;
    return 1;
}

/* ---- wiring ----------------------------------------------------------------------------- */

void headless_reset(void)
{
    memset(objs, 0, sizeof objs);
    memset(stock, 0, sizeof stock);
    memset(classes, 0, sizeof classes);
    nclasses = 0;
    memset(wins, 0, sizeof wins);
    focus = capture = active = 0;
    defproc = 0;
    qhead = qcount = 0;
    quit_posted = 0;
    quit_code = 0;
    headless_msgboxes = 0;
}

void headless_register(void)
{
    static const struct { const char *mod; uint16_t ord; ApiFn fn; } binds[] = {
        { "KERNEL", 127, p_GetPrivateProfileInt },
        { "KERNEL", 128, p_GetPrivateProfileString },
        { "KERNEL", 129, p_WritePrivateProfileString },

        { "USER",   1, u_MessageBox },
        { "USER",   5, u_InitApp },
        { "USER",   6, u_PostQuitMessage },
        { "USER",   7, u_ExitWindows },
        { "USER",  10, u_SetTimer },
        { "USER",  12, u_KillTimer },
        { "USER",  17, u_GetCursorPos },
        { "USER",  18, u_SetCapture },
        { "USER",  19, u_ReleaseCapture },
        { "USER",  22, u_SetFocus },
        { "USER",  23, u_GetFocus },
        { "USER",  28, u_ClientToScreen },
        { "USER",  29, u_ScreenToClient },
        { "USER",  30, u_WindowFromPoint },
        { "USER",  31, u_IsIconic },
        { "USER",  32, u_GetWindowRect },
        { "USER",  33, u_GetClientRect },
        { "USER",  34, u_EnableWindow },
        { "USER",  36, u_GetWindowText },
        { "USER",  37, u_SetWindowText },
        { "USER",  39, u_BeginPaint },
        { "USER",  40, u_EndPaint },
        { "USER",  41, u_CreateWindow },
        { "USER",  42, u_ShowWindow },
        { "USER",  46, u_GetParent },
        { "USER",  49, u_IsWindowVisible },
        { "USER",  53, u_DestroyWindow },
        { "USER",  56, u_MoveWindow },
        { "USER",  57, u_RegisterClass },
        { "USER",  60, u_GetActiveWindow },
        { "USER",  61, u_ScrollWindow },
        { "USER",  62, u_SetScrollPos },
        { "USER",  63, u_GetScrollPos },
        { "USER",  64, u_SetScrollRange },
        { "USER",  66, u_GetDC },
        { "USER",  68, u_ReleaseDC },
        { "USER",  69, u_SetCursor },
        { "USER",  81, u_True },                /* FillRect */
        { "USER",  83, u_True },                /* FrameRect */
        { "USER",  84, u_DrawIcon },
        { "USER",  85, u_DrawText },
        { "USER",  87, u_DialogBox },
        { "USER",  88, u_EndDialog },
        { "USER",  89, u_CreateDialog },
        { "USER",  91, u_GetDlgItem },
        { "USER",  92, u_SetDlgItemText },
        { "USER",  93, u_GetDlgItemText },
        { "USER",  96, u_CheckRadioButton },
        { "USER",  97, u_CheckDlgButton },
        { "USER",  98, u_IsDlgButtonChecked },
        { "USER", 101, u_SendDlgItemMessage },
        { "USER", 104, u_MessageBeep },
        { "USER", 105, u_FlashWindow },
        { "USER", 106, u_GetKeyState },
        { "USER", 107, u_DefWindowProc },
        { "USER", 108, u_GetMessage },
        { "USER", 109, u_PeekMessage },
        { "USER", 110, u_PostMessage },
        { "USER", 111, u_SendMessage },
        { "USER", 113, u_TranslateMessage },
        { "USER", 114, u_DispatchMessage },
        { "USER", 122, u_CallWindowProc },
        { "USER", 124, u_True },                /* UpdateWindow */
        { "USER", 125, u_Nothing },             /* InvalidateRect */
        { "USER", 127, u_Nothing },             /* ValidateRect */
        { "USER", 135, u_GetWindowLong },
        { "USER", 136, u_SetWindowLong },
        { "USER", 152, u_DestroyMenu },
        { "USER", 154, u_MenuState },           /* CheckMenuItem */
        { "USER", 155, u_MenuState },           /* EnableMenuItem */
        { "USER", 157, u_GetMenu },
        { "USER", 159, u_GetSubMenu },
        { "USER", 160, u_Nothing },             /* DrawMenuBar */
        { "USER", 171, u_WinHelp },
        { "USER", 173, u_LoadCursor },
        { "USER", 174, u_LoadIcon },
        { "USER", 175, u_LoadBitmap },
        { "USER", 177, u_LoadAccelerators },
        { "USER", 178, u_TranslateAccelerator },
        { "USER", 179, u_GetSystemMetrics },
        { "USER", 180, u_GetSysColor },
        { "USER", 232, u_SetWindowPos },
        { "USER", 249, u_GetKeyState },         /* GetAsyncKeyState */
        { "USER", 258, u_MapWindowPoints },
        { "USER", 262, u_GetWindow },
        { "USER", 263, u_MenuItemCount },
        { "USER", 272, u_IsZoomed },
        { "USER", 282, g_SelectPalette },
        { "USER", 283, u_Nothing },             /* RealizePalette */
        { "USER", 370, u_GetWindowPlacement },
        { "USER", 410, u_MenuOk },              /* InsertMenu */
        { "USER", 411, u_MenuOk },              /* AppendMenu */
        { "USER", 413, u_MenuOk },              /* DeleteMenu */
        { "USER", 415, u_CreatePopupMenu },
        { "USER", 416, u_TrackPopupMenu },
        { "USER", 457, u_DestroyObject },       /* DestroyIcon */
        { "USER", 458, u_DestroyObject },       /* DestroyCursor */

        { "GDI",   1, g_SetBkColor },
        { "GDI",   2, g_SetBkMode },
        { "GDI",   4, g_SetROP2 },
        { "GDI",   9, g_SetTextColor },
        { "GDI",  11, u_Nothing },              /* SetWindowOrg */
        { "GDI",  19, u_True },                 /* LineTo */
        { "GDI",  20, g_MoveTo },
        { "GDI",  21, g_Region },               /* ExcludeClipRect */
        { "GDI",  22, g_Region },               /* IntersectClipRect */
        { "GDI",  24, u_True },                 /* Ellipse */
        { "GDI",  27, u_True },                 /* Rectangle */
        { "GDI",  29, u_True },                 /* PatBlt */
        { "GDI",  31, g_SetPixel },
        { "GDI",  33, u_True },                 /* TextOut */
        { "GDI",  34, u_True },                 /* BitBlt */
        { "GDI",  38, g_Escape },
        { "GDI",  44, g_Region },               /* SelectClipRgn */
        { "GDI",  45, g_SelectObject },
        { "GDI",  51, g_CreateCompatibleBitmap },
        { "GDI",  52, g_CreateCompatibleDC },
        { "GDI",  57, g_CreateFontIndirect },
        { "GDI",  60, g_CreateBrush },          /* CreatePatternBrush */
        { "GDI",  61, g_CreatePen },
        { "GDI",  64, g_CreateRectRgn },
        { "GDI",  66, g_CreateBrush },          /* CreateSolidBrush */
        { "GDI",  68, g_DeleteDC },
        { "GDI",  69, g_DeleteObject },
        { "GDI",  75, g_GetBkColor },
        { "GDI",  80, g_GetDeviceCaps },
        { "GDI",  82, g_GetObject },
        { "GDI",  85, g_SetROP2 },              /* GetROP2 */
        { "GDI",  87, g_GetStockObject },
        { "GDI",  91, g_GetTextExtent },
        { "GDI",  93, g_GetTextMetrics },
        { "GDI", 148, u_Nothing },              /* SetBrushOrg */
        { "GDI", 150, u_True },                 /* UnrealizeObject */
        { "GDI", 351, u_True },                 /* ExtTextOut */
        { "GDI", 360, g_CreatePalette },
        { "GDI", 439, g_Lines },                /* StretchDIBits */
        { "GDI", 441, u_Nothing },              /* GetDIBits */

        { "COMMDLG",  1, cd_Cancelled },        /* GetOpenFileName */
        { "COMMDLG",  2, cd_Cancelled },        /* GetSaveFileName */
        { "COMMDLG", 20, cd_Cancelled },        /* PrintDlg */

        { "WAVEMIX",  4, wm_Fail },             /* Activate */
        { "WAVEMIX",  5, wm_NoSession },        /* OpenWave */
        { "WAVEMIX",  6, wm_Fail },             /* OpenChannel */
        { "WAVEMIX",  7, wm_Fail },             /* Play */
        { "WAVEMIX",  8, wm_Fail },             /* FlushChannel */
        { "WAVEMIX",  9, wm_Fail },             /* CloseChannel */
        { "WAVEMIX", 10, wm_Fail },             /* FreeWave */
        { "WAVEMIX", 11, wm_Fail },             /* CloseSession */
        { "WAVEMIX", 12, wm_Fail },             /* Pump */
        { "WAVEMIX", 14, wm_Fail },             /* GetInfo */
        { "WAVEMIX", 15, wm_NoSession },        /* ConfigureInit */
    };
    unsigned i;
    for (i = 0; i < sizeof binds / sizeof *binds; i++)
        api_bind(binds[i].mod, binds[i].ord, binds[i].fn);
}
