#pragma once

#include "CoreMinimal.h"
#include "ApexProtocolTypes.h"
#include "GameFramework/SaveGame.h"

#include "ApexProfiles.generated.h"

struct FApexDriverStats;

/** One driver profile on this machine, as the index lists it. */
USTRUCT()
struct APEXSIM_API FApexProfileEntry
{
	GENERATED_BODY()

	/** The profile's driver id (UApexProfileSave::DriverId). */
	UPROPERTY()
	FGuid Id;

	/** Copies of the profile's own name and picture, so the list needs no profile loaded. */
	UPROPERTY()
	FString Name;

	UPROPERTY()
	int32 Portrait = 0;

	/** The save slots this profile's UApexProfileSave and UApexSettingsSave live in. */
	UPROPERTY()
	FString ProfileSlot;

	UPROPERTY()
	FString SettingsSlot;
};

/**
 * The driver profiles on this machine and which one was used last
 * (`ApexProfiles.sav`). No account and no service: a profile is a pair of
 * save slots and an id made when it was created.
 */
UCLASS()
class APEXSIM_API UApexProfileIndexSave : public USaveGame
{
	GENERATED_BODY()

public:
	static constexpr const TCHAR* SlotName = TEXT("ApexProfiles");

	UPROPERTY()
	TArray<FApexProfileEntry> Profiles;

	/** The profile used last; the one a launch opens unless asked for another. */
	UPROPERTY()
	FGuid Current;
};

/** What one session the player drove adds to their stats (ApexProfiles::FileSession). */
struct FApexSessionStats
{
	EApexGameMode Mode = EApexGameMode::FreePractice;
	FString TrackId;
	FString CarId;
	int32 Laps = 0;
	double DistanceM = 0.0;
	/** The best lap of the session, seconds; 0 for none. */
	float BestLapSeconds = 0.0f;
	/** The server's classified position at the flag; 0 when not classified. */
	int32 FinishPosition = 0;
	/** Cars in the session, the player's own included. */
	int32 FieldSize = 0;
	int32 Incidents = 0;
};

namespace ApexProfiles
{
	/** The most profiles one machine keeps. */
	constexpr int32 MaxProfiles = 16;
	/** The longest driver name. */
	constexpr int32 MaxNameLength = 24;

	/**
	 * The index, read from its slot. An install from before profiles has
	 * none: its `ApexProfile` and `ApexSettings` slots become the first
	 * profile (`FirstProfileName` names it). Never null.
	 */
	APEXSIM_API UApexProfileIndexSave* LoadIndex();
	APEXSIM_API bool SaveIndex(UApexProfileIndexSave* Index);

	/** An index with one profile on the pre-profile slots, for a machine that has none. */
	APEXSIM_API void SeedFirstProfile(UApexProfileIndexSave& Index, const FString& Name, const FGuid& Id);

	/**
	 * The profile a launch opens: `-ApexProfile=` (a name, or a 1-based
	 * place in the list) when it names one, else the one used last, else
	 * the first. Null only for an empty index.
	 */
	APEXSIM_API const FApexProfileEntry* Chosen(const UApexProfileIndexSave& Index, const TCHAR* CommandLine);

	/** Whether the command line chose a profile (`-ApexProfile=`), so the launch picker stays shut. */
	APEXSIM_API bool CommandLineChoosesProfile(const TCHAR* CommandLine);

	/** A new profile's entry: its own slots, named after its id. */
	APEXSIM_API FApexProfileEntry MakeEntry(const FString& Name, int32 Portrait);

	/** A name as a profile keeps it: trimmed, bounded, never empty. */
	APEXSIM_API FString CleanName(const FString& Name);

	/** The id as it goes on the wire (`Authenticate.driver_id`): lower-case with hyphens. */
	APEXSIM_API FString WireId(const FGuid& Id);

	/** Add one session to a driver's stats; pure, for the tests. */
	APEXSIM_API void FileSession(FApexDriverStats& Stats, const FApexSessionStats& Session);

	/** The key a per-car best is kept under in FApexDriverStats::CarBests. */
	APEXSIM_API FString CarBestKey(const FString& TrackId, const FString& CarId);
}
