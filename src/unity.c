/* unity.c - the whole emulator as one translation unit.
 *
 * The Makefile compiles this file and nothing else.  Everything under src/ is
 * pulled in here, so the compiler sees the entire program at once and can
 * inline across what used to be object-file boundaries.  That matters more than
 * usual here: the hot path is cpu.c's interpreter loop reaching into sel.h and
 * thunk.c, and the build is -Oz, where a call it can prove is made once is a
 * call it can absorb.
 *
 * The sources stay ordinary .c files.  Each one still includes its own headers
 * and still compiles standalone, so nothing here depends on the order below and
 * `gcc -c src/cpu.c` remains a valid thing to do when bisecting a warning.  The
 * order is therefore just alphabetical, which is the order that needs no
 * justification and no maintenance.
 *
 * What a unity build does take away is the guarantee that a `static` name is
 * private to its file: every file-scope static now shares one namespace.  Three
 * collisions existed when this was introduced and each was resolved at the
 * source rather than papered over here -
 *
 *   - four identical private copies of `gstr` became gmem.h's g_str, which had
 *     been sitting there unused for exactly this purpose;
 *   - nedump.c's `rd16`, a duplicate of ne.c's, became dmp_rd16;
 *   - dlg.c's `pending_proc`/`pending_inst` became dlg_pending_*, since they
 *     track a dialog being created and winproc.c's track a window, and the two
 *     must not become one variable.
 *
 * Adding a file to src/ requires adding a line here.  The Makefile depends on
 * every source in the directory, so forgetting one shows up as a link error
 * naming the missing symbol, not as a stale object silently left behind.
 *
 * One source is missing on purpose: fuzz.c belongs to unity_fuzz.c, which
 * builds it as its own program.  It is not an omission to be tidied up - see
 * that file for why the emulator must not contain it.
 */

#include "api_dos.c"
#include "api_gdi.c"
#include "api_kernel.c"
#include "api_misc.c"
#include "api_profile.c"
#include "api_res.c"
#include "api_user.c"
#include "audio.c"
#include "cpu.c"
#include "disasm.c"
#include "dlg.c"
#include "fpu.c"
#include "handle.c"
#include "heap.c"
#include "imports.c"
#include "log.c"
#include "main.c"
#include "msg16.c"
#include "ne.c"
#include "nedump.c"
#include "resobj.c"
#include "sel.c"
#include "task.c"
#include "thunk.c"
#include "winproc.c"
