#ifndef XR_GUEST_BUS_CHECKS_H
#define XR_GUEST_BUS_CHECKS_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Generic cases use small synthetic regions. Retail cases use caller-owned
 * independent 64 MiB blocks and write only test patterns, never game data. */
unsigned xr_guest_bus_checks(void (*)(const char *, int, void *), void *, unsigned *);
unsigned xr_guest_retail_checks(void *ram, size_t ram_size, void *contiguous,
    size_t contiguous_size, void (*)(const char *, int, void *), void *, unsigned *);
#ifdef __cplusplus
}
#endif
#endif
