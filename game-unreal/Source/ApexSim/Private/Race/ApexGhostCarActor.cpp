#include "Race/ApexGhostCarActor.h"

#include "Components/StaticMeshComponent.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Race/ApexRaceCoordinate.h"

namespace
{
	/** Most a ghost wheel is drawn turning per frame; see the car actor's own. */
	TAutoConsoleVariable<float> CVarGhostWheelMaxDegPerFrame(
		TEXT("apexsim.ghost.WheelMaxDegPerFrame"),
		12.0f,
		TEXT("Most a ghost car's wheel is drawn turning per frame, in degrees"),
		ECVF_Default);

	/** The Interchange glTF parent's colour and emissive inputs. */
	const FName BaseColorFactorParam(TEXT("BaseColorFactor"));
	const FName BaseColorParam(TEXT("BaseColor"));
	const FName GhostEmissiveFactorParam(TEXT("EmissiveFactor"));

	/** A cold, slightly luminous car: unmistakably not a competitor. */
	const FLinearColor GhostTint(0.55f, 0.85f, 1.0f, 1.0f);
	const FLinearColor GhostGlow(0.10f, 0.28f, 0.40f, 1.0f);

	void TintComponent(UStaticMeshComponent& Component)
	{
		const int32 Slots = Component.GetNumMaterials();
		for (int32 Slot = 0; Slot < Slots; ++Slot)
		{
			if (UMaterialInstanceDynamic* Mid = Component.CreateAndSetMaterialInstanceDynamic(Slot))
			{
				// Whichever of the two names the parent exposes; an unknown
				// parameter is silently ignored.
				Mid->SetVectorParameterValue(BaseColorFactorParam, GhostTint);
				Mid->SetVectorParameterValue(BaseColorParam, GhostTint);
				Mid->SetVectorParameterValue(GhostEmissiveFactorParam, GhostGlow);
			}
		}
	}
}

AApexGhostCarActor::AApexGhostCarActor()
{
	// Not a rendered player: never part of the mirror captures' concerns or
	// the TV director's field. Hidden until a clock puts it on the lap.
	SetActorHiddenInGame(true);
}

void AApexGhostCarActor::BeginPlay()
{
	Super::BeginPlay();
	// A ghost makes no sound: the record lap's engine note would fight the
	// player's own, a few metres away at the same revs.
	SetEngineVolume(0.0f);
}

void AApexGhostCarActor::SetLap(const FApexGhostLap& InLap)
{
	Lap = InLap;
	bHasPrevLocation = false;
	Show(false);
}

void AApexGhostCarActor::SetClockMs(float Ms)
{
	ClockMs = Ms;
}

void AApexGhostCarActor::SetGhostVisible(bool bVisible)
{
	bVisibleWanted = bVisible;
}

bool AApexGhostCarActor::GetCurrentSample(FApexGhostSample& Out) const
{
	if (!Lap.IsValid() || ClockMs < 0.0f)
	{
		return false;
	}
	return Lap.SampleAt(ClockMs, Out);
}

void AApexGhostCarActor::ApplyGhostLook()
{
	if (UStaticMeshComponent* Body = GetMeshComponent())
	{
		TintComponent(*Body);
	}
	ForEachWheelComponent([](UStaticMeshComponent& Wheel) { TintComponent(Wheel); });
}

void AApexGhostCarActor::Show(bool bVisible)
{
	if (bShown == bVisible)
	{
		return;
	}
	bShown = bVisible;
	SetActorHiddenInGame(!bVisible);
	if (!bVisible)
	{
		bHasPrevLocation = false;
	}
}

void AApexGhostCarActor::Tick(float DeltaSeconds)
{
	// Deliberately not Super::Tick: the base reads its pose from the motion
	// buffer, which nothing feeds here.
	AActor::Tick(DeltaSeconds);

	FApexGhostSample Sample;
	if (!bVisibleWanted || !GetCurrentSample(Sample))
	{
		Show(false);
		return;
	}

	const FVector Location = ApexRace::ServerToUnrealPosition(Sample.Position);
	const FQuat Rotation =
		ApexRace::ServerToUnrealRotation(Sample.YawRad, Sample.PitchRad, Sample.RollRad).Quaternion();
	SetActorLocationAndRotation(Location, Rotation);

	// The wheels roll from how far the drawn car moved, as the live cars do.
	const float Rolled = bHasPrevLocation
		? ApexWheels::RolledDistanceM(PrevLocation, Location, Rotation, TeleportDistanceCm)
		: 0.0f;
	Wheels.Update(Sample.Steering, Rolled,
		FMath::DegreesToRadians(CVarGhostWheelMaxDegPerFrame.GetValueOnGameThread()));
	PrevLocation = Location;
	bHasPrevLocation = true;
	// The trace carries no throttle or brake: the lights stay off.
	SetPuppetState(Sample.SpeedMps, Sample.EngineRpm, Sample.Gear, Sample.Steering, 0.0f, 0.0f);

	Show(true);
}
