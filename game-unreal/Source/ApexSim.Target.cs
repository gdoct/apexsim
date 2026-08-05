using UnrealBuildTool;

public class ApexSimTarget : TargetRules
{
	public ApexSimTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.AddRange(new string[] { "ApexSimNet", "ApexSim" });
	}
}
