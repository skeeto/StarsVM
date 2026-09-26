/* libstars.c - the library's entry points: see stars.h.
 *
 * A call builds a whole Win16 machine, runs stars.exe on it with the batch
 * switches the operation needs, and takes the files it wrote.  Nothing
 * survives from one call to the next but the StarsVM, which is the parsed
 * module and never changes: the machine itself - selectors, heaps, the task,
 * files, windows - is rebuilt every time, because the emulator it is made of
 * keeps all of that in globals.  That is also why only one call can run at a
 * time.
 *
 * Every allocation comes from the caller's arena.  The game's 16 MB address
 * space is borrowed from the back of it for the length of the call; what the
 * call returns - the files, and the log - is taken from the front, and stays.
 * Anything that goes wrong partway, the arena running out included, unwinds
 * straight back to the entry point with longjmp: the machine it abandons is
 * about to be rebuilt from scratch, so there is nothing to tidy.
 */

#include "stars.h"
#include "lib.h"
#include "ne.h"
#include "sel.h"
#include "cpu.h"
#include "fpu.h"
#include "thunk.h"
#include "task.h"
#include "heap.h"
#include "dos.h"
#include "fs.h"
#include "res.h"
#include "hostclock.h"
#include "native.h"
#include "log.h"

#include <setjmp.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

void api_kernel_register(void);
void api_kernel_reset(void);
void api_common_register(void);
void api_dos_register(void);
void api_res_register(void);

/* The one stars.exe this was all written against: Stars! 2.70j.  Its import
   table is compiled in (imports.inc) and its hottest routines are replaced by
   address (native.c), so anything else is refused rather than half-run. */
#define EXE_LEN  3153152
#define EXE_FNV  0x1d3f5995a369037cull

/* Selector slots for the game, 64 KB each.  Turn generation on a 16-player
   game peaks at 112 of them, 50 turns in. */
#define GUEST_SLOTS 256

#define LOG_CAP (16 << 10)

/* Instructions are run in chunks this big, so a game spinning where no API
   call would notice still comes back to be counted. */
#define CHUNK 100000000u

struct StarsVM {
    NeModule mod;
};

typedef struct {
    char    *beg, *end;        /* what is left of the arena             */
    jmp_buf  jmp;
    int      err;
    int      fpu;              /* the host FPU is set up for the guest  */
    char    *log;
    ptrdiff_t loglen;
} Run;

/* The call in progress.  Static rather than on the caller's stack, since the
   busy flag already allows only one. */
static Run         the_run;
static Run        *run;        /* &the_run during a call, else NULL */
static atomic_flag busy = ATOMIC_FLAG_INIT;

/* ---- the arena ----------------------------------------------------------- */

void *lib_alloc(ptrdiff_t size)
{
    ptrdiff_t pad = (ptrdiff_t)(-(uintptr_t)run->beg & 15);
    char *p;
    if (size < 0 || size > run->end - run->beg - pad) lib_fail(STARS_ENOMEM);
    p = run->beg + pad;
    run->beg = p + size;
    return memset(p, 0, (size_t)size);
}

void *lib_grow(void *p, ptrdiff_t old, ptrdiff_t size)
{
    void *q;
    if ((char *)p + old == run->beg && size - old <= run->end - run->beg) {
        memset(run->beg, 0, (size_t)(size - old));
        run->beg += size - old;
        return p;
    }
    q = lib_alloc(size);
    memcpy(q, p, (size_t)old);
    return q;
}

void lib_fail(int err)
{
    run->err = err;
    longjmp(run->jmp, 1);
}

/* The loader's tables come from the arena too: the StarsVM's from the
   permanent one, at stars_init. */
void *ne_alloc(size_t n)
{
    return lib_alloc((ptrdiff_t)n);
}

/* ---- the log --------------------------------------------------------------- */

int log_verbose;
int log_console;

void log_msg(const char *fmt, ...)
{
    va_list ap;
    ptrdiff_t room;
    int n;

    if (!run || !run->log) return;
    room = LOG_CAP - run->loglen;
    if (room <= 1) return;
    va_start(ap, fmt);
    n = vsnprintf(run->log + run->loglen, (size_t)room, fmt, ap);
    va_end(ap);
    if (n > 0) run->loglen += n < room ? n : room - 1;
}

/* ---- a call ----------------------------------------------------------------- */

/* The start of every call but stars_init.  Nonzero is the error to return. */
static int enter(StarsArena *arena, StarsBuf *log)
{
    Run *r = &the_run;
    if (!arena || !arena->beg || arena->end < arena->beg) return STARS_EINPUT;
    if (atomic_flag_test_and_set(&busy)) return STARS_EBUSY;
    memset(r, 0, sizeof *r);
    r->beg = arena->beg;
    r->end = arena->end;
    run = r;
    if (log) {
        memset(log, 0, sizeof *log);
        if (r->end - r->beg >= LOG_CAP) {
            r->log = r->beg;
            r->beg += LOG_CAP;
        }
    }
    return 0;
}

/* The end of every call.  A call that succeeded leaves its output in front
   of the arena; one that failed leaves only its log, which is where it says
   why. */
static int leave(StarsArena *arena, StarsBuf *log, int rc)
{
    Run *r = &the_run;
    if (r->fpu) fpu_host_leave();
    fs_close_all();
    if (log && r->log) {
        log->data = (unsigned char *)r->log;
        log->len = r->loglen;
    }
    if (rc == STARS_OK) arena->beg = r->beg;
    else arena->beg = r->log ? r->log + r->loglen : arena->beg;
    run = NULL;
    atomic_flag_clear(&busy);
    return rc;
}

/* Build the machine, run stars.exe with `args` on it, and say how it went.
   The files are whatever the caller put in the directory beforehand. */
static int play(StarsVM *vm, const char *args, uint64_t budget)
{
    ptrdiff_t gsize = (ptrdiff_t)(GUEST_SLOTS + 1) * SEL_SLOT;
    char *top = (char *)((uintptr_t)run->end & ~(uintptr_t)15);
    NeModule m;
    int r;

    /* The game's address space is scratch, borrowed from the back. */
    if (top - run->beg < gsize) lib_fail(STARS_ENOMEM);
    run->end = top - gsize;
    sel_init_mem((uint8_t *)run->end, GUEST_SLOTS);

    heap_reset();
    dos_reset();
    api_res_reset();
    api_kernel_reset();
    hostclock_reset();
    headless_reset();
    cpu_stop_clear();
    if (!thunk_init() || !call16_init()) lib_fail(STARS_ECRASH);
    api_kernel_register();
    api_common_register();
    api_dos_register();
    api_res_register();
    headless_register();

    /* The module is loaded afresh into the new address space, which assigns
       its segments new selectors: a copy of the table takes them. */
    m = vm->mod;
    m.seg = lib_alloc((ptrdiff_t)(m.cseg ? m.cseg : 1) * (ptrdiff_t)sizeof *m.seg);
    memcpy(m.seg, vm->mod.seg, (size_t)m.cseg * sizeof *m.seg);
    if (!ne_load(&m, thunk_resolve, NULL)) lib_fail(STARS_ECRASH);
    {
        /* stars_init made sure this is the exe the routines were written
           for, so they always go in, and the line saying so is not news. */
        ptrdiff_t quiet = run->loglen;
        native_install(&m);
        run->loglen = quiet;
    }
    if (!task_start(&m, &cpu, args, 1, "C:\\STARS.EXE")) lib_fail(STARS_ECRASH);

    call16_budget = budget;
    fpu_host_enter();
    run->fpu = 1;
    for (;;) {
        r = cpu_run(&cpu, CHUNK);
        if (r != CPU_STEPS || cpu_stop_latched()) break;
        if (cpu.icount >= budget) break;
    }
    fpu_host_leave();
    run->fpu = 0;
    dos_shutdown();

    switch (r) {
    case CPU_HALT:
        return STARS_OK;
    case CPU_STEPS:
        log_msg("The game ran %llu instructions without finishing.\n",
                (unsigned long long)cpu.icount);
        return STARS_EHUNG;
    case CPU_NOAPI:
        log_msg("The game called %s, which the library does not provide.\n",
                thunk_last_missing() ? thunk_last_missing() : "an API");
        return STARS_ECRASH;
    default:
        log_msg("The emulation stopped: %s at %04X:%04X (op %02X %02X).\n",
                cpu_state_name(r), (unsigned)cpu.bad_cs, (unsigned)cpu.bad_ip,
                cpu.bad_op, cpu.bad_op2);
        /* The game has a habit of saying a file is corrupt and then falling
           over anyway, as it does under the emulator too.  Having said why,
           it is the game refusing the input more than the emulation
           failing. */
        return headless_msgboxes ? STARS_EGAME : STARS_ECRASH;
    }
}

/* ---- the directory ------------------------------------------------------------ */

static int buf_ok(StarsBuf b)
{
    return b.len == 0 || (b.data && b.len > 0 && b.len <= 0x7FFFFFFF);
}

/* The game's files go in as GAME.*: the name is the library's to choose, and
   nothing in the files depends on it. */
static void put_game(const StarsFS *fs)
{
    char name[16];
    int i;
    if (fs->hst.len) memfs_add("GAME.HST", fs->hst.data, fs->hst.len);
    if (fs->xy.len)  memfs_add("GAME.XY",  fs->xy.data,  fs->xy.len);
    for (i = 0; i < 16; i++) {
        if (fs->m[i].len) {
            snprintf(name, sizeof name, "GAME.M%d", i + 1);
            memfs_add(name, fs->m[i].data, fs->m[i].len);
        }
        if (fs->x[i].len) {
            snprintf(name, sizeof name, "GAME.X%d", i + 1);
            memfs_add(name, fs->x[i].data, fs->x[i].len);
        }
        if (fs->h[i].len) {
            snprintf(name, sizeof name, "GAME.H%d", i + 1);
            memfs_add(name, fs->h[i].data, fs->h[i].len);
        }
    }
}

/* Where a root file named for `ext` - "HST", "M3" - belongs in a StarsFS. */
static StarsBuf *slot_for(StarsFS *fs, const char *ext)
{
    int n;
    if (!strcmp(ext, "HST")) return &fs->hst;
    if (!strcmp(ext, "XY"))  return &fs->xy;
    if ((ext[0] == 'M' || ext[0] == 'X' || ext[0] == 'H') &&
        ext[1] >= '1' && ext[1] <= '9') {
        n = ext[1] - '0';
        if (ext[2] >= '0' && ext[2] <= '9' && !ext[3]) n = n * 10 + ext[2] - '0';
        else if (ext[2]) return NULL;
        if (n < 1 || n > 16) return NULL;
        return ext[0] == 'M' ? &fs->m[n - 1] : ext[0] == 'X' ? &fs->x[n - 1]
                                                             : &fs->h[n - 1];
    }
    return NULL;
}

/* The game as the directory now holds it: every file in the root whose
   extension says what it is, and whose name is `base` if one is given.  Files
   in directories - the backups - are not the game's current state. */
static void take_game(StarsFS *fs, const char *base)
{
    const char *name;
    const uint8_t *data;
    ptrdiff_t len;
    int i = 0, isdir, written;

    memset(fs, 0, sizeof *fs);
    while ((i = memfs_entry(i, &name, &data, &len, &isdir, &written)) != 0) {
        const char *dot = strrchr(name, '.');
        StarsBuf *b;
        if (isdir || strchr(name, '\\') || !dot) continue;
        if (base && (strncmp(name, base, (size_t)(dot - name)) ||
                     base[dot - name])) continue;
        if ((b = slot_for(fs, dot + 1)) != NULL) {
            b->data = (unsigned char *)data;
            b->len = len;
        }
    }
}

static int was_written(const char *want)
{
    const char *name;
    const uint8_t *data;
    ptrdiff_t len;
    int i = 0, isdir, written;
    while ((i = memfs_entry(i, &name, &data, &len, &isdir, &written)) != 0)
        if (!isdir && !strcmp(name, want)) return written;
    return 0;
}

/* ---- the API ------------------------------------------------------------------ */

static uint64_t fnv1a(const unsigned char *p, ptrdiff_t n)
{
    uint64_t h = 0xcbf29ce484222325ull;
    while (n--) { h ^= *p++; h *= 0x100000001b3ull; }
    return h;
}

int stars_init(StarsVM **out, StarsArena *perm, StarsBuf exe)
{
    StarsVM *vm;
    int rc;

    if (!out || !buf_ok(exe) || !exe.len) return STARS_EINPUT;
    *out = NULL;
    if (exe.len != EXE_LEN || fnv1a(exe.data, exe.len) != EXE_FNV)
        return STARS_EEXE;
    if ((rc = enter(perm, NULL)) != 0) return rc;
    if (setjmp(the_run.jmp)) return leave(perm, NULL, the_run.err);

    vm = lib_alloc((ptrdiff_t)sizeof *vm);
    vm->mod.img = lib_alloc(exe.len);
    memcpy(vm->mod.img, exe.data, (size_t)exe.len);
    vm->mod.imglen = (uint32_t)exe.len;
    if (!ne_parse(&vm->mod)) return leave(perm, NULL, STARS_EEXE);
    *out = vm;
    return leave(perm, NULL, STARS_OK);
}

int stars_generate(StarsVM *vm, StarsArena *arena, int nturns, StarsFS *fs,
                   StarsBuf *log)
{
    char args[32];
    StarsFS out;
    int rc, i;

    if (!vm || !fs || nturns < 1 || nturns > 1000) return STARS_EINPUT;
    if (!fs->hst.len || !fs->xy.len) return STARS_EINPUT;
    if (!buf_ok(fs->hst) || !buf_ok(fs->xy)) return STARS_EINPUT;
    for (i = 0; i < 16; i++)
        if (!buf_ok(fs->m[i]) || !buf_ok(fs->x[i]) || !buf_ok(fs->h[i]))
            return STARS_EINPUT;
    if ((rc = enter(arena, log)) != 0) return rc;
    if (setjmp(the_run.jmp)) return leave(arena, log, the_run.err);

    memfs_reset();
    put_game(fs);
    snprintf(args, sizeof args, " -g%d GAME.HST", nturns);
    rc = play(vm, args, (uint64_t)(nturns + 1) * 4000000000ull);
    if (rc == STARS_OK && !was_written("GAME.HST")) rc = STARS_EGAME;
    if (rc == STARS_OK) {
        take_game(&out, "GAME");
        *fs = out;
    }
    return leave(arena, log, rc);
}

int stars_newgame(StarsVM *vm, StarsArena *arena, StarsBuf def,
                  const StarsFile *races, int nraces, StarsFS *out,
                  StarsBuf *log)
{
    StarsFS fs;
    int rc, i;

    if (!vm || !out || !def.len || !buf_ok(def)) return STARS_EINPUT;
    if (nraces < 0 || nraces > 16 || (nraces && !races)) return STARS_EINPUT;
    for (i = 0; i < nraces; i++)
        if (!races[i].name || !races[i].buf.len || !buf_ok(races[i].buf))
            return STARS_EINPUT;
    if ((rc = enter(arena, log)) != 0) return rc;
    if (setjmp(the_run.jmp)) return leave(arena, log, the_run.err);

    memfs_reset();
    memfs_add("NEWGAME.DEF", def.data, def.len);
    for (i = 0; i < nraces; i++)
        memfs_add(races[i].name, races[i].buf.data, races[i].buf.len);
    rc = play(vm, " -a NEWGAME.DEF", 4000000000ull);
    if (rc == STARS_OK) {
        /* Named by the .def, so found by what they are. */
        take_game(&fs, NULL);
        if (!fs.hst.len || !fs.xy.len) rc = STARS_EGAME;
        else *out = fs;
    }
    return leave(arena, log, rc);
}

int stars_dump(StarsVM *vm, StarsArena *arena, const StarsFS *fs, int player,
               StarsDump *out, StarsBuf *log)
{
    const char *name;
    const uint8_t *data;
    ptrdiff_t len;
    int isdir, written;
    char args[32];
    StarsFS one;
    StarsDump d;
    int rc, i;

    if (!vm || !fs || !out || player < 1 || player > 16) return STARS_EINPUT;
    if (!fs->xy.len || !fs->m[player - 1].len) return STARS_EINPUT;
    if (!buf_ok(fs->xy) || !buf_ok(fs->m[player - 1]) ||
        !buf_ok(fs->x[player - 1]) || !buf_ok(fs->h[player - 1]))
        return STARS_EINPUT;
    if ((rc = enter(arena, log)) != 0) return rc;
    if (setjmp(the_run.jmp)) return leave(arena, log, the_run.err);

    /* The player's own files, and nobody else's. */
    memset(&one, 0, sizeof one);
    one.xy = fs->xy;
    one.m[player - 1] = fs->m[player - 1];
    one.x[player - 1] = fs->x[player - 1];
    one.h[player - 1] = fs->h[player - 1];
    memfs_reset();
    put_game(&one);
    snprintf(args, sizeof args, " -dfmp GAME.M%d", player);
    rc = play(vm, args, 4000000000ull);
    if (rc == STARS_OK) {
        memset(&d, 0, sizeof d);
        i = 0;
        while ((i = memfs_entry(i, &name, &data, &len, &isdir, &written)) != 0) {
            StarsBuf *b = NULL;
            if (isdir || !written) continue;
            if (!strcmp(name, "GAME.MAP")) b = &d.map;
            if (!strcmp(name, "GAME.PLA")) b = &d.pla;
            if (!strcmp(name, "GAME.FLE")) b = &d.fle;
            if (b) { b->data = (unsigned char *)data; b->len = len; }
        }
        if (!d.map.data || !d.pla.data || !d.fle.data) rc = STARS_EGAME;
        else *out = d;
    }
    return leave(arena, log, rc);
}

const char *stars_strerror(int err)
{
    switch (err) {
    case STARS_OK:        return "success";
    case STARS_ENOMEM:    return "arena too small";
    case STARS_EINPUT:    return "invalid argument";
    case STARS_EEXE:      return "not Stars! 2.70j";
    case STARS_EGAME:     return "the game reported a problem";
    case STARS_EPASSWORD: return "password protected";
    case STARS_EHUNG:     return "the game stopped to wait";
    case STARS_ECRASH:    return "emulation failed";
    case STARS_EBUSY:     return "another call is in progress";
    }
    return "unknown error";
}
