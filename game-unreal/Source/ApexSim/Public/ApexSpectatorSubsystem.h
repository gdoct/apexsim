#pragma once

#include "CoreMinimal.h"
#include "ApexProtocolTypes.h"
#include "ApexSpectatorStream.h"
#include "Async/Future.h"
#include "Containers/Ticker.h"
#include "Subsystems/GameInstanceSubsystem.h"

#include "ApexSpectatorSubsystem.generated.h"

class UApexNetSubsystem;

/** Where the stream being played comes from. */
UENUM()
enum class EApexSpectatorSource : uint8
{
	None,
	/** An `.apxs` file on this machine, on the game's own clock. */
	File,
	/** The server's showcase channel, as its records arrive. */
	Net,
};

/**
 * Plays a spectator stream (docs/SPECTATOR.md) into the race view: the
 * menu backdrop, from a local `.apxs` or from the server's showcase, through
 * the paths a live race already uses.
 *
 * One player, two sources. A file is read whole and inflated on a worker
 * thread, then its records are released as the game's clock reaches their
 * tick (a fixed-timestep run stays in step); the server's records are
 * applied as they come in from UApexNetSubsystem. Either way the records go
 * through FApexSpectatorPlayer and out as the roster, telemetry frames and
 * timing of a "demo session" (UApexNetSubsystem::BeginBackdropFeed), so the
 * race director, the TV camera, the engine sound and the HUD data see what
 * they see in a live race.
 *
 * UApexDemoModeSubsystem decides what to play; `apexsim.spectate.Info`
 * prints how it is going and `apexsim.spectate.Next` asks for another.
 */
UCLASS()
class APEXSIM_API UApexSpectatorSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/**
	 * Play a local file. Loading is off the game thread; IsLoading until it
	 * is in, then IsPlaying. False when the file is not there.
	 */
	bool PlayFile(const FString& Path);

	/** Watch the server's showcase channel `Id` (empty: the server's first). */
	bool WatchShowcase(const FString& Id);

	/** Stop whatever plays and end the backdrop feed. */
	void Stop();

	EApexSpectatorSource GetSource() const { return Source; }
	bool IsLoading() const { return bLoading; }
	/** A stream is being played into the backdrop feed. */
	bool IsPlaying() const { return Source != EApexSpectatorSource::None && !bLoading && !bFailed; }
	/** The source could not deliver (a bad file, a refused showcase). Cleared by Stop. */
	bool HasFailed() const { return bFailed; }
	const FString& GetFailure() const { return Failure; }
	/** The file or the showcase id being played. */
	const FString& GetSourceName() const { return SourceName; }

	const FApexStreamHeader& GetHeader() const { return Player.GetHeader(); }
	bool HasHeader() const { return Player.HasHeader(); }
	/** The stream's centerline (where the TV cameras stand), empty when it carries none. */
	const TArray<FVector2D>& GetCenterline() const { return Centerline; }
	/** Times the file has started over. */
	int32 GetLoops() const { return Loops; }

	/** `apexsim.spectate.Next` was pressed; UApexDemoModeSubsystem takes it. */
	bool TakeNextRequested() { const bool b = bNextRequested; bNextRequested = false; return b; }
	void RequestNext() { bNextRequested = true; }

	/** One line on the source, the epoch, the frames and the drops. */
	FString DescribeState() const;

	/** `-ApexShowcaseDir=` folders, then `Showcase/` beside the executable (the repo's `build/showcase` in the editor). */
	static TArray<FString> ShowcaseDirectories();
	/** Every `.apxs` under the showcase folders, by full path. */
	static TArray<FString> FindShowcaseFiles();

private:
	struct FLoadedFile
	{
		FApexStreamFile File;
		/** Every timed record body, inflated, in order. */
		TArray<TArray<uint8>> Records;
		FString Error;
		bool bOk = false;
	};

	bool Tick(float DeltaSeconds);
	void Drain();
	void HandleSpectatorJoined(const FString& StreamId, const FString& ShowcaseId);
	void HandleSpectatorRecords(TArrayView<const uint8> Run);
	UApexNetSubsystem* GetNet() const;
	void Fail(const FString& Why);

	FTSTicker::FDelegateHandle TickerHandle;
	FDelegateHandle JoinedHandle;
	FDelegateHandle RecordsHandle;

	EApexSpectatorSource Source = EApexSpectatorSource::None;
	FString SourceName;
	bool bLoading = false;
	bool bFailed = false;
	FString Failure;
	bool bNextRequested = false;

	FApexSpectatorPlayer Player;
	TArray<FVector2D> Centerline;
	bool bFeeding = false;
	FString StreamId;

	// --- File source ---
	TFuture<TSharedPtr<FLoadedFile>> Loading;
	TSharedPtr<FLoadedFile> Loaded;
	/** The content's clock in the file's ticks, and the next record to release. */
	double Clock = 0.0;
	int32 Cursor = 0;
	int32 Loops = 0;
	/** The timed records' ticks, parallel to Loaded->Records. */
	TArray<int64> RecordTicks;
};
