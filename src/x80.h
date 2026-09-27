/* x80.h - the x87's 80-bit extended format as plain data.
 *
 * An X80 is one x87 register's value: `m` the 64-bit significand with its
 * integer bit (J) explicit at bit 63, `se` the sign at bit 15 above a 15-bit
 * exponent biased by 16383.  Nothing here assumes a host format or an x87:
 * the conversions to and from the ten bytes of the memory image go a byte at
 * a time, and a backend that carries out x87 operations on X80s does so
 * through the interface below, whichever backend it is.
 *
 * An operation reads the control word from an X80Env and reports its effect
 * on the status word through it, and the caller commits that as
 *
 *     sw = (sw & ~env.cc) | env.sw
 *
 * so the operation decides which bits it defines (cc, which it may clear)
 * and which it sets (sw).  An exception flag belongs in sw but not in cc, so
 * that it accumulates the way the x87's sticky flags do; a condition code
 * the instruction leaves undefined belongs in neither, so that whatever the
 * guest last put there stays.
 */
#ifndef X80_H
#define X80_H

#include <stdint.h>

typedef struct {
    uint64_t m;
    uint16_t se;
} X80;

typedef struct {
    uint16_t cw;    /* in:  the control word, for precision and rounding */
    uint16_t sw;    /* out: the status word bits the operation sets      */
    uint16_t cc;    /* out: the bits it defines, cleared before sw goes in */
} X80Env;

/* The two-operand arithmetic, in the order the ModRM encoding of D8 lists
   them: ADD and SUB are a+b and a-b, SUBR b-a, DIV a/b and DIVR b/a. */
enum { X80_ADD, X80_MUL, X80_SUB = 4, X80_SUBR, X80_DIV, X80_DIVR };

/* The constants D9 E8..EE load, by their low three bits. */
enum { X80_ONE, X80_L2T, X80_L2E, X80_PI, X80_LG2, X80_LN2, X80_ZERO };

static inline void x80_get(X80 *v, const uint8_t b[10])
{
    v->m = (uint64_t)b[0]       | (uint64_t)b[1] << 8  |
           (uint64_t)b[2] << 16 | (uint64_t)b[3] << 24 |
           (uint64_t)b[4] << 32 | (uint64_t)b[5] << 40 |
           (uint64_t)b[6] << 48 | (uint64_t)b[7] << 56;
    v->se = (uint16_t)(b[8] | b[9] << 8);
}

static inline void x80_put(uint8_t b[10], const X80 *v)
{
    int i;
    for (i = 0; i < 8; i++) b[i] = (uint8_t)(v->m >> (8 * i));
    b[8] = (uint8_t)v->se;
    b[9] = (uint8_t)(v->se >> 8);
}

#endif
