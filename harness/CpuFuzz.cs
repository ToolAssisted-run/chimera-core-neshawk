// CPU fuzz-lockstep oracle: runs the genuine C# MOS6502X over a deterministic pseudo-random 64KB
// RAM image (executing random bytes exercises every opcode class, official and unofficial) and logs
// per-cycle register state. The C++ translation runs the identical program with the identical seed;
// the logs must match line-for-line.
//
// Invoked via: nesHawkOracle cpufuzz <cycles> <outFile> [seed]

using System;
using System.IO;
using BizHawk.Emulation.Cores.Components.M6502;

namespace NesHawkOracle
{
	internal sealed class FuzzLink : IMOS6502XLink
	{
		public readonly byte[] Mem = new byte[65536];
		public byte ReadMemory(ushort address) => Mem[address];
		public byte DummyReadMemory(ushort address) => Mem[address];
		public byte PeekMemory(ushort address) => Mem[address];
		public void WriteMemory(ushort address, byte value) => Mem[address] = value;
		public void OnExecFetch(ushort address) { }
	}

	internal static class CpuFuzz
	{
		// xorshift32: identical implementation on the C++ side
		private static uint _rng;
		private static uint Next()
		{
			_rng ^= _rng << 13;
			_rng ^= _rng >> 17;
			_rng ^= _rng << 5;
			return _rng;
		}

		public static int Run(string[] args)
		{
			long cycles = long.Parse(args[1]);
			var outPath = args[2];
			_rng = args.Length > 3 ? uint.Parse(args[3]) : 0xC0FFEE01u;

			var link = new FuzzLink();
			for (int i = 0; i < 65536; i++) link.Mem[i] = (byte)(Next() >> 24);
			// reset vector -> 0x8000 so execution starts in the middle of the random image
			link.Mem[0xFFFC] = 0x00;
			link.Mem[0xFFFD] = 0x80;

			var cpu = new MOS6502X<FuzzLink>(link) { BCD_Enabled = false };

			using var sw = new StreamWriter(outPath);
			for (long i = 0; i < cycles; i++)
			{
				if (i % 97 == 0) { cpu.NMI = (Next() & 1) != 0; cpu.IRQ = (Next() & 1) != 0; }
				cpu.ExecuteOne();
				sw.WriteLine($"{i} {cpu.PC:X4} {cpu.A:X2} {cpu.X:X2} {cpu.Y:X2} {cpu.P:X2} {cpu.S:X2} {cpu.opcode} {cpu.mi}");
			}
			Console.Error.WriteLine($"[cpufuzz] {cycles} cycles -> {outPath}");
			return 0;
		}
	}
}
