#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

static FILE *g_file;
static int   g_stdout_ok;
int log_verbose;
int log_console;

/* Whether writes to stdout will land anywhere.  Run from a shell, or with the
   output redirected, and there is already somewhere to write - stealing stdout
   with CONOUT$ in that case would throw it away. */
static int stdout_ready(void)
{
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    return out != NULL && out != INVALID_HANDLE_VALUE;
}

/* Somewhere for output to go.  This is a GUI binary, so a shell starts it
   without waiting and hands it no handles, and anything printed goes nowhere -
   which for --help, whose whole purpose is to be read, is the one outcome worth
   ruling out.  Borrowing the shell's own console fixes that; conjuring one is
   asked for separately, because a console of our own closes when we exit and
   takes the text with it, which the caller has to know to do anything about. */
int log_adopt_console(int conjure)
{
    if (g_stdout_ok || stdout_ready()) { g_stdout_ok = 1; return CON_ALREADY; }
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        if (!freopen("CONOUT$", "w", stdout)) return CON_NONE;
        if (!freopen("CONOUT$", "w", stderr)) { /* stdout is what counts */ }
        g_stdout_ok = 1;
        return CON_ALREADY;
    }
    if (!conjure || !AllocConsole()) return CON_NONE;
    if (!freopen("CONOUT$", "w", stdout)) return CON_NONE;
    if (!freopen("CONOUT$", "w", stderr)) { }
    if (!freopen("CONIN$", "r", stdin))   { /* only a pause needs it */ }
    g_stdout_ok = 1;
    return CON_MADE;
}

void log_open(const char *path)
{
    /* Started from Explorer there is nothing to write to and nothing to borrow,
       so a console appears only when --console asks for one.  A run of the game
       does not borrow the shell's by itself: attaching joins that console's
       Ctrl+C group, and a Ctrl+C typed at the prompt hours later - the prompt
       came back the moment we started - would then kill the game. */
    if (!g_stdout_ok) g_stdout_ok = stdout_ready();
    if (!g_stdout_ok && log_console) log_adopt_console(1);
    if (path) g_file = fopen(path, "w");
}

/* Whether anything written here will actually be seen.  When nothing will, a
   failure has to announce itself some other way. */
int log_visible(void)
{
    return g_stdout_ok || g_file != NULL;
}

void log_close(void)
{
    if (g_file) { fclose(g_file); g_file = NULL; }
}

static void vlog(const char *fmt, va_list ap)
{
    va_list ap2;
    va_copy(ap2, ap);
    if (g_stdout_ok) {
        vfprintf(stdout, fmt, ap);
        fflush(stdout);
    }
    if (g_file) { vfprintf(g_file, fmt, ap2); fflush(g_file); }
    va_end(ap2);
}

const char *log_wide(const wchar_t *s)
{
    static char buf[4][1024];
    static int next;
    char *out = buf[next++ & 3];

    if (!s) return "(null)";
    if (!WideCharToMultiByte(CP_UTF8, 0, s, -1, out, sizeof buf[0], NULL, NULL))
        snprintf(out, sizeof buf[0], "(unprintable path)");
    return out;
}

void log_msg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog(fmt, ap);
    va_end(ap);
}

void log_fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog(fmt, ap);
    va_end(ap);
    log_close();
    exit(1);
}
