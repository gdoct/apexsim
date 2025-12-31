// Copyright ApexSim Team. All Rights Reserved.

using UnrealBuildTool;
using System.Collections.Generic;

public class ApexSimTarget : TargetRules
{
	public ApexSimTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.V6;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;

		ExtraModuleNames.AddRange(new string[] { "ApexSim" });
	}
}
