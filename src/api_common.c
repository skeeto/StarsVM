/* api_common.c - the handlers that need nothing but the guest's memory.
 *
 * Both builds bind these: the emulator (src/unity.c) and the library
 * (src/unity_lib.c), which has no Win32 underneath.  They were in api_user.c,
 * api_gdi.c and api_misc.c until the library needed them without the rest of
 * those files, and they came out unchanged apart from the rectangle calls,
 * which had leaned on Win32's for arithmetic this short.
 */

#include "thunk.h"
#include "sel.h"
#include "gmem.h"
#include "log.h"
#include "hostclock.h"

#include <stdio.h>
#include <string.h>

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
    uint32_t now = host_tick();

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
    return 263;                        /* MCIERR_INVALID_DEVICE_NAME */
}

/* ---- the clock ------------------------------------------------------------- */

static uint32_t u_GetTickCount(Cpu *c, Args *a)
{
    (void)c; (void)a;
    return host_tick();
}

/* ---- wsprintf -------------------------------------------------------------- */

/* USER.420 wsprintf is the only cdecl import: the caller pushes right to left
   and cleans up afterwards, so its arguments read upward from the lowest
   address.  Sizes follow the 16-bit convention - %d, %x, %u and %c take a word
   unless an `l` modifier widens them, and %s is a far pointer.  385 call sites,
   so nearly every string the game puts on screen comes through here. */
static uint32_t u_wsprintf(Cpu *c, Args *a)
{
    uint32_t outp = arg_long_up(a);
    uint32_t fmtp = arg_long_up(a);
    char fmt[512], out[2048], spec[64];
    size_t fi = 0, oi = 0;

    (void)c;
    g_str(fmtp, fmt, sizeof fmt);

    while (fmt[fi] && oi + 1 < sizeof out) {
        size_t si = 0;
        int is_long = 0;

        if (fmt[fi] != '%') { out[oi++] = fmt[fi++]; continue; }
        if (fmt[fi + 1] == '%') { out[oi++] = '%'; fi += 2; continue; }

        /* Copy the conversion through to the host, noting the width modifier
           and stopping at the conversion character. */
        spec[si++] = fmt[fi++];
        while (fmt[fi] && si + 4 < sizeof spec &&
               strchr("-+ #0123456789.*", fmt[fi]))
            spec[si++] = fmt[fi++];
        while (fmt[fi] == 'l' || fmt[fi] == 'h' || fmt[fi] == 'F' ||
               fmt[fi] == 'N' || fmt[fi] == 'w') {
            if (fmt[fi] == 'l') is_long = 1;
            fi++;                              /* not passed to the host */
        }
        if (!fmt[fi]) break;

        switch (fmt[fi]) {
        case 'd': case 'i': {
            long v = is_long ? (long)(int32_t)arg_long_up(a)
                             : (long)arg_sword_up(a);
            spec[si++] = 'l';
            spec[si++] = fmt[fi++];
            spec[si] = 0;
            oi += (size_t)snprintf(out + oi, sizeof out - oi, spec, v);
            break;
        }
        case 'u': case 'x': case 'X': case 'o': {
            unsigned long v = is_long ? (unsigned long)arg_long_up(a)
                                      : (unsigned long)arg_word_up(a);
            spec[si++] = 'l';
            spec[si++] = fmt[fi++];
            spec[si] = 0;
            oi += (size_t)snprintf(out + oi, sizeof out - oi, spec, v);
            break;
        }
        case 'c': {
            uint16_t v = arg_word_up(a);
            spec[si++] = 'c';
            spec[si] = 0;
            fi++;
            oi += (size_t)snprintf(out + oi, sizeof out - oi, spec, (int)(v & 0xFF));
            break;
        }
        case 's': case 'S': {
            char sbuf[512];
            uint32_t sp = arg_long_up(a);
            g_str(sp, sbuf, sizeof sbuf);
            spec[si++] = 's';
            spec[si] = 0;
            fi++;
            oi += (size_t)snprintf(out + oi, sizeof out - oi, spec, sbuf);
            break;
        }
        default:
            /* Something we do not recognise: emit it literally rather than
               silently consuming an argument we cannot size. */
            out[oi++] = fmt[fi++];
            break;
        }
        if (oi >= sizeof out) { oi = sizeof out - 1; break; }
    }
    out[oi] = 0;

    /* The caller's buffer has no declared size; Win16 wsprintf has the same
       hazard, and callers size for 1024. */
    {
        uint16_t sel = SEGPTR_SEL(outp), off = SEGPTR_OFF(outp);
        size_t k;
        for (k = 0; k < oi; k++)
            sel_wr8(sel, (uint16_t)(off + k), (uint8_t)out[k]);
        sel_wr8(sel, (uint16_t)(off + k), 0);
    }
    return (uint32_t)oi;
}

/* ---- rectangles ------------------------------------------------------------ */

/* RECT16: four signed words, left, top, right, bottom. */
typedef struct { int16_t l, t, r, b; } Rect16;

static Rect16 rect_get(uint32_t p)
{
    Rect16 rc;
    uint16_t sel = SEGPTR_SEL(p), off = SEGPTR_OFF(p);
    rc.l = (int16_t)sel_rd16(sel, off);
    rc.t = (int16_t)sel_rd16(sel, (uint16_t)(off + 2));
    rc.r = (int16_t)sel_rd16(sel, (uint16_t)(off + 4));
    rc.b = (int16_t)sel_rd16(sel, (uint16_t)(off + 6));
    return rc;
}

static void rect_put(uint32_t p, Rect16 rc)
{
    uint16_t sel = SEGPTR_SEL(p), off = SEGPTR_OFF(p);
    sel_wr16(sel, off, (uint16_t)rc.l);
    sel_wr16(sel, (uint16_t)(off + 2), (uint16_t)rc.t);
    sel_wr16(sel, (uint16_t)(off + 4), (uint16_t)rc.r);
    sel_wr16(sel, (uint16_t)(off + 6), (uint16_t)rc.b);
}

static uint32_t u_SetRect(Cpu *c, Args *a)
{
    uint32_t p = arg_long(a);
    Rect16 rc;
    (void)c;
    rc.l = arg_sword(a); rc.t = arg_sword(a);
    rc.r = arg_sword(a); rc.b = arg_sword(a);
    rect_put(p, rc);
    return 1;
}

static uint32_t u_CopyRect(Cpu *c, Args *a)
{
    uint32_t dst = arg_long(a), src = arg_long(a);
    (void)c;
    rect_put(dst, rect_get(src));
    return 1;
}

static uint32_t u_EqualRect(Cpu *c, Args *a)
{
    uint32_t p1 = arg_long(a), p2 = arg_long(a);
    Rect16 x = rect_get(p1), y = rect_get(p2);
    (void)c;
    return x.l == y.l && x.t == y.t && x.r == y.r && x.b == y.b;
}

/* Inclusive of the top-left edges and exclusive of the bottom-right. */
static uint32_t u_PtInRect(Cpu *c, Args *a)
{
    uint32_t p = arg_long(a);
    uint32_t packed = arg_long(a);
    Rect16 rc = rect_get(p);
    int16_t x = (int16_t)(packed & 0xFFFF), y = (int16_t)(packed >> 16);
    (void)c;
    return x >= rc.l && x < rc.r && y >= rc.t && y < rc.b;
}

static uint32_t u_OffsetRect(Cpu *c, Args *a)
{
    uint32_t p = arg_long(a);
    int16_t dx = arg_sword(a), dy = arg_sword(a);
    Rect16 rc = rect_get(p);
    (void)c;
    rc.l = (int16_t)(rc.l + dx); rc.r = (int16_t)(rc.r + dx);
    rc.t = (int16_t)(rc.t + dy); rc.b = (int16_t)(rc.b + dy);
    rect_put(p, rc);
    return 1;
}

static uint32_t u_InflateRect(Cpu *c, Args *a)
{
    uint32_t p = arg_long(a);
    int16_t dx = arg_sword(a), dy = arg_sword(a);
    Rect16 rc = rect_get(p);
    (void)c;
    rc.l = (int16_t)(rc.l - dx); rc.r = (int16_t)(rc.r + dx);
    rc.t = (int16_t)(rc.t - dy); rc.b = (int16_t)(rc.b + dy);
    rect_put(p, rc);
    return 1;
}

/* An empty intersection comes back as the all-zero rectangle, and FALSE. */
static uint32_t u_IntersectRect(Cpu *c, Args *a)
{
    uint32_t dst = arg_long(a), s1 = arg_long(a), s2 = arg_long(a);
    Rect16 x = rect_get(s1), y = rect_get(s2), rc;
    (void)c;
    rc.l = x.l > y.l ? x.l : y.l;
    rc.t = x.t > y.t ? x.t : y.t;
    rc.r = x.r < y.r ? x.r : y.r;
    rc.b = x.b < y.b ? x.b : y.b;
    if (rc.l >= rc.r || rc.t >= rc.b) {
        memset(&rc, 0, sizeof rc);
        rect_put(dst, rc);
        return 0;
    }
    rect_put(dst, rc);
    return 1;
}

/* ---- arithmetic ------------------------------------------------------------ */

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

void api_common_register(void)
{
    api_bind("WIN87EM",   1, w_fpMath);
    api_bind("TOOLHELP", 80, t_TimerCount);
    api_bind("MMSYSTEM", 701, m_mciSendCommand);
    api_bind("USER",  13, u_GetTickCount);
    api_bind("USER",  15, u_GetTickCount);      /* GetCurrentTime is the same */
    api_bind("USER",  72, u_SetRect);
    api_bind("USER",  74, u_CopyRect);
    api_bind("USER",  76, u_PtInRect);
    api_bind("USER",  77, u_OffsetRect);
    api_bind("USER",  78, u_InflateRect);
    api_bind("USER",  79, u_IntersectRect);
    api_bind("USER", 244, u_EqualRect);
    api_bind("USER", 420, u_wsprintf);
    api_bind("GDI",  128, g_MulDiv);
}
