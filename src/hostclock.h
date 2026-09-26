/* hostclock.h - the clocks the guest can read, and a switch to pin them.
 *
 * Stars! salts every save it writes from the clock, and the save format
 * scrambles itself from that salt, so two runs of the same `-g` over the same
 * input agree in their first twelve bytes and nowhere else.  That costs us the
 * only whole-system check this project has: turn generation is deterministic
 * given its inputs, so "the generated turn files are byte-identical" would
 * otherwise cover hundreds of millions of instructions at once, which is far
 * beyond anything the single-instruction fuzzer can reach.
 *
 * --fixed-clock buys it back.  Pin everything the guest can read and the same
 * input produces the same bytes; measured, it does.
 *
 * Pinned time still advances.  It has to: the game polls the tick count while
 * it works, and a clock that never moved would be a clock it waited on.
 *
 * The library build (src/unity_lib.c) has no other clock: it is always pinned,
 * so its output is a function of its input and nothing else.
 */
#ifndef HOSTCLOCK_H
#define HOSTCLOCK_H

#include <stdint.h>

extern int clock_fixed;            /* --fixed-clock */

/* The local date and time, in the fields DOS reports them in. */
typedef struct {
    uint16_t year, month, day, dow;          /* dow: 0 is Sunday */
    uint16_t hour, minute, second, ms;
} HostTime;

/* GetTickCount/GetCurrentTime, and TOOLHELP's TimerCount. */
uint32_t host_tick(void);

/* int 21h AH=2Ah and AH=2Ch, the DOS date and time. */
void     host_localtime(HostTime *t);

/* Put a pinned clock back where it starts, so a second run in the same
   process reads the same times as the first. */
void     hostclock_reset(void);

#endif
