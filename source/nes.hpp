// Translation of the NES system glue:
//   NES class            <- src/BizHawk.Emulation.Cores/Consoles/Nintendo/NES/NES.Core.cs
//   CpuLink              <- src/BizHawk.Emulation.Cores/Consoles/Nintendo/NES/NES.CpuLink.cs
//   power-on / cart init <- src/BizHawk.Emulation.Cores/Consoles/Nintendo/NES/NES.cs (Init) and
//                           NES.BoardSystem.cs (BoardSystemHardReset)
//
// This file also holds the out-of-line bodies of every board/APU/PPU method that dereferences
// the NES (declared in nesBoards.hpp / nesApu.hpp / nesPpu.hpp) -- they need the complete type.
//
// Deliberately dropped relative to the C# source (never taken for this cart / headless use):
// VS system, FDS, cheats (num_cheats kept, always 0), memory/input callbacks, tracer, video
// provider, blip audio synthesis (EmitSample kept -- it has no feedback into emulation state),
// savestates (come with the jaffar integration).
//
// TotalExecutedCycles (NES.IDebuggable.cs) => cpu.TotalExecutedCycles.

#pragma once

#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>

#include "mos6502x.hpp"
#include "nesCommon.hpp"
#include "nesBoards.hpp"
#include "nesControllers.hpp"
#include "nesApu.hpp"
#include "nesPpu.hpp"

namespace nesHawk
{

class NES;

// NES.CpuLink.cs -- note DummyReadMemory forwards to nes.ReadMemory (NOT nes.DummyReadMemory)
struct CpuLink
{
  NES* nes;

  uint8_t DummyReadMemory(uint16_t address);
  void OnExecFetch(uint16_t address);
  uint8_t PeekMemory(uint16_t address);
  uint8_t ReadMemory(uint16_t address);
  void WriteMemory(uint16_t address, uint8_t value);
};

class NES
{
  public:

  //hardware/state
  CpuLink cpuLink{this};
  std::unique_ptr<MOS6502X<CpuLink>> cpu;
  std::unique_ptr<PPU> ppu;
  std::unique_ptr<APU> apu;
  uint8_t ram[0x800];
  uint8_t CIRAM[0x800]; //AKA nametables
  CartInfo cart;
  std::unique_ptr<NesBoard> board;
  int sprdma_countdown = 0;

  bool _irq_apu = false; //various irq signals that get merged to the cpu irq pin

  int cpuclockrate = 0;

  // new input system (one standard pad in port 1; port 2 unplugged by default, or a second standard
  // pad when controllerDeck._port2Connected is set -- see NesDeck)
  NesDeck controllerDeck;
  uint8_t latched4016 = 0;
  uint8_t _controllerButtons = 0;  // current frame's P1 buttons (ButtonBit mask)
  uint8_t _controllerButtons2 = 0; // current frame's P2 buttons (ButtonBit mask; ignored when port 2 unplugged)

  int old_s = 0;

  int64_t double_controller_read = 0;
  uint16_t double_controller_read_address = 0;
  uint8_t previous_controller1_read = 0;
  uint8_t previous_controller2_read = 0;
  bool dmc_dma_controller_conflict = false;
  bool joypadStrobed = false;
  uint8_t joypadStrobeValue = 0;

  // cheat system dropped; field kept so ReadMemory matches the C# shape
  int num_cheats = 0;

  int _frame = 0;
  int _lagcount = 0;
  bool lagged = false;
  bool islag = false;

  bool resetSignal = false;
  bool hardResetSignal = false;

  // these variables are for subframe input control
  bool controller_was_latched = false;
  bool frame_is_done = false;
  bool current_strobe = false;
  bool new_strobe = false;

  //PAL:
  //sequence of ppu clocks per cpu clock: 3,3,3,3,4
  //at least it should be, but something is off with that (start up time?) so it is 3,3,3,4,3 for now
  //NTSC:
  //sequence of ppu clocks per cpu clock: 3
  const uint8_t* cpu_sequence = nullptr;
  static constexpr uint8_t cpu_sequence_NTSC[5] = { 3, 3, 3, 3, 3 };
  static constexpr uint8_t cpu_sequence_PAL[5] = { 3, 3, 3, 4, 3 };
  int cpu_deadcounter = 0;

  int oam_dma_index = 0;
  bool oam_dma_exec = false;
  uint16_t oam_dma_addr = 0;
  uint8_t oam_dma_byte = 0;
  bool dmc_dma_exec = false;
  bool dmc_realign = false;
  uint8_t DB = 0; //old data bus values from previous reads

  int64_t TotalExecutedCycles() const { return cpu->TotalExecutedCycles; }

  // PPU cycle count as of the start of the current frame. cpu->ext_ppu_cycle is only materialized
  // at frame end (runppu stopped incrementing it per cycle), so anything that wants the count
  // MID-frame -- a tracer, say -- subtracts this instead of reading a stale field.
  int64_t extPpuCycleBase = 0;
  int32_t ExtPpuCycle() const { return (int32_t)(ppu->_totalCycles - extPpuCycleBase); }

  // ---- machine configuration (NESSyncSettings, NES.ISettable.cs) ----
  // Resolved before the first HardReset and fixed for the machine's life -- BizHawk reboots the
  // core when one of these changes. The translation used to hardcode what a default-constructed
  // NESSyncSettings gives you: NTSC, and the fceux power-on RAM pattern.
  PPU::Region _display_type = PPU::Region::NTSC;
  std::vector<uint8_t> initialWRamStatePattern; // empty = the fceux pattern (NES.Core.cs HardReset)

  // frame rate of the resolved region, set by HardReset (NES.Core.cs VsyncNum / VsyncDen)
  int VsyncNum = 0;
  int VsyncDen = 0;

  // NES.cs hands MOS6502X a TraceCallback; the CPU calls OnExecFetch immediately before fetching
  // an opcode, with every register still holding the pre-instruction state, which is the moment
  // BizHawk logs. Null unless a tracer is attached.
  void (*traceCallback)(NES& nes, uint16_t addr) = nullptr;

  // NES.Core.cs feeds a blip buffer from RunCpuOne: every change of the mixed APU output is a delta
  // at the current sample clock. The band-limited synthesis itself is not part of this core (the
  // source stays dependency-free), so the deltas leave through a callback and whoever wants sound
  // owns the buffer. Only called when NESHAWK_FULL_AV is defined.
  void (*sampleCallback)(void* ctx, uint32_t clock, int delta) = nullptr;
  void* sampleCallbackCtx = nullptr;

  // ---- power-on (NES.cs Init: the iNES/NES-UNROM path, plus header-driven NROM) ----
  // romFile = full .nes file including the 16-byte iNES header.
  NES(const uint8_t* romFile, size_t romFileSize,
      PPU::Region region = PPU::Region::NTSC,
      const uint8_t* wramPattern = nullptr, size_t wramPatternSize = 0)
  {
    _display_type = region;
    if (wramPattern != nullptr && wramPatternSize != 0)
      initialWRamStatePattern.assign(wramPattern, wramPattern + wramPatternSize);

    if (romFileSize < 16) throw std::runtime_error("ROM file smaller than an iNES header");
    if (memcmp(romFile, "NES\x1A", 4) != 0) throw std::runtime_error("not an iNES file");
    const int mapper = (romFile[6] >> 4) | (romFile[7] & 0xF0);
    const int chr8   = romFile[5];
    // Only the boards translated here (see NesBoard). The real NesHawk resolves the board from the
    // BootGod DB and has a hundred more.
    // The boards translated here (see NesBoard). The real NesHawk resolves a board from the BootGod
    // DB and has a hundred more; each one added here is a transliteration of its Boards/*.cs.
    NesBoard::Kind kind;
    switch (mapper)
    {
      case 0:  kind = NesBoard::Kind::NROM;  break;
      case 1:  kind = NesBoard::Kind::SxROM; break;
      case 2:  kind = NesBoard::Kind::UxROM; break;
      case 3:  kind = NesBoard::Kind::CNROM; break;
      case 7:  kind = NesBoard::Kind::AxROM; break;
      case 66: kind = NesBoard::Kind::GxROM; break;
      case 70: kind = NesBoard::Kind::B74x;  break;
      default: throw std::runtime_error("unsupported mapper (this translation has NROM, SxROM, UxROM, CNROM, AxROM, GxROM and mapper 70)");
    }
    if ((romFile[6] & 4) != 0) throw std::runtime_error("trainers are not supported");

    // Everything the two boards need comes out of the iNES header; NesHawk would take it from the
    // cart database, which is also where a board type with a bus-conflict or WRAM quirk would come
    // from. flags6 bit0 set = vertical mirroring -> pads (1,0); clear = horizontal -> (0,1).
    cart.PrgSize  = (int)romFile[4] * 16;
    cart.ChrSize  = chr8 * 8;
    cart.VramSize = chr8 != 0 ? 0 : 8; // CHR ROM or CHR RAM, never both
    cart.WramSize = 8;                 // NES.iNES.cs: "should be data[8], but that never worked"
    cart.PadH     = (romFile[6] & 1) ? 1 : 0;
    cart.PadV     = (romFile[6] & 1) ? 0 : 1;
    if (cart.PrgSize == 0) throw std::runtime_error("iNES header declares no PRG ROM");
    if (romFileSize < 16 + (size_t)cart.PrgSize * 1024 + (size_t)chr8 * 8 * 1024)
      throw std::runtime_error("ROM file too small for its header-declared sizes");

    board = std::make_unique<NesBoard>();
    board->kind = kind;
    board->Cart = cart;
    board->Create(this);
    board->Configure();

    board->Rom.assign(romFile + 16, romFile + 16 + (size_t)cart.PrgSize * 1024);

    //create the vram and wram if necessary
    if (cart.VramSize != 0)
      board->Vram.assign((size_t)cart.VramSize * 1024, 0);
    if (cart.WramSize != 0)
      board->Wram.assign((size_t)cart.WramSize * 1024, 0);

    // CHR ROM on the cart goes in Vrom (NesBoardBase distinguishes the two: Vram is writable
    // pattern memory, Vrom is the cartridge's own, and the banking boards index all of it)
    if (chr8 > 0)
    {
      const uint8_t* chr = romFile + 16 + (size_t)cart.PrgSize * 1024;
      board->Vrom.assign(chr, chr + (size_t)chr8 * 8 * 1024);
    }

    board->PostConfigure();

    // display type: cart.System "NES-NTSC" -> NTSC
    HardReset();
  }

  // NES.BoardSystem.cs
  void BoardSystemHardReset()
  {
    auto newboard = std::make_unique<NesBoard>();
    newboard->kind = board->kind;
    newboard->Cart = cart;
    newboard->Create(this);
    newboard->Configure();
    newboard->Rom = board->Rom;
    if (!board->Wram.empty())
      newboard->Wram.assign(board->Wram.size(), 0);
    if (!board->Vram.empty()) newboard->Vram.assign(board->Vram.size(), 0);
    newboard->Vrom = board->Vrom;   // cartridge CHR survives a reset
    newboard->PostConfigure();
    // (no battery SaveRam on this cart)

    board = std::move(newboard);
    ppu->HasClockPPU = board->WantsPpuClock();
  }

  void HardReset()
  {
    cpu = std::make_unique<MOS6502X<CpuLink>>(cpuLink);
    cpu->BCD_Enabled = false;

    ppu = std::make_unique<PPU>(this);
    memset(ram, 0, sizeof(ram));
    memset(CIRAM, 0, sizeof(CIRAM));

    // set up region (NES.Core.cs HardReset). VsyncNum/VsyncDen are the frame rate a frontend needs;
    // the PAL and Dendy numbers are BizHawk's own.
    {
      auto old = std::move(apu);
      switch (_display_type)
      {
        case PPU::Region::PAL:
          apu = std::make_unique<APU>(this, old.get(), true);
          ppu->setRegion(PPU::Region::PAL);
          cpuclockrate = 1662607;
          VsyncNum = cpuclockrate * 2;
          VsyncDen = 66495;
          cpu_sequence = cpu_sequence_PAL;
          break;
        // in bootgod, but BizHawk never resolves a cart to it -- only a region override reaches this
        case PPU::Region::Dendy:
          apu = std::make_unique<APU>(this, old.get(), false);
          ppu->setRegion(PPU::Region::Dendy);
          cpuclockrate = 1773448;
          VsyncNum = cpuclockrate;
          VsyncDen = 35464;
          cpu_sequence = cpu_sequence_NTSC;
          break;
        default:
          _display_type = PPU::Region::NTSC;
          apu = std::make_unique<APU>(this, old.get(), false);
          ppu->setRegion(PPU::Region::NTSC);
          cpuclockrate = 1789773;
          VsyncNum = cpuclockrate * 2;
          VsyncDen = 59561;
          cpu_sequence = cpu_sequence_NTSC;
          break;
      }
    }

    BoardSystemHardReset();

    // apu has some specific power up bahaviour that we will emulate here
    apu->NESHardReset();

    if (!initialWRamStatePattern.empty())
    {
      for (int i = 0; i < 0x800; i++)
      {
        ram[i] = initialWRamStatePattern[i % initialWRamStatePattern.size()];
      }
    }
    else
    {
      // check fceux's PowerNES and FCEU_MemoryRand function for more information:
      // relevant games: Cybernoid; Minna no Taabou no Nakayoshi Daisakusen; Huang Di; and maybe mechanized attack
      for (int i = 0; i < 0x800; i++)
      {
        if ((i & 4) != 0)
        {
          ram[i] = 0xFF;
        }
        else
        {
          ram[i] = 0x00;
        }
      }
    }

    // (per-game RAM-init special cases from the game DB do not apply to this cart)
  }

  // ---- frame loop (NES.Core.cs FrameAdvance) ----
  // buttons: P1 ButtonBit mask for this frame. buttons2: P2 mask (ignored unless port 2 is
  // connected). Power/Reset are not expressible in .sol inputs.
  bool FrameAdvance(uint8_t buttons, uint8_t buttons2 = 0)
  {
    _controllerButtons = buttons;
    _controllerButtons2 = buttons2;

#ifdef _NESHAWK_DETECT_BAD_ACCESS
    cpu->badAccessLatch = 0; // per-frame: report only derails caused by THIS frame's advance
#endif

    lagged = true;
    if (resetSignal)
    {
      board->NesSoftReset();
      cpu->NESSoftReset();
      apu->NESSoftReset();
      ppu->NESSoftReset();
    }
    else if (hardResetSignal)
    {
      HardReset();
    }

    resetSignal = false;     // controller.IsPressed("Reset")
    hardResetSignal = false; // controller.IsPressed("Power")

    cpu->ext_ppu_cycle = 0; // Reset this value at the beginning of each frame
    extPpuCycleBase = ppu->_totalCycles; // materialized at frame end (see below)

    if (ppu->ppudead > 0)
    {
      while (ppu->ppudead > 0)
      {
        ppu->NewDeadPPU();
      }
    }
    else
    {
      // do the vbl ticks seperate, that will save us a few checks that don't happen in active region
      while (ppu->do_vbl)
      {
        ppu->TickPPU_VBL();
      }

      // now do the rest of the frame
      while (ppu->do_active_sl)
      {
        ppu->TickPPU_active();
      }

      // now do the pre-NMI lines
      while (ppu->do_pre_vbl)
      {
        ppu->TickPPU_preVBL();
      }
    }

    if (lagged)
    {
      _lagcount++;
      islag = true;
    }
    else
      islag = false;

    num_cheats = 0;

    _frame++;

    // materialize ext_ppu_cycle (runppu no longer increments it per cycle; equal by construction)
    cpu->ext_ppu_cycle = (int32_t)(ppu->_totalCycles - extPpuCycleBase);

    return true;
  }

  // NES.Core.cs RunCpuOne
  NESHAWK_HOT_INLINE void RunCpuOne()
  {
    ///////////////////////////
    // OAM DMA start
    ///////////////////////////

    if (oam_dma_exec && apu->dmc_dma_countdown != 1 && !dmc_realign)
    {
      if (cpu_deadcounter == 0)
      {
        if (oam_dma_index % 2 == 0)
        {
          oam_dma_byte = ReadMemory(oam_dma_addr);
          oam_dma_addr++;
        }
        else
        {
          WriteMemory(0x2004, oam_dma_byte);
        }
        oam_dma_index++;
        if (oam_dma_index == 512)
        {
          oam_dma_exec = false;
        }
      }
      else
      {
        cpu_deadcounter--;
      }
    }

    dmc_realign = false;

    /////////////////////////////
    // OAM DMA end
    /////////////////////////////


    /////////////////////////////
    // dmc dma start
    /////////////////////////////

    if (apu->dmc_dma_countdown > 0)
    {
      if (apu->dmc_dma_countdown == 1)
      {
        dmc_realign = true;
      }

      // By this point the cpu should be frozen, if it is not, then we are in a multi-write opcode, add another cycle delay
      if (!cpu->RDY && !cpu->rdy_freeze && (apu->dmc_dma_countdown == apu->DMC_RDY_check))
      {
        apu->dmc_dma_countdown += 2;
      }

      cpu->RDY = false;
      dmc_dma_exec = true;
      apu->dmc_dma_countdown--;
      if (apu->dmc_dma_countdown == 0)
      {
        apu->RunDMCFetch();

        dmc_dma_exec = false;
        apu->dmc_dma_countdown = -1;

        if ((apu->dmc.timer == 2) && (apu->dmc.out_bits_remaining == 0))
        {
          if (apu->dmc.sample_length != 0)
          {
            apu->dmc.fill_glitch = true;
          }
        }

        if ((apu->dmc.timer == 4) && (apu->dmc.out_bits_remaining == 0) && (apu->dmc.sample_length == 1))
        {
          apu->dmc.fill_glitch_2 = true;
        }
      }
      else
      {
        // the DMC DMA Halt, Put cycles
        apu->RunDMCHaltFetch();
      }
    }

    /////////////////////////////
    // dmc dma end
    /////////////////////////////
    apu->RunOneFirst();

    cpu->IRQ = _irq_apu || board->IrqSignal;

    // DMC was started in the APU, but in this case it only lasts 1 cycle and is then aborted, so put this here
    // NOTE: for some famicoms, this will also clock controllers, this will need to be handled if emulating additional models
    if (apu->dmc.fill_glitch_2_end)
    {
      apu->dmc_dma_countdown = -1;
      dmc_dma_exec = false;
      apu->dmc.fill_glitch_2 = false;
      apu->dmc.fill_glitch_2_end = false;
    }

    cpu->ExecuteOne();
    board->ClockCpu();

#ifndef NESHAWK_HEADLESS
    int s = apu->EmitSample();

    if (s != old_s)
    {
      // NES.Core.cs: blip.AddDelta(apu.sampleclock, s - old_s)
      if (sampleCallback != nullptr) sampleCallback(sampleCallbackCtx, apu->sampleclock, s - old_s);
      old_s = s;
    }
    apu->sampleclock++;
#endif

    apu->RunOneLast();

    if (!cpu->RDY && !dmc_dma_exec && !oam_dma_exec)
    {
      cpu->RDY = true;
    }
  }

  // NES.Core.cs ReadReg (non-VS)
  uint8_t ReadReg(int addr)
  {
    uint8_t ret_spec;
    switch (addr)
    {
      case 0x4000:
      case 0x4001:
      case 0x4002:
      case 0x4003:
      case 0x4004:
      case 0x4005:
      case 0x4006:
      case 0x4007:
      case 0x4008:
      case 0x4009:
      case 0x400A:
      case 0x400B:
      case 0x400C:
      case 0x400D:
      case 0x400E:
      case 0x400F:
      case 0x4010:
      case 0x4011:
      case 0x4012:
      case 0x4013:
        return DB;
      case 0x4014: /*OAM DMA*/ break;
      case 0x4015: return (uint8_t)((uint8_t)(apu->ReadReg(addr) & 0xDF) + (uint8_t)(DB & 0x20));
      case 0x4016:
      {
        // special hardware glitch case (dmc_dma_exec && region != NTSC) does not apply: NTSC
        ret_spec = read_joyport(addr);
        return ret_spec;
      }
      case 0x4017:
      {
        ret_spec = read_joyport(addr);
        return ret_spec;
      }
      default:
        break;
    }
    return DB;
  }

  uint8_t PeekReg(int addr)
  {
    switch (addr)
    {
      case 0x4000:
      case 0x4001:
      case 0x4002:
      case 0x4003:
      case 0x4004:
      case 0x4005:
      case 0x4006:
      case 0x4007:
      case 0x4008:
      case 0x4009:
      case 0x400A:
      case 0x400B:
      case 0x400C:
      case 0x400D:
      case 0x400E:
      case 0x400F:
      case 0x4010:
      case 0x4011:
      case 0x4012:
      case 0x4013:
        return apu->PeekReg(addr);
      case 0x4014: /*OAM DMA*/ break;
      case 0x4015: return apu->PeekReg(addr);
      case 0x4016:
      case 0x4017:
        return peek_joyport(addr);
      default:
        break;
    }
    return 0xFF;
  }

  void WriteReg(int addr, uint8_t val)
  {
    switch (addr)
    {
      case 0x4000:
      case 0x4001:
      case 0x4002:
      case 0x4003:
      case 0x4004:
      case 0x4005:
      case 0x4006:
      case 0x4007:
      case 0x4008:
      case 0x4009:
      case 0x400A:
      case 0x400B:
      case 0x400C:
      case 0x400D:
      case 0x400E:
      case 0x400F:
      case 0x4010:
      case 0x4011:
      case 0x4012:
      case 0x4013:
        apu->WriteReg(addr, val);
        break;
      case 0x4014:
        //schedule a sprite dma event for beginning 1 cycle in the future.
        //this receives 2 because that's just the way it works out.
        oam_dma_addr = (uint16_t)(val << 8);
        sprdma_countdown = 1;

        if (sprdma_countdown > 0)
        {
          sprdma_countdown--;
          if (sprdma_countdown == 0)
          {
            if (apu->dmc.timer % 2 == 0)
            {
              cpu_deadcounter = 2;
            }
            else
            {
              cpu_deadcounter = 1;
            }
            oam_dma_exec = true;
            cpu->RDY = false;
            oam_dma_index = 0;
          }
        }
        break;
      case 0x4015: apu->WriteReg(addr, val); break;
      case 0x4016:
        write_joyport(val);
        break;
      case 0x4017: apu->WriteReg(addr, val); break;
      default:
        break;
    }
  }

  void write_joyport(uint8_t value)
  {
    joypadStrobeValue = value;
    joypadStrobed = true;
  }

  void strobe_joyport()
  {
    // The controllers only get strobed when transitioning from a get cycle to a put cycle.
    StrobeInfo si(latched4016, joypadStrobeValue);
    controllerDeck.Strobe(si, _controllerButtons, _controllerButtons2);
    latched4016 = joypadStrobeValue;
    new_strobe = (joypadStrobeValue & 1) != 0;
    if (current_strobe && !new_strobe)
    {
      controller_was_latched = true;
      lagged = false;
    }
    current_strobe = new_strobe;
  }

  uint8_t read_joyport(int addr)
  {
    uint8_t ret;
    if (TotalExecutedCycles() == double_controller_read && addr == double_controller_read_address)
    {
      if (addr == 0x4016)
      {
        ret = previous_controller1_read;
      }
      else
      {
        ret = previous_controller2_read;
      }
    }
    else
    {
      if (addr == 0x4016)
      {
        ret = controllerDeck.ReadA(_controllerButtons);
        previous_controller1_read = ret; // If the following CPU cycle is also reading from this controller port, read the same value without clocking the controller.
      }
      else
      {
        ret = controllerDeck.ReadB(_controllerButtons2);
        previous_controller2_read = ret;
      }
    }
    double_controller_read_address = (uint16_t)addr;
    double_controller_read = TotalExecutedCycles() + 1; // The shift register in the controller is only updated if the previous CPU cycle did not read from the controller port.
    if (dmc_dma_controller_conflict)
    {
      double_controller_read++; // since the DMC DMA fetch routine occurs before cpu.ExecuteOne() which updates TotalExecutedCycles, we need to increment this value.
    }

    ret &= 0x1f;
    ret |= (uint8_t)(0xe0 & DB);
    return ret;
  }

  uint8_t peek_joyport(int addr)
  {
    // at the moment, the new system doesn't support peeks
    return 0;
  }

  uint8_t DummyReadMemory(uint16_t addr) { return 0; }

  uint8_t PeekMemory(uint16_t addr)
  {
    uint8_t ret;

    if (addr >= 0x4020)
    {
      //easy optimization, since rom reads are so common, move this up (reordering the rest of these elseifs is not easy)
      ret = board->PeekCart(addr);
    }
    else if (addr < 0x0800)
    {
      ret = ram[addr];
    }
    else if (addr < 0x2000)
    {
      ret = ram[addr & 0x7FF];
    }
    else if (addr < 0x4000)
    {
      ret = board->PeekReg2xxx(addr);
    }
    else if (addr < 0x4020)
    {
      ret = PeekReg(addr); //we're not rebasing the register just to keep register names canonical
    }
    else
    {
      __builtin_trap(); // "Woopsie-doodle!"
    }

    return ret;
  }

  // memory callbacks dropped; the tracer hook stays because this is where BizHawk's
  // MOS6502X.TraceCallback fires (NES.CpuLink.cs -> ExecFetch)
  void ExecFetch(uint16_t addr)
  {
    if (traceCallback != nullptr) traceCallback(*this, addr);
  }

  uint8_t ReadMemory(uint16_t addr)
  {
    if (!oam_dma_exec && !dmc_dma_exec)
    {
      cpu->address_bus = addr;
    }
    uint8_t ret;

    if (addr >= 0x8000)
    {
      // easy optimization, since rom reads are so common, move this up (reordering the rest of these else ifs is not easy)
      ret = board->ReadPrg(addr - 0x8000);
      // (game-genie cheat overlay dropped; num_cheats is always 0)
    }
    else if (addr < 0x0800)
    {
      ret = ram[addr];
    }
    else if (addr < 0x2000)
    {
      ret = ram[addr & 0x7FF];
    }
    else if (addr < 0x4000)
    {
      ret = board->ReadReg2xxx(addr);
    }
    else if (addr < 0x6000)
    {
      // Famicom Disk System Registers, or open bus. APU Registers need to be activated based on the position of the 6502 address bus, see below.
      ret = board->ReadExp(addr - 0x4000);
    }
    else
    {
      ret = board->ReadWram(addr - 0x6000);
    }

    if (cpu->address_bus >= 0x4000 && cpu->address_bus <= 0x401F)
    {
      // Regardless of the address the CPU is intending to read,
      // if the 6502 address bus is pointing to the APU registers,
      // then the APU registers are active.
      if ((addr & 0x1F) == 0x15)
      {
        if (!dmc_dma_exec)
        {
          ret &= 0x20; // only bit 5 of $4015 is open bus
          ret |= (uint8_t)(ReadReg(0x4000 | (addr & 0x1F)) & 0xDF);
        }
        else
        {
          // Poke OAMSTATUS to potentially clear the APU Frame Counter IRQ Flag.
          // Does the DMC DMA sample get modified by this?
          ReadReg(0x4000 | (addr & 0x1F));
        }
      }
      if ((addr & 0x1F) == 0x16 || (addr & 0x1F) == 0x17)
      {
        ret &= 0xE0; // only bits 5, 6, and 7 of the controller ports are open bus
        ret |= (uint8_t)(ReadReg(0x4000 | (addr & 0x1F)) & 0x1F);
      }
    }

    if (!(cpu->address_bus >= 0x4000 && cpu->address_bus < 0x4020 && (addr & 0x1F) == 0x15))
    {
      // This register is internal to the CPU and so the external CPU data bus is disconnected when reading it.
      // Therefore the returned value cannot be seen by external devices and the value does not affect open bus.
      DB = ret;
    }

    return ret;
  }

  void WriteMemory(uint16_t addr, uint8_t value)
  {
    if (!oam_dma_exec)
    {
      cpu->address_bus = addr;
    }
    if (addr < 0x0800)
    {
      ram[addr] = value;
    }
    else if (addr < 0x2000)
    {
      ram[addr & 0x7FF] = value;
    }
    else if (addr < 0x4000)
    {
      board->WriteReg2xxx(addr, value);
    }
    else if (addr < 0x4020)
    {
      WriteReg(addr, value);
    }
    else if (addr < 0x6000)
    {
      board->WriteExp(addr - 0x4000, value);
    }
    else if (addr < 0x8000)
    {
      board->WriteWram(addr - 0x6000, value);
    }
    else
    {
      board->WritePrg(addr - 0x8000, value);
    }

    DB = value;
  }
};

// ======================= CpuLink bodies (NES.CpuLink.cs) =======================

inline uint8_t CpuLink::DummyReadMemory(uint16_t address) { return nes->ReadMemory(address); }
inline void CpuLink::OnExecFetch(uint16_t address) { nes->ExecFetch(address); }
inline uint8_t CpuLink::PeekMemory(uint16_t address) { return nes->PeekMemory(address); }
inline uint8_t CpuLink::ReadMemory(uint16_t address) { return nes->ReadMemory(address); }
inline void CpuLink::WriteMemory(uint16_t address, uint8_t value) { nes->WriteMemory(address, value); }

// ======================= board bodies (NesBoardBase.cs) =======================

inline uint8_t NesBoardBase::ReadWram(int addr)
{
  return !Wram.empty() ? Wram[addr & _wramMask] : NES_->DB;
}

inline uint8_t NesBoardBase::ReadExp(int addr)
{
  return NES_->DB;
}

inline uint8_t NesBoardBase::ReadReg2xxx(int addr)
{
  return NES_->ppu->ReadReg(addr & 7);
}

inline uint8_t NesBoardBase::PeekReg2xxx(int addr)
{
  return NES_->ppu->PeekReg(addr & 7);
}

inline void NesBoardBase::WriteReg2xxx(int addr, uint8_t value)
{
  NES_->ppu->WriteReg(addr, value);
}

inline void NesBoardBase::WritePpu(int addr, uint8_t value)
{
  if (addr < 0x2000)
  {
    if (!Vram.empty())
    {
      Vram[addr] = value;
    }
  }
  else
  {
    NES_->CIRAM[ApplyMirroring(addr)] = value;
  }
}

inline uint8_t NesBoardBase::ReadPpu(int addr)
{
  if (addr < 0x2000)
  {
    return !Vrom.empty() ? Vrom[addr] : Vram[addr];
  }

  return NES_->CIRAM[ApplyMirroring(addr)];
}

// ======================= APU bodies (APU.cs) =======================

inline void APU::DMCUnit::Run()
{
  // Quiescence fast path: no Clock() due (timer > 1), no delayed start pending (delay == 0) and
  // no DMA start possible this cycle (buffer filled, or nothing to fetch, or a DMA already in
  // flight). The body then reduces to the timer decrement. Exact-equivalent.
  if (timer > 1 && delay == 0 &&
      (sample_buffer_filled || (sample_length == 0 && !fill_glitch_2) || apu->dmc_dma_countdown != -1))
  {
    timer_just_reloaded = false;
    timer--;
    return;
  }

  timer_just_reloaded = false;
  if (timer > 0) timer--;
  if (timer == 0)
  {
    timer = timer_reload;
    Clock();
    timer_just_reloaded = true;
  }

  // Any time the sample buffer is in an empty state and bytes remaining is not zero, the following occur:
  // also note that the halt for DMC DMA occurs on APU cycles only (hence the timer check)
  if (!sample_buffer_filled && ((sample_length > 0) || fill_glitch_2) && (apu->dmc_dma_countdown == -1) && (delay == 0))
  {
    if (!fill_glitch)
    {
      if (!apu->call_from_write)
      {
        // when called due to empty bueffer while DMC running, there is no delay
        nes->cpu->RDY = false;
        nes->dmc_dma_exec = true;

        if (fill_glitch_2)
        {
          // this will only run for one cycle and not actually run a DMA
          apu->dmc_dma_countdown = -2;
          apu->DMC_RDY_check = -2;
          fill_glitch_2_end = true;
        }
        else
        {
          apu->dmc_dma_countdown = 3;
          apu->DMC_RDY_check = 2;
        }
      }
      else
      {
        // when called from write, either a 2 or 3 cycle delay in activation.
        if (timer % 2 == 0)
        {
          delay = 3;
        }
        else
        {
          delay = 2;
        }
      }
    }
    else
    {
      // if refill and empty happen simultaneously, do not do another refill and act as though the sample buffer was filled
      sample_buffer_filled = true;
      fill_glitch = false;
    }
  }

  // VisualNES and test roms verify that DMC DMA is 1 cycle shorter for calls from writes
  // however, if the third cycle lands on a write, there is a 2 cycle delay even if the instruction only has a single write
  // therefore, the RDY_check is 2 in both cases.
  if (delay != 0)
  {
    delay--;
    if (delay == 0)
    {
      apu->dmc_dma_countdown = 3;
      apu->DMC_RDY_check = 2;
      apu->call_from_write = false;
    }
  }
}

inline void APU::DMCUnit::Fetch()
{
  if (sample_length != 0)
  {
    nes->dmc_dma_controller_conflict = true;
    sample_buffer = nes->ReadMemory((uint16_t)sample_address);
    nes->dmc_dma_controller_conflict = false;

    sample_buffer_filled = true;
    if (nes->cpu->address_bus >= 0x4000 && nes->cpu->address_bus <= 0x401F)
    {
      if ((sample_address & 0x1F) == 0x15)
      {
        nes->DB = (uint8_t)sample_buffer;
      }
      else if ((sample_address & 0x1F) == 0x16 || (sample_address & 0x1F) == 0x4017) // (the 0x4017 comparison is verbatim from the C# source; it can never be true)
      {
        nes->DB = (uint8_t)((sample_buffer & 0xE0) | (nes->DB & 0x1F)); // The bus conflict leaves the open bus bits of the controller filled with the bits from the sample buffer.
        // NOTE: When reading a controller port, different console revisions have different open bus bits.
      }
    }
    sample_address = (uint16_t)(sample_address + 1);

    //sample address wraps to 0x8000, even though this cannot be reached by write to address reg
    if (sample_address == 0) { sample_address = 0x8000; }

    apu->dmc_reload_countdown = 3;
  }
}

inline void APU::RunDMCHaltFetch()
{
  nes->ReadMemory(nes->cpu->address_bus);
}

inline void APU::RunOneLast()
{
  // Quiescence fast path: everything below is driven by deterministic counter hits (the frame
  // sequencer compares sequencer_counter against fixed LUT values) or by flags armed elsewhere
  // (pending register writes, strobes, IRQ acks, DMC reload countdown, $4017 seq_tick, IRQ
  // assert countdown). When none is due this cycle, the whole body reduces to
  // sequencer_counter++ (len_clock_active/doing_tick_quarter stay false, irq_pending and
  // nes->_irq_apu are unchanged). Exact-equivalent; validated against the C# oracle.
  {
    const int nc = sequencer_counter + 1;
    const bool seqEvent = (sequencer_mode == 0)
      ? (nc == sequencer_lut[0][3] - 1 || nc == sequencer_lut[0][3] - 2 || nc == sequencer_lut[0][sequencer_step])
      : (nc == sequencer_lut[1][4] - 1 || nc == sequencer_lut[1][sequencer_step]);
    if (!seqEvent && !nes->joypadStrobed && !sequencer_irq_clear_pending && dmc_reload_countdown == 0 &&
        pending_reg == -1 && seq_tick == 0 && sequencer_irq_assert == 0 && (!sequencer_irq || sequencer_irq_flag))
    {
      sequencer_counter = nc;
      return;
    }
  }

  if (dmc.timer % 2 == 1)
  {
    // The controllers only get strobed when transitioning from a get cycle to a put cycle.
    if (nes->joypadStrobed)
    {
      nes->joypadStrobed = false;
      nes->strobe_joyport();
    }
  }
  else
  {
    // The frame counter interrupt flag is only cleared when transitioning from a put cycle to a get cycle.
    if (sequencer_irq_clear_pending)
    {
      sequencer_irq_clear_pending = false;
      sequencer_irq_flag = false;
    }
  }

  // we need to predict if there will be a length clock here, because the sequencer ticks last, but the
  // timer reload shouldn't happen if length clock and write happen simultaneously
  // I'm not sure if we can avoid this by simply processing the sequencer first
  // but at the moment that would break everything, so this is good enough for now
  if ((sequencer_counter == sequencer_check_1) || (sequencer_counter == sequencer_check_2))
  {
    len_clock_active = true;
  }

  // writes on the same cycle as reload disable IRQ if it is set, so put this here before writes
  if (dmc_reload_countdown > 0)
  {
    dmc_reload_countdown--;
    if (dmc_reload_countdown == 0)
    {
      dmc.sample_length--;

      if (dmc.sample_length == 0)
      {
        if (dmc.loop_flag)
        {
          dmc.sample_address = dmc.user_address;
          dmc.sample_length = dmc.user_length;
        }
        else if (dmc.irq_enabled && !dmc_irq_glitch) { dmc_irq = true; }
      }

      if (dmc.pending_disable)
      {
        dmc.sample_length = 0;
        dmc.pending_disable = false;
      }

      dmc_irq_glitch = false;
    }
  }

  // handle writes
  // notes: this set up is a bit convoluded at the moment, mainly because APU behaviour is not entirely understood
  // in partiuclar, there are several clock pulses affecting the APU, and when new written are latched is not known in detail
  // the current code simply matches known behaviour
  if (pending_reg != -1)
  {
    if (pending_reg == 0x4003 || pending_reg == 0x4007 || pending_reg == 0x4010 || pending_reg == 0x4015 || pending_reg == 0x4017)
    {
      _WriteReg(pending_reg, pending_val);
      pending_reg = -1;
    }
    else if (dmc.timer % 2 == 0)
    {
      _WriteReg(pending_reg, pending_val);
      pending_reg = -1;
    }
  }

  len_clock_active = false;

  sequencer_tick();
  sequencer_write_tick(seq_val);
  doing_tick_quarter = false;

  if (sequencer_irq_assert > 0)
  {
    sequencer_irq_assert--;
    if (sequencer_irq_assert == 0)
    {
      sequencer_irq = true;
    }
  }

  SyncIRQ();
  nes->_irq_apu = irq_pending;

  // since the units run concurrently, the APU frame sequencer is ran last because
  // it can change the output values of the pulse/triangle channels
  // we want the changes to affect it on the *next* cycle.
  if (!sequencer_irq_flag)
    sequencer_irq = false;

  // (DebugCallback dropped)
}

// ======================= PPU bodies (PPU.cs / PPU.regs.cs / PPU.run.cs) =======================

//when the ppu issues a write it goes through here and into the game board
inline void PPU::ppubus_write(int addr, uint8_t value)
{
  if (ppur.status.sl >= 241 || !PPUON())
    nes->board->AddressPpu(addr);

  nes->board->WritePpu(addr, value);
}

//when the ppu issues a read it goes through here and into the game board
inline uint8_t PPU::ppubus_read(int addr, bool ppu, bool addr_ppu)
{
  //hardware doesnt touch the bus when the PPU is disabled
  if (!PPUON() && ppu)
    return 0xFF;

  if (addr_ppu)
    nes->board->AddressPpu(addr);

  return nes->board->ReadPpu(addr);
}

//debug tools peek into the ppu through this
inline uint8_t PPU::ppubus_peek(int addr)
{
  return nes->board->PeekPPU(addr);
}

inline void PPU::runppu()
{
  //run one ppu cycle at a time so we can interact with the ppu and clockPPU at high granularity
  if (install_2006 > 0)
  {
    install_2006--;
    if (install_2006 == 0)
    {
      if (!race_2006) { ppur.install_latches(); }
      else { race_2006_2 = true; }

      //normally the address isnt observed by the board till it gets clocked by a read or write.
      //but maybe that's just because a ppu read/write shoves it on the address bus
      //apparently this shoves it on the address bus, too, or else blargg's mmc3 tests don't pass
      //ONLY if the ppu is not rendering
      if (ppur.status.sl >= 241 || !PPUON())
        nes->board->AddressPpu(ppur.get_2007access());
    }
  }

  race_2006 = false;

  if (install_2001 > 0)
  {
    install_2001--;
    if (install_2001 == 0)
    {
      show_bg_new = reg_2001.show_bg;
      show_obj_new = reg_2001.show_obj;
    }
  }

  ppur.status.cycle++;
  is_even_cycle = !is_even_cycle;

  if (ppur.status.cycle >= 257 && ppur.status.cycle <= 320 && ppur.status.sl <= 240 && PPUON())
  {
    reg_2003 = 0;
  }

  // Here we execute a CPU instruction if enough PPU cycles have passed
  // also do other things that happen at instruction level granularity
  // (NTSC build: cpu_sequence is {3,3,3,3,3}, so compare against the constant instead of the
  // table; cpu_step keeps cycling for state compatibility)
  cpu_stepcounter++;
  if (cpu_stepcounter == 3)
  {
    cpu_step++;
    if (cpu_step == 5) cpu_step = 0;
    cpu_stepcounter = 0;

    // this is where the CPU instruction is called
    nes->RunCpuOne();

    // decay the ppu bus, approximating real behaviour
    PpuOpenBusDecay(DecayType::None);

    // Check for NMIs
    if (NMI_PendingInstructions > 0)
    {
      NMI_PendingInstructions--;
      if (NMI_PendingInstructions <= 0)
      {
        nes->cpu->NMI = true;
      }
    }
  }

  if (HasClockPPU)
  {
    nes->board->ClockPpu();
  }
  _totalCycles += 1;
  // (cpu->ext_ppu_cycle is not incremented per cycle anymore: it is only observed at frame
  // boundaries, so FrameAdvance materializes it from _totalCycles at frame end)
}

//VRAM data register (r/w) (PPU.regs.cs)
inline void PPU::write_2007(uint8_t value)
{
  //does this take 4x longer? nestopia indicates so perhaps...

  int addr = ppur.get_2007access();
  if (ppuphase == PPU_PHASE_BG)
  {
    if (show_bg_new)
    {
      addr = ppur.get_ntread();
    }
  }

  if ((addr & 0x3F00) == 0x3F00)
  {
    //handle palette. this is being done nestopia style, because i found some documentation for it (appendix 1)
    addr &= 0x1F;
    uint8_t color = (uint8_t)(value & 0x3F); //are these bits really unwired? can they be read back somehow?

    PALRAM[addr] = color;
    if ((addr & 3) == 0)
    {
      PALRAM[addr ^ 0x10] = color;
    }
  }
  else
  {
    addr &= 0x3FFF;

    ppubus_write(addr, value);
  }

  ppur.increment2007(ppur.status.rendering() && PPUON(), (int)reg_2000.vram_incr32 != 0);

  //see comments in $2006
  if (ppur.status.sl >= 241 || !PPUON())
    nes->board->AddressPpu(ppur.get_2007access());
}

inline uint8_t PPU::read_2007()
{
  int addr = ppur.get_2007access() & 0x3FFF;
  int bus_case = 0;
  //ordinarily we return the buffered values
  uint8_t ret = VRAMBuffer;

  //in any case, we read from the ppu bus
  VRAMBuffer = ppubus_read(addr, false, false);

  //but reads from the palette are implemented in the PPU and return immediately
  if ((addr & 0x3F00) == 0x3F00)
  {
    ret = (uint8_t)(PALRAM[addr & 0x1F] + ((uint8_t)(ppu_open_bus & 0xC0)));
    bus_case = 1;
  }

  ppur.increment2007(ppur.status.rendering() && PPUON(), (int)reg_2000.vram_incr32 != 0);

  //see comments in $2006
  if (ppur.status.sl >= 241 || !PPUON())
    nes->board->AddressPpu(ppur.get_2007access());

  // update open bus here
  ppu_open_bus = ret;
  if (bus_case == 0)
  {
    PpuOpenBusDecay(DecayType::All);
  }
  else
  {
    PpuOpenBusDecay(DecayType::Low);
  }

  return ret;
}

inline uint8_t PPU::ReadReg(int addr)
{
  PpuOpenBusDecayFlush(); // owed decay steps predate this access
  uint8_t ret_spec;
  switch (addr)
  {
    case 0: return read_2000(); // (VS 2c05 swap dropped)
    case 1: return read_2001();
    case 2: return read_2002();
    case 3: return read_2003();
    case 4: return read_2004();
    case 5: return read_2005();
    case 6: return read_2006();
    case 7:
    {
      if (nes->cpu->TotalExecutedCycles == double_2007_read && !nes->dmc_dma_exec)
      {
        return ppu_open_bus;
      }
      else
      {
        ret_spec = read_2007();
        double_2007_read = nes->cpu->TotalExecutedCycles + 1;
      }
      return ret_spec;
    }
    default: __builtin_trap();
  }
}

// (PPU.run.cs)
inline void PPU::TickPPU_VBL()
{
  if (ppur.status.cycle == 0 && ppur.status.sl == 241 + preNMIlines)
  {
    nmi_destiny = reg_2000.vblank_nmi_gen && Reg2002_vblank_active;
    if (cpu_stepcounter == 2) { NMI_offset = 1; }
    else if (cpu_stepcounter == 1) { NMI_offset = 2; }
    else { NMI_offset = 0; }
  }
  else if (ppur.status.cycle <= 2 && nmi_destiny)
  {
    nmi_destiny &= reg_2000.vblank_nmi_gen && Reg2002_vblank_active;
  }
  else if (ppur.status.cycle == (3 + NMI_offset) && ppur.status.sl == 241 + preNMIlines)
  {
    if (nmi_destiny) { nes->cpu->NMI = true; }
    nes->board->AtVsyncNmi();
  }

  if (ppur.status.cycle == 340)
  {
    if (ppur.status.sl == 241 + preNMIlines + postNMIlines - 1)
    {
      Reg2002_vblank_clear_pending = true;
      idleSynch = !idleSynch;
      Reg2002_objhit = Reg2002_objoverflow = 0;
    }
  }

  runppu(); // note cycle ticks inside runppu

  if (ppur.status.cycle == 341)
  {
    if (Reg2002_vblank_clear_pending)
    {
      Reg2002_vblank_active = 0;
      Reg2002_vblank_clear_pending = false;
    }

    ppur.status.cycle = 0;
    ppur.status.sl++;
    if (ppur.status.sl == 241 + preNMIlines + postNMIlines)
    {
      do_vbl = false;
      ppur.status.sl = 0;
      do_active_sl = true;
    }
  }
}

inline void PPU::TickPPU_active()
{
  if (ppur.status.cycle < 256)
  {
    if (ppur.status.cycle == 0)
    {
      ppur.status.cycle = 0;

      spr_true_count = 0;
      soam_index = 0;
      oam_index_aux = 0;
      oam_index = 0;
      is_even_cycle = true;
      sprite_eval_write = true;
      sprite_zero_go = sprite_zero_in_range;

      sprite_zero_in_range = false;

      yp = ppur.status.sl - 1;
      ppuphase = PPU_PHASE_BG;

      // "If PPUADDR is not less then 8 when rendering starts, the first 8 bytes in OAM are written to from
      // the current location of PPUADDR"
      if (ppur.status.sl == 0 && PPUON() && reg_2003 >= 8 && _region == Region::NTSC)
      {
        for (int i = 0; i < 8; i++)
        {
          OAM[i] = OAM[(reg_2003 & 0xF8) + i];
        }
      }

      // (NTView/PPUView debug callbacks dropped)

      // set up intial values to use later
      yp_shift = yp << 8;
      xt = 0;
      xp = 0;

      sprite_eval_cycle = 0;

      xstart = xt << 3;
      target = yp_shift + xstart;
      rasterpos = xstart;

      spriteHeight = reg_2000.obj_size_16 ? 16 : 8;

      //check all the conditions that can cause things to render in these 8px
      renderspritenow = show_obj_new && (xt > 0 || reg_2001.show_obj_leftmost);
    }

    if (ppur.status.sl != 0)
    {
      /////////////////////////////////////////////
      // Sprite Evaluation Start
      /////////////////////////////////////////////

      if (sprite_eval_cycle < 64)
      {
        // the first 64 cycles of each scanline are used to initialize sceondary OAM
        // the actual effect setting a flag that always returns 0xFF from a OAM read
        // this is a bit of a shortcut to save some instructions
        // data is read from OAM as normal but never used
        if (!is_even_cycle)
        {
          soam[soam_index] = 0xFF;
          soam_index++;
        }
      }
      // otherwise, scan through OAM and test if sprites are in range
      // if they are, they get copied to the secondary OAM
      else
      {
        if (sprite_eval_cycle == 64)
        {
          soam_index = 0;
          oam_index = reg_2003;
        }

        if (oam_index >= 256)
        {
          oam_index = 0;
          sprite_eval_write = false;
        }

        if (is_even_cycle)
        {
          read_value = OAM[oam_index & 0xFF];
        }
        else if (sprite_eval_write)
        {
          //look for sprites
          if (spr_true_count == 0 && soam_index < 8)
          {
            soam[soam_index * 4] = read_value;
          }

          if (soam_index < 8)
          {
            if (yp >= read_value && yp < read_value + spriteHeight && spr_true_count == 0)
            {
              //a flag gets set if sprite zero is in range
              if (oam_index == reg_2003) { sprite_zero_in_range = true; }
              oam_index++;
              spr_true_count++;
            }
            else if (spr_true_count > 0 && spr_true_count < 4)
            {
              soam[soam_index * 4 + spr_true_count] = read_value;

              oam_index++;
              spr_true_count++;
              if (spr_true_count == 4)
              {
                soam_index++;
                // The X coordinate makes the same checks as the Y position to see if "read_value" is in range of the scanline.
                if (!(yp >= read_value && yp < read_value + spriteHeight) && spr_true_count == 4)
                {
                  // and if it isn't, clear the lower 2 bits of the OAM index.
                  oam_index &= 0x1FC; // This is an integer instead of a byte. Bit 8 is used to check if the OAM address overflows.
                }
                if (soam_index == 8)
                {
                  // oam_index could be pathologically misaligned at this point, so we have to find the next
                  // nearest actual sprite to work on >8 sprites per scanline option
                  oam_index_aux = (oam_index % 4) * 4;
                }

                spr_true_count = 0;
              }
            }
            else
            {
              // This object is out of the range of the scanline
              oam_index += 4;
              oam_index &= 0x1FC;
            }
          }
          else
          {
            if (yp >= read_value && yp < read_value + spriteHeight && PPUON())
            {
              Reg2002_objoverflow = true;
            }

            if (yp >= read_value && yp < read_value + spriteHeight && spr_true_count == 0)
            {
              spr_true_count++;
              oam_index++;
            }
            else if (spr_true_count > 0 && spr_true_count < 4)
            {
              oam_index++;
              spr_true_count++;
              if (spr_true_count == 4)
              {
                if (!(yp >= read_value && yp < read_value + spriteHeight) && spr_true_count == 4)
                {
                  oam_index &= 0x1FC;
                }
                // soam_index no longer increments now that Secondary OAM is full.
                spr_true_count = 0;
              }
            }
            else
            {
              // glitchy increments
              oam_index += 1;

              if ((oam_index & 3) != 0)
              {
                oam_index += 4;
              }
            }

            read_value = soam[(soam_index * 4) & 0x1F]; //writes change to reads
          }
        }
        else
        {
          // if we don't write sprites anymore, just scan through the oam
          read_value = soam[(soam_index * 4) & 0x1F];
          oam_index += 4;
        }
      }

      /////////////////////////////////////////////
      // Sprite Evaluation End
      /////////////////////////////////////////////

      int pixel = 0;
#ifndef NESHAWK_HEADLESS
      int pixelcolor = PALRAM[pixel];
#endif

      //process the current clock's worth of bg data fetching
      //this needs to be split into 8 pieces or else exact sprite 0 hitting wont work
      // due to the cpu not running while the sprite renders below
      if (PPUON()) { Read_bgdata(xp, xt + 2); }
      //according to qeed's doc, use palette 0 or $2006's value if it is & 0x3Fxx
      //at one point I commented this out to fix bottom-left garbage in DW4. but it's needed for full_nes_palette.
      //solution is to only run when PPU is actually OFF (left-suppression doesnt count)
#ifndef NESHAWK_HEADLESS
      else
      {
        // if there's anything wrong with how we're doing this, someone please chime in
        int addr = ppur.get_2007access();
        if ((addr & 0x3F00) == 0x3F00)
        {
          pixel = addr & 0x1F;
        }
        pixelcolor = PALRAM[pixel];
        pixelcolor |= 0x8000;
      }
#endif

      //generate the BG data (headless: the BG pixel is still needed for sprite-0 hit below)
      if (show_bg_new && (xt > 0 || reg_2001.show_bg_leftmost))
      {
        int bgtile = (rasterpos + ppur.fh) >> 3;
        uint8_t pt_0 = bgdata[bgtile].pt_0;
        uint8_t pt_1 = bgdata[bgtile].pt_1;
        int sel = 7 - (rasterpos + ppur.fh) & 7;
        pixel = ((pt_0 >> sel) & 1) | (((pt_1 >> sel) & 1) << 1);
        if (pixel != 0)
          pixel |= bgdata[bgtile].at;
#ifndef NESHAWK_HEADLESS
        pixelcolor = PALRAM[pixel];
#endif
      }

#ifndef NESHAWK_HEADLESS
      if (!nesSettings::DispBackground)
        pixelcolor = 0x8000;
#endif

      //check if the pixel has a sprite in it
      if (sl_sprites[256 + xt * 8 + xp] != 0 && renderspritenow)
      {
        int s = sl_sprites[xt * 8 + xp];

        //TODO - make sure we don't trigger spritehit if the edges are masked for either BG or OBJ
        //spritehit:
        //1. is it sprite#0?
        //2. is the bg pixel nonzero?
        //then, it is spritehit.
        Reg2002_objhit = (int)Reg2002_objhit | (int)(sprite_zero_go && s == 0 && pixel != 0 && rasterpos < 255 && show_bg_new && show_obj_new);
#ifndef NESHAWK_HEADLESS
        int spixel = sl_sprites[256 + xt * 8 + xp];
        int temp_attr = sl_sprites[512 + xt * 8 + xp];
        //priority handling, if in front of BG:
        bool drawsprite = !(((temp_attr & 0x20) != 0) && ((pixel & 3) != 0));
        if (drawsprite && nesSettings::DispSprites)
        {
          //bring in the palette bits and palettize
          spixel |= (temp_attr & 3) << 2;
          //save it for use in the framebuffer
          pixelcolor = PALRAM[0x10 + spixel];
        }
#endif
      } //oamcount loop

      runppu();

      if (xp == 6 && PPUON())
      {
        ppu_was_on = true;
        if (ppur.status.cycle == 255) { race_2006 = true; }
      }

      if (xp == 7 && PPUON())
      {
        ppur.increment_hsc();

        if ((ppur.status.cycle == 256) && ppu_was_on)
        {
          ppur.increment_vs();
        }

        if (race_2006_2)
        {
          if (ppur.status.cycle == 256)
          {
            ppur.fv &= ppur._fv;
            ppur.v &= ppur._v;
            ppur.h &= ppur._h;
            ppur.vt &= ppur._vt;
            ppur.ht &= ppur._ht;
          }
          else
          {
            ppur.fv = ppur._fv;
            ppur.v = ppur._v;
            ppur.h &= ppur._h;
            ppur.vt = ppur._vt;
            ppur.ht &= ppur._ht;
          }
        }

        ppu_was_on = false;
      }

      race_2006_2 = false;

#ifndef NESHAWK_HEADLESS
      pipeline(pixelcolor, xt * 8 + xp);
#endif
      target++;

      // clear out previous sprites from scanline buffer
      sl_sprites[256 + xt * 8 + xp] = 0;

      // end of visible part of the scanline
      sprite_eval_cycle++;
      xp++;
      rasterpos++;

      if (xp == 8)
      {
        xp = 0;
        xt++;

        xstart = xt << 3;
        target = yp_shift + xstart;
        rasterpos = xstart;

        spriteHeight = reg_2000.obj_size_16 ? 16 : 8;

        //check all the conditions that can cause things to render in these 8px
        renderspritenow = show_obj_new && (xt > 0 || reg_2001.show_obj_leftmost);
      }

      if (ppur.status.cycle > 63)
      {
        if (ppu_was_on_spr && !PPUON())
        {
          reg_2003++;
        }
      }

      ppu_was_on_spr = PPUON();
    }
    else
    {
      // if scanline is the pre-render line, we just read BG data
      Read_bgdata(xp, xt + 2);

      runppu();

      if (xp == 6 && PPUON())
      {
        ppu_was_on = true;
        if (ppur.status.cycle == 255) { race_2006 = true; }
      }

      if (xp == 7 && PPUON())
      {
        ppur.increment_hsc();

        if ((ppur.status.cycle == 256) && ppu_was_on)
        {
          ppur.increment_vs();
        }

        if (race_2006_2)
        {
          if (ppur.status.cycle == 256)
          {
            ppur.fv &= ppur._fv;
            ppur.v &= ppur._v;
            ppur.h &= ppur._h;
            ppur.vt &= ppur._vt;
            ppur.ht &= ppur._ht;
          }
          else
          {
            ppur.fv = ppur._fv;
            ppur.v = ppur._v;
            ppur.h &= ppur._h;
            ppur.vt = ppur._vt;
            ppur.ht &= ppur._ht;
          }
        }

        ppu_was_on = false;
      }

      race_2006_2 = false;

      xp++;

      if (xp == 8)
      {
        xp = 0;
        xt++;
      }
    }
  }
  else if (ppur.status.cycle < 320)
  {
    // after we are done with the visible part of the frame, we reach sprite transfer to temp OAM tables and such
    if (ppur.status.cycle == 256)
    {
      // do the more then 8 sprites stuff here where it is convenient
      // normally only 8 sprites are allowed, but with a particular setting we can have more then that
      // this extra bit takes care of it quickly
      soam_index_aux = 8;

      if (nesSettings::AllowMoreThanEightSprites)
      {
        while (oam_index_aux < 64 && soam_index_aux < 64)
        {
          //look for sprites
          soam[soam_index_aux * 4] = OAM[oam_index_aux * 4];
          if (yp >= OAM[oam_index_aux * 4] && yp < OAM[oam_index_aux * 4] + spriteHeight)
          {
            soam[soam_index_aux * 4 + 1] = OAM[oam_index_aux * 4 + 1];
            soam[soam_index_aux * 4 + 2] = OAM[oam_index_aux * 4 + 2];
            soam[soam_index_aux * 4 + 3] = OAM[oam_index_aux * 4 + 3];
            soam_index_aux++;
            oam_index_aux++;
          }
          else
          {
            oam_index_aux++;
          }
        }
      }

      soam_index_prev = soam_index_aux;

      if (soam_index_prev > 8 && !nesSettings::AllowMoreThanEightSprites)
        soam_index_prev = 8;

      ppuphase = PPU_PHASE_OBJ;

      spriteHeight = reg_2000.obj_size_16 ? 16 : 8;

      s = 0;
      ppu_aux_index = 0;

      junksprite = !PPUON();

      t_oam[s].oam_y = soam[s * 4];
      t_oam[s].oam_ind = soam[s * 4 + 1];
      t_oam[s].oam_attr = soam[s * 4 + 2];
      t_oam[s].oam_x = soam[s * 4 + 3];

      line = yp - t_oam[s].oam_y;
      if ((t_oam[s].oam_attr & 0x80) != 0) //vflip
        line = spriteHeight - line - 1;

      patternNumber = t_oam[s].oam_ind;
    }

    switch (ppu_aux_index)
    {
      case 0:
        //8x16 sprite handling:
        if (reg_2000.obj_size_16)
        {
          int bank = (patternNumber & 1) << 12;
          patternNumber &= ~1;
          patternNumber |= (line >> 3) & 1;
          patternAddress = (patternNumber << 4) | bank;
        }
        else
          patternAddress = (patternNumber << 4) | ((int)reg_2000.obj_pattern_hi << 12);

        //offset into the pattern for the current line.
        //tricky: tall sprites have already had lines>8 taken care of by getting a new pattern number above.
        //so we just need the line offset for the second pattern
        patternAddress += line & 7;

        ppubus_read(ppur.get_ntread(), true, true);

        read_value = t_oam[s].oam_y;
        runppu();
        break;
      case 1:
        if (ppur.status.sl == 0 && ppur.status.cycle == 305 && PPUON())
        {
          ppur.install_latches();

          read_value = t_oam[s].oam_ind;
          runppu();
        }
        else if ((ppur.status.sl != 0) && ppur.status.cycle == 257 && PPUON())
        {
          if (target <= 61441 && target > 0 && s == 0)
          {
            pipeline(0, 256);   //  last pipeline call option 1 of 2
            target++;
          }

          //at 257: 3d world runner is ugly if we do this at 256
          if (PPUON()/* && !race_2006_2*/) { ppur.install_h_latches(); }
          race_2006_2 = false;
          read_value = t_oam[s].oam_ind;
          runppu();
        }
        else
        {
          if (target <= 61441 && target > 0 && s == 0)
          {
            pipeline(0, 256);  //  last pipeline call option 2 of 2
            target++;
          }

          read_value = t_oam[s].oam_ind;
          runppu();
        }
        break;

      case 2:
        ppubus_read(ppur.get_atread(), true, true); //at or nt?
        read_value = t_oam[s].oam_attr;
        runppu();
        break;

      case 3:
        read_value = t_oam[s].oam_x;
        runppu();
        break;

      case 4:
        // if the PPU is off, we don't put anything on the bus
        if (junksprite)
        {
          ppubus_read(patternAddress, true, false);
          runppu();
        }
        else
        {
          temp_addr = patternAddress;
          t_oam[s].patterns_0 = ppubus_read(temp_addr, true, true);
          read_value = t_oam[s].oam_x;
          runppu();
        }
        break;
      case 5:
        runppu();
        break;
      case 6:
        // if the PPU is off, we don't put anything on the bus
        if (junksprite)
        {
          ppubus_read(patternAddress, true, false);
          runppu();
        }
        else
        {
          temp_addr += 8;
          t_oam[s].patterns_1 = ppubus_read(temp_addr, true, true);
          read_value = t_oam[s].oam_x;
          runppu();
        }
        break;
      case 7:
        // if the PPU is off, we don't put anything on the bus
        if (junksprite)
        {
          runppu();
        }
        else
        {
          runppu();

          // hflip
          if ((t_oam[s].oam_attr & 0x40) == 0)
          {
            t_oam[s].patterns_0 = BitReverse::Byte8()[t_oam[s].patterns_0];
            t_oam[s].patterns_1 = BitReverse::Byte8()[t_oam[s].patterns_1];
          }

          // if the sprites attribute is 0xFF, then this indicates a non-existent sprite
          // I think the logic here is that bits 2-4 in OAM are disabled, but soam is initialized with 0xFF
          // so the only way a sprite could have an 0xFF attribute is if it is not in the scope of the scanline
          if (t_oam[s].oam_attr == 0xFF)
          {
            t_oam[s].patterns_0 = 0;
            t_oam[s].patterns_1 = 0;
          }
        }
        break;
    }

    ppu_aux_index++;
    if (ppu_aux_index == 8)
    {
      // now that we have a sprite, we can fill in the next scnaline's sprite pixels with it
      // this saves quite a bit of processing compared to checking each pixel

      if (s < soam_index_prev && (ppur.status.sl != 0) && (ppur.status.sl != 240))
      {
        int temp_x = t_oam[s].oam_x;
        for (int i = 0; (temp_x + i) < 256 && i < 8; i++)
        {
          if (sl_sprites[256 + temp_x + i] == 0)
          {
            if (((t_oam[s].patterns_0 >> i) & 1) != 0 || ((t_oam[s].patterns_1 >> i) & 1) != 0)
            {
              int spixel = ((t_oam[s].patterns_0 >> i) & 1) != 0 ? 1 : 0;
              spixel |= (((t_oam[s].patterns_1 >> i) & 1) != 0 ? 2 : 0);

              sl_sprites[temp_x + i] = (uint8_t)s;
              sl_sprites[256 + temp_x + i] = (uint8_t)spixel;
              sl_sprites[512 + temp_x + i] = t_oam[s].oam_attr;
            }
          }
        }
      }

      ppu_aux_index = 0;
      s++;

      if (s < 8)
      {
        junksprite = !PPUON();

        t_oam[s].oam_y = soam[s * 4];
        t_oam[s].oam_ind = soam[s * 4 + 1];
        t_oam[s].oam_attr = soam[s * 4 + 2];
        t_oam[s].oam_x = soam[s * 4 + 3];

        line = yp - t_oam[s].oam_y;
        if ((t_oam[s].oam_attr & 0x80) != 0) //vflip
          line = spriteHeight - line - 1;

        patternNumber = t_oam[s].oam_ind;
      }
      else
      {
        // repeat all the above steps for more then 8 sprites but don't run any cycles
        // (soam_index_aux is always 8 here: AllowMoreThanEightSprites is false)
      }
    }
  }
  else
  {
    if (ppur.status.cycle < 336)
    {
      if (ppur.status.cycle == 320)
      {
        ppuphase = PPU_PHASE_BG;
        xt = 0;
        xp = 0;
      }

      // if scanline is the pre-render line, we just read BG data
      Read_bgdata(xp, xt);

      runppu();

      if (xp == 6 && PPUON())
      {
        ppu_was_on = true;
      }

      if (xp == 7 && PPUON())
      {
        if (!race_2006)
          ppur.increment_hsc();

        if (ppur.status.cycle == 256 && !race_2006)
          ppur.increment_vs();

        ppu_was_on = false;
      }

      xp++;

      if (xp == 8)
      {
        xp = 0;
        xt++;
      }
    }
    else if (ppur.status.cycle < 340)
    {
      if (ppur.status.cycle == 339)
      {
        evenOddDestiny = PPUON();
      }

      runppu();
    }
    else
    {
      // After memory access 170, the PPU simply rests for 4 cycles (or the
      // equivelant of half a memory access cycle) before repeating the whole
      // pixel/scanline rendering process. If the scanline being rendered is the very
      // first one on every second frame, then this delay simply doesn't exist.
      if (ppur.status.sl == 0 && idleSynch && evenOddDestiny && chopdot)
      { ppur.status.cycle++; } // increment cycle without running ppu
      else
      { runppu(); }

      ppur.status.cycle = 0;
      ppur.status.sl++;

      if (ppur.status.sl == 241)
      {
        do_active_sl = false;
        do_pre_vbl = true;
      }
    }
  }
}

inline void PPU::TickPPU_preVBL()
{
  if ((ppur.status.cycle == 340) && (ppur.status.sl == 241 + preNMIlines - 1))
  {
    Reg2002_vblank_active_pending = true;
  }

  runppu();

  if (ppur.status.cycle == 341)
  {
    ppur.status.cycle = 0;
    ppur.status.sl++;

    if (ppur.status.sl == 241 + preNMIlines)
    {
      if (Reg2002_vblank_active_pending)
      {
        Reg2002_vblank_active = 1;
        Reg2002_vblank_active_pending = false;
      }

      do_pre_vbl = false;
      do_vbl = true;

      ppu_init_frame();
      nes->frame_is_done = true;
    }
  }
}

//not quite emulating all the NES power up behavior
//since it is known that the NES ignores writes to some
//register before around a full frame, but no games
//should write to those regs during that time, it needs
//to wait for vblank
inline void PPU::NewDeadPPU()
{
  if (ppur.status.cycle == 241 * 341 - start_up_offset - 1)
  {
    Reg2002_vblank_active_pending = true;
  }

  runppu();

  if (ppur.status.cycle == 241 * 341 - start_up_offset)
  {
    if (Reg2002_vblank_active_pending)
    {
      Reg2002_vblank_active = 1;
      Reg2002_vblank_active_pending = false;
    }

    ppudead--;

    ppu_init_frame();

    do_vbl = true;

    nes->frame_is_done = true;
  }
}

} // namespace nesHawk
