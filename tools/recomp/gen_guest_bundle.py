"""Lift our own small x86 fixtures; never reads an XBE or game data."""
from pathlib import Path
import argparse
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "build" / "test-python"))
sys.path.insert(0, str(ROOT))
from tools.recomp.disasm import Disassembler
from tools.recomp.lifter import Lifter
from tools.recomp.translator import FP_STACK_MACROS

FIXTURES = {
    "scalar": "b878563412a3011000008b1d01100000",
    "signed": "0fbe05011000000fbf1d03100000",
    "add": "83050110000001",
    "bits": "813501100000ff00ff00",
    "stack": "50ff7424045b59",
    "callee": "53bbefbeadde5b",
    "shift": "c03d0110000001",
    "swap": "0fc8",
    "float": "d90501100000d91d09100000",
    "copy": "f3a5",
    "copyb": "f3a4",
    "copyw": "f366a5",
    "fillb": "f3aa",
    "fillw": "f366ab",
    "sse": "0f10060f1107",
}

def generate():
    lines = ["/* Generated ONLY from our synthetic fixture bytes. */"]
    lines += FP_STACK_MACROS
    for name, raw in FIXTURES.items():
        data = bytes.fromhex(raw)
        insns = Disassembler().disassemble_function(data, 0x10000, 0x10000 + len(data))
        assert sum(i.size for i in insns) == len(data)
        lifter = Lifter(explicit_guest_bus=True)
        body = [s for insn in insns for s in lifter.lift_instruction(insn)]
        assert not any("RECOMP_UNIMPL" in s or "TODO" in s for s in body), body
        lines += [f"static void fixture_{name}(void) {{", "    uint32_t _fa=0, _fb=0; int32_t _fas=0, _fbs=0;",
                  "    (void)_fa; (void)_fb; (void)_fas; (void)_fbs;"]
        lines.extend("    " + s for s in body)
        lines += ["}"]
    return "\n".join(lines) + "\n"

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    content = generate()
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(content, encoding="utf-8")
    print(f"Lifted {len(FIXTURES)} synthetic fixtures")
