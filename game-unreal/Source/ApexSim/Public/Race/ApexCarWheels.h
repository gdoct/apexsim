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
 * after import (docs/CAR_MODELS.md). The shared wheel mesh has its axle on X
 * and its face on +X, centred on the hub.
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
}

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

	/** Loads the spec's mesh and places the wheels; an unusable spec hides them. */
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

private:
	void Place();

	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMeshComponent>> Components;

	FApexWheelSpec Spec;
	FBoxSphereBounds MeshBounds{ForceInit};
	bool bHasWheels = false;
	bool bVisible = true;
	float SteerRad = 0.0f;
	/** Roll of the front and rear axles, radians, kept within one turn. */
	float SpinRad[2] = {0.0f, 0.0f};
};
