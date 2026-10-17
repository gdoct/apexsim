#include "ApexBoundsWatch.h"

#include "ApexSim.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/OutputDeviceRedirector.h"

static TAutoConsoleVariable<int32> CVarBoundsWatch(
	TEXT("apexsim.debug.BoundsWatch"),
	// About 0.2 ms a frame for a thousand primitives: on while the GPU hang
	// is hunted, never in a shipped game.
	UE_BUILD_SHIPPING ? 0 : 1,
	TEXT("Check every rendered primitive's bounds each frame and log NaN, infinite or out-of-world ones (0 off)."),
	ECVF_Default);

namespace ApexBoundsWatch
{
	FString Problem(const FBoxSphereBounds& Bounds)
	{
		// ContainsNaN is true of an infinity too.
		if (Bounds.Origin.ContainsNaN() || Bounds.BoxExtent.ContainsNaN() || !FMath::IsFinite(Bounds.SphereRadius))
		{
			return TEXT("NaN or infinite bounds");
		}
		if (Bounds.SphereRadius > MaxRadiusCm)
		{
			return FString::Printf(TEXT("radius %.1f km"), Bounds.SphereRadius / 1.0e5);
		}
		if (Bounds.Origin.Size() > MaxOriginCm)
		{
			return FString::Printf(TEXT("origin %.1f km out"), Bounds.Origin.Size() / 1.0e5);
		}
		return FString();
	}
}

bool UApexBoundsWatchSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	return World && (World->WorldType == EWorldType::Game || World->WorldType == EWorldType::PIE);
}

void UApexBoundsWatchSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	// After the actors have moved, before SendAllEndOfFrameUpdates hands the
	// frame's transforms to the render thread.
	PostActorTickHandle = FWorldDelegates::OnWorldPostActorTick.AddUObject(this, &UApexBoundsWatchSubsystem::HandlePostActorTick);
}

void UApexBoundsWatchSubsystem::Deinitialize()
{
	FWorldDelegates::OnWorldPostActorTick.Remove(PostActorTickHandle);
	Super::Deinitialize();
}

void UApexBoundsWatchSubsystem::HandlePostActorTick(UWorld* World, ELevelTick TickType, float DeltaSeconds)
{
	if (World != GetWorld() || CVarBoundsWatch.GetValueOnGameThread() == 0)
	{
		return;
	}
	const double Start = FPlatformTime::Seconds();
	int32 Count = 0;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		It->ForEachComponent<UPrimitiveComponent>(false, [this, &Count](UPrimitiveComponent* Component)
		{
			// Only what the renderer holds: a registered component with a proxy.
			if (Component->IsRegistered() && Component->SceneProxy)
			{
				++Count;
				Check(*Component);
			}
		});
	}
	ScanSeconds += FPlatformTime::Seconds() - Start;
	LastCount = Count;
	if (++Frames == 600)
	{
		UE_LOG(LogApexSim, Log, TEXT("Bounds watch: %d primitive(s), %.3f ms a frame over the last 600 frames"),
			LastCount, ScanSeconds * 1000.0 / Frames);
	}
}

void UApexBoundsWatchSubsystem::Check(const UPrimitiveComponent& Component)
{
	const FBoxSphereBounds& Bounds = Component.Bounds;
	const FString Problem = ApexBoundsWatch::Problem(Bounds);
	const AActor* Owner = Component.GetOwner();
	if (!Problem.IsEmpty())
	{
		bool bAlreadyReported = false;
		Reported.Add(&Component, &bAlreadyReported);
		if (!bAlreadyReported)
		{
			const FTransform& Transform = Component.GetComponentTransform();
			UE_LOG(LogApexSim, Error,
				TEXT("Bounds watch: %s on %s.%s (%s, %s): bounds origin %s extent %s radius %f; component at %s rotation %s scale %s"),
				*Problem, Owner ? *Owner->GetName() : TEXT("(no owner)"), *Component.GetName(), *Component.GetClass()->GetName(),
				Component.Mobility == EComponentMobility::Movable ? TEXT("movable") : TEXT("static"),
				*Bounds.Origin.ToString(), *Bounds.BoxExtent.ToString(), Bounds.SphereRadius,
				*Transform.GetLocation().ToString(), *Transform.GetRotation().ToString(), *Transform.GetScale3D().ToString());
			// The GPU may hang on this very frame: get the line to disk first.
			GLog->Flush();
		}
		return;
	}
	// A tenth over the last one, so a row of equal horizon tiles is one line.
	if (Bounds.SphereRadius > LargestRadiusCm * 1.1)
	{
		LargestRadiusCm = Bounds.SphereRadius;
		UE_LOG(LogApexSim, Log, TEXT("Bounds watch: largest primitive so far %s.%s (%s), radius %.2f km at %s"),
			Owner ? *Owner->GetName() : TEXT("(no owner)"), *Component.GetName(), *Component.GetClass()->GetName(),
			Bounds.SphereRadius / 1.0e5, *Bounds.Origin.ToString());
	}
}
