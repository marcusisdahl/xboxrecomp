/**
 * Xbox Static Recompilation - Runtime Type Definitions
 *
 * Type definitions and helper macros used by mechanically translated
 * x86 -> C code. Each original x86 function is translated to a C
 * function that uses these types and macros.
 *
 * This is a reusable template for ANY Xbox game. Game-specific
 * customization should go in separate headers.
 *
 * Memory model:
 *   Xbox data sections are mapped to their original VAs via
 *   CreateFileMapping + MapViewOfFileEx (see xbox_memory.h).
 *   Recompiled code accesses globals via pointer casts, e.g.:
 *     *(uint32_t*)0x003B2360
 *
 * Register model:
 *   Volatile registers (eax, ecx, edx, esp) are global variables,
 *   matching real x86 behavior where these registers are shared
 *   across all code. This enables correct argument passing via the
 *   simulated stack and return value communication via eax.
 *
 *   Callee-saved registers (ebx, esi, edi) are also global because
 *   callers pass implicit parameters through them (e.g. 'this' via
 *   esi in thiscall). The callee-save contract is enforced by
 *   PUSH32/POP32 instructions in the generated code, not by C local
 *   variable scoping.
 *
 *   ebp is NOT global - it stays local in each function because many
 *   FPO (Frame Pointer Omission) functions use it as scratch without
 *   save/restore. For SEH functions, g_seh_ebp bridges the gap.
 *
 * Calling convention:
 *   All translated functions are void(void). Arguments are passed
 *   on the simulated Xbox stack (via push instructions before call).
 *   Return values are communicated through g_eax.
 *   The call instruction pushes the real guest return address (the VA of
 *   the instruction after the call); ret discards it with esp += 4.
 *   The value is never used to transfer control -- control flow is C
 *   call/return -- but it must be correct because guest code reads it
 *   (__SEH_prolog's scope table, _alloca probes, "mov eax, [esp]").
 */

#ifndef RECOMP_TYPES_H
#define RECOMP_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
/* math.h is load-bearing, and its absence was invisible.
 *
 * The lifter emits sqrt() and fabs() for fsqrt/fabs -- 146 of them in one of
 * Halo's nine chunks alone -- and now sin/cos/tan/atan2/log2/exp2/fmod for the
 * x87 transcendentals. With no declaration in scope, C89 implicit declaration
 * makes every one of them return `int`: the caller reads EAX instead of XMM0 and
 * gets garbage, then converts that garbage to double. Vector normalisation is
 * 1/sqrt(x), so this corrupts every matrix the title builds.
 *
 * Nothing reported it because generated code is compiled with /w (see the game
 * CMakeLists) -- MSVC's C4013 was emitted and discarded. Same failure as the
 * missing stdlib.h in kernel_bridge.c, in a hotter path. */
#include <math.h>
#if defined(_M_IX86) || defined(_M_X64) || defined(__i386__) || defined(__x86_64__)
#include <xmmintrin.h>
#endif

/* MSVC's __forceinline -> gcc/clang equivalent on POSIX. */
#if !defined(_MSC_VER) && !defined(__forceinline)
#define __forceinline inline __attribute__((always_inline))
#endif

/* Guest debug output: INT 2D, the Xbox kernel debug trap.
 *
 * eax selects the service, ecx carries its argument. Service 1 prints the
 * ANSI_STRING ecx points at -- what OutputDebugStringA compiles to. The int3
 * that always follows is the slide byte the kernel steps over, so the lifter
 * emits nothing for it; lifting it to __debugbreak() terminated the process
 * with STATUS_BREAKPOINT the first time a title tried to print. */
void recomp_debug_service(uint32_t service, uint32_t arg_va);

/* MSVC's __debugbreak() intrinsic -> gcc/clang equivalent.
 * The auto-generated code emits __debugbreak for x86 INT 3 instructions.
 * !defined(__debugbreak) is not enough on its own: MinGW declares __debugbreak
 * as a function, not a macro, so that test passes and the macro is defined
 * anyway. Any windows.h reaching this TU afterwards then fails to declare it.
 * Excluding _WIN32 outright leaves the SDK's own int3 intrinsic in place. */
#if !defined(_MSC_VER) && !defined(_WIN32) && !defined(__debugbreak)
#define __debugbreak() __builtin_trap()
#endif

/* ================================================================
 * Memory offset
 * ================================================================ */

/**
 * Memory offset from Xbox VA to actual mapped address.
 * When Xbox memory is mapped at the original address (0x00010000),
 * this is 0 and the MEM macros are simple identity casts.
 * When mapped elsewhere, this adjusts all memory accesses.
 *
 * Set once during memory initialization, then read-only.
 */
extern ptrdiff_t g_xbox_mem_offset;

/* Bounds of the title's executable sections, filled in by
 * xbox_MemoryLayoutInit from the XBE's own section table. Zero means "not
 * loaded yet", which RECOMP_ICALL_IS_CODE treats as allow.
 *
 * These replace a hardcoded 0x00400000 cutoff that was only ever right for one
 * title -- see the comment where they are defined in xbox_memory_layout.c. */
/* --force-return: read once at startup, so the check at each forced ret is a
 * load rather than a getenv. Zero unless RECOMP_FORCE_RETURN is set, which is
 * what lets a build carrying forced functions behave normally by default. */
extern int g_force_return;

extern uint32_t g_xbox_code_lo;
extern uint32_t g_xbox_code_hi;

/* Is this indirect-call target plausibly code?
 *
 * Synthetic kernel thunks live at 0xFE0000xx and are dispatched by
 * recomp_lookup_kernel, so they are always allowed. Everything else has to lie
 * inside an executable section of the loaded title. A target that fails this is
 * a garbage pointer that reached a call site, and calling it would be worse
 * than dropping it. */
#define RECOMP_ICALL_IS_CODE(_va) \
    ((_va) >= 0xFE000000u || g_xbox_code_hi == 0u || \
     ((_va) >= g_xbox_code_lo && (_va) < g_xbox_code_hi))

/* ================================================================
 * Global registers
 * ================================================================ */

/**
 * Volatile x86 registers (caller-saved):
 *   eax - return values, general accumulator
 *   ecx - 'this' pointer for thiscall, loop counter
 *   edx - high dword of multiply/divide, general
 *   esp - stack pointer (initialized to top of Xbox stack)
 *
 * Callee-saved x86 registers (also global):
 *   ebx, esi, edi - global because callers pass implicit parameters
 *   through them. The callee-save contract is enforced by generated
 *   PUSH32/POP32 instructions.
 *
 * NOT global: ebp - stays local in each function because FPO
 * functions use it as scratch. For SEH, g_seh_ebp bridges the gap.
 */
/* Per-thread register state.
 *
 * These started as plain globals, which works exactly as long as one thread
 * runs recompiled code. Halo is the first title to create real workers (its
 * cache/file loader), and the runtime papered over that by running every worker
 * synchronously inside PsCreateSystemThreadEx -- so a worker that blocks
 * waiting for work never returns and startup deadlocks.
 *
 * On hardware each thread has its own register set, so model it that way.
 * Thread-local costs an indirection per access; a deadlock costs the title. */
#if defined(_MSC_VER)
#  define RECOMP_TLS __declspec(thread)
#elif defined(__GNUC__) || defined(__clang__)
#  define RECOMP_TLS __thread
#else
#  define RECOMP_TLS _Thread_local
#endif

extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
extern RECOMP_TLS uint32_t g_ebx, g_esi, g_edi;

/* x87 stack. Per-thread for the same reason the integer registers are:
 * arguments are passed in st(0)/st(1) across call boundaries. */
extern RECOMP_TLS double g_fp_stack[8];
extern RECOMP_TLS int g_fp_top;

/**
 * SEH frame pointer bridge.
 *
 * __SEH_prolog sets up ebp for the caller, but since ebp is a local
 * variable in each function, the caller can't see the prolog's change.
 * The prolog writes g_seh_ebp, and the caller reads it after the call.
 * Similarly, __SEH_epilog reads g_seh_ebp at entry and writes it at exit.
 */
/* Linear base of the fs segment: where the fake TIB lives. Generated code
 * adds this to every fs-relative address, so page zero stays unmapped and a
 * null dereference faults instead of hitting the TIB. Must match
 * XBOX_FS_BASE in src/kernel/xbox_memory_layout.h.
 *
 * Per-thread, because a TIB is. fs:[0] is the SEH chain head and fs:[4]
 * reaches the CRT's per-thread data, so a single shared base makes every
 * guest thread the same thread as far as the CRT is concerned. */
extern RECOMP_TLS uint32_t g_fs_base;
#define XBOX_FS_BASE g_fs_base

extern RECOMP_TLS uint32_t g_seh_ebp;

/* ---- non-local jumps (setjmp / longjmp) --------------------------------
 *
 * A recompiled function is a real C function, so restoring the guest esp is
 * only half of a longjmp: the abandoned frames are still on the native stack,
 * and control returns into them once the resume point finishes. Pairing each
 * guest jmp_buf with a native one -- taken at the setjmp call site, the only
 * place a native setjmp is valid -- makes the guest jump a native jump, so the
 * frames actually unwind.
 *
 * recomp_setjmp_slot() returns the native buffer for a guest jmp_buf address,
 * allocating one on first use. recomp_guest_longjmp() restores the guest
 * registers the CRT's longjmp would and jumps; it returns 0, without jumping,
 * for a buffer that has no native counterpart, leaving the caller to fall back
 * to the guest's own longjmp.
 */
/* Atomic read-modify-write, for the lock-prefixed instructions.
 *
 * "lock xadd" and "lock cmpxchg" are what InterlockedIncrement and
 * InterlockedCompareExchange compile to, so they carry a title's reference
 * counts and its lock-free lists. They used to lift to a TODO comment, which
 * meant a refcount that never moved and a compare-and-swap that never swapped
 * -- harmless while every guest thread ran synchronously, and not once the
 * runtime began spawning real ones.
 *
 * Genuinely atomic, not merely correct in isolation: the guest's own threads
 * now run on real host threads, so a read-modify-write that races is exactly
 * the bug these instructions exist to prevent.
 */
#if defined(RECOMP_EXPLICIT_GUEST_BUS)
/* The old macros receive host pointers. They must not run against guest
 * addresses on the explicit bus. The port must emit guest-address atomics. */
#define RECOMP_ATOMIC_ADD32(p, v) XR_PORT_GUEST_ATOMIC_ADD_REQUIRED()
#define RECOMP_ATOMIC_CAS32(p, cmp, val) XR_PORT_GUEST_ATOMIC_CAS_REQUIRED()
/* Guest-address forms the lifter emits in explicit mode. Defined after the
 * guest-reference facade below (they need xr_guest_require). */
#elif defined(_MSC_VER)
#include <intrin.h>
#define RECOMP_ATOMIC_ADD32(p, v)     ((uint32_t)_InterlockedExchangeAdd((volatile long *)(p), (long)(v)))
#define RECOMP_ATOMIC_CAS32(p, cmp, val)     ((uint32_t)_InterlockedCompareExchange((volatile long *)(p),                                            (long)(val), (long)(cmp)))
#else
#define RECOMP_ATOMIC_ADD32(p, v)     ((uint32_t)__sync_fetch_and_add((volatile uint32_t *)(p), (uint32_t)(v)))
#define RECOMP_ATOMIC_CAS32(p, cmp, val)     ((uint32_t)__sync_val_compare_and_swap((volatile uint32_t *)(p),                                            (uint32_t)(cmp), (uint32_t)(val)))
#endif

#include <setjmp.h>
jmp_buf *recomp_setjmp_slot(uint32_t buf_va);
int recomp_guest_longjmp(uint32_t buf_va, uint32_t value);
extern RECOMP_TLS uint32_t g_ebp;

/* EFLAGS.DF, and the signed step the string instructions take because of it.
 *
 * `cld`/`std` used to lift to a comment and every string instruction stepped
 * forwards regardless. That is right almost everywhere -- DF is 0 by ABI and
 * the compiler restores it -- but MSVC's strrchr is `std; repne scasb` from
 * the terminator backwards, so it scanned forwards off the end of the string
 * and returned a pointer into whatever followed. The Xbox Dashboard uses it to
 * split "y:\default.xip" into a mount path; with strrchr wrong the archive
 * registered itself under its own full filename, no resource in it could ever
 * be found by name, and the dashboard rebooted rather than showing a UI.
 * memmove's overlapping case is the same instruction and the same bug.
 */
extern RECOMP_TLS int g_df;
#define RECOMP_DF_STEP(n) (g_df ? -(int32_t)(n) : (int32_t)(n))

/* EFLAGS as pushfd and popfd see it.
 *
 * The arithmetic flags are lazy: a jcc rebuilds them from the instruction
 * that set them, so no EFLAGS word exists until pushfd needs one. g_eflags is
 * the last image popfd loaded, masked to the bits a P3 lets ring 0 write
 * (CF PF AF ZF SF TF IF DF OF IOPL NT AC ID), with bit 1 always set. The ID
 * bit (21) round-trips, which is how code learns the CPU has cpuid; a jcc
 * right after popfd reads its flags from here.
 *
 * recomp_pushfd(known, bits) builds the word: the flags in `known` from
 * `bits` (what the lifter rebuilt from the tracked setter), DF from g_df, the
 * rest from g_eflags. Thread-local like g_df.
 *
 * Only popfd writes g_eflags. cli and sti do not update its IF bit (they
 * have no lift that touches it), so a pushfd after them reports the IF of
 * the last popfd image -- set, unless the guest popped it clear. */
extern RECOMP_TLS uint32_t g_eflags;
#define RECOMP_EFL_WRITABLE 0x00247FD5u
static inline uint32_t recomp_pushfd(uint32_t known, uint32_t bits) {
    return (g_eflags & ~(known | 0x400u)) | (bits & known) | 0x2u
         | (g_df ? 0x400u : 0u);
}
static inline void recomp_popfd(uint32_t v) {
    g_eflags = (v & RECOMP_EFL_WRITABLE) | 0x2u;
    g_df = (int)((v >> 10) & 1u);
}

/* cpuid: out[0..3] = eax, ebx, ecx, edx for leaf `leaf` (subleaf unused).
 * The runtime answers as the Xbox's Coppermine-class Pentium III. */
void xbox_Cpuid(uint32_t leaf, uint32_t subleaf, uint32_t out[4]);

/* x87 control and status. Thread-local for the same reason the x87 stack
   above is: one guest routine can lift to several C functions, so a compare
   and the FNSTSW that reads it can land in different bodies, and the control
   word has to survive a call. (g_fp_stack/g_fp_top are declared above.) */
extern RECOMP_TLS uint16_t g_fp_control_word;
extern RECOMP_TLS int g_fp_cmp;
extern RECOMP_TLS uint16_t g_fp_cc;
/* x87 precision control (control word bits 8-9). The stack is double-backed,
 * which matches PC=53 and is close enough for PC=64, but PC=24 -- what the
 * Xbox runs with -- rounds every arithmetic result to float. A title that
 * compares a freshly computed value against the same value stored as a float
 * relies on that: Burnout 3's sorted draw list re-inserts a node by walking
 * while `v > next` / `v < prev`, and with v left at double precision it sits
 * between its own stored copy and a neighbour and walks back and forth
 * forever, which froze every race at the start line.
 *
 * PC narrows the significand only; the exponent keeps the register's range.
 * So a plain (float) cast is right only inside float's range -- outside it,
 * it would turn a large intermediate into inf (and inf*0 into NaN) or flush a
 * tiny one to 0, where the x87 carries on. Those go the long way.
 *
 * Every x87 arithmetic result goes through RECOMP_FP_PC (4,677 sites in
 * Burnout 3), so the common case is forced inline: |x| in float's normal
 * range is one frsp after two independent compares (NaN fails both), zero
 * returns as it is, and only the rest (NaN, inf, out of float range) calls
 * the out-of-line body -- the same results as before for every input. Out
 * of line, the Xbox 360 compiler called the whole function at every site
 * (a stack frame and a load-hit-store per x87 operation). */
#if defined(_MSC_VER)
#define RECOMP_HOT_INLINE static __forceinline
#define RECOMP_COLD static inline __declspec(noinline)
#elif defined(__GNUC__)
#define RECOMP_HOT_INLINE static inline __attribute__((always_inline))
#define RECOMP_COLD static inline __attribute__((noinline))
#else
#define RECOMP_HOT_INLINE static inline
#define RECOMP_COLD static inline
#endif
RECOMP_COLD double recomp_fp_round24_slow(double x) {
    double ax = fabs(x);
    int e;
    if ((ax <= 3.4028234663852886e38 && ax >= 1.1754943508222875e-38) || ax == 0.0
        || x != x || ax == INFINITY)
        return (double)(float)x;
    x = frexp(x, &e);                      /* |x| in [0.5, 1): float-exact range */
    return ldexp((double)(float)x, e);
}
RECOMP_HOT_INLINE double recomp_fp_round24(double x) {
    double ax = fabs(x);
    if (ax <= 3.4028234663852886e38 && ax >= 1.1754943508222875e-38)
        return (double)(float)x;
    if (ax == 0.0) return x;               /* the signed zero (double)(float)x gives */
    return recomp_fp_round24_slow(x);
}
RECOMP_HOT_INLINE double recomp_fp_pc(double x) {
    return (g_fp_control_word & 0x300u) ? x : recomp_fp_round24(x);
}
#define RECOMP_FP_PC(x) recomp_fp_pc((double)(x))
#define RECOMP_FCMP_CC(c) ((uint16_t)((c)==2 ? 0x4500u : (c)<0 ? 0x0100u : (c)>0 ? 0u : 0x4000u))
/* Values in the existing double-backed stack are all representable as normal
 * x87 extended values, including binary64 subnormals. Empty stack tags and
 * unsupported extended encodings are not represented by this stack model. */
/* Classification by comparison, for the hot x87 helpers. MSVC's isfinite,
 * isnan and isinf are CRT calls (_dtest in C++; _finite/_isnan on the Xbox
 * 360 shim), which cost more than the helpers' own work; a comparison stays
 * in the FPU (no float-to-integer register move, which stalls the 360).
 * Like RECOMP_FCMP, these rely on IEEE comparisons (/fp:precise). */
#define RECOMP_FP_MAX_FINITE 1.7976931348623157e308
static inline int recomp_fp_isfinite(double x) { return fabs(x) <= RECOMP_FP_MAX_FINITE; }
static inline int recomp_fp_isnan(double x) { return x != x; }
static inline int recomp_fp_isinf(double x) { return fabs(x) > RECOMP_FP_MAX_FINITE && x == x; }
static inline uint16_t recomp_fxam(double value) {
    return (uint16_t)((signbit(value) ? 0x0200u : 0u) |
        (recomp_fp_isnan(value) ? 0x0100u : recomp_fp_isinf(value) ? 0x0500u :
         value == 0.0 ? 0x4000u : 0x0400u));
}

/* x87 FSIN, FCOS, FSINCOS and FPTAN, as the hardware computes them.
 *
 * The x87 reduces the operand modulo pi/4 with a 66-bit pi,
 * 0xC90FDAA22168C234C * 2^-66, not the true pi libm reduces with. The
 * reduced argument is off by k * (pi - pi66) / 4 for k eighth-turns removed,
 * so the hardware returns f(x * (1 + D)), D = (pi - pi66) / pi66, where libm
 * returns f(x). At x = 1e10 the two cosines differ in the 11th digit, and the
 * x87's fsin(pi) is 1.2246063538223773e-16, not 1.2246467991473532e-16. A
 * title that runs sin() on an accumulated, never-wrapped angle drifts from
 * libm that way on a Pentium III, so the recompiled one has to drift too.
 *
 * 1 + D rounds to 1, so the shift is applied as a correction to libm's s and
 * c by angle addition, in the versine form (v = 1 - cos e = 2 sin^2(e/2)):
 * the correction stays small next to s and the sum rounds once, which keeps a
 * result near a zero of the function accurate. Below |e| = 2^-26 (|x| under
 * about 1.16e13) sin e is e and v is below half an ulp, so that costs no
 * libm call beyond the other one of the pair. tests/x87_trig/model.py derives
 * the vectors and the error budget (a third of the conformance tolerance).
 * tan = sin/cos loses relative accuracy as cos(x * (1 + D)) nears 0, as the
 * hardware's own tan does there; the budget sweep leaves out |cos| < 1e-2.
 *
 * For |x| <= pi/4 the hardware does no reduction and libm's value is
 * returned as it is. No precision-control rounding: PC only narrows FADD,
 * FSUB, FMUL, FDIV and FSQRT, and the transcendentals always deliver 64 bits.
 *
 * Finite |x| >= 2^63 is out of range: the hardware sets C2, leaves st0 alone
 * and pushes nothing. In range, C2 is cleared. MSVC's CRT reads exactly that,
 * `fsin; fnstsw ax; sahf; jp reduce`, and reduces with an fprem1 loop. Inf
 * and NaN give NaN with C2 clear (invalid is masked), as libm's do, and the
 * value FSINCOS and FPTAN push is that NaN too: FPTAN pushes no 1.0 then
 * (measured with tools.conformance and a stack probe on a Ryzen 7 3700X). Each
 * helper returns whether the operation completed, so the lifted FSINCOS and
 * FPTAN push only when the hardware does. */
#define RECOMP_X87_PI66_D 1.2874146789751208e-21 /* 0x1.8518e2da2e893p-70 */
static inline int recomp_x87_trig_oor(double x) {
    return fabs(x) >= 9223372036854775808.0 && recomp_fp_isfinite(x); /* 2^63 */
}
static inline void recomp_x87_shift(double x, double *s, double *c) {
    double e = x * RECOMP_X87_PI66_D, se, v, sx = *s, cx = *c;
    if (fabs(e) < 1.4901161193847656e-08) { /* 2^-26 */
        se = e; v = 0.0;
    } else {
        double h = sin(0.5 * e);
        se = sin(e); v = 2.0 * h * h;
    }
    *s = sx + (cx * se - sx * v);
    *c = cx - (sx * se + cx * v);
}
static inline int recomp_x87_cc_c2(uint16_t *cc, int oor) {
    *cc = (uint16_t)((*cc & ~0x0400u) | (oor ? 0x0400u : 0u));
    return !oor;
}
static inline int recomp_x87_fsin(double *st0, uint16_t *cc) {
    double x = *st0, s, c;
    if (recomp_x87_trig_oor(x)) return recomp_x87_cc_c2(cc, 1);
    if (!(fabs(x) > 0.78539816339744830962) || !recomp_fp_isfinite(x)) {
        *st0 = sin(x);
    } else {
        s = sin(x); c = cos(x);
        recomp_x87_shift(x, &s, &c);
        *st0 = s;
    }
    return recomp_x87_cc_c2(cc, 0);
}
static inline int recomp_x87_fcos(double *st0, uint16_t *cc) {
    double x = *st0, s, c;
    if (recomp_x87_trig_oor(x)) return recomp_x87_cc_c2(cc, 1);
    if (!(fabs(x) > 0.78539816339744830962) || !recomp_fp_isfinite(x)) {
        *st0 = cos(x);
    } else {
        s = sin(x); c = cos(x);
        recomp_x87_shift(x, &s, &c);
        *st0 = c;
    }
    return recomp_x87_cc_c2(cc, 0);
}
/* st0 becomes sin, and *cos_out is what FSINCOS then pushes. */
static inline int recomp_x87_fsincos(double *st0, double *cos_out, uint16_t *cc) {
    double x = *st0, s, c;
    if (recomp_x87_trig_oor(x)) return recomp_x87_cc_c2(cc, 1);
    s = sin(x); c = cos(x);
    if (fabs(x) > 0.78539816339744830962 && recomp_fp_isfinite(x))
        recomp_x87_shift(x, &s, &c);
    *st0 = s; *cos_out = c;
    return recomp_x87_cc_c2(cc, 0);
}
/* st0 becomes tan, and *push is what FPTAN then pushes: 1.0, which callers
 * divide by, or for an inf or NaN operand the NaN st0 now holds. */
static inline int recomp_x87_fptan(double *st0, double *push, uint16_t *cc) {
    double x = *st0, s, c;
    if (recomp_x87_trig_oor(x)) return recomp_x87_cc_c2(cc, 1);
    *push = 1.0;
    if (!recomp_fp_isfinite(x)) {
        *st0 = *push = tan(x);
    } else if (!(fabs(x) > 0.78539816339744830962)) {
        *st0 = tan(x);
    } else {
        s = sin(x); c = cos(x);
        recomp_x87_shift(x, &s, &c);
        *st0 = s / c;
    }
    return recomp_x87_cc_c2(cc, 0);
}

/* Result of an x87 compare, in the shape the status word wants:
 *   -1 less, 0 equal, 1 greater, 2 unordered (either operand is NaN).
 * The unordered case is not a curiosity: `fucompp` of a value with itself
 * followed by `test ah, 0x44; jp` is how this era's CRT asks "is this a NaN",
 * and collapsing it to "equal" answers no every time. */
#define RECOMP_FCMP(a, b)     (((a) != (a) || (b) != (b)) ? 2 : (a) < (b) ? -1 : (a) > (b) ? 1 : 0)
/* x87 rounding to an integral value under the guest RC bits (control word
 * bits 10-11: 0 nearest-even, 1 down, 2 up, 3 toward zero). FRNDINT rounds
 * this way, and so does FIST before its store; neither may follow the HOST's
 * rounding mode, which the guest's fldcw never reaches. MSVC's CRT floor() and
 * ceil() are `fldcw RC=down/up` (via _ctrlfp) around _frnd, a bare frndint,
 * so a frndint lifted as rint() made both of them round to nearest.
 * NaN, the infinities and values already integral come back unchanged, and a
 * zero result keeps the operand's sign, as FRNDINT's does.
 *
 * Every FIST/FISTP runs this, so it makes no library calls: |value| >= 2^52,
 * the infinities and NaN are integral or unordered and returned as they are;
 * below 2^52 the int64 truncation is exact, and floor/ceil/nearest-even
 * follow from it. Same results as floor/ceil/trunc/fmod, except that
 * recomp_x87_round may return +0.0 where they give -0.0 (FIST cannot tell;
 * recomp_frndint restores the sign). */
/* On the Xbox 360 the int64 round trips below are the expensive part: Xenon
 * has no FPR-to-GPR move, so each (int64_t)/(double) pair is fctidz, stfd,
 * ld, std, lfd, fcfid -- two load-hit-store stalls of about 40 cycles each,
 * three or four per call. The XMV player's cvtps2pi (MMX_CVT_F2I) spent a
 * quarter of the guest thread here. __fctidz/__fcfid keep the integer in the
 * FPR, and the parity test for ties is done there too (lo/2 is exact; it is
 * integral exactly when lo is even), so the whole rounding stays in FPRs and
 * never depends on the host FPSCR rounding mode. Checked by the console lane
 * test (cvtps2pi/cvttps2pi cases include exact ties and out-of-range values). */
#if defined(_XBOX) && !defined(_M_IX86) && !defined(_M_X64)
#define RECOMP_X87_ROUND_FPR 1
#include <ppcintrinsics.h>
#else
#define RECOMP_X87_ROUND_FPR 0
#endif
/* Forced inline on the Xbox 360, where the compiler kept it (and
 * recomp_fist) out of line: a call, a frame and a load-hit-store on the
 * saved link register per FIST. Other hosts keep plain inline. */
#if RECOMP_X87_ROUND_FPR
#define RECOMP_X87_INLINE RECOMP_HOT_INLINE
#else
#define RECOMP_X87_INLINE static inline
#endif
RECOMP_X87_INLINE double recomp_x87_round(double value, uint16_t control) {
    double t, lo, fraction;
    if (!(fabs(value) < 4503599627370496.0)) return value;   /* 2^52, inf, NaN */
#if RECOMP_X87_ROUND_FPR
    t = __fcfid(__fctidz(value));                             /* toward zero */
    switch ((control >> 10) & 3) {
    case 1: return t > value ? t - 1.0 : t;                   /* down */
    case 2: return t < value ? t + 1.0 : t;                   /* up */
    case 3: return t;                                         /* chop */
    default: {                                                /* nearest-even */
        double half;
        lo = t > value ? t - 1.0 : t;
        fraction = value - lo;
        if (fraction > 0.5) return lo + 1.0;
        if (fraction < 0.5) return lo;
        half = lo * 0.5;
        return half != __fcfid(__fctidz(half)) ? lo + 1.0 : lo;   /* lo odd */
    }
    }
#endif
    t = (double)(int64_t)value;                               /* toward zero */
    switch ((control >> 10) & 3) {
    case 1: return t > value ? t - 1.0 : t;                   /* down */
    case 2: return t < value ? t + 1.0 : t;                   /* up */
    case 3: return t;                                         /* chop */
    default:                                                  /* nearest-even */
        lo = t > value ? t - 1.0 : t;
        fraction = value - lo;
        if (fraction > 0.5 || (fraction == 0.5 && ((int64_t)lo & 1))) return lo + 1.0;
        return lo;
    }
}
static inline double recomp_frndint(double value, uint16_t control) {
    double rounded = recomp_x87_round(value, control);
    if (rounded != 0.0) return rounded;
    return value > 0.0 ? 0.0 : value < 0.0 ? copysign(0.0, -1.0) : value;
}
/* x87 integer stores use the guest RC bits, independently of host rounding.
 * Masked invalid conversions store the signed integer-indefinite value. */
#if RECOMP_X87_ROUND_FPR
RECOMP_COLD int64_t recomp_fist_slow(double value, uint16_t control, unsigned bits)
#else
static inline int64_t recomp_fist(double value, uint16_t control, unsigned bits)
#endif
{
    double rounded = recomp_x87_round(value, control);
    double limit = bits == 32 ? 2147483648.0 : bits == 16 ? 32768.0 :
                   bits == 64 ? 9223372036854775808.0 : ldexp(1.0, (int)bits - 1);
    if (!(rounded >= -limit && rounded < limit))               /* also NaN */
        return bits==64?INT64_MIN:-(INT64_C(1)<<(bits-1));
    return (int64_t)rounded;
}
#if RECOMP_X87_ROUND_FPR
/* Xbox 360: the 16- and 32-bit stores (all but a handful of FIST sites)
 * inline, converting through a 32-bit integer; every value they return,
 * the indefinite one included, is the sign extension of its low word, so no
 * wide 64-bit constant is built at the call site (the 2010-01 compiler can
 * fold those with a wrong top half). 64-bit stores stay out of line. */
RECOMP_HOT_INLINE int64_t recomp_fist(double value, uint16_t control, unsigned bits) {
    if (bits == 32 || bits == 16) {
        double rounded = recomp_x87_round(value, control);
        double limit = bits == 32 ? 2147483648.0 : 32768.0;
        if (rounded >= -limit && rounded < limit)              /* not NaN */
            return (int64_t)(int32_t)rounded;
        return bits == 32 ? (int64_t)(-2147483647 - 1) : (int64_t)-32768;
    }
    return recomp_fist_slow(value, control, bits);
}
#endif

/* ================================================================
 * ICALL trace ring buffer (for debugging indirect calls)
 * ================================================================ */

/** Size of the ring buffer (must be power of 2). */
#define ICALL_TRACE_SIZE 16

/** Ring buffer of recent indirect call target VAs. */
extern volatile uint32_t g_icall_trace[ICALL_TRACE_SIZE];

/** Current write index into the ring buffer. */
extern volatile uint32_t g_icall_trace_idx;

/** Total count of indirect calls executed. */
extern volatile uint64_t g_icall_count;

/**
 * Called when an indirect call target cannot be resolved.
 * Implement this in your game-specific code to log diagnostics.
 * The va parameter is the Xbox VA that failed to resolve.
 */
void recomp_icall_fail_log(uint32_t va);

/* Report an indirect call whose target is not code (a null or wild
 * function pointer). Rate-limited per address by the implementation,
 * because these usually arrive inside a loop -- which is exactly why
 * they must be reported: silently skipping one turns a diagnosable null
 * vtable call into an unexplained hang. */
void recomp_icall_not_code_log(uint32_t va);

/* Indirect-branch target feedback. The ring buffer above is crash forensics --
 * 16 entries, overwritten constantly. This is a durable, deduplicated record of
 * every target the title ever reached, for feeding back into the next codegen
 * run (tools/recomp/icall_feedback.py).
 *
 * The header is pulled in only when the feature is on, so a default build needs
 * neither the file nor src/kernel on its include path. Disabled,
 * RECOMP_ICALL_OBSERVE discards its arguments without expanding them, so the
 * RECOMP_ICALL_SEEN_* constants need not exist either. */
#ifdef RECOMP_ICALL_FEEDBACK
#include "recomp_icall_feedback.h"
#else
#define RECOMP_ICALL_OBSERVE(va, flags) ((void)0)
#define RECOMP_ICALL_OBSERVE_SITE(site, va) ((void)0)
#endif

/**
 * Function entry trace, emitted only for addresses passed to
 * tools.recomp --trace-functions. Bring-up is largely "which of these
 * init calls does it not come back from", and answering that by
 * overriding a function loses the body you were trying to observe.
 */
/* The guest's time source.
 *
 * Xbox's QueryPerformanceCounter is a bare rdtsc and its
 * QueryPerformanceFrequency returns the CPU clock as a constant, so the guest
 * divides this by 733,333,333 to get seconds. Returning the host TSC would
 * make that division wrong by the ratio of the two clocks; the runtime scales
 * to the console's rate instead. */
uint64_t xbox_ReadTimeStampCounter(void);

/* A guest busy-wait loop, lowered by tools.recomp --spin-waits: the back
 * edge of a loop that only reads memory and branches back calls this once
 * the condition was seen still true. In the "spin" pacing mode (the default)
 * it does nothing and the loop spins as on the console; in "sleep" the
 * thread blocks until the kernel signals a state change or 1 ms passes
 * (src/kernel/kernel_pacing.h). */
extern volatile int g_xbox_spin_sleep;
/* Non-zero while a device interrupt waits for the one-CPU gate holder: the
 * wait is then also the holder's safe point to run it, at any pacing
 * (src/kernel/kernel_hal.c, xbox_IrqPost). */
extern volatile int g_xbox_irq_pending;
void xbox_SpinWait(uint32_t va);
#define RECOMP_SPIN_WAIT(va) \
    do { if (g_xbox_spin_sleep | g_xbox_irq_pending) xbox_SpinWait(va); } while (0)

/* --patches (tools/recomp/patches.py): the value a patched guest load read,
 * or `value` while the setting `key` reads `when` (src/platform/
 * recomp_patch.h). A setting the host never enabled leaves the load alone. */
uint32_t recomp_patch_u32(const char *key, const char *when, uint32_t value,
                          uint32_t loaded);
uint32_t recomp_patch_u32_latch(const char *key, const char *when,
                                uint32_t value, uint32_t loaded, int mode);
#define RECOMP_PATCH_U32(key, when, value, loaded) \
    recomp_patch_u32((key), (when), (value), (loaded))
/* mode 1: this load samples the setting; 2: it follows the last sample. */
#define RECOMP_PATCH_U32_LATCH(key, when, value, loaded, mode) \
    recomp_patch_u32_latch((key), (when), (value), (loaded), (mode))
/* The float kinds (load_f32, store_f32): `expr` is a double expression of
 * the moved float and of the host's names (recomp_patch_var), evaluated only
 * while the setting is on. _BITS is the same for a float a `mov` carries in
 * a general register or stores from one. */
int recomp_patch_on(const char *key, const char *when, int mode);
double recomp_patch_var(const char *name);
static inline float RECOMP_F32_OF_BITS(uint32_t bits) {
    union { uint32_t u; float f; } v;
    v.u = bits;
    return v.f;
}
static inline uint32_t RECOMP_BITS_OF_F32(float f) {
    union { uint32_t u; float f; } v;
    v.f = f;
    return v.u;
}
/* kind "mark": the host learns the title reached this instruction (a
 * function registered for the name, recomp_patch_on_mark); nothing else. */
void recomp_patch_mark(const char *name);
#define RECOMP_PATCH_MARK(name) recomp_patch_mark(name)
#define RECOMP_PATCH_F32(key, when, mode, expr, loaded) \
    (recomp_patch_on((key), (when), (mode)) ? (float)(expr) : (loaded))
#define RECOMP_PATCH_F32_BITS(key, when, mode, expr, loaded) \
    (recomp_patch_on((key), (when), (mode)) \
        ? RECOMP_BITS_OF_F32((float)(expr)) : (loaded))

/* x86 port I/O (`in`/`out`). width is 1, 2 or 4 bytes. The runtime models
 * the ports titles are known to read (the field pin at 0x80C0) and logs the
 * first access to any other. */
uint32_t xbox_PortIn(uint32_t port, int width);
void     xbox_PortOut(uint32_t port, uint32_t value, int width);

void recomp_trace_enter(const char *name, uint32_t va);
#define RECOMP_TRACE_ENTER(name, va) recomp_trace_enter((name), (va))
void recomp_trace_exit(const char *name, uint32_t va);
#define RECOMP_TRACE_EXIT(name, va) recomp_trace_exit((name), (va))
void recomp_trace_esp(const char *name, const char *tag);
#define RECOMP_TRACE_ESP(name, tag) recomp_trace_esp((name), (tag))


/* ================================================================
 * Memory access helpers
 * ================================================================ */

/**
 * Translate an Xbox VA to an actual pointer.
 * Mask to 32-bit first: Xbox addresses are 32-bit and arithmetic
 * in the recompiled code can overflow. Without the mask, a 64-bit
 * uintptr_t cast preserves the overflow bits, landing us 4GB+ past
 * our mapping and causing access violations.
 */
#if defined(RECOMP_EXPLICIT_GUEST_BUS)
#define XBOX_PTR(addr) XR_RAW_GUEST_POINTER_REQUIRES_PORTING(addr)
#else
#define XBOX_PTR(addr) ((uintptr_t)(uint32_t)(addr) + g_xbox_mem_offset)
#endif

/** Read/write N bytes at a flat Xbox memory address. */
#if defined(RECOMP_EXPLICIT_GUEST_BUS)
#include "platform/recomp_guest_ref.h"
#define MEM8(addr) XR_MEM8(addr)
#define MEM16(addr) XR_MEM16(addr)
#define MEM32(addr) XR_MEM32(addr)
#define SMEM8(addr) XR_SMEM8(addr)
#define SMEM16(addr) XR_SMEM16(addr)
#define SMEM32(addr) XR_SMEM32(addr)
#define SMEM64(addr) XR_SMEM64(addr)
#define MEMF(addr) XR_MEMF(addr)
#define MEMD(addr) XR_MEMD(addr)
#include "platform/recomp_guest_atomic_win.h"
static inline uint32_t RECOMP_GUEST_ATOMIC_ADD32(uint32_t va, uint32_t v) {
    uint32_t old = 0;
    xr_guest_require(xr_guest_atomic_add32(g_recomp_guest_bus, va, v, &old), va, 4);
    return old;
}
static inline uint32_t RECOMP_GUEST_ATOMIC_CAS32(uint32_t va, uint32_t cmp, uint32_t val) {
    uint32_t old = 0;
    xr_guest_require(xr_guest_atomic_cas32(g_recomp_guest_bus, va, cmp, val, &old), va, 4);
    return old;
}
#else
#define MEM8(addr)   (*(volatile uint8_t  *)XBOX_PTR(addr))
#define MEM16(addr)  (*(volatile uint16_t *)XBOX_PTR(addr))
#define MEM32(addr)  (*(volatile uint32_t *)XBOX_PTR(addr))

/** Signed memory reads. */
#define SMEM8(addr)  (*(volatile int8_t   *)XBOX_PTR(addr))
#define SMEM16(addr) (*(volatile int16_t  *)XBOX_PTR(addr))
#define SMEM32(addr) (*(volatile int32_t  *)XBOX_PTR(addr))
#define SMEM64(addr) (*(volatile int64_t  *)XBOX_PTR(addr))

/** Float/double memory access. */
#define MEMF(addr)   (*(volatile float    *)XBOX_PTR(addr))
#define MEMD(addr)   (*(volatile double   *)XBOX_PTR(addr))
#endif

/** Device registers at a constant address, by call instead of by fault.
 *
 * The lifter emits these for an 8/16/32-bit load or store whose address is a
 * literal inside a trapped device window (tools/recomp/mmio_lower.py): the
 * APU's registers and the OHCI controllers. As MEMn they would fault and be
 * decoded by the trap handler; here they call the same device model directly
 * (xbox_MmioLoad/Store, kernel/xbox_memory_layout.c), which falls back to the
 * plain access -- and so to the trap -- for anything no model owns. */
uint32_t xbox_MmioLoad(uint32_t va, int size);
void     xbox_MmioStore(uint32_t va, uint32_t v, int size);
#define MMIO_RD8(addr)      ((uint8_t)xbox_MmioLoad((uint32_t)(addr), 1))
#define MMIO_RD16(addr)     ((uint16_t)xbox_MmioLoad((uint32_t)(addr), 2))
#define MMIO_RD32(addr)     xbox_MmioLoad((uint32_t)(addr), 4)
#define MMIO_WR8(addr, v)   xbox_MmioStore((uint32_t)(addr), (uint8_t)(v), 1)
#define MMIO_WR16(addr, v)  xbox_MmioStore((uint32_t)(addr), (uint16_t)(v), 2)
#define MMIO_WR32(addr, v)  xbox_MmioStore((uint32_t)(addr), (uint32_t)(v), 4)

/* ================================================================
 * SSE / XMM register state
 *
 * XMM is 128 bits of architectural state, not a scalar float. Modelling
 * it as a `float` made movaps/movups transfer 4 of 16 bytes and silently
 * drop the upper three lanes, and left the packed arithmetic with no
 * representation at all.
 *
 * The registers are global for the same reason the volatile GPRs are:
 * one guest routine can lift to several C functions, so a value produced
 * in one body and read in the next has to outlive the body that wrote it.
 * A function-local declaration would also shadow these, and the local
 * starts zeroed -- a returned float would silently read as 0.0.
 *
 * The helpers are lane-wise C rather than host intrinsics: the guest
 * semantics stay explicit (MINPS returning src on unordered, CMPNEQPS
 * being the unordered form) and the header stays portable.
 * ================================================================ */

/* Host byte order, for the register unions below and in the MMX file.
 *
 * The guest is little-endian and its memory stays little-endian on every
 * host (the explicit guest bus converts each scalar access). The register
 * unions are host objects, though, and a union's narrower views only line up
 * with the guest's lane numbering when the host is little-endian too: on the
 * Xbox 360 (big-endian PowerPC) byte 0 of a uint64_t is its MOST significant
 * byte, so `mm.ub[0]` is x86 byte lane 7 and `mm.ud[0]` is the high dword.
 * Every helper that moved data between lanes -- punpck*, pack*, pshufw,
 * psadbw, movd out of an mm register -- then moved the wrong lanes, and the
 * XMV decoder's MMX IDCT painted the intro movies as black-and-white noise
 * while the little-endian build of the same code showed them correctly.
 *
 * Set RECOMP_HOST_BIG_ENDIAN explicitly to override the detection. */
#ifndef RECOMP_HOST_BIG_ENDIAN
#  if defined(_M_PPC) || defined(_M_PPCBE) || defined(__BIG_ENDIAN__) || \
      defined(__ARMEB__) || defined(__MIPSEB__) || \
      (defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && \
       __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__) || \
      (defined(_XBOX) && !defined(_M_IX86) && !defined(_M_X64))
#    define RECOMP_HOST_BIG_ENDIAN 1
#  else
#    define RECOMP_HOST_BIG_ENDIAN 0
#  endif
#endif

/* movss/movd into a register as one assignment, not a 16-byte temporary
 * copied over the register. The Xbox 360 compiler builds the temporary in
 * the stack frame and copies it with 64-bit loads right after storing lane
 * 0 as a float -- a load-hit-store stall on Xenon, two per movss -- and
 * generated code does `xmmN = XMM_SCALAR(...)` at tens of thousands of
 * sites. With the proxy the assignment writes the four lanes in place. On
 * by default where it pays (big-endian C++ builds); same results anywhere,
 * and the PC lane test builds it with /DRECOMP_XMM_SCALAR_PROXY=1 too. */
#ifndef RECOMP_XMM_SCALAR_PROXY
#  if defined(__cplusplus)
#    define RECOMP_XMM_SCALAR_PROXY RECOMP_HOST_BIG_ENDIAN
#  else
#    define RECOMP_XMM_SCALAR_PROXY 0
#  endif
#endif
#if RECOMP_XMM_SCALAR_PROXY
struct RecompXmmScalar;
#endif

#ifndef RECOMP_XMM_DEFINED
#define RECOMP_XMM_DEFINED
typedef union RecompXmm {
    float    f[4];
    double   d[2];
    uint32_t u[4];
    int32_t  i[4];
    uint64_t q[2];
#if RECOMP_XMM_SCALAR_PROXY
    /* xmmN = XMM_SCALAR(v) straight into the register (see below). */
    inline union RecompXmm &operator=(const struct RecompXmmScalar &s);
#define RECOMP_XMM_HAS_SCALAR_ASSIGN 1
#endif
} RecompXmm;
#endif

/* Register operands of the XMM helpers: by const reference on the same
 * builds as the movss proxy. By value, the Xbox 360 compiler copies each
 * 16-byte union to the stack with 64-bit loads and stores and then reads
 * the lanes back as floats or words -- a load-hit-store per lane. By
 * reference the lanes are read where they are. Same results either way:
 * every helper builds its result in a local before returning it, so
 * `xmm0 = XMM_ADD(xmm0, xmm0)` still reads both operands first. */
#ifndef RECOMP_XMM_REF_ARGS
#define RECOMP_XMM_REF_ARGS RECOMP_XMM_SCALAR_PROXY
#endif
#if RECOMP_XMM_REF_ARGS
#define RECOMP_XMM_IN const RecompXmm &
#else
#define RECOMP_XMM_IN RecompXmm
#endif


/* XMM lane invariant: f[k], u[k] and i[k] are x86 dword lane k on every host
 * (each is one whole 32-bit element, so host byte order never splits one),
 * and XMM_MEM/XMM_STORE move the register dword by dword through MEM32. The
 * 64-bit views q[k] and d[k] are NOT x86 qword lane k on a big-endian host --
 * there q[0] holds lane 0 in its high half -- so they may only be used to copy
 * or clear a half as raw storage. Anything that reads or writes a 64-bit lane
 * as a number goes through these accessors, which are the plain union access
 * on a little-endian host. */
static inline uint64_t XMM_GET_Q(RecompXmm r, int k) {
#if RECOMP_HOST_BIG_ENDIAN
    return (uint64_t)r.u[k * 2] | ((uint64_t)r.u[k * 2 + 1] << 32);
#else
    return r.q[k];
#endif
}
static inline void XMM_SET_Q(RecompXmm *r, int k, uint64_t v) {
#if RECOMP_HOST_BIG_ENDIAN
    r->u[k * 2] = (uint32_t)v; r->u[k * 2 + 1] = (uint32_t)(v >> 32);
#else
    r->q[k] = v;
#endif
}
static inline double XMM_GET_D(RecompXmm r, int k) {
#if RECOMP_HOST_BIG_ENDIAN
    uint64_t bits = XMM_GET_Q(r, k); double v;
    memcpy(&v, &bits, sizeof v);
    return v;
#else
    return r.d[k];
#endif
}
static inline void XMM_SET_D(RecompXmm *r, int k, double v) {
#if RECOMP_HOST_BIG_ENDIAN
    uint64_t bits; memcpy(&bits, &v, sizeof bits);
    XMM_SET_Q(r, k, bits);
#else
    r->d[k] = v;
#endif
}

extern RECOMP_TLS RecompXmm g_xmm0, g_xmm1, g_xmm2, g_xmm3;
extern RECOMP_TLS RecompXmm g_xmm4, g_xmm5, g_xmm6, g_xmm7;

/* -- construction -- */

static inline RecompXmm XMM_ZERO(void) {
    RecompXmm r; r.q[0] = 0; r.q[1] = 0; return r;
}

/** movss from memory: lane 0 set, upper lanes zeroed. */
#if RECOMP_XMM_SCALAR_PROXY && defined(RECOMP_XMM_HAS_SCALAR_ASSIGN)
struct RecompXmmScalar {
    float v;
    operator RecompXmm() const { RecompXmm r = XMM_ZERO(); r.f[0] = v; return r; }
};
inline RecompXmm &RecompXmm::operator=(const RecompXmmScalar &s) {
    f[0] = s.v; u[1] = 0; u[2] = 0; u[3] = 0; return *this;
}
static inline RecompXmmScalar XMM_SCALAR(float v) {
    RecompXmmScalar s; s.v = v; return s;
}
#else
static inline RecompXmm XMM_SCALAR(float v) {
    RecompXmm r = XMM_ZERO(); r.f[0] = v; return r;
}
#endif

/** movsd from memory: low double set, high double zeroed. */
static inline RecompXmm XMM_SCALAR_DOUBLE(double v) {
    RecompXmm r = XMM_ZERO(); XMM_SET_D(&r, 0, v); return r;
}

/** movd: 32 raw bits into lane 0, upper lanes zeroed. */
static inline RecompXmm XMM_SCALAR_BITS(uint32_t bits) {
    RecompXmm r = XMM_ZERO(); r.u[0] = bits; return r;
}

/* -- guest memory --
 * Addresses are guest VAs, so they go through MEM32 like every other
 * access. Done lane-wise, which is also unaligned-safe for movups. */

#if defined(RECOMP_EXPLICIT_GUEST_BUS)
/* One fast-table lookup for all 16 bytes when they are plain RAM that the
 * fast path takes (recomp_guest_ref.h: a listed page; on big-endian hosts
 * also not crossing into the next page), else the four MEM32 accesses out
 * of line; the same four dword accesses either way. */
XR_REF_NOINLINE inline RecompXmm recomp_xmm_mem_slow(uint32_t addr) {
    RecompXmm r;
    r.u[0] = MEM32(addr);      r.u[1] = MEM32(addr + 4);
    r.u[2] = MEM32(addr + 8);  r.u[3] = MEM32(addr + 12);
    return r;
}
XR_REF_NOINLINE inline void recomp_xmm_store_slow(uint32_t addr, RecompXmm v) {
    MEM32(addr)      = v.u[0]; MEM32(addr + 4)  = v.u[1];
    MEM32(addr + 8)  = v.u[2]; MEM32(addr + 12) = v.u[3];
}
/* XR_GUEST_REF_OUTLINE (recomp_guest_ref.h) keeps these out of line too. */
#if defined(XR_GUEST_REF_OUTLINE)
#define RECOMP_XMM_MEM_FN XR_REF_NOINLINE inline
#else
#define RECOMP_XMM_MEM_FN static inline
#endif
RECOMP_XMM_MEM_FN RecompXmm XMM_MEM(uint32_t addr) {
    RecompXmm r;
    uint8_t *p = xr_guest_fast_ptr(addr, 16);
    if (!p) return recomp_xmm_mem_slow(addr);
    r.u[0] = xr_guest_raw_load32(p);     r.u[1] = xr_guest_raw_load32(p + 4);
    r.u[2] = xr_guest_raw_load32(p + 8); r.u[3] = xr_guest_raw_load32(p + 12);
    return r;
}
RECOMP_XMM_MEM_FN void XMM_STORE(uint32_t addr, RECOMP_XMM_IN v) {
    uint8_t *p = xr_guest_fast_ptr(addr, 16);
    if (!p) { recomp_xmm_store_slow(addr, v); return; }
    xr_guest_raw_store32(p, v.u[0]);     xr_guest_raw_store32(p + 4, v.u[1]);
    xr_guest_raw_store32(p + 8, v.u[2]); xr_guest_raw_store32(p + 12, v.u[3]);
}
#else
static inline RecompXmm XMM_MEM(uint32_t addr) {
    RecompXmm r;
    r.u[0] = MEM32(addr);      r.u[1] = MEM32(addr + 4);
    r.u[2] = MEM32(addr + 8);  r.u[3] = MEM32(addr + 12);
    return r;
}

static inline void XMM_STORE(uint32_t addr, RECOMP_XMM_IN v) {
    MEM32(addr)      = v.u[0]; MEM32(addr + 4)  = v.u[1];
    MEM32(addr + 8)  = v.u[2]; MEM32(addr + 12) = v.u[3];
}
#endif

/* movlps/movhps move 8 bytes into or out of one half, leaving the
 * other half alone. */
#define XMM_LOAD_LOW(dst, addr)   recomp_xmm_load_half(&(dst), (addr), 0)
#define XMM_LOAD_HIGH(dst, addr)  recomp_xmm_load_half(&(dst), (addr), 1)
#define XMM_STORE_LOW(addr, src)  recomp_xmm_store_half((addr), (src), 0)
#define XMM_STORE_HIGH(addr, src) recomp_xmm_store_half((addr), (src), 1)

static inline void recomp_xmm_load_half(RecompXmm *dst, uint32_t addr,
                                        int high) {
    dst->u[high * 2]     = MEM32(addr);
    dst->u[high * 2 + 1] = MEM32(addr + 4);
}

static inline void recomp_xmm_store_half(uint32_t addr, RECOMP_XMM_IN src,
                                         int high) {
    MEM32(addr)     = src.u[high * 2];
    MEM32(addr + 4) = src.u[high * 2 + 1];
}

/** movlhps: dst high = src low. */
static inline RecompXmm XMM_MOVE_LOW_TO_HIGH(RECOMP_XMM_IN a, RECOMP_XMM_IN b) {
    RecompXmm r; r.q[0] = a.q[0]; r.q[1] = b.q[0]; return r;
}

/** movhlps: dst low = src high. */
static inline RecompXmm XMM_MOVE_HIGH_TO_LOW(RECOMP_XMM_IN a, RECOMP_XMM_IN b) {
    RecompXmm r; r.q[0] = b.q[1]; r.q[1] = a.q[1]; return r;
}

/* -- packed arithmetic -- */

#define RECOMP_XMM_LANEWISE(name, expr)                                   \
    static inline RecompXmm name(RECOMP_XMM_IN a, RECOMP_XMM_IN b) {      \
        RecompXmm r; int i;                                               \
        for (i = 0; i < 4; ++i) { (void)a; (void)b; r.f[i] = (expr); }    \
        return r;                                                         \
    }

RECOMP_XMM_LANEWISE(XMM_ADD, a.f[i] + b.f[i])
RECOMP_XMM_LANEWISE(XMM_SUB, a.f[i] - b.f[i])
RECOMP_XMM_LANEWISE(XMM_MUL, a.f[i] * b.f[i])
RECOMP_XMM_LANEWISE(XMM_DIV, a.f[i] / b.f[i])
/* MINPS/MAXPS return the second operand when the lanes are unordered or
 * equal -- that is the hardware's tie-break, not a C fmin/fmax. */
RECOMP_XMM_LANEWISE(XMM_MIN, (a.f[i] < b.f[i]) ? a.f[i] : b.f[i])
RECOMP_XMM_LANEWISE(XMM_MAX, (a.f[i] > b.f[i]) ? a.f[i] : b.f[i])

#define RECOMP_XMM_BITWISE(name, expr)                                    \
    static inline RecompXmm name(RECOMP_XMM_IN a, RECOMP_XMM_IN b) {      \
        RecompXmm r; int i;                                               \
        for (i = 0; i < 4; ++i) { (void)a; (void)b; r.u[i] = (expr); }    \
        return r;                                                         \
    }

RECOMP_XMM_BITWISE(XMM_AND,  a.u[i] & b.u[i])
RECOMP_XMM_BITWISE(XMM_OR,   a.u[i] | b.u[i])
RECOMP_XMM_BITWISE(XMM_XOR,  a.u[i] ^ b.u[i])
/* ANDNPS is ~dst & src, not dst & ~src. */
RECOMP_XMM_BITWISE(XMM_ANDN, (~a.u[i]) & b.u[i])

/* Compares produce an all-ones or all-zero mask per lane. EQ/LT/LE are
 * the ordered forms (false when either lane is NaN); NEQ is the
 * unordered form, so it is true when a lane is NaN. */
RECOMP_XMM_BITWISE(XMM_CMP_EQ,  (a.f[i] == b.f[i]) ? 0xFFFFFFFFu : 0u)
RECOMP_XMM_BITWISE(XMM_CMP_LT,  (a.f[i] <  b.f[i]) ? 0xFFFFFFFFu : 0u)
RECOMP_XMM_BITWISE(XMM_CMP_LE,  (a.f[i] <= b.f[i]) ? 0xFFFFFFFFu : 0u)
RECOMP_XMM_BITWISE(XMM_CMP_NEQ, (a.f[i] == b.f[i]) ? 0u : 0xFFFFFFFFu)

/* The full SSE compare predicate set, by CMPPS/CMPSS immediate:
 * 0 EQ, 1 LT, 2 LE, 3 UNORD, 4 NEQ, 5 NLT, 6 NLE, 7 ORD. The N forms are
 * the negations, so they are true when either side is NaN. */
static inline int recomp_cmp_pred(float a, float b, int p) {
    switch (p & 7) {
    case 0:  return a == b;
    case 1:  return a < b;
    case 2:  return a <= b;
    case 3:  return a != a || b != b;
    case 4:  return !(a == b);
    case 5:  return !(a < b);
    case 6:  return !(a <= b);
    default: return !(a != a || b != b);
    }
}
static inline RecompXmm XMM_CMP_PRED(RECOMP_XMM_IN a, RECOMP_XMM_IN b, int p) {
    RecompXmm r; int i;
    for (i = 0; i < 4; ++i)
        r.u[i] = recomp_cmp_pred(a.f[i], b.f[i], p) ? 0xFFFFFFFFu : 0u;
    return r;
}
/* Packed unary ops; the first argument is unused, as for the binary forms. */
RECOMP_XMM_LANEWISE(XMM_SQRT,  sqrtf(b.f[i]))
RECOMP_XMM_LANEWISE(XMM_RSQRT, 1.0f / sqrtf(b.f[i]))
RECOMP_XMM_LANEWISE(XMM_RCP,   1.0f / b.f[i])

/** movmskps: the four lane sign bits, packed into the low nibble. */
static inline uint32_t XMM_MOVEMASK(RECOMP_XMM_IN a) {
    return ((a.u[0] >> 31) & 1u) | (((a.u[1] >> 31) & 1u) << 1)
         | (((a.u[2] >> 31) & 1u) << 2) | (((a.u[3] >> 31) & 1u) << 3);
}

/** shufps: lanes 0-1 selected out of `a`, lanes 2-3 out of `b`. */
static inline RecompXmm XMM_SHUFFLE(RECOMP_XMM_IN a, RECOMP_XMM_IN b, uint32_t imm) {
    RecompXmm r;
    r.u[0] = a.u[(imm >> 0) & 3u]; r.u[1] = a.u[(imm >> 2) & 3u];
    r.u[2] = b.u[(imm >> 4) & 3u]; r.u[3] = b.u[(imm >> 6) & 3u];
    return r;
}

/** unpcklps / unpckhps: interleave the low or high halves. */
static inline RecompXmm XMM_UNPACK_LOW(RECOMP_XMM_IN a, RECOMP_XMM_IN b) {
    RecompXmm r;
    r.u[0] = a.u[0]; r.u[1] = b.u[0]; r.u[2] = a.u[1]; r.u[3] = b.u[1];
    return r;
}

static inline RecompXmm XMM_UNPACK_HIGH(RECOMP_XMM_IN a, RECOMP_XMM_IN b) {
    RecompXmm r;
    r.u[0] = a.u[2]; r.u[1] = b.u[2]; r.u[2] = a.u[3]; r.u[3] = b.u[3];
    return r;
}

/* ================================================================
 * Flag computation helpers
 *
 * These macros compute x86 flags for conditional branches.
 * Used by the lifter's pattern-matching output:
 *   cmp a, b; jcc target  ->  if (COND(a, b)) goto target;
 * ================================================================ */

/* Unsigned comparison conditions (from CMP a, b -> a - b) */
#define CMP_EQ(a, b)  ((uint32_t)(a) == (uint32_t)(b))
#define CMP_NE(a, b)  ((uint32_t)(a) != (uint32_t)(b))
#define CMP_B(a, b)   ((uint32_t)(a) <  (uint32_t)(b))   /* below (CF=1) */
#define CMP_AE(a, b)  ((uint32_t)(a) >= (uint32_t)(b))   /* above or equal */
#define CMP_BE(a, b)  ((uint32_t)(a) <= (uint32_t)(b))   /* below or equal */
#define CMP_A(a, b)   ((uint32_t)(a) >  (uint32_t)(b))   /* above */

/* Signed comparison conditions */
/* x86 evaluates the signed conditions at the operand width, not at 32 bits.
   The generated code passes LO8/HI8/LO16 sub-register reads straight in, and
   those zero-extend, so recover the width and sign-extend before comparing. */
#define RECOMP_FLAG_WIDTH(a, b) (sizeof(a) < sizeof(b) ? sizeof(a) : sizeof(b))
#define RECOMP_SIGNED(value, width) \
    ((width) == 1u ? (int32_t)(int8_t)(uint8_t)(uint32_t)(value) \
     : (width) == 2u ? (int32_t)(int16_t)(uint16_t)(uint32_t)(value) \
     : (int32_t)(uint32_t)(value))
#define CMP_L(a, b)   (RECOMP_SIGNED(a, RECOMP_FLAG_WIDTH(a, b)) <  \
                       RECOMP_SIGNED(b, RECOMP_FLAG_WIDTH(a, b)))  /* less */
#define CMP_GE(a, b)  (RECOMP_SIGNED(a, RECOMP_FLAG_WIDTH(a, b)) >= \
                       RECOMP_SIGNED(b, RECOMP_FLAG_WIDTH(a, b)))  /* >= */
#define CMP_LE(a, b)  (RECOMP_SIGNED(a, RECOMP_FLAG_WIDTH(a, b)) <= \
                       RECOMP_SIGNED(b, RECOMP_FLAG_WIDTH(a, b)))  /* <= */
#define CMP_G(a, b)   (RECOMP_SIGNED(a, RECOMP_FLAG_WIDTH(a, b)) >  \
                       RECOMP_SIGNED(b, RECOMP_FLAG_WIDTH(a, b)))  /* > */

/* TEST-based conditions (AND without storing result) */
#define TEST_Z(a, b)  (((uint32_t)(a) & (uint32_t)(b)) == 0)  /* ZF=1 */
#define TEST_NZ(a, b) (((uint32_t)(a) & (uint32_t)(b)) != 0)  /* ZF=0 */
#define TEST_S(a, b)  (RECOMP_SIGNED((uint32_t)(a) & (uint32_t)(b), \
                                     RECOMP_FLAG_WIDTH(a, b)) < 0)  /* SF=1 */

/* ================================================================
 * Arithmetic with carry/overflow detection
 * ================================================================ */

/** Add with carry flag. Returns result, sets *cf. */
static inline uint32_t ADD32_CF(uint32_t a, uint32_t b, int *cf) {
    uint32_t r = a + b;
    *cf = (r < a);
    return r;
}

/** Sub with carry (borrow) flag. Returns result, sets *cf. */
static inline uint32_t SUB32_CF(uint32_t a, uint32_t b, int *cf) {
    *cf = (a < b);
    return a - b;
}

/* ================================================================
 * Rotation / shift helpers
 * ================================================================ */

/* x86 masks the rotate count to 5 bits, and THEN the rotate is modulo the
 * operand's own width -- so `rol al, 16` is a rotate by zero and `rol ax, 31`
 * is a rotate by 15. A narrow rotate performed at 32 bits is not a rotate at
 * all: the bits that should wrap around at bit 7 or 15 land above the operand
 * and are discarded by the store, which turns `ror al, 2` on 0x01 into 0x00
 * where x86 gives 0x40.
 *
 * The zero case is separated out because `val >> (32 - 0)` is a shift of a
 * uint32_t by 32, which is undefined behaviour -- it happened to survive
 * because x86 masks shift counts to 5 bits and gives back `val`, but the
 * compiler is under no obligation to agree, least of all at -O2. */
static inline uint32_t ROL32(uint32_t val, int n) {
    n &= 31;
    return n ? ((val << n) | (val >> (32 - n))) : val;
}

static inline uint32_t ROR32(uint32_t val, int n) {
    n &= 31;
    return n ? ((val >> n) | (val << (32 - n))) : val;
}

/* rcl/rcr: rotate through the carry flag.
 *
 * The carry is a bit sitting one place above the operand's top bit, so the
 * rotation is over width+1 bits. That is also why a count is reduced modulo
 * width+1 for the 8- and 16-bit forms rather than modulo the width: a byte
 * rotates through nine positions, not eight. The 32-bit form takes the count
 * masked to five bits and no further, which is already inside 33.
 *
 * `cf` carries in and out.
 */
static inline uint32_t RC_ROT(uint32_t val, unsigned n, int *cf,
                              unsigned width, int left) {
    unsigned mod = width + 1u;
    uint64_t mask = (width >= 32u) ? 0xFFFFFFFFull
                                   : ((((uint64_t)1 << width) - 1u));
    uint64_t x = (((uint64_t)(*cf & 1)) << width) | ((uint64_t)val & mask);

    n &= 31u;
    if (width < 32u)
        n %= mod;
    if (n) {
        uint64_t full = (((uint64_t)1 << mod) - 1u);
        x = left ? ((x << n) | (x >> (mod - n)))
                 : ((x >> n) | (x << (mod - n)));
        x &= full;
    }
    *cf = (int)((x >> width) & 1);
    return (uint32_t)(x & mask);
}

static inline uint8_t ROL8(uint8_t val, int n) {
    n = (n & 31) % 8;
    return n ? (uint8_t)((val << n) | (val >> (8 - n))) : val;
}

static inline uint8_t ROR8(uint8_t val, int n) {
    n = (n & 31) % 8;
    return n ? (uint8_t)((val >> n) | (val << (8 - n))) : val;
}

static inline uint16_t ROL16(uint16_t val, int n) {
    n = (n & 31) % 16;
    return n ? (uint16_t)((val << n) | (val >> (16 - n))) : val;
}

static inline uint16_t ROR16(uint16_t val, int n) {
    n = (n & 31) % 16;
    return n ? (uint16_t)((val >> n) | (val << (16 - n))) : val;
}

/* ================================================================
 * Sign/zero extension
 * ================================================================ */

#define ZX8(v)   ((uint32_t)(uint8_t)(v))
#define ZX16(v)  ((uint32_t)(uint16_t)(v))
#define SX8(v)   ((uint32_t)(int32_t)(int8_t)(v))
#define SX16(v)  ((uint32_t)(int32_t)(int16_t)(v))

/* ================================================================
 * Byte/word register access
 *
 * These macros extract or set partial registers, matching x86
 * behavior where writing AL doesn't affect bits 8-31 of EAX.
 * ================================================================ */

/** Extract low byte (al, bl, cl, dl). */
#define LO8(r)  ((uint8_t)((r) & 0xFF))
/** Extract high byte of low word (ah, bh, ch, dh). */
#define HI8(r)  ((uint8_t)(((r) >> 8) & 0xFF))
/** Extract low word (ax, bx, cx, dx). */
#define LO16(r) ((uint16_t)((r) & 0xFFFF))

/** Set low byte, preserving upper 24 bits. */
#define SET_LO8(r, v)  ((r) = ((r) & 0xFFFFFF00u) | ((uint32_t)(uint8_t)(v)))
/** Set high byte of low word, preserving other bits. */
#define SET_HI8(r, v)  ((r) = ((r) & 0xFFFF00FFu) | (((uint32_t)(uint8_t)(v)) << 8))
/** Set low word, preserving upper 16 bits. */
#define SET_LO16(r, v) ((r) = ((r) & 0xFFFF0000u) | ((uint32_t)(uint16_t)(v)))

/* ================================================================
 * Stack simulation
 *
 * For push/pop heavy prologues in the generated code.
 * ================================================================ */

/**
 * Push a 32-bit value onto the simulated stack.
 * Evaluates val BEFORE decrementing sp, matching x86 semantics
 * where push [esp+N] reads the operand before adjusting ESP.
 */
#define PUSH32(sp, val) do { \
    uint32_t _pv = (uint32_t)(val); \
    (sp) -= 4; \
    MEM32(sp) = _pv; \
} while(0)

/**
 * x86 parity flag: 1 when the low byte of the result has an EVEN number of set
 * bits (that is what PF means). Used by the x87 float-compare idiom
 * `fnstsw ax; test ah, mask; jp/jnp`, which is how all pre-SSE code branches on
 * a float comparison. Without a real parity here the branch was hardcoded and
 * every such comparison went one fixed direction.
 */
static inline int recomp_parity8(uint32_t x) {
    x &= 0xFFu; x ^= x >> 4; x ^= x >> 2; x ^= x >> 1;
    return (int)(~x & 1u);   /* 1 = even parity (PF set) */
}
#define RECOMP_PARITY8(x) recomp_parity8((uint32_t)(x))

/** Pop a 32-bit value from the simulated stack. */
#define POP32(sp, dst) do { \
    (dst) = MEM32(sp); \
    (sp) += 4; \
} while(0)

/* ================================================================
 * Byte swap (for endian conversion if needed)
 *
 * Xbox is little-endian like x86, so these are rarely needed,
 * but some games use bswap for network byte order or data parsing.
 * ================================================================ */

static inline uint32_t BSWAP32(uint32_t v) {
    return ((v >> 24) & 0xFF) | ((v >> 8) & 0xFF00) |
           ((v << 8) & 0xFF0000) | ((v << 24) & 0xFF000000u);
}

static inline uint16_t BSWAP16(uint16_t v) {
    return (uint16_t)((v >> 8) | (v << 8));
}

/* ================================================================
 * Indirect call dispatch
 *
 * The dispatch system resolves Xbox virtual addresses to native
 * function pointers at runtime. Three lookup sources are checked:
 *   1. Manual overrides (hand-written reimplementations)
 *   2. Generated dispatch table (auto-recompiled functions)
 *   3. Kernel thunk bridge (Xbox kernel function replacements)
 * ================================================================ */

/**
 * Generic function pointer type for all recompiled functions.
 * All translated functions are void(void) - arguments and return
 * values are passed through global registers and the simulated stack.
 */
#ifndef RECOMP_DISPATCH_H  /* avoid conflict with recomp_dispatch.h */
typedef void (*recomp_func_t)(void);

/**
 * Look up a recompiled function by its Xbox VA.
 * Returns NULL if the VA is not in the generated dispatch table.
 */
recomp_func_t recomp_lookup(uint32_t xbox_va);

/**
 * Build the flat, directly-indexed dispatch table.
 *
 * Turns recomp_lookup from a binary search over every translated function into
 * a bounds check and one indexed load -- the C form of Microsoft's
 * `jmp [table + guest_eip*8]`. Call it once at startup, before any recompiled
 * code runs.
 *
 * Entirely optional: if it is never called, or returns 0 because the allocation
 * failed, recomp_lookup keeps using the binary search and everything still
 * works. Costs 8 bytes per byte of guest code span (see
 * recomp_dispatch_flat_bytes), allocated with calloc so the untouched middle
 * stays uncommitted.
 *
 * Returns 1 if the flat table is in use, 0 if the search is.
 */
int recomp_dispatch_init(void);

/** Bytes held by the flat table, or 0 if it was never built. */
size_t recomp_dispatch_flat_bytes(void);

/**
 * Look up a kernel thunk function by its synthetic VA.
 * Kernel thunks live at 0xFE000000+ (synthetic addresses assigned
 * during kernel bridge initialization).
 * Returns NULL if the VA is not a kernel thunk.
 */
recomp_func_t recomp_lookup_kernel(uint32_t xbox_va);

/**
 * Look up a manually overridden function by its Xbox VA.
 * Manual overrides take priority over generated code.
 * Returns NULL if no manual override exists for this VA.
 */
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
#endif

/**
 * RECOMP_ABI_CALL - call a resolved target, optionally checking the ABI.
 *
 * ebx, esi and edi are callee-saved on x86, and the recompiler keeps them in
 * globals rather than on the host's stack. So a lifted function that never
 * reaches its own epilogue -- the usual cause is a decode that lost sync on
 * embedded data -- does not merely lose its own state, it silently corrupts
 * every caller's.
 *
 * That failure is invisible at the crash site: the caller carries on with a
 * wrong loop cursor and simply does less work. Half-Life 2's C++ static
 * initialiser walks 5,305 constructors with esi as the cursor and edi as the
 * limit; a single callee returning with those changed ended the walk early and
 * left Source with no registered interfaces, with no error anywhere.
 *
 * Build with -DRECOMP_ABI_CHECK to have every indirect call verify them and
 * name the first offenders. esp is deliberately not checked: the convention
 * decides whether the callee pops arguments, so there is no single correct
 * value -- but there is one invariant that holds under every convention:
 * the callee at least pops its own return address, so esp must come back
 * at least 4 higher than it went in. Coming back lower means the epilogue
 * never ran, which is the failure this exists to catch.
 *
 * Covers direct calls as well as indirect: tools/recomp emits every direct
 * call through this macro, which expands to a plain call when the flag is
 * off. That matters because CRT and static-initialiser paths -- where these
 * clobbers actually bite -- are almost entirely direct calls.
 * Generated code emits a direct call as a plain C call to the symbol, with no
 * macro to hook, so a direct callee that clobbers these registers is invisible
 * here. That matters more than it sounds -- CRT and static-initialiser paths
 * are almost entirely direct calls, so this found nothing at all on Half-Life
 * 2's static init, where the clobber demonstrably exists. It is the right tool
 * for vtable-dispatch-heavy code and the wrong one for early boot.
 */
#ifdef RECOMP_ABI_CHECK
void recomp_abi_violation_log(uint32_t va, uint32_t ebx0, uint32_t esi0,
                              uint32_t edi0, uint32_t esp0);
#define RECOMP_ABI_CALL(va, fn) do { \
    uint32_t _ab = g_ebx, _as = g_esi, _ad = g_edi, _ap = g_esp; \
    (fn)(); \
    if (g_ebx != _ab || g_esi != _as || g_edi != _ad || g_esp < _ap + 4) \
        recomp_abi_violation_log((va), _ab, _as, _ad, _ap); \
} while(0)
#else
#define RECOMP_ABI_CALL(va, fn) (fn)()
#endif

/* A guarded arm of an indirect call. When a recorded run saw a call site
 * reach only a few translated functions, the lifter compares the target
 * against each and calls the match directly (RECOMP_ABI_CALL), falling back
 * to RECOMP_ICALL_SAFE_AT for anything else -- so an unseen target is slow,
 * never wrong. Under RECOMP_ICALL_FEEDBACK the arms are counted, so a run
 * can say how often the guards held; otherwise they cost nothing. */
#ifdef RECOMP_ICALL_FEEDBACK
extern volatile uint64_t g_icall_guard_hits;
extern volatile uint64_t g_icall_guard_misses;
#define RECOMP_ICALL_GUARD_HIT()  ((void)g_icall_guard_hits++)
#define RECOMP_ICALL_GUARD_MISS() ((void)g_icall_guard_misses++)
#else
#define RECOMP_ICALL_GUARD_HIT()  ((void)0)
#define RECOMP_ICALL_GUARD_MISS() ((void)0)
#endif

/**
 * RECOMP_ICALL - Indirect call through the dispatch table.
 *
 * Looks up the Xbox VA and calls the translated function.
 * Falls back to kernel bridge for kernel thunk synthetic VAs.
 * The caller must PUSH32 the guest return address before this macro.
 * If not found, pops it back off to keep the stack balanced.
 *
 * RECOMP_ICALL_IS_CODE skips garbage VAs that come from uninitialized vtable
 * pointers. Kernel thunks at 0xFE000000+ are never blocked.
 *
 * Nothing to customize: the range comes from g_xbox_code_lo/g_xbox_code_hi,
 * which the memory layout fills in from the sections it actually mapped. This
 * used to say "change the VA range check to match your game's code range",
 * left over from a hardcoded 0x00400000 cutoff that was only ever right for
 * one title. Editing this header per project is no longer a thing.
 */
#define RECOMP_ICALL(xbox_va) do { \
    uint32_t _va = (uint32_t)(xbox_va); \
    g_icall_trace[g_icall_trace_idx & (ICALL_TRACE_SIZE-1)] = _va; \
    g_icall_trace_idx++; \
    g_icall_count++; \
    /* Skip garbage VAs outside code section + kernel thunk range */ \
    if (!RECOMP_ICALL_IS_CODE(_va)) { \
        recomp_icall_not_code_log(_va); \
        g_esp += 4; g_eax = 0; break; \
    } \
    recomp_func_t _fn = recomp_lookup_manual(_va); \
    if (!_fn) _fn = recomp_lookup(_va); \
    if (!_fn) _fn = recomp_lookup_kernel(_va); \
    if (_fn) { RECOMP_ICALL_OBSERVE(_va, RECOMP_ICALL_SEEN_RESOLVED); \
               RECOMP_ABI_CALL(_va, _fn); } \
    else { RECOMP_ICALL_OBSERVE(_va, RECOMP_ICALL_SEEN_UNRESOLVED); \
           recomp_icall_fail_log(_va); g_esp += 4; g_eax = 0; } \
} while(0)

/**
 * RECOMP_ICALL_SAFE - Stack-safe indirect call.
 *
 * Restores g_esp to saved_esp (pre-argument value) on lookup failure,
 * preventing stdcall argument leaks on failed vtable calls.
 * Use this when the caller pushes arguments that the callee would
 * normally clean up (stdcall convention).
 */
#define RECOMP_ICALL_SAFE(xbox_va, saved_esp) do { \
    uint32_t _va = (uint32_t)(xbox_va); \
    g_icall_trace[g_icall_trace_idx & (ICALL_TRACE_SIZE-1)] = _va; \
    g_icall_trace_idx++; \
    g_icall_count++; \
    if (!RECOMP_ICALL_IS_CODE(_va)) { \
        recomp_icall_not_code_log(_va); \
        g_esp = (saved_esp); g_eax = 0; break; \
    } \
    recomp_func_t _fn = recomp_lookup_manual(_va); \
    if (!_fn) _fn = recomp_lookup(_va); \
    if (!_fn) _fn = recomp_lookup_kernel(_va); \
    if (_fn) { RECOMP_ICALL_OBSERVE(_va, RECOMP_ICALL_SEEN_RESOLVED); \
               RECOMP_ABI_CALL(_va, _fn); } \
    else { RECOMP_ICALL_OBSERVE(_va, RECOMP_ICALL_SEEN_UNRESOLVED); \
           recomp_icall_fail_log(_va); g_esp = (saved_esp); g_eax = 0; } \
} while(0)

/**
 * RECOMP_ICALL_SAFE_AT - RECOMP_ICALL_SAFE that also names the call SITE.
 *
 * `site` is the guest address of the `call` instruction. Under
 * RECOMP_ICALL_FEEDBACK the runtime records which targets each site reaches
 * (icall_sites.dump), which is what lets the lifter guard a site with direct
 * calls on the next generation. Everything else is RECOMP_ICALL_SAFE.
 */
#define RECOMP_ICALL_SAFE_AT(xbox_va, saved_esp, site) do { \
    uint32_t _va = (uint32_t)(xbox_va); \
    g_icall_trace[g_icall_trace_idx & (ICALL_TRACE_SIZE-1)] = _va; \
    g_icall_trace_idx++; \
    g_icall_count++; \
    if (!RECOMP_ICALL_IS_CODE(_va)) { \
        recomp_icall_not_code_log(_va); \
        g_esp = (saved_esp); g_eax = 0; break; \
    } \
    RECOMP_ICALL_OBSERVE_SITE((site), _va); \
    recomp_func_t _fn = recomp_lookup_manual(_va); \
    if (!_fn) _fn = recomp_lookup(_va); \
    if (!_fn) _fn = recomp_lookup_kernel(_va); \
    if (_fn) { RECOMP_ICALL_OBSERVE(_va, RECOMP_ICALL_SEEN_RESOLVED); \
               RECOMP_ABI_CALL(_va, _fn); } \
    else { RECOMP_ICALL_OBSERVE(_va, RECOMP_ICALL_SEEN_UNRESOLVED); \
           recomp_icall_fail_log(_va); g_esp = (saved_esp); g_eax = 0; } \
} while(0)

/**
 * RECOMP_ITAIL - Indirect tail call (jmp through function pointer).
 *
 * No return address is pushed - reuses the current frame's return addr.
 * Used for tail-call optimization where the original code uses
 * jmp [reg] instead of call [reg].
 */
#define RECOMP_ITAIL(xbox_va) do { \
    uint32_t _va = (uint32_t)(xbox_va); \
    recomp_func_t _fn = recomp_lookup_manual(_va); \
    if (!_fn) _fn = recomp_lookup(_va); \
    if (!_fn) _fn = recomp_lookup_kernel(_va); \
    if (_fn) { RECOMP_ICALL_OBSERVE(_va, RECOMP_ICALL_SEEN_RESOLVED); \
               RECOMP_ABI_CALL(_va, _fn); } \
    else { RECOMP_ICALL_OBSERVE(_va, RECOMP_ICALL_SEEN_UNRESOLVED); \
           recomp_icall_fail_log(_va); g_esp += 4; g_eax = 0; } \
} while(0)

/* ================================================================
 * Register name aliases for generated code
 *
 * Map x86 volatile register names to global variables.
 * These #defines allow the generated code to use natural register
 * names (eax, ecx, edx, esp) which the preprocessor maps to the
 * corresponding globals (g_eax, g_ecx, g_edx, g_esp).
 *
 * Only active when RECOMP_GENERATED_CODE is defined (in generated
 * .c files) to avoid polluting hand-written code.
 *
 * Register caching (tools.recomp --reg-cache). A chunk generated with it
 * defines RECOMP_REG_CACHE before including this header, and then each
 * function keeps the registers it uses in C locals rc_eax.., rc_mm0..,
 * rc_xmm0..: the names below map there instead of to the TLS globals.
 * Every TLS access is a load or store through the thread pointer, and on a
 * host without store forwarding (Xenon) the compiler has to reload them
 * after every guest store, because a store through a pointer may alias a
 * global. A local whose address is never taken stays in a host register.
 *
 * The lifter declares the locals at function entry (filled from the
 * globals: RC_DECL_*), writes them back before anything that can read the
 * globals (RC_SPILL: calls, indirect calls, tail calls, returns, traces) and
 * reloads them after a call returns (RC_FILL). RC_SPILL and RC_FILL are
 * #defined per function, just before it, to list exactly the registers that
 * function caches, and #undef'd after it.
 *
 * There is no global named rc_*, so a use the lifter did not declare is a
 * compile error, never a silent read of a stale value. -DRECOMP_REG_CACHE_OFF
 * compiles the same generated code with the globals, as without the flag.
 *
 * Header macros must never name a register bare (eax, esp): under the cache
 * that would be the calling function's local. They use g_eax, g_esp.
 * ================================================================ */

#ifdef RECOMP_GENERATED_CODE
#define RECOMP_REG_CACHE_SUPPORTED 1
#if defined(RECOMP_REG_CACHE) && !defined(RECOMP_REG_CACHE_OFF)
#define RC_ACTIVE 1
#define eax rc_eax
#define ecx rc_ecx
#define edx rc_edx
#define esp rc_esp
#define ebx rc_ebx
#define esi rc_esi
#define edi rc_edi
/* Operands of ## are not macro-expanded, so RC_S(eax) is always
 * `g_eax = rc_eax;` whatever eax maps to. */
#define RC_DECL_GPR(r) uint32_t  rc_##r = g_##r;
#define RC_DECL_MMX(r) RecompMmx rc_##r = g_##r;
#define RC_DECL_XMM(r) RecompXmm rc_##r = g_##r;
#define RC_S(r) g_##r = rc_##r;
#define RC_L(r) rc_##r = g_##r;
/* The spin wait may run interrupt service routines on this thread's
 * registers (the desktop kernel_hal does), so the globals must be current
 * around it. It only calls out while the thread sleeps or an IRQ waits. */
#undef RECOMP_SPIN_WAIT
#define RECOMP_SPIN_WAIT(va) \
    do { if (g_xbox_spin_sleep | g_xbox_irq_pending) { \
        RC_SPILL(); xbox_SpinWait(va); RC_FILL(); } } while (0)
#else
#define eax g_eax
#define ecx g_ecx
#define edx g_edx
#define esp g_esp
#define ebx g_ebx
#define esi g_esi
#define edi g_edi
#define RC_DECL_GPR(r)
#define RC_DECL_MMX(r)
#define RC_DECL_XMM(r)
#define RC_S(r)
#define RC_L(r)
#endif

/* ================================================================
 * MMX register file
 *
 * The Xbox is a Pentium III and every XDK codec leans on MMX: the WMV
 * decoder's IDCT and motion compensation are almost nothing else. Modelled the
 * same way as RecompXmm -- a union of lane views over one 64-bit register --
 * because that is what the instructions are: the same bits read as bytes,
 * words or dwords.
 *
 * mm0..mm7 alias the x87 stack on real hardware. Nothing here does, and
 * nothing needs to: a title that interleaves the two calls emms between, and
 * emms is a no-op for us. Modelling the aliasing would mean giving up the
 * separate x87 model that the FPU work depends on, to reproduce a hazard the
 * hardware exists to let software avoid.
 * ================================================================ */

#ifndef RECOMP_MMX_DEFINED
#define RECOMP_MMX_DEFINED
typedef union RecompMmx {
    int8_t   b[8];
    uint8_t  ub[8];
    int16_t  w[4];
    uint16_t uw[4];
    int32_t  d[2];
    uint32_t ud[2];
    uint64_t q;
} RecompMmx;
#endif

extern RECOMP_TLS RecompMmx g_mm0, g_mm1, g_mm2, g_mm3;
extern RECOMP_TLS RecompMmx g_mm4, g_mm5, g_mm6, g_mm7;

/* MMX lane invariant: `q` is the register's 64-bit value as x86 sees it
 * (bit 0 is bit 0 of byte lane 0), on every host. MMX_MEM/MMX_STORE move q
 * through a 64-bit little-endian guest access, and psllq/psrlq/pand/... work
 * on q directly. The narrower views are reached ONLY through these macros,
 * which turn an x86 lane number into the union index holding that lane: the
 * same index on a little-endian host, the mirrored one on a big-endian host
 * (see RECOMP_HOST_BIG_ENDIAN above). Generated code uses them too -- movd
 * out of an mm register is RECOMP_MMX_UD(mmN, 0), never mmN.ud[0]. */
#if RECOMP_HOST_BIG_ENDIAN
#define RECOMP_MMX_IX(lanes, i) ((lanes) - 1 - (i))
#else
#define RECOMP_MMX_IX(lanes, i) (i)
#endif
#define RECOMP_MMX_B(r, i)  ((r).b[RECOMP_MMX_IX(8, (i))])
#define RECOMP_MMX_UB(r, i) ((r).ub[RECOMP_MMX_IX(8, (i))])
#define RECOMP_MMX_W(r, i)  ((r).w[RECOMP_MMX_IX(4, (i))])
#define RECOMP_MMX_UW(r, i) ((r).uw[RECOMP_MMX_IX(4, (i))])
#define RECOMP_MMX_D(r, i)  ((r).d[RECOMP_MMX_IX(2, (i))])
#define RECOMP_MMX_UD(r, i) ((r).ud[RECOMP_MMX_IX(2, (i))])

static inline RecompMmx MMX_ZERO(void) { RecompMmx r; r.q = 0; return r; }

/* CVTPS2PI follows MXCSR; CVTTPS2PI truncates regardless of its rounding mode.
 * Use SSE scalar conversions to avoid touching the host x87/MMX register file.
 * Non-x86 hosts use their floating-point environment for rounding instead. */
#if defined(_XBOX) && !defined(_M_IX86) && !defined(_M_X64)
/* Xbox 360: fctiw/fctiwz in the FPR, inline. fctiw rounds by FPSCR[RN],
 * which is nearest-even on this host and never changed (the game host
 * checks it at start-up). PPC saturates +overflow to 0x7FFFFFFF where x86
 * returns 0x80000000 for every invalid case, so values >= 2^31 are steered
 * to -2^31 first with fsel (NaN and -overflow already give 0x80000000).
 * The integer leaves the FPR with stfiwx: one load-hit-store, no compares
 * or branches (the generic path below was a call per lane with nine FP
 * compares, and the XMV player's colour converter runs 12 per 4 pixels). */
RECOMP_HOT_INLINE double recomp_f2i_fpr(float v, int truncate)
{
    double d = (double)v;
    d = __fsel(d - 2147483648.0, -2147483648.0, d);
    return truncate ? __fctiwz(d) : __fctiw(d);
}
RECOMP_HOT_INLINE int32_t MMX_CVT_F2I(float v, int truncate)
{
    __declspec(align(8)) int32_t t[2];
    __stfiwx(recomp_f2i_fpr(v, truncate), 0, t);
    return t[0];
}
#else
static inline int32_t MMX_CVT_F2I(float v, int truncate)
{
#if defined(_M_IX86) || defined(_M_X64) || defined(__i386__) || defined(__x86_64__)
    return truncate ? _mm_cvttss_si32(_mm_set_ss(v))
                    : _mm_cvtss_si32(_mm_set_ss(v));
#else
    /* recomp_x87_round with RC=nearest-even (the host default, which the old
     * nearbyint() followed) or RC=chop: same results as trunc()/nearbyint(),
     * without their CRT calls (_finite, _copysign, floor/ceil on the Xbox
     * 360 shim), which cost more than the rest of a cvtps2pi. */
    double rounded = recomp_x87_round((double)v, truncate ? 0x0C00u : 0u);
    if (!(rounded >= -2147483648.0 && rounded <= 2147483647.0))
        return (int32_t)0x80000000u;       /* integer indefinite */
    return (int32_t)rounded;
#endif
}
#endif

/* MMX lane access by shifting `q`, not through the union views.
 *
 * On the Xbox 360 the union views cost far more than the lane arithmetic:
 * the compiler spills the 64-bit argument to the stack and reads one lane at
 * a time back (std, then lbz/lhz from the same doubleword), and builds the
 * result with byte or halfword stores followed by one 64-bit load. Xenon has
 * no store forwarding, so each of those is a load-hit-store stall of about
 * 40 cycles, two or more per MMX instruction. With the lanes taken out of
 * `q` by shifts and put back with shifts and ors, the helpers stay in
 * registers.
 *
 * The results are the same on every host -- `q` is the x86 view everywhere
 * -- so this is on by default only where it pays (big-endian hosts); the
 * PC lane test builds it with /DRECOMP_MMX_SHIFT_LANES=1 as well and checks
 * it against the CPU's own MMX instructions. */
#ifndef RECOMP_MMX_SHIFT_LANES
#define RECOMP_MMX_SHIFT_LANES RECOMP_HOST_BIG_ENDIAN
#endif
#if RECOMP_MMX_SHIFT_LANES
#define RECOMP_MQ_U8(q, i)  ((uint32_t)(uint8_t)((q) >> ((i) * 8)))
#define RECOMP_MQ_S8(q, i)  ((int32_t)(int8_t)(uint8_t)((q) >> ((i) * 8)))
#define RECOMP_MQ_U16(q, i) ((uint32_t)(uint16_t)((q) >> ((i) * 16)))
#define RECOMP_MQ_S16(q, i) ((int32_t)(int16_t)(uint16_t)((q) >> ((i) * 16)))
#define RECOMP_MQ_U32(q, i) ((uint32_t)((q) >> ((i) * 32)))
#define RECOMP_MQ_S32(q, i) ((int32_t)(uint32_t)((q) >> ((i) * 32)))
/* Forced inline: with plain `inline` the Xbox 360 compiler (2010-01) dies
 * with C1001 on some of the XMV player's MMX functions (pxor of a register
 * with itself feeding packuswb, in sub_002D16CF); inlined up front they
 * compile, and they are a few instructions each anyway. */
#if defined(_MSC_VER)
#define RECOMP_MQ_FN static __forceinline
#elif defined(__GNUC__)
#define RECOMP_MQ_FN static inline __attribute__((always_inline))
#else
#define RECOMP_MQ_FN static inline
#endif
RECOMP_MQ_FN RecompMmx recomp_mmx_q(uint64_t q) { RecompMmx r; r.q = q; return r; }
#endif

#if defined(_XBOX) && !defined(_M_IX86) && !defined(_M_X64)
/* Both lanes leave the FPRs through one 8-byte slot: two stfiwx, one ld,
 * one load-hit-store instead of two. Big-endian: the high word is first. */
RECOMP_HOT_INLINE RecompMmx MMX_FROM_PS(float lo, float hi, int truncate)
{
    __declspec(align(8)) uint64_t t;
    __stfiwx(recomp_f2i_fpr(hi, truncate), 0, &t);
    __stfiwx(recomp_f2i_fpr(lo, truncate), 4, &t);
    return recomp_mmx_q(t);
}
#else
static inline RecompMmx MMX_FROM_PS(float lo, float hi, int truncate)
{
#if RECOMP_MMX_SHIFT_LANES
    return recomp_mmx_q((uint64_t)(uint32_t)MMX_CVT_F2I(lo, truncate)
                        | ((uint64_t)(uint32_t)MMX_CVT_F2I(hi, truncate) << 32));
#else
    RecompMmx r;
    RECOMP_MMX_D(r, 0) = MMX_CVT_F2I(lo, truncate);
    RECOMP_MMX_D(r, 1) = MMX_CVT_F2I(hi, truncate);
    return r;
#endif
}
#endif

/** cvtpi2ps: two signed dwords in, two singles out, into the LOW half of the
 * destination -- lanes 2 and 3 keep whatever they held. That detail is the
 * whole instruction: code that builds a float4 from two of these relies on
 * the first one surviving the second. */
static inline RecompXmm XMM_FROM_PI(RecompXmm dst, RecompMmx src)
{
#if RECOMP_MMX_SHIFT_LANES
    dst.f[0] = (float)RECOMP_MQ_S32(src.q, 0);
    dst.f[1] = (float)RECOMP_MQ_S32(src.q, 1);
#else
    dst.f[0] = (float)RECOMP_MMX_D(src, 0);
    dst.f[1] = (float)RECOMP_MMX_D(src, 1);
#endif
    return dst;
}

static inline RecompMmx MMX_MEM(uint32_t addr) {
    RecompMmx r;
#if defined(RECOMP_EXPLICIT_GUEST_BUS)
    r.q = XrGuestRef<uint64_t>(addr);
#else
    memcpy(&r, (const void *)XBOX_PTR(addr), 8);
#endif
    return r;
}

static inline void MMX_STORE(uint32_t addr, RecompMmx v) {
#if defined(RECOMP_EXPLICIT_GUEST_BUS)
    (XrGuestRef<uint64_t>(addr)) = v.q;
#else
    memcpy((void *)XBOX_PTR(addr), &v, 8);
#endif
}

/** movd mm, r/m32: the dword into lane 0, lane 1 cleared. */
static inline RecompMmx MMX_FROM32(uint32_t v) {
    RecompMmx r; r.q = (uint64_t)v; return r;
}

/* -- saturation helpers ---------------------------------------- */
static inline int16_t recomp_sat_i16(int32_t v) {
    return (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
}
static inline int8_t recomp_sat_i8(int32_t v) {
    return (int8_t)(v > 127 ? 127 : (v < -128 ? -128 : v));
}
static inline uint8_t recomp_sat_u8(int32_t v) {
    return (uint8_t)(v > 255 ? 255 : (v < 0 ? 0 : v));
}

static inline uint16_t recomp_sat_u16(int32_t v) {
    return (uint16_t)(v > 65535 ? 65535 : (v < 0 ? 0 : v));
}

#if !RECOMP_MMX_SHIFT_LANES
/* -- integer arithmetic, lane-wise, wrapping --------------------
 * LANE is one of the RECOMP_MMX_* accessors; EXPR reads lane i of a and b
 * through the same accessors. */
#define RECOMP_MMX_BINOP(NAME, LANES, LANE, EXPR)                       \
    static inline RecompMmx NAME(RecompMmx a, RecompMmx b) {            \
        RecompMmx r; int i;                                             \
        for (i = 0; i < (LANES); i++) { (void)b; LANE(r, i) = (EXPR); } \
        return r;                                                       \
    }

RECOMP_MMX_BINOP(MMX_PADDB, 8, RECOMP_MMX_B,
                 (int8_t)((uint8_t)RECOMP_MMX_B(a, i) + (uint8_t)RECOMP_MMX_B(b, i)))
RECOMP_MMX_BINOP(MMX_PADDW, 4, RECOMP_MMX_W,
                 (int16_t)((uint16_t)RECOMP_MMX_W(a, i) + (uint16_t)RECOMP_MMX_W(b, i)))
RECOMP_MMX_BINOP(MMX_PADDD, 2, RECOMP_MMX_D,
                 (int32_t)((uint32_t)RECOMP_MMX_D(a, i) + (uint32_t)RECOMP_MMX_D(b, i)))
RECOMP_MMX_BINOP(MMX_PSUBB, 8, RECOMP_MMX_B,
                 (int8_t)((uint8_t)RECOMP_MMX_B(a, i) - (uint8_t)RECOMP_MMX_B(b, i)))
RECOMP_MMX_BINOP(MMX_PSUBW, 4, RECOMP_MMX_W,
                 (int16_t)((uint16_t)RECOMP_MMX_W(a, i) - (uint16_t)RECOMP_MMX_W(b, i)))
RECOMP_MMX_BINOP(MMX_PSUBD, 2, RECOMP_MMX_D,
                 (int32_t)((uint32_t)RECOMP_MMX_D(a, i) - (uint32_t)RECOMP_MMX_D(b, i)))
RECOMP_MMX_BINOP(MMX_PADDSB, 8, RECOMP_MMX_B,
                 recomp_sat_i8((int32_t)RECOMP_MMX_B(a, i) + RECOMP_MMX_B(b, i)))
RECOMP_MMX_BINOP(MMX_PADDSW, 4, RECOMP_MMX_W,
                 recomp_sat_i16((int32_t)RECOMP_MMX_W(a, i) + RECOMP_MMX_W(b, i)))
RECOMP_MMX_BINOP(MMX_PSUBSB, 8, RECOMP_MMX_B,
                 recomp_sat_i8((int32_t)RECOMP_MMX_B(a, i) - RECOMP_MMX_B(b, i)))
RECOMP_MMX_BINOP(MMX_PSUBSW, 4, RECOMP_MMX_W,
                 recomp_sat_i16((int32_t)RECOMP_MMX_W(a, i) - RECOMP_MMX_W(b, i)))
RECOMP_MMX_BINOP(MMX_PADDUSB, 8, RECOMP_MMX_UB,
                 recomp_sat_u8((int32_t)RECOMP_MMX_UB(a, i) + RECOMP_MMX_UB(b, i)))
RECOMP_MMX_BINOP(MMX_PSUBUSB, 8, RECOMP_MMX_UB,
                 recomp_sat_u8((int32_t)RECOMP_MMX_UB(a, i) - RECOMP_MMX_UB(b, i)))
RECOMP_MMX_BINOP(MMX_PADDUSW, 4, RECOMP_MMX_UW,
                 recomp_sat_u16((int32_t)RECOMP_MMX_UW(a, i) + RECOMP_MMX_UW(b, i)))
RECOMP_MMX_BINOP(MMX_PSUBUSW, 4, RECOMP_MMX_UW,
                 recomp_sat_u16((int32_t)RECOMP_MMX_UW(a, i) - RECOMP_MMX_UW(b, i)))
RECOMP_MMX_BINOP(MMX_PMULLW, 4, RECOMP_MMX_W,
                 (int16_t)((int32_t)RECOMP_MMX_W(a, i) * RECOMP_MMX_W(b, i)))
RECOMP_MMX_BINOP(MMX_PMULHW, 4, RECOMP_MMX_W,
                 (int16_t)(((int32_t)RECOMP_MMX_W(a, i) * RECOMP_MMX_W(b, i)) >> 16))
RECOMP_MMX_BINOP(MMX_PAVGB, 8, RECOMP_MMX_UB,
                 (uint8_t)(((int32_t)RECOMP_MMX_UB(a, i) + RECOMP_MMX_UB(b, i) + 1) >> 1))
RECOMP_MMX_BINOP(MMX_PAVGW, 4, RECOMP_MMX_UW,
                 (uint16_t)(((int32_t)RECOMP_MMX_UW(a, i) + RECOMP_MMX_UW(b, i) + 1) >> 1))
RECOMP_MMX_BINOP(MMX_PMINSW, 4, RECOMP_MMX_W,
                 (RECOMP_MMX_W(a, i) < RECOMP_MMX_W(b, i) ? RECOMP_MMX_W(a, i) : RECOMP_MMX_W(b, i)))
RECOMP_MMX_BINOP(MMX_PMAXSW, 4, RECOMP_MMX_W,
                 (RECOMP_MMX_W(a, i) > RECOMP_MMX_W(b, i) ? RECOMP_MMX_W(a, i) : RECOMP_MMX_W(b, i)))
RECOMP_MMX_BINOP(MMX_PCMPEQB, 8, RECOMP_MMX_B,
                 (int8_t)(RECOMP_MMX_B(a, i) == RECOMP_MMX_B(b, i) ? -1 : 0))
RECOMP_MMX_BINOP(MMX_PCMPEQW, 4, RECOMP_MMX_W,
                 (int16_t)(RECOMP_MMX_W(a, i) == RECOMP_MMX_W(b, i) ? -1 : 0))
RECOMP_MMX_BINOP(MMX_PCMPEQD, 2, RECOMP_MMX_D,
                 (int32_t)(RECOMP_MMX_D(a, i) == RECOMP_MMX_D(b, i) ? -1 : 0))
RECOMP_MMX_BINOP(MMX_PCMPGTB, 8, RECOMP_MMX_B,
                 (int8_t)(RECOMP_MMX_B(a, i) > RECOMP_MMX_B(b, i) ? -1 : 0))
RECOMP_MMX_BINOP(MMX_PCMPGTW, 4, RECOMP_MMX_W,
                 (int16_t)(RECOMP_MMX_W(a, i) > RECOMP_MMX_W(b, i) ? -1 : 0))
RECOMP_MMX_BINOP(MMX_PCMPGTD, 2, RECOMP_MMX_D,
                 (int32_t)(RECOMP_MMX_D(a, i) > RECOMP_MMX_D(b, i) ? -1 : 0))

/* pmaddwd: multiply signed words, add adjacent pairs into dwords. */
static inline RecompMmx MMX_PMADDWD(RecompMmx a, RecompMmx b) {
    RecompMmx r;
    RECOMP_MMX_D(r, 0) = (int32_t)((uint32_t)((int32_t)RECOMP_MMX_W(a, 0) * RECOMP_MMX_W(b, 0))
                       + (uint32_t)((int32_t)RECOMP_MMX_W(a, 1) * RECOMP_MMX_W(b, 1)));
    RECOMP_MMX_D(r, 1) = (int32_t)((uint32_t)((int32_t)RECOMP_MMX_W(a, 2) * RECOMP_MMX_W(b, 2))
                       + (uint32_t)((int32_t)RECOMP_MMX_W(a, 3) * RECOMP_MMX_W(b, 3)));
    return r;
}

/* -- bitwise ---------------------------------------------------- */
static inline RecompMmx MMX_PAND(RecompMmx a, RecompMmx b)  { RecompMmx r; r.q = a.q & b.q; return r; }
static inline RecompMmx MMX_PANDN(RecompMmx a, RecompMmx b) { RecompMmx r; r.q = ~a.q & b.q; return r; }
static inline RecompMmx MMX_POR(RecompMmx a, RecompMmx b)   { RecompMmx r; r.q = a.q | b.q; return r; }
static inline RecompMmx MMX_PXOR(RecompMmx a, RecompMmx b)  { RecompMmx r; r.q = a.q ^ b.q; return r; }

/* -- shifts -----------------------------------------------------
 * A count at or past the lane width gives zero for the logical shifts and a
 * full sign fill for the arithmetic ones. The count is the whole 64-bit
 * register, not one lane, and it is unsigned -- a negative-looking count is a
 * huge one, which saturates the same way.
 */
#define RECOMP_MMX_SHIFT(NAME, LANES, LANE, WIDTH, OP)                  \
    static inline RecompMmx NAME(RecompMmx a, uint64_t cnt) {           \
        RecompMmx r; int i;                                             \
        for (i = 0; i < (LANES); i++)                                   \
            LANE(r, i) = (cnt >= (WIDTH)) ? 0 : (OP);                   \
        return r;                                                       \
    }
RECOMP_MMX_SHIFT(MMX_PSLLW, 4, RECOMP_MMX_UW, 16, (uint16_t)(RECOMP_MMX_UW(a, i) << cnt))
RECOMP_MMX_SHIFT(MMX_PSRLW, 4, RECOMP_MMX_UW, 16, (uint16_t)(RECOMP_MMX_UW(a, i) >> cnt))
RECOMP_MMX_SHIFT(MMX_PSLLD, 2, RECOMP_MMX_UD, 32, (uint32_t)(RECOMP_MMX_UD(a, i) << cnt))
RECOMP_MMX_SHIFT(MMX_PSRLD, 2, RECOMP_MMX_UD, 32, (uint32_t)(RECOMP_MMX_UD(a, i) >> cnt))

static inline RecompMmx MMX_PSLLQ(RecompMmx a, uint64_t cnt) {
    RecompMmx r; r.q = (cnt >= 64) ? 0 : (a.q << cnt); return r;
}
static inline RecompMmx MMX_PSRLQ(RecompMmx a, uint64_t cnt) {
    RecompMmx r; r.q = (cnt >= 64) ? 0 : (a.q >> cnt); return r;
}
static inline RecompMmx MMX_PSRAW(RecompMmx a, uint64_t cnt) {
    RecompMmx r; int i; uint64_t c = (cnt >= 16) ? 15 : cnt;
    for (i = 0; i < 4; i++) RECOMP_MMX_W(r, i) = (int16_t)(RECOMP_MMX_W(a, i) >> c);
    return r;
}
static inline RecompMmx MMX_PSRAD(RecompMmx a, uint64_t cnt) {
    RecompMmx r; int i; uint64_t c = (cnt >= 32) ? 31 : cnt;
    for (i = 0; i < 2; i++) RECOMP_MMX_D(r, i) = (int32_t)(RECOMP_MMX_D(a, i) >> c);
    return r;
}

/* -- unpack / pack ---------------------------------------------- */
static inline RecompMmx MMX_PUNPCKLBW(RecompMmx a, RecompMmx b) {
    RecompMmx r; int i;
    for (i = 0; i < 4; i++) {
        RECOMP_MMX_UB(r, i * 2) = RECOMP_MMX_UB(a, i);
        RECOMP_MMX_UB(r, i * 2 + 1) = RECOMP_MMX_UB(b, i);
    }
    return r;
}
static inline RecompMmx MMX_PUNPCKHBW(RecompMmx a, RecompMmx b) {
    RecompMmx r; int i;
    for (i = 0; i < 4; i++) {
        RECOMP_MMX_UB(r, i * 2) = RECOMP_MMX_UB(a, i + 4);
        RECOMP_MMX_UB(r, i * 2 + 1) = RECOMP_MMX_UB(b, i + 4);
    }
    return r;
}
static inline RecompMmx MMX_PUNPCKLWD(RecompMmx a, RecompMmx b) {
    RecompMmx r; int i;
    for (i = 0; i < 2; i++) {
        RECOMP_MMX_UW(r, i * 2) = RECOMP_MMX_UW(a, i);
        RECOMP_MMX_UW(r, i * 2 + 1) = RECOMP_MMX_UW(b, i);
    }
    return r;
}
static inline RecompMmx MMX_PUNPCKHWD(RecompMmx a, RecompMmx b) {
    RecompMmx r; int i;
    for (i = 0; i < 2; i++) {
        RECOMP_MMX_UW(r, i * 2) = RECOMP_MMX_UW(a, i + 2);
        RECOMP_MMX_UW(r, i * 2 + 1) = RECOMP_MMX_UW(b, i + 2);
    }
    return r;
}
static inline RecompMmx MMX_PUNPCKLDQ(RecompMmx a, RecompMmx b) {
    RecompMmx r;
    RECOMP_MMX_UD(r, 0) = RECOMP_MMX_UD(a, 0); RECOMP_MMX_UD(r, 1) = RECOMP_MMX_UD(b, 0);
    return r;
}
static inline RecompMmx MMX_PUNPCKHDQ(RecompMmx a, RecompMmx b) {
    RecompMmx r;
    RECOMP_MMX_UD(r, 0) = RECOMP_MMX_UD(a, 1); RECOMP_MMX_UD(r, 1) = RECOMP_MMX_UD(b, 1);
    return r;
}
static inline RecompMmx MMX_PACKSSWB(RecompMmx a, RecompMmx b) {
    RecompMmx r; int i;
    for (i = 0; i < 4; i++) RECOMP_MMX_B(r, i)     = recomp_sat_i8(RECOMP_MMX_W(a, i));
    for (i = 0; i < 4; i++) RECOMP_MMX_B(r, i + 4) = recomp_sat_i8(RECOMP_MMX_W(b, i));
    return r;
}
static inline RecompMmx MMX_PACKUSWB(RecompMmx a, RecompMmx b) {
    RecompMmx r; int i;
    for (i = 0; i < 4; i++) RECOMP_MMX_UB(r, i)     = recomp_sat_u8(RECOMP_MMX_W(a, i));
    for (i = 0; i < 4; i++) RECOMP_MMX_UB(r, i + 4) = recomp_sat_u8(RECOMP_MMX_W(b, i));
    return r;
}
static inline RecompMmx MMX_PACKSSDW(RecompMmx a, RecompMmx b) {
    RecompMmx r; int i;
    for (i = 0; i < 2; i++) RECOMP_MMX_W(r, i)     = recomp_sat_i16(RECOMP_MMX_D(a, i));
    for (i = 0; i < 2; i++) RECOMP_MMX_W(r, i + 2) = recomp_sat_i16(RECOMP_MMX_D(b, i));
    return r;
}

/* -- word insert / extract / shuffle ----------------------------- */
static inline RecompMmx MMX_PSHUFW(RecompMmx a, uint32_t imm) {
    RecompMmx r; int i;
    for (i = 0; i < 4; i++)
        RECOMP_MMX_UW(r, i) = RECOMP_MMX_UW(a, (int)((imm >> (i * 2)) & 3u));
    return r;
}
static inline RecompMmx MMX_PINSRW(RecompMmx a, uint32_t v, uint32_t imm) {
    RecompMmx r = a; RECOMP_MMX_UW(r, (int)(imm & 3u)) = (uint16_t)v; return r;
}
static inline uint32_t MMX_PEXTRW(RecompMmx a, uint32_t imm) {
    return (uint32_t)RECOMP_MMX_UW(a, (int)(imm & 3u));
}
static inline uint32_t MMX_PMOVMSKB(RecompMmx a) {
    uint32_t m = 0; int i;
    for (i = 0; i < 8; i++) if (RECOMP_MMX_UB(a, i) & 0x80) m |= (1u << i);
    return m;
}
static inline RecompMmx MMX_PSADBW(RecompMmx a, RecompMmx b) {
    RecompMmx r; int i; uint32_t s = 0;
    for (i = 0; i < 8; i++) {
        uint32_t x = RECOMP_MMX_UB(a, i), y = RECOMP_MMX_UB(b, i);
        s += x > y ? x - y : y - x;
    }
    r.q = (uint64_t)(uint16_t)s;
    return r;
}
#endif /* !RECOMP_MMX_SHIFT_LANES */

#if RECOMP_MMX_SHIFT_LANES
/* The same instructions with every lane taken from and put back into `q` by
 * shifts (see RECOMP_MMX_SHIFT_LANES). Wrapping adds and subtracts are SWAR:
 * the top bit of each lane is kept out of the carry chain and fixed up with
 * an xor. */
#define RECOMP_MQ_H8  0x8080808080808080ull
#define RECOMP_MQ_H16 0x8000800080008000ull
#define RECOMP_MQ_H32 0x8000000080000000ull
RECOMP_MQ_FN uint64_t recomp_mq_add(uint64_t a, uint64_t b, uint64_t h) {
    return ((a & ~h) + (b & ~h)) ^ ((a ^ b) & h);
}
RECOMP_MQ_FN uint64_t recomp_mq_sub(uint64_t a, uint64_t b, uint64_t h) {
    return ((a | h) - (b & ~h)) ^ ((a ^ ~b) & h);
}
RECOMP_MQ_FN RecompMmx MMX_PADDB(RecompMmx a, RecompMmx b) { return recomp_mmx_q(recomp_mq_add(a.q, b.q, RECOMP_MQ_H8)); }
RECOMP_MQ_FN RecompMmx MMX_PADDW(RecompMmx a, RecompMmx b) { return recomp_mmx_q(recomp_mq_add(a.q, b.q, RECOMP_MQ_H16)); }
RECOMP_MQ_FN RecompMmx MMX_PADDD(RecompMmx a, RecompMmx b) { return recomp_mmx_q(recomp_mq_add(a.q, b.q, RECOMP_MQ_H32)); }
RECOMP_MQ_FN RecompMmx MMX_PSUBB(RecompMmx a, RecompMmx b) { return recomp_mmx_q(recomp_mq_sub(a.q, b.q, RECOMP_MQ_H8)); }
RECOMP_MQ_FN RecompMmx MMX_PSUBW(RecompMmx a, RecompMmx b) { return recomp_mmx_q(recomp_mq_sub(a.q, b.q, RECOMP_MQ_H16)); }
RECOMP_MQ_FN RecompMmx MMX_PSUBD(RecompMmx a, RecompMmx b) { return recomp_mmx_q(recomp_mq_sub(a.q, b.q, RECOMP_MQ_H32)); }

/* Lane-wise: GET reads lane i of a and b (RECOMP_MQ_*), EXPR is the lane's
 * result, masked to BITS and put back at lane i. */
#define RECOMP_MQ_BINOP(NAME, LANES, BITS, EXPR)                        \
    RECOMP_MQ_FN RecompMmx NAME(RecompMmx a, RecompMmx b) {            \
        uint64_t acc = 0; int i;                                        \
        for (i = 0; i < (LANES); i++)                                   \
            acc |= ((uint64_t)((uint32_t)(EXPR) & (uint32_t)(0xFFFFFFFFu >> (32 - (BITS))))) << (i * (BITS)); \
        return recomp_mmx_q(acc);                                       \
    }
RECOMP_MQ_BINOP(MMX_PADDSB, 8, 8, recomp_sat_i8(RECOMP_MQ_S8(a.q, i) + RECOMP_MQ_S8(b.q, i)))
RECOMP_MQ_BINOP(MMX_PADDSW, 4, 16, recomp_sat_i16(RECOMP_MQ_S16(a.q, i) + RECOMP_MQ_S16(b.q, i)))
RECOMP_MQ_BINOP(MMX_PSUBSB, 8, 8, recomp_sat_i8(RECOMP_MQ_S8(a.q, i) - RECOMP_MQ_S8(b.q, i)))
RECOMP_MQ_BINOP(MMX_PSUBSW, 4, 16, recomp_sat_i16(RECOMP_MQ_S16(a.q, i) - RECOMP_MQ_S16(b.q, i)))
RECOMP_MQ_BINOP(MMX_PADDUSB, 8, 8, recomp_sat_u8((int32_t)RECOMP_MQ_U8(a.q, i) + (int32_t)RECOMP_MQ_U8(b.q, i)))
RECOMP_MQ_BINOP(MMX_PSUBUSB, 8, 8, recomp_sat_u8((int32_t)RECOMP_MQ_U8(a.q, i) - (int32_t)RECOMP_MQ_U8(b.q, i)))
RECOMP_MQ_BINOP(MMX_PADDUSW, 4, 16, recomp_sat_u16((int32_t)RECOMP_MQ_U16(a.q, i) + (int32_t)RECOMP_MQ_U16(b.q, i)))
RECOMP_MQ_BINOP(MMX_PSUBUSW, 4, 16, recomp_sat_u16((int32_t)RECOMP_MQ_U16(a.q, i) - (int32_t)RECOMP_MQ_U16(b.q, i)))
RECOMP_MQ_BINOP(MMX_PMULLW, 4, 16, RECOMP_MQ_S16(a.q, i) * RECOMP_MQ_S16(b.q, i))
RECOMP_MQ_BINOP(MMX_PMULHW, 4, 16, (RECOMP_MQ_S16(a.q, i) * RECOMP_MQ_S16(b.q, i)) >> 16)
RECOMP_MQ_BINOP(MMX_PAVGB, 8, 8, (RECOMP_MQ_U8(a.q, i) + RECOMP_MQ_U8(b.q, i) + 1) >> 1)
RECOMP_MQ_BINOP(MMX_PAVGW, 4, 16, (RECOMP_MQ_U16(a.q, i) + RECOMP_MQ_U16(b.q, i) + 1) >> 1)
RECOMP_MQ_BINOP(MMX_PMINSW, 4, 16, RECOMP_MQ_S16(a.q, i) < RECOMP_MQ_S16(b.q, i) ? RECOMP_MQ_S16(a.q, i) : RECOMP_MQ_S16(b.q, i))
RECOMP_MQ_BINOP(MMX_PMAXSW, 4, 16, RECOMP_MQ_S16(a.q, i) > RECOMP_MQ_S16(b.q, i) ? RECOMP_MQ_S16(a.q, i) : RECOMP_MQ_S16(b.q, i))
RECOMP_MQ_BINOP(MMX_PCMPEQB, 8, 8, RECOMP_MQ_U8(a.q, i) == RECOMP_MQ_U8(b.q, i) ? 0xFFu : 0u)
RECOMP_MQ_BINOP(MMX_PCMPEQW, 4, 16, RECOMP_MQ_U16(a.q, i) == RECOMP_MQ_U16(b.q, i) ? 0xFFFFu : 0u)
RECOMP_MQ_BINOP(MMX_PCMPEQD, 2, 32, RECOMP_MQ_U32(a.q, i) == RECOMP_MQ_U32(b.q, i) ? 0xFFFFFFFFu : 0u)
RECOMP_MQ_BINOP(MMX_PCMPGTB, 8, 8, RECOMP_MQ_S8(a.q, i) > RECOMP_MQ_S8(b.q, i) ? 0xFFu : 0u)
RECOMP_MQ_BINOP(MMX_PCMPGTW, 4, 16, RECOMP_MQ_S16(a.q, i) > RECOMP_MQ_S16(b.q, i) ? 0xFFFFu : 0u)
RECOMP_MQ_BINOP(MMX_PCMPGTD, 2, 32, RECOMP_MQ_S32(a.q, i) > RECOMP_MQ_S32(b.q, i) ? 0xFFFFFFFFu : 0u)

/* pmaddwd: multiply signed words, add adjacent pairs into dwords. */
RECOMP_MQ_FN RecompMmx MMX_PMADDWD(RecompMmx a, RecompMmx b) {
    uint32_t lo = (uint32_t)(RECOMP_MQ_S16(a.q, 0) * RECOMP_MQ_S16(b.q, 0))
                + (uint32_t)(RECOMP_MQ_S16(a.q, 1) * RECOMP_MQ_S16(b.q, 1));
    uint32_t hi = (uint32_t)(RECOMP_MQ_S16(a.q, 2) * RECOMP_MQ_S16(b.q, 2))
                + (uint32_t)(RECOMP_MQ_S16(a.q, 3) * RECOMP_MQ_S16(b.q, 3));
    return recomp_mmx_q((uint64_t)lo | ((uint64_t)hi << 32));
}

/* -- bitwise ---------------------------------------------------- */
RECOMP_MQ_FN RecompMmx MMX_PAND(RecompMmx a, RecompMmx b)  { return recomp_mmx_q(a.q & b.q); }
RECOMP_MQ_FN RecompMmx MMX_PANDN(RecompMmx a, RecompMmx b) { return recomp_mmx_q(~a.q & b.q); }
RECOMP_MQ_FN RecompMmx MMX_POR(RecompMmx a, RecompMmx b)   { return recomp_mmx_q(a.q | b.q); }
RECOMP_MQ_FN RecompMmx MMX_PXOR(RecompMmx a, RecompMmx b)  { return recomp_mmx_q(a.q ^ b.q); }

/* -- shifts (counts as in the union versions above) -------------- */
/* Lane shifts work on the two 32-bit halves with 32-bit masks. The Xbox 360
 * compiler (2010-01) folds a 64-bit replicated mask such as
 * (0xFFFFu >> 2) * 0x0001000100010001 into a wrong 64-bit constant (here
 * 0xFFFF3FFF3FFF3FFF: the top lane's mask is built with lis -1), and it
 * strength-reduces sign * 0xC000 with a 64-bit mask truncated to 32 bits.
 * Generated code passes literal counts, so those folds happen in the game.
 * 32-bit constants are materialised correctly. */
RECOMP_MQ_FN uint64_t recomp_mq_join(uint32_t lo, uint32_t hi) {
    return (uint64_t)lo | ((uint64_t)hi << 32);
}
RECOMP_MQ_FN RecompMmx MMX_PSLLW(RecompMmx a, uint64_t cnt) {
    uint32_t lo = (uint32_t)a.q, hi = (uint32_t)(a.q >> 32), m;
    unsigned c;
    if (cnt >= 16) return recomp_mmx_q(0);
    c = (unsigned)cnt;
    m = (0xFFFFu << c) & 0xFFFFu; m |= m << 16;
    return recomp_mmx_q(recomp_mq_join((lo << c) & m, (hi << c) & m));
}
RECOMP_MQ_FN RecompMmx MMX_PSRLW(RecompMmx a, uint64_t cnt) {
    uint32_t lo = (uint32_t)a.q, hi = (uint32_t)(a.q >> 32), m;
    unsigned c;
    if (cnt >= 16) return recomp_mmx_q(0);
    c = (unsigned)cnt;
    m = 0xFFFFu >> c; m |= m << 16;
    return recomp_mmx_q(recomp_mq_join((lo >> c) & m, (hi >> c) & m));
}
RECOMP_MQ_FN RecompMmx MMX_PSLLD(RecompMmx a, uint64_t cnt) {
    unsigned c;
    if (cnt >= 32) return recomp_mmx_q(0);
    c = (unsigned)cnt;
    return recomp_mmx_q(recomp_mq_join((uint32_t)a.q << c, (uint32_t)(a.q >> 32) << c));
}
RECOMP_MQ_FN RecompMmx MMX_PSRLD(RecompMmx a, uint64_t cnt) {
    unsigned c;
    if (cnt >= 32) return recomp_mmx_q(0);
    c = (unsigned)cnt;
    return recomp_mmx_q(recomp_mq_join((uint32_t)a.q >> c, (uint32_t)(a.q >> 32) >> c));
}
RECOMP_MQ_FN RecompMmx MMX_PSLLQ(RecompMmx a, uint64_t cnt) {
    return recomp_mmx_q((cnt >= 64) ? 0 : (a.q << cnt));
}
RECOMP_MQ_FN RecompMmx MMX_PSRLQ(RecompMmx a, uint64_t cnt) {
    return recomp_mmx_q((cnt >= 64) ? 0 : (a.q >> cnt));
}
/* Two words per 32-bit half, all unsigned: the logical shift, then the
 * sign fill (0xFFFF per negative lane, minus the kept bits). The per-lane
 * (int16_t) cast form was also miscompiled on the 360 (0x8000 >> 15 gave
 * 0x0001, the sign extension dropped). */
RECOMP_MQ_FN uint32_t recomp_mq_sraw32(uint32_t x, unsigned c) {
    uint32_t keep = 0xFFFFu >> c, neg;
    keep |= keep << 16;
    neg = ((x >> 15) & 1u) * 0xFFFFu | ((x >> 31) & 1u) * 0xFFFF0000u;
    return ((x >> c) & keep) | (neg & ~keep);
}
RECOMP_MQ_FN RecompMmx MMX_PSRAW(RecompMmx a, uint64_t cnt) {
    unsigned c = (unsigned)((cnt >= 16) ? 15 : cnt);
    return recomp_mmx_q(recomp_mq_join(recomp_mq_sraw32((uint32_t)a.q, c),
                                       recomp_mq_sraw32((uint32_t)(a.q >> 32), c)));
}
RECOMP_MQ_FN RecompMmx MMX_PSRAD(RecompMmx a, uint64_t cnt) {
    unsigned c = (unsigned)((cnt >= 32) ? 31 : cnt);
    return recomp_mmx_q((uint64_t)(uint32_t)(RECOMP_MQ_S32(a.q, 0) >> c)
                        | ((uint64_t)(uint32_t)(RECOMP_MQ_S32(a.q, 1) >> c) << 32));
}

/* -- unpack / pack ---------------------------------------------- */
/* Interleave the bytes (words) of two dwords, x86 lane order: x supplies
 * the even lanes, y the odd ones. Built from 32-bit halves, which the Xbox
 * 360 compiler handles well (a 64-bit spread-and-mask form of the same thing
 * crashed it, C1001, in the XMV player's MMX code). */
RECOMP_MQ_FN uint64_t recomp_mq_ilv8(uint32_t x, uint32_t y) {
    uint32_t lo = (x & 0xFFu) | ((y & 0xFFu) << 8) | ((x & 0xFF00u) << 8) | ((y & 0xFF00u) << 16);
    uint32_t hi = ((x >> 16) & 0xFFu) | (((y >> 16) & 0xFFu) << 8) | ((x >> 8) & 0xFF0000u) | (y & 0xFF000000u);
    return (uint64_t)lo | ((uint64_t)hi << 32);
}
RECOMP_MQ_FN uint64_t recomp_mq_ilv16(uint32_t x, uint32_t y) {
    uint32_t lo = (x & 0xFFFFu) | (y << 16);
    uint32_t hi = (x >> 16) | (y & 0xFFFF0000u);
    return (uint64_t)lo | ((uint64_t)hi << 32);
}
RECOMP_MQ_FN RecompMmx MMX_PUNPCKLBW(RecompMmx a, RecompMmx b) {
    return recomp_mmx_q(recomp_mq_ilv8((uint32_t)a.q, (uint32_t)b.q));
}
RECOMP_MQ_FN RecompMmx MMX_PUNPCKHBW(RecompMmx a, RecompMmx b) {
    return recomp_mmx_q(recomp_mq_ilv8((uint32_t)(a.q >> 32), (uint32_t)(b.q >> 32)));
}
RECOMP_MQ_FN RecompMmx MMX_PUNPCKLWD(RecompMmx a, RecompMmx b) {
    return recomp_mmx_q(recomp_mq_ilv16((uint32_t)a.q, (uint32_t)b.q));
}
RECOMP_MQ_FN RecompMmx MMX_PUNPCKHWD(RecompMmx a, RecompMmx b) {
    return recomp_mmx_q(recomp_mq_ilv16((uint32_t)(a.q >> 32), (uint32_t)(b.q >> 32)));
}
RECOMP_MQ_FN RecompMmx MMX_PUNPCKLDQ(RecompMmx a, RecompMmx b) {
    return recomp_mmx_q((a.q & 0xFFFFFFFFull) | (b.q << 32));
}
RECOMP_MQ_FN RecompMmx MMX_PUNPCKHDQ(RecompMmx a, RecompMmx b) {
    return recomp_mmx_q((a.q >> 32) | (b.q & 0xFFFFFFFF00000000ull));
}
RECOMP_MQ_FN RecompMmx MMX_PACKSSWB(RecompMmx a, RecompMmx b) {
    uint64_t acc = 0; int i;
    for (i = 0; i < 4; i++) acc |= (uint64_t)(uint8_t)recomp_sat_i8(RECOMP_MQ_S16(a.q, i)) << (i * 8);
    for (i = 0; i < 4; i++) acc |= (uint64_t)(uint8_t)recomp_sat_i8(RECOMP_MQ_S16(b.q, i)) << (i * 8 + 32);
    return recomp_mmx_q(acc);
}
RECOMP_MQ_FN RecompMmx MMX_PACKUSWB(RecompMmx a, RecompMmx b) {
    uint64_t acc = 0; int i;
    for (i = 0; i < 4; i++) acc |= (uint64_t)recomp_sat_u8(RECOMP_MQ_S16(a.q, i)) << (i * 8);
    for (i = 0; i < 4; i++) acc |= (uint64_t)recomp_sat_u8(RECOMP_MQ_S16(b.q, i)) << (i * 8 + 32);
    return recomp_mmx_q(acc);
}
RECOMP_MQ_FN RecompMmx MMX_PACKSSDW(RecompMmx a, RecompMmx b) {
    return recomp_mmx_q((uint64_t)(uint16_t)recomp_sat_i16(RECOMP_MQ_S32(a.q, 0))
                        | ((uint64_t)(uint16_t)recomp_sat_i16(RECOMP_MQ_S32(a.q, 1)) << 16)
                        | ((uint64_t)(uint16_t)recomp_sat_i16(RECOMP_MQ_S32(b.q, 0)) << 32)
                        | ((uint64_t)(uint16_t)recomp_sat_i16(RECOMP_MQ_S32(b.q, 1)) << 48));
}

/* -- word insert / extract / shuffle ----------------------------- */
RECOMP_MQ_FN RecompMmx MMX_PSHUFW(RecompMmx a, uint32_t imm) {
    uint64_t acc = 0; int i;
    for (i = 0; i < 4; i++)
        acc |= (uint64_t)RECOMP_MQ_U16(a.q, (imm >> (i * 2)) & 3u) << (i * 16);
    return recomp_mmx_q(acc);
}
RECOMP_MQ_FN RecompMmx MMX_PINSRW(RecompMmx a, uint32_t v, uint32_t imm) {
    unsigned s = (imm & 3u) * 16;
    return recomp_mmx_q((a.q & ~(0xFFFFull << s)) | ((uint64_t)(uint16_t)v << s));
}
RECOMP_MQ_FN uint32_t MMX_PEXTRW(RecompMmx a, uint32_t imm) {
    return RECOMP_MQ_U16(a.q, imm & 3u);
}
RECOMP_MQ_FN uint32_t MMX_PMOVMSKB(RecompMmx a) {
    uint32_t m = 0; int i;
    for (i = 0; i < 8; i++) m |= (uint32_t)((a.q >> (i * 8 + 7)) & 1u) << i;
    return m;
}
RECOMP_MQ_FN RecompMmx MMX_PSADBW(RecompMmx a, RecompMmx b) {
    int i; uint32_t s = 0;
    for (i = 0; i < 8; i++) {
        uint32_t x = RECOMP_MQ_U8(a.q, i), y = RECOMP_MQ_U8(b.q, i);
        s += x > y ? x - y : y - x;
    }
    return recomp_mmx_q((uint64_t)(uint16_t)s);
}
#endif /* RECOMP_MMX_SHIFT_LANES */

/* SSE and MMX register names: the same two mappings as the GPRs above. */
#ifdef RC_ACTIVE
#define xmm0 rc_xmm0
#define xmm1 rc_xmm1
#define xmm2 rc_xmm2
#define xmm3 rc_xmm3
#define xmm4 rc_xmm4
#define xmm5 rc_xmm5
#define xmm6 rc_xmm6
#define xmm7 rc_xmm7
#define mm0 rc_mm0
#define mm1 rc_mm1
#define mm2 rc_mm2
#define mm3 rc_mm3
#define mm4 rc_mm4
#define mm5 rc_mm5
#define mm6 rc_mm6
#define mm7 rc_mm7
#else
#define xmm0 g_xmm0
#define xmm1 g_xmm1
#define xmm2 g_xmm2
#define xmm3 g_xmm3
#define xmm4 g_xmm4
#define xmm5 g_xmm5
#define xmm6 g_xmm6
#define xmm7 g_xmm7
#define mm0 g_mm0
#define mm1 g_mm1
#define mm2 g_mm2
#define mm3 g_mm3
#define mm4 g_mm4
#define mm5 g_mm5
#define mm6 g_mm6
#define mm7 g_mm7
#endif
/* ebp is NOT global - it's local in each function.
 * For __SEH_prolog/epilog, use g_seh_ebp to bridge. */
#endif

/* ================================================================
 * Forward declarations for translated functions
 *
 * These are generated by the recompiler and included per-file.
 * The recomp_funcs.h header (generated) declares all translated
 * function prototypes.
 * ================================================================ */

/* An instruction the lifter has no translation for.
 *
 * The lifter used to emit a bare "TODO: mnemonic" C comment at such a site,
 * and a comment is a no-op: the instruction vanished, the guest carried
 * on, and nothing at runtime could say the site had been reached. Wreckless
 * died inside RtlAllocateHeap because `bsf` was a comment; Half-Life 2 painted
 * its intro red because `cvtpi2ps` was. Each was found by working backwards
 * from a subsystem failure that had nothing to do with the cause.
 *
 * Now the site calls this, with the instruction text and its guest address.
 * The instruction is STILL a no-op -- this changes no behaviour -- but the run
 * says which untranslated sites it reached, how often, and with what register
 * state, and with RECOMP_UNIMPL_TRAP=1 it stops at the first one. The lifter
 * keeps the comment beside the call so `grep TODO:` over a gen tree still
 * enumerates every site whether or not it is ever reached. */
void recomp_unimpl(const char *text, uint32_t va);
#define RECOMP_UNIMPL(_text, _va) recomp_unimpl((_text), (_va))

#endif /* RECOMP_TYPES_H */
