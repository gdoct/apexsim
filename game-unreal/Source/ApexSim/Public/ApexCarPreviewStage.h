#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "ApexCarPreviewStage.generated.h"

class URectLightComponent;
class USceneCaptureComponent2D;
class UStaticMesh;
class UStaticMeshComponent;
class UTextureRenderTarget2D;

/**
 * A single lit turntable that renders the selected car into a render target,
 * which the car-select screen samples through a UI material.
 *
 * One shared stage, not one per card. The Godot client gave every car card its
 * own live SubViewport (CarCard.cs); four concurrent scene captures is four
 * extra scene renders per frame, which is a lot to spend on a menu.
 *
 * Place exactly one of these in L_Menu.
 */
UCLASS()
class APEXSIM_API AApexCarPreviewStage : public AActor
{
	GENERATED_BODY()

public:
	AApexCarPreviewStage();

	virtual void Tick(float DeltaSeconds) override;

	/** Swaps the displayed car. Passing an unset mesh hides the stage. */
	UFUNCTION(BlueprintCallable, Category = "ApexSim|Preview")
	void SetCarMesh(const TSoftObjectPtr<UStaticMesh>& MeshToShow);

	/** Applies the per-car framing tweaks from the catalog row. */
	UFUNCTION(BlueprintCallable, Category = "ApexSim|Preview")
	void SetPreviewTransform(FVector Offset, FRotator Rotation, float Scale);

	UFUNCTION(BlueprintCallable, Category = "ApexSim|Preview")
	void SetTurntableEnabled(bool bEnabled) { bSpin = bEnabled; }

	/** Back to the framed three-quarter view, for a still rather than a spin. */
	UFUNCTION(BlueprintCallable, Category = "ApexSim|Preview")
	void ResetTurntable();

	/** Finds the stage in the current world, or null if the level has none. */
	UFUNCTION(BlueprintCallable, Category = "ApexSim|Preview", meta = (WorldContext = "WorldContextObject"))
	static AApexCarPreviewStage* Find(const UObject* WorldContextObject);

	/**
	 * The texture the car is rendered into. UImage::SetBrushResourceObject
	 * accepts a render target directly, so the preview needs no UI material.
	 */
	UFUNCTION(BlueprintPure, Category = "ApexSim|Preview")
	UTextureRenderTarget2D* GetPreviewRenderTarget() const { return PreviewRenderTarget; }

	/**
	 * Pixel size of the render target. Whatever draws it must keep this aspect
	 * (a scale box, or a brush of this size in a slot that does not stretch):
	 * the capture is a camera image, and stretching it to a panel of another
	 * shape stretches the car with it.
	 */
	UFUNCTION(BlueprintPure, Category = "ApexSim|Preview")
	FVector2D GetPreviewSize() const { return FVector2D(PreviewWidth, PreviewHeight); }

protected:
	virtual void BeginPlay() override;

	/** Everything hangs off here so the whole rig can be moved as one. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<USceneComponent> Root;

	/** Yaws continuously; the mesh is parented to it. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<USceneComponent> Turntable;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UStaticMeshComponent> CarMesh;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<USceneCaptureComponent2D> Capture;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<URectLightComponent> KeyLight;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<URectLightComponent> FillLight;

	/**
	 * Created at BeginPlay. A render target is a transient thing that only this
	 * actor writes and only the car-select screen reads, so it is built in code
	 * rather than checked in as an asset nothing else references.
	 */
	UPROPERTY(Transient, BlueprintReadOnly, Category = "ApexSim|Preview")
	TObjectPtr<UTextureRenderTarget2D> PreviewRenderTarget;

	/** 16:9, the shape of both places that show it (garage stage, session card). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ApexSim|Preview")
	int32 PreviewWidth = 1536;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ApexSim|Preview")
	int32 PreviewHeight = 864;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ApexSim|Preview")
	float TurntableDegreesPerSecond = 20.0f;

	/** Distance multiplier applied to the framed bounds when placing the camera. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ApexSim|Preview")
	float FramingMargin = 1.12f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ApexSim|Preview")
	float CaptureFieldOfView = 40.0f;

private:
	/** Recentres the mesh on the turntable and pulls the camera back to fit it. */
	void FrameCurrentMesh();

	bool bSpin = true;
	FVector PreviewOffset = FVector::ZeroVector;
	FRotator PreviewRotation = FRotator::ZeroRotator;
	float PreviewScale = 1.0f;
};
