/* unity_fuzz.c - the differential fuzzer as its own program.
 *
 * fuzz.c is the one source under src/ that unity.c does not include, and this
 * file is why.  The fuzzer's oracle works by emitting machine code into a page
 * it allocates PAGE_EXECUTE_READWRITE and then calls, which is the shape a
 * static security scanner is built to notice - and a game has no business
 * asking for writable-executable memory.  Built this way, fuzz.c:145 is the
 * only PAGE_EXECUTE_* in the tree and it is not in the emulator: the selector
 * arena reserves PAGE_NOACCESS and commits PAGE_READWRITE, because guest code
 * is interpreted rather than run, so StarsVM.exe requests no executable memory
 * at all.
 *
 * The other reason is plainer.  This tests Stars!VM against the host CPU; it
 * has nothing to say about Stars!, and nobody playing the game can reach it.
 *
 * The list below is the fuzzer's whole link closure, and it is closed for a
 * reason rather than by luck: -Wall makes an implicit declaration an error, so
 * a file can only call what its includes declare, and the include graph of
 * these seven reaches cpu.h, sel.h, log.h, fpu.h, thunk.h and imports.inc and
 * stops.  Nothing here touches task.h, heap.h, handle.h, ne.h or res.h - there
 * is no module registry, no window, no game.
 *
 * thunk.c and imports.c are ballast: cpu_step needs thunk_dispatch and
 * thunk_selector to link, but the fuzzer generates no control transfer, so
 * neither ever runs.  fpu.c used to be ballast too and is not any more - the
 * escape opcodes are generated now, so it is as much under test as cpu.c.
 * All of them are linked rather than stubbed on purpose: the whole point is
 * that the code under test is byte-for-byte the code the emulator runs, and a
 * stub is a second implementation to get wrong.
 */

#include "cpu.c"
#include "fpu.c"
#include "fuzz.c"
#include "imports.c"
#include "log.c"
#include "sel.c"
#include "thunk.c"
