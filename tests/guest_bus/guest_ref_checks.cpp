/* XrGuestRef (recomp_guest_ref.h) against the bus functions it inlines.
 *
 * Every access through the inline RAM fast path must give exactly what
 * xr_guest_load/xr_guest_store give: the same value, the same bytes written,
 * the same device callbacks, and the same fault status where they fail. The
 * map mixes everything the fast-page table must leave out: a device page,
 * a page split between two RAM mappings, read-only and write-only RAM,
 * partly covered pages, holes, and an alias of the first RAM block. Then the
 * table's ownership rules: another bus, no bus, re-initialisation,
 * xr_guest_fast_select and the retail layout. */
#include "platform/recomp_guest_ref.h"
#include "platform/recomp_guest_bus.h"
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

XR_REF_TLS XrGuestBus *g_recomp_guest_bus;

static jmp_buf s_fault_jump;
static int s_fault_armed;
static XrGuestStatus s_fault_status;
void xr_guest_access_fault(XrGuestStatus status, uint32_t, unsigned)
{
    if (!s_fault_armed) { fprintf(stderr, "unexpected guest fault %d\n", (int)status); abort(); }
    s_fault_status = status;
    longjmp(s_fault_jump, 1);
}

static unsigned s_count, s_failed;
static void check(const char *name, int ok)
{
    ++s_count;
    if (!ok) { ++s_failed; fprintf(stderr, "FAIL: %s\n", name); }
}

/* Backing stores. Two copies so a store can be replayed on each path. */
struct Mem {
    uint8_t low[0x3000];      /* 0x10000-0x12fff, aliased at 0x80010000 */
    uint8_t split_a[0x800];   /* 0x14000-0x147ff */
    uint8_t split_b[0x800];   /* 0x14800-0x14fff */
    uint8_t ro[0x1000];       /* 0x16000, read only */
    uint8_t wo[0x1000];       /* 0x17000, write only */
    uint8_t part[0x1800];     /* 0x18800-0x19fff: page 0x18 only half */
    uint32_t dev_reads, dev_writes, dev_last_addr, dev_last_width;
    uint64_t dev_last_value;
};
static Mem s_mem[2];

static int dev_read(void *c, uint32_t a, unsigned w, uint64_t *v)
{
    Mem *m = (Mem *)c;
    ++m->dev_reads; m->dev_last_addr = a; m->dev_last_width = w;
    *v = (uint64_t)a * 0x9E3779B97F4A7C15ull;
    return 1;
}
static int dev_write(void *c, uint32_t a, unsigned w, uint64_t v)
{
    Mem *m = (Mem *)c;
    ++m->dev_writes; m->dev_last_addr = a; m->dev_last_width = w; m->dev_last_value = v;
    return 1;
}

static void fill(Mem *m)
{
    uint8_t *p = (uint8_t *)m;
    for (size_t i = 0; i < offsetof(Mem, dev_reads); ++i) p[i] = (uint8_t)(i * 131u + 7u);
    m->dev_reads = m->dev_writes = m->dev_last_addr = m->dev_last_width = 0;
    m->dev_last_value = 0;
}

static XrGuestStatus map(XrGuestBus *bus, Mem *m)
{
    XrGuestStatus st;
    xr_guest_bus_init(bus);
    st = xr_guest_map_ram(bus, 0x10000, 0x3000, m->low, 3);
    if (st == XR_GUEST_OK) st = xr_guest_map_device(bus, 0x13000, 0x1000, 3, XR_GUEST_WIDTHS, m, dev_read, dev_write);
    if (st == XR_GUEST_OK) st = xr_guest_map_ram(bus, 0x14000, 0x800, m->split_a, 3);
    if (st == XR_GUEST_OK) st = xr_guest_map_ram(bus, 0x14800, 0x800, m->split_b, 3);
    if (st == XR_GUEST_OK) st = xr_guest_map_ram(bus, 0x16000, 0x1000, m->ro, XR_GUEST_READ);
    if (st == XR_GUEST_OK) st = xr_guest_map_ram(bus, 0x17000, 0x1000, m->wo, XR_GUEST_WRITE);
    if (st == XR_GUEST_OK) st = xr_guest_map_ram(bus, 0x18800, 0x1800, m->part, 3);
    if (st == XR_GUEST_OK) st = xr_guest_map_ram(bus, 0x80010000u, 0x3000, m->low, 3);
    return st;
}

/* One access through XrGuestRef; returns the fault status (OK if none). */
template<class T> static XrGuestStatus ref_load(uint32_t va, T *out)
{
    s_fault_armed = 1;
    if (setjmp(s_fault_jump)) { s_fault_armed = 0; return s_fault_status; }
    *out = XrGuestRef<T>(va);
    s_fault_armed = 0;
    return XR_GUEST_OK;
}
template<class T> static XrGuestStatus ref_store(uint32_t va, T v)
{
    s_fault_armed = 1;
    if (setjmp(s_fault_jump)) { s_fault_armed = 0; return s_fault_status; }
    (XrGuestRef<T>(va)) = v;
    s_fault_armed = 0;
    return XR_GUEST_OK;
}

template<class T> static void sweep(XrGuestBus *bus_ref, XrGuestBus *bus_ref_twin, const char *name)
{
    char label[96];
    unsigned bad_load = 0, bad_store = 0, fast = 0;
    for (uint32_t va = 0x0fff0; va < 0x1a010; ++va) {
        /* Load: compare with the bus function on the same bus. */
        fill(&s_mem[0]);
        uint64_t bits = 0;
        XrGuestStatus want = xr_guest_load(bus_ref, va, sizeof(T), &bits);
        Mem after_bus = s_mem[0];
        fill(&s_mem[0]);
        T got = 0;
        XrGuestStatus st = ref_load<T>(va, &got);
        T expect = XrGuestCodec<T>::decode(bits);
        if (st != want || (st == XR_GUEST_OK && memcmp(&got, &expect, sizeof(T))) ||
            memcmp(&s_mem[0], &after_bus, sizeof(Mem)))
            ++bad_load;
        if (xr_guest_fast_ptr(va, sizeof(T))) ++fast;
        /* Store: replay on the twin bus with the bus function, compare. */
        fill(&s_mem[0]); fill(&s_mem[1]);
        T value = XrGuestCodec<T>::decode(0x8877665544332211ull ^ ((uint64_t)va * 0x10001ull));
        XrGuestStatus want_st = xr_guest_store(bus_ref_twin, va, sizeof(T), XrGuestCodec<T>::encode(value));
        XrGuestStatus got_st = ref_store<T>(va, value);
        if (got_st != want_st || memcmp(&s_mem[0], &s_mem[1], sizeof(Mem))) ++bad_store;
    }
    snprintf(label, sizeof label, "%s loads match the bus", name); check(label, bad_load == 0);
    snprintf(label, sizeof label, "%s stores match the bus", name); check(label, bad_store == 0);
    snprintf(label, sizeof label, "%s some accesses fast", name); check(label, fast > 0);
    if (bad_load || bad_store) fprintf(stderr, "  %s: %u bad loads, %u bad stores\n", name, bad_load, bad_store);
}

static void device_counts()
{
    fill(&s_mem[0]);
    uint32_t v = 0;
    check("device load dispatches once", ref_load<uint32_t>(0x13010, &v) == XR_GUEST_OK &&
          s_mem[0].dev_reads == 1 && s_mem[0].dev_last_addr == 0x13010 && s_mem[0].dev_last_width == 4 &&
          v == (uint32_t)((0x13010ull * 0x9E3779B97F4A7C15ull) & 0xffffffffull));
    check("device store dispatches once", ref_store<uint16_t>(0x13ffe, 0xBEEF) == XR_GUEST_OK &&
          s_mem[0].dev_writes == 1 && s_mem[0].dev_last_value == 0xBEEF && s_mem[0].dev_last_width == 2);
    check("device page-crossing store faults", ref_store<uint32_t>(0x13ffe, 1) == XR_GUEST_BOUNDARY &&
          s_mem[0].dev_writes == 1);
}

/* Which path an access takes at the end of a listed page. Page 0x10's table
 * entry is pointed at a decoy copy for the duration: the fast path reads
 * and writes the decoy, the normal path the mapped RAM. An access that stays
 * in the page must be fast; one that crosses into page 0x11 must be fast
 * exactly when XR_GUEST_FAST_PAGE_CROSS is 1 (never one wide access across
 * the page on hosts where that traps, the Xbox 360). Bus `a` is current. */
static void page_cross_path()
{
    static uint8_t decoy[0x2000];
    uint8_t *const saved = xr_guest_fast_page[0x10];
    unsigned wrong = 0, wrong_value = 0, n = 0;
    for (unsigned width = 2; width <= 8; width *= 2) {
        for (uint32_t off = 0x1000 - width - 3; off < 0x1000; ++off) {
            const uint32_t va = 0x10000 + off;
            const int crosses = off + width > 0x1000;
            const int want_fast = !crosses || XR_GUEST_FAST_PAGE_CROSS;
            fill(&s_mem[0]);
            for (unsigned i = 0; i < sizeof decoy; ++i) decoy[i] = (uint8_t)(0xD0 ^ i);
            xr_guest_fast_page[0x10] = decoy;
            const uint8_t *src = want_fast ? decoy + off : s_mem[0].low + off;
            uint64_t want = 0;
            for (unsigned i = 0; i < width; ++i) want |= (uint64_t)src[i] << (8 * i);
            uint64_t got = width == 2 ? (uint64_t)(uint16_t)XrGuestRef<uint16_t>(va)
                         : width == 4 ? (uint64_t)(uint32_t)XrGuestRef<uint32_t>(va)
                                      : (uint64_t)XrGuestRef<uint64_t>(va);
            if (got != want) ++wrong_value;
            if ((xr_guest_fast_ptr(va, width) != 0) != want_fast) ++wrong;
            /* store: exactly the expected buffer changes */
            const uint64_t v = 0x0123456789ABCDEFull;
            const uint8_t *dst = want_fast ? decoy + off : s_mem[0].low + off;
            const uint8_t *other = want_fast ? s_mem[0].low + off : decoy + off;
            uint8_t other_before[8];
            memcpy(other_before, other, width);
            if (width == 2) (XrGuestRef<uint16_t>(va)) = (uint16_t)v;
            else if (width == 4) (XrGuestRef<uint32_t>(va)) = (uint32_t)v;
            else (XrGuestRef<uint64_t>(va)) = v;
            for (unsigned i = 0; i < width; ++i)
                if (dst[i] != (uint8_t)(v >> (8 * i))) ++wrong_value;
            if (memcmp(other, other_before, width)) ++wrong_value;
            ++n;
        }
    }
    /* 16 bytes (XMM_MEM/XMM_STORE resolve all of them with one lookup) */
    xr_guest_fast_page[0x10] = decoy;
    if (!xr_guest_fast_ptr(0x10ff0, 16)) ++wrong;
    if ((xr_guest_fast_ptr(0x10ff1, 16) != 0) != (XR_GUEST_FAST_PAGE_CROSS != 0)) ++wrong;
    if ((xr_guest_fast_ptr(0x10ffc, 16) != 0) != (XR_GUEST_FAST_PAGE_CROSS != 0)) ++wrong;
    if (!xr_guest_fast_ptr(0x10fff, 1)) ++wrong;
    xr_guest_fast_page[0x10] = saved;
    check("page-crossing accesses take the path XR_GUEST_FAST_PAGE_CROSS selects", n == 23 && wrong == 0);
    check("page-crossing accesses read and write the right bytes", wrong_value == 0);
    if (wrong || wrong_value) fprintf(stderr, "  page cross: %u wrong path, %u wrong values\n", wrong, wrong_value);
}

int main()
{
    static XrGuestBus a, a_twin, other;
    static uint8_t other_ram[0x2000];
    check("first bus maps", map(&a, &s_mem[0]) == XR_GUEST_OK);
    check("first bus to map RAM owns the table", xr_guest_fast_bus == &a);
    check("twin bus maps", map(&a_twin, &s_mem[1]) == XR_GUEST_OK);
    check("second bus does not take the table", xr_guest_fast_bus == &a);
    check("full RAM pages listed", xr_guest_fast_page[0x10] == s_mem[0].low && xr_guest_fast_page[0x11] == s_mem[0].low + 0x1000);
    check("last page before a device not listed", !xr_guest_fast_page[0x12]);
    check("alias pages listed", xr_guest_fast_page[0x80010] == s_mem[0].low && xr_guest_fast_page[0x80011] == s_mem[0].low + 0x1000);
    check("last page before a hole not listed", !xr_guest_fast_page[0x80012]);
    check("device page not listed", !xr_guest_fast_page[0x13]);
    check("split page not listed", !xr_guest_fast_page[0x14]);
    check("read-only page not listed", !xr_guest_fast_page[0x16]);
    check("write-only page not listed", !xr_guest_fast_page[0x17]);
    check("half-covered page not listed", !xr_guest_fast_page[0x18] && !xr_guest_fast_page[0x19]);
    check("hole not listed", !xr_guest_fast_page[0x15] && !xr_guest_fast_page[0x0f]);

    g_recomp_guest_bus = &a;
    sweep<uint8_t>(&a, &a_twin, "u8");
    sweep<uint16_t>(&a, &a_twin, "u16");
    sweep<uint32_t>(&a, &a_twin, "u32");
    sweep<uint64_t>(&a, &a_twin, "u64");
    sweep<int8_t>(&a, &a_twin, "s8");
    sweep<int16_t>(&a, &a_twin, "s16");
    sweep<int32_t>(&a, &a_twin, "s32");
    sweep<int64_t>(&a, &a_twin, "s64");
    sweep<float>(&a, &a_twin, "f32");
    sweep<double>(&a, &a_twin, "f64");
    device_counts();

    /* Compound assignment and the little-endian byte image. */
    fill(&s_mem[0]);
    (XrGuestRef<uint32_t>(0x10ffc)) = 0x11223344u;
    (XrGuestRef<uint32_t>(0x10ffc)) += 0x01010101u;
    check("fast store is little endian", s_mem[0].low[0xffc] == 0x45 && s_mem[0].low[0xfff] == 0x12);
    check("alias reads the same RAM", (uint32_t)XrGuestRef<uint32_t>(0x80010ffc) == 0x12233445u);
    (XrGuestRef<uint16_t>(0x10fff)) = 0xA1B2;   /* crosses into the next page, same mapping */
    check("page-crossing store", s_mem[0].low[0xfff] == 0xB2 && s_mem[0].low[0x1000] == 0xA1);
    page_cross_path();

    /* Another bus on this thread: never the table. */
    xr_guest_bus_init(&other);
    check("other bus maps", xr_guest_map_ram(&other, 0x10000, 0x2000, other_ram, 3) == XR_GUEST_OK);
    check("owner unchanged", xr_guest_fast_bus == &a);
    other_ram[0x10] = 0x5A;
#if !defined(XR_GUEST_FAST_ONE_BUS)
    /* XR_GUEST_FAST_ONE_BUS drops the per-access bus test: a thread with
     * another bus, or none, then reaches the table owner's RAM by design,
     * so these checks only hold without it. */
    g_recomp_guest_bus = &other;
    uint8_t b = 0;
    check("other bus reads its own RAM", ref_load<uint8_t>(0x10010, &b) == XR_GUEST_OK && b == 0x5A);
    check("other bus faults where it has no RAM", ref_load<uint8_t>(0x12000, &b) == XR_GUEST_UNMAPPED);
    g_recomp_guest_bus = NULL;
    check("no bus faults", ref_load<uint8_t>(0x10010, &b) == XR_GUEST_ARGUMENT);
    check("no bus store faults", ref_store<uint32_t>(0x10010, 0) == XR_GUEST_ARGUMENT);
#endif

    /* Ownership moves. */
    xr_guest_fast_select(&other);
    check("select rebuilds", xr_guest_fast_bus == &other && xr_guest_fast_page[0x10] == other_ram &&
          !xr_guest_fast_page[0x11] && !xr_guest_fast_page[0x12] && !xr_guest_fast_page[0x80010]);
    g_recomp_guest_bus = &a;
    fill(&s_mem[0]);
    uint32_t w = 0;
#if !defined(XR_GUEST_FAST_ONE_BUS)
    check("old owner still correct off the table",
          ref_load<uint32_t>(0x10000, &w) == XR_GUEST_OK && w == (uint32_t)(s_mem[0].low[0] | s_mem[0].low[1] << 8 |
                                                                           s_mem[0].low[2] << 16 | (uint32_t)s_mem[0].low[3] << 24));
#endif
    xr_guest_bus_init(&other);
    check("init releases the table", xr_guest_fast_bus == NULL && !xr_guest_fast_page[0x10] && !xr_guest_fast_page[0x11]);
    check("next RAM map takes it", xr_guest_map_ram(&other, 0x20000, 0x1000, other_ram, 3) == XR_GUEST_OK &&
          xr_guest_fast_bus == &other && !xr_guest_fast_page[0x20]);
    check("contiguous neighbour lists the page before it",
          xr_guest_map_ram(&other, 0x21000, 0x1000, other_ram + 0x1000, 3) == XR_GUEST_OK &&
          xr_guest_fast_page[0x20] == other_ram && !xr_guest_fast_page[0x21]);
    check("neighbour with other backing does not",
          xr_guest_map_ram(&other, 0x23000, 0x1000, other_ram + 0x1000, 3) == XR_GUEST_OK &&
          xr_guest_map_ram(&other, 0x24000, 0x1000, other_ram, 3) == XR_GUEST_OK && !xr_guest_fast_page[0x23]);
    check("read-only neighbour does not",
          xr_guest_map_ram(&other, 0x26000, 0x1000, other_ram, 3) == XR_GUEST_OK &&
          xr_guest_map_ram(&other, 0x27000, 0x1000, other_ram + 0x1000, XR_GUEST_READ) == XR_GUEST_OK &&
          !xr_guest_fast_page[0x26]);
    check("unaligned neighbour does not",
          xr_guest_map_ram(&other, 0x28000, 0x1000, other_ram, 3) == XR_GUEST_OK &&
          xr_guest_map_ram(&other, 0x29000, 0x800, other_ram + 0x1000, 3) == XR_GUEST_OK &&
          !xr_guest_fast_page[0x28] && !xr_guest_fast_page[0x29]);
    g_recomp_guest_bus = &other;
    other_ram[0xffe] = 0x11; other_ram[0xfff] = 0x22; other_ram[0x1000] = 0x33; other_ram[0x1001] = 0x44;
    check("access reaching into the next page", (XR_GUEST_FAST_PAGE_CROSS ? xr_guest_fast_ptr(0x20ffe, 4) != 0
                                                                           : xr_guest_fast_ptr(0x20ffe, 4) == 0) &&
          (uint32_t)XrGuestRef<uint32_t>(0x20ffe) == 0x44332211u);
    check("slow access at a listed page's neighbour", !xr_guest_fast_ptr(0x21ffe, 4) &&
          (uint32_t)XrGuestRef<uint16_t>(0x21ffe) == (uint32_t)(other_ram[0x1ffe] | other_ram[0x1fff] << 8));
    check("crossing into a hole faults", ref_load<uint32_t>(0x21ffe, &w) == XR_GUEST_UNMAPPED);
    g_recomp_guest_bus = &a;
    xr_guest_fast_select(NULL);
    check("select NULL clears", xr_guest_fast_bus == NULL && !xr_guest_fast_page[0x20]);
    check("device map does not take the table", xr_guest_map_device(&other, 0x30000, 0x1000, 3, XR_GUEST_WIDTHS,
          &s_mem[0], dev_read, dev_write) == XR_GUEST_OK && xr_guest_fast_bus == NULL);

    /* Retail layout: owned by the caller's bus, mirrors and aliases fast. */
    {
        static XrGuestBus retail;
        void *ram = calloc(1, XR_GUEST_RETAIL_RAM), *contiguous = calloc(1, XR_GUEST_RETAIL_RAM);
        if (!ram || !contiguous) { fprintf(stderr, "out of memory\n"); return 2; }
        xr_guest_bus_init(&retail);
        check("retail map", xr_guest_xbox_retail_map(&retail, ram, XR_GUEST_RETAIL_RAM, contiguous, XR_GUEST_RETAIL_RAM) == XR_GUEST_OK);
        check("retail bus owns the table", xr_guest_fast_bus == &retail);
        check("retail low and mirror pages", xr_guest_fast_page[0] == ram && xr_guest_fast_page[(0x70000000u >> 12) + 1] == (uint8_t *)ram + 0x1000);
        check("retail contiguous and tiled pages", xr_guest_fast_page[0x80000] == contiguous &&
              xr_guest_fast_page[0xF0001] == (uint8_t *)contiguous + 0x1000);
        check("retail end not listed", !xr_guest_fast_page[0x84000] && !xr_guest_fast_page[0x74000]);
        check("retail mirror ends not listed", !xr_guest_fast_page[0x03FFF] && !xr_guest_fast_page[0x73FFF] &&
              !xr_guest_fast_page[0x83FFF] && !xr_guest_fast_page[0xF3FFF] && xr_guest_fast_page[0x03FFE]);
        g_recomp_guest_bus = &retail;
        (XrGuestRef<uint32_t>(0xF0001234u)) = 0xCAFEF00Du;
        check("tiled store seen at 0x80000000", (uint32_t)XrGuestRef<uint32_t>(0x80001234u) == 0xCAFEF00Du);
        (XrGuestRef<uint32_t>(0x40001234u)) = 0x0BADF00Du;
        check("mirror store seen at 0", (uint32_t)XrGuestRef<uint32_t>(0x1234u) == 0x0BADF00Du);
        check("retail remap keeps ownership", xr_guest_xbox_retail_map(&retail, ram, XR_GUEST_RETAIL_RAM, contiguous, XR_GUEST_RETAIL_RAM) == XR_GUEST_OK &&
              xr_guest_fast_bus == &retail && xr_guest_fast_page[0x80000] == contiguous);
        check("failed retail map leaves table", xr_guest_xbox_retail_map(&retail, ram, 1, contiguous, XR_GUEST_RETAIL_RAM) == XR_GUEST_ARGUMENT &&
              xr_guest_fast_bus == &retail && xr_guest_fast_page[0] == ram);
        xr_guest_bus_init(&retail);
        check("retail release", xr_guest_fast_bus == NULL && !xr_guest_fast_page[0] && !xr_guest_fast_page[0xF0001]);
        g_recomp_guest_bus = NULL;
        free(ram); free(contiguous);
    }
    printf("Guest ref: %u/%u passed\n", s_count - s_failed, s_count);
    return s_failed ? 1 : 0;
}
