/* api_misc.c - WIN87EM, TOOLHELP, MMSYSTEM and COMMDLG.
 *
 * These are small, and two of them are only here because the game's C runtime
 * insists on asking.
 */

#include "thunk.h"
#include "handle.h"
#include "task.h"
#include "sel.h"
#include "gmem.h"
#include "log.h"
#include "hostclock.h"

#include <stdio.h>
#include <string.h>
#include <windows.h>

/* ---- WIN87EM ------------------------------------------------------------- */

/* WIN87EM.1 __fpMath, the Microsoft C floating-point dispatcher.  It is a
   register-convention entry: BX selects a subfunction, DX:AX carries a pointer,
   SI holds the environment selector, and the CARRY FLAG is the status - the
   caller does `jae` straight after.

   Stars! uses exactly four of them, measured over startup, turn generation and
   a clean exit: 0 to install, 3, 0x0B to ask whether a coprocessor is present,
   and 2 to deinstall on the way out.  The first three arrive at startup and the
   last only on a clean exit, which is why killing the process never shows it.

   0 and 2 are a pair - every install has a matching deinstall - and both are
   about putting a software emulator and its NMI vector in place.  There is no
   software emulator here: the x87 is emulated instruction by instruction in
   fpu.c, so there is nothing to install or take away, and succeeding without
   doing anything is the whole of the correct behaviour.

   The rest of the dispatcher, from Wine's reverse engineering, in case one ever
   turns up: 1 init, 4 set control word, 5 get control word, 6 round the top of
   stack to an integer, 7 pop it as an integer into DX:AX, 8 restore the status
   words, 9 clear the control word, 10 stack depth, 12 stash AX.  Several of
   those must return a value, and the default below answers 0 to everything -
   which for "get control word" would be a control word with every exception
   unmasked.  It is logged rather than guessed at silently, but a subfunction
   that reaches it wants implementing, not believing. */
static uint32_t w_fpMath(Cpu *c, Args *a)
{
    uint16_t bx = reg16(c, R_BX);

    (void)a;
    switch (bx) {
    case 0x00:                     /* install the emulator */
    case 0x02:                     /* and take it away again */
    case 0x03:                     /* second init step */
        c->eflags &= ~F_CF;        /* success */
        set_reg16(c, R_AX, 0);
        return 0;

    case 0x0B:                     /* is there a coprocessor? */
        c->eflags &= ~F_CF;
        /* The flag is read as DX:AX, so DX has to be cleared too.  It was not,
           and the guest happens to arrive here with DX holding a selector, so
           the answer went back as 013F0001 rather than 1.  Anything testing it
           for nonzero got the right idea anyway, which is why this survived. */
        set_reg16(c, R_DX, 0);
        set_reg16(c, R_AX, 1);     /* yes - we emulate a real one */
        return 0;

    default:
        log_msg("WIN87EM.fpMath: unknown subfunction BX=%04X "
                "(dx:ax=%04X:%04X) - reporting success\n",
                bx, reg16(c, R_DX), reg16(c, R_AX));
        c->eflags &= ~F_CF;
        set_reg16(c, R_AX, 0);
        return 0;
    }
}

/* ---- TOOLHELP ------------------------------------------------------------ */

/* TOOLHELP.80 TimerCount fills a TIMERINFO: dwSize, dwmsSinceStart,
   dwmsThisVM.  There is only one virtual machine here, so both are the same
   tick count. */
static uint32_t t_TimerCount(Cpu *c, Args *a)
{
    uint32_t p = arg_long(a);
    uint16_t sel = SEGPTR_SEL(p), off = SEGPTR_OFF(p);
    DWORD now = host_tick();

    (void)c;
    if (!p) return 0;
    sel_wr32(sel, (uint16_t)(off + 4), now);
    sel_wr32(sel, (uint16_t)(off + 8), now);
    return 1;
}

/* ---- CD audio -------------------------------------------------------------- */

/* MMSYSTEM.701 mciSendCommand.  The game's music is Red Book audio: MCI_OPEN on
   "cdaudio", then MCI_PLAY of a track between 2 and 21 off the retail disc.  It
   stores a track number and nothing else, so there is no music data anywhere to
   substitute.  Reporting "no such device" is therefore not a stub but the right
   answer - the game clears its music bit and stops asking, which is what it did
   on a machine with no CD in 1995. */
static uint32_t m_mciSendCommand(Cpu *c, Args *a)
{
    uint16_t dev = arg_word(a);
    uint16_t msg = arg_word(a);
    (void)c;
    arg_long(a);
    arg_long(a);
    if (log_verbose)
        log_msg("mciSendCommand(dev=%u, msg=%04X) stubbed\n", dev, msg);
    return MCIERR_INVALID_DEVICE_NAME;
}

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
    api_bind("WIN87EM",   1, w_fpMath);
    api_bind("TOOLHELP", 80, t_TimerCount);
    api_bind("MMSYSTEM", 701, m_mciSendCommand);

    api_bind("COMMDLG",  1, cd_GetOpenFileName);
    api_bind("COMMDLG",  2, cd_GetSaveFileName);
    api_bind("COMMDLG", 20, cd_PrintDlg);
}
