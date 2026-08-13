// Band-limited step resampler: turns the APU's stream of "the output changed by this much, at this
// CPU cycle" into 44.1kHz samples.
//
// Why not just sample the mixer every ~40 cycles: the APU output is a square-ish signal whose edges
// fall anywhere between two output samples, and those edges carry energy far above the output
// Nyquist. Point-sampling folds it back as audible tones, and the error depends on exactly where an
// edge landed, so it changes with the emulation clock rather than with the music.
//
// So each delta is written into the buffer as a band-limited impulse, in the sub-sample phase the
// delta actually fell at (the kernel, and the treble shaping baked into it, are documented in
// tools/gen-audio-kernel.py); the impulses sum, and reading integrates them once. The integral of a
// band-limited impulse is a band-limited step, which is what the analogue side of a real console
// produces anyway.
//
// The technique is the well-known band-limited step synthesis used by every accurate console
// emulator; this is an independent implementation of it, under this repository's MIT licence, with
// no state that needs allocating - the guest wants it in a fixed buffer it can keep out of
// savestates.
//
// Fixed point everywhere, deliberately: the same audio has to come out of a glibc host build and a
// musl guest build bit for bit, and the equivalence gate compares the two.

#pragma once

#include <cstdint>
#include <cstring>

#include "audioKernel.hpp"

namespace audio
{

/// <summary>
/// Capacity is in output samples; a frame's worth plus room for the impulse tail. Nothing is
/// allocated - place one of these wherever you want it to live.
/// </summary>
template<int Capacity>
class BandLimitedResampler
{
	public:

	// Time is kept in fixed point so a delta lands on a sub-sample phase rather than a sample.
	static constexpr int FracBits = 20;
	static constexpr int PhaseBits = audioKernel::PhaseBits;
	// what is left of the fraction after the phase index: a delta lands BETWEEN two kernel rows and
	// they are interpolated, which is worth about 11dB of noise floor over picking the nearer row
	static constexpr int InterpBits = FracBits - PhaseBits;
	static constexpr int64_t FracUnit = (int64_t)1 << FracBits;

	static_assert(audioKernel::PhaseCount == 1 << PhaseBits, "phase table size must be a power of two");
	static_assert(InterpBits > 0, "not enough fixed-point bits left to interpolate phases");

	// How fast the integrator leaks, as a right shift per sample. This is the DC blocker: the NES
	// mixer output never goes negative, so without it the integrator would settle at a large offset
	// and everything would clip. A shift of 9 puts the corner around 14Hz at 44.1kHz - below
	// hearing, which is the point of a DC blocker.
	static constexpr int BassShift = 9;

	/// <summary>Clocks per second of the source (the CPU) and the output sample rate.</summary>
	void SetRates(double clockRate, double sampleRate)
	{
		_factor = (int64_t)((sampleRate / clockRate) * (double)FracUnit + 0.5);
		Clear();
	}

	void Clear()
	{
		memset(_buf, 0, sizeof _buf);
		_offset = 0;
		_integrator = 0;
	}

	/// <summary>The output changed by <paramref name="delta"/> at clock <paramref name="time"/>,
	/// counted from the last EndFrame.</summary>
	void AddDelta(uint32_t time, int delta)
	{
		const int64_t position = _offset + (int64_t)time * _factor;
		const int index = (int)(position >> FracBits);
		const int phase = (int)((position >> (FracBits - PhaseBits)) & (audioKernel::PhaseCount - 1));
		if (index < 0 || index > Capacity) return; // a frame longer than the buffer; nothing sane to do

		const int64_t interp = position & ((1 << InterpBits) - 1);
		const int32_t *k0 = audioKernel::Impulse[phase];
		const int32_t *k1 = audioKernel::Impulse[phase + 1];
		int64_t *out = _buf + index;
		for (int i = 0; i < audioKernel::Width; i++)
		{
			const int64_t tap = k0[i] + (((int64_t)(k1[i] - k0[i]) * interp) >> InterpBits);
			out[i] += tap * delta;
		}
	}

	/// <summary>Ends the frame at clock <paramref name="time"/>; samples up to there are readable.
	/// The leftover fraction of a sample carries into the next frame, so no time is lost.</summary>
	void EndFrame(uint32_t time)
	{
		_offset += (int64_t)time * _factor;
	}

	int SamplesAvailable() const
	{
		const int n = (int)(_offset >> FracBits);
		return n < Capacity ? n : Capacity;
	}

	/// <summary>Integrates and drains up to <paramref name="max"/> samples. Returns how many.</summary>
	int ReadSamples(int16_t *out, int max)
	{
		int count = SamplesAvailable();
		if (count > max) count = max;

		for (int i = 0; i < count; i++)
		{
			_integrator += _buf[i];
			int64_t sample = _integrator >> audioKernel::UnitShift;
			_integrator -= _integrator >> BassShift;
			if (sample > 32767) sample = 32767;
			else if (sample < -32768) sample = -32768;
			out[i] = (int16_t)sample;
		}

		// Shift the buffer down. The tail beyond the frame boundary holds the part of the last
		// impulses that belongs to the next frame, so it moves rather than being dropped.
		const int keep = Capacity + audioKernel::Width - count;
		memmove(_buf, _buf + count, (size_t)keep * sizeof _buf[0]);
		memset(_buf + keep, 0, (size_t)count * sizeof _buf[0]);
		_offset -= (int64_t)count << FracBits;
		return count;
	}

	private:

	int64_t _buf[Capacity + audioKernel::Width] = {};
	int64_t _factor = 0;
	int64_t _offset = 0;
	int64_t _integrator = 0;
};

} // namespace audio
