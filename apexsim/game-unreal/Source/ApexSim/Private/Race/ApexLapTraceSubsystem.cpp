#include "Race/ApexLapTraceSubsystem.h"

#include "ApexMenuFlowSubsystem.h"
#include "ApexNetSubsystem.h"
#include "Catalog/ApexCatalogRows.h"
#include "ApexSim.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
	FString LapTraceSafeName(const FString& Name)
	{
		FString Safe;
		for (const TCHAR Char : Name)
		{
			Safe.AppendChar(FChar::IsAlnum(Char) || Char == TEXT('-') || Char == TEXT('_') ? Char : TEXT('_'));
		}
		return Safe.IsEmpty() ? FString(TEXT("unknown")) : Safe;
	}

	FAutoConsoleCommandWithWorld LapTracesCommand(
		TEXT("apexsim.telemetry.Laps"),
		TEXT("The lap traces held for the pause menu's LAP TRACES page: the last lap, the best, and where the CSVs go"),
		FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* World)
		{
			UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
			const UApexLapTraceSubsystem* Traces = GameInstance ? GameInstance->GetSubsystem<UApexLapTraceSubsystem>() : nullptr;
			if (!Traces)
			{
				return;
			}
			UE_LOG(LogApexSim, Display, TEXT("Lap traces: %d recorded this run, %d held; last %d ms (%d samples), best %d ms (%d samples%s); recording %s; files in %s"),
				Traces->GetLapsRecorded(), Traces->GetLaps().Num(), Traces->GetLast().LapMs, Traces->GetLast().Samples.Num(),
				Traces->GetBest().LapMs, Traces->GetBest().Samples.Num(), Traces->IsBestStored() ? TEXT(", stored") : TEXT(""),
				*Traces->DescribeRecorder(), *Traces->GetFolder());
		}));
}

void UApexLapTraceSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Collection.InitializeDependency<UApexNetSubsystem>();
	if (UApexNetSubsystem* Net = GetGameInstance()->GetSubsystem<UApexNetSubsystem>())
	{
		Net->OnTelemetry.AddDynamic(this, &UApexLapTraceSubsystem::HandleTelemetry);
		Net->OnGhostLap.AddDynamic(this, &UApexLapTraceSubsystem::HandleGhostLap);
	}
}

void UApexLapTraceSubsystem::Deinitialize()
{
	if (UApexNetSubsystem* Net = GetGameInstance()->GetSubsystem<UApexNetSubsystem>())
	{
		Net->OnTelemetry.RemoveDynamic(this, &UApexLapTraceSubsystem::HandleTelemetry);
		Net->OnGhostLap.RemoveDynamic(this, &UApexLapTraceSubsystem::HandleGhostLap);
	}
	Super::Deinitialize();
}

void UApexLapTraceSubsystem::HandleGhostLap(const FApexGhostLap& Ghost)
{
	SetRecordLap(Ghost);
}

void UApexLapTraceSubsystem::SetRecordLap(const FApexGhostLap& Ghost)
{
	RecordGhost = Ghost;
	RefreshRecord();
}

void UApexLapTraceSubsystem::RefreshRecord()
{
	Laps.RemoveAll([](const FApexLapTrace& Lap) { return Lap.bRecord; });
	if (!RecordGhost.IsValid())
	{
		return;
	}
	// Laid on the best lap, else the newest: the player's own line. Listed
	// right after the best, one step from where AGAINST opens.
	const FApexLapTrace* Ref = !Best.IsEmpty() ? &Best : (Laps.Num() > 0 ? &Laps[0] : nullptr);
	FApexLapTrace Record = ApexLapTrace::FromGhost(RecordGhost, Ref);
	const int32 BestIndex = GetBestIndex();
	Laps.Insert(MoveTemp(Record), BestIndex != INDEX_NONE ? BestIndex + 1 : Laps.Num());
}

FString UApexLapTraceSubsystem::FolderFor(const FString& TrackName, const FString& CarId)
{
	return FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Telemetry"), LapTraceSafeName(TrackName), LapTraceSafeName(CarId)));
}

void UApexLapTraceSubsystem::HandleTelemetry(const FApexTelemetryFrame& Frame)
{
	const UApexNetSubsystem* Net = GetGameInstance()->GetSubsystem<UApexNetSubsystem>();
	// Only the player's own driving: not a backdrop, a watched race or a replay.
	if (!Net || !Net->IsInSession() || Net->IsInDemoSession() || Net->IsSessionSpectator())
	{
		return;
	}
	const int32 LocalIndex = Net->GetLocalCarIndex();
	FString CarId;
	for (const FApexRosterEntry& Entry : Net->GetSessionRoster().Entries)
	{
		if (Entry.CarIndex == LocalIndex)
		{
			CarId = Entry.CarConfigId;
		}
	}
	FApexSessionSummary Session;
	const FString TrackName = Net->FindSessionById(Net->GetCurrentSessionId(), Session) ? Session.TrackName : FString();
	if (CarId.IsEmpty() || TrackName.IsEmpty())
	{
		return;
	}
	// The folder and the CSV name the car as the catalog does, not by its id.
	FApexCarCatalogRow Row;
	const UApexMenuFlowSubsystem* Flow = GetGameInstance()->GetSubsystem<UApexMenuFlowSubsystem>();
	const FString CarName = Flow && Flow->GetCarCatalogRow(CarId, Row) && !Row.DisplayName.IsEmpty() ? Row.DisplayName : CarId;
	Take(Frame, LocalIndex, TrackName, CarName);
}

void UApexLapTraceSubsystem::AddFrameForTest(const FApexTelemetryFrame& Frame, int32 LocalCarIndex, const FString& TrackName, const FString& CarId)
{
	bWriteFiles = false;
	Take(Frame, LocalCarIndex, TrackName, CarId);
}

void UApexLapTraceSubsystem::Begin(const FString& TrackName, const FString& CarId)
{
	Recorder.Reset();
	Last = FApexLapTrace();
	Previous = FApexLapTrace();
	Best = FApexLapTrace();
	Laps.Reset();
	// Another circuit or car: the record held is not this one's.
	RecordGhost = FApexGhostLap();
	bBestStored = false;
	Folder = FolderFor(TrackName, CarId);
	if (!bWriteFiles)
	{
		return;
	}
	// The quickest valid lap kept from an earlier session: the file names
	// carry the time, so only the best one is read whole.
	// Every stored lap is read (the files are a lap each): the newest are
	// listed for the page, and the quickest valid one is the best.
	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *FPaths::Combine(Folder, TEXT("*.csv")), true, false);
	int32 BestMs = MAX_int32;
	for (const FString& File : Files)
	{
		FString Text;
		FApexLapTrace Stored;
		if (!FFileHelper::LoadFileToString(Text, *FPaths::Combine(Folder, File)) || !ApexLapTrace::FromCsv(Text, Stored))
		{
			continue;
		}
		if (Stored.bValid && Stored.LapMs > 0 && Stored.LapMs < BestMs)
		{
			BestMs = Stored.LapMs;
			Best = Stored;
			bBestStored = true;
		}
		Laps.Add(MoveTemp(Stored));
	}
	Laps.Sort([](const FApexLapTrace& A, const FApexLapTrace& B) { return A.RecordedAt > B.RecordedAt; });
	const int32 BestIndex = GetBestIndex();
	if (Laps.Num() > MaxListedLaps)
	{
		FApexLapTrace KeptBest = BestIndex >= MaxListedLaps ? Laps[BestIndex] : FApexLapTrace();
		Laps.SetNum(MaxListedLaps);
		if (!KeptBest.IsEmpty())
		{
			Laps.Add(MoveTemp(KeptBest));
		}
	}
	if (bBestStored)
	{
		UE_LOG(LogApexSim, Log, TEXT("Lap traces: the stored best here is %d ms (%s)"), Best.LapMs, *Folder);
	}
}

int32 UApexLapTraceSubsystem::GetBestIndex() const
{
	if (Best.IsEmpty())
	{
		return INDEX_NONE;
	}
	return Laps.IndexOfByPredicate([this](const FApexLapTrace& Lap)
	{
		return Lap.LapMs == Best.LapMs && Lap.RecordedAt == Best.RecordedAt && Lap.LapNumber == Best.LapNumber;
	});
}

void UApexLapTraceSubsystem::Take(const FApexTelemetryFrame& Frame, int32 LocalCarIndex, const FString& TrackName, const FString& CarId)
{
	const FString NewKey = TrackName + TEXT("|") + CarId;
	if (NewKey != Key)
	{
		Key = NewKey;
		Begin(TrackName, CarId);
	}
	for (const FApexCarTelemetry& Car : Frame.Cars)
	{
		if (Car.CarIndex != LocalCarIndex)
		{
			continue;
		}
		FApexLapTrace Lap;
		if (!Recorder.Add(Car, Lap))
		{
			return;
		}
		Lap.CarId = CarId;
		Lap.TrackId = TrackName;
		++LapsRecorded;
		if (Lap.bValid && (Best.IsEmpty() || Lap.LapMs < Best.LapMs))
		{
			Best = Lap;
			bBestStored = false;
		}
		if (bWriteFiles)
		{
			const FString Path = FPaths::Combine(Folder, ApexLapTrace::FileName(Lap));
			if (FFileHelper::SaveStringToFile(ApexLapTrace::ToCsv(Lap), *Path))
			{
				UE_LOG(LogApexSim, Log, TEXT("Lap trace: lap %d, %d ms%s, %d samples -> %s"), Lap.LapNumber, Lap.LapMs,
					Lap.bValid ? TEXT("") : TEXT(" (invalid)"), Lap.Samples.Num(), *Path);
			}
		}
		Laps.RemoveAll([](const FApexLapTrace& Held) { return Held.bRecord; });
		Laps.Insert(Lap, 0);
		// The list keeps its length: the oldest lap that is not the best goes.
		if (Laps.Num() > MaxListedLaps + 1)
		{
			const int32 Keep = GetBestIndex();
			Laps.RemoveAt(Keep == Laps.Num() - 1 ? Laps.Num() - 2 : Laps.Num() - 1);
		}
		Previous = Last;
		Last = Lap;
		RefreshRecord();
		return;
	}
}
