#pragma once

#include "CoreMinimal.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"

/**
 * Where the editor finds the repo's two roots. The Unreal project is
 * `apexsim/game-unreal`, so its parent is the CODE root (`hud/`, `build/showcase`,
 * `build/guide`, ...), and the content repo sits beside the code one as
 * `apexsim-content` (`cars/`, `tracks/`, `props/`, `wheels/`, `textures/`,
 * `build/tracks`). A packaged game reads neither: its folders sit beside
 * ApexSim.exe. Header-only on purpose, so the commandlets and the tests share it.
 */
namespace ApexRepoPaths
{
	/** The code root: `<project>/..`, as a full path. */
	inline FString CodeRoot()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("..")));
	}

	/** The content root: `-ApexContentRoot=DIR`, else `<project>/../../apexsim-content`, as a full path. */
	inline FString ContentRoot()
	{
		FString Override;
		if (FParse::Value(FCommandLine::Get(), TEXT("-ApexContentRoot="), Override) && !Override.IsEmpty())
		{
			return FPaths::ConvertRelativePathToFull(Override);
		}
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT(".."), TEXT(".."), TEXT("apexsim-content")));
	}

	/** A path under the content root, e.g. `ContentPath(TEXT("cars"))`. */
	inline FString ContentPath(const FString& Relative)
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(ContentRoot(), Relative));
	}

	/** A path under the code root, e.g. `CodePath(TEXT("hud"))`. */
	inline FString CodePath(const FString& Relative)
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(CodeRoot(), Relative));
	}
}
