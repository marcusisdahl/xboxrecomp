/* Little-endian guest byte access, independent of host byte order/alignment.
 * These are ordinary RAM accesses, not atomic operations or MMIO dispatch.
 * Callers must resolve and bounds-check the entire span before calling.
 * Kept separate from recomp_types.h until generated lvalue accesses and the
 * runtime's direct guest casts have a complete big-endian implementation. */
#ifndef RECOMP_GUEST_LE_H
#define RECOMP_GUEST_LE_H

#include <stdint.h>
#include <string.h>
#include <limits.h>

#if defined(_MSC_VER)
#define XR_GUEST_INLINE static __inline
#else
#define XR_GUEST_INLINE static inline
#endif

typedef char xr_guest_require_octets[(CHAR_BIT == 8) ? 1 : -1];
typedef char xr_guest_require_float32[(sizeof(float) == 4) ? 1 : -1];
typedef char xr_guest_require_float64[(sizeof(double) == 8) ? 1 : -1];

XR_GUEST_INLINE uint8_t xr_guest_read8(const volatile void *address)
{
    return *(const volatile uint8_t *)address;
}

XR_GUEST_INLINE uint16_t xr_guest_read16(const volatile void *address)
{
    const volatile uint8_t *p = (const volatile uint8_t *)address;
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

XR_GUEST_INLINE uint32_t xr_guest_read32(const volatile void *address)
{
    const volatile uint8_t *p = (const volatile uint8_t *)address;
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
        ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

XR_GUEST_INLINE uint64_t xr_guest_read64(const volatile void *address)
{
    const volatile uint8_t *p = (const volatile uint8_t *)address;
    return (uint64_t)xr_guest_read32(p) |
        ((uint64_t)xr_guest_read32(p + 4) << 32);
}

XR_GUEST_INLINE void xr_guest_write8(volatile void *address, uint8_t value)
{
    *(volatile uint8_t *)address = value;
}

XR_GUEST_INLINE void xr_guest_write16(volatile void *address, uint16_t value)
{
    volatile uint8_t *p = (volatile uint8_t *)address;
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

XR_GUEST_INLINE void xr_guest_write32(volatile void *address, uint32_t value)
{
    volatile uint8_t *p = (volatile uint8_t *)address;
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

XR_GUEST_INLINE void xr_guest_write64(volatile void *address, uint64_t value)
{
    volatile uint8_t *p = (volatile uint8_t *)address;
    xr_guest_write32(p, (uint32_t)value);
    xr_guest_write32(p + 4, (uint32_t)(value >> 32));
}

XR_GUEST_INLINE int8_t xr_guest_read_s8(const volatile void *address)
{
    uint8_t u = xr_guest_read8(address);
    return (int8_t)(u < 0x80u ? (int)u : -1 - (int)(uint8_t)~u);
}

XR_GUEST_INLINE int16_t xr_guest_read_s16(const volatile void *address)
{
    uint16_t u = xr_guest_read16(address);
    return (int16_t)(u < 0x8000u ? (int)u : -1 - (int)(uint16_t)~u);
}

XR_GUEST_INLINE int32_t xr_guest_read_s32(const volatile void *address)
{
    uint32_t u = xr_guest_read32(address);
    return u <= 0x7fffffffu ? (int32_t)u : -1 - (int32_t)~u;
}

XR_GUEST_INLINE int64_t xr_guest_read_s64(const volatile void *address)
{
    uint64_t u = xr_guest_read64(address);
    return (u >> 63) == 0 ? (int64_t)u : -1 - (int64_t)~u;
}

/* Host integer and float representations have the same host byte order.
 * memcpy transfers the bits without alignment or strict-aliasing violations.
 * IEEE-754 bit patterns are checked by the host/console conformance tests. */
XR_GUEST_INLINE float xr_guest_read_f32(const volatile void *address)
{
    uint32_t bits = xr_guest_read32(address);
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

XR_GUEST_INLINE double xr_guest_read_f64(const volatile void *address)
{
    uint64_t bits = xr_guest_read64(address);
    double value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

XR_GUEST_INLINE void xr_guest_write_f32(volatile void *address, float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    xr_guest_write32(address, bits);
}

XR_GUEST_INLINE void xr_guest_write_f64(volatile void *address, double value)
{
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    xr_guest_write64(address, bits);
}

/* Architectural lane order: lane 0 is the lowest guest address. This does
 * not implement the runtime's entire SSE/MMX register union or arithmetic. */
XR_GUEST_INLINE void xr_guest_read_lanes32(const volatile void *address,
                                         uint32_t lanes[4])
{
    const volatile uint8_t *p = (const volatile uint8_t *)address;
    unsigned i;
    for (i = 0; i < 4; ++i) lanes[i] = xr_guest_read32(p + i * 4);
}

XR_GUEST_INLINE void xr_guest_write_lanes32(volatile void *address,
                                          const uint32_t lanes[4])
{
    volatile uint8_t *p = (volatile uint8_t *)address;
    unsigned i;
    for (i = 0; i < 4; ++i) xr_guest_write32(p + i * 4, lanes[i]);
}

#undef XR_GUEST_INLINE
#endif
