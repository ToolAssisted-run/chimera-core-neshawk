#!/usr/bin/env python3
"""Transliterates BizHawk's MOS 6502X C# sources into a C++ header.

Reads Execute.cs + MOS6502X.cs (BizHawk, MIT license) and emits mos6502x.hpp: a
template<typename TLink> class preserving the original 1:1 (same microcode table, same stage
methods, same dispatch switch) so every line stays auditable against the C# original.

The transform set is deliberately small and mechanical; anything it cannot handle it passes
through, leaving the C++ compiler to flag residuals for manual review.
"""
import re, sys, os

SRC = os.path.expanduser("~/BizHawk/src/BizHawk.Emulation.Cores/CPUs/MOS 6502X")
OUT = os.path.join(os.path.dirname(__file__), "..", "source", "mos6502x.hpp")

execute = open(os.path.join(SRC, "Execute.cs")).read()
main = open(os.path.join(SRC, "MOS6502X.cs")).read()

# ---------------------------------------------------------------- helpers
def type_map(s):
    s = re.sub(r"\bbyte\b", "uint8_t", s)
    s = re.sub(r"\bsbyte\b", "int8_t", s)
    s = re.sub(r"\bushort\b", "uint16_t", s)
    s = re.sub(r"\bulong\b", "uint64_t", s)
    s = re.sub(r"\blong\b", "int64_t", s)
    return s

def common_expr(s):
    # C# extension X.Bit(n) -> bit test
    s = re.sub(r"([A-Za-z_][A-Za-z0-9_]*)\.Bit\((\d+)\)", r"(((\1) >> \2 & 1) != 0)", s)
    # unchecked(...) is C++ default behavior for unsigned; drop the wrapper textually
    s = s.replace("unchecked(", "(")
    # flag properties: assignments first, then reads
    for f in ("FlagC", "FlagZ", "FlagI", "FlagD", "FlagB", "FlagT", "FlagV", "FlagN"):
        s = re.sub(rf"\b{f}\s*=\s*(.+?);", rf"Set{f}(\1);", s)
        s = re.sub(rf"\b{f}\b(?!\()", rf"Get{f}()", s)
    # Uop.X -> Uop::X
    s = re.sub(r"\bUop\.([A-Za-z0-9_]+)", r"Uop::\1", s)
    # 'is' pattern for enum equality
    s = re.sub(r"\bis\s+Uop::([A-Za-z0-9_]+)", r"== Uop::\1", s)
    # C# var -> auto
    s = re.sub(r"\bvar\b", "auto", s)
    # bare property use of Interrupted -> method call
    s = re.sub(r"\bInterrupted\b(?!\()", "Interrupted()", s)
    # exceptions -> abort (unreachable paths)
    s = re.sub(r"throw new InvalidOperationException\([^;]*\);", "__builtin_trap();", s)
    return type_map(s)

# ---------------------------------------------------------------- Uop enum
m = re.search(r"private enum Uop\s*\{(.*?)\n\t\t\}", execute, re.S)
uop_body = m.group(1)
uop_body = re.sub(r"//[^\n]*", lambda mm: mm.group(0), uop_body)  # keep comments
uop_enum = "  enum class Uop : uint16_t\n  {\n" + uop_body.replace("\t\t\t", "    ") + "\n  };\n"

# ---------------------------------------------------------------- microcode table
m = re.search(r"private static readonly Uop\[\]\[\] Microcode =\s*\{(.*?)\n\t\t\};", execute, re.S)
rows_src = m.group(1)
row_re = re.compile(r"(/\*.*?\*/)?\s*new Uop\[\]\s*\{(.*?)\}", re.S)
rows = row_re.findall(rows_src)
table_lines = []
row_decls = []
for i, (comment, body) in enumerate(rows):
    body = re.sub(r"\bUop\.", "Uop::", body).strip().rstrip(',')
    c = (comment or "").replace("\n", " ")
    row_decls.append(f"  static constexpr Uop MC_{i}[] = {{ {body} }}; {c}")
    table_lines.append(f"    MC_{i},")
microcode = "\n".join(row_decls) + "\n\n  static constexpr const Uop* Microcode[] =\n  {\n" + "\n".join(table_lines) + "\n  };\n"
n_rows = len(rows)

# ---------------------------------------------------------------- stage methods + dispatch
# take everything from InitOpcodeHandlers to the end of the class, minus tracing/disasm helpers
body_start = execute.index("private void InitOpcodeHandlers()")
body = execute[body_start:]
body = body[: body.rindex("public bool AtInstructionStart()")]

out_lines = []
skip_depth = 0
skip_until_close = False
for raw in body.splitlines():
    line = raw
    # strip attributes and C#-only noise
    if re.match(r"\s*\[MethodImpl", line):
        continue
    if re.search(r"\bState\(", line) or "Console.WriteLine" in line:
        line = re.sub(r"^(\s*)", r"\1// [not translated] ", line, count=1)
        out_lines.append(common_expr(line)); continue
    if "TraceCallback" in line or "Tracer" in line:
        # tracing hooks are not translated in v1
        line = re.sub(r"^(\s*)", r"\1// [not translated] ", line, count=1)
    line = line.replace("private void ", "void ")
    line = line.replace("public void ", "void ")
    line = line.replace("private const int", "static constexpr int")
    line = line.replace("public int", "int32_t")
    line = line.replace("private int", "int32_t")
    line = line.replace("public bool", "bool")
    line = line.replace("private bool", "bool")
    line = line.replace("public ushort", "uint16_t")
    line = line.replace("private ushort", "uint16_t")
    line = line.replace("private byte", "uint8_t")
    line = line.replace("public byte", "uint8_t")
    line = line.replace("_link.", "_link.")
    line = common_expr(line)
    # expression-bodied members: 'T Name() => expr;' -> '{ return expr; }'
    line = re.sub(r"(\)\s*)=>\s*(.*);", r"\1{ return \2; }", line)
    # C# 'int' local decls fine; casts
    line = line.replace("(uint8_t) ", "(uint8_t)")
    out_lines.append(line)
methods = "\n".join(out_lines)
# de-indent by two tabs (class+namespace in C#) to two spaces
methods = re.sub(r"^\t\t", "  ", methods, flags=re.M)
methods = methods.replace("\t", "  ")

# ---------------------------------------------------------------- state & flags from MOS6502X.cs
state = f"""
  // ---- constants (from MOS6502X.cs) ----
  static constexpr uint16_t NMIVector   = 0xFFFA;
  static constexpr uint16_t ResetVector = 0xFFFC;
  static constexpr uint16_t BRKVector   = 0xFFFE;
  static constexpr uint16_t IRQVector   = 0xFFFE;

  // N/Z flag pair per value (bit1 = Z for 0, bit7 = N); from MOS6502X.cs TableNZ
  static constexpr uint8_t TableNZ[256] = {{
    0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
    0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
    0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
    0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
    0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
    0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
    0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
    0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80
  }};

  bool debug = false; // debug-print hook in C#; unused here

  // ---- registers & pins (from MOS6502X.cs) ----
  uint8_t A = 0, X = 0, Y = 0, P = 0x20, S = 0;
  uint16_t PC = 0;
  bool IRQ = false, NMI = false, RDY = true;
  int32_t ext_ppu_cycle = 0;
  uint8_t AneConstant = 0xFF; // ANE/XAA magic constant (hardware-variable; BizHawk default)
  uint8_t LxaConstant = 0xFF; // LXA magic constant
  int64_t TotalExecutedCycles = 0;
  bool BCD_Enabled = true;    // NES core sets false

  // flag accessors (P bit fields), mirroring the C# properties
  bool GetFlagC() const {{ return (P & 0x01) != 0; }}  void SetFlagC(bool v) {{ P = v ? (P | 0x01) : (P & ~0x01); }}
  bool GetFlagZ() const {{ return (P & 0x02) != 0; }}  void SetFlagZ(bool v) {{ P = v ? (P | 0x02) : (P & ~0x02); }}
  bool GetFlagI() const {{ return (P & 0x04) != 0; }}  void SetFlagI(bool v) {{ P = v ? (P | 0x04) : (P & ~0x04); }}
  bool GetFlagD() const {{ return (P & 0x08) != 0; }}  void SetFlagD(bool v) {{ P = v ? (P | 0x08) : (P & ~0x08); }}
  bool GetFlagB() const {{ return (P & 0x10) != 0; }}  void SetFlagB(bool v) {{ P = v ? (P | 0x10) : (P & ~0x10); }}
  bool GetFlagT() const {{ return (P & 0x20) != 0; }}  void SetFlagT(bool v) {{ P = v ? (P | 0x20) : (P & ~0x20); }}
  bool GetFlagV() const {{ return (P & 0x40) != 0; }}  void SetFlagV(bool v) {{ P = v ? (P | 0x40) : (P & ~0x40); }}
  bool GetFlagN() const {{ return (P & 0x80) != 0; }}  void SetFlagN(bool v) {{ P = v ? (P | 0x80) : (P & ~0x80); }}

  void Reset()
  {{
    A = 0; X = 0; Y = 0; P = 0x20; S = 0; PC = 0;
    TotalExecutedCycles = 0;
    mi = 0;
    opcode = VOP_RESET;
    iflag_pending = true;
    RDY = true;
  }}

  void NESSoftReset()
  {{
    opcode = VOP_RESET;
    mi = 0;
    iflag_pending = true;
    SetFlagI(true);
  }}
"""

header = f"""#pragma once

// MOS 6502X: cycle-accurate 6502 as implemented by BizHawk's NesHawk core.
//
// TRANSLATED 1:1 from BizHawk (MIT license, original C) 2009-2025 BizHawk team):
//   src/BizHawk.Emulation.Cores/CPUs/MOS 6502X/{{Execute.cs, MOS6502X.cs}}
// by extern/nesHawk/tools/transliterate_cpu.py -- regenerate with that script; hand-edits go in
// the FIXUPS section of the script, not here. Structure (microcode table, stage methods,
// dispatch switch) intentionally mirrors the original so the two stay line-auditable.
//
// TLink must provide: uint8_t ReadMemory(uint16_t), uint8_t DummyReadMemory(uint16_t),
//                     uint8_t PeekMemory(uint16_t), void WriteMemory(uint16_t, uint8_t),
//                     void OnExecFetch(uint16_t)

#include <cstdint>

namespace nesHawk
{{

template <typename TLink>
class MOS6502X
{{
public:
  TLink& _link;

  explicit MOS6502X(TLink& link) : _link(link) {{ Reset(); }}

{uop_enum}
{microcode}
  static_assert(sizeof(Microcode) / sizeof(Microcode[0]) == {n_rows}, "microcode row count");
{state}
{methods}

  bool AtInstructionStart() const {{ return Microcode[opcode][mi] >= Uop::End; }}
}};

}} // namespace nesHawk
"""
os.makedirs(os.path.dirname(OUT), exist_ok=True)
open(OUT, "w").write(header)
print(f"wrote {OUT}: {len(header.splitlines())} lines ({n_rows} microcode rows)")
