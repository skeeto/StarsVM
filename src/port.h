/* port.h - the few C library gaps between the hosts the core builds on.
 *
 * The core - the interpreter, the loader, the heaps and KERNEL - is meant to
 * build anywhere GCC or Clang targets x86, because the library build
 * (src/unity_lib.c) runs it with no Win32 underneath.  What it wanted from the
 * Microsoft C runtime came down to case-insensitive comparison, which is ASCII
 * here on purpose: every string compared is a module, resource or INI name out
 * of a 1995 binary, and a locale has no business changing what they match.
 */
#ifndef PORT_H
#define PORT_H

#include <stddef.h>

static inline int ascii_lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

static inline int ascii_ncasecmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++) {
        int x = ascii_lower((unsigned char)*a);
        int y = ascii_lower((unsigned char)*b);
        if (x != y) return x - y;
        if (!x) return 0;
    }
    return 0;
}

static inline int ascii_casecmp(const char *a, const char *b)
{
    return ascii_ncasecmp(a, b, (size_t)-1);
}

#endif
