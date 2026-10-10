#pragma once

#include "Catalog/ApexCatalogRows.h"
#include "CoreMinimal.h"

#include "ApexCarWheels.generated.h"

class UStaticMeshComponent;

/**
 * The four wheels drawn on a car body.
 *
 * Every frame below is the BODY MESH's (the component the wheels hang off):
 * centimetres, nose on +Y, left on +X, floor at Z = 0 — the car GLBs' frame
 * after import (docs/content/cars.md). A wheel mesh (the class wheel, or a
 * car's own, and the rear pair's own when the spec has one) has its axle on
 * X and its face on +X, centred on the hub; it is scaled to the axle's
 * radius and width, so its size only needs to be close.
 */
namespace ApexWheels
{
	enum class EWheel : uint8
	{
		FrontLeft,
		FrontRight,
		RearLeft,
		RearRight,
	};

	inline constexpr int32 NumWheels = 4;

	inline bool IsFront(EWheel Wheel) { return Wheel == EWheel::FrontLeft || Wheel == EWheel::FrontRight; }
	inline bool IsLeft(EWheel Wheel) { return Wheel == EWheel::FrontLeft || Wheel == EWheel::RearLeft; }

	/** Radius of the wheel's axle, metres. */
	inline float RadiusM(const FApexWheelSpec& Spec, EWheel Wheel)
	{
		return IsFront(Wheel) ? Spec.FrontRadiusM : Spec.RearRadiusM;
	}

	/**
	 * Front wheel angle in radians for a steering input as the server holds it
	 * (-1..1, positive LEFT), positive to the left.
	 */
	APEXSIM_API float SteerAngleRad(const FApexWheelSpec& Spec, float SteeringInput);

	/**
	 * How far a wheel of this radius turns while the car covers `DistanceM`,
	 * radians; positive rolls forward.
	 */
	APEXSIM_API float RollAngleRad(float DistanceM, float RadiusM);

	/**
	 * How far a car rolled between two poses, metres: its movement along its
	 * own nose, so negative when it backs up, and nothing for sliding
	 * sideways, bobbing on its springs or standing still. A jump longer than
	 * `TeleportCm` (a respawn) rolls nothing.
	 */
	APEXSIM_API float RolledDistanceM(const FVector& FromCm, const FVector& ToCm, const FQuat& Rotation, float TeleportCm);

	/**
	 * A frame's roll as drawn: `StepRad` held to `MaxStepRad` either way (no
	 * limit when that is zero or less). A wheel at road speed turns tens of
	 * times a second; drawn at that rate it is a motion-blurred smear, and a
	 * step near a spoke's pitch strobes. Under the limit it rolls true.
	 */
	APEXSIM_API float DrawnSpinStepRad(float StepRad, float MaxStepRad);

	/**
	 * A wheel's transform relative to the body mesh: at its hub, sized from
	 * the wheel mesh's local bounds to the axle's width and diameter, its face
	 * outboard, steered by `SteerRad` (front only, positive left) and turned
	 * `SpinRad` about the axle (positive rolls forward).
	 */
	APEXSIM_API FTransform WheelTransform(
		const FApexWheelSpec& Spec, EWheel Wheel, const FBoxSphereBounds& WheelMeshBounds, float SteerRad, float SpinRad);

	/** The space the four unsteered wheels fill, in the body mesh's frame; empty for an unusable spec. */
	APEXSIM_API FBox WheelsBox(const FApexWheelSpec& Spec);

	/** The `wheel_tyre` material slot of the class wheels (docs/content/cars.md, Wheels). */
	inline const FName TyreSlot(TEXT("wheel_tyre"));

	/**
	 * How a treaded compound's tyre is drawn against the slick's authored
	 * rubber: a cooler, matte grey, slightly blue for an intermediate and a
	 * darker blue-grey for a wet, both nearly fully rough. The class wheels'
	 * tyres carry no UVs (`build_wheels.py` lathes them without any), so a
	 * groove texture cannot be laid round them. The fallback for a tyre slot
	 * not on `M_ApexCarTyre` (a project baked before it), which draws the
	 * tread itself. False for a slick, which keeps the model's own material.
	 */
	APEXSIM_API bool TreadedTint(EApexCompoundKind Kind, FLinearColor& OutBaseColour, float& OutRoughness);

	/** The class wheels' compound ring on the sidewall (docs/content/cars.md, Wheels). */
	inline const FName BandSlot(TEXT("wheel_band"));

	/**
	 * The sidewall ring of a rain tyre, linear: green for an intermediate,
	 * blue for a wet, as the rain tyres are marked. False for a slick, whose
	 * ring stays as the wheel model has it.
	 */
	APEXSIM_API bool CompoundBandColour(EApexCompoundKind Kind, FLinearColor& OutColour);

	/** What the tyre parent draws for a compound kind: 0 slick, 1 intermediate, 2 wet. */
	APEXSIM_API float TreadIndex(EApexCompoundKind Kind);

	/** How far a flat tyre squats at most, a share of its radius. */
	inline constexpr float MaxSagShare = 0.09f;

	/**
	 * How deflated a tyre at this running pressure looks, 0 (round) to 1
	 * (flat: the server's PUNCTURED_KPA, 15 kPa); nothing above 100 kPa,
	 * so a slow leak shows only once most of the air is gone. Unknown
	 * (negative) is round.
	 */
	APEXSIM_API float DeflatedShare(float PressureKpa);

	/**
	 * How wet the road leaves a tyre, 0..1, for the water under it (percent
	 * of heavy rain on the flat, as the road state's and the sky's
	 * `RoadWaterPct`): a damp road is a sheen, 40% and over soaks it.
	 */
	APEXSIM_API float WetnessForWater(float WaterPct);

	/**
	 * One step of a tyre's wetness toward `Target`: soaked in about a
	 * second and a half, dried off over about twenty (the tread throws the
	 * water off, the heat dries the film).
	 */
	APEXSIM_API float WetnessStep(float Current, float Target, float DeltaSeconds);

	/**
	 * Where a wheel touches the road, as the tyre parent's `theta` (radians
	 * round the wheel mesh's own X from +Y toward +Z), for the wheel's
	 * world transform: where a wheel locked now grinds its flat spot.
	 */
	APEXSIM_API float ContactTheta(const FTransform& WheelWorld);
}

/** One tyre's state as drawn (FApexCarWheelSet::SetTyreSurface). */
struct FApexTyreSurface
{
	/** Tread worn, 0..1 (telemetry's `tyre_wear` / 100). */
	float Wear = 0.0f;
	/** Water on the tyre, 0..1 (ApexWheels::WetnessStep). */
	float Wetness = 0.0f;
	/** Flat-spot depth, 0..1 of the worst (`flat_spot`). */
	float FlatSpot = 0.0f;
	/** Running pressure, kPa; negative when unknown, drawn round. */
	float PressureKpa = -1.0f;
};

/**
 * Four wheel components on a car body and the state that turns them. Held by
 * the race car actor and the menu's turntable; the components are made in
 * the owner's constructor and attached to its body mesh component.
 */
USTRUCT()
struct APEXSIM_API FApexCarWheelSet
{
	GENERATED_BODY()

	/** Constructor-only: creates the components as default subobjects of `Owner`. */
	void CreateComponents(UObject& Owner, USceneComponent* Body);

	/** Loads the spec's meshes (the rear pair's own, when it has one) and places the wheels; an unusable spec hides them. */
	void SetSpec(const FApexWheelSpec& InSpec);

	const FApexWheelSpec& GetSpec() const { return Spec; }
	bool HasWheels() const { return bHasWheels; }

	/**
	 * Rolls the wheels over `DistanceM` (negative backwards; see
	 * RolledDistanceM), at most `MaxStepRad` per call (see DrawnSpinStepRad),
	 * and steers the front pair.
	 */
	void Update(float SteeringInput, float DistanceM, float MaxStepRad = 0.0f);

	void SetVisible(bool bVisible);

	/** Applied to each wheel component, e.g. the turntable's capture-only flag. */
	void ForEachComponent(TFunctionRef<void(UStaticMeshComponent&)> Fn) const;

	/**
	 * Draws the tyres as the compound's kind looks: through the tyre parent,
	 * the kind's tread pattern (ApexWheels::TreadIndex). A tyre slot not on
	 * that parent (a project baked before it) falls back to the tint
	 * (ApexWheels::TreadedTint): a treaded kind gets each wheel's own
	 * instance of the tyre slot with the tint, a slick the shared material
	 * back. Idempotent.
	 */
	void SetTyreLook(EApexCompoundKind Kind);
	EApexCompoundKind GetTyreLook() const { return TyreLook; }

	/**
	 * Draws one tyre's state through the tyre parent (`M_ApexCarTyre`,
	 * ApexCarMaterials::TyreCpd): wear, water, a flat spot (laid where the
	 * wheel touches the road when the spot first appears; one per tyre)
	 * and a deflated tyre, whose hub drops by the squat so the flattened
	 * tread stays on the road. Only changed values are written.
	 */
	void SetTyreSurface(ApexWheels::EWheel Wheel, const FApexTyreSurface& Surface);
	const FApexTyreSurface& GetTyreSurface(ApexWheels::EWheel Wheel) const { return Surfaces[static_cast<int32>(Wheel)]; }

	/** How far a wheel's hub is dropped for a deflated tyre, cm in the body's frame. */
	float GetSagCm(ApexWheels::EWheel Wheel) const { return SagCm[static_cast<int32>(Wheel)]; }

	/**
	 * Where a wheel meets the road, in world space: its hub less its radius
	 * along world up. False with no wheels.
	 */
	bool ContactPatch(ApexWheels::EWheel Wheel, FVector& OutWorld) const;

private:
	void Place();
	/** Whether a wheel's tyre slot is drawn by the tyre parent (baked, and the mesh has the slot). */
	bool UsesTyreParent(int32 Index) const;
	/** Writes the tyre parent's custom data for one wheel: its shape, compound and state. */
	void WriteTyreData(int32 Index);

	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMeshComponent>> Components;

	FApexWheelSpec Spec;
	FBoxSphereBounds MeshBounds{ForceInit};
	/** The rear pair's model's bounds: the front's unless the spec has a rear model. */
	FBoxSphereBounds RearMeshBounds{ForceInit};
	bool bHasWheels = false;
	bool bVisible = true;
	/** What the tyres are drawn as; a new mesh is a slick until told otherwise. */
	EApexCompoundKind TyreLook = EApexCompoundKind::Slick;
	float SteerRad = 0.0f;
	/** Roll of the front and rear axles, radians, kept within one turn. */
	float SpinRad[2] = {0.0f, 0.0f};
	FApexTyreSurface Surfaces[ApexWheels::NumWheels];
	/** Each flat spot's `theta` (ApexWheels::ContactTheta). */
	float FlatSpotTheta[ApexWheels::NumWheels] = {0.0f, 0.0f, 0.0f, 0.0f};
	float SagCm[ApexWheels::NumWheels] = {0.0f, 0.0f, 0.0f, 0.0f};
};
