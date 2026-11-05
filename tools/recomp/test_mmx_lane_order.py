"""MMX/SSE lane order must not depend on the host's byte order.

The register unions in recomp_types.h overlay narrow lane arrays on one
64-bit value. On a little-endian host ub[i] is x86 byte lane i; on a
big-endian one (the Xbox 360) ub[0] is the most significant byte, x86 lane 7.
Every cross-lane MMX helper (punpck*, pack*, pshufw, psadbw, ...) and every
movd out of an mm register therefore moved the wrong lanes there, and the XMV
decoder's MMX IDCT painted the intro movies as noise.

The fix keeps `q` as the register value and reaches narrow lanes only through
RECOMP_MMX_<view>(reg, lane), which mirrors the index on a big-endian host.
These tests pin the two halves of that contract: the lifter never emits a raw
narrow view, and the header's MMX helpers never index one directly. The
behaviour itself is checked lane by lane, on the CPU and on the console, by
tests/simd_lanes.
"""

import pathlib
import re
import unittest

from .disasm import Instruction, Operand
from .lifter import Lifter

_ROOT = pathlib.Path(__file__).resolve().parents[2]
_RUNTIME = _ROOT / "templates" / "runtime" / "recomp_types.h"


def _insn(mnemonic, op_str, operands):
    instruction = Instruction(0, 4, mnemonic, op_str, "")
    instruction.operands = operands
    return instruction


def _reg(name):
    return Operand(type="reg", reg=name)


def _mem(base=None, disp=0, size=4):
    return Operand(type="mem", mem_base=base, mem_disp=disp, mem_size=size)


_RAW_VIEW = re.compile(r"\.(b|ub|w|uw|d|ud)\[")


class LifterEmitsLaneMacrosTest(unittest.TestCase):
    def _lift(self, mnemonic, op_str, operands):
        return "\n".join(Lifter().lift_instruction(_insn(mnemonic, op_str, operands)))

    def test_movd_to_gpr_uses_the_lane_macro(self):
        out = self._lift("movd", "eax, mm0", [_reg("eax"), _reg("mm0")])
        self.assertIn("RECOMP_MMX_UD(mm0, 0)", out)
        self.assertIsNone(_RAW_VIEW.search(out), out)

    def test_movd_to_memory_uses_the_lane_macro(self):
        out = self._lift("movd", "dword ptr [esp], mm1",
                         [_mem("esp"), _reg("mm1")])
        self.assertIn("RECOMP_MMX_UD(mm1, 0)", out)
        self.assertIsNone(_RAW_VIEW.search(out), out)

    def test_movd_into_mm_zero_extends_through_from32(self):
        out = self._lift("movd", "mm2, ecx", [_reg("mm2"), _reg("ecx")])
        self.assertIn("MMX_FROM32(ecx)", out)


class HeaderMmxHelpersUseLaneMacrosTest(unittest.TestCase):
    def setUp(self):
        src = _RUNTIME.read_text(encoding="utf-8")
        start = src.index("typedef union RecompMmx")
        end = src.index("#define xmm0 g_xmm0")
        self.mmx = src[start:end]
        self.src = src

    def test_no_helper_indexes_a_narrow_view_directly(self):
        # The accessor definitions themselves are the only raw indexing.
        code = re.sub(r"/\*.*?\*/", "", self.mmx, flags=re.S)
        body = "\n".join(l for l in code.splitlines()
                         if not l.startswith("#define RECOMP_MMX_"))
        # The union declaration names the views without an index expression.
        hits = [l for l in body.splitlines()
                if _RAW_VIEW.search(l) and not re.match(r"\s+u?int\d+_t\s+\w+\[\d\];", l)]
        self.assertEqual(hits, [])

    def test_big_endian_mirrors_the_lane_index(self):
        self.assertRegex(self.src,
                         r"#define RECOMP_MMX_IX\(lanes, i\) \(\(lanes\) - 1 - \(i\)\)")
        self.assertIn("#define RECOMP_MMX_UD(r, i) ((r).ud[RECOMP_MMX_IX(2, (i))])",
                      self.src)

    def test_xmm_double_lanes_go_through_accessors(self):
        start = self.src.index("static inline RecompXmm XMM_ZERO")
        end = self.src.index("#define RECOMP_XMM_LANEWISE")
        self.assertNotIn(".d[", self.src[start:end])
        self.assertIn("XMM_SET_D(&r, 0, v)", self.src[start:end])


if __name__ == "__main__":
    unittest.main()
