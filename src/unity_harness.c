/* unity_harness.c - StarsVM-harness.exe: the emulator with the LLM harness on.
 *
 * The same program as StarsVM.exe, not a second one that resembles it.  This
 * defines STARSVM_HARNESS and includes the ordinary unity build, so harness.h's
 * hooks stop being empty inline functions and harness.c becomes the named-pipe
 * control channel.  Everything else - the interpreter, the API layer, the game
 * - is the line that ships.
 *
 * A GUI binary, because it is the emulator and the game needs its windows; it
 * is left unstripped, because a failure in a dev tool is exactly when symbols
 * are wanted.  It is not built by `all`, and it is not part of a release.
 */

#define STARSVM_HARNESS 1
#include "unity.c"