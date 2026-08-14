/* Standalone driver for the waterboxed QuickerNesHawk core: runs core.wbx through the miniBox host
 * over a rom and reports per-frame video/audio/RAM digests, so the sandboxed build can be compared
 * against the same core built natively on the same inputs.
 *
 * usage: run-wbx <core.wbx> <rom.nes> <frames> [--rerecord] [--blank] [--settings <file.json>]
 *                 [--saveram-out <file>] [--saveram-in <file>]
 *
 * --rerecord round-trips the WHOLE guest machine through the host's save/load state around every
 *   frame; the digests must be identical either way.
 * --blank holds every button released, matching run-native --blank.
 * NESHAWK_FDS_BIOS, when set, is mounted as the "bios" firmware file the package declares - the
 * same channel the frontend uses, so a disk image runs here too.
 *
 * --settings mounts a settings JSON exactly as the frontend does, so a sync setting can be
 *   exercised without one.
 */
#include "minibox.h"
/* guest entry points are sysv64 even on a win64 host (MB_GUEST_ABI is a no-op
 * on Linux); without this the driver cannot call a guest on Windows at all */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* FNV-1a: a digest is all we need to compare two implementations frame by frame */
static uint64_t fnv(uint64_t h, const void *p, size_t n)
{
	const uint8_t *b = (const uint8_t *)p;
	if (!h) h = 1469598103934665603ULL;
	for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ULL; }
	return h;
}

typedef struct { FILE *f; } freader;
static intptr_t file_read(uintptr_t ud, uint8_t *d, uintptr_t s) { return (intptr_t)fread(d, 1, s, ((freader *)ud)->f); }
typedef struct { const uint8_t *p; size_t n, pos; } memreader;
static intptr_t mem_reader(uintptr_t ud, uint8_t *d, uintptr_t s)
{
	memreader *m = (memreader *)ud;
	size_t take = s < (m->n - m->pos) ? s : (m->n - m->pos);
	memcpy(d, m->p + m->pos, take); m->pos += take; return (intptr_t)take;
}
typedef struct { uint8_t *b; size_t len, cap, pos; } membuf;
static int32_t mem_write(uintptr_t ud, const uint8_t *d, uintptr_t n)
{
	membuf *m = (membuf *)ud;
	if (m->len + n > m->cap) { m->cap = (m->len + n) * 2 + 64; m->b = realloc(m->b, m->cap); }
	memcpy(m->b + m->len, d, n); m->len += n; return 0;
}
static intptr_t mem_read(uintptr_t ud, uint8_t *d, uintptr_t n)
{
	membuf *m = (membuf *)ud;
	uintptr_t avail = m->len - m->pos; if (n > avail) n = avail;
	memcpy(d, m->b + m->pos, n); m->pos += n; return (intptr_t)n;
}

/* A deterministic per-frame button pattern, identical in both drivers, so the
 * gate compares the INPUT path too rather than 300 frames of nothing pressed.
 * Bit order is the NES joypad's own: A,B,Select,Start,Up,Down,Left,Right. */
static uint8_t padForFrame(long frame)
{
	uint64_t x = (uint64_t)frame * 6364136223846793005ULL + 1442695040888963407ULL;
	x ^= x >> 33;
	return (uint8_t)(x & 0xFF);
}

typedef int (MB_GUEST_ABI *intfn)(void);
typedef void (MB_GUEST_ABI *framefn)(uint64_t);
typedef uintptr_t (MB_GUEST_ABI *ptrfn)(void);
typedef uintptr_t (MB_GUEST_ABI *ptrfn_i)(int);
typedef int (MB_GUEST_ABI *intfn_i)(int);
typedef int64_t (MB_GUEST_ABI *i64fn_i)(int);

/* zero when the guest does not export it: an optional ABI group is absent, not broken */
static uintptr_t tryproc(mb_host *h, const char *n)
{
	mb_return r; wbx_get_proc_addr(h, n, &r);
	if (r.error_message[0]) { fprintf(stderr, "proc %s: %s\n", n, r.error_message); exit(2); }
	return r.data;
}

static uintptr_t proc(mb_host *h, const char *n)
{
	uintptr_t p = tryproc(h, n);
	if (!p) { fprintf(stderr, "missing required export %s\n", n); exit(2); }
	return p;
}

static uint8_t *slurp(const char *p, long *n)
{
	FILE *f = fopen(p, "rb"); if (!f) return 0;
	fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
	uint8_t *b = malloc(*n ? *n : 1);
	if (fread(b, 1, *n, f) != (size_t)*n) { free(b); fclose(f); return 0; }
	fclose(f); return b;
}

int main(int argc, char **argv)
{
	const char *wbxPath = 0, *romPath = 0, *settingsPath = 0, *sramOut = 0, *sramIn = 0;
	long frames = 60; int rerecord = 0, blank = 0;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--rerecord")) rerecord = 1;
		else if (!strcmp(argv[i], "--blank")) blank = 1;
		else if (!strcmp(argv[i], "--settings") && i + 1 < argc) settingsPath = argv[++i];
		else if (!strcmp(argv[i], "--saveram-out") && i + 1 < argc) sramOut = argv[++i];
		else if (!strcmp(argv[i], "--saveram-in") && i + 1 < argc) sramIn = argv[++i];
		else if (!wbxPath) wbxPath = argv[i];
		else if (!romPath) romPath = argv[i];
		else frames = strtol(argv[i], 0, 0);
	}
	if (!wbxPath || !romPath) { fprintf(stderr, "usage: run-wbx <core.wbx> <rom.nes> <frames> [--rerecord] [--blank] [--settings <file.json>]\n"); return 2; }

	long romLen = 0;
	uint8_t *rom = slurp(romPath, &romLen);
	if (!rom) { fprintf(stderr, "cannot read %s\n", romPath); return 1; }
	FILE *wf = fopen(wbxPath, "rb");
	if (!wf) { fprintf(stderr, "cannot open %s\n", wbxPath); return 1; }

	/* Sized to the core's needs (see waterbox.config): a savestate costs work
	 * proportional to the declared layout, so an oversized one is pure per-frame cost. */
	mb_memory_layout_template layout = { 16u << 20, 16u << 20, 8u << 20, 16u << 20, 16u << 20 };
	freader fr = { wf };
	mb_return r;
	wbx_create_host(&layout, "core.wbx", file_read, (uintptr_t)&fr, &r);
	fclose(wf);
	if (r.error_message[0]) { fprintf(stderr, "create: %s\n", r.error_message); return 1; }
	mb_host *h = (mb_host *)r.data;

	memreader mr = { rom, (size_t)romLen, 0 };
	wbx_mount_file(h, "rom", mem_reader, (uintptr_t)&mr, false, &r);
	if (r.error_message[0]) { fprintf(stderr, "mount: %s\n", r.error_message); return 1; }

	/* The frontend always mounts "settings", empty when the user changed nothing; the guest reads
	 * it during Init. */
	long settingsLen = 0;
	uint8_t *settings = settingsPath ? slurp(settingsPath, &settingsLen) : 0;
	if (settingsPath && !settings) { fprintf(stderr, "cannot read %s\n", settingsPath); return 1; }
	memreader sr = { settings, (size_t)settingsLen, 0 };
	wbx_mount_file(h, "settings", mem_reader, (uintptr_t)&sr, false, &r);
	if (r.error_message[0]) { fprintf(stderr, "mount settings: %s\n", r.error_message); return 1; }

	/* Firmware, mounted under the id the package declares. Only mounted when the user has it: an
	 * absent file must look absent to the guest, not empty. */
	const char *biosPath = getenv("NESHAWK_FDS_BIOS");
	long biosLen = 0;
	uint8_t *bios = biosPath ? slurp(biosPath, &biosLen) : 0;
	memreader br = { bios, (size_t)biosLen, 0 };
	if (bios)
	{
		wbx_mount_file(h, "bios", mem_reader, (uintptr_t)&br, false, &r);
		if (r.error_message[0]) { fprintf(stderr, "mount bios: %s\n", r.error_message); return 1; }
	}

	wbx_activate_host(h, &r);
	intfn Init = (intfn)proc(h, "Init");
	if (Init() != 1) { fprintf(stderr, "Init failed (bad rom?)\n"); return 1; }

	/* save files: the optional guest group. --saveram-in is applied before the first frame, exactly
	 * where the frontend applies a .SaveRAM file; --saveram-out is written after the last one. */
	intfn SaveRamSize = (intfn)tryproc(h, "GetSaveRamSize");
	ptrfn GetSaveRam = (ptrfn)tryproc(h, "GetSaveRam");
	ptrfn_i GetSaveRamBuffer = (ptrfn_i)tryproc(h, "GetSaveRamBuffer");
	intfn_i PutSaveRam = (intfn_i)tryproc(h, "PutSaveRam");
	if (sramIn) {
		if (!GetSaveRamBuffer || !PutSaveRam) { fprintf(stderr, "core has no save file support\n"); return 1; }
		long n = 0;
		uint8_t *data = slurp(sramIn, &n);
		if (!data) { fprintf(stderr, "cannot read %s\n", sramIn); return 1; }
		void *dst = (void *)GetSaveRamBuffer((int)n);
		if (!dst) { fprintf(stderr, "core would not give a %ld byte save buffer\n", n); return 1; }
		memcpy(dst, data, (size_t)n);
		if (!PutSaveRam((int)n)) { fprintf(stderr, "core refused the save file\n"); return 1; }
		free(data);
	}

	framefn FrameAdvance = (framefn)proc(h, "FrameAdvance");
	ptrfn GetVideoBgra = (ptrfn)proc(h, "GetVideoBgra");
	ptrfn GetAudio = (ptrfn)proc(h, "GetAudio");
	intfn GetAudioSampleCount = (intfn)proc(h, "GetAudioSampleCount");
	intfn InputWasRead = (intfn)proc(h, "InputWasRead");
	intfn GetMemoryDomainCount = (intfn)proc(h, "GetMemoryDomainCount");
	ptrfn_i GetMemoryDomainName = (ptrfn_i)proc(h, "GetMemoryDomainName");
	ptrfn_i GetMemoryDomainPtr = (ptrfn_i)proc(h, "GetMemoryDomainPtr");
	i64fn_i GetMemoryDomainSize = (i64fn_i)proc(h, "GetMemoryDomainSize");
	intfn_i GetMemoryDomainWritable = (intfn_i)proc(h, "GetMemoryDomainWritable");

	/* the guest self-describes its domains only after Init - they depend on the cart */
	int nd = GetMemoryDomainCount();
	printf("domains=%d\n", nd);
	for (int i = 0; i < nd; i++) {
		printf("  [%d] %-20s size=%-8lld writable=%d\n", i,
			(const char *)GetMemoryDomainName(i), (long long)GetMemoryDomainSize(i),
			GetMemoryDomainWritable(i));
	}

	wbx_deactivate_host(h, &r);
	wbx_seal(h, &r);
	if (r.error_message[0]) { fprintf(stderr, "seal: %s\n", r.error_message); return 1; }
	wbx_activate_host(h, &r);

	uint64_t vh = 0, ah = 0;
	long lag = 0;
	membuf st = {0};
	for (long f = 0; f < frames; f++) {
		if (rerecord) {
			/* No deactivate/activate bracket: the host activates itself for the
			 * duration and restores what it found. Bracketing it unmaps and remaps
			 * the whole guest arena four times per frame, which for a rerecord
			 * replay costs far more than the state itself. */
			st.len = 0;
			wbx_save_state(h, mem_write, (uintptr_t)&st, &r);
			st.pos = 0;
			wbx_load_state(h, mem_read, (uintptr_t)&st, &r);
			if (r.error_message[0]) { fprintf(stderr, "rerecord: %s\n", r.error_message); return 1; }
		}
		/* the guest does its own packing from this mask - see waterbox.cpp */
		FrameAdvance(blank ? 0 : (uint64_t)padForFrame(f));
		vh = fnv(vh, (const void *)GetVideoBgra(), 256 * 240 * 4);
		ah = fnv(ah, (const void *)GetAudio(), (size_t)GetAudioSampleCount() * 2);
		/* lag is emulation-visible state too: a build with joypad-read detection
		 * compiled out reports every frame as lag, and RAM alone never notices */
		if (!InputWasRead()) lag++;
	}

	printf("frames=%ld\n", frames);
	printf("videoHash=%016llx\n", (unsigned long long)vh);
	printf("audioHash=%016llx\n", (unsigned long long)ah);
	printf("lagFrames=%ld\n", lag);
	for (int i = 0; i < nd; i++) {
		/* the domain list may have holes - a board with no PRG ROM (the disk system) leaves that
		 * index empty - and an absent domain is not a domain, so it is not reported */
		const char *dname = (const char *)GetMemoryDomainName(i);
		if (!dname) continue;
		uint64_t dh = fnv(0, (const void *)GetMemoryDomainPtr(i), (size_t)GetMemoryDomainSize(i));
		printf("domain[%s]=%016llx\n", dname, (unsigned long long)dh);
	}

	if (sramOut) {
		int n = SaveRamSize ? SaveRamSize() : 0;
		const void *src = (n > 0 && GetSaveRam) ? (const void *)GetSaveRam() : 0;
		FILE *f = fopen(sramOut, "wb");
		if (!f) { fprintf(stderr, "cannot write %s\n", sramOut); return 1; }
		if (src) fwrite(src, 1, (size_t)n, f);
		fclose(f);
		printf("saveRamBytes=%d\n", src ? n : 0);
	}

	wbx_deactivate_host(h, &r); wbx_destroy_host(h, &r);
	free(rom); free(st.b);
	return 0;
}
