// C++ side of the CPU fuzz-lockstep (see harness/CpuFuzz.cs for the C# oracle side).
// Identical pseudo-random RAM image, identical reset vector, identical per-cycle log format.
//
// Usage: cpuFuzz <cycles> <outFile> [seed]

#include "../source/mos6502x.hpp"
#include <cstdio>
#include <cstdlib>

struct FuzzLink
{
  uint8_t mem[65536];
  uint8_t ReadMemory(uint16_t a) { return mem[a]; }
  uint8_t DummyReadMemory(uint16_t a) { return mem[a]; }
  uint8_t PeekMemory(uint16_t a) { return mem[a]; }
  void    WriteMemory(uint16_t a, uint8_t v) { mem[a] = v; }
  void    OnExecFetch(uint16_t) {}
};

static uint32_t rngState;
static uint32_t next()
{
  rngState ^= rngState << 13;
  rngState ^= rngState >> 17;
  rngState ^= rngState << 5;
  return rngState;
}

int main(int argc, char** argv)
{
  if (argc < 3) { fprintf(stderr, "usage: cpuFuzz <cycles> <outFile> [seed]\n"); return 1; }
  const long cycles = atol(argv[1]);
  rngState          = argc > 3 ? (uint32_t)strtoul(argv[3], nullptr, 0) : 0xC0FFEE01u;

  FuzzLink link;
  for (int i = 0; i < 65536; i++) link.mem[i] = (uint8_t)(next() >> 24);
  link.mem[0xFFFC] = 0x00;
  link.mem[0xFFFD] = 0x80;

  nesHawk::MOS6502X<FuzzLink> cpu(link);
  cpu.BCD_Enabled = false;

  FILE* f = fopen(argv[2], "w");
  if (f == nullptr) { fprintf(stderr, "cannot open %s\n", argv[2]); return 1; }
  for (long i = 0; i < cycles; i++)
  {
    if (i % 97 == 0) { cpu.NMI = (next() & 1) != 0; cpu.IRQ = (next() & 1) != 0; }
    cpu.ExecuteOne();
    fprintf(f, "%ld %04X %02X %02X %02X %02X %02X %d %d\n", i, cpu.PC, cpu.A, cpu.X, cpu.Y, cpu.P, cpu.S, cpu.opcode, cpu.mi);
  }
  fclose(f);
  fprintf(stderr, "[cpufuzz-cpp] %ld cycles -> %s\n", cycles, argv[2]);
  return 0;
}
