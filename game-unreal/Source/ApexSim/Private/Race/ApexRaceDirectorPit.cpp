// The race director's pit box: the marker a driver who drives the pit lane
// themselves stops in, where the server's `PitBox` says
// (docs/server/pit-lane.md). The rest of the director is in
// ApexRaceDirector.cpp.

#include "Race/ApexRaceDirector.h"

#include "ApexNetSubsystem.h"
#include "ApexSim.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Camera/CameraComponent.h"
#include "GameFramework/SpringArmComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Race/ApexPitCamera.h"
#include "Race/ApexRaceCarActor.h"
#include "Race/ApexRaceCoordinate.h"

namespace ApexPitBoxMarker
{
	/** The outline: about a car's footprint, so a car inside it is within the crew's radius. */
	constexpr float LengthM = 5.0f;
	constexpr float WidthM = 2.4f;
	constexpr float LineM = 0.12f;
	/** The stop line across the front, thicker than the rest. */
	constexpr float StopLineM = 0.3f;
	/** Painted lines stand this proud of the ground, so the road does not swallow them. */
	constexpr float LineHeightM = 0.03f;
	/** The lollipop in front of the nose: a pole and a board facing the car, seen from down the lane. */
	constexpr float PostAheadM = LengthM / 2.0f + 0.8f;
	constexpr float PostHeightM = 1.4f;
	constexpr float BoardM = 0.55f;
	/**
	 * The ground is looked for from just above the spot (the server's height
	 * is the lane's own) down this far, cm. From higher up the trace found
	 * the garage's canopy and laid the outline on top of it.
	 */
	constexpr float TraceAboveCm = 60.0f;
	constexpr float TraceBelowCm = 300.0f;
	const FLinearColor Colour(1.0f, 0.72f, 0.02f);

	/** One piece: its middle (server frame, m, off the spot along and across the box and up) and its size (m). */
	struct FPiece
	{
		float Ahead;
		float Left;
		float Up;
		FVector SizeM;
	};

	const FPiece Pieces[] = {
		{0.0f, WidthM / 2.0f, LineHeightM / 2.0f, FVector(LengthM, LineM, LineHeightM)},
		{0.0f, -WidthM / 2.0f, LineHeightM / 2.0f, FVector(LengthM, LineM, LineHeightM)},
		{-LengthM / 2.0f, 0.0f, LineHeightM / 2.0f, FVector(LineM, WidthM, LineHeightM)},
		{LengthM / 2.0f, 0.0f, LineHeightM / 2.0f, FVector(StopLineM, WidthM, LineHeightM)},
		{PostAheadM, 0.0f, PostHeightM / 2.0f, FVector(0.06f, 0.06f, PostHeightM)},
		{PostAheadM, 0.0f, PostHeightM + BoardM / 2.0f, FVector(0.04f, BoardM, BoardM)},
	};
}

void AApexRaceDirector::UpdatePitBoxMarker()
{
	const UApexNetSubsystem* Net = GetNet();
	const FApexPitBox* Box = Net ? &Net->GetOwnPitBox() : nullptr;
	const AApexRaceCarActor* Local = Net ? FindCar(Net->GetLocalCarIndex()) : nullptr;
	// Only the driver's own box, only while they drive the lane themselves
	// (the server says so: before a takeover the autopilot's flag is off for
	// everyone) and the crew have yet to see them: a car leaving its box
	// would drive out through it.
	const bool bShow = Box && Box->bValid && Box->bManual && bTrackConditionsApplied && !bReplayView && !bDemoView
		&& !bSpectating && Local && FollowedCar == Local && bLocalInPitLane && !bLocalPitAutopilot
		&& !bLocalPitServicing && !bLocalServicedThisVisit;
	if (!bShow)
	{
		if (PitBoxMarker && PitBoxMarker->IsVisible())
		{
			PitBoxMarker->SetVisibility(false);
		}
		return;
	}

	if (!PitBoxMarker)
	{
		PitBoxMarker = NewObject<UInstancedStaticMeshComponent>(this, TEXT("PitBoxMarker"));
		PitBoxMarker->SetUsingAbsoluteLocation(true);
		PitBoxMarker->SetUsingAbsoluteRotation(true);
		PitBoxMarker->SetUsingAbsoluteScale(true);
		PitBoxMarker->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		PitBoxMarker->SetCastShadow(false);
		PitBoxMarker->SetMobility(EComponentMobility::Movable);
		if (UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")))
		{
			PitBoxMarker->SetStaticMesh(Cube);
		}
		if (UMaterialInterface* Shape = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial")))
		{
			UMaterialInstanceDynamic* Paint = UMaterialInstanceDynamic::Create(Shape, PitBoxMarker);
			Paint->SetVectorParameterValue(TEXT("Color"), ApexPitBoxMarker::Colour);
			PitBoxMarker->SetMaterial(0, Paint);
		}
		PitBoxMarker->SetupAttachment(GetRootComponent());
		PitBoxMarker->RegisterComponent();
	}

	if (PitBoxMarkerLaidFor != Box->PitBox)
	{
		PitBoxMarker->ClearInstances();
		// On the ground under the spot; the server's height when there is none to find.
		FVector Spot = ApexRace::ServerToUnrealPosition(Box->Position);
		FCollisionQueryParams Query(SCENE_QUERY_STAT(ApexPitBoxMarker), /*bTraceComplex*/ false, this);
		// A car parked on the spot (towed there) is not the ground.
		for (const TPair<int32, TObjectPtr<AApexRaceCarActor>>& Car : Cars)
		{
			Query.AddIgnoredActor(Car.Value.Get());
		}
		// A trace that starts inside something (the pit building's collision
		// reaches over the lane) reports where it started: that is skipped
		// and the trace made again.
		UWorld* World = GetWorld();
		for (int32 Try = 0; World && Try < 4; ++Try)
		{
			FHitResult Hit;
			if (!World->LineTraceSingleByObjectType(Hit, Spot + FVector(0.0, 0.0, ApexPitBoxMarker::TraceAboveCm),
					Spot - FVector(0.0, 0.0, ApexPitBoxMarker::TraceBelowCm), FCollisionObjectQueryParams(ECC_WorldStatic), Query))
			{
				break;
			}
			if (!Hit.bStartPenetrating)
			{
				Spot = Hit.ImpactPoint;
				break;
			}
			UE_LOG(LogApexSim, Verbose, TEXT("Pit box marker: the trace starts inside %s"), *GetNameSafe(Hit.GetComponent()));
			Query.AddIgnoredComponent(Hit.GetComponent());
		}
		const float Cos = FMath::Cos(Box->YawRad);
		const float Sin = FMath::Sin(Box->YawRad);
		const FRotator Yaw = ApexRace::ServerToUnrealRotation(Box->YawRad, 0.0f, 0.0f);
		for (const ApexPitBoxMarker::FPiece& Piece : ApexPitBoxMarker::Pieces)
		{
			// Along and across the box in the server's frame (+Y left), then over.
			const FVector Offset = ApexRace::ServerToUnrealPosition(FVector(
				Piece.Ahead * Cos - Piece.Left * Sin, Piece.Ahead * Sin + Piece.Left * Cos, Piece.Up));
			// The engine cube is a metre a side.
			PitBoxMarker->AddInstance(FTransform(Yaw, Spot + Offset, Piece.SizeM), /*bWorldSpace*/ true);
		}
		PitBoxMarkerLaidFor = Box->PitBox;
		UE_LOG(LogApexSim, Log, TEXT("Pit box %d marked at %s"), Box->PitBox + 1, *Spot.ToCompactString());
	}
	if (!PitBoxMarker->IsVisible())
	{
		PitBoxMarker->SetVisibility(true);
	}
}

void AApexRaceDirector::UpdatePitCamera()
{
	const UApexNetSubsystem* Net = GetNet();
	const FApexPitBox* Box = Net ? &Net->GetOwnPitBox() : nullptr;
	const AApexRaceCarActor* Local = Net ? FindCar(Net->GetLocalCarIndex()) : nullptr;
	bool bHold = false;
	FVector SpotCm = FVector::ZeroVector;
	// The player's own car in the chase view: the cockpit sees its own pit
	// board, and the TV and shot cameras have poses of their own.
	if (Box && Box->bValid && Local && FollowedCar == Local && !bCockpitView && !bTvView && !bFinishPanorama
		&& !bShotCameraPose && !bReplayView && !bDemoView && !bSpectating && bLocalInPitLane)
	{
		SpotCm = ApexRace::ServerToUnrealPosition(Box->Position);
		const float DistanceM = FVector::Dist2D(Local->GetActorLocation(), SpotCm) / ApexRace::MetresToCentimetres;
		bHold = ApexPitCamera::Holds(bPitCameraPose, bLocalPitServicing, DistanceM, GetFollowedSpeedKph() / 3.6f);
	}

	if (bHold != bPitCameraPose)
	{
		bPitCameraPose = bHold;
		ApplyCameraMode();
		if (!bHold)
		{
			// Back on the boom: its lag would swing it in from where it was
			// left when the pose took over.
			CameraBoom->bEnableCameraLag = false;
			CameraBoom->bEnableCameraRotationLag = false;
			BoomSnapFrames = 2;
		}
		UE_LOG(LogApexSim, Log, TEXT("Pit camera %s"), bHold ? TEXT("on: the car is at its box") : TEXT("off"));
	}
	if (!bPitCameraPose)
	{
		return;
	}

	const FVector Car = Local->GetActorLocation();
	FVector Camera = ApexRace::ServerToUnrealPosition(
		ApexPitCamera::Position(Box->Position, Box->YawRad, Box->LateralM, Box->LaneWidthM));
	Camera.Z = Car.Z + ApexPitCamera::HeightCm;
	const FVector Aim = Car + FVector(0.0, 0.0, ApexPitCamera::AimHeightCm);
	TvCamera->SetWorldLocationAndRotation(Camera, (Aim - Camera).Rotation());
	TvCamera->SetFieldOfView(ApexPitCamera::FovDeg);
	TvCamera->PostProcessSettings.DepthOfFieldFocalDistance = 0.0f;
}

void AApexRaceDirector::ForgetPitBoxMarker()
{
	if (PitBoxMarker)
	{
		PitBoxMarker->ClearInstances();
		PitBoxMarker->SetVisibility(false);
	}
	PitBoxMarkerLaidFor = -1;
	if (bPitCameraPose)
	{
		bPitCameraPose = false;
		ApplyCameraMode();
	}
	bLocalInPitLane = false;
	bLocalPitAutopilot = false;
	bLocalPitServicing = false;
	bLocalServicedThisVisit = false;
}
