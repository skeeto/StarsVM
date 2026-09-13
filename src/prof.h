/* prof.h - what the interpreter actually executed, and where the time went.
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
 *
 * Two kinds of question are answered.  Where the instructions are: per-address
 * counts, from which basic blocks and functions are reconstructed and ranked,
 * because a block that is 12% of everything executed is the one worth replacing
 * with a native routine and no opcode histogram can point at it.  And where the
 * seconds are: the interpreter, the host API handlers, the x87, the string
 * loops - measured with the timestamp counter, so that nobody optimises the
 * interpreter when the time was in ReadFile.
 */
#ifndef PROF_H
#define PROF_H

#include <stdint.h>
#include "ne.h"

#ifdef STARSVM_PROFILE

/* Called once the module is loaded and relocated, which is when guest code is
   final: it takes the copy that the self-modifying-code check compares against,
   sizes the per-address counters, and learns which selector is which segment
   so the report can speak in the segN:offset terms the docs use. */
void prof_begin(NeModule *m);

/* One instruction, at its first byte, after any prefixes. */
void prof_op(uint16_t cs, uint16_t ip, uint8_t op);

/* The second byte of a 0x0F instruction, which prof_op could not know. */
void prof_op2(uint8_t op2);

/* One REP string instruction: elements moved, and the ticks the loop took.
   The opcode histogram counts it as one instruction however long it ran. */
void prof_rep(uint32_t elems, uint64_t ticks);

/* The timestamp counter.  Zero in a non-profiling build, so an expression like
   prof_tick() - t0 folds away along with the hook it feeds. */
uint64_t prof_tick(void);

/* Ticks spent inside fpu_exec for one escape instruction. */
void prof_fpu(uint64_t ticks);

/* Exclusive time in one API handler: what the handler itself cost, less any
   guest code it called back into.  begin returns a token, end charges it. */
uint64_t prof_api_begin(void);
void     prof_api_end(unsigned index, uint64_t token);

/* Brackets a nested guest run (call16), so that its time is subtracted from
   the handler that made the callback and from nothing else. */
uint64_t prof_nest_begin(void);
void     prof_nest_end(uint64_t token);

/* One native routine ran in place of `instrs` guest instructions. */
void prof_native(unsigned site, uint32_t instrs);

void prof_report(void);

#else

static inline void prof_begin(NeModule *m) { (void)m; }
static inline void prof_op(uint16_t cs, uint16_t ip, uint8_t op)
{ (void)cs; (void)ip; (void)op; }
static inline void prof_op2(uint8_t op2) { (void)op2; }
static inline void prof_rep(uint32_t elems, uint64_t ticks)
{ (void)elems; (void)ticks; }
static inline uint64_t prof_tick(void) { return 0; }
static inline void prof_fpu(uint64_t ticks) { (void)ticks; }
static inline uint64_t prof_api_begin(void) { return 0; }
static inline void prof_api_end(unsigned index, uint64_t token)
{ (void)index; (void)token; }
static inline uint64_t prof_nest_begin(void) { return 0; }
static inline void prof_nest_end(uint64_t token) { (void)token; }
static inline void prof_native(unsigned site, uint32_t instrs)
{ (void)site; (void)instrs; }
static inline void prof_report(void) {}

#endif
#endif
