#define NOD3D
#define NONET
#if defined(_XBOX)
#include <xtl.h>
#else
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#include "recomp_guest_atomic_win.h"
#include "recomp_guest_le.h"

static LONG encoded(uint32_t value)
{
    LONG native;
    xr_guest_write32(&native, value);
    return native;
}
static XrGuestStatus target(const XrGuestBus *bus, uint32_t va, uint32_t *old, volatile LONG **out)
{
    void *p;
    if (!old || (va & 3u)) return XR_GUEST_ARGUMENT;
    XrGuestStatus status = xr_guest_ram_span(bus, va, 4, XR_GUEST_READ | XR_GUEST_WRITE, &p);
    if (status != XR_GUEST_OK) return status;
    if ((uintptr_t)p & 3u) return XR_GUEST_ARGUMENT;
    *out = (volatile LONG *)p;
    return XR_GUEST_OK;
}
XrGuestStatus xr_guest_atomic_add32(const XrGuestBus *bus, uint32_t va, uint32_t add, uint32_t *old)
{
    volatile LONG *p;
    XrGuestStatus status = target(bus, va, old, &p);
    if (status != XR_GUEST_OK) return status;
    LONG observed = InterlockedCompareExchange(p, 0, 0);
    for (;;) {
        uint32_t before = xr_guest_read32(&observed);
        LONG actual = InterlockedCompareExchange(p, encoded(before + add), observed);
        if (actual == observed) { *old = before; return XR_GUEST_OK; }
        observed = actual;
    }
}
XrGuestStatus xr_guest_atomic_cas32(const XrGuestBus *bus, uint32_t va, uint32_t compare,
                                  uint32_t replacement, uint32_t *old)
{
    volatile LONG *p;
    XrGuestStatus status = target(bus, va, old, &p);
    if (status != XR_GUEST_OK) return status;
    LONG actual = InterlockedCompareExchange(p, encoded(replacement), encoded(compare));
    *old = xr_guest_read32(&actual);
    return XR_GUEST_OK;
}
