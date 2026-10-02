#pragma once

#include "Catalog/ApexCatalogRows.h"
#include "CoreMinimal.h"

#include "ApexCarDriver.generated.h"

class UStaticMeshComponent;
class USceneComponent;

/**
 * The driver figure a generated car carries in its seat (car.toml
 * `[driver]`, docs/CAR_MODELS.md): a GLB of his own in the body mesh's
 * frame, so it sits on the body with no transform.
 *
 * He is drawn on every car but the one the cockpit camera sits in: that
 * camera is his eyes. There he is hidden, but still casts his shadow into
 * the cockpit, which is what a driver sees of himself.
 */
namespace ApexDriver
{
	/** Whether the figure renders: the car has one, its bodywork is shown, and he is not the camera. */
	inline bool IsDrawn(bool bHasDriver, bool bMeshVisible, bool bDriverVisible)
	{
		return bHasDriver && bMeshVisible && bDriverVisible;
	}

	/** Whether the hidden figure still casts a shadow: only when it is the camera that hides him. */
	inline bool CastsHiddenShadow(bool bHasDriver, bool bMeshVisible, bool bDriverVisible)
	{
		return bHasDriver && bMeshVisible && !bDriverVisible;
	}
}

/**
 * The driver component on a car body. Held by the race car actor (the race
 * director hides it for the car the cockpit camera rides); the component is
 * made in the owner's constructor and attached to its body mesh component.
 */
USTRUCT()
struct APEXSIM_API FApexCarDriver
{
	GENERATED_BODY()

	/** Constructor-only: creates the component as a default subobject of `Owner`. */
	void CreateComponent(UObject& Owner, USceneComponent* Body);

	/** Loads the spec's mesh; an unusable spec draws no driver. */
	void SetSpec(const FApexDriverSpec& InSpec);

	const FApexDriverSpec& GetSpec() const { return Spec; }
	bool HasDriver() const { return bHasDriver; }

	/** With the bodywork: a car whose mesh is hidden hides its driver too. */
	void SetMeshVisible(bool bVisible);

	/** False for the car the cockpit camera sits in. */
	void SetDriverVisible(bool bVisible);

	/** The figure's component, or null before CreateComponent. */
	UStaticMeshComponent* GetComponent() const { return Component; }

private:
	void Apply();

	UPROPERTY(Transient)
	TObjectPtr<UStaticMeshComponent> Component;

	FApexDriverSpec Spec;
	bool bHasDriver = false;
	bool bMeshVisible = true;
	bool bDriverVisible = true;
};
