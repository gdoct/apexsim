#pragma once

#include "Catalog/ApexCatalogRows.h"
#include "CoreMinimal.h"

#include "ApexCarDrsFlap.generated.h"

class UStaticMeshComponent;

/**
 * An F1 car's DRS flap, drawn apart from the body so it can open.
 *
 * The frame is the BODY MESH's, like the wheels' (ApexCarWheels.h):
 * centimetres, nose on +Y, left on +X, floor at Z = 0. The flap mesh's
 * origin is its hinge; the hinge axis is X.
 */
namespace ApexDrs
{
	/** How long the flap takes to swing fully open or shut, seconds. */
	inline constexpr float SwingSeconds = 0.18f;

	/**
	 * The flap's transform relative to the body mesh, `Open` of the way open
	 * (0 shut, 1 fully open, clamped): at the hinge, turned about X so the
	 * leading edge (+Y of the hinge) rises.
	 */
	APEXSIM_API FTransform FlapTransform(const FApexDrsFlapSpec& Spec, float Open);

	/** `Open` moved towards 1 (bOpen) or 0 over `DeltaSeconds`, at the swing rate. */
	APEXSIM_API float StepOpen(float Open, bool bOpen, float DeltaSeconds);
}

/**
 * The flap component on a car body and how far open it is. Held by the race
 * car actor (opened from the telemetry's `bDrsOpen`) and by the menu's
 * turntable (always shut); the component is made in the owner's constructor
 * and attached to its body mesh component.
 */
USTRUCT()
struct APEXSIM_API FApexCarDrsFlap
{
	GENERATED_BODY()

	/** Constructor-only: creates the component as a default subobject of `Owner`. */
	void CreateComponent(UObject& Owner, USceneComponent* Body);

	/** Loads the spec's mesh and seats the flap shut; an unusable spec hides it. */
	void SetSpec(const FApexDrsFlapSpec& InSpec);

	const FApexDrsFlapSpec& GetSpec() const { return Spec; }
	bool HasFlap() const { return bHasFlap; }

	/** Swings the flap towards open or shut; call every frame. */
	void Update(bool bWantOpen, float DeltaSeconds);

	void SetVisible(bool bVisible);

	/** The flap's component, or null before CreateComponent. */
	UStaticMeshComponent* GetComponent() const { return Component; }

private:
	void Place();

	UPROPERTY(Transient)
	TObjectPtr<UStaticMeshComponent> Component;

	FApexDrsFlapSpec Spec;
	float Open = 0.0f;
	bool bHasFlap = false;
	bool bVisible = true;
};
