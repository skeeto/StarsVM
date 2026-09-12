#ifndef LOG_H
#define LOG_H

#include <wchar.h>

void log_open(const char *path);
void log_close(void);
void log_msg(const char *fmt, ...);

/* A wide string rendered for the log.  Print paths with %s and this, never with
   %ls: vfprintf's %ls converts through the C locale, and in the default "C"
   locale it stops dead at the first character it cannot represent - which is
   precisely the paths worth logging, since a path that is pure ASCII was never
   the one in doubt.  The result is UTF-8 in one of a few rotating buffers, so
   two or three can appear in the same call. */
const char *log_wide(const wchar_t *s);
void log_fatal(const char *fmt, ...);   /* logs, then exits */

extern int log_verbose;                 /* --trace-api etc. */
extern int log_console;                 /* --console: conjure one if needed */

/* True when log output has somewhere to go.  A GUI build started from Explorer
   with no --log and no --console has nowhere, and a failure then has to be
   reported some other way. */
int log_visible(void);

/* Get output somewhere it can be seen, which a GUI binary otherwise cannot.
   Takes over the console the parent shell is using, or with conjure set makes
   one when there is none.  The answer says which, because a console of our own
   closes when the process does - text left in it is text nobody reads. */
enum { CON_NONE, CON_ALREADY, CON_MADE };
int log_adopt_console(int conjure);

#endif
