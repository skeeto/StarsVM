/* api_misc.c - WIN87EM, TOOLHELP, MMSYSTEM, WAVEMIX and COMMDLG.
 *
 * These are small, and two of them are only here because the game's C runtime
 * insists on asking.
 */

#include "thunk.h"
#include "handle.h"
#include "task.h"
#include "sel.h"
#include "log.h"

#include <stdio.h>
#include <string.h>
#include <windows.h>

/* ---- WIN87EM ------------------------------------------------------------- */

/* WIN87EM.1 __fpMath, the Microsoft C floating-point dispatcher.  It is a
   register-convention entry: BX selects a subfunction, DX:AX carries a pointer,
   SI holds the environment selector, and the CARRY FLAG is the status - the
   caller does `jae` straight after.  The startup code uses BX = 0 and 3 to
   initialize, then BX = 0x0B to ask whether a coprocessor is present.
   We emulate a real x87, so the honest answer to that last one is yes. */
static uint32_t w_fpMath(Cpu *c, Args *a)
{
    uint16_t bx = reg16(c, R_BX);

    (void)a;
    switch (bx) {
    case 0x00:                     /* initialize */
    case 0x03:                     /* second init step */
        c->eflags &= ~F_CF;        /* success */
        set_reg16(c, R_AX, 0);
        return 0;

    case 0x0B:                     /* is there a coprocessor? */
        c->eflags &= ~F_CF;
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
    DWORD now = GetTickCount();

    (void)c;
    if (!p) return 0;
    sel_wr32(sel, (uint16_t)(off + 4), now);
    sel_wr32(sel, (uint16_t)(off + 8), now);
    return 1;
}

/* ---- audio: stubbed for now ---------------------------------------------- */

/* MMSYSTEM.701 mciSendCommand.  The game drives CD audio through MCI for its
   soundtrack; report "no such device" so it gives up quietly rather than
   waiting on a device that will never answer. */
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

/* WaveMix: every entry reports success and does nothing, so the game's sound
   effects simply do not play.  The argument counts come from the DLL's own RETF
   immediates, so the stack stays balanced even though the calls do nothing. */
static uint32_t wm_ok(Cpu *c, Args *a)      { (void)c; (void)a; return 0; }
static uint32_t wm_init(Cpu *c, Args *a)    { (void)c; (void)a; return 0; }

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

    gstr(filep, file, sizeof file);
    gfilter(sel_rd32(sel, (uint16_t)(off + OFN16_lpstrFilter)),
            filter, sizeof filter);
    gstr(sel_rd32(sel, (uint16_t)(off + OFN16_lpstrInitialDir)), dir, sizeof dir);
    gstr(sel_rd32(sel, (uint16_t)(off + OFN16_lpstrTitle)), title, sizeof title);
    gstr(sel_rd32(sel, (uint16_t)(off + OFN16_lpstrDefExt)), deftext, sizeof deftext);

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

static uint32_t cd_PrintDlg(Cpu *c, Args *a)
{
    (void)c;
    arg_long(a);
    log_msg("PrintDlg is stubbed; printing is out of scope\n");
    return 0;
}

void api_misc_register(void)
{
    api_bind("WIN87EM",   1, w_fpMath);
    api_bind("TOOLHELP", 80, t_TimerCount);
    api_bind("MMSYSTEM", 701, m_mciSendCommand);

    api_bind("WAVEMIX",  4, wm_ok);      /* WaveMixActivate      */
    api_bind("WAVEMIX",  5, wm_ok);      /* WaveMixOpenWave      */
    api_bind("WAVEMIX",  6, wm_ok);      /* WaveMixOpenChannel   */
    api_bind("WAVEMIX",  7, wm_ok);      /* WaveMixPlay          */
    api_bind("WAVEMIX",  8, wm_ok);      /* WaveMixFlushChannel  */
    api_bind("WAVEMIX",  9, wm_ok);      /* WaveMixCloseChannel  */
    api_bind("WAVEMIX", 10, wm_ok);      /* WaveMixFreeWave      */
    api_bind("WAVEMIX", 11, wm_ok);      /* WaveMixCloseSession  */
    api_bind("WAVEMIX", 12, wm_ok);      /* WaveMixPump          */
    api_bind("WAVEMIX", 14, wm_ok);      /* WaveMixGetInfo       */
    api_bind("WAVEMIX", 15, wm_init);    /* WaveMixConfigureInit */

    api_bind("COMMDLG",  1, cd_GetOpenFileName);
    api_bind("COMMDLG",  2, cd_GetSaveFileName);
    api_bind("COMMDLG", 20, cd_PrintDlg);
}
