# Building the QuickerNesHawk core

This repository builds QuickerNesHawk, a C++ translation of BizHawk's NesHawk
(Nintendo Entertainment System and Famicom Disk System), as a core for
Chimera. The result is one file, `quickerneshawk.chimeraCore`, which Chimera
loads. The steps below are the ones this repository's CI runs on a fresh
clone (`.github/workflows/minihawk.yml`); when this document and the workflow
disagree, the workflow is right.

Placeholders used below:

- `<repo>` - the checkout of this repository. Its GitHub repository is
  `ToolAssisted-run/chimera-core-neshawk`; the core's id is `quickerneshawk`.
  Commands run from `<repo>` unless a block says otherwise.
- `<chimera>` - a checkout of https://github.com/ToolAssisted-run/chimera.
- `<miniBox>` - `<chimera>/extern/chimera-common-minibox`, a git submodule
  of Chimera. It is the sandbox host and the guest toolchain.

The scripts and the workflow say "miniHawk" and "EmuHawk". They mean the
Chimera checkout and `<chimera>/build/Chimera.exe`: the workflow's "Check out
miniHawk" step checks out `ToolAssisted-run/chimera`, and
`-r <miniHawk root>` takes `<chimera>`.

## Requirements

Cores are built on Linux, x86-64. CI uses GitHub's `ubuntu-latest` runner.

For the package and the core gate (workflow job `core-gate`):

```sh
sudo apt-get update
sudo apt-get install -y --no-install-recommends meson ninja-build build-essential python3
```

For the frontend gate, which builds all of Chimera (workflow job
`frontend-gate`):

```sh
sudo apt-get update
sudo apt-get install -y --no-install-recommends \
  meson ninja-build build-essential cmake pkg-config python3 \
  mono-complete xvfb \
  libgl1-mesa-dev libx11-dev libxext-dev libasound2-dev
```

The frontend gate also needs the .NET SDK 8.0. The workflow installs it with
`actions/setup-dotnet@v4` and `dotnet-version: '8.0'`. By hand, Chimera's
README gives the command and says why the distribution's SDK is not enough:

```sh
curl -sSL https://dot.net/v1/dotnet-install.sh | bash -s -- --channel 8.0
```

No compiler version is pinned. The build uses the gcc and g++ that
`build-essential` installs, and the package records that gcc version in its
`build.json`. The emulator is C++17 and uses only the standard library.

What the build fetches or builds by itself:

- The first miniBox build with `-Dguest_cpp=true` downloads the GCC source
  release that matches the host gcc (about 84 MB, with `curl`), and builds
  libstdc++ for the guest from it with `tar` and `make`. It needs network
  access once. This is in `<miniBox>/meson.build`.
- `waterbox/build-package.sh` configures and builds that miniBox tree, and
  configures the guest build, when they are not there yet.

## Get the sources

This repository has no submodules. The workflow uses `actions/checkout@v6`
with no submodule option:

```sh
git clone https://github.com/ToolAssisted-run/chimera-core-neshawk.git
```

A Chimera checkout. CI builds against Chimera's `main` branch
(`MINIHAWK_REF: main`). The package and the core gate need only the miniBox
submodule:

```sh
git clone https://github.com/ToolAssisted-run/chimera.git <chimera>
cd <chimera>
git submodule update --init extern/chimera-common-minibox
```

The frontend gate builds Chimera itself, and for that the workflow checks
Chimera out with every submodule:

```sh
cd <chimera>
git submodule update --init --recursive
```

Where each script looks for Chimera when it is not told:

| script | option | fallback |
|---|---|---|
| `waterbox/build-package.sh` | `-r <chimera>`, `-m <miniBox>` (or `MINIBOX_DIR`) | `../chimera` beside `<repo>`, `$HOME/chimera`, `../miniHawk` beside `<repo>`, `$HOME/miniHawk`; miniBox defaults to `<chimera>/extern/chimera-common-minibox` |
| `waterbox/setup-guest.sh` | `-m <miniBox>` (or `MINIBOX_DIR`) | `$HOME/chimera/extern/chimera-common-minibox` |
| `meson.build`, native build | `-Dminibox_dir=<miniBox>` | `../chimera/extern/chimera-common-minibox` beside `<repo>`; with neither, the sandbox drivers are not built |
| `waterbox/tests/run-frontend.sh` | `--minihawk-root <chimera>` | a directory named `miniHawk` beside `<repo>`, then `$HOME/miniHawk` |

The fallbacks differ from script to script. Pass the paths.

## Build miniBox

The host library and the C++ guest toolchain (musl plus libstdc++ built for
the sandbox). This core is C++, so it needs the `meson-cpp` flavor:

```sh
mb=<chimera>/extern/chimera-common-minibox
[ -f "$mb/build/meson-cpp/build.ninja" ] || meson setup "$mb/build/meson-cpp" "$mb" -Dguest_cpp=true
meson compile -C "$mb/build/meson-cpp"
```

The workflow caches `<miniBox>/build/meson-cpp` with `actions/cache@v4`,
because the guest toolchain is the slow part of the build and changes only
when miniBox does. By hand, keep the directory: the test in the first line
skips the configure when it is already there.

## Build the core

There are no patches. This repository has no `extern/`, no `patches/` and no
apply-patches step: the emulator is this repository's own header-only source
under `source/`, and `waterbox/waterbox.cpp` is the guest ABI layer over it.

One `meson.build` describes both builds. A cross configure is the guest; a
native configure is the reference, the gate's drivers and the repository's
standalone tools. Both compile the same headers with the same flags
(`core_args`): `-DNESHAWK_FULL_AV`, which turns on audio synthesis and the
pixel pipeline, `-fexceptions` and `-fno-rtti`.

The native reference and the drivers (needed by the gates, not by the
package):

```sh
meson setup build/meson-native "-Dminibox_dir=$mb"
ninja -C build/meson-native
```

This builds `run-native`, `run-wbx` and `run-tooling` in `build/meson-native`,
and the standalone tools `cpuFuzz`, `nesReplay`, `stateTest` and
`resyncReplay`. `run-native` is the same core built for the host; it is what
the gates compare the sandboxed core against. `run-wbx` and `run-tooling` run
the guest through miniBox's host library, so they are built only when meson
has a miniBox checkout. The frontend gate needs `run-native` alone, and the
workflow's `frontend-gate` job builds just that target:

```sh
ninja -C build/meson-native run-native
```

The guest:

```sh
sh waterbox/setup-guest.sh -m "$mb"
ninja -C build/meson-guest core.wbx
```

`setup-guest.sh` writes the cross file `build/guest-cross.ini` and configures
`build/meson-guest`. The cross file holds absolute paths of this machine; it
is under `build/`, which git ignores. The result is
`build/meson-guest/core.wbx`.

## Build the package

```sh
./waterbox/build-package.sh -r <chimera>
```

Options:

- `-r <chimera>` - the Chimera checkout the package is written into.
- `-m <miniBox>` - the miniBox checkout. Default:
  `<chimera>/extern/chimera-common-minibox`. `MINIBOX_DIR` does the same.
- `-o <dir>` - where the staging directory `package-staging` is made.
  Default: `<repo>/build`. It does not move the package.

What the script does, in order:

1. Configures `<miniBox>/build/meson-cpp` with `-Dguest_cpp=true` if it is not
   configured, and builds it.
2. Runs `waterbox/setup-guest.sh` if `build/meson-guest` is not configured,
   builds `core.wbx`, and checks it with miniBox's
   `source/guest/check-wbx.sh`.
3. Stages `core.wbx`, `waterbox.config`, `default_keybinds.json`,
   `file_slots.json` and the licence files that
   `waterbox/package-licenses.json` declares.
4. Stamps the version into the staged `waterbox.config` and writes
   `build.json` (source commit, toolchain, miniBox commit).
5. Writes `<chimera>/build/Cores/quickerneshawk.chimeraCore`, packs it a
   second time and stops if the two files differ.
6. Removes `<chimera>/build/CoreCache/quickerneshawk-*`, so Chimera does not
   load an older extraction.

It does not build the native reference; the package does not need it.

The version of a package is the commit it was built from. CI passes
`CORE_VERSION` (the full commit) and publishes that package. A package built
by hand, without `CORE_VERSION`, is stamped `<12-character commit>+local`,
or `<commit>-dirty+local` when `git diff --quiet HEAD` finds changes in the
tree. `versionDate` is the date of the commit in UTC, never the date of the
build. Hand-built packages are for testing: Chimera's publishing script
refuses a version that carries `+local` or `-dirty`.

## Install it into Chimera

Chimera ships no cores and downloads nothing: it has no network code. A core
gets into Chimera because somebody puts the file there.

- In a Chimera source checkout the cores folder is `<chimera>/build/Cores/`.
  `build-package.sh -r <chimera>` writes the package there, so there is
  nothing else to do.
- In a release bundle, copy `quickerneshawk.chimeraCore` into the `Cores`
  folder beside `Chimera.exe` (or into the folder chosen in File > Core
  Manager > Change folder...).
- File > Core Manager lists what is in that folder. Refresh List rescans it.

The same package file works on Linux and on Windows. The guest inside it is
run by Chimera's sandbox (miniBox) on either.

Packages built by CI are on this repository's Releases page,
https://github.com/ToolAssisted-run/chimera-core-neshawk/releases : a rolling
`dev` release and dated `nightly-YYYY-MM-DD` releases. The asset is named
`quickerneshawk-<version>.chimeraCore`. Download it and put it in the `Cores`
folder.

To use the core: File > New Project... and pick it. To play a rom with no
project, start Chimera with `--core=<package> <rom>`.

## Run the gates

There are two gates. CI needs both green before it publishes.

### The core gate

```sh
./waterbox/run-gate.sh
```

It needs `run-native` and `run-wbx` in `build/meson-native`, and
`build/meson-guest/core.wbx`, and nothing else: no .NET, no Mono, no X.

```
./waterbox/run-gate.sh [-n <native build dir>] [-g <guest build dir>] [-f <frames>] [rom...]
```

The default is 600 frames over `waterbox/roms/sprilo.nes`, a rom that is free
to distribute. Other roms can be given by path; a path that is not there
reports SKIP.

| leg | what it proves |
|---|---|
| `<rom>:equivalence` | video, audio, lag and memory-domain digests are identical between the native build and `core.wbx`, under the same per-frame button pattern |
| `<rom>:savestate` | saving and loading the whole machine around every frame (`run-wbx --rerecord`) changes no digest |
| `<rom>:turbo` | with drawing off for the first half of the run (`run-wbx --turbo`), the machine, the sound, the lag count and the pictures of the second half are unchanged - and the whole-run video hash differs, so the frames really went undrawn |
| `<rom>:saveram` | reports SKIP: `run-wbx` has no save-file channel for it, and the leg says so |
| `<rom>:tooling` | every tooling family (surfaces, registers, buses, trace) answers and lists something |
| `savedata:export`, `savedata:seeded`, `savedata:refused` | save data leaves through the save-data channel, a supplied save reaches the machine and comes back, and a machine that keeps no saves refuses save data |

The two builds share every line of emulation source. They differ in
toolchain and address space, so a difference here is a sandbox or code
generation problem. Whether the translation matches the C# NesHawk is a
separate question; `harness/` and `test/` answer it.

To run the gate over a Famicom disk image, set `NESHAWK_FDS_BIOS` to the
Disk System BIOS dump. The three drivers mount it as the `bios` firmware.

The last line reads `N ok, N failed`. A SKIP counts as neither. Only a FAIL
makes the exit code non-zero.

### The frontend gate

It runs the package inside Chimera and compares the machine with the native
reference. It needs a built Chimera, the installed package and `run-native`.
Build Chimera as the workflow does:

```sh
cd <chimera>
meson setup build/meson-linux --prefix "$PWD/build" --libdir dll
meson compile -C build/meson-linux
meson install -C build/meson-linux
dotnet build source/gui/Chimera.sln -c Release /nodeReuse:false -p:UseSharedCompilation=false
```

Then, from `<repo>`:

```sh
./waterbox/build-package.sh -r <chimera>
./waterbox/tests/run-frontend.sh --minihawk-root <chimera>
```

```
./waterbox/tests/run-frontend.sh [--minihawk-root <chimera>] [--frames N] [rom...]
```

It starts `<chimera>/build/Chimera.exe` under Mono with `--headless` and the
package. With `DISPLAY` unset it starts its own Xvfb; with `DISPLAY` set it
uses that display. The default is 600 frames over `waterbox/roms/sprilo.nes`.

| leg | what it proves |
|---|---|
| `<rom>:frontend` | after the frames, with nothing pressed, the work RAM is identical to the native reference run (`run-native --blank`) |
| `<rom>:settings:wram` | a string sync setting (`initialWRamStatePattern`) reaches the guest: the RAM at frame 1 changes |
| `<rom>:keybinds` | from a configuration that has never seen this controller, the bindings in `default_keybinds.json` become the frontend's defaults |
| `<rom>:settings:region` | `region` set to `pal` reaches the guest: the picture differs from NTSC |

Any result other than PASS counts as failed here, a rom that is not found
included. The gate picks this core by name in the configuration it writes,
because a Chimera install can hold more than one package for the NES. The
script notes that the `keybinds` leg is decisive only when no other installed
package declares the same controller, as in CI. Logs and dumps go to
`waterbox/tests/work/`, which git ignores.

### What CI runs

`.github/workflows/minihawk.yml` runs `core-gate` and `frontend-gate` on pull
requests and pushes to `main` that touch `waterbox/**`, `source/**` or the
workflow file; every day at 04:00 UTC; and on manual dispatch. `publish` runs
when both jobs passed and the event is not a pull request. A push publishes
the rolling `dev` release. The scheduled run publishes `nightly-YYYY-MM-DD`,
and only when `main` moved since the last nightly. A manual run takes `kind`,
`dev` or `nightly`; blank means `dev`.

A second workflow, `.github/workflows/ci.yml`, runs on every pull request and
push to `main`. It builds the native configure without miniBox and runs the
ROM-free 6502 regression (`cpufuzz-regression`, which compares `cpuFuzz`'s
trace with `test/cpuFuzz.golden`). On a fresh clone it runs:

```sh
python3 -m pip install meson ninja
meson setup build -Dbuildtype=release
ninja -C build
meson test -C build
```

The native configure in "Build the core" defines the same test.

`harness/` runs genuine BizHawk NesHawk to produce reference traces. It needs
.NET and a local BizHawk build, and is run by hand, not in CI.

## Files the core needs at run time

Game files, BIOS and firmware are never in this repository or in the package.
The user provides them. From `waterbox/file_slots.json` and
`waterbox/waterbox.config`:

- The game. Required, exactly one: an iNES cartridge image (`.nes`), or a
  Famicom Disk System image (`.fds`). The `machine` setting says which
  machine the project is: `nes` (the default) or `fds`.
- Save data (`.sav` or `.srm`). Optional, at most one. It is the file
  Emulator > Export Save Data... writes: the battery-backed RAM of a
  cartridge, or how a disk differs from the one loaded. A machine that keeps
  no saves refuses one.
- The Famicom Disk System BIOS, for an `fds` project only. A cartridge needs
  no firmware. The firmware id is `bios`, the usual file name is
  `disksys.rom`, and the size is 8192 bytes. The `fdsBios` setting picks the
  dump:

  | `fdsBios` | SHA-1 |
  |---|---|
  | `nintendo` (default) | `57FE1BDEE955BB48D357E463CCBF129496930B62` |
  | `twinFamicom` | `E4E41472C454F928E53EB10E0509BF7D1146ECC1` |

Only some cartridge boards are translated. `waterbox/README.md` ("Limits
worth knowing") lists them; a rom on any other board is refused at load with
a message.

`waterbox/roms/sprilo.nes` is free to distribute homebrew. It is there for
the gates.

## Troubleshooting

- `miniHawk checkout not found; pass -r <path>` (`build-package.sh`) or
  `pass --minihawk-root <path>` (`run-frontend.sh`): pass the Chimera
  checkout. The two scripts do not look in the same places, and
  `run-frontend.sh` does not look for a directory named `chimera`.
- `miniBox C++ guest toolchain missing under <miniBox>/build/meson-cpp`
  (`setup-guest.sh`): build miniBox with `-Dguest_cpp=true` first. The plain
  miniBox build has no libstdc++ for the guest.
- `no miniBox checkout: run-wbx and run-tooling are not built` (meson
  warning, native build): pass `-Dminibox_dir=<miniBox>`. Without the
  drivers the core gate cannot run.
- `-Dminibox_dir=... is not a directory` (meson error): the path is wrong.
- `could not download the GCC <version> source from any mirror` (miniBox
  build): the C++ guest toolchain needs network access the first time.
- `native build missing` or `guest build missing` (`run-gate.sh`): the gate
  builds nothing. Build both flavors first; the message prints the commands.
- `packaging is not deterministic` (`build-package.sh`): two packings of the
  same staging directory differed. The package's SHA-1 is the core's
  identity, so the script stops.
- `EmuHawk not built`, `package not installed` or `native reference not
  built` (`run-frontend.sh`): build Chimera, run `build-package.sh`, build
  `run-native`. The message names the missing file.
- `Xvfb not found (apt install xvfb)` (`run-frontend.sh`): install `xvfb`.
- A frontend leg fails with `no OK meta` or `run did not report OK`: read the
  log it names under `waterbox/tests/work/`. A first run that cannot make its
  configuration says `config bootstrap failed`; read
  `waterbox/tests/work/bootstrap.log`.
- A rom is refused at load: an unsupported board or a truncated file. The
  core reports it instead of emulating.
- The Chimera checkout moved: run `waterbox/setup-guest.sh` again. The cross
  file holds absolute paths; the script rewrites it and reconfigures.
