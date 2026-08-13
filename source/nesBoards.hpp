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

  /// True if this board counts PPU clocks; the PPU only calls ClockPpu when it does.
  virtual bool WantsPpuClock() const { return false; }
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

  /// NesBoardBase.HandleNormalPRGConflict: on a board without a latch, the value the cpu writes is
  /// ANDed with whatever the rom is driving on the same address.
  uint8_t HandleNormalPRGConflict(int addr, uint8_t value) { return (uint8_t)(value & ReadPrg(addr)); }

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
//   NROM  (mapper 0,  NROM.cs)                 - fixed PRG, no bank register at all
//   UxROM (mapper 2,  UxROM.cs)                - 16KB switchable + 16KB fixed
//   CNROM (mapper 3,  CNROM.cs)                - fixed PRG, 8KB CHR banking, bus conflicts
//   AxROM (mapper 7,  AxROM.cs)                - 32KB switchable + one-screen nametable select
//   B74x  (mapper 70, BANDAI_74_161_161_32.cs) - 16KB PRG and 8KB CHR from one write
//   GxROM (mapper 66, GxROM.cs)                - 32KB PRG and 8KB CHR from one write
//   SxROM (mapper 1,  SxROM.cs)                - MMC1: five writes shift into one register
class NesBoard final : public NesBoardBase
{
  public:

  enum class Kind
  {
    NROM,
    UxROM,
    CNROM,
    AxROM,
    B74x,
    GxROM,
    SxROM,
  };

  /// MMC1 (SxROM.cs). The cpu writes one bit at a time: five writes fill a shift register and the
  /// address bits 13-14 of the fifth say which of the four registers it lands in. A write arriving
  /// within four PPU clocks of the last is ignored - which is why the board wants ClockPpu.
  struct MMC1
  {
    static constexpr uint32_t PpuTimeout = 4; // "i don't know if this is right, but anything lower will not boot Bill & Ted"

    int shift_count = 0, shift_val = 0;
    int chr_mode = 1, prg_mode = 1, prg_slot = 1;
    int chr_0 = 0, chr_1 = 0, prg = 0;
    bool wram_disable = false;
    EMirrorType mirror = EMirrorType::Horizontal;
    uint32_t ppuclock = 0;

    int chr_banks_4k[2] = {0, 0};
    int prg_banks_16k[2] = {0, 0};

    MMC1() { StandardReset(); }

    void SyncCHR()
    {
      if (chr_mode == 0)
      {
        chr_banks_4k[0] = chr_0 & ~1;
        chr_banks_4k[1] = (chr_0 & ~1) + 1;
      }
      else
      {
        chr_banks_4k[0] = chr_0;
        chr_banks_4k[1] = chr_1;
      }
    }

    void SyncPRG()
    {
      if (prg_mode == 0)
      {
        //switch 32kb
        prg_banks_16k[0] = prg & ~1;
        prg_banks_16k[1] = (prg & ~1) + 1;
      }
      else if (prg_slot == 0)
      {
        //switch 16KB at $C000
        prg_banks_16k[0] = 0x00;
        prg_banks_16k[1] = prg;
      }
      else
      {
        //switch 16KB at $8000
        prg_banks_16k[0] = prg;
        prg_banks_16k[1] = 0x0F;
      }
    }

    void SerialReset()
    {
      prg_mode = 1;
      prg_slot = 1;
    }

    void StandardReset()
    {
      prg_mode = 1;
      prg_slot = 1;
      chr_mode = 1;
      shift_count = 0;
      shift_val = 0;
      SerialReset();
      mirror = EMirrorType::Horizontal;
      SyncCHR();
      SyncPRG();
    }

    void SerialWriteRegister(int addr, int value)
    {
      static const EMirrorType mirrorTypes[4] = {
        EMirrorType::OneScreenA, EMirrorType::OneScreenB, EMirrorType::Vertical, EMirrorType::Horizontal
      };
      switch (addr)
      {
        case 0: //8000-9FFF
          mirror = mirrorTypes[value & 3];
          prg_slot = (value >> 2) & 1;
          prg_mode = (value >> 3) & 1;
          chr_mode = (value >> 4) & 1;
          break;
        case 1: //A000-BFFF
          chr_0 = value & 0x1F;
          break;
        case 2: //C000-DFFF
          chr_1 = value & 0x1F;
          break;
        case 3: //E000-FFFF
          prg = value & 0xF;
          wram_disable = ((value >> 4) & 1) != 0;
          break;
      }
    }

    void Write(int addr, uint8_t value)
    {
      // MMC1_SerialController.Write
      const int data = value & 1;
      if (((value >> 7) & 1) != 0)
      {
        shift_count = 0;
        shift_val = 0;
        SerialReset();
      }
      else
      {
        shift_val >>= 1;
        shift_val |= data << 4;
        shift_count++;
        if (shift_count == 5)
        {
          SerialWriteRegister(addr >> 13, shift_val);
          shift_count = 0;
          shift_val = 0;
        }
      }
      SyncCHR();
      SyncPRG();
    }

    int PrgBank(int addr) const { return prg_banks_16k[addr >> 14]; }

    int ChrBank4k(int addr) const { return chr_banks_4k[(addr >> 12) & 1]; }
  };

  //configuration
  Kind kind = Kind::UxROM;
  int prg_mask = 0;        // in 16KB pages; 32KB pages for AxROM/GxROM
  int chr_mask = 0;        // in 8KB pages
  int prg_byte_mask = 0;   // CNROM addresses its fixed PRG by byte, as NROM does in the C#
  int vram_byte_mask = 0;
  bool bus_conflict = false;

  //state
  int prg = 0;
  int chr = 0;
  MMC1 mmc1;               // SxROM only
  int chr_bank_mask = 0;   // in 4KB pages, for MMC1
  int vram_mask = 0;

  bool Configure() override
  {
    // case "NES-UNROM": AssertPrg(128); AssertChr(0); AssertVram(8);
    //these boards always have 8KB of VRAM
    vram_byte_mask = (Cart.VramSize * 1024) - 1;
    prg_byte_mask = (Cart.PrgSize * 1024) - 1;
    chr_mask = Cart.ChrSize != 0 ? (Cart.ChrSize / 8) - 1 : 0;
    switch (kind)
    {
      case Kind::AxROM:
      case Kind::GxROM:
        prg_mask = (Cart.PrgSize / 32) - 1;
        break;
      default:
        prg_mask = (Cart.PrgSize / 16) - 1;
        break;
    }
    if (kind == Kind::AxROM)
    {
      // AxROM.cs: the board powers on showing the first nametable
      SetMirrorType(EMirrorType::OneScreenA);
    }
    else
    {
      SetMirrorType(Cart.PadH, Cart.PadV);
    }
    // CNROM's conflicts are per board type in the C#; iNES mapper 3 alone cannot tell them apart,
    // and the generic MAPPER003 case is the one that says no conflicts.
    bus_conflict = false;

    // SxROM.cs Configure: PRG in 16KB pages, CHR in 4KB pages, and VRAM (when the cart has CHR RAM
    // instead of CHR ROM) masked by its own size
    chr_bank_mask = Cart.ChrSize != 0 ? (Cart.ChrSize / 4) - 1 : 0;
    vram_mask = Cart.VramSize != 0 ? (Cart.VramSize * 1024) - 1 : 0;

    return true;
  }

  /// SxROM counts PPU clocks to reject writes that arrive too close together.
  bool WantsPpuClock() const override { return kind == Kind::SxROM; }

  void ClockPpu() override
  {
    if (mmc1.ppuclock < MMC1::PpuTimeout) mmc1.ppuclock++;
  }

  uint8_t ReadPrg(int addr) override
  {
    switch (kind)
    {
      case Kind::NROM:
      case Kind::CNROM:
        return Rom[addr & prg_byte_mask];
      case Kind::AxROM:
        return Rom[addr | (prg << 15)];     // one 32KB window
      case Kind::GxROM:
        return Rom[addr + (prg << 15)];
      case Kind::SxROM:
        return Rom[((mmc1.PrgBank(addr) & prg_mask) << 14) | (addr & 0x3FFF)];
      default:
      {
        int block = addr >> 14;
        int page = block == 1 ? prg_mask : prg;
        int ofs = addr & 0x3FFF;
        return Rom[(page << 14) | ofs];
      }
    }
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
      case Kind::CNROM:
        if (bus_conflict) value = HandleNormalPRGConflict(addr, value);
        chr = value & chr_mask;
        return;
      case Kind::AxROM:
        // one write does both jobs: bank in the low bits, nametable in bit 4. Bus conflicts are
        // off, which is what NesHawk does for plain iNES mapper 7 (only ACCLAIM-AOROM sets them).
        prg = value & prg_mask;
        SetMirrorType((value & 0x10) == 0 ? EMirrorType::OneScreenA : EMirrorType::OneScreenB);
        return;
      case Kind::B74x:
        prg = ((value >> 4) & 15) & prg_mask;
        chr = value & 15;
        return;
      case Kind::GxROM:
        chr = (value & 7) & chr_mask;
        prg = ((value >> 4) & 3) & prg_mask;
        return;
      case Kind::SxROM:
        // SxROM.WritePrg: writes closer together than four PPU clocks never reach the chip
        if (mmc1.ppuclock >= MMC1::PpuTimeout)
        {
          mmc1.ppuclock = 0;
          mmc1.Write(addr, value);
          SetMirrorType(mmc1.mirror); // often redundant, but gets the job done
        }
        return;
    }
  }

  uint8_t ReadPpu(int addr) override
  {
    if (addr < 0x2000)
    {
      if (kind == Kind::SxROM)
      {
        const int banked = ((mmc1.ChrBank4k(addr) & chr_bank_mask) << 12) | (addr & 0x0FFF);
        return Cart.VramSize != 0 ? Vram[banked & vram_mask] : Vrom[banked];
      }
      if (Vrom.empty()) return Vram[addr & vram_byte_mask];
      switch (kind)
      {
        case Kind::CNROM:
        case Kind::GxROM:
          return Vrom[addr + (chr << 13)];
        case Kind::B74x:
          return Vrom[(addr & 0x1FFF) + (chr * 0x2000)];
        default:
          return Vrom[addr];
      }
    }

    return NesBoardBase::ReadPpu(addr);
  }

  void WritePpu(int addr, uint8_t value) override
  {
    if (addr < 0x2000)
    {
      if (kind == Kind::SxROM)
      {
        // SxROM.WritePpu: CHR RAM is banked too; a CHR ROM cart ignores the write
        if (Cart.VramSize != 0)
        {
          const int banked = ((mmc1.ChrBank4k(addr) & chr_bank_mask) << 12) | (addr & 0x0FFF);
          Vram[banked & vram_mask] = value;
        }
      }
      else if (Vrom.empty()) Vram[addr & vram_byte_mask] = value; // CHR ROM ignores writes
    }
    else
    {
      NesBoardBase::WritePpu(addr, value);
    }
  }
};

} // namespace nesHawk
