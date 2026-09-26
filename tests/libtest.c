/* libtest.c - the library's tests.  See src/stars.h.
 *
 *   libtest STARS.EXE [BENCH]
 *
 * Run from the top of the tree.  Everything is checked against what the
 * emulator wrote from the same input under --fixed-clock, byte for byte:
 *
 *   tests/newgame/  a two-player game made from libtest.def and two race
 *                   files, then each player's dump and two generated turns.
 *                   The .def names its races and its universe by absolute
 *                   paths into directories that exist nowhere, as a real
 *                   host's would.
 *   BENCH           the benchmark game, when there is one (see
 *                   tools/bench.ps1): 1 and 10 generated turns against
 *                   golden1 and golden10.  Its sixteen players are all AI,
 *                   so every turn file is locked with the AI password,
 *                   "viewai", and each is dumped against BENCH/viewai/pN:
 *                   what the emulator dumps given that password,
 *
 *                       StarsVM --fixed-clock -- -p viewai -dfmp Game.mN
 *
 *                   run on orig's Game.xy, Game.mN and Game.hN.
 *
 * Every operation runs twice in the one process, since a second run over the
 * same input has to come out the same.  Then the ways a call can fail: a bad
 * exe, bad arguments, damaged files, every size of arena too small to work,
 * and a second call while one is running.
 *
 * The library does no I/O, so all of the file reading is here.
 */
#include "../src/stars.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

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

static StarsBuf slurpf(const char *fmt, const char *dir, const char *base, int n)
{
    char path[1024];
    snprintf(path, sizeof path, fmt, dir, base, n);
    return slurp(path);
}

static StarsFS load_game(const char *dir, const char *base)
{
    StarsFS fs;
    int i;
    memset(&fs, 0, sizeof fs);
    fs.hst = slurpf("%s/%s.hst", dir, base, 0);
    fs.xy  = slurpf("%s/%s.xy", dir, base, 0);
    for (i = 0; i < 16; i++) {
        fs.m[i] = slurpf("%s/%s.m%d", dir, base, i + 1);
        fs.x[i] = slurpf("%s/%s.x%d", dir, base, i + 1);
        fs.h[i] = slurpf("%s/%s.h%d", dir, base, i + 1);
    }
    return fs;
}

static void check(const char *what, int ok)
{
    printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
}

static int same(StarsBuf a, StarsBuf b)
{
    return a.len == b.len && (!a.len || !memcmp(a.data, b.data, (size_t)a.len));
}

/* The files that differ, by name, or "" when none do. */
static const char *game_diff(const StarsFS *a, const StarsFS *b)
{
    static char out[256];
    size_t n = 0;
    int i;
    out[0] = 0;
    if (!same(a->hst, b->hst)) n += (size_t)snprintf(out + n, sizeof out - n, " hst");
    if (!same(a->xy, b->xy))   n += (size_t)snprintf(out + n, sizeof out - n, " xy");
    for (i = 0; i < 16 && n < sizeof out - 8; i++) {
        if (!same(a->m[i], b->m[i])) n += (size_t)snprintf(out + n, sizeof out - n, " m%d", i + 1);
        if (!same(a->x[i], b->x[i])) n += (size_t)snprintf(out + n, sizeof out - n, " x%d", i + 1);
        if (!same(a->h[i], b->h[i])) n += (size_t)snprintf(out + n, sizeof out - n, " h%d", i + 1);
    }
    return out;
}

static int game_files(const StarsFS *fs)
{
    int i, n = (fs->hst.len > 0) + (fs->xy.len > 0);
    for (i = 0; i < 16; i++)
        n += (fs->m[i].len > 0) + (fs->x[i].len > 0) + (fs->h[i].len > 0);
    return n;
}

static void show_log(StarsBuf log)
{
    if (log.len) printf("---- log\n%.*s----\n", (int)log.len, (char *)log.data);
}

/* An operation's result as a line: what, how it went, and the log if the
   answer was not the one wanted. */
static int expect(const char *what, int rc, int want, StarsBuf log)
{
    char line[256];
    snprintf(line, sizeof line, "%s: %s", what, stars_strerror(rc));
    check(line, rc == want);
    if (rc != want) show_log(log);
    return rc == want;
}

#define ARENA (128 << 20)

static StarsVM   *vm;
static StarsArena work;
static char      *mark;

/* Put the working arena back as it was, so each operation starts equal. */
static StarsArena fresh(void)
{
    work.beg = mark;
    return work;
}

static void test_game(StarsFS got, const StarsFS *want, const char *what)
{
    char line[256];
    const char *d = game_diff(&got, want);
    snprintf(line, sizeof line, "%s: %d files identical%s%s", what,
             game_files(want), *d ? ", differing:" : "", d);
    check(line, !*d);
}

static void test_newgame(void)
{
    StarsBuf def = slurp("tests/newgame/libtest.def"), log;
    StarsFile races[2];
    StarsFS want = load_game("tests/newgame/expect", "libtest"), got;
    int round;

    races[0].name = "one.r1";
    races[0].buf = slurp("tests/newgame/one.r1");
    races[1].name = "two.r1";
    races[1].buf = slurp("tests/newgame/two.r1");
    for (round = 1; round <= 2; round++) {
        StarsArena a = fresh();
        if (!expect("newgame", stars_newgame(vm, &a, def, races, 2, &got, &log),
                    STARS_OK, log)) return;
        test_game(got, &want, round == 1 ? "newgame" : "newgame again");
    }
}

static void test_dump(int player)
{
    StarsFS fs = load_game("tests/newgame/expect", "libtest");
    char dir[64], what[64], path[128];
    StarsDump d;
    StarsBuf log;
    int round;

    snprintf(dir, sizeof dir, "tests/newgame/expect/p%d", player);
    for (round = 1; round <= 2; round++) {
        StarsArena a = fresh();
        snprintf(what, sizeof what, "dump player %d%s", player, round == 1 ? "" : " again");
        if (!expect(what, stars_dump(vm, &a, &fs, player, &d, &log), STARS_OK, log))
            return;
        snprintf(path, sizeof path, "%s/libtest.map", dir);
        check("  map identical", same(d.map, slurp(path)));
        snprintf(path, sizeof path, "%s/libtest.pla", dir);
        check("  pla identical", same(d.pla, slurp(path)));
        snprintf(path, sizeof path, "%s/libtest.fle", dir);
        check("  fle identical", same(d.fle, slurp(path)));
    }
}

static void test_generate(const char *dir, const char *base, int n,
                          const char *wantdir)
{
    StarsFS want = load_game(wantdir, base);
    char what[64];
    StarsBuf log;
    int round;

    for (round = 1; round <= 2; round++) {
        StarsFS fs = load_game(dir, base);
        StarsArena a = fresh();
        snprintf(what, sizeof what, "generate %d%s", n, round == 1 ? "" : " again");
        if (!expect(what, stars_generate(vm, &a, n, &fs, &log), STARS_OK, log))
            return;
        test_game(fs, &want, what);
    }
}

/* A locked turn file dumps as if the password had been given, and the
   caller's copy of it is left locked. */
static void test_locked(const char *bench)
{
    char orig[512], ref[512], what[64];
    StarsFS fs;
    int p;

    snprintf(orig, sizeof orig, "%s/orig", bench);
    fs = load_game(orig, "Game");
    for (p = 1; p <= 16; p++) {
        StarsBuf want[3], before, log;
        static const char *ext[3] = { "map", "pla", "fle" };
        StarsArena a = fresh();
        StarsDump d;
        int i, ok = 1;

        for (i = 0; i < 3; i++) {
            snprintf(ref, sizeof ref, "%s/viewai/p%d/Game.%s", bench, p, ext[i]);
            want[i] = slurp(ref);
        }
        if (!want[0].len) {
            printf("skip  locked player %d: no %s/viewai/p%d\n", p, bench, p);
            continue;
        }
        before = slurpf("%s/%s.m%d", orig, "Game", p);
        snprintf(what, sizeof what, "dump of locked player %d", p);
        if (!expect(what, stars_dump(vm, &a, &fs, p, &d, &log), STARS_OK, log))
            continue;
        ok = same(d.map, want[0]) && same(d.pla, want[1]) && same(d.fle, want[2]);
        check("  as dumped with the password", ok);
        check("  and the caller's file still locked", same(fs.m[p - 1], before));
    }
}

/* ---- failures -------------------------------------------------------------- */

static void test_input(StarsBuf exe, StarsArena *perm)
{
    StarsFS fs = load_game("tests/newgame/expect", "libtest"), broken;
    StarsBuf log, trunc = exe;
    StarsVM *bad;
    StarsDump d;
    StarsArena a;

    trunc.len -= 1;
    check("an exe a byte short is EEXE", stars_init(&bad, perm, trunc) == STARS_EEXE);
    trunc.len = 100;
    check("100 bytes of exe is EEXE", stars_init(&bad, perm, trunc) == STARS_EEXE);

    a = fresh();
    check("0 turns is EINPUT", stars_generate(vm, &a, 0, &fs, NULL) == STARS_EINPUT);
    broken = fs;
    broken.hst.len = 0;
    check("no host file is EINPUT",
          stars_generate(vm, &a, 1, &broken, NULL) == STARS_EINPUT);
    check("player 17 is EINPUT", stars_dump(vm, &a, &fs, 17, &d, NULL) == STARS_EINPUT);
    check("player 3 of 2 is EINPUT", stars_dump(vm, &a, &fs, 3, &d, NULL) == STARS_EINPUT);
    check("the arena untouched by all that", a.beg == mark);

    /* Damaged files are the game's to report, with its own words. */
    broken = fs;
    broken.hst.len /= 2;
    a = fresh();
    expect("half a host file", stars_generate(vm, &a, 1, &broken, &log),
           STARS_EGAME, log);
    check("  and the game said why", log.len && strstr((char *)log.data, "corrupt"));
    broken = fs;
    broken.xy.len = 16;
    a = fresh();
    expect("16 bytes of universe", stars_generate(vm, &a, 1, &broken, &log),
           STARS_EGAME, log);
}

/* Every arena from 24 MB down to nothing, in 64 KB steps: each call either
   works, with the same output as ever, or runs out and leaves the arena as it
   found it.  Some of them run out partway through the game. */
static void test_arena(void)
{
    StarsFS want = load_game("tests/newgame/expect/turn2", "libtest");
    ptrdiff_t size;
    int ok = 0, enomem = 0, wrong = 0;

    for (size = 24 << 20; size >= 0; size -= 64 << 10) {
        StarsFS fs = load_game("tests/newgame/expect", "libtest");
        StarsArena a = { mark, mark + size };
        int rc = stars_generate(vm, &a, 2, &fs, NULL);
        if (rc == STARS_OK) {
            ok++;
            if (*game_diff(&fs, &want)) wrong++;
        } else if (rc == STARS_ENOMEM && a.beg == mark) {
            enomem++;
        } else {
            printf("  arena of %ld bytes: %s\n", (long)size, stars_strerror(rc));
            wrong++;
        }
    }
    printf("  %d arenas worked, %d were too small\n", ok, enomem);
    check("every arena works or is too small", ok && enomem && !wrong);
}

/* Two threads each generate at once; exactly one of them is told to wait. */
static volatile int go;
static int results[2];

static void *racer(void *arg)
{
    int i = (int)(intptr_t)arg;
    StarsFS fs = load_game("tests/newgame/expect", "libtest");
    StarsArena a = { mark + i * (48 << 20), mark + (i + 1) * (48 << 20) };
    while (!go) {}
    results[i] = stars_generate(vm, &a, 50, &fs, NULL);
    return NULL;
}

static void test_busy(void)
{
    pthread_t t[2];
    int i;
    go = 0;
    for (i = 0; i < 2; i++) pthread_create(&t[i], NULL, racer, (void *)(intptr_t)i);
    go = 1;
    for (i = 0; i < 2; i++) pthread_join(t[i], NULL);
    printf("  racing calls: %s and %s\n", stars_strerror(results[0]),
           stars_strerror(results[1]));
    check("a second call at once is EBUSY",
          (results[0] == STARS_EBUSY) != (results[1] == STARS_EBUSY) &&
          (results[0] == STARS_OK || results[1] == STARS_OK));
}

int main(int argc, char **argv)
{
    StarsArena perm;
    StarsBuf exe;
    char *mem;
    int rc;

    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: libtest STARS.EXE [BENCHDIR]\n");
        return 2;
    }
    mem = malloc(ARENA);
    perm.beg = mem;
    perm.end = mem + (8 << 20);
    work.beg = perm.end;
    work.end = mem + ARENA;

    exe = slurp(argv[1]);
    if (!exe.len) { fprintf(stderr, "libtest: cannot read %s\n", argv[1]); return 2; }
    rc = stars_init(&vm, &perm, exe);
    check("init", rc == STARS_OK);
    if (rc) return 1;
    mark = work.beg;

    test_newgame();
    test_dump(1);
    test_dump(2);
    test_generate("tests/newgame/expect", "libtest", 2, "tests/newgame/expect/turn2");
    if (argc == 3) {
        char orig[512], golden[512];
        snprintf(orig, sizeof orig, "%s/orig", argv[2]);
        snprintf(golden, sizeof golden, "%s/golden1", argv[2]);
        test_generate(orig, "Game", 1, golden);
        snprintf(golden, sizeof golden, "%s/golden10", argv[2]);
        test_generate(orig, "Game", 10, golden);
        test_locked(argv[2]);
    }
    test_input(exe, &perm);
    test_arena();
    test_busy();

    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures != 0;
}
