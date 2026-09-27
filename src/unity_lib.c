/* unity_lib.c - the library, as one translation unit.  See stars.h.
 *
 * The emulator's interpreter, loader, heaps and KERNEL, with the Win32 halves
 * swapped out: fs_mem.c where fs_win32.c was, headless.c where the window
 * system was, libstars.c where main.c was; password.c is the library's
 * alone.  Nothing here includes <windows.h>, and STARSVM_LIB is what tells
 * the shared files so - the three of them that still care (sel.c,
 * hostclock.c, thunk.c) choose their library halves by it.
 *
 * Built for a static library, it exports nothing in particular; built with
 * STARS_SHARED for a DLL or shared object, only stars.h's functions are
 * visible.  The rest of the unity build's conventions are unity.c's.
 */

#define STARSVM_LIB 1

#ifdef STARS_SHARED
#  ifdef _WIN32
#    define STARS_API __declspec(dllexport)
#  else
#    define STARS_API __attribute__((visibility("default")))
#  endif
#endif

#include "api_common.c"
#include "api_dos.c"
#include "api_kernel.c"
#include "api_res.c"
#include "cpu.c"
#include "fpu.c"
#include "fs_mem.c"
#include "headless.c"
#include "heap.c"
#include "hostclock.c"
#include "imports.c"
#include "libstars.c"
#include "native.c"
#include "ne.c"
#include "password.c"
#include "sel.c"
#include "task.c"
#include "thunk.c"
#if defined(STARSVM_FPU_DUAL)
#include "x80.c"
#include "x80dual.c"
#endif
#include "x87hw.c"
