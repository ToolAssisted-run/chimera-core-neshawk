/* core.wbx - QuickerNesHawk as a miniHawk waterbox core.
 *
 * The emulator is the unmodified header-only core under ../source; this file is only the waterbox
 * ABI layer over it. The whole machine lives in guest memory, so the miniBox host savestates it
 * automatically - there is no serialize/deserialize export here, and nesSerialization.hpp is not
 * used at all.
 *
 * What the frontend sees is deliberately NesHawk's own surface: NesHawk's settings (the sync ones
 * shape the machine, the rest are presentation), NesHawk's palettes, NesHawk's memory domains, and
 * the tools BizHawk offers for this core - PPU viewer surfaces, the 6502 debugger's registers, the
 * system and PPU buses, and a trace logger whose lines are formatted exactly like NesHawk's.
 *
 * The rom arrives as a mounted file "rom" (read at Init), per the side-effect rule: all data
 * crosses the host interface, never a host path.
 */
#include <emulibc.h>
#include <waterbox_settings.h>
#include <waterbox_slots.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <nes.hpp>

#include "nesPalettes.hpp"
#include "nes6502Disasm.hpp"
#include "audioResampler.hpp"

namespace
{
	constexpr int FbWidth = 256;
	constexpr int FbHeight = 240;
	constexpr int SampleRate = 44100;
	// A PAL frame is ~882 samples at 44.1kHz; the buffer is the package's declared capacity and the
	// host is told the real count per frame.
	constexpr int MaxSamplesPerFrame = 2048;

	nesHawk::NES *g_nes = nullptr;
	uint32_t g_video[FbWidth * FbHeight];

	/* Turbo. The host sets this to 0 when nobody is going to look at the frame,
	 * and the PPU stops writing pixels. It is host policy rather than machine
	 * state, so it lives outside the savestate; the PPU's own copy of it is
	 * inside the savestate, which is why FrameAdvance re-asserts it from here
	 * every frame rather than trusting what a loaded state left behind. */
	ECL_INVISIBLE int g_render = 1;
	int16_t g_audio[MaxSamplesPerFrame];
	int g_audioSamples = 0;

	/* The resampler is not machine state - it is a filter over the machine's output - so it lives in
	 * invisible memory like the rest of the non-state scratch, and savestates neither carry it nor
	 * disturb it. Sound is continuous across a state load either way, because the deltas that reach
	 * it are. */
	ECL_INVISIBLE audio::BandLimitedResampler<MaxSamplesPerFrame> g_resampler;

	/* NesHawk expands a 64-colour palette into the 512 entries the PPU can address (6 colour bits
	 * plus 3 emphasis bits) and looks the framebuffer up in that; see NES.Core.cs SetPalette. */
	uint32_t g_paletteCompiled[512];

	/* ---- settings ----
	 * The sync ones (NESSyncSettings) arrive in the mounted "settings" file and are read once at
	 * Init, because they shape the machine; the rest (NESSettings) can change while the core runs
	 * and arrive through PutSettings. */
	enum PortMode
	{
		portNone = 0,
		portGamepad,
	};

	PortMode g_port1 = portGamepad;
	PortMode g_port2 = portNone;

	// presentation, all live (NESSettings)
	bool g_clipLeftAndRight = false;
	int32_t g_backgroundColor = -1; // <0 = off, else 0xRRGGBB (NesHawk keys this off the alpha byte)
	int g_topLine = 8, g_bottomLine = 231;
	int g_ntscTop = 8, g_ntscBottom = 231, g_palTop = 0, g_palBottom = 239;

	ECL_INVISIBLE char g_settingsBuf[4096];

	const uint8_t (*paletteByName(const char *name))[3]
	{
		if (!strcmp(name, "fceux")) return nesPalettes::FCEUX_Standard;
		if (!strcmp(name, "2c03_2c05")) return nesPalettes::palette_2c03_2c05;
		if (!strcmp(name, "2c04_001")) return nesPalettes::palette_2c04_001;
		if (!strcmp(name, "2c04_002")) return nesPalettes::palette_2c04_002;
		if (!strcmp(name, "2c04_003")) return nesPalettes::palette_2c04_003;
		if (!strcmp(name, "2c04_004")) return nesPalettes::palette_2c04_004;
		return nesPalettes::QuickNESPalette; // NESSettings' own default
	}

	void compilePalette(const uint8_t (*pal)[3])
	{
		for (int i = 0; i < 64 * 8; i++)
		{
			const int c = i & 63;
			int r = pal[c][0], g = pal[c][1], b = pal[c][2];
			nesPalettes::applyDeemphasis(r, g, b, i >> 6);
			g_paletteCompiled[i] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
		}
	}

	void applyLiveSettings()
	{
		char buf[32];
		if (wbx_setting_str("palette", buf, sizeof buf) < 0) strcpy(buf, "quickNES");
		compilePalette(paletteByName(buf));

		nesHawk::nesSettings::DispBackground = wbx_setting_bool("dispBackground", 1) != 0;
		nesHawk::nesSettings::DispSprites = wbx_setting_bool("dispSprites", 1) != 0;

		g_clipLeftAndRight = wbx_setting_bool("clipLeftAndRight", 0) != 0;
		g_backgroundColor = (int32_t)wbx_setting_long("backgroundColor", -1);
		g_ntscTop = (int)wbx_setting_long("ntscTopLine", 8);
		g_ntscBottom = (int)wbx_setting_long("ntscBottomLine", 231);
		g_palTop = (int)wbx_setting_long("palTopLine", 0);
		g_palBottom = (int)wbx_setting_long("palBottomLine", 239);

		if (g_nes != nullptr)
		{
			const bool ntsc = g_nes->_display_type == nesHawk::PPU::Region::NTSC;
			g_topLine = ntsc ? g_ntscTop : g_palTop;
			g_bottomLine = ntsc ? g_ntscBottom : g_palBottom;
			if (g_topLine < 0) g_topLine = 0;
			if (g_bottomLine > FbHeight - 1) g_bottomLine = FbHeight - 1;

			long vol = wbx_setting_long("apuVolume", 1);
			if (vol < 1) vol = 1;
			if (vol > 10) vol = 10;
			g_nes->apu->m_vol = (int)vol;
		}
	}

	nesHawk::PPU::Region parseRegion()
	{
		char buf[16];
		if (wbx_setting_str("region", buf, sizeof buf) < 0) return nesHawk::PPU::Region::NTSC;
		if (!strcmp(buf, "pal")) return nesHawk::PPU::Region::PAL;
		if (!strcmp(buf, "dendy")) return nesHawk::PPU::Region::Dendy;
		return nesHawk::PPU::Region::NTSC;
	}

	PortMode parsePort(const char *key, PortMode dflt)
	{
		char buf[16];
		if (wbx_setting_str(key, buf, sizeof buf) < 0) return dflt;
		if (!strcmp(buf, "none")) return portNone;
		if (!strcmp(buf, "gamepad")) return portGamepad;
		return dflt;
	}

	/* NesHawk takes InitialWRamStatePattern as a byte array; the settings channel carries scalars,
	 * so it arrives as a hex string ("A5" or "DE AD BE EF"). Anything unparseable yields an empty
	 * pattern, which is exactly NesHawk's "unset" and gives the fceux power-on fill. */
	int parseWRamPattern(uint8_t *out, int outsz)
	{
		char text[256];
		if (wbx_setting_str("initialWRamStatePattern", text, sizeof text) < 0) return 0;

		int n = 0;
		int nibbles = 0;
		uint8_t acc = 0;
		for (const char *p = text; *p != 0 && n < outsz; p++)
		{
			int v;
			if (*p >= '0' && *p <= '9') v = *p - '0';
			else if (*p >= 'a' && *p <= 'f') v = *p - 'a' + 10;
			else if (*p >= 'A' && *p <= 'F') v = *p - 'A' + 10;
			else continue; // separators are free-form: spaces, commas, "0x", whatever the user typed
			acc = (uint8_t)((acc << 4) | v);
			if (++nibbles == 2)
			{
				out[n++] = acc;
				nibbles = 0;
				acc = 0;
			}
		}
		return n;
	}

	/* Every change in the mixed APU output is a delta at the current sample clock; the resampler
	 * turns that stream into band-limited samples. This is the point where NES.Core.cs feeds its
	 * BlipBuffer. */
	void onSample(void *ctx, uint32_t clock, int delta)
	{
		((audio::BandLimitedResampler<MaxSamplesPerFrame> *)ctx)->AddDelta(clock, delta);
	}

	/* Why the last Init refused, in words meant for the person who has to fix it. The host reads
	 * this through GetLoadError and shows it instead of a stack trace, so a core is the one that
	 * explains itself: only it knows that this file is a disk image, or that its mapper is not
	 * translated yet. ECL_INVISIBLE - it is a diagnostic, not machine state. */
	ECL_INVISIBLE char g_loadError[512];

	void setLoadError(const char *what)
	{
		snprintf(g_loadError, sizeof g_loadError, "%s", what);
	}

	// Reads a whole mounted file (caller frees). Null if it is not mounted at all - which is how a
	// firmware file the user has not provided shows up, since the host only mounts what it has.
	uint8_t *readMounted(const char *name, uint32_t *outLen)
	{
		FILE *f = fopen(name, "rb");
		if (!f) return nullptr;
		fseek(f, 0, SEEK_END);
		long n = ftell(f);
		fseek(f, 0, SEEK_SET);
		auto *buf = (uint8_t *)malloc(n > 0 ? (size_t)n : 1);
		if (!buf) { fclose(f); return nullptr; }
		if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return nullptr; }
		fclose(f);
		*outLen = (uint32_t)n;
		return buf;
	}

	/* The memory domains NesHawk publishes for this board (NES.IMemoryDomains.cs), minus the two
	 * that are address spaces rather than storage - those are buses, and have their own ABI group.
	 * The host maps these by pointer once, after Init, which is why the Power button is not part of
	 * the controller: a hard reset rebuilds the board and would leave the host pointing at freed
	 * vectors. Soft reset, which is what the Reset button sends, touches none of them. */
	bool memoryArea(int which, const void **data, int *size, int *writable, const char **name)
	{
		if (!g_nes) return false;
		auto &board = *g_nes->board;
		*writable = 1;
		switch (which)
		{
			case 0: *data = g_nes->ram;           *size = 0x800; *name = "RAM";                return true;
			case 1: *data = g_nes->CIRAM;         *size = 0x800; *name = "CIRAM (nametables)"; return true;
			case 2: *data = g_nes->ppu->PALRAM;   *size = 32;    *name = "PALRAM";             return true;
			case 3: *data = g_nes->ppu->OAM;      *size = 256;   *name = "OAM";                return true;
			case 4:
				if (board.Rom.empty()) return false;
				*data = board.Rom.data(); *size = (int)board.Rom.size(); *name = "PRG ROM"; return true;
			case 5:
				// NesHawk publishes these as two domains, and so does the board: Vrom is the
				// cartridge's own CHR, Vram is writable pattern memory. A cart has one or the other.
				if (!board.Vrom.empty())
				{
					*data = board.Vrom.data(); *size = (int)board.Vrom.size(); *name = "CHR VROM"; return true;
				}
				if (board.Vram.empty()) return false;
				*data = board.Vram.data(); *size = (int)board.Vram.size(); *name = "VRAM"; return true;
			case 6:
				if (board.Wram.empty()) return false;
				*data = board.Wram.data(); *size = (int)board.Wram.size(); *name = "WRAM"; return true;
			default: return false;
		}
	}
}

extern "C"
{

/// The reason the last Init failed, or an empty string. Read by the host after Init returns 0.
ECL_EXPORT const char *GetLoadError(void) { return g_loadError; }

ECL_EXPORT int Init(void)
{
	g_loadError[0] = '\0';

	/* The cartridge. A chimera project mounts "slots" naming its canonical
	 * file ({"rom":["x.nes"]}, see file_slots.json); without the map, the
	 * legacy "rom" mount. */
	char romName[256] = "rom";
	wbx_slot_first("rom", romName, sizeof romName);
	uint32_t romLen = 0;
	uint8_t *rom = readMounted(romName, &romLen);
	if (!rom)
	{
		setLoadError("no rom was mounted");
		return 0;
	}

	/* The disk system BIOS, declared as firmware in waterbox.config and mounted by the frontend
	 * under that id. A cartridge never asks for it, so its absence is only an error for a disk
	 * image - and that error comes from the core, which is the part that knows. */
	uint32_t biosLen = 0;
	uint8_t *bios = readMounted("bios", &biosLen);

	// sync settings: everything that shapes the machine, read once (NESSyncSettings)
	const nesHawk::PPU::Region region = parseRegion();
	g_port1 = parsePort("port1", portGamepad);
	g_port2 = parsePort("port2", portNone);
	nesHawk::nesSettings::AllowMoreThanEightSprites = wbx_setting_bool("allowMoreThanEightSprites", 0) != 0;
	uint8_t wramPattern[128];
	const int wramPatternLen = parseWRamPattern(wramPattern, (int)sizeof wramPattern);

	// The core rejects a rom it cannot run - an unsupported mapper, a truncated file - by throwing,
	// which here means Init fails and the frontend says so, rather than emulating nonsense.
	try
	{
		g_nes = new nesHawk::NES(rom, romLen, region, wramPatternLen != 0 ? wramPattern : nullptr, (size_t)wramPatternLen,
		                         bios, (size_t)biosLen);
	}
	catch (const std::exception &e)
	{
		setLoadError(e.what());
		printf("QuickerNesHawk: cannot load this rom: %s\n", e.what());
		free(rom);
		free(bios);
		return 0;
	}
	free(rom);
	free(bios);

	g_nes->controllerDeck._port2Connected = g_port2 == portGamepad;

	// audio: the deltas the core emits are clocked by the CPU, so the ratio follows the region
	g_resampler.SetRates(g_nes->cpuclockrate, SampleRate);
	g_nes->sampleCallback = onSample;
	g_nes->sampleCallbackCtx = &g_resampler;

	/* What the cartridge starts with already saved.
	 *
	 * NesHawk already knows what a save is for this machine: battery-backed
	 * WRAM on a cartridge, and the difference from the disk as loaded on an
	 * FDS. The file the project mounted goes in through that same door, here -
	 * after the machine is built, so the memory exists, and inside Init, so it
	 * is part of the sealed baseline rather than of every savestate.
	 *
	 * A machine that keeps no saves refuses one rather than quietly ignoring
	 * it: a project carrying someone's progress that never loads is worse than
	 * one that will not start. */
	{
		char saveName[256] = "";
		if (wbx_slot_first("savedata", saveName, sizeof saveName))
		{
			if (!g_nes->board->HasSaveRam())
			{
				snprintf(g_loadError, sizeof g_loadError,
					"this machine keeps no saves - it has no battery and no disk - so "
					"'%s' would not be read.", saveName);
				return 0;
			}
			FILE *sf = fopen(saveName, "rb");
			if (sf == nullptr)
			{
				snprintf(g_loadError, sizeof g_loadError, "could not open '%s'", saveName);
				return 0;
			}
			std::vector<uint8_t> bytes;
			uint8_t chunk[16 * 1024];
			size_t got;
			while ((got = fread(chunk, 1, sizeof chunk, sf)) > 0)
				bytes.insert(bytes.end(), chunk, chunk + got);
			fclose(sf);
			try
			{
				g_nes->board->StoreSaveRam(bytes.data(), bytes.size());
			}
			catch (const std::exception &e)
			{
				snprintf(g_loadError, sizeof g_loadError, "'%s' does not fit this machine: %s",
					saveName, e.what());
				return 0;
			}
		}
	}

	// the non-sync settings were mounted alongside the sync ones, so the machine starts at the
	// user's chosen values rather than at the package defaults
	applyLiveSettings();
	return 1;
}

/* ---- save data (guest ABI: the savedata group) ----
 *
 * The user's way out, and the shape the savedata slot expects back. NesHawk
 * decides what a save IS for this machine - battery WRAM on a cartridge, the
 * difference from the disk as loaded on an FDS - so the name says which.
 *
 * docs/save-data.md said a core whose saves are plain machine memory need not
 * export the group, since savestates already carry them. True of REPRODUCTION,
 * false of the user: without this there is no way to take a saved game out of
 * the machine, and no way to start another project from it. The snapshot is
 * taken on Count(), as the ABI requires, because ReadSaveRam builds it. */
static std::vector<uint8_t> g_saveDataSnapshot;

ECL_EXPORT int32_t GetSaveDataFileCount(void)
{
	if (g_nes == nullptr || !g_nes->board->HasSaveRam())
		return 0;
	g_saveDataSnapshot = g_nes->board->ReadSaveRam();
	return g_saveDataSnapshot.empty() ? 0 : 1;
}

ECL_EXPORT const char *GetSaveDataFileName(int32_t i)
{
	if (i != 0 || g_nes == nullptr)
		return nullptr;
	/* the disk system saves what changed on the disk; a cartridge saves its battery */
	return g_nes->board->kind == nesHawk::NesBoard::Kind::FDS ? "disk.sav" : "battery.sav";
}

ECL_EXPORT int64_t GetSaveDataFileSize(int32_t i)
{
	return i == 0 ? (int64_t)g_saveDataSnapshot.size() : 0;
}

ECL_EXPORT const uint8_t *GetSaveDataFileBuffer(int32_t i)
{
	return i == 0 ? g_saveDataSnapshot.data() : nullptr;
}

/* ---- live (non-sync) settings ----
 * The mounted "settings" file is fixed for the core's lifetime, so a setting that can change while
 * the core runs arrives here instead: the host writes fresh JSON into g_settingsBuf and calls
 * PutSettings. The buffer is ECL_INVISIBLE because settings are not machine state and must not end
 * up in savestates. */
ECL_EXPORT int GetSettingsCapacity(void) { return (int)sizeof g_settingsBuf; }

ECL_EXPORT char *GetSettingsBuffer(void) { return g_settingsBuf; }

ECL_EXPORT void PutSettings(int length)
{
	if (length < 0 || length > (int)sizeof g_settingsBuf) return;
	wbx_settings_use_buffer(g_settingsBuf, length);
	applyLiveSettings();
	wbx_settings_use_file();
}

/* Input bit layout, matching waterbox.config's input.buttons order. The byte order within a pad is
 * the NES joypad's own (A, B, Select, Start, Up, Down, Left, Right), which is also nesControllers'
 * ButtonBit order, so a byte of the mask passes straight through.
 *   bits  0-7  P1
 *   bits  8-15 P2 (latched only when port 2 has a pad plugged in)
 *   bit  16    Reset
 *   bit  17    FDS Eject
 *   bits 18-21 FDS Insert 0..3
 * There is no Power button: a hard reset rebuilds the board, and the host has already mapped the
 * board's memory domains by pointer.
 *
 * The disk buttons are declared for every rom because a package's controller is fixed, and are
 * inert unless a disk system image is loaded. NesHawk declares one Insert per side of the mounted
 * image; four covers every commercial disk, and asking for a side the image does not have does
 * nothing rather than failing. */
ECL_EXPORT void FrameAdvance(uint64_t input)
{
	const uint8_t pad1 = g_port1 == portGamepad ? (uint8_t)(input & 0xFF) : 0;
	const uint8_t pad2 = g_port2 == portGamepad ? (uint8_t)((input >> 8) & 0xFF) : 0;

	g_nes->resetSignal = (input >> 16) & 1;

	// NES.Core.cs FrameAdvance: the drive is worked before the frame runs, and both buttons are
	// level triggered - holding Insert re-seats the disk every frame, as it does in NesHawk.
	if (g_nes->board->kind == nesHawk::NesBoard::Kind::FDS)
	{
		auto &fds = g_nes->board->fds;
		if ((input >> 17) & 1) fds.Eject();
		for (int side = 0; side < 4 && side < fds.NumSides(); side++)
		{
			if ((input >> (18 + side)) & 1) fds.InsertSide(side);
		}
	}
	g_nes->ppu->render_enabled = g_render != 0;
	g_nes->FrameAdvance(pad1, pad2);

	// Video: NesHawk's own framebuffer walk (NES.cs MyVideoProvider.FillFrameBuffer). xbuf holds a
	// 6-bit colour plus the 3 emphasis bits, and bit 15 marks a pixel the PPU did not draw, which
	// the backdrop colour replaces when one is set. Rows and columns outside the configured crop
	// are blanked rather than cut, because a package declares one fixed frame size.
	// In turbo the PPU wrote no pixels, so there is nothing to walk and the
	// buffer keeps the last frame that was drawn.
	if (g_render)
	{
		const bool useBackdrop = g_backgroundColor >= 0;
		const uint32_t backdrop = useBackdrop ? (0xFF000000u | (uint32_t)g_backgroundColor) : 0xFF000000u;
		const int left = g_clipLeftAndRight ? 8 : 0;
		const int right = g_clipLeftAndRight ? 247 : 255;
		const int16_t *src = g_nes->ppu->xbuf;
		for (int y = 0; y < FbHeight; y++)
		{
			const bool rowVisible = y >= g_topLine && y <= g_bottomLine;
			for (int x = 0; x < FbWidth; x++)
			{
				const int16_t pixel = src[(y << 8) + x];
				uint32_t out;
				if (!rowVisible || x < left || x > right) out = 0xFF000000u;
				else if ((pixel & 0x8000) != 0 && useBackdrop) out = backdrop;
				else out = g_paletteCompiled[pixel & 0x1FF];
				g_video[y * FbWidth + x] = out;
			}
		}
	}

	// Audio: close the frame at the APU's sample clock and drain (NES.Core.cs GetSamplesSync).
	g_resampler.EndFrame(g_nes->apu->sampleclock);
	g_nes->apu->sampleclock = 0;
	g_audioSamples = g_resampler.ReadSamples(g_audio, MaxSamplesPerFrame);
}

/* Turbo (optional guest ABI group): while off the core must produce no picture
 * and must otherwise be exactly the machine it would have been. run-gate.sh's
 * turbo leg is the proof - N undrawn frames plus one drawn one come out byte for
 * byte the same machine, and the same picture, as N+1 drawn ones. */
ECL_EXPORT void SetRenderingEnabled(int on) { g_render = on != 0; }

ECL_EXPORT uint32_t *GetVideoBgra(void) { return g_video; }
ECL_EXPORT int16_t *GetAudio(void) { return g_audio; }
ECL_EXPORT int GetAudioSampleCount(void) { return g_audioSamples; }

/* The frame rate and the samples per frame depend on the region, which is a user setting, so they
 * are answered after Init rather than declared in waterbox.config. */
ECL_EXPORT int GetVsyncNumerator(void) { return g_nes != nullptr ? g_nes->VsyncNum : 0; }
ECL_EXPORT int GetVsyncDenominator(void) { return g_nes != nullptr ? g_nes->VsyncDen : 0; }

/* A frame that never polled a controller is a lag frame - NesHawk's own islag (NES.IInputPollable.cs
 * IsLagFrame), which the core sets from the joypad read path. */
ECL_EXPORT int InputWasRead(void) { return g_nes != nullptr && !g_nes->islag; }

/* --- self-described memory domains (guest ABI v1) --- */
ECL_EXPORT int GetMemoryDomainCount(void)
{
	int n = 0;
	const void *data; int size, writable; const char *name;
	// a domain may be absent (no WRAM on this cart), so count the ones that answer
	for (int i = 0; i < 32; i++)
		if (memoryArea(i, &data, &size, &writable, &name)) n = i + 1;
	return n;
}

ECL_EXPORT const char *GetMemoryDomainName(int i)
{
	const void *data; int size, writable; const char *name = nullptr;
	return memoryArea(i, &data, &size, &writable, &name) ? name : nullptr;
}

ECL_EXPORT uint8_t *GetMemoryDomainPtr(int i)
{
	const void *data = nullptr; int size, writable; const char *name;
	return memoryArea(i, &data, &size, &writable, &name) ? (uint8_t *)data : nullptr;
}

ECL_EXPORT int64_t GetMemoryDomainSize(int i)
{
	const void *data; int size = 0, writable; const char *name;
	return memoryArea(i, &data, &size, &writable, &name) ? size : 0;
}

ECL_EXPORT int GetMemoryDomainWritable(int i)
{
	const void *data; int size, writable = 0; const char *name;
	return memoryArea(i, &data, &size, &writable, &name) ? writable : 0;
}

} // extern "C"

/* ===================== core-managed tooling (guest ABI v2) =====================
 *
 * miniHawk is core-agnostic, so the tools BizHawk builds for NesHawk (the PPU viewer, the
 * nametable viewer, the 6502 debugger, the trace logger) live HERE, published through four
 * system-neutral mechanisms: surfaces the core renders itself, named registers, named address
 * spaces, and a trace ring buffer drained once per frame. Every group is optional; the host probes
 * for the exports and enables only what it finds.
 */
namespace
{
	constexpr int NtWidth = 512, NtHeight = 480;   // 2x2 nametables of 256x240
	constexpr int PtWidth = 256, PtHeight = 128;   // 2 pattern tables of 128x128
	constexpr int OamWidth = 256, OamHeight = 256; // 64 sprites, 8 per row, room for 8x16
	constexpr int PalWidth = 256, PalHeight = 32;  // 2 rows of 16 swatches

	/* Tooling scratch lives in the INVISIBLE section: it is not part of any savestate, so opening a
	 * viewer or the trace logger costs nothing per state. Nothing in emulation may read it. */
	ECL_INVISIBLE uint32_t g_surface[NtWidth * NtHeight];

	inline uint32_t nesColor(int palIndex)
	{
		return g_paletteCompiled[palIndex & 0x3F];
	}

	uint8_t peekPpu(uint16_t addr)
	{
		return g_nes->ppu->ppubus_peek(addr & 0x3FFF);
	}

	/* Renders one 8x8 tile from the pattern table at `patternBase` into dst. */
	void drawTile(uint32_t *dst, int stride, int tile, int patternBase, const uint8_t *palette4)
	{
		for (int row = 0; row < 8; row++)
		{
			const int addr = patternBase + tile * 16 + row;
			const uint8_t lo = peekPpu((uint16_t)addr);
			const uint8_t hi = peekPpu((uint16_t)(addr + 8));
			for (int col = 0; col < 8; col++)
			{
				const int bit = 7 - col;
				const int v = ((lo >> bit) & 1) | (((hi >> bit) & 1) << 1);
				dst[row * stride + col] = nesColor(palette4[v]);
			}
		}
	}

	void renderNametables()
	{
		const int bgBase = g_nes->ppu->reg_2000.bg_pattern_hi ? 0x1000 : 0x0000;
		const uint8_t *palRam = g_nes->ppu->PALRAM;

		for (int nt = 0; nt < 4; nt++)
		{
			const int ntBase = 0x2000 + nt * 0x400;
			const int ox = (nt & 1) * 256, oy = (nt >> 1) * 240;
			for (int ty = 0; ty < 30; ty++)
			{
				for (int tx = 0; tx < 32; tx++)
				{
					const int tile = peekPpu((uint16_t)(ntBase + ty * 32 + tx));
					// attribute byte: 4x4 tile blocks, 2 bits per 2x2 quadrant
					const int attr = peekPpu((uint16_t)(ntBase + 0x3C0 + (ty / 4) * 8 + (tx / 4)));
					const int quad = ((ty & 2) << 1) | (tx & 2);
					const int palSel = (attr >> quad) & 3;
					uint8_t pal4[4];
					pal4[0] = palRam[0];
					for (int i = 1; i < 4; i++) pal4[i] = palRam[palSel * 4 + i];
					drawTile(&g_surface[(oy + ty * 8) * NtWidth + ox + tx * 8], NtWidth, tile, bgBase, pal4);
				}
			}
		}
	}

	void renderPatternTables()
	{
		const uint8_t *palRam = g_nes->ppu->PALRAM;
		uint8_t pal4[4] = { palRam[0], palRam[1], palRam[2], palRam[3] };
		for (int half = 0; half < 2; half++)
			for (int ty = 0; ty < 16; ty++)
				for (int tx = 0; tx < 16; tx++)
					drawTile(&g_surface[(ty * 8) * PtWidth + half * 128 + tx * 8], PtWidth,
						ty * 16 + tx, half * 0x1000, pal4);
	}

	void renderSprites()
	{
		// 8x16 sprites take their pattern table from the tile index's low bit, not from reg_2000
		const bool tall = (bool)g_nes->ppu->reg_2000.obj_size_16;
		const int spBase = g_nes->ppu->reg_2000.obj_pattern_hi ? 0x1000 : 0x0000;
		const uint8_t *oam = g_nes->ppu->OAM;
		const uint8_t *palRam = g_nes->ppu->PALRAM;
		memset(g_surface, 0, sizeof(uint32_t) * OamWidth * OamHeight);
		for (int s = 0; s < 64; s++)
		{
			const uint8_t tile = oam[s * 4 + 1];
			const uint8_t attr = oam[s * 4 + 2];
			uint8_t pal4[4];
			pal4[0] = palRam[0];
			for (int i = 1; i < 4; i++) pal4[i] = palRam[0x10 + (attr & 3) * 4 + i];
			const int ox = (s % 8) * 32, oy = (s / 8) * 32;
			if (tall)
			{
				const int base = (tile & 1) ? 0x1000 : 0x0000;
				drawTile(&g_surface[oy * OamWidth + ox], OamWidth, tile & ~1, base, pal4);
				drawTile(&g_surface[(oy + 8) * OamWidth + ox], OamWidth, (tile & ~1) + 1, base, pal4);
			}
			else
			{
				drawTile(&g_surface[oy * OamWidth + ox], OamWidth, tile, spBase, pal4);
			}
		}
	}

	void renderPalettes()
	{
		// PALRAM as the PPU viewer draws it: the four background palettes on top, the four sprite
		// palettes below, each entry a 16x16 swatch
		const uint8_t *palRam = g_nes->ppu->PALRAM;
		for (int i = 0; i < 32; i++)
		{
			const uint32_t color = nesColor(palRam[i]);
			const int ox = (i & 15) * 16, oy = (i >> 4) * 16;
			for (int y = 0; y < 16; y++)
				for (int x = 0; x < 16; x++)
					g_surface[(oy + y) * PalWidth + ox + x] = color;
		}
	}

	struct SurfaceDef { const char *name; int w, h; void (*render)(); };
	const SurfaceDef kSurfaces[] = {
		{ "Nametables",     NtWidth,  NtHeight,  renderNametables },
		{ "Pattern Tables", PtWidth,  PtHeight,  renderPatternTables },
		{ "Sprites (OAM)",  OamWidth, OamHeight, renderSprites },
		{ "Palettes",       PalWidth, PalHeight, renderPalettes },
	};
	constexpr int kSurfaceCount = (int)(sizeof kSurfaces / sizeof kSurfaces[0]);

	/* MOS6502X.GetCpuFlagsAndRegisters, in its own order: the five registers, then the flags as
	 * one-bit values. NesHawk's SetCpuRegister throws, so there is no SetRegisterValue export and
	 * the debugger shows the box read-only. */
	const char *const kRegNames[] = {
		"A", "X", "Y", "S", "PC", "P",
		"Flag C", "Flag Z", "Flag I", "Flag D", "Flag B", "Flag V", "Flag N", "Flag T",
	};
	const int kRegBits[] = { 8, 8, 8, 8, 16, 8, 1, 1, 1, 1, 1, 1, 1, 1 };
	const uint8_t kFlagMask[] = { 0x01, 0x02, 0x04, 0x08, 0x10, 0x40, 0x80, 0x20 };
	constexpr int kRegCount = (int)(sizeof kRegNames / sizeof kRegNames[0]);

	const char *const kBusNames[] = { "System Bus", "PPU Bus" };
	const int64_t kBusSizes[] = { 0x10000, 0x4000 };
	constexpr int kBusCount = 2;

	/* Trace ring buffer: the tracer runs inside the guest and appends NUL-terminated lines here;
	 * the host drains it once per frame. Crossing the sandbox boundary per instruction would be
	 * unusably slow. */
	// A traced NTSC frame is ~9700 instructions at ~95 bytes, so 1MB would leave almost no margin
	// (a PAL frame is longer still); the buffer lives in invisible memory and costs nothing per
	// savestate, so it is sized to never overflow in practice.
	constexpr int TraceBufBytes = 2 << 20;
	ECL_INVISIBLE char g_traceBuf[TraceBufBytes];
	int g_traceUsed = 0;
	int g_traceLines = 0;
	int g_traceEnabled = 0;
	int g_traceOverflow = 0;

	uint8_t peekCpu(uint16_t addr)
	{
		return g_nes->PeekMemory(addr);
	}

	/* MOS6502X.State(): "PC: bytes  DISASM" padded to 32 columns, then the register block. A tab
	 * splits the line into the host's two trace-logger columns - what executed on the left, machine
	 * state on the right - which is where BizHawk's two TraceInfo fields go. */
	void traceCallback(nesHawk::NES &nes, uint16_t pc)
	{
		if (!g_traceEnabled) return;

		char disasm[48];
		int length = 1;
		nesDisasm::disassemble(pc, disasm, (int)sizeof disasm, peekCpu, &length);

		char rawbytes[16] = { 0 };
		int at = 0;
		for (int i = 0; i < length && at < (int)sizeof rawbytes - 4; i++)
			at += snprintf(rawbytes + at, sizeof rawbytes - (size_t)at, " %02X", peekCpu((uint16_t)(pc + i)));

		char left[64];
		snprintf(left, sizeof left, "%04X: %-9s  %s ", pc, rawbytes, disasm);
		for (int i = (int)strlen(left); i < 32 && i < (int)sizeof left - 1; i++) { left[i] = ' '; left[i + 1] = 0; }

		const auto &cpu = *nes.cpu;
		const uint8_t p = cpu.P;
		char line[192];
		const int n = snprintf(line, sizeof line,
			"%s\tA:%02X  X:%02X  Y:%02X  SP:%02X  P:%02X  %c%c%c%c%c%c%c%c  Cy:%lld  PPU-Cy:%d",
			left, cpu.A, cpu.X, cpu.Y, cpu.S, p,
			(p & 0x80) ? 'N' : 'n', (p & 0x40) ? 'V' : 'v', (p & 0x20) ? 'T' : 't', (p & 0x10) ? 'B' : 'b',
			(p & 0x08) ? 'D' : 'd', (p & 0x04) ? 'I' : 'i', (p & 0x02) ? 'Z' : 'z', (p & 0x01) ? 'C' : 'c',
			(long long)cpu.TotalExecutedCycles, nes.ExtPpuCycle());
		if (n < 0) return;
		if (g_traceUsed + n + 1 > TraceBufBytes) { g_traceOverflow = 1; return; }
		memcpy(g_traceBuf + g_traceUsed, line, (size_t)n + 1);
		g_traceUsed += n + 1;
		g_traceLines++;
	}
}

extern "C"
{

/* ---- surfaces ---- */
ECL_EXPORT int GetSurfaceCount(void) { return kSurfaceCount; }
ECL_EXPORT const char *GetSurfaceName(int i) { return (i >= 0 && i < kSurfaceCount) ? kSurfaces[i].name : nullptr; }
ECL_EXPORT int GetSurfaceWidth(int i) { return (i >= 0 && i < kSurfaceCount) ? kSurfaces[i].w : 0; }
ECL_EXPORT int GetSurfaceHeight(int i) { return (i >= 0 && i < kSurfaceCount) ? kSurfaces[i].h : 0; }
ECL_EXPORT uint32_t *RenderSurface(int i)
{
	if (i < 0 || i >= kSurfaceCount || !g_nes) return nullptr;
	kSurfaces[i].render();
	return g_surface;
}

/* ---- cpu registers ---- */
ECL_EXPORT int GetRegisterCount(void) { return kRegCount; }
ECL_EXPORT const char *GetRegisterName(int i) { return (i >= 0 && i < kRegCount) ? kRegNames[i] : nullptr; }
ECL_EXPORT int GetRegisterBits(int i) { return (i >= 0 && i < kRegCount) ? kRegBits[i] : 0; }
ECL_EXPORT int64_t GetRegisterValue(int i)
{
	if (i < 0 || i >= kRegCount || !g_nes) return 0;
	const auto &cpu = *g_nes->cpu;
	switch (i)
	{
		case 0: return cpu.A;
		case 1: return cpu.X;
		case 2: return cpu.Y;
		case 3: return cpu.S;
		case 4: return cpu.PC;
		case 5: return cpu.P;
		default: return (cpu.P & kFlagMask[i - 6]) != 0;
	}
}
ECL_EXPORT int64_t GetExecutedCycles(void) { return g_nes != nullptr ? g_nes->TotalExecutedCycles() : 0; }

/* ---- address buses (peek/poke beyond the memory domains) ----
 * NesHawk publishes these as memory domains (NES.IMemoryDomains.cs); here they are buses, because
 * they are address SPACES resolved through the mapper rather than blocks of storage. */
ECL_EXPORT int GetBusCount(void) { return kBusCount; }
ECL_EXPORT const char *GetBusName(int i) { return (i >= 0 && i < kBusCount) ? kBusNames[i] : nullptr; }
ECL_EXPORT int GetBusWritable(int i) { return i >= 0 && i < kBusCount; }
ECL_EXPORT int64_t GetBusSize(int i) { return (i >= 0 && i < kBusCount) ? kBusSizes[i] : 0; }
ECL_EXPORT int PeekBus(int bus, int addr)
{
	if (!g_nes) return -1;
	if (bus == 0) return g_nes->PeekMemory((uint16_t)(addr & 0xFFFF));
	if (bus == 1) return g_nes->ppu->ppubus_peek(addr & 0x3FFF);
	return -1;
}
ECL_EXPORT void PokeBus(int bus, int addr, int value)
{
	if (!g_nes) return;
	if (bus == 0)
	{
		// NES.Core.cs ApplySystemBusPoke, minus the cheat path: a poke into ROM does nothing
		addr &= 0xFFFF;
		if (addr < 0x2000) g_nes->ram[addr & 0x7FF] = (uint8_t)value;
		else if (addr < 0x4000) g_nes->ppu->WriteReg(addr, (uint8_t)value);
		else if (addr < 0x4020) g_nes->WriteReg(addr, (uint8_t)value);
		else if (addr >= 0x6000 && addr < 0x8000) g_nes->board->WriteWram(addr - 0x6000, (uint8_t)value);
	}
	else if (bus == 1)
	{
		g_nes->ppu->ppubus_write(addr & 0x3FFF, (uint8_t)value);
	}
}

/* ---- instruction trace ---- */
ECL_EXPORT void TraceSetEnabled(int on)
{
	g_traceEnabled = on;
	if (g_nes != nullptr) g_nes->traceCallback = on ? traceCallback : nullptr;
	if (!on) { g_traceUsed = 0; g_traceLines = 0; g_traceOverflow = 0; }
}
ECL_EXPORT const char *TraceGetHeader(void) { return "6502: PC, bytes, disassembly | A, X, Y, SP, P, flags, cycles"; }
ECL_EXPORT int TraceGetLineCount(void) { return g_traceLines; }
ECL_EXPORT char *TraceGetBuffer(void) { return g_traceBuf; }
/* lets the host copy the whole frame's lines in one go instead of per line */
ECL_EXPORT int TraceGetUsedBytes(void) { return g_traceUsed; }
ECL_EXPORT int TraceGetOverflow(void) { return g_traceOverflow; }
ECL_EXPORT void TraceClear(void) { g_traceUsed = 0; g_traceLines = 0; g_traceOverflow = 0; }

} // extern "C"

int main() { return 0; }
