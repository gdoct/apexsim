#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Subsystems/GameInstanceSubsystem.h"

#include "ApexDemoModeSubsystem.generated.h"

class AApexRaceDirector;
class UApexMenuFlowSubsystem;
class UApexNetSubsystem;

/**
 * Demo mode: an AI-only race playing behind the menu, filmed by the
 * broadcast camera.
 *
 * Whenever the client is connected and the player is not in a session, this
 * asks the server for a `SessionKind::Demo` session (an unlisted AI race the
 * client spectates) on the player's chosen track, and hands it to the race
 * director's demo view. The shell fades its backdrop in by the director's
 * opacity. A finished race, a change of track or a long enough run fades
 * out and starts another.
 *
 * The player never has to get it out of the way: creating or joining a
 * session leaves the demo first (UApexNetSubsystem does that), and nothing
 * here asks for another until the player is out of their session again.
 *
 * `apexsim.demo.Enabled 0` or `-ApexNoDemo` turns it off; `-ApexAutoRace`
 * runs never start one.
 */
UCLASS()
class APEXSIM_API UApexDemoModeSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/** True while the demo is joined and on the director. */
	bool IsDemoRunning() const { return bViewBegun; }

	/** -ApexNoDemo, -ApexAutoRace or apexsim.demo.Enabled 0. */
	static bool IsDemoDisabled();

	/**
	 * Whether a demo is on its way: allowed, and nothing has yet shown it will
	 * not come (no server, a rejected login, no track with a level, a request
	 * the server turned down). The startup splash waits only while this holds.
	 */
	bool IsDemoExpected() const;

	/** The track the demo is on (or being started on), by id. */
	const FString& GetDemoTrackId() const { return TrackId; }

private:
	bool Tick(float DeltaSeconds);

	/** Everything that must hold for a demo to run; false tears one down. */
	bool IsDemoAllowed(const UApexNetSubsystem& Net) const;

	/**
	 * Pick the circuit: the player's own track when it has a level, since the
	 * main menu's title names it, else another track with a level.
	 */
	bool ChooseTrack(const UApexNetSubsystem& Net);

	/** Fade out, then leave; the next tick starts again if allowed. */
	void Restart(const TCHAR* Why);

	void HandleDemoSessionChanged(bool bJoined);

	UApexNetSubsystem* GetNet() const;
	UApexMenuFlowSubsystem* GetFlow() const;
	AApexRaceDirector* GetDirector() const;

	FTSTicker::FDelegateHandle TickerHandle;
	FDelegateHandle DemoChangedHandle;

	/** The chosen circuit: lobby id, level stem, and centerline for the cameras. */
	FString TrackId;
	FString TrackStem;
	TArray<FVector2D> Centerline;
	FString LastTrackId;

	bool bViewBegun = false;
	/** Fading out ahead of a restart. */
	bool bRestarting = false;
	/** Seconds until another demo may be asked for. */
	float Cooldown = 0.0f;
	/** Seconds the current demo has run. */
	float RunningFor = 0.0f;
	/** Seconds its race has been over. */
	float FinishedFor = 0.0f;
	/** Consecutive requests that came to nothing, for the back-off. */
	int32 Failures = 0;
	bool bWarnedNoTracks = false;
};
