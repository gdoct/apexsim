#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "ApexBoundsWatch.generated.h"

class UPrimitiveComponent;

/**
 * Watches for primitives whose bounds the renderer should never see: NaN or
 * infinite, or out past any circuit. Virtual shadow maps cull non-Nanite
 * geometry page by page over each instance's bounds
 * (`CullPerPageDrawCommandsCs`), and a hung GPU (DXGI_ERROR_DEVICE_HUNG)
 * was caught in exactly that shader with two warps left running: one
 * instance with absurd bounds is a loop that never ends. Pure for the tests
 * (`ApexSim.BoundsWatch`).
 */
namespace ApexBoundsWatch
{
	/** No circuit, its DEM horizon included, comes near this radius (20 km). */
	inline constexpr double MaxRadiusCm = 2.0e6;
	/** Nor this far from the world origin (50 km). */
	inline constexpr double MaxOriginCm = 5.0e6;
	/** From this radius on, a new largest primitive is logged (1 km). */
	inline constexpr double ReportRadiusCm = 1.0e5;

	/** What is wrong with these bounds, or empty when nothing is. */
	APEXSIM_API FString Problem(const FBoxSphereBounds& Bounds);
}

/**
 * Checks every rendered primitive of a game world after each frame's actor
 * ticks, before the frame's transforms go to the render thread. A bad one
 * is logged once (owner, component, mobility, bounds) as an error, and the
 * log flushed, so the line is on disk ahead of a GPU hang it may cause; a
 * new largest primitive past ReportRadiusCm is logged too.
 * `apexsim.debug.BoundsWatch 0` turns it off (off in Shipping).
 */
UCLASS()
class APEXSIM_API UApexBoundsWatchSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

private:
	void HandlePostActorTick(UWorld* World, ELevelTick TickType, float DeltaSeconds);
	void Check(const UPrimitiveComponent& Component);

	FDelegateHandle PostActorTickHandle;
	/** Components already reported, so a stuck one is one line. */
	TSet<TWeakObjectPtr<const UPrimitiveComponent>> Reported;
	double LargestRadiusCm = ApexBoundsWatch::ReportRadiusCm;
	/** Scan cost, logged once a few hundred frames in. */
	int32 Frames = 0;
	double ScanSeconds = 0.0;
	int32 LastCount = 0;
};
