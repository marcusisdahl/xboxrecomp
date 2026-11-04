/* C99 library pieces missing from the Xbox 360 SDK's 2010-era CRT.
 *
 * Force-include this (cl /FI) ahead of recomp_types.h when building the
 * runtime or generated code for _XBOX. It declares nothing on other hosts.
 *
 * trunc/nearbyint are implemented, not just declared: generated x87 code
 * (frndint, fistp) calls them, so a declaration-only shim links on the probe
 * but fails as soon as real game code is compiled.
 *
 * nearbyint follows the current FPU rounding mode, like C99's. The 360
 * default (round to nearest, ties to even) matches the x87 default the
 * title runs with; a title that changes the x87 control word still needs the
 * runtime's rounding-mode handling, which this header does not provide. */
#ifndef RECOMP_C99_X360_H
#define RECOMP_C99_X360_H
#if defined(_XBOX)
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef INFINITY
#define INFINITY HUGE_VAL
#endif
#ifndef NAN
#define NAN (HUGE_VAL - HUGE_VAL)
#endif

static __inline int recomp_c99_signbit(double x)
{
    unsigned __int64 bits;
    memcpy(&bits, &x, sizeof bits);
    return (int)(bits >> 63);
}
static __inline int recomp_c99_isnan(double x) { return _isnan(x) != 0; }
static __inline int recomp_c99_isfinite(double x) { return _finite(x) != 0; }
static __inline int recomp_c99_isinf(double x) { return !_finite(x) && !_isnan(x); }
static __inline double recomp_c99_copysign(double x, double y) { return _copysign(x, y); }

static __inline double recomp_c99_trunc(double x)
{
    if (!_finite(x) || fabs(x) >= 4503599627370496.0 /* 2^52 */)
        return x;
    return _copysign(x < 0.0 ? ceil(x) : floor(x), x);   /* keeps -0.0 */
}

static __inline double recomp_c99_nearbyint(double x)
{
    /* Adding and removing 2^52 leaves an integer rounded in the current
     * mode. volatile stops the compiler folding the pair away. */
    volatile double big = 4503599627370496.0, t;
    if (!_finite(x) || fabs(x) >= 4503599627370496.0)
        return x;
    t = x < 0.0 ? (x - big) + big : (x + big) - big;
    return _copysign(t, x);
}

/* x87 fyl2x / f2xm1 lower to these. Accurate to a few ulp, not the x87's
 * 80-bit intermediate; good enough for the title's audio/log-scale uses,
 * not bit-exact with the original hardware. */
static __inline double recomp_c99_log2(double x) { return log(x) * 1.4426950408889634073599; }
static __inline double recomp_c99_exp2(double x) { return pow(2.0, x); }
#define log2 recomp_c99_log2
#define exp2 recomp_c99_exp2

/* The console has no process environment. Generated stubs and runtime
 * diagnostics ask for opt-in variables; on the 360 the answer is "unset". */
static __inline char *recomp_x360_getenv(const char *name) { (void)name; return 0; }
#define getenv recomp_x360_getenv

#define signbit(x)   recomp_c99_signbit((double)(x))
#define isnan(x)     recomp_c99_isnan((double)(x))
#define isfinite(x)  recomp_c99_isfinite((double)(x))
#define isinf(x)     recomp_c99_isinf((double)(x))
#define copysign     recomp_c99_copysign
#define trunc        recomp_c99_trunc
#define nearbyint    recomp_c99_nearbyint

#endif /* _XBOX */
#endif
