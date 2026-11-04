# Explicit guest address translation

`src/platform/recomp_guest_bus.h` and `.c` provide a portable memory bus for
an eventual Xbox 360 host. Guest addresses are looked up in caller-configured
regions; they are never cast into host pointers. RAM access uses the verified
little-endian helpers. This module is compiled into Takedawn's v4 probe but
is not yet substituted for generated `MEM32` lvalues or runtime pointer casts.

## Test

```sh
cmake -S tests/guest_bus -B build/guest-bus
cmake --build build/guest-bus
ctest --test-dir build/guest-bus --output-on-failure
```

Verified on Windows x64 on 2026-11-03: compiled the C test, `bus_checks.c`,
and the bus implementation with VS2022 MSVC `/W4 /WX /O2 /std:c11 /TC`.
All 163 generic cases and 106 retail-layout cases pass. The same files also
pass in Takedawn's native C++ conformance harness alongside the original
208 little-endian and both TLS tests. The CMake commands above have not
been run here. The user's returned v4 log on 2026-11-03 confirms all 163
generic and 106 retail-layout cases also pass on the physical big-endian,
32-bit Xbox 360, using 128 MiB backing and 31 regions. Its original
208 memory checks and both TLS tests still pass. This confirms the tested
bus/profile behavior, not integration with translated game instructions
or real device models.

The generic tests cover aliases in both directions, unaligned scalar and
float transfers, lane transfers, mixed-width access, complete-span
permissions, mapping overlap/capacity, exact 32-bit address-space limits,
and failed reads/stores preserving outputs/RAM. Crossing adjacent RAM
regions is supported, including a mirror wrap. Failure in a later fragment
is detected before the store modifies any earlier fragment.

Device tests use a synthetic callback, not a real emulated device. They
check that one full-width access produces one callback with the original
guest address and a host integer value; unsupported widths, permissions,
and boundary crossings must not invoke it. Callback failure propagates,
and a failed read preserves its output. Side effects performed by a device
callback before it fails cannot be rolled back by the bus.

## Retail hosting profile

`xr_guest_xbox_retail_map` requires separate caller-owned 64 MiB low-RAM
and 64 MiB contiguous buffers. The 31 regions occupy only those 128 MiB
of backing storage:

| Guest window | Backing |
| --- | --- |
| `0x00000000` through `0x03FFFFFF` | Low RAM |
| 28 successive 64 MiB mirrors, ending at `0x73FFFFFF` | The same low RAM |
| `0x80000000` through `0x83FFFFFF` | Separate contiguous buffer |
| `0xF0000000` through `0xF3FFFFFF` | Alias of the contiguous buffer |

This follows the current desktop implementation in
`src/kernel/xbox_memory_layout.c`, including the deliberate separation of
contiguous allocations from low RAM so pinned buffers cannot overwrite the
loaded XBE image. Some older comments in the runtime header describe
hardware physical aliases instead; the profile follows the implementation.
It does not claim to model every original Xbox mapping.

No NV2A/APU/USB/flash windows or behavior are installed by this profile.
Accesses there report unmapped until their real model is explicitly added.
The profile does not reserve a 4 GiB host window and does not install host
exception handlers.

## Contracts and remaining integration

The caller owns and keeps all backing buffers alive, and supplies their
true sizes. Configure the map before starting guest workers. Mappings are
non-overlapping and remain fixed while in use. Read-only and write-only
RAM/device mappings are supported. A borrowed RAM pointer requires a
single contiguous mapping and retains little-endian data representation;
the caller must honor the requested permissions.

These accesses are not atomic. There is no guest allocator, page protection
mutation, cross-thread memory ordering, or hardware-register emulation in
this module. The region lookup is a correctness implementation; full-game
performance has not been measured. Generated accesses, SSE/MMX register
views, atomic instructions, and runtime guest casts still need integration
before running translated game code on the 360.
