// Translation of the PPU (the three C# partials merged into one class):
//   PPU.cs      -- region config, bus helpers, state, runppu
//   PPU.regs.cs -- registers 2000-2007, open-bus decay
//   PPU.run.cs  -- cycle-accurate VBL / active-scanline / pre-VBL ticks
//
// Methods that dereference the NES (bus, cpu, board, frame flags) are declared here and defined
// in nes.hpp: ppubus_write/read/peek, runppu, write_2007, read_2007, ReadReg, TickPPU_VBL,
// TickPPU_preVBL, NewDeadPPU. TickPPU_active stays here: its only NES dependency was
// nes.Settings, which lives in the nesSettings namespace below.
//
// Deliberately dropped (never taken for this cart / headless use): VS-system 2c05 register swap
// and peek_2002 special cases, light gun, NT/PPUView debug callbacks, SyncState (comes with the
// jaffar integration).

#pragma once

#include <cstdint>
#include <cstring>
#include "nesCommon.hpp"

namespace nesHawk
{

class NES;

// NESSettings (NES.ISettable.cs). These were compile-time constants while the only consumer was the
// oracle, which runs with a default-constructed NESSettings; a frontend lets the user change them
// while the machine runs, so they are ordinary globals now with the same defaults. They are
// settings, not machine state: nothing here belongs in a savestate.
namespace nesSettings
{
inline bool DispBackground = true;
inline bool DispSprites = true;
inline bool AllowMoreThanEightSprites = false;
}

class PPU
{
  public:

  int cpu_step = 0, cpu_stepcounter = 0;

  // Set by the board when it counts PPU cycles (MMC1 does, to ignore writes that arrive within
  // four clocks of each other). A plain bool rather than the old compile-time false: the branch is
  // perfectly predicted, and the alternative is one board per binary.
  bool HasClockPPU = false;

  // this only handles region differences within the PPU
  int preNMIlines = 0;
  int postNMIlines = 0;
  bool chopdot = false;

  enum class Region
  {
    NTSC,
    PAL,
    Dendy,
    RGB,
  };

  Region _region = Region::NTSC;

  void setRegion(Region value)
  {
    _region = value;
    SyncRegion();
  }

  void SyncRegion()
  {
    switch (_region)
    {
      case Region::NTSC:
        preNMIlines = 1; postNMIlines = 20; chopdot = true; break;
      case Region::PAL:
        preNMIlines = 1; postNMIlines = 70; chopdot = false; break;
      case Region::Dendy:
        preNMIlines = 51; postNMIlines = 20; chopdot = false; break;
      case Region::RGB:
        preNMIlines = 1; postNMIlines = 20; chopdot = false; break;
    }
  }

  //when the ppu issues a write it goes through here and into the game board
  void ppubus_write(int addr, uint8_t value); // -> nes.hpp

  //when the ppu issues a read it goes through here and into the game board
  uint8_t ppubus_read(int addr, bool ppu, bool addr_ppu); // -> nes.hpp

  //debug tools peek into the ppu through this
  uint8_t ppubus_peek(int addr); // -> nes.hpp

  static constexpr int PPU_PHASE_VBL = 0;
  static constexpr int PPU_PHASE_BG = 1;
  static constexpr int PPU_PHASE_OBJ = 2;

  int ppuphase = 0;

  NES* nes;

  PPU(NES* nes) : nes(nes)
  {
    // C# 'new' zero-fills; match it for every member array
    memset(OAM, 0, sizeof(OAM));
    memset(bgdata, 0, sizeof(bgdata));
    memset(xbuf, 0, sizeof(xbuf));
    memset(t_oam, 0, sizeof(t_oam));

    //power-up palette verified by blargg's power_up_palette test.
    //he speculates that these may differ depending on the system tested..
    //and I don't see why the ppu would waste any effort setting these..
    //but for the sake of uniformity, we'll do it.
    static constexpr uint8_t powerUpPalram[32] = {
      0x09,0x01,0x00,0x01,0x00,0x02,0x02,0x0D,0x08,0x10,0x08,0x24,0x00,0x00,0x04,0x2C,
      0x09,0x01,0x34,0x03,0x00,0x04,0x00,0x14,0x08,0x3A,0x00,0x02,0x00,0x20,0x2C,0x08
    };
    memcpy(PALRAM, powerUpPalram, sizeof(PALRAM));

    Reset();
  }

  void NESSoftReset()
  {
    //this hasn't been brought up to date since NEShawk was first made.
    //in particular http://wiki.nesdev.com/w/index.php/PPU_power_up_state should be studied, but theres no use til theres test cases
    Reset();
  }

  //state
  int ppudead = 0; //measured in frames
  bool idleSynch = false;
  int NMI_PendingInstructions = 0;
  uint8_t PPUGenLatch = 0;
  bool vtoggle = false;
  uint8_t VRAMBuffer = 0;
  uint8_t OAM[0x100];
  uint8_t PALRAM[32];

  int64_t _totalCycles = 0;
  int64_t TotalCycles() const { return _totalCycles; }

  void Reset()
  {
    regs_reset();
    ppudead = 1;
    idleSynch = true;
    ppu_open_bus = 0;
    for (int i = 0; i < 8; i++) ppu_open_bus_decay_timer[i] = 0;
    decay_pending = 0;
    recomputeDecayCheck();
    double_2007_read = 0;
    start_up_offset = 4;
  }

  NESHAWK_HOT_INLINE void runppu(); // -> nes.hpp (interacts with cpu/board at high granularity)

  // ================= PPU.regs.cs =================

  class Reg_2001
  {
    public:

    Bit color_disable; //Color disable (0: normal color; 1: AND all palette entries with 110000, effectively producing a monochrome display)
    Bit show_bg_leftmost; //Show leftmost 8 pixels of background
    Bit show_obj_leftmost; //Show sprites in leftmost 8 pixels
    Bit show_bg; //Show background
    Bit show_obj; //Show sprites
    Bit intense_green; //Intensify greens (and darken other colors)
    Bit intense_blue; //Intensify blues (and darken other colors)
    Bit intense_red; //Intensify reds (and darken other colors)

    int intensity_lsl_6 = 0; //an optimization..

    uint8_t getValue() const
    {
      return (uint8_t)((int)color_disable | ((int)show_bg_leftmost << 1) | ((int)show_obj_leftmost << 2) | ((int)show_bg << 3) | ((int)show_obj << 4) | ((int)intense_green << 5) | ((int)intense_blue << 6) | ((int)intense_red << 7));
    }

    void setValue(uint8_t value)
    {
      color_disable = (value & 1);
      show_bg_leftmost = (value >> 1) & 1;
      show_obj_leftmost = (value >> 2) & 1;
      show_bg = (value >> 3) & 1;
      show_obj = (value >> 4) & 1;
      intense_blue = (value >> 6) & 1;
      intense_red = (value >> 7) & 1;
      intense_green = (value >> 5) & 1;
      intensity_lsl_6 = ((value >> 5) & 7) << 6;
    }
  };

  bool PPUON() const { return show_bg_new || show_obj_new; }

  // this byte is used to simulate open bus reads and writes
  // it should be modified by every read and write to a ppu register
  uint8_t ppu_open_bus = 0;
  int64_t double_2007_read = 0; // emulates a hardware bug of back to back 2007 reads
  int ppu_open_bus_decay_timer[8] = {0};
  uint8_t glitchy_reads_2003[8] = {0};

  struct PPUSTATUS
  {
    int sl = 0;
    bool rendering() const { return sl >= 0 && sl < 241; }
    int cycle = 0;
  };

  //uses the internal counters concept at http://nesdev.icequake.net/PPU%20addressing.txt
  class PPUREGS
  {
    public:

    PPUREGS() { reset(); }

    //normal clocked regs. as the game can interfere with these at any time, they need to be savestated
    int fv = 0;//3
    int v = 0;//1
    int h = 0;//1
    int vt = 0;//5
    int ht = 0;//5

    //temp unlatched regs (need savestating, can be written to at any time)
    int _fv = 0, _vt = 0, _v = 0, _h = 0, _ht = 0;

    //other regs that need savestating
    int fh = 0;//3 (horz scroll)

    //cached state data. these are always reset at the beginning of a frame and don't need saving
    //but just to be safe, we're gonna save it
    PPUSTATUS status;

    void reset()
    {
      fv = v = h = vt = ht = 0;
      fh = 0;
      _fv = _v = _h = _vt = _ht = 0;
      status.cycle = 0;
      status.sl = 0;
    }

    void install_latches()
    {
      fv = _fv;
      v = _v;
      h = _h;
      vt = _vt;
      ht = _ht;
    }

    void install_h_latches()
    {
      ht = _ht;
      h = _h;
    }

    void increment_hsc()
    {
      //The first one, the horizontal scroll counter, consists of 6 bits, and is
      //made up by daisy-chaining the HT counter to the H counter. The HT counter is
      //then clocked every 8 pixel dot clocks (or every 8/3 CPU clock cycles).
      ht++;
      h += (ht >> 5);
      ht &= 31;
      h &= 1;
    }

    void increment_vs()
    {
      fv++;
      int fv_overflow = (fv >> 3);
      vt += fv_overflow;
      vt &= 31; //fixed tecmo super bowl
      if (vt == 30 && fv_overflow == 1) //caution here (only do it at the exact instant of overflow) fixes p'radikus conflict
      {
        v++;
        vt = 0;
      }
      fv &= 7;
      v &= 1;
    }

    int get_ntread() const
    {
      return 0x2000 | (v << 0xB) | (h << 0xA) | (vt << 5) | ht;
    }

    int get_2007access() const
    {
      return ((fv & 3) << 0xC) | (v << 0xB) | (h << 0xA) | (vt << 5) | ht;
    }

    //The PPU has an internal 4-position, 2-bit shifter, which it uses for
    //obtaining the 2-bit palette select data during an attribute table byte
    //fetch. To represent how this data is shifted in the diagram, letters a..c
    //are used in the diagram to represent the right-shift position amount to
    //apply to the data read from the attribute data (a is always 0). This is why
    //you only see bits 0 and 1 used off the read attribute data in the diagram.
    int get_atread() const
    {
      return 0x2000 | (v << 0xB) | (h << 0xA) | 0x3C0 | ((vt & 0x1C) << 1) | ((ht & 0x1C) >> 2);
    }

    void increment2007(bool rendering, bool by32)
    {
      if (rendering)
      {
        //don't do this:
        //if (by32) increment_vs();
        //else increment_hsc();
        //do this instead:
        increment_vs(); //yes, even if we're moving by 32
        return;
      }

      //If the VRAM address increment bit (2000.2) is clear (inc. amt. = 1), all the
      //scroll counters are daisy-chained (in the order of HT, VT, H, V, FV) so that
      //the carry out of each counter controls the next counter's clock rate. The
      //result is that all 5 counters function as a single 15-bit one. Any access to
      //2007 clocks the HT counter here.
      //
      //If the VRAM address increment bit is set (inc. amt. = 32), the only
      //difference is that the HT counter is no longer being clocked, and the VT
      //counter is now being clocked by access to 2007.
      if (by32)
      {
        vt++;
      }
      else
      {
        ht++;
        vt += (ht >> 5) & 1;
      }
      h += (vt >> 5);
      v += (h >> 1);
      fv += (v >> 1);
      ht &= 31;
      vt &= 31;
      h &= 1;
      v &= 1;
      fv &= 7;
    }
  };

  class Reg_2000
  {
    public:

    PPUREGS* _regs = nullptr;

    //these bits go straight into PPUR
    //(00 = $2000; 01 = $2400; 02 = $2800; 03 = $2c00)

    Bit vram_incr32; //(0: increment by 1, going across; 1: increment by 32, going down)
    Bit obj_pattern_hi; //Sprite pattern table address for 8x8 sprites (0: $0000; 1: $1000)
    Bit bg_pattern_hi; //Background pattern table address (0: $0000; 1: $1000)
    Bit obj_size_16; //Sprite size (0: 8x8 sprites; 1: 8x16 sprites)
    Bit ppu_layer; //PPU layer select (should always be 0 in the NES; some Nintendo arcade boards presumably had two PPUs)
    Bit vblank_nmi_gen; //Vertical blank NMI generation (0: off; 1: on)

    uint8_t getValue() const
    {
      return (uint8_t)(_regs->_h | (_regs->_v << 1) | ((int)vram_incr32 << 2) | ((int)obj_pattern_hi << 3) | ((int)bg_pattern_hi << 4) | ((int)obj_size_16 << 5) | ((int)ppu_layer << 6) | ((int)vblank_nmi_gen << 7));
    }

    void setValue(uint8_t value)
    {
      _regs->_h = value & 1;
      _regs->_v = (value >> 1) & 1;
      vram_incr32 = (value >> 2) & 1;
      obj_pattern_hi = (value >> 3) & 1;
      bg_pattern_hi = (value >> 4) & 1;
      obj_size_16 = (value >> 5) & 1;
      ppu_layer = (value >> 6) & 1;
      vblank_nmi_gen = (value >> 7) & 1;
    }
  };

  Bit Reg2002_objoverflow;  //Sprite overflow. The PPU can handle only eight sprites on one scanline and sets this bit if it starts drawing sprites.
  Bit Reg2002_objhit; //Sprite 0 overlap.  Set when a nonzero pixel of sprite 0 is drawn overlapping a nonzero background pixel.  Used for raster timing.
  Bit Reg2002_vblank_active;  //Vertical blank start (0: has not started; 1: has started)
  bool Reg2002_vblank_active_pending = false; //set if Reg2002_vblank_active is pending
  bool Reg2002_vblank_clear_pending = false; //ppu's clear of vblank flag is pending
  PPUREGS ppur;
  Reg_2000 reg_2000;
  Reg_2001 reg_2001;
  uint8_t reg_2003 = 0;
  uint8_t reg_2006_2 = 0;

  void regs_reset()
  {
    //TODO - would like to reconstitute the entire PPU instead of all this..
    ppur.reset();
    reg_2000 = Reg_2000();
    reg_2000._regs = &ppur;
    reg_2001 = Reg_2001();
    Reg2002_objoverflow = false;
    Reg2002_objhit = false;
    Reg2002_vblank_active = false;
    PPUGenLatch = 0;
    reg_2003 = 0;
    vtoggle = false;
    VRAMBuffer = 0;
  }

  //PPU CONTROL (write)
  void write_2000(uint8_t value)
  {
    if (!reg_2000.vblank_nmi_gen & ((value & 0x80) != 0) && (bool)Reg2002_vblank_active && !Reg2002_vblank_clear_pending)
    {
      //if we just unleashed the vblank interrupt then activate it now
      NMI_PendingInstructions = 2;
    }
    reg_2000.setValue(value);
  }

  uint8_t read_2000() { return ppu_open_bus; }
  uint8_t peek_2000() { return ppu_open_bus; }

  //PPU MASK (write)
  void write_2001(uint8_t value)
  {
    reg_2001.setValue(value);
    install_2001 = 2;
  }

  uint8_t read_2001() { return ppu_open_bus; }
  uint8_t peek_2001() { return ppu_open_bus; }

  //PPU STATUS (read)
  void write_2002(uint8_t value) {}

  uint8_t read_2002()
  {
    uint8_t ret = peek_2002();

    // reading from $2002 resets the destination for $2005 and $2006 writes
    vtoggle = false;
    Reg2002_vblank_active = 0;
    Reg2002_vblank_active_pending = false;

    // update the open bus here
    ppu_open_bus = ret;
    PpuOpenBusDecay(DecayType::High);
    return ret;
  }

  uint8_t peek_2002()
  {
    // (VS 2c05 special cases dropped -- not a VS system)
    return (uint8_t)(((int)Reg2002_vblank_active << 7) | ((int)Reg2002_objhit << 6) | ((int)Reg2002_objoverflow << 5) | (ppu_open_bus & 0x1F));
  }

  //OAM ADDRESS (write)
  void write_2003(int addr, uint8_t value)
  {
    if (_region == Region::NTSC)
    {
      // in NTSC this does several glitchy things to corrupt OAM
      // commented out for now until better understood
      reg_2003 = value;
    }
    else
    {
      // in PAL, just record the oam buffer write target
      reg_2003 = value;
    }
  }

  uint8_t read_2003() { return ppu_open_bus; }
  uint8_t peek_2003() { return ppu_open_bus; }

  //OAM DATA (write)
  void write_2004(uint8_t value)
  {
    if ((reg_2003 & 3) == 2)
    {
      //some of the OAM bits are unwired so we mask them out here
      //otherwise we just write this value and move on to the next oam byte
      value &= 0xE3;
    }
    if (ppur.status.rendering())
    {
      // don't write to OAM if the screen is on and we are in the active display area
      // this impacts sprite evaluation
      if (show_bg_new || show_obj_new)
      {
        // glitchy increment of OAM index
        oam_index += 4;
        oam_index &= 0x1FC;
        reg_2003 += 4;
        reg_2003 &= 0xFC;
      }
      else
      {
        OAM[reg_2003] = value;
        reg_2003++;
      }
    }
    else
    {
      OAM[reg_2003] = value;
      reg_2003++;
    }
  }

  uint8_t read_2004()
  {
    uint8_t ret;
    // behaviour depends on whether things are being rendered or not
    if (PPUON())
    {
      if (ppur.status.sl < 241)
      {
        if (ppur.status.cycle <= 64)
        {
          ret = 0xFF; // during this time all reads return FF
        }
        else if (ppur.status.cycle <= 256)
        {
          ret = read_value;
        }
        else if (ppur.status.cycle <= 320)
        {
          ret = read_value;
        }
        else
        {
          ret = soam[0];
        }
      }
      else
      {
        ret = OAM[reg_2003];
      }
    }
    else
    {
      ret = OAM[reg_2003];
    }

    ppu_open_bus = ret;
    PpuOpenBusDecay(DecayType::All);
    return ret;
  }

  uint8_t peek_2004() { return OAM[reg_2003]; }

  //SCROLL (write)
  void write_2005(uint8_t value)
  {
    if (!vtoggle)
    {
      ppur._ht = value >> 3;
      ppur.fh = value & 7;
    }
    else
    {
      ppur._vt = value >> 3;
      ppur._fv = value & 7;
    }
    vtoggle = !vtoggle;
  }

  uint8_t read_2005() { return ppu_open_bus; }
  uint8_t peek_2005() { return ppu_open_bus; }

  //VRAM address register (write)
  void write_2006(uint8_t value)
  {
    if (!vtoggle)
    {
      ppur._vt &= 0x07;
      ppur._vt |= (value & 0x3) << 3;
      ppur._h = (value >> 2) & 1;
      ppur._v = (value >> 3) & 1;
      ppur._fv = (value >> 4) & 3;
      reg_2006_2 = value;
    }
    else
    {
      ppur._vt &= 0x18;
      ppur._vt |= (value >> 5);
      ppur._ht = value & 31;

      // testing indicates that this operation is delayed by 3 pixels
      //ppur.install_latches();
      install_2006 = 3;
    }

    vtoggle = !vtoggle;
  }

  uint8_t read_2006() { return ppu_open_bus; }
  uint8_t peek_2006() { return ppu_open_bus; }

  //VRAM data register (r/w)
  void write_2007(uint8_t value); // -> nes.hpp (board AddressPpu)
  uint8_t read_2007();            // -> nes.hpp (board AddressPpu)

  uint8_t peek_2007()
  {
    int addr = ppur.get_2007access() & 0x3FFF;

    //ordinarily we return the buffered values
    uint8_t ret = VRAMBuffer;

    //in any case, we read from the ppu bus
    // can't do this in peek; updates the value that will be used later
    // VRAMBuffer = ppubus_peek(addr);

    //but reads from the palette are implemented in the PPU and return immediately
    if ((addr & 0x3F00) == 0x3F00)
    {
      ret = PALRAM[addr & 0x1F];
    }

    return ret;
  }

  uint8_t ReadReg(int addr); // -> nes.hpp (double-2007-read check needs cpu.TotalExecutedCycles)

  uint8_t PeekReg(int addr)
  {
    PpuOpenBusDecayFlush(); // owed decay steps predate this access
    switch (addr)
    {
      case 0: return peek_2000(); case 1: return peek_2001(); case 2: return peek_2002(); case 3: return peek_2003();
      case 4: return peek_2004(); case 5: return peek_2005(); case 6: return peek_2006(); case 7: return peek_2007();
      default: __builtin_trap();
    }
  }

  void WriteReg(int addr, uint8_t value)
  {
    PpuOpenBusDecayFlush(); // owed decay steps predate this access
    PPUGenLatch = value;
    ppu_open_bus = value;

    switch (addr & 0x07)
    {
      case 0: write_2000(value); break; // (VS 2c05 swap dropped)
      case 1: write_2001(value); break;
      case 2: write_2002(value); break;
      case 3: write_2003(addr, value); break;
      case 4: write_2004(value); break;
      case 5: write_2005(value); break;
      case 6: write_2006(value); break;
      case 7: write_2007(value); break;
      default: __builtin_trap();
    }
  }

  enum class DecayType
  {
    None = 0, // if there is no action, decrement the timer
    All = 1, // reset the timer for all bits (reg 2004 / 2007 (non-palette)
    High = 2, // reset the timer for high 3 bits (reg 2002)
    Low = 3 // reset the timer for all low 6 bits (reg 2007 (palette))
  };

  // The None decay runs once per CPU cycle, but its timers span ~1.8M cycles and ppu_open_bus is
  // only ever observed at PPU register accesses. So None calls are BATCHED: a pending counter is
  // incremented per cycle and the exact per-call semantics (timer==0 -> clear bit + reload, else
  // timer--) are materialized by PpuOpenBusDecayFlush() before every observation point (register
  // read/write/peek, timer reset, serialization) or when a clear would come due. Bit-exact with
  // the per-cycle original; validated against the C# oracle.
  int decay_pending = 0;    // owed None-steps since the last flush (transient, always 0 when observed)
  int decay_next_check = 1; // owed-step count at which the earliest bit-clear comes due

  void recomputeDecayCheck()
  {
    int m = ppu_open_bus_decay_timer[0];
    for (int i = 1; i < 8; i++)
      if (ppu_open_bus_decay_timer[i] < m) m = ppu_open_bus_decay_timer[i];
    decay_next_check = m + 1; // a bit with timer t clears on the (t+1)-th None step
  }

  void PpuOpenBusDecayFlush()
  {
    if (decay_pending > 0)
    {
      for (int i = 0; i < 8; i++)
      {
        int k = decay_pending;
        int t = ppu_open_bus_decay_timer[i];
        while (k > 0)
        {
          if (t == 0)
          {
            ppu_open_bus = (uint8_t)(ppu_open_bus & (0xff - (1 << i)));
            t = 1786840; // about 1 second worth of cycles
            k--;
          }
          else
          {
            int d = t < k ? t : k;
            t -= d;
            k -= d;
          }
        }
        ppu_open_bus_decay_timer[i] = t;
      }
      decay_pending = 0;
    }
    recomputeDecayCheck();
  }

  void PpuOpenBusDecay(DecayType action)
  {
    switch (action)
    {
      case DecayType::None:
        decay_pending++;
        if (decay_pending >= decay_next_check) PpuOpenBusDecayFlush();
        break;
      case DecayType::All:
        for (int i = 0; i < 8; i++)
        {
          ppu_open_bus_decay_timer[i] = 1786840;
        }
        recomputeDecayCheck();
        break;
      case DecayType::High:
        ppu_open_bus_decay_timer[7] = 1786840;
        ppu_open_bus_decay_timer[6] = 1786840;
        ppu_open_bus_decay_timer[5] = 1786840;
        recomputeDecayCheck();
        break;
      case DecayType::Low:
        for (int i = 0; i < 6; i++)
        {
          ppu_open_bus_decay_timer[i] = 1786840;
        }
        recomputeDecayCheck();
        break;
    }
  }

  // ================= PPU.run.cs =================

  struct BGDataRecord
  {
    uint8_t nt, at;
    uint8_t pt_0, pt_1;
  };

  BGDataRecord bgdata[34];

  int16_t xbuf[256 * 240];

  // values here are used in sprite evaluation
  int spr_true_count = 0;
  bool sprite_eval_write = false;
  uint8_t read_value = 0;
  int soam_index = 0;
  int soam_index_prev = 0;
  int oam_index = 0;
  int oam_index_aux = 0;
  int soam_index_aux = 0;
  bool is_even_cycle = false;
  bool sprite_zero_in_range = false;
  bool sprite_zero_go = false;
  int yp = 0;
  int target = 0;
  int spriteHeight = 0;
  uint8_t soam[256] = {0}; // in a real nes, this would only be 32, but we wish to allow more then 8 sprites per scanline
  bool ppu_was_on = false;
  bool ppu_was_on_spr = false;
  uint8_t sl_sprites[3 * 256] = {0};

  // installing vram address is delayed after second write to 2006, set this up here
  int install_2006 = 0;
  bool race_2006 = false, race_2006_2 = false;
  int install_2001 = 0;
  bool show_bg_new = false; //Show background
  bool show_obj_new = false; //Show sprites

  struct TempOAM
  {
    uint8_t oam_y;
    uint8_t oam_ind;
    uint8_t oam_attr;
    uint8_t oam_x;
    uint8_t patterns_0;
    uint8_t patterns_1;
  };

  TempOAM t_oam[64];

  int ppu_addr_temp = 0;

  // attempt to emulate graphics pipeline behaviour
  // experimental
  int pixelcolor_latch_1 = 0;

  // Turbo: the caller has said nobody will look at this frame. The pipeline is
  // the ONLY thing that writes xbuf, and the sprite-zero hit is decided
  // elsewhere (nes.hpp, from the sprite pattern and the raster position), so a
  // frame with this off is the same machine with no picture. It is the runtime
  // twin of NESHAWK_HEADLESS, which is the same claim made at compile time.
  bool render_enabled = true;

  void pipeline(int pixelcolor, int row_check)
  {
#ifndef NESHAWK_HEADLESS
    if (!render_enabled) return;
    if (row_check > 0)
    {
      if (reg_2001.color_disable)
        pixelcolor_latch_1 &= 0x30;

      //TODO - check flashing sirens in werewolf
      //tack on the deemph bits. THESE MAY BE ORDERED WRONG. PLEASE CHECK IN THE PALETTE CODE
      xbuf[target - 1] = (int16_t)(pixelcolor_latch_1 | reg_2001.intensity_lsl_6);
    }

    pixelcolor_latch_1 = pixelcolor;
#endif
    // headless: the pixel pipeline only feeds the framebuffer; callers keep their own
    // target/counter bookkeeping (that IS emulation state)
  }

  //address line 3 relates to the pattern table fetch occuring (the PPU always makes them in pairs).
  int get_ptread(int par) const
  {
    int hi = reg_2000.bg_pattern_hi;
    return (hi << 0xC) | (par << 0x4) | ppur.fv;
  }

  void Read_bgdata(int cycle, int i)
  {
    switch (cycle)
    {
      case 0:
        ppu_addr_temp = ppur.get_ntread();
        bgdata[i].nt = ppubus_read(ppu_addr_temp, true, true);
        break;
      case 1:
        break;
      case 2:
      {
        ppu_addr_temp = ppur.get_atread();
        uint8_t at = ppubus_read(ppu_addr_temp, true, true);

        //modify at to get appropriate palette shift
        if ((ppur.vt & 2) != 0) at >>= 4;
        if ((ppur.ht & 2) != 0) at >>= 2;
        at &= 0x03;
        at <<= 2;
        bgdata[i].at = at;
        break;
      }
      case 3:
        break;
      case 4:
        ppu_addr_temp = get_ptread(bgdata[i].nt);
        bgdata[i].pt_0 = ppubus_read(ppu_addr_temp, true, true);
        break;
      case 5:
        break;
      case 6:
        ppu_addr_temp |= 8;
        bgdata[i].pt_1 = ppubus_read(ppu_addr_temp, true, true);
        break;
      case 7:
        break;
    }
  }

  // these are states for the ppu incrementer
  bool do_vbl = false;
  bool do_active_sl = false;
  bool do_pre_vbl = false;

  bool nmi_destiny = false;
  bool evenOddDestiny = false;
  int start_up_offset = 0;
  int NMI_offset = 0;
  int yp_shift = 0;
  int sprite_eval_cycle = 0;
  int xt = 0;
  int xp = 0;
  int xstart = 0;
  int rasterpos = 0;
  bool renderspritenow = false;
  int s = 0;
  int ppu_aux_index = 0;
  bool junksprite = false;
  int line = 0;
  int patternNumber = 0;
  int patternAddress = 0;
  int temp_addr = 0;

  void ppu_init_frame()
  {
    ppur.status.sl = 241 + preNMIlines;
    ppur.status.cycle = 0;

    // These things happen at the start of every frame
    ppuphase = PPU_PHASE_VBL;
    memset(bgdata, 0, sizeof(bgdata)); // bgdata = new BGDataRecord[34]
  }

  NESHAWK_HOT_INLINE void TickPPU_VBL();    // -> nes.hpp (cpu.NMI, board AtVsyncNmi)
  NESHAWK_HOT_INLINE void TickPPU_active(); // -> nes.hpp (calls runppu/ppubus_read which need NES complete)
  NESHAWK_HOT_INLINE void TickPPU_preVBL(); // -> nes.hpp (frame_is_done)
  void NewDeadPPU();     // -> nes.hpp (frame_is_done)
};

} // namespace nesHawk
