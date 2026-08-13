// Savestate support for the translated NesHawk core.
//
// The field lists mirror the C# SyncState methods (NES.IStatable.cs, MOS6502X.cs SyncState,
// NesBoardBase/UxROM SyncState, PPU.cs SyncState, APU.cs SyncState, ControllerNES.SyncState) and
// then go further: EVERY mutable field is synced, including the ones C# omits (e.g.
// double_controller_read, joypadStrobed, pixelcolor_latch_1). C# omits them for savestate
// size/compat -- harmless in an interactive emulator -- but a search engine restores states across
// unrelated branches constantly, so any unsynced mutable field would leak state between branches
// and break determinism. The one deliberate exclusion is the PPU framebuffer xbuf (120KB,
// video-only, no feedback into emulation state).
//
// Constants/pointers (Rom, tables, cpu_sequence, wiring) are not synced.
//
// Usage:
//   StateSaver s(buf);  syncNesState(nes, s);   // save (or size with buf == nullptr)
//   StateLoader l(buf); syncNesState(nes, l);   // load

#pragma once

#include <cstdint>
#include <cstring>
#include "nes.hpp"

namespace nesHawk
{

class StateSaver
{
  public:

  // buf == nullptr -> dry run, only measures size
  StateSaver(uint8_t* buf) : _buf(buf) {}

  template <typename T>
  void operator()(T& v)
  {
    static_assert(std::is_trivially_copyable<T>::value, "sync fields must be trivially copyable");
    if (_buf != nullptr) memcpy(_buf + _pos, &v, sizeof(T));
    _pos += sizeof(T);
  }

  void bytes(void* p, size_t n)
  {
    if (_buf != nullptr) memcpy(_buf + _pos, p, n);
    _pos += n;
  }

  size_t size() const { return _pos; }

  private:

  uint8_t* _buf;
  size_t _pos = 0;
};

class StateLoader
{
  public:

  StateLoader(const uint8_t* buf) : _buf(buf) {}

  template <typename T>
  void operator()(T& v)
  {
    static_assert(std::is_trivially_copyable<T>::value, "sync fields must be trivially copyable");
    memcpy(&v, _buf + _pos, sizeof(T));
    _pos += sizeof(T);
  }

  void bytes(void* p, size_t n)
  {
    memcpy(p, _buf + _pos, n);
    _pos += n;
  }

  size_t size() const { return _pos; }

  private:

  const uint8_t* _buf;
  size_t _pos = 0;
};

namespace detail
{

// MOS6502X SyncState (all mutable fields; BCD_Enabled is configuration)
template <typename Op, typename TLink>
void syncCpu(MOS6502X<TLink>& cpu, Op& op)
{
  op(cpu.A);
  op(cpu.X);
  op(cpu.Y);
  op(cpu.P);
  op(cpu.PC);
  op(cpu.S);
  op(cpu.NMI);
  op(cpu.IRQ);
  op(cpu.RDY);
  op(cpu.TotalExecutedCycles);
  op(cpu.opcode);
  op(cpu.opcode2);
  op(cpu.opcode3);
  op(cpu.ea);
  op(cpu.alu_temp);
  op(cpu.mi);
  op(cpu.iflag_pending);
  op(cpu.interrupt_pending);
  op(cpu.branch_irq_hack);
  op(cpu.rdy_freeze);
  op(cpu.ext_ppu_cycle);
  op(cpu.H);
  op(cpu.address_bus);
}

template <typename Op>
void syncBit(Bit& b, Op& op)
{
  op(b._val);
}

template <typename Op>
void syncPpu(PPU& ppu, Op& op)
{
  // materialize any batched open-bus decay so ppu_open_bus / the timers are canonical (save),
  // and so pending is 0 before the timers get overwritten (load)
  ppu.PpuOpenBusDecayFlush();

  op(ppu.cpu_step);
  op(ppu.cpu_stepcounter);
  op(ppu.ppudead);
  op(ppu.idleSynch);
  op(ppu.NMI_PendingInstructions);
  op(ppu.PPUGenLatch);
  op(ppu.vtoggle);
  op(ppu.VRAMBuffer);
  op(ppu.ppu_addr_temp);

  op(ppu.spr_true_count);
  op(ppu.sprite_eval_write);
  op(ppu.read_value);
  op(ppu.soam_index_prev);
  op(ppu.sprite_zero_go);
  op(ppu.sprite_zero_in_range);
  op(ppu.is_even_cycle);
  op(ppu.soam_index);
  op(ppu.oam_index);
  op(ppu.oam_index_aux);
  op(ppu.soam_index_aux);
  op(ppu.yp);
  op(ppu.target);
  op(ppu.ppu_was_on);
  op(ppu.ppu_was_on_spr);
  op(ppu.spriteHeight);
  op(ppu.install_2006);
  op(ppu.race_2006);
  op(ppu.race_2006_2);
  op(ppu.install_2001);
  op(ppu.show_bg_new);
  op(ppu.show_obj_new);

  op(ppu.ppu_open_bus);
  op(ppu.double_2007_read);
  op.bytes(ppu.ppu_open_bus_decay_timer, sizeof(ppu.ppu_open_bus_decay_timer));
  op.bytes(ppu.glitchy_reads_2003, sizeof(ppu.glitchy_reads_2003));

  op.bytes(ppu.OAM, sizeof(ppu.OAM));
  op.bytes(ppu.soam, sizeof(ppu.soam));
  op.bytes(ppu.PALRAM, sizeof(ppu.PALRAM));
  op(ppu.ppuphase);

  syncBit(ppu.Reg2002_objoverflow, op);
  syncBit(ppu.Reg2002_objhit, op);
  syncBit(ppu.Reg2002_vblank_active, op);
  op(ppu.Reg2002_vblank_active_pending);
  op(ppu.Reg2002_vblank_clear_pending);

  // PPUREGS.SyncState
  op(ppu.ppur.fv);
  op(ppu.ppur.v);
  op(ppu.ppur.h);
  op(ppu.ppur.vt);
  op(ppu.ppur.ht);
  op(ppu.ppur._fv);
  op(ppu.ppur._v);
  op(ppu.ppur._h);
  op(ppu.ppur._vt);
  op(ppu.ppur._ht);
  op(ppu.ppur.fh);
  op(ppu.ppur.status.cycle);
  op(ppu.ppur.status.sl);

  // reg_2000/reg_2001 synced via Value in C#; field-wise here (same information)
  syncBit(ppu.reg_2000.vram_incr32, op);
  syncBit(ppu.reg_2000.obj_pattern_hi, op);
  syncBit(ppu.reg_2000.bg_pattern_hi, op);
  syncBit(ppu.reg_2000.obj_size_16, op);
  syncBit(ppu.reg_2000.ppu_layer, op);
  syncBit(ppu.reg_2000.vblank_nmi_gen, op);
  syncBit(ppu.reg_2001.color_disable, op);
  syncBit(ppu.reg_2001.show_bg_leftmost, op);
  syncBit(ppu.reg_2001.show_obj_leftmost, op);
  syncBit(ppu.reg_2001.show_bg, op);
  syncBit(ppu.reg_2001.show_obj, op);
  syncBit(ppu.reg_2001.intense_green, op);
  syncBit(ppu.reg_2001.intense_blue, op);
  syncBit(ppu.reg_2001.intense_red, op);
  op(ppu.reg_2001.intensity_lsl_6);
  op(ppu.reg_2003);
  op(ppu.reg_2006_2);

  // (xbuf deliberately not synced -- video-only)

  op(ppu._totalCycles);

  op(ppu.do_vbl);
  op(ppu.do_active_sl);
  op(ppu.do_pre_vbl);

  op(ppu.nmi_destiny);
  op(ppu.evenOddDestiny);
  op(ppu.start_up_offset);
  op(ppu.NMI_offset);
  op(ppu.yp_shift);
  op(ppu.sprite_eval_cycle);
  op(ppu.xt);
  op(ppu.xp);
  op(ppu.xstart);
  op(ppu.rasterpos);
  op(ppu.renderspritenow);
  op(ppu.s);
  op(ppu.ppu_aux_index);
  op(ppu.junksprite);
  op(ppu.line);
  op(ppu.patternNumber);
  op(ppu.patternAddress);
  op(ppu.temp_addr);
  op.bytes(ppu.sl_sprites, sizeof(ppu.sl_sprites));

  op.bytes(ppu.bgdata, sizeof(ppu.bgdata));
  op.bytes(ppu.t_oam, sizeof(ppu.t_oam));

  // (pixelcolor_latch_1 not synced: video-only, and frozen under NESHAWK_HEADLESS -- C# doesn't
  // sync it either)

  // after a load the timers just changed; re-derive the batched-decay checkpoint (harmless on save)
  ppu.recomputeDecayCheck();
}

template <typename Op>
void syncPulse(APU::PulseUnit& u, Op& op)
{
  op(u.duty_cnt);
  op(u.env_loop);
  op(u.env_constant);
  op(u.env_cnt_value);
  op(u.len_halt);
  op(u.sweep_en);
  op(u.sweep_divider_cnt);
  op(u.sweep_negate);
  op(u.sweep_shiftcount);
  op(u.sweep_reload);
  op(u.len_cnt);
  op(u.timer_raw_reload_value);
  op(u.timer_reload_value);
  op(u.lenctr_en);
  op(u.swp_divider_counter);
  op(u.swp_silence);
  op(u.duty_step);
  op(u.timer_counter);
  op(u.sample);
  op(u.duty_value);
  op(u.env_start_flag);
  op(u.env_divider);
  op(u.env_counter);
  op(u.env_output);
}

template <typename Op>
void syncNoise(APU::NoiseUnit& u, Op& op)
{
  op(u.env_cnt_value);
  op(u.env_loop);
  op(u.env_constant);
  op(u.mode_cnt);
  op(u.period_cnt);
  op(u.len_halt);
  op(u.len_cnt);
  op(u.lenctr_en);
  op(u.shift_register);
  op(u.timer_counter);
  op(u.sample);
  op(u.env_output);
  op(u.env_start_flag);
  op(u.env_divider);
  op(u.env_counter);
  op(u.noise_bit);
}

template <typename Op>
void syncTriangle(APU::TriangleUnit& u, Op& op)
{
  op(u.linear_counter_reload);
  op(u.control_flag);
  op(u.timer_cnt);
  op(u.reload_flag);
  op(u.len_cnt);
  op(u.halt_2);
  op(u.lenctr_en);
  op(u.linear_counter);
  op(u.timer);
  op(u.timer_cnt_reload);
  op(u.seq);
  op(u.sample);
}

template <typename Op>
void syncDmc(APU::DMCUnit& u, Op& op)
{
  op(u.irq_enabled);
  op(u.loop_flag);
  op(u.timer_reload);
  op(u.timer);
  op(u.user_address);
  op(u.user_length);
  op(u.sample_address);
  op(u.sample_length);
  op(u.sample_buffer);
  op(u.sample_buffer_filled);
  op(u.out_shift);
  op(u.out_bits_remaining);
  op(u.out_deltacounter);
  op(u.out_silence);
  op(u.fill_glitch);
  op(u.fill_glitch_2);
  op(u.fill_glitch_2_end);
  op(u.timer_just_reloaded);
  op(u.pending_disable);
  op(u.delay);
}

template <typename Op>
void syncApu(APU& apu, Op& op)
{
  op(apu.irq_pending);
  op(apu.dmc_irq);
  op(apu.dmc_irq_glitch);
  op(apu.dmc_reload_countdown);
  op(apu.pending_reg);
  op(apu.pending_val);

  op(apu.sequencer_counter);
  op(apu.sequencer_step);
  op(apu.sequencer_mode);
  op(apu.sequencer_irq_inhibit);
  op(apu.sequencer_irq);
  op(apu.sequence_reset_pending);
  op(apu.sequencer_irq_clear_pending);
  op(apu.sequencer_irq_assert);
  op(apu.sequencer_check_1);
  op(apu.sequencer_check_2);

  op(apu.dmc_dma_countdown);
  op(apu.DMC_RDY_check);
  op(apu.call_from_write);
  op(apu.seq_tick);
  op(apu.seq_val);
  op(apu.sequencer_irq_flag);
  op(apu.len_clock_active);

  op(apu.oldmix);
  op(apu.cart_sound);
  op(apu.old_cart_sound);

  syncPulse(apu.pulse[0], op);
  syncPulse(apu.pulse[1], op);
  syncTriangle(apu.triangle, op);
  syncNoise(apu.noise, op);
  syncDmc(apu.dmc, op);

  // beyond the C# list:
  op(apu.recalculate);
  op(apu.doing_tick_quarter);
  op(apu.sampleclock);
}

template <typename Op>
void syncBoard(NesBoard& board, Op& op)
{
  // NesBoardBase.SyncState: vram, wram, mirroring, irq signal; the board adds its bank register.
  // Mirroring is only mutable on AxROM, but it is synced unconditionally, as in the C#.
  op.bytes(board.Vram.data(), board.Vram.size());
  op.bytes(board._mirroring, sizeof board._mirroring);
  op(board.IrqSignal);
  op(board.prg);
}

} // namespace detail

// NES.IStatable.cs SyncState orchestration + the extra mutable fields (see file header)
template <typename Op>
void syncNesState(NES& nes, Op& op)
{
  op(nes._frame);
  op(nes._lagcount);
  op(nes.islag);
  detail::syncCpu(*nes.cpu, op);
  op.bytes(nes.ram, sizeof(nes.ram));
  op.bytes(nes.CIRAM, sizeof(nes.CIRAM));
  op(nes._irq_apu);
  op(nes.sprdma_countdown);
  op(nes.cpu_deadcounter);

  op(nes.old_s);

  // OAM related
  op(nes.oam_dma_index);
  op(nes.oam_dma_exec);
  op(nes.oam_dma_addr);
  op(nes.oam_dma_byte);
  op(nes.dmc_dma_exec);
  op(nes.dmc_realign);

  // single cycle execution related
  op(nes.current_strobe);
  op(nes.new_strobe);

  detail::syncBoard(*nes.board, op);
  detail::syncPpu(*nes.ppu, op);
  detail::syncApu(*nes.apu, op);

  op(nes.DB);

  op(nes.latched4016);
  op(nes.controllerDeck._left._resetting);
  op(nes.controllerDeck._left._latchedValue);
  op(nes.controllerDeck._right._resetting);
  op(nes.controllerDeck._right._latchedValue);

  op(nes.resetSignal);
  op(nes.hardResetSignal);

  // beyond the C# list (mutable but unsynced there):
  op(nes.lagged);
  op(nes.double_controller_read);
  op(nes.double_controller_read_address);
  op(nes.previous_controller1_read);
  op(nes.previous_controller2_read);
  op(nes.dmc_dma_controller_conflict);
  op(nes.joypadStrobed);
  op(nes.joypadStrobeValue);
  op(nes.controller_was_latched);
  op(nes.frame_is_done);
  op(nes._controllerButtons);
  op(nes._controllerButtons2);
}

inline size_t nesStateSize(NES& nes)
{
  StateSaver s(nullptr);
  syncNesState(nes, s);
  return s.size();
}

} // namespace nesHawk
