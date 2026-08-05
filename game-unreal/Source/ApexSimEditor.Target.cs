using UnrealBuildTool;

public class ApexSimEditorTarget : TargetRules
{
	public ApexSimEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.AddRange(new string[] { "ApexSimNet", "ApexSim" });
	}
}
