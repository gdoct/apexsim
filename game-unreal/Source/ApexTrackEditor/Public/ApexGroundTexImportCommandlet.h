#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "ApexGroundTexImportCommandlet.generated.h"

class UTexture2D;

/**
 * Brings the baked ground textures (`content/textures/ground/<set>_{col,
 * nrm,rough}.png`, written by `scripts/bake_ground_textures.py`) into the
 * project as the tiling maps `M_ApexTrackBase` samples.
 *
 * ```
 * python scripts/bake_ground_textures.py
 * UnrealEditor-Cmd.exe <uproject> -run=ApexGroundTexImport
 * UnrealEditor-Cmd.exe <uproject> -run=ApexGroundTexImport -set=asphalt,grass
 * ```
 *
 * Options:
 *   -set=A,B     only those sets (default: every set found)
 *   -source=DIR  the baked PNGs (default: <project>/../content/textures/ground)
 *   -dest=PATH   content root (default: /Game/Ground)
 *   -dryrun      list what would be imported, write nothing
 *
 * Each file becomes `<dest>/T_ground_<set>_<map>`. Unlike the prop kit
 * these are not per track: the same seven sets dress every circuit, so they
 * live outside `/Game/Tracks`, which the track import deletes and
 * regenerates wholesale.
 *
 * The PNGs are gitignored build products, and so are the textures: a fresh
 * clone has neither, and a track baked without them falls back to the
 * procedural grain the materials carried before. That is why this is a
 * separate run rather than a step of `ApexTrackImport` — it has to happen
 * once, before the first bake, and never again unless a generator changes.
 */
UCLASS()
class APEXTRACKEDITOR_API UApexGroundTexImportCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UApexGroundTexImportCommandlet();

	virtual int32 Main(const FString& Params) override;

private:
	struct FOptions
	{
		/** Empty means every set the source directory holds. */
		TArray<FString> Sets;
		FString SourceDir;
		FString DestRoot;
		bool bDryRun = false;
	};

	/** One PNG to import, and how it is to be interpreted. */
	struct FSource
	{
		FString Set;
		/** `col`, `nrm` or `rough`. */
		FString Map;
		FString Path;
	};

	static bool ParseOptions(const FString& Params, FOptions& Out, FString& OutError);
	static FString DefaultSourceDir();
	/** The PNGs the options select, sorted so a run is reproducible. */
	static bool CollectSources(const FOptions& Options, TArray<FSource>& OutSources, FString& OutError);
	static UTexture2D* ImportMap(
		const FString& PackageName, const FSource& Source, FString& OutError);
};
