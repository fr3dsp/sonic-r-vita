/**
 * sonicr_math.h — Precision-portable math.
 *
 * Routes double / long double math to either double or float at compile
 * time. Default = double (host x86 / SDL builds). Define SONICR_FLOAT32
 * for the Dreamcast SH4 `-m4-single-only` build, where double-precision
 * math is forbidden and `long double` libc routines (atan2l/sinl/lrintl)
 * are not provided by sh-elf newlib.
 */

#ifndef SONICR_MATH_H
#define SONICR_MATH_H

#include <math.h>

#ifdef SONICR_FLOAT32
typedef float sr_double;

static inline sr_double sr_atan2(sr_double y, sr_double x)
{
    return atan2f(y, x);
}

static inline sr_double sr_sqrt(sr_double x)
{
    return sqrtf(x);
}

static inline sr_double sr_sin(sr_double x) {
    return sinf(x);
}

static inline long sr_lrint(sr_double x) {
    return lrintf(x);
}
#else
typedef double sr_double;

static inline sr_double sr_atan2(sr_double y, sr_double x)
{
    return atan2(y, x);
}

static inline sr_double sr_sqrt(sr_double x)
{
    return sqrt(x);
}

static inline sr_double sr_sin(sr_double x)
{
    return sin(x);
}

static inline long sr_lrint(sr_double x)
{
    return lrint(x);
}
#endif

/* Always-float sqrt — independent of SONICR_FLOAT32. Use at sites where
 * the input is float and the result is float, to keep the operand off
 * the double-precision path on builds where sr_sqrt promotes (host SDL
 * default).
 *
 * On SH4 (DC) we go via fsrra (1/sqrt(x), 1 cycle) and one fmul, ~2
 * cycles total — substantially faster than fsqrt (~11 cycles). The
 * x==0 guard prevents the inf*0 → NaN that fsrra(0)*0 would produce.
 * Host falls back to libc sqrtf. */
#ifdef SONICR_DC
__attribute__((always_inline)) static inline float sr_inv_sqrtf(float x)
{
    asm volatile ("fsrra %0" : "+f"(x));
    return x;
}
__attribute__((always_inline)) static inline float sr_sqrtf(float x)
{
    return (x == 0.0f) ? 0.0f : sr_inv_sqrtf(x) * x;
}
#else
static inline float sr_sqrtf(float x)
{
    return sqrtf(x);
}
#endif

#endif /* SONICR_MATH_H */
