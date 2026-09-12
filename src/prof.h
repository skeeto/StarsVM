/* prof.h - what the interpreter actually executed.
 *
 * The hooks below are empty inline functions unless STARSVM_PROFILE is defined,
 * so the emulator carries the call sites and none of the machinery, and cpu.c
 * needs no #ifdef of its own.  src/unity_prof.c defines the macro and includes
 * unity.c, so the profiling build is the same program with counters switched
 * on rather than a second program that resembles it.
 *
 * Nothing here logs per instruction - log_msg flushes on every call, which at
 * half a billion instructions would be the only thing being measured.  It all
 * accumulates in memory and is reported once at exit.
 */
#ifndef PROF_H
#define PROF_H

#include <stdint.h>

#ifdef STARSVM_PROFILE

/* Called once the module is loaded and relocated, which is when guest code is
   final: it takes the copy that the self-modifying-code check compares against. */
void prof_begin(void);

/* One instruction, at its first byte, after any prefixes. */
void prof_op(uint16_t cs, uint16_t ip, uint8_t op);

/* The second byte of a 0x0F instruction, which prof_op could not know. */
void prof_op2(uint8_t op2);

/* Elements moved by one REP string instruction, which the opcode histogram
   counts as one instruction however long it ran. */
void prof_rep(uint32_t elems);

void prof_report(void);

#else

static inline void prof_begin(void) {}
static inline void prof_op(uint16_t cs, uint16_t ip, uint8_t op)
{ (void)cs; (void)ip; (void)op; }
static inline void prof_op2(uint8_t op2) { (void)op2; }
static inline void prof_rep(uint32_t elems) { (void)elems; }
static inline void prof_report(void) {}

#endif
#endif
