#include "Race/ApexLapTraceSubsystem.h"

#include "ApexNetSubsystem.h"
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
			UE_LOG(LogApexSim, Display, TEXT("Lap traces: %d recorded this run; last %d ms (%d samples), best %d ms (%d samples%s); files in %s"),
				Traces->GetLapsRecorded(), Traces->GetLast().LapMs, Traces->GetLast().Samples.Num(),
				Traces->GetBest().LapMs, Traces->GetBest().Samples.Num(), Traces->IsBestStored() ? TEXT(", stored") : TEXT(""),
				*Traces->GetFolder());
		}));
}

void UApexLapTraceSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Collection.InitializeDependency<UApexNetSubsystem>();
	if (UApexNetSubsystem* Net = GetGameInstance()->GetSubsystem<UApexNetSubsystem>())
	{
		Net->OnTelemetry.AddDynamic(this, &UApexLapTraceSubsystem::HandleTelemetry);
	}
}

void UApexLapTraceSubsystem::Deinitialize()
{
	if (UApexNetSubsystem* Net = GetGameInstance()->GetSubsystem<UApexNetSubsystem>())
	{
		Net->OnTelemetry.RemoveDynamic(this, &UApexLapTraceSubsystem::HandleTelemetry);
	}
	Super::Deinitialize();
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
	Take(Frame, LocalIndex, TrackName, CarId);
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
	Best = FApexLapTrace();
	bBestStored = false;
	Folder = FolderFor(TrackName, CarId);
	if (!bWriteFiles)
	{
		return;
	}
	// The quickest valid lap kept from an earlier session: the file names
	// carry the time, so only the best one is read whole.
	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *FPaths::Combine(Folder, TEXT("*.csv")), true, false);
	int32 BestMs = MAX_int32;
	for (const FString& File : Files)
	{
		if (File.Contains(TEXT("_invalid")))
		{
			continue;
		}
		FString Text;
		FApexLapTrace Stored;
		if (FFileHelper::LoadFileToString(Text, *FPaths::Combine(Folder, File)) && ApexLapTrace::FromCsv(Text, Stored)
			&& Stored.bValid && Stored.LapMs > 0 && Stored.LapMs < BestMs)
		{
			BestMs = Stored.LapMs;
			Best = MoveTemp(Stored);
			bBestStored = true;
		}
	}
	if (bBestStored)
	{
		UE_LOG(LogApexSim, Log, TEXT("Lap traces: the stored best here is %d ms (%s)"), Best.LapMs, *Folder);
	}
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
		Last = MoveTemp(Lap);
		return;
	}
}
