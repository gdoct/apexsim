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
 * `[wheels]` table of car.toml (docs/content/cars.md), filled by
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

	/**
	 * The wheel GLB a car found on disk draws — the class wheel
	 * (`<cars>/../wheels/<model>.glb`) or one in the car's own folder (a
	 * `model` ending in `.glb` or holding a `/`; ApexCarToml::IsCarLocalWheel)
	 * — built by `UApexCarContentSubsystem`; wins over `Mesh`. Never saved.
	 */
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Wheels")
	FString RuntimeModel;

	/**
	 * A different wheel for the rear pair (`[wheels] rear_model`: an F1 car's
	 * rears are wider and taller, and a model scaled to fit would stretch the
	 * front's rim). Unset, the rears draw `Mesh` like the fronts.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Wheels")
	TSoftObjectPtr<UStaticMesh> RearMesh;

	/** The rear wheel GLB of a car found on disk; wins over `RearMesh`. Empty: the rears draw the front's. Never saved. */
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Wheels")
	FString RearRuntimeModel;

	/** Whether the rear pair has a model of its own. */
	bool HasRearModel() const { return !RearMesh.IsNull() || !RearRuntimeModel.IsEmpty(); }

	/** A mesh and a size for both axles: enough to draw. */
	bool IsUsable() const
	{
		return (!Mesh.IsNull() || !RuntimeModel.IsEmpty()) && FrontRadiusM > 0.0f && RearRadiusM > 0.0f && FrontWidthM > 0.0f && RearWidthM > 0.0f;
	}

	bool operator==(const FApexWheelSpec& Other) const
	{
		return Mesh == Other.Mesh && FrontAxleM == Other.FrontAxleM && RearAxleM == Other.RearAxleM
			&& FrontTrackM == Other.FrontTrackM && RearTrackM == Other.RearTrackM
			&& FrontRadiusM == Other.FrontRadiusM && RearRadiusM == Other.RearRadiusM
			&& FrontWidthM == Other.FrontWidthM && RearWidthM == Other.RearWidthM
			&& MaxSteerRad == Other.MaxSteerRad && RuntimeModel == Other.RuntimeModel
			&& RearMesh == Other.RearMesh && RearRuntimeModel == Other.RearRuntimeModel;
	}
	bool operator!=(const FApexWheelSpec& Other) const { return !(*this == Other); }
};

/**
 * An F1 car's DRS flap: the rear wing's upper element, cut out of the body
 * mesh into its own so the client can open it. The `[drs_flap]` table of
 * car.toml (docs/content/cars.md), filled by ApexCarImport.
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

	/** The flap GLB beside a car found on disk, built at runtime; wins over `Mesh`. Never saved. */
	UPROPERTY(Transient, BlueprintReadOnly, Category = "DRS")
	FString RuntimeModel;

	bool IsUsable() const { return !Mesh.IsNull() || !RuntimeModel.IsEmpty(); }

	bool operator==(const FApexDrsFlapSpec& Other) const
	{
		return Mesh == Other.Mesh && HingeForwardM == Other.HingeForwardM && HingeUpM == Other.HingeUpM
			&& OpenDeg == Other.OpenDeg && RuntimeModel == Other.RuntimeModel;
	}
	bool operator!=(const FApexDrsFlapSpec& Other) const { return !(*this == Other); }
};

/**
 * The driver figure in a car's seat (car.toml `[driver]`, docs/content/cars.md):
 * a mesh in the body mesh's own frame, drawn on the body with no transform
 * and hidden for the car the cockpit camera sits in.
 */
USTRUCT(BlueprintType)
struct APEXSIM_API FApexDriverSpec
{
	GENERATED_BODY()

	/** A cooked mesh, when a car has one; the runtime GLB wins. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Driver")
	TSoftObjectPtr<UStaticMesh> Mesh;

	/** The driver GLB beside a car found on disk, built at runtime; wins over `Mesh`. Never saved. */
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Driver")
	FString RuntimeModel;

	bool IsUsable() const { return !Mesh.IsNull() || !RuntimeModel.IsEmpty(); }

	bool operator==(const FApexDriverSpec& Other) const
	{
		return Mesh == Other.Mesh && RuntimeModel == Other.RuntimeModel;
	}
	bool operator!=(const FApexDriverSpec& Other) const { return !(*this == Other); }
};

/** Which of the server's body damage zones a part belongs to (`CompactCarState.damage`, damage.rs). */
UENUM(BlueprintType)
enum class EApexDamageZone : uint8
{
	Front,
	Rear,
	Left,
	Right,
};

/**
 * A piece of bodywork that comes off in a big enough hit: a car.toml
 * `[[damage_part]]` table (docs/content/cars.md, Damage parts). The body GLB
 * is split at runtime: every triangle whose centre lies inside the box is
 * built into the part's own mesh (the first box to hold it wins), drawn on
 * the body until the zone's damage reaches `DetachPct` and then thrown off
 * as debris (`AApexCarDebrisActor`). A repair puts it back.
 *
 * The box is in the body mesh's frame, centimetres (nose +Y, left +X,
 * floor at Z = 0), like the wheels; the TOML gives it in the car's frame,
 * metres (`min_m` / `max_m` = forward, left, up).
 */
USTRUCT(BlueprintType)
struct APEXSIM_API FApexDamagePartSpec
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Damage")
	FString Name;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Damage")
	EApexDamageZone Zone = EApexDamageZone::Front;

	/** The zone's damage, percent, at which it comes off. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Damage")
	float DetachPct = 50.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Damage")
	FVector MinCm = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Damage")
	FVector MaxCm = FVector::ZeroVector;

	FBox Box() const { return FBox(MinCm, MaxCm); }

	bool operator==(const FApexDamagePartSpec& Other) const
	{
		return Name == Other.Name && Zone == Other.Zone && DetachPct == Other.DetachPct && MinCm == Other.MinCm
			&& MaxCm == Other.MaxCm;
	}
	bool operator!=(const FApexDamagePartSpec& Other) const { return !(*this == Other); }
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
 * One slot's base colour texture swapped by a texture livery: a
 * `textures = ["SLOT=file.png", ...]` entry of a `[[livery]]` table, resolved
 * to a file on disk (ApexCarLivery.h).
 */
USTRUCT(BlueprintType)
struct APEXSIM_API FApexLiveryTexture
{
	GENERATED_BODY()

	/** The material slot, by its glTF material name (`EXT_RIM`, `wheel_rim`). */
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Livery")
	FName Slot;

	/** The PNG or JPEG, a full path. */
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Livery")
	FString RuntimeTexture;

	bool operator==(const FApexLiveryTexture& Other) const
	{
		return Slot == Other.Slot && RuntimeTexture == Other.RuntimeTexture;
	}
	bool operator!=(const FApexLiveryTexture& Other) const { return !(*this == Other); }
};

/**
 * One of a car's extra paint schemes: a `[[livery]]` table in its car.toml.
 * The mesh stays the same; the client repaints the `car_paint` and
 * `car_accent` slots and swaps the `car_logo` texture, and a texture livery
 * (an imported car's skin) swaps the base colour texture of every
 * `car_skin*` slot and of any slot it names (ApexCarLivery.h).
 */
USTRUCT(BlueprintType)
struct APEXSIM_API FApexCarLivery
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Livery")
	FString Name;

	/**
	 * Body colour, linear (the glTF `BaseColorFactor` of `car_paint`). Zero
	 * alpha keeps the model's: a texture livery from car.toml need not name a
	 * paint colour.
	 */
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

	/** The logo PNG of a car found on disk, loaded at runtime; wins over `Logo`. Never saved. */
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Livery")
	FString RuntimeLogo;

	/**
	 * The skin (`skin`, a PNG or JPEG in the car folder): the base colour
	 * texture of every `car_skin*` slot. Empty keeps the model's. Never saved.
	 */
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Livery")
	FString RuntimeSkin;

	/** Other slots' base colour textures (`textures`), in the table's order. Never saved. */
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Livery")
	TArray<FApexLiveryTexture> RuntimeTextures;

	/** A picture of the car in this livery (`preview`), for a menu that wants one; nothing draws it yet. Never saved. */
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Livery")
	FString RuntimePreview;

	bool HasPaint() const { return Paint.A > 0.0f; }

	bool operator==(const FApexCarLivery& Other) const
	{
		return Name == Other.Name && Paint == Other.Paint && Accent == Other.Accent
			&& PaintMetallic == Other.PaintMetallic && Logo == Other.Logo && RuntimeLogo == Other.RuntimeLogo
			&& RuntimeSkin == Other.RuntimeSkin && RuntimeTextures == Other.RuntimeTextures
			&& RuntimePreview == Other.RuntimePreview;
	}
	bool operator!=(const FApexCarLivery& Other) const { return !(*this == Other); }
};

/** What a tyre compound is cut like: a slick, or one of the two treaded rain tyres. */
UENUM(BlueprintType)
enum class EApexCompoundKind : uint8
{
	Slick,
	Intermediate,
	Wet,
};

/**
 * One of a car's compounds (car.toml `[[tires.compound]]`, the server's
 * `tyre_thermal::Compound`): its name, in the order telemetry's `compound`
 * byte indexes them, and its kind, which is what the client draws (a
 * treaded tyre looks different) — the grip and wear figures are the
 * server's alone. A car without the tables has the five defaults.
 */
USTRUCT(BlueprintType)
struct APEXSIM_API FApexCompoundSpec
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Tyres")
	FString Name;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Tyres")
	EApexCompoundKind Kind = EApexCompoundKind::Slick;

	bool IsTreaded() const { return Kind != EApexCompoundKind::Slick; }

	bool operator==(const FApexCompoundSpec& Other) const { return Name == Other.Name && Kind == Other.Kind; }
	bool operator!=(const FApexCompoundSpec& Other) const { return !(*this == Other); }
};

/** One row per car. RowName == the `id` from `content/cars/{default,custom}/<folder>/car.toml`. */
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
	 * Checksum of the car.toml the row was read from, as the server computes
	 * it (ApexContentCrc.h / content_crc.rs): by `UApexCarContentSubsystem`
	 * for a car on disk, by every ApexCarImport run for a table row. Compared
	 * with the server's `ContentCrc` when the car is raced. 0 on a table row
	 * imported before the field existed.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Car")
	int64 SourceCrc = 0;

	/**
	 * The cooked body, for a row from `DT_CarCatalog` (ApexCarImport). A car
	 * found on disk has `RuntimeModel` instead. Read either through
	 * `ApexCarContent::LoadBody`.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Car")
	TSoftObjectPtr<UStaticMesh> Mesh;

	/**
	 * The body GLB of a car found on disk at runtime (`<cars>/<folder>/<model>`),
	 * built on first use by `UApexCarContentSubsystem`; wins over `Mesh`.
	 * Never saved in the table.
	 */
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Car")
	FString RuntimeModel;

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

	/**
	 * The driver in the seat (the generated cars; an unusable spec draws
	 * none). Derived from car.toml, like the flap.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Car")
	FApexDriverSpec Driver;

	/**
	 * Bodywork that comes off in a crash (car.toml `[[damage_part]]`; none on
	 * a car without the tables, whose body is drawn whole).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Car")
	TArray<FApexDamagePartSpec> DamageParts;

	/** What the engine sounds like. Derived from car.toml on every import, like the wheels. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Car")
	FApexEngineSoundSpec EngineSound;

	/**
	 * The tyres' working window, °C (car.toml `[tires]`
	 * `optimal_temperature_c` / `temperature_window_c`): the HUD colours the
	 * tread temperatures against it. The server's grip reads the same keys.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Car")
	float TyreOptimalC = 90.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Car")
	float TyreWindowC = 10.0f;

	/**
	 * The car's compounds in telemetry order (car.toml `[[tires.compound]]`,
	 * else the five defaults): which index is a treaded tyre, for the look
	 * of the wheels. Derived on every import, like the window.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Car")
	TArray<FApexCompoundSpec> Compounds;

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

	/**
	 * Where the circuit is, from its export's `metadata` (`latitude_deg`,
	 * `north_yaw_deg`): degrees north, and true north's yaw in the track
	 * frame (degrees counter-clockwise from +X). Only meaningful when
	 * `bHasLocation`; a table row and an export from before the keys have
	 * none, and the sky then keeps its 50° N with north down +X.
	 */
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Track")
	bool bHasLocation = false;

	UPROPERTY(Transient, BlueprintReadOnly, Category = "Track")
	float LatitudeDeg = 50.0f;

	UPROPERTY(Transient, BlueprintReadOnly, Category = "Track")
	float NorthYawDeg = 0.0f;
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
