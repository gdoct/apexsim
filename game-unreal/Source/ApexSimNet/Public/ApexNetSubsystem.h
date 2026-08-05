#pragma once

#include "CoreMinimal.h"
#include "ApexProtocolTypes.h"
// Included rather than forward-declared: the UHT-generated code instantiates
// TUniquePtr<FApexTcpConnection>'s deleter, which needs the complete type.
#include "ApexTcpConnection.h"
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

	// --- Actions --------------------------------------------------------------

	/** Connects and authenticates. Any existing connection is torn down first. */
	UFUNCTION(BlueprintCallable, Category = "ApexSim|Net")
	void Connect(
		const FString& InHost = TEXT("127.0.0.1"),
		int32 InPort = 9000,
		const FString& InPlayerName = TEXT("Player"),
		const FString& InToken = TEXT("dev-token"));

	/** Sends Disconnect (best effort) and closes the socket. */
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

	UFUNCTION(BlueprintPure, Category = "ApexSim|Net")
	bool IsInSession() const { return !CurrentSessionId.IsEmpty(); }

	UFUNCTION(BlueprintPure, Category = "ApexSim|Net")
	const FApexLobbyState& GetCachedLobbyState() const { return CachedLobbyState; }

	UFUNCTION(BlueprintPure, Category = "ApexSim|Net")
	bool FindCarById(const FString& CarId, FApexCarConfigSummary& OutCar) const;

	UFUNCTION(BlueprintPure, Category = "ApexSim|Net")
	bool FindTrackById(const FString& TrackId, FApexTrackConfigSummary& OutTrack) const;

	UFUNCTION(BlueprintPure, Category = "ApexSim|Net")
	bool FindSessionById(const FString& SessionId, FApexSessionSummary& OutSession) const;

private:
	/** Server heartbeat timeout is 5000 ms; 2 s leaves generous margin. */
	static constexpr float HeartbeatIntervalSeconds = 2.0f;
	static constexpr float LobbyStateDebounceSeconds = 1.0f;

	bool Tick(float DeltaSeconds);
	void HandleMessage(const FApexServerMessage& Message);
	void SetConnectionState(EApexConnectionState NewState, const FString& Detail);
	void SendPayload(TArray<uint8>&& Payload);
	void TeardownConnection();

	TUniquePtr<FApexTcpConnection> Connection;
	FTSTicker::FDelegateHandle TickerHandle;

	EApexConnectionState ConnectionState = EApexConnectionState::Disconnected;

	UPROPERTY()
	FApexLobbyState CachedLobbyState;

	FString Host;
	int32 Port = 9000;
	FString PlayerName;
	FString PlayerId;
	FString CurrentSessionId;

	uint32 ClientTick = 0;
	float TimeSinceHeartbeat = 0.0f;
	float TimeSinceLobbyRequest = LobbyStateDebounceSeconds;
	bool bWarnedEmptyCatalog = false;
};
