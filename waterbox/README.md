# QuickerNesHawk as a miniHawk waterbox core

A miniHawk core package is exactly two files — `core.wbx` (the sandboxed guest binary) and
`waterbox.config` — loaded by miniHawk's one built-in generic adapter. There is no managed
assembly and no native library in a package.

The emulation source under [`../source`](../source) is untouched by the sandbox: the same headers
build for the host and for the guest, and the gate below requires the two to agree byte for byte.

| file | what it is |
|---|---|
| `waterbox.cpp` | the guest ABI layer: input, video, audio, memory domains, and the core-managed tooling |
| `waterbox.config` | the static machine surface (geometry, controller, heap layout) and the user settings |
| `default_keybinds.json` | default bindings for the controller this package declares, from BizHawk's own |
| `nesPalettes.hpp` | NesHawk's palettes, generated from BizHawk's `Palettes.cs` |
| `nes6502Disasm.hpp` | NesHawk's 6502 disassembler, generated from BizHawk's `Disassembler.cs` |
| `audioResampler.hpp` / `audioKernel.hpp` | band-limited step resampler and its kernel |
| `tools/gen-*.py` | the generators for the three headers above |
| `build-core.sh` | builds `core.wbx` plus the three drivers |
| `build-package.sh` | builds the package and installs it into a miniHawk checkout |
| `run-native.cpp` / `run-wbx.c` | the equivalence gate: the same rom and inputs through the host build and the sandbox |
| `run-tooling.c` | exercises the optional tooling exports the way the frontend probes them |
| `tests/` | the frontend gate: the package running inside miniHawk |

## Building

```sh
./build-package.sh -r <miniHawk checkout>   # -> <miniHawk>/build/Cores/quickerneshawk.zip
```

The C++ guest toolchain (musl plus a libstdc++ built for the sandbox) comes from the miniBox
checkout inside miniHawk (`extern/miniBox`); `build-package.sh` configures and builds it on demand.

Two build flags matter:

- `NESHAWK_FULL_AV` — the core defaults to headless, which compiles out audio synthesis and the
  pixel pipeline entirely (neither feeds back into emulation, which is why the botting builds drop
  them). A frontend needs both.
- exceptions are **on**, unlike most waterbox guests: the core reports a rom it cannot run — an
  unsupported mapper, a truncated file — by throwing, and `Init` catches it so the frontend can say
  so instead of emulating nonsense.

## Gates

```sh
./run-gate.sh                       # core level: equivalence, savestates, tooling (free roms)
./run-gate.sh -f 600 <rom...>       # ...over your own roms
./tests/run-frontend.sh             # frontend level: the package running inside miniHawk
```

`run-gate.sh` runs the same rom, frame count and per-frame button pattern through the host build and
through the sandbox and requires identical video, audio, lag and memory-domain digests; then it
round-trips the whole machine through save/load state around **every** frame and requires the
digests to come out unchanged; then it checks that every tooling group the core claims actually
answers. What it proves is that the sandbox changed no emulation — fidelity to the C# NesHawk is a
different question, answered by [`../harness`](../harness) and [`../test`](../test).

`tests/run-frontend.sh` loads the package in EmuHawk (Mono, on a private Xvfb display), emulates
with nothing pressed, and requires the resulting work RAM to match the native reference exactly;
then it changes sync settings the way the settings dialog does and requires the machine to change,
and starts once from a config that has never seen this controller to check that the bindings the
package ships become the frontend's defaults.
It pins `PreferredCores` and the script asserts `emu.getcorename()`, because a miniHawk install can
easily hold two packages claiming the NES.

## What the frontend gets

**Settings** are NesHawk's, split as BizHawk splits them. Sync settings shape the machine, so
changing one reboots the core and movies record them; the rest are presentation and take effect
immediately.

| sync | |
|---|---|
| `region` | `ntsc` (default), `pal`, `dendy`. BizHawk's "Default" resolves through its cart database, which this core has no equivalent of. |
| `port1` / `port2` | `none` / `gamepad`. NesHawk also offers zappers, paddles and the four-score; only the standard pad is translated. |
| `initialWRamStatePattern` | power-on RAM fill, as hex. Empty means fceux's pattern. |
| `allowMoreThanEightSprites` | see below |

| non-sync | |
|---|---|
| `palette` | `quickNES` (NesHawk's default), `fceux`, `2c03_2c05`, `2c04_001`…`004` |
| `dispBackground`, `dispSprites` | draw each layer |
| `clipLeftAndRight` | blank the 8 leftmost and rightmost columns |
| `backgroundColor` | colour for pixels the PPU never drew, or -1 |
| `ntscTopLine` / `ntscBottomLine` / `palTopLine` / `palBottomLine` | which scanlines are shown |
| `apuVolume` | 1..10, NesHawk's own volume slider |

Two deliberate differences from BizHawk:

- `allowMoreThanEightSprites` is a **sync** setting here. BizHawk files it under presentation, but it
  changes what sprite evaluation puts in secondary OAM, which can change a sprite-zero hit — so in a
  frontend that treats movies as a reproduction contract it belongs on the machine-shaping side.
- The crop settings **blank** rows and columns instead of narrowing the picture: a package declares
  one frame size for the core's lifetime.

**Tools** are the ones BizHawk builds for NesHawk, published through miniHawk's system-neutral
groups, so the frontend never learns what a nametable is:

- surfaces — Nametables, Pattern Tables, Sprites (OAM), Palettes; rendered by the core into an
  invisible scratch buffer, so opening a viewer costs nothing per savestate
- registers — `A X Y S PC P` plus the eight flags, exactly `GetCpuFlagsAndRegisters`, and the
  executed-cycle count. No `SetRegisterValue`: NesHawk's `SetCpuRegister` throws, so the debugger
  shows the box read-only here too
- buses — System Bus (with NesHawk's own `ApplySystemBusPoke` rules) and PPU Bus
- trace — lines formatted character for character like NesHawk's, disassembly included, appended to
  a guest ring buffer and drained once per frame rather than crossing the sandbox per instruction

**Key bindings** come from `default_keybinds.json` in the package, transcribed from BizHawk's
`Assets/defctrl.json`. miniHawk ships no bindings of its own - a package that declares a controller
says how it is played by default - so without that file the core would arrive unplayable until the
user bound every key. Player 1 only and Reset unbound, which is BizHawk's own choice.

**Frame rate and sample count** are answered after Init (`GetVsyncNumerator`,
`GetVsyncDenominator`, `GetAudioSampleCount`) rather than declared in the config, because the region
is a user setting and a band-limited resampler does not produce the same number of samples
every frame.

## Limits worth knowing

- **Mappers**: the boards translated so far are SxROM/MMC1 (1), NROM (0), UxROM (2), CNROM (3),
  AxROM (7), GxROM (66) and mapper 70. Anything else is rejected at load with a message, not
  mis-emulated. Adding one is a small, well-bounded job: transliterate the board from BizHawk's
  `Boards/*.cs` into `nesBoards.hpp`, accept its mapper number in the NES constructor, and verify it
  frame by frame against genuine NesHawk with the oracle in [`../harness`](../harness).
- **No cart database.** NesHawk resolves the board from BootGod's database keyed by rom hash, and
  falls back to the iNES header; this translation only has the header. Where a dump's header lies
  (Mega Man's says mapper 66, the database says UNROM) NesHawk is right and this is not, so such a
  rom needs its board corrected by hand - or, eventually, a generated hash-to-board table compiled
  into the core.
- **No Power button**: the controller has Reset (soft) but not Power, because a hard reset rebuilds
  the board and the host has already mapped the board's memory domains by pointer.

## Audio

The APU hands out deltas ("the mix changed by this much, at this CPU cycle"), which is not something
you can point-sample at 44.1kHz without folding the edges back as audible tones. `audioResampler.hpp`
gives every delta a band-limited impulse in the sub-sample phase it actually fell at, sums them and
integrates once - band-limited step synthesis, the standard treatment, implemented here under this
repository's MIT licence with a generated kernel and no allocation. Fixed point throughout, because
the equivalence gate compares audio between a glibc host build and a musl guest build.

Measured against blip_buf, which is what BizHawk itself uses, on the same stream of deltas:

| | this | blip_buf |
|---|---|---|
| worst aliasing image, 1234.5Hz square | -57.7 dB | -57.8 dB (both at the 16-bit floor) |
| noise floor between harmonics | -115.2 dB | -118.2 dB |
| magnitude, real game audio, DC-12kHz | within 0.3 dB | reference |
| magnitude, 12-18kHz | within 0.5 dB | reference |

The kernel carries a first-order treble rolloff for exactly that reason: NesHawk sounds the way it
sounds partly because of the reference resampler's treble, and a core called NesHawk should not
suddenly be brighter. Shaping the kernel rather than filtering afterwards keeps the phase linear.
