#ifndef RECOMP_GUEST_ATOMIC_WIN_H
#define RECOMP_GUEST_ATOMIC_WIN_H
#include "recomp_guest_bus.h"
/* Aligned 32-bit RAM only. Mutually atomic with other calls to these APIs.
 * Concurrent ordinary bytewise bus loads/stores are not made atomic here.
 * No unaligned, split-region, or device operations are emulated. */
XrGuestStatus xr_guest_atomic_add32(const XrGuestBus *, uint32_t, uint32_t, uint32_t *old);
XrGuestStatus xr_guest_atomic_cas32(const XrGuestBus *, uint32_t, uint32_t compare,
                                  uint32_t replacement, uint32_t *old);
#endif
