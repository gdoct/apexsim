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
	PlayerId.Reset();
	CurrentSessionId.Reset();
	CachedLobbyState = FApexLobbyState();
	ClientTick = 0;
	TimeSinceHeartbeat = 0.0f;
	bWarnedEmptyCatalog = false;

	SetConnectionState(EApexConnectionState::Connecting,
		FString::Printf(TEXT("Connecting to %s:%d..."), *Host, Port));

	Connection = MakeUnique<FApexTcpConnection>(Host, Port, InToken, PlayerName);
	if (!Connection->Start())
	{
		Connection.Reset();
		SetConnectionState(EApexConnectionState::Failed, TEXT("Could not start the network thread"));
	}
}

void UApexNetSubsystem::Disconnect()
{
	if (Connection && Connection->IsConnected())
	{
		Connection->Send(ApexProtocol::EncodeDisconnect());
	}
	TeardownConnection();

	PlayerId.Reset();
	CurrentSessionId.Reset();
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
	EApexSessionKind SessionKind)
{
	UE_LOG(LogApexSimNet, Verbose, TEXT("-> CreateSession track=%s players=%d ai=%d laps=%d"),
		*TrackConfigId, MaxPlayers, AiCount, LapLimit);
	SendPayload(ApexProtocol::EncodeCreateSession(
		TrackConfigId,
		static_cast<uint8>(FMath::Clamp(MaxPlayers, 1, 255)),
		static_cast<uint8>(FMath::Clamp(AiCount, 0, 255)),
		static_cast<uint8>(FMath::Clamp(LapLimit, 1, 255)),
		SessionKind));
}

void UApexNetSubsystem::JoinSession(const FString& SessionId)
{
	UE_LOG(LogApexSimNet, Verbose, TEXT("-> JoinSession %s"), *SessionId);
	SendPayload(ApexProtocol::EncodeJoinSession(SessionId));
}

void UApexNetSubsystem::JoinAsSpectator(const FString& SessionId)
{
	UE_LOG(LogApexSimNet, Verbose, TEXT("-> JoinAsSpectator %s"), *SessionId);
	SendPayload(ApexProtocol::EncodeJoinAsSpectator(SessionId));
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

bool UApexNetSubsystem::Tick(float DeltaSeconds)
{
	TimeSinceLobbyRequest += DeltaSeconds;

	if (!Connection)
	{
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

		TeardownConnection();
		PlayerId.Reset();
		CurrentSessionId.Reset();

		if (Reason.bDuringConnect)
		{
			SetConnectionState(EApexConnectionState::Failed, Reason.Text);
		}
		else
		{
			SetConnectionState(EApexConnectionState::Disconnected, Reason.Text);
		}

		if (bWasAuthenticated || Reason.bDuringConnect)
		{
			OnDisconnected.Broadcast(Reason.Text);
		}
		return true;
	}

	if (ConnectionState == EApexConnectionState::Connecting && Connection->IsConnected())
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

		// Drain to the newest frame. Telemetry is a snapshot, not a stream of
		// events, so if several arrived between ticks only the last one matters
		// — but every frame is still broadcast so nothing that counts ticks
		// misses one.
		FApexTelemetryFrame Frame;
		while (UdpConnection->PopTelemetry(Frame))
		{
			LatestTelemetry = Frame;

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
		SetConnectionState(EApexConnectionState::Failed, Message.Reason);
		OnAuthFailed.Broadcast(Message.Reason);
		break;

	case EApexServerMessageType::HeartbeatAck:
		UE_LOG(LogApexSimNet, VeryVerbose, TEXT("<- HeartbeatAck server_tick=%lld"), Message.ServerTick);
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
		CurrentSessionId = Message.SessionId;
		UE_LOG(LogApexSimNet, Log, TEXT("<- SessionJoined SessionId=%s YourGridPosition=%d"),
			*CurrentSessionId, Message.GridPosition);
		OnSessionJoined.Broadcast(CurrentSessionId, Message.GridPosition);
		break;

	case EApexServerMessageType::SessionLeft:
		CurrentSessionId.Reset();
		CachedRoster = FApexSessionRoster();
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
		UE_LOG(LogApexSimNet, Log, TEXT("<- GameModeChanged mode=%d"), static_cast<int32>(Message.GameMode));
		OnGameModeChanged.Broadcast(Message.GameMode);
		break;

	case EApexServerMessageType::CountdownUpdate:
		OnCountdownUpdate.Broadcast(Message.CountdownSeconds);
		break;

	case EApexServerMessageType::Error:
		UE_LOG(LogApexSimNet, Warning, TEXT("<- Error %d: %s"), Message.ErrorCode, *Message.Reason);
		OnServerError.Broadcast(Message.ErrorCode, Message.Reason);
		break;

	case EApexServerMessageType::PlayerDisconnected:
		OnPlayerDisconnected.Broadcast(Message.PlayerId);
		break;

	case EApexServerMessageType::SessionRoster:
		CachedRoster = Message.Roster;
		UE_LOG(LogApexSimNet, Log, TEXT("<- SessionRoster %d car(s) for session %s"),
			CachedRoster.Entries.Num(), *CachedRoster.SessionId);
		OnSessionRosterUpdated.Broadcast(CachedRoster);
		break;

	default:
		UE_LOG(LogApexSimNet, Verbose, TEXT("<- ignoring server message '%s'"), *Message.VariantName);
		break;
	}
}
