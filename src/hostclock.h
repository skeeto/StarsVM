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
 */
#ifndef HOSTCLOCK_H
#define HOSTCLOCK_H

#include <stdint.h>
#include <windows.h>

extern int clock_fixed;            /* --fixed-clock */

/* GetTickCount/GetCurrentTime, and TOOLHELP's TimerCount. */
uint32_t host_tick(void);

/* int 21h AH=2Ah and AH=2Ch, the DOS date and time. */
void     host_localtime(SYSTEMTIME *st);

#endif
