"""Compare two runs of the MMX/SSE lane test (tests/simd_lanes).

    py -3 tests/simd_lanes/compare.py GOLDEN.txt OTHER.txt [GOLDEN.bin OTHER.bin]

GOLDEN is normally build/simd_lanes/simd_lanes_x86_explicit.txt (checked
against the CPU's own MMX/SSE instructions); OTHER is the console's
game:\\simd_lanes.txt. With the two .bin dumps, the first differing iteration
of each failing case is shown, lane by lane. Exit status 1 when any case
differs or is missing.
"""
import re
import sys

ITER = 1024


def cases(path):
    out = []
    with open(path, "r", encoding="ascii", errors="replace") as f:
        for line in f:
            m = re.match(r"\s*case\s+(\S+)\s+([0-9A-Fa-f]{16})", line)
            if m:
                out.append((m.group(1), m.group(2).upper()))
    return out


def main(argv):
    if len(argv) not in (3, 5):
        print(__doc__)
        return 2
    gold, other = cases(argv[1]), cases(argv[2])
    dumps = None
    if len(argv) == 5:
        dumps = [open(p, "rb").read() for p in argv[3:5]]
    other_map = dict(other)
    index = {name: i for i, (name, _) in enumerate(other)}
    bad = 0
    for gi, (name, h) in enumerate(gold):
        got = other_map.get(name)
        if got == h:
            continue
        bad += 1
        print(f"DIFF {name}: golden {h} other {got}")
        if dumps and got is not None:
            a = dumps[0][gi * ITER * 16:(gi + 1) * ITER * 16]
            b = dumps[1][index[name] * ITER * 16:(index[name] + 1) * ITER * 16]
            for it in range(min(len(a), len(b)) // 16):
                x, y = a[it * 16:it * 16 + 16], b[it * 16:it * 16 + 16]
                if x != y:
                    print(f"   first differing iteration {it}: golden {x.hex()} other {y.hex()}")
                    break
    print(f"{len(gold)} golden cases, {len(other)} other cases, {bad} differ")
    return 1 if bad or len(gold) != len(other) else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
