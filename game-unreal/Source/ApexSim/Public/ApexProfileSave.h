#pragma once

#include "CoreMinimal.h"
#include "ApexProtocolTypes.h"
#include "GameFramework/SaveGame.h"

#include "ApexProfileSave.generated.h"

/**
 * The little the client remembers between runs.
 *
 * The server keeps no per-player state — it has no account model, and a
 * `LobbyState` describes only what is available right now — so anything the
 * design calls "yours" (the setup you left off with, your best lap on a track)
 * has to be stored locally or not exist at all.
 *
 * Written through UApexMenuFlowSubsystem; nothing else should touch the slot.
 */
UCLASS()
class APEXSIM_API UApexProfileSave : public USaveGame
{
	GENERATED_BODY()

public:
	static constexpr const TCHAR* SlotName = TEXT("ApexProfile");

	UPROPERTY()
	FString DriverName = TEXT("Player");

	UPROPERTY()
	FString ServerHost = TEXT("127.0.0.1");

	UPROPERTY()
	int32 ServerPort = 9000;

	/** Content UUIDs, matched against the catalog and the server's lobby state. */
	UPROPERTY()
	FString LastCarId;

	UPROPERTY()
	FString LastTrackId;

	UPROPERTY()
	int32 MaxPlayers = 8;

	UPROPERTY()
	int32 AiCount = 0;

	UPROPERTY()
	int32 LapLimit = 5;

	UPROPERTY()
	EApexGameMode StartingMode = EApexGameMode::FreePractice;

	UPROPERTY()
	EApexSessionKind SessionKind = EApexSessionKind::Multiplayer;

	/** Track UUID -> best lap in seconds. Populated by the results screen. */
	UPROPERTY()
	TMap<FString, float> BestLapSeconds;
};
