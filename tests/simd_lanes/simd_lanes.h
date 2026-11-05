/* MMX/SSE lane test for templates/runtime/recomp_types.h.
 *
 * Runs every MMX_* / XMM_* helper (and the lane accesses the lifter emits
 * directly) on pseudo-random operands placed in guest RAM as little-endian
 * bytes, loads them through the same guest-memory path generated code uses,
 * stores the result back through it and hashes the stored bytes. The inputs
 * depend only on the case name, so every host must print the same hash for
 * every case; a big-endian host that disagrees with the x86 goldens has a
 * lane-order bug in that helper. */
#ifndef SIMD_LANES_H
#define SIMD_LANES_H
#include <stddef.h>

typedef void (*SimdLanesLog)(const char *line);

/* Results of one run. ref_* count the cases checked against the host's own
 * MMX/SSE instructions (x86 builds only; 0 elsewhere). place_failed counts
 * the cases whose outputs differ between the guest-memory placements
 * (simd_lanes.cpp: fast path, page-crossing, normal path). */
typedef struct SimdLanesResult {
    unsigned cases;
    unsigned ref_checked;
    unsigned ref_failed;
    unsigned place_failed;
} SimdLanesResult;

/* Writes one "case <name> <hash>" line per case, then a summary line.
 * dump, if not null, receives every case's raw (NaN-canonicalised) outputs
 * in case order, SIMD_LANES_ITER * 16 bytes per case; dump_cap is checked. */
SimdLanesResult simd_lanes_run(SimdLanesLog log, unsigned char *dump,
                               size_t dump_cap, size_t *dump_len);

#define SIMD_LANES_ITER 1024
#define SIMD_LANES_MAX_CASES 160

#endif
