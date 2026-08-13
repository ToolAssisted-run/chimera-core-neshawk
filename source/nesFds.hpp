// Famicom Disk System: the RAM adapter's disk drive and its extra sound channel.
//
// Transliteration of BizHawk's NES/FDS/RamAdapter.cs and NES/FDS/FDSAudio.cs. The board itself
// (FDS.cs) lives with the other boards in nesBoards.hpp, as Kind::FDS.
//
// The drive is modelled at the BIT level and clocked by the PPU: a disk is a 65500-byte stream of
// bits that passes under a fixed head at ~96.4 kHz, and the BIOS reads it a bit at a time through a
// shift register. That is why a .fds file cannot be handed to the drive as-is - the dumps in
// circulation are file-system level, with the gaps, block markers and CRCs stripped out - so
// FixFdsSide puts back the physical layout the drive expects to find.
//
// Comments in quotes, and the guesses about drive timings, are BizHawk's own.

#pragma once

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace nesHawk
{

class APU;

/// RamAdapter: "implements the FDS disk drive hardware, more or less"
struct FdsDrive
{
  // all timings are in terms of PPU cycles (@5.37mhz)
  enum class State
  {
    Running,   // moving over the disk
    Inserting, // new disk/side into the drive
    Spinup,    // motor starting
    Reset,     // head moving back to beginning
    Idle,      // nothing happening
  };

  /// the original contents of this disk when it was loaded. for virtual saveram diff
  std::vector<uint8_t> originaldisk;
  /// currently loaded disk side (ca 65k bytes); empty means no disk
  std::vector<uint8_t> disk;
  /// current disk location in BITS, not bytes
  int diskpos = 0;
  /// size of current disk in BITS, not bytes
  int disksize = 0;
  /// true if current disk is writeprotected
  bool writeprotect = true;

  /// ppu cycles until next action
  int cycleswaiting = 0;
  /// physical state of the drive
  State state = State::Idle;

  /// cached 4025 write; can be modified internally by some things
  uint8_t cached4025 = 0;
  /// can be raised on byte transfer complete
  bool irq = false;
  /// true if 4025.1 is set to true
  bool transferreset = false;

  /// 16 bit CRC register. in normal operation, will become all 0 on finishing a read
  uint16_t crc = 0;
  /// true if data being written to disk is currently being computed in CRC
  bool writecomputecrc = false;

  // read and write shift regs, with bit positions and latched values for reload
  uint8_t readreg = 0;
  uint8_t writereg = 0;
  int readregpos = 0;
  int writeregpos = 0;
  uint8_t readreglatch = 0;
  uint8_t writereglatch = 0;

  bool bytetransferflag = false;
  bool lookingforendofgap = false;

  /// advance a 16 bit CRC register with 1 new input bit. x.25 standard
  static uint16_t CCITT(uint16_t crc, int bit)
  {
    const int bitc = crc & 1;
    crc >>= 1;
    if ((bitc ^ bit) != 0) crc ^= 0x8408;
    return crc;
  }

  /// advance a 16 bit CRC register with 8 new input bits. x.25 standard
  static uint16_t CCITT_8(uint16_t crc, uint8_t b)
  {
    for (int i = 0; i < 8; i++) crc = CCITT(crc, (b >> i) & 1);
    return crc;
  }

  static void WriteBlock(std::vector<uint8_t>& dest, const uint8_t* data, size_t length, int pregap)
  {
    for (int i = 0; i < pregap - 1; i++) dest.push_back(0);
    uint16_t crc = 0;
    dest.push_back(0x80); // end of gap marker
    crc = CCITT_8(crc, 0x80);
    for (size_t i = 0; i < length; i++)
    {
      dest.push_back(data[i]);
      crc = CCITT_8(crc, data[i]);
    }
    dest.push_back((uint8_t)(crc & 0xFF));
    dest.push_back((uint8_t)(crc >> 8));
  }

  /// "the current circulating .fds dumps are horribly broken. here we attempt to fix them up as
  /// best as possible" - rebuild the gaps, block markers and CRCs the drive expects to see.
  static std::vector<uint8_t> FixFdsSide(const uint8_t* inputdisk, size_t inputSize)
  {
    size_t pos = 0;
    auto read = [&](size_t n) -> const uint8_t*
    {
      if (pos + n > inputSize) throw std::runtime_error("truncated FDS disk side");
      const uint8_t* p = inputdisk + pos;
      pos += n;
      return p;
    };

    std::vector<uint8_t> ret;
    ret.reserve(65500);

    // block 1: header
    const uint8_t* header = read(56);
    static const uint8_t compare[] = { 0x01, 0x2a, 0x4e, 0x49, 0x4e, 0x54, 0x45, 0x4e, 0x44, 0x4f, 0x2d, 0x48, 0x56, 0x43, 0x2a };
    for (size_t i = 0; i < sizeof compare; i++)
    {
      if (compare[i] != header[i]) throw std::runtime_error("Corrupt FDS block 1");
    }
    // the rest of block 1 isn't terribly important to parse
    WriteBlock(ret, header, 56, 3537);

    // block 2: number of files
    const uint8_t* numfileblock = read(2);
    if (numfileblock[0] != 0x02) throw std::runtime_error("Corrupt FDS block 2");
    const int numfiles = numfileblock[1];
    WriteBlock(ret, numfileblock, 2, 122);

    // repeating block 3 and 4: file header and file data
    for (int i = 0; i < numfiles; i++)
    {
      const uint8_t* fileheader = read(16);
      // a bad marker is a warning in NesHawk, not a refusal: plenty of dumps have them
      const int filesize = fileheader[13] + fileheader[14] * 256;
      const uint8_t* file = read((size_t)filesize + 1);
      WriteBlock(ret, fileheader, 16, 122);
      WriteBlock(ret, file, (size_t)filesize + 1, 122);
    }

    ret.resize(65500); // zero-fills, or truncates
    return ret;
  }

  /// set cycleswaiting param after a state change
  void SetCycles()
  {
    // these are mostly guesses
    switch (state)
    {
      case State::Running:   cycleswaiting = 56; break;      // "transfer rate of 96.4kHz"
      case State::Inserting: cycleswaiting = 535000; break;  // 100ms: drive engaging on the disk
      case State::Spinup:    cycleswaiting = 1070000; break; // 199ms: motor spinup, merged with the pre-gap
      case State::Idle:      cycleswaiting = 100000; break;  // irrelevant
      case State::Reset:     cycleswaiting = 535000; break;  // 100ms: head springing back to the outer edge
    }
  }

  /// eject the loaded disk
  void Eject()
  {
    disk.clear();
    state = State::Idle;
    SetCycles();
  }

  /// insert a new disk. might have to eject first???
  void Insert(std::vector<uint8_t> side, int bitlength, bool writeProtect)
  {
    if (side.size() * 8 < (size_t)bitlength) throw std::runtime_error("Disk too small for parameter!");
    disk = std::move(side);
    disksize = bitlength;
    diskpos = 0;
    writeprotect = writeProtect;
    state = State::Inserting;
    SetCycles();
    originaldisk = disk;
  }

  /// insert one 65500-byte side out of a .fds file, gaps and CRCs restored
  void InsertBrokenImage(const uint8_t* side, size_t length, bool writeProtect)
  {
    Insert(FixFdsSide(side, length), 65500 * 8, writeProtect);
  }

  void ApplyDiff(const std::vector<uint8_t>& data)
  {
    int bitsize = data[0] * 0x10000 + data[1] * 0x100 + data[2];
    if (bitsize != disksize) throw std::runtime_error("Disk size mismatch!");
    int pos = 0;
    while (bitsize > 0)
    {
      disk[pos] ^= data[pos + 3];
      pos++;
      bitsize -= 8;
    }
  }

  std::vector<uint8_t> MakeDiff() const
  {
    std::vector<uint8_t> ret((size_t)(disksize + 7) / 8 + 3, 0);
    int bitsize = disksize;
    ret[0] = (uint8_t)(bitsize / 0x10000);
    ret[1] = (uint8_t)(bitsize / 0x100);
    ret[2] = (uint8_t)bitsize;
    int pos = 0;
    while (bitsize > 0)
    {
      ret[pos + 3] = (uint8_t)(disk[pos] ^ originaldisk[pos]);
      pos++;
      bitsize -= 8;
    }
    return ret;
  }

  /// memorydomain debugging
  static constexpr int NumBytes = 65500;

  /// memorydomain debugging
  uint8_t PeekData(int addr) const
  {
    return (!disk.empty() && (size_t)addr < disk.size()) ? disk[(size_t)addr] : 0xFF;
  }

  /// data write reg
  void Write4024(uint8_t value)
  {
    bytetransferflag = false;
    writereglatch = value;
  }

  /// control reg
  void Write4025(uint8_t value)
  {
    if ((value & 1) != 0) // start motor
    {
      if (state == State::Idle && !disk.empty()) // no spinup when no disk
      {
        state = State::Spinup;
        SetCycles();
      }
    }
    transferreset = (value & 2) != 0;

    if ((cached4025 & 0x40) == 0 && (value & 0x40) != 0)
    {
      lookingforendofgap = true;

      if ((value & 4) == 0)
      {
        // write mode: reload and go
        writeregpos = 0;
        writereg = writereglatch;
        bytetransferflag = true;
        crc = 0;
        writecomputecrc = true;
      }
    }

    irq = false; // ??

    cached4025 = value;
  }

  /// general status reg, some bits are from outside the RamAdapter class
  uint8_t Read4030()
  {
    uint8_t ret = 0;
    if (bytetransferflag) ret |= 0x02;
    if (crc != 0) ret |= 0x10;
    if (diskpos == disksize) ret |= 0x40;             // end of disk
    if (!disk.empty() && !writeprotect) ret |= 0x80;  // writable disk

    // acked
    bytetransferflag = false;
    irq = false;

    return ret;
  }

  /// more status stuff
  uint8_t Read4031()
  {
    bytetransferflag = false;
    irq = false; //??
    // note that the shift regs are latched, hence a missed bit does not show up here
    return readreglatch;
  }

  /// more status stuff
  uint8_t Read4032() const
  {
    uint8_t ret = 0xFF;
    if (!disk.empty() && state != State::Inserting) ret &= (uint8_t)~0x01;
    if (!transferreset && (state == State::Running || state == State::Idle)) ret &= (uint8_t)~0x02;
    if (!disk.empty() && state != State::Inserting && !writeprotect) ret &= (uint8_t)~0x04;
    return ret;
  }

  /// clock at ~5.37mhz
  void Clock()
  {
    cycleswaiting--;
    if (cycleswaiting != 0) return;

    switch (state)
    {
      case State::Running:
        if (transferreset) MoveDummy();          // run head to end of disk
        else if ((cached4025 & 4) != 0) Read();  // read mode
        else Write();
        if (diskpos >= disksize) state = State::Reset;
        SetCycles();
        break;

      case State::Reset:
      case State::Inserting:
        state = State::Idle;
        diskpos = 0;
        SetCycles();
        break;

      case State::Spinup:
        state = State::Running;
        SetCycles();
        break;

      case State::Idle:
        SetCycles();
        break;
    }
  }

private:
  void Read()
  {
    const int bit = disk[(size_t)(diskpos >> 3)] >> (diskpos & 7) & 1;

    diskpos++;

    if (lookingforendofgap && (cached4025 & 0x10) == 0) // looking for end of gap, but not when CRC is active
    {
      if (bit == 1) // found!
      {
        lookingforendofgap = false;
        readregpos = 0;
        crc = 0;
        // the first '1' is included in the CRC
        crc = CCITT(crc, 1);
      }
      // else continue scanning gap
    }
    else // reading actual data
    {
      crc = CCITT(crc, bit);
      readreg &= (uint8_t)~(1 << readregpos);
      readreg |= (uint8_t)(bit << readregpos);
      readregpos++;
      if (readregpos == 8)
      {
        readregpos = 0;

        bytetransferflag = true;
        if ((cached4025 & 0x80) != 0) irq = true;
        readreglatch = readreg;

        if ((cached4025 & 0x10) != 0)
        {
          cached4025 &= (uint8_t)~0x10; // clear CRC reading. no real effect other than to silence debug??
        }
      }
    }
  }

  void Write()
  {
    if (writeprotect)
    {
      diskpos++;
      return;
    }

    bool bittowrite = false;

    // the variable is named for its function in read mode; in write mode, when not set,
    // write an endless stream of zeroes.
    if (lookingforendofgap)
    {
      bittowrite = (writereg & (1 << writeregpos)) != 0;
      if (writecomputecrc) crc = CCITT(crc, bittowrite ? 1 : 0);
      writeregpos++;
      if (writeregpos == 8)
      {
        writeregpos = 0;
        writereg = writereglatch;
        bytetransferflag = true;
        if ((cached4025 & 0x80) != 0) irq = true;

        if ((cached4025 & 0x10) != 0)
        {
          if (crc == 0)
          {
            cached4025 &= (uint8_t)~0x10; // clear CRC reading
            // "it seems that after a successful CRC, the writereglatch is reset to 0 value"
            writereglatch = 0;
          }

          writereg = (uint8_t)crc;
          crc >>= 8;
          // loaded the first CRC byte to write, so stop computing CRC on data
          writecomputecrc = false;
        }
      }
    }

    uint8_t tmp = disk[(size_t)(diskpos >> 3)];
    tmp &= (uint8_t)~(1 << (diskpos & 7));
    if (bittowrite) tmp |= (uint8_t)(1 << (diskpos & 7));
    disk[(size_t)(diskpos >> 3)] = tmp;
    diskpos++;
  }

  void MoveDummy()
  {
    // "It seems that the real disk doesn't keep on running at normal speed to the end while
    // resetting. Whoever told me that was mistaken..."
    diskpos += 5000;
  }
};

/// FDSAudio: the RAM adapter's wavetable channel, with its own envelope, sweep and modulator.
/// http://wiki.nesdev.com/w/index.php/FDS_audio
struct FdsAudio
{
  //4040:407f
  uint8_t waveram[64] = {};
  /// playback position, clocked by main unit
  int waverampos = 0;
  //4080
  /// volume level or envelope speed, depending on r4080_7
  int volumespd = 0;
  /// increase volume with envelope
  bool r4080_6 = false;
  /// disable volume envelope
  bool r4080_7 = false;
  //4082:4083
  /// speed to clock main unit
  int frequency = 0;
  /// disable volume and sweep
  bool r4083_6 = false;
  /// silence channel
  bool r4083_7 = false;
  //4084
  /// sweep gain or sweep speed, depending on r4084_7
  int sweepspd = 0;
  /// increase sweep with envelope
  bool r4084_6 = false;
  /// disable sweep unit
  bool r4084_7 = false;
  //4085
  /// 7 bit signed
  int sweepbias = 0;
  //4086:4087
  /// speed to clock modulation unit
  int modfreq = 0;
  /// disable modulation unit
  bool r4087_7 = false;
  //4088
  /// ring buffer, only 32 entries on hardware
  uint8_t modtable[64] = {};
  /// playback position
  int modtablepos = 0;
  //4089
  int mastervol_num = 1;
  int mastervol_den = 1;
  /// channel silenced and waveram writable
  bool waveram_writeenable = false;
  //408a
  int envspeed = 0;

  int volumeclock = 0;
  int sweepclock = 0;
  int modclock = 0;
  int mainclock = 0;

  int modoutput = 0;

  // read at 4090
  int volumegain = 0;
  // read at 4092
  int sweepgain = 0;

  int waveramoutput = 0;

  int latchedoutput = 0;

  /// C#'s `x <<= 25; x >>= 25` on an int: sign-extend a 7 bit value. Done through unsigned so the
  /// shift that discards the high bits is not signed overflow.
  static int SignExtend7(int value)
  {
    return (int32_t)((uint32_t)value << 25) >> 25;
  }

  void CalcMod()
  {
    // http://forums.nesdev.com/viewtopic.php?f=3&t=10233
    int tmp = sweepbias * sweepgain;
    int remainder = tmp & 15;
    tmp >>= 4;
    if (remainder > 0 && (tmp & 0x80) == 0)
    {
      if (sweepbias < 0) tmp -= 1;
      else tmp += 2;
    }

    // signed with unconventional bias
    if (tmp >= 192) tmp -= 256;
    else if (tmp < -64) tmp += 256;

    // round to nearest
    tmp *= frequency;
    remainder = tmp & 63;
    tmp >>= 6;
    if (remainder >= 32) tmp++;
    modoutput = tmp;
  }

  /// The difference goes to the APU's external-audio input, which is where a cart's own sound
  /// enters the mix (APU.ExternalQueue). Declared here, defined in nes.hpp, because it needs APU.
  void CalcOut(APU* apu);

  /// ~1.7mhz
  void Clock(APU* apu)
  {
    // volume envelope unit
    if (!r4080_7 && envspeed > 0 && !r4083_6)
    {
      volumeclock++;
      if (volumeclock >= 8 * envspeed * (volumespd + 1))
      {
        volumeclock = 0;
        if (r4080_6 && volumegain < 32) volumegain++;
        else if (!r4080_6 && volumegain > 0) volumegain--;
        CalcOut(apu);
      }
    }
    // sweep unit
    if (!r4084_7 && envspeed > 0 && !r4083_6)
    {
      sweepclock++;
      if (sweepclock >= 8 * envspeed * (sweepspd + 1))
      {
        sweepclock = 0;
        if (r4084_6 && sweepgain < 32) sweepgain++;
        else if (!r4084_6 && sweepgain > 0) sweepgain--;
        CalcMod();
      }
    }
    // modulation unit
    if (!r4087_7 && modfreq > 0)
    {
      modclock += modfreq;
      if (modclock >= 0x10000)
      {
        modclock -= 0x10000;
        // our modtable is really twice as big (64 entries)
        switch (modtable[modtablepos++])
        {
          case 0: sweepbias += 0; break;
          case 1: sweepbias += 1; break;
          case 2: sweepbias += 2; break;
          case 3: sweepbias += 4; break;
          case 4: sweepbias = 0; break;
          case 5: sweepbias -= 4; break;
          case 6: sweepbias -= 2; break;
          case 7: sweepbias -= 1; break;
        }
        sweepbias = SignExtend7(sweepbias);

        modtablepos &= 63;
        CalcMod();
      }
    }
    // main unit
    if (!r4083_7 && frequency > 0 && frequency + modoutput > 0 && !waveram_writeenable)
    {
      mainclock += frequency + modoutput;
      if (mainclock >= 0x10000)
      {
        mainclock -= 0x10000;
        waveramoutput = waveram[waverampos++];
        waverampos &= 63;
        CalcOut(apu);
      }
    }
  }

  void WriteReg(int addr, uint8_t value)
  {
    if (addr < 0x4080)
    {
      if (waveram_writeenable) waveram[addr - 0x4040] = (uint8_t)(value & 63);
      return;
    }
    switch (addr)
    {
      case 0x4080:
        r4080_6 = (value & 0x40) != 0;
        r4080_7 = (value & 0x80) != 0;
        volumeclock = 0;
        volumespd = value & 63;
        if (r4080_7) volumegain = value & 63; // envelope is off, so written value gets sent to gain directly
        break;
      case 0x4082:
        frequency &= 0xF00;
        frequency |= value;
        break;
      case 0x4083:
        frequency &= 0x0FF;
        frequency |= value << 8 & 0xF00;
        r4083_6 = (value & 0x40) != 0;
        r4083_7 = (value & 0x80) != 0;
        if (r4083_7) waverampos = 0;
        if (r4083_6)
        {
          volumeclock = 0;
          sweepclock = 0;
        }
        break;
      case 0x4084:
        sweepspd = value & 63;
        r4084_6 = (value & 0x40) != 0;
        r4084_7 = (value & 0x80) != 0;
        sweepclock = 0;
        if (r4084_7) sweepgain = value & 63;
        break;
      case 0x4085:
        sweepbias = SignExtend7(value & 0x7F);
        break;
      case 0x4086:
        modfreq &= 0xF00;
        modfreq |= value;
        if (r4087_7 || modfreq == 0) modoutput = 0; // when mod unit is disabled, mod output is fixed to 0, not hanging
        break;
      case 0x4087:
        modfreq &= 0x0FF;
        modfreq |= value << 8 & 0xF00;
        r4087_7 = (value & 0x80) != 0;
        if (r4087_7 || modfreq == 0) modoutput = 0;
        if (r4087_7) modclock = 0;
        break;
      case 0x4088:
        // write twice into virtual 64 unit buffer
        if (r4087_7)
        {
          modtable[modtablepos] = (uint8_t)(value & 7);
          modtablepos++;
          modtablepos &= 63;
          modtable[modtablepos] = (uint8_t)(value & 7);
          modtablepos++;
          modtablepos &= 63;
        }
        break;
      case 0x4089:
        switch (value & 3)
        {
          case 0: mastervol_num = 1; mastervol_den = 1; break;
          case 1: mastervol_num = 2; mastervol_den = 3; break;
          case 2: mastervol_num = 2; mastervol_den = 4; break;
          case 3: mastervol_num = 2; mastervol_den = 5; break;
        }
        waveram_writeenable = (value & 0x80) != 0;
        break;
      case 0x408A:
        envspeed = value;
        break;
    }
  }

  uint8_t ReadReg(int addr, uint8_t openbus) const
  {
    uint8_t ret = openbus;

    if (addr < 0x4080)
    {
      ret &= 0xC0;
      ret |= waveram[addr - 0x4040];
    }
    else if (addr == 0x4090)
    {
      ret &= 0xC0;
      ret |= (uint8_t)volumegain;
    }
    else if (addr == 0x4092)
    {
      ret &= 0xC0;
      ret |= (uint8_t)sweepgain;
    }
    return ret;
  }
};

} // namespace nesHawk
