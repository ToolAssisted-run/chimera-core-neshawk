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
checkout inside miniHawk (`extern/tools/chimera-common-minibox`); `build-package.sh` configures and builds it on demand.

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

**Persistent data** - what the machine keeps when it is switched off - is the optional
`GetPersistentSize` / `GetPersistent` / `GetPersistentBuffer` / `PutPersistent` group, plus
`GetPersistentName` and `GetPersistentId`: the frontend has no word for this, so the core supplies
both the label it is shown under ("Cartridge SRAM", "Disk Contents") and the id a bundle files it
under ("sram", "disk"). Battery-backed carts hand over their WRAM; the disk system hands over the
difference between the disk now and the disk as it was inserted, in NesHawk's own `FDSS` format, so
the file is interchangeable with BizHawk's. A cart with no battery reports zero and the entry does
not appear at all. `PutPersistent` returns 0 for data that does not fit the machine (wrong size,
wrong number of disk sides) rather than applying half of it.

**Frame rate and sample count** are answered after Init (`GetVsyncNumerator`,
`GetVsyncDenominator`, `GetAudioSampleCount`) rather than declared in the config, because the region
is a user setting and a band-limited resampler does not produce the same number of samples
every frame.

## Limits worth knowing

- **Mappers**: the boards translated so far are NROM (0), SxROM/MMC1 (1), UxROM (2), CNROM (3),
  TxROM/MMC3 (4), AxROM (7), GxROM (66), mapper 70 and the Famicom Disk System - 69 of the 79 roms
  in the reference collection, plus any disk image. Anything else is rejected at load with a message, not mis-emulated. Adding one is a small, well-bounded job: transliterate the board from BizHawk's
  `Boards/*.cs` into `nesBoards.hpp`, accept its mapper number in the NES constructor, and verify it
  frame by frame against genuine NesHawk with the oracle in [`../harness`](../harness).
- **No cart database.** NesHawk resolves the board from BootGod's database keyed by rom hash, and
  falls back to the iNES header; this translation only has the header. Where a dump's header lies
  (Mega Man's says mapper 66, the database says UNROM) NesHawk is right and this is not, so such a
  rom needs its board corrected by hand - or, eventually, a generated hash-to-board table compiled
  into the core.
- **No Power button**: the controller has Reset (soft) but not Power, because a hard reset rebuilds
  the board and the host has already mapped the board's memory domains by pointer.
- **Movies do not record the save file.** A movie that starts from a machine with a save in it
  replays against whatever save is on disk at the time. NesHawk records it in the movie; here the
  frontend would have to, and does not yet.

## The Famicom Disk System

A disk image is not a cartridge: the machine is the RAM adapter, which maps its own 8 KiB BIOS at
`$E000`, 32 KiB of RAM under it, and a disk drive and an extra sound channel at `$4020-$40FF`. All
three are translated - `FDS.cs` into the board (`Kind::FDS`), `RamAdapter.cs` and `FDSAudio.cs` into
[`nesFds.hpp`](../source/nesFds.hpp).

The drive is modelled at the bit level and clocked by the PPU: the disk is a stream of bits passing
under a fixed head at ~96.4 kHz, which the BIOS reads through a shift register. Circulating `.fds`
dumps are file-system level - gaps, block markers and CRCs stripped - so the physical layout is
rebuilt on insert, exactly as NesHawk does it. Both dump shapes load, headered and raw.

- **The BIOS is firmware the package declares** (`"firmware": [{ "id": "bios", ... }]`) and the
  frontend mounts it under that id, like the rom. It is declared optional, because a cartridge does
  not need it; a disk image without it is refused by the core with a message saying so.
- **Disk buttons**: `FDS Eject` and `FDS Insert 0..3`. NesHawk declares one Insert per side of the
  loaded image; a package's controller is fixed, so four are always declared and the ones the image
  has no side for do nothing. Both are level triggered, as in NesHawk - holding Insert re-seats the
  disk every frame.
- **Verified** the same way as every board: `Ai Senshi Nicol` is byte-identical to genuine NesHawk
  over 5400 frames (90 seconds, which is well past the disk load), and the sandbox gate passes on
  it at 1800 frames including per-frame savestate round-trips.

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
