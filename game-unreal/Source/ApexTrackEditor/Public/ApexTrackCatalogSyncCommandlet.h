#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "ApexTrackCatalogSyncCommandlet.generated.h"

class UDataTable;
class UTexture2D;

/**
 * Brings `DT_TrackCatalog` and the `T_Track_<Stem>` preview textures in line
 * with the track YAMLs, from the manifest `scripts/build_track_catalog.py`
 * bakes.
 *
 * ```
 * python scripts/build_track_catalog.py
 * ApexSimEditor-Cmd.exe <uproject> -run=ApexTrackCatalogSync
 * ```
 *
 * Options:
 *   -manifest=PATH   the baked manifest
 *                    (default: <project>/../content/tracks/export/track_catalog.json)
 *   -table=PATH      the catalog data table (default: /Game/Data/DT_TrackCatalog)
 *   -previews=PATH   content folder for preview textures (default: /Game/UI/TrackPreviews)
 *   -force           rewrite every row from the YAML and re-import every texture
 *   -dryrun          report what would change, write nothing
 *
 * Without -force the pass is additive: a track with no row gets one, a row
 * with no preview gets the texture, and anything else already in the table is
 * left as it is — so a hand-tuned display name survives a routine sync.
 */
UCLASS()
class APEXTRACKEDITOR_API UApexTrackCatalogSyncCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UApexTrackCatalogSyncCommandlet();

	virtual int32 Main(const FString& Params) override;

private:
	struct FOptions
	{
		FString ManifestPath;
		FString TablePath;
		FString PreviewRoot;
		bool bForce = false;
		bool bDryRun = false;
	};

	/** One manifest entry, i.e. one track YAML. */
	struct FEntry
	{
		FString TrackId;
		FString Stem;
		FString DisplayName;
		FString Country;
		FString City;
		FString Category;
		FString EnvironmentType;
		float LengthM = 0.0f;
		FString PreviewPng;
	};

	static bool ParseOptions(const FString& Params, FOptions& Out, FString& OutError);
	static bool LoadManifest(const FString& Path, TArray<FEntry>& OutEntries, FString& OutError);

	/**
	 * The preview texture for a track: the existing asset unless -force, else
	 * a fresh import of the PNG. Null (with OutError set) when the import fails;
	 * null with OutError empty when there is no PNG to import.
	 */
	UTexture2D* ResolvePreview(const FEntry& Entry, const FOptions& Options,
		TSet<UPackage*>& TouchedPackages, FString& OutError);

	static UTexture2D* ImportPreview(const FString& PackageName, const FString& PngPath, FString& OutError);
	static bool SavePackages(const TSet<UPackage*>& Packages, FString& OutError);
};
