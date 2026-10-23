#include "ApexSessionRecorder.h"

#include "ApexMenuFlowSubsystem.h"
#include "ApexNetSubsystem.h"
#include "ApexProfiles.h"
#include "ApexSim.h"
#include "Catalog/ApexCatalogRows.h"
#include "Engine/GameInstance.h"
#include "Race/ApexRaceCoordinate.h"

void UApexSessionRecorder::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	if (UApexNetSubsystem* Net = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexNetSubsystem>() : nullptr)
	{
		Net->OnTelemetry.AddDynamic(this, &UApexSessionRecorder::HandleTelemetry);
		Net->OnSessionStateChanged.AddDynamic(this, &UApexSessionRecorder::HandleSessionStateChanged);
		Net->OnSessionRosterUpdated.AddDynamic(this, &UApexSessionRecorder::HandleRosterUpdated);
	}
}

void UApexSessionRecorder::Deinitialize()
{
	if (UApexNetSubsystem* Net = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexNetSubsystem>() : nullptr)
	{
		Net->OnTelemetry.RemoveDynamic(this, &UApexSessionRecorder::HandleTelemetry);
		Net->OnSessionStateChanged.RemoveDynamic(this, &UApexSessionRecorder::HandleSessionStateChanged);
		Net->OnSessionRosterUpdated.RemoveDynamic(this, &UApexSessionRecorder::HandleRosterUpdated);
	}

	Super::Deinitialize();
}

void UApexSessionRecorder::BeginRecording()
{
	Results.Reset();
	LastLapNumber.Reset();
	LastLapTimeSeen.Reset();
	PersonalBestDelta = 0.0f;
	bRecording = true;

	UApexMenuFlowSubsystem* Flow = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexMenuFlowSubsystem>() : nullptr;
	if (Flow)
	{
		TrackId = Flow->GetPendingTrackId();
		LapLimit = Flow->EffectiveLapLimit();
		RaceSeconds = Flow->EffectiveRaceSeconds();
		QualifyingSeconds = Flow->EffectiveQualifyingSeconds();
		// The create screen's guess; the telemetry's own mode replaces it
		// (RecordFrame), for a guest and for a race after qualifying.
		RecordedMode = Flow->CreateStartingMode;

		FApexTrackCatalogRow Row;
		TrackLengthM = Flow->GetTrackCatalogRow(TrackId, Row) ? Row.LengthM : 0.0f;
	}

	// The session as it was made, when there is one: a guest's own create
	// settings say nothing about the host's race.
	if (const UApexNetSubsystem* Net = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexNetSubsystem>() : nullptr)
	{
		if (Net->IsInSession())
		{
			RaceSeconds = Net->GetSessionRaceSeconds();
			QualifyingSeconds = Net->GetSessionQualifyingSeconds();
			if (RaceSeconds > 0)
			{
				LapLimit = 0;
			}
		}
	}

	UE_LOG(LogApexSim, Log, TEXT("Recording session on track '%s' (%d lap limit, %d s)"), *TrackId, LapLimit, RaceSeconds);
}

void UApexSessionRecorder::FinishRecording()
{
	if (!bRecording)
	{
		return;
	}
	bRecording = false;

	ApplyRosterNames();
	SortResults();

	// File the local driver's best lap against the profile.
	UApexMenuFlowSubsystem* Flow = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexMenuFlowSubsystem>() : nullptr;
	const FApexCarResult* Local = FindLocalResult();
	if (Flow && Local && Local->BestLapSeconds > 0.0f && !TrackId.IsEmpty())
	{
		float Previous = 0.0f;
		const bool bHadPrevious = Flow->GetBestLapSeconds(TrackId, Previous);
		if (Flow->RecordBestLap(TrackId, Local->BestLapSeconds) && bHadPrevious)
		{
			PersonalBestDelta = Previous - Local->BestLapSeconds;
		}
	}

	// The driver's stats: what they drove here, whatever the mode.
	if (Flow && Local && (Local->LapsCompleted() > 0 || Local->DistanceM > 0.0f))
	{
		FApexSessionStats Session;
		Session.Mode = RecordedMode;
		Session.TrackId = TrackId;
		Session.CarId = Flow->GetPendingCarId();
		Session.Laps = Local->LapsCompleted();
		Session.DistanceM = Local->DistanceM;
		Session.BestLapSeconds = Local->BestLapSeconds;
		Session.FinishPosition = Local->FinishPosition;
		Session.FieldSize = Results.Num();
		// Incidents: race control's penalties and the laps track limits took.
		Session.Incidents = Local->LapValid.FilterByPredicate([](bool bValid) { return !bValid; }).Num();
		if (const UApexNetSubsystem* Net = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexNetSubsystem>() : nullptr)
		{
			for (const FApexRosterEntry& Entry : Net->GetSessionRoster().Entries)
			{
				if (Entry.PlayerId.Equals(Local->PlayerId, ESearchCase::IgnoreCase))
				{
					Session.Incidents += Entry.Penalties.Num();
				}
			}
		}
		Flow->FileSessionStats(Session);
	}

	UE_LOG(LogApexSim, Log, TEXT("Session recorded: %d car(s), local best %s"),
		Results.Num(),
		Local && Local->BestLapSeconds > 0.0f
			? *UApexMenuFlowSubsystem::FormatLapTime(Local->BestLapSeconds)
			: TEXT("none"));
}

bool UApexSessionRecorder::ClassifiesAhead(const FApexCarResult& A, const FApexCarResult& B, bool bByDistance)
{
	// The server's classification, when there is one, is the result.
	if (A.FinishPosition > 0 || B.FinishPosition > 0)
	{
		if (A.FinishPosition > 0 && B.FinishPosition > 0)
		{
			return A.FinishPosition < B.FinishPosition;
		}
		return A.FinishPosition > 0;
	}
	// A race not yet run to the flag: the running order.
	if (bByDistance && !FMath::IsNearlyEqual(A.DistanceM, B.DistanceM))
	{
		return A.DistanceM > B.DistanceM;
	}
	// Most laps first, then the quickest lap. A car with no lap at all sorts
	// last however far it got.
	if (A.LapsCompleted() != B.LapsCompleted())
	{
		return A.LapsCompleted() > B.LapsCompleted();
	}
	if (A.BestLapSeconds <= 0.0f) { return false; }
	if (B.BestLapSeconds <= 0.0f) { return true; }
	return A.BestLapSeconds < B.BestLapSeconds;
}

void UApexSessionRecorder::SortResults()
{
	const bool bByDistance = RecordedMode == EApexGameMode::Race
		|| Results.ContainsByPredicate([](const FApexCarResult& Result) { return Result.FinishPosition > 0; });
	Results.StableSort([bByDistance](const FApexCarResult& A, const FApexCarResult& B)
	{
		return ClassifiesAhead(A, B, bByDistance);
	});
}

FApexCarResult& UApexSessionRecorder::FindOrAddCar(int32 CarIndex)
{
	for (FApexCarResult& Result : Results)
	{
		if (Result.CarIndex == CarIndex)
		{
			return Result;
		}
	}

	FApexCarResult NewResult;
	NewResult.CarIndex = CarIndex;
	return Results[Results.Add(MoveTemp(NewResult))];
}

void UApexSessionRecorder::ApplyRosterNames()
{
	const UApexNetSubsystem* Net = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexNetSubsystem>() : nullptr;
	if (!Net)
	{
		return;
	}

	for (const FApexRosterEntry& Entry : Net->GetSessionRoster().Entries)
	{
		for (FApexCarResult& Result : Results)
		{
			if (Result.CarIndex == Entry.CarIndex)
			{
				Result.DriverName = Entry.PlayerName;
				Result.PlayerId = Entry.PlayerId;
				Result.bIsAi = Entry.bIsAi;
			}
		}
	}
}

void UApexSessionRecorder::HandleSessionStateChanged(EApexSessionState NewState)
{
	switch (NewState)
	{
	case EApexSessionState::Countdown:
		BeginRecording();
		break;

	case EApexSessionState::Finished:
		// The frame that says so is broadcast after this, and it is the one
		// that classifies the last car home (the session finishes on the tick
		// it takes the flag): take it in before the results freeze.
		if (bRecording)
		{
			if (const UApexNetSubsystem* Net = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexNetSubsystem>() : nullptr)
			{
				RecordFrame(Net->GetLatestTelemetry());
			}
		}
		FinishRecording();
		break;

	case EApexSessionState::Lobby:
		// Back to the lobby without finishing (everyone left, or the host reset
		// it): keep whatever was recorded rather than discarding it silently.
		FinishRecording();
		break;

	default:
		break;
	}
}

void UApexSessionRecorder::HandleRosterUpdated(const FApexSessionRoster& Roster)
{
	// Names can arrive after the cars have already been seen in telemetry.
	// Not the demo's: its roster would rename the last race's results.
	const UApexNetSubsystem* Net = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexNetSubsystem>() : nullptr;
	if (!Net || !Net->IsInDemoSession())
	{
		ApplyRosterNames();
	}
}

void UApexSessionRecorder::HandleTelemetry(const FApexTelemetryFrame& Frame)
{
	// The menu's demo race is somebody else's results.
	const UApexNetSubsystem* Net = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexNetSubsystem>() : nullptr;
	if (Net && Net->IsInDemoSession())
	{
		return;
	}

	if (!bRecording)
	{
		// A frame can arrive before the state transition that starts recording.
		if (Frame.SessionState != EApexSessionState::Racing)
		{
			return;
		}
		BeginRecording();
	}
	RecordFrame(Frame);
}

void UApexSessionRecorder::RecordFrame(const FApexTelemetryFrame& Frame)
{
	const UApexNetSubsystem* Net = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexNetSubsystem>() : nullptr;
	// What is being driven, as the server says: the create screen's mode is
	// not a guest's, and a session can run qualifying and then its race.
	switch (Frame.GameMode)
	{
	case EApexGameMode::FreePractice:
	case EApexGameMode::Qualification:
	case EApexGameMode::Practice:
	case EApexGameMode::Race:
	case EApexGameMode::Hotlap:
		RecordedMode = Frame.GameMode;
		break;
	default:
		break;
	}
	for (const FApexCarTelemetry& Car : Frame.Cars)
	{
		FApexCarResult& Result = FindOrAddCar(Car.CarIndex);

		// The server classifies a car on the frame its last lap rolls over, so
		// that lap still counts; anything after it is the cool-down lap.
		const bool bAlreadyFinished = Result.FinishPosition > 0;
		if (bAlreadyFinished)
		{
			continue;
		}
		Result.FinishPosition = Car.FinishPosition;

		Result.TopSpeedMps = FMath::Max(Result.TopSpeedMps, Car.SpeedMps);

		if (TrackLengthM > 0.0f)
		{
			// Laps plus the centerline station into the current one (the wire's
			// TrackProgress is metres, not a fraction). The station alone would
			// reset every lap, and integrating speed accumulates drift; the Max
			// also clips the grid's negative pre-start distance to zero.
			Result.DistanceM = FMath::Max(Result.DistanceM,
				ApexRace::RaceDistanceM(Car.CurrentLap, Car.TrackProgress, TrackLengthM));
		}

		// The server times the laps and judges track limits, so the completed
		// lap and the best come straight off the wire; the client only has to
		// notice the counter move to know a lap is new. Lap 0 to 1 is no lap:
		// the grid crossing the line, or the end of a qualifying outlap, whose
		// last lap time is the one from before the garage.
		// A car the server puts in its garage on the tick it crosses the line
		// (qualifying's flag) arrives with its counter reset: the lap shows
		// only as a new last lap time.
		const int32* PreviousLap = LastLapNumber.Find(Car.CarIndex);
		const int32* PreviousLastMs = LastLapTimeSeen.Find(Car.CarIndex);
		const bool bCounted = PreviousLap && *PreviousLap >= 1 && Car.CurrentLap > *PreviousLap;
		const bool bEndedInGarage = PreviousLap && *PreviousLap >= 1 && Car.bInGarage && Car.CurrentLap < *PreviousLap
			&& PreviousLastMs && *PreviousLastMs != Car.LastLapTimeMs;
		if ((bCounted || bEndedInGarage) && Car.LastLapTimeMs > 0)
		{
			Result.LapTimes.Add(Car.LastLapTimeMs / 1000.0f);
			Result.LapValid.Add(!Car.bLastLapInvalid);
			if (!Car.bLastLapInvalid)
			{
				++Result.ValidLaps;
			}
		}
		if (Car.BestLapTimeMs > 0)
		{
			Result.BestLapSeconds = Car.BestLapTimeMs / 1000.0f;
			if (Net)
			{
				if (const FApexCarTiming* Timing = Net->GetTimingBoard().Find(Car.CarIndex))
				{
					Result.BestLapSplitsSeconds.Reset(Timing->BestLapSplitsMs.Num());
					for (int32 SplitMs : Timing->BestLapSplitsMs)
					{
						Result.BestLapSplitsSeconds.Add(SplitMs / 1000.0f);
					}
					Result.BestSectorsSeconds.Reset(Timing->BestSplitsMs.Num());
					for (int32 SectorMs : Timing->BestSplitsMs)
					{
						Result.BestSectorsSeconds.Add(SectorMs / 1000.0f);
					}
				}
			}
		}

		LastLapNumber.Add(Car.CarIndex, Car.CurrentLap);
		LastLapTimeSeen.Add(Car.CarIndex, Car.LastLapTimeMs);
	}

	// Kept in order as it goes: a driver who has finished watches the results
	// screen fill in while the rest of the field is still out. A handful of
	// cars, once a frame.
	SortResults();
}

const FApexCarResult* UApexSessionRecorder::FindLocalResult() const
{
	const UApexNetSubsystem* Net = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexNetSubsystem>() : nullptr;
	if (!Net)
	{
		return nullptr;
	}

	const FString& LocalId = Net->GetPlayerId();
	for (const FApexCarResult& Result : Results)
	{
		if (!Result.bIsAi && Result.PlayerId.Equals(LocalId, ESearchCase::IgnoreCase))
		{
			return &Result;
		}
	}
	return nullptr;
}

float UApexSessionRecorder::GetConsistency() const
{
	const FApexCarResult* Local = FindLocalResult();
	if (!Local || Local->LapTimes.Num() < 2)
	{
		return 0.0f;
	}

	float Sum = 0.0f;
	for (float Lap : Local->LapTimes)
	{
		Sum += Lap;
	}
	const float Mean = Sum / Local->LapTimes.Num();
	if (Mean <= 0.0f)
	{
		return 0.0f;
	}

	float Variance = 0.0f;
	for (float Lap : Local->LapTimes)
	{
		Variance += FMath::Square(Lap - Mean);
	}
	const float StdDev = FMath::Sqrt(Variance / Local->LapTimes.Num());

	// Spread as a share of the average lap, inverted: identical laps give 1.
	return FMath::Clamp(1.0f - StdDev / Mean, 0.0f, 1.0f);
}
