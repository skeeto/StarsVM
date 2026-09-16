/* harness.c - the LLM harness: a named-pipe control channel for dev builds.
 *
 * Nothing here exists in a release build.  harness.h declares the hooks as
 * no-ops unless STARSVM_HARNESS is defined, and src/unity_harness.c defines it
 * and includes unity.c, exactly as src/unity_prof.c does for the profiler.  The
 * emulator carries the call sites and none of the machinery.
 *
 * The channel is a named pipe the emulator serves.  harness_pump() runs from
 * the message-loop safe points - the two message APIs and the two window-
 * procedure bridges - so every request is executed on the game's own thread,
 * with no locks and no extra threads.  A client that wants the pump to run
 * while the game sits idle in GetMessage posts it a harmless message; the
 * tools/starsmcp.py shim does exactly that after every request.
 *
 * The wire format is one request per line in and one JSON response per line
 * out.  Requests are simple words and numbers, so nothing here needs a JSON
 * parser; only a small writer, which is a hundred lines and no dependency.
 *
 *   PING                          liveness
 *   OBSERVE                       the whole window tree, with control ids
 *   WINDOW   <hwnd>               one window and its children
 *   CLICK    <hwnd> <id>          press a dialog control by id
 *   COMMAND  <hwnd> <id>          WM_COMMAND, which is what a menu item is
 *   SETTEXT  <hwnd> <id> <text>   set a control's text
 *   KEY      <hwnd> <vk>          post a key down/up
 *   TYPE     <hwnd> <text>        post each character as WM_CHAR
 *   MEMREAD  <sel> <off> <len>    guest bytes as hex
 *   MEMFIND  <hex>                search the selector arena for a pattern
 *   MEMWRITE <sel> <off> <hex>    write guest bytes
 *   STATUS                        connected, and whether a client is attached
 *
 * Every id is the guest's own: dialog item ids come straight out of the
 * template (dlg.c), and child-window ids are the ones CreateWindow was given
 * (api_user.c), so `CLICK <dialog> 201` names the same control on every run.
 */

#include "harness.h"

#ifdef STARSVM_HARNESS

#include "log.h"
#include "handle.h"
#include "sel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define HZ_IN    (64u * 1024u)
#define HZ_OUT   (256u * 1024u)
#define HZ_LINE  1024
#define HZ_WINS  512

/* ---- state ---------------------------------------------------------------- */

static const char *hz_name;
static int      hz_started;

/* Injected input state.  The harness drives the game by posting messages into
   its own queue, but the game reads the cursor and the modifier keys through
   GetCursorPos and GetKeyState, which by default ask the host and so would not
   reflect anything we injected.  Once we inject our first click we take those
   over: the cursor in screen coordinates, and a down/up bit per virtual key.
   The modifiers of the most recent injected input stay set, because the click
   is posted and handled after this command returns. */
static POINT hz_cursor;
static int   hz_have_cursor;
static BYTE  hz_keys[256];
static int   hz_mods;

/* ---- popup menus ---------------------------------------------------------- */

#define HZ_MENU_MAX 64

static int  hz_menu_count;
static int  hz_menu_id[HZ_MENU_MAX];
static char hz_menu_text[HZ_MENU_MAX][64];
static int  hz_menu_pick_set;
static int  hz_menu_pick_idx;

void harness_menu_record(void *menu)
{
    HMENU m = (HMENU)menu;
    int n = GetMenuItemCount(m), i, k = 0;

    hz_menu_count = 0;
    for (i = 0; i < n && k < HZ_MENU_MAX; i++) {
        UINT id = GetMenuItemID(m, i);
        if (id == (UINT)-1) continue;      /* separator */
        hz_menu_text[k][0] = 0;
        GetMenuStringA(m, i, hz_menu_text[k], (int)sizeof hz_menu_text[k], MF_BYPOSITION);
        if (!hz_menu_text[k][0]) continue; /* owner-drawn separator */
        hz_menu_id[k] = (int)id;
        k++;
    }
    hz_menu_count = k;
}

/* Wait for the agent's pick.  The game is blocked here inside TrackPopupMenu,
   which is not a place the normal pump reaches, so we run it ourselves: the
   agent's next requests (MENUITEMS, then PICK) are served while we wait, and a
   timeout keeps a menu nobody answers from wedging the game forever. */
int harness_menu_wait(void)
{
    int i;

    for (i = 0; i < 500 && !hz_menu_pick_set; i++) {
        harness_pump();
        Sleep(10);
    }
    if (!hz_menu_pick_set) return 0;
    hz_menu_pick_set = 0;
    if (hz_menu_pick_idx < 0 || hz_menu_pick_idx >= hz_menu_count) return 0;
    return hz_menu_id[hz_menu_pick_idx];
}

int harness_input_active(void) { return hz_have_cursor; }

int harness_input_cursor(int *x, int *y)
{
    if (!hz_have_cursor) return 0;
    *x = hz_cursor.x;
    *y = hz_cursor.y;
    return 1;
}

int harness_input_key(int vk)
{
    if (vk < 0 || vk > 255) return 0;
    return (hz_keys[vk] & 0x80) ? 0x8000 : 0;
}

static void hz_key_set(int vk, int down)
{
    if (vk < 0 || vk > 255) return;
    if (down) hz_keys[vk] |= 0x80; else hz_keys[vk] = 0;
}

static void hz_mods_set(int flags)
{
    int m = flags & (MK_SHIFT | MK_CONTROL);
    if ((m & MK_SHIFT) != (hz_mods & MK_SHIFT)) hz_key_set(VK_SHIFT, (m & MK_SHIFT) != 0);
    if ((m & MK_CONTROL) != (hz_mods & MK_CONTROL)) hz_key_set(VK_CONTROL, (m & MK_CONTROL) != 0);
    hz_mods = m;
}

static HANDLE     hz_pipe = INVALID_HANDLE_VALUE;
static HANDLE     hz_conn_ev, hz_read_ev, hz_write_ev;
static OVERLAPPED hz_conn_ov, hz_read_ov, hz_write_ov;
static int        hz_connecting, hz_connected, hz_reading, hz_writing;

static char  hz_in[HZ_IN];
static DWORD hz_inlen;
static char  hz_out[HZ_OUT];
static DWORD hz_outlen, hz_outsent;
static char  hz_line[HZ_LINE];
static int   hz_pumping;

static HWND hz_wins[HZ_WINS];
static int  hz_nwins;

/* A bounded journal of high-level UI events, so a client can ask what happened
   since it last looked instead of diffing screenshots.  The oldest entry sits
   at hz_jhead, which is where the next write goes. */
#define HZ_JOURNAL 512
static struct {
    unsigned long seq;
    char          kind[16];
    uintptr_t     hwnd;
    long          a, b;
    char          cls[16];
    char          title[80];
} hz_jbuf[HZ_JOURNAL];
static unsigned long hz_jseq;
static int           hz_jhead;

void harness_event(const char *kind, uintptr_t hwnd, long a, long b)
{
    if (!hz_started) return;
    hz_jbuf[hz_jhead].seq  = ++hz_jseq;
    snprintf(hz_jbuf[hz_jhead].kind, sizeof hz_jbuf[hz_jhead].kind,
             "%s", kind);
    hz_jbuf[hz_jhead].hwnd = hwnd;
    hz_jbuf[hz_jhead].a    = a;
    hz_jbuf[hz_jhead].b    = b;
    /* A bare handle does not say which window it was, and by the time the
       client reads the entry the window is usually gone, so the names are
       taken now, at the only moment they can be. */
    hz_jbuf[hz_jhead].cls[0] = 0;
    hz_jbuf[hz_jhead].title[0] = 0;
    if (hwnd && IsWindow((HWND)hwnd)) {
        GetClassNameA((HWND)hwnd, hz_jbuf[hz_jhead].cls,
                      (int)sizeof hz_jbuf[hz_jhead].cls);
        GetWindowTextA((HWND)hwnd, hz_jbuf[hz_jhead].title,
                       (int)sizeof hz_jbuf[hz_jhead].title);
    }
    hz_jhead = (hz_jhead + 1) % HZ_JOURNAL;
}

/* What each window drew during its last paint: the text, with the point it was
   drawn at, and the small primitives.  This is how the game's own-drawn panes -
   the tutor, the messages, and the map - are read: the emulator already sees
   every TextOut and every rectangle, and a star on the map is a tiny one, so
   the map can be reported as named coordinates instead of pixels. */
#define HZ_TEXTS 32
#define HZ_TEXT  8192
#define HZ_TMAX  1024
#define HZ_DMAX  8192
static struct {
    HWND hwnd;
    int  len;
    char buf[HZ_TEXT];
    int  ntext;
    struct { int x, y; char s[80]; } text[HZ_TMAX];
    int  ndraw;
    struct { char kind[8]; int x, y, w, h; } draw[HZ_DMAX];
} hz_texts[HZ_TEXTS];

static int hz_text_slot(HWND h, int create)
{
    int i, free_slot = -1;
    for (i = 0; i < HZ_TEXTS; i++) {
        if (hz_texts[i].hwnd == h) return i;
        if (create && !hz_texts[i].hwnd && free_slot < 0) free_slot = i;
    }
    if (create && free_slot >= 0) {
        hz_texts[free_slot].hwnd = h;
        hz_texts[free_slot].len = 0;
        hz_texts[free_slot].buf[0] = 0;
        hz_texts[free_slot].ntext = 0;
        hz_texts[free_slot].ndraw = 0;
        return free_slot;
    }
    return -1;
}

static int hz_slot_for_dc(void *hdc)
{
    HWND h = WindowFromDC((HDC)hdc);
    if (!h) return -1;
    return hz_text_slot(h, 1);
}

void harness_text_begin(unsigned hwnd16)
{
    int i;
    if (!hz_started) return;
    i = hz_text_slot(HWND_32((uint16_t)hwnd16), 1);
    if (i >= 0) {
        hz_texts[i].len = 0;
        hz_texts[i].ntext = 0;
        hz_texts[i].ndraw = 0;
    }
}

/* The last message box the game asked for, so the agent can read what it said
   even though the box was answered for it.  The sequence number lets a client
   tell a fresh box from the last one it already read: the text alone is
   indistinguishable from a repeat of the same complaint. */
static char hz_lastmsg[1024];
static unsigned long hz_msgseq;

int harness_msgbox(const char *caption, const char *text, unsigned type)
{
    int r = IDOK;
    if (!hz_started) return r;
    switch (type & 0xF) {
    case MB_YESNO:
    case MB_YESNOCANCEL:     r = IDYES;   break;
    case MB_RETRYCANCEL:     r = IDRETRY; break;
    case MB_ABORTRETRYIGNORE: r = IDABORT; break;
    case MB_OKCANCEL:        r = IDOK;    break;
    default:                 r = IDOK;    break;
    }
    hz_msgseq++;
    snprintf(hz_lastmsg, sizeof hz_lastmsg, "%s\n%s",
             caption ? caption : "", text ? text : "");
    harness_event("msgbox", 0, r, 0);
    log_msg("harness: auto-answered message box [%s] \"%s\" -> %d\n",
            caption ? caption : "", text ? text : "", r);
    return r;
}

void harness_text(void *hdc, int x, int y, const char *s, int len)
{
    int i, n;

    if (!hz_started) return;
    i = hz_slot_for_dc(hdc);
    if (i < 0) return;
    if (len < 0) len = (int)strlen(s);
    if (len > 0) {
        n = hz_texts[i].len;
        if (n && n < HZ_TEXT - 1) hz_texts[i].buf[n++] = '\n';
        if (len > HZ_TEXT - 1 - n) len = HZ_TEXT - 1 - n;
        memcpy(hz_texts[i].buf + n, s, len);
        hz_texts[i].len = n + len;
        hz_texts[i].buf[hz_texts[i].len] = 0;
        if (hz_texts[i].ntext < HZ_TMAX) {
            int k = hz_texts[i].ntext++;
            hz_texts[i].text[k].x = x;
            hz_texts[i].text[k].y = y;
            snprintf(hz_texts[i].text[k].s, sizeof hz_texts[i].text[k].s,
                     "%.79s", s);
        }
    }
}

void harness_draw(void *hdc, const char *kind, int x, int y, int w, int h)
{
    int i;
    if (!hz_started) return;
    i = hz_slot_for_dc(hdc);
    if (i < 0) return;
    if (hz_texts[i].ndraw < HZ_DMAX) {
        int k = hz_texts[i].ndraw++;
        snprintf(hz_texts[i].draw[k].kind, sizeof hz_texts[i].draw[k].kind,
                 "%.7s", kind);
        hz_texts[i].draw[k].x = x;
        hz_texts[i].draw[k].y = y;
        hz_texts[i].draw[k].w = w;
        hz_texts[i].draw[k].h = h;
    }
}

/* ---- output --------------------------------------------------------------- */

static void hz_puts(const char *s)
{
    size_t n = strlen(s);
    if (hz_outlen + n > sizeof hz_out - 1)
        n = sizeof hz_out - 1 - hz_outlen;
    memcpy(hz_out + hz_outlen, s, n);
    hz_outlen += (DWORD)n;
}

static void hz_putc(char c)
{
    if (hz_outlen < sizeof hz_out - 1) hz_out[hz_outlen++] = c;
}

static void hz_putd(long v)
{
    char b[32];
    snprintf(b, sizeof b, "%ld", v);
    hz_puts(b);
}

static void hz_put_hex(uintptr_t v)
{
    static const char d[] = "0123456789abcdef";
    char b[2 + sizeof(uintptr_t) * 2 + 1];
    int n = (int)(sizeof(uintptr_t) * 2), i;
    b[0] = '0'; b[1] = 'x';
    for (i = 0; i < n; i++) b[2 + i] = d[(v >> (4 * (n - 1 - i))) & 0xF];
    b[2 + n] = 0;
    hz_puts(b);
}

/* A string as a JSON string.  Bytes outside ASCII are emitted as \u00XX rather
   than passed through, so the response is always valid UTF-8 even though the
   game's own text is in the ANSI code page. */
static void hz_put_str(const char *s)
{
    hz_putc('"');
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { hz_putc('\\'); hz_putc((char)c); }
        else if (c == '\n') hz_puts("\\n");
        else if (c == '\r') hz_puts("\\r");
        else if (c == '\t') hz_puts("\\t");
        else if (c < 0x20 || c >= 0x80) {
            char b[8];
            snprintf(b, sizeof b, "\\u%04x", c);
            hz_puts(b);
        } else {
            hz_putc((char)c);
        }
    }
    hz_putc('"');
}

static void hz_reply_ok(void) { hz_puts("{\"ok\":true}\n"); }

static void hz_reply_err(const char *m)
{
    hz_puts("{\"ok\":false,\"error\":");
    hz_put_str(m);
    hz_puts("}\n");
}

/* ---- observation ---------------------------------------------------------- */

static BOOL CALLBACK hz_enum_top(HWND h, LPARAM lp)
{
    DWORD pid;
    (void)lp;
    GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId() && hz_nwins < HZ_WINS)
        hz_wins[hz_nwins++] = h;
    return TRUE;
}

static BOOL CALLBACK hz_enum_child(HWND h, LPARAM lp)
{
    (void)lp;
    if (hz_nwins < HZ_WINS) hz_wins[hz_nwins++] = h;
    return TRUE;
}

static void hz_emit_window(HWND h, int first)
{
    char cls[128], title[512];
    RECT r;
    LONG style;
    HWND parent, owner;
    int id, dialog;

    GetClassNameA(h, cls, sizeof cls);
    GetWindowTextA(h, title, sizeof title);
    GetWindowRect(h, &r);
    parent = GetParent(h);
    owner  = GetWindow(h, GW_OWNER);
    style  = (LONG)GetWindowLongPtrA(h, GWL_STYLE);
    id     = GetDlgCtrlID(h);
    dialog = !_stricmp(cls, "#32770");

    if (!first) hz_putc(',');
    hz_puts("{\"hwnd\":\"");
    hz_put_hex((uintptr_t)h);
    hz_puts("\",\"class\":");
    hz_put_str(cls);
    hz_puts(",\"title\":");
    hz_put_str(title);
    hz_puts(",\"id\":");
    hz_putd(id);
    hz_puts(",\"rect\":[");
    hz_putd(r.left); hz_putc(','); hz_putd(r.top); hz_putc(',');
    hz_putd(r.right - r.left); hz_putc(','); hz_putd(r.bottom - r.top);
    hz_puts("],\"visible\":");
    hz_puts(IsWindowVisible(h) ? "true" : "false");
    hz_puts(",\"enabled\":");
    hz_puts(IsWindowEnabled(h) ? "true" : "false");
    if (parent) { hz_puts(",\"parent\":\""); hz_put_hex((uintptr_t)parent); hz_putc('"'); }
    if (owner)  { hz_puts(",\"owner\":\"");  hz_put_hex((uintptr_t)owner);  hz_putc('"'); }
    if (dialog) hz_puts(",\"dialog\":true");

    if (!_stricmp(cls, "Button")) {
        int bs = (int)(style & 0xF);
        const char *kind = "button";
        if (bs == 2 || bs == 3 || bs == 5 || bs == 6) kind = "checkbox";
        else if (bs == 4 || bs == 9) kind = "radio";
        else if (bs == 7) kind = "groupbox";
        hz_puts(",\"kind\":\"");
        hz_puts(kind);
        hz_putc('"');
        if (bs != 7) {
            LRESULT ck = SendMessageA(h, BM_GETCHECK, 0, 0);
            hz_puts(",\"checked\":");
            hz_puts(ck ? "true" : "false");
        }
    } else if (!_stricmp(cls, "ComboBox")) {
        LRESULT sel = SendMessageA(h, CB_GETCURSEL, 0, 0);
        hz_puts(",\"kind\":\"combo\",\"selected\":");
        hz_putd((long)sel);
    } else if (!_stricmp(cls, "ListBox")) {
        LRESULT sel = SendMessageA(h, LB_GETCURSEL, 0, 0);
        hz_puts(",\"kind\":\"list\",\"selected\":");
        hz_putd((long)sel);
    } else if (!_stricmp(cls, "Edit")) {
        hz_puts(",\"kind\":\"edit\"");
    } else if (!_stricmp(cls, "Static")) {
        hz_puts(",\"kind\":\"static\"");
    }
    hz_putc('}');
}

static void hz_observe(void)
{
    int i, ntop;

    hz_nwins = 0;
    EnumWindows(hz_enum_top, 0);
    ntop = hz_nwins;
    for (i = 0; i < ntop; i++)
        EnumChildWindows(hz_wins[i], hz_enum_child, 0);

    hz_puts("{\"ok\":true,\"windows\":[");
    for (i = 0; i < hz_nwins; i++)
        hz_emit_window(hz_wins[i], i == 0);
    hz_puts("]}\n");
}

/* One window and its whole child tree.  A dialog's controls are not stable
   across actions - the message pane's Goto becomes a View once the subject is
   focused - so the client re-reads the dialog after acting, and this is the
   cheap way to do it. */
static void hz_window(HWND h)
{
    int i;

    if (!IsWindow(h)) { hz_reply_err("no such window"); return; }
    hz_nwins = 0;
    hz_wins[hz_nwins++] = h;
    EnumChildWindows(h, hz_enum_child, 0);

    hz_puts("{\"ok\":true,\"windows\":[");
    for (i = 0; i < hz_nwins; i++)
        hz_emit_window(hz_wins[i], i == 0);
    hz_puts("]}\n");
}

/* Idle means nothing is queued for this thread and no window of ours is waiting
   to repaint - the state a client waits for after acting.  `queued` counts the
   pending message (PeekMessage does not remove it); `dirty` counts windows with
   a non-empty update region. */
static void hz_idle(void)
{
    MSG m;
    int queued, dirty = 0, i, ntop;

    /* Look for any message but WM_NULL, and remove nothing.  WM_NULL is the
       client's wake-up, and counting it as activity would mean the game is
       never idle by its own asking; removing it, though, is worse - the game
       posts WM_NULL to itself, so a drain loop spins forever and wedges the
       very message it was called from. */
    queued = PeekMessageA(&m, NULL, 1, 0xFFFF, PM_NOREMOVE) ? 1 : 0;

    hz_nwins = 0;
    EnumWindows(hz_enum_top, 0);
    ntop = hz_nwins;
    for (i = 0; i < ntop; i++)
        EnumChildWindows(hz_wins[i], hz_enum_child, 0);
    for (i = 0; i < hz_nwins; i++) {
        RECT r;
        if (GetUpdateRect(hz_wins[i], &r, FALSE)) dirty++;
    }

    hz_puts("{\"ok\":true,\"idle\":");
    hz_puts((!queued && !dirty) ? "true" : "false");
    hz_puts(",\"queued\":");
    hz_putd(queued);
    hz_puts(",\"dirty\":");
    hz_putd(dirty);
    hz_puts(",\"seq\":");
    hz_putd((long)hz_jseq);
    hz_puts("}\n");
}

static void hz_journal(long since)
{
    int i, first = 1;

    hz_puts("{\"ok\":true,\"seq\":");
    hz_putd((long)hz_jseq);
    hz_puts(",\"events\":[");
    for (i = 0; i < HZ_JOURNAL; i++) {
        int s = (hz_jhead + i) % HZ_JOURNAL;
        if (hz_jbuf[s].seq <= (unsigned long)since) continue;
        if (!first) hz_putc(',');
        first = 0;
        hz_puts("{\"seq\":");
        hz_putd((long)hz_jbuf[s].seq);
        hz_puts(",\"kind\":");
        hz_put_str(hz_jbuf[s].kind);
        hz_puts(",\"hwnd\":\"");
        hz_put_hex(hz_jbuf[s].hwnd);
        hz_puts("\",\"a\":");
        hz_putd(hz_jbuf[s].a);
        hz_puts(",\"b\":");
        hz_putd(hz_jbuf[s].b);
        hz_puts(",\"class\":");
        hz_put_str(hz_jbuf[s].cls);
        hz_puts(",\"title\":");
        hz_put_str(hz_jbuf[s].title);
        hz_putc('}');
    }
    hz_puts("]}\n");
}

/* List a combo box's items, and select one by index, sending the parent the
   CBN_SELCHANGE it would have got from a real pick.  Many of the game's lists
   are combos with no control id, so the handle is the address. */
static void hz_combo(HWND h)
{
    int n = (int)SendMessageA(h, CB_GETCOUNT, 0, 0);
    int sel = (int)SendMessageA(h, CB_GETCURSEL, 0, 0);
    int i;

    hz_puts("{\"ok\":true,\"count\":");
    hz_putd(n);
    hz_puts(",\"sel\":");
    hz_putd(sel);
    hz_puts(",\"items\":[");
    for (i = 0; i < n; i++) {
        char b[256];
        LRESULT len = SendMessageA(h, CB_GETLBTEXTLEN, i, 0);
        if (len < 0 || len > (LRESULT)sizeof b - 1) len = (LRESULT)sizeof b - 1;
        SendMessageA(h, CB_GETLBTEXT, i, (LPARAM)b);
        b[len] = 0;
        if (i) hz_putc(',');
        hz_put_str(b);
    }
    hz_puts("]}\n");
}

static void hz_combo_sel(HWND h, int idx)
{
    LRESULT r = SendMessageA(h, CB_SETCURSEL, idx, 0);
    SendMessageA(GetParent(h), WM_COMMAND,
                 MAKEWPARAM((WORD)GetDlgCtrlID(h), CBN_SELCHANGE), (LPARAM)h);
    hz_puts("{\"ok\":true,\"sel\":");
    hz_putd((long)r);
    hz_puts("}\n");
}

/* List boxes, which the production and design dialogs are built from.  Same
   idea as the combos: read the items, pick one by index. */
static void hz_list(HWND h)
{
    int n = (int)SendMessageA(h, LB_GETCOUNT, 0, 0);
    int sel = (int)SendMessageA(h, LB_GETCURSEL, 0, 0);
    int i;

    hz_puts("{\"ok\":true,\"count\":");
    hz_putd(n);
    hz_puts(",\"sel\":");
    hz_putd(sel);
    hz_puts(",\"items\":[");
    for (i = 0; i < n; i++) {
        char b[256];
        LRESULT len = SendMessageA(h, LB_GETTEXTLEN, i, 0);
        if (len < 0 || len > (LRESULT)sizeof b - 1) len = (LRESULT)sizeof b - 1;
        SendMessageA(h, LB_GETTEXT, i, (LPARAM)b);
        b[len] = 0;
        if (i) hz_putc(',');
        hz_put_str(b);
    }
    hz_puts("]}\n");
}

static void hz_list_sel(HWND h, int idx)
{
    SendMessageA(h, LB_SETCURSEL, idx, 0);
    SendMessageA(GetParent(h), WM_COMMAND,
                 MAKEWPARAM((WORD)GetDlgCtrlID(h), LBN_SELCHANGE), (LPARAM)h);
    hz_puts("{\"ok\":true}\n");
}

/* Everything a window drew last paint, with coordinates: the text it put down
   and the primitives it filled.  On the map a star is a tiny filled shape and
   its name is text just below, so this turns the picture into named points. */
static void hz_map(HWND h)
{
    int i = hz_text_slot(h, 0), k;

    hz_puts("{\"ok\":true,\"texts\":[");
    if (i >= 0)
        for (k = 0; k < hz_texts[i].ntext; k++) {
            if (k) hz_putc(',');
            hz_putc('[');
            hz_putd(hz_texts[i].text[k].x); hz_putc(',');
            hz_putd(hz_texts[i].text[k].y); hz_putc(',');
            hz_put_str(hz_texts[i].text[k].s); hz_putc(']');
        }
    hz_puts("],\"draws\":[");
    if (i >= 0)
        for (k = 0; k < hz_texts[i].ndraw; k++) {
            if (k) hz_putc(',');
            hz_putc('[');
            hz_put_str(hz_texts[i].draw[k].kind); hz_putc(',');
            hz_putd(hz_texts[i].draw[k].x); hz_putc(',');
            hz_putd(hz_texts[i].draw[k].y); hz_putc(',');
            hz_putd(hz_texts[i].draw[k].w); hz_putc(',');
            hz_putd(hz_texts[i].draw[k].h); hz_putc(']');
        }
    hz_puts("]}\n");
}

/* ---- guest memory --------------------------------------------------------- */

/* The guest's address space is the selector arena: index = sel >> 3, and the
   bytes a selector addresses run for sel_tab[index].limit + 1 (the remainder,
   for a selector in the middle of a huge block).  These commands let a client
   read the game's own structures - the star table, the fleet list - instead of
   inferring them from what the game happens to draw, and write the few fields
   that stand in for a UI gesture, the map's scroll position above all.
   Reads and finds are bounded by the block's limit, so a bad request is an
   error and not a peek at some other block. */

static int hz_hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int hz_parse_hex(const char *s, unsigned char *out, int max, int *n)
{
    int k = 0;
    while (s[0] && s[1]) {
        int hi = hz_hexval(s[0]), lo = hz_hexval(s[1]);
        if (hi < 0 || lo < 0 || k >= max) return 0;
        out[k++] = (unsigned char)((hi << 4) | lo);
        s += 2;
    }
    if (s[0]) return 0;
    *n = k;
    return 1;
}

static unsigned char *hz_sel_span(uint16_t sel, long off, long len)
{
    unsigned i = SEL_INDEX(sel);
    if (i >= SEL_SLOTS || !sel_live[i]) return NULL;
    if (off < 0 || len <= 0) return NULL;
    if ((unsigned long)off + (unsigned long)len > sel_tab[i].limit + 1)
        return NULL;
    return sel_arena + ((size_t)i << 16) + off;
}

static void hz_memread(uint16_t sel, long off, long len)
{
    static const char d[] = "0123456789abcdef";
    unsigned char *p;
    long i;

    if (len > 65536) len = 65536;
    p = hz_sel_span(sel, off, len);
    if (!p) { hz_reply_err("bad range"); return; }
    hz_puts("{\"ok\":true,\"hex\":\"");
    for (i = 0; i < len; i++) {
        hz_putc(d[p[i] >> 4]);
        hz_putc(d[p[i] & 0xF]);
    }
    hz_puts("\"}\n");
}

/* Search every live selector for a byte pattern - a name, or a known value in
   little-endian - and report where it sits.  This is how a structure is found:
   search for a string the UI shows, then read around the hit. */
#define HZ_FIND_MAX 256
static void hz_memfind(const char *hex)
{
    unsigned char pat[256];
    static struct { uint16_t sel; long off; } found[HZ_FIND_MAX];
    int patlen, i, hits = 0;
    unsigned idx;

    if (!hz_parse_hex(hex, pat, (int)sizeof pat, &patlen) || patlen == 0) {
        hz_reply_err("bad hex pattern");
        return;
    }
    for (idx = 1; idx < SEL_SLOTS && hits < HZ_FIND_MAX; idx++) {
        unsigned char *base;
        unsigned long n, j;
        if (!sel_live[idx]) continue;
        base = sel_arena + ((size_t)idx << 16);
        n = sel_tab[idx].limit + 1;
        if (n > SEL_SLOT) n = SEL_SLOT;
        for (j = 0; j + (unsigned long)patlen <= n; j++) {
            if (base[j] != pat[0] || memcmp(base + j, pat, (size_t)patlen))
                continue;
            found[hits].sel = SEL_MAKE(idx);
            found[hits].off = (long)j;
            if (++hits >= HZ_FIND_MAX) break;
        }
    }
    hz_puts("{\"ok\":true,\"count\":");
    hz_putd(hits);
    hz_puts(",\"hits\":[");
    for (i = 0; i < hits; i++) {
        if (i) hz_putc(',');
        hz_puts("{\"sel\":\"");
        hz_put_hex(found[i].sel);
        hz_puts("\",\"off\":");
        hz_putd(found[i].off);
        hz_putc('}');
    }
    hz_puts("]}\n");
}

static void hz_memwrite(uint16_t sel, long off, const char *hex)
{
    unsigned char buf[4096];
    unsigned char *p;
    int n;

    if (!hz_parse_hex(hex, buf, (int)sizeof buf, &n) || n == 0) {
        hz_reply_err("bad hex data");
        return;
    }
    p = hz_sel_span(sel, off, n);
    if (!p) { hz_reply_err("bad range"); return; }
    memcpy(p, buf, (size_t)n);
    hz_puts("{\"ok\":true,\"len\":");
    hz_putd(n);
    hz_puts("}\n");
}

static void hz_text_show(HWND h)
{
    int i = hz_text_slot(h, 0);
    hz_puts("{\"ok\":true,\"hwnd\":\"");
    hz_put_hex((uintptr_t)h);
    hz_puts("\",\"text\":");
    hz_put_str(i >= 0 ? hz_texts[i].buf : "");
    hz_puts("}\n");
}

/* Capture a window to a 32-bit BMP.  BMP needs no library, and the shim turns
   it into a PNG for the agent to look at; a compressor in the emulator would be
   a dependency and a bug surface for no gain. */
static void hz_shot(HWND h, const char *path)
{
    RECT r;
    HDC dc, mem;
    BITMAPINFO bi;
    void *bits;
    HBITMAP bmp, old;
    BITMAPFILEHEADER fh;
    BITMAPINFOHEADER ih;
    FILE *f;
    DWORD row;
    int w, hgt;

    if (!GetWindowRect(h, &r)) { hz_reply_err("no window"); return; }
    w = r.right - r.left;
    hgt = r.bottom - r.top;
    if (w <= 0 || hgt <= 0 || w > 8192 || hgt > 8192) {
        hz_reply_err("bad window size");
        return;
    }

    dc = GetDC(NULL);
    mem = CreateCompatibleDC(dc);
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = hgt;          /* bottom-up */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bmp || !bits) {
        DeleteDC(mem); ReleaseDC(NULL, dc);
        hz_reply_err("cannot make a bitmap");
        return;
    }
    old = SelectObject(mem, bmp);
    PrintWindow(h, mem, 0);
    SelectObject(mem, old);

    f = fopen(path, "wb");
    if (!f) {
        DeleteObject(bmp); DeleteDC(mem); ReleaseDC(NULL, dc);
        hz_reply_err("cannot open the output path");
        return;
    }
    row = (DWORD)w * 4;
    memset(&fh, 0, sizeof fh);
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof fh + sizeof ih;
    fh.bfSize = fh.bfOffBits + row * (DWORD)hgt;
    memset(&ih, 0, sizeof ih);
    ih.biSize = sizeof ih;
    ih.biWidth = w;
    ih.biHeight = hgt;
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    ih.biCompression = BI_RGB;
    ih.biSizeImage = row * (DWORD)hgt;
    fwrite(&fh, sizeof fh, 1, f);
    fwrite(&ih, sizeof ih, 1, f);
    fwrite(bits, 1, row * (DWORD)hgt, f);
    fclose(f);

    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(NULL, dc);
    hz_reply_ok();
}

/* ---- commands ------------------------------------------------------------- */

/* Click a dialog control the way a user would: a button gets BM_CLICK, so the
   control itself decides what notification to send; anything else gets a
   synthetic press at its center, which is what the game's own-drawn controls
   read.
 *
 * Posted, never sent.  A button whose WM_COMMAND opens a modal dialog would
 * otherwise block here until the dialog closed, and the client would wait for a
 * reply that cannot come until it acts on the dialog it has not been told
 * about - a deadlock by construction.  A real click is posted too, so this is
 * also the faithful thing: the effect lands on the next turn of the guest's
 * message loop, which the client reaches by observing or waiting. */
static void hz_click_control(HWND c)
{
    char cls[64];

    GetClassNameA(c, cls, sizeof cls);
    if (!_stricmp(cls, "Button")) {
        PostMessageA(c, BM_CLICK, 0, 0);
    } else {
        RECT r;
        LPARAM lp;
        GetClientRect(c, &r);
        lp = MAKELPARAM((r.right - r.left) / 2, (r.bottom - r.top) / 2);
        PostMessageA(c, WM_LBUTTONDOWN, MK_LBUTTON, lp);
        PostMessageA(c, WM_LBUTTONUP, 0, lp);
    }
    hz_reply_ok();
}

static void hz_click(HWND parent, long id)
{
    HWND c = GetDlgItem(parent, (int)id);
    if (!c) { hz_reply_err("no such control"); return; }
    hz_click_control(c);
}

static void hz_exec(char *line)
{
    char *arg;

    while (*line == ' ') line++;
    if (!*line) return;
    arg = strchr(line, ' ');
    if (arg) { *arg++ = 0; while (*arg == ' ') arg++; }

    if (!strcmp(line, "PING")) {
        hz_puts("{\"ok\":true,\"harness\":true}\n");
    } else if (!strcmp(line, "STATUS")) {
        hz_puts("{\"ok\":true,\"connected\":");
        hz_puts(hz_connected ? "true" : "false");
        hz_puts("}\n");
    } else if (!strcmp(line, "OBSERVE")) {
        hz_observe();
    } else if (!strcmp(line, "WINDOW") && arg) {
        char *rest;
        uintptr_t h = (uintptr_t)_strtoui64(arg, &rest, 16);
        hz_window((HWND)h);
    } else if (!strcmp(line, "MEMREAD") && arg) {
        char *rest;
        uintptr_t sel = (uintptr_t)_strtoui64(arg, &rest, 16);
        long off = strtol(rest, &rest, 10);
        long len = strtol(rest, NULL, 10);
        hz_memread((uint16_t)sel, off, len);
    } else if (!strcmp(line, "MEMFIND") && arg) {
        hz_memfind(arg);
    } else if (!strcmp(line, "MEMWRITE") && arg) {
        char *rest;
        uintptr_t sel = (uintptr_t)_strtoui64(arg, &rest, 16);
        long off = strtol(rest, &rest, 10);
        while (*rest == ' ') rest++;
        hz_memwrite((uint16_t)sel, off, rest);
    } else if (!strcmp(line, "IDLE")) {
        hz_idle();
    } else if (!strcmp(line, "JOURNAL")) {
        hz_journal(arg ? strtol(arg, NULL, 10) : 0);
    } else if (!strcmp(line, "MOVE") && arg) {
        char *rest;
        uintptr_t h = (uintptr_t)_strtoui64(arg, &rest, 16);
        int x = (int)strtol(rest, &rest, 10);
        int y = (int)strtol(rest, NULL, 10);
        SetWindowPos((HWND)h, NULL, x, y, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        hz_puts("{\"ok\":true}\n");
    } else if (!strcmp(line, "REPAINT") && arg) {
        char *rest;
        uintptr_t h = (uintptr_t)_strtoui64(arg, &rest, 16);
        InvalidateRect((HWND)h, NULL, TRUE);
        UpdateWindow((HWND)h);
        hz_puts("{\"ok\":true}\n");
    } else if (!strcmp(line, "MAP") && arg) {
        char *rest;
        uintptr_t h = (uintptr_t)_strtoui64(arg, &rest, 16);
        hz_map((HWND)h);
    } else if (!strcmp(line, "LIST") && arg) {
        char *rest;
        uintptr_t h = (uintptr_t)_strtoui64(arg, &rest, 16);
        hz_list((HWND)h);
    } else if (!strcmp(line, "DBLCLK") && arg) {
        /* An optional shift flag, because a shift-double-click in a queue adds
           ten at once and the game reads GetKeyState to tell. */
        char *rest;
        uintptr_t h = (uintptr_t)_strtoui64(arg, &rest, 16);
        long shift = strtol(rest, NULL, 10);
        if (shift) { hz_key_set(VK_SHIFT, 1); hz_mods |= MK_SHIFT; }
        SendMessageA(GetParent((HWND)h), WM_COMMAND,
                     MAKEWPARAM((WORD)GetDlgCtrlID((HWND)h), LBN_DBLCLK), (LPARAM)h);
        if (shift) { hz_key_set(VK_SHIFT, 0); hz_mods &= ~MK_SHIFT; }
        hz_puts("{\"ok\":true}\n");
    } else if (!strcmp(line, "LISTSEL") && arg) {
        char *rest;
        uintptr_t h = (uintptr_t)_strtoui64(arg, &rest, 16);
        long idx = strtol(rest, NULL, 10);
        hz_list_sel((HWND)h, (int)idx);
    } else if (!strcmp(line, "COMBO") && arg) {
        char *rest;
        uintptr_t h = (uintptr_t)_strtoui64(arg, &rest, 16);
        hz_combo((HWND)h);
    } else if (!strcmp(line, "COMBOSEL") && arg) {
        char *rest;
        uintptr_t h = (uintptr_t)_strtoui64(arg, &rest, 16);
        long idx = strtol(rest, NULL, 10);
        hz_combo_sel((HWND)h, (int)idx);
    } else if (!strcmp(line, "MSGBOX")) {
        if (arg && !strcmp(arg, "clear")) {
            hz_lastmsg[0] = 0;
            hz_msgseq = 0;
            hz_reply_ok();
        } else {
            hz_puts("{\"ok\":true,\"seq\":");
            hz_putd((long)hz_msgseq);
            hz_puts(",\"text\":");
            hz_put_str(hz_lastmsg);
            hz_puts("}\n");
        }
    } else if (!strcmp(line, "MENUITEMS")) {
        int i;
        hz_puts("{\"ok\":true,\"items\":[");
        for (i = 0; i < hz_menu_count; i++) {
            char num[32];
            if (i) hz_puts(",");
            _snprintf(num, sizeof num, "{\"i\":%d,\"id\":%d,\"text\":", i, hz_menu_id[i]);
            hz_puts(num);
            hz_put_str(hz_menu_text[i]);
            hz_puts("}");
        }
        hz_puts("]}\n");
    } else if (!strcmp(line, "PICK") && arg) {
        hz_menu_pick_idx = (int)strtol(arg, NULL, 10);
        hz_menu_pick_set = 1;
        hz_puts("{\"ok\":true}\n");
    } else if (!strcmp(line, "TEXT") && arg) {
        char *rest;
        uintptr_t h = (uintptr_t)_strtoui64(arg, &rest, 16);
        hz_text_show((HWND)h);
    } else if (!strcmp(line, "SHOT") && arg) {
        char *rest;
        uintptr_t h = (uintptr_t)_strtoui64(arg, &rest, 16);
        while (*rest == ' ') rest++;
        if (*rest) hz_shot((HWND)h, rest);
        else hz_reply_err("no output path");
    } else if (!strcmp(line, "CLICK") && arg) {
        char *rest;
        uintptr_t h = (uintptr_t)_strtoui64(arg, &rest, 16);
        long id = strtol(rest, NULL, 10);
        hz_click((HWND)h, id);
    } else if (!strcmp(line, "CLICKAT") && arg) {
        /* A click at client coordinates, for the map and other surfaces that
           are not a control.  Everything is injected into the guest's own
           message queue: the cursor and the modifier keys are published to the
           emulated GetCursorPos/GetKeyState first, so a game that asks them
           sees the injected position and Shift, not the host's real input.
           Flags are the MK_ values, plus 16 = right button, 32 = double click. */
        char *rest;
        uintptr_t h = (uintptr_t)_strtoui64(arg, &rest, 16);
        long x = strtol(rest, &rest, 10);
        long y = strtol(rest, &rest, 10);
        long flags = strtol(rest, &rest, 10);
        HWND w = (HWND)h;
        LPARAM lp = MAKELPARAM(x, y);
        POINT pt;
        int mk = (int)(flags & (MK_SHIFT | MK_CONTROL));
        int right = (flags & 16) != 0, dbl = (flags & 32) != 0;
        if (!IsWindow(w)) { hz_reply_err("no such window"); return; }
        hz_menu_count = 0;    /* the menu a click opens is not recorded yet */
        pt.x = (int)x; pt.y = (int)y;
        ClientToScreen(w, &pt);
        hz_cursor = pt; hz_have_cursor = 1;
        hz_mods_set(mk);
        PostMessageA(w, WM_MOUSEMOVE, (WPARAM)mk, lp);
        if (right) {
            PostMessageA(w, WM_RBUTTONDOWN, (WPARAM)(mk | MK_RBUTTON), lp);
            PostMessageA(w, WM_RBUTTONUP, (WPARAM)mk, lp);
        } else {
            PostMessageA(w, WM_LBUTTONDOWN, (WPARAM)(mk | MK_LBUTTON), lp);
            PostMessageA(w, WM_LBUTTONUP, (WPARAM)mk, lp);
            if (dbl) {
                PostMessageA(w, WM_LBUTTONDBLCLK, (WPARAM)(mk | MK_LBUTTON), lp);
                PostMessageA(w, WM_LBUTTONUP, (WPARAM)mk, lp);
            }
        }
        hz_reply_ok();
    } else if (!strcmp(line, "DRAG") && arg) {
        /* Press at (x1,y1), move to (x2,y2), release - all in `hwnd` client
           coordinates, all injected.  For gauges and waypoint drags. */
        char *rest;
        uintptr_t h = (uintptr_t)_strtoui64(arg, &rest, 16);
        int x1 = (int)strtol(rest, &rest, 10), y1 = (int)strtol(rest, &rest, 10);
        int x2 = (int)strtol(rest, &rest, 10), y2 = (int)strtol(rest, &rest, 10);
        HWND w = (HWND)h;
        POINT pt; int i;
        if (!IsWindow(w)) { hz_reply_err("no such window"); return; }
        pt.x = x1; pt.y = y1; ClientToScreen(w, &pt); hz_cursor = pt; hz_have_cursor = 1;
        PostMessageA(w, WM_MOUSEMOVE, 0, MAKELPARAM(x1, y1));
        PostMessageA(w, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x1, y1));
        for (i = 1; i <= 8; i++) {
            int mx = x1 + (x2 - x1) * i / 8, my = y1 + (y2 - y1) * i / 8;
            pt.x = mx; pt.y = my; ClientToScreen(w, &pt); hz_cursor = pt;
            PostMessageA(w, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(mx, my));
        }
        pt.x = x2; pt.y = y2; ClientToScreen(w, &pt); hz_cursor = pt;
        PostMessageA(w, WM_LBUTTONUP, 0, MAKELPARAM(x2, y2));
        hz_reply_ok();
    } else if (!strcmp(line, "DRAG2") && arg) {
        /* Press in one window, drag to a point in another, release.  The moves
           keep going to the window that got the press (the one that will have
           captured the mouse), but the emulated cursor travels to the target,
           which is how the game decides where the drop landed. */
        char *rest;
        uintptr_t h1 = (uintptr_t)_strtoui64(arg, &rest, 16);
        int x1 = (int)strtol(rest, &rest, 10), y1 = (int)strtol(rest, &rest, 10);
        uintptr_t h2 = (uintptr_t)_strtoui64(rest, &rest, 16);
        int x2 = (int)strtol(rest, &rest, 10), y2 = (int)strtol(rest, &rest, 10);
        HWND w1 = (HWND)h1, w2 = (HWND)h2;
        POINT pt, sp, ep, cp; int i;
        if (!IsWindow(w1) || !IsWindow(w2)) { hz_reply_err("no such window"); return; }
        pt.x = x1; pt.y = y1; ClientToScreen(w1, &pt); hz_cursor = pt; hz_have_cursor = 1;
        sp = pt;
        ep.x = x2; ep.y = y2; ClientToScreen(w2, &ep);
        PostMessageA(w1, WM_MOUSEMOVE, 0, MAKELPARAM(x1, y1));
        PostMessageA(w1, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x1, y1));
        for (i = 1; i <= 8; i++) {
            cp.x = sp.x + (ep.x - sp.x) * i / 8;
            cp.y = sp.y + (ep.y - sp.y) * i / 8;
            hz_cursor = cp;
            ScreenToClient(w1, &cp);
            PostMessageA(w1, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(cp.x, cp.y));
        }
        hz_cursor = ep;
        cp = ep; ScreenToClient(w1, &cp);
        PostMessageA(w1, WM_LBUTTONUP, 0, MAKELPARAM(cp.x, cp.y));
        hz_reply_ok();
    } else if (!strcmp(line, "CLICKW") && arg) {
        /* A control by its own handle.  The game's custom panes give their
           children no control id, so GetDlgItem cannot reach them. */
        char *rest;
        uintptr_t h = (uintptr_t)_strtoui64(arg, &rest, 16);
        if (IsWindow((HWND)h)) hz_click_control((HWND)h);
        else hz_reply_err("no such window");
    } else if ((!strcmp(line, "COMMAND") || !strcmp(line, "MENU")) && arg) {
        char *rest;
        uintptr_t h = (uintptr_t)_strtoui64(arg, &rest, 16);
        long id = strtol(rest, NULL, 10);
        /* Posted for the same reason a click is: a command that opens a modal
           dialog must not hold the reply hostage. */
        PostMessageA((HWND)h, WM_COMMAND, MAKEWPARAM((WORD)id, 0), 0);
        hz_reply_ok();
    } else if (!strcmp(line, "SETTEXT") && arg) {
        char *rest;
        uintptr_t h = (uintptr_t)_strtoui64(arg, &rest, 16);
        long id = strtol(rest, &rest, 10);
        while (*rest == ' ') rest++;
        SetDlgItemTextA((HWND)h, (int)id, rest);
        hz_reply_ok();
    } else if (!strcmp(line, "KEY") && arg) {
        char *rest;
        uintptr_t h = (uintptr_t)_strtoui64(arg, &rest, 16);
        long vk = strtol(rest, NULL, 16);
        PostMessageA((HWND)h, WM_KEYDOWN, (WPARAM)vk, 1);
        PostMessageA((HWND)h, WM_KEYUP, (WPARAM)vk, 1);
        hz_reply_ok();
    } else if (!strcmp(line, "TYPE") && arg) {
        char *rest;
        uintptr_t h = (uintptr_t)_strtoui64(arg, &rest, 16);
        while (*rest == ' ') rest++;
        for (; *rest; rest++)
            PostMessageA((HWND)h, WM_CHAR, (WPARAM)(unsigned char)*rest, 1);
        hz_reply_ok();
    } else {
        hz_reply_err("unknown command");
    }
}

/* ---- the pipe ------------------------------------------------------------- */

static void hz_reset(void)
{
    if (hz_pipe != INVALID_HANDLE_VALUE) {
        DisconnectNamedPipe(hz_pipe);
        CloseHandle(hz_pipe);
        hz_pipe = INVALID_HANDLE_VALUE;
    }
    hz_connected = hz_connecting = hz_reading = hz_writing = 0;
    hz_inlen = hz_outlen = hz_outsent = 0;
}

static void hz_ensure_pipe(void)
{
    if (hz_pipe != INVALID_HANDLE_VALUE) return;

    hz_pipe = CreateNamedPipeA(hz_name,
                               PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                               PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                               PIPE_UNLIMITED_INSTANCES, HZ_OUT, HZ_IN, 0, NULL);
    if (hz_pipe == INVALID_HANDLE_VALUE) {
        log_msg("harness: CreateNamedPipe(%s) failed: %lu\n", hz_name, GetLastError());
        return;
    }
    hz_connected = hz_connecting = hz_reading = hz_writing = 0;
    hz_inlen = hz_outlen = hz_outsent = 0;

    memset(&hz_conn_ov, 0, sizeof hz_conn_ov);
    hz_conn_ov.hEvent = hz_conn_ev;
    ResetEvent(hz_conn_ev);
    if (ConnectNamedPipe(hz_pipe, &hz_conn_ov)) {
        hz_connected = 1;
    } else {
        DWORD e = GetLastError();
        if (e == ERROR_PIPE_CONNECTED) hz_connected = 1;
        else if (e == ERROR_IO_PENDING) hz_connecting = 1;
        else { log_msg("harness: ConnectNamedPipe failed: %lu\n", e); hz_reset(); }
    }
}

static void hz_connect_poll(void)
{
    DWORD n;
    if (hz_connected || !hz_connecting) return;
    if (GetOverlappedResult(hz_pipe, &hz_conn_ov, &n, FALSE)) {
        hz_connected = 1;
        hz_connecting = 0;
    } else if (GetLastError() != ERROR_IO_INCOMPLETE) {
        hz_reset();
    }
}

static void hz_process(void)
{
    for (;;) {
        char *nl = memchr(hz_in, '\n', hz_inlen);
        size_t len;
        if (!nl) break;
        len = (size_t)(nl - hz_in);
        if (len >= sizeof hz_line) len = sizeof hz_line - 1;
        memcpy(hz_line, hz_in, len);
        hz_line[len] = 0;
        if (len && hz_line[len - 1] == '\r') hz_line[len - 1] = 0;
        memmove(hz_in, nl + 1, hz_inlen - (DWORD)(nl + 1 - hz_in));
        hz_inlen -= (DWORD)(nl + 1 - hz_in);
        hz_exec(hz_line);
    }
}

static void hz_read_poll(void)
{
    DWORD n;

    if (!hz_reading) {
        if (hz_inlen >= sizeof hz_in) return;
        memset(&hz_read_ov, 0, sizeof hz_read_ov);
        hz_read_ov.hEvent = hz_read_ev;
        ResetEvent(hz_read_ev);
        if (ReadFile(hz_pipe, hz_in + hz_inlen, sizeof hz_in - hz_inlen,
                     &n, &hz_read_ov)) {
            hz_inlen += n;
            hz_process();
        } else if (GetLastError() == ERROR_IO_PENDING) {
            hz_reading = 1;
        } else {
            hz_reset();
        }
        return;
    }

    if (GetOverlappedResult(hz_pipe, &hz_read_ov, &n, FALSE)) {
        hz_reading = 0;
        if (n == 0) { hz_reset(); return; }   /* the client closed */
        hz_inlen += n;
        hz_process();
    } else if (GetLastError() != ERROR_IO_INCOMPLETE) {
        hz_reset();
    }
}

static void hz_flush(void)
{
    for (;;) {
        DWORD n;
        if (hz_outsent >= hz_outlen) {
            hz_outlen = hz_outsent = 0;
            return;
        }
        if (hz_writing) {
            if (GetOverlappedResult(hz_pipe, &hz_write_ov, &n, FALSE)) {
                hz_writing = 0;
                hz_outsent += n;
            } else if (GetLastError() == ERROR_IO_INCOMPLETE) {
                return;
            } else {
                hz_reset();
                return;
            }
            continue;
        }
        memset(&hz_write_ov, 0, sizeof hz_write_ov);
        hz_write_ov.hEvent = hz_write_ev;
        ResetEvent(hz_write_ev);
        if (WriteFile(hz_pipe, hz_out + hz_outsent, hz_outlen - hz_outsent,
                      &n, &hz_write_ov)) {
            hz_outsent += n;
        } else if (GetLastError() == ERROR_IO_PENDING) {
            hz_writing = 1;
            return;
        } else {
            hz_reset();
            return;
        }
    }
}

/* ---- entry points --------------------------------------------------------- */

void harness_start(const char *name)
{
    if (hz_started) return;
    hz_started = 1;
    hz_name = (name && *name) ? name : "\\\\.\\pipe\\StarsVM-harness";

    hz_conn_ev  = CreateEventA(NULL, TRUE, FALSE, NULL);
    hz_read_ev  = CreateEventA(NULL, TRUE, FALSE, NULL);
    hz_write_ev = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!hz_conn_ev || !hz_read_ev || !hz_write_ev) {
        log_msg("harness: cannot create events\n");
        hz_started = 0;
        return;
    }
    log_msg("harness: serving %s\n", hz_name);
}

void harness_pump(void)
{
    if (!hz_started || hz_pumping) return;
    hz_pumping = 1;

    hz_ensure_pipe();
    if (hz_pipe != INVALID_HANDLE_VALUE) {
        hz_connect_poll();
        if (hz_connected) {
            hz_read_poll();
            hz_flush();
        }
    }

    hz_pumping = 0;
}

int harness_active(void)
{
    return hz_started && hz_connected;
}

#endif /* STARSVM_HARNESS */