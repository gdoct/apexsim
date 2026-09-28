#pragma once

#include "CoreMinimal.h"
#include "Catalog/ApexCatalogRows.h"
#include "Race/ApexCockpitLayout.h"

/** A car.toml's `[wheels]` table: where the client draws the wheels, metres. */
struct FApexCarWheelsToml
{
	/**
	 * The class wheel `content/wheels/<Model>.glb`, or a GLB in the car's own
	 * folder when it ends in `.glb` or holds a `/` (IsCarLocalWheel); empty
	 * when the table is absent.
	 */
	FString Model;
	/** `rear_model`: the rear pair's own wheel, by the same rule; empty draws `Model` on all four. */
	FString RearModel;
	float FrontAxleM = 0.0f;
	float RearAxleM = 0.0f;
	float FrontTrackM = 0.0f;
	float RearTrackM = 0.0f;
	float FrontRadiusM = 0.0f;
	float RearRadiusM = 0.0f;
	float FrontWidthM = 0.0f;
	float RearWidthM = 0.0f;

	bool IsPresent() const { return !Model.IsEmpty(); }
};

/** The `[drs_flap]` table: the flap GLB beside the car.toml, its hinge and its travel. */
struct FApexCarDrsFlapToml
{
	/** Relative to the car folder; empty when the table is absent. */
	FString Model;
	float HingeForwardM = 0.0f;
	float HingeUpM = 0.0f;
	float OpenDeg = 0.0f;

	bool IsPresent() const { return !Model.IsEmpty(); }
};

/**
 * A `[[livery]]` table: colours are linear RGB, files relative to the car
 * folder. A colour livery names `paint` (and maybe `accent`, `metallic`,
 * `logo`); a texture livery, an imported car's skin, names `skin` and maybe
 * `textures` and `preview`, and may have no colour at all:
 *
 * ```toml
 * [[livery]]
 * name = "Gulf"
 * skin = "skins/gulf/Skin_00.png"                  # every car_skin* slot
 * textures = ["EXT_RIM=skins/gulf/EXT_RIM.png"]    # SLOT=file, one line
 * preview = "skins/gulf/preview.jpg"
 * ```
 */
struct FApexCarLiveryToml
{
	FString Name;
	/** Zero alpha when the table names no paint: the model's is kept. */
	FLinearColor Paint = FLinearColor(0.0f, 0.0f, 0.0f, 0.0f);
	/** Zero alpha when the table names no accent: the model's is kept. */
	FLinearColor Accent = FLinearColor(0.0f, 0.0f, 0.0f, 0.0f);
	float Metallic = -1.0f;
	FString Logo;
	FString Skin;
	/** `textures`: (slot, file) in the table's order. */
	TArray<TPair<FString, FString>> Textures;
	FString Preview;
	/** A `textures` value that is not a one-line array of `"SLOT=file"` strings; Parse fails on it. */
	bool bBadTextures = false;

	bool HasPaint() const { return Paint.A > 0.0f; }
};

/**
 * The optional `[preview]` table: how the turntable frames the car
 * (`FApexCarCatalogRow::PreviewOffset` and friends). Absent, a runtime car
 * takes the values from its `DT_CarCatalog` row, if it has one.
 *
 * ```toml
 * [preview]
 * offset_cm = [0.0, 0.0, 12.0]
 * rotation_deg = [0.0, 90.0, 0.0]   # pitch, yaw, roll
 * scale = 1.0
 * ```
 */
struct FApexCarPreviewToml
{
	bool bPresent = false;
	FVector OffsetCm = FVector::ZeroVector;
	FRotator Rotation = FRotator::ZeroRotator;
	float Scale = 1.0f;
};

/**
 * What the game and ApexCarImport read from a car.toml: the identity, a few
 * physics figures, the wheels, the DRS flap, the engine sound, the liveries
 * and the optional turntable and cockpit tweaks. The server reads the same
 * file for the physics and ignores everything client-side.
 */
struct FApexCarToml
{
	FString Id;
	FString Name;
	FString Model;
	FString Brand;
	FString CarClass;
	FString ManufacturerCountry;
	int32 ModelYear = 0;
	float MassKg = 0.0f;
	float MaxPowerKw = 0.0f;
	float MaxSteerRad = 0.0f;
	FApexCarWheelsToml Wheels;
	FApexCarDrsFlapToml DrsFlap;
	/**
	 * The `[sound]` table and the `[engine]` rev range, already in the
	 * row's shape. `Cylinders == 0` when the car has no `[sound]` table.
	 */
	FApexEngineSoundSpec Sound;
	/** The `[[livery]]` tables, in order. */
	TArray<FApexCarLiveryToml> Liveries;
	FApexCarPreviewToml Preview;
	/**
	 * The optional `[cockpit]` table (`style = "auto" | "open" | "closed"`,
	 * `eye_cm`, `wheel_cm`, `mirror_centre_cm`, `mirror_left_cm`,
	 * `mirror_right_cm`, each `[x, y, z]` in the car's frame;
	 * `wheel_rake_deg`, `wheel_lock_deg`; `rig_wheel` and `rig_dash`, true or
	 * false). Absent, a runtime car takes its `DT_CarCatalog` row's.
	 */
	bool bHasCockpit = false;
	FApexCockpitOverrides Cockpit;
	/**
	 * `[cockpit] steering_wheel_model`: the car's own steering wheel GLB,
	 * relative to the car folder (FApexCockpitOverrides::RuntimeSteeringWheel
	 * has the frame); empty for none.
	 */
	FString SteeringWheelModel;
};

namespace ApexCarToml
{
	/**
	 * A minimal TOML scan — `key = value` lines under `[table]` headers,
	 * strings, integers, floats and three-number arrays, comments stripped —
	 * which is all a car.toml's client side needs. Pure, for the tests.
	 */
	APEXSIM_API bool Parse(const FString& Text, FApexCarToml& Out, FString& OutError);

	/** The row's wheel figures from the TOML, with no mesh yet; an empty spec when the TOML has none. */
	APEXSIM_API FApexWheelSpec MakeWheelSpec(const FApexCarToml& Toml);

	/** The row's DRS flap figures from the TOML, with no mesh yet; an empty spec when the TOML has none. */
	APEXSIM_API FApexDrsFlapSpec MakeDrsFlapSpec(const FApexCarToml& Toml);

	/** `yotota-lmp2` -> `yotota_lmp2`: a folder name as a package or object name segment. */
	APEXSIM_API FString Segment(const FString& Folder);

	/**
	 * Whether a `[wheels] model` / `rear_model` names a GLB in the car's own
	 * folder (`wheels/front.glb`, `rim.glb`) rather than a class wheel in the
	 * shared wheels folder (`f1`): it ends in `.glb` or holds a path separator.
	 */
	APEXSIM_API bool IsCarLocalWheel(const FString& Model);
}
