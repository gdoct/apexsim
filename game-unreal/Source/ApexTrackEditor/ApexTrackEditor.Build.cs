using UnrealBuildTool;

public class ApexTrackEditor : ModuleRules
{
	public ApexTrackEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Json",
			"MeshDescription",
			"StaticMeshDescription",
			"UnrealEd",
			"AssetRegistry",
		});
	}
}
