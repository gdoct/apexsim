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

	CheckBytes(TEXT("CreateSession"),
		ApexProtocol::EncodeCreateSession(TrackId, 8, 3, 5, EApexSessionKind::Multiplayer),
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

#endif // WITH_DEV_AUTOMATION_TESTS
