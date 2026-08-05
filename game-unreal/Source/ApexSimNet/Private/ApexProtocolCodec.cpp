#include "ApexProtocolCodec.h"

#include "ApexSimNetModule.h"
#include "HAL/IConsoleManager.h"
#include "MsgPack/MsgPackFormat.h"
#include "MsgPack/MsgPackReader.h"
#include "MsgPack/MsgPackWriter.h"

namespace
{
	/**
	 * Centerline parsing is off by default: LobbyState is broadcast every ~2s
	 * carrying every 10th point of all 26 tracks (~14k {x,y} maps) and the menu
	 * shell never reads them. Turn it on to get a mini track map for free.
	 */
	TAutoConsoleVariable<int32> CVarParseCenterline(
		TEXT("apexsim.net.ParseCenterline"),
		0,
		TEXT("Parse TrackConfigSummary.Centerline from LobbyState instead of skipping it (0 = skip)."),
		ECVF_Default);

	// Envelope keys.
	constexpr const ANSICHAR* KeyType = "type";
	constexpr const ANSICHAR* KeyData = "data";

	/** Writes `{"type": <Variant>}` — the exact shape of a serde unit variant. */
	TArray<uint8> EncodeUnitVariant(const ANSICHAR* VariantName)
	{
		FMsgPackWriter Writer(32);
		Writer.WriteMapHeader(1);
		Writer.WriteString(KeyType);
		Writer.WriteString(VariantName);
		return MoveTemp(Writer.GetBuffer());
	}

	/** Opens `{"type": <Variant>, "data": {<FieldCount fields>` for the caller to fill. */
	void BeginDataVariant(FMsgPackWriter& Writer, const ANSICHAR* VariantName, int32 FieldCount)
	{
		Writer.WriteMapHeader(2);
		Writer.WriteString(KeyType);
		Writer.WriteString(VariantName);
		Writer.WriteString(KeyData);
		Writer.WriteMapHeader(FieldCount);
	}

	bool ParseLobbyPlayer(FMsgPackReader& Reader, FApexLobbyPlayer& Out)
	{
		int32 FieldCount = 0;
		if (!Reader.ReadMapHeader(FieldCount))
		{
			return false;
		}
		for (int32 i = 0; i < FieldCount; ++i)
		{
			FString Key;
			if (!Reader.ReadString(Key))
			{
				return false;
			}
			bool bOk = true;
			if (Key == TEXT("Id"))                { bOk = Reader.ReadString(Out.Id); }
			else if (Key == TEXT("Name"))         { bOk = Reader.ReadString(Out.Name); }
			else if (Key == TEXT("SelectedCar"))  { bOk = Reader.ReadStringOrNil(Out.SelectedCar); }
			else if (Key == TEXT("InSession"))    { bOk = Reader.ReadStringOrNil(Out.InSession); }
			else                                  { bOk = Reader.SkipValue(); }
			if (!bOk)
			{
				return false;
			}
		}
		return true;
	}

	bool ParseSessionSummary(FMsgPackReader& Reader, FApexSessionSummary& Out)
	{
		int32 FieldCount = 0;
		if (!Reader.ReadMapHeader(FieldCount))
		{
			return false;
		}
		for (int32 i = 0; i < FieldCount; ++i)
		{
			FString Key;
			if (!Reader.ReadString(Key))
			{
				return false;
			}
			bool bOk = true;
			uint64 Raw = 0;
			if (Key == TEXT("Id"))              { bOk = Reader.ReadString(Out.Id); }
			else if (Key == TEXT("TrackName"))  { bOk = Reader.ReadString(Out.TrackName); }
			else if (Key == TEXT("TrackFile"))  { bOk = Reader.ReadString(Out.TrackFile); }
			else if (Key == TEXT("HostName"))   { bOk = Reader.ReadString(Out.HostName); }
			else if (Key == TEXT("SessionKind")){ bOk = Reader.ReadUInt64(Raw); Out.SessionKind = static_cast<EApexSessionKind>(Raw); }
			else if (Key == TEXT("PlayerCount")){ bOk = Reader.ReadUInt64(Raw); Out.PlayerCount = static_cast<int32>(Raw); }
			else if (Key == TEXT("MaxPlayers")) { bOk = Reader.ReadUInt64(Raw); Out.MaxPlayers = static_cast<int32>(Raw); }
			else if (Key == TEXT("State"))      { bOk = Reader.ReadUInt64(Raw); Out.State = static_cast<EApexSessionState>(Raw); }
			else                                { bOk = Reader.SkipValue(); }
			if (!bOk)
			{
				return false;
			}
		}
		return true;
	}

	bool ParseCarConfigSummary(FMsgPackReader& Reader, FApexCarConfigSummary& Out)
	{
		int32 FieldCount = 0;
		if (!Reader.ReadMapHeader(FieldCount))
		{
			return false;
		}
		for (int32 i = 0; i < FieldCount; ++i)
		{
			FString Key;
			if (!Reader.ReadString(Key))
			{
				return false;
			}
			bool bOk = true;
			if (Key == TEXT("Id"))                   { bOk = Reader.ReadString(Out.Id); }
			else if (Key == TEXT("Name"))            { bOk = Reader.ReadString(Out.Name); }
			else if (Key == TEXT("ModelPath"))       { bOk = Reader.ReadString(Out.ModelPath); }
			else if (Key == TEXT("MassKg"))          { bOk = Reader.ReadFloat(Out.MassKg); }
			else if (Key == TEXT("MaxEngineForceN")) { bOk = Reader.ReadFloat(Out.MaxEngineForceN); }
			else                                     { bOk = Reader.SkipValue(); }
			if (!bOk)
			{
				return false;
			}
		}
		return true;
	}

	/**
	 * TrackPoint (network.rs:318) is the one struct with NO rename_all, so its
	 * keys are lowercase "x"/"y" even though the parent is PascalCase.
	 */
	bool ParseTrackPoint(FMsgPackReader& Reader, FVector2D& Out)
	{
		int32 FieldCount = 0;
		if (!Reader.ReadMapHeader(FieldCount))
		{
			return false;
		}
		for (int32 i = 0; i < FieldCount; ++i)
		{
			FString Key;
			if (!Reader.ReadString(Key))
			{
				return false;
			}
			bool bOk = true;
			float Component = 0.0f;
			if (Key == TEXT("x"))      { bOk = Reader.ReadFloat(Component); Out.X = Component; }
			else if (Key == TEXT("y")) { bOk = Reader.ReadFloat(Component); Out.Y = Component; }
			else                       { bOk = Reader.SkipValue(); }
			if (!bOk)
			{
				return false;
			}
		}
		return true;
	}

	bool ParseTrackConfigSummary(FMsgPackReader& Reader, FApexTrackConfigSummary& Out)
	{
		int32 FieldCount = 0;
		if (!Reader.ReadMapHeader(FieldCount))
		{
			return false;
		}
		for (int32 i = 0; i < FieldCount; ++i)
		{
			FString Key;
			if (!Reader.ReadString(Key))
			{
				return false;
			}
			bool bOk = true;
			if (Key == TEXT("Id"))
			{
				bOk = Reader.ReadString(Out.Id);
			}
			else if (Key == TEXT("Name"))
			{
				bOk = Reader.ReadString(Out.Name);
			}
			else if (Key == TEXT("Centerline"))
			{
				if (!ApexProtocol::ShouldParseCenterline())
				{
					bOk = Reader.SkipValue();
				}
				else
				{
					int32 PointCount = 0;
					bOk = Reader.ReadArrayHeader(PointCount);
					if (bOk)
					{
						Out.Centerline.Reserve(PointCount);
						for (int32 p = 0; p < PointCount && bOk; ++p)
						{
							FVector2D Point = FVector2D::ZeroVector;
							bOk = ParseTrackPoint(Reader, Point);
							Out.Centerline.Add(Point);
						}
					}
				}
			}
			else
			{
				bOk = Reader.SkipValue();
			}
			if (!bOk)
			{
				return false;
			}
		}
		return true;
	}

	template <typename ElementType, typename ParseFn>
	bool ParseArrayOf(FMsgPackReader& Reader, TArray<ElementType>& Out, ParseFn&& Parse)
	{
		int32 Count = 0;
		if (!Reader.ReadArrayHeader(Count))
		{
			return false;
		}
		Out.Reserve(Count);
		for (int32 i = 0; i < Count; ++i)
		{
			ElementType Element;
			if (!Parse(Reader, Element))
			{
				return false;
			}
			Out.Add(MoveTemp(Element));
		}
		return true;
	}

	bool ParseAuthSuccess(FMsgPackReader& Reader, FApexAuthSuccess& Out)
	{
		int32 FieldCount = 0;
		if (!Reader.ReadMapHeader(FieldCount))
		{
			return false;
		}
		for (int32 i = 0; i < FieldCount; ++i)
		{
			FString Key;
			if (!Reader.ReadString(Key))
			{
				return false;
			}
			bool bOk = true;
			uint64 Raw = 0;
			if (Key == TEXT("PlayerId"))             { bOk = Reader.ReadString(Out.PlayerId); }
			else if (Key == TEXT("ServerVersion"))   { bOk = Reader.ReadUInt64(Raw); Out.ServerVersion = static_cast<int64>(Raw); }
			else if (Key == TEXT("ProtocolVersion")) { bOk = Reader.ReadUInt64(Raw); Out.ProtocolVersion = static_cast<int32>(Raw); }
			else if (Key == TEXT("UdpToken"))        { bOk = Reader.ReadString(Out.UdpToken); }
			else if (Key == TEXT("UdpPort"))         { bOk = Reader.ReadUInt64(Raw); Out.UdpPort = static_cast<int32>(Raw); }
			else                                     { bOk = Reader.SkipValue(); }
			if (!bOk)
			{
				return false;
			}
		}
		return true;
	}

	bool ParseLobbyState(FMsgPackReader& Reader, FApexLobbyState& Out)
	{
		int32 FieldCount = 0;
		if (!Reader.ReadMapHeader(FieldCount))
		{
			return false;
		}
		for (int32 i = 0; i < FieldCount; ++i)
		{
			FString Key;
			if (!Reader.ReadString(Key))
			{
				return false;
			}
			bool bOk = true;
			if (Key == TEXT("PlayersInLobby"))
			{
				bOk = ParseArrayOf(Reader, Out.PlayersInLobby, &ParseLobbyPlayer);
			}
			else if (Key == TEXT("AvailableSessions"))
			{
				bOk = ParseArrayOf(Reader, Out.AvailableSessions, &ParseSessionSummary);
			}
			else if (Key == TEXT("CarConfigs"))
			{
				bOk = ParseArrayOf(Reader, Out.CarConfigs, &ParseCarConfigSummary);
			}
			else if (Key == TEXT("TrackConfigs"))
			{
				bOk = ParseArrayOf(Reader, Out.TrackConfigs, &ParseTrackConfigSummary);
			}
			else
			{
				bOk = Reader.SkipValue();
			}
			if (!bOk)
			{
				return false;
			}
		}
		return true;
	}

	/** Parses the inline (snake_case) fields of a struct variant. */
	bool ParseInlineVariantFields(FMsgPackReader& Reader, FApexServerMessage& Out)
	{
		int32 FieldCount = 0;
		if (!Reader.ReadMapHeader(FieldCount))
		{
			return false;
		}
		for (int32 i = 0; i < FieldCount; ++i)
		{
			FString Key;
			if (!Reader.ReadString(Key))
			{
				return false;
			}
			bool bOk = true;
			uint64 Raw = 0;
			if (Key == TEXT("reason"))                 { bOk = Reader.ReadString(Out.Reason); }
			else if (Key == TEXT("message"))           { bOk = Reader.ReadString(Out.Reason); }
			else if (Key == TEXT("code"))              { bOk = Reader.ReadUInt64(Raw); Out.ErrorCode = static_cast<int32>(Raw); }
			else if (Key == TEXT("server_tick"))       { bOk = Reader.ReadUInt64(Raw); Out.ServerTick = static_cast<int64>(Raw); }
			else if (Key == TEXT("countdown_seconds")) { bOk = Reader.ReadUInt64(Raw); Out.CountdownSeconds = static_cast<int32>(Raw); }
			else if (Key == TEXT("seconds_remaining")) { bOk = Reader.ReadUInt64(Raw); Out.CountdownSeconds = static_cast<int32>(Raw); }
			else if (Key == TEXT("mode"))              { bOk = Reader.ReadUInt64(Raw); Out.GameMode = static_cast<EApexGameMode>(Raw); }
			else                                       { bOk = Reader.SkipValue(); }
			if (!bOk)
			{
				return false;
			}
		}
		return true;
	}

	/** `SessionJoinedData` — PascalCase keys. */
	bool ParseSessionJoined(FMsgPackReader& Reader, FApexServerMessage& Out)
	{
		int32 FieldCount = 0;
		if (!Reader.ReadMapHeader(FieldCount))
		{
			return false;
		}
		for (int32 i = 0; i < FieldCount; ++i)
		{
			FString Key;
			if (!Reader.ReadString(Key))
			{
				return false;
			}
			bool bOk = true;
			uint64 Raw = 0;
			if (Key == TEXT("SessionId"))              { bOk = Reader.ReadString(Out.SessionId); }
			else if (Key == TEXT("YourGridPosition"))  { bOk = Reader.ReadUInt64(Raw); Out.GridPosition = static_cast<int32>(Raw); }
			else                                       { bOk = Reader.SkipValue(); }
			if (!bOk)
			{
				return false;
			}
		}
		return true;
	}

	/** `RosterEntry` (network.rs:457) — PascalCase keys. */
	bool ParseRosterEntry(FMsgPackReader& Reader, FApexRosterEntry& Out)
	{
		int32 FieldCount = 0;
		if (!Reader.ReadMapHeader(FieldCount))
		{
			return false;
		}
		for (int32 i = 0; i < FieldCount; ++i)
		{
			FString Key;
			if (!Reader.ReadString(Key))
			{
				return false;
			}
			bool bOk = true;
			uint64 Raw = 0;
			if (Key == TEXT("CarIndex"))        { bOk = Reader.ReadUInt64(Raw); Out.CarIndex = static_cast<int32>(Raw); }
			else if (Key == TEXT("PlayerId"))   { bOk = Reader.ReadString(Out.PlayerId); }
			else if (Key == TEXT("PlayerName")) { bOk = Reader.ReadString(Out.PlayerName); }
			else if (Key == TEXT("IsAi"))       { bOk = Reader.ReadBool(Out.bIsAi); }
			else                                { bOk = Reader.SkipValue(); }
			if (!bOk)
			{
				return false;
			}
		}
		return true;
	}

	/** `SessionRosterData` — PascalCase keys. */
	bool ParseSessionRoster(FMsgPackReader& Reader, FApexSessionRoster& Out)
	{
		int32 FieldCount = 0;
		if (!Reader.ReadMapHeader(FieldCount))
		{
			return false;
		}
		for (int32 i = 0; i < FieldCount; ++i)
		{
			FString Key;
			if (!Reader.ReadString(Key))
			{
				return false;
			}
			bool bOk = true;
			if (Key == TEXT("SessionId"))    { bOk = Reader.ReadString(Out.SessionId); }
			else if (Key == TEXT("Entries")) { bOk = ParseArrayOf(Reader, Out.Entries, &ParseRosterEntry); }
			else                             { bOk = Reader.SkipValue(); }
			if (!bOk)
			{
				return false;
			}
		}
		return true;
	}

	/** `PlayerDisconnectedData` — PascalCase keys. */
	bool ParsePlayerDisconnected(FMsgPackReader& Reader, FApexServerMessage& Out)
	{
		int32 FieldCount = 0;
		if (!Reader.ReadMapHeader(FieldCount))
		{
			return false;
		}
		for (int32 i = 0; i < FieldCount; ++i)
		{
			FString Key;
			if (!Reader.ReadString(Key))
			{
				return false;
			}
			bool bOk = (Key == TEXT("PlayerId")) ? Reader.ReadString(Out.PlayerId) : Reader.SkipValue();
			if (!bOk)
			{
				return false;
			}
		}
		return true;
	}

	// --- Positional decoding (UDP telemetry) ---------------------------------
	//
	// `to_vec` writes every struct as a bare array of its fields in declaration
	// order, with no names. There is nothing to match on, so the reader must
	// consume exactly the right number of values in exactly the right order.
	// Reading one field too few leaves the cursor mid-struct and every
	// subsequent value is garbage — hence the trailing skip loop in each parser.

	/** Number of fields in `CompactCarState` (network.rs:388). */
	constexpr int32 CompactCarFieldCount = 22;
	/** Number of fields in `CompactTelemetry` (network.rs:415). */
	constexpr int32 CompactTelemetryFieldCount = 5;

	/** Reads an `Option<T>` that the client does not need, as a plain skip. */
	bool SkipOptional(FMsgPackReader& Reader)
	{
		return Reader.TryReadNil() ? true : Reader.SkipValue();
	}

	bool ParseCompactCarState(FMsgPackReader& Reader, FApexCarTelemetry& Out)
	{
		int32 FieldCount = 0;
		if (!Reader.ReadArrayHeader(FieldCount))
		{
			return false;
		}

		// Read what we know, in order. Anything the server appends beyond this
		// is skipped; anything it *inserts* would desynchronise us, which is
		// the price of a positional encoding.
		const int32 Known = FMath::Min(FieldCount, CompactCarFieldCount);
		int32 Index = 0;
		bool bOk = true;

		auto Next = [&](auto&& Read) -> bool
		{
			if (Index >= Known)
			{
				return false;
			}
			++Index;
			return Read();
		};

		uint64 Raw = 0;
		int64 Signed = 0;

		bOk &= Next([&] { return Reader.ReadUInt64(Raw) ? (Out.CarIndex = static_cast<int32>(Raw), true) : false; });
		bOk &= Next([&] { float V = 0.0f; if (!Reader.ReadFloat(V)) { return false; } Out.Position.X = V; return true; });
		bOk &= Next([&] { float V = 0.0f; if (!Reader.ReadFloat(V)) { return false; } Out.Position.Y = V; return true; });
		bOk &= Next([&] { float V = 0.0f; if (!Reader.ReadFloat(V)) { return false; } Out.Position.Z = V; return true; });
		bOk &= Next([&] { return Reader.ReadFloat(Out.YawRad); });
		bOk &= Next([&] { return Reader.ReadFloat(Out.PitchRad); });
		bOk &= Next([&] { return Reader.ReadFloat(Out.RollRad); });
		bOk &= Next([&] { return Reader.ReadFloat(Out.SpeedMps); });
		bOk &= Next([&] { return Reader.ReadFloat(Out.Throttle); });
		bOk &= Next([&] { return Reader.ReadFloat(Out.Brake); });
		bOk &= Next([&] { return Reader.ReadFloat(Out.Steering); });
		bOk &= Next([&] { return Reader.ReadInt64(Signed) ? (Out.Gear = static_cast<int32>(Signed), true) : false; });
		bOk &= Next([&] { return Reader.ReadFloat(Out.EngineRpm); });
		// Suspension: 16 floats the shell has no use for, but they still have
		// to be consumed to stay aligned.
		bOk &= Next([&] { return Reader.SkipValue(); });
		bOk &= Next([&] { return Reader.ReadUInt64(Raw) ? (Out.CurrentLap = static_cast<int32>(Raw), true) : false; });
		bOk &= Next([&] { return Reader.ReadFloat(Out.TrackProgress); });
		bOk &= Next([&] { return SkipOptional(Reader); });   // finish_position
		bOk &= Next([&] { return Reader.ReadUInt64(Raw) ? (Out.CurrentLapTimeMs = static_cast<int32>(Raw), true) : false; });
		bOk &= Next([&] { return SkipOptional(Reader); });   // last_lap_time_ms
		bOk &= Next([&] { return SkipOptional(Reader); });   // best_lap_time_ms
		bOk &= Next([&] { return Reader.ReadBool(Out.bIsOnTrack); });
		bOk &= Next([&] { return Reader.ReadBool(Out.bIsColliding); });

		if (!bOk)
		{
			return false;
		}

		for (int32 Extra = Index; Extra < FieldCount; ++Extra)
		{
			if (!Reader.SkipValue())
			{
				return false;
			}
		}
		return true;
	}

	bool ParseCompactTelemetry(FMsgPackReader& Reader, FApexTelemetryFrame& Out)
	{
		int32 FieldCount = 0;
		if (!Reader.ReadArrayHeader(FieldCount))
		{
			return false;
		}
		if (FieldCount < CompactTelemetryFieldCount)
		{
			return false;
		}

		uint64 Raw = 0;
		if (!Reader.ReadUInt64(Raw)) { return false; }
		Out.ServerTick = static_cast<int64>(Raw);

		if (!Reader.ReadUInt64(Raw)) { return false; }
		Out.SessionState = static_cast<EApexSessionState>(Raw);

		if (!Reader.ReadUInt64(Raw)) { return false; }
		Out.GameMode = static_cast<EApexGameMode>(Raw);

		if (Reader.TryReadNil())
		{
			Out.CountdownMs = -1;
		}
		else if (Reader.ReadUInt64(Raw))
		{
			Out.CountdownMs = static_cast<int32>(Raw);
		}
		else
		{
			return false;
		}

		if (!ParseArrayOf(Reader, Out.Cars, &ParseCompactCarState))
		{
			return false;
		}

		for (int32 Extra = CompactTelemetryFieldCount; Extra < FieldCount; ++Extra)
		{
			if (!Reader.SkipValue())
			{
				return false;
			}
		}
		return true;
	}

	EApexServerMessageType VariantToType(const FString& Variant)
	{
		if (Variant == TEXT("AuthSuccess"))        { return EApexServerMessageType::AuthSuccess; }
		if (Variant == TEXT("AuthFailure"))        { return EApexServerMessageType::AuthFailure; }
		if (Variant == TEXT("HeartbeatAck"))       { return EApexServerMessageType::HeartbeatAck; }
		if (Variant == TEXT("LobbyState"))         { return EApexServerMessageType::LobbyState; }
		if (Variant == TEXT("SessionJoined"))      { return EApexServerMessageType::SessionJoined; }
		if (Variant == TEXT("SessionLeft"))        { return EApexServerMessageType::SessionLeft; }
		if (Variant == TEXT("SessionStarting"))    { return EApexServerMessageType::SessionStarting; }
		if (Variant == TEXT("GameModeChanged"))    { return EApexServerMessageType::GameModeChanged; }
		if (Variant == TEXT("CountdownUpdate"))    { return EApexServerMessageType::CountdownUpdate; }
		if (Variant == TEXT("Error"))              { return EApexServerMessageType::Error; }
		if (Variant == TEXT("PlayerDisconnected")) { return EApexServerMessageType::PlayerDisconnected; }
		if (Variant == TEXT("SessionRoster"))      { return EApexServerMessageType::SessionRoster; }
		if (Variant == TEXT("UdpHandshakeAck"))    { return EApexServerMessageType::UdpHandshakeAck; }
		if (Variant == TEXT("TelemetryCompact"))   { return EApexServerMessageType::TelemetryCompact; }

		// The named-encoding `Telemetry` is only used for server-side replays;
		// the wire carries TelemetryCompact.
		if (Variant == TEXT("Telemetry"))
		{
			return EApexServerMessageType::IgnoredVariant;
		}

		return EApexServerMessageType::Unknown;
	}

	bool ParseVariantData(FMsgPackReader& Reader, FApexServerMessage& Out)
	{
		switch (Out.Type)
		{
		case EApexServerMessageType::AuthSuccess:
			return ParseAuthSuccess(Reader, Out.AuthSuccess);

		case EApexServerMessageType::LobbyState:
			return ParseLobbyState(Reader, Out.LobbyState);

		case EApexServerMessageType::SessionJoined:
			return ParseSessionJoined(Reader, Out);

		case EApexServerMessageType::PlayerDisconnected:
			return ParsePlayerDisconnected(Reader, Out);

		case EApexServerMessageType::SessionRoster:
			return ParseSessionRoster(Reader, Out.Roster);

		case EApexServerMessageType::TelemetryCompact:
			// Only reachable if a compact frame ever arrives named; the UDP path
			// decodes it positionally.
			return Reader.SkipValue();

		case EApexServerMessageType::AuthFailure:
		case EApexServerMessageType::HeartbeatAck:
		case EApexServerMessageType::SessionStarting:
		case EApexServerMessageType::GameModeChanged:
		case EApexServerMessageType::CountdownUpdate:
		case EApexServerMessageType::Error:
			return ParseInlineVariantFields(Reader, Out);

		default:
			// Unknown or deliberately ignored: consume the payload so the
			// cursor stays aligned, and keep going.
			return Reader.SkipValue();
		}
	}
}

namespace ApexProtocol
{
	bool ShouldParseCenterline()
	{
		return CVarParseCenterline.GetValueOnAnyThread() != 0;
	}

	TArray<uint8> EncodeAuthenticate(const FString& Token, const FString& PlayerName)
	{
		FMsgPackWriter Writer(128);
		BeginDataVariant(Writer, "Authenticate", 3);
		Writer.WriteString("token");
		Writer.WriteString(Token);
		Writer.WriteString("player_name");
		Writer.WriteString(PlayerName);
		Writer.WriteString("protocol_version");
		Writer.WriteUInt(APEXSIM_PROTOCOL_VERSION);
		return MoveTemp(Writer.GetBuffer());
	}

	TArray<uint8> EncodeHeartbeat(uint32 ClientTick)
	{
		FMsgPackWriter Writer(32);
		BeginDataVariant(Writer, "Heartbeat", 1);
		Writer.WriteString("client_tick");
		Writer.WriteUInt(ClientTick);
		return MoveTemp(Writer.GetBuffer());
	}

	TArray<uint8> EncodeSelectCar(const FString& CarConfigId)
	{
		FMsgPackWriter Writer(96);
		BeginDataVariant(Writer, "SelectCar", 1);
		Writer.WriteString("car_config_id");
		Writer.WriteString(CarConfigId);
		return MoveTemp(Writer.GetBuffer());
	}

	TArray<uint8> EncodeRequestLobbyState()
	{
		return EncodeUnitVariant("RequestLobbyState");
	}

	TArray<uint8> EncodeCreateSession(
		const FString& TrackConfigId,
		uint8 MaxPlayers,
		uint8 AiCount,
		uint8 LapLimit,
		EApexSessionKind SessionKind)
	{
		FMsgPackWriter Writer(128);
		BeginDataVariant(Writer, "CreateSession", 5);
		Writer.WriteString("track_config_id");
		Writer.WriteString(TrackConfigId);
		Writer.WriteString("max_players");
		Writer.WriteUInt(MaxPlayers);
		Writer.WriteString("ai_count");
		Writer.WriteUInt(AiCount);
		Writer.WriteString("lap_limit");
		Writer.WriteUInt(LapLimit);
		Writer.WriteString("session_kind");
		Writer.WriteUInt(static_cast<uint8>(SessionKind));
		return MoveTemp(Writer.GetBuffer());
	}

	TArray<uint8> EncodeJoinSession(const FString& SessionId)
	{
		FMsgPackWriter Writer(96);
		BeginDataVariant(Writer, "JoinSession", 1);
		Writer.WriteString("session_id");
		Writer.WriteString(SessionId);
		return MoveTemp(Writer.GetBuffer());
	}

	TArray<uint8> EncodeJoinAsSpectator(const FString& SessionId)
	{
		FMsgPackWriter Writer(96);
		BeginDataVariant(Writer, "JoinAsSpectator", 1);
		Writer.WriteString("session_id");
		Writer.WriteString(SessionId);
		return MoveTemp(Writer.GetBuffer());
	}

	TArray<uint8> EncodeLeaveSession() { return EncodeUnitVariant("LeaveSession"); }
	TArray<uint8> EncodeStartSession() { return EncodeUnitVariant("StartSession"); }
	TArray<uint8> EncodeDisconnect()   { return EncodeUnitVariant("Disconnect"); }

	TArray<uint8> EncodeSetGameMode(EApexGameMode Mode)
	{
		FMsgPackWriter Writer(32);
		BeginDataVariant(Writer, "SetGameMode", 1);
		Writer.WriteString("mode");
		Writer.WriteUInt(static_cast<uint8>(Mode));
		return MoveTemp(Writer.GetBuffer());
	}

	TArray<uint8> EncodeStartCountdown(uint16 CountdownSeconds, EApexGameMode NextMode)
	{
		FMsgPackWriter Writer(48);
		BeginDataVariant(Writer, "StartCountdown", 2);
		Writer.WriteString("countdown_seconds");
		Writer.WriteUInt(CountdownSeconds);
		Writer.WriteString("next_mode");
		Writer.WriteUInt(static_cast<uint8>(NextMode));
		return MoveTemp(Writer.GetBuffer());
	}

	TArray<uint8> EncodeUdpHandshake(const FString& UdpToken)
	{
		FMsgPackWriter Writer(64);
		BeginDataVariant(Writer, "UdpHandshake", 1);
		Writer.WriteString("token");
		Writer.WriteString(UdpToken);
		return MoveTemp(Writer.GetBuffer());
	}

	TArray<uint8> EncodePlayerInput(uint32 ServerTickAck, const FApexPlayerInput& Input)
	{
		FMsgPackWriter Writer(96);
		BeginDataVariant(Writer, "PlayerInput", 6);
		Writer.WriteString("server_tick_ack");
		Writer.WriteUInt(ServerTickAck);
		Writer.WriteString("throttle");
		Writer.WriteFloat(Input.Throttle);
		Writer.WriteString("brake");
		Writer.WriteFloat(Input.Brake);
		Writer.WriteString("steering");
		Writer.WriteFloat(Input.Steering);
		Writer.WriteString("gear");
		if (Input.HasGear())
		{
			Writer.WriteInt(Input.Gear);
		}
		else
		{
			Writer.WriteNil();
		}
		// Clutch is always nil: the shell has no clutch input, and the server
		// treats None as "leave it alone".
		Writer.WriteString("clutch");
		Writer.WriteNil();
		return MoveTemp(Writer.GetBuffer());
	}

	bool DecodeUdpMessage(TArrayView<const uint8> Payload, FApexServerMessage& OutMessage, FString& OutError)
	{
		OutMessage = FApexServerMessage();

		if (Payload.Num() == 0)
		{
			OutError = TEXT("empty datagram");
			return false;
		}

		// A named envelope starts with a map header; a positional one with an
		// array header. That first byte is enough to pick the decoder.
		const uint8 Tag = Payload[0];
		if (!MsgPack::IsFixArray(Tag) && Tag != MsgPack::Array16 && Tag != MsgPack::Array32)
		{
			return DecodeServerMessage(Payload, OutMessage, OutError);
		}

		FMsgPackReader Reader(Payload);
		int32 EnvelopeCount = 0;
		if (!Reader.ReadArrayHeader(EnvelopeCount) || EnvelopeCount < 1)
		{
			OutError = Reader.HasError() ? Reader.GetError() : TEXT("malformed positional envelope");
			return false;
		}

		if (!Reader.ReadString(OutMessage.VariantName))
		{
			OutError = Reader.GetError();
			return false;
		}
		OutMessage.Type = VariantToType(OutMessage.VariantName);

		if (EnvelopeCount < 2)
		{
			// A positional unit variant carries no payload.
			return !Reader.HasError();
		}

		if (OutMessage.Type == EApexServerMessageType::TelemetryCompact)
		{
			if (!ParseCompactTelemetry(Reader, OutMessage.Telemetry))
			{
				OutError = Reader.HasError() ? Reader.GetError() : TEXT("failed to parse compact telemetry");
				return false;
			}
		}
		else if (!Reader.SkipValue())
		{
			OutError = Reader.GetError();
			return false;
		}

		for (int32 Extra = 2; Extra < EnvelopeCount; ++Extra)
		{
			if (!Reader.SkipValue())
			{
				OutError = Reader.GetError();
				return false;
			}
		}

		return !Reader.HasError();
	}

	bool DecodeServerMessage(TArrayView<const uint8> Payload, FApexServerMessage& OutMessage, FString& OutError)
	{
		OutMessage = FApexServerMessage();

		FMsgPackReader Reader(Payload);

		int32 EnvelopeFields = 0;
		if (!Reader.ReadMapHeader(EnvelopeFields))
		{
			OutError = Reader.GetError();
			return false;
		}

		bool bSawType = false;
		bool bSawData = false;

		for (int32 i = 0; i < EnvelopeFields; ++i)
		{
			FString Key;
			if (!Reader.ReadString(Key))
			{
				OutError = Reader.GetError();
				return false;
			}

			if (Key == TEXT("type"))
			{
				if (!Reader.ReadString(OutMessage.VariantName))
				{
					OutError = Reader.GetError();
					return false;
				}
				OutMessage.Type = VariantToType(OutMessage.VariantName);
				bSawType = true;
			}
			else if (Key == TEXT("data"))
			{
				// serde always emits "type" before "data", so the variant is
				// already known by the time the payload arrives.
				if (!bSawType)
				{
					OutError = TEXT("envelope has \"data\" before \"type\"");
					return false;
				}
				if (!ParseVariantData(Reader, OutMessage))
				{
					OutError = Reader.HasError()
						? Reader.GetError()
						: FString::Printf(TEXT("failed to parse payload of %s"), *OutMessage.VariantName);
					return false;
				}
				bSawData = true;
			}
			else if (!Reader.SkipValue())
			{
				OutError = Reader.GetError();
				return false;
			}
		}

		if (!bSawType)
		{
			OutError = TEXT("envelope has no \"type\" key");
			return false;
		}

		// A unit variant legitimately has no "data" — SessionLeft and
		// UdpHandshakeAck arrive this way.
		(void)bSawData;

		return !Reader.HasError();
	}
}
