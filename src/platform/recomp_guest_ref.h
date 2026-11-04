/* C++ lvalue facade for generated scalar memory expressions. Opt-in only.
 * Failure must unwind/stop guest execution; it must never synthesize zero. */
#ifndef RECOMP_GUEST_REF_H
#define RECOMP_GUEST_REF_H
#ifndef __cplusplus
#error Explicit guest memory lvalues require compiling generated code as C++.
#endif
#include "recomp_guest_bus.h"
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
    operator T() const {
        uint64_t bits = 0;
        xr_guest_require(xr_guest_load(g_recomp_guest_bus, address, sizeof(T), &bits), address, sizeof(T));
        return XrGuestCodec<T>::decode(bits);
    }
    T operator=(T value) {
        xr_guest_require(xr_guest_store(g_recomp_guest_bus, address, sizeof(T), XrGuestCodec<T>::encode(value)), address, sizeof(T));
        return value;
    }
    T operator=(const XrGuestRef &other) { return *this = (T)other; }
    template<class U> T operator=(U value) { return *this = (T)value; }
    T operator+=(T n) { return *this = XrGuestMath<T>::add((T)*this, n); }
    T operator-=(T n) { return *this = XrGuestMath<T>::sub((T)*this, n); }
    T operator|=(T n) { return *this = (T)((T)*this | n); }
    T operator&=(T n) { return *this = (T)((T)*this & n); }
    T operator^=(T n) { return *this = (T)((T)*this ^ n); }
    T operator++() { return *this += (T)1; }
    T operator--() { return *this -= (T)1; }
    T operator++(int) { T old = *this; *this += (T)1; return old; }
    T operator--(int) { T old = *this; *this -= (T)1; return old; }
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
