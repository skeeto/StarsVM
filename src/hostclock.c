/* hostclock.c - see hostclock.h for why a pinned clock is worth having. */

#include "hostclock.h"

#include <string.h>

int clock_fixed = 0;

/* A Win16 timer ticked every 55 ms, so that is what one look at a pinned clock
   costs.  The base is arbitrary but large, so that guest code subtracting two
   readings cannot underflow its way somewhere strange early in a run. */
#define FIXED_STEP 55u
#define FIXED_BASE 0x00100000u

static uint32_t fixed_ms = FIXED_BASE;

uint32_t host_tick(void)
{
    if (!clock_fixed) return GetTickCount();
    fixed_ms += FIXED_STEP;
    return fixed_ms;
}

void host_localtime(SYSTEMTIME *st)
{
    uint32_t ms, s;

    if (!clock_fixed) { GetLocalTime(st); return; }

    /* Walked from the same millisecond count as host_tick, so the two cannot
       disagree about how much time has passed.  The date is fixed outright;
       nothing here runs long enough for the day to matter. */
    ms = (fixed_ms += FIXED_STEP);
    s  = ms / 1000u;
    memset(st, 0, sizeof *st);
    st->wYear         = 2026;
    st->wMonth        = 1;
    st->wDay          = 1;
    st->wDayOfWeek    = 4;
    st->wHour         = (WORD)(s / 3600u % 24u);
    st->wMinute       = (WORD)(s / 60u % 60u);
    st->wSecond       = (WORD)(s % 60u);
    st->wMilliseconds = (WORD)(ms % 1000u);
}
