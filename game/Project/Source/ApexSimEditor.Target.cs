// Copyright ApexSim Team. All Rights Reserved.

using UnrealBuildTool;
using System.Collections.Generic;

public class ApexSimEditorTarget : TargetRules
{
	public ApexSimEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.V6;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;

		ExtraModuleNames.AddRange(new string[] { "ApexSim" });
	}
}
