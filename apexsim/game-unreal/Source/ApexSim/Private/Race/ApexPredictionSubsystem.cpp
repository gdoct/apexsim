#include "Race/ApexPredictionSubsystem.h"

#include "ApexNetSubsystem.h"
#include "ApexSim.h"
#include "ApexRepoPaths.h"
#include "ApexUdpConnection.h"
#include "Async/Async.h"
#include "Cars/ApexCarContentSubsystem.h"
#include "Catalog/ApexContentCrc.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Race/ApexRaceCoordinate.h"

namespace
{
	TAutoConsoleVariable<int32> CVarPredict(TEXT("apexsim.net.Predict"), 0,
		TEXT("Client prediction of the local car: 0 off, 1 on (drawn from the predicted state), ")
		TEXT("2 shadow (the loop runs and logs, the puppet is drawn). Command line: -ApexPredict[=N]."));

	TAutoConsoleVariable<int32> CVarPredictServerDriven(TEXT("apexsim.net.PredictServerDriven"), 0,
		TEXT("Draw the prediction while the server drives the car (the debug stand-in, an away driver): ")
		TEXT("the replay holds the controls the server last applied. For unattended checks of the loop."));

	TAutoConsoleVariable<bool> CVarPredictDebug(TEXT("apexsim.net.PredictDebug"), false,
		TEXT("An on-screen line with the prediction's state: lead, round trip, rewinds, replay depth, correction."));

	TAutoConsoleVariable<float> CVarPredictLogSeconds(TEXT("apexsim.net.PredictLogSeconds"), 5.0f,
		TEXT("How often the prediction logs its figures while it runs, s (0: never)."));

	/** Snapshots older than this, s: the stream stopped and the puppet takes over. */
	constexpr double SnapshotTimeoutSeconds = 0.5;

	/** Where the track YAMLs and their sidecars are: -ApexContentDir, else the content repo (`apexsim-content`, editor). */
	FString ContentRoot()
	{
		FString Override;
		if (FParse::Value(FCommandLine::Get(), TEXT("-ApexContentDir="), Override))
		{
			return FPaths::ConvertRelativePathToFull(Override);
		}
		return ApexRepoPaths::ContentRoot();
	}

	FString CarTomlPath(const FString& CarId)
	{
		const UApexCarContentSubsystem* Cars = UApexCarContentSubsystem::Get();
		const FApexCarCatalogRow* Row = Cars ? Cars->FindRow(CarId) : nullptr;
		if (!Row || Row->FolderName.IsEmpty())
		{
			return FString();
		}
		for (const FString& Dir : UApexCarContentSubsystem::CarDirectories())
		{
			for (const FString& Folder : UApexCarContentSubsystem::CarFolders(Dir))
			{
				const FString Path = FPaths::Combine(Folder, Row->FolderName, TEXT("car.toml"));
				if (IFileManager::Get().FileExists(*Path))
				{
					return Path;
				}
			}
		}
		return FString();
	}

	FString Hex(int64 Crc)
	{
		return FString::Printf(TEXT("%08X"), static_cast<uint32>(Crc & 0xFFFFFFFF));
	}
}

void UApexPredictionSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Collection.InitializeDependency<UApexNetSubsystem>();

	int32 Mode = 1;
	if (FParse::Param(FCommandLine::Get(), TEXT("ApexPredict")) || FParse::Value(FCommandLine::Get(), TEXT("-ApexPredict="), Mode))
	{
		CVarPredict->Set(FMath::Clamp(Mode, 0, 2), ECVF_SetByCommandline);
	}
	FString Why;
	UE_LOG(LogApexSim, Log, TEXT("Prediction: %s; apexsim.net.Predict %d"),
		ApexPhysics::IsAvailable(&Why) ? TEXT("the physics library is linked") : *Why, CVarPredict.GetValueOnGameThread());

	if (UApexNetSubsystem* Net = GetGameInstance()->GetSubsystem<UApexNetSubsystem>())
	{
		RoadStateHandle = Net->OnRoadState.AddUObject(this, &UApexPredictionSubsystem::HandleRoadState);
		TrackBundleHandle = Net->OnTrackBundleChunk.AddUObject(this, &UApexPredictionSubsystem::HandleTrackBundleChunk);
	}
	TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateUObject(this, &UApexPredictionSubsystem::Tick), 0.0f);
}

void UApexPredictionSubsystem::Deinitialize()
{
	if (TickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
		TickerHandle.Reset();
	}
	if (UApexNetSubsystem* Net = GetGameInstance()->GetSubsystem<UApexNetSubsystem>())
	{
		Net->OnRoadState.Remove(RoadStateHandle);
		Net->OnTrackBundleChunk.Remove(TrackBundleHandle);
	}
	Teardown(FString());
	if (bLoading)
	{
		// The load holds nothing of ours; let it finish before the module goes.
		PendingTrack.Wait();
		bLoading = false;
	}
	Super::Deinitialize();
}

void UApexPredictionSubsystem::SetStatus(const FString& NewStatus, bool bWarn)
{
	if (NewStatus == Status)
	{
		return;
	}
	Status = NewStatus;
	if (!Status.IsEmpty())
	{
		if (bWarn)
		{
			UE_LOG(LogApexSim, Warning, TEXT("Prediction: %s"), *Status);
		}
		else
		{
			UE_LOG(LogApexSim, Log, TEXT("Prediction: %s"), *Status);
		}
	}
}

void UApexPredictionSubsystem::Teardown(const FString& Why)
{
	if (Predictor.IsRunning())
	{
		LogWindow(FPlatformTime::Seconds(), true);
	}
	Predictor.End();
	Track.Reset();
	// A load in flight finishes on its own; its result is dropped with the future.
	PendingTrack = TFuture<FLoadResult>();
	bLoading = false;
	Download = FApexTrackBundleDownload();
	bDownloading = false;
	FeltTick = -1;
	LoadedKey.Reset();
	PendingRoad.Reset();
	bRoadRefused = false;
	LocalCarIndex = -1;
	bInUse = false;
	bDrawable = false;
	if (!Why.IsEmpty())
	{
		SetStatus(Why, false);
	}
}

TArray<FString> ApexTrackBundleCache::ToPrune(TArray<FApexCachedBundle> Files, const FString& Keep, int64 MaxCacheBytes)
{
	Files.Sort([](const FApexCachedBundle& A, const FApexCachedBundle& B) { return A.Used > B.Used; });
	int64 Total = 0;
	for (const FApexCachedBundle& File : Files)
	{
		if (File.Path == Keep)
		{
			Total += File.Bytes;
		}
	}
	TArray<FString> Gone;
	for (const FApexCachedBundle& File : Files)
	{
		if (File.Path == Keep)
		{
			continue;
		}
		if (Total + File.Bytes > MaxCacheBytes)
		{
			Gone.Add(File.Path);
			continue;
		}
		Total += File.Bytes;
	}
	return Gone;
}

void ApexTrackBundleCache::Prune(const FString& Keep)
{
	IFileManager& Files = IFileManager::Get();
	const FString Dir = FPaths::GetPath(Keep);
	TArray<FString> Names;
	Files.FindFiles(Names, *FPaths::Combine(Dir, TEXT("*.apxtrack")), true, false);
	TArray<FApexCachedBundle> Cached;
	for (const FString& Name : Names)
	{
		FApexCachedBundle& File = Cached.AddDefaulted_GetRef();
		File.Path = FPaths::Combine(Dir, Name);
		File.Bytes = Files.FileSize(*File.Path);
		File.Used = Files.GetTimeStamp(*File.Path);
	}
	for (const FString& Path : ToPrune(MoveTemp(Cached), Keep))
	{
		UE_LOG(LogApexSim, Log, TEXT("Prediction: the track bundle cache is over %lld MB; deleting %s"),
			MaxBytes / (1024 * 1024), *FPaths::GetCleanFilename(Path));
		Files.Delete(*Path, false, true, true);
	}
}

FString UApexPredictionSubsystem::BundleCachePath(uint32 Crc)
{
	return FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("ApexSim"), TEXT("TrackBundles"), Hex(Crc) + TEXT(".apxtrack")));
}

bool UApexPredictionSubsystem::StartLoad(const FString& Key, const FString& TrackFile, const FString& TrackId,
	const FString& CarId)
{
	UApexNetSubsystem* Net = GetGameInstance()->GetSubsystem<UApexNetSubsystem>();
	const FApexSessionPhysics& Physics = Net->GetSessionPhysics();

	// The track: the YAML the server simulates. A server that serves its
	// files as a bundle names their checksum, YAML and sidecars together:
	// this client's own copy is used when it is the same, else the bundle
	// (cached, or fetched). From an older server only this client's copy,
	// its YAML checked against the lobby's checksum.
	const FString YamlPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(ContentRoot(), TrackFile));
	const FString Stem = FPaths::GetBaseFilename(YamlPath);
	const bool bLocalYaml = IFileManager::Get().FileExists(*YamlPath);
	BundleCrc = Physics.BundleCrc;
	BundleBytes = Physics.BundleBytes;
	BundleChunks = Physics.BundleChunks;
	if (BundleCrc == 0)
	{
		TArray<uint8> Yaml;
		if (!bLocalYaml || !FFileHelper::LoadFileToArray(Yaml, *YamlPath))
		{
			SetStatus(FString::Printf(TEXT("off: this client has no physics copy of %s (looked for %s) and the server serves no track bundle (an older server)"),
				*TrackFile, *YamlPath), true);
			return false;
		}
		FApexTrackConfigSummary TrackSummary;
		const int64 ServerTrackCrc = Net->FindTrackById(TrackId, TrackSummary) ? TrackSummary.ContentCrc : 0;
		const uint32 LocalTrackCrc = ApexContentCrc::Compute(Yaml);
		switch (ApexContent::Compare(LocalTrackCrc, ServerTrackCrc))
		{
		case EApexContentMatch::Mismatch:
			SetStatus(FString::Printf(TEXT("off: %s differs from the server's (content checksum here %s, server %s)"),
				*FPaths::GetCleanFilename(YamlPath), *Hex(LocalTrackCrc), *Hex(ServerTrackCrc)), true);
			return false;
		case EApexContentMatch::Unknown:
			UE_LOG(LogApexSim, Warning, TEXT("Prediction: the server names no checksum for %s; predicting on this client's copy"),
				*FPaths::GetCleanFilename(YamlPath));
			break;
		default:
			break;
		}
		if (Physics.RoadContact == 1
			&& !IFileManager::Get().FileExists(*FPaths::Combine(FPaths::GetPath(YamlPath), Stem + TEXT(".road.msgpack"))))
		{
			SetStatus(FString::Printf(TEXT("off: the server drives %s's road mesh and this client has no %s.road.msgpack beside its YAML"),
				*Stem, *Stem), true);
			return false;
		}
	}

	// The car, from the same folders the game builds it from.
	const FString TomlPath = CarTomlPath(CarId);
	CarToml.Reset();
	if (TomlPath.IsEmpty() || !FFileHelper::LoadFileToArray(CarToml, *TomlPath))
	{
		SetStatus(FString::Printf(TEXT("off: this client has no car.toml for car %s"), *CarId), true);
		return false;
	}
	FApexCarConfigSummary CarSummary;
	const int64 ServerCarCrc = Net->FindCarById(CarId, CarSummary) ? CarSummary.ContentCrc : 0;
	const uint32 LocalCarCrc = ApexContentCrc::Compute(CarToml);
	if (ApexContent::Compare(LocalCarCrc, ServerCarCrc) == EApexContentMatch::Mismatch)
	{
		SetStatus(FString::Printf(TEXT("off: %s differs from the server's (content checksum here %s, server %s)"),
			*TomlPath, *Hex(LocalCarCrc), *Hex(ServerCarCrc)), true);
		return false;
	}

	LoadedKey = Key;
	CarName = CarSummary.Name.IsEmpty() ? CarId : CarSummary.Name;
	TrackName = Stem;
	TickRateHz = Physics.TickRateHz;
	RoadContact = Physics.RoadContact;
	Balance.Reset();
	if (Physics.bBalanced)
	{
		Balance = ApxBalance{ Physics.BallastKg, Physics.Power, Physics.Downforce };
	}
	GridPosition = FMath::Max(Net->GetSessionGridPosition(), 1);

	// The track's load (the road mesh is megabytes) runs off the game thread;
	// the handle is the game thread's from when it comes back.
	TArray<uint8> Conditions = Net->GetSessionConditionsBytes();
	const uint8 Contact = static_cast<uint8>(RoadContact);
	const uint32 Crc = BundleCrc;
	const FString CachePath = BundleCachePath(Crc);
	PendingTrack = Async(EAsyncExecution::ThreadPool, [YamlPath, bLocalYaml, Contact, Crc, CachePath, Conditions = MoveTemp(Conditions)]()
	{
		FLoadResult Result;
		FString Error;
		auto Keep = [&Result](TUniquePtr<ApexPhysics::FTrack> Loaded, const TCHAR* Source)
		{
			Result.Track = TSharedPtr<ApexPhysics::FTrack, ESPMode::ThreadSafe>(Loaded.Release());
			Result.Source = Source;
		};
		// This client's own files, when they are the server's (or the server
		// names no bundle, and the YAML was checked above).
		uint32 LocalCrc = 0;
		if (bLocalYaml && (Crc == 0 || (ApexPhysics::TrackFilesCrc(YamlPath, Contact, LocalCrc, Error) && LocalCrc == Crc)))
		{
			if (TUniquePtr<ApexPhysics::FTrack> Loaded = ApexPhysics::FTrack::Load(YamlPath, Contact, Conditions, Error))
			{
				Keep(MoveTemp(Loaded), TEXT("this client's files"));
			}
			else
			{
				UE_LOG(LogApexSim, Warning, TEXT("Prediction: the track did not load: %s"), *Error);
			}
			return Result;
		}
		if (bLocalYaml && Crc != 0)
		{
			UE_LOG(LogApexSim, Log, TEXT("Prediction: this client's %s and its sidecars are not the server's (checksum here %s, server %s); using the server's bundle"),
				*FPaths::GetCleanFilename(YamlPath), *Hex(LocalCrc), *Hex(Crc));
		}
		// The bundle, cached from an earlier session.
		TArray<uint8> Cached;
		if (FFileHelper::LoadFileToArray(Cached, *CachePath, FILEREAD_Silent))
		{
			if (TUniquePtr<ApexPhysics::FTrack> Loaded = ApexPhysics::FTrack::LoadBundle(Cached, Crc, Contact, Conditions, Error))
			{
				Keep(MoveTemp(Loaded), TEXT("the cached bundle"));
				// Used now: the cache drops the least recently used first.
				IFileManager::Get().SetTimeStamp(*CachePath, FDateTime::UtcNow());
				return Result;
			}
			UE_LOG(LogApexSim, Warning, TEXT("Prediction: the cached bundle %s was refused (%s); fetching it again"), *CachePath, *Error);
			IFileManager::Get().Delete(*CachePath, false, true, true);
		}
		Result.bNeedBundle = true;
		return Result;
	});
	bLoading = true;
	SetStatus(FString::Printf(TEXT("loading %s (%s, %d Hz) for %s"), *Stem,
		RoadContact == 1 ? TEXT("road mesh") : TEXT("centerline"), TickRateHz, *CarName), false);
	return true;
}

void UApexPredictionSubsystem::CreateCar()
{
	UApexNetSubsystem* Net = GetGameInstance()->GetSubsystem<UApexNetSubsystem>();
	CarSetup = Net->GetLastSentCarSetup();
	TArray<int8> Clicks;
	for (const int32 Click : CarSetup.Clicks)
	{
		Clicks.Add(static_cast<int8>(FMath::Clamp(Click, -128, 127)));
	}
	FString Error;
	TUniquePtr<ApexPhysics::FCar> Car =
		ApexPhysics::FCar::Create(*Track, CarToml, Clicks, GridPosition, Error, Balance.IsSet() ? &Balance.GetValue() : nullptr);
	if (!Car)
	{
		FailedKey = LoadedKey;
		Teardown(FString::Printf(TEXT("off: the car did not load: %s"), *Error));
		return;
	}
	Predictor.Begin(Track.Get(), MoveTemp(Car), TickRateHz);
	SnapshotSerial = Net->GetOwnCarSnapshotSerial();
	StateSerial = Net->GetOwnCarStateSerial();
	LastSentSeq = Net->GetLastInputSeq();
	SetStatus(FString::Printf(TEXT("ready: %s (%s, %d Hz), %s, %d knob(s) off stock; waiting for the car's state"),
		*TrackName, RoadContact == 1 ? TEXT("road mesh") : TEXT("centerline"), TickRateHz, *CarName,
		CarSetup.CountChanged()), false);
}

void UApexPredictionSubsystem::StartBundleLoad(TArray<uint8> Bundle)
{
	UApexNetSubsystem* Net = GetGameInstance()->GetSubsystem<UApexNetSubsystem>();
	TArray<uint8> Conditions = Net->GetSessionConditionsBytes();
	const uint8 Contact = static_cast<uint8>(RoadContact);
	const uint32 Crc = BundleCrc;
	const FString CachePath = BundleCachePath(Crc);
	PendingTrack = Async(EAsyncExecution::ThreadPool,
		[Contact, Crc, CachePath, Bundle = MoveTemp(Bundle), Conditions = MoveTemp(Conditions)]()
	{
		FLoadResult Result;
		FString Error;
		TUniquePtr<ApexPhysics::FTrack> Loaded = ApexPhysics::FTrack::LoadBundle(Bundle, Crc, Contact, Conditions, Error);
		if (!Loaded)
		{
			UE_LOG(LogApexSim, Warning, TEXT("Prediction: the server's track bundle %s was refused: %s"), *Hex(Crc), *Error);
			return Result;
		}
		// Kept by its checksum for the next session on this track.
		if (!FFileHelper::SaveArrayToFile(Bundle, *CachePath))
		{
			UE_LOG(LogApexSim, Warning, TEXT("Prediction: could not cache the track bundle at %s"), *CachePath);
		}
		else
		{
			ApexTrackBundleCache::Prune(CachePath);
		}
		Result.Track = TSharedPtr<ApexPhysics::FTrack, ESPMode::ThreadSafe>(Loaded.Release());
		Result.Source = TEXT("the server's bundle");
		return Result;
	});
	bLoading = true;
}

void UApexPredictionSubsystem::HandleTrackBundleChunk(const FApexTrackBundleChunk& Chunk)
{
	if (bDownloading)
	{
		Download.OnChunk(Chunk, FPlatformTime::Seconds());
	}
}

void FApexTrackBundleDownload::Begin(uint32 InCrc, int32 InChunks, int64 InBytes, double NowSeconds)
{
	*this = FApexTrackBundleDownload();
	Crc = InCrc;
	Chunks = FMath::Max(InChunks, 0);
	ExpectedBytes = InBytes;
	StartSeconds = NowSeconds;
	LastHeardSeconds = NowSeconds;
	TokensSeconds = NowSeconds;
	Received.SetNum(Chunks);
	if (Chunks == 0)
	{
		Failure = TEXT("the server names a bundle of no chunks");
	}
}

void FApexTrackBundleDownload::Pump(double NowSeconds, TFunctionRef<void(uint32, int32)> Send)
{
	if (!IsActive())
	{
		return;
	}
	if (NowSeconds - LastHeardSeconds > StallSeconds)
	{
		Failure = FString::Printf(TEXT("no chunk for %.0f s (%d of %d heard)"), StallSeconds, Heard, Chunks);
		return;
	}
	// A token bucket under the server's control-message rate, so asking for
	// the bundle never crowds out the session's own messages.
	Tokens = FMath::Min(RequestBurst, Tokens + (NowSeconds - TokensSeconds) * RequestsPerSecond);
	TokensSeconds = NowSeconds;
	while (NextToAsk < Chunks && InFlight < MaxInFlight && Tokens >= 1.0)
	{
		Send(Crc, NextToAsk++);
		++InFlight;
		Tokens -= 1.0;
	}
}

bool FApexTrackBundleDownload::OnChunk(const FApexTrackBundleChunk& Chunk, double NowSeconds)
{
	if (!IsActive() || Chunk.Crc != Crc)
	{
		return !HasFailed();
	}
	LastHeardSeconds = NowSeconds;
	if (Chunk.Chunks == 0)
	{
		Failure = TEXT("the server no longer has it");
		return false;
	}
	if (Chunk.Chunks != Chunks || !Received.IsValidIndex(Chunk.Chunk))
	{
		Failure = FString::Printf(TEXT("chunk %d of %d does not fit a bundle of %d"), Chunk.Chunk, Chunk.Chunks, Chunks);
		return false;
	}
	InFlight = FMath::Max(0, InFlight - 1);
	if (Received[Chunk.Chunk].Num() == 0 && Chunk.Bytes.Num() > 0)
	{
		Received[Chunk.Chunk] = Chunk.Bytes;
		++Heard;
	}
	return true;
}

TArray<uint8> FApexTrackBundleDownload::Join() const
{
	TArray<uint8> Out;
	if (!IsDone())
	{
		return Out;
	}
	Out.Reserve(static_cast<int32>(ExpectedBytes));
	for (const TArray<uint8>& Part : Received)
	{
		Out.Append(Part);
	}
	if (ExpectedBytes > 0 && Out.Num() != ExpectedBytes)
	{
		Out.Reset();
	}
	return Out;
}

void UApexPredictionSubsystem::HandleRoadState(const FApexRoadState& Road)
{
	if (Track.IsValid())
	{
		ApplyRoadState(Road);
	}
	else if ((bLoading || bDownloading) && PendingRoad.Num() < 256)
	{
		PendingRoad.Add(Road);
	}
}

void UApexPredictionSubsystem::ApplyRoadState(const FApexRoadState& Road)
{
	if (bRoadRefused || !Road.IsValid())
	{
		return;
	}
	FString Error;
	if (!Track->SetRoadRows(static_cast<uint32>(FMath::Max(Road.FirstCell, 0)), Road.Rows, Road.SnowRows, Road.LaneSnow, Error))
	{
		// A track with no road state, or rows of another shape: say it once.
		bRoadRefused = true;
		UE_LOG(LogApexSim, Warning, TEXT("Prediction: the road state was refused (%s); the road grips as loaded"), *Error);
	}
}

void UApexPredictionSubsystem::LogWindow(double NowSeconds, bool bForce)
{
	const float Every = CVarPredictLogSeconds.GetValueOnGameThread();
	if (!bForce && (Every <= 0.0f || NowSeconds - WindowStartSeconds < Every))
	{
		return;
	}
	const FApexPredictionStats Window = Predictor.TakeStats();
	if (WindowFrames > 0 && Predictor.IsSeated())
	{
		const TCHAR* Mode = CVarPredict.GetValueOnGameThread() == 2 ? TEXT("shadow") : TEXT("on");
		UE_LOG(LogApexSim, Log,
			TEXT("Prediction (%s, %.1f s): %s; drawn %d%% of %d frames%s%s; the puppet trails it by %.2f m (%.0f ms); rtt %.0f ms, align %+.1f ticks%s"),
			Mode, NowSeconds - WindowStartSeconds, *Window.Describe(TickRateHz),
			FMath::RoundToInt(100.0f * WindowDrawnFrames / WindowFrames), WindowFrames,
			LastWhyNotDrawn.IsEmpty() ? TEXT("") : TEXT(", else "), *LastWhyNotDrawn,
			WindowGapSamples > 0 ? WindowGapSumM / WindowGapSamples : 0.0,
			WindowGapSamples > 0 ? WindowGapSumMs / WindowGapSamples : 0.0,
			Predictor.GetRttSeconds() * 1000.0, Predictor.GetAlignTicks(), Predictor.IsServerDriven() ? TEXT(", the server drives") : TEXT(""));
		if (WindowFeelSamples > 0 || WindowRpmSamples > 0)
		{
			UE_LOG(LogApexSim, Log,
				TEXT("Prediction (%s): the steering torque felt %.0f ms before the server's (%d samples, off it by %.3f on average); the revs %.0f ms before (%d samples, off by %.0f rpm)"),
				Mode, WindowFeelSamples > 0 ? WindowFeelLeadMs / WindowFeelSamples : 0.0, WindowFeelSamples,
				WindowFeelSamples > 0 ? WindowFeelTorqueError / WindowFeelSamples : 0.0,
				WindowRpmSamples > 0 ? WindowRpmLeadMs / WindowRpmSamples : 0.0, WindowRpmSamples,
				WindowRpmSamples > 0 ? WindowRpmError / WindowRpmSamples : 0.0);
		}
	}
	WindowFeelLeadMs = 0.0;
	WindowFeelTorqueError = 0.0;
	WindowFeelSamples = 0;
	WindowRpmLeadMs = 0.0;
	WindowRpmError = 0.0;
	WindowRpmSamples = 0;
	WindowStartSeconds = NowSeconds;
	WindowFrames = 0;
	WindowDrawnFrames = 0;
	WindowGapSumM = 0.0;
	WindowGapSumMs = 0.0;
	WindowGapSamples = 0;
	LastWhyNotDrawn.Reset();
}

bool UApexPredictionSubsystem::Tick(float DeltaSeconds)
{
	const int32 Mode = CVarPredict.GetValueOnGameThread();
	UApexNetSubsystem* Net = GetGameInstance()->GetSubsystem<UApexNetSubsystem>();
	bInUse = false;
	if (Mode <= 0 || !Net)
	{
		if (!LoadedKey.IsEmpty() || bLoading)
		{
			Teardown(TEXT("off (apexsim.net.Predict 0)"));
		}
		return true;
	}
	FString Why;
	if (!ApexPhysics::IsAvailable(&Why))
	{
		SetStatus(FString::Printf(TEXT("unavailable: %s"), *Why), true);
		return true;
	}
	if (!Net->IsInSession() || Net->IsSessionSpectator() || Net->IsInDemoSession() || Net->IsHotlapWatch())
	{
		if (!LoadedKey.IsEmpty() || bLoading)
		{
			Teardown(TEXT("idle: no car of our own to predict"));
		}
		return true;
	}
	const FApexSessionPhysics& Physics = Net->GetSessionPhysics();
	if (!Physics.bValid)
	{
		Teardown(FString());
		SetStatus(TEXT("off: the server sends no SessionJoined.Physics (an older server)"), true);
		return true;
	}

	// What there is to predict: the session's track and our car in it.
	const int32 CarIndex = Net->GetLocalCarIndex();
	FApexSessionSummary Session;
	if (CarIndex < 0 || !Net->FindSessionById(Net->GetCurrentSessionId(), Session) || Session.TrackFile.IsEmpty())
	{
		return true;
	}
	FString CarId;
	for (const FApexRosterEntry& Entry : Net->GetSessionRoster().Entries)
	{
		if (Entry.CarIndex == CarIndex)
		{
			CarId = Entry.CarConfigId;
			break;
		}
	}
	if (CarId.IsEmpty())
	{
		return true;
	}
	const FString Key = FString::Printf(TEXT("%s|%s|%s|%d|%d|%d|%08X"), *Net->GetCurrentSessionId(), *Session.TrackFile, *CarId,
		Physics.TickRateHz, Physics.RoadContact, CarIndex, Physics.BundleCrc);
	if (Key != LoadedKey)
	{
		Teardown(FString());
		if (Key == FailedKey || !StartLoad(Key, Session.TrackFile, Session.TrackId, CarId))
		{
			FailedKey = Key;
			return true;
		}
		FailedKey.Reset();
	}
	LocalCarIndex = CarIndex;

	if (bDownloading)
	{
		// The track's bundle, a chunk at a time; prediction starts once it is here.
		const double DownloadNow = FPlatformTime::Seconds();
		Download.Pump(DownloadNow, [Net](uint32 Crc, int32 Chunk) { Net->RequestTrackBundleChunk(Crc, Chunk); });
		if (Download.HasFailed())
		{
			FailedKey = LoadedKey;
			Teardown(FString::Printf(TEXT("off: the track bundle %s did not come: %s"), *Hex(BundleCrc), *Download.Failure));
			return true;
		}
		if (!Download.IsDone())
		{
			return true;
		}
		TArray<uint8> Bundle = Download.Join();
		bDownloading = false;
		if (Bundle.Num() == 0)
		{
			FailedKey = LoadedKey;
			Teardown(FString::Printf(TEXT("off: the track bundle %s came short of its %lld bytes"), *Hex(BundleCrc), BundleBytes));
			return true;
		}
		UE_LOG(LogApexSim, Log, TEXT("Prediction: the track bundle %s came: %.2f MB in %d chunks, %.2f s after it was asked for"),
			*Hex(BundleCrc), Bundle.Num() / 1.0e6, Download.Chunks, DownloadNow - Download.StartSeconds);
		StartBundleLoad(MoveTemp(Bundle));
		return true;
	}
	if (bLoading)
	{
		if (!PendingTrack.IsReady())
		{
			return true;
		}
		FLoadResult Loaded = PendingTrack.Get();
		PendingTrack = TFuture<FLoadResult>();
		bLoading = false;
		if (Loaded.bNeedBundle)
		{
			// Neither this client's files nor a cached bundle: the server's.
			Download.Begin(BundleCrc, BundleChunks, BundleBytes, FPlatformTime::Seconds());
			bDownloading = true;
			SetStatus(FString::Printf(TEXT("fetching the track bundle %s from the server (%.2f MB in %d chunks)"),
				*Hex(BundleCrc), BundleBytes / 1.0e6, BundleChunks), false);
			return true;
		}
		Track = Loaded.Track;
		if (!Track.IsValid())
		{
			FailedKey = LoadedKey;
			Teardown(TEXT("off: the track did not load (see the warning above)"));
			return true;
		}
		UE_LOG(LogApexSim, Log, TEXT("Prediction: %s loaded from %s"), *TrackName, *Loaded.Source);
		for (const FApexRoadState& Road : PendingRoad)
		{
			ApplyRoadState(Road);
		}
		PendingRoad.Reset();
		CreateCar();
		if (!Predictor.IsRunning())
		{
			return true;
		}
	}
	if (Net->GetLastSentCarSetup() != CarSetup)
	{
		// The garage moved a knob: the server simulates the new setup from
		// the next tick, so the car is made again and seats on the next blob.
		UE_LOG(LogApexSim, Log, TEXT("Prediction: the setup changed; the car is made again"));
		CreateCar();
		if (!Predictor.IsRunning())
		{
			return true;
		}
	}

	// What came in this frame, then the step.
	const double Now = FPlatformTime::Seconds();
	TArray<FApexSentInput> Sent;
	Net->CopySentInputs(LastSentSeq, Sent);
	if (Sent.Num() > 0)
	{
		LastSentSeq = Sent.Last().Seq;
		Predictor.AddSentInputs(Sent, Now);
	}
	if (Net->GetOwnCarStateSerial() != StateSerial)
	{
		StateSerial = Net->GetOwnCarStateSerial();
		const FApexOwnCarStateAssembler& State = Net->GetOwnCarState();
		const bool bFirst = !Predictor.IsSeated();
		Predictor.OnStateBlob(State.Tick, State.Blob, Now);
		if (bFirst && Predictor.IsSeated())
		{
			SetStatus(FString::Printf(TEXT("running: %s at %s, %d Hz; seated on the server's state of tick %lld"),
				*CarName, *TrackName, TickRateHz, State.Tick), false);
		}
	}
	if (Net->GetOwnCarSnapshotSerial() != SnapshotSerial)
	{
		SnapshotSerial = Net->GetOwnCarSnapshotSerial();
		const double Arrived = Net->GetOwnCarSnapshotSeconds();
		Predictor.OnSnapshot(Net->GetOwnCarSnapshot(), Arrived > 0.0 && Arrived <= Now ? Arrived : Now);
		LastSnapshotSeconds = Now;
	}
	// While the server drives there is no input number to time the round
	// trip by: the heartbeat's ping and the fake latency stand in.
	Predictor.SetFallbackRttSeconds(FMath::Max(Net->GetPingMs(), 0) / 1000.0 + FApexUdpConnection::GetFakeLatencyMs() / 1000.0);

	// Whether this frame is drawn from the prediction.
	const FApexCarTelemetry* Car = nullptr;
	for (const FApexCarTelemetry& One : Net->GetLatestTelemetry().Cars)
	{
		if (One.CarIndex == CarIndex)
		{
			Car = &One;
			break;
		}
	}
	const TCHAR* WhyNot = nullptr;
	if (!Predictor.IsSeated())
	{
		WhyNot = TEXT("no state yet");
	}
	else if (Net->GetSessionState() != EApexSessionState::Racing)
	{
		WhyNot = TEXT("not racing");
	}
	else if (!Car)
	{
		WhyNot = TEXT("no telemetry");
	}
	else if (Car->bInPitLane || Car->bPitAutopilot)
	{
		WhyNot = TEXT("pit lane");
	}
	else if (Car->bInGarage)
	{
		WhyNot = TEXT("garage");
	}
	else if (Car->IsRecovering())
	{
		WhyNot = TEXT("recovery");
	}
	else if (Car->FinishPosition > 0)
	{
		WhyNot = TEXT("finished");
	}
	else if (Now - LastSnapshotSeconds > SnapshotTimeoutSeconds)
	{
		WhyNot = TEXT("no snapshots");
	}
	else if (Predictor.IsServerDriven() && CVarPredictServerDriven.GetValueOnGameThread() == 0)
	{
		WhyNot = TEXT("the server drives");
	}
	bDrawable = WhyNot == nullptr;
	Predictor.SetCounting(bDrawable);
	Predictor.Advance(Now, DeltaSeconds);
	bInUse = bDrawable && Mode == 1;
	UpdateFeel(Now);

	++WindowFrames;
	if (bInUse)
	{
		++WindowDrawnFrames;
	}
	else if (WhyNot)
	{
		LastWhyNotDrawn = WhyNot;
	}
	LogWindow(Now, false);

	if (CVarPredictDebug.GetValueOnGameThread() && GEngine)
	{
		const FApexPredictionStats& S = Predictor.GetStats();
		GEngine->AddOnScreenDebugMessage(static_cast<uint64>(0x50524544), 0.5f, bInUse ? FColor::Green : FColor::Orange,
			FString::Printf(TEXT("predict %s%s: head %lld, lead %.1f ticks, rtt %.0f ms, %d rewinds, depth %.1f, last %.2f mm, offset %.1f mm"),
				Mode == 2 ? TEXT("shadow") : TEXT("on"), bInUse ? TEXT("") : *FString::Printf(TEXT(" (puppet: %s)"), WhyNot ? WhyNot : TEXT("-")),
				Predictor.GetHeadTick(), Predictor.GetLeadTicks(Now), Predictor.GetRttSeconds() * 1000.0, S.Rewinds,
				S.MeanReplayTicks(), S.CorrectionsMm.Num() > 0 ? S.CorrectionsMm.Last() : 0.0f,
				Predictor.GetCorrectionOffsetM() * 1000.0f));
	}
	return true;
}

bool UApexPredictionSubsystem::GetRenderPose(int32 CarIndex, FVector& OutLocationCm, FQuat& OutRotation,
	FVector& OutVelocityCmPerS, float& OutSpeedMps, float& OutSteering) const
{
	if (!bInUse || CarIndex != LocalCarIndex || CarIndex < 0)
	{
		return false;
	}
	FApexPredictedPose Pose;
	if (!Predictor.GetPose(Pose))
	{
		return false;
	}
	OutLocationCm = ApexRace::ServerToUnrealPosition(Pose.PositionM);
	OutRotation = ApexRace::ServerToUnrealRotation(Pose.YawRad, Pose.PitchRad, Pose.RollRad).Quaternion();
	OutVelocityCmPerS = ApexRace::ServerToUnrealPosition(Pose.VelocityMps);
	OutSpeedMps = Pose.SpeedMps;
	OutSteering = Pose.Steering;
	return true;
}

bool UApexPredictionSubsystem::GetOwnCar(FApexPredictedPose& Out) const
{
	return bInUse && LocalCarIndex >= 0 && Predictor.GetPose(Out);
}

bool UApexPredictionSubsystem::GetOwnFeel(const FApexDriverFeedback*& OutFeel, uint32& OutSerial, double& OutSeconds) const
{
	if (!bInUse || LocalCarIndex < 0 || OwnFeelSerial == 0)
	{
		return false;
	}
	OutFeel = &OwnFeel;
	OutSerial = OwnFeelSerial;
	OutSeconds = OwnFeelSeconds;
	return true;
}

void UApexPredictionSubsystem::UpdateFeel(double NowSeconds)
{
	UApexNetSubsystem* Net = GetGameInstance()->GetSubsystem<UApexNetSubsystem>();
	if (!bDrawable || !Predictor.IsSeated())
	{
		// The server's figures again from here; the next run starts at its head.
		FeltTick = -1;
		LastServerFeelSerial = Net ? Net->GetDriverFeedbackSerial() : 0;
		LastServerSnapshotSerial = Net ? Net->GetOwnCarSnapshotSerial() : 0;
		return;
	}
	// What the clock reached since the last frame: each tick's feel, from the
	// server's collector in the library, folded as a frame's messages are.
	// Collected in shadow too, so the log can measure it against the server's.
	const int64 Head = Predictor.GetHeadTick();
	if (FeltTick < 0 || FeltTick > Head || Head - FeltTick > Predictor.GetTickRateHz())
	{
		FeltTick = Head - 1;
	}
	FApexDriverFeedback Feel;
	if (Predictor.CollectFeel(FeltTick, Head, NowSeconds, Feel) > 0)
	{
		OwnFeel = MoveTemp(Feel);
		++OwnFeelSerial;
		OwnFeelSeconds = NowSeconds;
	}
	FeltTick = Head;

	// How far ahead of the server's own figures the driver had them: the
	// server's newest torque sample and revs against when this client first
	// felt that tick, and how far apart the two were.
	if (Net && Net->GetDriverFeedbackSerial() != LastServerFeelSerial)
	{
		LastServerFeelSerial = Net->GetDriverFeedbackSerial();
		const FApexDriverFeedback& Server = Net->GetDriverFeedback();
		double FeltAt = 0.0;
		float Torque = 0.0f;
		float Rpm = 0.0f;
		if (Server.SteerTorque.Num() > 0 && Predictor.GetFelt(Server.ServerTick, FeltAt, Torque, Rpm))
		{
			WindowFeelLeadMs += (Net->GetDriverFeedbackTime() - FeltAt) * 1000.0;
			WindowFeelTorqueError += FMath::Abs(Torque - Server.SteerTorque.Last());
			++WindowFeelSamples;
		}
	}
	if (Net && Net->GetOwnCarSnapshotSerial() != LastServerSnapshotSerial)
	{
		LastServerSnapshotSerial = Net->GetOwnCarSnapshotSerial();
		const FApexOwnCarSnapshot& Server = Net->GetOwnCarSnapshot();
		double FeltAt = 0.0;
		float Torque = 0.0f;
		float Rpm = 0.0f;
		if (Predictor.GetFelt(Server.Tick, FeltAt, Torque, Rpm))
		{
			WindowRpmLeadMs += (Net->GetOwnCarSnapshotSeconds() - FeltAt) * 1000.0;
			WindowRpmError += FMath::Abs(Rpm - Server.EngineRpm);
			++WindowRpmSamples;
		}
	}
}

void UApexPredictionSubsystem::ReportPuppetPose(int32 CarIndex, const FVector& LocationCm, float SpeedMps)
{
	if (!bDrawable || CarIndex != LocalCarIndex || CarIndex < 0)
	{
		return;
	}
	FApexPredictedPose Pose;
	if (!Predictor.GetRawPose(Pose))
	{
		return;
	}
	const double GapM = FVector::Dist(ApexRace::ServerToUnrealPosition(Pose.PositionM), LocationCm) / ApexRace::MetresToCentimetres;
	WindowGapSumM += GapM;
	WindowGapSumMs += SpeedMps > 1.0f ? GapM / SpeedMps * 1000.0 : 0.0;
	++WindowGapSamples;
}
