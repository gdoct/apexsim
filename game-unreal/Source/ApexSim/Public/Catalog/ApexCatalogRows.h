#pragma once

#include "CoreMinimal.h"
#include "Engine/DataTable.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"

#include "ApexCatalogRows.generated.h"

/**
 * Local metadata for content the server only identifies by UUID.
 *
 * Two gaps in the wire protocol make these tables necessary:
 *
 *  - `CarConfigSummary.ModelPath` is built as `res://content/cars/{uuid}/{model}`
 *    (broadcast.rs:150), but the folders on disk are named `redhorse-rb20` and
 *    the like — so the path never resolves. There is no way to reach a car's
 *    mesh from server data alone.
 *  - `TrackConfigSummary` carries only Id and Name. The name is
 *    "Circuit of The Americas" while the preview image is `Austin.png`; nothing
 *    on the wire joins them.
 *
 * The Godot client papered over both by scanning content files at runtime
 * (CarModelCache.cs:97-128, TrackCard.cs:155-203). Baking the join into two
 * DataTables keyed by UUID is cheaper, and it fails visibly (placeholder art
 * plus one warning) rather than silently.
 *
 * Row names are the UUID exactly as the server sends it: lowercase, hyphenated.
 * Lookups are case-insensitive so a hand-edited row cannot break the join.
 */

/** One row per car. RowName == the `id` from `content/cars/<folder>/car.toml`. */
USTRUCT(BlueprintType)
struct APEXSIM_API FApexCarCatalogRow : public FTableRowBase
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Car")
	FString DisplayName;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Car")
	FString Brand;

	/** F1, GT3, Fun — the `class` field in car.toml. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Car")
	FString CarClass;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Car")
	FString ManufacturerCountry;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Car")
	int32 ModelYear = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Car")
	float MassKg = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Car")
	float MaxPowerKw = 0.0f;

	/** Content folder name, e.g. "redhorse-rb20". Useful for diagnostics. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Car")
	FString FolderName;

	/** Soft so the menu does not pull four car meshes into memory at startup. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Car")
	TSoftObjectPtr<UStaticMesh> Mesh;

	/** Per-car tweaks for framing the turntable preview. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Preview")
	FVector PreviewOffset = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Preview")
	FRotator PreviewRotation = FRotator::ZeroRotator;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Preview")
	float PreviewScale = 1.0f;
};

/** One row per track. RowName == the `track_id` from the track's YAML. */
USTRUCT(BlueprintType)
struct APEXSIM_API FApexTrackCatalogRow : public FTableRowBase
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Track")
	FString DisplayName;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Track")
	FString Country;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Track")
	FString City;

	/** F1, DTM, WEC, IndyCar — `metadata.category`. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Track")
	FString Category;

	/** forest, desert, urban — `metadata.environment_type`. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Track")
	FString EnvironmentType;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Track")
	float LengthM = 0.0f;

	/** YAML base name, e.g. "Austin" — this is what the preview PNG is named after. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Track")
	FString YamlBaseName;

	/** Empty for tracks with no preview art (Le Mans); falls back to a placeholder. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Track")
	TSoftObjectPtr<UTexture2D> PreviewImage;
};
