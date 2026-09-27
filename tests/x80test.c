/* x80test.c - x80.c against what an x87 does, on any host.
 *
 * The fuzzer's --x80 mode compares x80.c with the x87 case by case, but it
 * needs an x87 to do it.  This needs nothing but a C compiler: it draws the
 * same streams of cases (src/x80ops.h), runs them through x80.c alone, and
 * compares a hash of every block of outputs with the one the fuzzer recorded
 * from silicon in tests/x80vec.txt.  A match says x80.c computes on this
 * host - this compiler, this word size, this byte order's idea of integer
 * arithmetic - exactly what the x87 computed on those cases.
 *
 * By default it checks the first X80TEST_QUICK blocks of each operation, a
 * few seconds even under emulation; --heavy checks the rest of the recorded
 * blocks too.  A failing block is found case by case on an x86 with
 *
 *     StarsVM-fuzz --x80 OP --seed 0x5EED0F87 --rounds N
 *
 * N being the end of the block, which prints the first differing case.
 *
 * Usage: x80test [--heavy] [tests/x80vec.txt]
 */
#include "../src/x80.c"
#include "../src/x80ops.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The recorded hashes, per operation: one per quick block, then one over
   the heavy ones. */
static uint64_t want[X80_NOPS][X80TEST_QUICK + 1];
static unsigned have[X80_NOPS];

static int load(const char *path)
{
    char line[256], name[64], blocks[32];
    unsigned long long h;
    FILE *f = fopen(path, "r");
    unsigned k, b, lo, hi, n = 0;

    if (!f) {
        fprintf(stderr, "x80test: cannot read %s\n", path);
        return 0;
    }
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        if (sscanf(line, "%63s %31s %llx", name, blocks, &h) != 3) {
            fprintf(stderr, "x80test: %s: cannot read: %s", path, line);
            fclose(f);
            return 0;
        }
        for (k = 0; k < X80_NOPS && strcmp(x80ops[k].name, name); k++) {}
        if (k == X80_NOPS) {
            fprintf(stderr, "x80test: %s: no operation %s\n", path, name);
            fclose(f);
            return 0;
        }
        if (sscanf(blocks, "%u-%u", &lo, &hi) == 2) {
            if (lo != X80TEST_QUICK || hi != X80TEST_HEAVY - 1) {
                fprintf(stderr, "x80test: %s: %s %s is not blocks %u-%u\n",
                        path, name, blocks, X80TEST_QUICK, X80TEST_HEAVY - 1);
                fclose(f);
                return 0;
            }
            b = X80TEST_QUICK;
        } else {
            b = (unsigned)strtoul(blocks, NULL, 10);
            if (b >= X80TEST_QUICK) continue;
        }
        want[k][b] = h;
        have[k] |= 1u << b;
        n++;
    }
    fclose(f);
    return n > 0;
}

int main(int argc, char **argv)
{
    const char *path = "tests/x80vec.txt";
    int heavy = 0, bad = 0, i;
    unsigned k, b, c, nb;
    unsigned long long cases = 0;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--heavy")) heavy = 1;
        else if (argv[i][0] == '-') {
            fprintf(stderr, "usage: x80test [--heavy] [tests/x80vec.txt]\n");
            return 2;
        } else path = argv[i];
    }
    if (!load(path)) return 2;

    nb = heavy ? X80TEST_HEAVY : X80TEST_QUICK;
    for (k = 0; k < X80_NOPS; k++) {
        X80Rng g = x80ops_stream(X80TEST_SEED, k);
        uint64_t hv = X80OUT_HASH0;
        int first = -1;

        if (have[k] != (1u << (X80TEST_QUICK + 1)) - 1) {
            printf("FAIL  %-8s not all of its blocks are in %s\n", x80ops[k].name, path);
            bad = 1;
            continue;
        }
        for (b = 0; b < nb; b++) {
            uint64_t h = X80OUT_HASH0;
            for (c = 0; c < X80OPS_BLOCK; c++) {
                struct x80case cs;
                struct x80out o;
                x80case_gen(k, x80gen_u64(&g), &cs);
                X80OPS_RUN(x80_, k, &cs, &o);
                if (b < X80TEST_QUICK) h = x80out_hash(h, &o);
                else                   hv = x80out_hash(hv, &o);
            }
            if (b < X80TEST_QUICK && h != want[k][b] && first < 0) first = (int)b;
        }
        cases += (unsigned long long)nb * X80OPS_BLOCK;
        if (first >= 0) {
            printf("FAIL  %-8s block %d differs (cases %d to %d)\n", x80ops[k].name,
                   first, first * X80OPS_BLOCK, (first + 1) * X80OPS_BLOCK - 1);
            bad = 1;
        } else if (heavy && hv != want[k][X80TEST_QUICK]) {
            printf("FAIL  %-8s blocks %u-%u differ\n", x80ops[k].name,
                   X80TEST_QUICK, X80TEST_HEAVY - 1);
            bad = 1;
        } else {
            printf("ok    %-8s %u blocks\n", x80ops[k].name, nb);
        }
    }
    printf("%s: %llu cases, %s\n", bad ? "FAILED" : "passed", cases,
#ifdef X80_INT128
           "unsigned __int128"
#else
           "32-bit halves"
#endif
           );
    return bad;
}
