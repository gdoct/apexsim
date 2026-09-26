#pragma once

#include "CoreMinimal.h"
#include "Engine/DataTable.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Race/ApexCockpitLayout.h"

#include "ApexCatalogRows.generated.h"

/**
 * Local metadata for content the server only identifies by UUID.
 *
 * Two gaps in the wire protocol make these tables necessary:
 *
 *  - `CarConfigSummary.ModelPath` is built as `res://content/cars/{uuid}/{model}`
 *    (broadcast.rs:150), but the folders on disk are named `fugazzi-sf26` and
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

/**
 * Where a car's wheels go and which shared wheel model they use: the
 * `[wheels]` table of car.toml (docs/CAR_MODELS.md), filled by
 * ApexCarImport. The body meshes carry no wheels; the client draws four
 * copies of `Mesh`, sized per axle, steers the front pair and spins all four.
 *
 * Metres, in the body mesh's frame: axles ahead (+) or behind (-) its
 * origin, hubs one radius above its floor.
 */
USTRUCT(BlueprintType)
struct APEXSIM_API FApexWheelSpec
{
	GENERATED_BODY()

	/** The class's shared wheel, e.g. /Game/Cars/Wheels/f1/SM_Wheel_f1. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Wheels")
	TSoftObjectPtr<UStaticMesh> Mesh;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Wheels")
	float FrontAxleM = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Wheels")
	float RearAxleM = 0.0f;

	/** Hub centre to hub centre. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Wheels")
	float FrontTrackM = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Wheels")
	float RearTrackM = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Wheels")
	float FrontRadiusM = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Wheels")
	float RearRadiusM = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Wheels")
	float FrontWidthM = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Wheels")
	float RearWidthM = 0.0f;

	/** Front wheel angle at full steering input: `[physics] max_steering_angle_rad`. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Wheels")
	float MaxSteerRad = 0.0f;

	/** A mesh and a size for both axles: enough to draw. */
	bool IsUsable() const
	{
		return !Mesh.IsNull() && FrontRadiusM > 0.0f && RearRadiusM > 0.0f && FrontWidthM > 0.0f && RearWidthM > 0.0f;
	}

	bool operator==(const FApexWheelSpec& Other) const
	{
		return Mesh == Other.Mesh && FrontAxleM == Other.FrontAxleM && RearAxleM == Other.RearAxleM
			&& FrontTrackM == Other.FrontTrackM && RearTrackM == Other.RearTrackM
			&& FrontRadiusM == Other.FrontRadiusM && RearRadiusM == Other.RearRadiusM
			&& FrontWidthM == Other.FrontWidthM && RearWidthM == Other.RearWidthM
			&& MaxSteerRad == Other.MaxSteerRad;
	}
	bool operator!=(const FApexWheelSpec& Other) const { return !(*this == Other); }
};

/**
 * An F1 car's DRS flap: the rear wing's upper element, cut out of the body
 * mesh into its own so the client can open it. The `[drs_flap]` table of
 * car.toml (docs/CAR_MODELS.md), filled by ApexCarImport.
 *
 * The mesh's origin is the hinge, in the body mesh's frame otherwise; the
 * hinge axis runs across the car (the frame's X). Metres, like the wheels:
 * the hinge `HingeForwardM` ahead (+) of the body's origin and `HingeUpM`
 * above its floor. Open, the flap turns `OpenDeg` about the hinge with its
 * leading edge rising, which opens the slot under it.
 */
USTRUCT(BlueprintType)
struct APEXSIM_API FApexDrsFlapSpec
{
	GENERATED_BODY()

	/** e.g. /Game/Cars/fugazzi_sf26/Drs/SM_fugazzi_sf26_drs. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "DRS")
	TSoftObjectPtr<UStaticMesh> Mesh;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "DRS")
	float HingeForwardM = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "DRS")
	float HingeUpM = 0.0f;

	/** How far the flap turns open, degrees; positive lifts its leading edge. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "DRS")
	float OpenDeg = 0.0f;

	bool IsUsable() const { return !Mesh.IsNull(); }

	bool operator==(const FApexDrsFlapSpec& Other) const
	{
		return Mesh == Other.Mesh && HingeForwardM == Other.HingeForwardM && HingeUpM == Other.HingeUpM
			&& OpenDeg == Other.OpenDeg;
	}
	bool operator!=(const FApexDrsFlapSpec& Other) const { return !(*this == Other); }
};

/**
 * The engine as the client's synthesiser hears it: a car.toml's `[sound]`
 * table plus the rev range from its `[engine]`. See ApexEngineSound.h for what
 * each figure does to the note, and ApexEngineAudio::MakeSpec for the defaults
 * a car without the table gets from its class.
 */
USTRUCT(BlueprintType)
struct APEXSIM_API FApexEngineSoundSpec
{
	GENERATED_BODY()

	/** Four-stroke cylinders. 0 = no `[sound]` table: the class decides. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Sound")
	int32 Cylinders = 0;

	/** A crossplane V8: uneven firing into each bank, the burble. Ignored for anything but eight cylinders. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Sound")
	bool bCrossplane = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Sound")
	bool bTurbo = false;

	/** `[engine]` idle_rpm, redline_rpm and rev_limiter_rpm: neither end of the range is on the wire. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Sound")
	float IdleRpm = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Sound")
	float RedlineRpm = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Sound")
	float LimiterRpm = 0.0f;

	/** Primary pipe length, metres. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Sound")
	float ExhaustLengthM = 0.0f;

	/** 0 open pipes .. 1 a road muffler. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Sound")
	float Muffling = 0.0f;

	/** Overrun pops and shift cracks, 0..1. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Sound")
	float Pops = 0.0f;

	/** Straight-cut gearbox whine, 0..1. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Sound")
	float GearWhine = 0.0f;

	/** Induction roar, 0..1. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Sound")
	float IntakeRoar = 0.0f;

	bool operator==(const FApexEngineSoundSpec& Other) const
	{
		return Cylinders == Other.Cylinders && bCrossplane == Other.bCrossplane && bTurbo == Other.bTurbo
			&& IdleRpm == Other.IdleRpm && RedlineRpm == Other.RedlineRpm && LimiterRpm == Other.LimiterRpm
			&& ExhaustLengthM == Other.ExhaustLengthM && Muffling == Other.Muffling && Pops == Other.Pops
			&& GearWhine == Other.GearWhine && IntakeRoar == Other.IntakeRoar;
	}
	bool operator!=(const FApexEngineSoundSpec& Other) const { return !(*this == Other); }
};

/**
 * One of a car's extra paint schemes: a `[[livery]]` table in its car.toml.
 * The mesh stays the same; the client repaints the `car_paint` and
 * `car_accent` slots and swaps the `car_logo` texture (ApexCarLivery.h).
 */
USTRUCT(BlueprintType)
struct APEXSIM_API FApexCarLivery
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Livery")
	FString Name;

	/** Body colour, linear (the glTF `BaseColorFactor` of `car_paint`). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Livery")
	FLinearColor Paint = FLinearColor::White;

	/** The accent areas' colour, linear (`car_accent`); zero alpha keeps the model's. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Livery")
	FLinearColor Accent = FLinearColor(0.0f, 0.0f, 0.0f, 0.0f);

	/** The paint's metallic factor; negative keeps the model's. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Livery")
	float PaintMetallic = -1.0f;

	/** The wordmark on the flanks (`car_logo`); unset keeps the model's. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Livery")
	TSoftObjectPtr<UTexture2D> Logo;

	bool operator==(const FApexCarLivery& Other) const
	{
		return Name == Other.Name && Paint == Other.Paint && Accent == Other.Accent
			&& PaintMetallic == Other.PaintMetallic && Logo == Other.Logo;
	}
	bool operator!=(const FApexCarLivery& Other) const { return !(*this == Other); }
};

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

	/** Content folder name, e.g. "fugazzi-sf26". Useful for diagnostics. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Car")
	FString FolderName;

	/**
	 * Checksum of the car.toml the row was imported from, as the server
	 * computes it (ApexContentCrc.h / content_crc.rs). Refreshed on every
	 * ApexCarImport run; compared with the server's `ContentCrc` when the car
	 * is raced. 0 on a row imported before the field existed.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Car")
	int64 SourceCrc = 0;

	/** Soft so the menu does not pull four car meshes into memory at startup. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Car")
	TSoftObjectPtr<UStaticMesh> Mesh;

	/**
	 * The wheels drawn on the (wheel-less) body. Derived from car.toml on
	 * every import, like SourceCrc; not a hand-tuned field.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Car")
	FApexWheelSpec Wheels;

	/**
	 * The DRS flap, drawn apart from the body so it can open (F1 cars only;
	 * an unusable spec means the body carries its whole wing). Derived from
	 * car.toml on every import, like the wheels.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Car")
	FApexDrsFlapSpec DrsFlap;

	/** What the engine sounds like. Derived from car.toml on every import, like the wheels. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Car")
	FApexEngineSoundSpec EngineSound;

	/**
	 * The car's extra liveries, the car.toml's `[[livery]]` tables in order:
	 * livery N on the wire is `Liveries[N - 1]`, livery 0 the model as
	 * authored. Derived on every import, like the wheels.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Car")
	TArray<FApexCarLivery> Liveries;

	/** Per-car tweaks for framing the turntable preview. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Preview")
	FVector PreviewOffset = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Preview")
	FRotator PreviewRotation = FRotator::ZeroRotator;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Preview")
	float PreviewScale = 1.0f;

	/**
	 * Hand-placed cockpit points for the driver's view, in the car's frame.
	 * Left at zero, the seat and mirrors are derived from the mesh bounds;
	 * a mesh with a modelled interior wants its eye put exactly in the seat.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Cockpit")
	FApexCockpitOverrides Cockpit;
};

/** One row per track. RowName == the `track_id` from the track's YAML. */
USTRUCT(BlueprintType)
struct APEXSIM_API FApexTrackCatalogRow : public FTableRowBase
{
	GENERATED_BODY()

	/**
	 * The name the game shows: a parody of the real circuit's (`Zandervoort`),
	 * because the real names are the operators' trademarks. The YAML's `name`;
	 * refreshed on every ApexTrackCatalogSync run.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Track")
	FString DisplayName;

	/**
	 * What the circuit is modelled on, by place rather than by trademark
	 * ("Modelled on the circuit in the dunes at Zandvoort, Netherlands.").
	 * The YAML's `metadata.description`; refreshed on every sync run.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Track")
	FString Description;

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

	/**
	 * Checksum of the track YAML the catalog entry (and the baked level) came
	 * from, as the server computes it (`source_crc` in track_catalog.json).
	 * Refreshed on every ApexTrackCatalogSync run; compared with the server's
	 * `ContentCrc` when the level is streamed. 0 on a row synced before the
	 * field existed.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Track")
	int64 SourceCrc = 0;

	/** Empty for tracks with no preview art (Le Mans); falls back to a placeholder. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Track")
	TSoftObjectPtr<UTexture2D> PreviewImage;

	/**
	 * The preview of a track found on disk at runtime (`<Stem>.png` beside
	 * its export), loaded by `UApexTrackContentSubsystem`; never saved in the
	 * table. Read either through `UApexTrackContentSubsystem::PreviewOf`.
	 */
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Track")
	TObjectPtr<UTexture2D> RuntimePreview;
};

namespace ApexCatalog
{
	/**
	 * What a car class (`FApexCarCatalogRow::CarClass`) or a track category
	 * (`FApexTrackCatalogRow::Category`) is shown as. The data keeps the real
	 * series' names, which the logic keys on (`F1` picks the cockpit style and
	 * the engine sound) but which are trademarks, so every screen goes through
	 * this: F1 is Formula, WEC Endurance, DTM GT3 and IndyCar Independent.
	 * Anything else is shown as it is.
	 */
	inline FString DisplayClass(const FString& Raw)
	{
		const FString Key = Raw.TrimStartAndEnd();
		auto Is = [&Key](const TCHAR* Name) { return Key.Equals(Name, ESearchCase::IgnoreCase); };
		if (Is(TEXT("F1")))                            { return TEXT("Formula"); }
		if (Is(TEXT("WEC")) || Is(TEXT("Endurance")))  { return TEXT("Endurance"); }
		if (Is(TEXT("DTM")) || Is(TEXT("GT3")))        { return TEXT("GT3"); }
		if (Is(TEXT("IndyCar")) || Is(TEXT("Indy")))   { return TEXT("Independent"); }
		return Key;
	}
}
