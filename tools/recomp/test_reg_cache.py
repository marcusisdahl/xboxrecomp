"""
Self-check for register caching in function locals (--reg-cache).

Run: py -3 tools/recomp/test_reg_cache.py

Under the cache a generated function keeps the guest registers it names in C
locals. Every point where something other than the function's own code can
read or write the registers -- calls, indirect calls, tail calls, returns,
traces -- must write the locals back first (RC_SPILL), and every call that
returns must reload them (RC_FILL). A site that misses either is a silent
stale-register bug, so this checks the exact emission at each kind of site,
that the cache off is byte-for-byte the old output, the cached-set
computation, the generation-time lint, the chunk prologue, and the diff
checker that compares a cached generation against an uncached one.

Synthetic inputs only; no XBE or game data is read.
"""

import os
import re
import struct
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, ROOT)

from tools.recomp import config  # noqa: E402
from tools.recomp import translator as tr  # noqa: E402
from tools.recomp import regcache_diffcheck  # noqa: E402
from tools.recomp.translator import FunctionTranslator  # noqa: E402

BASE = 0x00010000

# Function layout of the synthetic image: (offset, bytes).
F_MAIN = 0x00     # call F_RET8 ; call [ecx+4] ; ret
F_RET8 = 0x20     # ret 8
F_TAIL = 0x30     # jmp F_RET8
F_ITAIL = 0x40    # jmp eax
F_JCC = 0x50      # test eax, eax ; jz F_RET8 ; ret
F_JECXZ = 0x60    # jecxz F_RET8 ; ret
F_FLAGLESS = 0x70  # jz F_RET8 ; ret
F_FALL = 0x80     # mov al, 1   (falls through into F_FALL2)
F_FALL2 = 0x82    # ret
F_REP = 0x90      # mov al, 1 ; rep movsd ; ret
F_MMX = 0xA0      # movq mm0, mm1 ; ret
F_X87 = 0xB0      # fld1 ; fstp st(0) ; ret
F_SEH = 0xC0      # call F_SEHP ; ret
F_SEHP = 0xD0     # ret          (the SEH prolog helper)
F_SWITCH = 0xE0   # jmp [eax*4 + TABLE] ; ret ; ret
TABLE = 0xEC
F_RETN = 0x100    # pop esi ; ret 4
SIZE = 0x110


def _rel32(at, insn_len, target):
    return struct.pack("<i", (BASE + target) - (BASE + at + insn_len))


def _image():
    img = bytearray(b"\xcc" * SIZE)

    def put(off, data):
        img[off:off + len(data)] = data

    put(F_MAIN, b"\xe8" + _rel32(F_MAIN, 5, F_RET8)
        + b"\xff\x51\x04" + b"\xc3")
    put(F_RET8, b"\xc2\x08\x00")
    put(F_TAIL, b"\xe9" + _rel32(F_TAIL, 5, F_RET8))
    put(F_ITAIL, b"\xff\xe0")
    put(F_JCC, b"\x85\xc0" + b"\x0f\x84" + _rel32(F_JCC + 2, 6, F_RET8)
        + b"\xc3")
    put(F_JECXZ, b"\xe3" + struct.pack("<b", F_RET8 - (F_JECXZ + 2)) + b"\xc3")
    put(F_FLAGLESS, b"\x0f\x84" + _rel32(F_FLAGLESS, 6, F_RET8) + b"\xc3")
    put(F_FALL, b"\xb0\x01")
    put(F_FALL2, b"\xc3")
    put(F_REP, b"\xb0\x01" + b"\xf3\xa5" + b"\xc3")
    put(F_MMX, b"\x0f\x6f\xc1" + b"\xc3")
    put(F_X87, b"\xd9\xe8" + b"\xdd\xd8" + b"\xc3")
    put(F_SEH, b"\xe8" + _rel32(F_SEH, 5, F_SEHP) + b"\xc3")
    put(F_SEHP, b"\xc3")
    put(F_SWITCH, b"\xff\x24\x85" + struct.pack("<I", BASE + TABLE)
        + b"\xc3\xc3")
    put(TABLE, struct.pack("<II", BASE + F_SWITCH + 7, BASE + F_SWITCH + 8))
    put(F_RETN, b"\x5e" + b"\xc2\x04\x00")
    config._install(
        [config.Section(".text", BASE, len(img), 0, len(img), True)],
        entry_point=BASE, kernel_thunk_addr=BASE, origin="reg-cache-test")
    return bytes(img)


_SIZES = {F_MAIN: 9, F_RET8: 3, F_TAIL: 5, F_ITAIL: 2, F_JCC: 9, F_JECXZ: 3,
          F_FLAGLESS: 7, F_FALL: 2, F_FALL2: 1, F_REP: 5, F_MMX: 4, F_X87: 5,
          F_SEH: 6, F_SEHP: 1, F_SWITCH: 9, F_RETN: 4}


def _db():
    return {BASE + off: {"start": f"0x{BASE + off:08X}", "end": BASE + off + n,
                         "_addr": BASE + off, "size": n,
                         "name": f"sub_{BASE + off:08X}"}
            for off, n in _SIZES.items()}


def _ft(level="full", **kw):
    img, db = _image(), _db()
    ft = FunctionTranslator(img, db, reg_cache=level,
                            seh_prolog=BASE + F_SEHP, seh_epilog=None, **kw)
    return ft, db


def _lift(off, level="full", **kw):
    ft, db = _ft(level, **kw)
    return ft.translate_function(BASE + off, db[BASE + off])


def _n(off):
    return f"sub_{BASE + off:08X}"


ALL = sorted(_SIZES)


# 1. off is unchanged ------------------------------------------------------

def test_off_is_the_old_output():
    # A Lifter that has the cache configured but not switched on for the
    # function must emit exactly what the plain default does.
    for off in ALL:
        plain = _lift(off, "off")
        assert "RC_" not in plain, plain
        ft, db = _ft("full", reg_cache_exclude={BASE + off})
        excl = ft.translate_function(BASE + off, db[BASE + off])
        # An excluded function only gains the empty per-function macros and
        # the push_macro wrappers around it; its body is the old one.
        norm, _ = regcache_diffcheck.normalise(excl)
        assert norm == plain, (off, excl, plain)


def test_off_spellings():
    c = _lift(F_MAIN, "off")
    assert (f"PUSH32(esp, 0x{BASE + 5:08X}u); RECOMP_ABI_CALL("
            f"0x{BASE + F_RET8:08X}u, {_n(F_RET8)}); /* call") in c, c
    assert "{ uint32_t _icall_esp = g_esp;" in c, c
    assert (f"RECOMP_ICALL_SAFE_AT(_icall_target, _icall_esp, "
            f"0x{BASE + 5:08X}u); }} /* indirect call */") in c, c
    assert "esp += 4; return; /* ret */" in c, c
    assert f"g_seh_ebp = ebp; {_n(F_RET8)}(); return;" in _lift(F_TAIL, "off")


# 2. direct call -------------------------------------------------------------

def test_direct_call_shape():
    c = _lift(F_MAIN)
    assert (f"PUSH32(esp, 0x{BASE + 5:08X}u); RC_SPILL(); RECOMP_ABI_CALL("
            f"0x{BASE + F_RET8:08X}u, {_n(F_RET8)}); RC_FILL(); /* call") in c, c


# 3. indirect call -----------------------------------------------------------

def test_indirect_call_shape():
    c = _lift(F_MAIN)
    line = next(l for l in c.splitlines() if "indirect call" in l)
    assert line.index("_icall_target = MEM32(ecx + 4)") < line.index("PUSH32"), line
    assert line.index("PUSH32") < line.index("RC_SPILL();") \
        < line.index("RECOMP_ICALL_SAFE_AT(") < line.index("RC_FILL();"), line
    assert "{ uint32_t _icall_esp = esp;" in c, c
    assert "_icall_esp = g_esp" not in c, c


def test_guarded_indirect_call_has_one_spill_and_one_fill():
    c = _lift(F_MAIN, icall_sites={BASE + 5: [BASE + F_RET8, BASE + F_TAIL]})
    line = next(l for l in c.splitlines() if "guarded 2" in l)
    assert line.count("RC_SPILL();") == 1 and line.count("RC_FILL();") == 1, line
    assert line.index("RC_SPILL();") < line.index("if (_icall_target ==")
    assert line.rindex("RC_FILL();") > line.rindex("RECOMP_ICALL_SAFE_AT(")
    assert line.endswith("} RC_FILL(); } /* indirect call, guarded 2 */"), line


def test_manual_call_shape():
    ft, db = _ft()
    ft.lifter.manual_functions = {BASE + F_RET8}
    c = ft.translate_function(BASE + F_MAIN, db[BASE + F_MAIN])
    assert (f"PUSH32(esp, 0x{BASE + 5:08X}u); RC_SPILL(); RECOMP_ICALL_SAFE("
            f"0x{BASE + F_RET8:08X}u, _icall_esp); RC_FILL(); /* manual call")\
        in c, c


# 4. ret ---------------------------------------------------------------------

def test_ret_shapes():
    assert "esp += 4; RC_SPILL(); return; /* ret */" in _lift(F_MAIN)
    c = _lift(F_RETN)
    assert "POP32(esp, esi);" in c and \
        "esp += 8; RC_SPILL(); return; /* ret 4 */" in c, c


def test_ret_variants_order():
    # SEH helper: publish the frame, then spill.
    c = _lift(F_SEHP)
    assert "g_seh_ebp = ebp; esp += 4; RC_SPILL(); return; /* ret */" in c, c
    # force_return writes the local before the spill carries it out.
    c = _lift(F_RETN, force_returns={BASE + F_RETN: 7})
    assert ("if (g_force_return) eax = 0x7U; esp += 8; RC_SPILL(); return;"
            in c), c
    # The exit trace reads the globals, so it gets its own spill.
    c = _lift(F_RETN, trace_functions={BASE + F_RETN})
    assert ('RC_SPILL(); RECOMP_TRACE_EXIT("%s", 0x%08X); esp += 8; '
            'RC_SPILL(); return; /* ret 4 */' % (_n(F_RETN), BASE + F_RETN)) \
        in c, c
    assert re.search(r'RC_SPILL\(\); RECOMP_TRACE_ESP\("%s", "pop esi"\);'
                     % _n(F_RETN), c), c
    # The entry trace stays before the declarations.
    lines = c.splitlines()
    i = lines.index("{")
    assert "RECOMP_TRACE_ENTER(" in lines[i + 1], lines[i + 1]
    assert lines[i + 2].lstrip().startswith("RC_DECL_GPR("), lines[i + 2]


# 5. every tail form ---------------------------------------------------------

def _tail_ok(c, call):
    want = f"g_seh_ebp = ebp; RC_SPILL(); {call}; return;"
    assert want in c, c
    for line in c.splitlines():
        if want in line:
            assert "RC_FILL" not in line, line


def test_tail_forms():
    _tail_ok(_lift(F_TAIL), f"{_n(F_RET8)}()")
    _tail_ok(_lift(F_ITAIL), "RECOMP_ITAIL(eax)")
    _tail_ok(_lift(F_JCC), f"{_n(F_RET8)}()")
    _tail_ok(_lift(F_JECXZ), f"{_n(F_RET8)}()")
    _tail_ok(_lift(F_FLAGLESS), f"{_n(F_RET8)}()")
    _tail_ok(_lift(F_FALL), f"{_n(F_FALL2)}()")
    sw = _lift(F_SWITCH)
    assert "goto loc_" in sw, sw
    _tail_ok(sw, "RECOMP_ITAIL(_jt)")


def test_manual_tail_forms():
    ft, db = _ft()
    ft.lifter.manual_functions = {BASE + F_RET8}
    for off in (F_TAIL, F_JCC):
        c = ft.translate_function(BASE + off, db[BASE + off])
        _tail_ok(c, f"RECOMP_ITAIL(0x{BASE + F_RET8:08X}u)")


def test_forced_tail_writes_the_global():
    c = _lift(F_TAIL, force_returns={BASE + F_TAIL: 1})
    assert (f"RC_SPILL(); {_n(F_RET8)}(); if (g_force_return) g_eax = 0x1U;"
            f" return;") in c, c
    c = _lift(F_TAIL, "off", force_returns={BASE + F_TAIL: 1})
    assert f"{_n(F_RET8)}(); if (g_force_return) eax = 0x1U; return;" in c, c


def test_trap_and_fallthrough_spill():
    ft, _ = _ft()
    ft.lifter.reg_cache_on = True
    assert ft.lifter._rcs() == "RC_SPILL(); "
    assert ft.lifter._tail_bridge() == "g_seh_ebp = ebp; RC_SPILL(); "
    src = open(tr.__file__, encoding="utf-8").read()
    assert 'f"    {self.lifter._rcs()}return;"' in src
    assert "{self.lifter._tail_bridge()}{ft_name}(); return;" in src


# 6. SEH ---------------------------------------------------------------------

def test_seh_call_order():
    c = _lift(F_SEH)
    body = c[c.index("loc_"):]
    seq = [body.index(s) for s in (
        "g_seh_ebp = ebp; /* publish frame to SEH helper */",
        "RC_SPILL(); RECOMP_ABI_CALL(",
        "RC_FILL();",
        "ebp = g_seh_ebp; /* read back frame from SEH helper */")]
    assert seq == sorted(seq), c


# 7. the cached set ----------------------------------------------------------

def _decl(c):
    return next((l.strip() for l in c.splitlines() if "RC_DECL_" in l), "")


def test_cached_set():
    assert "RC_DECL_GPR(eax)" in _decl(_lift(F_FALL))       # al -> eax
    d = _decl(_lift(F_REP))
    for r in ("ecx", "esi", "edi"):
        assert f"RC_DECL_GPR({r})" in d, d
    d = _decl(_lift(F_MMX))
    assert "RC_DECL_MMX(mm0)" in d and "RC_DECL_MMX(mm1)" in d, d
    c = _lift(F_X87)
    assert "fp_stack" not in _decl(c) and "RC_DECL_" in c, c
    assert "#define RC_SPILL() do { RC_S(esp) } while (0)" in c, c
    # A register named only in a comment or a string declares nothing.
    used = tr.rc_used_registers([
        "    /* mov eax, ecx */", '    RECOMP_UNIMPL("mov edx, ebx", 0x1u);',
        "    esp += 4; return; /* ret */"])
    assert used == {"esp"}, used


def test_gpr_level_wraps_uncached_mmx():
    c = _lift(F_MMX, "gpr")
    assert "RC_DECL_MMX" not in c, c
    for r in ("mm0", "mm1"):
        assert (f'#pragma push_macro("{r}")\n#undef {r}\n#define {r} g_{r}'
                in c), c
        assert f'#pragma pop_macro("{r}")' in c, c
    assert c.index("#pragma push_macro") < c.index("void sub_")
    assert c.index("#pragma pop_macro") > c.rindex("}")


def test_defines_bracket_each_function():
    c = _lift(F_MAIN)
    lines = c.rstrip("\n").splitlines()
    assert lines[0].startswith("#define RC_SPILL() do { RC_S(")
    assert lines[1].startswith("#define RC_FILL() do { RC_L(")
    assert lines[-2:] == ["#undef RC_SPILL", "#undef RC_FILL"], lines[-3:]


# 8. the lint ----------------------------------------------------------------

def _lint(*body, cached=("eax", "esp")):
    tr.reg_cache_lint(list(body), "sub_test", set(cached))


def _raises(*body):
    try:
        _lint(*body)
    except ValueError:
        return True
    return False


def test_lint():
    _lint("    PUSH32(esp, 0x1u); RC_SPILL(); sub_00011000(); RC_FILL();",
          "    eax = xbox_MmioLoad(0xFD000000u, 4);",
          "    esp += 4; RC_SPILL(); return;")
    assert _raises("    foo();")
    assert _raises("    esp += 4; return;")
    assert _raises("    RC_SPILL(); eax = 1; sub_00011000(); RC_FILL();")
    assert _raises("    RC_SPILL(); sub_00011000(); eax = 1; RC_FILL();")
    assert _raises("    RC_SPILL(); sub_00011000();")
    assert _raises("    RC_SPILL(); RECOMP_ITAIL(eax);")
    # Arguments are evaluated before the call; reading a register there is
    # fine, and a read-only trace needs no fill.
    _lint("    RC_SPILL(); RECOMP_ITAIL(MEM32(esp + 4)); return;",
          '    RC_SPILL(); RECOMP_TRACE_ESP("f", "pop eax");',
          "    POP32(esp, eax);")


# 9. header names the lint relies on ---------------------------------------

def test_boundary_names_exist():
    hdr = open(os.path.join(ROOT, "templates", "runtime", "recomp_types.h"),
               encoding="utf-8").read()
    for name in sorted(tr.RC_BOUNDARY | tr.RC_READ_ONLY_BOUNDARY):
        if name == "setjmp":
            assert "#include <setjmp.h>" in hdr
            continue
        assert re.search(r"\b%s\b" % name, hdr), name
    assert "#define RECOMP_REG_CACHE_SUPPORTED 1" in hdr


# 10. chunk prologue -------------------------------------------------------

class _StubTranslator:
    def __init__(self):
        self.owned_function_starts = set()

        class _L:
            referenced_calls = {}
            unimplemented = {}
            manual_functions = set()
            wrapped_functions = set()
        self.lifter = _L()

    def translate_function(self, addr, info):
        return f"void {info['name']}(void) {{ }}\n"


def _batch(reg_cache, out):
    b = tr.BatchTranslator.__new__(tr.BatchTranslator)
    b.explicit_guest_bus = True
    b.reg_cache = reg_cache
    b.output_dir = out
    b.translator = _StubTranslator()
    b.abi_db = {}
    b.title = None
    return b


def test_chunk_prologue():
    funcs = [(0x00011000, {"name": "sub_00011000"})]
    with tempfile.TemporaryDirectory() as d:
        for level in ("full", "off"):
            gen = os.path.join(d, level)
            _batch(level, gen).translate_batch_split(list(funcs), gen,
                                                     chunk_size=1)
            hdr = open(os.path.join(gen, "recomp_funcs.h"), encoding="utf-8").read()
            assert "RECOMP_REG_CACHE" not in hdr, hdr
            chunk = open(os.path.join(gen, "recomp_0000.c"), encoding="utf-8").read()
            lines = chunk.splitlines()
            if level == "off":
                assert "RECOMP_REG_CACHE" not in chunk, chunk
                continue
            inc = lines.index('#include "recomp_funcs.h"')
            assert lines[inc - 1] == "#define RECOMP_REG_CACHE 1", lines
            assert lines[inc - 2] == "#define RECOMP_GENERATED_CODE", lines
            assert lines[inc + 1:inc + 4] == tr.REG_CACHE_GUARD, lines
            single = os.path.join(d, "single.c")
            _batch(level, d).translate_batch(list(funcs), output_file=single)
            text = open(single, encoding="utf-8").read()
            assert (text.index("#define RECOMP_REG_CACHE 1")
                    < text.index('#include "recomp_types.h"')
                    < text.index("#ifndef RECOMP_REG_CACHE_SUPPORTED")), text


# 12. the generated-code diff check ---------------------------------------

def test_diffcheck_fixture():
    with tempfile.TemporaryDirectory() as d:
        off, on = os.path.join(d, "off"), os.path.join(d, "on")
        os.makedirs(off)
        os.makedirs(on)
        prologue = ("#define RECOMP_GENERATED_CODE\n"
                    '#include "recomp_funcs.h"\n#include <math.h>\n\n')
        on_prologue = ("#define RECOMP_GENERATED_CODE\n"
                       "#define RECOMP_REG_CACHE 1\n"
                       '#include "recomp_funcs.h"\n'
                       + "\n".join(tr.REG_CACHE_GUARD) + "\n"
                       "#include <math.h>\n\n")
        bodies_off, bodies_on = [], []
        for off_ in (F_MAIN, F_TAIL, F_RETN, F_SEH, F_SWITCH):
            bodies_off.append(_lift(off_, "off"))
            bodies_on.append(_lift(off_, "full",
                                   force_returns={BASE + F_TAIL: 1})
                             if off_ == F_TAIL else _lift(off_, "full"))
        bodies_off[1] = _lift(F_TAIL, "off", force_returns={BASE + F_TAIL: 1})
        bodies_on.append(_lift(F_MMX, "gpr"))
        bodies_off.append(_lift(F_MMX, "off"))
        with open(os.path.join(off, "recomp_0000.c"), "w", encoding="utf-8") as f:
            f.write(prologue + "\n".join(bodies_off))
        with open(os.path.join(on, "recomp_0000.c"), "w", encoding="utf-8") as f:
            f.write(on_prologue + "\n".join(bodies_on))
        import io
        report = io.StringIO()
        assert regcache_diffcheck.compare(off, on, out=report), report.getvalue()
        text = report.getvalue()
        assert "normalised output identical" in text, text
        assert "functions cached: 5" in text, text
        assert "excluded/partly uncached functions: 1" in text, text
        # A real difference is reported.
        with open(os.path.join(on, "recomp_0000.c"), "a", encoding="utf-8") as f:
            f.write("\n    eax = 1;\n")
        assert not regcache_diffcheck.compare(off, on, out=io.StringIO())


# 11. compile and run --------------------------------------------------------

def test_conformance_corpus_with_the_cache():
    """The conformance corpus (real MSVC functions, with calls into CRT
    helpers) lifted with --reg-cache full and run against the CPU."""
    from tools.conformance import __main__ as conformance
    if conformance._find_vcvars() is None:
        try:
            import pytest
        except ImportError:
            print("skip conformance_corpus_with_the_cache (no 32-bit MSVC)")
            return
        pytest.skip("needs a 32-bit MSVC (vcvars32.bat)")
    rc = conformance.main_with_args(["--only", "corpus", "--reg-cache", "full"],
                                    allow_container=False)
    assert rc == 0, rc


if __name__ == "__main__":
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
            print("ok ", name)
    print("all passed")
