using UnrealBuildTool;

public class ApexSimInput : ModuleRules
{
	public ApexSimInput(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"InputCore",
			"InputDevice",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"ApplicationCore",
			"Json",
		});

		// DirectInput 8 is the one API every wheelbase, pedal set, shifter and
		// button box on Windows speaks, force feedback included. Both libraries
		// ship with the Windows SDK.
		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PublicSystemLibraries.AddRange(new string[] { "dinput8.lib", "dxguid.lib" });
		}
	}
}
