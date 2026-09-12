#pragma once

#include "CoreMinimal.h"
#include "ApexProtocolTypes.h"
// Included rather than forward-declared: the UHT-generated code instantiates
// the TUniquePtr deleters, which need the complete types.
#include "ApexTcpConnection.h"
#include "ApexUdpConnection.h"
#include "Containers/Ticker.h"
#include "Subsystems/GameInstanceSubsystem.h"

#include "ApexNetSubsystem.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FApexOnConnectionStateChanged, EApexConnectionState, NewState, const FString&, Detail);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FApexOnAuthSucceeded, const FString&, PlayerId, int32, ServerVersion);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FApexOnAuthFailed, const FString&, Reason);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FApexOnLobbyStateUpdated, const FApexLobbyState&, LobbyState);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FApexOnSessionJoined, const FString&, SessionId, int32, GridPosition);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FApexOnSessionLeft);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FApexOnSessionStarting, int32, CountdownSeconds);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FApexOnGameModeChanged, EApexGameMode, NewMode);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FApexOnCountdownUpdate, int32, SecondsRemaining);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FApexOnServerError, int32, Code, const FString&, Message);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FApexOnPlayerDisconnected, const FString&, PlayerId);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FApexOnDisconnected, const FString&, Reason);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FApexOnSessionRosterUpdated, const FApexSessionRoster&, Roster);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FApexOnRacingLineUpdated, const FApexRacingLineData&, Line);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FApexOnTelemetry, const FApexTelemetryFrame&, Frame);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FApexOnUdpReady);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FApexOnSessionStateChanged, EApexSessionState, NewState);
DECLARE_MULTICAST_DELEGATE_OneParam(FApexOnDemoSessionChanged, bool /*bJoined*/);

/**
 * Owns the connection to the ApexSim server and translates it into Blueprint
 * delegates and calls. This is the only thing the UI talks to.
 *
 * Rate-limit discipline (transport.rs:31-37): control traffic is limited to
 * 10 msg/s sustained with a burst of 20, and 200 violations force a
 * disconnect. The server already broadcasts LobbyState every ~2 s, so nothing
 * here polls RequestLobbyState on a timer and manual refresh is debounced.
 */
UCLASS()
class APEXSIMNET_API UApexNetSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	UApexNetSubsystem();
	/**
	 * Defined out of line: TUniquePtr<FApexTcpConnection> needs the complete
	 * type to destroy, and FApexTcpConnection is only forward-declared here.
	 */
	virtual ~UApexNetSubsystem() override;

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	// --- Delegates ------------------------------------------------------------

	UPROPERTY(BlueprintAssignable, Category = "ApexSim|Net")
	FApexOnConnectionStateChanged OnConnectionStateChanged;

	UPROPERTY(BlueprintAssignable, Category = "ApexSim|Net")
	FApexOnAuthSucceeded OnAuthSucceeded;

	UPROPERTY(BlueprintAssignable, Category = "ApexSim|Net")
	FApexOnAuthFailed OnAuthFailed;

	UPROPERTY(BlueprintAssignable, Category = "ApexSim|Net")
	FApexOnLobbyStateUpdated OnLobbyStateUpdated;

	UPROPERTY(BlueprintAssignable, Category = "ApexSim|Net")
	FApexOnSessionJoined OnSessionJoined;

	UPROPERTY(BlueprintAssignable, Category = "ApexSim|Net")
	FApexOnSessionLeft OnSessionLeft;

	UPROPERTY(BlueprintAssignable, Category = "ApexSim|Net")
	FApexOnSessionStarting OnSessionStarting;

	UPROPERTY(BlueprintAssignable, Category = "ApexSim|Net")
	FApexOnGameModeChanged OnGameModeChanged;

	UPROPERTY(BlueprintAssignable, Category = "ApexSim|Net")
	FApexOnCountdownUpdate OnCountdownUpdate;

	UPROPERTY(BlueprintAssignable, Category = "ApexSim|Net")
	FApexOnServerError OnServerError;

	UPROPERTY(BlueprintAssignable, Category = "ApexSim|Net")
	FApexOnPlayerDisconnected OnPlayerDisconnected;

	UPROPERTY(BlueprintAssignable, Category = "ApexSim|Net")
	FApexOnDisconnected OnDisconnected;

	/** Car index -> player identity. Arrives reliably over TCP on join and on change. */
	UPROPERTY(BlueprintAssignable, Category = "ApexSim|Race")
	FApexOnSessionRosterUpdated OnSessionRosterUpdated;

	/**
	 * The racing line for the local car, from the server once per session join.
	 * Also fires with an empty line when the session is left.
	 */
	UPROPERTY(BlueprintAssignable, Category = "ApexSim|Race")
	FApexOnRacingLineUpdated OnRacingLineUpdated;

	/** Fires once the UDP handshake is acknowledged and telemetry can flow. */
	UPROPERTY(BlueprintAssignable, Category = "ApexSim|Race")
	FApexOnUdpReady OnUdpReady;

	/** One decoded telemetry frame, on the game thread. ~60Hz by default. */
	UPROPERTY(BlueprintAssignable, Category = "ApexSim|Race")
	FApexOnTelemetry OnTelemetry;

	/**
	 * Session state transitions, derived from the telemetry stream.
	 *
	 * `StartSession` moves the server to Countdown without sending any TCP
	 * message about it, so the telemetry frame — which carries state and mode on
	 * every packet — is the only reliable source.
	 */
	UPROPERTY(BlueprintAssignable, Category = "ApexSim|Race")
	FApexOnSessionStateChanged OnSessionStateChanged;

	/**
	 * The menu's demo session was joined (true) or left (false), which covers
	 * a failed request too. A demo session raises none of the session
	 * delegates above — no OnSessionJoined, OnSessionLeft, OnSessionStateChanged
	 * or OnGameModeChanged — so nothing that reacts to the player's own session
	 * sees it. The roster and telemetry still flow: the race director draws it.
	 */
	FApexOnDemoSessionChanged OnDemoSessionChanged;

	// --- Actions --------------------------------------------------------------

	/**
	 * Connects and authenticates. Any existing connection is torn down first.
	 *
	 * A refused or lost connection is retried automatically on a backoff (see
	 * EApexConnectionState::Reconnecting) until Disconnect() is called or the
	 * server rejects the credentials: the usual reason a connect fails is a
	 * local server that has not started yet, and the lobby should fill in on
	 * its own once it has.
	 */
	UFUNCTION(BlueprintCallable, Category = "ApexSim|Net")
	void Connect(
		const FString& InHost = TEXT("127.0.0.1"),
		int32 InPort = 9000,
		const FString& InPlayerName = TEXT("Player"),
		const FString& InToken = TEXT("dev-token"));

	/** Sends Disconnect (best effort), closes the socket and stops any retry. */
	UFUNCTION(BlueprintCallable, Category = "ApexSim|Net")
	void Disconnect();

	/**
	 * Asks for a lobby snapshot. Debounced — calls inside the debounce window
	 * are dropped rather than queued, because the server charges violations for
	 * exceeding 10 control messages a second.
	 */
	UFUNCTION(BlueprintCallable, Category = "ApexSim|Net")
	bool RequestLobbyState();

	UFUNCTION(BlueprintCallable, Category = "ApexSim|Net")
	void SelectCar(const FString& CarConfigId);

	UFUNCTION(BlueprintCallable, Category = "ApexSim|Net")
	void CreateSession(
		const FString& TrackConfigId,
		int32 MaxPlayers = 8,
		int32 AiCount = 0,
		int32 LapLimit = 5,
		EApexSessionKind SessionKind = EApexSessionKind::Multiplayer);

	/**
	 * Ask for an AI-only race to watch behind the menu (SessionKind::Demo).
	 * Answered with OnDemoSessionChanged, never with the session delegates.
	 */
	void CreateDemoSession(const FString& TrackConfigId, int32 AiCount, int32 LapLimit);

	/** Leave the demo session, or withdraw a request for one. Nothing if there is neither. */
	void LeaveDemoSession();

	/** Joined to a demo session. `IsInSession` is false throughout one. */
	bool IsInDemoSession() const { return bInDemoSession; }

	/** A demo session has been asked for and not yet answered. */
	bool IsDemoSessionRequested() const { return bDemoRequested; }

	/** State of the demo session from its telemetry; Lobby before the first frame. */
	EApexSessionState GetDemoSessionState() const { return DemoSessionState; }

	/**
	 * A create or join for a session the player takes part in is on its way
	 * and has not been answered by SessionJoined or an Error yet. The demo
	 * waits for it rather than taking the connection back.
	 */
	bool IsSessionRequestPending() const
	{
		// A request the server never answers must not park the demo for good.
		return bSessionRequestPending && FPlatformTime::Seconds() - SessionRequestSentSeconds < 10.0;
	}

	UFUNCTION(BlueprintCallable, Category = "ApexSim|Net")
	void JoinSession(const FString& SessionId);

	UFUNCTION(BlueprintCallable, Category = "ApexSim|Net")
	void JoinAsSpectator(const FString& SessionId);

	UFUNCTION(BlueprintCallable, Category = "ApexSim|Net")
	void LeaveSession();

	/**
	 * Sends StartSession. NOT wired to the menu's "Start Race" button — that is
	 * deliberately a no-op, because starting a session puts the server into a
	 * driving mode and begins broadcasting telemetry to a client that has no
	 * track view. Exposed for when the racing client lands.
	 */
	UFUNCTION(BlueprintCallable, Category = "ApexSim|Net")
	void StartSession();

	UFUNCTION(BlueprintCallable, Category = "ApexSim|Net")
	void SetGameMode(EApexGameMode Mode);

	UFUNCTION(BlueprintCallable, Category = "ApexSim|Net")
	void StartCountdown(int32 Seconds, EApexGameMode NextMode);

	/**
	 * Driver aids the server runs for this player. Both live on the server
	 * because they need what the protocol never sends: the automatic gearbox
	 * the car's redline and ratios, speed-sensitive steering its grip,
	 * downforce and wheelbase. The client only forwards the settings, on
	 * joining a session and whenever they change.
	 */
	UFUNCTION(BlueprintCallable, Category = "ApexSim|Net")
	void SetDriverAids(bool bAutoGearbox, bool bSteeringAssist);

	// --- State ----------------------------------------------------------------

	UFUNCTION(BlueprintPure, Category = "ApexSim|Net")
	EApexConnectionState GetConnectionState() const { return ConnectionState; }

	UFUNCTION(BlueprintPure, Category = "ApexSim|Net")
	bool IsAuthenticated() const { return ConnectionState == EApexConnectionState::Authenticated; }

	UFUNCTION(BlueprintPure, Category = "ApexSim|Net")
	const FString& GetPlayerId() const { return PlayerId; }

	UFUNCTION(BlueprintPure, Category = "ApexSim|Net")
	const FString& GetPlayerName() const { return PlayerName; }

	UFUNCTION(BlueprintPure, Category = "ApexSim|Net")
	const FString& GetCurrentSessionId() const { return CurrentSessionId; }

	/** In a session the player takes part in: a demo session does not count. */
	UFUNCTION(BlueprintPure, Category = "ApexSim|Net")
	bool IsInSession() const { return !CurrentSessionId.IsEmpty() && !bInDemoSession; }

	UFUNCTION(BlueprintPure, Category = "ApexSim|Net")
	const FApexLobbyState& GetCachedLobbyState() const { return CachedLobbyState; }

	/**
	 * Round trip of the most recent heartbeat, in milliseconds; -1 until one
	 * completes. Refreshed every HeartbeatIntervalSeconds, so it is a coarse
	 * health indicator rather than a live latency read.
	 */
	UFUNCTION(BlueprintPure, Category = "ApexSim|Net")
	int32 GetPingMs() const { return PingMs; }

	UFUNCTION(BlueprintPure, Category = "ApexSim|Net")
	bool FindCarById(const FString& CarId, FApexCarConfigSummary& OutCar) const;

	UFUNCTION(BlueprintPure, Category = "ApexSim|Net")
	bool FindTrackById(const FString& TrackId, FApexTrackConfigSummary& OutTrack) const;

	UFUNCTION(BlueprintPure, Category = "ApexSim|Net")
	bool FindSessionById(const FString& SessionId, FApexSessionSummary& OutSession) const;

	// --- Race / UDP -----------------------------------------------------------

	/**
	 * Sets the controls sent to the server. Cheap — call every frame.
	 * Does nothing until the UDP handshake has completed.
	 */
	UFUNCTION(BlueprintCallable, Category = "ApexSim|Race")
	void SetPlayerInput(const FApexPlayerInput& Input);

	UFUNCTION(BlueprintPure, Category = "ApexSim|Race")
	bool IsUdpReady() const;

	UFUNCTION(BlueprintPure, Category = "ApexSim|Race")
	const FApexSessionRoster& GetSessionRoster() const { return CachedRoster; }

	/** The current session's racing line; empty (not IsValid) until one arrives. */
	UFUNCTION(BlueprintPure, Category = "ApexSim|Race")
	const FApexRacingLineData& GetRacingLine() const { return CachedRacingLine; }

	/** The most recent telemetry frame, for anything that polls rather than binds. */
	UFUNCTION(BlueprintPure, Category = "ApexSim|Race")
	const FApexTelemetryFrame& GetLatestTelemetry() const { return LatestTelemetry; }

	/**
	 * What the local car's driver should feel: every `DriverFeedback` taken
	 * off the socket since the last net tick, merged into one (see
	 * FApexDriverFeedback::Absorb). Polled by the force-feedback devices.
	 */
	const FApexDriverFeedback& GetDriverFeedback() const { return LatestDriverFeedback; }

	/** Bumps each net tick that brought new feedback, so a device fires each transient once. */
	uint32 GetDriverFeedbackSerial() const { return DriverFeedbackSerial; }

	/** FPlatformTime::Seconds when feedback last arrived; 0 before the first. The stream stops when the car does. */
	double GetDriverFeedbackTime() const { return DriverFeedbackTime; }

	/** The car index assigned to this player, or -1 if the roster has no entry yet. */
	UFUNCTION(BlueprintPure, Category = "ApexSim|Race")
	int32 GetLocalCarIndex() const;

	UFUNCTION(BlueprintPure, Category = "ApexSim|Race")
	EApexSessionState GetSessionState() const { return CurrentSessionState; }

	UFUNCTION(BlueprintPure, Category = "ApexSim|Race")
	EApexGameMode GetGameMode() const { return CurrentGameMode; }

private:
	/** Server heartbeat timeout is 5000 ms; 2 s leaves generous margin. */
	static constexpr float HeartbeatIntervalSeconds = 2.0f;
	static constexpr float LobbyStateDebounceSeconds = 1.0f;

	/** Retry delays double from the first value up to the cap: 1, 2, 4, 5, 5... */
	static constexpr float ReconnectInitialDelaySeconds = 1.0f;
	static constexpr float ReconnectMaxDelaySeconds = 5.0f;

	bool Tick(float DeltaSeconds);
	void HandleMessage(const FApexServerMessage& Message);
	void SetConnectionState(EApexConnectionState NewState, const FString& Detail);
	void SendPayload(TArray<uint8>&& Payload);
	void TeardownConnection();

	/** Spawns the TCP thread for Host:Port with the remembered credentials. */
	void StartConnectionAttempt();

	/** Moves to Reconnecting and arms the backoff timer for the next attempt. */
	void ScheduleReconnect(const FString& Reason);

	/** Starts the UDP side once AuthSuccess has handed over a token and port. */
	void StartUdp(const FApexAuthSuccess& Auth);

	TUniquePtr<FApexTcpConnection> Connection;
	TUniquePtr<FApexUdpConnection> UdpConnection;
	FTSTicker::FDelegateHandle TickerHandle;

	UPROPERTY()
	FApexSessionRoster CachedRoster;

	UPROPERTY()
	FApexRacingLineData CachedRacingLine;

	/** Forget the racing line, telling listeners if there was one. */
	void ClearRacingLine();

	UPROPERTY()
	FApexTelemetryFrame LatestTelemetry;

	FApexDriverFeedback LatestDriverFeedback;
	uint32 DriverFeedbackSerial = 0;
	double DriverFeedbackTime = 0.0;

	/** Latched so OnUdpReady fires exactly once per connection. */
	bool bUdpReadyBroadcast = false;

	EApexSessionState CurrentSessionState = EApexSessionState::Lobby;
	EApexGameMode CurrentGameMode = EApexGameMode::Lobby;

	EApexConnectionState ConnectionState = EApexConnectionState::Disconnected;

	UPROPERTY()
	FApexLobbyState CachedLobbyState;

	FString Host;
	int32 Port = 9000;
	FString PlayerName;
	/** Kept for reconnects; the server reads it once per connection. */
	FString Token;
	FString PlayerId;

	/** Set by Connect(), cleared by Disconnect() and by an AuthFailure. */
	bool bReconnectEnabled = false;
	int32 ReconnectAttempt = 0;
	float TimeUntilReconnect = 0.0f;
	FString CurrentSessionId;

	// --- Demo session -----------------------------------------------------------

	/** Clear the demo bookkeeping, telling listeners if a demo was joined or asked for. */
	void ResetDemoSession();

	bool bInDemoSession = false;
	bool bDemoRequested = false;
	/**
	 * LeaveSession messages sent on the demo's behalf that the server has not
	 * answered yet. The server answers every LeaveSession with SessionLeft, in
	 * order, so the next that many SessionLefts belong to the demo — including
	 * one for a demo request that failed and left nothing to leave.
	 */
	int32 DemoLeavesInFlight = 0;
	/** Whether every car in a telemetry frame is in the current roster (a stale frame of another session is not). */
	bool FrameFitsRoster(const FApexTelemetryFrame& Frame) const;
	EApexSessionState DemoSessionState = EApexSessionState::Lobby;
	bool bSessionRequestPending = false;
	double SessionRequestSentSeconds = 0.0;

	uint32 ClientTick = 0;
	float TimeSinceHeartbeat = 0.0f;

	/** When the outstanding heartbeat went out; 0 when none is in flight. */
	double HeartbeatSentSeconds = 0.0;
	int32 PingMs = -1;

	float TimeSinceLobbyRequest = LobbyStateDebounceSeconds;
	bool bWarnedEmptyCatalog = false;
};
