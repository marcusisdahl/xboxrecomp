"""
Check that --reg-cache output differs from the uncached output only by the
register cache itself.

usage: py -3 -m tools.recomp.regcache_diffcheck GEN_OFF GEN_ON

GEN_OFF is a generated dir made without --reg-cache, GEN_ON the same inputs
with it. Every recomp_*.c of GEN_ON is normalised -- the RC_* spill, fill and
declaration emissions, the per-function RC_SPILL/RC_FILL definitions, the
push_macro wrappers of uncached registers and the chunk prologue are removed,
and the two spellings the cache changes (`_icall_esp = esp`, and `g_eax` in
a forced tail return or a setjmp resume) are mapped back -- and must then
equal the GEN_OFF file byte for byte. recomp_types.h is not compared.

Exits 0 when identical, 1 otherwise, and prints statistics: functions cached,
average size of the cached set, spill and fill sites, excluded functions.
"""

import glob
import os
import re
import sys

_DROP_LINE = re.compile(
    r"^(?:#define RECOMP_REG_CACHE 1"
    r"|#ifndef RECOMP_REG_CACHE_SUPPORTED"
    r"|#error recomp_types\.h predates --reg-cache.*"
    r"|#define RC_SPILL\(\) .*|#define RC_FILL\(\) .*"
    r"|#undef RC_SPILL|#undef RC_FILL"
    r"|\s*(?:RC_DECL_(?:GPR|MMX|XMM)\(\w+\)\s*)+)$")
_REG = r"(?:eax|ecx|edx|ebx|esp|esi|edi|mm[0-7]|xmm[0-7])"
_PUSH = re.compile(r'^#pragma push_macro\("(%s)"\)$' % _REG)
_POP = re.compile(r'^#pragma pop_macro\("(%s)"\)$' % _REG)


def normalise(text):
    """GEN_ON chunk text -> what GEN_OFF should hold. Also returns stats."""
    out = []
    stats = {"cached": 0, "regs": 0, "spills": 0, "fills": 0, "excluded": []}
    lines = text.split("\n")
    i = 0
    pending_guard_endif = False
    while i < len(lines):
        line = lines[i]
        if line == "#ifndef RECOMP_REG_CACHE_SUPPORTED":
            pending_guard_endif = True
            i += 1
            continue
        if pending_guard_endif and line == "#endif":
            pending_guard_endif = False
            i += 1
            continue
        m = re.match(r"^#define RC_SPILL\(\) do \{ (.*) \} while \(0\)$", line)
        if m:
            regs = re.findall(r"RC_S\((\w+)\)", m.group(1))
            if regs:
                stats["cached"] += 1
                stats["regs"] += len(regs)
        if _DROP_LINE.match(line):
            i += 1
            continue
        pm = _PUSH.match(line)
        if pm:
            r = pm.group(1)
            # push_macro, #undef r, #define r g_r
            i += 3
            continue
        if _POP.match(line):
            i += 1
            continue
        stats["spills"] += line.count("RC_SPILL();")
        stats["fills"] += line.count("RC_FILL();")
        line = line.replace("RC_SPILL(); ", "").replace(" RC_FILL();", "")
        line = line.replace("{ uint32_t _icall_esp = esp;",
                            "{ uint32_t _icall_esp = g_esp;")
        line = re.sub(r"(\(\); if \(g_force_return\)) g_eax = ", r"\1 eax = ",
                      line)
        line = re.sub(r"(RECOMP_ITAIL\([^;]*\); if \(g_force_return\)) g_eax = ",
                      r"\1 eax = ", line)
        line = line.replace("ebp = g_seh_ebp; g_eax = (uint32_t)_sjv;",
                            "ebp = g_seh_ebp; eax = (uint32_t)_sjv;")
        out.append(line)
        i += 1
    return "\n".join(out), stats


def _excluded(text):
    """Functions with uncached registers wrapped in push_macro."""
    names = []
    for m in re.finditer(r'#pragma push_macro\("\w+"\)\n#undef \w+\n'
                         r'#define \w+ g_\w+\n(?:.*\n)*?void (\w+)\(void\)',
                         text):
        names.append(m.group(1))
    return sorted(set(names))


def compare(gen_off, gen_on, out=sys.stdout):
    files = sorted(os.path.basename(p) for p in
                   glob.glob(os.path.join(gen_on, "recomp_*.c")))
    off_files = sorted(os.path.basename(p) for p in
                       glob.glob(os.path.join(gen_off, "recomp_*.c")))
    ok = files == off_files
    if not ok:
        print(f"file sets differ: only on {sorted(set(files) - set(off_files))}"
              f", only off {sorted(set(off_files) - set(files))}", file=out)
    total = {"cached": 0, "regs": 0, "spills": 0, "fills": 0}
    excluded = []
    size_on = size_off = lines_on = lines_off = 0
    for name in files:
        with open(os.path.join(gen_on, name), encoding="utf-8") as f:
            on = f.read()
        try:
            with open(os.path.join(gen_off, name), encoding="utf-8") as f:
                off = f.read()
        except OSError:
            continue
        size_on += len(on.encode("utf-8"))
        size_off += len(off.encode("utf-8"))
        lines_on += on.count("\n")
        lines_off += off.count("\n")
        norm, st = normalise(on)
        for k in total:
            total[k] += st[k]
        excluded += _excluded(on)
        if norm != off:
            ok = False
            a, b = norm.split("\n"), off.split("\n")
            for n, (x, y) in enumerate(zip(a, b), 1):
                if x != y:
                    print(f"{name}:{n}: normalised on != off\n  on : {x}\n"
                          f"  off: {y}", file=out)
                    break
            else:
                print(f"{name}: lengths differ ({len(a)} vs {len(b)} lines)",
                      file=out)
    avg = total["regs"] / total["cached"] if total["cached"] else 0.0
    print(f"chunks: {len(files)}; normalised output "
          f"{'identical' if ok else 'DIFFERENT'}", file=out)
    print(f"functions cached: {total['cached']}; average cached set "
          f"{avg:.2f} registers (written back: all cached, --reg-cache-spill "
          f"all)", file=out)
    print(f"spill sites: {total['spills']}; fill sites: {total['fills']}",
          file=out)
    print(f"excluded/partly uncached functions: {len(excluded)}"
          + (f" ({', '.join(excluded[:20])}{' ...' if len(excluded) > 20 else ''})"
             if excluded else ""), file=out)
    print(f"size: off {size_off} bytes / {lines_off} lines, on {size_on} "
          f"bytes / {lines_on} lines (+{size_on - size_off} bytes, "
          f"+{lines_on - lines_off} lines)", file=out)
    return ok


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if len(argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    return 0 if compare(argv[0], argv[1]) else 1


if __name__ == "__main__":
    sys.exit(main())
