#include "ApexNetSubsystem.h"

#include "ApexDriverKey.h"

#include "ApexProtocolCodec.h"
#include "ApexSimNetModule.h"
#include "ApexTcpConnection.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Misc/CommandLine.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Parse.h"

UApexNetSubsystem::UApexNetSubsystem() = default;

namespace
{
	/** GameUserSettings section holding one resume token per server. */
	const TCHAR* ResumeSection = TEXT("ApexSim.Resume");

	/** A resume token is one driver's on one server: each profile keeps its own seat. */
	FString ResumeKey(const FString& Host, int32 Port, const FString& DriverId)
	{
		return DriverId.IsEmpty()
			? FString::Printf(TEXT("%s_%d"), *Host, Port)
			: FString::Printf(TEXT("%s_%d_%s"), *Host, Port, *DriverId);
	}
}

namespace
{
	FAutoConsoleCommandWithWorldAndArgs ChatCommand(
		TEXT("apexsim.net.Chat"),
		TEXT("Say a chat line, as the lobby's box or the race's chat line would: apexsim.net.Chat <text>"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
			if (UApexNetSubsystem* Net = GameInstance ? GameInstance->GetSubsystem<UApexNetSubsystem>() : nullptr)
			{
				Net->SendChat(FString::Join(Args, TEXT(" ")));
			}
		}));

	FAutoConsoleCommandWithWorld DropCommand(
		TEXT("apexsim.net.Drop"),
		TEXT("Close the server connection as a lost network would (no Disconnect sent): the client reconnects and, ")
		TEXT("in a session it was driving in, rejoins by itself"),
		FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* World)
		{
			UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
			if (UApexNetSubsystem* Net = GameInstance ? GameInstance->GetSubsystem<UApexNetSubsystem>() : nullptr)
			{
				Net->DropConnection();
			}
		}));
}

// FApexTcpConnection is complete here (see the include above), which is what
// lets TUniquePtr destroy it.
UApexNetSubsystem::~UApexNetSubsystem() = default;

void UApexNetSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// The fake latency's command-line default (an unattended run at a ping).
	int32 FakeLatencyMs = 0;
	if (FParse::Value(FCommandLine::Get(), TEXT("-ApexFakeLatencyMs="), FakeLatencyMs))
	{
		if (IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(TEXT("apexsim.net.FakeLatencyMs")))
		{
			Var->Set(FakeLatencyMs, ECVF_SetByCommandline);
		}
		UE_LOG(LogApexSimNet, Log, TEXT("Fake UDP latency: %d ms round trip (-ApexFakeLatencyMs)"), FakeLatencyMs);
	}

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
	if (Auth.UdpKey.IsEmpty())
	{
		// A server from before sealed UDP: it would drop our sealed
		// datagrams, so stay on TCP telemetry rather than handshake forever.
		UE_LOG(LogApexSimNet, Warning,
			TEXT("AuthSuccess carried no UDP key (older server); telemetry stays on TCP and there is no force feedback"));
		return;
	}

	UdpConnection = MakeUnique<FApexUdpConnection>(Host, Auth.UdpPort, Auth.UdpToken, Auth.UdpKey);
	if (!UdpConnection->Start())
	{
		UdpConnection.Reset();
	}
}

uint32 UApexNetSubsystem::GetLastInputSeq() const
{
	return UdpConnection.IsValid() ? UdpConnection->GetLastInputSeq() : 0;
}

void UApexNetSubsystem::CopySentInputs(uint32 AfterSeq, TArray<FApexSentInput>& Out) const
{
	if (UdpConnection.IsValid())
	{
		UdpConnection->CopySentInputs(AfterSeq, Out);
	}
	else
	{
		Out.Reset();
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

int32 UApexNetSubsystem::GetQualifyingLapsLeft() const
{
	if (CurrentGameMode != EApexGameMode::Qualification || CurrentShootoutState.bValid)
	{
		return -1;
	}
	return ApexQualifyingLapAllowance::LapsLeft(CurrentQualifyingLapAllowance, GetQualifyingTimedLaps());
}

int32 UApexNetSubsystem::GetQualifyingTimedLaps() const
{
	// The server's count, carried in the roster: it counts every timed lap
	// against the allowance, from the session's start whenever we joined.
	for (const FApexRosterEntry& Entry : CachedRoster.Entries)
	{
		if (Entry.PlayerId.Equals(PlayerId, ESearchCase::IgnoreCase))
		{
			return Entry.QualifyingLaps;
		}
	}
	return 0;
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
	ResumeToken.Reset();
	if (GConfig)
	{
		GConfig->GetString(ResumeSection, *ResumeKey(Host, Port, DriverId), ResumeToken, GGameUserSettingsIni);
	}
	ClearRejoinOffer();
	AutoRejoinSessionId.Reset();
	PlayerId.Reset();
	CurrentSessionId.Reset();
	bSessionSpectator = false;

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

	Connection = MakeUnique<FApexTcpConnection>(Host, Port, Token, PlayerName, TlsOptions, ResumeToken, DriverId,
		DriverId.IsEmpty() ? FString() : ApexDriverKey::PublicKeyHex(DriverKeySeed));
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
	bSessionSpectator = false;
	ResetSpectate();
	SetConnectionState(EApexConnectionState::Disconnected, TEXT("Disconnected"));
}

void UApexNetSubsystem::DropConnection()
{
	if (!Connection)
	{
		return;
	}
	UE_LOG(LogApexSimNet, Log, TEXT("Dropping the connection (apexsim.net.Drop): no Disconnect sent"));
	// Stop() closes the socket under the worker, which then reports the
	// connection gone; Tick takes that as any lost connection.
	Connection->Stop();
}

void UApexNetSubsystem::SendChat(const FString& Text)
{
	const FString Line = Text.TrimStartAndEnd();
	if (Line.IsEmpty() || !IsAuthenticated())
	{
		return;
	}
	UE_LOG(LogApexSimNet, Log, TEXT("-> Chat: %s"), *Line);
	SendPayload(ApexProtocol::EncodeChat(Line.Left(200)));
}

void UApexNetSubsystem::ListShowcases()
{
	UE_LOG(LogApexSimNet, Verbose, TEXT("-> ListShowcases"));
	SendPayload(ApexProtocol::EncodeListShowcases());
}

void UApexNetSubsystem::SpectateShowcase(const FString& Id)
{
	if (!IsAuthenticated() || !CurrentSessionId.IsEmpty())
	{
		return;
	}
	UE_LOG(LogApexSimNet, Log, TEXT("-> SpectateShowcase %s"), Id.IsEmpty() ? TEXT("(first)") : *Id);
	bSpectateRequested = true;
	bSpectating = false;
	SendPayload(ApexProtocol::EncodeSpectateShowcase(Id));
}

void UApexNetSubsystem::SpectateSession(const FString& SessionId)
{
	if (!IsAuthenticated() || !CurrentSessionId.IsEmpty())
	{
		return;
	}
	UE_LOG(LogApexSimNet, Log, TEXT("-> SpectateSession %s"), *SessionId);
	bSpectateRequested = true;
	bSpectating = false;
	SendPayload(ApexProtocol::EncodeSpectateSession(SessionId));
}

void UApexNetSubsystem::LeaveSpectate()
{
	if (!bSpectating && !bSpectateRequested)
	{
		return;
	}
	UE_LOG(LogApexSimNet, Log, TEXT("-> LeaveSpectate"));
	if (Connection && Connection->IsConnected())
	{
		SendPayload(ApexProtocol::EncodeLeaveSpectate());
	}
	ResetSpectate();
}

void UApexNetSubsystem::ResetSpectate()
{
	bSpectating = false;
	bSpectateRequested = false;
	if (UdpConnection)
	{
		UdpConnection->DiscardQueuedSpectatorRecords();
	}
}

void UApexNetSubsystem::BeginBackdropFeed(const FApexSessionConditions& Conditions)
{
	if (bBackdropFeed)
	{
		EndBackdropFeed();
	}
	bBackdropFeed = true;
	DemoSessionState = EApexSessionState::Lobby;
	CurrentConditions = Conditions;
	CachedRoster = FApexSessionRoster();
	PitServices.Reset();
	ClearLapTiming();
	UE_LOG(LogApexSimNet, Log, TEXT("Backdrop feed begins (%s)"), *Conditions.Describe());
	OnDemoSessionChanged.Broadcast(true);
}

void UApexNetSubsystem::FeedBackdropRoster(const FApexSessionRoster& Roster)
{
	if (!bBackdropFeed)
	{
		return;
	}
	CachedRoster = Roster;
	OnSessionRosterUpdated.Broadcast(CachedRoster);
}

void UApexNetSubsystem::FeedBackdropTelemetry(const FApexTelemetryFrame& Frame)
{
	if (!bBackdropFeed)
	{
		return;
	}
	LatestTelemetry = Frame;
	DemoSessionState = Frame.SessionState;
	OnTelemetry.Broadcast(LatestTelemetry);
}

void UApexNetSubsystem::FeedBackdropRoad(const FApexRoadState& Road)
{
	if (!bBackdropFeed || !Road.IsValid())
	{
		return;
	}
	OnRoadState.Broadcast(Road);
}

void UApexNetSubsystem::FeedBackdropGhost(const FApexGhostLap& Lap)
{
	if (!bBackdropFeed || !Lap.IsValid())
	{
		return;
	}
	CachedGhostLap = Lap;
	OnGhostLap.Broadcast(CachedGhostLap);
}

void UApexNetSubsystem::FeedBackdropPitService(const FApexPitService& Stop)
{
	if (!bBackdropFeed)
	{
		return;
	}
	PitServices.Add(Stop.CarIndex, Stop);
}

void UApexNetSubsystem::FeedBackdropSectors(const FApexTrackSectors& Sectors)
{
	if (!bBackdropFeed)
	{
		return;
	}
	CachedSectors = Sectors;
	TimingBoard.Reset(CachedSectors.SectorCount());
}

void UApexNetSubsystem::FeedBackdropLapTiming(const FApexLapTiming& Timing)
{
	if (!bBackdropFeed)
	{
		return;
	}
	TimingBoard.Apply(Timing);
	OnLapTiming.Broadcast(Timing);
}

void UApexNetSubsystem::EndBackdropFeed()
{
	if (!bBackdropFeed)
	{
		return;
	}
	bBackdropFeed = false;
	DemoSessionState = EApexSessionState::Lobby;
	CurrentConditions = FApexSessionConditions();
	CachedRoster = FApexSessionRoster();
	ClearLapTiming();
	UE_LOG(LogApexSimNet, Log, TEXT("Backdrop feed ends"));
	OnDemoSessionChanged.Broadcast(false);
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
	UE_LOG(LogApexSimNet, Verbose, TEXT("-> SelectCar %s livery %d"), *CarConfigId, PendingLivery);
	SendPayload(ApexProtocol::EncodeSelectCar(CarConfigId, PendingLivery));
}

void UApexNetSubsystem::CreateSession(
	const FString& TrackConfigId,
	int32 MaxPlayers,
	int32 AiCount,
	int32 LapLimit,
	EApexSessionKind SessionKind,
	const FApexAllowedAssists& AllowedAssists,
	const FApexSessionConditions& Conditions,
	EApexDamageLevel Damage,
	int32 AiSkill,
	int32 RaceSeconds)
{
	CreateSessionWithOrder(TrackConfigId, MaxPlayers, AiCount, LapLimit, SessionKind, AllowedAssists, Conditions,
		Damage, AiSkill, RaceSeconds, TArray<FString>());
}

void UApexNetSubsystem::CreateSessionWithOrder(
	const FString& TrackConfigId,
	int32 MaxPlayers,
	int32 AiCount,
	int32 LapLimit,
	EApexSessionKind SessionKind,
	const FApexAllowedAssists& AllowedAssists,
	const FApexSessionConditions& Conditions,
	EApexDamageLevel Damage,
	int32 AiSkill,
	int32 RaceSeconds,
	const TArray<FString>& GridOrder,
	int32 QualifyingSeconds,
	const FApexWeekendFormat& Weekend,
	int32 QualifyingCutoffPct,
	int32 MandatoryStops,
	bool bCompoundChange,
	EApexQualifyingFormat QualifyingFormat,
	int32 QualifyingLapAllowance,
	const FApexRaceRules& RaceRules)
{
	UE_LOG(LogApexSimNet, Verbose, TEXT("-> CreateSession track=%s players=%d ai=%d laps=%d race_seconds=%d qualifying_seconds=%d locked_assists=%d conditions=%s damage=%d ai_skill=%d"),
		*TrackConfigId, MaxPlayers, AiCount, LapLimit, RaceSeconds, QualifyingSeconds, AllowedAssists.CountLocked(), *Conditions.Describe(),
		static_cast<int32>(Damage), AiSkill);
	// The server takes a showcase viewer off their channel by itself.
	ResetSpectate();
	bSessionRequestPending = true;
	bSpectatorJoinRequested = false;
	SessionRequestSentSeconds = FPlatformTime::Seconds();
	SendPayload(ApexProtocol::EncodeCreateSession(
		TrackConfigId,
		static_cast<uint8>(FMath::Clamp(MaxPlayers, 1, 255)),
		static_cast<uint8>(FMath::Clamp(AiCount, 0, 255)),
		static_cast<uint8>(FMath::Clamp(LapLimit, 1, 255)),
		SessionKind,
		AllowedAssists,
		Conditions,
		Damage,
		AiSkill,
		RaceSeconds,
		GridOrder,
		QualifyingSeconds,
		Weekend,
		nullptr,
		QualifyingCutoffPct,
		MandatoryStops,
		bCompoundChange,
		QualifyingFormat,
		QualifyingLapAllowance,
		RaceRules));
}

void UApexNetSubsystem::JoinSession(const FString& SessionId)
{
	UE_LOG(LogApexSimNet, Verbose, TEXT("-> JoinSession %s"), *SessionId);
	ResetSpectate();
	bSessionRequestPending = true;
	bSpectatorJoinRequested = false;
	SessionRequestSentSeconds = FPlatformTime::Seconds();
	SendPayload(ApexProtocol::EncodeJoinSession(SessionId));
}

void UApexNetSubsystem::RejoinSession()
{
	if (!HasRejoinOffer())
	{
		return;
	}
	UE_LOG(LogApexSimNet, Log, TEXT("Rejoining session %s"), *RejoinSessionId);
	JoinSession(RejoinSessionId);
}

void UApexNetSubsystem::GiveUpSeat()
{
	if (!HasRejoinOffer())
	{
		return;
	}
	UE_LOG(LogApexSimNet, Log, TEXT("-> GiveUpSeat %s"), *RejoinSessionId);
	SendPayload(ApexProtocol::EncodeGiveUpSeat());
	ClearRejoinOffer();
}

void UApexNetSubsystem::SetRejoinOffer(const FString& SessionId, const FString& TrackId, EApexSessionKind Kind)
{
	RejoinSessionId = SessionId;
	RejoinTrackId = TrackId;
	RejoinSessionKind = Kind;
	OnRejoinOfferChanged.Broadcast();
}

void UApexNetSubsystem::ClearRejoinOffer()
{
	if (RejoinSessionId.IsEmpty())
	{
		return;
	}
	RejoinSessionId.Reset();
	RejoinTrackId.Reset();
	OnRejoinOfferChanged.Broadcast();
}

void UApexNetSubsystem::JoinAsSpectator(const FString& SessionId)
{
	UE_LOG(LogApexSimNet, Verbose, TEXT("-> JoinAsSpectator %s"), *SessionId);
	ResetSpectate();
	bSessionRequestPending = true;
	bSpectatorJoinRequested = true;
	SessionRequestSentSeconds = FPlatformTime::Seconds();
	SendPayload(ApexProtocol::EncodeJoinAsSpectator(SessionId));
}

namespace
{
	FApexSessionSummary HotlapSummary(const FString& TrackConfigId, const FString& TrackName, const FString& TrackStem,
		const FApexSessionConditions& Conditions)
	{
		FApexSessionSummary Summary;
		Summary.TrackId = TrackConfigId;
		Summary.TrackName = TrackName;
		Summary.TrackFile = FString::Printf(TEXT("tracks/default/%s/%s.yaml"), *TrackStem, *TrackStem);
		Summary.SessionKind = EApexSessionKind::HotlapWatch;
		Summary.State = EApexSessionState::Racing;
		Summary.Conditions = Conditions;
		Summary.MaxPlayers = 1;
		return Summary;
	}
}

void UApexNetSubsystem::BeginLocalHotlapWatch(const FString& TrackConfigId, const FString& TrackName,
	const FString& TrackStem, const FApexSessionConditions& Conditions, TArrayView<const uint8> Corners)
{
	// A server's showcase gives way, as for any other backdrop.
	if (bSpectating || bSpectateRequested)
	{
		LeaveSpectate();
	}
	bHotlapWatchLocal = true;
	HotlapWatchSummary = HotlapSummary(TrackConfigId, TrackName, TrackStem, Conditions);
	CachedCorners = FApexTrackCorners();
	if (Corners.Num() > 0)
	{
		FApexServerMessage Message;
		FString Error;
		if (ApexProtocol::DecodeServerMessage(Corners, Message, Error) && Message.Type == EApexServerMessageType::TrackCorners)
		{
			CachedCorners = Message.TrackCorners;
		}
		else
		{
			UE_LOG(LogApexSimNet, Warning, TEXT("The local watch's corners did not decode: %s"), *Error);
		}
	}
	UE_LOG(LogApexSimNet, Log, TEXT("Hotlap watch simulated here: track=%s conditions=%s, %d corner(s)"), *TrackConfigId,
		*Conditions.Describe(), CachedCorners.Corners.Num());
}

void UApexNetSubsystem::SetLocalHotlapWatchId(const FString& StreamId)
{
	if (bHotlapWatchLocal)
	{
		HotlapWatchSummary.Id = StreamId;
		CachedCorners.SessionId = StreamId;
	}
}

void UApexNetSubsystem::EndLocalHotlapWatch()
{
	if (!bHotlapWatchLocal)
	{
		return;
	}
	bHotlapWatchLocal = false;
	CachedCorners = FApexTrackCorners();
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
	// A hotlap has no grid to count down from: everyone starts in the garage
	// and goes out when they are ready, so the mode is set outright.
	if (NextMode == EApexGameMode::Hotlap)
	{
		SetGameMode(NextMode);
		return;
	}
	UE_LOG(LogApexSimNet, Verbose, TEXT("-> StartCountdown %d"), Seconds);
	SendPayload(ApexProtocol::EncodeStartCountdown(
		static_cast<uint16>(FMath::Clamp(Seconds, 0, 65535)), NextMode));
}

void UApexNetSubsystem::OpenGrid(int32 Seconds)
{
	UE_LOG(LogApexSimNet, Log, TEXT("-> OpenGrid %d"), Seconds);
	SendPayload(ApexProtocol::EncodeOpenGrid(static_cast<uint16>(FMath::Clamp(Seconds, 0, 65535))));
}

void UApexNetSubsystem::BeginSession(int32 Seconds, EApexGameMode NextMode)
{
	if (NextMode == EApexGameMode::Race)
	{
		OpenGrid(Seconds);
		return;
	}
	StartCountdown(Seconds, NextMode);
}

void UApexNetSubsystem::SetReady(bool bReady)
{
	UE_LOG(LogApexSimNet, Log, TEXT("-> SetReady %d"), bReady ? 1 : 0);
	SendPayload(ApexProtocol::EncodeSetReady(bReady));
}

void UApexNetSubsystem::SetLivery(int32 Livery)
{
	SetPendingLivery(Livery);
	UE_LOG(LogApexSimNet, Log, TEXT("-> SetLivery %d"), PendingLivery);
	SendPayload(ApexProtocol::EncodeSetLivery(static_cast<uint8>(PendingLivery)));
}

void UApexNetSubsystem::AdvanceWeekend()
{
	UE_LOG(LogApexSimNet, Log, TEXT("-> AdvanceWeekend"));
	SendPayload(ApexProtocol::EncodeAdvanceWeekend());
}

void UApexNetSubsystem::SetDriverAids(
	bool bAutoGearbox, bool bSteeringAssist, bool bAbs, EApexTractionControl TractionControl, bool bPitAutopilot)
{
	UE_LOG(LogApexSimNet, Verbose, TEXT("-> SetDriverAids auto_gearbox=%d steering_assist=%d abs=%d traction_control=%d pit_autopilot=%d"),
		bAutoGearbox ? 1 : 0, bSteeringAssist ? 1 : 0, bAbs ? 1 : 0, static_cast<int32>(TractionControl),
		bPitAutopilot ? 1 : 0);
	SendPayload(ApexProtocol::EncodeSetDriverAids(bAutoGearbox, bSteeringAssist, bAbs, TractionControl, bPitAutopilot));
}

void UApexNetSubsystem::SetPitPlan(const FApexPitPlan& Plan)
{
	UE_LOG(LogApexSimNet, Log, TEXT("-> SetPitPlan keep_tyres=%d compound=%d fuel_l=%.1f no_repair=%d pads=%d pad_set=%d box=%d"),
		Plan.bKeepTyres ? 1 : 0, Plan.Compound, Plan.FuelL, Plan.bNoRepair ? 1 : 0, Plan.Pads,
		Plan.bChoosePadSet ? Plan.PadSet : 99, Plan.bBoxThisLap ? 1 : 0);
	SendPayload(ApexProtocol::EncodeSetPitPlan(Plan));
}

void UApexNetSubsystem::RecoverCar(EApexRecoverDestination Destination)
{
	UE_LOG(LogApexSimNet, Log, TEXT("-> RecoverCar %s"),
		Destination == EApexRecoverDestination::Pits ? TEXT("pits") : TEXT("track"));
	SendPayload(ApexProtocol::EncodeRecoverCar(Destination));
}

void UApexNetSubsystem::HotlapRelocate(EApexHotlapDestination Destination, bool bColdTyres)
{
	UE_LOG(LogApexSimNet, Log, TEXT("-> HotlapRelocate %s%s"),
		Destination == EApexHotlapDestination::Garage ? TEXT("garage") : TEXT("track"),
		bColdTyres ? TEXT(" on cold tyres") : TEXT(""));
	SendPayload(ApexProtocol::EncodeHotlapRelocate(Destination, bColdTyres));
}

void UApexNetSubsystem::RequestLeaderboard(const FString& TrackConfigId, const FString& CarConfigId)
{
	UE_LOG(LogApexSimNet, Log, TEXT("-> RequestLeaderboard %s %s"), *TrackConfigId, CarConfigId.IsEmpty() ? TEXT("(every car)") : *CarConfigId);
	SendPayload(ApexProtocol::EncodeRequestLeaderboard(TrackConfigId, CarConfigId));
}

const TArray<FApexLeaderboardEntry>& UApexNetSubsystem::GetLeaderboard(const FString& TrackConfigId, const FString& CarConfigId) const
{
	static const TArray<FApexLeaderboardEntry> None;
	const TArray<FApexLeaderboardEntry>* Found = LeaderboardByKey.Find((TrackConfigId + TEXT("|") + CarConfigId).ToLower());
	return Found ? *Found : None;
}

bool UApexNetSubsystem::HasLeaderboard(const FString& TrackConfigId, const FString& CarConfigId) const
{
	return LeaderboardByKey.Contains((TrackConfigId + TEXT("|") + CarConfigId).ToLower());
}

void UApexNetSubsystem::RequestQualifyingResults(const FString& TrackConfigId)
{
	UE_LOG(LogApexSimNet, Log, TEXT("-> RequestQualifyingResults %s"), *TrackConfigId);
	SendPayload(ApexProtocol::EncodeRequestQualifyingResults(TrackConfigId));
}

const TArray<FApexQualifyingResult>& UApexNetSubsystem::GetQualifyingResults(const FString& TrackConfigId) const
{
	static const TArray<FApexQualifyingResult> None;
	const TArray<FApexQualifyingResult>* Found = QualifyingByTrack.Find(TrackConfigId.ToLower());
	return Found ? *Found : None;
}

void UApexNetSubsystem::RequestGhost()
{
	UE_LOG(LogApexSimNet, Log, TEXT("-> RequestGhost"));
	SendPayload(ApexProtocol::EncodeRequestGhost());
}

void UApexNetSubsystem::RequestTrackBundleChunk(uint32 Crc, int32 Chunk)
{
	UE_LOG(LogApexSimNet, Verbose, TEXT("-> RequestTrackBundle %08X chunk %d"), Crc, Chunk);
	SendPayload(ApexProtocol::EncodeRequestTrackBundle(Crc, Chunk));
}

void UApexNetSubsystem::SetCarSetup(const FApexCarSetup& Setup)
{
	UE_LOG(LogApexSimNet, Verbose, TEXT("-> SetCarSetup %d knob(s) off stock"), Setup.CountChanged());
	LastSentSetup = Setup;
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

void UApexNetSubsystem::ClearLapTiming()
{
	CachedSectors = FApexTrackSectors();
	CachedCorners = FApexTrackCorners();
	CachedLapRecord = FApexLapRecord();
	CachedGhostLap = FApexGhostLap();
	CachedSetupSheet = FApexCarSetupSheet();
	PitServices.Reset();
	OwnPitBox = FApexPitBox();
	TimingBoard.Reset();
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
	// A watched hotlap is unlisted: the summary made when it was asked for.
	if (IsHotlapWatch() && !SessionId.IsEmpty() && HotlapWatchSummary.Id.Equals(SessionId, ESearchCase::IgnoreCase))
	{
		OutSession = HotlapWatchSummary;
		return true;
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
		// Driving when it dropped: a quick way back rejoins by itself.
		AutoRejoinSessionId = IsInSession() && !bSessionSpectator ? CurrentSessionId : FString();
		AutoRejoinUntilSeconds = FPlatformTime::Seconds() + AutoRejoinWindowSeconds;
		PlayerId.Reset();
		CurrentSessionId.Reset();
		bSessionSpectator = false;
		ResetSpectate();
		// The server offers a held seat again once we are back.
		ClearRejoinOffer();
		// Telemetry has stopped, so nothing else takes the state back to Lobby:
		// the race view closes, and a rejoin's first frame opens it again.
		if (CurrentSessionState != EApexSessionState::Lobby)
		{
			CurrentSessionState = EApexSessionState::Lobby;
			OnSessionStateChanged.Broadcast(CurrentSessionState);
		}
		CurrentGameMode = EApexGameMode::Lobby;

		if (bAuthRejected)
		{
			bReconnectEnabled = false;
		}
		else if (Reason.bPermanent)
		{
			// The server's certificate failed the check settings.yml asks for:
			// the same certificate would fail again, so say why and stop.
			bReconnectEnabled = false;
			SetConnectionState(EApexConnectionState::Failed, Reason.Text);
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
			// The own car's snapshot and state slices are taken off each
			// message before it is folded in: a slice is only whole with its
			// neighbours.
			if (Feedback.OwnCar.IsSet())
			{
				LatestOwnCar = Feedback.OwnCar.GetValue();
				LatestOwnCarSeconds = Feedback.ReceivedSeconds;
				++OwnCarSerial;
			}
			if (Feedback.OwnState.IsSet() && OwnCarState.Add(Feedback.OwnState.GetValue()))
			{
				++OwnCarStateSerial;
			}
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

		// Spectator stream frames, in arrival order: the player (not this
		// subsystem) decides which epoch they belong to.
		TArray<uint8> Records;
		while (UdpConnection->PopSpectatorRecord(Records))
		{
			if (bSpectating)
			{
				OnSpectatorRecords.Broadcast(Records);
			}
		}

		// Drain to the newest frame. Telemetry is a snapshot, not a stream of
		// events, so if several arrived between ticks only the last one matters
		// — but every frame is still broadcast so nothing that counts ticks
		// misses one.
		FApexTelemetryFrame Frame;
		while (UdpConnection->PopTelemetry(Frame))
		{
			LatestTelemetry = Frame;

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
			Connection->Send(ApexProtocol::EncodeHeartbeat(++ClientTick, PingMs));
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
		// The proof goes before anything else this connection sends, so a
		// join after it is seated under the profile's id.
		if (!Message.AuthSuccess.KeyChallenge.IsEmpty() && !DriverId.IsEmpty())
		{
			const FString Signature = ApexDriverKey::SignHex(DriverKeySeed, ApexDriverKey::ProofMessage(Message.AuthSuccess.KeyChallenge, DriverId));
			if (!Signature.IsEmpty())
			{
				UE_LOG(LogApexSimNet, Log, TEXT("-> ProveDriverKey"));
				SendPayload(ApexProtocol::EncodeProveDriverKey(Signature));
			}
		}
		if (!Message.AuthSuccess.ResumeToken.IsEmpty() && Message.AuthSuccess.ResumeToken != ResumeToken)
		{
			ResumeToken = Message.AuthSuccess.ResumeToken;
			if (GConfig)
			{
				GConfig->SetString(ResumeSection, *ResumeKey(Host, Port, DriverId), *ResumeToken, GGameUserSettingsIni);
				GConfig->Flush(false, GGameUserSettingsIni);
			}
		}
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

		// The offered session has ended (it is no longer listed).
		if (HasRejoinOffer() && !CachedLobbyState.AvailableSessions.ContainsByPredicate(
				[this](const FApexSessionSummary& Session) { return Session.Id == RejoinSessionId; }))
		{
			ClearRejoinOffer();
		}

		OnLobbyStateUpdated.Broadcast(CachedLobbyState);
		break;

	case EApexServerMessageType::RejoinAvailable:
		UE_LOG(LogApexSimNet, Log, TEXT("<- RejoinAvailable SessionId=%s TrackId=%s Kind=%d"),
			*Message.SessionId, *Message.TrackId, static_cast<int32>(Message.SessionKind));
		SetRejoinOffer(Message.SessionId, Message.TrackId, Message.SessionKind);
		if (Message.SessionId == AutoRejoinSessionId && FPlatformTime::Seconds() <= AutoRejoinUntilSeconds)
		{
			UE_LOG(LogApexSimNet, Log, TEXT("Back within %.0f s of the drop: rejoining by itself"), AutoRejoinWindowSeconds);
			RejoinSession();
		}
		AutoRejoinSessionId.Reset();
		break;

	case EApexServerMessageType::SessionJoined:
		{
			// Moved by the server (a dashboard restart seats everyone in the new
			// session): a join for another session than ours that nobody here
			// asked for. A spectator is seated at grid position 0.
			const bool bServerMove = !bSessionRequestPending && !bSpectatorJoinRequested
				&& !CurrentSessionId.IsEmpty() && CurrentSessionId != Message.SessionId;
			bSessionRequestPending = false;
			// A menu backdrop still playing ends here, before this session's
			// state goes in: the demo subsystem stops it only once it hears of
			// the join, after the roster and sectors that follow this message
			// have arrived, and ending the feed then wiped them (and the sky),
			// leaving the player in a race view with no cars.
			EndBackdropFeed();
			// Seated as a spectator: no car of our own, every car is someone else's.
			bSessionSpectator = bServerMove ? Message.GridPosition == 0 : bSpectatorJoinRequested;
			bSpectatorJoinRequested = false;
			CachedCorners = FApexTrackCorners();
			if (bServerMove)
			{
				// A hotlap being watched is replaced without a SessionLeft in
				// between (another car, another sky), and so is a session the
				// server restarted, so nothing has put the old one's state back:
				// the new session starts from the lobby, and its first frame is
				// what opens the race view again.
				if (CurrentSessionState != EApexSessionState::Lobby)
				{
					CurrentSessionState = EApexSessionState::Lobby;
					OnSessionStateChanged.Broadcast(CurrentSessionState);
				}
				if (CurrentGameMode != EApexGameMode::Lobby)
				{
					CurrentGameMode = EApexGameMode::Lobby;
					OnGameModeChanged.Broadcast(CurrentGameMode);
				}
			}

			// different track or car.
			ClearRacingLine();
			ClearLapTiming();
			CurrentSessionId = Message.SessionId;
			CurrentAllowedAssists = Message.AllowedAssists;
			CurrentConditions = Message.Conditions;
			CurrentDamage = Message.Damage;
			CurrentAiSkill = Message.AiSkill;
			CurrentRaceSeconds = Message.RaceSeconds;
			CurrentQualifyingSeconds = Message.QualifyingSeconds;
			CurrentQualifyingCutoffPct = Message.QualifyingCutoffPct;
			CurrentMandatoryStops = Message.MandatoryStops;
			bCurrentCompoundChange = Message.bCompoundChange;
			CurrentQualifyingFormat = Message.QualifyingFormat;
			CurrentQualifyingLapAllowance = Message.QualifyingLapAllowance;
			CurrentRaceRules = Message.RaceRules;
			CurrentWeekend = Message.Weekend;
			CurrentPhysics = Message.Physics;
			CurrentConditionsBytes = Message.ConditionsBytes;
			CurrentGridPosition = Message.GridPosition;
			// The own car of the session left is not this one's.
			LatestOwnCar = FApexOwnCarSnapshot();
			OwnCarState.Reset();
			// The session's WeekendState (and a running shoot-out's state)
			// follows its SessionJoined.
			CurrentWeekendState = FApexWeekendState();
			CurrentShootoutState = FApexShootoutState();
			DiscardTelemetryOfPreviousSession();
			// In a session now: the seat held for us is this one or given up.
			ClearRejoinOffer();
			UE_LOG(LogApexSimNet, Log, TEXT("<- SessionJoined SessionId=%s YourGridPosition=%d LockedAssists=%d Conditions=%s Damage=%d AiSkill=%d RaceSeconds=%d QualifyingSeconds=%d"),
				*CurrentSessionId, Message.GridPosition, CurrentAllowedAssists.CountLocked(), *CurrentConditions.Describe(),
				static_cast<int32>(CurrentDamage), CurrentAiSkill, CurrentRaceSeconds, CurrentQualifyingSeconds);
			OnSessionJoined.Broadcast(CurrentSessionId, Message.GridPosition);

			// The cached lobby state predates this session, so anything resolving the
			// session by id against it — the track level, the lobby screen — misses
			// until the next periodic broadcast. Refresh right away.
			TimeSinceLobbyRequest = LobbyStateDebounceSeconds;
			RequestLobbyState();
		}
		break;

	case EApexServerMessageType::SessionLeft:
		CurrentSessionId.Reset();
		bSessionSpectator = false;
		CurrentAllowedAssists = FApexAllowedAssists();
		CurrentConditions = FApexSessionConditions();
		CurrentDamage = EApexDamageLevel::Full;
		CurrentAiSkill = ApexAiSkill::Mixed;
		CurrentRaceSeconds = 0;
		CurrentQualifyingSeconds = 0;
		CurrentQualifyingCutoffPct = 0;
		CurrentMandatoryStops = 0;
		bCurrentCompoundChange = false;
		CurrentQualifyingFormat = EApexQualifyingFormat::Open;
		CurrentQualifyingLapAllowance = 0;
		CurrentRaceRules = FApexRaceRules();
		CurrentWeekend = FApexWeekendFormat();
		CurrentWeekendState = FApexWeekendState();
		CurrentShootoutState = FApexShootoutState();
		CurrentPhysics = FApexSessionPhysics();
		CurrentConditionsBytes.Reset();
		CurrentGridPosition = 0;
		CachedRoster = FApexSessionRoster();
		ClearRacingLine();
		ClearLapTiming();
		LatestDriverFeedback = FApexDriverFeedback();
		DriverFeedbackTime = 0.0;
		LatestOwnCar = FApexOwnCarSnapshot();
		OwnCarState.Reset();
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
		if (bSpectateRequested && !bSessionRequestPending)
		{
			UE_LOG(LogApexSimNet, Warning, TEXT("<- Error %d for the showcase: %s"), Message.ErrorCode, *Message.Reason);
			bSpectateRequested = false;
			OnSpectatorJoined.Broadcast(FString(), FString());
			break;
		}
		bSessionRequestPending = false;
		bSpectatorJoinRequested = false;
		UE_LOG(LogApexSimNet, Warning, TEXT("<- Error %d: %s"), Message.ErrorCode, *Message.Reason);
		OnServerError.Broadcast(Message.ErrorCode, Message.Reason);
		break;

	case EApexServerMessageType::TrackBundleChunk:
		UE_LOG(LogApexSimNet, Verbose, TEXT("<- TrackBundleChunk %08X %d of %d, %d bytes"), Message.TrackBundleChunk.Crc,
			Message.TrackBundleChunk.Chunk, Message.TrackBundleChunk.Chunks, Message.TrackBundleChunk.Bytes.Num());
		OnTrackBundleChunk.Broadcast(Message.TrackBundleChunk);
		break;

	case EApexServerMessageType::Chat:
		if (Message.Chat.bValid)
		{
			FApexChatLine Line = Message.Chat;
			Line.ArrivedSeconds = FPlatformTime::Seconds();
			UE_LOG(LogApexSimNet, Log, TEXT("<- Chat %s%s: %s"), *Line.From, Line.bInSession ? TEXT(" (session)") : TEXT(""), *Line.Text);
			ChatLines.Add(Line);
			if (ChatLines.Num() > MaxChatLines)
			{
				ChatLines.RemoveAt(0, ChatLines.Num() - MaxChatLines);
			}
			OnChat.Broadcast(Line);
		}
		break;

	case EApexServerMessageType::ServerNotice:
		// Not the Error path: a notice answers no request, so the pending
		// flags stay as they are.
		UE_LOG(LogApexSimNet, Log, TEXT("<- ServerNotice: %s"), *Message.Reason);
		OnServerNotice.Broadcast(Message.Reason);
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
		// A car that left took its index with it; the next car given it has
		// made no stop yet.
		for (auto It = PitServices.CreateIterator(); It; ++It)
		{
			const int32 CarIndex = It.Key();
			if (!CachedRoster.Entries.ContainsByPredicate([CarIndex](const FApexRosterEntry& Entry) { return Entry.CarIndex == CarIndex; }))
			{
				It.RemoveCurrent();
			}
		}
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

	case EApexServerMessageType::TrackCorners:
		CachedCorners = Message.TrackCorners;
		UE_LOG(LogApexSimNet, Log, TEXT("<- TrackCorners %d corner(s) over %.0f m"),
			CachedCorners.Corners.Num(), CachedCorners.TrackLengthM);
		break;

	case EApexServerMessageType::RoadState:
		// The backdrop's road comes from its stream, not from a session we
		// are not driving in.
		if (!bBackdropFeed && Message.RoadState.IsValid())
		{
			UE_LOG(LogApexSimNet, VeryVerbose, TEXT("<- RoadState %d cell(s) from %d, %d debris"),
				Message.RoadState.NumCells(), Message.RoadState.FirstCell, Message.RoadState.Debris.Num());
			OnRoadState.Broadcast(Message.RoadState);
		}
		break;

	case EApexServerMessageType::TrackSectors:
		CachedSectors = Message.TrackSectors;
		TimingBoard.Reset(CachedSectors.SectorCount());
		UE_LOG(LogApexSimNet, Log, TEXT("<- TrackSectors %d sector(s) over %.0f m"),
			CachedSectors.SectorCount(), CachedSectors.TrackLengthM);
		break;

	case EApexServerMessageType::LapTiming:
		TimingBoard.Apply(Message.LapTiming);
		UE_LOG(LogApexSimNet, Verbose, TEXT("<- LapTiming car %d lap %d sector %d %d ms%s%s"),
			Message.LapTiming.CarIndex, Message.LapTiming.Lap, Message.LapTiming.Sector + 1,
			Message.LapTiming.SectorTimeMs,
			Message.LapTiming.bIsLapEnd ? TEXT(" (lap end)") : TEXT(""),
			Message.LapTiming.bValid ? TEXT("") : TEXT(" INVALID"));
		OnLapTiming.Broadcast(Message.LapTiming);
		break;

	case EApexServerMessageType::LapRecord:
		CachedLapRecord = Message.LapRecord;
		UE_LOG(LogApexSimNet, Log, TEXT("<- LapRecord %s: %d ms%s (track record %d ms by %s)"),
			*CachedLapRecord.PlayerName, CachedLapRecord.LapTimeMs,
			CachedLapRecord.bIsNew ? TEXT(" NEW") : TEXT(""),
			CachedLapRecord.TrackRecordMs, *CachedLapRecord.TrackRecordHolder);
		OnLapRecord.Broadcast(CachedLapRecord);
		break;

	case EApexServerMessageType::GhostLap:
		CachedGhostLap = Message.GhostLap;
		UE_LOG(LogApexSimNet, Log, TEXT("<- GhostLap %d sample(s), %d ms"),
			CachedGhostLap.Samples.Num(), CachedGhostLap.LapTimeMs);
		OnGhostLap.Broadcast(CachedGhostLap);
		break;

	case EApexServerMessageType::WeekendState:
		// A new phase is a fresh timing sheet, as the server's is: the last
		// round's bests and purple sectors are not this one's.
		if (CurrentWeekendState.bValid
			&& (CurrentWeekendState.Phase != Message.WeekendState.Phase || CurrentWeekendState.Round != Message.WeekendState.Round))
		{
			TimingBoard.Reset(TimingBoard.SectorCount);
		}
		CurrentWeekendState = Message.WeekendState;
		// A phase may run under its own damage rule: the session's rule is
		// the phase's while it runs.
		CurrentDamage = CurrentWeekendState.Damage;
		// So may its sky (a weekend day's own): the garage and the lobby
		// show the day's, not the one the session was joined under.
		if (CurrentWeekendState.bHasConditions)
		{
			CurrentConditions = CurrentWeekendState.Conditions;
		}
		UE_LOG(LogApexSimNet, Log, TEXT("<- WeekendState phase=%d round=%d/%d finished=%d advancing=%d eliminated=%d damage=%d sky=%s"),
			static_cast<int32>(CurrentWeekendState.Phase), CurrentWeekendState.Round, CurrentWeekendState.Rounds,
			CurrentWeekendState.bFinished ? 1 : 0, CurrentWeekendState.Advancing, CurrentWeekendState.Eliminated.Num(),
			static_cast<int32>(CurrentDamage), CurrentWeekendState.bHasConditions ? *CurrentConditions.Describe() : TEXT("-"));
		OnWeekendStateChanged.Broadcast();
		break;

	case EApexServerMessageType::ShootoutState:
		CurrentShootoutState = Message.ShootoutState;
		UE_LOG(LogApexSimNet, Log, TEXT("<- ShootoutState cars=%d running=%d done=%d flying=%d"),
			CurrentShootoutState.Order.Num(), CurrentShootoutState.Running, CurrentShootoutState.Done,
			CurrentShootoutState.bFlying ? 1 : 0);
		OnShootoutStateChanged.Broadcast();
		break;

	case EApexServerMessageType::Leaderboard:
		LeaderboardByKey.Add((Message.LeaderboardTrackId + TEXT("|") + Message.LeaderboardCarId).ToLower(), Message.Leaderboard);
		UE_LOG(LogApexSimNet, Log, TEXT("<- Leaderboard %d lap(s) for track %s"), Message.Leaderboard.Num(), *Message.LeaderboardTrackId);
		OnLeaderboard.Broadcast(Message.LeaderboardTrackId, Message.LeaderboardCarId);
		break;

	case EApexServerMessageType::QualifyingResults:
		QualifyingByTrack.Add(Message.QualifyingTrackId.ToLower(), Message.QualifyingResults);
		UE_LOG(LogApexSimNet, Log, TEXT("<- QualifyingResults %d for track %s"),
			Message.QualifyingResults.Num(), *Message.QualifyingTrackId);
		OnQualifyingResults.Broadcast(Message.QualifyingTrackId);
		break;

	case EApexServerMessageType::PitBox:
		OwnPitBox = Message.PitBox;
		UE_LOG(LogApexSimNet, Log, TEXT("<- PitBox %d at (%.1f, %.1f)m, %.1f m off the lane's middle, %s"),
			OwnPitBox.PitBox + 1, OwnPitBox.Position.X, OwnPitBox.Position.Y, OwnPitBox.LateralM,
			OwnPitBox.bManual ? TEXT("manual pit lane") : TEXT("pit autopilot"));
		break;

	case EApexServerMessageType::PitService:
	{
		const FApexPitService& Stop = Message.PitService;
		PitServices.Add(Stop.CarIndex, Stop);
		UE_LOG(LogApexSimNet, Log, TEXT("<- PitService car %d box %d: tyres %.1f s (compound %d), fuel %.1f s (%.1f L), repairs %.1f s (%.0f%%), %.1f s"),
			Stop.CarIndex, Stop.PitBox + 1, Stop.TyresS, Stop.Compound, Stop.FuelS, Stop.FuelL, Stop.RepairS, Stop.RepairPct, Stop.TotalS);
		break;
	}

	case EApexServerMessageType::Showcases:
		CachedShowcases = Message.Showcases;
		UE_LOG(LogApexSimNet, Log, TEXT("<- Showcases %d channel(s)"), CachedShowcases.Num());
		OnShowcases.Broadcast(CachedShowcases);
		break;

	case EApexServerMessageType::SpectatorJoined:
		if (!bSpectateRequested)
		{
			// Withdrawn while in flight; the LeaveSpectate sent then takes
			// the server back out of it.
			UE_LOG(LogApexSimNet, Log, TEXT("<- SpectatorJoined (already withdrawn) %s"), *Message.ShowcaseId);
			break;
		}
		bSpectateRequested = false;
		bSpectating = true;
		if (UdpConnection)
		{
			UdpConnection->DiscardQueuedSpectatorRecords();
		}
		UE_LOG(LogApexSimNet, Log, TEXT("<- SpectatorJoined showcase %s stream %s"), *Message.ShowcaseId, *Message.StreamId);
		OnSpectatorJoined.Broadcast(Message.StreamId, Message.ShowcaseId);
		break;

	case EApexServerMessageType::SpectatorRecord:
		if (bSpectating)
		{
			OnSpectatorRecords.Broadcast(Message.SpectatorRecords);
		}
		break;

	case EApexServerMessageType::CarSetupSheet:
		CachedSetupSheet = Message.CarSetupSheet;
		UE_LOG(LogApexSimNet, Log, TEXT("<- CarSetupSheet %d knob(s), %d gear(s), %.2f L/lap"),
			CachedSetupSheet.Knobs.Num(), CachedSetupSheet.GearRatios.Num(), CachedSetupSheet.LapFuelL);
		OnCarSetupSheet.Broadcast(CachedSetupSheet);
		break;

	default:
		UE_LOG(LogApexSimNet, Verbose, TEXT("<- ignoring server message '%s'"), *Message.VariantName);
		break;
	}
}
