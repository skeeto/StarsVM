#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

static FILE *g_file;
static int   g_stdout_ok;
int log_verbose;
int log_console;

void log_open(const char *path)
{
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);

    /* Run from a shell, or with the output redirected, and there is already
       somewhere to write - stealing stdout with CONOUT$ in that case would
       throw it away.  Run from Explorer and there is nowhere, and this is a GUI
       binary, so a console appears only when --console asks for one. */
    g_stdout_ok = (out != NULL && out != INVALID_HANDLE_VALUE);
    if (!g_stdout_ok && log_console) {
        if (!AttachConsole(ATTACH_PARENT_PROCESS))
            AllocConsole();
        if (!freopen("CONOUT$", "w", stdout)) { /* nothing else we can do */ }
        if (!freopen("CONOUT$", "w", stderr)) { }
        g_stdout_ok = 1;
    }
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
