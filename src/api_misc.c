/* api_misc.c - COMMDLG.
 *
 * WIN87EM, TOOLHELP and MMSYSTEM moved to api_common.c, being as portable as
 * the interpreter; what is left here is the file and print dialogs.
 */

#include "thunk.h"
#include "handle.h"
#include "task.h"
#include "sel.h"
#include "gmem.h"
#include "log.h"

#include <stdio.h>
#include <string.h>
#include <windows.h>

/* WaveMix lives in audio.c, which reimplements all eleven entries on waveOut. */

/* ---- COMMDLG ------------------------------------------------------------- */

/* OPENFILENAME16 is 72 bytes.  Only the fields the game needs are translated;
   the buffers it passes are its own, written in place through their far
   pointers. */
#define OFN16_hwndOwner      0x04
#define OFN16_lpstrFilter    0x08
#define OFN16_nFilterIndex   0x14
#define OFN16_lpstrFile      0x18
#define OFN16_nMaxFile       0x1C
#define OFN16_lpstrFileTitle 0x20
#define OFN16_nMaxFileTitle  0x24
#define OFN16_lpstrInitialDir 0x28
#define OFN16_lpstrTitle     0x2C
#define OFN16_Flags          0x30
#define OFN16_nFileOffset    0x34
#define OFN16_nFileExtension 0x36
#define OFN16_lpstrDefExt    0x38

/* A filter is a run of NUL-terminated strings ending in an extra NUL, so it
   cannot be copied with a plain string copy. */
static char *gfilter(uint32_t segptr, char *buf, size_t n)
{
    uint16_t sel = SEGPTR_SEL(segptr), off = SEGPTR_OFF(segptr);
    size_t i = 0;
    int zeros = 0;

    if (!segptr) return NULL;
    while (i + 2 < n) {
        uint8_t ch = sel_rd8(sel, (uint16_t)(off + i));
        buf[i++] = (char)ch;
        if (ch == 0) { if (++zeros == 2) break; }
        else zeros = 0;
    }
    buf[i] = 0;
    buf[i + 1] = 0;
    return buf;
}

static uint32_t commdlg_file(Cpu *c, Args *a, int save)
{
    uint32_t p = arg_long(a);
    uint16_t sel = SEGPTR_SEL(p), off = SEGPTR_OFF(p);
    OPENFILENAMEA ofn;
    char filter[512], file[MAX_PATH], title[128], dir[MAX_PATH], deftext[16];
    uint32_t filep, titlep;
    BOOL ok;

    (void)c;
    if (!p) return 0;
    memset(&ofn, 0, sizeof ofn);

    filep  = sel_rd32(sel, (uint16_t)(off + OFN16_lpstrFile));
    titlep = sel_rd32(sel, (uint16_t)(off + OFN16_lpstrFileTitle));

    g_str(filep, file, sizeof file);
    gfilter(sel_rd32(sel, (uint16_t)(off + OFN16_lpstrFilter)),
            filter, sizeof filter);
    g_str(sel_rd32(sel, (uint16_t)(off + OFN16_lpstrInitialDir)), dir, sizeof dir);
    g_str(sel_rd32(sel, (uint16_t)(off + OFN16_lpstrTitle)), title, sizeof title);
    g_str(sel_rd32(sel, (uint16_t)(off + OFN16_lpstrDefExt)), deftext, sizeof deftext);

    /* The Win95-era struct size gets the classic dialog rather than the shell
       one, which is what a program of this vintage expects to be talking to. */
    ofn.lStructSize  = OPENFILENAME_SIZE_VERSION_400;
    ofn.hwndOwner    = HWND_32(sel_rd16(sel, (uint16_t)(off + OFN16_hwndOwner)));
    ofn.lpstrFilter  = filter[0] ? filter : NULL;
    ofn.nFilterIndex = sel_rd32(sel, (uint16_t)(off + OFN16_nFilterIndex));
    ofn.lpstrFile    = file;
    ofn.nMaxFile     = sizeof file;
    ofn.lpstrFileTitle = title;
    ofn.nMaxFileTitle  = sizeof title;
    ofn.lpstrInitialDir = dir[0] ? dir : task.exedir;
    ofn.lpstrDefExt  = deftext[0] ? deftext : NULL;
    ofn.Flags = sel_rd32(sel, (uint16_t)(off + OFN16_Flags)) &
                ~(DWORD)(OFN_ENABLEHOOK | OFN_ENABLETEMPLATE);

    ok = save ? GetSaveFileNameA(&ofn) : GetOpenFileNameA(&ofn);
    if (!ok) return 0;

    /* Write the results back through the guest's own pointers. */
    if (filep) {
        uint16_t fs = SEGPTR_SEL(filep), fo = SEGPTR_OFF(filep);
        uint32_t max = sel_rd32(sel, (uint16_t)(off + OFN16_nMaxFile));
        unsigned i;
        for (i = 0; file[i] && i + 1 < max; i++)
            sel_wr8(fs, (uint16_t)(fo + i), (uint8_t)file[i]);
        sel_wr8(fs, (uint16_t)(fo + i), 0);
    }
    if (titlep) {
        uint16_t ts = SEGPTR_SEL(titlep), to = SEGPTR_OFF(titlep);
        unsigned i;
        for (i = 0; title[i]; i++)
            sel_wr8(ts, (uint16_t)(to + i), (uint8_t)title[i]);
        sel_wr8(ts, (uint16_t)(to + i), 0);
    }
    sel_wr32(sel, (uint16_t)(off + OFN16_nFilterIndex), ofn.nFilterIndex);
    sel_wr16(sel, (uint16_t)(off + OFN16_nFileOffset), ofn.nFileOffset);
    sel_wr16(sel, (uint16_t)(off + OFN16_nFileExtension), ofn.nFileExtension);
    return 1;
}

static uint32_t cd_GetOpenFileName(Cpu *c, Args *a) { return commdlg_file(c, a, 0); }
static uint32_t cd_GetSaveFileName(Cpu *c, Args *a) { return commdlg_file(c, a, 1); }

/* PRINTDLG16 is 52 bytes.  The game passes no hooks and no templates, so only
   the fields it reads back matter.  PD_RETURNDC is the whole point: Stars!
   never imports CreateDC, so the only printer DC it can ever hold is the one
   this call hands back. */
#define PD16_hwndOwner  0x04
#define PD16_hDevMode   0x06
#define PD16_hDevNames  0x08
#define PD16_hDC        0x0A
#define PD16_Flags      0x0C
#define PD16_nFromPage  0x10
#define PD16_nToPage    0x12
#define PD16_nMinPage   0x14
#define PD16_nMaxPage   0x16
#define PD16_nCopies    0x18

static uint32_t cd_PrintDlg(Cpu *c, Args *a)
{
    uint32_t p = arg_long(a);
    uint16_t sel = SEGPTR_SEL(p), off = SEGPTR_OFF(p);
    static HGLOBAL keep_mode, keep_names;
    PRINTDLGA pd;
    uint32_t flags;

    (void)c;
    if (!p) return 0;
    flags = sel_rd32(sel, (uint16_t)(off + PD16_Flags));

    memset(&pd, 0, sizeof pd);
    pd.lStructSize = sizeof pd;
    pd.hwndOwner = HWND_32(sel_rd16(sel, (uint16_t)(off + PD16_hwndOwner)));
    /* The hook and template flags are the only ones that cannot survive: they
       name 16-bit code and resources.  Everything else the game asks for means
       the same thing in Win32, including PD_RETURNDEFAULT - the game's first
       call, which wants the default printer's DC and no dialog at all. */
    pd.Flags = (flags & ~(DWORD)(PD_ENABLEPRINTHOOK | PD_ENABLESETUPHOOK |
                                 PD_ENABLEPRINTTEMPLATE |
                                 PD_ENABLESETUPTEMPLATE |
                                 PD_ENABLEPRINTTEMPLATEHANDLE |
                                 PD_ENABLESETUPTEMPLATEHANDLE)) | PD_RETURNDC;
    /* The two shell handles stay on this side: they are opaque to the guest,
       which only passes them back, and a Win16 global handle for them would be
       a lie we would then have to keep up.  PD_RETURNDEFAULT insists on having
       neither. */
    if (!(pd.Flags & PD_RETURNDEFAULT)) {
        pd.hDevMode  = keep_mode;
        pd.hDevNames = keep_names;
    }
    pd.nFromPage = sel_rd16(sel, (uint16_t)(off + PD16_nFromPage));
    pd.nToPage   = sel_rd16(sel, (uint16_t)(off + PD16_nToPage));
    pd.nMinPage  = sel_rd16(sel, (uint16_t)(off + PD16_nMinPage));
    pd.nMaxPage  = sel_rd16(sel, (uint16_t)(off + PD16_nMaxPage));
    pd.nCopies   = sel_rd16(sel, (uint16_t)(off + PD16_nCopies));

    if (!PrintDlgA(&pd)) {
        DWORD err = CommDlgExtendedError();
        /* No error at all is the Cancel button, which is not worth a word. */
        if (err) log_msg("PrintDlg failed (%lu)\n", err);
        return 0;
    }
    if (pd.hDevMode && pd.hDevMode != keep_mode) {
        if (keep_mode) GlobalFree(keep_mode);
        keep_mode = pd.hDevMode;
    }
    if (pd.hDevNames && pd.hDevNames != keep_names) {
        if (keep_names) GlobalFree(keep_names);
        keep_names = pd.hDevNames;
    }
    if (log_verbose)
        log_msg("PrintDlg: dc=%p %dx%d at %d dpi\n", (void *)pd.hDC,
                GetDeviceCaps(pd.hDC, HORZRES), GetDeviceCaps(pd.hDC, VERTRES),
                GetDeviceCaps(pd.hDC, LOGPIXELSX));

    sel_wr16(sel, (uint16_t)(off + PD16_hDC), HDC_16(pd.hDC));
    sel_wr32(sel, (uint16_t)(off + PD16_Flags), pd.Flags);
    sel_wr16(sel, (uint16_t)(off + PD16_nFromPage), (uint16_t)pd.nFromPage);
    sel_wr16(sel, (uint16_t)(off + PD16_nToPage),   (uint16_t)pd.nToPage);
    sel_wr16(sel, (uint16_t)(off + PD16_nCopies),   (uint16_t)pd.nCopies);
    return 1;
}

void api_misc_register(void)
{

    api_bind("COMMDLG",  1, cd_GetOpenFileName);
    api_bind("COMMDLG",  2, cd_GetSaveFileName);
    api_bind("COMMDLG", 20, cd_PrintDlg);
}
