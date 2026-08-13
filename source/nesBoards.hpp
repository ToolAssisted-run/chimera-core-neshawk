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
//   TxROM (mapper 4,  TxROM.cs + MMC3.cs)      - MMC3: eight bank registers and the A12 scanline IRQ
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
    TxROM,
  };

  /// MMC3 (MMC3.cs + TxROM.cs). Eight bank registers written through a two-address protocol, and a
  /// scanline counter that watches PPU address line A12 rise - which is why the board wants both
  /// AddressPpu and ClockPpu. The IRQ it raises is how these games split the screen.
  struct MMC3
  {
    int reg_addr = 0;
    bool chr_mode = false, prg_mode = false;
    uint8_t regs[8] = { 0, 2, 4, 5, 6, 7, 0, 1 };
    uint8_t mirror = 0;
    int a12_old = 0;
    uint8_t irq_reload = 0, irq_counter = 0;
    bool irq_pending = false, irq_enable = false, irq_reload_flag = false;
    bool wram_enable = false, wram_write_protect = false;
    bool just_cleared_pending = false, just_cleared = false;
    int separator_counter = 0;
    int irq_countdown = 0;
    uint8_t cmd = 0;
    int MirrorMask = 1;
    uint8_t chr_regs_1k[8] = {};
    uint8_t prg_regs_8k[4] = {};

    /// MMC3.cs picks the chip revision from the cart database's chip list and falls back to MMC3C;
    /// with no database this is always that fallback, and MMC3C is the newer IRQ behaviour.
    static constexpr bool oldIrqType = false;

    EMirrorType MirrorType() const
    {
      switch (mirror)
      {
        case 1: return EMirrorType::Horizontal;
        case 2: return EMirrorType::OneScreenA;
        case 3: return EMirrorType::OneScreenB;
        default: return EMirrorType::Vertical;
      }
    }

    void Sync()
    {
      if (prg_mode)
      {
        prg_regs_8k[0] = 0xFE;
        prg_regs_8k[1] = regs[7];
        prg_regs_8k[2] = regs[6];
        prg_regs_8k[3] = 0xFF;
      }
      else
      {
        prg_regs_8k[0] = regs[6];
        prg_regs_8k[1] = regs[7];
        prg_regs_8k[2] = 0xFE;
        prg_regs_8k[3] = 0xFF;
      }
      const uint8_t r0_0 = (uint8_t)(regs[0] & ~1);
      const uint8_t r0_1 = (uint8_t)(regs[0] | 1);
      const uint8_t r1_0 = (uint8_t)(regs[1] & ~1);
      const uint8_t r1_1 = (uint8_t)(regs[1] | 1);
      if (chr_mode)
      {
        chr_regs_1k[0] = regs[2]; chr_regs_1k[1] = regs[3];
        chr_regs_1k[2] = regs[4]; chr_regs_1k[3] = regs[5];
        chr_regs_1k[4] = r0_0;    chr_regs_1k[5] = r0_1;
        chr_regs_1k[6] = r1_0;    chr_regs_1k[7] = r1_1;
      }
      else
      {
        chr_regs_1k[0] = r0_0;    chr_regs_1k[1] = r0_1;
        chr_regs_1k[2] = r1_0;    chr_regs_1k[3] = r1_1;
        chr_regs_1k[4] = regs[2]; chr_regs_1k[5] = regs[3];
        chr_regs_1k[6] = regs[4]; chr_regs_1k[7] = regs[5];
      }
    }

    MMC3() { Sync(); }

    int PrgBank8k(int addr) const { return prg_regs_8k[addr >> 13]; }

    int ChrBank1k(int addr) const { return chr_regs_1k[addr >> 10]; }
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
  MMC3 mmc3;               // TxROM only
  int chr_bank_mask = 0;   // in 4KB pages, for MMC1
  int chr_bank_mask_1k = 0;// in 1KB pages, for MMC3
  int prg_bank_mask_8k = 0;// in 8KB pages, for MMC3
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

    // MMC3Board_Base.BaseSetup: PRG in 8KB banks, CHR in 1KB banks (or VRAM's own size on a
    // CHR-RAM board), and the chip powers up vertically mirrored
    prg_bank_mask_8k = (Cart.PrgSize / 8) - 1;
    chr_bank_mask_1k = (Cart.ChrSize != 0 ? Cart.ChrSize : Cart.VramSize) - 1;
    if (kind == Kind::TxROM) SetMirrorType(EMirrorType::Vertical);

    return true;
  }

  /// SxROM counts PPU clocks to reject writes that arrive too close together; MMC3 counts them to
  /// time its scanline IRQ.
  bool WantsPpuClock() const override { return kind == Kind::SxROM || kind == Kind::TxROM; }

  void ClockPpu() override
  {
    if (kind == Kind::SxROM)
    {
      if (mmc1.ppuclock < MMC1::PpuTimeout) mmc1.ppuclock++;
      return;
    }
    // MMC3.ClockPPU
    if (mmc3.separator_counter > 0) mmc3.separator_counter--;
    if (mmc3.irq_countdown > 0)
    {
      mmc3.irq_countdown--;
      if (mmc3.irq_countdown == 0) ClockMmc3Irq();
    }
    if (mmc3.just_cleared)
    {
      mmc3.irq_counter = 0;
      if (MMC3::oldIrqType) mmc3.irq_reload_flag = true;
    }
    mmc3.just_cleared = mmc3.just_cleared_pending;
    mmc3.just_cleared_pending = false;
  }

  /// MMC3.AddressPPU: the counter is clocked by A12 going high, with a filter that ignores rises
  /// closer together than 15 PPU cycles. MMC3 cannot see the internal pattern tables (fixes Recca).
  void AddressPpu(int addr) override
  {
    if (kind != Kind::TxROM || addr >= 0x3F00) return;
    const int a12 = (addr >> 12) & 1;
    if (a12 == 1 && mmc3.a12_old == 0)
    {
      if (mmc3.separator_counter > 0)
      {
        mmc3.separator_counter = 15;
      }
      else
      {
        mmc3.separator_counter = 15;
        mmc3.irq_countdown = 5;
      }
    }
    mmc3.a12_old = a12;
  }

  /// MMC3.ClockIRQ + IRQ_EQ_Pass
  void ClockMmc3Irq()
  {
    const int last_irq_counter = mmc3.irq_counter;
    if (mmc3.irq_reload_flag || mmc3.irq_counter == 0) mmc3.irq_counter = mmc3.irq_reload;
    else mmc3.irq_counter--;

    if (mmc3.irq_counter == 0)
    {
      const bool pass = MMC3::oldIrqType ? (last_irq_counter != 0 || mmc3.irq_reload_flag) : true;
      if (pass)
      {
        if (mmc3.irq_enable) mmc3.irq_pending = true;
        SyncIRQ(mmc3.irq_pending);
      }
    }
    mmc3.irq_reload_flag = false;
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
      case Kind::TxROM:
        return Rom[((mmc3.PrgBank8k(addr) & prg_bank_mask_8k) << 13) | (addr & 0x1FFF)];
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
      case Kind::TxROM:
        WriteMmc3(addr, value);
        SetMirrorType(mmc3.MirrorType()); // often redundant, but gets the job done
        return;
    }
  }

  /// MMC3.WritePRG: the register pair at each address decodes from bits 13-14 and bit 0.
  void WriteMmc3(int addr, uint8_t value)
  {
    switch (addr & 0x6001)
    {
      case 0x0000: //$8000
        mmc3.cmd = value;
        mmc3.chr_mode = ((value >> 7) & 1) != 0;
        mmc3.prg_mode = ((value >> 6) & 1) != 0;
        mmc3.reg_addr = value & 7;
        mmc3.Sync();
        break;
      case 0x0001: //$8001
        mmc3.regs[mmc3.reg_addr] = value;
        mmc3.Sync();
        break;
      case 0x2000: //$A000
        mmc3.mirror = (uint8_t)(value & mmc3.MirrorMask);
        SetMirrorType(mmc3.MirrorType());
        break;
      case 0x2001: //$A001
        mmc3.wram_write_protect = ((value >> 6) & 1) != 0;
        mmc3.wram_enable = ((value >> 7) & 1) != 0;
        break;
      case 0x4000: //$C000 - IRQ reload value
        mmc3.irq_reload = value;
        break;
      case 0x4001: //$C001 - IRQ clear; does not take immediate effect (fixes Klax)
        mmc3.just_cleared_pending = true;
        break;
      case 0x6000: //$E000 - IRQ acknowledge / disable
        mmc3.irq_enable = false;
        mmc3.irq_pending = false;
        SyncIRQ(mmc3.irq_pending);
        break;
      case 0x6001: //$E001 - IRQ enable
        mmc3.irq_enable = true;
        SyncIRQ(mmc3.irq_pending);
        break;
    }
  }

  /// MMC3Board_Base.MapCHR, which allows non-power-of-two CHR sizes
  int MapChr(int addr) const
  {
    int bank_1k = mmc3.ChrBank1k(addr);
    bank_1k %= chr_bank_mask_1k + 1;
    return (bank_1k << 10) | (addr & 0x3FF);
  }

  uint8_t ReadPpu(int addr) override
  {
    if (addr < 0x2000)
    {
      if (kind == Kind::TxROM)
      {
        const int banked = MapChr(addr);
        return !Vrom.empty() ? Vrom[banked] : Vram[banked];
      }
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
      if (kind == Kind::TxROM)
      {
        // MMC3Board_Base.WritePpu: a CHR ROM board has no Vram and drops the write
        if (!Vram.empty()) Vram[MapChr(addr)] = value;
      }
      else if (kind == Kind::SxROM)
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
