/* msg16.c - guest -> host message marshaling.  See msg16.h. */

#include "msg16.h"
#include "handle.h"
#include "gmem.h"
#include "log.h"
#include "winproc.h"

#include <stdio.h>
#include <string.h>

/* ---- marshaling ----------------------------------------------------------- */

/* What has to be moved back into guest memory once the real window has run. */
enum { OUT_NONE = 0, OUT_STR, OUT_RECT, OUT_LINE, OUT_INTS, OUT_DATA,
       OUT_DATALEN };

#define BUFSZ 4096

struct marshal {
    UINT     msg;
    WPARAM   wp;
    LPARAM   lp;
    int      out;
    uint32_t outp;              /* guest destination */
    unsigned outmax;            /* bytes available there */
    int      ret;               /* H_* when the result is a handle */
    int      refuse;            /* no honest translation exists */
    int      local;             /* lp points into this record, and no out
                                   path says so already */
    UINT     after;             /* a message without parameters to follow it */
    char     buf[BUFSZ];
    RECT     rect;
    INT      tabs[64];
    INT      ints[256];
};

/* Whether a list or combo box keeps strings.  One that is owner-draw without
   LBS_HASSTRINGS / CBS_HASSTRINGS keeps only a 32-bit value per item, and the
   lParam of an add, insert, find or select is that value itself: the control
   stores it, compares by it (through WM_COMPAREITEM if sorted), and hands it
   back in WM_DRAWITEM, WM_DELETEITEM and LB_GETITEMDATA.  Copied as a string,
   every item's data would be the address of our copy on the host stack - and
   a value that is no far pointer would fault the guest on the way.  Wine asks
   the same question, in WINPROC_TestLBForStr.  The two classes keep the flag
   in different bits; the message has already been renumbered for the window's
   class, so the caller knows which it is. */
static int keeps_strings(HWND hwnd, int combo)
{
    DWORD style = (DWORD)GetWindowLongA(hwnd, GWL_STYLE);

    if (combo)
        return !(style & (CBS_OWNERDRAWFIXED | CBS_OWNERDRAWVARIABLE))
            || (style & CBS_HASSTRINGS);
    return !(style & (LBS_OWNERDRAWFIXED | LBS_OWNERDRAWVARIABLE))
        || (style & LBS_HASSTRINGS);
}

/* A message whose lParam is a string the guest is handing in.  LB_DIR and
   CB_DIR take a file spec whatever the style. */
static int lp_is_in_string(HWND hwnd, UINT msg)
{
    switch (msg) {
    case WM_SETTEXT:
    case EM_REPLACESEL:
    case LB_DIR:
    case CB_DIR:
        return 1;
    case LB_ADDSTRING: case LB_INSERTSTRING: case LB_FINDSTRING:
    case LB_SELECTSTRING: case LB_FINDSTRINGEXACT:
        return keeps_strings(hwnd, 0);
    case CB_ADDSTRING: case CB_INSERTSTRING: case CB_FINDSTRING:
    case CB_SELECTSTRING: case CB_FINDSTRINGEXACT:
        return keeps_strings(hwnd, 1);
    default:
        return 0;
    }
}

/* A message whose wParam is an item index, which Win16 passed as a 16-bit int
   with -1 part of the contract: append for an insert, no selection for a
   SETCURSEL, the whole list for a find or select, the selection field rather
   than the items for CB_GETITEMHEIGHT and CB_SETITEMHEIGHT.  Zero-extended,
   -1 reaches USER32 as 65535, an index past the end: the insert fails and adds
   nothing, the selection stays where it was, the heights are the items'.  The
   finds only work because USER32 starts a search from an out-of-range index
   at the top.  This is the list Wine's user.exe16 sign-extends, in
   listbox_proc16 and combo_proc16, less LB_SETSEL, which is below, and plus
   LB_GETITEMDATA and LB_SETITEMDATA.  Wine has only their combo box
   counterparts, two authors' habits that change nothing there, as Wine's list
   box refuses -1 to either.  USER32 takes -1 to either SETITEMDATA as every
   item - an unchecked LB_ERR passed on as the index included - and a combo
   box only hands the message on to its list box, so the two have to agree. */
static int wp_is_index(UINT msg)
{
    switch (msg) {
    case LB_INSERTSTRING: case LB_FINDSTRING: case LB_FINDSTRINGEXACT:
    case LB_SELECTSTRING: case LB_SETCURSEL: case LB_GETSEL:
    case LB_SETANCHORINDEX: case LB_GETITEMRECT:
    case LB_GETITEMDATA: case LB_SETITEMDATA:
    case CB_INSERTSTRING: case CB_FINDSTRING: case CB_FINDSTRINGEXACT:
    case CB_SELECTSTRING: case CB_SETCURSEL: case CB_GETLBTEXT:
    case CB_GETLBTEXTLEN: case CB_GETITEMDATA: case CB_SETITEMDATA:
    case CB_GETITEMHEIGHT: case CB_SETITEMHEIGHT:
        return 1;
    default:
        return 0;
    }
}

/* The rewrites that need nothing but the message's own three values: the
   renumbering, an index's sign, a handle's 32-bit value, the pairs whose
   parameters Win32 packs differently.  Each undoes one of winproc.c's
   msg_to_16_values, and the two are all a posted message can need: the
   guest's message loop hands the guest a MSG made by msg16_pack, and what it
   is handed back goes to DispatchMessage and the rest through here. */
void msg16_unpack(HWND hwnd, uint16_t msg16, uint16_t wp, uint32_t lp,
                  struct msg32 *m)
{
    m->msg   = msg16_to_32(hwnd, msg16);
    m->wp    = wp;
    m->lp    = (LPARAM)lp;
    m->after = 0;
    if (wp_is_index(m->msg))
        m->wp = (WPARAM)(int16_t)wp;

    switch (m->msg) {

    case EM_SETSEL:
        /* Win16 packs both ends into lParam and uses wParam as a scroll flag;
           Win32 passes the start in wParam and the end in lParam.  Left alone,
           every "select this range" becomes "select from 0 to somewhere past
           the end", and the next keystroke wipes the field. */
        if ((int16_t)LOWORD(lp) == -1) {         /* the Win16 deselect-all form */
            m->wp = (WPARAM)-1;
            m->lp = 0;
        } else {
            m->wp = (WPARAM)LOWORD(lp);
            m->lp = (LPARAM)(int16_t)HIWORD(lp);
        }
        /* The scroll flag is a multiline control's: 0 scrolls the caret into
           view and 1 does not (KB Q102641).  Win32's EM_SETSEL never
           scrolls a multiline control, so 0 needs an EM_SCROLLCARET after
           it.  A single-line control ignored the flag, and Win32's scrolls
           one whatever it is told. */
        if (!wp && (GetWindowLongA(hwnd, GWL_STYLE) & ES_MULTILINE))
            m->after = EM_SCROLLCARET;
        break;

    /* ---- an index that can be -1 ------------------------------------------- */

    /* The list and combo box messages with one in wParam were taken care of
       at the top, by wp_is_index. */

    case LB_SETSEL:
        /* The item is lParam's low word, -1 for every item, and the high word
           goes unread.  Wine's listbox_proc16 sign-extends wParam instead,
           which is only the select-or-deselect flag.  USER32 happens to take
           a zero-extended 0xFFFF as -1 here, but not a high word that Win16
           would have ignored. */
        m->lp = (LPARAM)(int16_t)LOWORD(lp);
        break;

    case EM_LINEINDEX:
    case EM_LINEFROMCHAR:
    case EM_LINELENGTH:
        /* -1 is the caret's line, or for EM_LINELENGTH what is left unselected
           on the selection's lines; zero-extended, it asks about a character
           or line past the end.  Only -1 itself, as Wine does for
           EM_LINEINDEX: the others take a character position, and an edit
           control's text can run past 32K. */
        if (wp == 0xFFFF) m->wp = (WPARAM)-1;
        break;

    /* ---- handles in wParam ------------------------------------------------- */

    case WM_SETFONT:
        m->wp = (WPARAM)h32(H_FONT, wp);
        break;

    case WM_ERASEBKGND:
    case WM_ICONERASEBKGND:
    case WM_PAINT:
        m->wp = (WPARAM)HDC_32(wp);
        break;

    case WM_SETCURSOR:
    case WM_MOUSEACTIVATE:
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_INITDIALOG:
        m->wp = (WPARAM)HWND_32(wp);
        break;

    case WM_INITMENU:
    case WM_INITMENUPOPUP:
        m->wp = (WPARAM)HMENU_32(wp);
        break;

    /* ---- the pairs whose two parameters swap round ------------------------- */

    case WM_COMMAND:
    case WM_VKEYTOITEM:
    case WM_CHARTOITEM:
        /* The control in lParam's low word, and in the high word a notify
           code, or for the list box's two the caret's index. */
        m->wp = MAKEWPARAM(wp, HIWORD(lp));
        m->lp = (LPARAM)HWND_32(LOWORD(lp));
        break;

    case WM_HSCROLL:
    case WM_VSCROLL:
        m->wp = MAKEWPARAM(wp, LOWORD(lp));
        m->lp = (LPARAM)HWND_32(HIWORD(lp));
        break;

    case WM_ACTIVATE:
        m->wp = MAKEWPARAM(wp, 0);
        m->lp = (LPARAM)HWND_32(HIWORD(lp));
        break;

    case WM_MENUSELECT:
    case WM_MENUCHAR:
        /* The flags in lParam's low word and the menu in its high word.
           Win16 names a submenu by its handle where Win32 gives its position
           in the menu, so the handle has to be looked for there. */
        m->wp = MAKEWPARAM(wp, LOWORD(lp));
        m->lp = (LPARAM)HMENU_32(HIWORD(lp));
        if (m->msg == WM_MENUSELECT && (LOWORD(lp) & MF_POPUP) && m->lp) {
            HMENU menu = (HMENU)m->lp, sub = HMENU_32(wp);
            int i, n = GetMenuItemCount(menu);
            for (i = 0; sub && i < n; i++)
                if (GetSubMenu(menu, i) == sub) {
                    m->wp = MAKEWPARAM(i, LOWORD(lp));
                    break;
                }
        }
        break;

    case WM_PARENTNOTIFY:
        /* For a child's birth or death, the child in lParam's low word and its
           id in the high; for a click, where it was, the same both ways. */
        if (wp == WM_CREATE || wp == WM_DESTROY) {
            m->wp = MAKEWPARAM(wp, HIWORD(lp));
            m->lp = (LPARAM)HWND_32(LOWORD(lp));
        }
        break;

    case WM_ENTERIDLE:
        m->lp = (LPARAM)HWND_32(LOWORD(lp));
        break;

    case EM_LINESCROLL:
        /* Win16 packs both counts into lParam, lines in the low word and
           columns in the high, with wParam unused; Win32 moved the columns
           to wParam.  Both are signed, and a negative count scrolls up or
           left.  Passed through as it was, three lines up became 65533
           down and every column 65536 lines.  Wine's edit_proc16 takes it
           apart the same way. */
        m->wp = (WPARAM)(int16_t)HIWORD(lp);
        m->lp = (LPARAM)(int16_t)LOWORD(lp);
        break;

    /* ---- one Win16 message fanning out into seven -------------------------- */

    case 0x0019: {                                  /* WM_CTLCOLOR */
        unsigned type = HIWORD(lp);
        if (type > 6) type = 6;
        m->msg = WM_CTLCOLORMSGBOX + type;
        m->wp  = (WPARAM)HDC_32(wp);
        m->lp  = (LPARAM)HWND_32(LOWORD(lp));
        break;
    }

    default:
        break;
    }
}

static void marshal_in(HWND hwnd, uint16_t msg16, uint16_t wp, uint32_t lp,
                       struct marshal *m)
{
    struct msg32 v;

    msg16_unpack(hwnd, msg16, wp, lp, &v);
    memset(m, 0, sizeof *m);
    m->msg   = v.msg;
    m->wp    = v.wp;
    m->lp    = v.lp;
    m->after = v.after;

    if (lp_is_in_string(hwnd, m->msg)) {
        g_str(lp, m->buf, sizeof m->buf);
        m->lp = (LPARAM)m->buf;
        m->local = 1;
        return;
    }

    switch (m->msg) {

    /* ---- text coming back out --------------------------------------------- */

    case LB_GETTEXTLEN:
    case CB_GETLBTEXTLEN:
        /* An item's "length" when it is item data is its size, which is the
           4 of Win16's DWORD whatever a ULONG_PTR is here: see OUT_DATA. */
        if (!keeps_strings(hwnd, m->msg == CB_GETLBTEXTLEN))
            m->out = OUT_DATALEN;
        break;

    case LB_GETTEXT:
    case CB_GETLBTEXT:
        if (!keeps_strings(hwnd, m->msg == CB_GETLBTEXT)) {
            /* No string to fetch: the control writes the item's data. */
            m->out  = OUT_DATA;
            m->outp = lp;
            m->lp   = (LPARAM)m->buf;
            break;
        }
        /* fall through */
    case WM_GETTEXT:
        /* WM_GETTEXT is bounded by wParam; the listbox and combobox forms are
           not bounded at all, which is a hazard in Win16 just as much as here -
           the caller is expected to have asked for the length first. */
        m->out    = OUT_STR;
        m->outp   = lp;
        m->outmax = (m->msg == WM_GETTEXT) ? (wp ? wp : 1) : sizeof m->buf;
        if (m->outmax > sizeof m->buf) m->outmax = sizeof m->buf;
        if (m->msg == WM_GETTEXT) m->wp = m->outmax;
        m->lp = (LPARAM)m->buf;
        break;

    case EM_GETLINE:
        /* The buffer's first word carries its own size, in and out. */
        m->out    = OUT_LINE;
        m->outp   = lp;
        m->outmax = sel_rd16(SEGPTR_SEL(lp), SEGPTR_OFF(lp));
        if (m->outmax > sizeof m->buf - 2) m->outmax = sizeof m->buf - 2;
        *(uint16_t *)m->buf = (uint16_t)m->outmax;
        m->lp = (LPARAM)m->buf;
        break;

    case LB_GETSELITEMS: {
        /* The guest's array is 16-bit; USER32 writes 32-bit ints.  Forwarded
           raw this scribbles over whatever lies at the numeric value of the far
           pointer - about 21 MB into the address space - which is why the only
           multi-select list in the game, Merge Fleets, took the process with
           it. */
        unsigned n = wp;
        if (n > sizeof m->ints / sizeof m->ints[0])
            n = sizeof m->ints / sizeof m->ints[0];
        m->out    = OUT_INTS;
        m->outp   = lp;
        m->outmax = n;
        m->wp     = n;
        m->lp     = n ? (LPARAM)m->ints : 0;
        break;
    }

    /* ---- rectangles -------------------------------------------------------- */

    case EM_SETRECT:
    case EM_SETRECTNP: {
        int16_t r16[4];
        g_read(lp, r16, sizeof r16);
        m->rect.left = r16[0]; m->rect.top = r16[1];
        m->rect.right = r16[2]; m->rect.bottom = r16[3];
        m->lp = (LPARAM)&m->rect;
        m->local = 1;
        break;
    }

    case EM_GETRECT:
    case LB_GETITEMRECT:
    case CB_GETDROPPEDCONTROLRECT:
        m->out  = OUT_RECT;
        m->outp = lp;
        m->lp   = (LPARAM)&m->rect;
        break;

    /* ---- an array of ints, 16 bits wide in Win16 --------------------------- */

    case EM_SETTABSTOPS:
    case LB_SETTABSTOPS: {
        unsigned n = wp, i;
        if (n > sizeof m->tabs / sizeof m->tabs[0])
            n = sizeof m->tabs / sizeof m->tabs[0];
        for (i = 0; i < n; i++)
            m->tabs[i] = (int16_t)sel_rd16(SEGPTR_SEL(lp),
                                           (uint16_t)(SEGPTR_OFF(lp) + i * 2));
        m->wp = n;
        m->lp = n ? (LPARAM)m->tabs : 0;
        m->local = n != 0;
        break;
    }

    /* ---- results that are handles ------------------------------------------ */

    case WM_GETFONT:
        m->ret = H_FONT;
        break;

    case WM_QUERYDRAGICON:
        m->ret = H_ICON;
        break;

    case WM_CTLCOLORMSGBOX:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSCROLLBAR:
    case WM_CTLCOLORSTATIC:                         /* WM_CTLCOLOR, unpacked */
        m->ret = H_BRUSH;
        break;

    /* ---- messages that only make sense coming the other way ---------------- */

    case WM_NCCREATE:
    case WM_CREATE:
    case WM_GETMINMAXINFO:
    case WM_NCCALCSIZE:
    case WM_WINDOWPOSCHANGING:
    case WM_WINDOWPOSCHANGED:
    case WM_MEASUREITEM:
    case WM_DRAWITEM:
    case WM_DELETEITEM:
    case WM_COMPAREITEM:
        /* lParam is a 16-bit struct we would have to widen, and nothing in
           Win16 sends these by hand.  Say so rather than hand USER32 a
           segmented address and let it fault somewhere else. */
        m->refuse = 1;
        break;

    default:
        break;
    }
}

/* A list or combo box answers LB_ERR (CB_ERR is the same -1) when there is
   no such item, or for LB_GETSELITEMS when it is not a multiple-selection
   list, and writes nothing, so nothing is copied back: the guest's buffer
   stays as it was, as USER32 leaves ours and Win16 left the guest's.
   WM_GETTEXT, the other one copied out as a string, never answers -1.
   LB_GETITEMRECT is not among them: for an item that is not there USER32
   empties the rectangle, and the record's, zeroed, is copied back. */
static uint32_t marshal_out(struct marshal *m, LRESULT r)
{
    switch (m->out) {
    case OUT_STR:
        if ((LONG)r != LB_ERR) g_puts(m->outp, m->buf, m->outmax);
        break;
    case OUT_LINE: {
        /* EM_GETLINE returns the character count and does not terminate. */
        unsigned n = (unsigned)r;
        if (n > m->outmax) n = m->outmax;
        g_write(m->outp, m->buf, n);
        break;
    }
    case OUT_INTS: {
        /* The result is how many the control actually wrote. */
        unsigned n = (unsigned)r, i;
        if ((LONG)r == LB_ERR) break;
        if (n > m->outmax) n = m->outmax;
        for (i = 0; i < n; i++)
            sel_wr16(SEGPTR_SEL(m->outp),
                     (uint16_t)(SEGPTR_OFF(m->outp) + i * 2),
                     (uint16_t)m->ints[i]);
        break;
    }
    case OUT_DATA:
        /* Win16's item data is a DWORD.  An x64 USER32 writes a ULONG_PTR and
           reports its size, 8, where Win16 reported 4; its low half is the
           first four bytes either way. */
        if ((LONG)r != LB_ERR) {
            g_write(m->outp, m->buf, 4);
            r = 4;
        }
        break;
    case OUT_DATALEN:
        /* And the length queries say 8 too - a combo box's does even on a
           32-bit build - where Win16 said 4. */
        if ((LONG)r != LB_ERR) r = 4;
        break;
    case OUT_RECT: {
        int16_t r16[4];
        r16[0] = (int16_t)m->rect.left;  r16[1] = (int16_t)m->rect.top;
        r16[2] = (int16_t)m->rect.right; r16[3] = (int16_t)m->rect.bottom;
        g_write(m->outp, r16, sizeof r16);
        break;
    }
    default:
        break;
    }

    if (m->ret != H_NONE)
        return h16(m->ret, (void *)(uintptr_t)r);
    return (uint32_t)r;
}

/* A send, or with `proc` a call of that procedure, which is the same thing
   done by hand. */
static uint32_t deliver(WNDPROC proc, HWND hwnd, uint16_t msg16, uint16_t wp,
                        uint32_t lp)
{
    struct marshal m;
    LRESULT r;

    marshal_in(hwnd, msg16, wp, lp, &m);
    if (m.refuse) {
        log_msg("*** guest sent message %04X (Win32 %04X) by hand; its lParam is "
                "a 16-bit struct with no translation on this path\n", msg16, m.msg);
        return 0;
    }
    if (proc) {
        r = CallWindowProcA(proc, hwnd, m.msg, m.wp, m.lp);
        if (m.after) CallWindowProcA(proc, hwnd, m.after, 0, 0);
    } else {
        r = SendMessageA(hwnd, m.msg, m.wp, m.lp);
        if (m.after) SendMessageA(hwnd, m.after, 0, 0);
    }
    return marshal_out(&m, r);
}

uint32_t msg16_send(HWND hwnd, uint16_t msg16, uint16_t wp, uint32_t lp)
{
    return deliver(NULL, hwnd, msg16, wp, lp);
}

uint32_t msg16_call(WNDPROC proc, HWND hwnd, uint16_t msg16, uint16_t wp,
                    uint32_t lp)
{
    return deliver(proc, hwnd, msg16, wp, lp);
}

/* The messages Win32 will not post whatever their lParam, where marshal_in
   can leave one that is no pointer at all: item data for a list without
   strings, or a count of 0 that resets the tab stops.  Win32 refuses them by
   number (ERROR_MESSAGE_SYNC_ONLY); Win16 posted them.  It refuses more than
   these.  Some carry a far pointer nothing here translates, which is better
   refused than sent, since the window would read through it.  Others carry
   none - WM_ERASEBKGND, WM_INITDIALOG, WM_PARENTNOTIFY, and queries such as
   EM_GETSEL whose answer a post throws away - and are refused as they always
   were; the game posts none of them. */
static int post_refused(UINT msg)
{
    switch (msg) {
    case LB_ADDSTRING: case LB_INSERTSTRING: case LB_FINDSTRING:
    case LB_SELECTSTRING: case LB_FINDSTRINGEXACT:
    case CB_ADDSTRING: case CB_INSERTSTRING: case CB_FINDSTRING:
    case CB_SELECTSTRING: case CB_FINDSTRINGEXACT:
    case LB_SETTABSTOPS: case EM_SETTABSTOPS:
        return 1;
    default:
        return 0;
    }
}

uint32_t msg16_post(HWND hwnd, uint16_t msg16, uint16_t wp, uint32_t lp)
{
    struct marshal m;

    marshal_in(hwnd, msg16, wp, lp, &m);
    if (m.refuse) return msg16_send(hwnd, msg16, wp, lp);  /* which says why */

    /* A post outlives this call, so anything that had to be copied into a local
       buffer cannot go out this way - the buffer is gone before the message is
       read.  Send it instead; that changes the timing, which is the lesser of
       the two wrongs.  So too the messages Win32 will not post even when they
       carry no pointer; see post_refused. */
    if (m.out == OUT_NONE && !m.local && !post_refused(m.msg)) {
        if (!PostMessageA(hwnd, m.msg, m.wp, m.lp)) return 0;
        if (m.after) PostMessageA(hwnd, m.after, 0, 0);
        return 1;
    }

    /* The guest asks whether the post went through, not for the result: an
       index of 0 is no failure, and a window that is gone is one. */
    if (!IsWindow(hwnd)) return 0;
    log_msg("*** guest posted message %04X, which cannot go out as a post; "
            "sending it instead\n", msg16);
    msg16_send(hwnd, msg16, wp, lp);
    return 1;
}
