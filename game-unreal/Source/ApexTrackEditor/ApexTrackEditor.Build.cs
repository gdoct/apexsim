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
			// FApexTrackCatalogRow, the DT_TrackCatalog row struct.
			"ApexSim",
			// FImage, for importing preview PNGs as textures.
			"ImageCore",
		});
	}
}
