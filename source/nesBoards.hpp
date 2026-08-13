// Translation of the board layer, restricted to what the Prince of Persia cart uses.
//   NesBoardBase <- src/BizHawk.Emulation.Cores/Consoles/Nintendo/NES/Boards/NesBoardBase.cs
//   UxROM        <- src/BizHawk.Emulation.Cores/Consoles/Nintendo/NES/Boards/UxROM.cs
//
// Cart configuration is hardcoded from the oracle's resolved CartInfo for this ROM
// (BootGod DB entry NES-UNROM-09): BoardType NES-UNROM, PrgSize 128, ChrSize 0, WramSize 0,
// VramSize 8, PadH 1, PadV 0, WramBattery false, System NES-NTSC.
//
// Deliberately dropped relative to the C# source (never taken for this cart): VS-system paths,
// FDS/NSF boards, CDL mapping, InitialRegisterValues, SyncState (savestates come with the
// jaffar integration). Note NesHawk's UxROM models NO PRG bus conflicts (see the C# source's own
// comment) -- fidelity target is NesHawk, so neither do we.

#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

namespace nesHawk
{

class NES; // the board reads NES.DB / NES.ppu / NES.CIRAM; those bodies live in nes.hpp

// resolved cart parameters (CartInfo.cs subset)
struct CartInfo
{
  int PrgSize = 128; // KB
  int ChrSize = 0;   // KB
  int WramSize = 0;  // KB
  int VramSize = 8;  // KB
  int PadH = 1;
  int PadV = 0;
  bool WramBattery = false;
};

enum class EMirrorType
{
  Vertical, Horizontal, OneScreenA, OneScreenB
};

class NesBoardBase
{
  public:

  virtual ~NesBoardBase() = default;

  void Create(NES* nes) { NES_ = nes; }

  virtual void NesSoftReset() {}

  virtual bool Configure() = 0;
  virtual void ClockPpu() {}
  virtual void ClockCpu() {}
  virtual void AtVsyncNmi() {}

  CartInfo Cart;
  NES* NES_ = nullptr;

  virtual void SyncIRQ(bool flag) { IrqSignal = flag; }

  bool IrqSignal = false;

  // state: constant for NROM/UxROM, but AxROM rewrites it on every bank switch
  int _mirroring[4] = {0, 0, 0, 0};

  protected:

  void SetMirroring(int a, int b, int c, int d)
  {
    _mirroring[0] = a;
    _mirroring[1] = b;
    _mirroring[2] = c;
    _mirroring[3] = d;
  }

  public:

  static EMirrorType CalculateMirrorType(int pad_h, int pad_v)
  {
    if (pad_h == 0)
    {
      return pad_v == 0
        ? EMirrorType::OneScreenA
        : EMirrorType::Horizontal;
    }

    if (pad_v == 0)
    {
      return EMirrorType::Vertical;
    }

    return EMirrorType::OneScreenB;
  }

  protected:

  void SetMirrorType(int pad_h, int pad_v) { SetMirrorType(CalculateMirrorType(pad_h, pad_v)); }

  public:

  void SetMirrorType(EMirrorType mirrorType)
  {
    switch (mirrorType)
    {
      case EMirrorType::Horizontal: SetMirroring(0, 0, 1, 1); break;
      case EMirrorType::Vertical: SetMirroring(0, 1, 0, 1); break;
      case EMirrorType::OneScreenA: SetMirroring(0, 0, 0, 0); break;
      case EMirrorType::OneScreenB: SetMirroring(1, 1, 1, 1); break;
      default: SetMirroring(-1, -1, -1, -1); break; //crash!
    }
  }

  protected:

  int ApplyMirroring(int addr) const
  {
    int block = (addr >> 10) & 3;
    block = _mirroring[block];
    int ofs = addr & 0x3FF;
    return (block << 10) | ofs;
  }

  public:

  virtual uint8_t ReadPrg(int addr) { return Rom[addr]; }

  virtual void WritePrg(int addr, uint8_t value) {}

  virtual void WriteWram(int addr, uint8_t value)
  {
    if (!Wram.empty())
    {
      Wram[addr & _wramMask] = value;
    }
  }

  private:

  int _wramMask = 0;

  public:

  virtual void PostConfigure() { _wramMask = (Cart.WramSize * 1024) - 1; }

  virtual uint8_t ReadWram(int addr); // returns NES.DB when no WRAM -> body in nes.hpp

  virtual void WriteExp(int addr, uint8_t value) {}

  virtual uint8_t ReadExp(int addr); // returns NES.DB -> body in nes.hpp

  virtual uint8_t ReadReg2xxx(int addr);  // -> NES.ppu.ReadReg(addr & 7), body in nes.hpp
  virtual uint8_t PeekReg2xxx(int addr);  // -> NES.ppu.PeekReg(addr & 7), body in nes.hpp
  virtual void WriteReg2xxx(int addr, uint8_t value); // -> NES.ppu.WriteReg(addr, value), body in nes.hpp

  virtual void WritePpu(int addr, uint8_t value); // base uses NES.CIRAM -> body in nes.hpp

  virtual void AddressPpu(int addr) {}

  virtual uint8_t PeekPPU(int addr) { return ReadPpu(addr); }

  virtual uint8_t ReadPpu(int addr); // base uses NES.CIRAM -> body in nes.hpp

  virtual uint8_t PeekCart(int addr)
  {
    uint8_t ret;
    if (addr >= 0x8000)
    {
      ret = ReadPrg(addr - 0x8000); // easy optimization, since rom reads are so common, move this up (reordering the rest of these else ifs is not easy)
    }
    else if (addr < 0x6000)
    {
      ret = ReadExp(addr - 0x4000);
    }
    else
    {
      ret = ReadWram(addr - 0x6000);
    }

    return ret;
  }

  std::vector<uint8_t> Wram;
  std::vector<uint8_t> Vram;
  std::vector<uint8_t> Rom;
  std::vector<uint8_t> Vrom;
};

// The translated boards, in ONE class on purpose. BizHawk has a class per board and dispatches
// virtually; here the board is asked on every cpu read of $8000+ (every opcode fetch), so a virtual
// call would cost more than the branch on `kind` does. `final` keeps those calls devirtualised and
// inlined, which is why this reads as three boards wearing one coat:
//   NROM  (mapper 0, NROM.cs)  - fixed PRG, no bank register at all
//   UxROM (mapper 2, UxROM.cs) - 16KB switchable + 16KB fixed, mirroring from the cart
//   AxROM (mapper 7, AxROM.cs) - 32KB switchable, and the same write picks a one-screen nametable
class NesBoard final : public NesBoardBase
{
  public:

  enum class Kind
  {
    NROM,
    UxROM,
    AxROM,
  };

  //configuration
  Kind kind = Kind::UxROM;
  int prg_mask = 0;        // in 16KB pages (UxROM); 32KB pages for AxROM
  int vram_byte_mask = 0;
  bool chrIsRom = false;   // pattern space is CHR ROM (writes ignored)

  //state
  int prg = 0;

  bool Configure() override
  {
    // case "NES-UNROM": AssertPrg(128); AssertChr(0); AssertVram(8);
    //these boards always have 8KB of VRAM
    vram_byte_mask = (Cart.VramSize * 1024) - 1;
    if (kind == Kind::AxROM)
    {
      // AxROM.cs: 32KB pages, and the board powers on showing the first nametable
      prg_mask = (Cart.PrgSize / 32) - 1;
      SetMirrorType(EMirrorType::OneScreenA);
    }
    else
    {
      prg_mask = (Cart.PrgSize / 16) - 1;
      SetMirrorType(Cart.PadH, Cart.PadV);
    }

    return true;
  }

  uint8_t ReadPrg(int addr) override
  {
    if (kind == Kind::AxROM) return Rom[addr | (prg << 15)]; // one 32KB window
    int block = addr >> 14;
    int page = block == 1 ? prg_mask : prg;
    int ofs = addr & 0x3FFF;
    return Rom[(page << 14) | ofs];
  }

  void WritePrg(int addr, uint8_t value) override
  {
    switch (kind)
    {
      case Kind::NROM:
        return; // no bank register: on the real cart these writes do nothing
      case Kind::UxROM:
        prg = value & prg_mask; // adjust_prg is identity for NES-UNROM
        return;
      case Kind::AxROM:
        // one write does both jobs: bank in the low bits, nametable in bit 4. Bus conflicts are
        // off, which is what NesHawk does for plain iNES mapper 7 (only ACCLAIM-AOROM sets them).
        prg = value & prg_mask;
        SetMirrorType((value & 0x10) == 0 ? EMirrorType::OneScreenA : EMirrorType::OneScreenB);
        return;
    }
  }

  uint8_t ReadPpu(int addr) override
  {
    if (addr < 0x2000)
    {
      return Vram[addr & vram_byte_mask];
    }
    else
    {
      return NesBoardBase::ReadPpu(addr);
    }
  }

  void WritePpu(int addr, uint8_t value) override
  {
    if (addr < 0x2000)
    {
      if (chrIsRom == false) Vram[addr & vram_byte_mask] = value;
    }
    else
    {
      NesBoardBase::WritePpu(addr, value);
    }
  }
};

} // namespace nesHawk
