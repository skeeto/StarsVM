/* unity_prof.c - StarsVM-prof.exe: the emulator with its counters switched on.
 *
 * Not a second program that resembles the emulator - the same one.  This
 * defines STARSVM_PROFILE and includes the ordinary unity build, so every line
 * the profiler measures is the line that ships.  What the macro changes is
 * prof.c, which is otherwise an empty file, and the hooks in prof.h, which are
 * otherwise empty inline functions.  cpu.c and main.c carry the call sites and
 * no #ifdef.
 *
 * Console, and unstripped, for the same reason the fuzzer and the packer are:
 * its output is the point, and a failure in it is exactly when symbols are
 * wanted.
 */

#define STARSVM_PROFILE 1
#include "unity.c"
