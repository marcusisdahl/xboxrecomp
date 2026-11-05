/* Per-access cost of explicit-guest-bus memory accesses (XrGuestRef and the
 * recomp_types.h helpers built on it), and of the x87 rounding helpers.
 *
 * The guest map copies the Xbox 360 host's (source/platform/xbox360/
 * game_host.cpp): 64 MiB of RAM at 0 with one 4 KiB debug-watch page served
 * by device callbacks, the 0x80000000 contiguous alias, and a device window
 * at 0xFD000000. Every case runs the access exactly as generated code would
 * and prints "bench <name> <ns per op> <checksum>". The checksums must match
 * between builds (they are what the accesses read back); the times are what
 * the fast path changes.
 *
 * PC: build_pc.ps1. Xbox 360: GuestRefBench360.vcxproj (writes
 * game:\guest_ref_bench.txt and echoes each line as debug output). */
#if defined(_XBOX)
#include "platform/recomp_c99_x360.h"
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
/* Before recomp_types.h: its register macros (eax, esp, ...) break winnt.h. */
#if defined(_XBOX)
#pragma push_macro("_X")
#undef _X
#define NOD3D
#define NONET
#include <xtl.h>
#pragma pop_macro("_X")
#else
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#define RECOMP_EXPLICIT_GUEST_BUS
#define RECOMP_GENERATED_CODE
#include "recomp_types.h"

#if defined(_MSC_VER) && _MSC_VER < 1900
#define bench_snprintf _snprintf
#else
#define bench_snprintf snprintf
#endif

XR_REF_TLS XrGuestBus *g_recomp_guest_bus;
static XrGuestBus s_bus;
static FILE *s_log;

static void log_line(const char *line)
{
    if (s_log) { fputs(line, s_log); fputs("\r\n", s_log); }
    fputs(line, stdout); fputs("\n", stdout);
#if defined(_XBOX)
    OutputDebugStringA(line);
    OutputDebugStringA("\n");
#endif
}

void xr_guest_access_fault(XrGuestStatus s, uint32_t a, unsigned w)
{
    char line[96];
    bench_snprintf(line, sizeof line, "FATAL guest fault status=%d va=0x%08X width=%u",
                   (int)s, (unsigned)a, w);
    log_line(line);
    abort();
}

static uint8_t *s_ram;
static const uint32_t kWatchPage = 0x0041A000u;
static int watch_read(void *, uint32_t addr, unsigned width, uint64_t *out)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < width; ++i) v |= (uint64_t)s_ram[addr + i] << (8 * i);
    *out = v; return 1;
}
static int watch_write(void *, uint32_t addr, unsigned width, uint64_t value)
{
    for (unsigned i = 0; i < width; ++i) s_ram[addr + i] = (uint8_t)(value >> (8 * i));
    return 1;
}
static uint32_t s_reg[64];
static int dev_read(void *, uint32_t addr, unsigned, uint64_t *out)
{
    *out = s_reg[(addr >> 2) & 63]; return 1;
}
static int dev_write(void *, uint32_t addr, unsigned, uint64_t value)
{
    s_reg[(addr >> 2) & 63] = (uint32_t)value; return 1;
}

static int map_bus(void)
{
    XrGuestStatus st;
    s_ram = (uint8_t *)VirtualAlloc(NULL, XR_GUEST_RETAIL_RAM, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!s_ram) return 0;
    xr_guest_bus_init(&s_bus);
    st = xr_guest_map_ram(&s_bus, 0, kWatchPage, s_ram, XR_GUEST_READ | XR_GUEST_WRITE);
    if (st == XR_GUEST_OK) st = xr_guest_map_device(&s_bus, kWatchPage, 0x1000, XR_GUEST_READ | XR_GUEST_WRITE,
                                                    XR_GUEST_WIDTHS, 0, watch_read, watch_write);
    if (st == XR_GUEST_OK) st = xr_guest_map_ram(&s_bus, kWatchPage + 0x1000, XR_GUEST_RETAIL_RAM - kWatchPage - 0x1000,
                                                 s_ram + kWatchPage + 0x1000, XR_GUEST_READ | XR_GUEST_WRITE);
    if (st == XR_GUEST_OK) st = xr_guest_map_ram(&s_bus, 0x80000000u, XR_GUEST_RETAIL_RAM, s_ram,
                                                 XR_GUEST_READ | XR_GUEST_WRITE);
    if (st == XR_GUEST_OK) st = xr_guest_map_device(&s_bus, 0xFD000000u, 0x1000000u, XR_GUEST_READ | XR_GUEST_WRITE,
                                                    XR_GUEST_WIDTHS, 0, dev_read, dev_write);
    g_recomp_guest_bus = &s_bus;
    return st == XR_GUEST_OK;
}

static double now_ns(void)
{
    static double scale;
    LARGE_INTEGER t;
    if (!scale) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        scale = 1e9 / (double)f.QuadPart;
    }
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * scale;
}

/* 16 KiB of guest data: L1-resident on both hosts, so the time is the
 * access path, not the memory system. */
#define SPAN 0x4000u
#define BASE 0x00200000u
static const unsigned kOps = 1u << 22;

/* Each case returns a checksum and does kOps accesses. noinline keeps the
 * compiler from merging cases or hoisting setup between them. */
#if defined(_MSC_VER)
#define BENCH_NOINLINE __declspec(noinline)
#else
#define BENCH_NOINLINE __attribute__((noinline))
#endif

BENCH_NOINLINE static uint64_t b_load32(uint32_t base)
{
    uint64_t sum = 0;
    for (unsigned i = 0; i < kOps; ++i) sum += MEM32(base + ((i * 4u) & (SPAN - 1)));
    return sum;
}
BENCH_NOINLINE static uint64_t b_store32(uint32_t base)
{
    for (unsigned i = 0; i < kOps; ++i) MEM32(base + ((i * 4u) & (SPAN - 1))) = i;
    return MEM32(base) + (uint64_t)MEM32(base + SPAN - 4);
}
BENCH_NOINLINE static uint64_t b_load8(uint32_t base)
{
    uint64_t sum = 0;
    for (unsigned i = 0; i < kOps; ++i) sum += MEM8(base + (i & (SPAN - 1)));
    return sum;
}
BENCH_NOINLINE static uint64_t b_load16s(uint32_t base)
{
    uint64_t sum = 0;
    for (unsigned i = 0; i < kOps; ++i) sum += (uint64_t)(int64_t)SMEM16(base + ((i * 2u) & (SPAN - 1)));
    return sum;
}
BENCH_NOINLINE static uint64_t b_rmw32(uint32_t base)
{
    for (unsigned i = 0; i < kOps; ++i) MEM32(base + ((i * 4u) & (SPAN - 1))) += 3u;
    return MEM32(base) + (uint64_t)MEM32(base + 8);
}
BENCH_NOINLINE static uint64_t b_unaligned32(uint32_t base)
{
    uint64_t sum = 0;
    for (unsigned i = 0; i < kOps; ++i) sum += MEM32(base + 1u + ((i * 4u) & (SPAN - 16)));
    return sum;
}
BENCH_NOINLINE static uint64_t b_loadf(uint32_t base)
{
    double sum = 0;
    for (unsigned i = 0; i < kOps; ++i) sum += MEMF(base + ((i * 4u) & (SPAN - 1)));
    uint64_t bits; memcpy(&bits, &sum, 8); return bits;
}
BENCH_NOINLINE static uint64_t b_storef(uint32_t base)
{
    for (unsigned i = 0; i < kOps; ++i) MEMF(base + ((i * 4u) & (SPAN - 1))) = (float)(i & 1023u);
    return MEM32(base + 4);
}
BENCH_NOINLINE static uint64_t b_loadd(uint32_t base)
{
    double sum = 0;
    for (unsigned i = 0; i < kOps; ++i) sum += MEMD(base + ((i * 8u) & (SPAN - 1)));
    uint64_t bits; memcpy(&bits, &sum, 8); return bits;
}
BENCH_NOINLINE static uint64_t b_xmm(uint32_t base)
{
    for (unsigned i = 0; i < kOps / 4; ++i) {
        uint32_t a = base + ((i * 16u) & (SPAN - 1));
        RecompXmm x = XMM_MEM(a);
        x.u[0] += 1u;
        XMM_STORE(a, x);
    }
    return MEM32(base) + (uint64_t)MEM32(base + 12);
}
BENCH_NOINLINE static uint64_t b_mmx(uint32_t base)
{
    uint64_t sum = 0;
    for (unsigned i = 0; i < kOps / 2; ++i) sum += MMX_MEM(base + ((i * 8u) & (SPAN - 1))).q;
    return sum;
}
BENCH_NOINLINE static uint64_t b_watch32(uint32_t)
{
    uint64_t sum = 0;
    for (unsigned i = 0; i < kOps / 4; ++i) sum += MEM32(kWatchPage + ((i * 4u) & 0xFFFu));
    return sum;
}
BENCH_NOINLINE static uint64_t b_device32(uint32_t)
{
    uint64_t sum = 0;
    for (unsigned i = 0; i < kOps / 4; ++i) sum += MEM32(0xFD000000u + ((i * 4u) & 0xFFu));
    return sum;
}
BENCH_NOINLINE static uint64_t b_cross32(uint32_t base)
{
    /* Straddles a 4 KiB page each time: always the out-of-line path. */
    uint64_t sum = 0;
    for (unsigned i = 0; i < kOps / 4; ++i) sum += MEM32(base + 0xFFEu + ((i & 3u) << 12));
    return sum;
}
static volatile double s_fp_in[256];
BENCH_NOINLINE static uint64_t b_fist(uint32_t)
{
    int64_t sum = 0;
    for (unsigned i = 0; i < kOps; ++i) sum += recomp_fist(s_fp_in[i & 255u], 0x027F, 32);
    return (uint64_t)sum;
}
BENCH_NOINLINE static uint64_t b_fist_trunc(uint32_t)
{
    int64_t sum = 0;
    for (unsigned i = 0; i < kOps; ++i) sum += recomp_fist(s_fp_in[i & 255u], 0x0E7F, 32);
    return (uint64_t)sum;
}
BENCH_NOINLINE static uint64_t b_frndint(uint32_t)
{
    double sum = 0;
    for (unsigned i = 0; i < kOps; ++i) sum += recomp_frndint(s_fp_in[i & 255u], (uint16_t)(0x027F | ((i & 3u) << 10)));
    uint64_t bits; memcpy(&bits, &sum, 8); return bits;
}

#if defined(RECOMP_FP_MAX_FINITE)
/* The x87 rounding helpers as they were before they stopped calling the
 * CRT, kept here as the reference the new ones must match bit for bit. */
static double old_frndint(double value, uint16_t control) {
    double rounded;
    if (!isfinite(value)) return value;
    switch((control>>10)&3) {
    case 1: rounded=floor(value); break;
    case 2: rounded=ceil(value); break;
    case 3: rounded=trunc(value); break;
    default: {
        double lo=floor(value), fraction=value-lo;
        rounded=lo;
        if(fraction>0.5 || (fraction==0.5 && fmod(lo,2.0)!=0.0)) rounded=lo+1.0;
        break;
    }
    }
    return rounded == 0.0 ? copysign(0.0, value) : rounded;
}
static int64_t old_fist(double value, uint16_t control, unsigned bits) {
    double rounded = old_frndint(value, control);
    double limit=ldexp(1.0,(int)bits-1);
    if(!isfinite(rounded) || rounded < -limit || rounded >= limit)
        return bits==64?INT64_MIN:-(INT64_C(1)<<(bits-1));
    return (int64_t)rounded;
}
static uint64_t dbits(double d) { uint64_t b; memcpy(&b, &d, 8); return b; }
static double from_bits(uint64_t b) { double d; memcpy(&d, &b, 8); return d; }

/* Old and new helpers over special values, ties, boundaries and random
 * doubles of every magnitude; returns the number of mismatches. */
static unsigned x87_equivalence(unsigned *checked)
{
    static const double kSpecial[] = {
        0.0, 0.5, 1.0, 1.5, 2.5, 3.5, 0.49999999999999994, 0.5000000000000001, 1e-300, 4.9e-324,
        2147483647.0, 2147483647.5, 2147483648.0, 2147483648.5, 2147483646.5, 32767.5, 32768.0, 32767.49,
        4503599627370495.5, 4503599627370496.0, 4503599627370497.0, 9007199254740993.0,
        9223372036854775807.0, 9223372036854774784.0, 1e300, 1.7976931348623157e308
    };
    unsigned bad = 0, n = 0;
    uint64_t seed = 0x9E3779B97F4A7C15ull;
    double inf = from_bits(0x7FF0000000000000ull), nan = from_bits(0x7FF8000000000000ull);
    for (unsigned i = 0; i < 4000000u; ++i) {
        double v;
        if (i < 2 * sizeof kSpecial / sizeof kSpecial[0]) {
            v = kSpecial[i / 2]; if (i & 1) v = -v;
        } else if (i < 2 * sizeof kSpecial / sizeof kSpecial[0] + 4) {
            v = (i & 1) ? inf : nan; if (i & 2) v = -v;
        } else {
            seed = seed * 6364136223846793005ull + 1442695040888963407ull;
            uint64_t r = seed >> 11;
            switch (i & 3) {
            case 0: v = from_bits(seed); break;                              /* any pattern */
            case 1: v = ((double)(int64_t)(r & 0xFFFFF) - 524288.0) * 0.25; break;  /* quarters */
            case 2: v = ((double)(int64_t)(r & 0xFFFFFFFFFull) - 34359738368.0) / 1024.0; break;
            default: v = ldexp((double)(r & 0xFFFFFFFFFFFFFull) / 4503599627370496.0, (int)((r >> 52) % 140) - 70);
                     if (seed & 1) v = -v; break;
            }
        }
        for (unsigned rc = 0; rc < 4; ++rc) {
            uint16_t cw = (uint16_t)(0x027F | (rc << 10));
            double a = old_frndint(v, cw), b = recomp_frndint(v, cw);
            ++n;
            if (dbits(a) != dbits(b) && !(a != a && b != b)) ++bad;
            for (unsigned k = 0; k < 3; ++k) {
                unsigned bits = k == 0 ? 16 : k == 1 ? 32 : 64;
                ++n;
                if (old_fist(v, cw, bits) != recomp_fist(v, cw, bits)) ++bad;
            }
        }
        n += 3;
        if (!!isfinite(v) != !!recomp_fp_isfinite(v)) ++bad;
        if (!!isnan(v) != !!recomp_fp_isnan(v)) ++bad;
        if (!!isinf(v) != !!recomp_fp_isinf(v)) ++bad;
    }
    *checked = n;
    return bad;
}
#endif

typedef uint64_t (*BenchFn)(uint32_t);
struct Bench { const char *name; BenchFn fn; uint32_t base; unsigned ops; };

int main(int argc, char **argv)
{
    static const Bench kBench[] = {
        { "load32", b_load32, BASE, kOps },
        { "load32_alias80", b_load32, 0x80000000u | BASE, kOps },
        { "store32", b_store32, BASE, kOps },
        { "load8", b_load8, BASE, kOps },
        { "load16s", b_load16s, BASE, kOps },
        { "rmw32", b_rmw32, BASE, kOps },
        { "unaligned32", b_unaligned32, BASE, kOps },
        { "loadf", b_loadf, BASE, kOps },
        { "storef", b_storef, BASE, kOps },
        { "loadd", b_loadd, BASE, kOps },
        { "xmm_load_store", b_xmm, BASE, kOps / 4 },
        { "mmx_load", b_mmx, BASE, kOps / 2 },
        { "watch32_slow", b_watch32, 0, kOps / 4 },
        { "device32_slow", b_device32, 0, kOps / 4 },
        { "pagecross32_slow", b_cross32, BASE, kOps / 4 },
        { "fist_nearest", b_fist, 0, kOps },
        { "fist_trunc", b_fist_trunc, 0, kOps },
        { "frndint_rc", b_frndint, 0, kOps },
    };
    char line[160];
#if defined(_XBOX)
    s_log = fopen("game:\\guest_ref_bench.txt", "wb");
    log_line("Takedawn 360 boot trace (guest_ref_bench)");
#else
    if (argc > 1) s_log = fopen(argv[1], "wb");
    log_line("guest_ref_bench");
#endif
    (void)argc; (void)argv;
    if (!map_bus()) { log_line("FAIL map"); return 1; }
    for (uint32_t i = 0; i < 0x20000u; ++i) s_ram[BASE + i] = (uint8_t)(i * 37u + (i >> 8));
    for (uint32_t i = 0; i < 0x1000u; ++i) s_ram[kWatchPage + i] = (uint8_t)(i * 11u);
    /* Float/double data that stays finite and small when read back. */
    for (uint32_t i = 0; i < SPAN / 4; ++i) {
        float f = (float)((int)(i % 97u) - 48) * 0.25f;
        uint32_t bits; memcpy(&bits, &f, 4);
        s_ram[BASE + 0x10000u + i * 4] = (uint8_t)bits; s_ram[BASE + 0x10001u + i * 4] = (uint8_t)(bits >> 8);
        s_ram[BASE + 0x10002u + i * 4] = (uint8_t)(bits >> 16); s_ram[BASE + 0x10003u + i * 4] = (uint8_t)(bits >> 24);
    }
    for (unsigned i = 0; i < 256; ++i) s_fp_in[i] = ((double)i - 128.0) * 0.37 + (i & 1 ? 0.5 : 0.0);
    double total = 0;
    for (unsigned k = 0; k < sizeof kBench / sizeof kBench[0]; ++k) {
        const Bench &b = kBench[k];
        uint32_t base = b.base;
        if (b.fn == b_loadf || b.fn == b_loadd) base += 0x10000u;
        if (b.fn == b_storef) base += 0x8000u;
        b.fn(base);                                  /* warm up */
        double t0 = now_ns();
        uint64_t sum = b.fn(base);
        double ns = (now_ns() - t0) / (double)b.ops;
        total += ns;
        bench_snprintf(line, sizeof line, "bench %-18s %8.2f ns/op  sum %016llX", b.name, ns,
                       (unsigned long long)sum);
        log_line(line);
    }
#if defined(RECOMP_FP_MAX_FINITE)   /* the helpers that no longer call the CRT */
    {
        unsigned checked = 0, bad = x87_equivalence(&checked);
        bench_snprintf(line, sizeof line, "check x87 helpers vs CRT reference: %u mismatches in %u", bad, checked);
        log_line(line);
    }
#endif
    if (s_log) fclose(s_log);
    s_log = NULL;
    log_line("DONE");
#if defined(_XBOX)
    Sleep(1000);
    XLaunchNewImage(NULL, 0);
#endif
    return 0;
}
