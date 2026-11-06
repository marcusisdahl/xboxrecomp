/* C++ lvalue facade for generated scalar memory expressions. Opt-in only.
 * Failure must unwind/stop guest execution; it must never synthesize zero. */
#ifndef RECOMP_GUEST_REF_H
#define RECOMP_GUEST_REF_H
#ifndef __cplusplus
#error Explicit guest memory lvalues require compiling generated code as C++.
#endif
#include "recomp_guest_bus.h"
#include "recomp_guest_le.h"
#include <stdlib.h>
#include <string.h>
#if defined(_MSC_VER)
#define XR_REF_TLS __declspec(thread)
#else
#define XR_REF_TLS __thread
#endif
extern XR_REF_TLS XrGuestBus *g_recomp_guest_bus;
void xr_guest_access_fault(XrGuestStatus, uint32_t, unsigned);

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable:4702) /* abort is intentionally a last-resort guard. */
#endif
inline void xr_guest_require(XrGuestStatus status, uint32_t address, unsigned width)
{
    if (status != XR_GUEST_OK) {
        xr_guest_access_fault(status, address, width);
        abort(); /* A mistakenly returning fault handler cannot resume a guest. */
    }
}
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

template<unsigned N> struct XrGuestStorage;
template<> struct XrGuestStorage<1> { typedef uint8_t Type; };
template<> struct XrGuestStorage<2> { typedef uint16_t Type; };
template<> struct XrGuestStorage<4> { typedef uint32_t Type; };
template<> struct XrGuestStorage<8> { typedef uint64_t Type; };

/* ---- RAM fast path -------------------------------------------------
 * The common access -- plain read/write RAM, starting in a 4 KiB page the
 * fast table lists for the bus it was built for (recomp_guest_bus.h) -- is a
 * direct little-endian load or store. Everything else (devices, the host's
 * watch pages, unmapped or read-only memory, accesses that start in the last
 * page before any of those, on big-endian hosts any access that crosses a
 * page boundary (XR_GUEST_FAST_PAGE_CROSS), another bus, no bus) takes xr_guest_load/
 * xr_guest_store out of line, unchanged. The accesses are volatile like the
 * bytewise RAM path's (guest spin loops re-read memory) and need no alignment.
 *
 * Inlining: the primitives below are forced inline, but the per-width access
 * functions (xr_guest_ref_load_fn/xr_guest_ref_store_fn) are plain inline,
 * so the compiler inlines them into ordinary functions and calls them once a
 * function has used up its inlining budget (the call still takes the fast
 * path, with the address in a register). Forcing every access inline made
 * the Xbox 360 compiler's optimisation time explode on the largest generated
 * functions (one 4200-instruction function: 14 s -> over 20 minutes; the
 * whole game library: 4 -> 50 minutes). XR_GUEST_REF_OUTLINE makes every
 * access such a call. */
#if defined(_MSC_VER)
#define XR_REF_NOINLINE __declspec(noinline)
#ifndef XR_REF_INLINE
#define XR_REF_INLINE __forceinline
#endif
#else
#define XR_REF_NOINLINE __attribute__((noinline))
#ifndef XR_REF_INLINE
#define XR_REF_INLINE inline __attribute__((always_inline))
#endif
#endif

/* The fast-path test: page is xr_guest_fast_page[va >> 12], on the bus the
 * table describes. A listed page is followed by contiguous RAM, so `bytes`
 * (1..XR_GUEST_FAST_PAGE_SIZE) at va need no bounds check even when they
 * reach into the next page -- but see XR_GUEST_FAST_PAGE_CROSS below for
 * hosts where such an access must not be issued as one wide access.
 *
 * XR_GUEST_FAST_ONE_BUS (opt-in, define it for every unit that includes
 * this header) drops the per-access check of the thread's bus: a host that
 * runs generated code only on threads whose g_recomp_guest_bus is the
 * table's owner gets the same results with a shorter inline sequence. A
 * thread with another bus, or none, would then reach the owner's RAM
 * instead of its own mappings or a fault, so hosts with several buses must
 * leave it undefined. */
#if defined(XR_GUEST_FAST_ONE_BUS)
#define XR_GUEST_FAST_BUS_OK() 1
#else
#define XR_GUEST_FAST_BUS_OK() (g_recomp_guest_bus == xr_guest_fast_bus)
#endif
#define XR_GUEST_FAST_AT(page, va) ((page) + ((va) & (XR_GUEST_FAST_PAGE_SIZE - 1)))

#if defined(_XBOX) && !defined(_M_IX86) && !defined(_M_X64)
/* Xbox 360 (big-endian PowerPC): lhbrx/lwbrx/sthbrx/stwbrx. Xenon handles
 * an unaligned cacheable integer access in hardware while it stays inside
 * one 4 KiB page (measured: an unaligned load32 that crosses a 128-byte
 * cache line costs about 20 ns more, nothing otherwise), but one that
 * crosses a page takes an alignment trap (about 170 ns). Such accesses are
 * kept off the fast path (XR_GUEST_FAST_PAGE_CROSS 0, below). */
#include <ppcintrinsics.h>
#define XR_REF_LD16(p) ((uint16_t)__loadvolatileshortbytereverse(0, (p)))
#define XR_REF_LD32(p) ((uint32_t)__loadvolatilewordbytereverse(0, (p)))
#define XR_REF_ST16(p, v) __storevolatileshortbytereverse((unsigned short)(v), 0, (p))
#define XR_REF_ST32(p, v) __storevolatilewordbytereverse((unsigned int)(v), 0, (p))
#elif defined(_M_IX86) || defined(_M_X64) || defined(_M_ARM64) || defined(_M_ARM) || \
    defined(__i386__) || defined(__x86_64__) || \
    (defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
/* Little-endian host: guest bytes are host order already. */
#if defined(_MSC_VER)
typedef uint16_t XrRefU16; typedef uint32_t XrRefU32; typedef uint64_t XrRefU64;
#if defined(_M_ARM64) || defined(_M_ARM)
#define XR_REF_UA __unaligned
#else
#define XR_REF_UA
#endif
#else
typedef uint16_t XrRefU16 __attribute__((aligned(1), may_alias));
typedef uint32_t XrRefU32 __attribute__((aligned(1), may_alias));
typedef uint64_t XrRefU64 __attribute__((aligned(1), may_alias));
#define XR_REF_UA
#endif
#define XR_REF_LE_HOST 1
#define XR_REF_LD16(p) (*(const volatile XR_REF_UA XrRefU16 *)(p))
#define XR_REF_LD32(p) (*(const volatile XR_REF_UA XrRefU32 *)(p))
#define XR_REF_LD64(p) (*(const volatile XR_REF_UA XrRefU64 *)(p))
#define XR_REF_ST16(p, v) (void)(*(volatile XR_REF_UA XrRefU16 *)(p) = (uint16_t)(v))
#define XR_REF_ST32(p, v) (void)(*(volatile XR_REF_UA XrRefU32 *)(p) = (uint32_t)(v))
#define XR_REF_ST64(p, v) (void)(*(volatile XR_REF_UA XrRefU64 *)(p) = (uint64_t)(v))
#else
/* Any other host: the portable bytewise little-endian codec. */
#define XR_REF_LD16(p) xr_guest_read16(p)
#define XR_REF_LD32(p) xr_guest_read32(p)
#define XR_REF_ST16(p, v) xr_guest_write16((p), (uint16_t)(v))
#define XR_REF_ST32(p, v) xr_guest_write32((p), (uint32_t)(v))
#endif
#if !defined(XR_REF_LD64)
/* Two dwords, low address = low half, as the bytewise path assembles it. */
#define XR_REF_LD64(p) ((uint64_t)XR_REF_LD32(p) | ((uint64_t)XR_REF_LD32((p) + 4) << 32))
#define XR_REF_ST64(p, v) (XR_REF_ST32((p), (uint32_t)(v)), XR_REF_ST32((p) + 4, (uint32_t)((uint64_t)(v) >> 32)))
#endif

/* XR_GUEST_FAST_PAGE_CROSS: 1 lets an access that starts in a listed page
 * and ends in the next one take the fast path (one wide access across the
 * page boundary: fine on x86/x64/ARM); 0 sends it to the normal, bytewise
 * path, so no wide access ever crosses a page. Default: 1 on little-endian
 * hosts (unchanged behaviour), 0 elsewhere (the Xbox 360, see above). The
 * test is then one more compare per access: va's offset in its page is at
 * most 4096 - bytes. A host may define it either way. */
#if !defined(XR_GUEST_FAST_PAGE_CROSS)
#if defined(XR_REF_LE_HOST)
#define XR_GUEST_FAST_PAGE_CROSS 1
#else
#define XR_GUEST_FAST_PAGE_CROSS 0
#endif
#endif
#if XR_GUEST_FAST_PAGE_CROSS
#define XR_GUEST_FAST_SPAN_OK(va, bytes) 1
#else
#define XR_GUEST_FAST_SPAN_OK(va, bytes) \
    ((bytes) == 1 || ((va) & (XR_GUEST_FAST_PAGE_SIZE - 1)) <= XR_GUEST_FAST_PAGE_SIZE - (bytes))
#endif
#define XR_GUEST_FAST_OK(page, va, bytes) ((page) && XR_GUEST_FAST_SPAN_OK(va, bytes) && XR_GUEST_FAST_BUS_OK())

/* Host pointer for `bytes` (1..XR_GUEST_FAST_PAGE_SIZE) at va, or NULL for
 * the normal path. */
XR_REF_INLINE uint8_t *xr_guest_fast_ptr(uint32_t va, unsigned bytes)
{
    uint8_t *page = xr_guest_fast_page[va >> XR_GUEST_FAST_PAGE_SHIFT];
    (void)bytes;
    return XR_GUEST_FAST_OK(page, va, bytes) ? XR_GUEST_FAST_AT(page, va) : 0;
}

template<unsigned N> struct XrGuestRaw;
template<> struct XrGuestRaw<1> {
    static XR_REF_INLINE uint64_t load(uint8_t *p) { return *(volatile uint8_t *)p; }
    static XR_REF_INLINE void store(uint8_t *p, uint64_t v) { *(volatile uint8_t *)p = (uint8_t)v; }
};
template<> struct XrGuestRaw<2> {
    static XR_REF_INLINE uint64_t load(uint8_t *p) { return XR_REF_LD16(p); }
    static XR_REF_INLINE void store(uint8_t *p, uint64_t v) { XR_REF_ST16(p, v); }
};
template<> struct XrGuestRaw<4> {
    static XR_REF_INLINE uint64_t load(uint8_t *p) { return XR_REF_LD32(p); }
    static XR_REF_INLINE void store(uint8_t *p, uint64_t v) { XR_REF_ST32(p, v); }
};
template<> struct XrGuestRaw<8> {
    static XR_REF_INLINE uint64_t load(uint8_t *p) { return XR_REF_LD64(p); }
    static XR_REF_INLINE void store(uint8_t *p, uint64_t v) { XR_REF_ST64(p, v); }
};

/* The normal path, kept out of line so each inline access stays small. */
XR_REF_NOINLINE inline uint64_t xr_guest_ref_load_slow(uint32_t va, unsigned width)
{
    uint64_t bits = 0;
    xr_guest_require(xr_guest_load(g_recomp_guest_bus, va, width, &bits), va, width);
    return bits;
}
XR_REF_NOINLINE inline void xr_guest_ref_store_slow(uint32_t va, unsigned width, uint64_t bits)
{
    xr_guest_require(xr_guest_store(g_recomp_guest_bus, va, width, bits), va, width);
}

/* One access of N bytes: the fast path, else the normal one. */
#if defined(XR_GUEST_REF_OUTLINE)
#define XR_REF_ACCESS_FN XR_REF_NOINLINE
#else
#define XR_REF_ACCESS_FN inline
#endif
template<unsigned N> XR_REF_ACCESS_FN uint64_t xr_guest_ref_load_fn(uint32_t va)
{
    uint8_t *page = xr_guest_fast_page[va >> XR_GUEST_FAST_PAGE_SHIFT];
    if (XR_GUEST_FAST_OK(page, va, N)) return XrGuestRaw<N>::load(XR_GUEST_FAST_AT(page, va));
    return xr_guest_ref_load_slow(va, N);
}
template<unsigned N> XR_REF_ACCESS_FN void xr_guest_ref_store_fn(uint32_t va, uint64_t bits)
{
    uint8_t *page = xr_guest_fast_page[va >> XR_GUEST_FAST_PAGE_SHIFT];
    if (XR_GUEST_FAST_OK(page, va, N)) XrGuestRaw<N>::store(XR_GUEST_FAST_AT(page, va), bits);
    else xr_guest_ref_store_slow(va, N, bits);
}

/* A 32-bit RAM load/store for callers that already hold a fast pointer
 * (XMM_MEM/XMM_STORE resolve all 16 bytes with one lookup). */
XR_REF_INLINE uint32_t xr_guest_raw_load32(uint8_t *p) { return (uint32_t)XrGuestRaw<4>::load(p); }
XR_REF_INLINE void xr_guest_raw_store32(uint8_t *p, uint32_t v) { XrGuestRaw<4>::store(p, v); }
template<class T> struct XrGuestCodec {
    typedef typename XrGuestStorage<sizeof(T)>::Type Bits;
    static T decode(uint64_t bits) {
        T value; Bits n = (Bits)bits;
        memcpy(&value, &n, sizeof(T));
        return value;
    }
    static uint64_t encode(T value) {
        Bits n; memcpy(&n, &value, sizeof(T)); return n;
    }
};
template<class T> struct XrGuestMath {
    static T add(T a, T b) { return XrGuestCodec<T>::decode(XrGuestCodec<T>::encode(a) + XrGuestCodec<T>::encode(b)); }
    static T sub(T a, T b) { return XrGuestCodec<T>::decode(XrGuestCodec<T>::encode(a) - XrGuestCodec<T>::encode(b)); }
};
template<> struct XrGuestMath<float> {
    static float add(float a, float b) { return a + b; }
    static float sub(float a, float b) { return a - b; }
};
template<> struct XrGuestMath<double> {
    static double add(double a, double b) { return a + b; }
    static double sub(double a, double b) { return a - b; }
};

template<class T> class XrGuestRef {
    uint32_t address;
    XrGuestRef *operator&(); /* Native pointers to proxy temporaries are invalid. */
public:
    explicit XrGuestRef(uint32_t va) : address(va) {}
    XR_REF_INLINE operator T() const {
        return XrGuestCodec<T>::decode(xr_guest_ref_load_fn<sizeof(T)>(address));
    }
    XR_REF_INLINE T operator=(T value) {
        xr_guest_ref_store_fn<sizeof(T)>(address, XrGuestCodec<T>::encode(value));
        return value;
    }
    /* The forwarding operators are forced inline too: left to the compiler,
     * the Xbox 360 build kept some as out-of-line calls in big functions
     * (a call that only forwards to the inline load and store). */
    XR_REF_INLINE T operator=(const XrGuestRef &other) { return *this = (T)other; }
    template<class U> XR_REF_INLINE T operator=(U value) { return *this = (T)value; }
    XR_REF_INLINE T operator+=(T n) { return *this = XrGuestMath<T>::add((T)*this, n); }
    XR_REF_INLINE T operator-=(T n) { return *this = XrGuestMath<T>::sub((T)*this, n); }
    XR_REF_INLINE T operator|=(T n) { return *this = (T)((T)*this | n); }
    XR_REF_INLINE T operator&=(T n) { return *this = (T)((T)*this & n); }
    XR_REF_INLINE T operator^=(T n) { return *this = (T)((T)*this ^ n); }
    XR_REF_INLINE T operator++() { return *this += (T)1; }
    XR_REF_INLINE T operator--() { return *this -= (T)1; }
    XR_REF_INLINE T operator++(int) { T old = *this; *this += (T)1; return old; }
    XR_REF_INLINE T operator--(int) { T old = *this; *this -= (T)1; return old; }
};

#define XR_MEM8(a) XrGuestRef<uint8_t>((uint32_t)(a))
#define XR_MEM16(a) XrGuestRef<uint16_t>((uint32_t)(a))
#define XR_MEM32(a) XrGuestRef<uint32_t>((uint32_t)(a))
#define XR_SMEM8(a) XrGuestRef<int8_t>((uint32_t)(a))
#define XR_SMEM16(a) XrGuestRef<int16_t>((uint32_t)(a))
#define XR_SMEM32(a) XrGuestRef<int32_t>((uint32_t)(a))
#define XR_SMEM64(a) XrGuestRef<int64_t>((uint32_t)(a))
#define XR_MEMF(a) XrGuestRef<float>((uint32_t)(a))
#define XR_MEMD(a) XrGuestRef<double>((uint32_t)(a))
#endif
