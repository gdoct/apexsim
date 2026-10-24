#pragma once

#include "CoreMinimal.h"
#include "Catalog/ApexCatalogRows.h"
#include "Containers/Ticker.h"
#include "Subsystems/GameInstanceSubsystem.h"

#include "ApexLiveryExchange.generated.h"

struct FApexLiveryChunk;
struct FApexLiveryPackage;

/**
 * Players' own liveries between clients (N11; apexsim-content/docs/cars.md
 * "Own liveries online"). In a session whose pick is one of the player's own
 * (UApexNetSubsystem::IsPendingLiveryOwn) it packs that livery
 * (ApexLiveryPackage) and uploads it, once per session and pick; for every
 * other car whose roster entry names an own livery (FApexRosterEntry::OwnLivery)
 * it takes the package from Saved/ApexSim/Liveries/<hash>.apxlivery or fetches
 * it a chunk at a time, checks it (the hash, the server's caps), unpacks its
 * images beside it and says so (OnLiveryReady), and the race director paints
 * the car with it (FindWorn).
 */
UCLASS()
class APEXSIM_API UApexLiveryExchange : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/** The livery `Hash` names, unpacked, when it is here and was painted for `CarId`; else null. */
	const FApexCarLivery* FindWorn(const FString& Hash, const FString& CarId) const;

	/** A livery a roster named is here now (fetched or read from the cache): the cars wearing it can be repainted. */
	FSimpleMulticastDelegate OnLiveryReady;

	/** The hash of the livery last uploaded in this session; empty for none. */
	const FString& GetUploadedHash() const { return UploadHash; }

	/** Where the packages are kept: Saved/ApexSim/Liveries. */
	static FString CacheDir();

private:
	bool Tick(float DeltaSeconds);
	void HandleChunk(const FApexLiveryChunk& Chunk);
	UFUNCTION()
	void HandleSessionJoined(const FString& SessionId, int32 GridPosition);
	/** Pack and queue the player's own livery for the server, when the pick is one. */
	void StartUpload(const FString& CarId, int32 Livery);
	/** Take a package's bytes (cached or just fetched) as `Hash`: false when they are not that livery. */
	bool Accept(const FString& Hash, TArrayView<const uint8> Bytes, bool bSave);

	FTSTicker::FDelegateHandle TickerHandle;
	FDelegateHandle ChunkHandle;

	struct FReady
	{
		FString Car;
		FApexCarLivery Livery;
	};
	TMap<FString, FReady> Ready;
	/** Hashes the server did not have, or whose bytes did not check out: not asked for again. */
	TSet<FString> Failed;
	/** Hashes looked for in the cache once already. */
	TSet<FString> CacheTried;

	/** What was last uploaded for: session, car, pick. */
	FString UploadedKey;
	FString UploadHash;
	TArray<TArray<uint8>> UploadChunks;
	int32 UploadNext = 0;

	/** The livery being fetched, one at a time, a chunk in flight. */
	FString FetchHash;
	int32 FetchChunks = 0;
	int32 FetchNext = 0;
	int32 FetchTries = 0;
	TArray<uint8> FetchBytes;
	double FetchAskedAt = 0.0;
	bool bFetchInFlight = false;

	double LastSendSeconds = 0.0;
};

namespace ApexLiveryExchange
{
	/**
	 * The package of one of the player's own liveries: its colours and the
	 * images it names read from disk. False with a reason when a file does
	 * not read, an image is not a PNG or JPEG, or the whole is over the cap.
	 */
	APEXSIM_API bool Pack(const FApexCarLivery& Livery, const FString& CarId, FApexLiveryPackage& Out, FString& OutError);

	/** The package's images written into `Folder` and the livery that paints with them. */
	APEXSIM_API bool Unpack(const FApexLiveryPackage& Package, const FString& Folder, FApexCarLivery& Out);

	/** A hash as the wire names a livery: 64 lowercase hex digits (and so a safe file name). */
	APEXSIM_API bool IsHash(const FString& Hash);

	/** The packages kept past `MaxBytes`, oldest first: what to delete. */
	APEXSIM_API TArray<FString> ToPrune(TArray<TPair<FString, int64>> FilesOldestFirst, int64 MaxBytes);
}
