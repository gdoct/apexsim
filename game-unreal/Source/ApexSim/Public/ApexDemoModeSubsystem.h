#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "ApexProtocolTypes.h"
#include "Subsystems/GameInstanceSubsystem.h"

#include "ApexDemoModeSubsystem.generated.h"

class AApexRaceDirector;
class UApexMenuFlowSubsystem;
class UApexNetSubsystem;
class UApexSpectatorSubsystem;

/** Where the menu's backdrop race comes from. */
UENUM()
enum class EApexBackdropSource : uint8
{
	None,
	/** The server's showcase channel: a rendered race it plays to everyone in the menu. */
	Showcase,
	/** A rendered race from `Showcase/` on this machine, needing no server. */
	LocalFile,
	/** A `SessionKind::Demo` session simulated for this client (an older server). */
	DemoSession,
	/** A replay the player chose (UApexReplayRecorder): it stays until they leave it. */
	Replay,
};

/**
 * Demo mode: an AI race playing behind the menu, filmed by the broadcast
 * camera.
 *
 * Whenever the player is not in a session, this puts a race on the race
 * director's demo view, from the first source that can deliver one
 * (docs/SPECTATOR.md):
 *
 *  1. the server's showcase, when connected and the lobby lists one
 *     (`SpectateShowcase`, through UApexSpectatorSubsystem);
 *  2. a local `.apxs` from `Showcase/` beside the executable (the repo's
 *     `build/showcase` in the editor): the pending track's file when one
 *     matches and its checksums agree with this machine's content, else any;
 *     this needs no server, so the startup splash can end on it offline;
 *  3. a `SessionKind::Demo` session, for a server that predates showcases.
 *
 * The shell fades its backdrop in by the director's opacity. A finished race,
 * a change of track, `apexsim.spectate.Next` or a long enough run fades out
 * and starts another. The player never has to get it out of the way:
 * creating or joining a session leaves the backdrop first (UApexNetSubsystem
 * does that), and nothing here asks for another until the player is out of
 * their session again.
 *
 * A showcase's or a file's sky is the file's, because the AI raced in it;
 * only a demo session rolls one (`RollConditions`, `apexsim.demo.RandomSky`).
 * `apexsim.demo.Enabled 0` or `-ApexNoDemo` turns it all off; `-ApexNoShowcase`
 * keeps to demo sessions; `-ApexShowcase=<file|id>` names what to play;
 * `-ApexAutoRace` runs never start one.
 */
UCLASS()
class APEXSIM_API UApexDemoModeSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/** True while the backdrop is joined and on the director. */
	bool IsDemoRunning() const { return bViewBegun; }

	/** -ApexNoDemo, -ApexAutoRace or apexsim.demo.Enabled 0. */
	static bool IsDemoDisabled();

	/**
	 * Whether a backdrop is on its way: allowed, and nothing has yet shown it
	 * will not come (no server and no local file, a rejected login, no track
	 * with a level, a request the server turned down). The startup splash
	 * waits only while this holds.
	 */
	bool IsDemoExpected() const;

	/**
	 * A sky for one demo session, in quarter hours like the create screen.
	 * Weighted so the menu is mostly over dry daylight: weather sunny 38,
	 * cloudy 30, overcast 22, light rain 6, heavy rain 4 (out of 100: rain
	 * one race in ten); the clock 08:00-17:45 70%, dawn (06:00-07:45) or dusk
	 * (18:00-20:45) 20%, night 10%.
	 */
	static FApexSessionConditions RollConditions(FRandomStream& Random);

	/** The track the backdrop is on (or being started on), by id. */
	const FString& GetDemoTrackId() const { return TrackId; }

	/** What the running backdrop comes from. */
	EApexBackdropSource GetSource() const { return Source; }

	/**
	 * Put a replay (an `.apxs` from Saved/Replays) on the director in place
	 * of the backdrop: it does not loop or move on, and holds on its end.
	 * False with a reason when it cannot be played (unreadable, or a circuit
	 * this machine has no export of).
	 */
	bool PlayReplay(const FString& Path, FString& OutError);
	/** Back to the ordinary backdrop. */
	void StopReplay();
	bool IsPlayingReplay() const { return Source == EApexBackdropSource::Replay; }

	/**
	 * Which local showcase file to play for a track, if any: the track's own
	 * when it is on disk and its checksums agree with the catalog rows given
	 * (0 on either side is "unknown" and passes), else any other that does,
	 * skipping `Avoid`. Pure, for the tests: `Files` are full paths, `Headers`
	 * their headers and rosters in the same order.
	 */
	struct FFileCandidate
	{
		FString Path;
		FString TrackStem;
		FString TrackId;
		int64 TrackCrc = 0;
		/** Car config id -> content_crc of every car in the file. */
		TArray<TPair<FString, int64>> Cars;
	};
	struct FContentCrcs
	{
		/** Track id -> (stem, crc) of every track with an export on this machine. */
		TMap<FString, TPair<FString, int64>> Tracks;
		/** Car id -> crc of every car this machine has. */
		TMap<FString, int64> Cars;
	};
	static int32 ChooseFile(const TArray<FFileCandidate>& Files, const FContentCrcs& Content, const FString& PendingTrackId,
		const FString& Avoid, TArray<FString>* OutWhySkipped = nullptr);

private:
	bool Tick(float DeltaSeconds);

	/** Everything that must hold for a backdrop to run; false tears one down. */
	bool IsDemoAllowed(const UApexNetSubsystem& Net) const;

	/** Try the sources in order; true when one was started. */
	bool StartBackdrop(UApexNetSubsystem& Net);
	bool StartShowcase(UApexNetSubsystem& Net);
	bool StartLocalFile(UApexNetSubsystem& Net);
	bool StartDemoSession(UApexNetSubsystem& Net);

	/** The circuit for a demo session: the player's own track when it has a level, else another. */
	bool ChooseTrack(const UApexNetSubsystem& Net);
	/** The local showcase files with their headers, read once and kept. */
	void ScanLocalFiles();
	FContentCrcs GatherContentCrcs(const UApexNetSubsystem& Net) const;

	/** Fade out, then stop; the next tick starts again if allowed. */
	void Restart(const TCHAR* Why);
	/** Stop whatever runs, by its source. */
	void Teardown(UApexNetSubsystem& Net);

	void HandleDemoSessionChanged(bool bJoined);
	void HandleShowcases(const TArray<FApexShowcaseSummary>& Showcases);

	UApexNetSubsystem* GetNet() const;
	UApexMenuFlowSubsystem* GetFlow() const;
	UApexSpectatorSubsystem* GetSpectator() const;
	AApexRaceDirector* GetDirector() const;

	FTSTicker::FDelegateHandle TickerHandle;
	FDelegateHandle DemoChangedHandle;
	FDelegateHandle ShowcasesHandle;

	EApexBackdropSource Source = EApexBackdropSource::None;
	/** The chosen circuit: lobby id, level stem, and centerline for the cameras. */
	FString TrackId;
	FString TrackStem;
	TArray<FVector2D> Centerline;
	FString LastTrackId;
	/** The showcase channel or file last played, not to be played twice running. */
	FString LastShowcaseId;
	FString LastFile;
	/**
	 * The pending track the player was on when they asked for the next race:
	 * that request moves on round the playlist instead of favouring it, and
	 * the "player chose another track" restart ignores it until it changes.
	 */
	FString SkippedPendingTrack;

	/** `-ApexShowcase=`: a file path or a showcase id to play, and nothing else. */
	FString ForcedShowcase;
	bool bNoShowcase = false;

	/** The server's channels, asked for once per connection. */
	bool bShowcasesAsked = false;
	bool bShowcasesKnown = false;
	float SecondsSinceInit = 0.0f;
	float SecondsSinceAsked = 0.0f;

	/** Local files, scanned once. */
	bool bFilesScanned = false;
	TArray<FFileCandidate> LocalFiles;

	bool bViewBegun = false;
	/** Fading out ahead of a restart. */
	bool bRestarting = false;
	/** Seconds until another backdrop may be asked for. */
	float Cooldown = 0.0f;
	/** Seconds the current backdrop has run. */
	float RunningFor = 0.0f;
	/** Seconds its race has been over. */
	float FinishedFor = 0.0f;
	/** Consecutive requests that came to nothing, for the back-off. */
	int32 Failures = 0;
	bool bWarnedNoTracks = false;
};
