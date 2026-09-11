/* api_gdi.c - the 41 GDI entry points Stars! imports.
 *
 * Most are a handle map away from their Win32 counterpart.  The ones that are
 * not, and that will silently corrupt drawing if got wrong:
 *
 *  - Nine of them return a DWORD in DX:AX rather than a word in AX, and four of
 *    those pack a POINT or SIZE that Win32 returns through an out-parameter:
 *    MoveTo, SetWindowOrg, SetBrushOrg and GetTextExtent.  They return 0 on
 *    failure rather than a packed value.
 *  - TEXTMETRIC16 is 31 bytes, its field ORDER differs from Win32 (not just the
 *    widths), and tmOverhang sits at the odd offset 0x19.
 *  - GDI.128 MulDiv is not Win32 MulDiv: it is a 16-bit-clamped variant.
 *  - GetDeviceCaps must report 2048 colours where the host says -1.
 *  - CreateRectRgn with left >= right yields an empty region rather than failing.
 */

#include "thunk.h"
#include "handle.h"
#include "task.h"
#include "sel.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

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

/* A GDI object handle of any kind. */
static HGDIOBJ obj32(uint16_t h) { return (HGDIOBJ)h32(H_GDIOBJ, h); }

/* ---- device context and state -------------------------------------------- */

static uint32_t g_SetBkColor(Cpu *c, Args *a)
{
    HDC dc = HDC_32(arg_word(a));
    COLORREF col = arg_long(a);
    (void)c;
    return SetBkColor(dc, col);
}

static uint32_t g_GetBkColor(Cpu *c, Args *a)
{
    (void)c;
    return GetBkColor(HDC_32(arg_word(a)));
}

static uint32_t g_SetBkMode(Cpu *c, Args *a)
{
    HDC dc = HDC_32(arg_word(a));
    (void)c;
    return (uint32_t)SetBkMode(dc, arg_sword(a));
}

static uint32_t g_SetROP2(Cpu *c, Args *a)
{
    HDC dc = HDC_32(arg_word(a));
    (void)c;
    return (uint32_t)SetROP2(dc, arg_sword(a));
}

static uint32_t g_GetROP2(Cpu *c, Args *a)
{
    (void)c;
    return (uint32_t)GetROP2(HDC_32(arg_word(a)));
}

static uint32_t g_SetTextColor(Cpu *c, Args *a)
{
    HDC dc = HDC_32(arg_word(a));
    COLORREF col = arg_long(a);
    (void)c;
    return SetTextColor(dc, col);
}

static uint32_t g_SetWindowOrg(Cpu *c, Args *a)
{
    HDC dc = HDC_32(arg_word(a));
    int x = arg_sword(a), y = arg_sword(a);
    POINT p;
    (void)c;
    if (!SetWindowOrgEx(dc, x, y, &p)) return 0;
    return (uint32_t)MAKELONG(p.x, p.y);
}

static uint32_t g_SetBrushOrg(Cpu *c, Args *a)
{
    HDC dc = HDC_32(arg_word(a));
    int x = arg_sword(a), y = arg_sword(a);
    POINT p;
    (void)c;
    if (!SetBrushOrgEx(dc, x, y, &p)) return 0;
    return (uint32_t)MAKELONG(p.x, p.y);
}

static uint32_t g_MoveTo(Cpu *c, Args *a)
{
    HDC dc = HDC_32(arg_word(a));
    int x = arg_sword(a), y = arg_sword(a);
    POINT p;
    (void)c;
    if (!MoveToEx(dc, x, y, &p)) return 0;
    return (uint32_t)MAKELONG(p.x, p.y);
}

static uint32_t g_LineTo(Cpu *c, Args *a)
{
    HDC dc = HDC_32(arg_word(a));
    int x = arg_sword(a), y = arg_sword(a);
    (void)c;
    return (uint32_t)LineTo(dc, x, y);
}

static uint32_t g_Rectangle(Cpu *c, Args *a)
{
    HDC dc = HDC_32(arg_word(a));
    int l = arg_sword(a), t = arg_sword(a), r = arg_sword(a), b = arg_sword(a);
    (void)c;
    return (uint32_t)Rectangle(dc, l, t, r, b);
}

static uint32_t g_Ellipse(Cpu *c, Args *a)
{
    HDC dc = HDC_32(arg_word(a));
    int l = arg_sword(a), t = arg_sword(a), r = arg_sword(a), b = arg_sword(a);
    (void)c;
    return (uint32_t)Ellipse(dc, l, t, r, b);
}

static uint32_t g_PatBlt(Cpu *c, Args *a)
{
    HDC dc = HDC_32(arg_word(a));
    int x = arg_sword(a), y = arg_sword(a), w = arg_sword(a), h = arg_sword(a);
    DWORD rop = arg_long(a);
    (void)c;
    return (uint32_t)PatBlt(dc, x, y, w, h, rop);
}

static uint32_t g_SetPixel(Cpu *c, Args *a)
{
    HDC dc = HDC_32(arg_word(a));
    int x = arg_sword(a), y = arg_sword(a);
    COLORREF col = arg_long(a);
    (void)c;
    return SetPixel(dc, x, y, col);
}

static uint32_t g_TextOut(Cpu *c, Args *a)
{
    char buf[1024];
    HDC dc = HDC_32(arg_word(a));
    int x = arg_sword(a), y = arg_sword(a);
    uint32_t s = arg_long(a);
    int len = arg_sword(a);
    (void)c;
    gstr(s, buf, sizeof buf);
    if (len < 0 || (size_t)len > sizeof buf) len = (int)strlen(buf);
    return (uint32_t)TextOutA(dc, x, y, buf, len);
}

static uint32_t g_ExtTextOut(Cpu *c, Args *a)
{
    char buf[1024];
    HDC dc = HDC_32(arg_word(a));
    int x = arg_sword(a), y = arg_sword(a);
    uint16_t flags = arg_word(a);
    uint32_t rp = arg_long(a);
    uint32_t s = arg_long(a);
    uint16_t len = arg_word(a);
    uint32_t dxp = arg_long(a);
    RECT r;
    INT *dx = NULL;
    BOOL ok;

    (void)c;
    gstr(s, buf, sizeof buf);
    if (len > sizeof buf - 1) len = sizeof buf - 1;
    if (rp) {
        uint16_t sel = SEGPTR_SEL(rp), off = SEGPTR_OFF(rp);
        r.left   = (int16_t)sel_rd16(sel, off);
        r.top    = (int16_t)sel_rd16(sel, (uint16_t)(off + 2));
        r.right  = (int16_t)sel_rd16(sel, (uint16_t)(off + 4));
        r.bottom = (int16_t)sel_rd16(sel, (uint16_t)(off + 6));
    }
    if (dxp && len) {
        /* The spacing array is INT16 in Win16 and INT in Win32. */
        uint16_t sel = SEGPTR_SEL(dxp), off = SEGPTR_OFF(dxp);
        unsigned i;
        dx = malloc(sizeof(INT) * len);
        if (dx)
            for (i = 0; i < len; i++)
                dx[i] = (int16_t)sel_rd16(sel, (uint16_t)(off + i * 2));
    }
    ok = ExtTextOutA(dc, x, y, flags, rp ? &r : NULL, buf, len, dx);
    free(dx);
    return (uint32_t)ok;
}

static uint32_t g_BitBlt(Cpu *c, Args *a)
{
    HDC dst = HDC_32(arg_word(a));
    int x = arg_sword(a), y = arg_sword(a), w = arg_sword(a), h = arg_sword(a);
    HDC src = HDC_32(arg_word(a));
    int sx = arg_sword(a), sy = arg_sword(a);
    DWORD rop = arg_long(a);
    (void)c;
    return (uint32_t)BitBlt(dst, x, y, w, h, src, sx, sy, rop);
}

static uint32_t g_ExcludeClipRect(Cpu *c, Args *a)
{
    HDC dc = HDC_32(arg_word(a));
    int l = arg_sword(a), t = arg_sword(a), r = arg_sword(a), b = arg_sword(a);
    (void)c;
    return (uint32_t)ExcludeClipRect(dc, l, t, r, b);
}

static uint32_t g_IntersectClipRect(Cpu *c, Args *a)
{
    HDC dc = HDC_32(arg_word(a));
    int l = arg_sword(a), t = arg_sword(a), r = arg_sword(a), b = arg_sword(a);
    (void)c;
    return (uint32_t)IntersectClipRect(dc, l, t, r, b);
}

static uint32_t g_SelectClipRgn(Cpu *c, Args *a)
{
    HDC dc = HDC_32(arg_word(a));
    HRGN rgn = (HRGN)h32(H_RGN, arg_word(a));
    (void)c;
    return (uint32_t)SelectClipRgn(dc, rgn);
}

static uint32_t g_SelectObject(Cpu *c, Args *a)
{
    HDC dc = HDC_32(arg_word(a));
    uint16_t h = arg_word(a);
    HGDIOBJ prev;
    DWORD type;

    (void)c;
    prev = SelectObject(dc, obj32(h));
    if (!prev) return 0;
    type = GetObjectType(prev);
    switch (type) {
    case OBJ_BITMAP:  return h16(H_BITMAP, prev);
    case OBJ_BRUSH:   return h16(H_BRUSH, prev);
    case OBJ_PEN:
    case OBJ_EXTPEN:  return h16(H_PEN, prev);
    case OBJ_FONT:    return h16(H_FONT, prev);
    case OBJ_PAL:     return h16(H_PALETTE, prev);
    case OBJ_REGION:  return h16(H_RGN, prev);
    default:          return h16(H_GDIOBJ, prev);
    }
}

static uint32_t g_CreateCompatibleDC(Cpu *c, Args *a)
{
    (void)c;
    return HDC_16(CreateCompatibleDC(HDC_32(arg_word(a))));
}

static uint32_t g_DeleteDC(Cpu *c, Args *a)
{
    uint16_t h = arg_word(a);
    HDC dc = HDC_32(h);
    BOOL r;
    (void)c;
    r = DeleteDC(dc);
    h_release(h);
    return (uint32_t)r;
}

static uint32_t g_CreateCompatibleBitmap(Cpu *c, Args *a)
{
    HDC dc = HDC_32(arg_word(a));
    int w = arg_sword(a), h = arg_sword(a);
    (void)c;
    return h16(H_BITMAP, CreateCompatibleBitmap(dc, w, h));
}

static uint32_t g_CreatePen(Cpu *c, Args *a)
{
    int style = arg_sword(a), width = arg_sword(a);
    COLORREF col = arg_long(a);
    LOGPEN lp;
    (void)c;
    if (style > PS_INSIDEFRAME) return 0;
    lp.lopnStyle = (UINT)style;
    lp.lopnWidth.x = width;
    lp.lopnWidth.y = 0;
    lp.lopnColor = col;
    return h16(H_PEN, CreatePenIndirect(&lp));
}

static uint32_t g_CreateSolidBrush(Cpu *c, Args *a)
{
    (void)c;
    return h16(H_BRUSH, CreateSolidBrush(arg_long(a)));
}

static uint32_t g_CreatePatternBrush(Cpu *c, Args *a)
{
    HBITMAP bm = (HBITMAP)h32(H_BITMAP, arg_word(a));
    (void)c;
    return h16(H_BRUSH, CreatePatternBrush(bm));
}

static uint32_t g_CreateRectRgn(Cpu *c, Args *a)
{
    int l = arg_sword(a), t = arg_sword(a), r = arg_sword(a), b = arg_sword(a);
    (void)c;
    /* Win16 yields an empty region rather than failing on an inverted rect, and
       real code relies on it. */
    if (l >= r) { l = t = r = b = 0; }
    return h16(H_RGN, CreateRectRgn(l, t, r, b));
}

/* LOGFONT16 is 50 bytes; the face name occupies the last 32. */
static uint32_t g_CreateFontIndirect(Cpu *c, Args *a)
{
    uint32_t p = arg_long(a);
    uint16_t sel = SEGPTR_SEL(p), off = SEGPTR_OFF(p);
    LOGFONTA lf;
    unsigned i;

    (void)c;
    if (!p) return 0;
    memset(&lf, 0, sizeof lf);
    lf.lfHeight         = (int16_t)sel_rd16(sel, off);
    lf.lfWidth          = (int16_t)sel_rd16(sel, (uint16_t)(off + 2));
    lf.lfEscapement     = (int16_t)sel_rd16(sel, (uint16_t)(off + 4));
    lf.lfOrientation    = (int16_t)sel_rd16(sel, (uint16_t)(off + 6));
    lf.lfWeight         = (int16_t)sel_rd16(sel, (uint16_t)(off + 8));
    lf.lfItalic         = sel_rd8(sel, (uint16_t)(off + 10));
    lf.lfUnderline      = sel_rd8(sel, (uint16_t)(off + 11));
    lf.lfStrikeOut      = sel_rd8(sel, (uint16_t)(off + 12));
    lf.lfCharSet        = sel_rd8(sel, (uint16_t)(off + 13));
    lf.lfOutPrecision   = sel_rd8(sel, (uint16_t)(off + 14));
    lf.lfClipPrecision  = sel_rd8(sel, (uint16_t)(off + 15));
    lf.lfQuality        = sel_rd8(sel, (uint16_t)(off + 16));
    lf.lfPitchAndFamily = sel_rd8(sel, (uint16_t)(off + 17));
    for (i = 0; i < LF_FACESIZE - 1; i++) {
        uint8_t ch = sel_rd8(sel, (uint16_t)(off + 18 + i));
        lf.lfFaceName[i] = (CHAR)ch;
        if (!ch) break;
    }
    lf.lfFaceName[LF_FACESIZE - 1] = 0;
    return h16(H_FONT, CreateFontIndirectA(&lf));
}

static uint32_t g_DeleteObject(Cpu *c, Args *a)
{
    uint16_t h = arg_word(a);
    BOOL r;
    (void)c;
    r = DeleteObject(obj32(h));
    h_release(h);
    return (uint32_t)r;
}

static uint32_t g_GetStockObject(Cpu *c, Args *a)
{
    int idx = arg_sword(a);
    HGDIOBJ o;
    (void)c;
    o = GetStockObject(idx);
    if (!o) return 0;
    switch (GetObjectType(o)) {
    case OBJ_BRUSH: return h16(H_BRUSH, o);
    case OBJ_PEN:   return h16(H_PEN, o);
    case OBJ_FONT:  return h16(H_FONT, o);
    case OBJ_PAL:   return h16(H_PALETTE, o);
    default:        return h16(H_GDIOBJ, o);
    }
}

static uint32_t g_GetDeviceCaps(Cpu *c, Args *a)
{
    HDC dc = HDC_32(arg_word(a));
    int cap = arg_sword(a);
    int r;
    (void)c;
    r = GetDeviceCaps(dc, cap);
    /* A palette-less modern display reports -1 colours, which a 1995 program
       does not expect; real Win16 shims report 2048. */
    if (cap == NUMCOLORS && r == -1) r = 2048;
    return (uint32_t)(int16_t)r;
}

/* BITMAP16 is 14 bytes, and bmPlanes/bmBitsPixel are BYTEs where Win32 uses
   WORDs.  bmBits is always handed back as zero: a 16-bit caller has no use for
   a host pointer. */
static uint32_t g_GetObject(Cpu *c, Args *a)
{
    uint16_t h = arg_word(a);
    int count = arg_sword(a);
    uint32_t p = arg_long(a);
    uint16_t sel = SEGPTR_SEL(p), off = SEGPTR_OFF(p);
    HGDIOBJ o = obj32(h);

    (void)c;
    if (!o || !p || count <= 0) return 0;
    switch (GetObjectType(o)) {
    case OBJ_BITMAP: {
        BITMAP bm;
        if (count < 14 || !GetObjectA(o, sizeof bm, &bm)) return 0;
        sel_wr16(sel, off,               (uint16_t)bm.bmType);
        sel_wr16(sel, (uint16_t)(off+2), (uint16_t)bm.bmWidth);
        sel_wr16(sel, (uint16_t)(off+4), (uint16_t)bm.bmHeight);
        sel_wr16(sel, (uint16_t)(off+6), (uint16_t)bm.bmWidthBytes);
        sel_wr8 (sel, (uint16_t)(off+8), (uint8_t)bm.bmPlanes);
        sel_wr8 (sel, (uint16_t)(off+9), (uint8_t)bm.bmBitsPixel);
        sel_wr32(sel, (uint16_t)(off+10), 0);
        return 14;
    }
    case OBJ_PEN: {
        LOGPEN lp;
        if (count < 10 || !GetObjectA(o, sizeof lp, &lp)) return 0;
        sel_wr16(sel, off,               (uint16_t)lp.lopnStyle);
        sel_wr16(sel, (uint16_t)(off+2), (uint16_t)lp.lopnWidth.x);
        sel_wr16(sel, (uint16_t)(off+4), (uint16_t)lp.lopnWidth.y);
        sel_wr32(sel, (uint16_t)(off+6), lp.lopnColor);
        return 10;
    }
    case OBJ_BRUSH: {
        LOGBRUSH lb;
        if (count < 8 || !GetObjectA(o, sizeof lb, &lb)) return 0;
        sel_wr16(sel, off,               (uint16_t)lb.lbStyle);
        sel_wr32(sel, (uint16_t)(off+2), lb.lbColor);
        sel_wr16(sel, (uint16_t)(off+6), (uint16_t)lb.lbHatch);
        return 8;
    }
    default:
        log_msg("GetObject: unsupported object type for handle %04X\n", h);
        return 0;
    }
}

/* TEXTMETRIC16: 31 bytes, and the field ORDER differs from Win32 - tmWeight
   comes before the byte block, and tmOverhang sits at the odd offset 0x19. */
static uint32_t g_GetTextMetrics(Cpu *c, Args *a)
{
    HDC dc = HDC_32(arg_word(a));
    uint32_t p = arg_long(a);
    uint16_t sel = SEGPTR_SEL(p), off = SEGPTR_OFF(p);
    TEXTMETRICA tm;

    (void)c;
    if (!GetTextMetricsA(dc, &tm)) return 0;
    sel_wr16(sel, (uint16_t)(off + 0x00), (uint16_t)tm.tmHeight);
    sel_wr16(sel, (uint16_t)(off + 0x02), (uint16_t)tm.tmAscent);
    sel_wr16(sel, (uint16_t)(off + 0x04), (uint16_t)tm.tmDescent);
    sel_wr16(sel, (uint16_t)(off + 0x06), (uint16_t)tm.tmInternalLeading);
    sel_wr16(sel, (uint16_t)(off + 0x08), (uint16_t)tm.tmExternalLeading);
    sel_wr16(sel, (uint16_t)(off + 0x0A), (uint16_t)tm.tmAveCharWidth);
    sel_wr16(sel, (uint16_t)(off + 0x0C), (uint16_t)tm.tmMaxCharWidth);
    sel_wr16(sel, (uint16_t)(off + 0x0E), (uint16_t)tm.tmWeight);
    sel_wr8 (sel, (uint16_t)(off + 0x10), tm.tmItalic);
    sel_wr8 (sel, (uint16_t)(off + 0x11), tm.tmUnderlined);
    sel_wr8 (sel, (uint16_t)(off + 0x12), tm.tmStruckOut);
    sel_wr8 (sel, (uint16_t)(off + 0x13), tm.tmFirstChar);
    sel_wr8 (sel, (uint16_t)(off + 0x14), tm.tmLastChar);
    sel_wr8 (sel, (uint16_t)(off + 0x15), tm.tmDefaultChar);
    sel_wr8 (sel, (uint16_t)(off + 0x16), tm.tmBreakChar);
    sel_wr8 (sel, (uint16_t)(off + 0x17), tm.tmPitchAndFamily);
    sel_wr8 (sel, (uint16_t)(off + 0x18), tm.tmCharSet);
    sel_wr16(sel, (uint16_t)(off + 0x19), (uint16_t)tm.tmOverhang);
    sel_wr16(sel, (uint16_t)(off + 0x1B), (uint16_t)tm.tmDigitizedAspectX);
    sel_wr16(sel, (uint16_t)(off + 0x1D), (uint16_t)tm.tmDigitizedAspectY);
    return 1;
}

static uint32_t g_GetTextExtent(Cpu *c, Args *a)
{
    char buf[1024];
    HDC dc = HDC_32(arg_word(a));
    uint32_t s = arg_long(a);
    int len = arg_sword(a);
    SIZE sz;

    (void)c;
    gstr(s, buf, sizeof buf);
    if (len < 0 || (size_t)len > sizeof buf) len = (int)strlen(buf);
    if (!GetTextExtentPoint32A(dc, buf, len, &sz)) return 0;
    return (uint32_t)MAKELONG(sz.cx, sz.cy);
}

/* GDI.128 MulDiv is NOT the Win32 MulDiv: it clamps to 16 bits and returns
   -32768 on overflow or a zero divisor. */
static uint32_t g_MulDiv(Cpu *c, Args *a)
{
    int m1 = arg_sword(a), m2 = arg_sword(a), d = arg_sword(a);
    int32_t r;

    (void)c;
    if (d == 0) return (uint32_t)(int16_t)-32768;
    r = (int32_t)m1 * m2;
    r = (r + (d / 2) * ((r < 0) ? -1 : 1)) / d;
    if (r > 32767 || r < -32768) return (uint32_t)(int16_t)-32768;
    return (uint32_t)(int16_t)r;
}

static uint32_t g_UnrealizeObject(Cpu *c, Args *a)
{
    (void)c;
    return (uint32_t)UnrealizeObject(obj32(arg_word(a)));
}

static uint32_t g_CreatePalette(Cpu *c, Args *a)
{
    /* LOGPALETTE is layout-identical in Win16 and Win32, so this only needs
       the entries copied out of guest memory. */
    uint32_t p = arg_long(a);
    uint16_t sel = SEGPTR_SEL(p), off = SEGPTR_OFF(p);
    LOGPALETTE *lp;
    uint16_t n;
    HPALETTE pal;
    unsigned i;

    (void)c;
    if (!p) return 0;
    n = sel_rd16(sel, (uint16_t)(off + 2));
    if (!n || n > 1024) return 0;
    lp = malloc(sizeof(LOGPALETTE) + (size_t)n * sizeof(PALETTEENTRY));
    if (!lp) return 0;
    lp->palVersion = sel_rd16(sel, off);
    lp->palNumEntries = n;
    for (i = 0; i < n; i++) {
        uint16_t e = (uint16_t)(off + 4 + i * 4);
        lp->palPalEntry[i].peRed   = sel_rd8(sel, e);
        lp->palPalEntry[i].peGreen = sel_rd8(sel, (uint16_t)(e + 1));
        lp->palPalEntry[i].peBlue  = sel_rd8(sel, (uint16_t)(e + 2));
        lp->palPalEntry[i].peFlags = sel_rd8(sel, (uint16_t)(e + 3));
    }
    pal = CreatePalette(lp);
    free(lp);
    return h16(H_PALETTE, pal);
}

/* ---- device-independent bitmaps ------------------------------------------- */

/* Copy a BITMAPINFO out of guest memory into a host buffer.
   Two things bite here:
     - the colour table follows the DECLARED header size, not sizeof(BITMAPINFOHEADER);
     - with DIB_PAL_COLORS the table holds WORD palette indices, not RGBQUADs,
       so they are resolved against the DC's palette here and the caller then
       uses DIB_RGB_COLORS.  That avoids depending on the host's handling of
       palette indices, which varies. */
static BITMAPINFO *read_bitmapinfo(uint32_t p, HDC dc, uint16_t *usage,
                                   uint32_t *image_bytes)
{
    uint16_t sel = SEGPTR_SEL(p), off = SEGPTR_OFF(p);
    uint32_t hdrsize;
    BITMAPINFO *bi;
    uint32_t ncolors, i;
    LONG width, height;
    WORD bits;

    if (!p) return NULL;
    hdrsize = sel_rd32(sel, off);

    bi = calloc(1, sizeof(BITMAPINFOHEADER) + 256 * sizeof(RGBQUAD));
    if (!bi) return NULL;
    bi->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);

    if (hdrsize == 12) {                      /* BITMAPCOREHEADER */
        width  = (int16_t)sel_rd16(sel, (uint16_t)(off + 4));
        height = (int16_t)sel_rd16(sel, (uint16_t)(off + 6));
        bi->bmiHeader.biPlanes   = sel_rd16(sel, (uint16_t)(off + 8));
        bits = sel_rd16(sel, (uint16_t)(off + 10));
        bi->bmiHeader.biBitCount = bits;
        bi->bmiHeader.biCompression = BI_RGB;
        ncolors = (bits <= 8) ? (1u << bits) : 0;
    } else {
        width  = (LONG)sel_rd32(sel, (uint16_t)(off + 4));
        height = (LONG)sel_rd32(sel, (uint16_t)(off + 8));
        bi->bmiHeader.biPlanes   = sel_rd16(sel, (uint16_t)(off + 12));
        bits = sel_rd16(sel, (uint16_t)(off + 14));
        bi->bmiHeader.biBitCount = bits;
        bi->bmiHeader.biCompression = sel_rd32(sel, (uint16_t)(off + 16));
        bi->bmiHeader.biSizeImage  = sel_rd32(sel, (uint16_t)(off + 20));
        bi->bmiHeader.biXPelsPerMeter = (LONG)sel_rd32(sel, (uint16_t)(off + 24));
        bi->bmiHeader.biYPelsPerMeter = (LONG)sel_rd32(sel, (uint16_t)(off + 28));
        bi->bmiHeader.biClrUsed      = sel_rd32(sel, (uint16_t)(off + 32));
        bi->bmiHeader.biClrImportant = sel_rd32(sel, (uint16_t)(off + 36));
        ncolors = bi->bmiHeader.biClrUsed ? bi->bmiHeader.biClrUsed
                                          : ((bits <= 8) ? (1u << bits) : 0);
    }
    bi->bmiHeader.biWidth  = width;
    bi->bmiHeader.biHeight = height;
    if (ncolors > 256) ncolors = 256;

    if (*usage == DIB_PAL_COLORS) {
        PALETTEENTRY pe[256];
        UINT got = 0;
        HPALETTE pal = (HPALETTE)GetCurrentObject(dc, OBJ_PAL);
        if (pal) got = GetPaletteEntries(pal, 0, 256, pe);
        for (i = 0; i < ncolors; i++) {
            uint16_t idx = sel_rd16(sel, (uint16_t)(off + hdrsize + i * 2));
            if (got) {
                /* The hardware wraps an out-of-range index rather than failing. */
                const PALETTEENTRY *e = &pe[idx % got];
                bi->bmiColors[i].rgbRed   = e->peRed;
                bi->bmiColors[i].rgbGreen = e->peGreen;
                bi->bmiColors[i].rgbBlue  = e->peBlue;
            }
        }
        bi->bmiHeader.biClrUsed = ncolors;
        *usage = DIB_RGB_COLORS;               /* resolved; tell Win32 so */
    } else {
        for (i = 0; i < ncolors; i++) {
            uint16_t e = (uint16_t)(off + hdrsize + i * (hdrsize == 12 ? 3 : 4));
            bi->bmiColors[i].rgbBlue  = sel_rd8(sel, e);
            bi->bmiColors[i].rgbGreen = sel_rd8(sel, (uint16_t)(e + 1));
            bi->bmiColors[i].rgbRed   = sel_rd8(sel, (uint16_t)(e + 2));
        }
    }

    if (image_bytes) {
        LONG h = height < 0 ? -height : height;
        uint32_t stride = ((uint32_t)width * bits + 31) / 32 * 4;
        *image_bytes = stride * (uint32_t)h;
    }
    return bi;
}

static uint32_t g_StretchDIBits(Cpu *c, Args *a)
{
    HDC dc      = HDC_32(arg_word(a));
    int xd = arg_sword(a), yd = arg_sword(a);
    int wd = arg_sword(a), hd = arg_sword(a);
    int xs = arg_sword(a), ys = arg_sword(a);
    int ws = arg_sword(a), hs = arg_sword(a);
    uint32_t bitsp = arg_long(a);
    uint32_t bmip  = arg_long(a);
    uint16_t usage = arg_word(a);
    DWORD rop      = arg_long(a);
    BITMAPINFO *bi;
    uint32_t need = 0;
    int r;

    (void)c;
    bi = read_bitmapinfo(bmip, dc, &usage, &need);
    if (!bi) return 0;
    /* The pixels may span more than one selector for a large bitmap; the arena
       lays a huge block out contiguously, so a single pointer covers it. */
    r = StretchDIBits(dc, xd, yd, wd, hd, xs, ys, ws, hs,
                      bitsp ? sel_ptr(SEGPTR_SEL(bitsp), SEGPTR_OFF(bitsp)) : NULL,
                      bi, usage, rop);
    free(bi);
    return (uint32_t)(int16_t)r;
}

static uint32_t g_GetDIBits(Cpu *c, Args *a)
{
    HDC dc        = HDC_32(arg_word(a));
    uint16_t hbm  = arg_word(a);
    uint16_t start = arg_word(a);
    uint16_t lines = arg_word(a);
    uint32_t bitsp = arg_long(a);
    uint32_t bmip  = arg_long(a);
    uint16_t usage = arg_word(a);
    BITMAPINFO *bi;
    uint32_t need = 0;
    int r;

    (void)c;
    bi = read_bitmapinfo(bmip, dc, &usage, &need);
    if (!bi) return 0;
    r = GetDIBits(dc, (HBITMAP)h32(H_BITMAP, hbm), start, lines,
                  bitsp ? sel_ptr(SEGPTR_SEL(bitsp), SEGPTR_OFF(bitsp)) : NULL,
                  bi, usage);
    /* Copy the header back: the caller uses it to size its buffer. */
    if (bmip) {
        uint16_t sel = SEGPTR_SEL(bmip), off = SEGPTR_OFF(bmip);
        if (sel_rd32(sel, off) != 12) {
            sel_wr32(sel, (uint16_t)(off + 4),  (uint32_t)bi->bmiHeader.biWidth);
            sel_wr32(sel, (uint16_t)(off + 8),  (uint32_t)bi->bmiHeader.biHeight);
            sel_wr16(sel, (uint16_t)(off + 12), bi->bmiHeader.biPlanes);
            sel_wr16(sel, (uint16_t)(off + 14), bi->bmiHeader.biBitCount);
            sel_wr32(sel, (uint16_t)(off + 20), bi->bmiHeader.biSizeImage);
        }
    }
    free(bi);
    return (uint32_t)(int16_t)r;
}

static uint32_t g_Escape(Cpu *c, Args *a)
{
    HDC dc = HDC_32(arg_word(a));
    int esc = arg_sword(a);
    (void)c; (void)dc;
    arg_sword(a);                       /* count */
    arg_long(a);                        /* input */
    arg_long(a);                        /* output */
    /* Printing is out of scope; report the escape as unsupported, which is what
       a driver without that capability does. */
    log_msg("GDI.Escape(%d) is stubbed\n", esc);
    return 0;
}

void api_gdi_register(void)
{
    api_bind("GDI",   1, g_SetBkColor);
    api_bind("GDI",   2, g_SetBkMode);
    api_bind("GDI",   4, g_SetROP2);
    api_bind("GDI",   9, g_SetTextColor);
    api_bind("GDI",  11, g_SetWindowOrg);
    api_bind("GDI",  19, g_LineTo);
    api_bind("GDI",  20, g_MoveTo);
    api_bind("GDI",  21, g_ExcludeClipRect);
    api_bind("GDI",  22, g_IntersectClipRect);
    api_bind("GDI",  24, g_Ellipse);
    api_bind("GDI",  27, g_Rectangle);
    api_bind("GDI",  29, g_PatBlt);
    api_bind("GDI",  31, g_SetPixel);
    api_bind("GDI",  33, g_TextOut);
    api_bind("GDI",  34, g_BitBlt);
    api_bind("GDI",  38, g_Escape);
    api_bind("GDI",  44, g_SelectClipRgn);
    api_bind("GDI",  45, g_SelectObject);
    api_bind("GDI",  51, g_CreateCompatibleBitmap);
    api_bind("GDI",  52, g_CreateCompatibleDC);
    api_bind("GDI",  57, g_CreateFontIndirect);
    api_bind("GDI",  60, g_CreatePatternBrush);
    api_bind("GDI",  61, g_CreatePen);
    api_bind("GDI",  64, g_CreateRectRgn);
    api_bind("GDI",  66, g_CreateSolidBrush);
    api_bind("GDI",  68, g_DeleteDC);
    api_bind("GDI",  69, g_DeleteObject);
    api_bind("GDI",  75, g_GetBkColor);
    api_bind("GDI",  80, g_GetDeviceCaps);
    api_bind("GDI",  82, g_GetObject);
    api_bind("GDI",  85, g_GetROP2);
    api_bind("GDI",  87, g_GetStockObject);
    api_bind("GDI",  91, g_GetTextExtent);
    api_bind("GDI",  93, g_GetTextMetrics);
    api_bind("GDI", 128, g_MulDiv);
    api_bind("GDI", 148, g_SetBrushOrg);
    api_bind("GDI", 150, g_UnrealizeObject);
    api_bind("GDI", 351, g_ExtTextOut);
    api_bind("GDI", 360, g_CreatePalette);
    api_bind("GDI", 439, g_StretchDIBits);
    api_bind("GDI", 441, g_GetDIBits);
}
