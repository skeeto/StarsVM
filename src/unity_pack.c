/* unity_pack.c - the packer as its own program.
 *
 * `make onefile` builds the emulator, builds this, and runs it to produce one
 * self-contained executable: the emulator with the game compressed and
 * appended.  It is a build-time tool, so none of its cost - a match finder
 * over the whole module, a dynamic-programming parse, several MB of scratch -
 * is anywhere near the thing people run.
 *
 * It shares only the format with the emulator, through pack.h, and it shares
 * the *decoder* with it through unpack.c, which is deliberate: the packer
 * decodes its own output and compares it to the input before writing anything,
 * so the two sides cannot drift apart without the build failing.
 */

#include "log.c"
#include "pack.c"
#include "unpack.c"

static const char usage[] =
    "\n"
    "Compresses a Win16 module and appends it to a copy of the emulator, so the\n"
    "result is one file that needs nothing installed beside it.\n"
    "\n"
    "  -d N    match search depth (default 256; higher is slower and smaller)\n"
    "  -r N    parse rounds (default 3; the parse is re-priced each round)\n"
    "  -q      only complain\n"
    "\n"
    "With no emulator given, writes just the compressed payload, which is how\n"
    "to measure the codec without linking anything.\n";

int main(int argc, char **argv)
{
    const char *prog = "StarsVM-pack";
    const char *emu = NULL, *mod = NULL, *out = NULL;
    uint32_t depth = 256;
    int rounds = 3, verbose = 1, i, npos = 0;
    uint8_t *raw, *emub = NULL, *payload;
    uint32_t rawlen, emulen = 0, paylen;
    FILE *f;

    if (argv[0] && argv[0][0]) {
        const char *p;
        prog = argv[0];
        for (p = argv[0]; *p; p++)
            if (*p == '\\' || *p == '/' || *p == ':') prog = p + 1;
    }

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-d") && i + 1 < argc) depth = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(a, "-r") && i + 1 < argc) rounds = atoi(argv[++i]);
        else if (!strcmp(a, "-q")) verbose = 0;
        else if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            printf("usage: %s [-d N] [-r N] [-q] <module> <output> [emulator]\n",
                   prog);
            fputs(usage, stdout);
            return 0;
        } else if (a[0] == '-' && a[1]) {
            fprintf(stderr, "%s: unknown option %s\n", prog, a);
            return 2;
        } else {
            if (npos == 0) mod = a;
            else if (npos == 1) out = a;
            else if (npos == 2) emu = a;
            npos++;
        }
    }
    if (npos < 2) {
        fprintf(stderr, "usage: %s [-d N] [-r N] [-q] <module> <output> [emulator]\n",
                prog);
        return 2;
    }

    log_console = 1;
    log_open(NULL);                       /* unpack.c reports through log_msg */

    raw = slurp(mod, &rawlen);
    if (!raw) return 1;
    if (!rawlen) { fprintf(stderr, "%s: %s is empty\n", prog, mod); return 1; }
    if (emu) {
        emub = slurp(emu, &emulen);
        if (!emub) return 1;
        /* The other way round from the supported order, and silently wrong:
           an Authenticode signature covers everything up to the certificate
           table, so a payload appended after one falls outside it, and Windows
           rejects a file that has anything beyond the table at all. */
        if (cert_offset(emub, emulen))
            fprintf(stderr, "%s: %s is already signed, and appending to it "
                    "invalidates that signature - sign %s instead, afterwards"
                    "\n", prog, emu, out);
    }

    if (verbose) printf("%s: %s, %u bytes\n", prog, mod, rawlen);
    payload = pack_compress(raw, rawlen, depth, rounds, &paylen, verbose);
    if (!payload) return 1;

    f = fopen(out, "wb");
    if (!f) { fprintf(stderr, "%s: cannot write %s\n", prog, out); return 1; }
    if (emulen && fwrite(emub, 1, emulen, f) != emulen) goto wfail;
    if (fwrite(payload, 1, paylen, f) != paylen) goto wfail;
    if (fclose(f)) goto wfail2;

    if (verbose) {
        printf("  payload       %9u bytes  (%.1f%% of the module)\n",
               paylen, 100.0 * paylen / rawlen);
        printf("  %s      %9u bytes\n", out, emulen + paylen);
        if (emulen)
            printf("  was          %9u bytes uncompressed, now %.1f%%\n",
                   emulen + rawlen,
                   100.0 * (emulen + paylen) / (emulen + rawlen));
    }
    return 0;

wfail:
    fclose(f);
wfail2:
    fprintf(stderr, "%s: write failed on %s\n", prog, out);
    return 1;
}
