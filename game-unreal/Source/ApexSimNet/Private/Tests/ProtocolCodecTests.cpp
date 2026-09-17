#include "Misc/AutomationTest.h"

#include "ApexProtocolCodec.h"
#include "ApexProtocolTypes.h"
#include "HAL/IConsoleManager.h"
#include "Tests/ApexGoldenBlobs.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	constexpr EAutomationTestFlags ApexTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter;

	const FString CarId    = TEXT("11111111-2222-3333-4444-555555555555");
	const FString TrackId  = TEXT("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee");
	const FString SessId   = TEXT("01234567-89ab-cdef-0123-456789abcdef");
	const FString PlayerId = TEXT("deadbeef-0000-1111-2222-333344445555");

	/** Renders a mismatch as a hex diff so a failure names the offending byte. */
	FString DescribeMismatch(TArrayView<const uint8> Actual, TArrayView<const uint8> Expected)
	{
		if (Actual.Num() != Expected.Num())
		{
			return FString::Printf(TEXT("length %d, expected %d"), Actual.Num(), Expected.Num());
		}
		for (int32 i = 0; i < Actual.Num(); ++i)
		{
			if (Actual[i] != Expected[i])
			{
				return FString::Printf(
					TEXT("first difference at byte %d: got 0x%02X, expected 0x%02X"),
					i, Actual[i], Expected[i]);
			}
		}
		return TEXT("identical");
	}
}

// -----------------------------------------------------------------------------
// Encode: the client must produce exactly the bytes rmp_serde does.
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexProtocolGoldenEncodeTest,
	"ApexSim.Net.Protocol.GoldenEncode",
	ApexTestFlags)

bool FApexProtocolGoldenEncodeTest::RunTest(const FString& Parameters)
{
	auto CheckBytes = [this](const TCHAR* Label, const TArray<uint8>& Actual, TArrayView<const uint8> Expected)
	{
		const bool bMatches = Actual.Num() == Expected.Num()
			&& FMemory::Memcmp(Actual.GetData(), Expected.GetData(), Expected.Num()) == 0;
		TestTrue(
			FString::Printf(TEXT("%s encodes byte-for-byte (%s)"),
				Label, *DescribeMismatch(Actual, Expected)),
			bMatches);
	};

	CheckBytes(TEXT("Authenticate"),
		ApexProtocol::EncodeAuthenticate(TEXT("dev-token"), TEXT("Player")),
		ApexGolden::C_Authenticate);

	// The unit variants are the sharpest trap: a one-key map with no "data".
	// Emitting `"data": {}` fails from_slice on the server, and each parse
	// failure costs 10 rate-limit violations (200 => forced disconnect).
	CheckBytes(TEXT("RequestLobbyState"),
		ApexProtocol::EncodeRequestLobbyState(),
		ApexGolden::C_RequestLobbyState);

	CheckBytes(TEXT("LeaveSession"),
		ApexProtocol::EncodeLeaveSession(),
		ApexGolden::C_LeaveSession);

	CheckBytes(TEXT("Disconnect"),
		ApexProtocol::EncodeDisconnect(),
		ApexGolden::C_Disconnect);

	CheckBytes(TEXT("Heartbeat"),
		ApexProtocol::EncodeHeartbeat(300),
		ApexGolden::C_Heartbeat);

	CheckBytes(TEXT("SelectCar"),
		ApexProtocol::EncodeSelectCar(CarId),
		ApexGolden::C_SelectCar);

	FApexAllowedAssists CreateAssists;
	CreateAssists.bTractionControl = false;
	CreateAssists.bSteeringAssist = false;
	FApexSessionConditions CreateConditions;
	CreateConditions.Weather = EApexWeather::LightRain;
	CreateConditions.TimeOfDayMinutes = 21 * 60 + 30;
	CheckBytes(TEXT("CreateSession"),
		ApexProtocol::EncodeCreateSession(TrackId, 8, 3, 5, EApexSessionKind::Multiplayer, CreateAssists, CreateConditions),
		ApexGolden::C_CreateSession);

	CheckBytes(TEXT("JoinSession"),
		ApexProtocol::EncodeJoinSession(SessId),
		ApexGolden::C_JoinSession);

	CheckBytes(TEXT("SetGameMode"),
		ApexProtocol::EncodeSetGameMode(EApexGameMode::FreePractice),
		ApexGolden::C_SetGameMode);

	CheckBytes(TEXT("StartCountdown"),
		ApexProtocol::EncodeStartCountdown(10, EApexGameMode::Race),
		ApexGolden::C_StartCountdown);

	CheckBytes(TEXT("SetDriverAids"),
		ApexProtocol::EncodeSetDriverAids(true, true, false, EApexTractionControl::High),
		ApexGolden::C_SetDriverAids);

	{
		FApexCarSetup Setup;
		const int32 Clicks[] = { 1, -2, -3, 4, -5, 5, -1, 2, 3, -3, 0, 1, -4, 4 };
		for (int32 Index = 0; Index < FApexCarSetup::KnobCount; ++Index)
		{
			Setup.Clicks[Index] = Clicks[Index];
		}
		CheckBytes(TEXT("SetCarSetup"), ApexProtocol::EncodeSetCarSetup(Setup), ApexGolden::C_SetCarSetup);
	}

	CheckBytes(TEXT("HotlapRelocate"),
		ApexProtocol::EncodeHotlapRelocate(EApexHotlapDestination::Track),
		ApexGolden::C_HotlapRelocate);
	CheckBytes(TEXT("RequestGhost"), ApexProtocol::EncodeRequestGhost(), ApexGolden::C_RequestGhost);

	return true;
}

// -----------------------------------------------------------------------------
// The setup's clicks: ranges, one-sided knobs, and the read-out.
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexCarSetupClicksTest,
	"ApexSim.Net.CarSetup.Clicks",
	ApexTestFlags)

bool FApexCarSetupClicksTest::RunTest(const FString& Parameters)
{
	FApexCarSetup Setup;
	TestTrue(TEXT("a new setup is stock"), Setup.IsStock());
	TestEqual(TEXT("a new setup has every knob"), Setup.Clicks.Num(), FApexCarSetup::KnobCount);

	TestTrue(TEXT("a click within range moves the knob"), Setup.SetClick(ApexCarSetup::SpringFront, 2));
	TestFalse(TEXT("the same value again is no change"), Setup.SetClick(ApexCarSetup::SpringFront, 2));
	TestEqual(TEXT("one knob changed"), Setup.CountChanged(), 1);

	Setup.SetClick(ApexCarSetup::SpringRear, 40);
	TestEqual(TEXT("a wild click is pinned to the top of the range"), Setup.GetClick(ApexCarSetup::SpringRear), 5);
	Setup.SetClick(ApexCarSetup::RevLimiter, 3);
	TestEqual(TEXT("the rev limiter cannot be raised"), Setup.GetClick(ApexCarSetup::RevLimiter), 0);
	Setup.SetClick(ApexCarSetup::TorqueMap, -9);
	TestEqual(TEXT("the torque map bottoms out at five clicks"), Setup.GetClick(ApexCarSetup::TorqueMap), -5);

	// A slot from an older build may hold too few knobs; Clamp fills it in.
	FApexCarSetup Short;
	Short.Clicks.SetNum(3);
	Short.Clicks[0] = 99;
	Short.Clamp();
	TestEqual(TEXT("clamp restores the knob count"), Short.Clicks.Num(), FApexCarSetup::KnobCount);
	TestEqual(TEXT("clamp pins the values"), Short.GetClick(0), 5);
	TestEqual(TEXT("clamp zeroes the missing knobs"), Short.GetClick(FApexCarSetup::KnobCount - 1), 0);

	TestEqual(TEXT("stock reads as 0"), ApexCarSetup::Describe(ApexCarSetup::SpringFront, 0), FString(TEXT("0")));
	TestEqual(TEXT("clicks carry their effect"),
		ApexCarSetup::Describe(ApexCarSetup::SpringFront, 2), FString(TEXT("+2  (+8%)")));
	TestEqual(TEXT("the limiter reads in rpm"),
		ApexCarSetup::Describe(ApexCarSetup::RevLimiter, -3), FString(TEXT("-3  (-300 rpm)")));
	TestEqual(TEXT("pressures read in kPa"),
		ApexCarSetup::Describe(ApexCarSetup::TyrePressureRear, -1), FString(TEXT("-1  (-5 kPa)")));

	// The wire key table must line up with the enum it is indexed by.
	TestEqual(TEXT("first key"), FString(ApexCarSetup::Knob(ApexCarSetup::TyrePressureFront).Key), FString(TEXT("tyre_pressure_front")));
	TestEqual(TEXT("last key"), FString(ApexCarSetup::Knob(ApexCarSetup::AntiRollRear).Key), FString(TEXT("anti_roll_rear")));
	return true;
}

// -----------------------------------------------------------------------------
// Decode: every server message, against bytes the server actually produces.
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexProtocolGoldenDecodeTest,
	"ApexSim.Net.Protocol.GoldenDecode",
	ApexTestFlags)

bool FApexProtocolGoldenDecodeTest::RunTest(const FString& Parameters)
{
	auto Decode = [this](const TCHAR* Label, TArrayView<const uint8> Bytes, FApexServerMessage& Out) -> bool
	{
		FString Error;
		const bool bOk = ApexProtocol::DecodeServerMessage(Bytes, Out, Error);
		TestTrue(FString::Printf(TEXT("%s decodes (%s)"), Label, *Error), bOk);
		return bOk;
	};

	{
		FApexServerMessage Message;
		if (Decode(TEXT("AuthSuccess"), ApexGolden::S_AuthSuccess, Message))
		{
			TestEqual(TEXT("AuthSuccess type"), Message.Type, EApexServerMessageType::AuthSuccess);
			TestEqual(TEXT("AuthSuccess.PlayerId"), Message.AuthSuccess.PlayerId, PlayerId);
			TestEqual(TEXT("AuthSuccess.ServerVersion"), Message.AuthSuccess.ServerVersion, static_cast<int64>(1));
			TestEqual(TEXT("AuthSuccess.ProtocolVersion"), Message.AuthSuccess.ProtocolVersion, 2);
			TestEqual(TEXT("AuthSuccess.UdpToken"), Message.AuthSuccess.UdpToken, FString(TEXT("udp-tok")));
			TestEqual(TEXT("AuthSuccess.UdpPort"), Message.AuthSuccess.UdpPort, 9001);
		}
	}

	{
		FApexServerMessage Message;
		if (Decode(TEXT("AuthFailure"), ApexGolden::S_AuthFailure, Message))
		{
			TestEqual(TEXT("AuthFailure type"), Message.Type, EApexServerMessageType::AuthFailure);
			TestEqual(TEXT("AuthFailure.reason"), Message.Reason, FString(TEXT("protocol version mismatch")));
		}
	}

	{
		// A unit variant: a one-key envelope with no "data" at all.
		FApexServerMessage Message;
		if (Decode(TEXT("SessionLeft"), ApexGolden::S_SessionLeft, Message))
		{
			TestEqual(TEXT("SessionLeft type"), Message.Type, EApexServerMessageType::SessionLeft);
		}
	}

	{
		FApexServerMessage Message;
		if (Decode(TEXT("SessionJoined"), ApexGolden::S_SessionJoined, Message))
		{
			TestEqual(TEXT("SessionJoined type"), Message.Type, EApexServerMessageType::SessionJoined);
			TestEqual(TEXT("SessionJoined.SessionId"), Message.SessionId, SessId);
			TestEqual(TEXT("SessionJoined.YourGridPosition"), Message.GridPosition, 3);
			TestTrue(TEXT("SessionJoined from an older server allows every assist"),
				Message.AllowedAssists.AllowsEverything());
		}
	}

	{
		FApexServerMessage Message;
		if (Decode(TEXT("SessionJoined with assists"), ApexGolden::S_SessionJoinedAssists, Message))
		{
			TestEqual(TEXT("SessionJoinedAssists type"), Message.Type, EApexServerMessageType::SessionJoined);
			TestEqual(TEXT("SessionJoinedAssists.SessionId"), Message.SessionId, SessId);
			TestEqual(TEXT("SessionJoinedAssists.YourGridPosition"), Message.GridPosition, 3);
			TestEqual(TEXT("SessionJoinedAssists.SessionKind"), Message.SessionKind, EApexSessionKind::Practice);
			// The snake_case keys under the PascalCase parent: get one wrong and
			// the field silently keeps its default (true), which is why each is
			// pinned, and the false ones in particular.
			TestFalse(TEXT("AllowedAssists.abs"), Message.AllowedAssists.bAbs);
			TestTrue(TEXT("AllowedAssists.traction_control"), Message.AllowedAssists.bTractionControl);
			TestFalse(TEXT("AllowedAssists.auto_gearbox"), Message.AllowedAssists.bAutoGearbox);
			TestTrue(TEXT("AllowedAssists.steering_assist"), Message.AllowedAssists.bSteeringAssist);
			TestFalse(TEXT("AllowedAssists.racing_line"), Message.AllowedAssists.bRacingLine);
			TestEqual(TEXT("AllowedAssists.CountLocked"), Message.AllowedAssists.CountLocked(), 3);
			TestEqual(TEXT("Conditions.weather"), Message.Conditions.Weather, EApexWeather::HeavyRain);
			TestEqual(TEXT("Conditions.time_of_day_minutes"), Message.Conditions.TimeOfDayMinutes, 6 * 60 + 15);
			TestEqual(TEXT("Conditions.Describe"), Message.Conditions.Describe(), FString(TEXT("Heavy rain · 06:15")));
		}
	}

	{
		// A message from a server without the field is a sunny afternoon.
		FApexServerMessage Message;
		if (Decode(TEXT("SessionJoined without conditions"), ApexGolden::S_SessionJoined, Message))
		{
			TestTrue(TEXT("Conditions default"), Message.Conditions.IsDefault());
			TestEqual(TEXT("Conditions default clock"), Message.Conditions.ClockText(), FString(TEXT("13:00")));
		}
	}

	{
		FApexServerMessage Message;
		if (Decode(TEXT("Error"), ApexGolden::S_Error, Message))
		{
			TestEqual(TEXT("Error type"), Message.Type, EApexServerMessageType::Error);
			TestEqual(TEXT("Error.code"), Message.ErrorCode, 404);
			TestEqual(TEXT("Error.message"), Message.Reason, FString(TEXT("session not found")));
		}
	}

	{
		FApexServerMessage Message;
		if (Decode(TEXT("HeartbeatAck"), ApexGolden::S_HeartbeatAck, Message))
		{
			TestEqual(TEXT("HeartbeatAck type"), Message.Type, EApexServerMessageType::HeartbeatAck);
			TestEqual(TEXT("HeartbeatAck.server_tick"), Message.ServerTick, static_cast<int64>(4242));
		}
	}

	{
		FApexServerMessage Message;
		if (Decode(TEXT("GameModeChanged"), ApexGolden::S_GameModeChanged, Message))
		{
			TestEqual(TEXT("GameModeChanged type"), Message.Type, EApexServerMessageType::GameModeChanged);
			// Serialize_repr means this arrives as the integer 2, not "Countdown".
			TestEqual(TEXT("GameModeChanged.mode"), Message.GameMode, EApexGameMode::Countdown);
		}
	}

	{
		FApexServerMessage Message;
		if (Decode(TEXT("CountdownUpdate"), ApexGolden::S_CountdownUpdate, Message))
		{
			TestEqual(TEXT("CountdownUpdate type"), Message.Type, EApexServerMessageType::CountdownUpdate);
			TestEqual(TEXT("CountdownUpdate.seconds_remaining"), Message.CountdownSeconds, 7);
		}
	}

	{
		FApexServerMessage Message;
		if (Decode(TEXT("PlayerDisconnected"), ApexGolden::S_PlayerDisconnected, Message))
		{
			TestEqual(TEXT("PlayerDisconnected type"), Message.Type, EApexServerMessageType::PlayerDisconnected);
			TestEqual(TEXT("PlayerDisconnected.PlayerId"), Message.PlayerId, PlayerId);
		}
	}

	return true;
}

// -----------------------------------------------------------------------------
// LobbyState gets its own test: it is the message the whole UI is built on,
// and the one where a PascalCase slip degrades silently into empty lists.
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexProtocolLobbyStateDecodeTest,
	"ApexSim.Net.Protocol.LobbyStateDecode",
	ApexTestFlags)

bool FApexProtocolLobbyStateDecodeTest::RunTest(const FString& Parameters)
{
	FApexServerMessage Message;
	FString Error;
	if (!TestTrue(
			FString::Printf(TEXT("LobbyState decodes (%s)"), *Error),
			ApexProtocol::DecodeServerMessage(ApexGolden::S_LobbyState, Message, Error)))
	{
		return false;
	}

	TestEqual(TEXT("LobbyState type"), Message.Type, EApexServerMessageType::LobbyState);

	const FApexLobbyState& Lobby = Message.LobbyState;

	// Non-empty arrays are the actual assertion: a mis-cased key yields an
	// empty list with no error, which is exactly the failure this pins down.
	if (TestEqual(TEXT("one player in lobby"), Lobby.PlayersInLobby.Num(), 1))
	{
		const FApexLobbyPlayer& Player = Lobby.PlayersInLobby[0];
		TestEqual(TEXT("player Id"), Player.Id, FString(TEXT("deadbeef-0000-1111-2222-333344445555")));
		// UTF-8 through a fixstr whose byte length exceeds its character count.
		TestEqual(TEXT("player Name decodes UTF-8"), Player.Name, FString(TEXT("Nürburgring Nils")));
		TestEqual(TEXT("player SelectedCar"), Player.SelectedCar, CarId);
		// InSession is None on the wire -> nil -> empty string, not a failure.
		TestTrue(TEXT("player InSession is empty for a nil"), Player.InSession.IsEmpty());
		TestTrue(TEXT("player has selected a car"), Player.HasSelectedCar());
	}

	if (TestEqual(TEXT("one available session"), Lobby.AvailableSessions.Num(), 1))
	{
		const FApexSessionSummary& Session = Lobby.AvailableSessions[0];
		TestEqual(TEXT("session Id"), Session.Id, SessId);
		TestEqual(TEXT("session TrackName decodes UTF-8"), Session.TrackName, FString(TEXT("São Paulo")));
		TestEqual(TEXT("session TrackFile"), Session.TrackFile, FString(TEXT("tracks/real/SaoPaulo.yaml")));
		TestEqual(TEXT("session HostName"), Session.HostName, FString(TEXT("Player")));
		// Both of these are u8 integers on the wire, never strings.
		TestEqual(TEXT("session SessionKind"), Session.SessionKind, EApexSessionKind::Practice);
		TestEqual(TEXT("session State"), Session.State, EApexSessionState::Lobby);
		TestEqual(TEXT("session PlayerCount"), Session.PlayerCount, 1);
		TestEqual(TEXT("session MaxPlayers"), Session.MaxPlayers, 8);
		TestTrue(TEXT("session is joinable"), Session.IsJoinable());
	}

	if (TestEqual(TEXT("one car config"), Lobby.CarConfigs.Num(), 1))
	{
		const FApexCarConfigSummary& Car = Lobby.CarConfigs[0];
		TestEqual(TEXT("car Id"), Car.Id, CarId);
		TestEqual(TEXT("car Name"), Car.Name, FString(TEXT("Red Horse RB20")));
		TestEqual(TEXT("car MassKg"), Car.MassKg, 798.0f);
		TestEqual(TEXT("car MaxEngineForceN"), Car.MaxEngineForceN, 12000.5f);
	}

	if (TestEqual(TEXT("one track config"), Lobby.TrackConfigs.Num(), 1))
	{
		const FApexTrackConfigSummary& Track = Lobby.TrackConfigs[0];
		TestEqual(TEXT("track Id"), Track.Id, TrackId);
		TestEqual(TEXT("track Name"), Track.Name, FString(TEXT("Circuit of The Americas")));
		// Centerline parsing is off by default, so it must be skipped cleanly
		// rather than partially consumed.
		TestEqual(TEXT("centerline is skipped by default"), Track.Centerline.Num(), 0);
	}

	// Turn centerline parsing on and confirm the lowercase x/y keys work — the
	// one struct in the payload that is NOT PascalCase.
	{
		IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(TEXT("apexsim.net.ParseCenterline"));
		if (TestNotNull(TEXT("apexsim.net.ParseCenterline exists"), CVar))
		{
			const int32 Previous = CVar->GetInt();
			CVar->Set(1, ECVF_SetByCode);

			FApexServerMessage WithCenterline;
			FString CenterlineError;
			if (TestTrue(
					FString::Printf(TEXT("LobbyState decodes with centerline on (%s)"), *CenterlineError),
					ApexProtocol::DecodeServerMessage(ApexGolden::S_LobbyState, WithCenterline, CenterlineError)))
			{
				if (TestEqual(TEXT("track configs still present"), WithCenterline.LobbyState.TrackConfigs.Num(), 1))
				{
					const TArray<FVector2D>& Points = WithCenterline.LobbyState.TrackConfigs[0].Centerline;
					if (TestEqual(TEXT("two centerline points"), Points.Num(), 2))
					{
						TestEqual(TEXT("point 0 x (lowercase key)"), Points[0].X, 1.5);
						TestEqual(TEXT("point 0 y (lowercase key)"), Points[0].Y, -2.25);
						TestEqual(TEXT("point 1 x"), Points[1].X, 3.0);
						TestEqual(TEXT("point 1 y"), Points[1].Y, 4.0);
					}
				}
			}

			CVar->Set(Previous, ECVF_SetByCode);
		}
	}

	return true;
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexProtocolRobustnessTest,
	"ApexSim.Net.Protocol.Robustness",
	ApexTestFlags)

bool FApexProtocolRobustnessTest::RunTest(const FString& Parameters)
{
	// Every truncation of the largest real message must be rejected, never
	// crash, and never half-populate.
	{
		TArrayView<const uint8> Full(ApexGolden::S_LobbyState);
		for (int32 PrefixLength = 0; PrefixLength < Full.Num(); ++PrefixLength)
		{
			FApexServerMessage Message;
			FString Error;
			const bool bDecoded = ApexProtocol::DecodeServerMessage(
				TArrayView<const uint8>(Full.GetData(), PrefixLength), Message, Error);
			TestFalse(
				FString::Printf(TEXT("a %d byte prefix of LobbyState is rejected"), PrefixLength),
				bDecoded);
		}
	}

	// An unknown variant must decode successfully as Unknown — a newer server
	// adding a message must not break an older client.
	{
		// {"type": "SomeFutureMessage", "data": {"whatever": 1}}
		const uint8 FutureMessage[] = {
			0x82,
			0xA4, 't', 'y', 'p', 'e',
			0xB1, 'S', 'o', 'm', 'e', 'F', 'u', 't', 'u', 'r', 'e', 'M', 'e', 's', 's', 'a', 'g', 'e',
			0xA4, 'd', 'a', 't', 'a',
			0x81, 0xA8, 'w', 'h', 'a', 't', 'e', 'v', 'e', 'r', 0x01
		};
		FApexServerMessage Message;
		FString Error;
		TestTrue(
			FString::Printf(TEXT("an unknown variant decodes without error (%s)"), *Error),
			ApexProtocol::DecodeServerMessage(
				TArrayView<const uint8>(FutureMessage, UE_ARRAY_COUNT(FutureMessage)), Message, Error));
		TestEqual(TEXT("an unknown variant is reported as Unknown"), Message.Type, EApexServerMessageType::Unknown);
		TestEqual(TEXT("the unknown variant name is preserved"), Message.VariantName, FString(TEXT("SomeFutureMessage")));
	}

	// A known variant carrying an unknown extra field must still decode: this
	// is what makes the client forward-compatible with server-side additions.
	{
		// {"type": "SessionJoined", "data": {"SessionId": "...", "YourGridPosition": 3, "NewField": true}}
		const uint8 ExtraField[] = {
			0x82,
			0xA4, 't', 'y', 'p', 'e',
			0xAD, 'S', 'e', 's', 's', 'i', 'o', 'n', 'J', 'o', 'i', 'n', 'e', 'd',
			0xA4, 'd', 'a', 't', 'a',
			0x83,
			0xA9, 'S', 'e', 's', 's', 'i', 'o', 'n', 'I', 'd',
			0xA4, 'a', 'b', 'c', 'd',
			0xB0, 'Y', 'o', 'u', 'r', 'G', 'r', 'i', 'd', 'P', 'o', 's', 'i', 't', 'i', 'o', 'n', 0x03,
			0xA8, 'N', 'e', 'w', 'F', 'i', 'e', 'l', 'd', 0xC3
		};
		FApexServerMessage Message;
		FString Error;
		TestTrue(
			FString::Printf(TEXT("an unknown field is skipped (%s)"), *Error),
			ApexProtocol::DecodeServerMessage(
				TArrayView<const uint8>(ExtraField, UE_ARRAY_COUNT(ExtraField)), Message, Error));
		TestEqual(TEXT("the known variant still resolves"), Message.Type, EApexServerMessageType::SessionJoined);
		TestEqual(TEXT("known fields survive an unknown neighbour"), Message.SessionId, FString(TEXT("abcd")));
		TestEqual(TEXT("grid position survives an unknown neighbour"), Message.GridPosition, 3);
		TestEqual(TEXT("a join without a kind is an ordinary session"), Message.SessionKind, EApexSessionKind::Multiplayer);
	}

	// The menu's demo session is told apart by the kind on the join itself.
	{
		// {"type": "SessionJoined", "data": {"SessionId": "abcd", "YourGridPosition": 0, "SessionKind": 3}}
		const uint8 DemoJoin[] = {
			0x82,
			0xA4, 't', 'y', 'p', 'e',
			0xAD, 'S', 'e', 's', 's', 'i', 'o', 'n', 'J', 'o', 'i', 'n', 'e', 'd',
			0xA4, 'd', 'a', 't', 'a',
			0x83,
			0xA9, 'S', 'e', 's', 's', 'i', 'o', 'n', 'I', 'd',
			0xA4, 'a', 'b', 'c', 'd',
			0xB0, 'Y', 'o', 'u', 'r', 'G', 'r', 'i', 'd', 'P', 'o', 's', 'i', 't', 'i', 'o', 'n', 0x00,
			0xAB, 'S', 'e', 's', 's', 'i', 'o', 'n', 'K', 'i', 'n', 'd', 0x03
		};
		FApexServerMessage Message;
		FString Error;
		TestTrue(
			FString::Printf(TEXT("a demo join decodes (%s)"), *Error),
			ApexProtocol::DecodeServerMessage(
				TArrayView<const uint8>(DemoJoin, UE_ARRAY_COUNT(DemoJoin)), Message, Error));
		TestEqual(TEXT("the join says it is a demo"), Message.SessionKind, EApexSessionKind::Demo);
	}

	// Garbage must be rejected rather than misread.
	{
		const uint8 Garbage[] = {0xC1, 0xC1, 0xC1};
		FApexServerMessage Message;
		FString Error;
		TestFalse(TEXT("garbage is rejected"),
			ApexProtocol::DecodeServerMessage(
				TArrayView<const uint8>(Garbage, UE_ARRAY_COUNT(Garbage)), Message, Error));
	}

	// An empty payload is rejected.
	{
		FApexServerMessage Message;
		FString Error;
		TestFalse(TEXT("an empty payload is rejected"),
			ApexProtocol::DecodeServerMessage(TArrayView<const uint8>(), Message, Error));
	}

	return true;
}

// -----------------------------------------------------------------------------
// RacingLine: parallel X/Y/Z/Phase arrays zipped into points.
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexProtocolRacingLineDecodeTest,
	"ApexSim.Net.Protocol.RacingLineDecode",
	ApexTestFlags)

bool FApexProtocolRacingLineDecodeTest::RunTest(const FString& Parameters)
{
	FApexServerMessage Message;
	FString Error;
	if (!TestTrue(FString::Printf(TEXT("RacingLine decodes (%s)"), *Error),
			ApexProtocol::DecodeServerMessage(ApexGolden::S_RacingLine, Message, Error)))
	{
		return false;
	}

	TestEqual(TEXT("RacingLine type"), Message.Type, EApexServerMessageType::RacingLine);
	const FApexRacingLineData& Line = Message.RacingLine;
	TestEqual(TEXT("session id"), Line.SessionId, SessId);
	TestEqual(TEXT("spacing"), Line.SpacingM, 2.5f);
	if (TestEqual(TEXT("two points"), Line.Points.Num(), 2) && TestEqual(TEXT("two phases"), Line.Phases.Num(), 2))
	{
		TestEqual(TEXT("point 0"), Line.Points[0], FVector(1.5, 0.25, 0.0));
		TestEqual(TEXT("point 1"), Line.Points[1], FVector(-2.0, 3.0, 1.0));
		TestEqual(TEXT("phase 0 is throttle"), Line.Phases[0], EApexLinePhase::Throttle);
		TestEqual(TEXT("phase 1 is brake"), Line.Phases[1], EApexLinePhase::Brake);
	}
	return true;
}


// -----------------------------------------------------------------------------
// Lap timing: the sector lines, the splits and the records.
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexProtocolLapTimingDecodeTest,
	"ApexSim.Net.Protocol.LapTimingDecode",
	ApexTestFlags)

bool FApexProtocolLapTimingDecodeTest::RunTest(const FString& Parameters)
{
	FString Error;

	FApexServerMessage SectorsMessage;
	if (TestTrue(TEXT("TrackSectors decodes"),
			ApexProtocol::DecodeServerMessage(ApexGolden::S_TrackSectors, SectorsMessage, Error)))
	{
		TestEqual(TEXT("TrackSectors type"), SectorsMessage.Type, EApexServerMessageType::TrackSectors);
		const FApexTrackSectors& Sectors = SectorsMessage.TrackSectors;
		TestEqual(TEXT("session id"), Sectors.SessionId, SessId);
		TestEqual(TEXT("track length"), Sectors.TrackLengthM, 5793.0f);
		TestTrue(TEXT("sectors are usable"), Sectors.IsValid());
		if (TestEqual(TEXT("two boundaries"), Sectors.BoundariesM.Num(), 2))
		{
			TestEqual(TEXT("boundary 1"), Sectors.BoundariesM[0], 1931.0f);
			TestEqual(TEXT("boundary 2"), Sectors.BoundariesM[1], 3862.0f);
		}
		TestEqual(TEXT("three sectors"), Sectors.SectorCount(), 3);
		// Sector 1 starts at the line, and the boundary itself belongs to the
		// sector it opens.
		TestEqual(TEXT("at the line"), Sectors.SectorAt(0.0f), 0);
		TestEqual(TEXT("just before the first line"), Sectors.SectorAt(1930.0f), 0);
		TestEqual(TEXT("on the first line"), Sectors.SectorAt(1931.0f), 1);
		TestEqual(TEXT("past the second"), Sectors.SectorAt(5000.0f), 2);
	}

	FApexServerMessage TimingMessage;
	if (TestTrue(TEXT("LapTiming decodes"),
			ApexProtocol::DecodeServerMessage(ApexGolden::S_LapTiming, TimingMessage, Error)))
	{
		TestEqual(TEXT("LapTiming type"), TimingMessage.Type, EApexServerMessageType::LapTiming);
		const FApexLapTiming& Timing = TimingMessage.LapTiming;
		TestEqual(TEXT("car index"), Timing.CarIndex, 2);
		TestEqual(TEXT("lap"), Timing.Lap, 4);
		TestEqual(TEXT("sector"), Timing.Sector, 2);
		TestEqual(TEXT("sector time"), Timing.SectorTimeMs, 27431);
		TestEqual(TEXT("lap time"), Timing.LapTimeMs, 82615);
		TestTrue(TEXT("closes the lap"), Timing.bIsLapEnd);
		TestTrue(TEXT("legal"), Timing.bValid);
		// Flags 0b1001: the driver's best lap and the session's best sector.
		TestTrue(TEXT("personal best lap"), Timing.bPersonalBestLap);
		TestFalse(TEXT("not the session's best lap"), Timing.bSessionBestLap);
		TestFalse(TEXT("not a personal best sector"), Timing.bPersonalBestSector);
		TestTrue(TEXT("session best sector"), Timing.bSessionBestSector);
	}

	FApexServerMessage RecordMessage;
	if (TestTrue(TEXT("LapRecord decodes"),
			ApexProtocol::DecodeServerMessage(ApexGolden::S_LapRecord, RecordMessage, Error)))
	{
		TestEqual(TEXT("LapRecord type"), RecordMessage.Type, EApexServerMessageType::LapRecord);
		const FApexLapRecord& Record = RecordMessage.LapRecord;
		TestEqual(TEXT("player"), Record.PlayerName, FString(TEXT("Ayrton")));
		TestEqual(TEXT("track id"), Record.TrackId, TrackId);
		TestEqual(TEXT("car id"), Record.CarConfigId, CarId);
		TestEqual(TEXT("lap time"), Record.LapTimeMs, 82615);
		TestTrue(TEXT("new record"), Record.bIsNew);
		TestTrue(TEXT("has a ghost"), Record.bHasGhost);
		TestEqual(TEXT("track record"), Record.TrackRecordMs, 81900);
		TestEqual(TEXT("track record holder"), Record.TrackRecordHolder, FString(TEXT("Alain")));
		if (TestEqual(TEXT("three splits"), Record.SplitsMs.Num(), 3))
		{
			int32 Total = 0;
			for (int32 Split : Record.SplitsMs)
			{
				Total += Split;
			}
			TestEqual(TEXT("splits add up to the lap"), Total, Record.LapTimeMs);
		}
	}

	return true;
}

/**
 * The timing board is the client's whole memory of a session's laps, so the
 * bests it files have to survive the order the messages arrive in.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexTimingBoardTest,
	"ApexSim.Net.Protocol.TimingBoard",
	ApexTestFlags)

bool FApexTimingBoardTest::RunTest(const FString& Parameters)
{
	FApexTimingBoard Board;
	Board.Reset(3);

	auto Cross = [](int32 Car, int32 Lap, int32 Sector, int32 SectorMs, int32 LapMs, bool bValid)
	{
		FApexLapTiming Timing;
		Timing.CarIndex = Car;
		Timing.Lap = Lap;
		Timing.Sector = Sector;
		Timing.SectorTimeMs = SectorMs;
		Timing.LapTimeMs = LapMs;
		Timing.bIsLapEnd = LapMs > 0;
		Timing.bValid = bValid;
		return Timing;
	};

	// Car 0 sets a clean 1:22.615 — its first lap, so every part of it is a
	// personal best, and with nobody else out, the session's too.
	for (int32 Sector = 0; Sector < 3; ++Sector)
	{
		const int32 Splits[3] = { 27100, 28084, 27431 };
		FApexLapTiming Timing = Cross(0, 1, Sector, Splits[Sector], Sector == 2 ? 82615 : 0, true);
		Timing.bPersonalBestSector = true;
		Timing.bSessionBestSector = true;
		Timing.bPersonalBestLap = Sector == 2;
		Timing.bSessionBestLap = Sector == 2;
		Board.Apply(Timing);
	}

	const FApexCarTiming* Car = Board.Find(0);
	if (!TestNotNull(TEXT("car 0 is on the board"), Car))
	{
		return false;
	}
	TestEqual(TEXT("best lap"), Car->BestLapMs, 82615);
	TestEqual(TEXT("last lap"), Car->LastLapMs, 82615);
	TestTrue(TEXT("last lap counted"), Car->bLastLapValid);
	TestEqual(TEXT("session best lap"), Board.SessionBestLapMs, 82615);
	TestEqual(TEXT("session best holder"), Board.SessionBestLapCarIndex, 0);
	TestEqual(TEXT("optimal lap is the three bests"), Car->OptimalLapMs(), 82615);
	TestEqual(TEXT("the lap in progress is empty again"), Car->CurrentSplitsMs[0], 0);

	// A struck lap: the server sends no best flags, so nothing on the board
	// moves however quick the time was.
	Board.Apply(Cross(0, 2, 0, 20000, 0, false));
	Board.Apply(Cross(0, 2, 1, 20000, 0, false));
	Board.Apply(Cross(0, 2, 2, 20000, 60000, false));
	Car = Board.Find(0);
	TestEqual(TEXT("best lap unchanged by a struck lap"), Car->BestLapMs, 82615);
	TestEqual(TEXT("last lap is still recorded"), Car->LastLapMs, 60000);
	TestFalse(TEXT("last lap did not count"), Car->bLastLapValid);
	TestEqual(TEXT("best sector unchanged"), Car->BestSplitsMs[0], 27100);

	// A second car takes a sector but not the lap.
	FApexLapTiming Purple = Cross(1, 1, 0, 26500, 0, true);
	Purple.bPersonalBestSector = true;
	Purple.bSessionBestSector = true;
	Board.Apply(Purple);
	TestEqual(TEXT("session best sector 1"), Board.SessionBestSplitsMs[0], 26500);
	TestEqual(TEXT("session best lap still car 0"), Board.SessionBestLapCarIndex, 0);

	// And a board that never heard the sector count still files a crossing.
	FApexTimingBoard Fresh;
	Fresh.Apply(Cross(3, 1, 1, 30000, 0, true));
	TestEqual(TEXT("sector count grows to fit"), Fresh.SectorCount, 2);
	TestNotNull(TEXT("car filed"), Fresh.Find(3));
	return true;
}

// -----------------------------------------------------------------------------
// The ghost lap: struct-of-arrays on the wire, one sample per moment here,
// and a pose blended between samples for a puppet car.
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexProtocolGhostLapTest,
	"ApexSim.Net.Protocol.GhostLap",
	ApexTestFlags)

bool FApexProtocolGhostLapTest::RunTest(const FString& Parameters)
{
	FString Error;
	FApexServerMessage Message;
	if (!TestTrue(FString::Printf(TEXT("GhostLap decodes (%s)"), *Error),
			ApexProtocol::DecodeServerMessage(ApexGolden::S_GhostLap, Message, Error)))
	{
		return false;
	}
	TestEqual(TEXT("GhostLap type"), Message.Type, EApexServerMessageType::GhostLap);
	const FApexGhostLap& Lap = Message.GhostLap;
	TestEqual(TEXT("track id"), Lap.TrackId, TrackId);
	TestEqual(TEXT("car id"), Lap.CarConfigId, CarId);
	TestEqual(TEXT("lap time"), Lap.LapTimeMs, 82615);
	TestEqual(TEXT("sample rate"), Lap.SampleHz, 20.0f);
	TestTrue(TEXT("a lap with two samples is playable"), Lap.IsValid());
	if (!TestEqual(TEXT("two samples"), Lap.Samples.Num(), 2))
	{
		return false;
	}
	TestEqual(TEXT("t0"), Lap.Samples[0].TimeMs, 0);
	TestEqual(TEXT("t1"), Lap.Samples[1].TimeMs, 50);
	TestEqual(TEXT("position 0"), Lap.Samples[0].Position, FVector(1.5, -2.0, 0.25));
	TestEqual(TEXT("position 1"), Lap.Samples[1].Position, FVector(4.5, -2.5, 0.25));
	TestEqual(TEXT("roll 0"), Lap.Samples[0].RollRad, -0.125f);
	TestEqual(TEXT("speed 1"), Lap.Samples[1].SpeedMps, 61.0f);
	TestEqual(TEXT("steering 1"), Lap.Samples[1].Steering, -0.2f);
	TestEqual(TEXT("gear 0"), Lap.Samples[0].Gear, 4);
	TestEqual(TEXT("a negative gear is signed on the wire"), Lap.Samples[1].Gear, -1);
	TestEqual(TEXT("rpm 1"), Lap.Samples[1].EngineRpm, 9100.0f);

	// Halfway between the samples: position and speed blend, the gear steps.
	FApexGhostSample Mid;
	TestTrue(TEXT("mid-lap sample is inside the lap"), Lap.SampleAt(25.0f, Mid));
	TestEqual(TEXT("blended position"), Mid.Position, FVector(3.0, -2.25, 0.25));
	TestEqual(TEXT("blended speed"), Mid.SpeedMps, 60.5f);
	TestEqual(TEXT("blended roll"), Mid.RollRad, -0.0625f);
	TestEqual(TEXT("gear steps at the second sample"), Mid.Gear, -1);
	FApexGhostSample Before;
	TestTrue(TEXT("before the line is the first sample"), Lap.SampleAt(-10.0f, Before));
	TestEqual(TEXT("first sample"), Before.Position, Lap.Samples[0].Position);
	FApexGhostSample After;
	TestFalse(TEXT("past the last sample the lap is over"), Lap.SampleAt(1000.0f, After));
	TestEqual(TEXT("held at the last sample"), After.Position, Lap.Samples[1].Position);

	// The yaw blends the short way round the seam.
	FApexGhostLap Seam;
	Seam.LapTimeMs = 100;
	FApexGhostSample A;
	A.TimeMs = 0;
	A.YawRad = 3.0f;
	FApexGhostSample B;
	B.TimeMs = 100;
	B.YawRad = -3.0f;
	Seam.Samples = { A, B };
	FApexGhostSample AtSeam;
	Seam.SampleAt(50.0f, AtSeam);
	TestTrue(TEXT("yaw crosses the seam, not the long way"), FMath::Abs(FMath::Abs(AtSeam.YawRad) - PI) < 0.01f);

	TestFalse(TEXT("an empty reply is not a lap"), FApexGhostLap().IsValid());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
