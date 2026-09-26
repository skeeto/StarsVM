/* mkicon.c - extract an icon from the game's NE and write it as a Windows .ico.
 *
 * An RT_GROUP_ICON directory is almost the ICO file format already: same
 * header, same 14-byte entries, except that the last field of a resource entry
 * is a WORD naming the RT_ICON resource that holds the image, where an ICO file
 * wants a DWORD byte offset to the image inside the file.  So the conversion
 * is: widen that field, lay the images out after the directory, and fill in the
 * offsets.
 *
 *   mkicon <stars.exe> <out.ico> <out.rc> [GROUPNAME]
 *
 * The .rc output is written either way: with an ICON directive when the icon
 * was produced, and with a comment when the game could not be read, so that a
 * build without the game present still compiles.
 *
 * This runs on the build host, not the target, so it is plain ISO C with no
 * Windows headers and is built with $(HOSTCC) rather than $(CC).
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char    *exe;             /* for messages */
static unsigned char *img;
static size_t         imglen;

static void die(const char *msg)
{
    fprintf(stderr, "mkicon: %s\n", msg);
    exit(1);
}

/* Every read is bounds-checked: a truncated or foreign file fails with a
   message rather than a crash. */
static unsigned u8(size_t o)
{
    if (o >= imglen) {
        fprintf(stderr, "mkicon: %s is truncated or not an NE file\n", exe);
        exit(1);
    }
    return img[o];
}
static unsigned u16(size_t o) { return u8(o) | u8(o + 1) << 8; }
static unsigned long u32(size_t o)
{
    return (unsigned long)u16(o) | (unsigned long)u16(o + 2) << 16;
}

static void put16(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
}
static void put32(unsigned char *p, unsigned long v)
{
    put16(p, (unsigned)(v & 0xFFFF));
    put16(p + 2, (unsigned)(v >> 16));
}

/* One entry of the NE resource table. */
struct res { unsigned type, id; size_t off, len; };

/* Walk the NE resource table into a flat list.  Where a type appears twice
   the later run wins, as a lookup by type would find it. */
static struct res *resources(size_t *n, size_t *base)
{
    size_t ne = u32(0x3C), p, cap = 64, count = 0;
    struct res *r = malloc(cap * sizeof *r);
    unsigned shift;

    if (!r) die("out of memory");
    *base = ne + u16(ne + 0x24);
    shift = u16(*base);
    p = *base + 2;
    for (;;) {
        unsigned tid = u16(p), k, cnt;
        if (!tid) break;
        cnt = u16(p + 2);
        for (k = 0; k < cnt; k++) {
            size_t e = p + 8 + (size_t)k * 12;
            if (count == cap) {
                cap *= 2;
                r = realloc(r, cap * sizeof *r);
                if (!r) die("out of memory");
            }
            r[count].type = tid;
            r[count].id   = u16(e + 6);
            r[count].off  = (size_t)u16(e) << shift;
            r[count].len  = (size_t)u16(e + 2) << shift;
            count++;
        }
        p += 8 + (size_t)cnt * 12;
    }
    *n = count;
    return r;
}

/* A resource's name, as the game spells it: its string when it has one,
   "#N" when it is numbered. */
static void label_of(size_t base, unsigned id, char *out, size_t size)
{
    if (id & 0x8000) {
        snprintf(out, size, "#%u", id & 0x7FFF);
    } else {
        size_t i, n = u8(base + id);
        for (i = 0; i < n && i + 1 < size; i++)
            out[i] = (char)u8(base + id + 1 + i);
        out[i] = 0;
    }
}

static int same(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        int x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x += 'a' - 'A';
        if (y >= 'A' && y <= 'Z') y += 'a' - 'A';
        if (x != y) return 0;
    }
    return *a == *b;
}

/* The last type run of a kind, as a lookup by type would find it. */
static size_t run_start(const struct res *r, size_t n, unsigned type,
                        size_t *end)
{
    size_t i, start = n;
    *end = n;
    for (i = 0; i < n; i++) {
        if (r[i].type != type) continue;
        if (i == 0 || r[i - 1].type != type) start = i;
        *end = i + 1;
    }
    if (start == n) *end = n;
    return start;
}

int main(int argc, char **argv)
{
    const char *ico, *rc, *group;
    struct res *r, *grp = NULL;
    size_t nres, base, gs, ge, is, ie, i, count, offset, total, at;
    char label[256], path[1024];
    unsigned char *out;
    FILE *f;

    if (argc < 4) {
        fputs("usage: mkicon <stars.exe> <out.ico> <out.rc> [GROUPNAME]\n",
              stderr);
        return 1;
    }
    exe = argv[1]; ico = argv[2]; rc = argv[3];
    group = argc > 4 ? argv[4] : NULL;

    /* No game is not an error: the build goes on without an icon. */
    f = fopen(exe, "rb");
    if (!f) {
        const char *why = strerror(errno);
        FILE *o = fopen(rc, "wb");
        if (!o) die("cannot write the .rc");
        fprintf(o, "/* no icon: %s: %s */\n", exe, why);
        fclose(o);
        printf("mkicon: %s: %s; building without an icon\n", exe, why);
        return 0;
    }
    fseek(f, 0, SEEK_END);
    imglen = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    img = malloc(imglen ? imglen : 1);
    if (!img) die("out of memory");
    if (fread(img, 1, imglen, f) != imglen) {
        fprintf(stderr, "mkicon: cannot read %s\n", exe);
        return 1;
    }
    fclose(f);

    r = resources(&nres, &base);

    gs = run_start(r, nres, 0x800E, &ge);           /* RT_GROUP_ICON */
    for (i = gs; i < ge; i++) {
        label_of(base, r[i].id, label, sizeof label);
        if (!group || same(label, group)) { grp = &r[i]; break; }
    }
    if (!grp) {
        fprintf(stderr, "mkicon: no RT_GROUP_ICON named '%s'\n",
                group ? group : "");
        return 1;
    }
    is = run_start(r, nres, 0x8003, &ie);           /* RT_ICON */

    /* ICONDIR, then one 16-byte ICONDIRENTRY per image, then the images. */
    count  = u16(grp->off + 4);
    offset = 6 + count * 16;
    total  = offset;
    {
        /* First pass: find each image and its size, so the output can be
           allocated once. */
        size_t *ioff = malloc((count ? count : 1) * sizeof *ioff);
        size_t *ilen = malloc((count ? count : 1) * sizeof *ilen);
        if (!ioff || !ilen) die("out of memory");

        for (i = 0; i < count; i++) {
            size_t e = grp->off + 6 + i * 14, j, found = 0;
            unsigned long size = u32(e + 8);
            unsigned rid = u16(e + 12);
            for (j = is; j < ie; j++) {
                if ((r[j].id & 0x7FFF) != rid) continue;
                ioff[i] = r[j].off;
                ilen[i] = size < r[j].len ? (size_t)size : r[j].len;
                /* Clamp to the file, as a slice past its end would. */
                if (ioff[i] > imglen) ioff[i] = imglen;
                if (ilen[i] > imglen - ioff[i]) ilen[i] = imglen - ioff[i];
                found = 1;
            }
            if (!found) {
                fprintf(stderr, "mkicon: group names RT_ICON %u, which is "
                        "absent\n", rid);
                return 1;
            }
            total += ilen[i];
        }

        out = calloc(total, 1);
        if (!out) die("out of memory");
        put16(out + 0, 0);
        put16(out + 2, 1);
        put16(out + 4, (unsigned)count);
        at = offset;
        for (i = 0; i < count; i++) {
            size_t e = grp->off + 6 + i * 14;
            unsigned char *d = out + 6 + i * 16;
            d[0] = (unsigned char)u8(e);                /* width */
            d[1] = (unsigned char)u8(e + 1);            /* height */
            d[2] = (unsigned char)u8(e + 2);            /* colours */
            d[3] = (unsigned char)u8(e + 3);            /* reserved */
            put16(d + 4, u16(e + 4));                   /* planes */
            put16(d + 6, u16(e + 6));                   /* bits */
            put32(d + 8, (unsigned long)ilen[i]);
            put32(d + 12, (unsigned long)at);
            memcpy(out + at, img + ioff[i], ilen[i]);
            at += ilen[i];
        }
        free(ioff);
        free(ilen);
    }

    f = fopen(ico, "wb");
    if (!f || fwrite(out, 1, total, f) != total || fclose(f))
        die("cannot write the .ico");

    /* windres wants forward slashes, or doubled backslashes. */
    snprintf(path, sizeof path, "%s", ico);
    for (i = 0; path[i]; i++) if (path[i] == '\\') path[i] = '/';
    f = fopen(rc, "wb");
    if (!f) die("cannot write the .rc");
    fprintf(f, "1 ICON \"%s\"\n", path);
    if (fclose(f)) die("cannot write the .rc");

    label_of(base, grp->id, label, sizeof label);
    printf("mkicon: %s -> %s (%lu bytes)\n", label, ico, (unsigned long)total);
    return 0;
}
