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
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"RenderCore",
			// UApexUiSoundWave derives from USoundWave, whose vtable reaches
			// IAudioProxyDataFactory; that symbol is exported from here.
			"AudioExtensions",
		});
	}
}
