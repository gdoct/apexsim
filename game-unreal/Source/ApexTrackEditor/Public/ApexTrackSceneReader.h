#pragma once

#include "CoreMinimal.h"

struct FApexTrackScene;

/** Loads `.uescene.json` files produced by `ats-export`. */
class APEXTRACKEDITOR_API FApexTrackSceneReader
{
public:
	/**
	 * Parse one export.
	 *
	 * Returns false and fills `OutError` on unreadable files, wrong format
	 * tags, an unsupported version, or geometry whose buffers disagree with
	 * each other. Nothing partial is written to `OutScene` on failure.
	 */
	static bool LoadFromFile(const FString& Path, FApexTrackScene& OutScene, FString& OutError);

	/** Format tag every export carries. */
	static const TCHAR* FormatTag();

	/** Highest format version this reader understands. */
	static int32 SupportedVersion();
};
