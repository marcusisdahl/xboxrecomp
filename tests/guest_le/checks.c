#include "checks.h"
#include "platform/recomp_guest_le.h"

#define CHECK(name, condition) do { \
    int ok = !!(condition); \
    ++count; \
    if (!ok) ++failed; \
    if (report) report(name, ok, context); \
} while (0)

unsigned xr_guest_le_checks(void (*report)(const char *, int, void *),
                            void *context, unsigned *total)
{
    static const uint8_t pattern[16] = {
        0x98,0xba,0xdc,0xfe,0x67,0x45,0x23,0x01,
        0x00,0x00,0xc0,0x3f,0x00,0x00,0x20,0xc0
    };
    static const uint8_t minus_one[8] = {255,255,255,255,255,255,255,255};
    static const uint8_t min16[2] = {0,128};
    static const uint8_t min32[4] = {0,0,0,128};
    static const uint8_t min64[8] = {0,0,0,0,0,0,0,128};
    static const uint8_t double_value[8] = {0,0,0,0,0,0,4,192}; /* -2.5 */
    static const uint8_t fzero[4] = {0,0,0,128};
    static const uint8_t fquiet_nan[4] = {0x45,0x23,0xc1,0x7f};
    static const uint8_t dquiet_nan[8] = {0xbc,0x9a,0x78,0x56,0x34,0x12,0xf8,0x7f};
    uint8_t buffer[48], *p;
    uint32_t lanes[4];
    unsigned offset, count = 0, failed = 0;

    /* Each fixed byte vector tests a known result, not just a round-trip
     * through the same potentially wrong read and write implementation. */
    for (offset = 0; offset < 8; ++offset) {
        memset(buffer, 0xa5, sizeof(buffer));
        p = buffer + 8 + offset;
        memcpy(p, pattern, sizeof(pattern));
        CHECK("READ8", xr_guest_read8(p) == 0x98);
        CHECK("READ16 LE", xr_guest_read16(p) == 0xba98u);
        CHECK("READ32 LE", xr_guest_read32(p) == 0xfedcba98u);
        CHECK("READ64 LE", xr_guest_read64(p) == UINT64_C(0x01234567fedcba98));
        CHECK("SIGNED8", xr_guest_read_s8(p) == -104);
        CHECK("SIGNED16", xr_guest_read_s16(p) == -17768);
        CHECK("SIGNED32", xr_guest_read_s32(p) == -19088744);
        CHECK("SIGNED64 POSITIVE", xr_guest_read_s64(p) == INT64_C(0x01234567fedcba98));
        CHECK("FLOAT32 READ", xr_guest_read_f32(p + 8) == 1.5f);
        memcpy(p, double_value, 8);
        CHECK("FLOAT64 READ", xr_guest_read_f64(p) == -2.5);
        memcpy(p, minus_one, 8);
        CHECK("SIGNED64 NEGATIVE", xr_guest_read_s64(p) == -1);
        memcpy(p, min16, 2);
        CHECK("SIGNED16 MINIMUM", xr_guest_read_s16(p) == -32768);
        memcpy(p, min32, 4);
        CHECK("SIGNED32 MINIMUM", xr_guest_read_s32(p) == (-2147483647 - 1));
        memcpy(p, min64, 8);
        CHECK("SIGNED64 MINIMUM", xr_guest_read_s64(p) == (-INT64_C(9223372036854775807) - 1));

        memset(buffer, 0xa5, sizeof(buffer));
        xr_guest_write8(p, 0x98);
        CHECK("WRITE8 AND GUARDS", p[0] == 0x98 && p[-1] == 0xa5 && p[1] == 0xa5);
        xr_guest_write16(p, 0xba98u);
        CHECK("WRITE16 LE AND GUARDS", !memcmp(p, pattern, 2) && p[-1] == 0xa5 && p[2] == 0xa5);
        xr_guest_write32(p, 0xfedcba98u);
        CHECK("WRITE32 LE AND GUARDS", !memcmp(p, pattern, 4) && p[-1] == 0xa5 && p[4] == 0xa5);
        xr_guest_write64(p, UINT64_C(0x01234567fedcba98));
        CHECK("WRITE64 LE AND GUARDS", !memcmp(p, pattern, 8) && p[-1] == 0xa5 && p[8] == 0xa5);
        xr_guest_write_f32(p, 1.5f);
        CHECK("FLOAT32 WRITE", !memcmp(p, pattern + 8, 4));
        xr_guest_write_f64(p, -2.5);
        CHECK("FLOAT64 WRITE", !memcmp(p, double_value, 8));
        memcpy(p, fzero, 4);
        xr_guest_write_f32(p + 16, xr_guest_read_f32(p));
        CHECK("FLOAT32 NEGATIVE ZERO", !memcmp(p + 16, fzero, 4));
        memcpy(p, fquiet_nan, 4);
        xr_guest_write_f32(p + 16, xr_guest_read_f32(p));
        CHECK("FLOAT32 QUIET NAN PAYLOAD", !memcmp(p + 16, fquiet_nan, 4));
        memcpy(p, dquiet_nan, 8);
        xr_guest_write_f64(p + 16, xr_guest_read_f64(p));
        CHECK("FLOAT64 QUIET NAN PAYLOAD", !memcmp(p + 16, dquiet_nan, 8));

        memcpy(p, pattern, 16);
        xr_guest_read_lanes32(p, lanes);
        CHECK("SIMD MEMORY LANE ORDER", lanes[0] == 0xfedcba98u && lanes[1] == 0x01234567u &&
            lanes[2] == 0x3fc00000u && lanes[3] == 0xc0200000u);
        memset(buffer, 0xa5, sizeof(buffer));
        xr_guest_write_lanes32(p, lanes);
        CHECK("SIMD MEMORY STORE", !memcmp(p, pattern, 16) && p[-1] == 0xa5 && p[16] == 0xa5);
        xr_guest_write8(p + 1, 0x11);
        CHECK("MIXED WIDTH OVERLAP", xr_guest_read32(p) == 0xfedc1198u);
    }
    if (total) *total = count;
    return failed;
}
