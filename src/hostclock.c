/* hostclock.c - see hostclock.h for why a pinned clock is worth having. */

#include "hostclock.h"

#include <string.h>
#ifndef STARSVM_LIB
#include <windows.h>
#endif

#ifdef STARSVM_LIB
int clock_fixed = 1;
#else
int clock_fixed = 0;
#endif

/* A Win16 timer ticked every 55 ms, so that is what one look at a pinned clock
   costs.  The base is arbitrary but large, so that guest code subtracting two
   readings cannot underflow its way somewhere strange early in a run. */
#define FIXED_STEP 55u
#define FIXED_BASE 0x00100000u

static uint32_t fixed_ms = FIXED_BASE;

void hostclock_reset(void)
{
    fixed_ms = FIXED_BASE;
}

uint32_t host_tick(void)
{
#ifndef STARSVM_LIB
    if (!clock_fixed) return GetTickCount();
#endif
    fixed_ms += FIXED_STEP;
    return fixed_ms;
}

void host_localtime(HostTime *t)
{
    uint32_t ms, s;

#ifndef STARSVM_LIB
    if (!clock_fixed) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        t->year   = st.wYear;
        t->month  = st.wMonth;
        t->day    = st.wDay;
        t->dow    = st.wDayOfWeek;
        t->hour   = st.wHour;
        t->minute = st.wMinute;
        t->second = st.wSecond;
        t->ms     = st.wMilliseconds;
        return;
    }
#endif

    /* Walked from the same millisecond count as host_tick, so the two cannot
       disagree about how much time has passed.  The date is fixed outright;
       nothing here runs long enough for the day to matter. */
    ms = (fixed_ms += FIXED_STEP);
    s  = ms / 1000u;
    memset(t, 0, sizeof *t);
    t->year   = 2026;
    t->month  = 1;
    t->day    = 1;
    t->dow    = 4;
    t->hour   = (uint16_t)(s / 3600u % 24u);
    t->minute = (uint16_t)(s / 60u % 60u);
    t->second = (uint16_t)(s % 60u);
    t->ms     = (uint16_t)(ms % 1000u);
}
