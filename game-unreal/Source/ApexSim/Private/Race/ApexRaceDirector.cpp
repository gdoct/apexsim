#include "Race/ApexRaceDirector.h"

#include "ApexMenuFlowSubsystem.h"
#include "ApexNetSubsystem.h"
#include "ApexSim.h"
#include "Camera/CameraComponent.h"
#include "Engine/GameInstance.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/SpringArmComponent.h"
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
		Car->SetActorLabel(FString::Printf(TEXT("Car%d_%s"), Entry.CarIndex, *Entry.PlayerName));
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

	FollowedCar = Target;
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

	PollDrivingInput();
}

void AApexRaceDirector::PollDrivingInput()
{
	UApexNetSubsystem* Net = GetNet();
	APlayerController* PlayerController = UGameplayStatics::GetPlayerController(this, 0);
	if (!Net || !PlayerController || !Net->IsUdpReady())
	{
		return;
	}

	// Deliberately crude: polled key state rather than an Enhanced Input mapping
	// context. This exists so the transport milestone is drivable; real input
	// binding (wheel, pedals, axes) is its own piece of work.
	const bool bForward = PlayerController->IsInputKeyDown(EKeys::W) || PlayerController->IsInputKeyDown(EKeys::Up);
	const bool bBack    = PlayerController->IsInputKeyDown(EKeys::S) || PlayerController->IsInputKeyDown(EKeys::Down);
	const bool bLeft    = PlayerController->IsInputKeyDown(EKeys::A) || PlayerController->IsInputKeyDown(EKeys::Left);
	const bool bRight   = PlayerController->IsInputKeyDown(EKeys::D) || PlayerController->IsInputKeyDown(EKeys::Right);

	FApexPlayerInput Input;
	Input.Throttle = bForward ? 1.0f : 0.0f;
	Input.Brake = bBack ? 1.0f : 0.0f;
	// Steering is in the server's frame, where positive is to the left.
	Input.Steering = (bLeft ? 1.0f : 0.0f) - (bRight ? 1.0f : 0.0f);
	// Leave the gearbox alone; the server holds whatever gear it has.
	Input.Gear = -128;

	Net->SetPlayerInput(Input);
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

	if (const UApexNetSubsystem* Net = GetNet())
	{
		SyncCarsToRoster(Net->GetSessionRoster());
	}
	UpdateCameraTarget();

	if (APlayerController* PlayerController = UGameplayStatics::GetPlayerController(this, 0))
	{
		PlayerController->SetViewTargetWithBlend(this, 0.4f);
	}

	UE_LOG(LogApexSim, Log, TEXT("Race view active with %d car(s)"), Cars.Num());
}

void AApexRaceDirector::EndRaceView()
{
	if (!bRaceViewActive)
	{
		return;
	}
	bRaceViewActive = false;
	FollowedCar = nullptr;
	DestroyAllCars();

	UE_LOG(LogApexSim, Log, TEXT("Race view ended"));
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
