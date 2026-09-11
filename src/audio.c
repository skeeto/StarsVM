/* audio.c - WAVEMIX, reimplemented natively on waveOut.
 *
 * Stars! plays its battle effects through WAVEMIX.DLL, Angel Diaz's 1993
 * "Realtime Wave Mixing DLL" - a software mixer from the days when a sound card
 * could play exactly one stream.  The whole of the game's audio is segment 35,
 * 846 bytes, which calls nothing but these eleven ordinals and mciSendCommand.
 *
 * We do not run the shipped DLL as guest code.  That would need a module
 * registry, LibMain, data-segment handling, a full waveOut layer underneath it,
 * and - fatally - 16-bit completion callbacks arriving on an audio thread, which
 * a single-threaded interpreter around one global Cpu cannot take.  Instead the
 * eleven entry points are implemented here against the host's waveOut, one
 * device per channel, and Windows does the mixing the DLL used to do in
 * software.  All six of the game's waves share one format, so there is no
 * resampling, scheduling or clipping to write.
 *
 * NO HOST CALLBACK MAY ENTER GUEST CODE.  Every device is opened CALLBACK_NULL
 * and finished buffers are recycled by polling WHDR_DONE, driven from
 * WaveMixPump - which the guest already calls thirteen times per battle frame,
 * because the software mixer needed pumping - and opportunistically from every
 * other entry point.  No threads, no callbacks, nothing to lock.  This follows
 * the precedent set by u_SetTimer in api_user.c, which passes a NULL timer proc
 * for the same reason.
 *
 * The flag values below are the SDK's WAVEMIX.H constants.  The game passes
 * WMIX_USELRUCHANNEL to every WaveMixPlay while always naming channel 0, which
 * is the difference between one voice and eight: iChannel is ignored and the
 * sound goes wherever there is room.  That is the entire reason it opened eight
 * channels, and the reason a battle sounds like a battle.
 */

#include "thunk.h"
#include "handle.h"
#include "task.h"
#include "sel.h"
#include "gmem.h"
#include "heap.h"
#include "res.h"
#include "audio.h"
#include "log.h"

#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <mmsystem.h>

/* ---- WAVEMIX.H ------------------------------------------------------------ */

/* WaveMixOpenWave dwFlags: where the wave comes from.  The game passes
   WMIX_RESOURCE and a MAKEINTRESOURCE name. */
#define WMIX_FILE           0x0001
#define WMIX_RESOURCE       0x0002
#define WMIX_MEMORY         0x0004

/* MIXPLAYPARAMS dwFlags. */
#define WMIX_QUEUEWAVE      0x0000
#define WMIX_CLEARQUEUE     0x0001
#define WMIX_USELRUCHANNEL  0x0002
#define WMIX_HIPRIORITY     0x0004
#define WMIX_WAIT           0x0008

/* WaveMixFlushChannel dwFlags.  The game flushes channels 0-6 with WMIX_NOREMIX
   and channel 7 without it, which is the documented idiom for "flush the lot and
   rebuild the mix once at the end".  With Windows mixing for us there is no mix
   to rebuild, so the flag costs nothing either way. */
#define WMIX_ALL            0x0001
#define WMIX_NOREMIX        0x0002

/* MIXCONFIG dwFlags: which of the optional fields are filled in. */
#define WMIX_CONFIG_CHANNELS      0x0001
#define WMIX_CONFIG_SAMPLINGRATE  0x0002

/* Structure layouts, byte-packed as Win16 built them.  Each leads with a wSize
   the caller fills in, which is the 1993 convention and our validation hook:
   2+4+2+2 = 10, 2+1+1+12+4 = 20, and 2+2+2+4+2+4+2 = 18 all match the sizes the
   game declares. */
#define MIXCONFIG_SIZE          10
#define MC_wSize                 0
#define MC_dwFlags               2
#define MC_wChannels             6
#define MC_wSamplingRate         8

#define WAVEMIXINFO_SIZE        20
#define WMI_wSize                0
#define WMI_bVersionMajor        2
#define WMI_bVersionMinor        3
#define WMI_szDate               4      /* 12 bytes */
#define WMI_dwFormats           16

#define MIXPLAYPARAMS_SIZE      18
#define MPP_wSize                0
#define MPP_hMixSession          2
#define MPP_iChannel             4
#define MPP_lpMixWave            6
#define MPP_hWndNotify          10
#define MPP_dwFlags             12
#define MPP_wLoops              16

/* The MIXWAVE we hand back is a real block in the guest's address space, not a
   fabricated handle: the DLL let callers look inside one, so ours has to survive
   being looked inside.  It opens with a PCMWAVEFORMAT, then the pointer and
   length of the sample data, which sits in the same block just past the header.
   The guest only ever stores the pointer and hands it back, but a block that
   reads as nonsense would be a trap laid for later. */
#define MW_wFormatTag            0
#define MW_nChannels             2
#define MW_nSamplesPerSec        4
#define MW_nAvgBytesPerSec       8
#define MW_nBlockAlign          12
#define MW_wBitsPerSample       14
#define MW_lpData               16
#define MW_dwDataLength         20
#define MW_HEADER               32     /* sample data starts here */

/* ---- state ---------------------------------------------------------------- */

/* The game opens channels 0 through 7.  Sixteen costs nothing and means a guest
   channel number we did not expect is clamped rather than refused. */
#define NCHAN   16
#define NHDR     4      /* buffers queued per channel before it has to wait */
#define NWAVE   16      /* the game opens six */

typedef struct {
    int          open;              /* WaveMixOpenChannel said so              */
    HWAVEOUT     dev;               /* NULL until this channel first plays     */
    WAVEFORMATEX fmt;               /* the format `dev` was opened in          */
    WAVEHDR      hdr[NHDR];
    int          busy[NHDR];        /* header is prepared and in flight        */
    unsigned     inflight;
    unsigned     seq;               /* play order, for the LRU choice          */
} Channel;

typedef struct {
    int          used;
    uint16_t     hmem;              /* guest block: MIXWAVE header, then PCM   */
    uint32_t     lpmixwave;         /* the far pointer the guest holds         */
    WAVEFORMATEX fmt;
    const uint8_t *pcm;             /* into the selector arena, which never moves */
    uint32_t     pcmlen;
    uint16_t     id;                /* resource id, for the log                */
} Wave;

static struct {
    int      up;                    /* a session exists                        */
    uint16_t handle;                /* what the guest calls it                 */
    int      suspended;             /* WaveMixActivate(FALSE)                  */
    int      nodevice;              /* the host has no usable output; stop trying */
    Channel  ch[NCHAN];
    Wave     wave[NWAVE];
    unsigned seq;
} mix;

/* Failure is a nonzero WORD; the SDK's own codes are not interesting to a caller
   that, like this one, never looks. */
#define WM_OK    0u
#define WM_FAIL  1u

/* ---- buffer recycling ----------------------------------------------------- */

/* Reclaim any header the device has finished with.  This is the whole of our
   completion handling and it must stay cheap: WaveMixPump runs thirteen times
   per battle frame, and a channel that has nothing in flight costs one test. */
static void chan_service(Channel *c)
{
    int i;

    if (!c->dev || !c->inflight) return;
    for (i = 0; i < NHDR; i++) {
        if (c->busy[i] && (c->hdr[i].dwFlags & WHDR_DONE)) {
            waveOutUnprepareHeader(c->dev, &c->hdr[i], sizeof c->hdr[i]);
            c->busy[i] = 0;
            c->inflight--;
        }
    }
}

static void mix_service(void)
{
    int i;
    for (i = 0; i < NCHAN; i++) chan_service(&mix.ch[i]);
}

/* Stop a channel and give every buffer back.  waveOutReset marks all pending
   headers done, so the service pass afterwards is what actually unprepares
   them; unpreparing a header the device still owns fails. */
static void chan_reset(Channel *c)
{
    if (!c->dev) return;
    waveOutReset(c->dev);
    chan_service(c);
}

static void chan_close(Channel *c)
{
    if (!c->dev) return;
    chan_reset(c);
    waveOutClose(c->dev);
    c->dev = NULL;
    c->inflight = 0;
    memset(c->busy, 0, sizeof c->busy);
}

/* ---- devices -------------------------------------------------------------- */

/* Open this channel's device, in the format the wave wants.  Lazily, because
   eight opens at WaveMixOpenChannel time would cost a visible pause at the top
   of the first battle and seven of them would never be used. */
static int chan_device(Channel *c, const WAVEFORMATEX *fmt)
{
    MMRESULT r;

    /* Between WaveMixActivate(FALSE) and the matching TRUE the app has given
       the device back, so there is nothing to open onto. */
    if (mix.nodevice || mix.suspended) return 0;
    if (c->dev) {
        if (memcmp(&c->fmt, fmt, sizeof *fmt) == 0) return 1;
        chan_close(c);          /* a different format needs a different device */
    }

    r = waveOutOpen(&c->dev, WAVE_MAPPER, fmt, 0, 0, CALLBACK_NULL);
    if (r != MMSYSERR_NOERROR) {
        static int shown;
        int i, any = 0;

        c->dev = NULL;
        /* One channel failing is ordinary: a device has a limit on how many
           streams it will mix, and the effect simply plays elsewhere.  The
           FIRST one failing is different - it means there is no output at all,
           and a battle would then ask again per effect for nothing. */
        for (i = 0; i < NCHAN; i++) if (mix.ch[i].dev) any = 1;
        if (!any) {
            mix.nodevice = 1;
            log_msg("audio: waveOutOpen failed (mmsys error %u); "
                    "no usable waveform output, effects are silent\n",
                    (unsigned)r);
        } else if (shown < 8) {
            shown++;
            log_msg("audio: waveOutOpen failed for channel %d (mmsys error %u); "
                    "that voice is dropped\n", (int)(c - mix.ch), (unsigned)r);
        }
        return 0;
    }
    c->fmt = *fmt;
    return 1;
}

/* WMIX_USELRUCHANNEL: the caller's iChannel is advisory, so put the sound
   wherever it will not cut anything off.  A channel that is idle is always the
   best answer - it keeps the number of open devices down to what the battle
   actually needs - and when every channel is busy the least recently started one
   is the one whose sound is closest to finished. */
static Channel *chan_pick(int want)
{
    int i, best = -1;

    for (i = 0; i < NCHAN; i++)
        if (mix.ch[i].open && !mix.ch[i].inflight) return &mix.ch[i];
    for (i = 0; i < NCHAN; i++)
        if (mix.ch[i].open && (best < 0 || mix.ch[i].seq < mix.ch[best].seq))
            best = i;
    if (best >= 0) return &mix.ch[best];

    /* No channel is open.  The real DLL refuses this outright ("You must open a
       channel before you can play a wave!"), and so do we. */
    (void)want;
    return NULL;
}

/* Queue one wave.  waveOut takes the buffer by pointer and plays it where it
   lies, so the sample data has to outlive the call - it does: it sits in the
   selector arena, which is reserved once and never moves. */
static int chan_play(Channel *c, Wave *w, unsigned loops)
{
    WAVEHDR *h = NULL;
    MMRESULT r;
    int i;

    if (!chan_device(c, &w->fmt)) return 0;
    chan_service(c);
    for (i = 0; i < NHDR; i++) if (!c->busy[i]) { h = &c->hdr[i]; break; }
    if (!h) return 0;               /* this channel is saturated; caller retries */

    memset(h, 0, sizeof *h);
    h->lpData         = (LPSTR)w->pcm;
    h->dwBufferLength = w->pcmlen;
    if (loops) {
        h->dwLoops  = loops + 1;
        h->dwFlags |= WHDR_BEGINLOOP | WHDR_ENDLOOP;
    }
    r = waveOutPrepareHeader(c->dev, h, sizeof *h);
    if (r != MMSYSERR_NOERROR) {
        log_msg("audio: waveOutPrepareHeader failed (%u)\n", (unsigned)r);
        return 0;
    }
    r = waveOutWrite(c->dev, h, sizeof *h);
    if (r != MMSYSERR_NOERROR) {
        waveOutUnprepareHeader(c->dev, h, sizeof *h);
        log_msg("audio: waveOutWrite failed (%u)\n", (unsigned)r);
        return 0;
    }
    c->busy[i] = 1;
    c->inflight++;
    c->seq = ++mix.seq;
    return 1;
}

/* ---- RIFF ----------------------------------------------------------------- */

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

/* Walk a RIFF WAVE and pick out the format and the samples.
 *
 * `len` is the RESOURCE length, which is rounded up to the NE table's alignment
 * - 64 bytes here - and so is larger than the file.  Trusting it would walk into
 * the padding and read zero-length chunks with a zero id; the RIFF header's own
 * size field is the honest end, and is what bounds the walk. */
static int riff_parse(const uint8_t *p, uint32_t len, WAVEFORMATEX *fmt,
                      const uint8_t **data, uint32_t *datalen)
{
    uint32_t end, at;
    int have_fmt = 0, have_data = 0;

    *data = NULL;
    *datalen = 0;
    if (len < 12 || memcmp(p, "RIFF", 4) != 0 || memcmp(p + 8, "WAVE", 4) != 0)
        return 0;
    end = le32(p + 4) + 8;
    if (end > len) end = len;

    for (at = 12; at + 8 <= end; ) {
        const uint8_t *ck = p + at;
        uint32_t sz = le32(ck + 4);

        if (sz > end - at - 8) sz = end - at - 8;      /* truncated final chunk;
                                                          written this way round
                                                          so a corrupt length
                                                          cannot overflow */
        if (memcmp(ck, "fmt ", 4) == 0 && sz >= 16) {
            memset(fmt, 0, sizeof *fmt);
            fmt->wFormatTag      = le16(ck + 8);
            fmt->nChannels       = le16(ck + 10);
            fmt->nSamplesPerSec  = le32(ck + 12);
            fmt->nAvgBytesPerSec = le32(ck + 16);
            fmt->nBlockAlign     = le16(ck + 20);
            fmt->wBitsPerSample  = le16(ck + 22);
            fmt->cbSize          = 0;
            have_fmt = 1;
        } else if (memcmp(ck, "data", 4) == 0) {
            *data = ck + 8;
            *datalen = sz;
            have_data = 1;
        }
        at += 8 + sz + (sz & 1);                        /* chunks are word-aligned */
    }

    if (!have_fmt || !have_data || !*datalen) return 0;
    if (fmt->wFormatTag != WAVE_FORMAT_PCM) return 0;   /* the DLL refused these too */
    if (!fmt->nBlockAlign)
        fmt->nBlockAlign = (WORD)(fmt->nChannels * ((fmt->wBitsPerSample + 7) / 8));
    if (!fmt->nAvgBytesPerSec)
        fmt->nAvgBytesPerSec = fmt->nSamplesPerSec * fmt->nBlockAlign;
    return 1;
}

/* ---- waves ---------------------------------------------------------------- */

static Wave *wave_find(uint32_t lpmixwave)
{
    int i;
    if (!lpmixwave) return NULL;
    for (i = 0; i < NWAVE; i++)
        if (mix.wave[i].used && mix.wave[i].lpmixwave == lpmixwave)
            return &mix.wave[i];
    return NULL;
}

static Wave *wave_by_id(uint16_t id)
{
    int i;
    for (i = 0; i < NWAVE; i++)
        if (mix.wave[i].used && mix.wave[i].id == id) return &mix.wave[i];
    return NULL;
}

/* Build the guest-visible MIXWAVE for a resource, with the samples copied in
   behind the header so one block serves both sides: the guest sees a far pointer
   to real data, and waveOut plays the same bytes through sel_ptr. */
static Wave *wave_open(const uint8_t *res, uint32_t reslen, uint16_t id)
{
    WAVEFORMATEX fmt;
    const uint8_t *data;
    uint32_t datalen;
    uint16_t hmem, sel;
    uint8_t *blk;
    Wave *w;
    int i;

    if (!riff_parse(res, reslen, &fmt, &data, &datalen)) {
        log_msg("audio: resource %u is not a PCM WAVE we can play\n", id);
        return NULL;
    }
    for (i = 0; i < NWAVE; i++) if (!mix.wave[i].used) break;
    if (i == NWAVE) { log_msg("audio: too many open waves\n"); return NULL; }
    w = &mix.wave[i];

    /* A fixed global block, so the handle is the selector and nothing can move
       it: waveOut plays the samples where they lie and must find them there
       until the device is finished with them. */
    hmem = gmem_alloc(0, MW_HEADER + datalen);
    if (!hmem) return NULL;
    sel = gmem_sel(hmem);
    blk = sel_ptr(sel, 0);

    memcpy(blk + MW_HEADER, data, datalen);
    sel_wr16(sel, MW_wFormatTag,      fmt.wFormatTag);
    sel_wr16(sel, MW_nChannels,       fmt.nChannels);
    sel_wr32(sel, MW_nSamplesPerSec,  fmt.nSamplesPerSec);
    sel_wr32(sel, MW_nAvgBytesPerSec, fmt.nAvgBytesPerSec);
    sel_wr16(sel, MW_nBlockAlign,     fmt.nBlockAlign);
    sel_wr16(sel, MW_wBitsPerSample,  fmt.wBitsPerSample);
    sel_wr32(sel, MW_lpData,          SEGPTR(sel, MW_HEADER));
    sel_wr32(sel, MW_dwDataLength,    datalen);

    w->used      = 1;
    w->hmem      = hmem;
    w->lpmixwave = SEGPTR(sel, 0);
    w->fmt       = fmt;
    w->pcm       = blk + MW_HEADER;
    w->pcmlen    = datalen;
    w->id        = id;

    if (log_verbose)
        log_msg("audio: wave %u = %u Hz, %u bit, %u ch, %u bytes (%.2f s)\n",
                id, (unsigned)fmt.nSamplesPerSec, fmt.wBitsPerSample,
                fmt.nChannels, datalen,
                fmt.nAvgBytesPerSec ? (double)datalen / fmt.nAvgBytesPerSec : 0.0);
    return w;
}

/* A wave's samples are still under the device's nose if it is playing them, so
   every channel has to be stopped before the block goes away. */
static void wave_free(Wave *w)
{
    int i;
    if (!w->used) return;
    for (i = 0; i < NCHAN; i++) chan_reset(&mix.ch[i]);
    if (w->hmem) gmem_free(w->hmem);
    memset(w, 0, sizeof *w);
}

/* ---- the eleven entry points ---------------------------------------------- */

/* WAVEMIX.15 WaveMixConfigureInit(LPMIXCONFIG).  Returns the session handle, or
   zero.  Zero is what the stub used to return, and the game reads that as "no
   audio", latches it, and never calls another WaveMix entry - which is why
   nothing here had ever been exercised. */
static uint32_t wm_ConfigureInit(Cpu *c, Args *a)
{
    uint32_t cfgp = arg_long(a);

    (void)c;
    if (cfgp) {
        uint16_t sel = SEGPTR_SEL(cfgp), off = SEGPTR_OFF(cfgp);
        uint16_t size = sel_rd16(sel, (uint16_t)(off + MC_wSize));
        uint32_t flags = sel_rd32(sel, (uint16_t)(off + MC_dwFlags));

        if (size < MIXCONFIG_SIZE) {
            log_msg("audio: MIXCONFIG wSize is %u, expected %u\n",
                    size, MIXCONFIG_SIZE);
        } else if (log_verbose) {
            /* dwFlags says which of the two optional fields were filled in, and
               the game sets only WMIX_CONFIG_CHANNELS - it writes eight bytes
               into a ten-byte struct and leaves wSamplingRate as whatever was on
               its stack.  Reading it unconditionally would be reading rubbish. */
            log_msg("audio: MIXCONFIG flags=%08lX", (unsigned long)flags);
            if (flags & WMIX_CONFIG_CHANNELS)
                log_msg(" channels=%u",
                        sel_rd16(sel, (uint16_t)(off + MC_wChannels)));
            if (flags & WMIX_CONFIG_SAMPLINGRATE)
                log_msg(" rate=%u",
                        sel_rd16(sel, (uint16_t)(off + MC_wSamplingRate)));
            log_msg("\n");
        }
        /* The mixer's own channel count and sampling rate configured a software
           mixer that no longer exists: the host mixes, each wave plays in its
           own format, and there is nothing here to tune. */
    }

    if (!mix.up) {
        memset(&mix, 0, sizeof mix);
        mix.up = 1;
        mix.handle = h16(H_MIXSESSION, &mix);
        log_msg("audio: WaveMix session %04X open\n", mix.handle);
    }
    return mix.handle;
}

/* WAVEMIX.14 WaveMixGetInfo(LPWAVEMIXINFO).  The caller fills in wSize and the
   DLL fills in the rest; the old stub reported success and wrote nothing, which
   left the game reading its own uninitialised buffer. */
static uint32_t wm_GetInfo(Cpu *c, Args *a)
{
    uint32_t p = arg_long(a);
    uint16_t sel = SEGPTR_SEL(p), off = SEGPTR_OFF(p);
    uint16_t size;
    WAVEOUTCAPSA caps;
    DWORD formats = 0;

    (void)c;
    if (!p) return WM_FAIL;
    size = sel_rd16(sel, (uint16_t)(off + WMI_wSize));
    if (size < WAVEMIXINFO_SIZE) {
        log_msg("audio: WAVEMIXINFO wSize is %u, expected %u\n",
                size, WAVEMIXINFO_SIZE);
        return WM_FAIL;
    }

    /* dwFormats is the WAVE_FORMAT_x bitmask of what can actually be played, so
       ask the device rather than claim. */
    if (waveOutGetNumDevs() &&
        waveOutGetDevCapsA(WAVE_MAPPER, &caps, sizeof caps) == MMSYSERR_NOERROR)
        formats = caps.dwFormats;

    /* The version is the one thing here a caller could reasonably branch on, so
       report the contract we implement rather than something of our own: the
       shipped DLL is WaveMix 1.50.  szDate is that DLL's build date, and we are
       not that build, so leave it empty rather than claim one - the field is
       already zero, since the guest reads its own buffer and we only write what
       we can honestly fill in.  The game discards all of it. */
    sel_wr8(sel, (uint16_t)(off + WMI_bVersionMajor), 1);
    sel_wr8(sel, (uint16_t)(off + WMI_bVersionMinor), 50);
    sel_wr8(sel, (uint16_t)(off + WMI_szDate), 0);
    sel_wr32(sel, (uint16_t)(off + WMI_dwFormats), formats);
    if (log_verbose)
        log_msg("audio: WaveMixGetInfo -> version 1.50, formats %08lX\n",
                (unsigned long)formats);
    return WM_OK;
}

/* WAVEMIX.6 WaveMixOpenChannel(HMIXSESSION, int iChannel, DWORD dwFlags).
   Bookkeeping only: the device behind a channel is opened when something first
   plays on it. */
static uint32_t wm_OpenChannel(Cpu *c, Args *a)
{
    uint16_t sess = arg_word(a);
    int16_t  chan = arg_sword(a);
    uint32_t flags = arg_long(a);

    (void)c; (void)sess;
    if (chan < 0 || chan >= NCHAN) {
        log_msg("audio: WaveMixOpenChannel for channel %d, which we do not have\n",
                chan);
        return WM_FAIL;
    }
    mix.ch[chan].open = 1;
    if (log_verbose)
        log_msg("audio: channel %d open (flags %08lX)\n",
                chan, (unsigned long)flags);
    mix_service();
    return WM_OK;
}

/* WAVEMIX.5 WaveMixOpenWave(HMIXSESSION, LPSTR name, HINSTANCE, DWORD flags).
   Returns an LPMIXWAVE far pointer, or zero.  The game passes WMIX_RESOURCE and
   a MAKEINTRESOURCE name - a far pointer with a zero selector whose offset is
   the numeric id - against the string-named resource type "WAVE". */
static uint32_t wm_OpenWave(Cpu *c, Args *a)
{
    uint16_t sess = arg_word(a);
    uint32_t namep = arg_long(a);
    uint16_t hinst = arg_word(a);
    uint32_t flags = arg_long(a);
    const uint8_t *data = NULL;
    uint32_t len = 0;
    uint16_t type, id = 0;
    Wave *w;

    (void)c; (void)sess; (void)hinst;
    if (!mix.up) { log_msg("audio: WaveMixOpenWave with no session\n"); return 0; }

    type = res_type_key("WAVE");
    if (!type) {
        log_msg("audio: the module has no \"WAVE\" resource type\n");
        return 0;
    }

    if (SEGPTR_SEL(namep) == 0) {
        id = SEGPTR_OFF(namep);
        if ((w = wave_by_id(id)) != NULL) return w->lpmixwave;  /* already open */
        data = res_locate_id(type, id, &len);
    } else {
        char name[64];

        g_str(namep, name, sizeof name);
        data = res_locate_name(type, name, &len);
        if (!data) log_msg("audio: no WAVE resource named \"%s\"\n", name);
    }

    if ((flags & (WMIX_FILE | WMIX_MEMORY)) && !(flags & WMIX_RESOURCE))
        log_msg("audio: WaveMixOpenWave flags %08lX asks for a file or a memory "
                "image; only resources are implemented\n", (unsigned long)flags);
    if (!data) {
        if (id) log_msg("audio: no WAVE resource %u\n", id);
        return 0;
    }

    w = wave_open(data, len, id);
    return w ? w->lpmixwave : 0;
}

/* WAVEMIX.7 WaveMixPlay(LPMIXPLAYPARAMS). */
static uint32_t wm_Play(Cpu *c, Args *a)
{
    uint32_t p = arg_long(a);
    uint16_t sel = SEGPTR_SEL(p), off = SEGPTR_OFF(p);
    uint16_t size, sess, hwnd, loops;
    int16_t  chan;
    uint32_t lpwave, flags;
    Channel *ch;
    Wave *w;

    (void)c;
    mix_service();
    if (!p) { log_msg("audio: WaveMixPlay with a null parameter block\n");
              return WM_FAIL; }

    size   = sel_rd16(sel, (uint16_t)(off + MPP_wSize));
    sess   = sel_rd16(sel, (uint16_t)(off + MPP_hMixSession));
    chan   = (int16_t)sel_rd16(sel, (uint16_t)(off + MPP_iChannel));
    lpwave = sel_rd32(sel, (uint16_t)(off + MPP_lpMixWave));
    hwnd   = sel_rd16(sel, (uint16_t)(off + MPP_hWndNotify));
    flags  = sel_rd32(sel, (uint16_t)(off + MPP_dwFlags));
    loops  = sel_rd16(sel, (uint16_t)(off + MPP_wLoops));

    if (size < MIXPLAYPARAMS_SIZE) {
        log_msg("audio: MIXPLAYPARAMS wSize is %u, expected %u\n",
                size, MIXPLAYPARAMS_SIZE);
        return WM_FAIL;
    }
    if (!mix.up || sess != mix.handle) {
        log_msg("audio: WaveMixPlay against session %04X, not %04X\n",
                sess, mix.handle);
        return WM_FAIL;
    }
    /* hWndNotify would want a 16-bit callback on a completing buffer, which is
       the one thing this shim cannot do.  The game passes zero. */
    if (hwnd)
        log_msg("audio: WaveMixPlay asks to notify window %04X; "
                "completion notification is not supported\n", hwnd);

    w = wave_find(lpwave);
    if (!w) {
        log_msg("audio: WaveMixPlay with an unknown wave %08lX\n",
                (unsigned long)lpwave);
        return WM_FAIL;
    }

    /* WMIX_USELRUCHANNEL says iChannel is a suggestion; without it the caller
       means the channel it named. */
    if (flags & WMIX_USELRUCHANNEL) {
        ch = chan_pick(chan);
    } else if (chan >= 0 && chan < NCHAN && mix.ch[chan].open) {
        ch = &mix.ch[chan];
    } else {
        ch = NULL;
    }
    if (!ch) {
        log_msg("audio: WaveMixPlay on channel %d, which is not open\n", chan);
        return WM_FAIL;
    }
    if (flags & WMIX_CLEARQUEUE) chan_reset(ch);

    if (log_verbose)
        log_msg("audio: play wave %u on channel %d (asked %d), flags %08lX\n",
                w->id, (int)(ch - mix.ch), chan, (unsigned long)flags);
    if (!chan_play(ch, w, loops)) return WM_FAIL;
    return WM_OK;
}

/* WAVEMIX.8 WaveMixFlushChannel(HMIXSESSION, int iChannel, DWORD dwFlags). */
static uint32_t wm_FlushChannel(Cpu *c, Args *a)
{
    uint16_t sess = arg_word(a);
    int16_t  chan = arg_sword(a);
    uint32_t flags = arg_long(a);
    int i;

    (void)c; (void)sess;
    if (flags & WMIX_ALL) {
        for (i = 0; i < NCHAN; i++) chan_reset(&mix.ch[i]);
        return WM_OK;
    }
    if (chan < 0 || chan >= NCHAN) return WM_FAIL;
    chan_reset(&mix.ch[chan]);
    return WM_OK;
}

/* WAVEMIX.9 WaveMixCloseChannel(HMIXSESSION, int iChannel, DWORD dwFlags). */
static uint32_t wm_CloseChannel(Cpu *c, Args *a)
{
    uint16_t sess = arg_word(a);
    int16_t  chan = arg_sword(a);
    uint32_t flags = arg_long(a);
    int i;

    (void)c; (void)sess;
    if (flags & WMIX_ALL) {
        for (i = 0; i < NCHAN; i++) { chan_close(&mix.ch[i]); mix.ch[i].open = 0; }
        return WM_OK;
    }
    if (chan < 0 || chan >= NCHAN) return WM_FAIL;
    chan_close(&mix.ch[chan]);
    mix.ch[chan].open = 0;
    return WM_OK;
}

/* WAVEMIX.10 WaveMixFreeWave(HMIXSESSION, LPMIXWAVE).
 *
 * Expect to be handed NULL, repeatedly, and to be given none of the waves that
 * are actually open.  The game's SoundShutdown walks the same six-slot table
 * that OpenWaves walks, but with the zero test inverted - OpenWaves skips a slot
 * that is already filled (`cmpl [si],0; jne skip`, correct), while SoundShutdown
 * skips a slot that is filled and frees the empty ones.  So every battle exit
 * calls this six times with a null pointer and never frees a thing.  That is the
 * game's bug, not ours, and nothing is leaked by it: WaveMixCloseSession follows
 * immediately and takes the whole session, waves included.  Returning failure is
 * correct and the game discards it, as it discards every return here. */
static uint32_t wm_FreeWave(Cpu *c, Args *a)
{
    uint16_t sess = arg_word(a);
    uint32_t lpwave = arg_long(a);
    Wave *w;

    (void)c; (void)sess;
    w = wave_find(lpwave);
    if (!w) return WM_FAIL;
    wave_free(w);
    return WM_OK;
}

/* WAVEMIX.11 WaveMixCloseSession(HMIXSESSION). */
static uint32_t wm_CloseSession(Cpu *c, Args *a)
{
    uint16_t sess = arg_word(a);
    (void)c; (void)sess;
    audio_shutdown();
    return WM_OK;
}

/* WAVEMIX.12 WaveMixPump(void).  The software mixer needed the application to
   hand it time from inside its own animation loop; the game still does, thirteen
   times per battle frame, which is exactly the guest-driven, main-thread service
   point our polled completion handling wants.  It must stay cheap. */
static uint32_t wm_Pump(Cpu *c, Args *a)
{
    (void)c; (void)a;
    mix_service();
    return WM_OK;
}

/* WAVEMIX.4 WaveMixActivate(HMIXSESSION, BOOL fActivate).  This is the battle
   VCR opening and closing: segment 30 calls it with TRUE on the way in, right
   after SoundInit, and with FALSE on the way out, right after flushing every
   channel.  It is the only WaveMix entry the audio segment itself never calls.

   Deactivating gives the waveform device back, which is what the DLL did - in
   1993 there was one of them and another application might want it.  We can
   afford to be equally polite: the devices reopen by themselves on the next
   effect, and the only thing that plays one is a battle, which activates first. */
static uint32_t wm_Activate(Cpu *c, Args *a)
{
    uint16_t sess = arg_word(a);
    uint16_t on = arg_word(a);
    int i;

    (void)c; (void)sess;
    if (!!mix.suspended == !on) return WM_OK;
    mix.suspended = !on;
    if (mix.suspended)
        for (i = 0; i < NCHAN; i++) chan_close(&mix.ch[i]);
    if (log_verbose)
        log_msg("audio: %s\n", mix.suspended ? "deactivated" : "activated");
    return WM_OK;
}

/* ---- shutdown ------------------------------------------------------------- */

/* Every device closed and every block returned.  Called by WaveMixCloseSession,
   which the game's own SoundShutdown reaches, and again from main() because a
   guest that stops some other way must not leave a device open. */
void audio_shutdown(void)
{
    int i;

    for (i = 0; i < NCHAN; i++) { chan_close(&mix.ch[i]); mix.ch[i].open = 0; }
    for (i = 0; i < NWAVE; i++) if (mix.wave[i].used) wave_free(&mix.wave[i]);
    if (mix.up) log_msg("audio: WaveMix session closed\n");
    mix.up = 0;
    mix.handle = 0;
    mix.suspended = 0;
}

/* ---- the --play-wave harness ---------------------------------------------- */

/* The effects only ever play inside the battle VCR, so "does our audio work" and
   "can we reach a fight" are two questions that should not have to be answered
   at once.  This drives exactly the path the guest drives, without the guest.
   Resource id 0 means all six, overlapping, which is what a battle sounds like
   and what proves the LRU channel choice is doing its job. */
int audio_selftest(unsigned id)
{
    static const uint16_t all[] = { 2601, 2602, 2611, 2612, 2621, 2631 };
    uint16_t type = res_type_key("WAVE");
    unsigned i, n = 0;
    DWORD until;

    if (!type) { log_msg("audio: no \"WAVE\" resource type in this module\n");
                 return 1; }

    memset(&mix, 0, sizeof mix);
    mix.up = 1;
    mix.handle = h16(H_MIXSESSION, &mix);
    for (i = 0; i < 8; i++) mix.ch[i].open = 1;

    for (i = 0; i < sizeof all / sizeof *all; i++) {
        const uint8_t *data;
        uint32_t len;
        Channel *ch;
        Wave *w;

        if (id && all[i] != id) continue;
        data = res_locate_id(type, all[i], &len);
        if (!data) { log_msg("audio: no WAVE resource %u\n", all[i]); continue; }
        w = wave_open(data, len, all[i]);
        if (!w) continue;
        ch = chan_pick(0);
        log_msg("audio: playing %u (%u bytes) on channel %d\n",
                all[i], w->pcmlen, ch ? (int)(ch - mix.ch) : -1);
        if (!ch || !chan_play(ch, w, 0))
            log_msg("audio: could not play %u\n", all[i]);
        n++;
        if (!id) Sleep(150);        /* overlap them, as a battle would */
    }
    if (!n) { log_msg("audio: nothing to play\n"); audio_shutdown(); return 1; }

    /* Pump the way the guest would, until everything has drained. */
    until = GetTickCount() + 8000;
    for (;;) {
        int busy = 0;
        mix_service();
        for (i = 0; i < NCHAN; i++) if (mix.ch[i].inflight) busy = 1;
        if (!busy || GetTickCount() > until) break;
        Sleep(10);
    }
    audio_shutdown();
    return 0;
}

/* ---------------------------------------------------------------------------- */

void api_audio_register(void)
{
    api_bind("WAVEMIX",  4, wm_Activate);
    api_bind("WAVEMIX",  5, wm_OpenWave);
    api_bind("WAVEMIX",  6, wm_OpenChannel);
    api_bind("WAVEMIX",  7, wm_Play);
    api_bind("WAVEMIX",  8, wm_FlushChannel);
    api_bind("WAVEMIX",  9, wm_CloseChannel);
    api_bind("WAVEMIX", 10, wm_FreeWave);
    api_bind("WAVEMIX", 11, wm_CloseSession);
    api_bind("WAVEMIX", 12, wm_Pump);
    api_bind("WAVEMIX", 14, wm_GetInfo);
    api_bind("WAVEMIX", 15, wm_ConfigureInit);
}
