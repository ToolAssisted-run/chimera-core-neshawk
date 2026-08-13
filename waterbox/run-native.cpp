/* The reference half of the equivalence gate: the same core, built natively against the host libc,
 * driven over the same rom with the same per-frame inputs as run-wbx, printing the same digests.
 *
 * What this compares is the SANDBOX, not the translation: identical sources go through two very
 * different toolchains (glibc/-O2/PIE here, musl/-mcmodel=large/static non-PIE inside the guest),
 * and the emulated machine has to come out bit for bit the same anyway. Fidelity to the C# NesHawk
 * is a different gate - see ../harness and ../test.
 *
 * The settings applied here are the package's defaults (see waterbox.config); run-wbx mounts an
 * empty settings file, so the guest resolves the same ones.
 *
 * usage: run-native <rom.nes> <frames> [--blank]
 *
 * --blank holds every button released, which is what a frontend replaying nothing does; the
 * frontend witness compares its RAM dump against this run.
 */
#define NESHAWK_FULL_AV 1

#include "../source/nes.hpp"
#include "nesPalettes.hpp"
#include "audioResampler.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace
{
	constexpr int FbWidth = 256, FbHeight = 240;
	constexpr int SampleRate = 44100;
	constexpr int MaxSamplesPerFrame = 2048;

	uint32_t paletteCompiled[512];
	uint32_t video[FbWidth * FbHeight];
	int16_t audioBuf[MaxSamplesPerFrame];

	uint64_t fnv(uint64_t h, const void *p, size_t n)
	{
		const uint8_t *b = (const uint8_t *)p;
		if (!h) h = 1469598103934665603ULL;
		for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ULL; }
		return h;
	}

	// identical to the driver's, so the gate compares the input path too
	uint8_t padForFrame(long frame)
	{
		uint64_t x = (uint64_t)frame * 6364136223846793005ULL + 1442695040888963407ULL;
		x ^= x >> 33;
		return (uint8_t)(x & 0xFF);
	}

	audio::BandLimitedResampler<MaxSamplesPerFrame> resampler;

	void onSample(void *, uint32_t clock, int delta)
	{
		resampler.AddDelta(clock, delta);
	}

	uint8_t *slurp(const char *path, long *len)
	{
		FILE *f = fopen(path, "rb");
		if (!f) return nullptr;
		fseek(f, 0, SEEK_END);
		*len = ftell(f);
		fseek(f, 0, SEEK_SET);
		auto *b = (uint8_t *)malloc(*len ? (size_t)*len : 1);
		if (fread(b, 1, (size_t)*len, f) != (size_t)*len) { free(b); fclose(f); return nullptr; }
		fclose(f);
		return b;
	}

	struct Domain { const char *name; const void *data; size_t size; };

	std::vector<Domain> domainsOf(nesHawk::NES &nes)
	{
		auto &board = *nes.board;
		std::vector<Domain> d = {
			{ "RAM", nes.ram, 0x800 },
			{ "CIRAM (nametables)", nes.CIRAM, 0x800 },
			{ "PALRAM", nes.ppu->PALRAM, 32 },
			{ "OAM", nes.ppu->OAM, 256 },
		};
		if (!board.Rom.empty()) d.push_back({ "PRG ROM", board.Rom.data(), board.Rom.size() });
		if (!board.Vram.empty()) d.push_back({ board.chrIsRom ? "CHR VROM" : "VRAM", board.Vram.data(), board.Vram.size() });
		if (!board.Wram.empty()) d.push_back({ "WRAM", board.Wram.data(), board.Wram.size() });
		return d;
	}
}

int main(int argc, char **argv)
{
	const char *romPath = nullptr;
	long frames = 60;
	bool blank = false;
	for (int i = 1; i < argc; i++)
	{
		if (!strcmp(argv[i], "--blank")) blank = true;
		else if (!romPath) romPath = argv[i];
		else frames = strtol(argv[i], nullptr, 0);
	}
	if (!romPath)
	{
		fprintf(stderr, "usage: run-native <rom.nes> <frames> [--blank]\n");
		return 2;
	}

	long romLen = 0;
	uint8_t *rom = slurp(romPath, &romLen);
	if (!rom) { fprintf(stderr, "cannot read %s\n", romPath); return 1; }

	nesHawk::NES *nes = nullptr;
	try
	{
		nes = new nesHawk::NES(rom, (size_t)romLen);
	}
	catch (const std::exception &e)
	{
		fprintf(stderr, "cannot load this rom: %s\n", e.what());
		return 1;
	}
	free(rom);

	resampler.SetRates(nes->cpuclockrate, SampleRate);
	nes->sampleCallback = onSample;

	// package defaults: the quickNES palette, no backdrop override, NTSC crop 8..231
	for (int i = 0; i < 64 * 8; i++)
	{
		const int c = i & 63;
		int r = nesPalettes::QuickNESPalette[c][0];
		int g = nesPalettes::QuickNESPalette[c][1];
		int b = nesPalettes::QuickNESPalette[c][2];
		nesPalettes::applyDeemphasis(r, g, b, i >> 6);
		paletteCompiled[i] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
	}
	const int topLine = 8, bottomLine = 231;

	auto domains = domainsOf(*nes);
	printf("domains=%d\n", (int)domains.size());
	for (size_t i = 0; i < domains.size(); i++)
		printf("  [%d] %-20s size=%-8lld writable=1\n", (int)i, domains[i].name, (long long)domains[i].size);

	uint64_t vh = 0, ah = 0;
	long lag = 0;
	for (long f = 0; f < frames; f++)
	{
		nes->FrameAdvance(blank ? 0 : padForFrame(f), 0);

		const int16_t *src = nes->ppu->xbuf;
		for (int y = 0; y < FbHeight; y++)
		{
			const bool rowVisible = y >= topLine && y <= bottomLine;
			for (int x = 0; x < FbWidth; x++)
				video[y * FbWidth + x] = rowVisible ? paletteCompiled[src[(y << 8) + x] & 0x1FF] : 0xFF000000u;
		}

		resampler.EndFrame(nes->apu->sampleclock);
		nes->apu->sampleclock = 0;
		const int samples = resampler.ReadSamples(audioBuf, MaxSamplesPerFrame);

		vh = fnv(vh, video, sizeof video);
		ah = fnv(ah, audioBuf, (size_t)samples * 2);
		if (nes->islag) lag++;
	}

	printf("frames=%ld\n", frames);
	printf("videoHash=%016llx\n", (unsigned long long)vh);
	printf("audioHash=%016llx\n", (unsigned long long)ah);
	printf("lagFrames=%ld\n", lag);
	for (const auto &d : domains)
		printf("domain[%s]=%016llx\n", d.name, (unsigned long long)fnv(0, d.data, d.size));

	return 0;
}
