#include "School/ApexSchoolSubsystem.h"

#include "ApexMenuFlowSubsystem.h"
#include "ApexNetSubsystem.h"
#include "ApexSim.h"
#include "Async/Async.h"
#include "Catalog/ApexCatalogRows.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Guide/ApexTrackGuide.h"
#include "Guide/ApexTrackGuideSubsystem.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Race/ApexPhysicsLib.h"
#include "Race/ApexRaceDirector.h"
#include "Race/ApexReplayClip.h"
#include "Race/ApexSchoolMarkerActor.h"

namespace ApexSchoolSub
{
	/** Seconds the car keeps rolling after a try before it is sent to the garage. */
	constexpr float ParkDelayS = 1.5f;
	/** The guide's recording is sampled this often to time the guide car, Hz. */
	constexpr double MeasureHz = 120.0;

	UApexSchoolSubsystem* SchoolOf(const UWorld* World)
	{
		const UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
		return GameInstance ? GameInstance->GetSubsystem<UApexSchoolSubsystem>() : nullptr;
	}

	/** A car of the server's lobby whose catalog name contains `Wanted`; empty when none. */
	FString FindCar(const UApexNetSubsystem& Net, const UApexMenuFlowSubsystem& Flow, const FString& Wanted)
	{
		for (const FApexCarConfigSummary& Car : Net.GetCachedLobbyState().CarConfigs)
		{
			FApexCarCatalogRow Row;
			if (Flow.GetCarCatalogRow(Car.Id, Row) && Row.DisplayName.Contains(Wanted))
			{
				return Car.Id;
			}
		}
		return FString();
	}

	FAutoConsoleCommandWithWorldAndArgs RunCommand(TEXT("apexsim.school.Run"),
		TEXT("apexsim.school.Run <track stem> <test id: brake|exit|corner|lap> [car name]: prepare the circuit's licence tests and start one."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			UApexSchoolSubsystem* School = SchoolOf(World);
			const UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
			UApexMenuFlowSubsystem* Flow = GameInstance ? GameInstance->GetSubsystem<UApexMenuFlowSubsystem>() : nullptr;
			const UApexNetSubsystem* Net = GameInstance ? GameInstance->GetSubsystem<UApexNetSubsystem>() : nullptr;
			if (!School || !Flow || !Net || Args.Num() < 2)
			{
				UE_LOG(LogApexSim, Warning, TEXT("apexsim.school.Run <track stem> <test id> [car name]"));
				return;
			}
			if (Args.Num() >= 3)
			{
				const FString Car = FindCar(*Net, *Flow, FString::Join(TArrayView<const FString>(Args).RightChop(2), TEXT(" ")));
				if (!Car.IsEmpty())
				{
					Flow->SetPendingCar(Car);
				}
			}
			const FString Track = Flow->FindTrackIdByStem(Args[0]);
			if (Track.IsEmpty())
			{
				UE_LOG(LogApexSim, Warning, TEXT("apexsim.school.Run: no circuit '%s'"), *Args[0]);
				return;
			}
			School->RequestRun(Track, Args[1]);
		}));

	FAutoConsoleCommandWithWorldAndArgs GoCommand(TEXT("apexsim.school.Go"),
		TEXT("The licence test's START (or RETRY): out onto the run-up."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>&, UWorld* World)
		{
			if (UApexSchoolSubsystem* School = SchoolOf(World))
			{
				School->Go();
			}
		}));

	FAutoConsoleCommandWithWorldAndArgs NextCommand(TEXT("apexsim.school.Next"),
		TEXT("The circuit's next licence test."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>&, UWorld* World)
		{
			if (UApexSchoolSubsystem* School = SchoolOf(World))
			{
				School->NextTest();
			}
		}));

	FAutoConsoleCommandWithWorldAndArgs LeaveCommand(TEXT("apexsim.school.Leave"),
		TEXT("Leave the licence test's session for the School screen."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>&, UWorld* World)
		{
			if (UApexSchoolSubsystem* School = SchoolOf(World))
			{
				School->LeaveToSchool();
			}
		}));
}

void UApexSchoolSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	UApexNetSubsystem* Net = Collection.InitializeDependency<UApexNetSubsystem>();
	Collection.InitializeDependency<UApexMenuFlowSubsystem>();
	Collection.InitializeDependency<UApexTrackGuideSubsystem>();
	if (Net)
	{
		Net->OnTelemetry.AddDynamic(this, &UApexSchoolSubsystem::HandleTelemetry);
		Net->OnSessionLeft.AddDynamic(this, &UApexSchoolSubsystem::HandleSessionLeft);
	}
	FString Why;
	if (!ApexPhysics::IsAvailable(&Why) || !ApexSimContent::Find(SimTracksDir, SimCarsDir, Why))
	{
		SimTracksDir.Reset();
		SimCarsDir.Reset();
	}
}

void UApexSchoolSubsystem::Deinitialize()
{
	MakeJob.Cancel();
	if (Measuring.IsValid())
	{
		Measuring.Wait();
	}
	DestroyMarker();
	Super::Deinitialize();
}

TStatId UApexSchoolSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UApexSchoolSubsystem, STATGROUP_Tickables);
}

bool UApexSchoolSubsystem::IsTickable() const
{
	return Prep == EApexSchoolPrep::Making || Prep == EApexSchoolPrep::Measuring || bSession;
}

// --- A circuit's tests ---------------------------------------------------------

const FApexSchoolMedal* UApexSchoolSubsystem::GetBest(const FString& TestId) const
{
	const UApexMenuFlowSubsystem* Flow = GetGameInstance()->GetSubsystem<UApexMenuFlowSubsystem>();
	return Flow ? ApexSchool::FindMedal(Flow->GetSchoolMedals(), ApexSchool::MedalKey(TrackId, CarId, TestId)) : nullptr;
}

void UApexSchoolSubsystem::Prepare(const FString& InTrackId)
{
	const UApexMenuFlowSubsystem* Flow = GetGameInstance()->GetSubsystem<UApexMenuFlowSubsystem>();
	if (!Flow)
	{
		return;
	}
	const FString PendingCar = Flow->GetPendingCarId();
	const bool bSame = InTrackId == TrackId && PendingCar == CarId;
	if (bSame && (Prep == EApexSchoolPrep::Ready || Prep == EApexSchoolPrep::Making || Prep == EApexSchoolPrep::Measuring))
	{
		return;
	}
	MakeJob.Cancel();
	if (Measuring.IsValid())
	{
		Measuring.Wait();
		Measuring = TFuture<float>();
	}
	TrackId = InTrackId;
	CarId = PendingCar;
	Tests.Reset();
	bMadeGuide = false;
	Stem.Reset();
	CarClass.Reset();
	GuideLapM = 0.0f;

	FApexTrackCatalogRow Track;
	FApexCarCatalogRow Car;
	if (!Flow->GetTrackCatalogRow(TrackId, Track) || Track.YamlBaseName.IsEmpty())
	{
		FailPrepare(TEXT("This circuit is not installed on this machine"));
		return;
	}
	if (CarId.IsEmpty() || !Flow->GetCarCatalogRow(CarId, Car) || Car.CarClass.IsEmpty())
	{
		FailPrepare(TEXT("Pick a car first: the tests are made for its class"));
		return;
	}
	Stem = Track.YamlBaseName;
	CarClass = Car.CarClass;
	ContinuePrepare();
}

void UApexSchoolSubsystem::RequestRun(const FString& InTrackId, const FString& TestId)
{
	PendingTestId = TestId;
	Prepare(InTrackId);
	// Ready already (the same circuit and car): Tick starts it.
	UE_LOG(LogApexSim, Log, TEXT("School: run %s on %s requested"), *TestId, *InTrackId);
}

bool UApexSchoolSubsystem::CanMakeGuide(const FString& InStem) const
{
	const UApexTrackGuideSubsystem* Guides = GetGameInstance()->GetSubsystem<UApexTrackGuideSubsystem>();
	return !SimTracksDir.IsEmpty() && Guides && Guides->CanMake(InStem);
}

void UApexSchoolSubsystem::FailPrepare(const FString& Why)
{
	Prep = EApexSchoolPrep::Failed;
	PrepMessage = Why;
	PendingTestId.Reset();
	UE_LOG(LogApexSim, Log, TEXT("School: no tests for %s (%s): %s"), *Stem, *CarClass, *Why);
	Changed();
}

void UApexSchoolSubsystem::ContinuePrepare()
{
	UApexTrackGuideSubsystem* Guides = GetGameInstance()->GetSubsystem<UApexTrackGuideSubsystem>();
	const UApexMenuFlowSubsystem* Flow = GetGameInstance()->GetSubsystem<UApexMenuFlowSubsystem>();
	if (!Guides || !Flow)
	{
		FailPrepare(TEXT("The game is not ready"));
		return;
	}
	Guides->Rescan();
	const FApexGuideFileInfo* File = Guides->GetFiles().FindByPredicate([this](const FApexGuideFileInfo& F) {
		return F.Stem.Equals(Stem, ESearchCase::IgnoreCase) && F.Class.Equals(CarClass, ESearchCase::IgnoreCase);
	});
	FApexTrackGuide Guide;
	FString Json;
	FString Error;
	bool bUsable = File && FFileHelper::LoadFileToString(Json, *File->Path) && ApexTrackGuide::Parse(Json, Guide, Error);
	// Made from another version of the circuit: its stations are not this one's.
	FApexTrackCatalogRow Track;
	if (bUsable && Guide.SourceCrc != 0 && Flow->GetTrackCatalogRow(TrackId, Track) && Track.SourceCrc != 0 && Track.SourceCrc != Guide.SourceCrc)
	{
		bUsable = false;
		Error = TEXT("the track guide was made from another version of the circuit");
	}
	if (!bUsable)
	{
		if (!bMadeGuide && CanMakeGuide(Stem))
		{
			const FString Yaml = ApexSimContent::TrackYaml(SimTracksDir, Stem);
			const FString OutDir = UApexTrackGuideSubsystem::CacheDirectory();
			const FString CarsDir = SimCarsDir;
			const FString Class = CarClass;
			if (MakeJob.Start(FString::Printf(TEXT("School guide %s %s"), *Stem, *CarClass),
					[Yaml, CarsDir, Class, OutDir](const volatile uint8* Cancel, FString& JobError) {
						return ApexPhysics::BuildGuide(Yaml, CarsDir, Class, OutDir, Cancel, JobError);
					}))
			{
				bMadeGuide = true;
				Prep = EApexSchoolPrep::Making;
				PrepMessage = TEXT("Making the circuit's track guide for the tests");
				UE_LOG(LogApexSim, Log, TEXT("School: making the %s guide of %s"), *CarClass, *Stem);
				Changed();
				return;
			}
		}
		FailPrepare(File ? FString::Printf(TEXT("The track guide cannot be used: %s"), *Error)
			: FString::Printf(TEXT("No %s track guide of this circuit here, and none can be made"), *ApexCatalog::DisplayClass(CarClass)));
		return;
	}

	Tests = ApexSchool::BuildTests(Guide);
	GuideLapM = Guide.Track.LengthM;
	if (Tests.Num() == 0)
	{
		FailPrepare(TEXT("The track guide has no corner to make a test of"));
		return;
	}
	const ApexSchool::FTest* CornerTime = Tests.FindByPredicate([](const ApexSchool::FTest& T) { return T.Kind == EApexSchoolKind::CornerTime; });
	if (!CornerTime)
	{
		Prep = EApexSchoolPrep::Ready;
		PrepMessage.Reset();
		UE_LOG(LogApexSim, Log, TEXT("School: %d test(s) on %s for the %s class"), Tests.Num(), *Stem, *CarClass);
		Changed();
		return;
	}

	// The corner time's reference: the guide car's own time through the
	// corner, judged from its recording as a try would be.
	const FString Recording = FPaths::Combine(FPaths::GetPath(File->Path), Guide.Recording);
	const ApexSchool::FTest Test = *CornerTime;
	const float LapM = GuideLapM;
	Prep = EApexSchoolPrep::Measuring;
	PrepMessage = TEXT("Timing the guide car through the corners");
	Changed();
	Measuring = Async(EAsyncExecution::ThreadPool, [Recording, Test, LapM]() -> float
	{
		FApexReplayClip Clip;
		FString LoadError;
		if (!Clip.LoadFromFile(Recording, LoadError) || !Clip.IsValid())
		{
			return 0.0f;
		}
		// Timed as a try: entry to exit, the clip's own clock.
		ApexSchool::FTest Timed = Test;
		ApexSchool::SetReference(Timed, 1000.0f);
		ApexSchool::FJudge Judge;
		Judge.Begin(Timed, LapM);
		const double Duration = Clip.GetDurationSeconds();
		FApexTelemetryFrame Frame;
		for (double Time = 0.0; Time <= Duration; Time += 1.0 / ApexSchoolSub::MeasureHz)
		{
			Clip.SampleAt(Time, Frame);
			const FApexCarTelemetry* Car = Frame.Cars.FindByPredicate([](const FApexCarTelemetry& C) { return C.CarIndex == 0; });
			if (!Car)
			{
				continue;
			}
			ApexSchool::FSample Sample;
			Sample.TimeS = Time;
			Sample.StationM = Car->TrackProgress;
			Sample.SpeedMps = Car->SpeedMps;
			Sample.Lap = Car->CurrentLap;
			Sample.bInGarage = Car->bInGarage;
			if (Judge.Feed(Sample))
			{
				break;
			}
		}
		return Judge.GetPhase() == ApexSchool::EJudgePhase::Done ? Judge.GetResult().Seconds : 0.0f;
	});
}

bool UApexSchoolSubsystem::StartTest(int32 Index, FString& OutError)
{
	UApexMenuFlowSubsystem* Flow = GetGameInstance()->GetSubsystem<UApexMenuFlowSubsystem>();
	UApexNetSubsystem* Net = GetGameInstance()->GetSubsystem<UApexNetSubsystem>();
	if (!Flow || !Net)
	{
		OutError = TEXT("The game is not ready");
		return false;
	}
	if (Prep != EApexSchoolPrep::Ready || !Tests.IsValidIndex(Index) || !Tests[Index].IsReady())
	{
		OutError = TEXT("The test is not ready");
		return false;
	}
	if (!Net->IsAuthenticated())
	{
		OutError = TEXT("Not connected to a server yet");
		return false;
	}
	if (Net->IsInSession())
	{
		OutError = TEXT("Leave the session first");
		return false;
	}
	ActiveIndex = Index;
	Stage = EApexSchoolStage::Briefing;
	Notice.Reset();
	bNewBest = false;
	bSession = true;
	bReturnToSchool = false;
	ParkIn = -1.0f;
	bCarInGarage = true;

	// The hotlap path of the track select's Hotlap: create, count into the
	// hotlap on join. Alone, dry, at midday.
	Flow->SetPendingTrack(TrackId);
	Flow->SetPendingCar(CarId);
	Net->SelectCar(CarId);
	Flow->bAutoStartOnJoin = true;
	Flow->AutoStartMode = EApexGameMode::Hotlap;
	UE_LOG(LogApexSim, Log, TEXT("School: %s (%s) on %s, car %s"), *Tests[Index].Title, *Tests[Index].Id, *Stem, *CarId);
	Net->CreateSession(TrackId, 1, 0, 0, EApexSessionKind::Practice, Flow->CreateAllowedAssists, FApexSessionConditions(), Flow->CreateDamage);
	Changed();
	return true;
}

// --- In the session ------------------------------------------------------------

bool UApexSchoolSubsystem::IsSchoolSession() const
{
	const UApexNetSubsystem* Net = GetGameInstance()->GetSubsystem<UApexNetSubsystem>();
	return bSession && Net && Net->IsInSession();
}

float UApexSchoolSubsystem::LapLengthM() const
{
	const UApexNetSubsystem* Net = GetGameInstance()->GetSubsystem<UApexNetSubsystem>();
	return Net && Net->GetTrackSectors().IsValid() ? Net->GetTrackSectors().TrackLengthM : GuideLapM;
}

void UApexSchoolSubsystem::Go()
{
	UApexNetSubsystem* Net = GetGameInstance()->GetSubsystem<UApexNetSubsystem>();
	const ApexSchool::FTest* Test = GetActiveTest();
	if (!IsSchoolSession() || !Net || !Test || Stage == EApexSchoolStage::Driving)
	{
		return;
	}
	Judge.Begin(*Test, LapLengthM());
	Stage = EApexSchoolStage::Driving;
	Notice.Reset();
	bNewBest = false;
	ParkIn = -1.0f;
	Net->HotlapRelocate(EApexHotlapDestination::Track, false);
	UE_LOG(LogApexSim, Log, TEXT("School: %s out"), *Test->Id);
	Changed();
}

void UApexSchoolSubsystem::NextTest()
{
	if (!IsSchoolSession() || Tests.Num() == 0 || Stage == EApexSchoolStage::Driving)
	{
		return;
	}
	for (int32 Step = 1; Step <= Tests.Num(); ++Step)
	{
		const int32 Index = (ActiveIndex + Step) % Tests.Num();
		if (Tests[Index].IsReady())
		{
			ActiveIndex = Index;
			break;
		}
	}
	Stage = EApexSchoolStage::Briefing;
	Notice.Reset();
	bNewBest = false;
	Changed();
}

void UApexSchoolSubsystem::LeaveToSchool()
{
	UApexNetSubsystem* Net = GetGameInstance()->GetSubsystem<UApexNetSubsystem>();
	if (!IsSchoolSession() || !Net)
	{
		return;
	}
	bReturnToSchool = true;
	Net->LeaveSession();
}

bool UApexSchoolSubsystem::ConsumeReturnToSchool()
{
	const bool bWas = bReturnToSchool;
	bReturnToSchool = false;
	return bWas;
}

void UApexSchoolSubsystem::HandleSessionLeft()
{
	if (!bSession)
	{
		return;
	}
	bSession = false;
	Stage = EApexSchoolStage::Off;
	ParkIn = -1.0f;
	DestroyMarker();
	Changed();
}

void UApexSchoolSubsystem::HandleTelemetry(const FApexTelemetryFrame& Frame)
{
	UApexNetSubsystem* Net = GetGameInstance()->GetSubsystem<UApexNetSubsystem>();
	if (!bSession || !Net || !Net->IsInSession() || Net->IsInDemoSession())
	{
		return;
	}
	const int32 LocalIndex = Net->GetLocalCarIndex();
	const FApexCarTelemetry* Local = Frame.Cars.FindByPredicate([LocalIndex](const FApexCarTelemetry& Car) { return Car.CarIndex == LocalIndex; });
	if (!Local || !ApexIsGarageMode(Frame.GameMode))
	{
		return;
	}
	bCarInGarage = Local->bInGarage;
	if (Stage != EApexSchoolStage::Driving)
	{
		return;
	}
	const int32 Rate = Net->GetSessionPhysics().TickRateHz;
	ApexSchool::FSample Sample;
	Sample.TimeS = Rate > 0 ? static_cast<double>(Frame.ServerTick) / Rate : FPlatformTime::Seconds();
	Sample.StationM = Local->TrackProgress;
	Sample.SpeedMps = Local->SpeedMps;
	Sample.Lap = Local->CurrentLap;
	Sample.bLapInvalid = Local->bLapInvalid;
	Sample.bInGarage = Local->bInGarage;
	Sample.LastLapMs = Local->LastLapTimeMs;
	Sample.bLastLapInvalid = Local->bLastLapInvalid;
	if (!Judge.Feed(Sample))
	{
		return;
	}
	const ApexSchool::FTest& Test = Judge.GetTest();
	if (Judge.GetPhase() == ApexSchool::EJudgePhase::Aborted)
	{
		Stage = EApexSchoolStage::Briefing;
		Notice = Judge.GetAbortReason();
		UE_LOG(LogApexSim, Log, TEXT("School: %s given up: %s"), *Test.Id, *Notice);
	}
	else
	{
		Stage = EApexSchoolStage::Result;
		const ApexSchool::FResult& Result = Judge.GetResult();
		if (UApexMenuFlowSubsystem* Flow = GetGameInstance()->GetSubsystem<UApexMenuFlowSubsystem>())
		{
			bNewBest = Flow->FileSchoolResult(ApexSchool::MedalKey(TrackId, CarId, Test.Id), Test, Result);
		}
		UE_LOG(LogApexSim, Log, TEXT("School: %s on %s: %s, %s%s%s"), *Test.Id, *Stem, *ApexSchool::MedalName(Result.Medal),
			*ApexSchool::FigureText(Test, Result.Value, true), Result.Why.IsEmpty() ? TEXT("") : TEXT(", "), *Result.Why);
	}
	if (!Local->bInGarage)
	{
		ParkIn = ApexSchoolSub::ParkDelayS;
	}
	Changed();
}

void UApexSchoolSubsystem::Tick(float DeltaTime)
{
	if (Prep == EApexSchoolPrep::Making)
	{
		ApexPhysics::EJobResult Result;
		FString Error;
		double Seconds = 0.0;
		if (MakeJob.Poll(Result, Error, Seconds))
		{
			if (Result != ApexPhysics::EJobResult::Done)
			{
				FailPrepare(FString::Printf(TEXT("The track guide could not be made: %s"), *Error));
			}
			else
			{
				UE_LOG(LogApexSim, Log, TEXT("School: made the %s guide of %s in %.1f s"), *CarClass, *Stem, Seconds);
				ContinuePrepare();
			}
		}
	}
	else if (Prep == EApexSchoolPrep::Measuring && Measuring.IsValid() && Measuring.IsReady())
	{
		const float Seconds = Measuring.Get();
		Measuring = TFuture<float>();
		const int32 Index = Tests.IndexOfByPredicate([](const ApexSchool::FTest& T) { return T.Kind == EApexSchoolKind::CornerTime; });
		if (Tests.IsValidIndex(Index))
		{
			if (Seconds > 0.0f)
			{
				ApexSchool::SetReference(Tests[Index], Seconds);
			}
			else
			{
				UE_LOG(LogApexSim, Warning, TEXT("School: the guide car could not be timed through %s; no corner time test"), *Tests[Index].Place);
				Tests.RemoveAt(Index);
			}
		}
		Prep = EApexSchoolPrep::Ready;
		PrepMessage.Reset();
		UE_LOG(LogApexSim, Log, TEXT("School: %d test(s) on %s for the %s class (corner reference %.3f s)"), Tests.Num(), *Stem, *CarClass, Seconds);
		Changed();
	}

	if (Prep == EApexSchoolPrep::Ready && !PendingTestId.IsEmpty())
	{
		const int32 Index = Tests.IndexOfByPredicate([this](const ApexSchool::FTest& T) { return T.Id.Equals(PendingTestId, ESearchCase::IgnoreCase); });
		PendingTestId.Reset();
		FString Error;
		if (!StartTest(Index, Error))
		{
			UE_LOG(LogApexSim, Warning, TEXT("School: cannot start the test: %s"), *Error);
		}
	}

	if (!bSession)
	{
		return;
	}
	if (ParkIn >= 0.0f)
	{
		ParkIn -= DeltaTime;
		if (ParkIn < 0.0f && !bCarInGarage)
		{
			if (UApexNetSubsystem* Net = GetGameInstance()->GetSubsystem<UApexNetSubsystem>(); Net && IsSchoolSession())
			{
				Net->HotlapRelocate(EApexHotlapDestination::Garage, false);
			}
		}
	}
	UpdateMarker();
}

// --- The marks on the road -----------------------------------------------------

void UApexSchoolSubsystem::UpdateMarker()
{
	if (!IsSchoolSession())
	{
		return;
	}
	if (Marker && MarkerTest == ActiveIndex)
	{
		return;
	}
	const UGameInstance* GameInstance = GetGameInstance();
	AApexRaceDirector* Director = GameInstance ? AApexRaceDirector::Find(GameInstance->GetWorld()) : nullptr;
	const TArray<FVector>* Centerline = Director ? Director->GetTrackCenterline() : nullptr;
	const ApexSchool::FTest* Test = GetActiveTest();
	if (!Centerline || Centerline->Num() < 3 || !Test)
	{
		return;
	}
	if (!Marker)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Marker = Director->GetWorld()->SpawnActor<AApexSchoolMarkerActor>(AApexSchoolMarkerActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, Params);
	}
	if (Marker)
	{
		Marker->SetMarks(*Test, *Centerline);
		MarkerTest = ActiveIndex;
	}
}

void UApexSchoolSubsystem::DestroyMarker()
{
	if (Marker)
	{
		Marker->Destroy();
		Marker = nullptr;
	}
	MarkerTest = INDEX_NONE;
}
