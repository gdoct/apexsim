#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "ApexCloudLayer.generated.h"

class UMaterialInstanceDynamic;
class UStaticMeshComponent;

/**
 * The cloud over the circuit: a dome of the engine's sphere round the
 * camera, drawn with `M_ApexCloudLayer` (ApexTrackMaterials::CloudName, an
 * unlit translucent parent the material bake writes), which projects a
 * noise onto a plane `CloudHeight` above the camera and shows cloud where
 * the noise clears `1 - CloudCover`: a few cumulus on a cloudy day, one
 * grey sheet from overcast on, nothing in the sun. The race director sets
 * the cover, the colour and how bright the sheet is from the sky it lights
 * by (`ApexSky::FSkyState::CloudCover`, `CloudColor`, `CloudBrightness`),
 * and the wind, which the cloud drifts with: the actor adds the drift up
 * frame by frame (`CloudOffset`), so a gust speeds the cloud up rather than
 * moving the whole sky by the change in wind times the time so far. Nothing is drawn without the
 * parent (a project baked before it has none); `-run=ApexMaterialBake`
 * writes it.
 */
UCLASS()
class APEXSIM_API AApexCloudLayerActor : public AActor
{
	GENERATED_BODY()

public:
	AApexCloudLayerActor();

	virtual void Tick(float DeltaSeconds) override;

	/** The sky's cloud: cover 0..1, the sheet's colour, its brightness in cd/m². */
	void SetSky(float Cover, const FLinearColor& Color, float Brightness);

	/** The wind the cloud drifts with, cm/s in world axes (the dome scales it down: a cloud is far). */
	void SetWind(const FVector& InWindCmPerS);

	bool IsDrawn() const { return bDrawn; }
	float GetCover() const { return CoverNow; }

	/** The dome's radius, cm: well past any track, inside the far plane. */
	static constexpr float RadiusCm = 2000000.0f;
	/** Where the cloud plane lies above the camera, cm. */
	static constexpr float CloudHeightCm = 150000.0f;
	/** The wind up there against the wind at the ground. */
	static constexpr float WindAloft = 8.0f;

private:
	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<UStaticMeshComponent> Dome;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> Material;

	float CoverNow = 0.0f;
	bool bDrawn = false;
	/** The wind now, cm/s, and how far it has carried the cloud, cm. */
	FVector2D WindCmPerS = FVector2D::ZeroVector;
	FVector2D DriftCm = FVector2D::ZeroVector;
};
