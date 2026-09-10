#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Race/ApexCockpitLayout.h"

#include "ApexCockpitRig.generated.h"

class AApexRaceCarActor;
class UApexCockpitDashWidget;
class UApexMirrorWidget;
class UMaterialInterface;
class USceneCaptureComponent2D;
class UStaticMesh;
class UStaticMeshComponent;
class UTextureRenderTarget2D;
class UWidgetComponent;

/** What the rig should be showing and rendering this frame. */
struct FApexCockpitFeatures
{
	/** The driver is in the cockpit view; nothing on the rig draws otherwise. */
	bool bCockpitActive = false;
	bool bWheel = true;
	bool bMirrors = true;
	/** Rear view for the HUD; captured in either camera view. */
	bool bVirtualMirror = false;
	/** 0 low .. 2 high: capture resolution and how many mirrors refresh per frame. */
	int32 MirrorQuality = 1;
	bool bMetric = true;
};

/**
 * The inside of the followed car: a steering wheel with a display on its hub,
 * and mirrors that show the field behind.
 *
 * Everything here is built from engine primitives and widgets, because the
 * car meshes are exteriors — they give a cockpit position, not a cockpit.
 * The rig is attached to the followed car so it rides the same smoothed
 * transform; where each part sits comes from the car's FApexCockpitLayout,
 * and the driver's eye (with the seat adjustments) is what the screens face.
 *
 * Mirrors are scene captures into render targets shown on widget faces.
 * Captures are the expensive part — each is a scene render — so they are
 * not left on "every frame": the rig refreshes them itself, round-robin, at
 * a rate the mirror-quality setting picks.
 *
 * The faces are unlit, and the race is exposed for a 50 klux sun, so at a
 * tint of 1 they would render black. `apexsim.cockpit.ScreenNits` is the
 * emissive multiplier that keeps them readable against daylight.
 */
UCLASS()
class APEXSIM_API AApexCockpitRig : public AActor
{
	GENERATED_BODY()

public:
	AApexCockpitRig();

	virtual void Tick(float DeltaSeconds) override;

	/** Move into this car: lay the parts out for it and ride along. */
	void AttachToCar(AApexRaceCarActor* Car);
	void DetachFromCar();

	/** The eye the screens turn to face, in the car's frame. */
	void SetEyeLocal(const FVector& InEyeLocal);

	void SetFeatures(const FApexCockpitFeatures& InFeatures);

	/** Session countdown for the display; negative when not counting. */
	void SetCountdownMs(int32 Ms) { CountdownMs = Ms; }

	/** The rear view for the HUD, or null while the virtual mirror is off. */
	UTextureRenderTarget2D* GetVirtualMirrorTexture() const;

protected:
	virtual void BeginPlay() override;

private:
	/** One mirror: its glass, its bezel and the camera behind it. */
	struct FMirror
	{
		TObjectPtr<USceneComponent> Mount;
		TObjectPtr<UStaticMeshComponent> Bezel;
		TObjectPtr<UWidgetComponent> Face;
		TObjectPtr<USceneCaptureComponent2D> Capture;
		TObjectPtr<UTextureRenderTarget2D> Target;
		FIntPoint BaseResolution = FIntPoint(384, 240);
		float FovDeg = 40.0f;
		bool bWanted = false;
	};

	void BuildWheel();
	FMirror BuildMirror(const TCHAR* Name, const FIntPoint& BaseResolution, float FovDeg, bool bWithFace);

	/** Position every part from the layout and turn the faces to the eye. */
	void PlaceParts();
	void PlaceMirror(FMirror& Mirror, const FVector& Local, const FVector2D& SizeCm, const FRotator& CaptureRotation);
	void ApplyVisibility();
	void EnsureRenderTargets();
	void ApplyScreenBrightness();

	void UpdateWheel(float DeltaSeconds);
	void PushDash();
	void RunCaptures();

	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<USceneComponent> Root;

	// --- Wheel ------------------------------------------------------------------

	/** Raked by the layout and rolled by the steering; everything of the wheel hangs off it. */
	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<USceneComponent> WheelPivot;

	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<UStaticMeshComponent> WheelHub;

	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<UStaticMeshComponent> GripLeft;

	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<UStaticMeshComponent> GripRight;

	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<UStaticMeshComponent> SpokeTop;

	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<UStaticMeshComponent> SpokeBottom;

	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<UWidgetComponent> Dash;

	// --- Mirrors ----------------------------------------------------------------

	FMirror Centre;
	FMirror Left;
	FMirror Right;
	/** No glass: this one only feeds the HUD. */
	FMirror Virtual;

	/** Kept so the garbage collector sees what the FMirror structs hold. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UObject>> Owned;

	UPROPERTY(Transient)
	TObjectPtr<UStaticMesh> CubeMesh;

	UPROPERTY(Transient)
	TObjectPtr<UStaticMesh> CylinderMesh;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> ShapeMaterial;

	UPROPERTY(Transient)
	TObjectPtr<AApexRaceCarActor> Car;

	FApexCockpitLayout Layout;
	FVector EyeLocal = FVector::ZeroVector;
	FApexCockpitFeatures Features;
	int32 CountdownMs = -1;

	float WheelRollDeg = 0.0f;
	/** Highest RPM the car has shown: the light bar's full scale. */
	float ObservedMaxRpm = 8000.0f;
	/** Which mirror the round-robin refreshes next. */
	int32 CaptureCursor = 0;
	/** Frames since the last refresh, for the low-quality half rate. */
	int32 CaptureSkip = 0;
	/** Quality the render targets were sized for; -1 until built. */
	int32 BuiltQuality = -1;
	float AppliedNits = -1.0f;
	bool bPlaced = false;
};
