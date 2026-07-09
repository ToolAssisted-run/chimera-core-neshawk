// Savestate completeness test for the translated NesHawk core.
//
// Straight-runs a .sol recording per-frame RAM, snapshotting the full state at several checkpoint
// frames. Then, for each checkpoint, restores the snapshot into a FRESH NES instance and replays
// the remaining inputs: every subsequent frame's RAM must match the straight run byte-exactly.
// A fresh instance is the strict variant -- it catches any mutable field missing from the
// serializer (a leak would surface as a divergence after restore).
//
// Usage: stateTest <rom.nes> <inputs.sol> [checkpoint frames...]

#include "../source/nes.hpp"
#include "../source/nesSerialization.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>

static uint8_t parseToken(const std::string& token)
{
  size_t bar = token.find('|', 1);
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
      default: break;
    }
  }
  return buttons;
}

int main(int argc, char** argv)
{
  if (argc < 3)
  {
    fprintf(stderr, "usage: stateTest <rom.nes> <inputs.sol> [checkpoint frames...]\n");
    return 1;
  }

  std::ifstream romFile(argv[1], std::ios::binary);
  std::vector<uint8_t> rom((std::istreambuf_iterator<char>(romFile)), std::istreambuf_iterator<char>());

  std::ifstream solFile(argv[2]);
  std::vector<uint8_t> inputs;
  std::string line;
  while (std::getline(solFile, line))
    if (!line.empty()) inputs.push_back(parseToken(line));

  std::vector<size_t> checkpoints;
  for (int i = 3; i < argc; i++) checkpoints.push_back(strtoull(argv[i], nullptr, 0));
  if (checkpoints.empty()) checkpoints = {1, 100, 801, 2000, 3500, (inputs.size() > 100 ? inputs.size() - 100 : 1)};

  // straight run: record RAM per frame + state snapshots at checkpoints
  nesHawk::NES nes(rom.data(), rom.size());
  const size_t stateSize = nesHawk::nesStateSize(nes);
  fprintf(stderr, "[stateTest] state size: %zu bytes, %zu frames, %zu checkpoints\n", stateSize, inputs.size(), checkpoints.size());

  std::vector<uint8_t> ramLog(inputs.size() * 0x800);
  std::vector<std::vector<uint8_t>> snaps(checkpoints.size(), std::vector<uint8_t>(stateSize));

  for (size_t f = 0; f < inputs.size(); f++)
  {
    nes.FrameAdvance(inputs[f]);
    memcpy(ramLog.data() + f * 0x800, nes.ram, 0x800);
    for (size_t c = 0; c < checkpoints.size(); c++)
      if (checkpoints[c] == f + 1) // snapshot AFTER frame f (state at "frame count = f+1")
      {
        nesHawk::StateSaver s(snaps[c].data());
        nesHawk::syncNesState(nes, s);
      }
  }

  // restore each checkpoint into a fresh instance and replay the tail
  int failures = 0;
  for (size_t c = 0; c < checkpoints.size(); c++)
  {
    size_t start = checkpoints[c];
    if (start >= inputs.size()) continue;

    nesHawk::NES nes2(rom.data(), rom.size());
    nesHawk::StateLoader l(snaps[c].data());
    nesHawk::syncNesState(nes2, l);

    size_t firstBad = (size_t)-1;
    for (size_t f = start; f < inputs.size(); f++)
    {
      nes2.FrameAdvance(inputs[f]);
      if (memcmp(nes2.ram, ramLog.data() + f * 0x800, 0x800) != 0)
      {
        firstBad = f;
        break;
      }
    }

    if (firstBad == (size_t)-1)
      fprintf(stderr, "[stateTest] checkpoint @%zu: OK (%zu tail frames identical)\n", start, inputs.size() - start);
    else
    {
      fprintf(stderr, "[stateTest] checkpoint @%zu: FAIL, first divergent frame %zu\n", start, firstBad);
      failures++;
    }
  }

  if (failures == 0) { fprintf(stderr, "[stateTest] ALL CHECKPOINTS PASS\n"); return 0; }
  return 1;
}
