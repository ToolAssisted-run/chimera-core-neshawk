// Golden-oracle harness: runs genuine BizHawk NesHawk headless.
//
// Loads a NES ROM, feeds one jaffar-format input line per frame (|..|UDLRSsBA|), and writes the
// full 2KB system RAM after every frame as flat binary (identical layout to jaffar-player
// --dumpRam), so the C++ NesHawk translation can be validated byte-exactly, frame by frame,
// against the original implementation.
//
// Usage: nesHawkOracle <rom.nes> <inputs.sol> <out.ram> [maxFrames]

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using BizHawk.Common;
using BizHawk.Emulation.Common;
using BizHawk.Emulation.Cores.Nintendo.NES;

namespace NesHawkOracle
{
	internal sealed class NullFileProvider : ICoreFileProvider
	{
		public string GetRetroSaveRAMDirectory(string corePath) => throw new NotSupportedException();
		public string GetRetroSystemPath(string corePath) => throw new NotSupportedException();
		public string GetUserPath(string sysID, bool temp) => Path.GetTempPath();
		/// <summary>
		/// The one firmware this harness can hand over: the FDS BIOS, from
		/// NESHAWK_FDS_BIOS. A disk image will not load without it, and no other
		/// firmware is involved in anything NesHawk emulates here.
		/// </summary>
		public byte[] GetFirmware(FirmwareID id, string msg = null)
		{
			if (id.System != "NES" || id.Firmware != "Bios_FDS") return null;
			var path = Environment.GetEnvironmentVariable("NESHAWK_FDS_BIOS");
			return path is not null && File.Exists(path) ? File.ReadAllBytes(path) : null;
		}
		public byte[] GetFirmwareOrThrow(FirmwareID id, string msg = null) => throw new NotSupportedException();
		public (byte[] FW, GameInfo Game) GetFirmwareWithGameInfoOrThrow(FirmwareID id, string msg = null) => throw new NotSupportedException();
	}

	internal sealed class NullGLProvider : IOpenGLProvider
	{
		public bool SupportsGLVersion(int major, int minor) => false;
		public object RequestGLContext(int major, int minor, bool coreProfile) => throw new NotSupportedException();
		public void ReleaseGLContext(object context) { }
		public void ActivateGLContext(object context) { }
		public void DeactivateGLContext() { }
		public IntPtr GetGLProcAddress(string proc) => IntPtr.Zero;
	}

	/// <summary>One-frame controller: parses a jaffar joypad token (|..|UDLRSsBA|) into P1 buttons.</summary>
	internal sealed class SolController : IController
	{
		public ControllerDefinition Definition { get; }
		private readonly HashSet<string> _pressed = new();

		// Button-name resolution: the NES core exposes P1 buttons like "P1 Up" / "P1 A"; resolve by suffix
		// so this stays robust across naming variants.
		private readonly Dictionary<char, string> _map = new();

		public SolController(ControllerDefinition def)
		{
			Definition = def;
			foreach (var (c, suffix) in new[] { ('U', "Up"), ('D', "Down"), ('L', "Left"), ('R', "Right"), ('S', "Start"), ('s', "Select"), ('B', "B"), ('A', "A") })
			{
				var name = def.BoolButtons.FirstOrDefault(b => b.StartsWith("P1 ", StringComparison.Ordinal) && b.Substring(3) == suffix);
				if (name == null) throw new InvalidOperationException($"could not resolve P1 button '{suffix}' in [{string.Join(", ", def.BoolButtons)}]");
				_map[c] = name;
			}
		}

		public void SetFromToken(string token)
		{
			_pressed.Clear();
			// token: |..|UDLRSsBA| -- port 2 section may follow; we only drive P1.
			int bar = token.IndexOf('|', 1);
			if (token.Length < bar + 9) throw new InvalidDataException($"bad input token: '{token}'");
			string pad = token.Substring(bar + 1, 8);
			foreach (var c in pad)
				if (c != '.')
					_pressed.Add(_map[c]);
		}

		public bool IsPressed(string button) => _pressed.Contains(button);
		public int AxisValue(string name) => 0;
		public IReadOnlyCollection<(string Name, int Strength)> GetHapticsSnapshot() => Array.Empty<(string, int)>();
		public void SetHapticChannelStrength(string name, int strength) { }
	}

	internal static class Program
	{
		private static int Main(string[] args)
		{
			if (args.Length >= 1 && args[0] == "cpufuzz") return CpuFuzz.Run(args);

			if (args.Length < 3)
			{
				Console.Error.WriteLine("usage: nesHawkOracle <rom.nes> <inputs.sol> <out.ram> [maxFrames]");
				return 1;
			}

			var romPath = args[0];
			var solPath = args[1];
			var outPath = args[2];
			var maxFrames = args.Length > 3 ? int.Parse(args[3]) : int.MaxValue;

			var rom = File.ReadAllBytes(romPath);
			var lines = File.ReadAllLines(solPath).Where(l => l.Length > 0).ToArray();

			// Client-responsibility database initializations (board identification blocks without them)
			var gamedb = Environment.GetEnvironmentVariable("HOME") + "/BizHawk/Assets/gamedb";
			BizHawk.Emulation.Common.Database.InitializeDatabase(gamedb, gamedb, silent: true);
			BootGodDb.Initialize(gamedb);

			var comm = new CoreComm(msg => Console.Error.WriteLine($"[show] {msg}"), (msg, _) => Console.Error.WriteLine($"[osd] {msg}"),
				new NullFileProvider(), CoreComm.CorePreferencesFlags.None, new NullGLProvider());
			var game = new GameInfo { Name = Path.GetFileNameWithoutExtension(romPath), System = VSystemID.Raw.NES };
			var nes = new NES(comm, game, rom, new NES.NESSettings(), new NES.NESSyncSettings());

			Console.Error.WriteLine($"[oracle] board: {nes.BoardName}, buttons: [{string.Join(", ", nes.ControllerDefinition.BoolButtons)}]");

			// Dump the resolved cart configuration (internal field, via reflection) so the C++
			// translation can hardcode identical parameters instead of reimplementing board ID.
			var cartField = typeof(NES).GetField("cart", System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Instance);
			var cartObj = cartField.GetValue(nes);
			foreach (var p in cartObj.GetType().GetProperties())
				Console.Error.WriteLine($"[cart] {p.Name} = {p.GetValue(cartObj)}");
			foreach (var f in cartObj.GetType().GetFields())
				Console.Error.WriteLine($"[cart] {f.Name} = {f.GetValue(cartObj)}");

			var controller = new SolController(nes.ControllerDefinition);
			var ramDomain = nes.AsMemoryDomains()["RAM"];
			if (ramDomain == null || ramDomain.Size != 0x800) throw new InvalidOperationException($"unexpected RAM domain (size {ramDomain?.Size})");

			using var outFile = File.Create(outPath);
			var buf = new byte[0x800];

			int frames = Math.Min(lines.Length, maxFrames);
			for (int i = 0; i < frames; i++)
			{
				controller.SetFromToken(lines[i]);
				nes.FrameAdvance(controller, render: false, rendersound: false);
				ramDomain.BulkPeekByte(0L.MutableRangeTo(ramDomain.Size - 1), buf);
				outFile.Write(buf, 0, buf.Length);
			}

			Console.Error.WriteLine($"[oracle] wrote {frames} frames x 2KB to {outPath}");
			return 0;
		}
	}
}
