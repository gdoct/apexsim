#include "ApexNetSubsystem.h"

#include "ApexProtocolCodec.h"
#include "ApexSimNetModule.h"
#include "ApexTcpConnection.h"

UApexNetSubsystem::UApexNetSubsystem() = default;

// FApexTcpConnection is complete here (see the include above), which is what
// lets TUniquePtr destroy it.
UApexNetSubsystem::~UApexNetSubsystem() = default;

void UApexNetSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// FTSTicker rather than FTickableGameObject: no CDO-tick guard to write, no
	// GetStatId boilerplate, and it survives PIE map transitions cleanly.
	TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateUObject(this, &UApexNetSubsystem::Tick),
		0.0f);
}

void UApexNetSubsystem::Deinitialize()
{
	if (TickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
		TickerHandle.Reset();
	}

	// The thread must be joined before Deinitialize returns, or it can outlive
	// the subsystem and dispatch into a destroyed object.
	TeardownConnection();

	Super::Deinitialize();
}

void UApexNetSubsystem::TeardownConnection()
{
	// UDP first: it is the noisier thread, and nothing it produces is useful
	// once the TCP session is going away.
	if (UdpConnection)
	{
		UdpConnection->Shutdown();
		UdpConnection.Reset();
	}
	if (Connection)
	{
		Connection->Shutdown();
		Connection.Reset();
	}
	bUdpReadyBroadcast = false;
}

void UApexNetSubsystem::StartUdp(const FApexAuthSuccess& Auth)
{
	if (Auth.UdpToken.IsEmpty() || Auth.UdpPort <= 0)
	{
		UE_LOG(LogApexSimNet, Warning,
			TEXT("AuthSuccess carried no usable UDP token/port; telemetry will not flow"));
		return;
	}

	UdpConnection = MakeUnique<FApexUdpConnection>(Host, Auth.UdpPort, Auth.UdpToken);
	if (!UdpConnection->Start())
	{
		UdpConnection.Reset();
	}
}

void UApexNetSubsystem::SetPlayerInput(const FApexPlayerInput& Input)
{
	if (UdpConnection)
	{
		UdpConnection->SetPlayerInput(Input);
	}
}

bool UApexNetSubsystem::IsUdpReady() const
{
	return UdpConnection && UdpConnection->IsHandshakeComplete();
}

int32 UApexNetSubsystem::GetLocalCarIndex() const
{
	for (const FApexRosterEntry& Entry : CachedRoster.Entries)
	{
		if (Entry.PlayerId.Equals(PlayerId, ESearchCase::IgnoreCase))
		{
			return Entry.CarIndex;
		}
	}
	return -1;
}

void UApexNetSubsystem::SetConnectionState(EApexConnectionState NewState, const FString& Detail)
{
	if (ConnectionState == NewState)
	{
		return;
	}
	ConnectionState = NewState;
	OnConnectionStateChanged.Broadcast(NewState, Detail);
}

void UApexNetSubsystem::Connect(const FString& InHost, int32 InPort, const FString& InPlayerName, const FString& InToken)
{
	TeardownConnection();

	Host = InHost;
	Port = InPort;
	PlayerName = InPlayerName;
	Token = InToken;
	PlayerId.Reset();
	CurrentSessionId.Reset();

	bReconnectEnabled = true;
	ReconnectAttempt = 0;
	TimeUntilReconnect = 0.0f;

	SetConnectionState(EApexConnectionState::Connecting,
		FString::Printf(TEXT("Connecting to %s:%d..."), *Host, Port));
	StartConnectionAttempt();
}

void UApexNetSubsystem::StartConnectionAttempt()
{
	// Everything scoped to one TCP session starts over, on a reconnect as much
	// as on the first attempt: the server issues a new player id, the lobby
	// snapshot is re-requested and the heartbeat counter restarts.
	CachedLobbyState = FApexLobbyState();
	ClientTick = 0;
	TimeSinceHeartbeat = 0.0f;
	HeartbeatSentSeconds = 0.0;
	PingMs = -1;
	bWarnedEmptyCatalog = false;

	Connection = MakeUnique<FApexTcpConnection>(Host, Port, Token, PlayerName);
	if (!Connection->Start())
	{
		Connection.Reset();
		bReconnectEnabled = false;
		SetConnectionState(EApexConnectionState::Failed, TEXT("Could not start the network thread"));
	}
}

void UApexNetSubsystem::ScheduleReconnect(const FString& Reason)
{
	// 1, 2, 4, 5, 5... seconds: a server started a moment after the client is
	// picked up almost at once, and a dead address is not hammered.
	TimeUntilReconnect = FMath::Min(
		ReconnectInitialDelaySeconds * static_cast<float>(1 << FMath::Min(ReconnectAttempt, 8)),
		ReconnectMaxDelaySeconds);
	++ReconnectAttempt;

	UE_LOG(LogApexSimNet, Log, TEXT("%s; retrying %s:%d in %.0f s (attempt %d)"),
		*Reason, *Host, Port, TimeUntilReconnect, ReconnectAttempt);
	SetConnectionState(EApexConnectionState::Reconnecting,
		FString::Printf(TEXT("%s — retrying in %.0f s"), *Reason, TimeUntilReconnect));
}

void UApexNetSubsystem::Disconnect()
{
	bReconnectEnabled = false;
	if (Connection && Connection->IsConnected())
	{
		Connection->Send(ApexProtocol::EncodeDisconnect());
	}
	TeardownConnection();

	PlayerId.Reset();
	CurrentSessionId.Reset();
	ResetDemoSession();
	SetConnectionState(EApexConnectionState::Disconnected, TEXT("Disconnected"));
}

void UApexNetSubsystem::SendPayload(TArray<uint8>&& Payload)
{
	if (!Connection || !Connection->IsConnected())
	{
		UE_LOG(LogApexSimNet, Warning, TEXT("Dropped an outbound message: not connected"));
		return;
	}
	Connection->Send(MoveTemp(Payload));
}

bool UApexNetSubsystem::RequestLobbyState()
{
	if (TimeSinceLobbyRequest < LobbyStateDebounceSeconds)
	{
		return false;
	}
	TimeSinceLobbyRequest = 0.0f;
	UE_LOG(LogApexSimNet, Verbose, TEXT("-> RequestLobbyState"));
	SendPayload(ApexProtocol::EncodeRequestLobbyState());
	return true;
}

void UApexNetSubsystem::SelectCar(const FString& CarConfigId)
{
	UE_LOG(LogApexSimNet, Verbose, TEXT("-> SelectCar %s"), *CarConfigId);
	SendPayload(ApexProtocol::EncodeSelectCar(CarConfigId));
}

void UApexNetSubsystem::CreateSession(
	const FString& TrackConfigId,
	int32 MaxPlayers,
	int32 AiCount,
	int32 LapLimit,
	EApexSessionKind SessionKind,
	const FApexAllowedAssists& AllowedAssists,
	const FApexSessionConditions& Conditions)
{
	UE_LOG(LogApexSimNet, Verbose, TEXT("-> CreateSession track=%s players=%d ai=%d laps=%d locked_assists=%d conditions=%s"),
		*TrackConfigId, MaxPlayers, AiCount, LapLimit, AllowedAssists.CountLocked(), *Conditions.Describe());
	// The server holds one session per player: the menu's demo goes first.
	LeaveDemoSession();
	bSessionRequestPending = true;
	SessionRequestSentSeconds = FPlatformTime::Seconds();
	SendPayload(ApexProtocol::EncodeCreateSession(
		TrackConfigId,
		static_cast<uint8>(FMath::Clamp(MaxPlayers, 1, 255)),
		static_cast<uint8>(FMath::Clamp(AiCount, 0, 255)),
		static_cast<uint8>(FMath::Clamp(LapLimit, 1, 255)),
		SessionKind,
		AllowedAssists,
		Conditions));
}

void UApexNetSubsystem::JoinSession(const FString& SessionId)
{
	UE_LOG(LogApexSimNet, Verbose, TEXT("-> JoinSession %s"), *SessionId);
	LeaveDemoSession();
	bSessionRequestPending = true;
	SessionRequestSentSeconds = FPlatformTime::Seconds();
	SendPayload(ApexProtocol::EncodeJoinSession(SessionId));
}

void UApexNetSubsystem::JoinAsSpectator(const FString& SessionId)
{
	UE_LOG(LogApexSimNet, Verbose, TEXT("-> JoinAsSpectator %s"), *SessionId);
	LeaveDemoSession();
	bSessionRequestPending = true;
	SessionRequestSentSeconds = FPlatformTime::Seconds();
	SendPayload(ApexProtocol::EncodeJoinAsSpectator(SessionId));
}

void UApexNetSubsystem::CreateDemoSession(const FString& TrackConfigId, int32 AiCount, int32 LapLimit)
{
	if (bInDemoSession || bDemoRequested || !CurrentSessionId.IsEmpty() || !IsAuthenticated())
	{
		return;
	}
	UE_LOG(LogApexSimNet, Log, TEXT("-> CreateSession (demo) track=%s ai=%d laps=%d"), *TrackConfigId, AiCount, LapLimit);
	bDemoRequested = true;
	DemoSessionState = EApexSessionState::Lobby;
	const uint8 Field = static_cast<uint8>(FMath::Clamp(AiCount, 1, 255));
	// Nobody drives in a demo, so the allowed set is moot; everything is
	// allowed. The sky is the default: the menu is designed over daylight.
	SendPayload(ApexProtocol::EncodeCreateSession(
		TrackConfigId, Field, Field, static_cast<uint8>(FMath::Clamp(LapLimit, 1, 255)), EApexSessionKind::Demo,
		FApexAllowedAssists(), FApexSessionConditions()));
}

void UApexNetSubsystem::LeaveDemoSession()
{
	if (!bInDemoSession && !bDemoRequested)
	{
		return;
	}
	UE_LOG(LogApexSimNet, Log, TEXT("-> LeaveSession (demo)"));
	++DemoLeavesInFlight;
	SendPayload(ApexProtocol::EncodeLeaveSession());
	// From here the demo is over as far as the client is concerned: whatever
	// the server still sends for it (its join, if the request was in flight)
	// is dropped.
	ResetDemoSession();
}

void UApexNetSubsystem::ResetDemoSession()
{
	const bool bWasDemo = bInDemoSession || bDemoRequested;
	if (bInDemoSession)
	{
		CurrentSessionId.Reset();
		CachedRoster = FApexSessionRoster();
	}
	bInDemoSession = false;
	bDemoRequested = false;
	DemoSessionState = EApexSessionState::Lobby;
	if (!Connection)
	{
		// No socket, no answers left to wait for.
		DemoLeavesInFlight = 0;
		bSessionRequestPending = false;
	}
	if (bWasDemo)
	{
		OnDemoSessionChanged.Broadcast(false);
	}
}

void UApexNetSubsystem::LeaveSession()
{
	UE_LOG(LogApexSimNet, Verbose, TEXT("-> LeaveSession"));
	SendPayload(ApexProtocol::EncodeLeaveSession());
}

void UApexNetSubsystem::StartSession()
{
	UE_LOG(LogApexSimNet, Verbose, TEXT("-> StartSession"));
	SendPayload(ApexProtocol::EncodeStartSession());
}

void UApexNetSubsystem::SetGameMode(EApexGameMode Mode)
{
	UE_LOG(LogApexSimNet, Verbose, TEXT("-> SetGameMode %d"), static_cast<int32>(Mode));
	SendPayload(ApexProtocol::EncodeSetGameMode(Mode));
}

void UApexNetSubsystem::StartCountdown(int32 Seconds, EApexGameMode NextMode)
{
	UE_LOG(LogApexSimNet, Verbose, TEXT("-> StartCountdown %d"), Seconds);
	SendPayload(ApexProtocol::EncodeStartCountdown(
		static_cast<uint16>(FMath::Clamp(Seconds, 0, 65535)), NextMode));
}

void UApexNetSubsystem::SetDriverAids(
	bool bAutoGearbox, bool bSteeringAssist, bool bAbs, EApexTractionControl TractionControl)
{
	UE_LOG(LogApexSimNet, Verbose, TEXT("-> SetDriverAids auto_gearbox=%d steering_assist=%d abs=%d traction_control=%d"),
		bAutoGearbox ? 1 : 0, bSteeringAssist ? 1 : 0, bAbs ? 1 : 0, static_cast<int32>(TractionControl));
	SendPayload(ApexProtocol::EncodeSetDriverAids(bAutoGearbox, bSteeringAssist, bAbs, TractionControl));
}

void UApexNetSubsystem::SetCarSetup(const FApexCarSetup& Setup)
{
	UE_LOG(LogApexSimNet, Verbose, TEXT("-> SetCarSetup %d knob(s) off stock"), Setup.CountChanged());
	SendPayload(ApexProtocol::EncodeSetCarSetup(Setup));
}

bool UApexNetSubsystem::FindCarById(const FString& CarId, FApexCarConfigSummary& OutCar) const
{
	for (const FApexCarConfigSummary& Car : CachedLobbyState.CarConfigs)
	{
		if (Car.Id.Equals(CarId, ESearchCase::IgnoreCase))
		{
			OutCar = Car;
			return true;
		}
	}
	return false;
}

bool UApexNetSubsystem::FindTrackById(const FString& TrackId, FApexTrackConfigSummary& OutTrack) const
{
	for (const FApexTrackConfigSummary& Track : CachedLobbyState.TrackConfigs)
	{
		if (Track.Id.Equals(TrackId, ESearchCase::IgnoreCase))
		{
			OutTrack = Track;
			return true;
		}
	}
	return false;
}

void UApexNetSubsystem::ClearRacingLine()
{
	if (CachedRacingLine.Points.Num() == 0)
	{
		return;
	}
	CachedRacingLine = FApexRacingLineData();
	OnRacingLineUpdated.Broadcast(CachedRacingLine);
}

bool UApexNetSubsystem::FindSessionById(const FString& SessionId, FApexSessionSummary& OutSession) const
{
	for (const FApexSessionSummary& Session : CachedLobbyState.AvailableSessions)
	{
		if (Session.Id.Equals(SessionId, ESearchCase::IgnoreCase))
		{
			OutSession = Session;
			return true;
		}
	}
	return false;
}

void UApexNetSubsystem::DiscardTelemetryOfPreviousSession()
{
	// Telemetry carries no session id. Every frame queued before this join was
	// sent for the session just left (the server stops broadcasting it before
	// it answers the leave over the same TCP stream), and after a hitch the
	// queue can hold seconds of them. FrameFitsRoster cannot tell them apart
	// when the old session had fewer cars than the new one: the demo's ten
	// cars fit a fourteen-car roster, and its Countdown state was applied to
	// a session still sitting in Lobby, which put the race view over the
	// lobby screen with nothing counting down.
	if (!UdpConnection)
	{
		return;
	}
	const int32 Discarded = UdpConnection->DiscardQueuedTelemetry();
	if (Discarded > 0)
	{
		UE_LOG(LogApexSimNet, Log, TEXT("Dropped %d queued telemetry frame(s) from before the join"), Discarded);
	}
}

bool UApexNetSubsystem::FrameFitsRoster(const FApexTelemetryFrame& Frame) const
{
	if (CachedRoster.Entries.IsEmpty())
	{
		// Before the roster there is nothing to place a car by; the roster
		// follows SessionJoined on the same TCP stream, so this is brief.
		return Frame.Cars.IsEmpty();
	}
	for (const FApexCarTelemetry& Car : Frame.Cars)
	{
		if (Car.CarIndex < 0 || Car.CarIndex >= CachedRoster.Entries.Num())
		{
			return false;
		}
	}
	return true;
}

bool UApexNetSubsystem::Tick(float DeltaSeconds)
{
	TimeSinceLobbyRequest += DeltaSeconds;

	if (!Connection)
	{
		// Between attempts. Only the timer runs here, so a server that is down
		// costs one socket connect every few seconds rather than one per frame.
		if (bReconnectEnabled && ConnectionState == EApexConnectionState::Reconnecting)
		{
			TimeUntilReconnect -= DeltaSeconds;
			if (TimeUntilReconnect <= 0.0f)
			{
				StartConnectionAttempt();
			}
		}
		return true;
	}

	FApexServerMessage Message;
	while (Connection->PopMessage(Message))
	{
		HandleMessage(Message);
	}

	FApexDisconnectReason Reason;
	if (Connection->PopDisconnectReason(Reason))
	{
		const bool bWasAuthenticated = ConnectionState == EApexConnectionState::Authenticated;
		// AuthFailure lands before the server closes the socket, so by now the
		// state is already Failed; the same token would only earn the same answer.
		const bool bAuthRejected = ConnectionState == EApexConnectionState::Failed;

		TeardownConnection();
		PlayerId.Reset();
		CurrentSessionId.Reset();
		ResetDemoSession();

		if (bAuthRejected)
		{
			bReconnectEnabled = false;
		}
		else if (bReconnectEnabled)
		{
			ScheduleReconnect(Reason.Text);
		}
		else if (Reason.bDuringConnect)
		{
			SetConnectionState(EApexConnectionState::Failed, Reason.Text);
		}
		else
		{
			SetConnectionState(EApexConnectionState::Disconnected, Reason.Text);
		}

		// Only a lost *session* is something the rest of the client must react
		// to (drop the race view, back to the menu). A refused connect travels
		// through the state change alone, so the connect dialog is not yanked
		// away from someone who has just typed the wrong address.
		if (bWasAuthenticated)
		{
			OnDisconnected.Broadcast(Reason.Text);
		}
		return true;
	}

	if ((ConnectionState == EApexConnectionState::Connecting || ConnectionState == EApexConnectionState::Reconnecting)
		&& Connection->IsConnected())
	{
		SetConnectionState(EApexConnectionState::Authenticating, TEXT("Authenticating..."));
	}

	if (UdpConnection)
	{
		if (!bUdpReadyBroadcast && UdpConnection->IsHandshakeComplete())
		{
			bUdpReadyBroadcast = true;
			OnUdpReady.Broadcast();
		}

		// Force feedback is not a snapshot: each message holds the peaks of its
		// own interval, so everything that arrived since the last tick is
		// merged rather than only the newest kept.
		FApexDriverFeedback Feedback;
		bool bFeedbackArrived = false;
		while (UdpConnection->PopDriverFeedback(Feedback))
		{
			if (bFeedbackArrived)
			{
				LatestDriverFeedback.Absorb(Feedback);
			}
			else
			{
				LatestDriverFeedback = MoveTemp(Feedback);
				bFeedbackArrived = true;
			}
		}
		if (bFeedbackArrived)
		{
			++DriverFeedbackSerial;
			DriverFeedbackTime = FPlatformTime::Seconds();
		}

		// Drain to the newest frame. Telemetry is a snapshot, not a stream of
		// events, so if several arrived between ticks only the last one matters
		// — but every frame is still broadcast so nothing that counts ticks
		// misses one.
		FApexTelemetryFrame Frame;
		while (UdpConnection->PopTelemetry(Frame))
		{
			LatestTelemetry = Frame;

			if (bInDemoSession)
			{
				// The demo's state is its own: the player's session is still Lobby.
				DemoSessionState = Frame.SessionState;
				OnTelemetry.Broadcast(LatestTelemetry);
				continue;
			}
			if (DemoLeavesInFlight > 0)
			{
				// The tail of a demo being left.
				continue;
			}
			if (!FrameFitsRoster(Frame))
			{
				// A frame of another session, whose car indices point past
				// this roster. Applying it would take the other session's
				// state (Racing) for this one and skip the countdown. Frames
				// queued before the join are dropped at SessionJoined; this
				// catches one that slipped in behind it.
				UE_LOG(LogApexSimNet, Verbose, TEXT("Telemetry frame with %d car(s) ignored: roster has %d"),
					Frame.Cars.Num(), CachedRoster.Entries.Num());
				continue;
			}

			// Every frame carries the authoritative state and mode. `StartSession`
			// moves the server to Countdown without any TCP notification, so this
			// is the only place a client reliably learns the session has begun.
			if (Frame.SessionState != CurrentSessionState)
			{
				CurrentSessionState = Frame.SessionState;
				UE_LOG(LogApexSimNet, Log, TEXT("Session state -> %d (from telemetry)"),
					static_cast<int32>(CurrentSessionState));
				OnSessionStateChanged.Broadcast(CurrentSessionState);
			}
			if (Frame.GameMode != CurrentGameMode)
			{
				CurrentGameMode = Frame.GameMode;
				UE_LOG(LogApexSimNet, Log, TEXT("Game mode -> %d (from telemetry)"),
					static_cast<int32>(CurrentGameMode));
				OnGameModeChanged.Broadcast(CurrentGameMode);
			}

			OnTelemetry.Broadcast(LatestTelemetry);
		}
	}

	if (ConnectionState == EApexConnectionState::Authenticated)
	{
		TimeSinceHeartbeat += DeltaSeconds;
		if (TimeSinceHeartbeat >= HeartbeatIntervalSeconds)
		{
			TimeSinceHeartbeat = 0.0f;
			HeartbeatSentSeconds = FPlatformTime::Seconds();
			Connection->Send(ApexProtocol::EncodeHeartbeat(++ClientTick));
		}
	}

	return true;
}

void UApexNetSubsystem::HandleMessage(const FApexServerMessage& Message)
{
	switch (Message.Type)
	{
	case EApexServerMessageType::AuthSuccess:
	{
		PlayerId = Message.AuthSuccess.PlayerId;
		UE_LOG(LogApexSimNet, Log, TEXT("<- AuthSuccess PlayerId=%s ServerVersion=%lld ProtocolVersion=%d UdpPort=%d"),
			*PlayerId, Message.AuthSuccess.ServerVersion, Message.AuthSuccess.ProtocolVersion, Message.AuthSuccess.UdpPort);

		ReconnectAttempt = 0;
		SetConnectionState(EApexConnectionState::Authenticated, TEXT("Connected"));
		OnAuthSucceeded.Broadcast(PlayerId, static_cast<int32>(Message.AuthSuccess.ServerVersion));

		// The UDP token is single-use and only valid for this connection, so the
		// handshake starts the moment it arrives.
		StartUdp(Message.AuthSuccess);

		// Ask once immediately so the first snapshot lands in <100 ms instead
		// of waiting up to 2 s for the periodic broadcast.
		TimeSinceLobbyRequest = LobbyStateDebounceSeconds;
		RequestLobbyState();
		break;
	}

	case EApexServerMessageType::AuthFailure:
		UE_LOG(LogApexSimNet, Warning, TEXT("<- AuthFailure: %s"), *Message.Reason);
		bReconnectEnabled = false;
		SetConnectionState(EApexConnectionState::Failed, Message.Reason);
		OnAuthFailed.Broadcast(Message.Reason);
		break;

	case EApexServerMessageType::HeartbeatAck:
		// The only round trip the protocol offers: nothing else the client sends
		// is acknowledged, so this is where latency comes from.
		if (HeartbeatSentSeconds > 0.0)
		{
			PingMs = FMath::RoundToInt((FPlatformTime::Seconds() - HeartbeatSentSeconds) * 1000.0);
			HeartbeatSentSeconds = 0.0;
		}
		UE_LOG(LogApexSimNet, VeryVerbose, TEXT("<- HeartbeatAck server_tick=%lld ping=%d ms"), Message.ServerTick, PingMs);
		break;

	case EApexServerMessageType::LobbyState:
		CachedLobbyState = Message.LobbyState;
		UE_LOG(LogApexSimNet, Verbose, TEXT("<- LobbyState players=%d sessions=%d cars=%d tracks=%d"),
			CachedLobbyState.PlayersInLobby.Num(),
			CachedLobbyState.AvailableSessions.Num(),
			CachedLobbyState.CarConfigs.Num(),
			CachedLobbyState.TrackConfigs.Num());

		// Zero cars AND zero tracks is never legitimate against this server, so
		// it almost certainly means a key-name mismatch in the decoder rather
		// than an empty catalog. Say so once, loudly.
		if (!bWarnedEmptyCatalog
			&& CachedLobbyState.CarConfigs.Num() == 0
			&& CachedLobbyState.TrackConfigs.Num() == 0)
		{
			bWarnedEmptyCatalog = true;
			UE_LOG(LogApexSimNet, Warning,
				TEXT("LobbyState decoded with 0 cars and 0 tracks. Either the server loaded no content, ")
				TEXT("or the PascalCase payload keys in ApexProtocolCodec no longer match the server."));
		}

		OnLobbyStateUpdated.Broadcast(CachedLobbyState);
		break;

	case EApexServerMessageType::SessionJoined:
		if (Message.SessionKind == EApexSessionKind::Demo)
		{
			if (!bDemoRequested)
			{
				// Withdrawn while in flight; the LeaveSession sent then takes
				// the server back out of it.
				UE_LOG(LogApexSimNet, Log, TEXT("<- SessionJoined (demo, already withdrawn) SessionId=%s"), *Message.SessionId);
				break;
			}
			bDemoRequested = false;
			bInDemoSession = true;
			DemoSessionState = EApexSessionState::Lobby;
			CurrentSessionId = Message.SessionId;
			CurrentConditions = Message.Conditions;
			CachedRoster = FApexSessionRoster();
			ClearRacingLine();
			DiscardTelemetryOfPreviousSession();
			UE_LOG(LogApexSimNet, Log, TEXT("<- SessionJoined (demo) SessionId=%s"), *CurrentSessionId);
			OnDemoSessionChanged.Broadcast(true);
			break;
		}
		bSessionRequestPending = false;
		// The new session's line follows this message; the old one is for a
		// different track or car.
		ClearRacingLine();
		CurrentSessionId = Message.SessionId;
		CurrentAllowedAssists = Message.AllowedAssists;
		CurrentConditions = Message.Conditions;
		DiscardTelemetryOfPreviousSession();
		UE_LOG(LogApexSimNet, Log, TEXT("<- SessionJoined SessionId=%s YourGridPosition=%d LockedAssists=%d Conditions=%s"),
			*CurrentSessionId, Message.GridPosition, CurrentAllowedAssists.CountLocked(), *CurrentConditions.Describe());
		OnSessionJoined.Broadcast(CurrentSessionId, Message.GridPosition);

		// The cached lobby state predates this session, so anything resolving the
		// session by id against it — the track level, the lobby screen — misses
		// until the next periodic broadcast. Refresh right away.
		TimeSinceLobbyRequest = LobbyStateDebounceSeconds;
		RequestLobbyState();
		break;

	case EApexServerMessageType::SessionLeft:
		if (DemoLeavesInFlight > 0)
		{
			// The answer to a LeaveSession sent for the demo, which was reset
			// when it went out.
			--DemoLeavesInFlight;
			UE_LOG(LogApexSimNet, Log, TEXT("<- SessionLeft (demo)"));
			break;
		}
		if (bInDemoSession)
		{
			UE_LOG(LogApexSimNet, Log, TEXT("<- SessionLeft (demo, ended by the server)"));
			ResetDemoSession();
			break;
		}
		CurrentSessionId.Reset();
		CurrentAllowedAssists = FApexAllowedAssists();
		CurrentConditions = FApexSessionConditions();
		CachedRoster = FApexSessionRoster();
		ClearRacingLine();
		LatestDriverFeedback = FApexDriverFeedback();
		DriverFeedbackTime = 0.0;
		// Telemetry stops when the session ends, so nothing would ever drive
		// this back to Lobby otherwise.
		if (CurrentSessionState != EApexSessionState::Lobby)
		{
			CurrentSessionState = EApexSessionState::Lobby;
			OnSessionStateChanged.Broadcast(CurrentSessionState);
		}
		UE_LOG(LogApexSimNet, Log, TEXT("<- SessionLeft"));
		OnSessionLeft.Broadcast();
		break;

	case EApexServerMessageType::SessionStarting:
		UE_LOG(LogApexSimNet, Log, TEXT("<- SessionStarting countdown=%d"), Message.CountdownSeconds);
		OnSessionStarting.Broadcast(Message.CountdownSeconds);
		break;

	case EApexServerMessageType::GameModeChanged:
		if (bInDemoSession || DemoLeavesInFlight > 0)
		{
			break;
		}
		UE_LOG(LogApexSimNet, Log, TEXT("<- GameModeChanged mode=%d"), static_cast<int32>(Message.GameMode));
		OnGameModeChanged.Broadcast(Message.GameMode);
		break;

	case EApexServerMessageType::CountdownUpdate:
		OnCountdownUpdate.Broadcast(Message.CountdownSeconds);
		break;

	case EApexServerMessageType::Error:
		if (bDemoRequested && !bSessionRequestPending)
		{
			// Errors carry no request id; one that lands while only a demo is
			// being asked for is the demo's, and a backdrop that could not
			// start is not news for the player.
			UE_LOG(LogApexSimNet, Warning, TEXT("<- Error %d for the demo session: %s"), Message.ErrorCode, *Message.Reason);
			ResetDemoSession();
			break;
		}
		bSessionRequestPending = false;
		UE_LOG(LogApexSimNet, Warning, TEXT("<- Error %d: %s"), Message.ErrorCode, *Message.Reason);
		OnServerError.Broadcast(Message.ErrorCode, Message.Reason);
		break;

	case EApexServerMessageType::PlayerDisconnected:
		OnPlayerDisconnected.Broadcast(Message.PlayerId);
		break;

	case EApexServerMessageType::SessionRoster:
		if (!Message.Roster.SessionId.Equals(CurrentSessionId, ESearchCase::IgnoreCase))
		{
			// For a demo being left, or a session not joined yet: telemetry
			// indices would be read against the wrong field.
			UE_LOG(LogApexSimNet, Verbose, TEXT("<- SessionRoster for session %s ignored (current: %s)"),
				*Message.Roster.SessionId, *CurrentSessionId);
			break;
		}
		CachedRoster = Message.Roster;
		UE_LOG(LogApexSimNet, Log, TEXT("<- SessionRoster %d car(s) for session %s"),
			CachedRoster.Entries.Num(), *CachedRoster.SessionId);
		OnSessionRosterUpdated.Broadcast(CachedRoster);
		break;

	case EApexServerMessageType::RacingLine:
		CachedRacingLine = Message.RacingLine;
		UE_LOG(LogApexSimNet, Log, TEXT("<- RacingLine %d point(s) every %.2f m for session %s"),
			CachedRacingLine.Points.Num(), CachedRacingLine.SpacingM, *CachedRacingLine.SessionId);
		OnRacingLineUpdated.Broadcast(CachedRacingLine);
		break;

	default:
		UE_LOG(LogApexSimNet, Verbose, TEXT("<- ignoring server message '%s'"), *Message.VariantName);
		break;
	}
}
