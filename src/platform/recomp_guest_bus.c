#include "recomp_guest_bus.h"
#include "recomp_guest_le.h"

static int range_ok(uint32_t address, uint32_t bytes)
{
    return bytes != 0 && (uint64_t)address + bytes <= (UINT64_C(1) << 32);
}

static const XrGuestRegion *find_region(const XrGuestBus *bus, uint32_t address)
{
    unsigned i;
    for (i = 0; i < bus->count; ++i) {
        const XrGuestRegion *r = &bus->regions[i];
        if (address >= r->base && address - r->base < r->size) return r;
    }
    return NULL;
}

void xr_guest_bus_init(XrGuestBus *bus)
{
    if (bus) memset(bus, 0, sizeof(*bus));
}

static XrGuestStatus add_region(XrGuestBus *bus, const XrGuestRegion *region)
{
    unsigned i;
    uint64_t end = (uint64_t)region->base + region->size;
    if (!bus || !range_ok(region->base, region->size) || !region->permissions ||
        (region->permissions & ~(XR_GUEST_READ | XR_GUEST_WRITE))) return XR_GUEST_ARGUMENT;
    for (i = 0; i < bus->count; ++i) {
        const XrGuestRegion *r = &bus->regions[i];
        if ((uint64_t)region->base < (uint64_t)r->base + r->size && r->base < end)
            return XR_GUEST_OVERLAP;
    }
    if (bus->count == XR_GUEST_MAX_REGIONS) return XR_GUEST_CAPACITY;
    bus->regions[bus->count++] = *region;
    return XR_GUEST_OK;
}

XrGuestStatus xr_guest_map_ram(XrGuestBus *bus, uint32_t base, uint32_t size,
                               void *backing, unsigned permissions)
{
    XrGuestRegion r;
    if (!backing) return XR_GUEST_ARGUMENT;
    memset(&r, 0, sizeof(r));
    r.base = base; r.size = size; r.permissions = permissions;
    r.ram = (uint8_t *)backing;
    return add_region(bus, &r);
}

XrGuestStatus xr_guest_map_device(XrGuestBus *bus, uint32_t base, uint32_t size,
    unsigned permissions, unsigned widths, void *context,
    XrGuestDeviceRead read, XrGuestDeviceWrite write)
{
    XrGuestRegion r;
    if (!widths || (widths & ~XR_GUEST_WIDTHS) ||
        ((permissions & XR_GUEST_READ) && !read) ||
        ((permissions & XR_GUEST_WRITE) && !write)) return XR_GUEST_ARGUMENT;
    memset(&r, 0, sizeof(r));
    r.base = base; r.size = size; r.permissions = permissions;
    r.widths = widths; r.context = context; r.read = read; r.write = write;
    return add_region(bus, &r);
}

/* Validate the entire RAM span before any load/store. Scalar/lane accesses
 * need at most 16 fragments even when every byte is a separate mapping. */
typedef struct Fragment { uint8_t *ram; unsigned bytes; } Fragment;
static XrGuestStatus ram_plan(const XrGuestBus *bus, uint32_t address,
    unsigned bytes, unsigned access, Fragment pieces[16], unsigned *count)
{
    unsigned used = 0, done = 0;
    if (!bus || bytes > 16 || !range_ok(address, bytes)) return XR_GUEST_ARGUMENT;
    while (done < bytes) {
        const XrGuestRegion *r = find_region(bus, address + done);
        uint32_t offset, available;
        unsigned take;
        if (!r) return XR_GUEST_UNMAPPED;
        if ((r->permissions & access) != access) return XR_GUEST_PERMISSION;
        if (!r->ram) return XR_GUEST_BOUNDARY;
        offset = address + done - r->base;
        available = r->size - offset;
        take = available < bytes - done ? available : bytes - done;
        pieces[used].ram = r->ram + offset;
        pieces[used++].bytes = take;
        done += take;
    }
    *count = used;
    return XR_GUEST_OK;
}

static void gather(const Fragment *pieces, unsigned count, uint8_t *bytes)
{
    unsigned i, j, done = 0;
    for (i = 0; i < count; ++i)
        for (j = 0; j < pieces[i].bytes; ++j)
            bytes[done++] = xr_guest_read8(pieces[i].ram + j);
}

static void scatter(const Fragment *pieces, unsigned count, const uint8_t *bytes)
{
    unsigned i, j, done = 0;
    for (i = 0; i < count; ++i)
        for (j = 0; j < pieces[i].bytes; ++j)
            xr_guest_write8(pieces[i].ram + j, bytes[done++]);
}

static int scalar_width(unsigned width)
{
    return width == 1 || width == 2 || width == 4 || width == 8;
}

static uint64_t mask_value(uint64_t value, unsigned width)
{
    return width == 8 ? value : value & ((UINT64_C(1) << (width * 8)) - 1);
}

static XrGuestStatus device_check(const XrGuestRegion *r, uint32_t address,
                                 unsigned width, unsigned access)
{
    if ((r->permissions & access) != access) return XR_GUEST_PERMISSION;
    if (width > r->size - (address - r->base)) return XR_GUEST_BOUNDARY;
    if (!(r->widths & (1u << width))) return XR_GUEST_WIDTH;
    return XR_GUEST_OK;
}

XrGuestStatus xr_guest_load(const XrGuestBus *bus, uint32_t address,
                           unsigned width, uint64_t *value)
{
    const XrGuestRegion *r;
    Fragment pieces[16];
    unsigned count;
    uint8_t bytes[8];
    uint64_t result;
    XrGuestStatus status;
    if (!bus || !value || !scalar_width(width) || !range_ok(address, width)) return XR_GUEST_ARGUMENT;
    r = find_region(bus, address);
    if (!r) return XR_GUEST_UNMAPPED;
    if (!r->ram) {
        status = device_check(r, address, width, XR_GUEST_READ);
        if (status != XR_GUEST_OK) return status;
        if (!r->read(r->context, address, width, &result)) return XR_GUEST_DEVICE;
    } else {
        status = ram_plan(bus, address, width, XR_GUEST_READ, pieces, &count);
        if (status != XR_GUEST_OK) return status;
        gather(pieces, count, bytes);
        result = width == 8 ? xr_guest_read64(bytes) : width == 4 ? xr_guest_read32(bytes) :
            width == 2 ? xr_guest_read16(bytes) : xr_guest_read8(bytes);
    }
    *value = mask_value(result, width);
    return XR_GUEST_OK;
}

XrGuestStatus xr_guest_store(const XrGuestBus *bus, uint32_t address,
                            unsigned width, uint64_t value)
{
    const XrGuestRegion *r;
    Fragment pieces[16];
    unsigned count, i;
    uint8_t bytes[8];
    XrGuestStatus status;
    if (!bus || !scalar_width(width) || !range_ok(address, width)) return XR_GUEST_ARGUMENT;
    r = find_region(bus, address);
    if (!r) return XR_GUEST_UNMAPPED;
    if (!r->ram) {
        status = device_check(r, address, width, XR_GUEST_WRITE);
        if (status != XR_GUEST_OK) return status;
        return r->write(r->context, address, width, mask_value(value, width)) ? XR_GUEST_OK : XR_GUEST_DEVICE;
    }
    status = ram_plan(bus, address, width, XR_GUEST_WRITE, pieces, &count);
    if (status != XR_GUEST_OK) return status;
    for (i = 0; i < width; ++i) bytes[i] = (uint8_t)(value >> (i * 8));
    scatter(pieces, count, bytes);
    return XR_GUEST_OK;
}

XrGuestStatus xr_guest_load_f32(const XrGuestBus *bus, uint32_t address, float *value)
{
    uint64_t bits;
    uint32_t low;
    XrGuestStatus status;
    if (!value) return XR_GUEST_ARGUMENT;
    status = xr_guest_load(bus, address, 4, &bits);
    if (status != XR_GUEST_OK) return status;
    low = (uint32_t)bits;
    memcpy(value, &low, sizeof(*value));
    return XR_GUEST_OK;
}

XrGuestStatus xr_guest_load_f64(const XrGuestBus *bus, uint32_t address, double *value)
{
    uint64_t bits;
    XrGuestStatus status;
    if (!value) return XR_GUEST_ARGUMENT;
    status = xr_guest_load(bus, address, 8, &bits);
    if (status == XR_GUEST_OK) memcpy(value, &bits, sizeof(*value));
    return status;
}

XrGuestStatus xr_guest_store_f32(const XrGuestBus *bus, uint32_t address, float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return xr_guest_store(bus, address, 4, bits);
}

XrGuestStatus xr_guest_store_f64(const XrGuestBus *bus, uint32_t address, double value)
{
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return xr_guest_store(bus, address, 8, bits);
}

XrGuestStatus xr_guest_bus_load_lanes32(const XrGuestBus *bus, uint32_t address, uint32_t lanes[4])
{
    Fragment pieces[16]; unsigned count;
    uint8_t bytes[16]; XrGuestStatus status;
    if (!lanes) return XR_GUEST_ARGUMENT;
    status = ram_plan(bus, address, 16, XR_GUEST_READ, pieces, &count);
    if (status != XR_GUEST_OK) return status;
    gather(pieces, count, bytes);
    xr_guest_read_lanes32(bytes, lanes);
    return XR_GUEST_OK;
}

XrGuestStatus xr_guest_bus_store_lanes32(const XrGuestBus *bus, uint32_t address, const uint32_t lanes[4])
{
    Fragment pieces[16]; unsigned count;
    uint8_t bytes[16]; XrGuestStatus status;
    if (!lanes) return XR_GUEST_ARGUMENT;
    status = ram_plan(bus, address, 16, XR_GUEST_WRITE, pieces, &count);
    if (status != XR_GUEST_OK) return status;
    xr_guest_write_lanes32(bytes, lanes);
    scatter(pieces, count, bytes);
    return XR_GUEST_OK;
}

XrGuestStatus xr_guest_ram_span(const XrGuestBus *bus, uint32_t address, uint32_t bytes,
                               unsigned access, void **pointer)
{
    const XrGuestRegion *r;
    if (!bus || !pointer || !range_ok(address, bytes) || !access ||
        (access & ~(XR_GUEST_READ | XR_GUEST_WRITE))) return XR_GUEST_ARGUMENT;
    r = find_region(bus, address);
    if (!r) return XR_GUEST_UNMAPPED;
    if ((r->permissions & access) != access) return XR_GUEST_PERMISSION;
    if (!r->ram || bytes > r->size - (address - r->base)) return XR_GUEST_BOUNDARY;
    *pointer = r->ram + (address - r->base);
    return XR_GUEST_OK;
}

XrGuestStatus xr_guest_xbox_retail_map(XrGuestBus *bus, void *ram, size_t ram_size,
                                     void *contiguous, size_t contiguous_size)
{
    XrGuestBus next;
    XrGuestStatus status;
    uintptr_t a = (uintptr_t)ram, b = (uintptr_t)contiguous;
    unsigned i;
    if (!bus || !ram || !contiguous || ram_size < XR_GUEST_RETAIL_RAM ||
        contiguous_size < XR_GUEST_RETAIL_RAM ||
        (a <= b ? b - a : a - b) < XR_GUEST_RETAIL_RAM) return XR_GUEST_ARGUMENT;
    xr_guest_bus_init(&next);
    for (i = 0; i <= 28; ++i) {
        status = xr_guest_map_ram(&next, i * XR_GUEST_RETAIL_RAM, XR_GUEST_RETAIL_RAM,
            ram, XR_GUEST_READ | XR_GUEST_WRITE);
        if (status != XR_GUEST_OK) return status;
    }
    status = xr_guest_map_ram(&next, 0x80000000u, XR_GUEST_RETAIL_RAM, contiguous, XR_GUEST_READ | XR_GUEST_WRITE);
    if (status != XR_GUEST_OK) return status;
    status = xr_guest_map_ram(&next, 0xf0000000u, XR_GUEST_RETAIL_RAM, contiguous, XR_GUEST_READ | XR_GUEST_WRITE);
    if (status != XR_GUEST_OK) return status;
    *bus = next;
    return XR_GUEST_OK;
}

const char *xr_guest_status_name(XrGuestStatus status)
{
    switch (status) {
    case XR_GUEST_OK: return "OK";
    case XR_GUEST_ARGUMENT: return "INVALID ARGUMENT";
    case XR_GUEST_CAPACITY: return "REGION CAPACITY";
    case XR_GUEST_OVERLAP: return "REGION OVERLAP";
    case XR_GUEST_UNMAPPED: return "UNMAPPED";
    case XR_GUEST_PERMISSION: return "ACCESS DENIED";
    case XR_GUEST_BOUNDARY: return "REGION BOUNDARY";
    case XR_GUEST_DEVICE: return "DEVICE FAILURE";
    case XR_GUEST_WIDTH: return "DEVICE WIDTH";
    default: return "UNKNOWN";
    }
}
