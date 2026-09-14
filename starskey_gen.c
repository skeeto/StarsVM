// $ clang --target=wasm32 -Oz -s -nostdlib -Wl,--no-entry starskey_gen.c
#include <stdint.h>

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

static int32_t charval_raw(int c)
{
    int32_t v = (c >= 'A' && c <= 'Z') ? (c - 'A') : (c - 0x16);
    return v & 0xffff;
}

static int32_t charval32(int c)
{
    int32_t v = charval_raw(c);
    if (v < 0x20 || (v & 0x8000))
        v = (v & 0xff00) | ((v & 0xff) ^ 0x15);
    return v & 0xffff;
}

static int32_t raw32(int c)
{
    return charval_raw(c);
}

static uint32_t sext32(uint16_t w)
{
    return (uint32_t)(int16_t)w;
}

static uint32_t step(uint32_t *lo, uint32_t *ext, uint32_t mod)
{
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

static uint32_t compute_V(const char *s)
{
    uint32_t V = (uint32_t)raw32(s[0]);
    V = V * 36 + (uint32_t)charval32(s[1]);
    V = V * 36 + (uint32_t)charval32(s[4]);
    V = V * 36 + (uint32_t)charval32(s[7]);
    V = V * 36 + (uint32_t)charval32(s[3]);
    return V;
}

// Serial acceptance, as the game actually tests it (seg15:0x2CEE): not
// blacklisted, top digit in {2,4,6,18,22} (so the first character is one of
// C/E/G/S/W), and the low four base-36 digits in [0x64,0x16E360].
static const uint32_t BLACKLIST[] = {
    0x01CE7893u, 0x01D1FBFDu, 0x01D9D3FDu, 0x0099F50Fu, 0x009BBE28u,
    0x00E770B0u, 0x00C1E874u, 0x00C2C5A2u, 0x00D8A96Fu, 0x0035458Fu,
    0x0034CD48u, 0x0034CED4u, 0x00684DD4u, 0x007599BFu, 0x0070C0DBu,
    0x00799D1Au, 0x00479DD2u, 0x0235F254u, 0x02342951u, 0x02343D2Cu,
    0x02340C98u, 0x02447E57u, 0x024240D9u,
};

static int first_char_ok(uint32_t top)
{
    return top == 2 || top == 4 || top == 6 || top == 18 || top == 22;
}

static int blacklisted(uint32_t V)
{
    for (int i = 0; i < (int)(sizeof(BLACKLIST) / sizeof(*BLACKLIST)); i++)
        if (BLACKLIST[i] == V) return 1;
    return 0;
}

static int serial_ok(const char *s)
{
    uint32_t V = compute_V(s);
    uint32_t top = V / 1679616u;        // 36^4
    uint32_t rem = V % 1679616u;
    if (blacklisted(V)) return 0;
    if (!first_char_ok(top)) return 0;
    if (rem < 0x64u || rem > 0x16E360u) return 0;
    return 1;
}

static void init_state(uint32_t V, uint32_t *lo, uint32_t *ext)
{
    uint32_t i1 = ((V & 0xff) ^ 0x35) & 0x7f;
    uint32_t i2 = ((((V >> 7) & 0xff) ^ 0xdc) & 0x7f);
    if (i2 == i1) i2 = (i2 + 1) & 0x7f;
    *lo = sext32(TABLE[i1]);
    *ext = sext32(TABLE[i2]);
}

static uint32_t run_check(uint32_t V, uint32_t *lo, uint32_t *ext)
{
    init_state(V, lo, ext);
    uint32_t result = 0;
    for (int it = 0; it < 3; it++) {
        uint32_t N = (V >> (14 + 4 * it)) & 0xf;
        for (uint32_t k = 0; k < N + 1; k++) step(lo, ext, 256);
        result = result * 256 + step(lo, ext, 256);
    }
    return result;
}

static int invert_char(uint32_t t)
{
    for (int i = 0; i < 36; i++)
        if ((uint32_t)charval32(CHARS[i]) == t) return CHARS[i];
    return 0;
}

static uint32_t rand32(uint64_t *s)
{
    *s = *s*0x3243f6a8885a308d + 1;
    return *s >> 32;
}

static void generate(uint64_t *seed, char *out)
{
    for (;;) {
        int c0 = CHARS[rand32(seed) % 36];
        int c1 = CHARS[rand32(seed) % 36];
        int c4 = CHARS[rand32(seed) % 36];
        int c7 = CHARS[rand32(seed) % 36];
        int c3 = CHARS[rand32(seed) % 36];
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
            if (serial_ok(out))
                return;
        }
    }
}


#ifdef __wasm__
[[clang::export_name("generate")]]
uint64_t wasm_generate(uint64_t seed) {
    char out[9];
    generate(&seed, out);
    uint64_t r = 0;
    for (int i = 0; i < 8; i++) r = (r << 8) | (uint8_t)out[i];
    return r;
}

// Takes the 8-char serial packed the same way wasm_generate returns it
// (first char in the high byte) and classifies it:
//   0 = INVALID  (fails the run_check checksum)
//   1 = VALID    (passes the run_check checksum and serial_ok)
//   2 = USURPER  (passes the run_check checksum but fails serial_ok)
[[clang::export_name("check_serial")]]
int32_t wasm_check_serial(uint64_t packed) {
    char s[9];
    for (int i = 7; i >= 0; i--) s[i] = (char)((packed >> (8 * (7 - i))) & 0xff);
    s[8] = 0;
    uint32_t V = compute_V(s);
    uint32_t lo, ext, result = run_check(V, &lo, &ext);
    int ok = (result % 36 == (uint32_t)charval32(s[2]))
         && ((result / 36) % 36 == (uint32_t)charval32(s[5]))
         && ((result / 1296) % 36 == (uint32_t)charval32(s[6]));
    if (!ok) return 0;
    if (serial_ok(s)) return 1;
    return 2;
}


#else
#include <assert.h>
#include <ctype.h>
#include <stdbool.h>

#define lenof(a)    (int)(sizeof(a) / sizeof(*(a)))

// A serial is good when its checksum digits (positions 2, 5, 6) match what
// run_check derives from V, and V passes serial_ok.
static bool check(const char *v)
{
    char t[9];
    for (int i = 0; i < 8; i++) t[i] = (char)toupper((unsigned char)v[i]);
    t[8] = 0;
    uint32_t V = compute_V(t);
    uint32_t lo, ext, result = run_check(V, &lo, &ext);
    if (result % 36 != (uint32_t)charval32(t[2])) return false;
    result /= 36;
    if (result % 36 != (uint32_t)charval32(t[5])) return false;
    result /= 36;
    if (result % 36 != (uint32_t)charval32(t[6])) return false;
    return serial_ok(t);
}

int main()
{
    static char good[][8] = {
        "E4TP99BW", "E2BO659A", "ECN101WP", "EUTBD0AV",
    };
    for (int i = 0; i < lenof(good); i++) assert(check(good[i]));
    static char bad[][8] = {
        "CJWLADUE", "CXG6TIQX", "CXLSIXTL", "CXR6TCXY", "EPA8IRTP",
        "GVZC2FZ5", "GX5RODYA", "HAIDAAWS", "HB5JYSQH", "IFNKZG2R",
        "JU9VWLDF", "SEOMBS8E", "STDAG49L", "SU0ODX1T", "WBXU2GIX",
        "WCES1YRZ", "WV1EFJKF", "WV53517J", "WX8Z8HLP",
    };
    for (int i = 0; i < lenof(bad); i++) assert(!check(bad[i]));
}
#endif
