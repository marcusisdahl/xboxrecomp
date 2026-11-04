"""
Self-check for the opt-in --explicit-guest-bus translation mode.

Run: py -3 tools/recomp/test_explicit_guest_bus.py

Synthetic inputs only: no XBE and no game data is read. Checks that

  * the CLI exposes the flag and BatchTranslator threads it to the Lifter;
  * every generated translation unit (single-file output, the split header
    and therefore every chunk and stub file that includes it) switches the
    memory model on BEFORE recomp_types.h is parsed;
  * the prologue tolerates the same macro on the compiler command line and
    refuses a recomp_types.h that was already included in desktop mode;
  * REP MOVS/STOS lower to elementwise MEMn accesses with no raw host
    pointer, while the default (desktop) output is byte-for-byte unchanged.

If MSVC's cl.exe is on PATH (a VS developer prompt), the generated split
header and a chunk are also compiled as C++ against the real runtime header
in explicit mode, and the include-order guard is checked to fire.
"""

import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "build", "test-python"))
sys.path.insert(0, ROOT)

from tools.recomp import translator as tr  # noqa: E402
from tools.recomp.lifter import Lifter  # noqa: E402

PROLOGUE = tr.EXPLICIT_GUEST_BUS_PROLOGUE
FAILS = []


def check(cond, what):
    if not cond:
        FAILS.append(what)
        print("FAIL:", what)


class _StubLifter:
    def __init__(self):
        self.referenced_calls = {}
        self.unimplemented = {}
        self.manual_functions = set()
        self.wrapped_functions = set()


class _StubTranslator:
    """Stands in for FunctionTranslator: returns fixed bodies."""
    def __init__(self):
        self.owned_function_starts = set()
        self.lifter = _StubLifter()

    def translate_function(self, addr, info):
        return f"void {info['name']}(void) {{ MEM32(0x{addr:08X}u) = 1; }}\n"


def _batch(explicit, out):
    b = tr.BatchTranslator.__new__(tr.BatchTranslator)
    b.explicit_guest_bus = explicit
    b.output_dir = out
    b.translator = _StubTranslator()
    b.abi_db = {}
    b.title = None
    return b


FUNCS = [(0x00011000, {"name": "sub_00011000"}),
         (0x00011010, {"name": "sub_00011010"})]


def _prologue_before_include(text, where):
    lines = text.splitlines()
    inc = lines.index('#include "recomp_types.h"')
    check(lines[inc - len(PROLOGUE):inc] == PROLOGUE,
          f"{where}: explicit prologue directly precedes recomp_types.h")


def test_cli_flag():
    r = subprocess.run([sys.executable, "-m", "tools.recomp", "--help"],
                       cwd=ROOT, capture_output=True, text=True)
    check(r.returncode == 0 and "--explicit-guest-bus" in r.stdout,
          "CLI --help lists --explicit-guest-bus")


def test_threading():
    # FunctionTranslator -> Lifter, without an XBE.
    ft = tr.FunctionTranslator(b"", {}, explicit_guest_bus=True)
    check(ft.lifter.explicit_guest_bus is True, "FunctionTranslator threads flag")
    ft = tr.FunctionTranslator(b"", {})
    check(ft.lifter.explicit_guest_bus is False, "FunctionTranslator default off")
    # BatchTranslator.__init__ forwards it to its FunctionTranslator.
    src = open(tr.__file__, encoding="utf-8").read()
    check("explicit_guest_bus=explicit_guest_bus)" in src,
          "BatchTranslator passes explicit_guest_bus to FunctionTranslator")


def test_outputs():
    with tempfile.TemporaryDirectory() as d:
        for explicit in (True, False):
            sub = os.path.join(d, "exp" if explicit else "def")
            os.makedirs(sub)
            b = _batch(explicit, sub)
            single = os.path.join(sub, "single.c")
            b.translate_batch(list(FUNCS), output_file=single)
            text = open(single, encoding="utf-8").read()
            if explicit:
                _prologue_before_include(text, "single-file")
                check(text.index("#define RECOMP_GENERATED_CODE")
                      < text.index("#ifndef RECOMP_EXPLICIT_GUEST_BUS"),
                      "single-file keeps RECOMP_GENERATED_CODE first")
            else:
                check("RECOMP_EXPLICIT_GUEST_BUS" not in text,
                      "single-file default has no explicit mode")
            gen = os.path.join(sub, "gen")
            b.translate_batch_split(list(FUNCS), gen, chunk_size=1)
            hdr = open(os.path.join(gen, "recomp_funcs.h"), encoding="utf-8").read()
            if explicit:
                _prologue_before_include(hdr, "split header")
            else:
                check("RECOMP_EXPLICIT_GUEST_BUS" not in hdr,
                      "split header default has no explicit mode")
            for name in sorted(os.listdir(gen)):
                if not name.endswith(".c"):
                    continue
                body = open(os.path.join(gen, name), encoding="utf-8").read()
                includes = [l for l in body.splitlines() if l.startswith("#include")]
                check(includes and includes[0] == '#include "recomp_funcs.h"',
                      f"{name}: first include is the mode-setting header")
                check('#include "recomp_types.h"' not in body,
                      f"{name}: does not include recomp_types.h directly")
        if shutil.which("cl"):
            _compile_check(os.path.join(d, "exp", "gen"))


def _compile_check(gen):
    inc = ["/I" + gen, "/I" + os.path.join(ROOT, "src"),
           "/I" + os.path.join(ROOT, "src", "platform")]
    base = ["cl", "/nologo", "/c", "/TP", "/std:c++17", "/W3", "/WX",
            "/Fo" + gen + os.sep] + inc
    chunk = os.path.join(gen, "recomp_0000.c")
    r = subprocess.run(base + [chunk], capture_output=True, text=True)
    check(r.returncode == 0, "MSVC compiles an explicit chunk as C++:\n"
          + r.stdout[-2000:])
    r = subprocess.run(base + ["/DRECOMP_EXPLICIT_GUEST_BUS", chunk],
                       capture_output=True, text=True)
    check(r.returncode == 0, "explicit chunk tolerates /DRECOMP_EXPLICIT_GUEST_BUS:\n"
          + r.stdout[-2000:])
    bad = os.path.join(gen, "bad_order.cpp")
    with open(bad, "w") as f:
        f.write('#include "recomp_types.h"\n#include "recomp_funcs.h"\n')
    r = subprocess.run(base + [bad], capture_output=True, text=True)
    check(r.returncode != 0 and "before explicit guest-bus mode" in r.stdout,
          "include-order guard fires when recomp_types.h came first")
    print("  (compiled with MSVC)")


REP = {
    "rep movsb": (1, True), "rep movsw": (2, True), "rep movsd": (4, True),
    "rep stosb": (1, False), "rep stosw": (2, False), "rep stosd": (4, False),
}


def test_rep_lowering():
    for m, (width, movs) in REP.items():
        exp = "\n".join(Lifter(explicit_guest_bus=True)._lift_rep_string(None, m))
        check("XBOX_PTR" not in exp and "memcpy" not in exp and "memset" not in exp,
              f"{m}: explicit form has no raw host pointer")
        check(f"MEM{width * 8}(edi) = " in exp, f"{m}: stores MEM{width * 8}(edi)")
        check(f"RECOMP_DF_STEP({width})" in exp, f"{m}: honours DF")
        check(("esi += _step" in exp) == movs, f"{m}: esi only advances for movs")
        check("while (ecx)" in exp and "--ecx" in exp, f"{m}: consumes ecx")
        # Desktop path untouched.
        dflt = Lifter()._lift_rep_string(None, m)
        check(dflt == Lifter(explicit_guest_bus=False)._lift_rep_string(None, m)
              and "while (ecx) {" not in "\n".join(dflt),
              f"{m}: default form unchanged")
    # Compares/scans are not routed through the new path.
    a = Lifter(explicit_guest_bus=True)._lift_rep_string(None, "repe cmpsb")
    b = Lifter()._lift_rep_string(None, "repe cmpsb")
    check(a == b, "repe cmpsb identical in both modes")


def test_atomics():
    from tools.recomp.disasm import Disassembler
    # lock xadd [ecx], eax ; lock cmpxchg [ecx], edx  (synthetic bytes)
    raw = bytes.fromhex("f00fc101" "f00fb111")
    insns = Disassembler().disassemble_function(raw, 0x10000, 0x10000 + len(raw))
    for explicit in (True, False):
        lifter = Lifter(explicit_guest_bus=explicit)
        out = "\n".join(s for i in insns for s in lifter.lift_instruction(i))
        if explicit:
            check("RECOMP_GUEST_ATOMIC_ADD32((uint32_t)(" in out
                  and "RECOMP_GUEST_ATOMIC_CAS32((uint32_t)(" in out
                  and "XBOX_PTR" not in out, "explicit atomics use guest addresses")
        else:
            check("RECOMP_ATOMIC_ADD32(XBOX_PTR(" in out
                  and "RECOMP_ATOMIC_CAS32(XBOX_PTR(" in out,
                  "default atomics unchanged")


if __name__ == "__main__":
    test_atomics()
    test_cli_flag()
    test_threading()
    test_outputs()
    test_rep_lowering()
    if FAILS:
        print(f"{len(FAILS)} failure(s)")
        sys.exit(1)
    print("explicit guest bus: all checks passed")
