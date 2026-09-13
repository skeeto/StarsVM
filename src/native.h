/* native.h - guest routines the emulator runs natively.
 *
 * The interpreter runs one fixed program, and a profile of turn generation
 * (make prof) shows that program spending a quarter of its instructions in five
 * basic blocks and half in twenty.  Those are game logic - the nearest-object
 * scan, the habitability formula, the random generator - small, closed, and
 * exactly specifiable from their disassembly.  Each one here is rewritten in C
 * and patched over the guest code: the first byte of the site becomes 0xD6,
 * an opcode no 16-bit compiler emits (SALC), which the interpreter dispatches
 * to the routine instead of decoding.
 *
 * The contract a routine keeps is the interpreter's, not the game's: starting
 * at its site it may perform any number of complete guest instructions and
 * must stop at an instruction boundary with the machine exactly as the
 * interpreter would have left it there - registers, flags computed by the
 * interpreter's own helpers, memory, the stack.  It may stop wherever it
 * likes, including at its own site after k iterations of a loop, and it may
 * decline, in which case the instruction it was patched over runs instead.
 * Because the interpreter re-dispatches every time control comes back to the
 * site, "run the iterations I model, decline the one I do not" composes into
 * a loop that is native on the common path and interpreted on the rare one,
 * with nothing to hand off but the machine state.
 *
 * Sites are named by NE segment and offset, as docs/copy-protection.md names
 * things, because a selector is only what this run happened to hand out.
 * Each carries the bytes expected there, and is not patched if they differ:
 * the routines are true of stars.exe 2.70j and would be quietly wrong for
 * anything else.
 *
 * Two switches keep them honest.  --no-native patches nothing, which is the
 * A/B and the bisection tool.  --verify-native N runs, every Nth time a site
 * is hit, both the routine and the code it replaced from the same state, and
 * requires the interpreter to arrive at the routine's stopping point with the
 * same registers, flags and memory.  The same state at the same address means
 * the same future, so that is the whole of what correctness means here and it
 * needs no description of what a routine touches.
 */
#ifndef NATIVE_H
#define NATIVE_H

#include <stdint.h>
#include "cpu.h"
#include "ne.h"

/* A routine returns the number of guest instructions it stood in for, or 0 to
   decline, in which case it must have changed nothing. */
typedef int (*NativeFn)(Cpu *c);

/* Patch every site whose bytes match.  After ne_load, before anything runs.
   Returns how many were patched. */
int native_install(NeModule *m);

/* --no-native: install nothing. */
void native_disable(void);

/* --verify-native N: check every Nth hit of each site against the interpreter. */
void native_verify(unsigned every);

/* For reports: the name of site `i`, and the name of whatever site is patched
   at an address (NULL if none), which is how a trace shows "native rand"
   rather than a stray SALC. */
const char *native_site_name(unsigned i);
const char *native_name_at(uint16_t sel, uint16_t off);

/* What ran natively, for the line report_stop prints beside the count of
   interpreted instructions. */
extern uint64_t native_calls, native_instrs;

#endif
