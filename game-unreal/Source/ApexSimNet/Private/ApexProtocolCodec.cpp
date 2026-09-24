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

	/** `SessionConditions` — snake_case keys even under a PascalCase parent (no rename_all). */
	bool ParseSessionConditions(FMsgPackReader& Reader, FApexSessionConditions& Out)
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
			if (Key == TEXT("weather"))
			{
				bOk = Reader.ReadUInt64(Raw);
				Out.Weather = Raw < static_cast<uint64>(FApexSessionConditions::WeatherCount)
					? static_cast<EApexWeather>(Raw)
					: EApexWeather::Sunny;
			}
			else if (Key == TEXT("time_of_day_minutes"))
			{
				bOk = Reader.ReadUInt64(Raw);
				Out.TimeOfDayMinutes = static_cast<int32>(Raw);
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
		Out = Out.Clamped();
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
			else if (Key == TEXT("TrackId"))    { bOk = Reader.ReadString(Out.TrackId); }
			else if (Key == TEXT("HostName"))   { bOk = Reader.ReadString(Out.HostName); }
			else if (Key == TEXT("SessionKind")){ bOk = Reader.ReadUInt64(Raw); Out.SessionKind = static_cast<EApexSessionKind>(Raw); }
			else if (Key == TEXT("PlayerCount")){ bOk = Reader.ReadUInt64(Raw); Out.PlayerCount = static_cast<int32>(Raw); }
			else if (Key == TEXT("MaxPlayers")) { bOk = Reader.ReadUInt64(Raw); Out.MaxPlayers = static_cast<int32>(Raw); }
			else if (Key == TEXT("State"))      { bOk = Reader.ReadUInt64(Raw); Out.State = static_cast<EApexSessionState>(Raw); }
			else if (Key == TEXT("Conditions")) { bOk = ParseSessionConditions(Reader, Out.Conditions); }
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
			uint64 Raw = 0;
			if (Key == TEXT("Id"))                   { bOk = Reader.ReadString(Out.Id); }
			else if (Key == TEXT("Name"))            { bOk = Reader.ReadString(Out.Name); }
			else if (Key == TEXT("ModelPath"))       { bOk = Reader.ReadString(Out.ModelPath); }
			else if (Key == TEXT("MassKg"))          { bOk = Reader.ReadFloat(Out.MassKg); }
			else if (Key == TEXT("MaxEngineForceN")) { bOk = Reader.ReadFloat(Out.MaxEngineForceN); }
			else if (Key == TEXT("ContentCrc"))      { bOk = Reader.ReadUInt64(Raw); Out.ContentCrc = static_cast<int64>(Raw & 0xFFFFFFFFu); }
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
			else if (Key == TEXT("ContentCrc"))
			{
				uint64 Raw = 0;
				bOk = Reader.ReadUInt64(Raw);
				Out.ContentCrc = static_cast<int64>(Raw & 0xFFFFFFFFu);
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

	/** `AllowedAssists` — snake_case keys even under a PascalCase parent (no rename_all). */
	bool ParseAllowedAssists(FMsgPackReader& Reader, FApexAllowedAssists& Out)
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
			if (Key == TEXT("abs"))                   { bOk = Reader.ReadBool(Out.bAbs); }
			else if (Key == TEXT("traction_control")) { bOk = Reader.ReadBool(Out.bTractionControl); }
			else if (Key == TEXT("auto_gearbox"))     { bOk = Reader.ReadBool(Out.bAutoGearbox); }
			else if (Key == TEXT("steering_assist"))  { bOk = Reader.ReadBool(Out.bSteeringAssist); }
			else if (Key == TEXT("racing_line"))      { bOk = Reader.ReadBool(Out.bRacingLine); }
			else                                      { bOk = Reader.SkipValue(); }
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
			else if (Key == TEXT("SessionKind"))       { bOk = Reader.ReadUInt64(Raw); Out.SessionKind = static_cast<EApexSessionKind>(Raw); }
			else if (Key == TEXT("AllowedAssists"))    { bOk = ParseAllowedAssists(Reader, Out.AllowedAssists); }
			else if (Key == TEXT("Conditions"))        { bOk = ParseSessionConditions(Reader, Out.Conditions); }
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
			else if (Key == TEXT("CarConfigId")) { bOk = Reader.ReadString(Out.CarConfigId); }
			else if (Key == TEXT("Livery"))     { bOk = Reader.ReadUInt64(Raw); Out.Livery = static_cast<int32>(Raw); }
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

	bool ParseFloatArray(FMsgPackReader& Reader, TArray<float>& Out)
	{
		int32 Count = 0;
		if (!Reader.ReadArrayHeader(Count))
		{
			return false;
		}
		Out.SetNumUninitialized(Count);
		for (int32 i = 0; i < Count; ++i)
		{
			if (!Reader.ReadFloat(Out[i]))
			{
				return false;
			}
		}
		return true;
	}

	/**
	 * `RacingLineData` — PascalCase keys. The positions arrive as parallel
	 * X/Y/Z arrays and are zipped into points once all three are in, since
	 * nothing guarantees the key order.
	 */
	bool ParseRacingLine(FMsgPackReader& Reader, FApexRacingLineData& Out)
	{
		int32 FieldCount = 0;
		if (!Reader.ReadMapHeader(FieldCount))
		{
			return false;
		}
		TArray<float> X, Y, Z;
		TArray<uint8> Phase;
		for (int32 i = 0; i < FieldCount; ++i)
		{
			FString Key;
			if (!Reader.ReadString(Key))
			{
				return false;
			}
			bool bOk = true;
			if (Key == TEXT("SessionId"))     { bOk = Reader.ReadString(Out.SessionId); }
			else if (Key == TEXT("SpacingM")) { bOk = Reader.ReadFloat(Out.SpacingM); }
			else if (Key == TEXT("X"))        { bOk = ParseFloatArray(Reader, X); }
			else if (Key == TEXT("Y"))        { bOk = ParseFloatArray(Reader, Y); }
			else if (Key == TEXT("Z"))        { bOk = ParseFloatArray(Reader, Z); }
			else if (Key == TEXT("Phase"))
			{
				int32 Count = 0;
				bOk = Reader.ReadArrayHeader(Count);
				Phase.Reserve(Count);
				for (int32 p = 0; p < Count && bOk; ++p)
				{
					uint64 Raw = 0;
					bOk = Reader.ReadUInt64(Raw);
					Phase.Add(static_cast<uint8>(FMath::Min<uint64>(Raw, 255)));
				}
			}
			else { bOk = Reader.SkipValue(); }
			if (!bOk)
			{
				return false;
			}
		}

		// Mismatched arrays mean a broken message, not a shorter line: drop it
		// rather than draw points at the origin.
		const int32 Count = X.Num();
		if (Y.Num() != Count || Z.Num() != Count || Phase.Num() != Count)
		{
			return false;
		}
		Out.Points.SetNumUninitialized(Count);
		Out.Phases.SetNumUninitialized(Count);
		for (int32 i = 0; i < Count; ++i)
		{
			Out.Points[i] = FVector(X[i], Y[i], Z[i]);
			Out.Phases[i] = Phase[i] <= static_cast<uint8>(EApexLinePhase::Brake)
				? static_cast<EApexLinePhase>(Phase[i])
				: EApexLinePhase::Partial;
		}
		return true;
	}

	/** `TrackSectorsData` — PascalCase keys. */
	bool ParseTrackSectors(FMsgPackReader& Reader, FApexTrackSectors& Out)
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
			if (Key == TEXT("SessionId"))         { bOk = Reader.ReadString(Out.SessionId); }
			else if (Key == TEXT("TrackLengthM")) { bOk = Reader.ReadFloat(Out.TrackLengthM); }
			else if (Key == TEXT("BoundariesM"))  { bOk = ParseFloatArray(Reader, Out.BoundariesM); }
			else { bOk = Reader.SkipValue(); }
			if (!bOk)
			{
				return false;
			}
		}
		return true;
	}

	/**
	 * `LapTimingData` — PascalCase keys. `Flags` is a bit field, unpacked
	 * here into the four booleans the HUD paints with.
	 */
	bool ParseLapTiming(FMsgPackReader& Reader, FApexLapTiming& Out)
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
			if (Key == TEXT("CarIndex"))           { bOk = Reader.ReadUInt64(Raw); Out.CarIndex = static_cast<int32>(Raw); }
			else if (Key == TEXT("Lap"))           { bOk = Reader.ReadUInt64(Raw); Out.Lap = static_cast<int32>(Raw); }
			else if (Key == TEXT("Sector"))        { bOk = Reader.ReadUInt64(Raw); Out.Sector = static_cast<int32>(Raw); }
			else if (Key == TEXT("SectorTimeMs"))  { bOk = Reader.ReadUInt64(Raw); Out.SectorTimeMs = static_cast<int32>(Raw); }
			else if (Key == TEXT("LapTimeMs"))     { bOk = Reader.ReadUInt64(Raw); Out.LapTimeMs = static_cast<int32>(Raw); }
			else if (Key == TEXT("IsLapEnd"))      { bOk = Reader.ReadBool(Out.bIsLapEnd); }
			else if (Key == TEXT("Valid"))         { bOk = Reader.ReadBool(Out.bValid); }
			else if (Key == TEXT("Flags"))
			{
				bOk = Reader.ReadUInt64(Raw);
				Out.bPersonalBestLap = (Raw & 1) != 0;
				Out.bSessionBestLap = (Raw & 2) != 0;
				Out.bPersonalBestSector = (Raw & 4) != 0;
				Out.bSessionBestSector = (Raw & 8) != 0;
			}
			else { bOk = Reader.SkipValue(); }
			if (!bOk)
			{
				return false;
			}
		}
		return true;
	}

	/**
	 * `GhostLapData` — PascalCase keys, struct-of-arrays on the wire, zipped
	 * into one sample per moment here. Arrays of unequal length are cut to
	 * the shortest, so a truncated message still yields a playable lap.
	 */
	bool ParseGhostLap(FMsgPackReader& Reader, FApexGhostLap& Out)
	{
		int32 FieldCount = 0;
		if (!Reader.ReadMapHeader(FieldCount))
		{
			return false;
		}
		TArray<float> Pose;
		TArray<float> Speed;
		TArray<float> Steering;
		TArray<float> Rpm;
		TArray<int32> TimeMs;
		TArray<int32> Gear;
		auto ReadIntArray = [&Reader](TArray<int32>& Values, bool bSigned)
		{
			int32 Count = 0;
			if (!Reader.ReadArrayHeader(Count))
			{
				return false;
			}
			Values.SetNumUninitialized(Count);
			for (int32 i = 0; i < Count; ++i)
			{
				if (bSigned)
				{
					int64 Raw = 0;
					if (!Reader.ReadInt64(Raw)) { return false; }
					Values[i] = static_cast<int32>(Raw);
				}
				else
				{
					uint64 Raw = 0;
					if (!Reader.ReadUInt64(Raw)) { return false; }
					Values[i] = static_cast<int32>(Raw);
				}
			}
			return true;
		};
		for (int32 i = 0; i < FieldCount; ++i)
		{
			FString Key;
			if (!Reader.ReadString(Key))
			{
				return false;
			}
			bool bOk = true;
			uint64 Raw = 0;
			if (Key == TEXT("TrackId"))          { bOk = Reader.ReadString(Out.TrackId); }
			else if (Key == TEXT("CarConfigId")) { bOk = Reader.ReadString(Out.CarConfigId); }
			else if (Key == TEXT("LapTimeMs"))   { bOk = Reader.ReadUInt64(Raw); Out.LapTimeMs = static_cast<int32>(Raw); }
			else if (Key == TEXT("SampleHz"))    { bOk = Reader.ReadFloat(Out.SampleHz); }
			else if (Key == TEXT("TMs"))         { bOk = ReadIntArray(TimeMs, false); }
			else if (Key == TEXT("Pose"))        { bOk = ParseFloatArray(Reader, Pose); }
			else if (Key == TEXT("SpeedMps"))    { bOk = ParseFloatArray(Reader, Speed); }
			else if (Key == TEXT("Steering"))    { bOk = ParseFloatArray(Reader, Steering); }
			else if (Key == TEXT("Gear"))        { bOk = ReadIntArray(Gear, true); }
			else if (Key == TEXT("EngineRpm"))   { bOk = ParseFloatArray(Reader, Rpm); }
			else { bOk = Reader.SkipValue(); }
			if (!bOk)
			{
				return false;
			}
		}

		int32 Count = FMath::Min(TimeMs.Num(), Pose.Num() / 6);
		Count = FMath::Min(Count, Speed.Num());
		Out.Samples.Reset(Count);
		for (int32 i = 0; i < Count; ++i)
		{
			FApexGhostSample& S = Out.Samples.AddDefaulted_GetRef();
			S.TimeMs = TimeMs[i];
			S.Position = FVector(Pose[i * 6], Pose[i * 6 + 1], Pose[i * 6 + 2]);
			S.YawRad = Pose[i * 6 + 3];
			S.PitchRad = Pose[i * 6 + 4];
			S.RollRad = Pose[i * 6 + 5];
			S.SpeedMps = Speed[i];
			S.Steering = Steering.IsValidIndex(i) ? Steering[i] : 0.0f;
			S.Gear = Gear.IsValidIndex(i) ? Gear[i] : 0;
			S.EngineRpm = Rpm.IsValidIndex(i) ? Rpm[i] : 0.0f;
		}
		return true;
	}

	/** `LapRecordData` — PascalCase keys. */
	bool ParseLapRecord(FMsgPackReader& Reader, FApexLapRecord& Out)
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
			if (Key == TEXT("PlayerName"))              { bOk = Reader.ReadString(Out.PlayerName); }
			else if (Key == TEXT("TrackId"))            { bOk = Reader.ReadString(Out.TrackId); }
			else if (Key == TEXT("CarConfigId"))        { bOk = Reader.ReadString(Out.CarConfigId); }
			else if (Key == TEXT("LapTimeMs"))          { bOk = Reader.ReadUInt64(Raw); Out.LapTimeMs = static_cast<int32>(Raw); }
			else if (Key == TEXT("IsNew"))              { bOk = Reader.ReadBool(Out.bIsNew); }
			else if (Key == TEXT("TrackRecordMs"))      { bOk = Reader.ReadUInt64(Raw); Out.TrackRecordMs = static_cast<int32>(Raw); }
			else if (Key == TEXT("TrackRecordHolder"))  { bOk = Reader.ReadString(Out.TrackRecordHolder); }
			else if (Key == TEXT("HasGhost"))           { bOk = Reader.ReadBool(Out.bHasGhost); }
			else if (Key == TEXT("SplitsMs"))
			{
				int32 Count = 0;
				bOk = Reader.ReadArrayHeader(Count);
				Out.SplitsMs.Reset(Count);
				for (int32 p = 0; p < Count && bOk; ++p)
				{
					bOk = Reader.ReadUInt64(Raw);
					Out.SplitsMs.Add(static_cast<int32>(Raw));
				}
			}
			else { bOk = Reader.SkipValue(); }
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
	constexpr int32 CompactCarFieldCount = 23;
	/** Number of fields in `CompactTelemetry` (network.rs:415). */
	constexpr int32 CompactTelemetryFieldCount = 5;

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
		bOk &= Next([&]
		{
			Out.FinishPosition = 0;
			if (Reader.TryReadNil())
			{
				return true;
			}
			return Reader.ReadUInt64(Raw) ? (Out.FinishPosition = static_cast<int32>(Raw), true) : false;
		});
		bOk &= Next([&] { return Reader.ReadUInt64(Raw) ? (Out.CurrentLapTimeMs = static_cast<int32>(Raw), true) : false; });
		bOk &= Next([&]
		{
			Out.LastLapTimeMs = 0;
			if (Reader.TryReadNil())
			{
				return true;
			}
			return Reader.ReadUInt64(Raw) ? (Out.LastLapTimeMs = static_cast<int32>(Raw), true) : false;
		});
		bOk &= Next([&]
		{
			Out.BestLapTimeMs = 0;
			if (Reader.TryReadNil())
			{
				return true;
			}
			return Reader.ReadUInt64(Raw) ? (Out.BestLapTimeMs = static_cast<int32>(Raw), true) : false;
		});
		bOk &= Next([&] { return Reader.ReadBool(Out.bIsOnTrack); });
		bOk &= Next([&] { return Reader.ReadBool(Out.bIsColliding); });
		// Track limits, appended after `is_colliding`: a server that predates
		// the field simply sends one value fewer and every lap reads as clean.
		Out.bLapInvalid = false;
		Out.bLastLapInvalid = false;
		Out.bInGarage = false;
		if (Index < Known)
		{
			bOk &= Next([&]
			{
				if (!Reader.ReadUInt64(Raw))
				{
					return false;
				}
				Out.bLapInvalid = (Raw & 1) != 0;
				Out.bLastLapInvalid = (Raw & 2) != 0;
				Out.bInGarage = (Raw & 4) != 0;
				return true;
			});
		}

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

	/**
	 * Fields every `DriverFeedback` (server/src/feedback.rs) has. Later fields
	 * are appended and read only when present: `steer_kick` is the tenth.
	 */
	constexpr int32 DriverFeedbackFieldCount = 9;

	/** More steering samples than any sane tick/telemetry ratio sends; the server caps at 32. */
	constexpr int32 MaxSteerSamples = 256;

	/**
	 * One per-wheel `[T; 4]` from `DriverFeedback`, element by element into the
	 * wheels. Anything past four is skipped, so a server that ever sends more
	 * wheels cannot desynchronise the fields after it.
	 */
	template <typename ReadFn>
	bool ParseWheelArray(FMsgPackReader& Reader, FApexWheelFeedback (&Wheels)[4], ReadFn&& Read)
	{
		int32 Count = 0;
		if (!Reader.ReadArrayHeader(Count) || Count < 4)
		{
			return false;
		}
		for (int32 Wheel = 0; Wheel < 4; ++Wheel)
		{
			if (!Read(Reader, Wheels[Wheel]))
			{
				return false;
			}
		}
		for (int32 Extra = 4; Extra < Count; ++Extra)
		{
			if (!Reader.SkipValue())
			{
				return false;
			}
		}
		return true;
	}

	bool ParseDriverFeedback(FMsgPackReader& Reader, FApexDriverFeedback& Out)
	{
		int32 FieldCount = 0;
		if (!Reader.ReadArrayHeader(FieldCount) || FieldCount < DriverFeedbackFieldCount)
		{
			return false;
		}

		uint64 Raw = 0;
		if (!Reader.ReadUInt64(Raw)) { return false; }
		Out.ServerTick = static_cast<int64>(Raw);

		int32 SampleCount = 0;
		if (!Reader.ReadArrayHeader(SampleCount) || SampleCount > MaxSteerSamples)
		{
			return false;
		}
		Out.SteerTorque.Reset(SampleCount);
		for (int32 i = 0; i < SampleCount; ++i)
		{
			float Sample = 0.0f;
			if (!Reader.ReadFloat(Sample)) { return false; }
			Out.SteerTorque.Add(Sample);
		}

		const bool bWheelsOk =
			ParseWheelArray(Reader, Out.Wheels, [](FMsgPackReader& R, FApexWheelFeedback& W) { return R.ReadFloat(W.SlipRatio); })
			&& ParseWheelArray(Reader, Out.Wheels, [](FMsgPackReader& R, FApexWheelFeedback& W) { return R.ReadFloat(W.SlipAngle); })
			&& ParseWheelArray(Reader, Out.Wheels, [](FMsgPackReader& R, FApexWheelFeedback& W)
				{
					uint64 Surface = 0;
					if (!R.ReadUInt64(Surface)) { return false; }
					// A surface this client does not know yet is at least not road.
					W.Surface = static_cast<EApexContactSurface>(FMath::Min<uint64>(Surface, static_cast<uint64>(EApexContactSurface::Off)));
					return true;
				})
			&& ParseWheelArray(Reader, Out.Wheels, [](FMsgPackReader& R, FApexWheelFeedback& W) { return R.ReadFloat(W.SuspensionMps); });
		if (!bWheelsOk)
		{
			return false;
		}

		if (!Reader.ReadBool(Out.bAbsActive)) { return false; }
		if (!Reader.ReadBool(Out.bTcActive)) { return false; }
		if (!Reader.ReadFloat(Out.ImpactMps)) { return false; }

		int32 Read = DriverFeedbackFieldCount;
		Out.SteerKick = 0.0f;
		if (FieldCount > Read)
		{
			if (!Reader.ReadFloat(Out.SteerKick)) { return false; }
			++Read;
		}

		for (int32 Extra = Read; Extra < FieldCount; ++Extra)
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
		if (Variant == TEXT("RacingLine"))         { return EApexServerMessageType::RacingLine; }
		if (Variant == TEXT("TrackSectors"))       { return EApexServerMessageType::TrackSectors; }
		if (Variant == TEXT("LapTiming"))          { return EApexServerMessageType::LapTiming; }
		if (Variant == TEXT("LapRecord"))          { return EApexServerMessageType::LapRecord; }
		if (Variant == TEXT("GhostLap"))           { return EApexServerMessageType::GhostLap; }
		if (Variant == TEXT("UdpHandshakeAck"))    { return EApexServerMessageType::UdpHandshakeAck; }
		if (Variant == TEXT("TelemetryCompact"))   { return EApexServerMessageType::TelemetryCompact; }
		if (Variant == TEXT("DriverFeedback"))     { return EApexServerMessageType::DriverFeedback; }

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

		case EApexServerMessageType::RacingLine:
			return ParseRacingLine(Reader, Out.RacingLine);

		case EApexServerMessageType::TrackSectors:
			return ParseTrackSectors(Reader, Out.TrackSectors);

		case EApexServerMessageType::LapTiming:
			return ParseLapTiming(Reader, Out.LapTiming);

		case EApexServerMessageType::LapRecord:
			return ParseLapRecord(Reader, Out.LapRecord);

		case EApexServerMessageType::GhostLap:
			return ParseGhostLap(Reader, Out.GhostLap);

		case EApexServerMessageType::TelemetryCompact:
		case EApexServerMessageType::DriverFeedback:
			// Only reachable if a positional message ever arrives named; the UDP
			// path decodes them positionally.
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

	TArray<uint8> EncodeSelectCar(const FString& CarConfigId, int32 Livery)
	{
		FMsgPackWriter Writer(104);
		BeginDataVariant(Writer, "SelectCar", 2);
		Writer.WriteString("car_config_id");
		Writer.WriteString(CarConfigId);
		Writer.WriteString("livery");
		Writer.WriteUInt(static_cast<uint64>(FMath::Clamp(Livery, 0, 255)));
		return MoveTemp(Writer.GetBuffer());
	}

	TArray<uint8> EncodeRequestLobbyState()
	{
		return EncodeUnitVariant("RequestLobbyState");
	}

	/** `AllowedAssists` (data.rs): no rename_all, so snake_case keys wherever it nests. */
	void WriteAllowedAssists(FMsgPackWriter& Writer, const FApexAllowedAssists& Assists)
	{
		Writer.WriteMapHeader(5);
		Writer.WriteString("abs");
		Writer.WriteBool(Assists.bAbs);
		Writer.WriteString("traction_control");
		Writer.WriteBool(Assists.bTractionControl);
		Writer.WriteString("auto_gearbox");
		Writer.WriteBool(Assists.bAutoGearbox);
		Writer.WriteString("steering_assist");
		Writer.WriteBool(Assists.bSteeringAssist);
		Writer.WriteString("racing_line");
		Writer.WriteBool(Assists.bRacingLine);
	}

	/** `SessionConditions` (data.rs): no rename_all, so snake_case keys wherever it nests. */
	void WriteSessionConditions(FMsgPackWriter& Writer, const FApexSessionConditions& Conditions)
	{
		const FApexSessionConditions Clamped = Conditions.Clamped();
		Writer.WriteMapHeader(2);
		Writer.WriteString("weather");
		Writer.WriteUInt(static_cast<uint8>(Clamped.Weather));
		Writer.WriteString("time_of_day_minutes");
		Writer.WriteUInt(static_cast<uint16>(Clamped.TimeOfDayMinutes));
	}

	TArray<uint8> EncodeCreateSession(
		const FString& TrackConfigId,
		uint8 MaxPlayers,
		uint8 AiCount,
		uint8 LapLimit,
		EApexSessionKind SessionKind,
		const FApexAllowedAssists& AllowedAssists,
		const FApexSessionConditions& Conditions)
	{
		FMsgPackWriter Writer(320);
		BeginDataVariant(Writer, "CreateSession", 7);
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
		Writer.WriteString("allowed_assists");
		WriteAllowedAssists(Writer, AllowedAssists);
		Writer.WriteString("conditions");
		WriteSessionConditions(Writer, Conditions);
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

	TArray<uint8> EncodeSetDriverAids(
		bool bAutoGearbox, bool bSteeringAssist, bool bAbs, EApexTractionControl TractionControl)
	{
		FMsgPackWriter Writer(96);
		BeginDataVariant(Writer, "SetDriverAids", 4);
		Writer.WriteString("auto_gearbox");
		Writer.WriteBool(bAutoGearbox);
		Writer.WriteString("steering_assist");
		Writer.WriteBool(bSteeringAssist);
		// Both are Option<> on the server: a value is always sent, so the
		// player's choice — not the car file's — is what runs.
		Writer.WriteString("abs");
		Writer.WriteBool(bAbs);
		Writer.WriteString("traction_control");
		Writer.WriteUInt(static_cast<uint8>(TractionControl));
		return MoveTemp(Writer.GetBuffer());
	}

	TArray<uint8> EncodeSetCarSetup(const FApexCarSetup& Setup)
	{
		FApexCarSetup Clamped = Setup;
		Clamped.Clamp();

		FMsgPackWriter Writer(256);
		BeginDataVariant(Writer, "SetCarSetup", FApexCarSetup::KnobCount);
		for (int32 Index = 0; Index < FApexCarSetup::KnobCount; ++Index)
		{
			Writer.WriteString(ApexCarSetup::Knob(Index).Key);
			// Signed: a negative click is a negative fixint on the wire, which
			// is what serde's i8 expects; WriteUInt would send -2 as a huge u64.
			Writer.WriteInt(Clamped.GetClick(Index));
		}
		return MoveTemp(Writer.GetBuffer());
	}

	TArray<uint8> EncodeHotlapRelocate(EApexHotlapDestination Destination)
	{
		FMsgPackWriter Writer(48);
		BeginDataVariant(Writer, "HotlapRelocate", 1);
		Writer.WriteString("destination");
		Writer.WriteUInt(static_cast<uint8>(Destination));
		return MoveTemp(Writer.GetBuffer());
	}

	TArray<uint8> EncodeRequestGhost() { return EncodeUnitVariant("RequestGhost"); }

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
		else if (OutMessage.Type == EApexServerMessageType::DriverFeedback)
		{
			if (!ParseDriverFeedback(Reader, OutMessage.DriverFeedback))
			{
				OutError = Reader.HasError() ? Reader.GetError() : TEXT("failed to parse driver feedback");
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
