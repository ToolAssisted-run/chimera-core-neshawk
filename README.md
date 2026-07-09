# QuickerNesHawk

A **header-only C++ translation of BizHawk's [NesHawk](https://github.com/TASEmulators/BizHawk)
NES core** — the cycle-accurate, TAS-grade Nintendo Entertainment System emulator used by BizHawk.

The C# original is translated 1:1 into portable C++17 so that it can be embedded directly (no .NET
runtime, no marshalling) into native tools such as [JaffarPlus](https://github.com/SergioMartin86/jaffarPlus),
where a *ground-truth* NES core is needed to confirm that search results hold on the reference
emulator rather than on a faster approximation.

## Credits & provenance

All emulation logic in this repository derives from **BizHawk's NesHawk core**, authored by the
**BizHawk team** and contributors and distributed under the MIT License. The 6502 CPU, PPU, APU,
mappers, and system glue are transliterations of the original C# sources; each header names the
exact BizHawk file(s) it came from at the top so the two stay line-auditable. Please support and
credit the upstream project:

- BizHawk: https://github.com/TASEmulators/BizHawk
- NesHawk lives under `src/BizHawk.Emulation.Cores/Consoles/Nintendo/NES/` and
  `src/BizHawk.Emulation.Cores/CPUs/MOS 6502X/`.

This translation is released under the same MIT License (see [`LICENSE`](LICENSE)). It is an
independent derivative and is **not** affiliated with or endorsed by the BizHawk project.

## Layout

| Path        | Contents                                                                           |
|-------------|------------------------------------------------------------------------------------|
| `source/`   | The header-only core. Pure C++17 (only the standard library) — no external deps.   |
| `tools/`    | Small standalone drivers: CPU fuzz generator, ROM replay, state round-trip test.   |
| `harness/`  | The C# **golden oracle**: runs *genuine* BizHawk NesHawk headless to emit reference traces. |
| `test/`     | Self-contained regression test + checked-in golden trace (no ROM, no .NET needed). |

### `source/`

| Header                 | Translated from (BizHawk)                                             |
|------------------------|----------------------------------------------------------------------|
| `mos6502x.hpp`         | `CPUs/MOS 6502X/{MOS6502X.cs, Execute.cs}`                            |
| `nesPpu.hpp`           | `.../NES/PPU.cs`, `PPU.regs.cs`, `PPU.run.cs`                         |
| `nesApu.hpp`           | `.../NES/APU.cs`                                                      |
| `nesBoards.hpp`        | `.../NES/Boards/` (the mappers translated so far)                    |
| `nesControllers.hpp`   | `.../NES/NESControllerDefinitions` / joypad wiring                    |
| `nes.hpp`              | `NES.Core.cs`, `NES.CpuLink.cs`, `NES.cs`, `NES.BoardSystem.cs`       |
| `nesSerialization.hpp` | savestate (save/load) support for the translated state               |
| `nesCommon.hpp`        | shared enums / small helpers                                         |

The core is a template over a "link" type (see `mos6502x.hpp`), so the host wires memory/PPU/APU
access with zero virtual-call overhead.

## Building

Requires a C++17 compiler and [Meson](https://mesonbuild.com/) + Ninja.

```sh
meson setup build
ninja -C build
```

This builds the standalone tools in `tools/`:

- **`cpuFuzz`** — seeds RAM deterministically, runs the 6502 for N cycles, dumps a per-cycle trace.
  Needs no ROM; used by the regression test below.
- **`nesReplay`** / **`stateTest`** — replay a jaffar-format input movie on a ROM, and exercise the
  savestate save/load round-trip. (These need a NES ROM, which is **not** included.)

Header-only consumers can skip Meson entirely and just `#include "source/nes.hpp"`.

## Testing

```sh
meson test -C build
```

The `cpufuzz-regression` test compiles and runs `cpuFuzz` and diffs its output against the
checked-in golden trace (`test/cpuFuzz.golden`). Because the C++ CPU is deterministic, this is a
fast, ROM-free, .NET-free guard against behavioral regressions in the 6502 core.

### Full byte-exact validation against BizHawk (`harness/`)

For end-to-end fidelity the `harness/` project hosts the *real* BizHawk NesHawk headless and dumps
the full 2 KB system RAM after every frame (identical layout to `jaffar-player --dumpRam`). Running
the same ROM + input movie through both cores and diffing the per-frame RAM proves the translation
byte-exactly. This requires a local BizHawk build and .NET, so it is run manually rather than in CI;
`harness/Harness.csproj` documents the expected BizHawk reference paths.

## Status

The CPU is fuzz-validated identical to BizHawk over millions of cycles. The full system (PPU/APU/
mappers) is validated frame-by-frame on real games; mappers are translated on demand, so a ROM using
a not-yet-translated board needs its board added to `nesBoards.hpp` first.
