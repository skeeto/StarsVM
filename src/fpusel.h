/* fpusel.h - which x87 backend this build uses, decided once.
 *
 * FPU=hw (STARSVM_FPU_HW) runs the guest's x87 on the host's (x87hw.c),
 * FPU=soft (STARSVM_FPU_SOFT) in integers (x80.c), and FPU=dual
 * (STARSVM_FPU_DUAL) both side by side, comparing (x80dual.c).  With none
 * given, an x86 gets hw and anything else soft.  hw and dual need an x86,
 * and say so rather than failing somewhere inside the asm.
 *
 * FPU_USE_X87HW and FPU_USE_X80 are defined for whichever backends the
 * build compiles in; fpu.c and the unity files go by these alone.
 */
#ifndef FPUSEL_H
#define FPUSEL_H

#if defined(__i386__) || defined(__x86_64__)
#  define FPU_HOST_X86 1
#endif

#if defined(STARSVM_FPU_HW) || defined(STARSVM_FPU_DUAL)
#  ifndef FPU_HOST_X86
#    error "FPU=hw and FPU=dual need an x86 host; build this target with FPU=soft"
#  endif
#  define FPU_USE_X87HW 1
#  ifdef STARSVM_FPU_DUAL
#    define FPU_USE_X80 1
#  endif
#elif defined(STARSVM_FPU_SOFT) || !defined(FPU_HOST_X86)
#  define FPU_USE_X80 1
#else
#  define FPU_USE_X87HW 1
#endif

#endif
