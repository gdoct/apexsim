using UnrealBuildTool;

/**
 * The startup splash hold, loaded before the engine creates the game window
 * (LoadingPhase PreEarlyLoadingScreen), which is far earlier than a module
 * with UObjects can load. No UObjects here: Win32, DWM and the core ticker.
 * UApexStartupSplashSubsystem in ApexSim decides when the hold ends.
 */
public class ApexSimBoot : ModuleRules
{
	public ApexSimBoot(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] { "Core" });

		// DWMWA_CLOAK hides the game window from the compositor while it renders.
		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PublicSystemLibraries.Add("dwmapi.lib");
		}
	}
}
