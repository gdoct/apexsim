#include "ApexReplaySubsystem.h"

#include "ApexMenuFlowSubsystem.h"
#include "ApexSim.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetBlueprintLibrary.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/GameUserSettings.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Race/ApexRaceCoordinate.h"
#include "Race/ApexRaceDirector.h"
#include "Race/ApexReplayClip.h"
#include "Race/ApexShotCamera.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UnrealClient.h"

// Named, not anonymous: this module is unity-built.
namespace ApexReplayArgs
{
	bool ParseClock(const FString& Text, int32& OutMinutes)
	{
		FString Hours;
		FString Minutes;
		if (!Text.Split(TEXT(":"), &Hours, &Minutes) || !Hours.IsNumeric() || !Minutes.IsNumeric())
		{
			return false;
		}
		const int32 H = FCString::Atoi(*Hours);
		const int32 M = FCString::Atoi(*Minutes);
		if (H < 0 || H > 23 || M < 0 || M > 59)
		{
			return false;
		}
		OutMinutes = H * 60 + M;
		return true;
	}

	bool ParseWeather(const FString& Text, EApexWeather& Out)
	{
		const FString Key = Text.Replace(TEXT(" "), TEXT("")).Replace(TEXT("_"), TEXT("")).ToLower();
		if (Key == TEXT("sunny") || Key == TEXT("clear")) { Out = EApexWeather::Sunny; return true; }
		if (Key == TEXT("cloudy")) { Out = EApexWeather::Cloudy; return true; }
		if (Key == TEXT("overcast")) { Out = EApexWeather::Overcast; return true; }
		if (Key == TEXT("lightrain") || Key == TEXT("rain")) { Out = EApexWeather::LightRain; return true; }
		if (Key == TEXT("heavyrain") || Key == TEXT("storm")) { Out = EApexWeather::HeavyRain; return true; }
		return false;
	}

	bool ParseFloat(const TCHAR* Key, float& InOut)
	{
		FString Text;
		if (FParse::Value(FCommandLine::Get(), Key, Text))
		{
			InOut = FCString::Atof(*Text);
			return true;
		}
		return false;
	}

	bool ParseDouble(const TCHAR* Key, double& InOut)
	{
		FString Text;
		if (FParse::Value(FCommandLine::Get(), Key, Text))
		{
			InOut = FCString::Atod(*Text);
			return true;
		}
		return false;
	}

	/** Wall-clock seconds: loading and settling take real time, whatever the fixed step says. */
	double Now()
	{
		return FPlatformTime::Seconds();
	}
}

bool UApexReplaySubsystem::IsReplayRun()
{
	FString Path;
	return FParse::Value(FCommandLine::Get(), TEXT("ApexReplay="), Path) && !Path.IsEmpty();
}

bool UApexReplaySubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	return IsReplayRun() && Super::ShouldCreateSubsystem(Outer);
}

void UApexReplaySubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// Offline: the shell must not go looking for a server (which would also
	// start a demo race and a lobby of its own under the replay).
	if (UApexMenuFlowSubsystem* Flow = Collection.InitializeDependency<UApexMenuFlowSubsystem>())
	{
		Flow->bAutoConnectOnStartup = false;
	}

	if (!ParseCommandLine())
	{
		Phase = EPhase::Done;
		return;
	}
	if (!RecordDir.IsEmpty())
	{
		// Game time steps exactly 1/fps per rendered frame, however long a
		// frame takes to draw and write: a slow machine renders slowly, not
		// differently.
		FApp::SetUseFixedTimeStep(true);
		FApp::SetFixedDeltaTime(1.0 / Fps);
	}
	Phase = EPhase::WaitWorld;
	PhaseSeconds = 0.0f;
	PhaseStartedAt = FPlatformTime::Seconds();
	UE_LOG(LogApexSim, Log, TEXT("Replay run: %s, %s camera, %s"), *ClipPath, ApexReplayCam::ModeName(Camera.Mode),
		RecordDir.IsEmpty() ? TEXT("playing") : *FString::Printf(TEXT("recording %d fps to %s"), Fps, *RecordDir));
}

void UApexReplaySubsystem::Deinitialize()
{
	Phase = EPhase::Idle;
	Clip.Reset();
	Super::Deinitialize();
}

TStatId UApexReplaySubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UApexReplaySubsystem, STATGROUP_Tickables);
}

bool UApexReplaySubsystem::ParseCommandLine()
{
	using namespace ApexReplayArgs;
	const TCHAR* Cmd = FCommandLine::Get();
	FParse::Value(Cmd, TEXT("ApexReplay="), ClipPath);
	ClipPath = FPaths::ConvertRelativePathToFull(ClipPath);
	// Known before anything can fail, so a scripted render that cannot start
	// says why in its output folder and quits instead of idling in the menu.
	if (FParse::Value(Cmd, TEXT("ApexReplayRecord="), RecordDir))
	{
		RecordDir = FPaths::ConvertRelativePathToFull(RecordDir);
		IFileManager::Get().MakeDirectory(*RecordDir, true);
		bExitWhenDone = !FParse::Param(Cmd, TEXT("ApexReplayNoExit"));
	}

	Clip = MakeShared<FApexReplayClip>();
	FString Error;
	if (!Clip->LoadFromFile(ClipPath, Error))
	{
		Fail(FString::Printf(TEXT("cannot play %s: %s"), *ClipPath, *Error));
		return false;
	}

	// --- camera ---
	FString Text;
	if (FParse::Value(Cmd, TEXT("ApexReplayCam="), Text) && !ApexReplayCam::ParseMode(Text, Camera.Mode))
	{
		UE_LOG(LogApexSim, Warning, TEXT("Replay: unknown camera \"%s\" (tv, fixed, pan, chase, cockpit)"), *Text);
	}
	if (FParse::Value(Cmd, TEXT("ApexReplayFollow="), Text))
	{
		if (!ApexReplayCam::ParseFollow(Text, Camera.Follow, Camera.FollowIndex))
		{
			UE_LOG(LogApexSim, Warning, TEXT("Replay: -ApexReplayFollow=%s is not leader, nearest or a car index"), *Text);
		}
		// Naming a car for the broadcast camera means "film this car".
		Camera.bLockTv = true;
	}
	Camera.bLockTv |= FParse::Param(Cmd, TEXT("ApexReplayLockTv"));
	if (FParse::Value(Cmd, TEXT("ApexReplayShot="), Text) && !ApexReplayCam::ParseTvShot(Text, Camera.TvShot))
	{
		UE_LOG(LogApexSim, Warning, TEXT("Replay: unknown TV shot \"%s\""), *Text);
	}
	int32 Seed = Camera.Seed;
	if (FParse::Value(Cmd, TEXT("ApexReplaySeed="), Seed))
	{
		Camera.Seed = Seed;
	}
	if (FParse::Value(Cmd, TEXT("ApexReplayChase="), Text))
	{
		for (int32 Level = 0; Level < ApexChase::Num(); ++Level)
		{
			if (Text.Equals(ApexChase::Get(Level).Name, ESearchCase::IgnoreCase) || Text == FString::FromInt(Level))
			{
				Camera.ChaseLevel = Level;
			}
		}
	}

	// The tripod: the screenshot camera's own switches, server frame.
	TArray<double> Values;
	ApexShotCamera::FPose Pose;
	FString Requested;
	const bool bLookAt = FParse::Value(Cmd, TEXT("ApexCameraLookAt="), Requested, false);
	if (bLookAt || FParse::Value(Cmd, TEXT("ApexCamera="), Requested, false))
	{
		if (ApexShotCamera::ParseNumberList(Requested, Values)
			&& (bLookAt ? ApexShotCamera::PoseFromLookAt(Values, Pose) : ApexShotCamera::PoseFromGoto(Values, Pose)))
		{
			Camera.bHasEye = true;
			Camera.EyeCm = Pose.LocationCm;
			Camera.EyeRotation = Pose.Rotation;
			if (bLookAt)
			{
				Camera.bHasLook = true;
				Camera.LookCm = ApexRace::ServerToUnrealPosition(FVector(Values[3], Values[4], Values[5]));
			}
		}
		else
		{
			UE_LOG(LogApexSim, Warning, TEXT("Replay: camera pose \"%s\" does not parse"), *Requested);
		}
	}
	if (FParse::Value(Cmd, TEXT("ApexReplayLook="), Requested, false) && ApexShotCamera::ParseNumberList(Requested, Values)
		&& Values.Num() == 3)
	{
		Camera.bHasLook = true;
		Camera.LookCm = ApexRace::ServerToUnrealPosition(FVector(Values[0], Values[1], Values[2]));
	}
	ParseFloat(TEXT("ApexCameraFov="), Camera.FovDeg);
	ParseFloat(TEXT("ApexReplayLookBias="), Camera.LookBias);
	ParseFloat(TEXT("ApexReplayPanSpeed="), Camera.PanSpeed);
	ParseFloat(TEXT("ApexReplayLead="), Camera.LeadSeconds);
	ParseFloat(TEXT("ApexReplayFrameWidth="), Camera.FrameWidthM);
	ParseFloat(TEXT("ApexReplayMinFov="), Camera.MinFovDeg);
	float ClearanceM = Camera.GroundClearanceCm / 100.0f;
	if (ParseFloat(TEXT("ApexReplayGroundClearance="), ClearanceM))
	{
		Camera.GroundClearanceCm = ClearanceM * 100.0f;
	}

	// --- sky ---
	Conditions = Clip->GetConditions();
	int32 Minutes = 0;
	if ((FParse::Value(Cmd, TEXT("ApexReplayTimeOfDay="), Text) || FParse::Value(Cmd, TEXT("ApexTimeOfDay="), Text)))
	{
		if (ParseClock(Text, Minutes))
		{
			Conditions.TimeOfDayMinutes = Minutes;
		}
		else
		{
			UE_LOG(LogApexSim, Warning, TEXT("Replay: time of day \"%s\" is not hh:mm"), *Text);
		}
	}
	EApexWeather Weather;
	if ((FParse::Value(Cmd, TEXT("ApexReplayWeather="), Text) || FParse::Value(Cmd, TEXT("ApexWeather="), Text)))
	{
		if (ParseWeather(Text, Weather))
		{
			Conditions.Weather = Weather;
		}
		else
		{
			UE_LOG(LogApexSim, Warning, TEXT("Replay: unknown weather \"%s\""), *Text);
		}
	}

	// --- timing ---
	ParseDouble(TEXT("ApexReplayStart="), StartSeconds);
	ParseDouble(TEXT("ApexReplayDuration="), DurationSeconds);
	ParseDouble(TEXT("ApexReplayPreroll="), PrerollSeconds);
	ParseFloat(TEXT("ApexReplayWarmup="), WarmupSeconds);
	ParseFloat(TEXT("ApexReplayLoadTimeout="), LoadTimeoutSeconds);
	bLoop = FParse::Param(Cmd, TEXT("ApexReplayLoop"));
	bShowUi = FParse::Param(Cmd, TEXT("ApexReplayShowUi"));
	const double ClipSeconds = Clip->GetDurationSeconds();
	StartSeconds = FMath::Clamp(StartSeconds, 0.0, ClipSeconds);
	EndSeconds = DurationSeconds > 0.0 ? FMath::Min(StartSeconds + DurationSeconds, ClipSeconds) : ClipSeconds;
	PrerollSeconds = FMath::Max(PrerollSeconds, 0.0);

	// --- recording ---
	if (!RecordDir.IsEmpty())
	{
		FParse::Value(Cmd, TEXT("ApexReplayFps="), Fps);
		Fps = FMath::Clamp(Fps, 1, 240);
		TotalFrames = FMath::Max(1, FMath::RoundToInt((EndSeconds - StartSeconds) * Fps));
		bLoop = false;
		// A stale frame from an earlier, longer render would end up in the video.
		TArray<FString> Old;
		IFileManager::Get().FindFiles(Old, *(RecordDir / TEXT("frame_*.png")), true, false);
		for (const FString& Name : Old)
		{
			IFileManager::Get().Delete(*(RecordDir / Name));
		}
		IFileManager::Get().Delete(*(RecordDir / TEXT("replay_done.json")));
	}
	if (FParse::Value(Cmd, TEXT("ApexReplayRes="), Text))
	{
		FString W;
		FString H;
		if (Text.Split(TEXT("x"), &W, &H) && W.IsNumeric() && H.IsNumeric())
		{
			Resolution = FIntPoint(FCString::Atoi(*W), FCString::Atoi(*H));
		}
	}
	return true;
}

void UApexReplaySubsystem::Fail(const FString& Why)
{
	UE_LOG(LogApexSim, Error, TEXT("Replay: %s"), *Why);
	Phase = EPhase::Done;
	if (!RecordDir.IsEmpty())
	{
		const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetBoolField(TEXT("ok"), false);
		Root->SetStringField(TEXT("error"), Why);
		FString Json;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
		FJsonSerializer::Serialize(Root, Writer);
		FFileHelper::SaveStringToFile(Json, *(RecordDir / TEXT("replay_done.json")));
	}
	if (bExitWhenDone && !RecordDir.IsEmpty())
	{
		FPlatformMisc::RequestExitWithStatus(false, 3);
	}
}

AApexRaceDirector* UApexReplaySubsystem::GetDirector() const
{
	const UGameInstance* GameInstance = GetGameInstance();
	UWorld* World = GameInstance ? GameInstance->GetWorld() : nullptr;
	return World ? AApexRaceDirector::Find(World) : nullptr;
}

void UApexReplaySubsystem::ApplyResolution()
{
	if (Resolution.X <= 0 || Resolution.Y <= 0 || !GEngine)
	{
		return;
	}
	// Not saved: settings.yml and the save slots keep the player's own mode.
	if (UGameUserSettings* Settings = GEngine->GetGameUserSettings())
	{
		Settings->SetScreenResolution(Resolution);
		Settings->SetFullscreenMode(EWindowMode::Windowed);
		Settings->ApplyResolutionSettings(false);
	}
}

void UApexReplaySubsystem::HideShell()
{
	UWorld* World = GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr;
	if (!World || bShowUi)
	{
		return;
	}
	TArray<UUserWidget*> Widgets;
	UWidgetBlueprintLibrary::GetAllWidgetsOfClass(World, Widgets, UUserWidget::StaticClass(), /*TopLevelOnly*/ true);
	for (UUserWidget* Widget : Widgets)
	{
		if (Widget && Widget->GetVisibility() != ESlateVisibility::Collapsed)
		{
			Widget->SetVisibility(ESlateVisibility::Collapsed);
		}
	}
	if (APlayerController* PlayerController = UGameplayStatics::GetPlayerController(World, 0))
	{
		PlayerController->bShowMouseCursor = false;
	}
}

void UApexReplaySubsystem::RequestFrame()
{
	const FString Path = RecordDir / FString::Printf(TEXT("frame_%06d.png"), FrameIndex);
	// The viewport as drawn this frame, without the UI.
	FScreenshotRequest::RequestScreenshot(Path, /*bShowUI*/ bShowUi, /*bAddFilenameSuffix*/ false);
	++FrameIndex;
}

void UApexReplaySubsystem::WriteManifest()
{
	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetBoolField(TEXT("ok"), true);
	Root->SetStringField(TEXT("clip"), ClipPath);
	Root->SetStringField(TEXT("track"), Clip ? Clip->GetTrackStem() : FString());
	Root->SetNumberField(TEXT("fps"), Fps);
	Root->SetNumberField(TEXT("frames"), FrameIndex);
	Root->SetNumberField(TEXT("start_s"), StartSeconds);
	Root->SetNumberField(TEXT("end_s"), EndSeconds);
	Root->SetStringField(TEXT("camera"), ApexReplayCam::ModeName(Camera.Mode));
	Root->SetStringField(TEXT("conditions"), Conditions.Describe());
	Root->SetNumberField(TEXT("render_seconds"), FPlatformTime::Seconds() - RecordStartedAt);
	FString Json;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
	FJsonSerializer::Serialize(Root, Writer);
	FFileHelper::SaveStringToFile(Json, *(RecordDir / TEXT("replay_done.json")));
}

void UApexReplaySubsystem::Tick(float DeltaTime)
{
	// Waiting, loading and settling are real time: under a fixed step a fast
	// loading frame would otherwise count as a whole frame's worth.
	PhaseSeconds = static_cast<float>(ApexReplayArgs::Now() - PhaseStartedAt);
	++PhaseFrames;
	auto Enter = [this](EPhase Next)
	{
		Phase = Next;
		PhaseSeconds = 0.0f;
		PhaseFrames = 0;
		PhaseStartedAt = ApexReplayArgs::Now();
	};

	switch (Phase)
	{
	case EPhase::WaitWorld:
	{
		AApexRaceDirector* Director = GetDirector();
		if (!Director)
		{
			if (PhaseSeconds > 60.0f)
			{
				Fail(TEXT("no race director in the world (is the menu map L_Menu the default map?)"));
			}
			return;
		}
		ApplyResolution();
		HideShell();
		Director->BeginReplayView(Clip, Camera, Conditions);
		if (!Director->HasReplayTrackLevel())
		{
			UE_LOG(LogApexSim, Warning, TEXT("Replay: the cars will drive in an empty world"));
		}
		Enter(EPhase::Loading);
		return;
	}
	case EPhase::Loading:
	{
		HideShell();
		AApexRaceDirector* Director = GetDirector();
		if (!Director)
		{
			Fail(TEXT("the race director went away"));
			return;
		}
		if (Director->IsReplayReady())
		{
			UE_LOG(LogApexSim, Log, TEXT("Replay: track and sky in after %d frame(s)"), PhaseFrames);
			Enter(EPhase::Warmup);
		}
		else if (!Director->HasReplayTrackLevel() || PhaseSeconds > LoadTimeoutSeconds)
		{
			UE_LOG(LogApexSim, Warning, TEXT("Replay: rolling without a loaded track level"));
			Enter(EPhase::Warmup);
		}
		return;
	}
	case EPhase::Warmup:
	{
		HideShell();
		if (PhaseSeconds < WarmupSeconds)
		{
			return;
		}
		if (AApexRaceDirector* Director = GetDirector())
		{
			Director->PlayReplay(FMath::Max(StartSeconds - PrerollSeconds, 0.0));
		}
		RecordStartedAt = FPlatformTime::Seconds();
		Enter(EPhase::Playing);
		return;
	}
	case EPhase::Playing:
	{
		HideShell();
		AApexRaceDirector* Director = GetDirector();
		if (!Director)
		{
			Fail(TEXT("the race director went away"));
			return;
		}
		const double Now = Director->GetReplayTime();
		if (!RecordDir.IsEmpty())
		{
			// The director moved the clock and the cars in this frame's world
			// tick; this frame is drawn after us, and that draw is the capture.
			if (Now >= StartSeconds - 1.0e-6 && FrameIndex < TotalFrames)
			{
				RequestFrame();
				if (FrameIndex % Fps == 0)
				{
					UE_LOG(LogApexSim, Log, TEXT("Replay: %d / %d frames (%.1f fps)"), FrameIndex, TotalFrames,
						FrameIndex / FMath::Max(FPlatformTime::Seconds() - RecordStartedAt, 0.001));
				}
			}
			if (FrameIndex >= TotalFrames)
			{
				Enter(EPhase::Finishing);
			}
			return;
		}
		if (Now >= EndSeconds)
		{
			if (bLoop)
			{
				Director->PlayReplay(FMath::Max(StartSeconds - PrerollSeconds, 0.0));
			}
			else
			{
				UE_LOG(LogApexSim, Log, TEXT("Replay: end of clip; holding the last frame"));
				Enter(EPhase::Done);
			}
		}
		return;
	}
	case EPhase::Finishing:
		// The last request is written during the draw after it was made.
		if (PhaseFrames < 3)
		{
			return;
		}
		WriteManifest();
		UE_LOG(LogApexSim, Log, TEXT("Replay: wrote %d frame(s) to %s in %.1f s"), FrameIndex, *RecordDir,
			FPlatformTime::Seconds() - RecordStartedAt);
		Enter(EPhase::Done);
		if (bExitWhenDone)
		{
			FPlatformMisc::RequestExit(false);
		}
		return;
	default:
		return;
	}
}
