#include "ApexSessionRecorder.h"

#include "ApexMenuFlowSubsystem.h"
#include "ApexNetSubsystem.h"
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
	LastLapTimeMs.Reset();
	LapWasClean.Reset();
	PersonalBestDelta = 0.0f;
	bRecording = true;

	UApexMenuFlowSubsystem* Flow = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexMenuFlowSubsystem>() : nullptr;
	if (Flow)
	{
		TrackId = Flow->GetPendingTrackId();
		LapLimit = Flow->CreateLapLimit;
		RecordedMode = Flow->CreateStartingMode;

		FApexTrackCatalogRow Row;
		TrackLengthM = Flow->GetTrackCatalogRow(TrackId, Row) ? Row.LengthM : 0.0f;
	}

	UE_LOG(LogApexSim, Log, TEXT("Recording session on track '%s' (%d lap limit)"), *TrackId, LapLimit);
}

void UApexSessionRecorder::FinishRecording()
{
	if (!bRecording)
	{
		return;
	}
	bRecording = false;

	ApplyRosterNames();

	// Classification: most laps first, then the quickest lap. A car with no lap
	// at all sorts last however far it got.
	Results.Sort([](const FApexCarResult& A, const FApexCarResult& B)
	{
		if (A.LapsCompleted() != B.LapsCompleted())
		{
			return A.LapsCompleted() > B.LapsCompleted();
		}
		if (A.BestLapSeconds <= 0.0f) { return false; }
		if (B.BestLapSeconds <= 0.0f) { return true; }
		return A.BestLapSeconds < B.BestLapSeconds;
	});

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

	UE_LOG(LogApexSim, Log, TEXT("Session recorded: %d car(s), local best %s"),
		Results.Num(),
		Local && Local->BestLapSeconds > 0.0f
			? *UApexMenuFlowSubsystem::FormatLapTime(Local->BestLapSeconds)
			: TEXT("none"));
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
	ApplyRosterNames();
}

void UApexSessionRecorder::HandleTelemetry(const FApexTelemetryFrame& Frame)
{
	if (!bRecording)
	{
		// A frame can arrive before the state transition that starts recording.
		if (Frame.SessionState != EApexSessionState::Racing)
		{
			return;
		}
		BeginRecording();
	}

	for (const FApexCarTelemetry& Car : Frame.Cars)
	{
		FApexCarResult& Result = FindOrAddCar(Car.CarIndex);

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

		bool& bClean = LapWasClean.FindOrAdd(Car.CarIndex, true);
		if (!Car.bIsOnTrack)
		{
			bClean = false;
		}

		const int32* PreviousLap = LastLapNumber.Find(Car.CarIndex);
		if (PreviousLap && Car.CurrentLap > *PreviousLap)
		{
			// The lap counter has moved on, so the time carried in the previous
			// frame is the completed lap. CurrentLapTimeMs has already reset here.
			const int32* CompletedMs = LastLapTimeMs.Find(Car.CarIndex);
			if (CompletedMs && *CompletedMs > 0)
			{
				const float Seconds = *CompletedMs / 1000.0f;
				Result.LapTimes.Add(Seconds);
				if (Result.BestLapSeconds <= 0.0f || Seconds < Result.BestLapSeconds)
				{
					Result.BestLapSeconds = Seconds;
				}
				if (bClean)
				{
					++Result.ValidLaps;
				}
			}
			bClean = true;
		}

		LastLapNumber.Add(Car.CarIndex, Car.CurrentLap);
		LastLapTimeMs.Add(Car.CarIndex, Car.CurrentLapTimeMs);
	}
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
