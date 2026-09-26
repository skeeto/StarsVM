/* api_res_gui.c - resource data made into Win32 objects: bitmaps and
 * accelerator tables.  The emulator's half of api_res.c; the library has no
 * Win32 to make them in, and src/headless.c answers these calls instead.
 */

#include "thunk.h"
#include "handle.h"
#include "task.h"
#include "sel.h"
#include "log.h"
#include "res.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

/* ---- bitmaps -------------------------------------------------------------- */

/* A Win16 RT_BITMAP is a raw DIB with no file header.  Its first DWORD is the
   header size: 12 means the old BITMAPCOREHEADER, 40 the modern one.  Stars!
   uses 40-byte headers throughout, but the core form is cheap to accept. */
static HBITMAP dib_to_bitmap(const uint8_t *data, uint32_t len)
{
    BITMAPINFOHEADER bih;
    const uint8_t *bits;
    uint32_t hdrsize, palbytes, ncolors;
    BITMAPINFO *bi;
    HBITMAP bm = NULL;
    HDC dc;

    if (len < 12) return NULL;
    hdrsize = (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
              ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);

    memset(&bih, 0, sizeof bih);
    bih.biSize = sizeof bih;
    if (hdrsize == 12) {
        bih.biWidth    = (int16_t)(data[4] | (data[5] << 8));
        bih.biHeight   = (int16_t)(data[6] | (data[7] << 8));
        bih.biPlanes   = (WORD)(data[8] | (data[9] << 8));
        bih.biBitCount = (WORD)(data[10] | (data[11] << 8));
        bih.biCompression = BI_RGB;
        ncolors = (bih.biBitCount <= 8) ? (1u << bih.biBitCount) : 0;
        palbytes = ncolors * 3;             /* RGBTRIPLE in the core form */
    } else if (hdrsize >= 40 && len >= 40) {
        memcpy(&bih, data, sizeof bih);
        bih.biSize = sizeof bih;
        ncolors = bih.biClrUsed ? bih.biClrUsed
                                : ((bih.biBitCount <= 8) ? (1u << bih.biBitCount) : 0);
        palbytes = ncolors * 4;             /* RGBQUAD */
    } else {
        return NULL;
    }

    bi = calloc(1, sizeof(BITMAPINFOHEADER) + 256 * sizeof(RGBQUAD));
    if (!bi) return NULL;
    bi->bmiHeader = bih;
    if (ncolors) {
        uint32_t i;
        const uint8_t *src = data + hdrsize;
        for (i = 0; i < ncolors && i < 256; i++) {
            if (hdrsize == 12) {
                bi->bmiColors[i].rgbBlue  = src[i * 3 + 0];
                bi->bmiColors[i].rgbGreen = src[i * 3 + 1];
                bi->bmiColors[i].rgbRed   = src[i * 3 + 2];
            } else {
                bi->bmiColors[i].rgbBlue  = src[i * 4 + 0];
                bi->bmiColors[i].rgbGreen = src[i * 4 + 1];
                bi->bmiColors[i].rgbRed   = src[i * 4 + 2];
            }
        }
    }
    /* The colour table follows the DECLARED header size, not sizeof(BITMAPINFOHEADER). */
    bits = data + hdrsize + palbytes;
    if ((uint32_t)(bits - data) > len) { free(bi); return NULL; }

    /* A 1bpp resource has to become a MONOCHROME bitmap, not a colour one that
       merely holds black and white pixels, because the two behave differently
       the moment they are blitted.  Copying a monochrome source to a colour
       destination is a conversion: 0 bits take the destination's text colour
       and 1 bits its background colour.  That is how a program of this vintage
       tints one sprite per player, and Stars! does exactly that - SetTextColor,
       SetBkColor, then the classic pair of blits, SRCAND with the mask and
       SRCPAINT with the image.

       CreateDIBitmap against a screen DC hands back a 32bpp bitmap whatever the
       DIB's depth, so the conversion never happens and the black and white get
       copied literally: the star map's fleet markers come out as white
       triangles in black squares instead of the owner's colour.

       The bits are repacked rather than handed straight over, because the two
       layouts disagree twice: a DIB's rows run bottom-up and are DWORD-aligned,
       a monochrome DDB's run top-down and are WORD-aligned. */
    if (bih.biBitCount == 1 && bih.biPlanes == 1) {
        int w = (int)bih.biWidth;
        int rows = bih.biHeight < 0 ? -(int)bih.biHeight : (int)bih.biHeight;
        int topdown = bih.biHeight < 0;
        size_t src_stride = (((size_t)w + 31) / 32) * 4;
        size_t dst_stride = (((size_t)w + 15) / 16) * 2;
        uint8_t *mono;
        int y;

        if (w <= 0 || rows <= 0) { free(bi); return NULL; }
        if ((size_t)(bits - data) + src_stride * (size_t)rows > len) {
            free(bi);
            return NULL;
        }
        mono = calloc((size_t)rows, dst_stride);
        if (!mono) { free(bi); return NULL; }
        for (y = 0; y < rows; y++) {
            const uint8_t *s = bits +
                (size_t)(topdown ? y : rows - 1 - y) * src_stride;
            memcpy(mono + (size_t)y * dst_stride, s, dst_stride);
        }
        bm = CreateBitmap(w, rows, 1, 1, mono);
        free(mono);
        free(bi);
        return bm;
    }

    dc = GetDC(NULL);
    bm = CreateDIBitmap(dc, &bi->bmiHeader, CBM_INIT, bits, bi, DIB_RGB_COLORS);
    ReleaseDC(NULL, dc);
    free(bi);
    return bm;
}

static uint32_t u_LoadBitmap(Cpu *c, Args *a)
{
    uint16_t hinst = arg_word(a);
    uint32_t namep = arg_long(a);
    NeModule *m = task.mod;
    uint32_t name;
    NeResource nr;
    HBITMAP bm;

    (void)c; (void)hinst;
    name = res_key(m, namep, 0);
    if (!name || !ne_find_resource(m, RT16_BITMAP, name, &nr)) {
        log_msg("LoadBitmap: resource %04X not found\n", name);
        return 0;
    }
    if (nr.off + nr.len > m->imglen) return 0;
    bm = dib_to_bitmap(m->img + nr.off, nr.len);
    if (!bm) {
        log_msg("LoadBitmap: could not build a bitmap from resource %04X "
                "(%u bytes)\n", name, nr.len);
        return 0;
    }
    return h16(H_BITMAP, bm);
}

/* ---- accelerators --------------------------------------------------------- */

/* A Win16 RT_ACCELERATOR is a packed array of 5-byte entries:
     BYTE fVirt, WORD key, WORD cmd
   which is field-identical to the Win32 ACCEL struct, so this is a straight
   copy.  Bit 0x80 of fVirt marks the last entry and must be masked off, or it
   would be misread as a flag. */
static uint32_t u_LoadAccelerators(Cpu *c, Args *a)
{
    uint16_t hinst = arg_word(a);
    uint32_t namep = arg_long(a);
    NeModule *m = task.mod;
    uint32_t name;
    NeResource nr;
    ACCEL *acc;
    unsigned count, i;
    HACCEL h;

    (void)c; (void)hinst;
    name = res_key(m, namep, 0);
    if (!name || !ne_find_resource(m, RT16_ACCELERATOR, name, &nr)) {
        log_msg("LoadAccelerators: resource %04X not found\n", name);
        return 0;
    }
    count = nr.len / 5;
    if (!count) return 0;
    acc = calloc(count, sizeof *acc);
    if (!acc) return 0;

    for (i = 0; i < count; i++) {
        const uint8_t *e = m->img + nr.off + i * 5;
        acc[i].fVirt = (BYTE)(e[0] & 0x7F);
        acc[i].key   = (WORD)(e[1] | (e[2] << 8));
        acc[i].cmd   = (WORD)(e[3] | (e[4] << 8));
        if (e[0] & 0x80) { count = i + 1; break; }   /* last entry */
    }
    h = CreateAcceleratorTableA(acc, (int)count);
    free(acc);
    if (!h) {
        log_msg("LoadAccelerators: CreateAcceleratorTable failed: %lu\n",
                GetLastError());
        return 0;
    }
    return h16(H_ACCEL, h);
}

static uint32_t u_TranslateAccelerator(Cpu *c, Args *a)
{
    uint16_t hwnd = arg_word(a);
    uint16_t hacc = arg_word(a);
    uint32_t msgp = arg_long(a);
    uint16_t sel = SEGPTR_SEL(msgp), off = SEGPTR_OFF(msgp);
    MSG msg;

    (void)c;
    /* Only the message, wParam and lParam are used; hwnd comes from the
       argument, not from the MSG. */
    memset(&msg, 0, sizeof msg);
    msg.message = sel_rd16(sel, (uint16_t)(off + 2));
    msg.wParam  = sel_rd16(sel, (uint16_t)(off + 4));
    msg.lParam  = (LPARAM)sel_rd32(sel, (uint16_t)(off + 6));
    return (uint32_t)TranslateAcceleratorA(HWND_32(hwnd),
                                           (HACCEL)h32(H_ACCEL, hacc), &msg);
}

void api_res_gui_register(void)
{
    api_bind("USER",   177, u_LoadAccelerators);
    api_bind("USER",   178, u_TranslateAccelerator);
    api_bind("USER",   175, u_LoadBitmap);
}
