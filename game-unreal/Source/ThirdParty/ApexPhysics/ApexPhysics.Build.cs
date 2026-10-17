using System.IO;
using UnrealBuildTool;

/// <summary>
/// The server's car step as a prebuilt static library (server/ffi, crate
/// apexsim-physics), for the client's prediction of its own car
/// (docs/proposals/client-prediction.md, docs/building.md).
///
/// The header is the hand-written server/ffi/apexsim_physics.h, read where it
/// lives. The library is built from the server's source, never checked in:
/// scripts/build_physics_lib.ps1 runs `cargo build --release` in server/ffi
/// and copies the .lib to lib/Win64/ here. Without it the client still
/// builds and runs, with WITH_APEX_PHYSICS 0 and prediction unavailable.
/// UBT reads this file again only when it changes, so the script touches it
/// when the library appears or goes.
/// </summary>
public class ApexPhysics : ModuleRules
{
	public ApexPhysics(ReadOnlyTargetRules Target) : base(Target)
	{
		Type = ModuleType.External;

		string RepoRoot = Path.GetFullPath(Path.Combine(ModuleDirectory, "..", "..", "..", ".."));
		PublicSystemIncludePaths.Add(Path.Combine(RepoRoot, "server", "ffi"));

		string Library = Path.Combine(ModuleDirectory, "lib", "Win64", "apexsim_physics.lib");
		bool bHaveLibrary = Target.Platform == UnrealTargetPlatform.Win64 && File.Exists(Library);
		PublicDefinitions.Add("WITH_APEX_PHYSICS=" + (bHaveLibrary ? "1" : "0"));
		if (!bHaveLibrary)
		{
			return;
		}

		PublicAdditionalLibraries.Add(Library);
		// What the Rust standard library and the server crate link against
		// (`cargo rustc --release --lib --crate-type staticlib -- --print
		// native-static-libs`); the CRT is the dynamic one, as every module's.
		PublicSystemLibraries.AddRange(new string[]
		{
			"kernel32.lib", "winmm.lib", "bcrypt.lib", "advapi32.lib", "ntdll.lib",
			"userenv.lib", "ws2_32.lib", "dbghelp.lib",
		});
	}
}
