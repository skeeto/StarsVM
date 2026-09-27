/* libbench.c - how long the library takes to generate turns.
 *
 * The emulator's timings (tools/bench.ps1, tools/ab.ps1) need Windows; this
 * needs only the library, so it is the one to run on any other machine -
 * an ARM board, say, where the x87 is carried out in integers.  It loads the
 * benchmark game (see tools/bench.ps1 for bench/'s layout), generates TURNS
 * turns ROUNDS times from the same files, checks every run's output against
 * bench/goldenTURNS when that is there, and prints each run's time and the
 * median.  The first run is a warm-up and is not counted.
 *
 * Usage: libbench STARS.EXE BENCHDIR [TURNS [ROUNDS]]
 */
#ifndef _WIN32
#  define _POSIX_C_SOURCE 199309L      /* clock_gettime, under -std=c11 */
#endif
#include "../src/stars.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#  include <windows.h>
#else
#  include <time.h>
#endif

static StarsBuf slurp(const char *path)
{
    StarsBuf b = {0, 0};
    FILE *f = fopen(path, "rb");
    long n;
    if (!f) return b;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    b.data = malloc(n > 0 ? (size_t)n : 1);
    if (b.data && fread(b.data, 1, (size_t)n, f) == (size_t)n) b.len = n;
    fclose(f);
    return b;
}

static StarsFS load_game(const char *dir)
{
    char path[1024];
    StarsFS fs;
    int i;

    memset(&fs, 0, sizeof fs);
    snprintf(path, sizeof path, "%s/Game.hst", dir);
    fs.hst = slurp(path);
    snprintf(path, sizeof path, "%s/Game.xy", dir);
    fs.xy = slurp(path);
    for (i = 0; i < 16; i++) {
        snprintf(path, sizeof path, "%s/Game.m%d", dir, i + 1);
        fs.m[i] = slurp(path);
        snprintf(path, sizeof path, "%s/Game.x%d", dir, i + 1);
        fs.x[i] = slurp(path);
        snprintf(path, sizeof path, "%s/Game.h%d", dir, i + 1);
        fs.h[i] = slurp(path);
    }
    return fs;
}

static int same(StarsBuf a, StarsBuf b)
{
    return a.len == b.len && (!a.len || !memcmp(a.data, b.data, (size_t)a.len));
}

static int same_game(const StarsFS *a, const StarsFS *b)
{
    int i, ok = same(a->hst, b->hst) && same(a->xy, b->xy);
    for (i = 0; i < 16; i++)
        ok = ok && same(a->m[i], b->m[i]) && same(a->x[i], b->x[i]) &&
             same(a->h[i], b->h[i]);
    return ok;
}

/* Seconds on a monotonic clock. */
static double now(void)
{
#ifdef _WIN32
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    return (double)t.QuadPart / (double)f.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
#endif
}

static int cmp(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

#define ARENA (256 << 20)

int main(int argc, char **argv)
{
    char dir[1024];
    StarsFS orig, golden;
    StarsArena perm, work;
    StarsBuf exe;
    StarsVM *vm;
    double t[64];
    char *mem, *mark;
    int turns, rounds, i, rc, check;

    if (argc < 3 || argc > 5) {
        fprintf(stderr, "usage: libbench STARS.EXE BENCHDIR [TURNS [ROUNDS]]\n");
        return 2;
    }
    turns = argc > 3 ? atoi(argv[3]) : 10;
    rounds = argc > 4 ? atoi(argv[4]) : 5;
    if (turns < 1 || rounds < 1 || rounds > 63) {
        fprintf(stderr, "libbench: TURNS at least 1, ROUNDS 1 to 63\n");
        return 2;
    }

    exe = slurp(argv[1]);
    if (!exe.len) { fprintf(stderr, "libbench: cannot read %s\n", argv[1]); return 2; }
    snprintf(dir, sizeof dir, "%s/orig", argv[2]);
    orig = load_game(dir);
    if (!orig.hst.len) { fprintf(stderr, "libbench: no %s/Game.hst\n", dir); return 2; }
    snprintf(dir, sizeof dir, "%s/golden%d", argv[2], turns);
    golden = load_game(dir);
    check = golden.hst.len > 0;

    mem = malloc(ARENA);
    if (!mem) { fprintf(stderr, "libbench: out of memory\n"); return 2; }
    perm.beg = mem;
    perm.end = mem + (8 << 20);
    rc = stars_init(&vm, &perm, exe);
    if (rc) { fprintf(stderr, "libbench: init: %s\n", stars_strerror(rc)); return 1; }
    mark = perm.end;

    printf("%d turns, %d rounds after a warm-up%s\n", turns, rounds,
           check ? ", each checked against the golden output" : "");
    for (i = 0; i <= rounds; i++) {
        StarsFS fs = orig;
        double t0;
        work.beg = mark;
        work.end = mem + ARENA;
        t0 = now();
        rc = stars_generate(vm, &work, turns, &fs, NULL);
        t[i] = now() - t0;
        if (rc) { printf("round %d: %s\n", i, stars_strerror(rc)); return 1; }
        if (check && !same_game(&fs, &golden)) {
            printf("round %d: output differs from the golden\n", i);
            return 1;
        }
        if (i) printf("round %d: %.3f s\n", i, t[i]);
    }
    qsort(t + 1, (size_t)rounds, sizeof *t, cmp);
    printf("median: %.3f s\n", rounds % 2 ? t[1 + rounds / 2]
                                           : (t[rounds / 2] + t[1 + rounds / 2]) / 2);
    return 0;
}
