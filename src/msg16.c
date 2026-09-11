/* msg16.c - guest -> host message marshaling.  See msg16.h. */

#include "msg16.h"
#include "handle.h"
#include "gmem.h"
#include "log.h"

#include <stdio.h>
#include <string.h>

/* ---- renumbering ---------------------------------------------------------- */

/* The offset between a Win16 control message and its Win32 number, per class.
   Win16 numbered all of these from WM_USER, which is why the ranges collide and
   why the class has to be consulted. */
UINT msg16_to_32_for(HWND hwnd, uint16_t msg16)
{
    char cls[32];

    if (msg16 < 0x0400 || !hwnd) return msg16;
    if (!GetClassNameA(hwnd, cls, sizeof cls)) return msg16;

    if (!_stricmp(cls, "Button")) {
        if (msg16 <= 0x0404) return msg16 - 0x0310;          /* BM_*  */
    } else if (!_stricmp(cls, "Edit")) {
        if (msg16 <= 0x0422) return msg16 - 0x0350;          /* EM_*  */
    } else if (!_stricmp(cls, "ListBox")) {
        if (msg16 >= 0x0401 && msg16 <= 0x0427)
            return msg16 - 0x0281;                           /* LB_*  */
    } else if (!_stricmp(cls, "ComboBox")) {
        if (msg16 <= 0x0421) return msg16 - 0x02C0;          /* CB_*  */
    } else if (!_stricmp(cls, "ScrollBar")) {
        if (msg16 <= 0x0409) return msg16 - 0x0320;          /* SBM_* */
    } else if (!_stricmp(cls, "Static")) {
        if (msg16 <= 0x0402) return msg16 - 0x0290;          /* STM_* */
    }
    return msg16;
}

/* ---- marshaling ----------------------------------------------------------- */

/* What has to be moved back into guest memory once the real window has run. */
enum { OUT_NONE = 0, OUT_STR, OUT_RECT, OUT_LINE, OUT_INTS };

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
    char     buf[BUFSZ];
    RECT     rect;
    INT      tabs[64];
    INT      ints[256];
};

/* A message whose lParam is a string the guest is handing in. */
static int lp_is_in_string(UINT msg)
{
    switch (msg) {
    case WM_SETTEXT:
    case EM_REPLACESEL:
    case LB_ADDSTRING: case LB_INSERTSTRING: case LB_FINDSTRING:
    case LB_SELECTSTRING: case LB_FINDSTRINGEXACT: case LB_DIR:
    case CB_ADDSTRING: case CB_INSERTSTRING: case CB_FINDSTRING:
    case CB_SELECTSTRING: case CB_FINDSTRINGEXACT: case CB_DIR:
        return 1;
    default:
        return 0;
    }
}

static void marshal_in(HWND hwnd, uint16_t msg16, uint16_t wp, uint32_t lp,
                       struct marshal *m)
{
    memset(m, 0, sizeof *m);
    m->msg = msg16_to_32_for(hwnd, msg16);
    m->wp  = wp;
    m->lp  = (LPARAM)lp;

    if (lp_is_in_string(m->msg)) {
        g_str(lp, m->buf, sizeof m->buf);
        m->lp = (LPARAM)m->buf;
        return;
    }

    switch (m->msg) {

    /* ---- text coming back out --------------------------------------------- */

    case WM_GETTEXT:
    case LB_GETTEXT:
    case CB_GETLBTEXT:
        /* WM_GETTEXT is bounded by wParam; the listbox and combobox forms are
           not bounded at all, which is a hazard in Win16 just as much as here -
           the caller is expected to have asked for the length first. */
        m->out    = OUT_STR;
        m->outp   = lp;
        m->outmax = (m->msg == WM_GETTEXT) ? (wp ? wp : 1) : sizeof m->buf;
        if (m->outmax > sizeof m->buf) m->outmax = sizeof m->buf;
        m->wp = (m->msg == WM_GETTEXT) ? m->outmax : wp;
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
        break;
    }

    /* ---- handles in wParam ------------------------------------------------- */

    case WM_SETFONT:
        m->wp = (WPARAM)h32(H_FONT, wp);
        break;

    case WM_GETFONT:
        m->ret = H_FONT;
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
        m->wp = (WPARAM)HWND_32(wp);
        break;

    case WM_INITMENU:
    case WM_INITMENUPOPUP:
        m->wp = (WPARAM)HMENU_32(wp);
        break;

    case WM_QUERYDRAGICON:
        m->ret = H_ICON;
        break;

    /* ---- the pairs whose two parameters swap round ------------------------- */

    case WM_COMMAND:
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
        m->wp = MAKEWPARAM(wp, HIWORD(lp));
        m->lp = (LPARAM)HMENU_32(LOWORD(lp));
        break;

    /* ---- one Win16 message fanning out into seven -------------------------- */

    case 0x0019: {                                  /* WM_CTLCOLOR */
        unsigned type = HIWORD(lp);
        if (type > 6) type = 6;
        m->msg = WM_CTLCOLORMSGBOX + type;
        m->wp  = (WPARAM)HDC_32(wp);
        m->lp  = (LPARAM)HWND_32(LOWORD(lp));
        m->ret = H_BRUSH;
        break;
    }

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

static uint32_t marshal_out(struct marshal *m, LRESULT r)
{
    switch (m->out) {
    case OUT_STR:
        g_puts(m->outp, m->buf, m->outmax);
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
        if (n > m->outmax) n = m->outmax;
        for (i = 0; i < n; i++)
            sel_wr16(SEGPTR_SEL(m->outp),
                     (uint16_t)(SEGPTR_OFF(m->outp) + i * 2),
                     (uint16_t)m->ints[i]);
        break;
    }
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

uint32_t msg16_send(HWND hwnd, uint16_t msg16, uint16_t wp, uint32_t lp)
{
    struct marshal m;
    LRESULT r;

    marshal_in(hwnd, msg16, wp, lp, &m);
    if (m.refuse) {
        log_msg("*** guest sent message %04X (Win32 %04X) by hand; its lParam is "
                "a 16-bit struct with no translation on this path\n", msg16, m.msg);
        return 0;
    }
    r = SendMessageA(hwnd, m.msg, m.wp, m.lp);
    return marshal_out(&m, r);
}

uint32_t msg16_post(HWND hwnd, uint16_t msg16, uint16_t wp, uint32_t lp)
{
    struct marshal m;

    marshal_in(hwnd, msg16, wp, lp, &m);

    /* A post outlives this call, so anything that had to be copied into a local
       buffer cannot go out this way - the buffer is gone before the message is
       read.  Send it instead; that changes the timing, which is the lesser of
       the two wrongs. */
    if (m.refuse || m.out != OUT_NONE || lp_is_in_string(m.msg)) {
        log_msg("*** guest posted message %04X, which carries a pointer; "
                "sending it instead so the data is still there\n", msg16);
        return msg16_send(hwnd, msg16, wp, lp);
    }
    return (uint32_t)PostMessageA(hwnd, m.msg, m.wp, m.lp);
}
