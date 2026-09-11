#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

static const uint16_t TABLE[128] = {
    0x0003, 0x0005, 0x0007, 0x000b, 0x000d, 0x0011, 0x0013, 0x0017,
    0x001d, 0x001f, 0x0025, 0x0029, 0x002b, 0x002f, 0x0035, 0x003b,
    0x003d, 0x0043, 0x0047, 0x0049, 0x004f, 0x0053, 0x0059, 0x0061,
    0x0065, 0x0067, 0x006b, 0x006d, 0x0071, 0x007f, 0x0083, 0x0089,
    0x008b, 0x0095, 0x0097, 0x009d, 0x00a3, 0x00a7, 0x00ad, 0x00b3,
    0x00b5, 0x00bf, 0x00c1, 0x00c5, 0x00c7, 0x00d3, 0x00df, 0x00e3,
    0x00e5, 0x00e9, 0x00ef, 0x00f1, 0x00fb, 0x0101, 0x0107, 0x0117,
    0x010f, 0x0115, 0x0119, 0x011b, 0x0125, 0x0133, 0x0137, 0x0139,
    0x013d, 0x014b, 0x0151, 0x015b, 0x015d, 0x0161, 0x0167, 0x016f,
    0x0175, 0x017b, 0x017f, 0x0185, 0x018d, 0x0191, 0x0199, 0x01a3,
    0x01a5, 0x01af, 0x01b1, 0x01b7, 0x01bb, 0x01c1, 0x01c9, 0x01cd,
    0x01cf, 0x01d3, 0x01df, 0x01e7, 0x01eb, 0x01f3, 0x01f7, 0x01fd,
    0x0209, 0x020b, 0x021d, 0x0223, 0x022d, 0x0233, 0x0239, 0x023b,
    0x0241, 0x024b, 0x0251, 0x0257, 0x0259, 0x025f, 0x0265, 0x0269,
    0x026b, 0x0277, 0x0281, 0x0283, 0x0287, 0x028d, 0x0293, 0x0295,
    0x02a1, 0x02a5, 0x02ab, 0x02b3, 0x02bd, 0x02c5, 0x02cf, 0x02d7,
};

static const char CHARS[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";

static const uint32_t M1 = 0x7fffffabu;
static const uint32_t M2 = 0x7fffff07u;
static const uint32_t M3 = 0x7fffffaau;

static int32_t charval_raw(int c) {
    int32_t v = (c >= 'A' && c <= 'Z') ? (c - 'A') : (c - 0x16);
    return v & 0xffff;
}

static int32_t charval32(int c) {
    int32_t v = charval_raw(c);
    if (v < 0x20 || (v & 0x8000))
        v = (v & 0xff00) | ((v & 0xff) ^ 0x15);
    return v & 0xffff;
}

static int32_t raw32(int c) {
    return charval_raw(c);
}

static uint32_t sext32(uint16_t w) {
    return (uint32_t)(int16_t)w;
}

static uint32_t step(uint32_t *lo, uint32_t *ext, uint32_t mod) {
    int32_t los = (int32_t)*lo;
    int32_t exts = (int32_t)*ext;
    int32_t q1 = los / -53668;
    uint32_t r1 = (uint32_t)((int64_t)q1 * (int64_t)M1 + (int64_t)los * 40014);
    if ((int32_t)r1 < 0) r1 += M1;
    int32_t q2 = exts / -52774;
    uint32_t r2 = (uint32_t)((int64_t)q2 * (int64_t)M2 + (int64_t)exts * 40692);
    if ((int32_t)r2 < 0) r2 += M2;
    uint32_t diff = r1 - r2;
    if (diff == 0 || (int32_t)diff < 0) diff += M3;
    *lo = r1;
    *ext = r2;
    return diff % mod;
}

static uint32_t compute_V(const char *s) {
    uint32_t V = (uint32_t)raw32(s[0]);
    V = V * 36 + (uint32_t)charval32(s[1]);
    V = V * 36 + (uint32_t)charval32(s[4]);
    V = V * 36 + (uint32_t)charval32(s[7]);
    V = V * 36 + (uint32_t)charval32(s[3]);
    return V;
}

/* Restart-valid window for the game's blob extraction: the polynomial V is used
 * as a far pointer (selector:offset) and the decoded struct is copied to V+4.
 * On real Windows 3.1 (DOSBox) the app's writable data segments occupy selector
 * indices 12-14 in both the LDT and GDT (verified empirically: LDT 12-14 and
 * GDT 13-14 all work), so V must be in [0x640000, 0x780000). Indices <= 8 and
 * >= 15 (plus all higher selectors) are non-writable: the copy faults and
 * extraction silently fails (V=0 at restart = "usurper"). Confirmed in DOSBox:
 * Good 0x0067a9b4/0x006d914f/0x006f0a76/0x00773366, GDT band 0x0068-0x0073;
 * Usurper 0x0034xxxx-0x0047xxxx and 0x00799d1a+. The offset cap keeps the
 * (up to ~0x400 byte) copy inside the segment limit (off + 4 + 0x3FC <= 0xFFFF). */
#define V_MIN 0x00640000u
#define V_MAX 0x00780000u

static int restart_ok(const char *s) {
    uint32_t V = compute_V(s);
    return V >= V_MIN && V < V_MAX && (V & 0xFFFF) < 0xFC00;
}

static void init_state(uint32_t V, uint32_t *lo, uint32_t *ext) {
    uint32_t i1 = ((V & 0xff) ^ 0x35) & 0x7f;
    uint32_t i2 = ((((V >> 7) & 0xff) ^ 0xdc) & 0x7f);
    if (i2 == i1) i2 = (i2 + 1) & 0x7f;
    *lo = sext32(TABLE[i1]);
    *ext = sext32(TABLE[i2]);
}

static uint32_t run_check(uint32_t V, uint32_t *lo, uint32_t *ext) {
    init_state(V, lo, ext);
    uint32_t result = 0;
    for (int it = 0; it < 3; it++) {
        uint32_t N = (V >> (14 + 4 * it)) & 0xf;
        for (uint32_t k = 0; k < N + 1; k++) step(lo, ext, 256);
        result = result * 256 + step(lo, ext, 256);
    }
    return result;
}

static int check(const char *s) {
    char t[9];
    for (int i = 0; i < 8; i++) t[i] = toupper((unsigned char)s[i]);
    t[8] = 0;
    uint32_t V = compute_V(t);
    uint32_t lo, ext, result = run_check(V, &lo, &ext);
    if (result % 36 != (uint32_t)charval32(t[2])) return 0;
    result /= 36;
    if (result % 36 != (uint32_t)charval32(t[5])) return 0;
    result /= 36;
    if (result % 36 != (uint32_t)charval32(t[6])) return 0;
    return 1;
}

static int invert_char(uint32_t t) {
    for (int i = 0; i < 36; i++)
        if ((uint32_t)charval32(CHARS[i]) == t) return CHARS[i];
    return 0;
}

static void generate(char *out) {
    for (;;) {
        int c0 = CHARS[rand() % 36];
        int c1 = CHARS[rand() % 36];
        int c4 = CHARS[rand() % 36];
        int c7 = CHARS[rand() % 36];
        int c3 = CHARS[rand() % 36];
        char v[9];
        v[0] = (char)c0; v[1] = (char)c1; v[3] = (char)c3;
        v[4] = (char)c4; v[7] = (char)c7;
        uint32_t V = compute_V(v);
        uint32_t lo, ext, result = run_check(V, &lo, &ext);
        int c2 = invert_char(result % 36);
        int c5 = invert_char((result / 36) % 36);
        int c6 = invert_char((result / 1296) % 36);
        if (c2 && c5 && c6) {
            out[0] = (char)c0; out[1] = (char)c1; out[2] = (char)c2;
            out[3] = (char)c3; out[4] = (char)c4; out[5] = (char)c5;
            out[6] = (char)c6; out[7] = (char)c7; out[8] = 0;
            if (restart_ok(out))
                return;
        }
    }
}

int main(int argc, char **argv) {
    int n = argc>1 ? atoi(argv[1]) : 1;
    for (int i = 0; i < n; i++) {
        char buf[9];
        generate(buf);
        puts(buf);
    }
    return 0;
}
