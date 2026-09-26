/* stars.h - Stars! 2.70j hosting as a library.
 *
 * Runs the game's own batch modes - a new game from a .def (stars.exe -a),
 * turn generation (-g) and the text dumps (-d) - on files held in memory.  The
 * library does no I/O of any kind: every byte comes from its caller and every
 * byte it produces goes back to it, so it can sit inside a server, behind an
 * FFI, or in a command-line tool that does the reading and writing itself.
 *
 * It needs x86 or x86-64, since the game's floating point is run on the host's
 * own x87, and GCC or Clang to build.  The operating system does not matter.
 *
 * Memory.  Everything the library allocates comes from an arena the caller
 * hands it, never from malloc:
 *
 *     char *mem = malloc(64 << 20);
 *     StarsArena a = { mem, mem + (64 << 20) };
 *
 * A call takes what it needs from the front of the arena, advancing a.beg
 * past whatever it hands back, and borrows scratch space from the back for
 * the length of the call only.  Nothing is ever freed: to reclaim a call's
 * output, put a.beg back where it was.  Every output buffer points into the
 * arena and lives exactly as long as that memory does.  A call that fails -
 * STARS_ENOMEM included - leaves the arena as it was but for its log, which
 * is left at the front for the caller to read.
 *
 * STARS_ARENA_MIN is enough for a call on a small game, most of it the game's
 * own 16 MB address space.  The files come on top of that - a few hundred KB
 * for a small game, a few MB for a large one late on.  stars_init keeps a
 * copy of the exe, so its arena needs a little over 3 MB.
 *
 * Buffers.  A StarsBuf with len 0 is an absent file.  Input bytes are only
 * ever read, however the pointer is qualified.
 *
 * Threads.  One call at a time per process: the machine inside is global.  A
 * call made while another is running fails with STARS_EBUSY rather than wait.
 *
 * Passwords do not apply.  Generation never asks for one, and a dump opens a
 * player's file as if given the right one, without the caller's file being
 * changed: see stars_dump.
 */
#ifndef STARS_H
#define STARS_H

#include <stddef.h>

#ifndef STARS_API
#define STARS_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct { char *beg, *end; } StarsArena;
typedef struct { unsigned char *data; ptrdiff_t len; } StarsBuf;

/* A file by name, for the race files a .def names. */
typedef struct { const char *name; StarsBuf buf; } StarsFile;

/* A game: the host file, the universe, and per player (index 0 is player 1)
   the turn file .mN, the orders .xN and the history .hN. */
typedef struct {
    StarsBuf hst, xy;
    StarsBuf m[16], x[16], h[16];
} StarsFS;

/* The three text dumps, tab-separated as the game writes them. */
typedef struct { StarsBuf map, pla, fle; } StarsDump;

enum {
    STARS_OK     = 0,
    STARS_ENOMEM = 1,     /* the arena is too small                          */
    STARS_EINPUT = 2,     /* an argument is out of range or a file is absent  */
    STARS_EEXE   = 3,     /* not the stars.exe of Stars! 2.70j                */
    STARS_EGAME  = 4,     /* the game refused or failed; its words are in log */
    STARS_EHUNG  = 5,     /* the game stopped to wait for something           */
    STARS_ECRASH = 6,     /* the emulation failed; log says where             */
    STARS_EBUSY  = 7      /* another call is in progress                      */
};

#define STARS_ARENA_MIN (20 << 20)

typedef struct StarsVM StarsVM;

/* Take the game in: check that `exe` is Stars! 2.70j and prepare it, copying
   what is needed into `perm`, so `exe` need not outlive the call.  The
   StarsVM lives in `perm`, and every other call takes it. */
STARS_API int stars_init(StarsVM **vm, StarsArena *perm, StarsBuf exe);

/* stars.exe -a: create a game from a .def.  The .def names its race files,
   which are looked up among `races` by file name, whatever directory the .def
   puts them in.  On success `out` holds the new game's .hst, .xy and a .mN per
   player. */
STARS_API int stars_newgame(StarsVM *vm, StarsArena *arena, StarsBuf def,
                            const StarsFile *races, int nraces, StarsFS *out,
                            StarsBuf *log);

/* stars.exe -gN: generate `nturns` turns, 1 to 1000, from the game in `fs`,
   which is updated in place: .hst, .mN and .hN are replaced and the .xN
   consumed.  Players with no .xN simply submitted nothing. */
STARS_API int stars_generate(StarsVM *vm, StarsArena *arena, int nturns,
                             StarsFS *fs, StarsBuf *log);

/* stars.exe -dfmp: player `player`'s (1 to 16) view of the game in `fs` as
   text.  Reads that player's .mN, with .xN and .hN if present, and .xy.  A
   password on the .mN is no obstacle: the game is given a copy with it
   blanked, and writes the same dumps it would given the password. */
STARS_API int stars_dump(StarsVM *vm, StarsArena *arena, const StarsFS *fs,
                         int player, StarsDump *out, StarsBuf *log);

/* `log`, where a call takes one, may be NULL.  Otherwise it receives the
   run's diagnostics: every message box the game raised, and where anything
   went wrong. */

/* A short English name for a STARS_* code. */
STARS_API const char *stars_strerror(int err);

#ifdef __cplusplus
}
#endif

#endif
