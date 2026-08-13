#include "Race/ApexRaceDirector.h"

#include "ApexMenuFlowSubsystem.h"
#include "ApexNetSubsystem.h"
#include "ApexPlayerController.h"
#include "ApexSim.h"
#include "Camera/CameraComponent.h"
#include "Engine/GameInstance.h"
#include "Engine/LevelStreamingDynamic.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/SpringArmComponent.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Kismet/GameplayStatics.h"
#include "Race/ApexRaceCarActor.h"
#include "Race/ApexRaceCoordinate.h"

AApexRaceDirector::AApexRaceDirector()
{
	PrimaryActorTick.bCanEverTick = true;

	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);

	CameraBoom = CreateDefaultSubobject<USpringArmComponent>(TEXT("CameraBoom"));
	CameraBoom->SetupAttachment(Root);
	CameraBoom->TargetArmLength = 900.0f;
	CameraBoom->SetRelativeRotation(FRotator(-12.0f, 0.0f, 0.0f));
	// The boom lags behind the car rather than being welded to it, which reads
	// as a chase camera instead of a rigid mount.
	CameraBoom->bEnableCameraLag = true;
	CameraBoom->bEnableCameraRotationLag = true;
	CameraBoom->CameraLagSpeed = 6.0f;
	CameraBoom->CameraRotationLagSpeed = 5.0f;
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

	// Exactly one camera component may be active on a view target; the other
	// is what `SetCockpitView` switches to.
	CockpitCamera->SetActive(true);
	ChaseCamera->SetActive(false);
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
	}
}

void AApexRaceDirector::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (UApexNetSubsystem* Net = GetNet())
	{
		Net->OnSessionRosterUpdated.RemoveDynamic(this, &AApexRaceDirector::HandleRosterUpdated);
		Net->OnTelemetry.RemoveDynamic(this, &AApexRaceDirector::HandleTelemetry);
		Net->OnSessionLeft.RemoveDynamic(this, &AApexRaceDirector::HandleSessionLeft);
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
			Cars.Add(Entry.CarIndex, Car);

			// The roster says who is driving but not what they chose, and the
			// protocol never tells us another player's car. Everyone gets the
			// local player's mesh, or the fallback.
			TSoftObjectPtr<UStaticMesh> Mesh = DefaultCarMesh;
			if (Flow && Flow->HasPendingCar())
			{
				FApexCarCatalogRow Row;
				if (Flow->GetCarCatalogRow(Flow->GetPendingCarId(), Row) && !Row.Mesh.IsNull())
				{
					Mesh = Row.Mesh;
				}
			}
			Car->SetCarMesh(Mesh);
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
			It.RemoveCurrent();
		}
	}

	UE_LOG(LogApexSim, Log, TEXT("Race roster: %d car(s) spawned"), Cars.Num());
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

	for (const FApexCarTelemetry& Car : Frame.Cars)
	{
		if (AApexRaceCarActor* Actor = FindCar(Car.CarIndex))
		{
			Actor->ApplyTelemetry(Car);
		}
	}
}

void AApexRaceDirector::UpdateCameraTarget()
{
	const UApexNetSubsystem* Net = GetNet();
	const int32 LocalIndex = Net ? Net->GetLocalCarIndex() : -1;

	AApexRaceCarActor* Target = FindCar(LocalIndex);
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
		}
		FollowedCar = Target;
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

	UpdateCockpitCamera();
	PollViewInput();
	PollDrivingInput();
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
	UE_LOG(LogApexSim, Log, TEXT("Camera: %s"), bCockpitView ? TEXT("cockpit") : TEXT("chase"));
}

void AApexRaceDirector::ApplyCameraMode()
{
	CockpitCamera->SetActive(bCockpitView);
	ChaseCamera->SetActive(!bCockpitView);

	// From the driver's seat the car's own bodywork fills the screen, so the
	// followed car stops drawing. Only that one: the rest of the field has to
	// stay visible, which is why this is not a flag on the mesh itself.
	if (FollowedCar)
	{
		FollowedCar->SetMeshVisible(!bCockpitView);
	}
}

void AApexRaceDirector::UpdateCockpitCamera()
{
	if (!FollowedCar)
	{
		return;
	}
	// Full rotation, not the levelled yaw the boom uses: through a banked
	// corner or over a kerb the horizon should tilt with the car.
	const FRotator CarRotation = FollowedCar->GetActorRotation();
	CockpitCamera->SetWorldLocationAndRotation(
		FollowedCar->GetActorLocation() + CarRotation.RotateVector(CockpitEyeOffset), CarRotation);
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
		SetCockpitView(!bCockpitView);
	}
}

float AApexRaceDirector::GetFollowedSpeedKph() const
{
	return FollowedCar ? ApexRace::MpsToKph(FollowedCar->GetSpeedMps()) : 0.0f;
}

void AApexRaceDirector::BeginRaceView()
{
	if (bRaceViewActive)
	{
		return;
	}
	bRaceViewActive = true;
	bLoggedFirstTelemetry = false;

	LoadTrackLevel();

	if (const UApexNetSubsystem* Net = GetNet())
	{
		SyncCarsToRoster(Net->GetSessionRoster());
	}
	UpdateCameraTarget();
	UpdateCockpitCamera();
	ApplyCameraMode();
	ApplyRaceInputMode(true);

	if (APlayerController* PlayerController = UGameplayStatics::GetPlayerController(this, 0))
	{
		PlayerController->SetViewTargetWithBlend(this, 0.4f);
	}

	UE_LOG(LogApexSim, Log,
		TEXT("Race view active with %d car(s). Drive with WASD, C swaps cockpit/chase"),
		Cars.Num());
}

void AApexRaceDirector::EndRaceView()
{
	if (!bRaceViewActive)
	{
		return;
	}
	bRaceViewActive = false;
	// Un-hide before dropping the reference, or a car kept for a later race
	// would come back invisible.
	if (FollowedCar)
	{
		FollowedCar->SetMeshVisible(true);
	}
	FollowedCar = nullptr;
	DestroyAllCars();
	UnloadTrackLevel();
	ApplyRaceInputMode(false);

	UE_LOG(LogApexSim, Log, TEXT("Race view ended"));
}

bool AApexRaceDirector::IsTrackLevelLoaded() const
{
	return TrackLevel && TrackLevel->IsLevelLoaded();
}

FString AApexRaceDirector::ResolveTrackLevelPath() const
{
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
}
