"""
Header audit for register caching (tools.recomp --reg-cache).

Run: py -3 tools/recomp/test_reg_cache_header.py

Under RECOMP_REG_CACHE the bare register names (eax, esp, mm4, xmm0) in a
generated function mean that function's local copies, not the TLS globals.
A header macro that names a register bare would therefore read or write the
caller's local, and the RC_FILL after the call would then silently overwrite
what the macro wrote. RECOMP_ICALL's failure path did exactly that with
`eax = 0` until it was changed to g_eax. This keeps every macro and inline
body of the runtime header on the global names, outside the mapping block
itself.
"""

import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
HEADER = os.path.join(ROOT, "templates", "runtime", "recomp_types.h")
INCLUDED = [
    os.path.join(ROOT, "src", "platform", "recomp_guest_ref.h"),
    os.path.join(ROOT, "src", "platform", "recomp_guest_bus.h"),
    os.path.join(ROOT, "src", "platform", "recomp_guest_le.h"),
    os.path.join(ROOT, "src", "platform", "recomp_guest_atomic_win.h"),
    os.path.join(ROOT, "src", "platform", "recomp_c99_x360.h"),
    os.path.join(ROOT, "src", "kernel", "recomp_icall_feedback.h"),
]

REG = re.compile(r"\b(eax|ecx|edx|ebx|esp|esi|edi|mm[0-7]|xmm[0-7])\b")
MAPPING = re.compile(
    r"^\s*#\s*define\s+(eax|ecx|edx|ebx|esp|esi|edi|mm[0-7]|xmm[0-7])\s+"
    r"(g|rc)_\1\s*$")


def _code_lines(text):
    """Source lines with comments and string literals blanked out."""
    text = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"),
                  text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    text = re.sub(r'"(?:\\.|[^"\\\n])*"', '""', text)
    return text.split("\n")


def _read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def test_no_bare_register_outside_the_mapping():
    bad = []
    for path in [HEADER] + [p for p in INCLUDED if os.path.exists(p)]:
        for n, line in enumerate(_code_lines(_read(path)), 1):
            if REG.search(line) and not MAPPING.match(line):
                bad.append(f"{os.path.basename(path)}:{n}: {line.strip()}")
    assert not bad, "bare register names in runtime headers:\n" + "\n".join(bad)


def test_mapping_covers_every_register_both_ways():
    lines = _code_lines(_read(HEADER))
    pairs = set()
    for line in lines:
        m = MAPPING.match(line)
        if m:
            pairs.add((m.group(1), m.group(2)))
    regs = ["eax", "ecx", "edx", "ebx", "esp", "esi", "edi"]
    regs += [f"mm{i}" for i in range(8)] + [f"xmm{i}" for i in range(8)]
    for r in regs:
        assert (r, "g") in pairs, f"no global mapping for {r}"
        assert (r, "rc") in pairs, f"no cached mapping for {r}"


def test_supported_marker_and_switches():
    text = _read(HEADER)
    assert re.search(r"#define RECOMP_REG_CACHE_SUPPORTED 1", text)
    assert ("#if defined(RECOMP_REG_CACHE) && !defined(RECOMP_REG_CACHE_OFF)"
            in text)
    for m in ("RC_DECL_GPR", "RC_DECL_MMX", "RC_DECL_XMM", "RC_S", "RC_L"):
        # Defined in both branches: as code when caching, empty otherwise.
        assert len(re.findall(r"#define %s\(r\)" % m, text)) == 2, m
    # The spin wait spills and refills around the out-of-line call.
    assert re.search(r"RC_SPILL\(\); xbox_SpinWait\(va\); RC_FILL\(\);", text)


def test_no_global_named_rc():
    # The fail-safe: an undeclared cached register is a compile error only
    # while nothing else defines a name rc_eax.
    for path in [HEADER] + [p for p in INCLUDED if os.path.exists(p)]:
        for line in _code_lines(_read(path)):
            if MAPPING.match(line) or "##" in line:
                continue
            assert not re.search(
                r"\brc_(eax|ecx|edx|ebx|esp|esi|edi|mm[0-7]|xmm[0-7])\b",
                line), line


if __name__ == "__main__":
    test_no_bare_register_outside_the_mapping()
    test_mapping_covers_every_register_both_ways()
    test_supported_marker_and_switches()
    test_no_global_named_rc()
    print("all passed")
