#include "Race/ApexRaceDirector.h"

#include "ApexMenuFlowSubsystem.h"
#include "ApexNetSubsystem.h"
#include "ApexPlayerController.h"
#include "ApexSettingsSubsystem.h"
#include "ApexSim.h"
#include "Camera/CameraComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/SpotLightComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/Level.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/GameInstance.h"
#include "Engine/LevelStreamingDynamic.h"
#include "Engine/SkyLight.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/SpringArmComponent.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Components/StaticMeshComponent.h"
#include "Audio/ApexRoadSound.h"
#include "Audio/ApexUiAudioSubsystem.h"
#include "Input/ApexForceFeedback.h"
#include "Race/ApexCockpitRig.h"
#include "Race/ApexGhostCarActor.h"
#include "Race/ApexRaceCarActor.h"
#include "Race/ApexRaceCoordinate.h"
#include "Race/ApexRacingLineActor.h"
#include "Race/ApexRainActor.h"
#include "Race/ApexShotCamera.h"

AApexRaceDirector::AApexRaceDirector()
{
	PrimaryActorTick.bCanEverTick = true;
	// The cockpit camera is placed from the followed car's transform, so this
	// has to tick after the car has moved for the frame. Cars tick in the
	// default group; without this (and the per-car prerequisite set in
	// UpdateCameraTarget) the order varied frame to frame, and the wheel and
	// mirrors — attached to the car, centimetres from the eye — jumped a
	// frame's worth of travel forward and back against the camera.
	PrimaryActorTick.TickGroup = TG_PostPhysics;

	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);

	CameraBoom = CreateDefaultSubobject<USpringArmComponent>(TEXT("CameraBoom"));
	CameraBoom->SetupAttachment(Root);
	// Length, height, pitch and lag all come from the chase ladder's current
	// rung (ApplyChaseView); these are the far rung's, so a boom that is never
	// applied behaves as the one chase camera used to.
	CameraBoom->TargetArmLength = ApexChase::Get(ChaseLevel).ArmLengthCm;
	CameraBoom->SetRelativeRotation(FRotator(ApexChase::Get(ChaseLevel).PitchDeg, 0.0f, 0.0f));
	// The boom lags behind the car rather than being welded to it, which reads
	// as a chase camera instead of a rigid mount. The closer rungs barely lag:
	// from the roof the camera is part of the car, and a lagging one there
	// swings the roofline about.
	CameraBoom->bEnableCameraLag = true;
	CameraBoom->bEnableCameraRotationLag = true;
	CameraBoom->CameraLagSpeed = ApexChase::Get(ChaseLevel).LagSpeed;
	CameraBoom->CameraRotationLagSpeed = ApexChase::Get(ChaseLevel).RotationLagSpeed;
	CameraBoom->CameraLagMaxDistance = ApexChase::Get(ChaseLevel).LagMaxDistanceCm;
	CameraBoom->bDoCollisionTest = false;

	ChaseCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("ChaseCamera"));
	ChaseCamera->SetupAttachment(CameraBoom);

	// Attached to the root only so it has a parent; its world transform is
	// driven straight from the car every tick, so nothing about the boom's
	// lag or its levelled yaw leaks into the cockpit view.
	CockpitCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("CockpitCamera"));
	CockpitCamera->SetupAttachment(Root);
	CockpitCamera->SetAbsolute(true, true, false);
	CockpitCamera->FieldOfView = 95.0f;

	// Placed only by the shot commands, in world space, and never by Tick.
	ShotCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("ShotCamera"));
	ShotCamera->SetupAttachment(Root);
	ShotCamera->SetAbsolute(true, true, false);
	ShotCamera->FieldOfView = 70.0f;

	TvCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("TvCamera"));
	TvCamera->SetupAttachment(Root);
	TvCamera->SetAbsolute(true, true, false);
	TvCamera->FieldOfView = 60.0f;
	// Focus and aperture change per shot; the overrides stay on and a zero
	// focal distance is what turns the effect off for the onboard views.
	TvCamera->PostProcessBlendWeight = 1.0f;
	TvCamera->PostProcessSettings.bOverride_DepthOfFieldFocalDistance = true;
	TvCamera->PostProcessSettings.bOverride_DepthOfFieldFstop = true;
	TvCamera->PostProcessSettings.DepthOfFieldFocalDistance = 0.0f;
	TvCamera->PostProcessSettings.DepthOfFieldFstop = 4.0f;

	// Exactly one camera component may be active on a view target; the other
	// is what `SetCockpitView` switches to. Cockpit starts active to match
	// `bCockpitView`'s default.
	CockpitCamera->SetActive(true);
	ChaseCamera->SetActive(false);
	ShotCamera->SetActive(false);
	TvCamera->SetActive(false);
}

namespace
{
	/** Log a shot pose in both frames, so it can be pasted back as a switch. */
	void LogShotPose(const FVector& LocationCm, const FRotator& Rotation, float Fov)
	{
		const FVector Server = ApexRace::UnrealToServerPosition(LocationCm);
		double Yaw = 0.0;
		double Pitch = 0.0;
		ApexRace::UnrealRotationToServerView(Rotation, Yaw, Pitch);
		UE_LOG(LogApexSim, Log,
			TEXT("Shot camera: -ApexCamera=%.2f,%.2f,%.2f,%.2f,%.2f (server m / deg) fov %.1f; "
				 "Unreal loc (%.0f, %.0f, %.0f) cm rot (P=%.2f Y=%.2f R=%.2f)"),
			Server.X, Server.Y, Server.Z, Yaw, Pitch, Fov, LocationCm.X, LocationCm.Y, LocationCm.Z,
			Rotation.Pitch, Rotation.Yaw, Rotation.Roll);
	}

	AApexRaceDirector* FindDirectorForCommand(UWorld* World)
	{
		AApexRaceDirector* Director = AApexRaceDirector::Find(World);
		if (!Director)
		{
			UE_LOG(LogApexSim, Warning, TEXT("apexsim.cam: no race director in this world"));
		}
		return Director;
	}

	/** Parse a Goto (X Y Z [Yaw [Pitch]]) or LookAt (X Y Z TX TY TZ) list into a pose. */
	bool ParseShotPose(const FString& Text, bool bLookAt, ApexShotCamera::FPose& OutPose)
	{
		TArray<double> Values;
		return ApexShotCamera::ParseNumberList(Text, Values)
			&& (bLookAt ? ApexShotCamera::PoseFromLookAt(Values, OutPose)
						: ApexShotCamera::PoseFromGoto(Values, OutPose));
	}

	void RunShotCommand(const TArray<FString>& Args, UWorld* World, bool bLookAt)
	{
		ApexShotCamera::FPose Pose;
		if (!ParseShotPose(FString::Join(Args, TEXT(" ")), bLookAt, Pose))
		{
			UE_LOG(LogApexSim, Warning, TEXT("Usage: %s (server frame: metres, +Y left, yaw CCW, pitch up)"),
				bLookAt ? TEXT("apexsim.cam.LookAt X Y Z TX TY TZ") : TEXT("apexsim.cam.Goto X Y Z [YawDeg] [PitchDeg]"));
			return;
		}
		if (AApexRaceDirector* Director = FindDirectorForCommand(World))
		{
			Director->SetShotCameraPose(Pose.LocationCm, Pose.Rotation);
		}
	}

	/** apexsim.hotlap.Out / .Garage / .Replay / .Stop: the garage card's buttons from the console. */
	void RunHotlapCommand(UWorld* World, const TCHAR* What)
	{
		AApexRaceDirector* Director = FindDirectorForCommand(World);
		UApexNetSubsystem* Net = World && World->GetGameInstance() ? World->GetGameInstance()->GetSubsystem<UApexNetSubsystem>() : nullptr;
		if (!Director || !Net)
		{
			return;
		}
		if (FCString::Stricmp(What, TEXT("Out")) == 0)
		{
			Net->HotlapRelocate(EApexHotlapDestination::Track);
		}
		else if (FCString::Stricmp(What, TEXT("Garage")) == 0)
		{
			Net->HotlapRelocate(EApexHotlapDestination::Garage);
		}
		else if (FCString::Stricmp(What, TEXT("Replay")) == 0)
		{
			if (!Director->BeginGhostReplay())
			{
				UE_LOG(LogApexSim, Warning, TEXT("apexsim.hotlap.Replay: no record lap to replay"));
			}
		}
		else if (FCString::Stricmp(What, TEXT("Stop")) == 0)
		{
			Director->EndGhostReplay();
		}
	}

	FAutoConsoleCommandWithWorld HotlapOutCommand(
		TEXT("apexsim.hotlap.Out"),
		TEXT("Hotlap: out of the garage onto the run-up before the line."),
		FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* World) { RunHotlapCommand(World, TEXT("Out")); }));
	FAutoConsoleCommandWithWorld HotlapGarageCommand(
		TEXT("apexsim.hotlap.Garage"),
		TEXT("Hotlap: back into the garage."),
		FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* World) { RunHotlapCommand(World, TEXT("Garage")); }));
	FAutoConsoleCommandWithWorld HotlapReplayCommand(
		TEXT("apexsim.hotlap.Replay"),
		TEXT("Hotlap: replay the record lap with the ghost and the replay cameras."),
		FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* World) { RunHotlapCommand(World, TEXT("Replay")); }));
	FAutoConsoleCommandWithWorld HotlapStopCommand(
		TEXT("apexsim.hotlap.Stop"),
		TEXT("Hotlap: stop the replay."),
		FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* World) { RunHotlapCommand(World, TEXT("Stop")); }));

	FAutoConsoleCommandWithWorldAndArgs ShotGotoCommand(
		TEXT("apexsim.cam.Goto"),
		TEXT("Park the shot camera: X Y Z [YawDeg] [PitchDeg], server frame (metres, +Y left, yaw CCW from +X, pitch up)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
			{ RunShotCommand(Args, World, /*bLookAt*/ false); }));

	FAutoConsoleCommandWithWorldAndArgs ShotLookAtCommand(
		TEXT("apexsim.cam.LookAt"),
		TEXT("Park the shot camera at X Y Z looking at TX TY TZ, server frame (metres, +Y left)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
			{ RunShotCommand(Args, World, /*bLookAt*/ true); }));

	FAutoConsoleCommandWithWorldAndArgs ShotReleaseCommand(
		TEXT("apexsim.cam.Release"),
		TEXT("Hand the view back from the shot camera to the cockpit or chase camera."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>&, UWorld* World)
			{
				if (AApexRaceDirector* Director = FindDirectorForCommand(World))
				{
					Director->ReleaseShotCamera();
				}
			}));

	TAutoConsoleVariable<int32> CVarTvDebug(
		TEXT("apexsim.tv.Debug"),
		0,
		TEXT("1 prints the broadcast camera's shot, subject and lens on screen."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarTvPace(
		TEXT("apexsim.tv.Pace"),
		1.0f,
		TEXT("Scales how long the broadcast camera holds a shot: below 1 cuts faster."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarTvDof(
		TEXT("apexsim.tv.DepthOfField"),
		1,
		TEXT("0 turns off the broadcast camera's depth of field."),
		ECVF_Default);

	FAutoConsoleCommandWithWorldAndArgs TvViewCommand(
		TEXT("apexsim.tv.View"),
		TEXT("1 films the race with the broadcast camera, 0 hands back to the driving cameras."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
			{
				if (AApexRaceDirector* Director = FindDirectorForCommand(World))
				{
					Director->SetTvView(Args.Num() == 0 || FCString::Atoi(*Args[0]) != 0);
				}
			}));

	FAutoConsoleCommandWithWorldAndArgs TvShotCommand(
		TEXT("apexsim.tv.Shot"),
		TEXT("Hold the broadcast camera on one shot: grid, trackside, helicopter, tracking, chase, onboard, nose, reverse; none (or no argument) cuts freely again."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
			{
				AApexRaceDirector* Director = FindDirectorForCommand(World);
				if (!Director)
				{
					return;
				}
				ApexTv::EShot Wanted = ApexTv::EShot::None;
				if (Args.Num() > 0)
				{
					for (int32 Index = 1; Index < static_cast<int32>(ApexTv::EShot::Count); ++Index)
					{
						if (Args[0].Equals(ApexTv::ShotName(static_cast<ApexTv::EShot>(Index)), ESearchCase::IgnoreCase))
						{
							Wanted = static_cast<ApexTv::EShot>(Index);
						}
					}
				}
				Director->GetTvDirector().ForceShot(Wanted);
				UE_LOG(LogApexSim, Log, TEXT("Broadcast camera: %s"),
					Wanted == ApexTv::EShot::None ? TEXT("cutting freely") : ApexTv::ShotName(Wanted));
			}));

	FAutoConsoleCommandWithWorldAndArgs TvCutCommand(
		TEXT("apexsim.tv.Cut"),
		TEXT("Make the broadcast camera cut now."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>&, UWorld* World)
			{
				if (AApexRaceDirector* Director = FindDirectorForCommand(World))
				{
					Director->GetTvDirector().RequestCut();
				}
			}));

	/**
	 * A prop: a tree, a stand or a bridge deck is not ground for a camera to
	 * stand on. The track import tags every prop actor `ApexProp`; the
	 * generated stand-ins are also named `SM_Prop_<kind>`.
	 */
	bool IsPropHit(const FHitResult& Hit)
	{
		static const FName PropTag(TEXT("ApexProp"));
		if (const AActor* Actor = Hit.GetActor(); Actor && Actor->ActorHasTag(PropTag))
		{
			return true;
		}
		const UStaticMeshComponent* Mesh = Cast<UStaticMeshComponent>(Hit.GetComponent());
		return Mesh && Mesh->GetStaticMesh() && Mesh->GetStaticMesh()->GetName().StartsWith(TEXT("SM_Prop_"));
	}

	FAutoConsoleCommandWithWorldAndArgs ChaseViewCommand(
		TEXT("apexsim.cam.Chase"),
		TEXT("Driving camera: a chase rung by index (0 closest) or name (roof, close, near, far), or \"cockpit\". No argument steps to the next, as C does."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
			{
				AApexRaceDirector* Director = FindDirectorForCommand(World);
				if (!Director)
				{
					return;
				}
				if (Args.Num() == 0)
				{
					Director->CycleView();
					return;
				}
				int32 Level = ApexChase::CockpitLevel;
				if (!ApexChase::FindByName(Args[0], Level))
				{
					if (!Args[0].IsNumeric())
					{
						UE_LOG(LogApexSim, Warning,
							TEXT("Usage: apexsim.cam.Chase [0-%d | roof | close | near | far | cockpit]"),
							ApexChase::Num() - 1);
						return;
					}
					Level = FCString::Atoi(*Args[0]);
				}
				if (Level == ApexChase::CockpitLevel)
				{
					Director->SetCockpitView(true);
				}
				else
				{
					Director->SetChaseLevel(Level);
				}
			}));

	FAutoConsoleCommandWithWorldAndArgs ShotFovCommand(
		TEXT("apexsim.cam.Fov"),
		TEXT("Horizontal field of view of the shot camera, degrees."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
			{
				TArray<double> Values;
				if (!ApexShotCamera::ParseNumberList(FString::Join(Args, TEXT(" ")), Values) || Values.Num() != 1)
				{
					UE_LOG(LogApexSim, Warning, TEXT("Usage: apexsim.cam.Fov Degrees"));
					return;
				}
				if (AApexRaceDirector* Director = FindDirectorForCommand(World))
				{
					Director->SetShotCameraFov(static_cast<float>(Values[0]));
				}
			}));
}

AApexRaceDirector* AApexRaceDirector::Find(const UObject* WorldContextObject)
{
	const UWorld* World = GEngine
		? GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull)
		: nullptr;
	if (!World)
	{
		return nullptr;
	}
	for (TActorIterator<AApexRaceDirector> It(const_cast<UWorld*>(World)); It; ++It)
	{
		return *It;
	}
	return nullptr;
}

UApexNetSubsystem* AApexRaceDirector::GetNet() const
{
	const UGameInstance* GameInstance = GetGameInstance();
	return GameInstance ? GameInstance->GetSubsystem<UApexNetSubsystem>() : nullptr;
}

UApexSettingsSubsystem* AApexRaceDirector::GetSettings() const
{
	const UGameInstance* GameInstance = GetGameInstance();
	return GameInstance ? GameInstance->GetSubsystem<UApexSettingsSubsystem>() : nullptr;
}

UApexMenuFlowSubsystem* AApexRaceDirector::GetFlow() const
{
	const UGameInstance* GameInstance = GetGameInstance();
	return GameInstance ? GameInstance->GetSubsystem<UApexMenuFlowSubsystem>() : nullptr;
}

void AApexRaceDirector::BeginPlay()
{
	Super::BeginPlay();

	if (UApexNetSubsystem* Net = GetNet())
	{
		Net->OnSessionRosterUpdated.AddDynamic(this, &AApexRaceDirector::HandleRosterUpdated);
		Net->OnTelemetry.AddDynamic(this, &AApexRaceDirector::HandleTelemetry);
		Net->OnSessionLeft.AddDynamic(this, &AApexRaceDirector::HandleSessionLeft);
		Net->OnLobbyStateUpdated.AddDynamic(this, &AApexRaceDirector::HandleLobbyStateUpdated);
		Net->OnRacingLineUpdated.AddDynamic(this, &AApexRaceDirector::HandleRacingLineUpdated);
		Net->OnGhostLap.AddDynamic(this, &AApexRaceDirector::HandleGhostLap);
	}
}

void AApexRaceDirector::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (UApexNetSubsystem* Net = GetNet())
	{
		Net->OnSessionRosterUpdated.RemoveDynamic(this, &AApexRaceDirector::HandleRosterUpdated);
		Net->OnTelemetry.RemoveDynamic(this, &AApexRaceDirector::HandleTelemetry);
		Net->OnSessionLeft.RemoveDynamic(this, &AApexRaceDirector::HandleSessionLeft);
		Net->OnLobbyStateUpdated.RemoveDynamic(this, &AApexRaceDirector::HandleLobbyStateUpdated);
		Net->OnRacingLineUpdated.RemoveDynamic(this, &AApexRaceDirector::HandleRacingLineUpdated);
	}
	DestroyAllCars();
	Super::EndPlay(EndPlayReason);
}

void AApexRaceDirector::HandleRosterUpdated(const FApexSessionRoster& Roster)
{
	SyncCarsToRoster(Roster);
	UpdateCameraTarget();
}

void AApexRaceDirector::HandleSessionLeft()
{
	EndRaceView();
}

void AApexRaceDirector::HandleLobbyStateUpdated(const FApexLobbyState& LobbyState)
{
	// A race that started before the lobby cache knew about its session resolves
	// no track path in BeginRaceView; the next lobby snapshot is what makes the
	// session — and its track file — findable, so try again here.
	if (bRaceViewActive && !TrackLevel)
	{
		LoadTrackLevel();
	}
}

void AApexRaceDirector::HandleRacingLineUpdated(const FApexRacingLineData& Line)
{
	// Normally the line lands between SessionJoined and the race view, and
	// BeginRaceView picks it up from the cache; this is for one that arrives
	// (or is withdrawn) mid-race.
	if (RacingLine)
	{
		RacingLine->SetLine(Line);
	}
}

void AApexRaceDirector::EnsureRacingLine()
{
	UWorld* World = GetWorld();
	if (RacingLine || !World)
	{
		return;
	}
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	RacingLine = World->SpawnActor<AApexRacingLineActor>(
		AApexRacingLineActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, Params);
	if (RacingLine)
	{
		if (const UApexNetSubsystem* Net = GetNet())
		{
			RacingLine->SetLine(Net->GetRacingLine());
		}
		RacingLine->SetWet(Sky.bWetRoad);
		ApplyRacingLineSetting();
	}
}

void AApexRaceDirector::DestroyRacingLine()
{
	if (RacingLine)
	{
		RacingLine->Destroy();
		RacingLine = nullptr;
	}
}

void AApexRaceDirector::SnapRacingLineToTrack()
{
	// Like the start lights: the level's actors, and their collision, only
	// exist once it is visible, not merely loaded.
	if (RacingLine && RacingLine->HasLine() && !RacingLine->IsOnGround() && IsTrackLevelLoaded()
		&& TrackLevel->IsLevelVisible())
	{
		RacingLine->SnapToGround(TrackLevel->GetLoadedLevel());
	}
}

void AApexRaceDirector::ApplyRacingLineSetting()
{
	if (!RacingLine)
	{
		return;
	}
	const UApexSettingsSave* Values = GetSettings() ? GetSettings()->Get() : nullptr;
	EApexRacingLine Mode = Values ? Values->RacingLine : EApexRacingLine::Off;
	FString Requested;
	if (FParse::Value(FCommandLine::Get(), TEXT("ApexRacingLine="), Requested))
	{
		Mode = Requested.Equals(TEXT("full"), ESearchCase::IgnoreCase)       ? EApexRacingLine::Full
			: Requested.Equals(TEXT("braking"), ESearchCase::IgnoreCase) ? EApexRacingLine::BrakingOnly
			                                                                  : EApexRacingLine::Off;
	}
	RacingLine->SetMode(Mode);
}

AApexRaceCarActor* AApexRaceDirector::FindCar(int32 CarIndex) const
{
	const TObjectPtr<AApexRaceCarActor>* Found = Cars.Find(CarIndex);
	return Found ? Found->Get() : nullptr;
}

void AApexRaceDirector::SyncCarsToRoster(const FApexSessionRoster& Roster)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	const UApexMenuFlowSubsystem* Flow = GetFlow();

	TSet<int32> Wanted;
	for (const FApexRosterEntry& Entry : Roster.Entries)
	{
		Wanted.Add(Entry.CarIndex);

		AApexRaceCarActor* Car = FindCar(Entry.CarIndex);
		if (!Car)
		{
			FActorSpawnParameters Params;
			Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			Car = World->SpawnActor<AApexRaceCarActor>(
				AApexRaceCarActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, Params);
			if (!Car)
			{
				continue;
			}
			Car->SetCarIndex(Entry.CarIndex);
			Car->SetHeadlights(bRaceViewActive && Sky.bHeadlights);
			if (const UApexSettingsSave* Values = GetSettings() ? GetSettings()->Get() : nullptr)
			{
				Car->SetMixVolumes(Values->EngineVolume, Values->RoadVolume);
			}
			Cars.Add(Entry.CarIndex, Car);
			CarIdShown.Remove(Entry.CarIndex);
			VerifyLocalCarContent();
		}

		// Each car in its own mesh and cockpit. An older server sends no car
		// id: then everyone gets the local player's car, as before.
		FString CarId = Entry.CarConfigId;
		if (CarId.IsEmpty() && Flow && Flow->HasPendingCar())
		{
			CarId = Flow->GetPendingCarId();
		}
		const FString* Shown = CarIdShown.Find(Entry.CarIndex);
		if (!Shown || !Shown->Equals(CarId, ESearchCase::IgnoreCase))
		{
			CarIdShown.Add(Entry.CarIndex, CarId);
			ApplyCatalogMesh(Car, CarId);
		}

		Car->SetDisplayName(Entry.PlayerName);
		// Actor labels are an editor convenience and do not exist in a game
		// build; without the guard the shipping target does not compile.
#if WITH_EDITOR
		Car->SetActorLabel(FString::Printf(TEXT("Car%d_%s"), Entry.CarIndex, *Entry.PlayerName));
#endif
	}

	// Drop anyone who left.
	for (auto It = Cars.CreateIterator(); It; ++It)
	{
		if (!Wanted.Contains(It.Key()))
		{
			if (AApexRaceCarActor* Car = It.Value().Get())
			{
				Car->Destroy();
			}
			CarIdShown.Remove(It.Key());
			It.RemoveCurrent();
		}
	}

	if (bDemoView)
	{
		ApplyDemoWorldVisibility();
	}

	UE_LOG(LogApexSim, Log, TEXT("Race roster: %d car(s) spawned"), Cars.Num());
}

void AApexRaceDirector::ApplyCatalogMesh(AApexRaceCarActor* Car, const FString& CarId)
{
	if (!Car)
	{
		return;
	}
	const UApexMenuFlowSubsystem* Flow = GetFlow();
	TSoftObjectPtr<UStaticMesh> Mesh = DefaultCarMesh;
	// The fallback mesh has its wheels modelled in: none drawn on it.
	FApexWheelSpec Wheels;
	FApexCarCatalogRow Row;
	if (Flow && !CarId.IsEmpty() && Flow->GetCarCatalogRow(CarId, Row))
	{
		if (!Row.Mesh.IsNull())
		{
			Mesh = Row.Mesh;
			Wheels = Row.Wheels;
		}
		Car->SetCockpitSpec(Row.CarClass, Row.Cockpit);
		Car->SetEngineSound(Row.EngineSound, Row.CarClass);
	}
	else
	{
		UE_LOG(LogApexSim, Warning, TEXT("Race roster: no catalog row for car %s (car %d), drawing the fallback"),
			*CarId, Car->GetCarIndex());
		Car->SetCockpitSpec(FString(), FApexCockpitOverrides());
		Car->SetEngineSound(FApexEngineSoundSpec(), FString());
	}
	Car->SetCarMesh(Mesh);
	Car->SetWheels(Wheels);
}

void AApexRaceDirector::HandleTelemetry(const FApexTelemetryFrame& Frame)
{
	if (!bLoggedFirstTelemetry && Frame.Cars.Num() > 0)
	{
		bLoggedFirstTelemetry = true;
		const FApexCarTelemetry& First = Frame.Cars[0];
		UE_LOG(LogApexSim, Log,
			TEXT("First telemetry frame: tick=%lld cars=%d car0 pos=(%.1f, %.1f, %.1f)m speed=%.1f km/h"),
			Frame.ServerTick, Frame.Cars.Num(),
			First.Position.X, First.Position.Y, First.Position.Z,
			ApexRace::MpsToKph(First.SpeedMps));
	}

	const EApexSessionState PreviousState = LatestFrameState;
	LatestFrameState = Frame.SessionState;
	const UApexNetSubsystem* Net = GetNet();
	const int32 LocalIndex = Net ? Net->GetLocalCarIndex() : -1;
	for (const FApexCarTelemetry& Car : Frame.Cars)
	{
		if (AApexRaceCarActor* Actor = FindCar(Car.CarIndex))
		{
			Actor->ApplyTelemetry(Car, Frame.ServerTick);
			// Someone else's car parked in its hotlap garage is out of the
			// way on the server and not drawn here; the player's own stays,
			// it is what the garage view looks at.
			if (Car.CarIndex != LocalIndex)
			{
				Actor->SetActorHiddenInGame(Car.bInGarage);
			}
		}
		if (Car.CarIndex == LocalIndex)
		{
			bLocalInGarage = Car.bInGarage;
			LocalLap = Car.CurrentLap;
			LocalLapTimeMs = Car.CurrentLapTimeMs;
		}
		FCarProgress& Progress = CarProgress.FindOrAdd(Car.CarIndex);
		Progress.Lap = Car.CurrentLap;
		Progress.StationM = Car.TrackProgress;
		Progress.bOnTrack = Car.bIsOnTrack;
	}

	UpdateStartLights(Frame);
	UpdateRaceBleeps(Frame, PreviousState);
	if (Rig)
	{
		Rig->SetCountdownMs(Frame.SessionState == EApexSessionState::Countdown ? Frame.CountdownMs : -1);
	}
}

void AApexRaceDirector::FindStartLights()
{
	// A streamed level reports loaded before its actors are in the world;
	// they arrive when it becomes visible, so search only from then on.
	if (bSearchedStartLights || !IsTrackLevelLoaded() || !TrackLevel->IsLevelVisible())
	{
		return;
	}
	bSearchedStartLights = true;
	StartLightLenses.Reset();

	static const FName GantryTag(TEXT("ApexStartLights"));
	static const FName LensTag(TEXT("ApexStartLight"));
	TArray<AActor*> Found;
	UGameplayStatics::GetAllActorsWithTag(this, GantryTag, Found);
	if (Found.Num() == 0)
	{
		UE_LOG(LogApexSim, Log, TEXT("Track level has no start-light gantry"));
		return;
	}

	TArray<UStaticMeshComponent*> Lenses;
	Found[0]->GetComponents<UStaticMeshComponent>(Lenses);
	Lenses.RemoveAll([](const UStaticMeshComponent* C) { return !C->ComponentHasTag(LensTag); });
	// Light0..Light4, left to right from the grid: name order is the row order.
	Lenses.Sort([](const UStaticMeshComponent& A, const UStaticMeshComponent& B) {
		return A.GetName() < B.GetName();
	});
	for (UStaticMeshComponent* Lens : Lenses)
	{
		if (UMaterialInstanceDynamic* Mid = Lens->CreateAndSetMaterialInstanceDynamic(0))
		{
			StartLightLenses.Add(Mid);
		}
	}
	LitStartLights = -1;
	UE_LOG(LogApexSim, Log, TEXT("Start-light gantry found with %d lights"), StartLightLenses.Num());
}

void AApexRaceDirector::ForgetStartLights()
{
	StartLightLenses.Reset();
	LitStartLights = -1;
	bSearchedStartLights = false;
}

void AApexRaceDirector::UpdateStartLights(const FApexTelemetryFrame& Frame)
{
	FindStartLights();
	if (StartLightLenses.Num() == 0)
	{
		return;
	}

	// Formula 1 procedure: the five lights come on one per second, and the
	// race starts when they all go out. The server counts down in
	// milliseconds and switches to Racing at zero, so "all out" is simply
	// any frame that is not a countdown.
	int32 Lit = 0;
	if (Frame.SessionState == EApexSessionState::Countdown && Frame.CountdownMs >= 0)
	{
		const int32 WholeSecondsLeft = Frame.CountdownMs / 1000;
		Lit = FMath::Clamp(StartLightLenses.Num() - WholeSecondsLeft, 0, StartLightLenses.Num());
	}
	if (Lit == LitStartLights)
	{
		return;
	}
	LitStartLights = Lit;

	static const FName EmissiveParam(TEXT("EmissiveStrength"));
	for (int32 Index = 0; Index < StartLightLenses.Num(); ++Index)
	{
		if (UMaterialInstanceDynamic* Mid = StartLightLenses[Index])
		{
			Mid->SetScalarParameterValue(EmissiveParam, Index < Lit ? StartLightOnEmissive : 0.0f);
		}
	}
}

void AApexRaceDirector::UpdateRaceBleeps(const FApexTelemetryFrame& Frame, EApexSessionState PreviousState)
{
	if (bDemoView)
	{
		return;
	}

	// The countdown: one tick per light, so only the last five seconds of a
	// longer count beep, and the frame that leaves the countdown for a race
	// is the go. A frame joined mid-count ticks for the second it lands in.
	if (Frame.SessionState == EApexSessionState::Countdown && Frame.CountdownMs >= 0)
	{
		const int32 WholeSecondsLeft = Frame.CountdownMs / 1000;
		if (WholeSecondsLeft != BleepCountdownSecond && WholeSecondsLeft < 5)
		{
			ApexUiAudio::Play(this, EApexUiSound::CountdownTick);
		}
		BleepCountdownSecond = WholeSecondsLeft;
	}
	else
	{
		if (PreviousState == EApexSessionState::Countdown && Frame.SessionState == EApexSessionState::Racing)
		{
			ApexUiAudio::Play(this, EApexUiSound::CountdownGo);
		}
		BleepCountdownSecond = -1;
	}

	// The line: the server bumps a car's lap as it crosses. Lap 0 -> 1 is a
	// grid car rolling over the line just after the go, which already beeped,
	// so only a completed lap counts.
	const UApexNetSubsystem* Net = GetNet();
	const int32 LocalIndex = Net ? Net->GetLocalCarIndex() : -1;
	if (LocalIndex != BleepCarIndex)
	{
		BleepCarIndex = LocalIndex;
		BleepLap = -1;
	}
	if (LocalIndex < 0)
	{
		return;
	}
	for (const FApexCarTelemetry& Car : Frame.Cars)
	{
		if (Car.CarIndex != LocalIndex)
		{
			continue;
		}
		// The last car's line crossing can be the frame that finishes the session.
		const bool bRaceRunning = Frame.SessionState == EApexSessionState::Racing || PreviousState == EApexSessionState::Racing;
		if (BleepLap >= 1 && Car.CurrentLap > BleepLap && bRaceRunning)
		{
			ApexUiAudio::Play(this, EApexUiSound::LapLine);
		}
		BleepLap = Car.CurrentLap;
		break;
	}
}

void AApexRaceDirector::UpdateCameraTarget()
{
	const UApexNetSubsystem* Net = GetNet();
	const int32 LocalIndex = Net ? Net->GetLocalCarIndex() : -1;

	// The broadcast camera decides who is on screen; the driving cameras ride
	// the player's car — or the ghost, while its lap is being replayed.
	AApexRaceCarActor* Target = bTvView ? FindCar(Tv.GetTargetCarIndex()) : FindCar(LocalIndex);
	if (bGhostReplay && Ghost)
	{
		Target = Ghost;
	}
	if (!Target && bTvView)
	{
		Target = FindCar(LocalIndex);
	}
	if (!Target)
	{
		// Spectating, or the roster has no entry for us yet: follow whatever
		// car exists so the view is never pointed at nothing.
		for (const TPair<int32, TObjectPtr<AApexRaceCarActor>>& Pair : Cars)
		{
			if (Pair.Value)
			{
				Target = Pair.Value;
				break;
			}
		}
	}

	if (FollowedCar != Target)
	{
		// Whoever we were watching gets their bodywork back before we climb
		// into someone else's cockpit.
		if (FollowedCar)
		{
			FollowedCar->SetMeshVisible(true);
			RemoveTickPrerequisiteActor(FollowedCar);
		}
		FollowedCar = Target;
		if (FollowedCar)
		{
			// Same frame, after the car: see the tick group note in the constructor.
			AddTickPrerequisiteActor(FollowedCar);
		}
		// A new car is a new frame of reference for the inertia estimate.
		bHavePrevMotion = false;
		HeadOffset = FVector::ZeroVector;
		LateralG = 0.0f;
		LongitudinalG = 0.0f;
		if (Rig)
		{
			Rig->AttachToCar(FollowedCar);
			Rig->SetEyeLocal(CockpitEyeLocal());
		}
		if (bRaceViewActive)
		{
			ApplyCameraMode();
		}
	}
}

void AApexRaceDirector::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (!bRaceViewActive)
	{
		return;
	}

	if (!FollowedCar)
	{
		UpdateCameraTarget();
	}

	// The boom is on this actor rather than parented to the car, so the director
	// follows the car's smoothed transform. That keeps the camera lag working
	// off one source of motion instead of compounding two.
	if (FollowedCar)
	{
		SetActorLocation(FollowedCar->GetActorLocation());
		SetActorRotation(FRotator(0.0f, FollowedCar->GetActorRotation().Yaw, 0.0f));
	}

	SnapRacingLineToTrack();
	ApplyTrackLevelConditions();
	UpdateCameraFeel(DeltaSeconds);
	if (bTvView)
	{
		UpdateTvCamera(DeltaSeconds);
	}
	if (bDemoView)
	{
		// The menu owns input; nothing here drives, looks or swaps cameras.
		UpdateDemoOpacity(DeltaSeconds);
		return;
	}
	UpdateHeadMotion(DeltaSeconds);
	UpdateLook(DeltaSeconds);
	UpdateCockpitCamera();
	UpdateCarAudio();
	if (bGhostReplay)
	{
		UpdateGhostReplay(DeltaSeconds);
	}
	else
	{
		UpdateGhost(DeltaSeconds);
	}
	PollViewInput();
	PollDrivingInput();
}

// --- Ghost and replay --------------------------------------------------------------

void AApexRaceDirector::HandleGhostLap(const FApexGhostLap& Lap)
{
	if (Ghost)
	{
		Ghost->SetLap(Lap);
		bGhostClockValid = false;
	}
	EnsureGhost();
}

void AApexRaceDirector::EnsureGhost()
{
	UWorld* World = GetWorld();
	const UApexNetSubsystem* Net = GetNet();
	const UApexMenuFlowSubsystem* Flow = GetFlow();
	if (!World || !Net || !bRaceViewActive || bDemoView || !Net->GetGhostLap().IsValid())
	{
		DestroyGhost();
		return;
	}
	if (Ghost)
	{
		return;
	}
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	Ghost = World->SpawnActor<AApexGhostCarActor>(
		AApexGhostCarActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, Params);
	if (!Ghost)
	{
		return;
	}
	Ghost->SetCarIndex(-1);
	Ghost->SetDisplayName(TEXT("Ghost"));
	// The record was set in the car the player is in: the ghost is that car.
	ApplyCatalogMesh(Ghost, Flow ? Flow->GetPendingCarId() : FString());
	Ghost->ApplyGhostLook();
	Ghost->SetLap(Net->GetGhostLap());
	bGhostClockValid = false;
	bGhostOverlapHidden = false;
#if WITH_EDITOR
	Ghost->SetActorLabel(TEXT("GhostCar"));
#endif
	UE_LOG(LogApexSim, Log, TEXT("Ghost car spawned for a %d ms lap"), Ghost->GetLapTimeMs());
}

void AApexRaceDirector::DestroyGhost()
{
	if (bGhostReplay)
	{
		EndGhostReplay();
	}
	if (Ghost)
	{
		Ghost->Destroy();
		Ghost = nullptr;
	}
	bGhostClockValid = false;
}

void AApexRaceDirector::UpdateGhost(float DeltaSeconds)
{
	const UApexNetSubsystem* Net = GetNet();
	if (!Ghost)
	{
		EnsureGhost();
		if (!Ghost)
		{
			return;
		}
	}
	const UApexSettingsSave* Values = GetSettings() ? GetSettings()->Get() : nullptr;
	const bool bWanted = Net && Net->GetGameMode() == EApexGameMode::Hotlap
		&& (!Values || Values->bGhostCar)
		&& !bLocalInGarage && LocalLap > 0;
	if (!bWanted)
	{
		Ghost->SetClockMs(-1.0f);
		bGhostClockValid = false;
		return;
	}

	// The lap time arrives in 60 Hz lumps; the ghost's clock runs on its own
	// and is trimmed toward it, snapping only on a new lap or a long stall.
	const float TargetMs = static_cast<float>(LocalLapTimeMs);
	if (!bGhostClockValid || FMath::Abs(TargetMs - GhostClockMs) > 500.0f)
	{
		GhostClockMs = TargetMs;
		bGhostClockValid = true;
	}
	else
	{
		GhostClockMs += DeltaSeconds * 1000.0f;
		GhostClockMs += (TargetMs - GhostClockMs) * FMath::Min(1.0f, DeltaSeconds * 4.0f);
	}
	Ghost->SetClockMs(GhostClockMs);

	// Never inside the player's own car: hidden close up, back once clear.
	const AApexRaceCarActor* Local = FindCar(Net->GetLocalCarIndex());
	if (Local && Ghost->IsGhostShown())
	{
		const float Distance = static_cast<float>(FVector::Dist(Local->GetActorLocation(), Ghost->GetActorLocation()));
		if (bGhostOverlapHidden ? Distance > GhostShowDistanceCm : Distance < GhostHideDistanceCm)
		{
			bGhostOverlapHidden = !bGhostOverlapHidden;
		}
	}
	else if (Local)
	{
		FApexGhostSample Sample;
		if (Ghost->GetCurrentSample(Sample))
		{
			const float Distance = static_cast<float>(
				FVector::Dist(Local->GetActorLocation(), ApexRace::ServerToUnrealPosition(Sample.Position)));
			bGhostOverlapHidden = Distance < GhostShowDistanceCm;
		}
	}
	Ghost->SetGhostVisible(!bGhostOverlapHidden);
}

int32 AApexRaceDirector::GetGhostLapTimeMs() const
{
	return Ghost ? Ghost->GetLapTimeMs() : 0;
}

bool AApexRaceDirector::BeginGhostReplay()
{
	EnsureGhost();
	if (!Ghost || !Ghost->HasLap() || !bRaceViewActive)
	{
		return false;
	}
	if (bGhostReplay)
	{
		return true;
	}
	bGhostReplay = true;
	ReplayClockMs = -ReplayLeadInSeconds * 1000.0f;
	ReplayShot = -1;
	ReplayShotSeconds = 0.0f;
	bReplayCockpitWas = bCockpitView;
	// The replay's chase shot is a camera on the car, not the player's own
	// distance: a roof cam would show none of the lap.
	ReplayChaseLevelWas = ChaseLevel;
	ChaseLevel = ApexChase::DefaultLevel();
	bGhostOverlapHidden = false;
	Ghost->SetGhostVisible(true);
	Ghost->SetClockMs(0.0f);
	// The camera moves onto the ghost; the boom's lag swings it there.
	UpdateCameraTarget();
	CutReplayShot();
	UE_LOG(LogApexSim, Log, TEXT("Ghost replay: %d ms lap"), Ghost->GetLapTimeMs());
	return true;
}

void AApexRaceDirector::EndGhostReplay()
{
	if (!bGhostReplay)
	{
		return;
	}
	bGhostReplay = false;
	bShotCameraPose = false;
	bCockpitView = bReplayCockpitWas;
	ChaseLevel = ReplayChaseLevelWas;
	ShotCamera->SetFieldOfView(ReplayShotCameraRestFov);
	if (Ghost)
	{
		Ghost->SetClockMs(-1.0f);
		Ghost->SetMeshVisible(true);
	}
	bGhostClockValid = false;
	UpdateCameraTarget();
	ApplyCameraMode();
	UE_LOG(LogApexSim, Log, TEXT("Ghost replay ended"));
}

void AApexRaceDirector::CutReplayShot()
{
	// Chase, trackside, onboard, trackside, and round again: the two
	// trackside shots stand at different corners because the ghost has
	// moved on between them.
	ReplayShot = (ReplayShot + 1) % 4;
	ReplayShotSeconds = 0.0f;
	const bool bTrackside = ReplayShot == 1 || ReplayShot == 3;
	bShotCameraPose = bTrackside;
	bCockpitView = ReplayShot == 2;
	if (bTrackside && Ghost)
	{
		// Well ahead of the car, off to one side and a little up, on a long
		// lens, looking back at it: the ghost drives toward the camera for a
		// few seconds and past it. Aimed here as well as every frame, so the
		// cut never shows a frame of the old aim.
		const FVector Forward = Ghost->GetActorForwardVector();
		const FVector Right = Ghost->GetActorRightVector();
		ReplayCameraLocation = Ghost->GetActorLocation() + Forward * ReplayTracksideAheadCm + Right * 1200.0f + FVector(0.0f, 0.0f, 320.0f);
		ShotCamera->SetWorldLocationAndRotation(ReplayCameraLocation, (Ghost->GetActorLocation() - ReplayCameraLocation).Rotation());
		ShotCamera->SetFieldOfView(ReplayTracksideFov);
	}
	else
	{
		ShotCamera->SetFieldOfView(ReplayShotCameraRestFov);
	}
	ApplyCameraMode();
	// The onboard shot sits where the driver does; the ghost's shell is
	// drawn around the camera otherwise, tint and all.
	if (Ghost)
	{
		Ghost->SetMeshVisible(!bCockpitView);
	}
	UE_LOG(LogApexSim, Verbose, TEXT("Replay shot %d (%s) at %.1f s: ghost (%.0f, %.0f, %.0f) camera (%.0f, %.0f, %.0f)"),
		ReplayShot, bTrackside ? TEXT("trackside") : (bCockpitView ? TEXT("onboard") : TEXT("chase")), ReplayClockMs / 1000.0f,
		Ghost ? Ghost->GetActorLocation().X : 0.0, Ghost ? Ghost->GetActorLocation().Y : 0.0, Ghost ? Ghost->GetActorLocation().Z : 0.0,
		bTrackside ? ReplayCameraLocation.X : 0.0, bTrackside ? ReplayCameraLocation.Y : 0.0, bTrackside ? ReplayCameraLocation.Z : 0.0);
}

void AApexRaceDirector::UpdateGhostReplay(float DeltaSeconds)
{
	if (!Ghost || !Ghost->HasLap())
	{
		EndGhostReplay();
		return;
	}
	ReplayClockMs += DeltaSeconds * 1000.0f;
	if (ReplayClockMs > Ghost->GetLapTimeMs() + 1000.0f)
	{
		EndGhostReplay();
		return;
	}
	// The lead-in holds the ghost at the line.
	Ghost->SetGhostVisible(true);
	Ghost->SetClockMs(FMath::Max(0.0f, ReplayClockMs));

	ReplayShotSeconds += DeltaSeconds;
	const bool bTrackside = ReplayShot == 1 || ReplayShot == 3;
	if (bTrackside)
	{
		const FVector ToGhost = Ghost->GetActorLocation() - ReplayCameraLocation;
		ShotCamera->SetWorldRotation(ToGhost.Rotation());
		// Cut once the car has gone well past the lens, or if it never comes
		// (a trackside shot placed off the road into a bend).
		const bool bPassed = FVector::DotProduct(ToGhost, Ghost->GetActorForwardVector()) > 4000.0f;
		if ((bPassed && ReplayShotSeconds > 3.0f) || ReplayShotSeconds > 10.0f)
		{
			CutReplayShot();
		}
	}
	else if (ReplayShotSeconds > 6.0f)
	{
		CutReplayShot();
	}
}

void AApexRaceDirector::UpdateHeadMotion(float DeltaSeconds)
{
	const UApexSettingsSave* Values = GetSettings() ? GetSettings()->Get() : nullptr;
	const float Amount = Values ? Values->HeadMotion : 0.0f;
	if (!FollowedCar || DeltaSeconds <= KINDA_SMALL_NUMBER)
	{
		return;
	}

	// Both from the actor's eased transform rather than the wire: telemetry
	// speed steps at the broadcast rate, and a rate taken from a step is a
	// spike on the frame it lands and nothing on the frames between.
	const FVector Location = FollowedCar->GetActorLocation();
	const float Yaw = FollowedCar->GetActorRotation().Yaw;
	const float Speed = bHavePrevMotion
		? static_cast<float>(FVector::Dist(Location, PrevLocation) / ApexRace::MetresToCentimetres) / DeltaSeconds
		: FollowedCar->GetSpeedMps();
	if (bHavePrevMotion)
	{
		const float RawLongG = (Speed - PrevSpeedMps) / DeltaSeconds / 9.81f;
		const float YawRate = FMath::DegreesToRadians(FRotator::NormalizeAxis(Yaw - PrevYawDeg)) / DeltaSeconds;
		// Unreal yaw grows clockwise from above, so a positive rate is a
		// right turn: acceleration to the right, +Y.
		const float RawLatG = Speed * YawRate / 9.81f;
		LongitudinalG = FMath::FInterpTo(LongitudinalG, FMath::Clamp(RawLongG, -4.0f, 4.0f), DeltaSeconds, 5.0f);
		LateralG = FMath::FInterpTo(LateralG, FMath::Clamp(RawLatG, -4.0f, 4.0f), DeltaSeconds, 5.0f);
	}
	PrevLocation = Location;
	PrevSpeedMps = Speed;
	PrevYawDeg = Yaw;
	bHavePrevMotion = true;

	const FVector Target = ApexCockpit::HeadLean(LateralG, LongitudinalG, Amount);
	HeadOffset = FMath::VInterpTo(HeadOffset, Target, DeltaSeconds, 6.0f);
}

void AApexRaceDirector::UpdateLook(float DeltaSeconds)
{
	const UApexSettingsSave* Values = GetSettings() ? GetSettings()->Get() : nullptr;
	const AApexPlayerController* PlayerController =
		Cast<AApexPlayerController>(UGameplayStatics::GetPlayerController(this, 0));

	float Target = 0.0f;
	bool bLookBack = false;
	if (PlayerController)
	{
		const FApexDriveInput& Drive = PlayerController->GetDriveInput();
		// Straight behind wins over the axis: a stick held sideways while the
		// button is pressed should not settle on a quarter turn.
		bLookBack = Drive.bLookBack;
		Target = bLookBack ? 180.0f : Drive.Look * 90.0f;
	}
	if (FollowedCar && Values && !bLookBack)
	{
		Target += ApexCockpit::ApexLookYawDeg(FollowedCar->GetSteering(), Values->LookToApex);
	}

	// Quick enough to feel like a glance, slow enough that a tap on the key
	// does not cut straight to the side window.
	LookYawDeg = FMath::FInterpTo(LookYawDeg, Target, DeltaSeconds, 10.0f);

	// The chase camera turns with the head too, on its boom, which keeps
	// the rotation lag and levelled pitch it already has. The pitch is the
	// rung's: the closer the camera, the less it looks down at the car.
	CameraBoom->SetRelativeRotation(FRotator(ApexChase::Get(ChaseLevel).PitchDeg, LookYawDeg, 0.0f));
}

void AApexRaceDirector::UpdateCameraFeel(float DeltaSeconds)
{
	if (!FollowedCar)
	{
		return;
	}
	const float SpeedFrac = FMath::Clamp(GetFollowedSpeedKph() / FeelTopSpeedKph, 0.0f, 1.0f);
	// Squared, so the effects live in the top half of the speed range: FOV
	// creeping open at parking speed just looks broken.
	const float Intensity = SpeedFrac * SpeedFrac;

	CurrentFovBoost =
		FMath::FInterpTo(CurrentFovBoost, SpeedFovBoostDeg * Intensity, DeltaSeconds, 3.0f);
	CockpitCamera->SetFieldOfView(
		FMath::Clamp(BaseCockpitFov + 0.6f * CurrentFovBoost, 50.0f, 130.0f));
	ChaseCamera->SetFieldOfView(
		FMath::Clamp(BaseChaseFov + ApexChase::Get(ChaseLevel).FovDeltaDeg + CurrentFovBoost, 50.0f, 130.0f));

	// Micro-shake from layered perlin noise, tuned to read as airflow and
	// road texture: fractions of a degree in the cockpit, a few centimeters
	// of translation on the chase camera. The frequencies are co-prime-ish
	// so the layers never visibly sync up.
	const float Time = static_cast<float>(GetWorld()->GetTimeSeconds());
	auto Wobble = [Time](float Frequency, float Offset) {
		return FMath::PerlinNoise1D(Time * Frequency + Offset);
	};
	CockpitShake = FRotator(Wobble(11.3f, 0.0f) * 0.25f * Intensity,
		Wobble(8.7f, 40.0f) * 0.15f * Intensity, Wobble(9.1f, 80.0f) * 0.35f * Intensity);
	ChaseCamera->SetRelativeLocation(FVector(0.0f, Wobble(7.9f, 120.0f) * 4.0f * Intensity,
		Wobble(10.7f, 160.0f) * 3.0f * Intensity));
}

void AApexRaceDirector::ApplyRaceEnvironment()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// The session's sky, the demo's included (it rolls its own). A server
	// from before conditions is a sunny early afternoon.
	const UApexNetSubsystem* Net = GetNet();
	const FApexSessionConditions Conditions = Net ? Net->GetSessionConditions() : FApexSessionConditions();
	Sky = ApexSky::Derive(Conditions);
	UE_LOG(LogApexSim, Log, TEXT("Race sky: %s, sun %.1f deg at %.0f deg, %s %.0f lux, exposure %.1f..%.1f EV, rain %.2f, headlights %d, floodlights %d"),
		*Conditions.Describe(), Sky.Sun.ElevationDeg, Sky.Sun.AzimuthDeg, Sky.bMoon ? TEXT("moon") : TEXT("sun"),
		Sky.LightIntensity, Sky.ExposureMinEv, Sky.ExposureMaxEv, Sky.RainIntensity, Sky.bHeadlights, Sky.bFloodlights);

	// The menu world's sun points wherever the menu looked good, at the
	// engine's default 10 lux — which is why race scenes used to render as
	// low-contrast pastel: sun and ambient were the same order of magnitude,
	// and auto-exposure normalized whatever the albedo said. For driving it
	// becomes an actual sun: tens of thousands of lux where the clock puts
	// it, or the moon after dark, and movable because nothing in a streamed
	// track level has baked lighting.
	for (TActorIterator<ADirectionalLight> It(World); It; ++It)
	{
		ADirectionalLight* Sun = *It;
		ULightComponent* SunComponent = Sun->GetLightComponent();
		if (!bMenuSunSaved)
		{
			MenuSun = Sun;
			MenuSunRotation = Sun->GetActorRotation();
			MenuSunColor = SunComponent->GetLightColor();
			MenuSunIntensity = SunComponent->Intensity;
			bMenuSunSaved = true;
		}
		SunComponent->SetMobility(EComponentMobility::Movable);
		Sun->SetActorRotation(Sky.LightRotation);
		SunComponent->SetLightColor(Sky.LightColor);
		SunComponent->SetIntensity(Sky.LightIntensity);
		SunComponent->SetCastShadows(Sky.bCastShadows);
		if (UDirectionalLightComponent* Directional =
				Cast<UDirectionalLightComponent>(SunComponent))
		{
			// The atmosphere (and through it the real-time sky light) has to
			// be driven by this sun, or the ground gets daylight while the
			// sky keeps its 10-lux dusk. The moon is not a sun: with it the
			// atmosphere goes dark, which is the night.
			Directional->bAtmosphereSunLight = Sky.bAtmosphereLight;
			Directional->LightSourceAngle = Sky.LightSourceAngleDeg;
			Directional->MarkRenderStateDirty();
		}
		break;
	}

	// The sky light's one static capture was taken in the menu void; put it
	// on real-time capture so ambient and reflections follow the actual sky
	// over the actual circuit. Under cloud the sky is the lamp, so its
	// capture is scaled up to what an overcast day actually delivers.
	for (TActorIterator<ASkyLight> It(World); It; ++It)
	{
		USkyLightComponent* SkyComponent = (*It)->GetLightComponent();
		SkyComponent->SetMobility(EComponentMobility::Movable);
		SkyComponent->SetRealTimeCaptureEnabled(true);
		SkyComponent->SetIntensity(Sky.SkyLightIntensity);
		break;
	}

	// Exposure and grading, over the track level's own volume: its daylight
	// clamp (10..15 EV) would render a night race black.
	if (!SkyPostProcess)
	{
		FActorSpawnParameters Params;
		Params.Name = MakeUniqueObjectName(World, APostProcessVolume::StaticClass(), FName(TEXT("ApexSkyPostProcess")));
		SkyPostProcess = World->SpawnActor<APostProcessVolume>(FVector::ZeroVector, FRotator::ZeroRotator, Params);
	}
	if (SkyPostProcess)
	{
		SkyPostProcess->bUnbound = true;
		SkyPostProcess->Priority = 10.0f;
		SkyPostProcess->bEnabled = true;
		FPostProcessSettings& Settings = SkyPostProcess->Settings;
		Settings.bOverride_AutoExposureMinBrightness = true;
		Settings.AutoExposureMinBrightness = Sky.ExposureMinEv;
		Settings.bOverride_AutoExposureMaxBrightness = true;
		Settings.AutoExposureMaxBrightness = Sky.ExposureMaxEv;
		Settings.bOverride_AutoExposureBias = true;
		Settings.AutoExposureBias = Sky.ExposureBias;
		Settings.bOverride_ColorSaturation = true;
		Settings.ColorSaturation = FVector4(Sky.Saturation, Sky.Saturation, Sky.Saturation, 1.0f);
		Settings.bOverride_ColorContrast = true;
		Settings.ColorContrast = FVector4(Sky.Contrast, Sky.Contrast, Sky.Contrast, 1.0f);
		// Wet air blooms the lights; a night's few lights bloom most.
		Settings.bOverride_BloomIntensity = true;
		Settings.BloomIntensity = Sky.RainIntensity > 0.0f ? 0.55f : (Sky.bMoon ? 0.45f : 0.3f);
	}

	if (Sky.RainIntensity > 0.0f && !Rain)
	{
		Rain = World->SpawnActor<AApexRainActor>(AApexRainActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator);
	}
	if (Rain)
	{
		Rain->SetIntensity(Sky.RainIntensity);
	}
	if (RacingLine)
	{
		RacingLine->SetWet(Sky.bWetRoad);
	}

	for (const TPair<int32, TObjectPtr<AApexRaceCarActor>>& Pair : Cars)
	{
		if (Pair.Value)
		{
			Pair.Value->SetHeadlights(Sky.bHeadlights);
		}
	}

	// The fog and the masts come with the level, once it is visible.
	ForgetTrackLevelConditions();
}

void AApexRaceDirector::RestoreMenuEnvironment()
{
	if (ADirectionalLight* Sun = MenuSun.Get(); Sun && bMenuSunSaved)
	{
		Sun->SetActorRotation(MenuSunRotation);
		Sun->GetLightComponent()->SetLightColor(MenuSunColor);
		Sun->GetLightComponent()->SetIntensity(MenuSunIntensity);
		Sun->GetLightComponent()->SetCastShadows(true);
		if (UDirectionalLightComponent* Directional = Cast<UDirectionalLightComponent>(Sun->GetLightComponent()))
		{
			Directional->bAtmosphereSunLight = true;
			Directional->LightSourceAngle = 0.5357f;
			Directional->MarkRenderStateDirty();
		}
	}
	// The sky light stays on real-time capture: it is simply correct, in the
	// menu as much as in the race.
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<ASkyLight> It(World); It; ++It)
		{
			(*It)->GetLightComponent()->SetIntensity(1.0f);
			break;
		}
	}
	if (SkyPostProcess)
	{
		SkyPostProcess->bEnabled = false;
	}
	if (Rain)
	{
		Rain->SetIntensity(0.0f);
	}
	for (const TPair<int32, TObjectPtr<AApexRaceCarActor>>& Pair : Cars)
	{
		if (Pair.Value)
		{
			Pair.Value->SetHeadlights(false);
		}
	}
	Sky = ApexSky::FSkyState();
	ForgetTrackLevelConditions();
}

void AApexRaceDirector::ForgetTrackLevelConditions()
{
	bTrackConditionsApplied = false;
	if (FloodlightRig)
	{
		FloodlightRig->Destroy();
		FloodlightRig = nullptr;
	}
}

void AApexRaceDirector::ApplyTrackLevelConditions()
{
	// A streamed level reports loaded before its actors are in the world;
	// they arrive when it becomes visible, so only from then on.
	if (bTrackConditionsApplied || !bRaceViewActive || !IsTrackLevelLoaded() || !TrackLevel->IsLevelVisible())
	{
		return;
	}
	ULevel* Level = TrackLevel->GetLoadedLevel();
	if (!Level)
	{
		return;
	}
	bTrackConditionsApplied = true;

	static const FName RoughnessParam(TEXT("Roughness"));
	static const FName RoughnessNoiseParam(TEXT("RoughnessNoise"));
	static const FName EmissiveStrengthParam(TEXT("EmissiveStrength"));
	static const FName LampTag(TEXT("ApexEmissive_floodlight_lamp"));

	int32 WetSlots = 0;
	int32 Masts = 0;
	int32 Lamps = 0;
	UWorld* World = GetWorld();

	for (AActor* Actor : Level->Actors)
	{
		if (!Actor)
		{
			continue;
		}

		// The level's fog: the bake's thin haze, or the weather's.
		if (AExponentialHeightFog* FogActor = Cast<AExponentialHeightFog>(Actor))
		{
			UExponentialHeightFogComponent* Fog = FogActor->GetComponent();
			Fog->SetFogDensity(Sky.FogDensity);
			Fog->SetFogHeightFalloff(Sky.FogHeightFalloff);
			Fog->SetStartDistance(Sky.FogStartDistanceCm);
			Fog->SetFogInscatteringColor(Sky.FogColor);
			continue;
		}

		TArray<UStaticMeshComponent*> Meshes;
		Actor->GetComponents<UStaticMeshComponent>(Meshes);
		for (UStaticMeshComponent* Mesh : Meshes)
		{
			// Wet road: the tarmac's sheen comes up on every material of the
			// bake's road family: the road and pit lane, and the rubbered
			// wear bands the racing line runs on (`wear_core`, `wear_edge`).
			if (Sky.bWetRoad)
			{
				const int32 Slots = Mesh->GetNumMaterials();
				for (int32 Slot = 0; Slot < Slots; ++Slot)
				{
					const UMaterialInterface* Material = Mesh->GetMaterial(Slot);
					if (!Material)
					{
						continue;
					}
					const FString Name = Material->GetName();
					if (Name.StartsWith(TEXT("MI_road")) || Name.StartsWith(TEXT("MI_pit_lane"))
						|| Name.StartsWith(TEXT("MI_wear_")))
					{
						if (UMaterialInstanceDynamic* Wet = Mesh->CreateDynamicMaterialInstance(Slot))
						{
							Wet->SetScalarParameterValue(RoughnessParam, Sky.RoadRoughness);
							Wet->SetScalarParameterValue(RoughnessNoiseParam, 0.08f);
							++WetSlots;
						}
					}
				}
			}

			// The floodlight lamps glow after dark.
			if (Sky.LampGlow > 0.0f && Mesh->ComponentHasTag(LampTag))
			{
				const int32 Slots = Mesh->GetNumMaterials();
				for (int32 Slot = 0; Slot < Slots; ++Slot)
				{
					const UMaterialInterface* Material = Mesh->GetMaterial(Slot);
					if (Material && Material->GetName().Contains(TEXT("glow_floodlight_lamp")))
					{
						if (UMaterialInstanceDynamic* Glow = Mesh->CreateDynamicMaterialInstance(Slot))
						{
							Glow->SetScalarParameterValue(EmissiveStrengthParam, Sky.LampGlow);
							++Lamps;
						}
					}
				}
			}

			// And the masts light the track: one spot per instance, from the
			// head of the mast down onto the road side (local +Y after the
			// builder's face-road flip).
			UInstancedStaticMeshComponent* Instanced = Cast<UInstancedStaticMeshComponent>(Mesh);
			if (!Sky.bFloodlights || !Instanced || !Instanced->GetStaticMesh())
			{
				continue;
			}
			const FString MeshName = Instanced->GetStaticMesh()->GetName();
			const bool bMast = MeshName.Contains(TEXT("floodlight_tower"));
			const bool bPost = MeshName.Contains(TEXT("lamp_post"));
			if (!bMast && !bPost)
			{
				continue;
			}
			if (!FloodlightRig && World)
			{
				FloodlightRig = World->SpawnActor<AActor>(AActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator);
				if (FloodlightRig)
				{
					USceneComponent* RigRoot = NewObject<USceneComponent>(FloodlightRig, TEXT("Root"));
					FloodlightRig->SetRootComponent(RigRoot);
					RigRoot->RegisterComponent();
				}
			}
			if (!FloodlightRig)
			{
				continue;
			}
			const int32 Count = Instanced->GetInstanceCount();
			for (int32 Index = 0; Index < Count && Masts < MaxFloodlights; ++Index)
			{
				FTransform Instance;
				if (!Instanced->GetInstanceTransform(Index, Instance, /*bWorldSpace*/ true))
				{
					continue;
				}
				const FVector Head = Instance.TransformPosition(bMast ? FVector(0.0f, 230.0f, 2800.0f) : FVector(0.0f, 110.0f, 750.0f));
				const FRotator Aim = (Instance.GetRotation() * FRotator(bMast ? -52.0f : -70.0f, 90.0f, 0.0f).Quaternion()).Rotator();
				USpotLightComponent* Lamp = NewObject<USpotLightComponent>(FloodlightRig);
				Lamp->SetMobility(EComponentMobility::Movable);
				Lamp->SetIntensityUnits(ELightUnits::Lumens);
				Lamp->SetIntensity(bMast ? 220000.0f : 12000.0f);
				Lamp->SetLightColor(FLinearColor(1.0f, 0.95f, 0.82f));
				Lamp->SetAttenuationRadius(bMast ? 16000.0f : 3500.0f);
				Lamp->SetInnerConeAngle(bMast ? 30.0f : 40.0f);
				Lamp->SetOuterConeAngle(bMast ? 52.0f : 60.0f);
				Lamp->SetCastShadows(false);
				Lamp->SetupAttachment(FloodlightRig->GetRootComponent());
				Lamp->SetWorldLocationAndRotation(Head, Aim);
				Lamp->RegisterComponent();
				++Masts;
			}
		}
	}

	UE_LOG(LogApexSim, Log, TEXT("Track level conditions applied: fog %.4f, %d wet road slots, %d floodlights, %d lamp faces"),
		Sky.FogDensity, WetSlots, Masts, Lamps);
}

void AApexRaceDirector::PollDrivingInput()
{
	UApexNetSubsystem* Net = GetNet();
	AApexPlayerController* PlayerController =
		Cast<AApexPlayerController>(UGameplayStatics::GetPlayerController(this, 0));
	if (!Net || !PlayerController || !Net->IsUdpReady())
	{
		return;
	}

	const FApexDriveInput& Drive = PlayerController->GetDriveInput();

	FApexPlayerInput Input;
	Input.Throttle = Drive.Throttle;
	Input.Brake = Drive.Brake;
	// The one conversion: the action is in screen convention (+1 is right)
	// and the server's frame has positive steering to the left.
	Input.Steering = -Drive.Steer;

	// Gear is a request, not a state. -128 is the protocol's "leave it
	// alone", so a shift only goes out on the tick its key was pressed.
	Input.Gear = -128;
	const int32 GearDelta = PlayerController->ConsumeGearDelta();
	if (GearDelta != 0 && FollowedCar)
	{
		// Clamped to the range the server accepts: reverse through tenth.
		Input.Gear = FMath::Clamp(FollowedCar->GetGear() + GearDelta, -1, 10);
	}
	// The automatic gearbox runs on the server (see SetDriverAids): it knows
	// the car's redline and ratios, which the client never learns.

	Net->SetPlayerInput(Input);
}

void AApexRaceDirector::SetCockpitView(bool bCockpit)
{
	if (bCockpitView == bCockpit)
	{
		return;
	}
	bCockpitView = bCockpit;
	ApplyCameraMode();
	UE_LOG(LogApexSim, Log, TEXT("Camera: %s"), *DescribeView());
}

void AApexRaceDirector::SetChaseLevel(int32 Level)
{
	const int32 Clamped = FMath::Clamp(Level, 0, ApexChase::Num() - 1);
	if (ChaseLevel == Clamped && !bCockpitView)
	{
		return;
	}
	ChaseLevel = Clamped;
	bCockpitView = false;
	// Remembered for the next race, like the view a race opens in.
	if (UApexSettingsSubsystem* SettingsSubsystem = GetSettings())
	{
		SettingsSubsystem->SetChaseLevel(ChaseLevel);
	}
	ApplyCameraMode();
	UE_LOG(LogApexSim, Log, TEXT("Camera: %s"), *DescribeView());
}

void AApexRaceDirector::CycleView()
{
	const int32 Next = ApexChase::NextLevel(bCockpitView ? ApexChase::CockpitLevel : ChaseLevel);
	if (Next == ApexChase::CockpitLevel)
	{
		SetCockpitView(true);
	}
	else
	{
		SetChaseLevel(Next);
	}
}

FString AApexRaceDirector::DescribeView() const
{
	if (bTvView)
	{
		return TEXT("broadcast");
	}
	return bCockpitView
		? FString(TEXT("cockpit"))
		: FString::Printf(TEXT("chase %s (%.1f m)"), ApexChase::Get(ChaseLevel).Name,
			ApexChase::Get(ChaseLevel).ArmLengthCm / 100.0f);
}

void AApexRaceDirector::SetFieldOfView(float Degrees)
{
	// These are the resting values; `UpdateCameraFeel` widens both with
	// speed on top of them.
	BaseCockpitFov = FMath::Clamp(Degrees, 60.0f, 120.0f);
	// The chase view sits further back, where the same number reads much wider;
	// it keeps a fixed offset below the cockpit's rather than a separate row in
	// the settings screen.
	BaseChaseFov = FMath::Clamp(BaseCockpitFov - 15.0f, 50.0f, 120.0f);
	CockpitCamera->SetFieldOfView(BaseCockpitFov);
	ChaseCamera->SetFieldOfView(BaseChaseFov + ApexChase::Get(ChaseLevel).FovDeltaDeg);
}

void AApexRaceDirector::SetShotCameraPose(const FVector& LocationCm, const FRotator& Rotation)
{
	ShotCamera->SetWorldLocationAndRotation(LocationCm, Rotation);
	bShotCameraPose = true;
	ApplyCameraMode();
	LogShotPose(LocationCm, Rotation, ShotCamera->FieldOfView);
	if (!bRaceViewActive)
	{
		UE_LOG(LogApexSim, Log, TEXT("Shot camera: no race view yet; the pose applies when one begins"));
	}
}

void AApexRaceDirector::ReleaseShotCamera()
{
	if (!bShotCameraPose)
	{
		return;
	}
	bShotCameraPose = false;
	ApplyCameraMode();
	UE_LOG(LogApexSim, Log, TEXT("Shot camera released: %s view"), *DescribeView());
}

void AApexRaceDirector::SetShotCameraFov(float Degrees)
{
	ShotCamera->SetFieldOfView(FMath::Clamp(Degrees, 5.0f, 170.0f));
	UE_LOG(LogApexSim, Log, TEXT("Shot camera fov %.1f"), ShotCamera->FieldOfView);
}

void AApexRaceDirector::ApplyShotCameraCommandLine()
{
	FString Fov;
	if (FParse::Value(FCommandLine::Get(), TEXT("ApexCameraFov="), Fov))
	{
		SetShotCameraFov(FCString::Atof(*Fov));
	}

	// Not stopping on separators: the value is itself a comma list.
	FString Requested;
	const bool bLookAt = FParse::Value(FCommandLine::Get(), TEXT("ApexCameraLookAt="), Requested, false);
	if (!bLookAt && !FParse::Value(FCommandLine::Get(), TEXT("ApexCamera="), Requested, false))
	{
		return;
	}
	ApexShotCamera::FPose Pose;
	if (ParseShotPose(Requested, bLookAt, Pose))
	{
		SetShotCameraPose(Pose.LocationCm, Pose.Rotation);
	}
	else
	{
		UE_LOG(LogApexSim, Warning, TEXT("%s\"%s\" is not %s (server frame, comma separated)"),
			bLookAt ? TEXT("-ApexCameraLookAt=") : TEXT("-ApexCamera="), *Requested,
			bLookAt ? TEXT("X,Y,Z,TX,TY,TZ") : TEXT("X,Y,Z[,Yaw[,Pitch]]"));
	}
}

void AApexRaceDirector::ApplyChaseView()
{
	const ApexChase::FView& View = ApexChase::Get(ChaseLevel);
	CameraBoom->TargetArmLength = View.ArmLengthCm;
	// World space, added to the boom's origin: a plain lift off the car,
	// unturned by the boom's pitch, so the roof cam sits over the roof rather
	// than being swung backwards by it.
	CameraBoom->TargetOffset = FVector(0.0f, 0.0f, View.HeightCm);
	CameraBoom->CameraLagSpeed = View.LagSpeed;
	CameraBoom->CameraRotationLagSpeed = View.RotationLagSpeed;
	CameraBoom->CameraLagMaxDistance = View.LagMaxDistanceCm;
	// UpdateLook owns the yaw; this keeps the pitch right on the frame the
	// rung changes, before the next look update.
	CameraBoom->SetRelativeRotation(FRotator(View.PitchDeg, CameraBoom->GetRelativeRotation().Yaw, 0.0f));
	ChaseCamera->SetFieldOfView(
		FMath::Clamp(BaseChaseFov + View.FovDeltaDeg + CurrentFovBoost, 50.0f, 130.0f));
}

void AApexRaceDirector::ApplyCameraMode()
{
	// A shot pose outranks both driving cameras, and everything that re-applies
	// the mode (a new followed car, C, a settings change) comes through here,
	// which is what keeps it parked.
	CockpitCamera->SetActive(bCockpitView && !bTvView && !bShotCameraPose);
	ChaseCamera->SetActive(!bCockpitView && !bTvView && !bShotCameraPose);
	TvCamera->SetActive(bTvView && !bShotCameraPose);
	ShotCamera->SetActive(bShotCameraPose);
	ApplyChaseView();

	// From the driver's seat the car's own bodywork is what frames the view —
	// unless the mesh has no interior to speak of, in which case the player
	// can switch it off. Only the followed car: the rest of the field has to
	// stay visible, which is why this is not a flag on the mesh itself.
	const UApexSettingsSave* Values = GetSettings() ? GetSettings()->Get() : nullptr;
	const bool bShowOwnCar = bShotCameraPose || bTvView || !bCockpitView || !Values || Values->bCockpitShowCar;
	if (FollowedCar && !(bDemoView && !bDemoWorldVisible))
	{
		FollowedCar->SetMeshVisible(bShowOwnCar);
	}
	PushRigFeatures();
}

FVector AApexRaceDirector::CockpitEyeLocal() const
{
	if (!FollowedCar)
	{
		return FVector::ZeroVector;
	}
	const UApexSettingsSave* Values = GetSettings() ? GetSettings()->Get() : nullptr;
	const FVector Seat = Values ? FVector(Values->SeatForwardCm, 0.0f, Values->SeatHeightCm) : FVector::ZeroVector;
	return FollowedCar->GetCockpitLayout().Eye + Seat;
}

void AApexRaceDirector::UpdateCockpitCamera()
{
	if (!FollowedCar)
	{
		return;
	}
	const UApexSettingsSave* Values = GetSettings() ? GetSettings()->Get() : nullptr;
	const float HorizonLock = Values ? Values->HorizonLock : 0.0f;
	const float ViewPitch = Values ? Values->ViewPitchDeg : 0.0f;

	// The car's orientation as far as the horizon lock allows, then the
	// driver's gaze and head turn in the car's frame, then the shake.
	const FRotator CarRotation = FollowedCar->GetActorRotation();
	const FQuat View = ApexCockpit::ViewRotation(CarRotation, HorizonLock, ViewPitch, LookYawDeg)
		* CockpitShake.Quaternion();
	const FVector EyeLocal = CockpitEyeLocal() + HeadOffset;
	CockpitCamera->SetWorldLocationAndRotation(
		FollowedCar->GetActorLocation() + CarRotation.RotateVector(EyeLocal), View);
}

void AApexRaceDirector::ApplyCameraSettings()
{
	const UApexSettingsSave* Values = GetSettings() ? GetSettings()->Get() : nullptr;
	if (!Values)
	{
		return;
	}
	SetFieldOfView(Values->FieldOfView);
	// The chase distance is a camera setting like any other, so a change made
	// in the overlay reaches the boom here. The view itself is not touched:
	// picking a distance from the cockpit leaves the player in the cockpit.
	// A rung named on the command line owns the race and is not overwritten.
	if (!bChaseLevelFromCommandLine)
	{
		ChaseLevel = FMath::Clamp(Values->ChaseViewLevel, 0, ApexChase::Num() - 1);
	}
	if (Rig)
	{
		// The seat moved: the screens turn to face the new eye.
		Rig->SetEyeLocal(CockpitEyeLocal());
	}
	ApplyCameraMode();
}

void AApexRaceDirector::PushRigFeatures()
{
	if (!Rig)
	{
		return;
	}
	const UApexSettingsSave* Values = GetSettings() ? GetSettings()->Get() : nullptr;
	FApexCockpitFeatures Features;
	// From the shot or broadcast camera the wheel and mirrors would float in mid-air.
	Features.bCockpitActive = bCockpitView && !bShotCameraPose && !bTvView;
	if (Values)
	{
		Features.bWheel = Values->bCockpitWheel;
		Features.bMirrors = Values->bCockpitMirrors;
		Features.bVirtualMirror = Values->bVirtualMirror;
		Features.MirrorQuality = Values->MirrorQuality;
		Features.bMetric = Values->Units == EApexUnits::Metric;
	}
	Rig->SetFeatures(Features);
}

UTextureRenderTarget2D* AApexRaceDirector::GetVirtualMirrorTexture() const
{
	return Rig ? Rig->GetVirtualMirrorTexture() : nullptr;
}

void AApexRaceDirector::EnsureRig()
{
	UWorld* World = GetWorld();
	if (Rig || !World)
	{
		return;
	}
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	Rig = World->SpawnActor<AApexCockpitRig>(AApexCockpitRig::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, Params);
	if (Rig && FollowedCar)
	{
		Rig->AttachToCar(FollowedCar);
		Rig->SetEyeLocal(CockpitEyeLocal());
	}
}

void AApexRaceDirector::DestroyRig()
{
	if (Rig)
	{
		Rig->Destroy();
		Rig = nullptr;
	}
}

void AApexRaceDirector::ApplyRaceInputMode(bool bRacing)
{
	if (AApexPlayerController* PlayerController =
			Cast<AApexPlayerController>(UGameplayStatics::GetPlayerController(this, 0)))
	{
		PlayerController->SetDriveInputEnabled(bRacing);
	}
}

void AApexRaceDirector::PollViewInput()
{
	AApexPlayerController* PlayerController =
		Cast<AApexPlayerController>(UGameplayStatics::GetPlayerController(this, 0));
	if (PlayerController && PlayerController->ConsumeCameraToggle())
	{
		CycleView();
	}
}

float AApexRaceDirector::GetFollowedSpeedKph() const
{
	return FollowedCar ? ApexRace::MpsToKph(FollowedCar->GetSpeedMps()) : 0.0f;
}

void AApexRaceDirector::BeginRaceView()
{
	if (bDemoView)
	{
		// The player's own race takes the view from the menu's demo.
		EndDemoView();
	}
	if (bRaceViewActive)
	{
		return;
	}
	bRaceViewActive = true;
	bLoggedFirstTelemetry = false;
	bHasTvPose = false;
	bTvView = false;
	Tv.Reset(static_cast<int32>(FPlatformTime::Cycles()));
	CarProgress.Reset();
	CarMotion.Reset();
	BleepCountdownSecond = -1;
	BleepLap = -1;
	BleepCarIndex = -1;

	LoadTrackLevel();
	ApplyRaceEnvironment();

	// The view a race opens in comes from the settings; -ApexView=chase or
	// -ApexView=cockpit overrides it for a screenshot run.
	if (const UApexSettingsSave* Values = GetSettings() ? GetSettings()->Get() : nullptr)
	{
		bCockpitView = Values->bStartInCockpit;
		ChaseLevel = FMath::Clamp(Values->ChaseViewLevel, 0, ApexChase::Num() - 1);
	}
	FString RequestedView;
	if (FParse::Value(FCommandLine::Get(), TEXT("ApexView="), RequestedView))
	{
		bCockpitView = !RequestedView.Equals(TEXT("chase"), ESearchCase::IgnoreCase);
		bTvView = RequestedView.Equals(TEXT("tv"), ESearchCase::IgnoreCase);
		// A rung can also be named outright (-ApexView=roof|close|near|far),
		// for a screenshot run of one distance.
		int32 Named = ApexChase::CockpitLevel;
		if (ApexChase::FindByName(RequestedView, Named) && Named >= 0)
		{
			bCockpitView = false;
			ChaseLevel = Named;
			bChaseLevelFromCommandLine = true;
		}
	}
	// Whether or not it opens on it, a race can be switched to the broadcast
	// camera, which wants the circuit's centerline for its trackside positions.
	if (const UApexNetSubsystem* Net = GetNet())
	{
		FApexSessionSummary Session;
		if (Net->FindSessionById(Net->GetCurrentSessionId(), Session))
		{
			for (const FApexTrackConfigSummary& Candidate : Net->GetCachedLobbyState().TrackConfigs)
			{
				if (Candidate.Name == Session.TrackName)
				{
					Tv.SetPath(Candidate.Centerline);
					break;
				}
			}
		}
	}
	LookYawDeg = 0.0f;
	HeadOffset = FVector::ZeroVector;
	bHavePrevMotion = false;
	// Before the camera mode is first applied below, so the race opens on the shot.
	ApplyShotCameraCommandLine();

	if (const UApexNetSubsystem* Net = GetNet())
	{
		SyncCarsToRoster(Net->GetSessionRoster());
	}
	UpdateCameraTarget();
	EnsureRig();
	EnsureRacingLine();
	UpdateCockpitCamera();
	// The cameras only exist while racing, so the saved camera block has had
	// nowhere to land until now. This also applies the camera mode.
	ApplyCameraSettings();
	ApplyRaceInputMode(true);

	if (APlayerController* PlayerController = UGameplayStatics::GetPlayerController(this, 0))
	{
		PlayerController->SetViewTargetWithBlend(this, 0.4f);
	}

	UE_LOG(LogApexSim, Log,
		TEXT("Race view active with %d car(s) in the %s view. Drive with WASD, C steps through the cameras"),
		Cars.Num(), *DescribeView());
}

void AApexRaceDirector::EndRaceView()
{
	// The demo view is ended by whoever began it (UApexDemoModeSubsystem).
	if (!bRaceViewActive || bDemoView)
	{
		return;
	}
	bRaceViewActive = false;
	bTvView = false;
	DestroyGhost();
	bLocalInGarage = false;
	LocalLap = 0;
	// A shot is for one race; the next one re-reads the command line.
	bShotCameraPose = false;
	ApplyCameraMode();
	// Un-hide before dropping the reference, or a car kept for a later race
	// would come back invisible.
	if (FollowedCar)
	{
		FollowedCar->SetMeshVisible(true);
	}
	FollowedCar = nullptr;
	DestroyRig();
	DestroyRacingLine();
	DestroyAllCars();
	UnloadTrackLevel();
	RestoreMenuEnvironment();
	ApplyRaceInputMode(false);

	UE_LOG(LogApexSim, Log, TEXT("Race view ended"));
}

bool AApexRaceDirector::IsTrackLevelLoaded() const
{
	return TrackLevel && TrackLevel->IsLevelLoaded();
}

FString AApexRaceDirector::ResolveTrackLevelPath() const
{
	if (bDemoView)
	{
		// A demo session is unlisted, so there is no session summary to read
		// the track file from; whoever started it named the track.
		const FString DemoPath = FString::Printf(TEXT("/Game/Tracks/%s/L_%s"), *DemoTrackStem, *DemoTrackStem);
		return !DemoTrackStem.IsEmpty() && FPackageName::DoesPackageExist(DemoPath) ? DemoPath : FString();
	}

	const UApexNetSubsystem* Net = GetNet();
	if (!Net)
	{
		return FString();
	}

	FApexSessionSummary Session;
	if (!Net->FindSessionById(Net->GetCurrentSessionId(), Session) || Session.TrackFile.IsEmpty())
	{
		return FString();
	}

	// "tracks/real/Monza.yaml" -> "Monza", which is both the export stem and
	// the folder the importer generated into.
	const FString Stem = FPaths::GetBaseFilename(Session.TrackFile);
	if (Stem.IsEmpty())
	{
		return FString();
	}

	const FString PackagePath = FString::Printf(TEXT("/Game/Tracks/%s/L_%s"), *Stem, *Stem);
	if (!FPackageName::DoesPackageExist(PackagePath))
	{
		UE_LOG(LogApexSim, Warning,
			TEXT("No imported level for track \"%s\" at %s — cars will race in an empty world. "
				 "Bake it with `cargo run --bin ats-export -- --all` and import it with "
				 "`ApexSimEditor-Cmd <uproject> -run=ApexTrackImport -all`"),
			*Session.TrackName, *PackagePath);
		return FString();
	}
	return PackagePath;
}

void AApexRaceDirector::LoadTrackLevel()
{
	if (TrackLevel)
	{
		return;
	}

	const FString PackagePath = ResolveTrackLevelPath();
	if (PackagePath.IsEmpty())
	{
		return;
	}

	// A level instance, not a travel: the menu world owns this actor and the
	// entire UI, and travelling would destroy both mid-race.
	bool bSuccess = false;
	TrackLevel = ULevelStreamingDynamic::LoadLevelInstanceBySoftObjectPtr(this,
		TSoftObjectPtr<UWorld>(FSoftObjectPath(PackagePath)), FVector::ZeroVector,
		FRotator::ZeroRotator, bSuccess);
	if (!bSuccess || !TrackLevel)
	{
		UE_LOG(LogApexSim, Warning, TEXT("Failed to stream track level %s"), *PackagePath);
		TrackLevel = nullptr;
		return;
	}

	UE_LOG(LogApexSim, Log, TEXT("Streaming track level %s"), *PackagePath);
	ForgetStartLights();
	VerifyTrackContent();
}

void AApexRaceDirector::VerifyTrackContent()
{
	UApexMenuFlowSubsystem* Flow = GetFlow();
	const UApexNetSubsystem* Net = GetNet();
	if (!Flow || !Net)
	{
		return;
	}

	FString TrackId;
	if (bDemoView)
	{
		// Unlisted, so no summary carries its id; the stem is all the demo knows.
		TrackId = Flow->FindTrackIdByStem(DemoTrackStem);
	}
	else
	{
		FApexSessionSummary Session;
		if (Net->FindSessionById(Net->GetCurrentSessionId(), Session))
		{
			TrackId = Session.TrackId;
		}
	}
	if (TrackId.IsEmpty() || TrackId == VerifiedTrackId)
	{
		return;
	}
	VerifiedTrackId = TrackId;
	Flow->VerifyTrackContent(TrackId, !bDemoView);
}

void AApexRaceDirector::VerifyLocalCarContent()
{
	UApexMenuFlowSubsystem* Flow = GetFlow();
	if (!Flow || bDemoView || !Flow->HasPendingCar())
	{
		return;
	}
	const FString CarId = Flow->GetPendingCarId();
	if (CarId == VerifiedCarId)
	{
		return;
	}
	VerifiedCarId = CarId;
	Flow->VerifyCarContent(CarId, true);
}

void AApexRaceDirector::UnloadTrackLevel()
{
	if (!TrackLevel)
	{
		return;
	}
	// Clearing both flags is what actually retires a streamed instance;
	// there is no single "unload now" call on ULevelStreaming.
	TrackLevel->SetShouldBeVisible(false);
	TrackLevel->SetShouldBeLoaded(false);
	TrackLevel = nullptr;
	ForgetStartLights();
	ForgetTrackLevelConditions();
	VerifiedTrackId.Reset();
}

void AApexRaceDirector::DestroyAllCars()
{
	for (const TPair<int32, TObjectPtr<AApexRaceCarActor>>& Pair : Cars)
	{
		if (AApexRaceCarActor* Car = Pair.Value.Get())
		{
			Car->Destroy();
		}
	}
	Cars.Reset();
	CarIdShown.Reset();
	VerifiedCarId.Reset();
}

// --- Broadcast camera ------------------------------------------------------------

void AApexRaceDirector::SetTvView(bool bTv)
{
	if (bDemoView || bTvView == bTv)
	{
		// The demo is filmed by nothing else.
		return;
	}
	bTvView = bTv;
	bHasTvPose = false;
	if (bTvView)
	{
		Tv.RequestCut();
	}
	else
	{
		UpdateCameraTarget();
	}
	ApplyCameraMode();
	UE_LOG(LogApexSim, Log, TEXT("Camera: %s"), *DescribeView());
}

void AApexRaceDirector::UpdateTvCamera(float DeltaSeconds)
{
	UWorld* World = GetWorld();
	if (!World || DeltaSeconds <= 0.0f)
	{
		return;
	}

	const float TrackLengthM = static_cast<float>(Tv.GetPath().LengthCm / ApexRace::MetresToCentimetres);
	TArray<ApexTv::FCar> Field;
	Field.Reserve(Cars.Num());
	for (const TPair<int32, TObjectPtr<AApexRaceCarActor>>& Pair : Cars)
	{
		AApexRaceCarActor* Car = Pair.Value.Get();
		// Hidden until its first telemetry: it is still sitting at the origin.
		if (!Car || Car->IsHidden())
		{
			continue;
		}
		const FVector Location = Car->GetActorLocation();
		FCarMotion& Motion = CarMotion.FindOrAdd(Pair.Key);
		if (Motion.bValid && FVector::Dist(Location, Motion.PrevLocation) < 3000.0)
		{
			const FVector Measured = (Location - Motion.PrevLocation) / DeltaSeconds;
			Motion.Velocity = FMath::VInterpTo(Motion.Velocity, Measured, DeltaSeconds, 8.0f);
		}
		else
		{
			Motion.Velocity = Car->GetActorForwardVector() * Car->GetSpeedMps() * ApexRace::MetresToCentimetres;
		}
		Motion.PrevLocation = Location;
		Motion.bValid = true;

		ApexTv::FCar& Entry = Field.AddDefaulted_GetRef();
		Entry.CarIndex = Pair.Key;
		Entry.Location = Location;
		Entry.Rotation = Car->GetActorRotation();
		Entry.Velocity = Motion.Velocity;
		Entry.Body = Car->GetBodyBox();
		Entry.EyeLocal = Car->GetCockpitLayout().Eye;
		const FCarProgress* Progress = CarProgress.Find(Pair.Key);
		Entry.RaceDistanceM = Progress ? ApexRace::RaceDistanceM(Progress->Lap, Progress->StationM, TrackLengthM) : 0.0f;
		Entry.bOffTrack = Progress && !Progress->bOnTrack;
	}

	FCollisionQueryParams Params(SCENE_QUERY_STAT(ApexTvCamera), /*bTraceComplex*/ true, this);
	const FCollisionObjectQueryParams Statics(ECC_WorldStatic);
	ApexTv::FWorldQueries Queries;
	Queries.GroundZ = [World, &Params, &Statics](const FVector& Above, double& OutGroundZ)
	{
		TArray<FHitResult> Hits;
		World->LineTraceMultiByObjectType(Hits, Above, Above - FVector(0.0, 0.0, 200000.0), Statics, Params);
		Hits.Sort([](const FHitResult& A, const FHitResult& B) { return A.Distance < B.Distance; });
		for (const FHitResult& Hit : Hits)
		{
			if (!IsPropHit(Hit))
			{
				OutGroundZ = Hit.ImpactPoint.Z;
				return true;
			}
		}
		return false;
	};
	Queries.IsClear = [World, &Params, &Statics](const FVector& From, const FVector& To)
	{
		return !World->LineTraceTestByObjectType(From, To, Statics, Params);
	};

	Tv.Tuning().PaceScale = CVarTvPace.GetValueOnGameThread();
	ApexTv::FPose Pose;
	const bool bCountdown = LatestFrameState == EApexSessionState::Countdown;
	if (!Tv.Tick(Field, bCountdown, DeltaSeconds, static_cast<float>(World->GetRealTimeSeconds()), Queries, Pose))
	{
		return;
	}
	bHasTvPose = true;

	TvCamera->SetWorldLocationAndRotation(Pose.Location, Pose.Rotation);
	TvCamera->SetFieldOfView(Pose.FovDeg);
	const bool bDof = Pose.Aperture > 0.0f && CVarTvDof.GetValueOnGameThread() != 0;
	TvCamera->PostProcessSettings.DepthOfFieldFocalDistance = bDof ? Pose.FocusDistanceCm : 0.0f;
	TvCamera->PostProcessSettings.DepthOfFieldFstop = bDof ? Pose.Aperture : 22.0f;

	if (!FollowedCar || FollowedCar->GetCarIndex() != Tv.GetTargetCarIndex())
	{
		UpdateCameraTarget();
	}
	if (Tv.GetCutCount() != LoggedTvCuts)
	{
		LoggedTvCuts = Tv.GetCutCount();
		UE_LOG(LogApexSim, Verbose, TEXT("Broadcast camera: %s on car %d (cut: %s, after %.1f s)"),
			ApexTv::ShotName(Tv.GetShot()), Tv.GetTargetCarIndex(), Tv.GetLastCutReason(), Tv.GetLastShotHeld());
	}

	if (CVarTvDebug.GetValueOnGameThread() != 0 && GEngine)
	{
		GEngine->AddOnScreenDebugMessage(static_cast<uint64>(GetUniqueID()) + 7000, 0.0f, FColor::Yellow,
			FString::Printf(TEXT("TV %s on car %d  %.1f s  fov %.1f  f/%.1f @ %.0f m  cuts %d"),
				ApexTv::ShotName(Tv.GetShot()), Tv.GetTargetCarIndex(), Tv.GetShotAge(), Pose.FovDeg,
				Pose.Aperture, Pose.FocusDistanceCm / 100.0f, Tv.GetCutCount()));
	}
}

// --- Demo view -------------------------------------------------------------------

void AApexRaceDirector::BeginDemoView(const FString& TrackStem, const TArray<FVector2D>& Centerline)
{
	if (bRaceViewActive && !bDemoView)
	{
		// The player's race has the view.
		return;
	}
	if (bDemoView)
	{
		EndDemoView();
	}

	bDemoView = true;
	bRaceViewActive = true;
	bTvView = true;
	bHasTvPose = false;
	bDemoWorldVisible = true;
	bDemoFadeOut = false;
	bLoggedFirstTelemetry = false;
	bShotCameraPose = false;
	DemoTrackStem = TrackStem;
	DemoOpacity = 0.0f;
	DemoReadyFor = 0.0f;
	LatestFrameState = EApexSessionState::Lobby;
	CarProgress.Reset();
	CarMotion.Reset();
	BleepCountdownSecond = -1;
	BleepLap = -1;
	BleepCarIndex = -1;
	Tv.Reset(static_cast<int32>(FPlatformTime::Cycles()));
	Tv.SetPath(Centerline);

	LoadTrackLevel();
	ApplyRaceEnvironment();
	if (const UApexNetSubsystem* Net = GetNet())
	{
		SyncCarsToRoster(Net->GetSessionRoster());
	}
	UpdateCameraTarget();
	ApplyCameraMode();

	if (APlayerController* PlayerController = UGameplayStatics::GetPlayerController(this, 0))
	{
		// The shell stays opaque until the backdrop fades in, so no blend is needed.
		PlayerController->SetViewTarget(this);
	}
	UE_LOG(LogApexSim, Log, TEXT("Demo view: %s, %d centerline point(s) for the trackside cameras"),
		*TrackStem, Tv.GetPath().Points.Num());
}

void AApexRaceDirector::EndDemoView()
{
	if (!bDemoView)
	{
		return;
	}
	bDemoView = false;
	bRaceViewActive = false;
	bTvView = false;
	bHasTvPose = false;
	DemoOpacity = 0.0f;
	DemoReadyFor = 0.0f;
	LatestFrameState = EApexSessionState::Lobby;
	if (FollowedCar)
	{
		RemoveTickPrerequisiteActor(FollowedCar);
	}
	FollowedCar = nullptr;
	DestroyAllCars();
	CarProgress.Reset();
	CarMotion.Reset();
	BleepCountdownSecond = -1;
	BleepLap = -1;
	BleepCarIndex = -1;
	UnloadTrackLevel();
	if (bDemoWorldVisible)
	{
		// Hidden, it already gave the menu its lighting back.
		RestoreMenuEnvironment();
	}
	bDemoWorldVisible = true;
	DemoTrackStem.Reset();
	ApplyCameraMode();
	UE_LOG(LogApexSim, Log, TEXT("Demo view ended"));
}

void AApexRaceDirector::SetDemoWorldVisible(bool bVisible)
{
	if (!bDemoView || bDemoWorldVisible == bVisible)
	{
		return;
	}
	bDemoWorldVisible = bVisible;
	if (!bVisible)
	{
		DemoOpacity = 0.0f;
	}
	DemoReadyFor = 0.0f;
	if (TrackLevel)
	{
		TrackLevel->SetShouldBeVisible(bVisible);
	}
	if (bVisible)
	{
		ApplyRaceEnvironment();
	}
	else
	{
		RestoreMenuEnvironment();
	}
	ApplyDemoWorldVisibility();
	// The gantry's actors leave the world with the level and come back new.
	ForgetStartLights();
}

void AApexRaceDirector::ApplyAudioSettings()
{
	const UApexSettingsSave* Values = GetSettings() ? GetSettings()->Get() : nullptr;
	if (!Values)
	{
		return;
	}
	for (const TPair<int32, TObjectPtr<AApexRaceCarActor>>& Pair : Cars)
	{
		if (AApexRaceCarActor* Car = Pair.Value.Get())
		{
			Car->SetMixVolumes(Values->EngineVolume, Values->RoadVolume);
		}
	}
}

void AApexRaceDirector::UpdateCarAudio()
{
	// From the driver's seat of a closed car the engine comes through the
	// bulkhead; every other car, and every other camera, hears it outright.
	const bool bInCabin = bCockpitView && !bTvView && !bShotCameraPose && !bGhostReplay;
	for (const TPair<int32, TObjectPtr<AApexRaceCarActor>>& Pair : Cars)
	{
		if (AApexRaceCarActor* Car = Pair.Value.Get())
		{
			Car->SetHeardFromCabin(bInCabin && Car == FollowedCar);
		}
	}

	const UApexNetSubsystem* Net = GetNet();
	AApexRaceCarActor* Local = Net ? FindCar(Net->GetLocalCarIndex()) : nullptr;
	if (!Local)
	{
		return;
	}
	// Tracked even while silent, or the first frame back would replay old hits.
	const uint32 Serial = Net->GetDriverFeedbackSerial();
	const bool bNewMessage = Serial != LastRoadFeedbackSerial;
	LastRoadFeedbackSerial = Serial;

	// The server only sends feedback for a car that is being simulated, so a
	// stale message means a garage, a finished session or a dead connection.
	constexpr double StaleSeconds = 0.5;
	if (bLocalInGarage || bGhostReplay || FPlatformTime::Seconds() - Net->GetDriverFeedbackTime() > StaleSeconds)
	{
		Local->StopRoadSound();
		return;
	}

	const ApexFfb::FSignals Signals = ApexFfb::MakeSignals(
		Net->GetDriverFeedback(), Local->GetSpeedMps(), Local->GetGear(), bNewMessage);
	ApexRoadSynth::FInputs Levels;
	Levels.SpeedMps = Signals.SpeedMps;
	Levels.FrontSlide = Signals.FrontSlide;
	Levels.RearSlide = Signals.RearSlide;
	Levels.Lockup = Signals.Lockup;
	Levels.Wheelspin = Signals.Wheelspin;
	Levels.CurbLeft = Signals.CurbLeft;
	Levels.CurbRight = Signals.CurbRight;
	Levels.OffTrack = Signals.OffTrack;
	Levels.bWet = Sky.bWetRoad;
	Local->UpdateRoadSound(Levels, Signals.BumpMps, Signals.ImpactMps);
}

void AApexRaceDirector::ApplyDemoWorldVisibility()
{
	for (const TPair<int32, TObjectPtr<AApexRaceCarActor>>& Pair : Cars)
	{
		if (AApexRaceCarActor* Car = Pair.Value.Get())
		{
			Car->SetMeshVisible(bDemoWorldVisible);
			Car->SetEngineVolume(bDemoWorldVisible ? DemoEngineVolume : 0.0f);
		}
	}
}

bool AApexRaceDirector::IsDemoReady() const
{
	if (!bDemoView || bDemoFadeOut)
	{
		return false;
	}
	if (!bDemoWorldVisible)
	{
		return Cars.Num() > 0 && IsTrackLevelLoaded();
	}
	return DemoOpacity >= 1.0f;
}

void AApexRaceDirector::UpdateDemoOpacity(float DeltaSeconds)
{
	const bool bReady = bDemoWorldVisible && !bDemoFadeOut && bHasTvPose && Cars.Num() > 0 && IsTrackLevelLoaded()
		&& TrackLevel->IsLevelVisible();
	DemoReadyFor = bReady ? DemoReadyFor + DeltaSeconds : 0.0f;
	// A moment's grace once everything is in: the sky capture and the first
	// textures settle before anyone sees them.
	const float Target = DemoReadyFor > 0.75f ? 1.0f : 0.0f;
	DemoOpacity = Target > DemoOpacity
		? FMath::Min(Target, DemoOpacity + DeltaSeconds * 0.8f)
		: FMath::Max(Target, DemoOpacity - DeltaSeconds * 3.0f);
}
