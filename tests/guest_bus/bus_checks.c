#include "bus_checks.h"
#include "platform/recomp_guest_bus.h"
#include "platform/recomp_guest_le.h"

#define CHECK(name, condition) do { \
    int ok = !!(condition); ++count; if (!ok) ++failed; \
    if (report) report(name, ok, context); \
} while (0)

typedef struct Device {
    unsigned reads, writes, width;
    uint32_t address;
    uint64_t value;
    int fail;
} Device;

static int device_read(void *context, uint32_t address, unsigned width, uint64_t *value)
{
    Device *d = (Device *)context;
    ++d->reads; d->address = address; d->width = width;
    *value = d->value;
    return !d->fail;
}

static int device_write(void *context, uint32_t address, unsigned width, uint64_t value)
{
    Device *d = (Device *)context;
    ++d->writes; d->address = address; d->width = width; d->value = value;
    return !d->fail;
}

unsigned xr_guest_bus_checks(void (*report)(const char *, int, void *),
                              void *context, unsigned *total)
{
    XrGuestBus bus;
    uint8_t ram[64], other[32], top[16], sentinel[16];
    uint64_t value;
    uint32_t lanes[4] = {0x11223344u,0x55667788u,0x99aabbccu,0xddeeff00u};
    uint32_t loaded[4];
    void *pointer;
    float f;
    double d;
    Device device;
    unsigned i, count = 0, failed = 0;
    memset(ram, 0xa5, sizeof(ram)); memset(other, 0x5a, sizeof(other));
    memset(top, 0, sizeof(top)); memset(sentinel, 0xcc, sizeof(sentinel));
    memset(&device, 0, sizeof(device));
    xr_guest_bus_init(&bus);
    CHECK("EMPTY BUS READ", xr_guest_load(&bus, 0x1000, 4, &value) == XR_GUEST_UNMAPPED);
    CHECK("MAP NULL BUS", xr_guest_map_ram(NULL, 0, 16, ram, 3) == XR_GUEST_ARGUMENT);
    CHECK("MAP NULL RAM", xr_guest_map_ram(&bus, 0, 16, NULL, 3) == XR_GUEST_ARGUMENT);
    CHECK("MAP EMPTY", xr_guest_map_ram(&bus, 0, 0, ram, 3) == XR_GUEST_ARGUMENT);
    CHECK("MAP OVERFLOW", xr_guest_map_ram(&bus, 0xfffffff0u, 17, top, 3) == XR_GUEST_ARGUMENT);
    CHECK("MAP BAD PERMISSION", xr_guest_map_ram(&bus, 0, 16, ram, 8) == XR_GUEST_ARGUMENT);
    CHECK("MAP BASE", xr_guest_map_ram(&bus, 0x1000, 32, ram, 3) == XR_GUEST_OK);
    CHECK("MAP ALIAS", xr_guest_map_ram(&bus, 0x2000, 32, ram, 3) == XR_GUEST_OK);
    CHECK("MAP OVERLAP", xr_guest_map_ram(&bus, 0x101f, 16, other, 3) == XR_GUEST_OVERLAP);
    CHECK("MAP CONTAINED", xr_guest_map_ram(&bus, 0x1004, 4, other, 3) == XR_GUEST_OVERLAP);
    CHECK("MAP OVERLAP UNCHANGED", bus.count == 2);
    CHECK("MAP ADJACENT", xr_guest_map_ram(&bus, 0x1020, 32, other, 3) == XR_GUEST_OK);
    CHECK("MAP READ ONLY", xr_guest_map_ram(&bus, 0x3000, 16, sentinel, XR_GUEST_READ) == XR_GUEST_OK);
    CHECK("MAP WRITE ONLY", xr_guest_map_ram(&bus, 0x4000, 16, ram + 32, XR_GUEST_WRITE) == XR_GUEST_OK);
    CHECK("MAP TOP OF ADDRESS SPACE", xr_guest_map_ram(&bus, 0xfffffff0u, 16, top, 3) == XR_GUEST_OK);
    CHECK("LOAD NULL OUTPUT", xr_guest_load(&bus, 0x1000, 4, NULL) == XR_GUEST_ARGUMENT);
    CHECK("LOAD INVALID WIDTH", xr_guest_load(&bus, 0x1000, 3, &value) == XR_GUEST_ARGUMENT);
    value = UINT64_C(0xdeadbeef01234567);
    CHECK("FAILED READ OUTPUT UNCHANGED", xr_guest_load(&bus, 0x5000, 4, &value) == XR_GUEST_UNMAPPED && value == UINT64_C(0xdeadbeef01234567));
    CHECK("WRITE TO READ ONLY", xr_guest_store(&bus, 0x3000, 4, 0) == XR_GUEST_PERMISSION && sentinel[0] == 0xcc);
    CHECK("READ FROM WRITE ONLY", xr_guest_load(&bus, 0x4000, 4, &value) == XR_GUEST_PERMISSION);
    CHECK("STORE8 MASKS UPPER BITS", xr_guest_store(&bus, 0x1000, 1, 0x1234) == XR_GUEST_OK && ram[0] == 0x34);
    CHECK("LOAD8 ZERO EXTENDS", xr_guest_load(&bus, 0x1000, 1, &value) == XR_GUEST_OK && value == 0x34);
    CHECK("STORE16 MASKS UPPER BITS", xr_guest_store(&bus, 0x1001, 2, 0x1234abcd) == XR_GUEST_OK && ram[1] == 0xcd && ram[2] == 0xab);
    CHECK("LOAD16 ZERO EXTENDS", xr_guest_load(&bus, 0x1001, 2, &value) == XR_GUEST_OK && value == 0xabcd);
    CHECK("STORE INVALID WIDTH", xr_guest_store(&bus, 0x1000, 0, 0) == XR_GUEST_ARGUMENT && ram[0] == 0x34);

    for (i = 0; i < 8; ++i) {
        CHECK("UNALIGNED STORE64", xr_guest_store(&bus, 0x1000 + i, 8, UINT64_C(0x0123456789abcdef)) == XR_GUEST_OK);
        CHECK("ALIAS RAW LITTLE ENDIAN BYTES", ram[i] == 0xef && ram[i + 1] == 0xcd && ram[i + 6] == 0x23 && ram[i + 7] == 0x01);
        value = 0;
        CHECK("ALIAS LOAD64", xr_guest_load(&bus, 0x2000 + i, 8, &value) == XR_GUEST_OK && value == UINT64_C(0x0123456789abcdef));
        CHECK("REVERSE ALIAS STORE32", xr_guest_store(&bus, 0x2000 + i, 4, 0x12345678) == XR_GUEST_OK);
        CHECK("REVERSE ALIAS LOAD32", xr_guest_load(&bus, 0x1000 + i, 4, &value) == XR_GUEST_OK && value == 0x12345678);
    }
    CHECK("CROSS REGION STORE", xr_guest_store(&bus, 0x101e, 4, 0x12345678) == XR_GUEST_OK);
    CHECK("CROSS REGION CORRECT BYTES", ram[30] == 0x78 && ram[31] == 0x56 && other[0] == 0x34 && other[1] == 0x12);
    CHECK("CROSS REGION LOAD", xr_guest_load(&bus, 0x101e, 4, &value) == XR_GUEST_OK && value == 0x12345678);
    CHECK("CROSS HOLE STORE REJECTED", xr_guest_store(&bus, 0x103e, 4, 0) == XR_GUEST_UNMAPPED && other[30] == 0x5a && other[31] == 0x5a);
    CHECK("CROSS READ ONLY MAP", xr_guest_map_ram(&bus, 0x1040, 16, sentinel, XR_GUEST_READ) == XR_GUEST_OK);
    CHECK("CROSS PERMISSION NO PARTIAL STORE", xr_guest_store(&bus, 0x103e, 4, 0) == XR_GUEST_PERMISSION && other[30] == 0x5a && other[31] == 0x5a && sentinel[0] == 0xcc);
    CHECK("LAST ADDRESS WRITE", xr_guest_store(&bus, 0xffffffffu, 1, 0x7f) == XR_GUEST_OK && top[15] == 0x7f);
    CHECK("TOP EIGHT BYTES", xr_guest_store(&bus, 0xfffffff8u, 8, UINT64_C(0x0123456789abcdef)) == XR_GUEST_OK);
    CHECK("TOP EIGHT LOAD", xr_guest_load(&bus, 0xfffffff8u, 8, &value) == XR_GUEST_OK && value == UINT64_C(0x0123456789abcdef));
    CHECK("WRAPPING LOAD REJECTED", xr_guest_load(&bus, 0xffffffffu, 2, &value) == XR_GUEST_ARGUMENT);
    CHECK("WRAPPING STORE REJECTED", xr_guest_store(&bus, 0xfffffffeu, 4, 0) == XR_GUEST_ARGUMENT && top[14] == 0x23 && top[15] == 0x01);
    pointer = sentinel;
    CHECK("BORROW RAM SPAN", xr_guest_ram_span(&bus, 0x1003, 4, XR_GUEST_READ, &pointer) == XR_GUEST_OK && pointer == ram + 3);
    pointer = sentinel;
    CHECK("BORROW SPLIT SPAN DENIED", xr_guest_ram_span(&bus, 0x101f, 2, XR_GUEST_READ, &pointer) == XR_GUEST_BOUNDARY && pointer == sentinel);
    CHECK("BORROW READ ONLY WRITE DENIED", xr_guest_ram_span(&bus, 0x3000, 4, XR_GUEST_WRITE, &pointer) == XR_GUEST_PERMISSION);
    CHECK("FLOAT32 STORE", xr_guest_store_f32(&bus, 0x101f, 1.5f) == XR_GUEST_OK);
    CHECK("FLOAT32 READ THROUGH SPLIT", xr_guest_load_f32(&bus, 0x101f, &f) == XR_GUEST_OK && f == 1.5f);
    CHECK("FLOAT64 STORE", xr_guest_store_f64(&bus, 0x101d, -2.5) == XR_GUEST_OK);
    CHECK("FLOAT64 READ THROUGH SPLIT", xr_guest_load_f64(&bus, 0x101d, &d) == XR_GUEST_OK && d == -2.5);
    CHECK("SIMD STORE THROUGH SPLIT", xr_guest_bus_store_lanes32(&bus, 0x1019, lanes) == XR_GUEST_OK);
    CHECK("SIMD LOAD THROUGH SPLIT", xr_guest_bus_load_lanes32(&bus, 0x1019, loaded) == XR_GUEST_OK && !memcmp(lanes, loaded, sizeof(lanes)));
    memset(loaded, 0xff, sizeof(loaded));
    CHECK("FAILED SIMD OUTPUT UNCHANGED", xr_guest_bus_load_lanes32(&bus, 0x104f, loaded) == XR_GUEST_UNMAPPED && loaded[0] == 0xffffffffu && loaded[3] == 0xffffffffu);

    CHECK("MAP DEVICE", xr_guest_map_device(&bus, 0xfe800000u, 16, 3, 1u << 4, &device, device_read, device_write) == XR_GUEST_OK);
    CHECK("DEVICE STORE ONCE", xr_guest_store(&bus, 0xfe800004u, 4, UINT64_C(0x12345678abcdef01)) == XR_GUEST_OK && device.writes == 1 && device.width == 4 && device.address == 0xfe800004u && device.value == 0xabcdef01u);
    device.value = UINT64_C(0xdeadbeef12345678);
    CHECK("DEVICE READ ONCE", xr_guest_load(&bus, 0xfe800004u, 4, &value) == XR_GUEST_OK && value == 0x12345678 && device.reads == 1);
    CHECK("DEVICE WIDTH NO SIDE EFFECT", xr_guest_load(&bus, 0xfe800004u, 2, &value) == XR_GUEST_WIDTH && device.reads == 1);
    CHECK("DEVICE CROSS BOUNDARY NO SIDE EFFECT", xr_guest_store(&bus, 0xfe80000eu, 4, 0) == XR_GUEST_BOUNDARY && device.writes == 1);
    pointer = sentinel;
    CHECK("DEVICE HAS NO BORROWED POINTER", xr_guest_ram_span(&bus, 0xfe800000u, 4, XR_GUEST_READ, &pointer) == XR_GUEST_BOUNDARY && pointer == sentinel);
    device.fail = 1; value = UINT64_C(0xfeedfacecafebabe);
    CHECK("DEVICE FAILURE PROPAGATED", xr_guest_load(&bus, 0xfe800004u, 4, &value) == XR_GUEST_DEVICE && value == UINT64_C(0xfeedfacecafebabe));
    CHECK("DEVICE WRITE FAILURE PROPAGATED", xr_guest_store(&bus, 0xfe800004u, 4, 0) == XR_GUEST_DEVICE);
    CHECK("DEVICE MISSING CALLBACK", xr_guest_map_device(&bus, 0xfe900000u, 4, XR_GUEST_READ, 1u << 4, &device, NULL, device_write) == XR_GUEST_ARGUMENT);
    CHECK("DEVICE INVALID WIDTH MASK", xr_guest_map_device(&bus, 0xfe900000u, 4, 3, 1u << 3, &device, device_read, device_write) == XR_GUEST_ARGUMENT);
    CHECK("RAM BEFORE DEVICE MAP", xr_guest_map_ram(&bus, 0xfe7ffffeu, 2, ram, 3) == XR_GUEST_OK);
    ram[0] = 0x12; ram[1] = 0x34;
    CHECK("MIXED RAM DEVICE STORE REJECTED", xr_guest_store(&bus, 0xfe7ffffeu, 4, 0) == XR_GUEST_BOUNDARY && ram[0] == 0x12 && ram[1] == 0x34 && device.writes == 2);
    xr_guest_bus_init(&bus);
    for (i = 0; i < XR_GUEST_MAX_REGIONS; ++i)
        CHECK("FILL REGION TABLE", xr_guest_map_ram(&bus, i * 2, 1, ram, 3) == XR_GUEST_OK);
    CHECK("TABLE FULL", xr_guest_map_ram(&bus, 0x1000, 1, ram, 3) == XR_GUEST_CAPACITY && bus.count == XR_GUEST_MAX_REGIONS);
    if (total) *total = count;
    return failed;
}

unsigned xr_guest_retail_checks(void *ram, size_t ram_size, void *contiguous,
    size_t contiguous_size, void (*report)(const char *, int, void *),
    void *context, unsigned *total)
{
    XrGuestBus bus;
    uint8_t *r = (uint8_t *)ram, *c = (uint8_t *)contiguous;
    uint64_t value = 0;
    unsigned i, count = 0, failed = 0;
    xr_guest_bus_init(&bus);
    CHECK("RETAIL MAP INIT", xr_guest_xbox_retail_map(&bus, ram, ram_size, contiguous, contiguous_size) == XR_GUEST_OK);
    if (!bus.count) { if (total) *total = count; return failed; }
    CHECK("RETAIL REGION COUNT", bus.count == 31);
    CHECK("RETAIL BAD SIZE NO CHANGE", xr_guest_xbox_retail_map(&bus, ram, XR_GUEST_RETAIL_RAM - 1, contiguous, contiguous_size) == XR_GUEST_ARGUMENT && bus.count == 31);
    CHECK("RETAIL OVERLAPPING BUFFERS REJECTED", xr_guest_xbox_retail_map(&bus, ram, ram_size, ram, ram_size) == XR_GUEST_ARGUMENT && bus.count == 31);
    CHECK("RETAIL BASE STORE", xr_guest_store(&bus, 0x10001, 4, 0xa1b2c3d4) == XR_GUEST_OK);
    for (i = 0; i <= 28; ++i) {
        CHECK("RETAIL MIRROR READ", xr_guest_load(&bus, i * XR_GUEST_RETAIL_RAM + 0x10001, 4, &value) == XR_GUEST_OK && value == 0xa1b2c3d4);
        CHECK("RETAIL MIRROR WRITE", xr_guest_store(&bus, i * XR_GUEST_RETAIL_RAM + 0x20003, 4, i) == XR_GUEST_OK);
        CHECK("RETAIL MIRROR WRITE REACHES BASE", xr_guest_load(&bus, 0x20003, 4, &value) == XR_GUEST_OK && value == i);
    }
    CHECK("RETAIL WRAP TO NEXT MIRROR", xr_guest_store(&bus, XR_GUEST_RETAIL_RAM - 2, 4, 0x12345678) == XR_GUEST_OK && r[XR_GUEST_RETAIL_RAM - 2] == 0x78 && r[XR_GUEST_RETAIL_RAM - 1] == 0x56 && r[0] == 0x34 && r[1] == 0x12);
    CHECK("RETAIL READ ACROSS MIRROR", xr_guest_load(&bus, XR_GUEST_RETAIL_RAM - 2, 4, &value) == XR_GUEST_OK && value == 0x12345678);
    r[XR_GUEST_RETAIL_RAM - 1] = 0xa5;
    CHECK("RETAIL LAST MIRROR NO PARTIAL WRITE", xr_guest_store(&bus, 0x73ffffffu, 4, 0) == XR_GUEST_UNMAPPED && r[XR_GUEST_RETAIL_RAM - 1] == 0xa5);
    CHECK("RETAIL CONTIGUOUS STORE", xr_guest_store(&bus, 0x80010001u, 4, 0x11223344) == XR_GUEST_OK);
    CHECK("RETAIL CONTIGUOUS SEPARATE FROM LOW RAM", xr_guest_load(&bus, 0x10001, 4, &value) == XR_GUEST_OK && value == 0xa1b2c3d4);
    CHECK("RETAIL TILED ALIAS READ", xr_guest_load(&bus, 0xf0010001u, 4, &value) == XR_GUEST_OK && value == 0x11223344);
    CHECK("RETAIL TILED ALIAS WRITE", xr_guest_store(&bus, 0xf0010001u, 4, 0x55667788) == XR_GUEST_OK && c[0x10001] == 0x88);
    CHECK("RETAIL TILED WRITE REACHES CONTIGUOUS", xr_guest_load(&bus, 0x80010001u, 4, &value) == XR_GUEST_OK && value == 0x55667788);
    CHECK("RETAIL CONTIGUOUS END REJECTED", xr_guest_load(&bus, 0x84000000u, 4, &value) == XR_GUEST_UNMAPPED);
    CHECK("RETAIL TILED END REJECTED", xr_guest_load(&bus, 0xf4000000u, 4, &value) == XR_GUEST_UNMAPPED);
    CHECK("RETAIL GPU NEEDS EXPLICIT MODEL", xr_guest_load(&bus, 0xfd000000u, 4, &value) == XR_GUEST_UNMAPPED);
    CHECK("RETAIL APU NEEDS EXPLICIT MODEL", xr_guest_load(&bus, 0xfe800010u, 4, &value) == XR_GUEST_UNMAPPED);
    CHECK("RETAIL USB NEEDS EXPLICIT MODEL", xr_guest_store(&bus, 0xfed00000u, 4, 0) == XR_GUEST_UNMAPPED);
    CHECK("RETAIL ALL ONES POINTER REJECTED", xr_guest_load(&bus, 0xffffffffu, 4, &value) == XR_GUEST_ARGUMENT);
    if (total) *total = count;
    return failed;
}
