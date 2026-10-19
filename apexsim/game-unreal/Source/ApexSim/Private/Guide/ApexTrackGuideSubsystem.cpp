#include "Guide/ApexTrackGuideSubsystem.h"

#include "ApexMenuFlowSubsystem.h"
#include "ApexSim.h"
#include "Async/Async.h"
#include "Catalog/ApexCatalogRows.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Race/ApexChaseView.h"
#include "Race/ApexRaceCoordinate.h"
#include "Race/ApexRaceDirector.h"
#include "Race/ApexReplayClip.h"
#include "Track/ApexTrackContentSubsystem.h"

// Named, not anonymous: this module is unity-built.
namespace ApexGuideSub
{
	/** A tripod stands this far over the ground the trace finds. */
	constexpr float TripodClearanceCm = 150.0f;
	/** Longest the track may take to build before the guide plays in what there is. */
	constexpr float BuildTimeoutSeconds = 90.0f;
	/** The world settles (exposure, the first textures) this long before it is revealed. */
	constexpr float RevealDelaySeconds = 0.4f;
	constexpr float RevealPerSecond = 2.5f;
	/** A tripod turns toward the car by at most this share of its field of view. */
	constexpr float PanShareOfFov = 0.3f;

	TAutoConsoleVariable<float> CVarFrameBias(
		TEXT("apexsim.guide.FrameBias"),
		0.10f,
		TEXT("Track guide: how far right of centre the cameras put their subject, as a share of the horizontal field of view (the card covers the left of the screen). 0 centres it."),
		ECVF_Default);

	UApexTrackGuideSubsystem* Find(UWorld* World)
	{
		const UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
		UApexTrackGuideSubsystem* Guide = GameInstance ? GameInstance->GetSubsystem<UApexTrackGuideSubsystem>() : nullptr;
		if (!Guide)
		{
			UE_LOG(LogApexSim, Warning, TEXT("apexsim.guide: no game instance"));
		}
		return Guide;
	}

	FAutoConsoleCommandWithWorldAndArgs OpenCommand(
		TEXT("apexsim.guide.Open"),
		TEXT("Open a track guide: apexsim.guide.Open <Stem> [Class]."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			UApexTrackGuideSubsystem* Guide = Find(World);
			if (!Guide || Args.Num() == 0)
			{
				UE_LOG(LogApexSim, Warning, TEXT("Usage: apexsim.guide.Open <Stem> [Class]"));
				return;
			}
			FString Error;
			if (!Guide->Open(Args[0], Args.Num() > 1 ? Args[1] : FString(), Error))
			{
				UE_LOG(LogApexSim, Warning, TEXT("apexsim.guide.Open: %s"), *Error);
				Guide->OnFailed.Broadcast(Error);
			}
		}));

	FAutoConsoleCommandWithWorldAndArgs NextCommand(TEXT("apexsim.guide.Next"), TEXT("Track guide: the next corner."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>&, UWorld* World)
		{
			if (UApexTrackGuideSubsystem* Guide = Find(World)) { Guide->Next(); }
		}));

	FAutoConsoleCommandWithWorldAndArgs PrevCommand(TEXT("apexsim.guide.Prev"), TEXT("Track guide: the corner before."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>&, UWorld* World)
		{
			if (UApexTrackGuideSubsystem* Guide = Find(World)) { Guide->Previous(); }
		}));

	FAutoConsoleCommandWithWorldAndArgs CornerCommand(TEXT("apexsim.guide.Corner"),
		TEXT("Track guide: jump to a corner by its place in the lap (1 = the first); 0 is the overview."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			UApexTrackGuideSubsystem* Guide = Find(World);
			const int32 Number = Args.Num() > 0 ? FCString::Atoi(*Args[0]) : 0;
			if (Guide && Number <= 0) { Guide->ShowOverview(); }
			else if (Guide) { Guide->GoToCorner(Number, /*bCut*/ true); }
		}));

	FAutoConsoleCommandWithWorldAndArgs CameraCommand(TEXT("apexsim.guide.Camera"),
		TEXT("Track guide: the next camera, or camera N of the cycle (1 = the first)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			UApexTrackGuideSubsystem* Guide = Find(World);
			if (Guide && Args.Num() > 0) { Guide->SetCamera(FCString::Atoi(*Args[0])); }
			else if (Guide) { Guide->CycleCamera(1); }
		}));

	FAutoConsoleCommandWithWorldAndArgs PauseCommand(TEXT("apexsim.guide.Pause"), TEXT("Track guide: pause or resume."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>&, UWorld* World)
		{
			if (UApexTrackGuideSubsystem* Guide = Find(World)) { Guide->TogglePause(); }
		}));

	FAutoConsoleCommandWithWorldAndArgs CloseCommand(TEXT("apexsim.guide.Close"), TEXT("Track guide: back to the menu."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>&, UWorld* World)
		{
			if (UApexTrackGuideSubsystem* Guide = Find(World)) { Guide->Close(); }
		}));

	FAutoConsoleCommandWithWorldAndArgs RescanCommand(TEXT("apexsim.guide.Rescan"), TEXT("Track guide: look for guide files again."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>&, UWorld* World)
		{
			if (UApexTrackGuideSubsystem* Guide = Find(World)) { Guide->Rescan(); }
		}));
}

void UApexTrackGuideSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Collection.InitializeDependency<UApexMenuFlowSubsystem>();
	Collection.InitializeDependency<UApexTrackContentSubsystem>();
	Rescan();
}

void UApexTrackGuideSubsystem::Deinitialize()
{
	State = EApexGuideState::Idle;
	Clip.Reset();
	Super::Deinitialize();
}

TStatId UApexTrackGuideSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UApexTrackGuideSubsystem, STATGROUP_Tickables);
}

TArray<FString> UApexTrackGuideSubsystem::GuideDirectories()
{
	TArray<FString> Dirs;
	FString Override;
	if (FParse::Value(FCommandLine::Get(), TEXT("-ApexGuideDir="), Override))
	{
		TArray<FString> Parts;
		Override.ParseIntoArray(Parts, TEXT("+"), true);
		for (const FString& Part : Parts)
		{
			Dirs.Add(FPaths::ConvertRelativePathToFull(Part));
		}
	}
	// Beside ApexSim.exe in a package (ProjectDir is <Release>/Game/ApexSim/),
	// the repo's build/guide in the editor: the same rule as the tracks.
	const FString Default = FPlatformProperties::RequiresCookedData()
		? FPaths::Combine(FPaths::ProjectDir(), TEXT(".."), TEXT("Guide"))
		: FPaths::Combine(FPaths::ProjectDir(), TEXT(".."), TEXT("build"), TEXT("guide"));
	Dirs.AddUnique(FPaths::ConvertRelativePathToFull(Default));
	return Dirs;
}

void UApexTrackGuideSubsystem::Rescan()
{
	Files.Reset();
	for (const FString& Dir : GuideDirectories())
	{
		TArray<FString> Names;
		IFileManager::Get().FindFiles(Names, *(Dir / (FString(TEXT("*")) + ApexTrackGuide::JsonSuffix())), true, false);
		Names.Sort();
		for (const FString& Name : Names)
		{
			FApexGuideFileInfo Info;
			if (!ApexTrackGuide::ParseFileName(Name, Info.Stem, Info.Class))
			{
				continue;
			}
			// The first folder wins, as for the tracks.
			const bool bKnown = Files.ContainsByPredicate([&Info](const FApexGuideFileInfo& F) {
				return F.Stem.Equals(Info.Stem, ESearchCase::IgnoreCase) && F.Class.Equals(Info.Class, ESearchCase::IgnoreCase);
			});
			if (!bKnown)
			{
				Info.Path = Dir / Name;
				Files.Add(MoveTemp(Info));
			}
		}
	}
	bScanned = true;
	UE_LOG(LogApexSim, Log, TEXT("Track guides: %d file(s) in %s"), Files.Num(), *FString::Join(GuideDirectories(), TEXT(", ")));
}

bool UApexTrackGuideSubsystem::HasGuide(const FString& Stem) const
{
	if (Stem.IsEmpty() || !Files.ContainsByPredicate([&Stem](const FApexGuideFileInfo& F) { return F.Stem.Equals(Stem, ESearchCase::IgnoreCase); }))
	{
		return false;
	}
	const UApexTrackContentSubsystem* Content = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexTrackContentSubsystem>() : nullptr;
	return Content && Content->HasTrack(Stem);
}

const FApexGuideFileInfo* UApexTrackGuideSubsystem::FindGuide(const FString& Stem, const FString& PreferredClass) const
{
	TArray<FString> Classes;
	for (const FApexGuideFileInfo& File : Files)
	{
		if (File.Stem.Equals(Stem, ESearchCase::IgnoreCase))
		{
			Classes.Add(File.Class);
		}
	}
	const FString Class = ApexTrackGuide::ChooseClass(Classes, PreferredClass);
	return Class.IsEmpty() ? nullptr : Files.FindByPredicate([&Stem, &Class](const FApexGuideFileInfo& F) {
		return F.Stem.Equals(Stem, ESearchCase::IgnoreCase) && F.Class.Equals(Class, ESearchCase::IgnoreCase);
	});
}

FString UApexTrackGuideSubsystem::GetPreferredClass() const
{
	const UApexMenuFlowSubsystem* Flow = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexMenuFlowSubsystem>() : nullptr;
	FApexCarCatalogRow Row;
	if (Flow && Flow->HasPendingCar() && Flow->GetCarCatalogRow(Flow->GetPendingCarId(), Row))
	{
		return Row.CarClass;
	}
	return FString();
}

AApexRaceDirector* UApexTrackGuideSubsystem::GetDirector() const
{
	const UGameInstance* GameInstance = GetGameInstance();
	return GameInstance ? AApexRaceDirector::Find(GameInstance->GetWorld()) : nullptr;
}

bool UApexTrackGuideSubsystem::Open(const FString& Stem, const FString& Class, FString& OutError)
{
	if (!bScanned)
	{
		Rescan();
	}
	const FApexGuideFileInfo* File = FindGuide(Stem, Class.IsEmpty() ? GetPreferredClass() : Class);
	if (!File)
	{
		OutError = FString::Printf(TEXT("there is no track guide for %s"), *Stem);
		return false;
	}
	const UApexTrackContentSubsystem* Content = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexTrackContentSubsystem>() : nullptr;
	if (!Content || !Content->HasTrack(File->Stem))
	{
		OutError = TEXT("the circuit is not installed on this machine");
		return false;
	}
	if (!GetDirector())
	{
		OutError = TEXT("the game is not ready");
		return false;
	}
	FString Json;
	if (!FFileHelper::LoadFileToString(Json, *File->Path))
	{
		OutError = FString::Printf(TEXT("cannot read %s"), *File->Path);
		return false;
	}
	FApexTrackGuide Parsed;
	FString Error;
	if (!ApexTrackGuide::Parse(Json, Parsed, Error))
	{
		OutError = FString::Printf(TEXT("%s: %s"), *FPaths::GetCleanFilename(File->Path), *Error);
		return false;
	}
	const FString Recording = FPaths::Combine(FPaths::GetPath(File->Path), Parsed.Recording);
	if (!IFileManager::Get().FileExists(*Recording))
	{
		OutError = FString::Printf(TEXT("the guide's recording %s is missing"), *Parsed.Recording);
		return false;
	}
	if (Parsed.Track.Stem.IsEmpty())
	{
		Parsed.Track.Stem = File->Stem;
	}

	// Made from another version of the circuit: it still plays, and says so.
	if (const UApexMenuFlowSubsystem* Flow = GetGameInstance()->GetSubsystem<UApexMenuFlowSubsystem>())
	{
		FApexTrackCatalogRow Row;
		const FString TrackId = Flow->FindTrackIdByStem(File->Stem);
		if (Parsed.SourceCrc != 0 && !TrackId.IsEmpty() && Flow->GetTrackCatalogRow(TrackId, Row) && Row.SourceCrc != 0
			&& Row.SourceCrc != Parsed.SourceCrc)
		{
			UE_LOG(LogApexSim, Warning, TEXT("Track guide %s was made from another version of the track (crc %lld, here %lld); regenerate it with apexsim-replay guide"),
				*FPaths::GetCleanFilename(File->Path), Parsed.SourceCrc, static_cast<int64>(Row.SourceCrc));
		}
	}

	Close();
	Guide = MoveTemp(Parsed);
	GuidePath = File->Path;
	State = EApexGuideState::Loading;
	StateSeconds = 0.0f;
	Reveal = 0.0f;
	bShownValid = false;
	bKeepCarCamera = false;
	Cameras.Reset();
	CameraIndex = 0;
	Loading = Async(EAsyncExecution::ThreadPool, [Recording]() -> FLoaded
	{
		FLoaded Out;
		TSharedPtr<FApexReplayClip> Loaded = MakeShared<FApexReplayClip>();
		if (Loaded->LoadFromFile(Recording, Out.Error) && Loaded->IsValid())
		{
			Out.Clip = Loaded;
		}
		else if (Out.Error.IsEmpty())
		{
			Out.Error = TEXT("the recording has no frames");
		}
		return Out;
	});
	UE_LOG(LogApexSim, Log, TEXT("Track guide: opening %s (%s, %d corner(s))"), *GuidePath, *Guide.Class, Guide.Corners.Num());
	OnActiveChanged.Broadcast(true);
	return true;
}

void UApexTrackGuideSubsystem::Close()
{
	if (State == EApexGuideState::Idle)
	{
		return;
	}
	State = EApexGuideState::Idle;
	// A load still running finishes into a future nobody reads.
	Loading = TFuture<FLoaded>();
	if (AApexRaceDirector* Director = GetDirector(); Director && Director->IsGuideViewActive())
	{
		Director->EndReplayView();
	}
	Clip.Reset();
	Cameras.Reset();
	Reveal = 0.0f;
	bShownValid = false;
	UE_LOG(LogApexSim, Log, TEXT("Track guide closed"));
	OnActiveChanged.Broadcast(false);
}

void UApexTrackGuideSubsystem::Fail(const FString& Why)
{
	UE_LOG(LogApexSim, Error, TEXT("Track guide: %s"), *Why);
	Close();
	OnFailed.Broadcast(Why);
}

void UApexTrackGuideSubsystem::Tick(float DeltaTime)
{
	StateSeconds += DeltaTime;
	AApexRaceDirector* Director = GetDirector();
	if (!Director)
	{
		Fail(TEXT("the race director went away"));
		return;
	}

	switch (State)
	{
	case EApexGuideState::Loading:
	{
		if (!Loading.IsValid() || !Loading.IsReady())
		{
			return;
		}
		FLoaded Result = Loading.Get();
		Loading = TFuture<FLoaded>();
		if (!Result.Clip.IsValid())
		{
			Fail(FString::Printf(TEXT("cannot play the recording: %s"), *Result.Error));
			return;
		}
		Clip = Result.Clip;
		if (!Clip->GetTrackStem().IsEmpty() && !Clip->GetTrackStem().Equals(Guide.Track.Stem, ESearchCase::IgnoreCase))
		{
			UE_LOG(LogApexSim, Warning, TEXT("Track guide: the recording is of %s, the guide of %s"), *Clip->GetTrackStem(), *Guide.Track.Stem);
		}
		Director->BeginGuideView(Clip, Clip->GetConditions());
		if (!Director->IsGuideViewActive())
		{
			Fail(TEXT("the race director would not play it"));
			return;
		}
		Player.Setup(Guide, Clip->GetDurationSeconds());
		PushClock();
		State = EApexGuideState::Building;
		StateSeconds = 0.0f;
		return;
	}

	case EApexGuideState::Building:
		if (!Director->IsGuideViewActive())
		{
			Fail(TEXT("the guide view was ended"));
			return;
		}
		PushClock();
		if (Director->IsReplayReady())
		{
			StartRunning();
		}
		else if (!Director->HasReplayTrackLevel())
		{
			Fail(TEXT("the circuit could not be built"));
		}
		else if (StateSeconds > ApexGuideSub::BuildTimeoutSeconds)
		{
			UE_LOG(LogApexSim, Warning, TEXT("Track guide: the circuit is taking too long; playing anyway"));
			StartRunning();
		}
		return;

	case EApexGuideState::Running:
		if (!Director->IsGuideViewActive())
		{
			Close();
			return;
		}
		if (StateSeconds > ApexGuideSub::RevealDelaySeconds)
		{
			Reveal = FMath::Min(1.0f, Reveal + DeltaTime * ApexGuideSub::RevealPerSecond);
		}
		if (ApexGuideSub::CVarFrameBias.GetValueOnGameThread() != AppliedFrameBias)
		{
			// Retuned from the console: the camera on screen takes it at once.
			ApplyCamera();
		}
		// A hitch is not a reason to skip half a corner.
		Player.Tick(FMath::Min(DeltaTime, 0.1f));
		UpdateView();
		return;

	default:
		return;
	}
}

void UApexTrackGuideSubsystem::StartRunning()
{
	State = EApexGuideState::Running;
	StateSeconds = 0.0f;
	bShownValid = false;
	if (PendingCorner > 0)
	{
		Player.GoToCorner(PendingCorner - 1, /*bCut*/ true);
		PendingCorner = 0;
	}
	UpdateView();
	if (PendingCamera > 0)
	{
		SetCamera(PendingCamera);
		PendingCamera = 0;
	}
	LogState(TEXT("running"));
}

void UApexTrackGuideSubsystem::PushClock()
{
	if (AApexRaceDirector* Director = GetDirector())
	{
		Director->SetGuideClock(Player.GetTime(), Player.GetRate(), Player.ConsumeCut());
	}
}

void UApexTrackGuideSubsystem::UpdateView()
{
	if (State != EApexGuideState::Running)
	{
		PushClock();
		return;
	}
	const ApexGuide::EPhase Phase = Player.GetPhase();
	// Normal and slow are one view: the corner's.
	const ApexGuide::EPhase ViewPhase = Phase == ApexGuide::EPhase::Slow ? ApexGuide::EPhase::Normal : Phase;
	if (!bShownValid || ViewPhase != ShownPhase || Player.GetCorner() != ShownCorner)
	{
		const bool bArrived = bShownValid && ShownPhase == ApexGuide::EPhase::Travel && ViewPhase == ApexGuide::EPhase::Normal
			&& ShownCorner == Player.GetCorner();
		bShownValid = true;
		ShownPhase = ViewPhase;
		ShownCorner = Player.GetCorner();
		BuildCameras();
		ApplyCamera();
		LogState(bArrived ? TEXT("arrived") : TEXT("view"));
	}
	PushClock();
}

void UApexTrackGuideSubsystem::BuildCameras()
{
	Cameras.Reset();
	const FApexGuideCorner* Corner = GetCorner();
	const bool bTravel = Player.GetPhase() == ApexGuide::EPhase::Travel;
	if (!bTravel)
	{
		const TArray<FApexGuideCamera>& FromFile = Corner ? Corner->Cameras : Guide.OverviewCameras;
		for (const FApexGuideCamera& Source : FromFile)
		{
			FCamera& Camera = Cameras.AddDefaulted_GetRef();
			Camera.Name = Source.Name;
			Camera.Kind = EApexGuideCameraKind::Fixed;
			Camera.EyeCm = ApexRace::ServerToUnrealPosition(Source.EyeM);
			Camera.LookCm = ApexRace::ServerToUnrealPosition(Source.LookM);
			Camera.FovDeg = Source.FovDeg;
			Camera.bChecked = false;
		}
	}
	auto AddOwn = [this](const TCHAR* Name, EApexGuideCameraKind Kind)
	{
		FCamera& Camera = Cameras.AddDefaulted_GetRef();
		Camera.Name = Name;
		Camera.Kind = Kind;
		Camera.bChecked = true;
		Camera.bUsable = true;
	};
	AddOwn(TEXT("Broadcast"), EApexGuideCameraKind::Broadcast);
	AddOwn(TEXT("Chase"), EApexGuideCameraKind::Chase);
	AddOwn(TEXT("Onboard"), EApexGuideCameraKind::Onboard);

	// On the way to a corner the chase camera rides along (or the car camera
	// the player chose); at a corner, its first tripod (or that car camera).
	const EApexGuideCameraKind Wanted = bKeepCarCamera ? KeptKind : bTravel ? EApexGuideCameraKind::Chase : EApexGuideCameraKind::Fixed;
	const int32 Found = Cameras.IndexOfByPredicate([Wanted](const FCamera& C) { return C.Kind == Wanted; });
	CameraIndex = Found != INDEX_NONE ? Found : Cameras.IndexOfByPredicate([](const FCamera& C) { return C.Kind == EApexGuideCameraKind::Chase; });
	if (!bKeepCarCamera && !bTravel && Wanted == EApexGuideCameraKind::Fixed)
	{
		// The first tripod that can see its corner; else the broadcast camera.
		const int32 Usable = StepCamera(0, 0);
		CameraIndex = Usable != INDEX_NONE ? Usable : CameraIndex;
	}
}

bool UApexTrackGuideSubsystem::CheckCamera(FCamera& Camera) const
{
	if (Camera.bChecked)
	{
		return Camera.bUsable;
	}
	const AApexRaceDirector* Director = GetDirector();
	if (!Director)
	{
		return false;
	}
	Camera.bChecked = true;
	Camera.bUsable = Director->CheckTripod(Camera.EyeCm, Camera.LookCm, ApexGuideSub::TripodClearanceCm);
	if (!Camera.bUsable)
	{
		UE_LOG(LogApexSim, Log, TEXT("Track guide: tripod \"%s\" cannot see its corner from here; skipped"), *Camera.Name);
	}
	return Camera.bUsable;
}

int32 UApexTrackGuideSubsystem::StepCamera(int32 From, int32 Delta)
{
	const int32 Count = Cameras.Num();
	if (Count == 0)
	{
		return INDEX_NONE;
	}
	// Delta 0: `From` itself if usable, else the next that is.
	const int32 Step = Delta < 0 ? -1 : 1;
	for (int32 K = Delta == 0 ? 0 : 1; K <= Count; ++K)
	{
		const int32 Index = ((From + K * Step) % Count + Count) % Count;
		FCamera& Camera = Cameras[Index];
		if (Camera.Kind != EApexGuideCameraKind::Fixed || CheckCamera(Camera))
		{
			return Index;
		}
	}
	return INDEX_NONE;
}

void UApexTrackGuideSubsystem::ApplyCamera()
{
	AApexRaceDirector* Director = GetDirector();
	if (!Director || !Cameras.IsValidIndex(CameraIndex))
	{
		return;
	}
	FCamera& Camera = Cameras[CameraIndex];
	if (Camera.Kind == EApexGuideCameraKind::Fixed && !CheckCamera(Camera))
	{
		const int32 Next = StepCamera(CameraIndex, 1);
		if (Next == INDEX_NONE || Next == CameraIndex)
		{
			return;
		}
		CameraIndex = Next;
		ApplyCamera();
		return;
	}

	ApexReplayCam::FSettings Settings;
	Settings.Follow = ApexReplayCam::EFollow::Index;
	Settings.FollowIndex = 0;
	AppliedFrameBias = ApexGuideSub::CVarFrameBias.GetValueOnGameThread();
	// Clear of the card on the left; the onboard view stays as the driver sees it.
	Settings.FrameBias = Camera.Kind == EApexGuideCameraKind::Onboard ? 0.0f : AppliedFrameBias;
	switch (Camera.Kind)
	{
	case EApexGuideCameraKind::Fixed:
		Settings.Mode = ApexReplayCam::EMode::Pan;
		Settings.bHasEye = true;
		Settings.EyeCm = Camera.EyeCm;
		Settings.bHasLook = true;
		Settings.LookCm = Camera.LookCm;
		Settings.FovDeg = Camera.FovDeg;
		Settings.MaxPanDeg = Camera.FovDeg * ApexGuideSub::PanShareOfFov;
		Settings.PanSpeed = 2.5f;
		Settings.LeadSeconds = 0.15f;
		// Seated already, by CheckTripod.
		Settings.GroundClearanceCm = -1.0f;
		break;
	case EApexGuideCameraKind::Broadcast:
		Settings.Mode = ApexReplayCam::EMode::Tv;
		Settings.bLockTv = true;
		break;
	case EApexGuideCameraKind::Chase:
	{
		Settings.Mode = ApexReplayCam::EMode::Chase;
		int32 Near = ApexChase::DefaultLevel();
		ApexChase::FindByName(TEXT("near"), Near);
		Settings.ChaseLevel = Near;
		break;
	}
	case EApexGuideCameraKind::Onboard:
		Settings.Mode = ApexReplayCam::EMode::Cockpit;
		break;
	}
	Director->SetGuideCamera(Settings);
}

void UApexTrackGuideSubsystem::CycleCamera(int32 Delta)
{
	if (State != EApexGuideState::Running || Cameras.Num() == 0)
	{
		return;
	}
	const int32 Next = StepCamera(CameraIndex, Delta < 0 ? -1 : 1);
	if (Next == INDEX_NONE)
	{
		return;
	}
	CameraIndex = Next;
	bKeepCarCamera = Cameras[CameraIndex].Kind != EApexGuideCameraKind::Fixed;
	KeptKind = Cameras[CameraIndex].Kind;
	ApplyCamera();
	LogState(TEXT("camera"));
}

void UApexTrackGuideSubsystem::SetCamera(int32 Number)
{
	if (State != EApexGuideState::Running || Cameras.Num() == 0)
	{
		PendingCamera = Number;
		return;
	}
	const int32 Index = StepCamera(FMath::Clamp(Number - 1, 0, Cameras.Num() - 1), 0);
	if (Index == INDEX_NONE)
	{
		return;
	}
	CameraIndex = Index;
	bKeepCarCamera = Cameras[CameraIndex].Kind != EApexGuideCameraKind::Fixed;
	KeptKind = Cameras[CameraIndex].Kind;
	ApplyCamera();
	LogState(TEXT("camera"));
}

void UApexTrackGuideSubsystem::Next()
{
	if (State == EApexGuideState::Running)
	{
		Player.Next();
		UpdateView();
	}
}

void UApexTrackGuideSubsystem::Previous()
{
	if (State == EApexGuideState::Running && Player.Previous())
	{
		UpdateView();
	}
}

void UApexTrackGuideSubsystem::ShowOverview()
{
	if (State == EApexGuideState::Running)
	{
		Player.ShowOverview();
		UpdateView();
	}
}

void UApexTrackGuideSubsystem::GoToCorner(int32 Number, bool bCut)
{
	if (State != EApexGuideState::Running)
	{
		PendingCorner = Number;
		return;
	}
	if (Number < 1 || Number > Player.NumCorners())
	{
		return;
	}
	Player.GoToCorner(Number - 1, bCut);
	UpdateView();
}

void UApexTrackGuideSubsystem::TogglePause()
{
	if (State == EApexGuideState::Running)
	{
		Player.TogglePause();
		PushClock();
	}
}

void UApexTrackGuideSubsystem::Run(const ApexGuide::FCommand& Command)
{
	using ApexGuide::EAction;
	switch (Command.Action)
	{
	case EAction::Previous: Previous(); break;
	case EAction::Next: Next(); break;
	case EAction::Camera: CycleCamera(1); break;
	case EAction::CameraBack: CycleCamera(-1); break;
	case EAction::Pause: TogglePause(); break;
	case EAction::Overview: ShowOverview(); break;
	case EAction::Corner: GoToCorner(Command.Corner); break;
	case EAction::Leave: Close(); break;
	default: break;
	}
}

const FApexGuideCorner* UApexTrackGuideSubsystem::GetCorner() const
{
	return Guide.Corners.IsValidIndex(Player.GetCorner()) ? &Guide.Corners[Player.GetCorner()] : nullptr;
}

FString UApexTrackGuideSubsystem::GetCameraName() const
{
	return Cameras.IsValidIndex(CameraIndex) ? Cameras[CameraIndex].Name : FString();
}

FString UApexTrackGuideSubsystem::GetCarName() const
{
	if (!Guide.CarName.IsEmpty())
	{
		return Guide.CarName;
	}
	const UApexMenuFlowSubsystem* Flow = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexMenuFlowSubsystem>() : nullptr;
	FApexCarCatalogRow Row;
	return Flow && Flow->GetCarCatalogRow(Guide.CarId, Row) ? Row.DisplayName : FString();
}

void UApexTrackGuideSubsystem::ApplyCommandLine()
{
	if (bCommandLineApplied)
	{
		return;
	}
	FString Requested;
	if (!FParse::Value(FCommandLine::Get(), TEXT("ApexGuide="), Requested) || Requested.IsEmpty())
	{
		bCommandLineApplied = true;
		return;
	}
	if (!GetDirector())
	{
		// The menu world is not up yet; asked again next frame.
		return;
	}
	bCommandLineApplied = true;
	FString Stem = Requested;
	FString Class;
	Requested.Split(TEXT(":"), &Stem, &Class);
	FParse::Value(FCommandLine::Get(), TEXT("ApexGuideCorner="), PendingCorner);
	FParse::Value(FCommandLine::Get(), TEXT("ApexGuideCamera="), PendingCamera);
	FString Error;
	UE_LOG(LogApexSim, Log, TEXT("-ApexGuide=%s: opening the track guide"), *Requested);
	if (!Open(Stem, Class, Error))
	{
		UE_LOG(LogApexSim, Warning, TEXT("-ApexGuide=%s: %s"), *Requested, *Error);
		OnFailed.Broadcast(Error);
	}
}

void UApexTrackGuideSubsystem::LogState(const TCHAR* What) const
{
	const FApexGuideCorner* Corner = GetCorner();
	UE_LOG(LogApexSim, Log, TEXT("Track guide %s: %s %s, %.2f s, camera %d/%d %s"), What,
		Corner ? *ApexTrackGuide::TurnLabel(*Corner) : TEXT("overview"), ApexGuide::PhaseName(Player.GetPhase()),
		Player.GetTime(), CameraIndex + 1, Cameras.Num(), *GetCameraName());
}
