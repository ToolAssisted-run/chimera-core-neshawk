// Translation of the APU.
//   APU / PulseUnit / NoiseUnit / TriangleUnit / DMCUnit
//     <- src/BizHawk.Emulation.Cores/Consoles/Nintendo/NES/APU.cs
//
// The whole unit is kept even though we never output sound, because the DMC DMA steals CPU
// cycles and the frame sequencer raises IRQs -- both timing-critical. EmitSample is kept too
// (it only mutates oldmix/recalculate, no feedback into emulation state); the blip synthesis
// buffer is the one thing dropped.
//
// Methods that touch the NES (bus reads, RDY, strobe_joyport, _irq_apu) are declared here and
// defined in nes.hpp: DMCUnit::Run, DMCUnit::Fetch, APU::RunDMCHaltFetch, APU::RunOneLast.
//
// NTSC-only instance for this cart, but the PAL tables are kept so the constructor matches.

#pragma once

#include <cstdint>
#include "nesCommon.hpp"

namespace nesHawk
{

class NES;

namespace apuTables
{
static constexpr int DMC_RATE_NTSC[] = { 428, 380, 340, 320, 286, 254, 226, 214, 190, 160, 142, 128, 106, 84, 72, 54 };
static constexpr int DMC_RATE_PAL[] = { 398, 354, 316, 298, 276, 236, 210, 198, 176, 148, 132, 118, 98, 78, 66, 50 };
static constexpr int LENGTH_TABLE[] = { 10, 254, 20, 2, 40, 4, 80, 6, 160, 8, 60, 10, 14, 12, 26, 14, 12, 16, 24, 18, 48, 20, 96, 22, 192, 24, 72, 26, 16, 28, 32, 30 };

static constexpr uint8_t PULSE_DUTY[4][8] = {
  {0,1,0,0,0,0,0,0}, // (12.5%)
  {0,1,1,0,0,0,0,0}, // (25%)
  {0,1,1,1,1,0,0,0}, // (50%)
  {1,0,0,1,1,1,1,1}, // (25% negated (75%))
};

static constexpr uint8_t TRIANGLE_TABLE[] =
{
  15, 14, 13, 12, 11, 10,  9,  8,  7,  6,  5,  4,  3,  2,  1,  0,
  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15
};

static constexpr int NOISE_TABLE_NTSC[] =
{
  4, 8, 16, 32, 64, 96, 128, 160, 202, 254, 380, 508, 762, 1016, 2034, 4068
};

static constexpr int NOISE_TABLE_PAL[] =
{
  4, 7, 14, 30, 60, 88, 118, 148, 188, 236, 354, 472, 708,  944, 1890, 3778
};

static constexpr int sequencer_lut_ntsc_0[] = {7457,14913,22371,29830};
static constexpr int sequencer_lut_ntsc_1[] = {7457,14913,22371,29830,37282};
static constexpr int sequencer_lut_pal_0[] = {8313,16627,24939,33254};
static constexpr int sequencer_lut_pal_1[] = {8313,16627,24939,33254,41566};
} // namespace apuTables

class APU
{
  public:

  int m_vol = 1;

  int dmc_dma_countdown = -1;
  int DMC_RDY_check = 0;
  bool call_from_write = false;

  bool recalculate = false;

  NES* nes;

  class PulseUnit
  {
    public:

    PulseUnit(APU* apu, int unit) : unit(unit), apu(apu) {}
    int unit;
    APU* apu;

    // reg0
    int duty_cnt = 0, env_loop = 0, env_constant = 0, env_cnt_value = 0;
    bool len_halt = false;
    // reg1
    int sweep_en = 0, sweep_divider_cnt = 0, sweep_negate = 0, sweep_shiftcount = 0;

    bool sweep_reload = false;
    // reg2/3
    int len_cnt = 0;
    int timer_raw_reload_value = 0, timer_reload_value = 0;

    // misc..
    int lenctr_en = 0;

    bool IsLenCntNonZero() const { return len_cnt > 0; }

    void WriteReg(int addr, uint8_t val)
    {
      switch (addr)
      {
        case 0:
          env_cnt_value = val & 0xF;
          env_constant = (val >> 4) & 1;
          env_loop = (val >> 5) & 1;
          duty_cnt = (val >> 6) & 3;
          break;
        case 1:
          sweep_shiftcount = val & 7;
          sweep_negate = (val >> 3) & 1;
          sweep_divider_cnt = (val >> 4) & 7;
          sweep_en = (val >> 7) & 1;
          sweep_reload = true;
          break;
        case 2:
          timer_reload_value = (timer_reload_value & 0x700) | val;
          timer_raw_reload_value = timer_reload_value * 2 + 2;
          break;
        case 3:
          if (apu->len_clock_active)
          {
            if (len_cnt == 0)
            {
              len_cnt = apuTables::LENGTH_TABLE[(val >> 3) & 0x1F] + 1;
            }
          } else
          {
            len_cnt = apuTables::LENGTH_TABLE[(val >> 3) & 0x1F];
          }

          timer_reload_value = (timer_reload_value & 0xFF) | ((val & 0x07) << 8);
          timer_raw_reload_value = timer_reload_value * 2 + 2;
          duty_step = 0;
          env_start_flag = 1;

          // allow the lenctr_en to kill the len_cnt
          set_lenctr_en(lenctr_en);
          break;
      }
    }

    void set_lenctr_en(int value)
    {
      lenctr_en = value;
      // if the length counter is not enabled, then we must disable the length system in this way
      if (lenctr_en == 0) len_cnt = 0;
    }

    // state
    int swp_divider_counter = 0;
    bool swp_silence = false;
    int duty_step = 0;
    int timer_counter = 0;
    int sample = 0;
    bool duty_value = false;

    int env_start_flag = 0, env_divider = 0, env_counter = 0;
    int env_output = 0;

    void clock_length_and_sweep()
    {
      // this should be optimized to update only when `timer_reload_value` changes
      int sweep_shifter = timer_reload_value >> sweep_shiftcount;
      if (sweep_negate == 1)
        sweep_shifter = -sweep_shifter - unit;
      sweep_shifter += timer_reload_value;

      // this sweep logic is always enabled:
      swp_silence = (timer_reload_value < 8 || (sweep_shifter > 0x7FF)); // && sweep_negate == 0));

      // does enable only block the pitch bend? does the clocking proceed?
      if (sweep_en == 1)
      {
        // clock divider
        if (swp_divider_counter != 0) swp_divider_counter--;
        if (swp_divider_counter == 0)
        {
          swp_divider_counter = sweep_divider_cnt + 1;

          // divider was clocked: process sweep pitch bend
          if (sweep_shiftcount != 0 && !swp_silence)
          {
            timer_reload_value = sweep_shifter;
            timer_raw_reload_value = (timer_reload_value << 1) + 2;
          }
          // TODO - does this change the user's reload value or the latched reload value?
        }

        // handle divider reload, after clocking happens
        if (sweep_reload)
        {
          swp_divider_counter = sweep_divider_cnt + 1;
          sweep_reload = false;
        }
      }

      // env_loop doubles as "halt length counter"
      if ((env_loop == 0 || len_halt) && len_cnt > 0)
        len_cnt--;
    }

    void clock_env()
    {
      if (env_start_flag == 1)
      {
        env_start_flag = 0;
        env_divider = env_cnt_value;
        env_counter = 15;
      }
      else
      {
        if (env_divider != 0)
        {
          env_divider--;
        } else if (env_divider == 0)
        {
          env_divider = env_cnt_value;
          if (env_counter == 0)
          {
            if (env_loop == 1)
            {
              env_counter = 15;
            }
          }
          else env_counter--;
        }
      }
    }

    void Run()
    {
      if (env_constant == 1)
        env_output = env_cnt_value;
      else env_output = env_counter;

      if (timer_counter > 0) timer_counter--;
      if (timer_counter == 0 && timer_raw_reload_value != 0)
      {
        if (duty_step==7)
        {
          duty_step = 0;
        } else
        {
          duty_step++;
        }
        duty_value = apuTables::PULSE_DUTY[duty_cnt][duty_step] == 1;
        // reload timer
        timer_counter = timer_raw_reload_value;
      }

      int newsample;

      if (duty_value) // high state of duty cycle
      {
        newsample = env_output;
        if (swp_silence || len_cnt == 0)
          newsample = 0; // silenced
      }
      else
        newsample = 0; // duty cycle is 0, silenced.

      if (newsample != sample)
      {
        apu->recalculate = true;
        sample = newsample;
      }
    }
  };

  class NoiseUnit
  {
    public:

    APU* apu;

    // reg0 (sweep)
    int env_cnt_value = 0, env_loop = 0, env_constant = 0;
    bool len_halt = false;

    // reg2 (mode and period)
    int mode_cnt = 0, period_cnt = 0;

    // reg3 (length counter and envelop trigger)
    int len_cnt = 0;

    // set from apu:
    int lenctr_en = 0;

    // state
    int shift_register = 1;
    int timer_counter = 0;
    int sample = 0;
    int env_output = 0, env_start_flag = 0, env_divider = 0, env_counter = 0;
    bool noise_bit = true;

    const int* NOISE_TABLE;

    NoiseUnit(APU* apu, bool pal) : apu(apu)
    {
      NOISE_TABLE = pal ? apuTables::NOISE_TABLE_PAL : apuTables::NOISE_TABLE_NTSC;
    }

    bool IsLenCntNonZero() const { return len_cnt > 0; }

    void WriteReg(int addr, uint8_t val)
    {
      switch (addr)
      {
        case 0:
          env_cnt_value = val & 0xF;
          env_constant = (val >> 4) & 1;
          // we want to delay a halt until after a length clock if they happen on the same cycle
          if (env_loop==0 && ((val >> 5) & 1)==1)
          {
            len_halt = true;
          }
          env_loop = (val >> 5) & 1;
          break;
        case 1:
          break;
        case 2:
          period_cnt = NOISE_TABLE[val & 0xF];
          mode_cnt = (val >> 7) & 1;
          break;
        case 3:
          if (apu->len_clock_active)
          {
            if (len_cnt == 0)
            {
              len_cnt = apuTables::LENGTH_TABLE[(val >> 3) & 0x1F] + 1;
            }
          }
          else
          {
            len_cnt = apuTables::LENGTH_TABLE[(val >> 3) & 0x1F];
          }

          set_lenctr_en(lenctr_en);
          env_start_flag = 1;
          break;
      }
    }

    void set_lenctr_en(int value)
    {
      lenctr_en = value;
      // if the length counter is not enabled, then we must disable the length system in this way
      if (lenctr_en == 0) len_cnt = 0;
    }

    void clock_env()
    {
      if (env_start_flag == 1)
      {
        env_start_flag = 0;
        env_divider = (env_cnt_value + 1);
        env_counter = 15;
      }
      else
      {
        if (env_divider != 0) env_divider--;
        if (env_divider == 0)
        {
          env_divider = (env_cnt_value + 1);
          if (env_counter == 0)
          {
            if (env_loop == 1)
            {
              env_counter = 15;
            }
          }
          else env_counter--;
        }
      }
    }

    void clock_length_and_sweep()
    {
      if (len_cnt > 0 && (env_loop == 0 || len_halt))
        len_cnt--;
    }

    void Run()
    {
      if (env_constant == 1)
        env_output = env_cnt_value;
      else env_output = env_counter;

      if (timer_counter > 0) timer_counter--;
      if (timer_counter == 0 && period_cnt != 0)
      {
        // reload timer
        timer_counter = period_cnt;
        int feedback_bit;
        if (mode_cnt == 1) feedback_bit = (shift_register >> 6) & 1;
        else feedback_bit = (shift_register >> 1) & 1;
        int feedback = feedback_bit ^ (shift_register & 1);
        shift_register >>= 1;
        shift_register &= ~(1 << 14);
        shift_register |= (feedback << 14);
        noise_bit = (shift_register & 1) != 0;
      }

      int newsample;
      if (len_cnt == 0) newsample = 0;
      else if (noise_bit) newsample = env_output; // switched, was 0?
      else newsample = 0;
      if (newsample != sample)
      {
        apu->recalculate = true;
        sample = newsample;
      }
    }
  };

  class TriangleUnit
  {
    public:

    // reg0
    int linear_counter_reload = 0, control_flag = 0;
    // reg1 (n/a)
    // reg2/3
    int timer_cnt = 0, reload_flag = 0, len_cnt = 0;
    bool halt_2 = false;
    // misc..
    int lenctr_en = 0;
    int linear_counter = 0, timer = 0, timer_cnt_reload = 0;
    int seq = 0;
    int sample = 0;

    APU* apu;
    TriangleUnit(APU* apu) : apu(apu) {}

    bool IsLenCntNonZero() const { return len_cnt > 0; }

    void set_lenctr_en(int value)
    {
      lenctr_en = value;
      // if the length counter is not enabled, then we must disable the length system in this way
      if (lenctr_en == 0) len_cnt = 0;
    }

    void WriteReg(int addr, uint8_t val)
    {
      switch (addr)
      {
        case 0:
          linear_counter_reload = (val & 0x7F);
          control_flag = (val >> 7) & 1;
          break;
        case 1: break;
        case 2:
          timer_cnt = (timer_cnt & ~0xFF) | val;
          timer_cnt_reload = timer_cnt + 1;
          break;
        case 3:
          timer_cnt = (timer_cnt & 0xFF) | ((val & 0x7) << 8);
          timer_cnt_reload = timer_cnt + 1;
          if (apu->len_clock_active)
          {
            if (len_cnt == 0)
            {
              len_cnt = apuTables::LENGTH_TABLE[(val >> 3) & 0x1F] + 1;
            }
          }
          else
          {
            len_cnt = apuTables::LENGTH_TABLE[(val >> 3) & 0x1F];
          }
          reload_flag = 1;

          // allow the lenctr_en to kill the len_cnt
          set_lenctr_en(lenctr_en);
          break;
      }
    }

    void Run()
    {
      // when clocked by timer, seq steps forward
      // except when linear counter or length counter is 0
      bool en = len_cnt != 0 && linear_counter != 0;

      bool do_clock = false;
      if (timer > 0) timer--;
      if (timer == 0)
      {
        do_clock = true;
        timer = timer_cnt_reload;
      }

      if (en && do_clock)
      {
        int newsample;

        seq = (seq + 1) & 0x1F;

        newsample = apuTables::TRIANGLE_TABLE[seq];

        // special hack: frequently, games will use the maximum frequency triangle in order to mute it
        // apparently this results in the DAC for the triangle wave outputting a steady level at about 7.5
        // so we'll emulate it at the digital level
        if (timer_cnt_reload == 1) newsample = 8;

        if (newsample != sample)
        {
          apu->recalculate = true;
          sample = newsample;
        }
      }
    }

    void clock_length_and_sweep()
    {
      // env_loop doubles as "halt length counter"
      if (len_cnt > 0 && control_flag == 0)
        len_cnt--;
    }

    void clock_linear_counter()
    {
      if (reload_flag == 1)
      {
        linear_counter = linear_counter_reload;
      }
      else if (linear_counter != 0)
      {
        linear_counter--;
      }

      if (control_flag == 0) { reload_flag = 0; }
    }
  }; // class TriangleUnit

  class DMCUnit
  {
    public:

    APU* apu;
    NES* nes;
    const int* DMC_RATE;

    DMCUnit(APU* apu, bool pal) : apu(apu)
    {
      nes = apu->nes;
      out_silence = true;
      DMC_RATE = pal ? apuTables::DMC_RATE_PAL : apuTables::DMC_RATE_NTSC;
      timer_reload = DMC_RATE[0];
      timer = 1020; // confirmed in VisualNES, on console it seems the APU runs a couple cycles before CPU exits reset
      sample_buffer_filled = false;
      out_deltacounter = 64;
      out_bits_remaining = 7; //confirmed in VisualNES
      user_address = 0xC000;
      sample_address = 0xC000;
      user_length = 1;
    }

    bool irq_enabled = false;
    bool loop_flag = false;
    int timer_reload;

    // dmc delay per visual 2a03
    int delay = 0;

    // this timer never stops, ever, so it is convenient to use for even/odd timing used elsewhere
    int timer;
    int user_address;
    uint32_t user_length, sample_length = 0;
    int sample_address, sample_buffer = 0;
    bool sample_buffer_filled;

    int out_shift = 0, out_bits_remaining, out_deltacounter;
    bool out_silence;
    // happens when buffer is filled and emptied at the same time
    bool fill_glitch = false;
    // happens when a write triggered refill that sets length to zero happens too close to an automatic DMA
    // (causes 1-cycle blips in dmc_dma_start_test_v2)
    bool fill_glitch_2 = false;
    bool fill_glitch_2_end = false;

    bool pending_disable = false;

    bool timer_just_reloaded = false;

    int sample() const { return out_deltacounter /* - 64*/; }

    NESHAWK_HOT_INLINE void Run(); // needs NES (cpu.RDY, dmc_dma_exec) -> body in nes.hpp

    void Clock()
    {
      // If the silence flag is clear, bit 0 of the shift register is applied to the counter as follows:
      // if bit 0 is clear and the delta-counter is greater than 1, the counter is decremented by 2;
      // otherwise, if bit 0 is set and the delta-counter is less than 126, the counter is incremented by 2
      if (!out_silence)
      {
        // apply current sample bit to delta counter
        if ((out_shift & 1) != 0)
        {
          if (out_deltacounter < 126)
            out_deltacounter += 2;
        }
        else
        {
          if (out_deltacounter > 1)
            out_deltacounter -= 2;
        }
        apu->recalculate = true;
      }

      // The right shift register is clocked.
      out_shift >>= 1;

      // The bits-remaining counter is decremented. If it becomes zero, a new cycle is started.
      if (out_bits_remaining == 0)
      {
        // The bits-remaining counter is loaded with 8.
        out_bits_remaining = 7;
        // If the sample buffer is empty then the silence flag is set
        if (!sample_buffer_filled)
        {
          out_silence = true;
        }
        else
        // otherwise, the silence flag is cleared and the sample buffer is emptied into the shift register.
        {
          out_silence = false;
          out_shift = sample_buffer;
          sample_buffer_filled = false;
        }
      }
      else out_bits_remaining--;
    }

    void set_lenctr_en(bool en)
    {
      if(!en)
      {
        // in these cases, the disable happens right as the reload begins, it is cancelled similar to a write triggered reload
        // cancelling an automatic reload, and has the same timing (still uses fill_glitch_2)
        if (((timer == 3) || (timer == 2)) && (out_bits_remaining == 0) && (sample_length != 0))
        {
          sample_length = 0;
          fill_glitch_2 = true;
          apu->dmc_irq = false;
          apu->SyncIRQ();
          return;
        }

        // in these cases the disable happens too late and the reload (andpotential IRQ) still happen
        if ((timer == 1) && (out_bits_remaining == 0) && (sample_length != 0))
        {
          pending_disable = true;
          apu->dmc_irq = false;
          apu->SyncIRQ();
          return;
        }

        // if a fetch / reload is in progress, writing here as no immediate effect
        // but it seems IRQs can be cancelled after the fetch before IRQ
        if ((apu->dmc_dma_countdown > 0) || (delay != 0) || (apu->dmc_reload_countdown != 0))
        {
          if (apu->dmc_reload_countdown != 0) { apu->dmc_irq_glitch = true; }

          pending_disable = true;
        }
        else
        {
          sample_length = 0;
        }
      }
      else
      {
        // only start playback if playback is stopped
        if (sample_length == 0)
        {
          sample_address = user_address;
          sample_length = user_length;
        }
        if (!sample_buffer_filled)
        {
          // apparently the dmc is different if called from a cpu write, let's try
          apu->call_from_write = true;
        }
      }

      // irq is acknowledged or sure to be clear, in either case
      apu->dmc_irq = false;
      apu->SyncIRQ();
    }

    bool IsLenCntNonZero() const
    {
      return sample_length != 0;
    }

    void WriteReg(int addr, uint8_t val)
    {
      switch (addr)
      {
        case 0:
          irq_enabled = ((val >> 7) & 1) != 0;
          loop_flag = ((val >> 6) & 1) != 0;
          timer_reload = DMC_RATE[val & 0xF];
          if (!irq_enabled) apu->dmc_irq = false;
          apu->SyncIRQ();
          break;
        case 1:
          out_deltacounter = val & 0x7F;
          apu->recalculate = true;
          break;
        case 2:
          user_address = 0xC000 | (val << 6);
          break;
        case 3:
          user_length = ((uint32_t)val << 4) + 1;
          break;
      }
    }

    void Fetch(); // needs NES (bus read, DB conflict) -> body in nes.hpp
  };

  APU(NES* nes, APU* old, bool pal)
    : nes(nes),
      pulse{PulseUnit(this, 1), PulseUnit(this, 0)},
      triangle(this),
      noise(this, pal),
      dmc(this, pal)
  {
    sequencer_lut[0] = pal ? apuTables::sequencer_lut_pal_0 : apuTables::sequencer_lut_ntsc_0;
    sequencer_lut[1] = pal ? apuTables::sequencer_lut_pal_1 : apuTables::sequencer_lut_ntsc_1;

    if (old != nullptr)
    {
      m_vol = old->m_vol;
    }
  }

  PulseUnit pulse[2];
  TriangleUnit triangle;
  NoiseUnit noise;
  DMCUnit dmc;

  bool irq_pending = false;
  int dmc_reload_countdown = 0;
  bool dmc_irq = false;
  bool dmc_irq_glitch = false;
  int pending_reg = -1;
  bool doing_tick_quarter = false;
  uint8_t pending_val = 0;
  int seq_tick = 0;
  uint8_t seq_val = 0;
  bool len_clock_active = false;

  int sequencer_counter = 0, sequencer_step = 0, sequencer_mode = 0, sequencer_irq_inhibit = 0, sequencer_irq_assert = 0;
  bool sequencer_irq = false, sequence_reset_pending = false, sequencer_irq_clear_pending = false, sequencer_irq_flag = false;

  void RunDMCFetch()
  {
    dmc.Fetch();
  }

  void RunDMCHaltFetch(); // needs NES (bus read at cpu.address_bus) -> body in nes.hpp

  const int* sequencer_lut[2];

  int sequencer_check_1 = 0, sequencer_check_2 = 0;

  void sequencer_write_tick(uint8_t val)
  {
    if (seq_tick > 0)
    {
      seq_tick--;

      if (seq_tick == 0)
      {
        sequencer_mode = (val >> 7) & 1;

        // check if we will be doing the extra frame ticks or not
        if (sequencer_mode == 1)
        {
          if (!doing_tick_quarter)
          {
            QuarterFrame();
            HalfFrame();
          }
        }

        sequencer_irq_inhibit = (val >> 6) & 1;
        if (sequencer_irq_inhibit == 1)
        {
          sequencer_irq_flag = false;
        }

        sequencer_counter = 0;
        sequencer_step = 0;

        if (sequencer_mode == 0) { sequencer_check_2 = sequencer_lut[0][3] - 2; }
        else { sequencer_check_2 = sequencer_lut[1][4] - 2; }
      }
    }
  }

  void sequencer_tick()
  {
    sequencer_counter++;
    if (sequencer_mode == 0 && sequencer_counter == sequencer_lut[0][3] - 1)
    {
      if (sequencer_irq_inhibit == 0)
      {
        sequencer_irq_assert = 2;
        sequencer_irq_flag = true;
      }

      HalfFrame();
    }
    if (sequencer_mode == 0 && sequencer_counter == sequencer_lut[0][3] - 2 && sequencer_irq_inhibit == 0)
    {
      //sequencer_irq_assert = 2;
      sequencer_irq_flag = true;
    }
    if (sequencer_mode == 1 && sequencer_counter == sequencer_lut[1][4] - 1)
    {
      HalfFrame();
    }
    if (sequencer_lut[sequencer_mode][sequencer_step] != sequencer_counter)
      return;
    sequencer_check();
  }

  void SyncIRQ()
  {
    irq_pending = sequencer_irq | dmc_irq;
  }

  void sequencer_check()
  {
    bool quarter, half, reset;
    switch (sequencer_mode)
    {
      case 0: // 4-step
        quarter = true;
        half = sequencer_step == 1;
        reset = sequencer_step == 3;
        if (reset && sequencer_irq_inhibit == 0)
        {
          // sequencer_irq_assert = 2;
          sequencer_irq_flag = true;
        }
        break;

      case 1: // 5-step
        quarter = sequencer_step != 3;
        half = sequencer_step == 1;
        reset = sequencer_step == 4;
        break;

      default:
        __builtin_trap(); // InvalidOperationException
    }

    if (reset)
    {
      sequencer_counter = 0;
      sequencer_step = 0;
    }
    else sequencer_step++;

    if (quarter) QuarterFrame();
    if (half) HalfFrame();
  }

  void HalfFrame()
  {
    doing_tick_quarter = true;
    pulse[0].clock_length_and_sweep();
    pulse[1].clock_length_and_sweep();
    triangle.clock_length_and_sweep();
    noise.clock_length_and_sweep();
  }

  void QuarterFrame()
  {
    doing_tick_quarter = true;
    pulse[0].clock_env();
    pulse[1].clock_env();
    triangle.clock_linear_counter();
    noise.clock_env();
  }

  void NESSoftReset()
  {
    // need to study what happens to apu and stuff..
    sequencer_irq = false;
    sequencer_irq_flag = false;
    _WriteReg(0x4015, 0);

    // for 4017, its as if the last value written gets rewritten
    sequencer_mode = (seq_val >> 7) & 1;
    sequencer_irq_inhibit = (seq_val >> 6) & 1;
    if (sequencer_irq_inhibit == 1)
    {
      sequencer_irq_flag = false;
    }
    sequencer_counter = 0;
    sequencer_step = 0;

    sequencer_check_1 = (sequencer_lut[0][1] - 1);

    if (sequencer_mode == 0) { sequencer_check_2 = sequencer_lut[0][3] - 2; }
    else { sequencer_check_2 = sequencer_lut[1][4] - 2; }

    dmc.fill_glitch = false;
    dmc.fill_glitch_2 = false;
  }

  void NESHardReset()
  {
    // "at power on it is as if $00 was written to $4017 9-12 cycles before the reset vector"
    // DMC seems to run for a couple cycles after reset, so aim for the upper end of that range (12)
    sequencer_counter = 2;

    sequencer_check_1 = (sequencer_lut[0][1] - 1);

    if (sequencer_mode == 0) { sequencer_check_2 = sequencer_lut[0][3] - 2; }
    else { sequencer_check_2 = sequencer_lut[1][4] - 2; }

    dmc.fill_glitch = false;
    dmc.fill_glitch_2 = false;
  }

  void WriteReg(int addr, uint8_t val)
  {
    pending_reg = addr;
    pending_val = val;
  }

  void _WriteReg(int addr, uint8_t val)
  {
    int index = addr - 0x4000;
    int reg = index & 3;
    int channel = index >> 2;
    switch (channel)
    {
      case 0:
        pulse[0].WriteReg(reg, val);
        break;
      case 1:
        pulse[1].WriteReg(reg, val);
        break;
      case 2:
        triangle.WriteReg(reg, val);
        break;
      case 3:
        noise.WriteReg(reg, val);
        break;
      case 4:
        dmc.WriteReg(reg, val);
        break;
      case 5:
        if (addr == 0x4015)
        {
          pulse[0].set_lenctr_en(val & 1);
          pulse[1].set_lenctr_en((val >> 1) & 1);
          triangle.set_lenctr_en((val >> 2) & 1);
          noise.set_lenctr_en((val >> 3) & 1);
          dmc.set_lenctr_en(((val >> 4) & 1) != 0);
        }
        else if (addr == 0x4017)
        {
          if (dmc.timer % 2 == 0)
          {
            seq_tick = 3;
          } else
          {
            seq_tick = 4;
          }

          seq_val = val;
        }
        break;
    }
  }

  uint8_t PeekReg(int addr)
  {
    switch (addr)
    {
      case 0x4015:
        {
          //notice a missing bit here. should properly emulate with empty / Data bus
          //if an interrupt flag was set at the same moment of the read, it will read back as 1 but it will not be cleared.
          int dmc_nonzero = dmc.IsLenCntNonZero() ? 1 : 0;
          int noise_nonzero = noise.IsLenCntNonZero() ? 1 : 0;
          int tri_nonzero = triangle.IsLenCntNonZero() ? 1 : 0;
          int pulse1_nonzero = pulse[1].IsLenCntNonZero() ? 1 : 0;
          int pulse0_nonzero = pulse[0].IsLenCntNonZero() ? 1 : 0;
          int ret = ((dmc_irq ? 1 : 0) << 7) | ((sequencer_irq_flag ? 1 : 0) << 6) | (dmc_nonzero << 4) | (noise_nonzero << 3) | (tri_nonzero << 2) | (pulse1_nonzero << 1) | (pulse0_nonzero);
          return (uint8_t)ret;
        }
      default:
        // don't return 0xFF here or SMB will break
        return 0x00;
    }
  }

  uint8_t ReadReg(int addr)
  {
    switch (addr)
    {
      case 0x4015:
        {
          uint8_t ret = PeekReg(0x4015);
          sequencer_irq_clear_pending = true;
          SyncIRQ();
          return ret;
        }
      default:
        // don't return 0xFF here or SMB will break
        return 0x00;
    }
  }

  NESHAWK_HOT_INLINE void RunOneFirst()
  {
#ifndef NESHAWK_HEADLESS
    // waveform generation: audio-only (timer_counter/duty_step/shift_register/seq/sample feed
    // nothing but EmitSample)
    pulse[0].Run();
    pulse[1].Run();
    triangle.Run();
    noise.Run();
#endif
    dmc.Run(); // DMC always runs: its timer drives DMA cycle stealing

    // len_halt clears stay: they gate the frame sequencer's length clocking
    pulse[0].len_halt = false;
    pulse[1].len_halt = false;
    noise.len_halt = false;
  }

  NESHAWK_HOT_INLINE void RunOneLast(); // needs NES (strobe_joyport, _irq_apu) -> body in nes.hpp

  uint32_t sampleclock = 0;

  int oldmix = 0;
  int cart_sound = 0;
  int old_cart_sound = 0;

  /// APU.ExternalQueue: where a cart's own sound enters the mix. "only call in board.ClockCPU()"
  void ExternalQueue(int value)
  {
    cart_sound = value + old_cart_sound;

    if (cart_sound != old_cart_sound)
    {
      recalculate = true;
      old_cart_sound = cart_sound;
    }
  }

  int EmitSample()
  {
    if (recalculate)
    {
      recalculate = false;

      int s_pulse0 = pulse[0].sample;
      int s_pulse1 = pulse[1].sample;
      int s_tri = triangle.sample;
      int s_noise = noise.sample;
      int s_dmc = dmc.sample();

      // more properly correct
      float pulse_out = s_pulse0 == 0 && s_pulse1 == 0
        ? 0
        : 95.88f / ((8128.0f / (s_pulse0 + s_pulse1)) + 100.0f);

      float tnd_out = s_tri == 0 && s_noise == 0 && s_dmc == 0
        ? 0
        : 159.79f / (1 / ((s_tri / 8227.0f) + (s_noise / 12241.0f) + (s_dmc / 22638.0f)) + 100);

      float output = pulse_out + tnd_out;

      // this needs to leave enough headroom for straying DC bias due to the DMC unit getting stuck outputs. smb3 is bad about that.
      oldmix = (int)(20000 * output * (1 + m_vol / 5)) + cart_sound;
    }

    return oldmix;
  }
};

} // namespace nesHawk
