#include "Race/ApexRaceDirector.h"

#include "ApexMenuFlowSubsystem.h"
#include "ApexNetSubsystem.h"
#include "ApexPlayerController.h"
#include "ApexSettingsSubsystem.h"
#include "ApexSim.h"
#include "Camera/CameraComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/SkyLightComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/GameInstance.h"
#include "Engine/LevelStreamingDynamic.h"
#include "Engine/SkyLight.h"
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
	// is what `SetCockpitView` switches to. Chase starts active to match
	// `bCockpitView`'s default.
	CockpitCamera->SetActive(false);
	ChaseCamera->SetActive(true);
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

	UpdateCameraFeel(DeltaSeconds);
	UpdateCockpitCamera();
	PollViewInput();
	PollDrivingInput();
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
	ChaseCamera->SetFieldOfView(FMath::Clamp(BaseChaseFov + CurrentFovBoost, 50.0f, 130.0f));

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

	// The menu world's sun points wherever the menu looked good, at the
	// engine's default 10 lux — which is why race scenes used to render as
	// low-contrast pastel: sun and ambient were the same order of magnitude,
	// and auto-exposure normalized whatever the albedo said. For driving it
	// becomes an actual sun: tens of thousands of lux, low and warm, and
	// movable because nothing in a streamed track level has baked lighting.
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
		Sun->SetActorRotation(FRotator(-26.0f, 41.0f, 0.0f));
		SunComponent->SetLightColor(FLinearColor(1.0f, 0.93f, 0.84f));
		SunComponent->SetIntensity(50000.0f);
		if (UDirectionalLightComponent* Directional =
				Cast<UDirectionalLightComponent>(SunComponent))
		{
			// The atmosphere (and through it the real-time sky light) has to
			// be driven by this sun, or the ground gets daylight while the
			// sky keeps its 10-lux dusk.
			Directional->bAtmosphereSunLight = true;
			Directional->MarkRenderStateDirty();
		}
		break;
	}

	// The sky light's one static capture was taken in the menu void; put it
	// on real-time capture so ambient and reflections follow the actual sky
	// over the actual circuit.
	for (TActorIterator<ASkyLight> It(World); It; ++It)
	{
		USkyLightComponent* SkyComponent = (*It)->GetLightComponent();
		SkyComponent->SetMobility(EComponentMobility::Movable);
		SkyComponent->SetRealTimeCaptureEnabled(true);
		break;
	}
}

void AApexRaceDirector::RestoreMenuEnvironment()
{
	if (ADirectionalLight* Sun = MenuSun.Get(); Sun && bMenuSunSaved)
	{
		Sun->SetActorRotation(MenuSunRotation);
		Sun->GetLightComponent()->SetLightColor(MenuSunColor);
		Sun->GetLightComponent()->SetIntensity(MenuSunIntensity);
	}
	// The sky light stays on real-time capture: it is simply correct, in the
	// menu as much as in the race.
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
	else
	{
		Input.Gear = PollAutoGearbox();
	}

	Net->SetPlayerInput(Input);
}

int32 AApexRaceDirector::PollAutoGearbox()
{
	const UApexSettingsSubsystem* Settings = GetSettings();
	const UApexNetSubsystem* Net = GetNet();
	if (!Settings || !Settings->Get() || !Settings->Get()->bAutoGearbox || !Net)
	{
		return -128;
	}

	// The server has no automatic-gearbox flag, so an auto box is a client that
	// sends the shift the driver would have. Everything it needs — revs and the
	// gear actually engaged — is on the telemetry frame.
	const int32 LocalIndex = Net->GetLocalCarIndex();
	const FApexCarTelemetry* Local = Net->GetLatestTelemetry().Cars.FindByPredicate(
		[LocalIndex](const FApexCarTelemetry& Car) { return Car.CarIndex == LocalIndex; });
	if (!Local || Local->Gear < 0)
	{
		// Reverse is never chosen automatically: only the driver knows they meant
		// to back up rather than to stop.
		return -128;
	}

	int32 Wanted = Local->Gear;
	if (Local->EngineRpm > AutoShiftUpRpm && Local->Gear < 10)
	{
		Wanted = Local->Gear + 1;
	}
	else if (Local->EngineRpm < AutoShiftDownRpm && Local->Gear > 1)
	{
		Wanted = Local->Gear - 1;
	}
	else if (Local->Gear == 0 && Local->Throttle > 0.05f)
	{
		// Pulling away from neutral.
		Wanted = 1;
	}

	if (Wanted == Local->Gear)
	{
		return -128;
	}

	// Telemetry lags the request by a frame or two, so without a hold the same
	// shift is sent repeatedly and the box hunts.
	const double Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0;
	if (Now - LastAutoShiftTime < AutoShiftHoldSeconds)
	{
		return -128;
	}
	LastAutoShiftTime = Now;
	return Wanted;
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
	ChaseCamera->SetFieldOfView(BaseChaseFov);
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
	// corner or over a kerb the horizon should tilt with the car. The shake
	// composes in the car's own frame, after its orientation.
	const FRotator CarRotation = FollowedCar->GetActorRotation();
	const FQuat ShakenRotation = CarRotation.Quaternion() * CockpitShake.Quaternion();
	CockpitCamera->SetWorldLocationAndRotation(
		FollowedCar->GetActorLocation() + CarRotation.RotateVector(CockpitEyeOffset),
		ShakenRotation);
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
	ApplyRaceEnvironment();

	if (const UApexNetSubsystem* Net = GetNet())
	{
		SyncCarsToRoster(Net->GetSessionRoster());
	}
	UpdateCameraTarget();
	UpdateCockpitCamera();
	ApplyCameraMode();
	ApplyRaceInputMode(true);

	// The cameras only exist while racing, so the saved field of view has had
	// nowhere to land until now.
	if (const UApexSettingsSubsystem* Settings = GetSettings())
	{
		if (const UApexSettingsSave* Values = Settings->Get())
		{
			SetFieldOfView(Values->FieldOfView);
		}
	}

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
