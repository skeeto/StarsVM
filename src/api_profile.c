/* api_profile.c - the private-profile (.INI) API, implemented in-tree.
 *
 * These are deliberately NOT forwarded to Win32's GetPrivateProfileString and
 * friends.  Win32's versions are subject to IniFileMapping registry
 * redirection and Windows-directory virtualization, so where the file actually
 * ends up is out of our hands.  Doing the parsing here makes the location
 * exact and predictable.
 *
 * Placement rule: any filename that is not already absolute resolves against
 * the directory holding stars.exe.  That satisfies "the INI must live next to
 * the EXE" regardless of what name the game constructs at runtime - and it does
 * construct it at runtime, since there is no .ini string anywhere in the
 * binary.  A bare filename is exactly what would otherwise land in C:\Windows.
 */

#include "thunk.h"
#include "task.h"
#include "sel.h"
#include "gmem.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>

/* When disabled, supply a fixed GlobalSettings for Stars.ini */
#define GLOBAL_SETTINGS_PRESET "cXK3c0vpLLSpdAgeAMJdjUcWXpnp"  // EGGSWAIN
int prompt_serial = 0;

static void pstr(uint32_t segptr, const char *s, unsigned max)
{
    uint16_t sel = SEGPTR_SEL(segptr), off = SEGPTR_OFF(segptr);
    unsigned i = 0;
    if (!segptr || !max) return;
    for (; s[i] && i + 1 < max; i++)
        sel_wr8(sel, (uint16_t)(off + i), (uint8_t)s[i]);
    sel_wr8(sel, (uint16_t)(off + i), 0);
}

/* Resolve a profile filename to an absolute path next to the module.
 *
 * Wide, because this is a path we open ourselves rather than one we hand to the
 * guest.  The guest's own name for the file is bytes in its code page and is
 * converted here, but the directory comes from task.exedirw - what Windows
 * really calls it - so an installation under a directory the code page cannot
 * spell still finds its Stars.ini instead of a trail of question marks. */
static void ini_path(const char *name, wchar_t *out, size_t n)
{
    wchar_t wname[MAX_PATH];
    /* Only reached when the guest passes no filename at all, which Win16 took
       as a request for WIN.INI.  Stars! never does it - it builds "Stars.ini"
       at runtime, every time - so rather than plant a WIN.INI beside the game
       or invent a second settings file, a nameless call lands in the one file
       that is really there.  It is logged, because a call arriving here means
       a filename went missing somewhere upstream, and the quiet version of
       that is a setting which reads back wrong for no visible reason. */
    const wchar_t *leaf = L"Stars.ini";

    out[0] = 0;
    if (name && name[0]) {
        if (!MultiByteToWideChar(CP_ACP, 0, name, -1, wname,
                                 (int)(sizeof wname / sizeof *wname)))
            wname[0] = 0;
        if (name[1] == ':' || name[0] == '\\' || name[0] == '/') {
            if (wcslen(wname) < n) wcscpy(out, wname);   /* already absolute */
            else log_msg("profile: absolute path too long: %s\n", log_wide(wname));
            return;
        }
        leaf = wname;
    } else {
        log_msg("profile: no filename given, using %s\n", log_wide(leaf));
    }

    /* Refuse rather than truncate.  The old code clamped the directory to half
       the buffer so the filename would always fit, which meant an installation
       more than that deep silently wrote its settings to a directory that does
       not exist - the INI simply never appeared, with nothing said. */
    if (wcslen(task.exedirw) + 1 + wcslen(leaf) + 1 > n) {
        log_msg("profile: no room for %s\\%s in a %u-character path\n",
                log_wide(task.exedirw), log_wide(leaf), (unsigned)n);
        return;
    }
    _snwprintf(out, n - 1, L"%ls\\%ls", task.exedirw, leaf);
    out[n - 1] = 0;
}

/* ---- a small INI model --------------------------------------------------- */

/* The files involved are a few kilobytes, so read-modify-write of the whole
   thing is simpler than anything incremental and fast enough. */
static char *slurp(const wchar_t *path, size_t *len)
{
    FILE *f = _wfopen(path, L"rb");
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
static int ini_get(const wchar_t *path, const char *section, const char *key,
                   char *value, size_t vlen)
{
    size_t len;
    char *text;
    char *line, *next;
    int in_section = 0, found = 0;

    if (!path[0]) return 0;
    text = slurp(path, &len);
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
static int ini_set(const wchar_t *path, const char *section, const char *key,
                   const char *value)
{
    size_t len;
    char *text;
    FILE *out;
    char *line, *next;
    int in_section = 0, wrote = 0, seen_section = 0;
    wchar_t tmp[MAX_PATH + 8];

    /* ini_path refuses rather than truncates, and hands back an empty string
       when it does.  Without this the ".tmp" suffix would be the whole name and
       the settings would land in a file called ".tmp" in whatever the current
       directory happens to be - which is worse than not writing them at all,
       because it looks like it worked. */
    if (!path[0]) return 0;
    text = slurp(path, &len);

    _snwprintf(tmp, sizeof tmp / sizeof *tmp - 1, L"%.*ls.tmp",
               (int)(sizeof tmp / sizeof *tmp - 6), path);
    tmp[sizeof tmp / sizeof *tmp - 1] = 0;
    out = _wfopen(tmp, L"wb");
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
    DeleteFileW(path);
    if (!MoveFileW(tmp, path)) {
        log_msg("profile: cannot replace %s\n", log_wide(path));
        return 0;
    }
    return 1;
}

/* ---- the API ------------------------------------------------------------- */

static uint32_t p_GetPrivateProfileString(Cpu *c, Args *a)
{
    char sec[128], key[128], def[512], file[MAX_PATH];
    wchar_t path[MAX_PATH];
    char value[512];
    uint32_t secp = arg_long(a);
    uint32_t keyp = arg_long(a);
    uint32_t defp = arg_long(a);
    uint32_t bufp = arg_long(a);
    uint16_t size = arg_word(a);
    uint32_t filep = arg_long(a);

    (void)c;
    g_str(secp, sec, sizeof sec);
    g_str(keyp, key, sizeof key);
    g_str(defp, def, sizeof def);
    g_str(filep, file, sizeof file);
    ini_path(file, path, sizeof path / sizeof *path);

    int found = ini_get(path, sec, key, value, sizeof value);
    if (!found) {
        snprintf(value, sizeof value, "%s", def);
    }

    if (!strcmp(key, "GlobalSettings")) {
        if (prompt_serial) {
            snprintf(value, sizeof value, " ");  // force empty
        } else if (!found) {
            snprintf(value, sizeof value, "%s", GLOBAL_SETTINGS_PRESET);
        }
    }

    pstr(bufp, value, size);
    if (log_verbose)
        log_msg("GetPrivateProfileString [%s] %s -> \"%s\" (%s)\n",
                sec, key, value, log_wide(path));
    {
        size_t n = strlen(value);
        if (size && n > (size_t)size - 1) n = (size_t)size - 1;
        return (uint32_t)n;
    }
}

static uint32_t p_GetPrivateProfileInt(Cpu *c, Args *a)
{
    char sec[128], key[128], file[MAX_PATH], value[64];
    wchar_t path[MAX_PATH];
    uint32_t secp = arg_long(a);
    uint32_t keyp = arg_long(a);
    int16_t  def  = arg_sword(a);
    uint32_t filep = arg_long(a);
    long v;

    (void)c;
    g_str(secp, sec, sizeof sec);
    g_str(keyp, key, sizeof key);
    g_str(filep, file, sizeof file);
    ini_path(file, path, sizeof path / sizeof *path);

    if (!ini_get(path, sec, key, value, sizeof value)) v = def;
    else v = strtol(value, NULL, 0);
    if (log_verbose)
        log_msg("GetPrivateProfileInt [%s] %s -> %ld (%s)\n", sec, key, v,
                log_wide(path));
    return (uint32_t)(uint16_t)v;
}

static uint32_t p_WritePrivateProfileString(Cpu *c, Args *a)
{
    char sec[128], key[128], val[512], file[MAX_PATH];
    wchar_t path[MAX_PATH];
    uint32_t secp = arg_long(a);
    uint32_t keyp = arg_long(a);
    uint32_t valp = arg_long(a);
    uint32_t filep = arg_long(a);

    (void)c;
    g_str(secp, sec, sizeof sec);
    g_str(keyp, key, sizeof key);
    g_str(valp, val, sizeof val);
    g_str(filep, file, sizeof file);
    ini_path(file, path, sizeof path / sizeof *path);

    if (log_verbose)
        log_msg("WritePrivateProfileString [%s] %s = \"%s\" (%s)\n",
                sec, key, val, log_wide(path));
    return (uint32_t)ini_set(path, sec, keyp ? key : NULL, valp ? val : NULL);
}

void api_profile_register(void)
{
    api_bind("KERNEL", 127, p_GetPrivateProfileInt);
    api_bind("KERNEL", 128, p_GetPrivateProfileString);
    api_bind("KERNEL", 129, p_WritePrivateProfileString);
}
