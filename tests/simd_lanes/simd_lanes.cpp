/* MMX/SSE lane test for templates/runtime/recomp_types.h (see simd_lanes.h).
 *
 * Build modes:
 *   default            explicit guest bus (RECOMP_EXPLICIT_GUEST_BUS), as the
 *                      Xbox 360 port compiles generated code; C++ required.
 *   SIMD_LANES_FLAT    the desktop flat-memory model (MEMn are host pointer
 *                      casts through g_xbox_mem_offset); little-endian only.
 *   SIMD_LANES_MAIN    adds a console main() that prints the lines and can
 *                      write the raw dump (argv[1]).
 * On a 32-bit x86 build every case that has an MMX/SSE instruction behind it
 * is also checked against that instruction (intrinsics), so the goldens are
 * Intel's semantics, not merely the helper agreeing with itself. x64 MSVC has
 * no MMX intrinsics, so there the check is skipped.
 */
#if defined(_XBOX)
#include "platform/recomp_c99_x360.h"
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#if defined(_M_IX86) && !defined(SIMD_LANES_NO_REF)
#define SIMD_LANES_REF 1
#include <mmintrin.h>
#include <xmmintrin.h>
#include <emmintrin.h>
#endif
#include "simd_lanes.h"

#if !defined(SIMD_LANES_FLAT)
#define RECOMP_EXPLICIT_GUEST_BUS
#endif
#define RECOMP_GENERATED_CODE
#include "recomp_types.h"

#if defined(_MSC_VER) && _MSC_VER < 1900
#define lanes_snprintf _snprintf
#else
#define lanes_snprintf snprintf
#endif

/* Runtime symbols the header refers to. */
#if defined(SIMD_LANES_FLAT)
ptrdiff_t g_xbox_mem_offset;
#else
XR_REF_TLS XrGuestBus *g_recomp_guest_bus;
void xr_guest_access_fault(XrGuestStatus s, uint32_t a, unsigned w)
{
    char line[96];
    lanes_snprintf(line, sizeof line, "FATAL guest fault status=%d va=0x%08X width=%u",
                   (int)s, (unsigned)a, w);
    puts(line);
    abort();
}
#endif

/* Guest layout: inputs a|b|c contiguous at VA_IN, output at VA_OUT. Three
 * pages of RAM, so with the explicit bus the fast-page table
 * (recomp_guest_ref.h) lists pages 0x10 and 0x11 (each followed by RAM) but
 * not 0x12 (the last). Each case runs at three placements (kPlace): the
 * main one in a listed page (the inline fast path, as most generated
 * accesses), the data straddling page boundaries (a page-crossing access is
 * fast on little-endian hosts, the normal path on big-endian ones), and in
 * the unlisted last page (the out-of-line xr_guest_load/xr_guest_store
 * path). The printed hashes are the main placement's; the other two must
 * give the same bytes. */
#define GUEST_BASE 0x00010000u
#define GUEST_SIZE 0x3000u
static uint32_t s_va_in = GUEST_BASE + 0x100u, s_va_out = GUEST_BASE + 0x200u;
#define VA_IN  (s_va_in)
#define VA_A   (VA_IN)
#define VA_B   (VA_IN + 16u)
#define VA_C   (VA_IN + 32u)
#define VA_OUT (s_va_out)
struct Place { const char *name; uint32_t in, out; };
static const Place kPlace[] = {
    { "listed page", GUEST_BASE + 0x0100u, GUEST_BASE + 0x0200u },
    /* a|b|c from 0x10FE8: b and the unaligned XMM/MMX loads cross into page
     * 0x11; the output from 0x11FFE: its first dword crosses into 0x12 */
    { "page-crossing", GUEST_BASE + 0x0FE8u, GUEST_BASE + 0x1FFEu },
    { "unlisted page", GUEST_BASE + 0x2100u, GUEST_BASE + 0x2200u },
};
static uint8_t s_ram[GUEST_SIZE];
#if !defined(SIMD_LANES_FLAT)
static XrGuestBus s_bus;
#endif

struct Ctx {
    uint8_t in[48];   /* a = in, b = in + 16, c = in + 32 (little-endian bytes) */
    uint8_t o[16];    /* what VA_OUT holds before the case runs */
    uint32_t imm;
    uint8_t out[16];  /* reference result (x86 build) */
};

/* ---- recomp side: each case runs the helper exactly as generated code would */
#define MMXA MMX_MEM(VA_A)
#define MMXB MMX_MEM(VA_B)
#define XMMA XMM_MEM(VA_A)
#define XMMB XMM_MEM(VA_B)

#define R_MMX2(H) static void r_##H(const Ctx *) { MMX_STORE(VA_OUT, H(MMXA, MMXB)); }
#define R_MMXSH(H) \
    static void r_##H(const Ctx *) { MMX_STORE(VA_OUT, H(MMXA, MMX_MEM(VA_C).q)); } \
    static void r_##H##_I(const Ctx *c) { MMX_STORE(VA_OUT, H(MMXA, (uint64_t)(c->imm % 70u))); }
#define R_XMM2(H) static void r_##H(const Ctx *) { XMM_STORE(VA_OUT, H(XMMA, XMMB)); }

R_MMX2(MMX_PADDB) R_MMX2(MMX_PADDW) R_MMX2(MMX_PADDD)
R_MMX2(MMX_PSUBB) R_MMX2(MMX_PSUBW) R_MMX2(MMX_PSUBD)
R_MMX2(MMX_PADDSB) R_MMX2(MMX_PADDSW) R_MMX2(MMX_PSUBSB) R_MMX2(MMX_PSUBSW)
R_MMX2(MMX_PADDUSB) R_MMX2(MMX_PSUBUSB) R_MMX2(MMX_PADDUSW) R_MMX2(MMX_PSUBUSW)
R_MMX2(MMX_PMULLW) R_MMX2(MMX_PMULHW) R_MMX2(MMX_PMADDWD)
R_MMX2(MMX_PAVGB) R_MMX2(MMX_PAVGW) R_MMX2(MMX_PMINSW) R_MMX2(MMX_PMAXSW)
R_MMX2(MMX_PCMPEQB) R_MMX2(MMX_PCMPEQW) R_MMX2(MMX_PCMPEQD)
R_MMX2(MMX_PCMPGTB) R_MMX2(MMX_PCMPGTW) R_MMX2(MMX_PCMPGTD)
R_MMX2(MMX_PUNPCKLBW) R_MMX2(MMX_PUNPCKHBW) R_MMX2(MMX_PUNPCKLWD)
R_MMX2(MMX_PUNPCKHWD) R_MMX2(MMX_PUNPCKLDQ) R_MMX2(MMX_PUNPCKHDQ)
R_MMX2(MMX_PACKSSWB) R_MMX2(MMX_PACKUSWB) R_MMX2(MMX_PACKSSDW)
R_MMX2(MMX_PSADBW)
R_MMX2(MMX_PAND) R_MMX2(MMX_PANDN) R_MMX2(MMX_POR) R_MMX2(MMX_PXOR)
R_MMXSH(MMX_PSLLW) R_MMXSH(MMX_PSRLW) R_MMXSH(MMX_PSRAW)
R_MMXSH(MMX_PSLLD) R_MMXSH(MMX_PSRLD) R_MMXSH(MMX_PSRAD)
R_MMXSH(MMX_PSLLQ) R_MMXSH(MMX_PSRLQ)
/* Literal shift counts, as generated code passes them (mm0 = MMX_PSRAW(mm0, 4u)):
 * the compiler folds the count into the lane masks, which runtime counts
 * never exercise. */
#define R_SHK(H, K) static void r_##H##_K##K(const Ctx *) { MMX_STORE(VA_OUT, H(MMXA, K##u)); }
#define R_SHK7(H) R_SHK(H, 1) R_SHK(H, 2) R_SHK(H, 4) R_SHK(H, 8) R_SHK(H, 15) R_SHK(H, 16) R_SHK(H, 31)
R_SHK7(MMX_PSLLW) R_SHK7(MMX_PSRLW) R_SHK7(MMX_PSRAW) R_SHK7(MMX_PSLLD) R_SHK7(MMX_PSRLD) R_SHK7(MMX_PSRAD) R_SHK7(MMX_PSLLQ) R_SHK7(MMX_PSRLQ)

static void r_MMX_ZERO(const Ctx *) { MMX_STORE(VA_OUT, MMX_ZERO()); }
static void r_MOVQ_LOAD_STORE(const Ctx *c) { MMX_STORE(VA_OUT, MMX_MEM(VA_A + (c->imm & 7u))); }
static void r_MOVD_TO_MM(const Ctx *) { MMX_STORE(VA_OUT, MMX_FROM32(MEM32(VA_A))); }
/* movd r32, mm and movd m32, mm, as the lifter emits them. */
static void r_MOVD_FROM_MM(const Ctx *) { MEM32(VA_OUT) = RECOMP_MMX_UD(MMXA, 0); }
static void r_MOVD_FROM_MM_GP(const Ctx *) {
    RecompMmx m = MMX_PUNPCKHDQ(MMXA, MMXB);       /* lane 0 = a's high dword */
    uint32_t gp = RECOMP_MMX_UD(m, 0);
    MEM32(VA_OUT) = gp; MEM32(VA_OUT + 4) = RECOMP_MMX_UD(MMXB, 0);
}
static void r_MMX_PSHUFW(const Ctx *c) { MMX_STORE(VA_OUT, MMX_PSHUFW(MMXA, c->imm & 0xFFu)); }
static void r_MMX_PINSRW(const Ctx *c) { MMX_STORE(VA_OUT, MMX_PINSRW(MMXA, MEM32(VA_B), c->imm & 0xFFu)); }
static void r_MMX_PEXTRW(const Ctx *c) { MEM32(VA_OUT) = MMX_PEXTRW(MMXA, c->imm & 0xFFu); }
static void r_MMX_PMOVMSKB(const Ctx *) { MEM32(VA_OUT) = MMX_PMOVMSKB(MMXA); }
/* cvtps2pi / cvttps2pi from m64 and from an xmm register. */
static void r_CVTPS2PI_MEM(const Ctx *) { MMX_STORE(VA_OUT, MMX_FROM_PS(MEMF(VA_A), MEMF((VA_A) + 4), 0)); }
static void r_CVTTPS2PI_MEM(const Ctx *) { MMX_STORE(VA_OUT, MMX_FROM_PS(MEMF(VA_A), MEMF((VA_A) + 4), 1)); }
static void r_CVTPS2PI_XMM(const Ctx *c) {
    RecompXmm x = XMMA;
    MMX_STORE(VA_OUT, MMX_FROM_PS(x.f[0], x.f[1], (int)(c->imm & 1u)));
}
/* cvtpi2ps xmm, mm / m64: low half converted, high half kept. */
static void r_CVTPI2PS(const Ctx *) { XMM_STORE(VA_OUT, XMM_FROM_PI(XMMB, MMXA)); }
static void r_CVTPI2PS_MEM(const Ctx *) { XMM_STORE(VA_OUT, XMM_FROM_PI(XMMB, MMX_MEM(VA_A))); }
/* A short MMX pipeline in the shape of the XMV IDCT/MC inner loops. */
static void r_MMX_PIPELINE(const Ctx *) {
    RecompMmx z = MMX_ZERO(), p = MMXA, q = MMXB, lo, hi, m, s;
    lo = MMX_PUNPCKLBW(p, z); hi = MMX_PUNPCKHBW(p, z);
    m = MMX_PMADDWD(lo, q); s = MMX_PSRAD(m, 3);
    lo = MMX_PADDW(lo, MMX_PSRAW(q, 2)); hi = MMX_PSUBSW(hi, MMX_PSHUFW(q, 0x1Bu));
    p = MMX_PACKUSWB(lo, hi);
    s = MMX_PACKSSDW(s, MMX_PUNPCKHDQ(s, m));
    MMX_STORE(VA_OUT, p);
    MEM32(VA_OUT + 8) = RECOMP_MMX_UD(s, 0);
    MEM32(VA_OUT + 12) = RECOMP_MMX_UD(MMX_PSRLQ(s, 32u), 0);
}

R_XMM2(XMM_ADD) R_XMM2(XMM_SUB) R_XMM2(XMM_MUL) R_XMM2(XMM_DIV)
R_XMM2(XMM_MIN) R_XMM2(XMM_MAX)
R_XMM2(XMM_AND) R_XMM2(XMM_OR) R_XMM2(XMM_XOR) R_XMM2(XMM_ANDN)
R_XMM2(XMM_CMP_EQ) R_XMM2(XMM_CMP_LT) R_XMM2(XMM_CMP_LE) R_XMM2(XMM_CMP_NEQ)
R_XMM2(XMM_SQRT) R_XMM2(XMM_RSQRT) R_XMM2(XMM_RCP)
R_XMM2(XMM_UNPACK_LOW) R_XMM2(XMM_UNPACK_HIGH)
R_XMM2(XMM_MOVE_LOW_TO_HIGH) R_XMM2(XMM_MOVE_HIGH_TO_LOW)

static void r_XMM_ZERO(const Ctx *) { XMM_STORE(VA_OUT, XMM_ZERO()); }
/* Assigned over a register holding other data, as generated code does
 * (xmmN = XMM_SCALAR(...)): lanes 1-3 must come out zero. */
static void r_XMM_SCALAR(const Ctx *) {
    RecompXmm t = XMM_MEM(VA_B);
    t = XMM_SCALAR(MEMF(VA_A));
    XMM_STORE(VA_OUT, t);
}
static void r_XMM_SCALAR_DOUBLE(const Ctx *) { XMM_STORE(VA_OUT, XMM_SCALAR_DOUBLE(MEMD(VA_A))); }
static void r_XMM_SCALAR_BITS(const Ctx *) { XMM_STORE(VA_OUT, XMM_SCALAR_BITS(MEM32(VA_A))); }
static void r_XMM_MEM_STORE(const Ctx *c) { XMM_STORE(VA_OUT, XMM_MEM(VA_A + (c->imm & 15u))); }
static void r_XMM_LOAD_LOW(const Ctx *) { RecompXmm x = XMMB; XMM_LOAD_LOW(x, VA_A); XMM_STORE(VA_OUT, x); }
static void r_XMM_LOAD_HIGH(const Ctx *) { RecompXmm x = XMMB; XMM_LOAD_HIGH(x, VA_A); XMM_STORE(VA_OUT, x); }
static void r_XMM_STORE_LOW(const Ctx *) { XMM_STORE_LOW(VA_OUT, XMMA); }
static void r_XMM_STORE_HIGH(const Ctx *) { XMM_STORE_HIGH(VA_OUT, XMMA); }
static void r_XMM_CMP_PRED(const Ctx *c) { XMM_STORE(VA_OUT, XMM_CMP_PRED(XMMA, XMMB, (int)(c->imm & 7u))); }
static void r_XMM_MOVEMASK(const Ctx *) { MEM32(VA_OUT) = XMM_MOVEMASK(XMMA); }
static void r_XMM_SHUFFLE(const Ctx *c) { XMM_STORE(VA_OUT, XMM_SHUFFLE(XMMA, XMMB, c->imm & 0xFFu)); }
/* Scalar forms, in the lane-0 shape the lifter emits. */
static void r_ADDSS(const Ctx *) { RecompXmm x = XMMA; x.f[0] = x.f[0] + MEMF(VA_B); XMM_STORE(VA_OUT, x); }
static void r_SUBSS(const Ctx *) { RecompXmm x = XMMA; x.f[0] = x.f[0] - MEMF(VA_B); XMM_STORE(VA_OUT, x); }
static void r_MULSS(const Ctx *) { RecompXmm x = XMMA; x.f[0] = x.f[0] * MEMF(VA_B); XMM_STORE(VA_OUT, x); }
static void r_DIVSS(const Ctx *) { RecompXmm x = XMMA; x.f[0] = x.f[0] / MEMF(VA_B); XMM_STORE(VA_OUT, x); }
static void r_CMPSS(const Ctx *c) {
    RecompXmm x = XMMA;
    x.u[0] = recomp_cmp_pred(x.f[0], MEMF(VA_B), (int)(c->imm & 7u)) ? 0xFFFFFFFFu : 0u;
    XMM_STORE(VA_OUT, x);
}
static void r_MOVSS_STORE(const Ctx *) { MEMF(VA_OUT) = XMMA.f[0]; }
static void r_MOVD_FROM_XMM(const Ctx *) { MEM32(VA_OUT) = XMMA.u[0]; }
/* The 64-bit lane accessors (movsd-style double and qword views). */
static void r_XMM_GET_Q(const Ctx *c) {
    uint64_t v = XMM_GET_Q(XMMA, (int)(c->imm & 1u));
    MEM32(VA_OUT) = (uint32_t)v; MEM32(VA_OUT + 4) = (uint32_t)(v >> 32);
}
static void r_XMM_SET_Q(const Ctx *c) {
    RecompXmm x = XMMA;
    XMM_SET_Q(&x, (int)(c->imm & 1u), (uint64_t)MEM32(VA_B) | ((uint64_t)MEM32(VA_B + 4) << 32));
    XMM_STORE(VA_OUT, x);
}
static void r_XMM_GET_D(const Ctx *c) { MEMD(VA_OUT) = XMM_GET_D(XMMA, (int)(c->imm & 1u)); }
static void r_XMM_SET_D(const Ctx *c) {
    RecompXmm x = XMMA; XMM_SET_D(&x, (int)(c->imm & 1u), MEMD(VA_B)); XMM_STORE(VA_OUT, x);
}

/* ---- x86 reference side ------------------------------------------------- */
#if defined(SIMD_LANES_REF)
static __m64 m64(const uint8_t *p) { __m64 v; memcpy(&v, p, 8); return v; }
static __m128 m128(const uint8_t *p) { return _mm_loadu_ps((const float *)p); }
static void put64(Ctx *c, __m64 v) { memcpy(c->out, &v, 8); _mm_empty(); }
static void put128(Ctx *c, __m128 v) { _mm_storeu_ps((float *)c->out, v); }
#define F_MMX2(H, I) static void f_##H(Ctx *c) { put64(c, I(m64(c->in), m64(c->in + 16))); }
#define F_MMXSH(H, I) \
    static void f_##H(Ctx *c) { put64(c, I(m64(c->in), m64(c->in + 32))); } \
    static void f_##H##_I(Ctx *c) { put64(c, I(m64(c->in), _mm_cvtsi32_si64((int)(c->imm % 70u)))); }
#define F_XMM2(H, I) static void f_##H(Ctx *c) { put128(c, I(m128(c->in), m128(c->in + 16))); }
F_MMX2(MMX_PADDB, _mm_add_pi8) F_MMX2(MMX_PADDW, _mm_add_pi16) F_MMX2(MMX_PADDD, _mm_add_pi32)
F_MMX2(MMX_PSUBB, _mm_sub_pi8) F_MMX2(MMX_PSUBW, _mm_sub_pi16) F_MMX2(MMX_PSUBD, _mm_sub_pi32)
F_MMX2(MMX_PADDSB, _mm_adds_pi8) F_MMX2(MMX_PADDSW, _mm_adds_pi16)
F_MMX2(MMX_PSUBSB, _mm_subs_pi8) F_MMX2(MMX_PSUBSW, _mm_subs_pi16)
F_MMX2(MMX_PADDUSB, _mm_adds_pu8) F_MMX2(MMX_PSUBUSB, _mm_subs_pu8)
F_MMX2(MMX_PADDUSW, _mm_adds_pu16) F_MMX2(MMX_PSUBUSW, _mm_subs_pu16)
F_MMX2(MMX_PMULLW, _mm_mullo_pi16) F_MMX2(MMX_PMULHW, _mm_mulhi_pi16) F_MMX2(MMX_PMADDWD, _mm_madd_pi16)
F_MMX2(MMX_PAVGB, _mm_avg_pu8) F_MMX2(MMX_PAVGW, _mm_avg_pu16)
F_MMX2(MMX_PMINSW, _mm_min_pi16) F_MMX2(MMX_PMAXSW, _mm_max_pi16)
F_MMX2(MMX_PCMPEQB, _mm_cmpeq_pi8) F_MMX2(MMX_PCMPEQW, _mm_cmpeq_pi16) F_MMX2(MMX_PCMPEQD, _mm_cmpeq_pi32)
F_MMX2(MMX_PCMPGTB, _mm_cmpgt_pi8) F_MMX2(MMX_PCMPGTW, _mm_cmpgt_pi16) F_MMX2(MMX_PCMPGTD, _mm_cmpgt_pi32)
F_MMX2(MMX_PUNPCKLBW, _mm_unpacklo_pi8) F_MMX2(MMX_PUNPCKHBW, _mm_unpackhi_pi8)
F_MMX2(MMX_PUNPCKLWD, _mm_unpacklo_pi16) F_MMX2(MMX_PUNPCKHWD, _mm_unpackhi_pi16)
F_MMX2(MMX_PUNPCKLDQ, _mm_unpacklo_pi32) F_MMX2(MMX_PUNPCKHDQ, _mm_unpackhi_pi32)
F_MMX2(MMX_PACKSSWB, _mm_packs_pi16) F_MMX2(MMX_PACKUSWB, _mm_packs_pu16) F_MMX2(MMX_PACKSSDW, _mm_packs_pi32)
F_MMX2(MMX_PSADBW, _mm_sad_pu8)
F_MMX2(MMX_PAND, _mm_and_si64) F_MMX2(MMX_PANDN, _mm_andnot_si64)
F_MMX2(MMX_POR, _mm_or_si64) F_MMX2(MMX_PXOR, _mm_xor_si64)
F_MMXSH(MMX_PSLLW, _mm_sll_pi16) F_MMXSH(MMX_PSRLW, _mm_srl_pi16) F_MMXSH(MMX_PSRAW, _mm_sra_pi16)
F_MMXSH(MMX_PSLLD, _mm_sll_pi32) F_MMXSH(MMX_PSRLD, _mm_srl_pi32) F_MMXSH(MMX_PSRAD, _mm_sra_pi32)
F_MMXSH(MMX_PSLLQ, _mm_sll_si64) F_MMXSH(MMX_PSRLQ, _mm_srl_si64)
#define F_SHK(H, I, K) static void f_##H##_K##K(Ctx *c) { put64(c, I(m64(c->in), K)); }
#define F_SHK7(H, I) F_SHK(H, I, 1) F_SHK(H, I, 2) F_SHK(H, I, 4) F_SHK(H, I, 8)     F_SHK(H, I, 15) F_SHK(H, I, 16) F_SHK(H, I, 31)
F_SHK7(MMX_PSLLW, _mm_slli_pi16) F_SHK7(MMX_PSRLW, _mm_srli_pi16) F_SHK7(MMX_PSRAW, _mm_srai_pi16) F_SHK7(MMX_PSLLD, _mm_slli_pi32) F_SHK7(MMX_PSRLD, _mm_srli_pi32) F_SHK7(MMX_PSRAD, _mm_srai_pi32) F_SHK7(MMX_PSLLQ, _mm_slli_si64) F_SHK7(MMX_PSRLQ, _mm_srli_si64)

#define SW4(F, n) F(n) F((n) + 1) F((n) + 2) F((n) + 3)
#define SW16(F, n) SW4(F, n) SW4(F, (n) + 4) SW4(F, (n) + 8) SW4(F, (n) + 12)
#define SW64(F, n) SW16(F, n) SW16(F, (n) + 16) SW16(F, (n) + 32) SW16(F, (n) + 48)
#define SW256(F) SW64(F, 0) SW64(F, 64) SW64(F, 128) SW64(F, 192)

static void f_MMX_ZERO(Ctx *c) { put64(c, _mm_setzero_si64()); }
static void f_MOVQ_LOAD_STORE(Ctx *c) { put64(c, m64(c->in + (c->imm & 7u))); }
static void f_MOVD_TO_MM(Ctx *c) { int v; memcpy(&v, c->in, 4); put64(c, _mm_cvtsi32_si64(v)); }
static void f_MOVD_FROM_MM(Ctx *c) { int v = _mm_cvtsi64_si32(m64(c->in)); _mm_empty(); memcpy(c->out, &v, 4); }
static void f_MOVD_FROM_MM_GP(Ctx *c) {
    int v = _mm_cvtsi64_si32(_mm_unpackhi_pi32(m64(c->in), m64(c->in + 16)));
    int w = _mm_cvtsi64_si32(m64(c->in + 16)); _mm_empty();
    memcpy(c->out, &v, 4); memcpy(c->out + 4, &w, 4);
}
static void f_MMX_PSHUFW(Ctx *c) {
    __m64 a = m64(c->in), r = a;
#define PSHUFW_CASE(n) case (n): r = _mm_shuffle_pi16(a, (n)); break;
    switch (c->imm & 0xFFu) { SW256(PSHUFW_CASE) }
    put64(c, r);
}
static void f_MMX_PINSRW(Ctx *c) {
    __m64 a = m64(c->in), r = a; int v; memcpy(&v, c->in + 16, 4);
    switch (c->imm & 3u) {
    case 0: r = _mm_insert_pi16(a, v, 0); break; case 1: r = _mm_insert_pi16(a, v, 1); break;
    case 2: r = _mm_insert_pi16(a, v, 2); break; default: r = _mm_insert_pi16(a, v, 3); break;
    }
    put64(c, r);
}
static void f_MMX_PEXTRW(Ctx *c) {
    __m64 a = m64(c->in); int v;
    switch (c->imm & 3u) {
    case 0: v = _mm_extract_pi16(a, 0); break; case 1: v = _mm_extract_pi16(a, 1); break;
    case 2: v = _mm_extract_pi16(a, 2); break; default: v = _mm_extract_pi16(a, 3); break;
    }
    _mm_empty(); memcpy(c->out, &v, 4);
}
static void f_MMX_PMOVMSKB(Ctx *c) { int v = _mm_movemask_pi8(m64(c->in)); _mm_empty(); memcpy(c->out, &v, 4); }
static __m128 low2(const uint8_t *p) { return _mm_loadl_pi(_mm_setzero_ps(), (const __m64 *)p); }
static void f_CVTPS2PI_MEM(Ctx *c) { put64(c, _mm_cvtps_pi32(low2(c->in))); }
static void f_CVTTPS2PI_MEM(Ctx *c) { put64(c, _mm_cvttps_pi32(low2(c->in))); }
static void f_CVTPS2PI_XMM(Ctx *c) {
    __m128 x = m128(c->in);
    put64(c, (c->imm & 1u) ? _mm_cvttps_pi32(x) : _mm_cvtps_pi32(x));
}
static void f_CVTPI2PS(Ctx *c) { __m128 r = _mm_cvtpi32_ps(m128(c->in + 16), m64(c->in)); _mm_empty(); put128(c, r); }
static void f_CVTPI2PS_MEM(Ctx *c) { f_CVTPI2PS(c); }
static void f_MMX_PIPELINE(Ctx *c) {
    __m64 z = _mm_setzero_si64(), p = m64(c->in), q = m64(c->in + 16), lo, hi, m, s;
    lo = _mm_unpacklo_pi8(p, z); hi = _mm_unpackhi_pi8(p, z);
    m = _mm_madd_pi16(lo, q); s = _mm_srai_pi32(m, 3);
    lo = _mm_add_pi16(lo, _mm_srai_pi16(q, 2)); hi = _mm_subs_pi16(hi, _mm_shuffle_pi16(q, 0x1B));
    p = _mm_packs_pu16(lo, hi);
    s = _mm_packs_pi32(s, _mm_unpackhi_pi32(s, m));
    int s0 = _mm_cvtsi64_si32(s), s1 = _mm_cvtsi64_si32(_mm_srli_si64(s, 32));
    memcpy(c->out, &p, 8); memcpy(c->out + 8, &s0, 4); memcpy(c->out + 12, &s1, 4);
    _mm_empty();
}
F_XMM2(XMM_ADD, _mm_add_ps) F_XMM2(XMM_SUB, _mm_sub_ps) F_XMM2(XMM_MUL, _mm_mul_ps)
F_XMM2(XMM_DIV, _mm_div_ps) F_XMM2(XMM_MIN, _mm_min_ps) F_XMM2(XMM_MAX, _mm_max_ps)
F_XMM2(XMM_AND, _mm_and_ps) F_XMM2(XMM_OR, _mm_or_ps) F_XMM2(XMM_XOR, _mm_xor_ps)
F_XMM2(XMM_ANDN, _mm_andnot_ps)
F_XMM2(XMM_CMP_EQ, _mm_cmpeq_ps) F_XMM2(XMM_CMP_LT, _mm_cmplt_ps)
F_XMM2(XMM_CMP_LE, _mm_cmple_ps) F_XMM2(XMM_CMP_NEQ, _mm_cmpneq_ps)
F_XMM2(XMM_UNPACK_LOW, _mm_unpacklo_ps) F_XMM2(XMM_UNPACK_HIGH, _mm_unpackhi_ps)
F_XMM2(XMM_MOVE_LOW_TO_HIGH, _mm_movelh_ps) F_XMM2(XMM_MOVE_HIGH_TO_LOW, _mm_movehl_ps)
static void f_XMM_SQRT(Ctx *c) { put128(c, _mm_sqrt_ps(m128(c->in + 16))); }
static void f_XMM_ZERO(Ctx *c) { put128(c, _mm_setzero_ps()); }
static void f_XMM_SCALAR(Ctx *c) { put128(c, _mm_load_ss((const float *)c->in)); }
static void f_XMM_SCALAR_DOUBLE(Ctx *c) { put128(c, _mm_castpd_ps(_mm_load_sd((const double *)c->in))); }
static void f_XMM_SCALAR_BITS(Ctx *c) { int v; memcpy(&v, c->in, 4); put128(c, _mm_castsi128_ps(_mm_cvtsi32_si128(v))); }
static void f_XMM_MEM_STORE(Ctx *c) { put128(c, m128(c->in + (c->imm & 15u))); }
static void f_XMM_LOAD_LOW(Ctx *c) { put128(c, _mm_loadl_pi(m128(c->in + 16), (const __m64 *)c->in)); }
static void f_XMM_LOAD_HIGH(Ctx *c) { put128(c, _mm_loadh_pi(m128(c->in + 16), (const __m64 *)c->in)); }
static void f_XMM_STORE_LOW(Ctx *c) { _mm_storel_pi((__m64 *)c->out, m128(c->in)); }
static void f_XMM_STORE_HIGH(Ctx *c) { _mm_storeh_pi((__m64 *)c->out, m128(c->in)); }
static void f_XMM_CMP_PRED(Ctx *c) {
    __m128 a = m128(c->in), b = m128(c->in + 16), r;
    switch (c->imm & 7u) {
    case 0: r = _mm_cmpeq_ps(a, b); break;  case 1: r = _mm_cmplt_ps(a, b); break;
    case 2: r = _mm_cmple_ps(a, b); break;  case 3: r = _mm_cmpunord_ps(a, b); break;
    case 4: r = _mm_cmpneq_ps(a, b); break; case 5: r = _mm_cmpnlt_ps(a, b); break;
    case 6: r = _mm_cmpnle_ps(a, b); break; default: r = _mm_cmpord_ps(a, b); break;
    }
    put128(c, r);
}
static void f_XMM_MOVEMASK(Ctx *c) { int v = _mm_movemask_ps(m128(c->in)); memcpy(c->out, &v, 4); }
static void f_XMM_SHUFFLE(Ctx *c) {
    __m128 a = m128(c->in), b = m128(c->in + 16), r = a;
#define SHUFPS_CASE(n) case (n): r = _mm_shuffle_ps(a, b, (n)); break;
    switch (c->imm & 0xFFu) { SW256(SHUFPS_CASE) }
    put128(c, r);
}
static __m128 ss_b(const Ctx *c) { return _mm_load_ss((const float *)(c->in + 16)); }
static void f_ADDSS(Ctx *c) { put128(c, _mm_add_ss(m128(c->in), ss_b(c))); }
static void f_SUBSS(Ctx *c) { put128(c, _mm_sub_ss(m128(c->in), ss_b(c))); }
static void f_MULSS(Ctx *c) { put128(c, _mm_mul_ss(m128(c->in), ss_b(c))); }
static void f_DIVSS(Ctx *c) { put128(c, _mm_div_ss(m128(c->in), ss_b(c))); }
static void f_CMPSS(Ctx *c) {
    __m128 a = m128(c->in), b = ss_b(c), r;
    switch (c->imm & 7u) {
    case 0: r = _mm_cmpeq_ss(a, b); break;  case 1: r = _mm_cmplt_ss(a, b); break;
    case 2: r = _mm_cmple_ss(a, b); break;  case 3: r = _mm_cmpunord_ss(a, b); break;
    case 4: r = _mm_cmpneq_ss(a, b); break; case 5: r = _mm_cmpnlt_ss(a, b); break;
    case 6: r = _mm_cmpnle_ss(a, b); break; default: r = _mm_cmpord_ss(a, b); break;
    }
    put128(c, r);
}
static void f_MOVSS_STORE(Ctx *c) { _mm_store_ss((float *)c->out, m128(c->in)); }
static void f_MOVD_FROM_XMM(Ctx *c) { int v = _mm_cvtsi128_si32(_mm_castps_si128(m128(c->in))); memcpy(c->out, &v, 4); }
static void f_XMM_GET_Q(Ctx *c) { memcpy(c->out, c->in + 8 * (c->imm & 1u), 8); }
static void f_XMM_SET_Q(Ctx *c) { memcpy(c->out, c->in, 16); memcpy(c->out + 8 * (c->imm & 1u), c->in + 16, 8); }
static void f_XMM_GET_D(Ctx *c) {
    __m128d v = _mm_loadu_pd((const double *)c->in);
    if (c->imm & 1u) v = _mm_unpackhi_pd(v, v);
    _mm_store_sd((double *)c->out, v);
}
static void f_XMM_SET_D(Ctx *c) {
    __m128d v = _mm_loadu_pd((const double *)c->in), d = _mm_load_sd((const double *)(c->in + 16));
    v = (c->imm & 1u) ? _mm_unpacklo_pd(v, d) : _mm_move_sd(v, d);
    _mm_storeu_pd((double *)c->out, v);
}
#define REF(H) f_##H
#define SIMD_LANES_REF_ON 1
#else
#define REF(H) 0
#define SIMD_LANES_REF_ON 0
#endif

/* ---- case table ---------------------------------------------------------- */
enum { IN_INT, IN_FLT, IN_CVT, IN_CNT };
enum { OUT_INT, OUT_FLT };
struct Case {
    const char *name;
    unsigned char ina, inb, inc, outk;
    void (*run)(const Ctx *);
    void (*ref)(Ctx *);
};
#define C_MMX(H) { #H, IN_INT, IN_INT, IN_INT, OUT_INT, r_##H, REF(H) },
#define C_SH(H) { #H, IN_INT, IN_INT, IN_CNT, OUT_INT, r_##H, REF(H) }, \
                { #H "_IMM", IN_INT, IN_INT, IN_INT, OUT_INT, r_##H##_I, REF(H##_I) },
#define C_SHK(H, K) { #H "_K" #K, IN_INT, IN_INT, IN_INT, OUT_INT, r_##H##_K##K, REF(H##_K##K) },
#define C_SHK7(H) C_SHK(H, 1) C_SHK(H, 2) C_SHK(H, 4) C_SHK(H, 8) C_SHK(H, 15) C_SHK(H, 16) C_SHK(H, 31)
#define C_XF(H) { #H, IN_FLT, IN_FLT, IN_FLT, OUT_FLT, r_##H, REF(H) },
#define C_XI(H) { #H, IN_FLT, IN_FLT, IN_INT, OUT_INT, r_##H, REF(H) },
#define C_NOREF(H, a, b, k) { #H, a, b, IN_INT, k, r_##H, 0 },

static const Case kCases[] = {
    C_MMX(MMX_PADDB) C_MMX(MMX_PADDW) C_MMX(MMX_PADDD)
    C_MMX(MMX_PSUBB) C_MMX(MMX_PSUBW) C_MMX(MMX_PSUBD)
    C_MMX(MMX_PADDSB) C_MMX(MMX_PADDSW) C_MMX(MMX_PSUBSB) C_MMX(MMX_PSUBSW)
    C_MMX(MMX_PADDUSB) C_MMX(MMX_PSUBUSB) C_MMX(MMX_PADDUSW) C_MMX(MMX_PSUBUSW)
    C_MMX(MMX_PMULLW) C_MMX(MMX_PMULHW) C_MMX(MMX_PMADDWD)
    C_MMX(MMX_PAVGB) C_MMX(MMX_PAVGW) C_MMX(MMX_PMINSW) C_MMX(MMX_PMAXSW)
    C_MMX(MMX_PCMPEQB) C_MMX(MMX_PCMPEQW) C_MMX(MMX_PCMPEQD)
    C_MMX(MMX_PCMPGTB) C_MMX(MMX_PCMPGTW) C_MMX(MMX_PCMPGTD)
    C_MMX(MMX_PUNPCKLBW) C_MMX(MMX_PUNPCKHBW) C_MMX(MMX_PUNPCKLWD)
    C_MMX(MMX_PUNPCKHWD) C_MMX(MMX_PUNPCKLDQ) C_MMX(MMX_PUNPCKHDQ)
    C_MMX(MMX_PACKSSWB) C_MMX(MMX_PACKUSWB) C_MMX(MMX_PACKSSDW)
    C_MMX(MMX_PSADBW)
    C_MMX(MMX_PAND) C_MMX(MMX_PANDN) C_MMX(MMX_POR) C_MMX(MMX_PXOR)
    C_SH(MMX_PSLLW) C_SH(MMX_PSRLW) C_SH(MMX_PSRAW)
    C_SH(MMX_PSLLD) C_SH(MMX_PSRLD) C_SH(MMX_PSRAD)
    C_SH(MMX_PSLLQ) C_SH(MMX_PSRLQ)
    C_SHK7(MMX_PSLLW) C_SHK7(MMX_PSRLW) C_SHK7(MMX_PSRAW) C_SHK7(MMX_PSLLD) C_SHK7(MMX_PSRLD) C_SHK7(MMX_PSRAD) C_SHK7(MMX_PSLLQ) C_SHK7(MMX_PSRLQ)
    C_MMX(MMX_ZERO) C_MMX(MOVQ_LOAD_STORE) C_MMX(MOVD_TO_MM)
    C_MMX(MOVD_FROM_MM) C_MMX(MOVD_FROM_MM_GP)
    C_MMX(MMX_PSHUFW) C_MMX(MMX_PINSRW) C_MMX(MMX_PEXTRW) C_MMX(MMX_PMOVMSKB)
    { "CVTPS2PI_MEM", IN_CVT, IN_INT, IN_INT, OUT_INT, r_CVTPS2PI_MEM, REF(CVTPS2PI_MEM) },
    { "CVTTPS2PI_MEM", IN_CVT, IN_INT, IN_INT, OUT_INT, r_CVTTPS2PI_MEM, REF(CVTTPS2PI_MEM) },
    { "CVTPS2PI_XMM", IN_CVT, IN_INT, IN_INT, OUT_INT, r_CVTPS2PI_XMM, REF(CVTPS2PI_XMM) },
    { "CVTPI2PS", IN_INT, IN_FLT, IN_INT, OUT_FLT, r_CVTPI2PS, REF(CVTPI2PS) },
    { "CVTPI2PS_MEM", IN_INT, IN_FLT, IN_INT, OUT_FLT, r_CVTPI2PS_MEM, REF(CVTPI2PS_MEM) },
    C_MMX(MMX_PIPELINE)
    C_XF(XMM_ADD) C_XF(XMM_SUB) C_XF(XMM_MUL) C_XF(XMM_DIV)
    C_XF(XMM_MIN) C_XF(XMM_MAX)
    C_XI(XMM_AND) C_XI(XMM_OR) C_XI(XMM_XOR) C_XI(XMM_ANDN)
    C_XI(XMM_CMP_EQ) C_XI(XMM_CMP_LT) C_XI(XMM_CMP_LE) C_XI(XMM_CMP_NEQ) C_XI(XMM_CMP_PRED)
    C_XF(XMM_SQRT)
    /* rsqrtps/rcpps are hardware approximations; the helpers compute the
     * exact value on purpose, so there is no instruction to check against. */
    C_NOREF(XMM_RSQRT, IN_FLT, IN_FLT, OUT_FLT) C_NOREF(XMM_RCP, IN_FLT, IN_FLT, OUT_FLT)
    C_XI(XMM_MOVEMASK) C_XI(XMM_SHUFFLE) C_XI(XMM_UNPACK_LOW) C_XI(XMM_UNPACK_HIGH)
    C_XI(XMM_MOVE_LOW_TO_HIGH) C_XI(XMM_MOVE_HIGH_TO_LOW)
    C_XI(XMM_ZERO) C_XF(XMM_SCALAR) C_XI(XMM_SCALAR_DOUBLE)
    { "XMM_SCALAR_BITS", IN_INT, IN_INT, IN_INT, OUT_INT, r_XMM_SCALAR_BITS, REF(XMM_SCALAR_BITS) },
    { "XMM_MEM_STORE", IN_INT, IN_INT, IN_INT, OUT_INT, r_XMM_MEM_STORE, REF(XMM_MEM_STORE) },
    C_XI(XMM_LOAD_LOW) C_XI(XMM_LOAD_HIGH) C_XI(XMM_STORE_LOW) C_XI(XMM_STORE_HIGH)
    C_XF(ADDSS) C_XF(SUBSS) C_XF(MULSS) C_XF(DIVSS) C_XI(CMPSS)
    C_XF(MOVSS_STORE) C_XI(MOVD_FROM_XMM)
    { "XMM_GET_Q", IN_INT, IN_INT, IN_INT, OUT_INT, r_XMM_GET_Q, REF(XMM_GET_Q) },
    { "XMM_SET_Q", IN_INT, IN_INT, IN_INT, OUT_INT, r_XMM_SET_Q, REF(XMM_SET_Q) },
    C_XI(XMM_GET_D) C_XI(XMM_SET_D)
};

/* ---- input generation: bytes only, so it is host-independent ------------- */
static uint64_t s_rng;
static uint32_t rnd32(void)
{
    /* xorshift64* */
    s_rng ^= s_rng >> 12; s_rng ^= s_rng << 25; s_rng ^= s_rng >> 27;
    return (uint32_t)((s_rng * 2685821657736338717ULL) >> 32);
}
static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint32_t float_bits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static uint32_t gen_float(void)
{
    static const uint32_t special[] = {
        0x00000000u, 0x80000000u, 0x7F800000u, 0xFF800000u, 0x7FC00000u, 0x3F800000u,
        0xBF800000u, 0x3F000000u, 0x7F7FFFFFu, 0x00800000u, 0x40200000u, 0xC0200000u
    };
    /* One rnd32() per statement: the order C evaluates the operands of one
     * expression in is unspecified, and x86 and x64 MSVC already disagree. */
    uint32_t r = rnd32(), e, m;
    if ((r & 15u) == 0) return special[rnd32() % 12u];
    e = 127u - 20u + rnd32() % 41u;
    m = rnd32() & 0x7FFFFFu;
    return (r & 0x80000000u) | (e << 23) | m;
}
static uint32_t gen_cvt(void)
{
    uint32_t s, e, m;
    switch (rnd32() & 3u) {
    case 0: /* up to 2^40: in and out of int32 range */
        s = rnd32() & 0x80000000u; e = 127u + rnd32() % 41u; m = rnd32() & 0x7FFFFFu;
        return s | (e << 23) | m;
    case 1: /* exact ties, which round to even */
        return float_bits((float)((int32_t)(rnd32() % 2001u) - 1000) + 0.5f);
    case 2:
        s = rnd32() & 0x80000000u; e = 127u - 5u + rnd32() % 26u; m = rnd32() & 0x7FFFFFu;
        return s | (e << 23) | m;
    default:
        return gen_float();
    }
}
static uint8_t gen_byte(void)
{
    uint32_t r = rnd32();
    switch (r & 7u) {
    case 0: return 0x00; case 1: return 0xFF; case 2: return 0x80; case 3: return 0x7F;
    default: return (uint8_t)(r >> 8);
    }
}
static void gen_block(uint8_t *p, int kind)
{
    int i;
    switch (kind) {
    case IN_FLT: for (i = 0; i < 4; i++) put_le32(p + i * 4, gen_float()); break;
    case IN_CVT: for (i = 0; i < 4; i++) put_le32(p + i * 4, gen_cvt()); break;
    case IN_CNT:
        if ((rnd32() & 3u) == 0) { uint32_t lo = rnd32(); put_le32(p, lo); put_le32(p + 4, rnd32()); }
        else { put_le32(p, rnd32() % 70u); put_le32(p + 4, 0); }
        for (i = 8; i < 16; i++) p[i] = gen_byte();
        break;
    default: for (i = 0; i < 16; i++) p[i] = gen_byte(); break;
    }
}
static uint32_t fnv32(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; ++s) { h ^= (uint8_t)*s; h *= 16777619u; }
    return h;
}
static void canon(uint8_t *p, int kind)
{
    int i;
    if (kind != OUT_FLT) return;
    for (i = 0; i < 4; i++) {
        uint32_t v = get_le32(p + i * 4);
        if ((v & 0x7F800000u) == 0x7F800000u && (v & 0x7FFFFFu)) put_le32(p + i * 4, 0x7FC00000u);
    }
}
static void hex(char *dst, const uint8_t *p, int n)
{
    static const char digits[] = "0123456789abcdef";
    int i;
    for (i = 0; i < n; i++) { dst[i * 2] = digits[p[i] >> 4]; dst[i * 2 + 1] = digits[p[i] & 15]; }
    dst[n * 2] = 0;
}

SimdLanesResult simd_lanes_run(SimdLanesLog log, unsigned char *dump,
                               size_t dump_cap, size_t *dump_len)
{
    SimdLanesResult res = { 0, 0, 0, 0 };
    size_t used = 0;
    unsigned k;
    char line[256];
    uint32_t probe = 0x01020304u;
    int host_be = ((const uint8_t *)&probe)[0] == 1;
#if defined(SIMD_LANES_FLAT)
    g_xbox_mem_offset = (ptrdiff_t)((uintptr_t)s_ram - GUEST_BASE);
    const char *mode = "flat";
#else
    xr_guest_bus_init(&s_bus);
    if (xr_guest_map_ram(&s_bus, GUEST_BASE, GUEST_SIZE, s_ram, XR_GUEST_READ | XR_GUEST_WRITE) != XR_GUEST_OK) {
        log("FATAL could not map guest RAM");
        return res;
    }
    g_recomp_guest_bus = &s_bus;
    const char *mode = "explicit-bus";
#endif
    lanes_snprintf(line, sizeof line, "simd_lanes: host %s-endian, RECOMP_HOST_BIG_ENDIAN=%d, %s, %u iterations, ref=%s",
                   host_be ? "big" : "little", (int)RECOMP_HOST_BIG_ENDIAN, mode, (unsigned)SIMD_LANES_ITER,
#if defined(SIMD_LANES_REF)
                   "x86-intrinsics"
#else
                   "none"
#endif
                   );
    log(line);
    if (host_be != RECOMP_HOST_BIG_ENDIAN) log("FAIL RECOMP_HOST_BIG_ENDIAN does not match the host");

#if !defined(SIMD_LANES_FLAT)
    /* Say which path each placement takes, so a run shows the fast path was
     * really exercised (16 bytes at the output, as XMM_STORE resolves them). */
    for (k = 0; k < sizeof kPlace / sizeof kPlace[0]; k++) {
        lanes_snprintf(line, sizeof line, "placement %u (%s): in 0x%05X %s, out 0x%05X %s", k, kPlace[k].name,
                       (unsigned)kPlace[k].in, xr_guest_fast_ptr(kPlace[k].in, 16) ? "fast" : "normal",
                       (unsigned)kPlace[k].out, xr_guest_fast_ptr(kPlace[k].out, 16) ? "fast" : "normal");
        log(line);
    }
#endif
    unsigned place_diff = 0;
    for (k = 0; k < sizeof kCases / sizeof kCases[0]; k++) {
        const Case *cs = &kCases[k];
        uint64_t h = 14695981039346656037ULL, h_main = 0;
        unsigned it, mism = 0, pl;
        int place_bad = 0;
        Ctx c;
        for (pl = 0; pl < sizeof kPlace / sizeof kPlace[0]; pl++) {
        h = 14695981039346656037ULL;
        s_va_in = kPlace[pl].in; s_va_out = kPlace[pl].out;
        s_rng = 0x9E3779B97F4A7C15ULL ^ ((uint64_t)fnv32(cs->name) << 1) ^ 1u;
        for (it = 0; it < SIMD_LANES_ITER; it++) {
            uint8_t got[16];
            int i;
            gen_block(c.in, cs->ina); gen_block(c.in + 16, cs->inb); gen_block(c.in + 32, cs->inc);
            for (i = 0; i < 16; i++) c.o[i] = (uint8_t)rnd32();
            c.imm = rnd32();
            memset(s_ram + (VA_IN - GUEST_BASE) + 48, 0, 16);
            memcpy(s_ram + (VA_IN - GUEST_BASE), c.in, 48);
            memcpy(s_ram + (VA_OUT - GUEST_BASE), c.o, 16);
            cs->run(&c);
            memcpy(got, s_ram + (VA_OUT - GUEST_BASE), 16);
            canon(got, cs->outk);
            for (i = 0; i < 16; i++) { h ^= got[i]; h *= 1099511628211ULL; }
            if (pl) continue;   /* the other placements only compare hashes */
            if (dump && used + 16 <= dump_cap) { memcpy(dump + used, got, 16); used += 16; }
#if defined(SIMD_LANES_REF)
            if (cs->ref) {
                memcpy(c.out, c.o, 16);
                cs->ref(&c);
                canon(c.out, cs->outk);
                if (memcmp(c.out, got, 16) != 0) {
                    if (mism++ < 2) {
                        char hi[97], hg[33], hw[33];
                        hex(hi, c.in, 48); hex(hg, got, 16); hex(hw, c.out, 16);
                        lanes_snprintf(line, sizeof line, "  REF MISMATCH %s it=%u imm=%08X in=%s got=%s want=%s",
                                       cs->name, it, (unsigned)c.imm, hi, hg, hw);
                        log(line);
                    }
                }
            }
#endif
        }
        if (!pl) h_main = h;
        else if (h != h_main) {
            place_bad = 1;
            lanes_snprintf(line, sizeof line, "  PLACEMENT DIFF %s: %s %08X%08X, %s %08X%08X", cs->name,
                           kPlace[0].name, (unsigned)(h_main >> 32), (unsigned)h_main,
                           kPlace[pl].name, (unsigned)(h >> 32), (unsigned)h);
            log(line);
        }
        }
        h = h_main;
        s_va_in = kPlace[0].in; s_va_out = kPlace[0].out;
        if (place_bad) place_diff++;
        if (cs->ref && SIMD_LANES_REF_ON) {
            res.ref_checked++;
            if (mism) res.ref_failed++;
        }
        lanes_snprintf(line, sizeof line, "case %-22s %08X%08X%s%s", cs->name,
                       (unsigned)(h >> 32), (unsigned)h,
                       !(cs->ref && SIMD_LANES_REF_ON) ? "" : mism ? "  ref:FAIL" : "  ref:ok",
                       place_bad ? "  placement:FAIL" : "");
        log(line);
        res.cases++;
    }
    lanes_snprintf(line, sizeof line, "simd_lanes: %u cases, %u checked against x86 instructions, %u ref failures",
                   res.cases, res.ref_checked, res.ref_failed);
    log(line);
    lanes_snprintf(line, sizeof line, "simd_lanes: %u placements per case, %u cases differ between placements",
                   (unsigned)(sizeof kPlace / sizeof kPlace[0]), place_diff);
    log(line);
    res.place_failed = place_diff;
    if (dump_len) *dump_len = used;
    return res;
}

#if defined(SIMD_LANES_MAIN)
static void print_line(const char *s) { puts(s); fflush(stdout); }
int main(int argc, char **argv)
{
    static unsigned char dump[SIMD_LANES_MAX_CASES * SIMD_LANES_ITER * 16];
    size_t len = 0;
    SimdLanesResult r = simd_lanes_run(print_line, dump, sizeof dump, &len);
    if (argc > 1) {
        FILE *f = fopen(argv[1], "wb");
        if (!f || fwrite(dump, 1, len, f) != len) { puts("could not write dump"); return 2; }
        fclose(f);
    }
    return r.ref_failed || r.place_failed ? 1 : 0;
}
#endif
