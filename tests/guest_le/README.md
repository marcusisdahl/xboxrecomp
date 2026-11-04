# Guest little-endian byte access

Standalone conformance tests for `src/platform/recomp_guest_le.h`, requiring
no game files or runtime libraries. Run through CMake/CTest:

```sh
cmake -S tests/guest_le -B build/guest-le
cmake --build build/guest-le
ctest --test-dir build/guest-le --output-on-failure
```

The same `checks.c` is linked into Takedawn's Xbox 360 runtime probe, so
hardware tests exercise the same access functions and reference vectors.
There are 208 cases over offsets 0 through 7. Fixed byte arrays establish
the expected integer/float values and store byte order independently;
tests do not rely only on a read/write round-trip.

Verified on Windows x64 on 2026-11-03: built the two C files with VS2022
MSVC `/W4 /WX /O2 /TC`, all 208 cases pass. The Xbox 360 probe compiles
these files as C++ with the stock PowerPC compiler and `/fp:precise`.
The user's returned v2 and v3 console logs on 2026-11-03 confirm all 208
checks pass on the physical big-endian, 32-bit Xbox 360. The CMake commands
above have not been run on this machine.

This is a foundation for a big-endian host port, not an enabled host.
`recomp_types.h` still uses native memory lvalues, and the runtime contains
direct guest casts. These must be ported together before this header can
replace guest access throughout the runtime. The helpers have no address
mapping, bounds checking, atomicity, or device behavior; the caller must
supply a valid entire RAM span. Float functions require IEEE-754 layouts
with the same byte order as the corresponding host integer type.
