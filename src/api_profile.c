/* api_profile.c - the private-profile (.INI) API, implemented in-tree.
 *
 * These are deliberately NOT forwarded to Win32's GetPrivateProfileString and
 * friends.  Win32's versions are subject to IniFileMapping registry
 * redirection and Windows-directory virtualization, so where the file actually
 * ends up is out of our hands.  Doing the parsing here makes the location
 * exact and predictable.
 *
 * Placement rule: any filename that is not already absolute resolves against
 * the directory holding Stars!.exe.  That satisfies "the INI must live next to
 * the EXE" regardless of what name the game constructs at runtime - and it does
 * construct it at runtime, since there is no .ini string anywhere in the
 * binary.  A bare filename is exactly what would otherwise land in C:\Windows.
 */

#include "thunk.h"
#include "task.h"
#include "sel.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

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

static void pstr(uint32_t segptr, const char *s, unsigned max)
{
    uint16_t sel = SEGPTR_SEL(segptr), off = SEGPTR_OFF(segptr);
    unsigned i = 0;
    if (!segptr || !max) return;
    for (; s[i] && i + 1 < max; i++)
        sel_wr8(sel, (uint16_t)(off + i), (uint8_t)s[i]);
    sel_wr8(sel, (uint16_t)(off + i), 0);
}

/* Resolve a profile filename to an absolute path next to the game. */
static void ini_path(const char *name, char *out, size_t n)
{
    size_t dl = strlen(task.exedir);

    if (dl > n / 2) dl = n / 2;                 /* keep room for the filename */
    if (!name || !name[0]) {
        snprintf(out, n, "%.*s\\stars16.ini", (int)dl, task.exedir);
        return;
    }
    if (name[1] == ':' || name[0] == '\\' || name[0] == '/') {
        snprintf(out, n, "%.*s", (int)n - 1, name);   /* already absolute */
        return;
    }
    snprintf(out, n, "%.*s\\%.*s", (int)dl, task.exedir,
             (int)(n - dl - 2), name);
}

/* ---- a small INI model --------------------------------------------------- */

/* The files involved are a few kilobytes, so read-modify-write of the whole
   thing is simpler than anything incremental and fast enough. */
static char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    char *buf;
    long n;

    *len = 0;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    buf = malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(buf);
        return NULL;
    }
    fclose(f);
    buf[n] = 0;
    *len = (size_t)n;
    return buf;
}

static void trim(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n-1] == ' ' || s[n-1] == '\t' ||
                 s[n-1] == '\r' || s[n-1] == '\n')) s[--n] = 0;
}

static const char *skip_ws(const char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

/* Find `key` in `section`.  Returns 1 and fills `value` on success. */
static int ini_get(const char *path, const char *section, const char *key,
                   char *value, size_t vlen)
{
    size_t len;
    char *text = slurp(path, &len);
    char *line, *next;
    int in_section = 0, found = 0;

    if (!text) return 0;
    for (line = text; line && *line; line = next) {
        char *eol = strchr(line, '\n');
        next = eol ? eol + 1 : NULL;
        if (eol) *eol = 0;
        trim(line);
        {
            const char *p = skip_ws(line);
            if (*p == '[') {
                const char *end = strchr(p, ']');
                size_t n = end ? (size_t)(end - p - 1) : 0;
                in_section = (n == strlen(section) &&
                              _strnicmp(p + 1, section, n) == 0);
            } else if (in_section && *p && *p != ';') {
                const char *eq = strchr(p, '=');
                if (eq) {
                    size_t n = (size_t)(eq - p);
                    while (n && (p[n-1] == ' ' || p[n-1] == '\t')) n--;
                    if (n == strlen(key) && _strnicmp(p, key, n) == 0) {
                        snprintf(value, vlen, "%s", skip_ws(eq + 1));
                        trim(value);
                        found = 1;
                        break;
                    }
                }
            }
        }
    }
    free(text);
    return found;
}

/* Set or remove a key.  A NULL value removes the key; a NULL key removes the
   whole section.  The file is rewritten in place, preserving order. */
static int ini_set(const char *path, const char *section, const char *key,
                   const char *value)
{
    size_t len;
    char *text = slurp(path, &len);
    FILE *out;
    char *line, *next;
    int in_section = 0, wrote = 0, seen_section = 0;
    char tmp[MAX_PATH + 8];

    snprintf(tmp, sizeof tmp, "%.*s.tmp", (int)(sizeof tmp - 6), path);
    out = fopen(tmp, "wb");
    if (!out) { free(text); return 0; }

    for (line = text; line && *line; line = next) {
        char *eol = strchr(line, '\n');
        char saved[512];
        next = eol ? eol + 1 : NULL;
        if (eol) *eol = 0;
        snprintf(saved, sizeof saved, "%s", line);
        trim(saved);
        {
            const char *p = skip_ws(saved);
            if (*p == '[') {
                const char *end = strchr(p, ']');
                size_t n = end ? (size_t)(end - p - 1) : 0;
                /* Leaving a section we were editing: append the key if it was
                   not already present. */
                if (in_section && key && value && !wrote) {
                    fprintf(out, "%s=%s\r\n", key, value);
                    wrote = 1;
                }
                in_section = (n == strlen(section) &&
                              _strnicmp(p + 1, section, n) == 0);
                if (in_section) seen_section = 1;
                if (in_section && !key) { continue; }   /* drop the section */
                fprintf(out, "%s\r\n", saved);
                continue;
            }
            if (in_section && !key) continue;           /* dropping the section */
            if (in_section && key) {
                const char *eq = strchr(p, '=');
                if (eq) {
                    size_t n = (size_t)(eq - p);
                    while (n && (p[n-1] == ' ' || p[n-1] == '\t')) n--;
                    if (n == strlen(key) && _strnicmp(p, key, n) == 0) {
                        if (value) { fprintf(out, "%s=%s\r\n", key, value); wrote = 1; }
                        continue;                       /* replaced or removed */
                    }
                }
            }
            fprintf(out, "%s\r\n", saved);
        }
    }

    if (key && value && !wrote) {
        if (!seen_section) fprintf(out, "[%s]\r\n", section);
        fprintf(out, "%s=%s\r\n", key, value);
    }
    fclose(out);
    free(text);

    /* Replace atomically enough for our purposes. */
    DeleteFileA(path);
    if (!MoveFileA(tmp, path)) {
        log_msg("profile: cannot replace %s\n", path);
        return 0;
    }
    return 1;
}

/* ---- the API ------------------------------------------------------------- */

static uint32_t p_GetPrivateProfileString(Cpu *c, Args *a)
{
    char sec[128], key[128], def[512], file[MAX_PATH], path[MAX_PATH];
    char value[512];
    uint32_t secp = arg_long(a);
    uint32_t keyp = arg_long(a);
    uint32_t defp = arg_long(a);
    uint32_t bufp = arg_long(a);
    uint16_t size = arg_word(a);
    uint32_t filep = arg_long(a);

    (void)c;
    gstr(secp, sec, sizeof sec);
    gstr(keyp, key, sizeof key);
    gstr(defp, def, sizeof def);
    gstr(filep, file, sizeof file);
    ini_path(file, path, sizeof path);

    if (!ini_get(path, sec, key, value, sizeof value))
        snprintf(value, sizeof value, "%s", def);

    pstr(bufp, value, size);
    if (log_verbose)
        log_msg("GetPrivateProfileString [%s] %s -> \"%s\" (%s)\n",
                sec, key, value, path);
    {
        size_t n = strlen(value);
        if (size && n > (size_t)size - 1) n = (size_t)size - 1;
        return (uint32_t)n;
    }
}

static uint32_t p_GetPrivateProfileInt(Cpu *c, Args *a)
{
    char sec[128], key[128], file[MAX_PATH], path[MAX_PATH], value[64];
    uint32_t secp = arg_long(a);
    uint32_t keyp = arg_long(a);
    int16_t  def  = arg_sword(a);
    uint32_t filep = arg_long(a);
    long v;

    (void)c;
    gstr(secp, sec, sizeof sec);
    gstr(keyp, key, sizeof key);
    gstr(filep, file, sizeof file);
    ini_path(file, path, sizeof path);

    if (!ini_get(path, sec, key, value, sizeof value)) v = def;
    else v = strtol(value, NULL, 0);
    if (log_verbose)
        log_msg("GetPrivateProfileInt [%s] %s -> %ld (%s)\n", sec, key, v, path);
    return (uint32_t)(uint16_t)v;
}

static uint32_t p_WritePrivateProfileString(Cpu *c, Args *a)
{
    char sec[128], key[128], val[512], file[MAX_PATH], path[MAX_PATH];
    uint32_t secp = arg_long(a);
    uint32_t keyp = arg_long(a);
    uint32_t valp = arg_long(a);
    uint32_t filep = arg_long(a);

    (void)c;
    gstr(secp, sec, sizeof sec);
    gstr(keyp, key, sizeof key);
    gstr(valp, val, sizeof val);
    gstr(filep, file, sizeof file);
    ini_path(file, path, sizeof path);

    if (log_verbose)
        log_msg("WritePrivateProfileString [%s] %s = \"%s\" (%s)\n",
                sec, key, val, path);
    return (uint32_t)ini_set(path, sec, keyp ? key : NULL, valp ? val : NULL);
}

void api_profile_register(void)
{
    api_bind("KERNEL", 127, p_GetPrivateProfileInt);
    api_bind("KERNEL", 128, p_GetPrivateProfileString);
    api_bind("KERNEL", 129, p_WritePrivateProfileString);
}
