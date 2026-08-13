// C++ side of the system-level validation (see harness/Program.cs for the C# oracle side).
// Replays a jaffar .sol on the translated NesHawk core and writes the full 2KB system RAM after
// every frame as flat binary -- identical format to the oracle, so grading is a byte compare.
//
// Usage: nesReplay <rom.nes> <inputs.sol> <out.ram> [maxFrames]

#include "../source/nes.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>

// parse one jaffar input token |..|UDLRSsBA| into a ButtonBit mask (P1 only)
static uint8_t parseToken(const std::string& token, size_t lineNo)
{
  size_t bar = token.find('|', 1);
  if (bar == std::string::npos || token.size() < bar + 9)
  {
    fprintf(stderr, "bad input token at line %zu: '%s'\n", lineNo, token.c_str());
    exit(1);
  }
  uint8_t buttons = 0;
  for (int i = 0; i < 8; i++)
  {
    switch (token[bar + 1 + i])
    {
      case 'U': buttons |= nesHawk::BUTTON_UP; break;
      case 'D': buttons |= nesHawk::BUTTON_DOWN; break;
      case 'L': buttons |= nesHawk::BUTTON_LEFT; break;
      case 'R': buttons |= nesHawk::BUTTON_RIGHT; break;
      case 'S': buttons |= nesHawk::BUTTON_START; break;
      case 's': buttons |= nesHawk::BUTTON_SELECT; break;
      case 'B': buttons |= nesHawk::BUTTON_B; break;
      case 'A': buttons |= nesHawk::BUTTON_A; break;
      case '.': break;
      default:
        fprintf(stderr, "bad input char '%c' at line %zu: '%s'\n", token[bar + 1 + i], lineNo, token.c_str());
        exit(1);
    }
  }
  return buttons;
}

int main(int argc, char** argv)
{
  if (argc < 4)
  {
    fprintf(stderr, "usage: nesReplay <rom.nes> <inputs.sol> <out.ram> [maxFrames]\n");
    return 1;
  }

  std::ifstream romFile(argv[1], std::ios::binary);
  if (!romFile) { fprintf(stderr, "cannot open rom %s\n", argv[1]); return 1; }
  std::vector<uint8_t> rom((std::istreambuf_iterator<char>(romFile)), std::istreambuf_iterator<char>());

  std::ifstream solFile(argv[2]);
  if (!solFile) { fprintf(stderr, "cannot open sol %s\n", argv[2]); return 1; }
  std::vector<uint8_t> inputs;
  std::string line;
  size_t lineNo = 0;
  while (std::getline(solFile, line))
  {
    lineNo++;
    if (line.empty()) continue;
    inputs.push_back(parseToken(line, lineNo));
  }

  size_t maxFrames = argc > 4 ? strtoull(argv[4], nullptr, 0) : inputs.size();
  if (maxFrames > inputs.size()) maxFrames = inputs.size();

  // An FDS disk image needs the disk system BIOS; the C# oracle reads the same variable, so a
  // comparison run gets the identical machine on both sides.
  std::vector<uint8_t> bios;
  if (const char* biosPath = getenv("NESHAWK_FDS_BIOS"))
  {
    if (FILE* f = fopen(biosPath, "rb"))
    {
      uint8_t buf[8192];
      const size_t n = fread(buf, 1, sizeof buf, f);
      fclose(f);
      bios.assign(buf, buf + n);
    }
  }

  nesHawk::NES nes(rom.data(), rom.size(), nesHawk::PPU::Region::NTSC, nullptr, 0,
                   bios.empty() ? nullptr : bios.data(), bios.size());

  FILE* out = fopen(argv[3], "wb");
  if (out == nullptr) { fprintf(stderr, "cannot open %s\n", argv[3]); return 1; }

  for (size_t i = 0; i < maxFrames; i++)
  {
    nes.FrameAdvance(inputs[i]);
    fwrite(nes.ram, 1, 0x800, out);
  }
  fclose(out);

  fprintf(stderr, "[nesReplay] wrote %zu frames x 2KB to %s (lag %d)\n", maxFrames, argv[3], nes._lagcount);
  return 0;
}
