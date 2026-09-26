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
			// Runtime tracks: the scene builder hands mesh descriptions between
			// threads (Track/ApexTrackSceneBuilder.h).
			"MeshDescription",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"RenderCore",
			// UApexUiSoundWave derives from USoundWave, whose vtable reaches
			// IAudioProxyDataFactory; that symbol is exported from here.
			"AudioExtensions",
			// The startup splash hold, which UApexStartupSplashSubsystem ends.
			"ApexSimBoot",
			// Replay clips (`-ApexReplay=`) are JSON cut by `apexsim-replay`,
			// and track exports (`.uescene.json`) are JSON too.
			"Json",
			// Runtime tracks: static mesh attributes for the builder, and the
			// collision component's body setup.
			"StaticMeshDescription",
			"PhysicsCore",
		});
	}
}
