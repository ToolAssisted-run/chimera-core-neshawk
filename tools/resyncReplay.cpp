// Adaptive movie resync onto the NesHawk core.
//
// Takes the GAME-FRAME input sequence (inputs the game actually consumed, i.e. the source movie
// with its lag-frame padding removed) and re-times it against NesHawk: each frame the head of the
// queue is fed to the core, and it is popped only when the game actually latched the controller
// that frame (nes.islag == false). Lag-frame count differences between the source emulator and
// NesHawk then re-pad themselves automatically. The emitted per-frame .sol is the NesHawk-synced
// movie.
//
// Usage: resyncReplay <rom.nes> <gameInputs.txt> <out.sol> <out.ram> [maxFrames] [--menuPrefix]
//
// --menuPrefix: instead of starting the queue at power-on, self-generate the boot/menu prefix
// (nulls -> held Start at the title -> held Right at the story screen -> wait for level-1 room-1
// to be drawn, RAM 0x51 == 1) and only then start the adaptive queue. Menu poll patterns differ
// between emulators, so cross-emulator game-frame alignment is only meaningful from a common
// deterministic anchor -- the drawn==1 transition. The queue must then be the game-frame inputs
// from that same anchor on the source side.

#include "../source/nes.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <deque>

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

static std::string tokenOf(uint8_t b)
{
  std::string s = "|..|........|";
  if (b & nesHawk::BUTTON_UP) s[4] = 'U';
  if (b & nesHawk::BUTTON_DOWN) s[5] = 'D';
  if (b & nesHawk::BUTTON_LEFT) s[6] = 'L';
  if (b & nesHawk::BUTTON_RIGHT) s[7] = 'R';
  if (b & nesHawk::BUTTON_START) s[8] = 'S';
  if (b & nesHawk::BUTTON_SELECT) s[9] = 's';
  if (b & nesHawk::BUTTON_B) s[10] = 'B';
  if (b & nesHawk::BUTTON_A) s[11] = 'A';
  return s;
}

int main(int argc, char** argv)
{
  if (argc < 5)
  {
    fprintf(stderr, "usage: resyncReplay <rom.nes> <gameInputs.txt> <out.sol> <out.ram> [maxFrames]\n");
    return 1;
  }

  std::ifstream romFile(argv[1], std::ios::binary);
  std::vector<uint8_t> rom((std::istreambuf_iterator<char>(romFile)), std::istreambuf_iterator<char>());

  std::ifstream inFile(argv[2]);
  std::deque<uint8_t> queue;
  std::string line;
  while (std::getline(inFile, line))
    if (!line.empty()) queue.push_back(parseToken(line));

  const size_t totalInputs = queue.size();
  size_t maxFrames = totalInputs * 3;
  bool menuPrefix = false;
  for (int i = 5; i < argc; i++)
  {
    if (strcmp(argv[i], "--menuPrefix") == 0) menuPrefix = true;
    else maxFrames = strtoull(argv[i], nullptr, 0);
  }

  nesHawk::NES nes(rom.data(), rom.size());

  std::ofstream outSol(argv[3]);
  FILE* outRam = fopen(argv[4], "wb");
  // sidecar: one byte per emitted frame, 1 = the game consumed the fed input this frame
  FILE* outFlags = fopen((std::string(argv[4]) + ".flags").c_str(), "wb");

  size_t frames = 0, consumed = 0;

  if (menuPrefix)
  {
    // one lambda per prefix frame: feed a fixed input, record it, no queue interaction
    auto prefixFrame = [&](uint8_t b)
    {
      nes.FrameAdvance(b);
      outSol << tokenOf(b) << "\n";
      fwrite(nes.ram, 1, 0x800, outRam);
      uint8_t flag = 0;
      fwrite(&flag, 1, 1, outFlags);
      frames++;
    };

    while (frames < 560) prefixFrame(0);                          // boot + title fade-in (title sits stable for thousands of frames)
    for (int i = 0; i < 30; i++) prefixFrame(nesHawk::BUTTON_START); // held Start: guaranteed edge at the title
    while (frames < 650) prefixFrame(0);                          // story screen appears
    for (int i = 0; i < 30; i++) prefixFrame(nesHawk::BUTTON_RIGHT); // held Right: advance/skip story
    while (nes.ram[0x51] != 1 && frames < 4000) prefixFrame(0);   // wait for level-1 room-1 drawn
    if (nes.ram[0x51] != 1)
    {
      fprintf(stderr, "[resyncReplay] menu prefix failed: drawn room never became 1 (frame %zu)\n", frames);
      return 2;
    }
    fprintf(stderr, "[resyncReplay] menu prefix done: drawn==1 at frame %zu\n", frames);
  }
  while (!queue.empty() && frames < maxFrames)
  {
    uint8_t b = queue.front();
    nes.FrameAdvance(b);
    outSol << tokenOf(b) << "\n";
    fwrite(nes.ram, 1, 0x800, outRam);
    uint8_t flag = nes.islag ? 0 : 1;
    fwrite(&flag, 1, 1, outFlags);
    frames++;
    if (!nes.islag)
    {
      queue.pop_front();
      consumed++;
    }
  }
  fclose(outFlags);

  fclose(outRam);
  fprintf(stderr, "[resyncReplay] frames emitted: %zu, game inputs consumed: %zu/%zu, final level(0x70)=%u\n",
          frames, consumed, totalInputs, nes.ram[0x70]);
  return queue.empty() ? 0 : 1;
}
