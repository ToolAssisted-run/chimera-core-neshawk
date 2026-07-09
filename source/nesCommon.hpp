// Translation of the small BizHawk.Common helpers the NES core depends on.
//   Bit        <- src/BizHawk.Common/Bit.cs
//   BitReverse <- src/BizHawk.Common/BitReverse.cs
//
// Semantics preserved 1:1: Bit converts implicitly to/from int and bool, so C# expressions like
// `(show_bg << 3) | show_obj` and `!vblank_nmi_gen` translate without source changes.

#pragma once

#include <cstdint>

// NESHAWK_HEADLESS (default ON): skip audio synthesis (pulse/noise/triangle waveform generation,
// sample mixing) and video output (palette lookups, pixel pipeline, xbuf stores). These paths have
// NO feedback into emulation state -- guest RAM, PPU/APU timing, IRQ/NMI and DMA behavior are
// bit-identical either way (regression-gated against the C# oracle). Everything with feedback
// stays: DMC unit (DMA cycle stealing), frame sequencer (IRQs, $4015), sprite evaluation and
// sprite-0 hit. Define NESHAWK_FULL_AV to keep the audio/video paths (e.g. for a future renderer).
#ifndef NESHAWK_FULL_AV
  #define NESHAWK_HEADLESS 1
#endif

// The per-cycle call chain (TickPPU_* -> runppu -> RunCpuOne -> RunOneLast) runs ~90K times per
// frame; GCC declines to inline it on function-size heuristics, paying call/prologue overhead per
// PPU cycle. Each link has a single (or very few) call site(s), so force the chain flat.
#define NESHAWK_HOT_INLINE inline __attribute__((always_inline))

namespace nesHawk
{

struct Bit
{
  uint32_t _val;

  Bit() : _val(0) {}
  Bit(int rhs) : _val((uint32_t)rhs) {}
  Bit(bool rhs) : _val(rhs ? 1u : 0u) {}

  operator int() const { return (int)_val; }
  operator bool() const { return _val != 0; }
};

class BitReverse
{
  public:

  // same table the C# static constructor builds (Byte8[i] = bit-reversed i)
  static const uint8_t* Byte8()
  {
    static const Table t;
    return t.data;
  }

  private:

  struct Table
  {
    uint8_t data[256];
    Table()
    {
      int bits = 8;
      const int n = 1 << 8;
      int m = 1;
      int a = n >> 1;
      int j = 2;
      data[0] = 0;
      data[1] = (uint8_t)a;
      while ((--bits) != 0)
      {
        m <<= 1;
        a >>= 1;
        for (int i = 0; i < m; i++) data[j++] = (uint8_t)(data[i] + a);
      }
    }
  };
};

} // namespace nesHawk
