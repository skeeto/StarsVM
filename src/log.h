#ifndef LOG_H
#define LOG_H

void log_open(const char *path);
void log_close(void);
void log_msg(const char *fmt, ...);
void log_fatal(const char *fmt, ...);   /* logs, then exits */

extern int log_verbose;                 /* --trace-api etc. */
extern int log_console;                 /* --console: conjure one if needed */

/* True when log output has somewhere to go.  A GUI build started from Explorer
   with no --log and no --console has nowhere, and a failure then has to be
   reported some other way. */
int log_visible(void);

#endif
