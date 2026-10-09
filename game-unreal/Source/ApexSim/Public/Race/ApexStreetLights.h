#pragma once

#include "CoreMinimal.h"

/**
 * Real light sources for the track's lamp props after dark, and the budget
 * that decides which of them are on. Pure data and arithmetic (no assets), so
 * `ApexSim.Lights.*` tests run headless.
 *
 * Every lamp instance of the level gets its light components once; only the
 * nearest `Max` within `Range` of the camera are visible (`Select`, re-run
 * about twice a second by the race director), with hysteresis so a light at
 * the edge of the budget does not flicker.
 */
namespace ApexLights
{
	/** One light on a prop, in the instance's local frame (cm, UE axes). */
	struct FLightSpec
	{
		FVector LocalCm = FVector::ZeroVector;
		/** Rotation relative to the instance; a spot shines along its +X. */
		FRotator AimLocal = FRotator::ZeroRotator;
		bool bSpot = true;
		float Lumens = 0.0f;
		float RadiusCm = 0.0f;
		float InnerConeDeg = 0.0f;
		float OuterConeDeg = 0.0f;
		FLinearColor Color = FLinearColor::White;
	};

	/**
	 * The constants, in one place. Lumens are in the units the older
	 * `lamp_post` spot already uses (12 000 lm, 7.5 m up, 60 deg outer cone,
	 * about 250 lux under it at the exposure the night sky sets); the arm lamp
	 * is that x1.5 at its 9.2 m height and wider cone.
	 */
	inline constexpr float ArmLumens = 80000.0f;
	inline constexpr float ArmRadiusCm = 2800.0f;
	inline constexpr float ArmInnerDeg = 60.0f;
	inline constexpr float ArmOuterDeg = 110.0f;
	inline constexpr float ArmColorTemperatureK = 3300.0f;
	/** The arm heads: +-3.4 m across the road (local Y), 9.2 m up. */
	inline constexpr float ArmHeadYCm = 340.0f;
	inline constexpr float ArmHeadZCm = 920.0f;
	/** Pitch of an arm light (-90 is straight down); it leans this far toward the road. */
	inline constexpr float ArmPitchDeg = -85.0f;
	inline constexpr float GlobeLumens = 30000.0f;
	inline constexpr float GlobeRadiusCm = 1200.0f;
	inline constexpr float GlobeHeadZCm = 610.0f;
	inline constexpr float BalloonLumens = 60000.0f;
	inline constexpr float BalloonRadiusCm = 1800.0f;
	inline constexpr float BalloonHeadZCm = 820.0f;
	inline constexpr float TrussLumens = 20000.0f;
	inline constexpr float TrussRadiusCm = 1600.0f;
	inline constexpr float TrussInnerDeg = 40.0f;
	inline constexpr float TrussOuterDeg = 75.0f;

	/** Defaults of `apexsim.lights.Max` and `apexsim.lights.Range` (metres). */
	inline constexpr int32 DefaultMaxLights = 90;
	inline constexpr float DefaultRangeM = 260.0f;
	/**
	 * A light that is on is treated as this much nearer when ranking, and
	 * stays on out to this much farther than `Range`.
	 */
	inline constexpr float StickyShare = 0.2f;
	/** Seconds between two selections. */
	inline constexpr float SelectIntervalS = 0.5f;
	/** Most light components a level may spawn, whatever its lamp count. */
	inline constexpr int32 MaxSpawned = 1600;

	/**
	 * The lights of the prop whose static mesh is called `MeshName`
	 * (`SM_lamp_arm_twin`...): empty for any other prop. Covers the floodlight
	 * mast and lamp post the director always lit, and the Marina Bay
	 * `lamp_arm_twin`, `lamp_globe_pole`, `lamp_balloon_tether(_orange)` and
	 * `pit_light_truss_6m`.
	 */
	APEXSIM_API TArray<FLightSpec> SpecsFor(const FString& MeshName);

	/**
	 * Choose which of `Distances` (cm, one per light) are on: the `MaxOn`
	 * nearest within `RangeCm`, with a light that is already on ranked and
	 * ranged `StickyShare` more generously. `InOutOn` has one flag per light
	 * (resized and cleared if the sizes differ). Ties go to the lower index.
	 * Returns the number on.
	 */
	APEXSIM_API int32 Select(const TArray<float>& Distances, TArray<uint8>& InOutOn, int32 MaxOn, float RangeCm);
}	 // namespace ApexLights
