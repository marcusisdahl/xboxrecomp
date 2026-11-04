/* Explicit guest address translation for hosts without a flat guest VA window.
 * Configure mappings before starting guest threads. Backing storage remains
 * caller-owned. RAM accesses are little endian, unaligned-safe and non-atomic.
 * The bus never falls back to casting a guest address into a host pointer. */
#ifndef RECOMP_GUEST_BUS_H
#define RECOMP_GUEST_BUS_H
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XR_GUEST_MAX_REGIONS 64
#define XR_GUEST_READ 1u
#define XR_GUEST_WRITE 2u
#define XR_GUEST_WIDTHS ((1u << 1) | (1u << 2) | (1u << 4) | (1u << 8))
#define XR_GUEST_RETAIL_RAM (64u * 1024u * 1024u)

typedef enum XrGuestStatus {
    XR_GUEST_OK, XR_GUEST_ARGUMENT, XR_GUEST_CAPACITY, XR_GUEST_OVERLAP,
    XR_GUEST_UNMAPPED, XR_GUEST_PERMISSION, XR_GUEST_BOUNDARY,
    XR_GUEST_DEVICE, XR_GUEST_WIDTH
} XrGuestStatus;

/* A callback returns nonzero on success. Values are host integers; the bus
 * does byte-order conversion only for RAM. Width is 1, 2, 4, or 8 bytes. */
typedef int (*XrGuestDeviceRead)(void *, uint32_t, unsigned, uint64_t *);
typedef int (*XrGuestDeviceWrite)(void *, uint32_t, unsigned, uint64_t);

typedef struct XrGuestRegion {
    uint32_t base, size;
    unsigned permissions, widths;
    uint8_t *ram;
    void *context;
    XrGuestDeviceRead read;
    XrGuestDeviceWrite write;
} XrGuestRegion;

typedef struct XrGuestBus {
    unsigned count;
    XrGuestRegion regions[XR_GUEST_MAX_REGIONS];
} XrGuestBus;

void xr_guest_bus_init(XrGuestBus *);
XrGuestStatus xr_guest_map_ram(XrGuestBus *, uint32_t base, uint32_t size,
                               void *backing, unsigned permissions);
XrGuestStatus xr_guest_map_device(XrGuestBus *, uint32_t base, uint32_t size,
    unsigned permissions, unsigned widths, void *context,
    XrGuestDeviceRead, XrGuestDeviceWrite);

/* On failure, output values/pointers and RAM are unchanged. A scalar access
 * may cross adjacent RAM mappings, including the end of a RAM mirror.
 * Device accesses must fit one device region and dispatch once at full width.
 * Failure from a device callback cannot roll back that callback's side effects. */
XrGuestStatus xr_guest_load(const XrGuestBus *, uint32_t, unsigned width, uint64_t *);
XrGuestStatus xr_guest_store(const XrGuestBus *, uint32_t, unsigned width, uint64_t);
XrGuestStatus xr_guest_load_f32(const XrGuestBus *, uint32_t, float *);
XrGuestStatus xr_guest_load_f64(const XrGuestBus *, uint32_t, double *);
XrGuestStatus xr_guest_store_f32(const XrGuestBus *, uint32_t, float);
XrGuestStatus xr_guest_store_f64(const XrGuestBus *, uint32_t, double);
XrGuestStatus xr_guest_bus_load_lanes32(const XrGuestBus *, uint32_t, uint32_t lanes[4]);
XrGuestStatus xr_guest_bus_store_lanes32(const XrGuestBus *, uint32_t, const uint32_t lanes[4]);

/* Borrow a pointer only for a single contiguous RAM mapping. Devices and
 * spans crossing region boundaries return XR_GUEST_BOUNDARY. The caller must
 * honor permissions and keep storage alive; guest data is still little endian. */
XrGuestStatus xr_guest_ram_span(const XrGuestBus *, uint32_t, uint32_t bytes,
                                unsigned access, void **);

/* Match the existing toolkit's retail hosting layout: low 64 MiB plus 28
 * mirrors; a separate 64 MiB contiguous window at 0x80000000; and its tiled
 * alias at 0xF0000000. The separation from low RAM is a runtime convention.
 * No GPU/APU/USB/register behavior is installed. Failure leaves bus unchanged. */
XrGuestStatus xr_guest_xbox_retail_map(XrGuestBus *, void *ram, size_t ram_size,
                                      void *contiguous, size_t contiguous_size);
const char *xr_guest_status_name(XrGuestStatus);

#ifdef __cplusplus
}
#endif
#endif
