#include "ApexProfiles.h"

#include "ApexProfileSave.h"
#include "ApexSettingsSave.h"
#include "ApexSim.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/Parse.h"

namespace ApexProfiles
{
	UApexProfileIndexSave* LoadIndex()
	{
		UApexProfileIndexSave* Index = nullptr;
		if (UGameplayStatics::DoesSaveGameExist(UApexProfileIndexSave::SlotName, 0))
		{
			Index = Cast<UApexProfileIndexSave>(UGameplayStatics::LoadGameFromSlot(UApexProfileIndexSave::SlotName, 0));
		}
		if (!Index)
		{
			Index = Cast<UApexProfileIndexSave>(UGameplayStatics::CreateSaveGameObject(UApexProfileIndexSave::StaticClass()));
		}
		// Entries whose slots are gone (deleted by hand) leave the list.
		Index->Profiles.RemoveAll([](const FApexProfileEntry& Entry)
		{
			return !Entry.Id.IsValid() || Entry.ProfileSlot.IsEmpty() || Entry.SettingsSlot.IsEmpty();
		});
		if (Index->Profiles.Num() == 0)
		{
			// Before profiles there was one save: it becomes the first
			// profile, under its own name and the id it may already carry.
			FString Name = TEXT("Player");
			FGuid Id;
			if (UGameplayStatics::DoesSaveGameExist(UApexProfileSave::SlotName, 0))
			{
				if (const UApexProfileSave* Old = Cast<UApexProfileSave>(UGameplayStatics::LoadGameFromSlot(UApexProfileSave::SlotName, 0)))
				{
					Name = Old->DriverName;
					Id = Old->DriverId;
				}
			}
			SeedFirstProfile(*Index, Name, Id.IsValid() ? Id : FGuid::NewGuid());
			SaveIndex(Index);
			UE_LOG(LogApexSim, Log, TEXT("Profiles: started the list with '%s' on the existing save"), *Index->Profiles[0].Name);
		}
		return Index;
	}

	bool SaveIndex(UApexProfileIndexSave* Index)
	{
		return Index && UGameplayStatics::SaveGameToSlot(Index, UApexProfileIndexSave::SlotName, 0);
	}

	void SeedFirstProfile(UApexProfileIndexSave& Index, const FString& Name, const FGuid& Id)
	{
		FApexProfileEntry& Entry = Index.Profiles.AddDefaulted_GetRef();
		Entry.Id = Id;
		Entry.Name = CleanName(Name);
		Entry.ProfileSlot = UApexProfileSave::SlotName;
		Entry.SettingsSlot = UApexSettingsSave::SlotName;
		Index.Current = Id;
	}

	bool CommandLineChoosesProfile(const TCHAR* CommandLine)
	{
		FString Wanted;
		return CommandLine && FParse::Value(CommandLine, TEXT("-ApexProfile="), Wanted) && !Wanted.IsEmpty();
	}

	const FApexProfileEntry* Chosen(const UApexProfileIndexSave& Index, const TCHAR* CommandLine)
	{
		if (Index.Profiles.Num() == 0)
		{
			return nullptr;
		}
		FString Wanted;
		if (CommandLine && FParse::Value(CommandLine, TEXT("-ApexProfile="), Wanted) && !Wanted.IsEmpty())
		{
			for (const FApexProfileEntry& Entry : Index.Profiles)
			{
				if (Entry.Name.Equals(Wanted, ESearchCase::IgnoreCase))
				{
					return &Entry;
				}
			}
			if (Wanted.IsNumeric() && Index.Profiles.IsValidIndex(FCString::Atoi(*Wanted) - 1))
			{
				return &Index.Profiles[FCString::Atoi(*Wanted) - 1];
			}
			UE_LOG(LogApexSim, Warning, TEXT("Profiles: -ApexProfile=%s names no profile; opening the last one used"), *Wanted);
		}
		const FApexProfileEntry* Last = Index.Profiles.FindByPredicate([&Index](const FApexProfileEntry& Entry) { return Entry.Id == Index.Current; });
		return Last ? Last : &Index.Profiles[0];
	}

	FApexProfileEntry MakeEntry(const FString& Name, int32 Portrait)
	{
		FApexProfileEntry Entry;
		Entry.Id = FGuid::NewGuid();
		Entry.Name = CleanName(Name);
		Entry.Portrait = Portrait;
		const FString Tag = Entry.Id.ToString(EGuidFormats::Digits).Left(8).ToLower();
		Entry.ProfileSlot = FString::Printf(TEXT("%s_%s"), UApexProfileSave::SlotName, *Tag);
		Entry.SettingsSlot = FString::Printf(TEXT("%s_%s"), UApexSettingsSave::SlotName, *Tag);
		return Entry;
	}

	FString CleanName(const FString& Name)
	{
		FString Clean = Name.TrimStartAndEnd().Left(MaxNameLength).TrimEnd();
		return Clean.IsEmpty() ? FString(TEXT("Player")) : Clean;
	}

	FString WireId(const FGuid& Id)
	{
		return Id.IsValid() ? Id.ToString(EGuidFormats::DigitsWithHyphensLower) : FString();
	}

	FString CarBestKey(const FString& TrackId, const FString& CarId)
	{
		return TrackId.ToLower() + TEXT("|") + CarId.ToLower();
	}

	void FileSession(FApexDriverStats& Stats, const FApexSessionStats& Session)
	{
		Stats.Sessions += 1;
		Stats.Laps += FMath::Max(0, Session.Laps);
		Stats.DistanceM += FMath::Max(0.0, Session.DistanceM);
		Stats.Incidents += FMath::Max(0, Session.Incidents);
		// A race counts when the player took the flag in it; a win needs
		// someone to beat.
		if (Session.Mode == EApexGameMode::Race && Session.FinishPosition > 0)
		{
			Stats.Races += 1;
			if (Session.FieldSize > 1)
			{
				Stats.Wins += Session.FinishPosition == 1 ? 1 : 0;
				Stats.Podiums += Session.FinishPosition <= 3 ? 1 : 0;
			}
		}
		if (Session.BestLapSeconds > 0.0f && !Session.TrackId.IsEmpty() && !Session.CarId.IsEmpty())
		{
			float& Best = Stats.CarBests.FindOrAdd(CarBestKey(Session.TrackId, Session.CarId), 0.0f);
			if (Best <= 0.0f || Session.BestLapSeconds < Best)
			{
				Best = Session.BestLapSeconds;
			}
		}
	}
}
