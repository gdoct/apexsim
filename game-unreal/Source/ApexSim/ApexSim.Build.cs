using UnrealBuildTool;

public class ApexSim : ModuleRules
{
	public ApexSim(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore",
			"EnhancedInput",
			"UMG",
			"Slate",
			"SlateCore",
			"ApexSimNet",
			// Wheels, pedals and their force feedback: DirectInput devices as
			// ordinary keys, which is what the bindings and the FFB mixer use.
			"ApexSimInput",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"RenderCore",
			// UApexUiSoundWave derives from USoundWave, whose vtable reaches
			// IAudioProxyDataFactory; that symbol is exported from here.
			"AudioExtensions",
			// The startup splash hold, which UApexStartupSplashSubsystem ends.
			"ApexSimBoot",
		});
	}
}
