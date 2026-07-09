// Translation of the controller deck, restricted to what this setup uses: a standard NES
// controller in port 1 and nothing in port 2 (the oracle's ControllerDefinition lists P1
// buttons only).
//   StrobeInfo / ControllerNES / UnpluggedNES / NesDeck / SerialUtil
//     <- src/BizHawk.Emulation.Cores/Consoles/Nintendo/NES/NESControllers.cs
//
// IController is replaced by a plain 8-bit button mask for the current frame; bit order matches
// ControllerNES.Buttons = { "0A", "0B", "0Select", "0Start", "0Up", "0Down", "0Left", "0Right" }
// (bit 0 = A .. bit 7 = Right), which is exactly the order SerialUtil.Latch shifts out.

#pragma once

#include <cstdint>

namespace nesHawk
{

// frame-input button bits, in ControllerNES latch order
enum ButtonBit : uint8_t
{
  BUTTON_A      = 0x01,
  BUTTON_B      = 0x02,
  BUTTON_SELECT = 0x04,
  BUTTON_START  = 0x08,
  BUTTON_UP     = 0x10,
  BUTTON_DOWN   = 0x20,
  BUTTON_LEFT   = 0x40,
  BUTTON_RIGHT  = 0x80,
};

struct StrobeInfo
{
  int OUT0;
  int OUT1;
  int OUT2;
  int OUT0old;
  int OUT1old;
  int OUT2old;

  StrobeInfo(uint8_t oldValue, uint8_t newValue)
  {
    OUT0old = oldValue & 1;
    OUT1old = oldValue >> 1 & 1;
    OUT2old = oldValue >> 2 & 1;
    OUT0 = newValue & 1;
    OUT1 = newValue >> 1 & 1;
    OUT2 = newValue >> 2 & 1;
  }
};

class ControllerNES
{
  public:

  bool _resetting = false;
  int32_t _latchedValue = 0;

  // reset is not edge triggered; so long as it's high, the latch is continuously reloading
  // so we need to latch in two places:
  // 1. when OUT0 goes low, to get the last set
  // 2. when even reading with OUT0 high, since new data for controller is always loading

  void Latch(uint8_t buttons)
  {
    // SerialUtil.Latch: button bits 0-7, then 1s in all remaining bits (endless stream of 1s)
    _latchedValue = (int32_t)(0xFFFFFF00u | buttons);
  }

  void Strobe(const StrobeInfo& s, uint8_t buttons)
  {
    _resetting = s.OUT0 != 0;
    if (s.OUT0 < s.OUT0old)
      Latch(buttons);
  }

  uint8_t Read(uint8_t buttons)
  {
    if (_resetting)
      Latch(buttons);
    uint8_t ret = (uint8_t)(_latchedValue & 1);
    if (!_resetting)
      _latchedValue >>= 1; // ASR not LSR, so endless stream of 1s after data
    return ret;
  }
};

// port 2: UnpluggedNES -- Strobe is a no-op, Read returns 0

class NesDeck
{
  public:

  ControllerNES _left;

  void Strobe(const StrobeInfo& s, uint8_t buttons)
  {
    _left.Strobe(s, buttons);
    // right port unplugged: no-op
  }

  uint8_t ReadA(uint8_t buttons)
  {
    return (uint8_t)(_left.Read(buttons) & 0x19);
  }

  uint8_t ReadB(uint8_t buttons)
  {
    return 0; // UnpluggedNES.Read() & 0x19
  }
};

} // namespace nesHawk
