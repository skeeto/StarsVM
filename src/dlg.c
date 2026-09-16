/* dlg.c - dialogs.
 *
 * Three parts: converting a Win16 DLGTEMPLATE into the Win32 form, a bridge
 * that lets a 16-bit DLGPROC be the dialog procedure of a real Win32 dialog,
 * and the ten USER entry points the game calls.
 *
 * Win32 dialog templates are always Unicode, and the item fields are in a
 * different order from Win16 (Win16: x, y, cx, cy, id, style; Win32: style,
 * exstyle, x, y, cx, cy, id), with every item aligned to 4 bytes.  Getting any
 * of that wrong produces a dialog that either fails to create or comes up with
 * scrambled controls, so the converter is written against the templates this
 * binary actually ships - all 36 of them use only the stock control classes,
 * carry no per-item creation data, and name their text with strings rather than
 * ordinals.  The ordinal forms are handled anyway; they cost three lines.
 */

#include "thunk.h"
#include "winproc.h"
#include "handle.h"
#include "task.h"
#include "sel.h"
#include "log.h"
#include "res.h"
#include "gmem.h"
#include "msg16.h"
#include "harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

/* ---- template conversion -------------------------------------------------- */

struct wbuf {
    uint16_t *base, *p, *end;
    int       overflow;
};

static void w_word(struct wbuf *b, uint16_t v)
{
    if (b->p >= b->end) { b->overflow = 1; return; }
    *b->p++ = v;
}

static void w_dword(struct wbuf *b, uint32_t v)
{
    w_word(b, (uint16_t)v);
    w_word(b, (uint16_t)(v >> 16));
}

static void w_align4(struct wbuf *b)
{
    if ((size_t)(b->p - b->base) & 1) w_word(b, 0);
}

/* Copy a NUL-terminated Win16 string, widening it. */
static void w_str(struct wbuf *b, const uint8_t **src, const uint8_t *end)
{
    const uint8_t *s = *src;
    while (s < end && *s) {
        WCHAR wc = 0;
        char ch = (char)*s++;
        MultiByteToWideChar(CP_ACP, 0, &ch, 1, &wc, 1);
        w_word(b, (uint16_t)wc);
    }
    if (s < end) s++;                      /* the terminator */
    w_word(b, 0);
    *src = s;
}

/* Either an 0xFF-prefixed ordinal or a string. */
static void w_sz_or_ord(struct wbuf *b, const uint8_t **src, const uint8_t *end)
{
    const uint8_t *s = *src;
    if (s < end && *s == 0xFF) {
        uint16_t ord = (uint16_t)(s[1] | (s[2] << 8));
        w_word(b, 0xFFFF);
        w_word(b, ord);
        *src = s + 3;
        return;
    }
    w_str(b, src, end);
}

/* Skip a Win16 sz-or-ordinal without emitting anything. */
static const uint8_t *skip_sz_or_ord(const uint8_t *s, const uint8_t *end)
{
    if (s < end && *s == 0xFF) return s + 3 < end ? s + 3 : end;
    while (s < end && *s) s++;
    return s < end ? s + 1 : end;
}

/* How much wider a dialog unit was across than Win32 makes it, as a fraction.
   Both Windows map a dialog unit through the dialog font's average character
   width, and both measure that average the same way - the 52 letters, divided
   by 52 - but they disagree about the last step.  MS Sans Serif 8 point
   measures 323 pixels for the 52, an average of 6.212; Win32's
   GdiGetCharDimensions makes that 6, and Win 3.1 made it 7.

   Seven is what the game's own text says it must be.  The tutor draws its
   prose itself, in Arial sized from LOGPIXELSY and so identical on both, and
   the width it wrapped at under Win 3.1 can be read straight back out of a
   screenshot: ten consecutive line breaks bracket it, in our own font, to
   231-232 pixels, which puts the tutor's client between 253 and 260 and its
   145 units at 1.745 to 1.793 pixels each.  Six gives 1.5 and eight gives 2;
   only seven, at 1.75, is inside.  The height needs no such help - both take
   it from tmHeight, 13 either way - which is why the dialogs were the right
   height and the wrong width, and why only the tutor, the one dialog whose
   text has to wrap, ever ran out of room.

   Returns the numerator and denominator of the correction, 1/1 when there is
   nothing to correct. */
static void hbase_ratio(int pt, const char *face, int *num, int *den)
{
    static const char *alpha =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    LOGFONTA lf;
    HDC dc;
    HFONT f;
    HGDIOBJ old;
    SIZE s;

    *num = *den = 1;
    if (pt <= 0 || !face || !face[0]) return;    /* the system font: no change */
    dc = GetDC(NULL);
    if (!dc) return;
    memset(&lf, 0, sizeof lf);
    lf.lfHeight  = -MulDiv(pt, GetDeviceCaps(dc, LOGPIXELSY), 72);
    lf.lfCharSet = DEFAULT_CHARSET;
    lstrcpynA(lf.lfFaceName, face, LF_FACESIZE);
    f = CreateFontIndirectA(&lf);
    if (f) {
        old = SelectObject(dc, f);
        if (GetTextExtentPoint32A(dc, alpha, 52, &s)) {
            int win32 = (int)((s.cx / 26 + 1) / 2);          /* to nearest */
            int win16 = (int)((s.cx + 51) / 52);             /* rounded up */
            if (win32 > 0 && win16 > win32) { *num = win16; *den = win32; }
        }
        SelectObject(dc, old);
        DeleteObject(f);
    }
    ReleaseDC(NULL, dc);
}

/* Convert the Win16 dialog template at `src` into a Win32 one.  Returns a
   malloc'd block the caller frees, or NULL if the template is malformed. */
static void *dlg16_to_32(const uint8_t *src, uint32_t len)
{
    const uint8_t *p = src, *end = src + len;
    struct wbuf b;
    uint32_t style;
    unsigned cdit, i;
    int hnum = 1, hden = 1;      /* the horizontal unit's Win16 correction */
    size_t words = (size_t)len * 2 + 128;

    b.base = (uint16_t *)calloc(words, sizeof(uint16_t));
    if (!b.base) return NULL;
    b.p = b.base;
    b.end = b.base + words;
    b.overflow = 0;

    if (len < 14) goto bad;

    style = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
            ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    p += 4;
    cdit = *p++;

    w_dword(&b, style);
    /* No WS_EX_APPWINDOW here.  Every one of this game's templates is
       captioned, so gating on the caption would have excluded nothing and given
       all 36 dialog types their own taskbar button and Alt-Tab slot, appearing
       and vanishing on almost every click.  It is not needed either: activating
       a taskbar button whose window is disabled by a modal dialog brings the
       dialog forward, and the one dialog owned by the splash rather than the
       frame is reachable now that the splash has a button of its own. */
    w_dword(&b, 0);                                   /* dwExtendedStyle */
    w_word(&b, (uint16_t)cdit);

    /* The font sits behind three strings and the horizontal geometry in front
       of them, so it has to be read ahead of where it lies. */
    if (style & DS_SETFONT) {
        const uint8_t *q = p + 8;                     /* past x, y, cx, cy */
        q = skip_sz_or_ord(q, end);                   /* menu    */
        q = skip_sz_or_ord(q, end);                   /* class   */
        q = skip_sz_or_ord(q, end);                   /* caption */
        if (q + 2 < end) {
            char face[LF_FACESIZE];
            unsigned k;
            int pt = (int)(q[0] | (q[1] << 8));
            q += 2;
            for (k = 0; k + 1 < LF_FACESIZE && q + k < end && q[k]; k++)
                face[k] = (char)q[k];
            face[k] = 0;
            hbase_ratio(pt, face, &hnum, &hden);
        }
    }

    for (i = 0; i < 4; i++) {                         /* x, y, cx, cy */
        uint16_t v = (uint16_t)(p[0] | (p[1] << 8));
        if (i == 0 || i == 2) v = (uint16_t)MulDiv(v, hnum, hden);
        w_word(&b, v);
        p += 2;
    }

    w_sz_or_ord(&b, &p, end);                         /* menu    */
    w_sz_or_ord(&b, &p, end);                         /* class   */
    w_sz_or_ord(&b, &p, end);                         /* caption */

    if (style & DS_SETFONT) {
        if (p + 2 > end) goto bad;
        w_word(&b, (uint16_t)(p[0] | (p[1] << 8)));   /* point size */
        p += 2;
        w_str(&b, &p, end);                           /* typeface   */
    }

    for (i = 0; i < cdit; i++) {
        uint16_t geom[5];
        uint32_t istyle;
        unsigned extra, k;

        if (p + 14 > end) goto bad;
        for (k = 0; k < 5; k++) {                     /* x, y, cx, cy, id */
            geom[k] = (uint16_t)(p[0] | (p[1] << 8));
            if (k == 0 || k == 2) geom[k] = (uint16_t)MulDiv(geom[k], hnum, hden);
            p += 2;
        }
        istyle = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                 ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        p += 4;

        /* Same accommodation as u_CreateWindow makes for a listbox the game
           creates itself: let the control be exactly the size the template
           asked for instead of shrinking to a whole number of rows.  Win32
           measures integral height against the client rect where Win16 did not,
           so without this a designed-for-Win16 list comes up a row short. */
        if (p < end && *p == 0x83) istyle |= 0x0100;   /* LBS_NOINTEGRALHEIGHT */

        w_align4(&b);
        w_dword(&b, istyle);
        w_dword(&b, 0);                               /* dwExtendedStyle */
        for (k = 0; k < 4; k++) w_word(&b, geom[k]);
        w_word(&b, geom[4]);                          /* id */

        /* The one-byte class ordinals 0x80..0x85 are BUTTON, EDIT, STATIC,
           LISTBOX, SCROLLBAR and COMBOBOX; Win32 spells the same thing as
           0xFFFF followed by the value. */
        if (p < end && (*p & 0x80)) {
            w_word(&b, 0xFFFF);
            w_word(&b, *p++);
        } else {
            w_str(&b, &p, end);
        }

        w_sz_or_ord(&b, &p, end);                     /* item text */

        if (p >= end) goto bad;
        extra = *p++;
        /* Win32 counts creation data in bytes including its own word; zero
           means none, which is every item in this binary. */
        w_word(&b, extra ? (uint16_t)(extra + 2) : 0);
        if (extra) {
            unsigned nwords = (extra + 1) / 2;
            if (p + extra > end) goto bad;
            if (b.p + nwords >= b.end) { b.overflow = 1; goto bad; }
            memcpy(b.p, p, extra);
            b.p += nwords;
            p += extra;
        }
        (void)k;
    }

    if (b.overflow) goto bad;
    return b.base;

bad:
    log_msg("dialog template is malformed (%u bytes)\n", len);
    free(b.base);
    return NULL;
}

/* ---- the dialog procedure bridge ------------------------------------------ */

/* Dialogs are kept apart from the window table on purpose: a dialog's window
   procedure belongs to USER32, and only its DLGPROC is the guest's.  Treating
   the two as the same thing would send messages straight to the guest and skip
   DefDlgProc, which is what makes tab order, default buttons and mnemonics
   work. */
#define MAX_DIALOGS 32
static struct {
    HWND     hwnd;
    uint32_t proc16;
    uint16_t hinstance;
    int      modeless;
} dlgs[MAX_DIALOGS];

static uint32_t dlg_pending_proc;
static uint16_t dlg_pending_inst;
static int      dlg_pending_modeless;

static int dlg_slot(HWND h)
{
    int i, free_slot = -1;
    for (i = 0; i < MAX_DIALOGS; i++) {
        if (dlgs[i].hwnd == h) return i;
        if (!dlgs[i].hwnd && free_slot < 0) free_slot = i;
    }
    return free_slot;
}

uint32_t dlg_proc_get(HWND h)
{
    int i = dlg_slot(h);
    return (i >= 0 && dlgs[i].hwnd == h) ? dlgs[i].proc16 : 0;
}

void dlg_proc_set(HWND h, uint32_t proc16, uint16_t hinst)
{
    int i = dlg_slot(h);
    if (i < 0) { log_msg("dlg: dialog table full\n"); return; }
    dlgs[i].hwnd = h;
    dlgs[i].proc16 = proc16;
    dlgs[i].hinstance = hinst;
}

static INT_PTR CALLBACK dlgproc_bridge(HWND hwnd, UINT msg,
                                       WPARAM wp, LPARAM lp)
{
    uint32_t proc16 = dlg_proc_get(hwnd);
    uint16_t hinst;
    int ret_handle = H_NONE;
    int slot;
    uint32_t r;

    if (!proc16) {
        /* The first message for a dialog we are creating; adopt it. */
        if (!dlg_pending_proc) return FALSE;
        dlg_proc_set(hwnd, dlg_pending_proc, dlg_pending_inst);
        slot = dlg_slot(hwnd);
        if (slot >= 0) dlgs[slot].modeless = dlg_pending_modeless;
        proc16 = dlg_pending_proc;
        dlg_pending_proc = 0;
    }
    slot = dlg_slot(hwnd);
    hinst = (slot >= 0) ? dlgs[slot].hinstance : task.hinstance;

    if (msg == WM_MOUSEWHEEL) return FALSE;

    /* Once a stop is latched no guest instruction can run, so a modal dialog
       would spin forever in USER32's loop with nothing behind it.  Close it. */
    if (cpu_stop_latched()) {
        if (slot >= 0 && !dlgs[slot].modeless) EndDialog(hwnd, 0);
        return TRUE;
    }

    /* One call, and the harness decides what the message means.  Writing the
       pump and the three events out here instead left branches behind that
       changed the release build's instruction scheduling, although every one
       of them compiled to nothing - the same trap as the DrawText hook. */
    harness_dlg(hwnd, msg, wp);

    r = winproc_call16(hwnd, proc16, hinst, msg, wp, lp, &ret_handle);

    /* DialogBox puts a modal dialog on screen whether or not the template asked
       for WS_VISIBLE - that is precisely what separates it from CreateDialog,
       and several templates here rely on it: the ship designer is 0x80C800C0,
       with no WS_VISIBLE, and would otherwise lay itself out, paint itself and
       run its modal loop entirely out of sight. */
    if (msg == WM_INITDIALOG && slot >= 0 && !dlgs[slot].modeless &&
        !IsWindowVisible(hwnd))
        ShowWindow(hwnd, SW_SHOWNORMAL);

    if (msg == WM_NCDESTROY && slot >= 0) memset(&dlgs[slot], 0, sizeof dlgs[0]);

    /* A DLGPROC returns only a handled/not-handled flag - except for the
       messages whose result is the whole point, where DefDlgProc takes the
       return value itself. */
    if (ret_handle != H_NONE)
        return (INT_PTR)winproc_ret_handle(ret_handle, r);
    return (INT_PTR)(uint16_t)r;
}

/* ---- the entry points ----------------------------------------------------- */

/* Look up the template the guest named and convert it.  A Win16 template name
   is a far pointer to a string, or MAKEINTRESOURCE. */
static void *template_for(uint32_t namep, char *label, size_t labelsz)
{
    const uint8_t *raw;
    uint32_t len = 0;

    if (SEGPTR_SEL(namep) == 0)
        snprintf(label, labelsz, "#%u", SEGPTR_OFF(namep));
    else
        g_str(namep, label, labelsz);

    raw = res_locate(RT16_DIALOG, namep, &len);
    if (!raw) {
        log_msg("*** no RT_DIALOG resource named %s\n", label);
        return NULL;
    }
    return dlg16_to_32(raw, len);
}

static uint32_t d_DialogBox(Cpu *c, Args *a)
{
    uint16_t inst  = arg_word(a);
    uint32_t namep = arg_long(a);
    uint16_t parent = arg_word(a);
    uint32_t proc  = arg_long(a);
    char label[64];
    void *tmpl;
    INT_PTR r;

    (void)c;
    tmpl = template_for(namep, label, sizeof label);
    if (!tmpl) return (uint32_t)-1;

    dlg_pending_proc = proc;
    dlg_pending_inst = inst ? inst : task.hinstance;
    dlg_pending_modeless = 0;
    r = DialogBoxIndirectParamW(GetModuleHandleA(NULL),
                                (LPCDLGTEMPLATEW)tmpl, HWND_32(parent),
                                dlgproc_bridge, 0);
    dlg_pending_proc = 0;
    free(tmpl);
    if (r == -1)
        log_msg("*** DialogBox(%s) failed: %lu\n", label, GetLastError());
    return (uint32_t)(int32_t)r;
}

static uint32_t d_CreateDialog(Cpu *c, Args *a)
{
    uint16_t inst  = arg_word(a);
    uint32_t namep = arg_long(a);
    uint16_t parent = arg_word(a);
    uint32_t proc  = arg_long(a);
    char label[64];
    void *tmpl;
    HWND h;

    (void)c;
    tmpl = template_for(namep, label, sizeof label);
    if (!tmpl) return 0;

    dlg_pending_proc = proc;
    dlg_pending_inst = inst ? inst : task.hinstance;
    dlg_pending_modeless = 1;
    h = CreateDialogIndirectParamW(GetModuleHandleA(NULL),
                                   (LPCDLGTEMPLATEW)tmpl, HWND_32(parent),
                                   dlgproc_bridge, 0);
    dlg_pending_proc = 0;
    free(tmpl);
    if (!h) {
        log_msg("*** CreateDialog(%s) failed: %lu\n", label, GetLastError());
        return 0;
    }
    return HWND_16(h);
}

static uint32_t d_EndDialog(Cpu *c, Args *a)
{
    uint16_t hdlg = arg_word(a);
    int16_t  res  = arg_sword(a);
    (void)c;
    return (uint32_t)EndDialog(HWND_32(hdlg), res);
}

static uint32_t d_GetDlgItem(Cpu *c, Args *a)
{
    uint16_t hdlg = arg_word(a);
    int16_t  id   = arg_sword(a);
    (void)c;
    return HWND_16(GetDlgItem(HWND_32(hdlg), id));
}

static uint32_t d_SetDlgItemText(Cpu *c, Args *a)
{
    uint16_t hdlg = arg_word(a);
    int16_t  id   = arg_sword(a);
    uint32_t textp = arg_long(a);
    char buf[1024];
    (void)c;
    g_str(textp, buf, sizeof buf);
    return (uint32_t)SetDlgItemTextA(HWND_32(hdlg), id, buf);
}

static uint32_t d_GetDlgItemText(Cpu *c, Args *a)
{
    uint16_t hdlg = arg_word(a);
    int16_t  id   = arg_sword(a);
    uint32_t bufp = arg_long(a);
    uint16_t max  = arg_word(a);
    char buf[1024];
    UINT n;

    (void)c;
    if (max > sizeof buf) max = sizeof buf;
    if (!max) return 0;
    n = GetDlgItemTextA(HWND_32(hdlg), id, buf, max);
    g_puts(bufp, buf, max);
    return n;
}

static uint32_t d_CheckRadioButton(Cpu *c, Args *a)
{
    uint16_t hdlg  = arg_word(a);
    int16_t  first = arg_sword(a);
    int16_t  last  = arg_sword(a);
    int16_t  check = arg_sword(a);
    (void)c;
    return (uint32_t)CheckRadioButton(HWND_32(hdlg), first, last, check);
}

static uint32_t d_CheckDlgButton(Cpu *c, Args *a)
{
    uint16_t hdlg  = arg_word(a);
    int16_t  id    = arg_sword(a);
    uint16_t check = arg_word(a);
    (void)c;
    return (uint32_t)CheckDlgButton(HWND_32(hdlg), id, check);
}

static uint32_t d_IsDlgButtonChecked(Cpu *c, Args *a)
{
    uint16_t hdlg = arg_word(a);
    int16_t  id   = arg_sword(a);
    (void)c;
    return (uint32_t)IsDlgButtonChecked(HWND_32(hdlg), id);
}

/* The control this reaches is a real Win32 control, so the message number and
   anything it points at both have to be translated - see msg16.c. */
static uint32_t d_SendDlgItemMessage(Cpu *c, Args *a)
{
    uint16_t hdlg = arg_word(a);
    int16_t  id   = arg_sword(a);
    uint16_t msg  = arg_word(a);
    uint16_t wp   = arg_word(a);
    uint32_t lp   = arg_long(a);
    HWND item = GetDlgItem(HWND_32(hdlg), id);

    (void)c;
    if (!item) {
        log_msg("SendDlgItemMessage: dialog %04X has no item %d\n", hdlg, id);
        return 0;
    }
    return msg16_send(item, msg, wp, lp);
}

void api_dlg_register(void)
{
    api_bind("USER",  87, d_DialogBox);
    api_bind("USER",  88, d_EndDialog);
    api_bind("USER",  89, d_CreateDialog);
    api_bind("USER",  91, d_GetDlgItem);
    api_bind("USER",  92, d_SetDlgItemText);
    api_bind("USER",  93, d_GetDlgItemText);
    api_bind("USER",  96, d_CheckRadioButton);
    api_bind("USER",  97, d_CheckDlgButton);
    api_bind("USER",  98, d_IsDlgButtonChecked);
    api_bind("USER", 101, d_SendDlgItemMessage);
}
