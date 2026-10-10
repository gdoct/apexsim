#include "Misc/AutomationTest.h"

#include "ApexErs.h"
#include "ApexProtocolCodec.h"
#include "ApexProtocolTypes.h"
#include "Tests/ApexUdpGoldenBlobs.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	constexpr EAutomationTestFlags ApexUdpTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter;

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
				return FString::Printf(TEXT("first difference at byte %d: got 0x%02X, expected 0x%02X"),
					i, Actual[i], Expected[i]);
			}
		}
		return TEXT("identical");
	}
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexUdpGoldenEncodeTest,
	"ApexSim.Net.Udp.GoldenEncode",
	ApexUdpTestFlags)

bool FApexUdpGoldenEncodeTest::RunTest(const FString& Parameters)
{
	auto CheckBytes = [this](const TCHAR* Label, const TArray<uint8>& Actual, TArrayView<const uint8> Expected)
	{
		const bool bMatches = Actual.Num() == Expected.Num()
			&& FMemory::Memcmp(Actual.GetData(), Expected.GetData(), Expected.Num()) == 0;
		TestTrue(FString::Printf(TEXT("%s encodes byte-for-byte (%s)"),
			Label, *DescribeMismatch(Actual, Expected)), bMatches);
	};

	CheckBytes(TEXT("UdpHandshake"),
		ApexProtocol::EncodeUdpHandshake(TEXT("udp-tok")),
		ApexUdpGolden::C_UdpHandshake);

	// The seal around every datagram we send (server: `cargo test
	// udp_seal_wire_format`): marker, sequence number 7, 16 bytes of
	// HMAC-SHA1 under "udp-key", then the handshake above.
	{
		const TArray<uint8> Sealed = ApexProtocol::SealUdpDatagram(
			TEXT("udp-key"), 7, ApexProtocol::EncodeUdpHandshake(TEXT("udp-tok")));
		CheckBytes(TEXT("SealedUdpHandshake"), Sealed, ApexUdpGolden::C_SealedUdpHandshake);
		if (Sealed.Num() > 25)
		{
			TestEqual(TEXT("sealed datagrams start with 0xC1"), Sealed[0], (uint8)0xC1);
			TestEqual(TEXT("the sequence number is big-endian"), Sealed[8], (uint8)7);
		}
		// Another sequence number or key changes the tag and nothing else.
		const TArray<uint8> Next = ApexProtocol::SealUdpDatagram(
			TEXT("udp-key"), 8, ApexProtocol::EncodeUdpHandshake(TEXT("udp-tok")));
		TestEqual(TEXT("the seal is the same length for every sequence number"), Next.Num(), Sealed.Num());
		TestNotEqual(TEXT("a new sequence number gets a new tag"),
			FMemory::Memcmp(Next.GetData() + 9, Sealed.GetData() + 9, 16), 0);
		const TArray<uint8> Other = ApexProtocol::SealUdpDatagram(
			TEXT("other-key"), 7, ApexProtocol::EncodeUdpHandshake(TEXT("udp-tok")));
		TestNotEqual(TEXT("another key gets another tag"),
			FMemory::Memcmp(Other.GetData() + 9, Sealed.GetData() + 9, 16), 0);
	}

	FApexPlayerInput Input;
	Input.Throttle = 1.0f;
	Input.Brake = 0.0f;
	Input.Steering = -0.5f;
	Input.Gear = 4;
	Input.bDrs = false;
	Input.Headlights = -1;
	Input.bFlash = false;
	Input.ErsMode = 1;
	Input.bErsBoost = false;
	CheckBytes(TEXT("PlayerInput"),
		ApexProtocol::EncodePlayerInput(4242, Input),
		ApexUdpGolden::C_PlayerInput);

	// Every bool, the nil switch and the mode are one byte, so a held button
	// or a set switch is the same message with that byte changed (server:
	// `cargo test player_input_headlights_wire_format`). From the end: the
	// overtake button, `a9 "ers_boost"`, the mode, `a8 "ers_mode"`, flash,
	// `a5 "flash"`, the switch, `aa "headlights"`, drs.
	{
		const int32 Len = (int32)UE_ARRAY_COUNT(ApexUdpGolden::C_PlayerInput);
		const int32 BoostAt = Len - 1;
		const int32 ModeAt = BoostAt - 11;
		const int32 FlashAt = ModeAt - 10;
		const int32 SwitchAt = FlashAt - 7;
		const int32 DrsAt = SwitchAt - 12;
		TestEqual(TEXT("the switch left to the sky is nil"), ApexUdpGolden::C_PlayerInput[SwitchAt], (uint8)0xC0);

		Input.bDrs = true;
		Input.Headlights = 0;
		Input.bFlash = true;
		Input.ErsMode = 2;
		Input.bErsBoost = true;
		const TArray<uint8> Held = ApexProtocol::EncodePlayerInput(4242, Input);
		if (TestEqual(TEXT("PlayerInput with every button set is the same length"), Held.Num(), Len))
		{
			TestEqual(TEXT("DRS held is true"), Held[DrsAt], (uint8)0xC3);
			TestEqual(TEXT("headlights switched off is false"), Held[SwitchAt], (uint8)0xC2);
			TestEqual(TEXT("flash held is true"), Held[FlashAt], (uint8)0xC3);
			TestEqual(TEXT("attack is 2"), Held[ModeAt], (uint8)0x02);
			TestEqual(TEXT("overtake held is true"), Held[BoostAt], (uint8)0xC3);
		}
		Input.ErsMode = -1;
		TestEqual(TEXT("the car's own mode is nil"),
			ApexProtocol::EncodePlayerInput(4242, Input)[ModeAt], (uint8)0xC0);
		Input.Headlights = 1;
		TestEqual(TEXT("headlights switched on is true"),
			ApexProtocol::EncodePlayerInput(4242, Input)[SwitchAt], (uint8)0xC3);
	}

	return true;
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexUdpGoldenDecodeTest,
	"ApexSim.Net.Udp.GoldenDecode",
	ApexUdpTestFlags)

bool FApexUdpGoldenDecodeTest::RunTest(const FString& Parameters)
{
	// The ack arrives named even though it comes over UDP, so the dispatcher has
	// to cope with both encodings on the same socket.
	{
		FApexServerMessage Message;
		FString Error;
		TestTrue(FString::Printf(TEXT("UdpHandshakeAck decodes (%s)"), *Error),
			ApexProtocol::DecodeUdpMessage(ApexUdpGolden::S_UdpHandshakeAck, Message, Error));
		TestEqual(TEXT("UdpHandshakeAck type"), Message.Type, EApexServerMessageType::UdpHandshakeAck);
	}

	{
		FApexServerMessage Message;
		FString Error;
		if (TestTrue(FString::Printf(TEXT("SessionRoster decodes (%s)"), *Error),
				ApexProtocol::DecodeServerMessage(ApexUdpGolden::S_SessionRoster, Message, Error)))
		{
			TestEqual(TEXT("SessionRoster type"), Message.Type, EApexServerMessageType::SessionRoster);
			TestEqual(TEXT("roster session id"), Message.Roster.SessionId,
				FString(TEXT("01234567-89ab-cdef-0123-456789abcdef")));
			if (TestEqual(TEXT("two roster entries"), Message.Roster.Entries.Num(), 2))
			{
				TestEqual(TEXT("entry 0 car index"), Message.Roster.Entries[0].CarIndex, 0);
				TestEqual(TEXT("entry 0 name"), Message.Roster.Entries[0].PlayerName, FString(TEXT("Player")));
				TestFalse(TEXT("entry 0 is human"), Message.Roster.Entries[0].bIsAi);
				TestEqual(TEXT("entry 1 car index"), Message.Roster.Entries[1].CarIndex, 1);
				// Non-ASCII survives the roster too.
				TestEqual(TEXT("entry 1 name decodes UTF-8"), Message.Roster.Entries[1].PlayerName,
					FString(TEXT("AI Nürburgring")));
				TestTrue(TEXT("entry 1 is AI"), Message.Roster.Entries[1].bIsAi);
				// The blob predates the car id: an older server's roster still decodes.
				TestTrue(TEXT("no car id in an old roster"), Message.Roster.Entries[1].CarConfigId.IsEmpty());
			}
		}
	}

	// A timed race's clock: a sixth field after the cars.
	{
		FApexServerMessage Message;
		FString Error;
		if (TestTrue(FString::Printf(TEXT("TelemetryCompact with a race clock decodes (%s)"), *Error),
				ApexProtocol::DecodeUdpMessage(ApexUdpGolden::S_TelemetryCompactRaceClock, Message, Error)))
		{
			const FApexTelemetryFrame& Frame = Message.Telemetry;
			TestEqual(TEXT("clock frame tick"), Frame.ServerTick, static_cast<int64>(123456));
			TestEqual(TEXT("clock frame has no cars"), Frame.Cars.Num(), 0);
			TestTrue(TEXT("clock frame has a race clock"), Frame.HasRaceClock());
			TestEqual(TEXT("race time left"), Frame.RaceLeftMs, 0);
			TestEqual(TEXT("final lap"), Frame.RaceFinalLap, 37);
			TestFalse(TEXT("a six-field frame has no sky"), Frame.Sky.bValid);
		}
	}

	// The sky now: a seventh field, with the race clock nil before it.
	{
		FApexServerMessage Message;
		FString Error;
		if (TestTrue(FString::Printf(TEXT("TelemetryCompact with a sky decodes (%s)"), *Error),
				ApexProtocol::DecodeUdpMessage(ApexUdpGolden::S_TelemetryCompactSky, Message, Error)))
		{
			const FApexTelemetryFrame& Frame = Message.Telemetry;
			TestEqual(TEXT("sky frame tick"), Frame.ServerTick, static_cast<int64>(123456));
			TestFalse(TEXT("a nil race clock is no race clock"), Frame.HasRaceClock());
			const FApexSkyNow& Sky = Frame.Sky;
			TestTrue(TEXT("sky valid"), Sky.bValid);
			TestEqual(TEXT("sky clock"), Sky.ClockS, 15 * 3600 + 30 * 60 + 15);
			TestEqual(TEXT("sky clock text"), Sky.ClockText(), FString(TEXT("15:30")));
			TestEqual(TEXT("sky weather"), Sky.Weather, EApexWeather::LightRain);
			TestEqual(TEXT("sky rain"), Sky.RainPct, 37);
			TestEqual(TEXT("sky cloud"), Sky.CloudPct, 88);
			TestEqual(TEXT("sky road water"), Sky.RoadWaterPct, 21);
			TestEqual(TEXT("sky air (signed)"), Sky.AirC, -2);
			TestEqual(TEXT("sky track"), Sky.TrackC, 14);
			TestEqual(TEXT("sky wind"), Sky.WindKph, 23);
			TestEqual(TEXT("sky wind to"), Sky.WindToDeg, 300);
			TestEqual(TEXT("sky rubber"), Sky.LineRubberPct, 64);
			TestEqual(TEXT("sky time scale"), Sky.TimeScale, 24);
			TestEqual(TEXT("sky next weather"), Sky.NextWeather, static_cast<int32>(EApexWeather::HeavyRain));
			TestEqual(TEXT("sky next in"), Sky.NextInS, 1250);
			TestTrue(TEXT("sky has a next change"), Sky.HasNextChange());
			// 300° counter-clockwise from +X: along +X and toward -Y (the right).
			const FVector2D Wind = Sky.WindServerMps();
			TestTrue(TEXT("sky wind vector +X"), Wind.X > 0.0);
			TestTrue(TEXT("sky wind vector -Y"), Wind.Y < 0.0);
			TestTrue(TEXT("sky wind vector speed"), FMath::IsNearlyEqual(Wind.Size(), 23.0 / 3.6, 1e-3));
		}
	}

	// The frames from before the sky still decode with none.
	{
		FApexServerMessage Message;
		FString Error;
		if (ApexProtocol::DecodeUdpMessage(ApexUdpGolden::S_TelemetryCompact, Message, Error))
		{
			TestFalse(TEXT("an old frame has no sky"), Message.Telemetry.Sky.bValid);
		}
	}

	// The real prize: positional telemetry.
	{
		FApexServerMessage Message;
		FString Error;
		if (TestTrue(FString::Printf(TEXT("TelemetryCompact decodes (%s)"), *Error),
				ApexProtocol::DecodeUdpMessage(ApexUdpGolden::S_TelemetryCompact, Message, Error)))
		{
			TestEqual(TEXT("TelemetryCompact type"), Message.Type, EApexServerMessageType::TelemetryCompact);

			const FApexTelemetryFrame& Frame = Message.Telemetry;
			TestEqual(TEXT("server tick"), Frame.ServerTick, static_cast<int64>(123456));
			TestEqual(TEXT("session state"), Frame.SessionState, EApexSessionState::Racing);
			TestEqual(TEXT("game mode"), Frame.GameMode, EApexGameMode::Race);
			TestEqual(TEXT("countdown is absent"), Frame.CountdownMs, -1);
			TestFalse(TEXT("a lap race has no race clock"), Frame.HasRaceClock());

			if (TestEqual(TEXT("two cars"), Frame.Cars.Num(), 2))
			{
				const FApexCarTelemetry& Car = Frame.Cars[0];
				TestEqual(TEXT("car 0 index"), Car.CarIndex, 0);
				// Position is the strongest signal that field order is right: a
				// one-field slip would put yaw or speed in here.
				TestEqual(TEXT("car 0 pos X"), Car.Position.X, 100.5);
				TestEqual(TEXT("car 0 pos Y"), Car.Position.Y, -20.25);
				TestEqual(TEXT("car 0 pos Z"), Car.Position.Z, 0.5);
				TestEqual(TEXT("car 0 yaw"), Car.YawRad, 1.5f);
				TestEqual(TEXT("car 0 pitch"), Car.PitchRad, 0.0f);
				TestEqual(TEXT("car 0 roll"), Car.RollRad, -0.25f);
				TestEqual(TEXT("car 0 speed"), Car.SpeedMps, 42.0f);
				TestEqual(TEXT("car 0 throttle"), Car.Throttle, 1.0f);
				TestEqual(TEXT("car 0 brake"), Car.Brake, 0.0f);
				TestEqual(TEXT("car 0 steering"), Car.Steering, -0.5f);
				TestEqual(TEXT("car 0 gear"), Car.Gear, 4);
				TestEqual(TEXT("car 0 rpm"), Car.EngineRpm, 11000.0f);
				// Everything below here is read *after* the 16-float suspension
				// array, so it only lands if that array was skipped correctly.
				TestEqual(TEXT("car 0 lap"), Car.CurrentLap, 3);
				TestEqual(TEXT("car 0 track progress"), Car.TrackProgress, 0.75f);
				TestEqual(TEXT("car 0 still racing"), Car.FinishPosition, 0);
				TestEqual(TEXT("car 0 lap time"), Car.CurrentLapTimeMs, 65432);
				TestTrue(TEXT("car 0 on track"), Car.bIsOnTrack);
				TestFalse(TEXT("car 0 not colliding"), Car.bIsColliding);

				TestEqual(TEXT("car 1 index"), Frame.Cars[1].CarIndex, 1);
				TestEqual(TEXT("car 1 speed"), Frame.Cars[1].SpeedMps, 42.0f);
			}
		}
	}

	// Force feedback, positional as well. Every per-wheel array is read into
	// the same four wheels, so a slip by one element would shift a value onto
	// the wrong wheel rather than fail.
	{
		FApexServerMessage Message;
		FString Error;
		if (TestTrue(FString::Printf(TEXT("DriverFeedback decodes (%s)"), *Error),
				ApexProtocol::DecodeUdpMessage(ApexUdpGolden::S_DriverFeedback, Message, Error)))
		{
			TestEqual(TEXT("DriverFeedback type"), Message.Type, EApexServerMessageType::DriverFeedback);
			const FApexDriverFeedback& Feedback = Message.DriverFeedback;
			TestEqual(TEXT("server tick"), Feedback.ServerTick, static_cast<int64>(1234));
			if (TestEqual(TEXT("two steering samples"), Feedback.SteerTorque.Num(), 2))
			{
				TestEqual(TEXT("first sample"), Feedback.SteerTorque[0], 0.25f);
				TestEqual(TEXT("second sample"), Feedback.SteerTorque[1], -0.5f);
			}
			const FApexWheelFeedback* W = Feedback.Wheels;
			TestEqual(TEXT("FL slip ratio"), W[0].SlipRatio, 1.5f);
			TestEqual(TEXT("FR slip ratio"), W[1].SlipRatio, -2.0f);
			TestEqual(TEXT("RR slip ratio"), W[3].SlipRatio, 0.5f);
			TestEqual(TEXT("FR slip angle"), W[1].SlipAngle, 0.25f);
			TestEqual(TEXT("RL slip angle"), W[2].SlipAngle, -1.0f);
			TestEqual(TEXT("RR slip angle"), W[3].SlipAngle, 3.0f);
			TestEqual(TEXT("FL on the road"), W[0].Surface, EApexContactSurface::Road);
			TestEqual(TEXT("FR on a curb"), W[1].Surface, EApexContactSurface::Curb);
			TestEqual(TEXT("RL off track"), W[2].Surface, EApexContactSurface::Off);
			TestEqual(TEXT("FL suspension"), W[0].SuspensionMps, 0.5f);
			TestEqual(TEXT("FR suspension"), W[1].SuspensionMps, -0.25f);
			TestEqual(TEXT("RR suspension"), W[3].SuspensionMps, 2.0f);
			TestTrue(TEXT("ABS active"), Feedback.bAbsActive);
			TestFalse(TEXT("TC idle"), Feedback.bTcActive);
			TestEqual(TEXT("impact"), Feedback.ImpactMps, 4.5f);
			TestEqual(TEXT("steering kick"), Feedback.SteerKick, -1.5f);
			TestEqual(TEXT("steering input"), Feedback.SteerInput, 0.25f);
			TestEqual(TEXT("column stiffness"), Feedback.SteerStiffness, -4.0f);
			TestEqual(TEXT("front load"), Feedback.FrontLoad, 1.5f);
			TestEqual(TEXT("no flat spot from a 13-field server"), Feedback.Wheels[1].FlatSpot, 0.0f);
		}

		// Flat spots appended: percent per wheel.
		FApexServerMessage Flat;
		if (TestTrue(FString::Printf(TEXT("flat-spot DriverFeedback decodes (%s)"), *Error),
				ApexProtocol::DecodeUdpMessage(ApexUdpGolden::S_DriverFeedbackFlatSpot, Flat, Error)))
		{
			const FApexWheelFeedback* W = Flat.DriverFeedback.Wheels;
			TestEqual(TEXT("FL round"), W[0].FlatSpot, 0.0f);
			TestEqual(TEXT("FR flat spot"), W[1].FlatSpot, 0.4f);
			TestEqual(TEXT("RR flat spot"), W[3].FlatSpot, 1.0f);
			TestEqual(TEXT("front load before it"), Flat.DriverFeedback.FrontLoad, 1.5f);
		}

		// A server from before the stiffness sends ten fields: the kick, and
		// no prediction (a flat slope, a statically loaded front).
		TArray<uint8> TenFields(ApexUdpGolden::S_DriverFeedback, UE_ARRAY_COUNT(ApexUdpGolden::S_DriverFeedback) - 15);
		TenFields[16] = 0x9A;
		FApexServerMessage Ten;
		if (TestTrue(FString::Printf(TEXT("ten-field DriverFeedback decodes (%s)"), *Error),
				ApexProtocol::DecodeUdpMessage(TenFields, Ten, Error)))
		{
			TestEqual(TEXT("ten-field kick"), Ten.DriverFeedback.SteerKick, -1.5f);
			TestEqual(TEXT("no slope from an older server"), Ten.DriverFeedback.SteerStiffness, 0.0f);
			TestEqual(TEXT("static front load from an older server"), Ten.DriverFeedback.FrontLoad, 1.0f);
		}

		// A server from before the kick sends nine fields: still a message,
		// with no kick in it.
		TArray<uint8> NineFields(ApexUdpGolden::S_DriverFeedback, UE_ARRAY_COUNT(ApexUdpGolden::S_DriverFeedback) - 20);
		NineFields[16] = 0x99;
		FApexServerMessage Older;
		if (TestTrue(FString::Printf(TEXT("nine-field DriverFeedback decodes (%s)"), *Error),
				ApexProtocol::DecodeUdpMessage(NineFields, Older, Error)))
		{
			TestEqual(TEXT("older impact"), Older.DriverFeedback.ImpactMps, 4.5f);
			TestEqual(TEXT("no kick from an older server"), Older.DriverFeedback.SteerKick, 0.0f);
		}
	}

	return true;
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexDriverFeedbackAbsorbTest,
	"ApexSim.Net.Udp.DriverFeedbackAbsorb",
	ApexUdpTestFlags)

bool FApexDriverFeedbackAbsorbTest::RunTest(const FString& Parameters)
{
	// Two messages arriving in one game frame must read as one interval: the
	// kerb strike and impact in the first survive the quiet second.
	FApexDriverFeedback First;
	First.ServerTick = 100;
	First.SteerTorque = {0.1f, 0.2f};
	First.Wheels[1].Surface = EApexContactSurface::Curb;
	First.Wheels[2].SuspensionMps = -3.0f;
	First.ImpactMps = 6.0f;
	First.SteerKick = 0.4f;

	FApexDriverFeedback Second;
	Second.ServerTick = 104;
	Second.SteerTorque = {0.3f, 0.4f};
	Second.Wheels[2].SuspensionMps = 1.0f;
	Second.Wheels[0].SlipAngle = -1.5f;
	Second.bAbsActive = true;
	Second.SteerKick = -0.9f;

	First.Absorb(Second);
	TestEqual(TEXT("newest tick"), First.ServerTick, static_cast<int64>(104));
	TestEqual(TEXT("samples appended"), First.SteerTorque.Num(), 4);
	TestEqual(TEXT("samples in order"), First.SteerTorque[3], 0.4f);
	TestEqual(TEXT("curb kept"), First.Wheels[1].Surface, EApexContactSurface::Curb);
	TestEqual(TEXT("largest hit kept, with its sign"), First.Wheels[2].SuspensionMps, -3.0f);
	TestEqual(TEXT("later slide taken"), First.Wheels[0].SlipAngle, -1.5f);
	TestTrue(TEXT("ABS from the second"), First.bAbsActive);
	TestEqual(TEXT("impact kept"), First.ImpactMps, 6.0f);
	TestEqual(TEXT("hardest kick kept, with its sign"), First.SteerKick, -0.9f);
	return true;
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexUdpRobustnessTest,
	"ApexSim.Net.Udp.Robustness",
	ApexUdpTestFlags)

bool FApexUdpRobustnessTest::RunTest(const FString& Parameters)
{
	// Datagrams get truncated and corrupted; none of that may read out of bounds
	// or half-fill a frame.
	{
		TArrayView<const uint8> Full(ApexUdpGolden::S_TelemetryCompact);
		for (int32 PrefixLength = 0; PrefixLength < Full.Num(); ++PrefixLength)
		{
			FApexServerMessage Message;
			FString Error;
			const bool bDecoded = ApexProtocol::DecodeUdpMessage(
				TArrayView<const uint8>(Full.GetData(), PrefixLength), Message, Error);
			TestFalse(FString::Printf(TEXT("a %d byte prefix of telemetry is rejected"), PrefixLength), bDecoded);
		}
	}

	{
		TArrayView<const uint8> Full(ApexUdpGolden::S_DriverFeedback);
		for (int32 PrefixLength = 0; PrefixLength < Full.Num(); ++PrefixLength)
		{
			FApexServerMessage Message;
			FString Error;
			const bool bDecoded = ApexProtocol::DecodeUdpMessage(
				TArrayView<const uint8>(Full.GetData(), PrefixLength), Message, Error);
			TestFalse(FString::Printf(TEXT("a %d byte prefix of driver feedback is rejected"), PrefixLength), bDecoded);
		}
	}

	{
		const uint8 Garbage[] = {0xC1, 0xC1, 0xC1, 0xC1};
		FApexServerMessage Message;
		FString Error;
		TestFalse(TEXT("garbage datagram is rejected"),
			ApexProtocol::DecodeUdpMessage(
				TArrayView<const uint8>(Garbage, UE_ARRAY_COUNT(Garbage)), Message, Error));
	}

	{
		FApexServerMessage Message;
		FString Error;
		TestFalse(TEXT("empty datagram is rejected"),
			ApexProtocol::DecodeUdpMessage(TArrayView<const uint8>(), Message, Error));
	}

	// A positional variant we do not know must not be mistaken for telemetry.
	{
		// ["SomeFutureUdpMessage", [1, 2]]
		const uint8 Future[] = {
			0x92,
			0xB3, 'S', 'o', 'm', 'e', 'F', 'u', 't', 'u', 'r', 'e', 'U', 'd', 'p', 'M', 'e', 's', 's', 'a', 'g', 'e',
			0x92, 0x01, 0x02
		};
		FApexServerMessage Message;
		FString Error;
		TestTrue(FString::Printf(TEXT("an unknown positional variant decodes without error (%s)"), *Error),
			ApexProtocol::DecodeUdpMessage(
				TArrayView<const uint8>(Future, UE_ARRAY_COUNT(Future)), Message, Error));
		TestEqual(TEXT("unknown positional variant is Unknown"), Message.Type, EApexServerMessageType::Unknown);
		TestEqual(TEXT("no cars were invented"), Message.Telemetry.Cars.Num(), 0);
	}

	return true;
}


/**
 * The lap fields at the end of `CompactCarState`.
 *
 * They are appended, not inserted, so both shapes have to decode: the current
 * 23-field car with its times and its track-limit flags, and the 22-field car
 * an older server sends, which reads as a clean lap with no times.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexUdpLapFieldsTest,
	"ApexSim.Net.Udp.LapFields",
	ApexUdpTestFlags)

bool FApexUdpLapFieldsTest::RunTest(const FString& Parameters)
{
	{
		FApexServerMessage Message;
		FString Error;
		if (TestTrue(FString::Printf(TEXT("23-field telemetry decodes (%s)"), *Error),
				ApexProtocol::DecodeUdpMessage(ApexUdpGolden::S_TelemetryCompactLapFlags, Message, Error))
			&& TestEqual(TEXT("one car"), Message.Telemetry.Cars.Num(), 1))
		{
			const FApexCarTelemetry& Car = Message.Telemetry.Cars[0];
			// Position first: a one-field slip anywhere would land here.
			TestEqual(TEXT("pos X"), Car.Position.X, 100.5);
			TestEqual(TEXT("gear"), Car.Gear, 4);
			TestEqual(TEXT("lap"), Car.CurrentLap, 3);
			TestEqual(TEXT("current lap time"), Car.CurrentLapTimeMs, 91234);
			TestEqual(TEXT("last lap"), Car.LastLapTimeMs, 82615);
			TestEqual(TEXT("best lap"), Car.BestLapTimeMs, 82615);
			TestTrue(TEXT("on track"), Car.bIsOnTrack);
			TestFalse(TEXT("not colliding"), Car.bIsColliding);
			TestTrue(TEXT("lap in progress is struck"), Car.bLapInvalid);
			TestFalse(TEXT("the completed lap stood"), Car.bLastLapInvalid);
			TestFalse(TEXT("not in a garage"), Car.bInGarage);
		}
	}

	{
		// lap_flags bit 2: a hotlap car parked in its garage.
		FApexServerMessage Message;
		FString Error;
		if (TestTrue(FString::Printf(TEXT("garage telemetry decodes (%s)"), *Error),
				ApexProtocol::DecodeUdpMessage(ApexUdpGolden::S_TelemetryCompactGarage, Message, Error))
			&& TestEqual(TEXT("one car"), Message.Telemetry.Cars.Num(), 1))
		{
			const FApexCarTelemetry& Car = Message.Telemetry.Cars[0];
			TestTrue(TEXT("in the garage"), Car.bInGarage);
			TestTrue(TEXT("the other bits still read"), Car.bLapInvalid);
			TestFalse(TEXT("lights off"), Car.bHeadlights);
			TestFalse(TEXT("not flashing"), Car.bHeadlightFlash);
		}
	}

	{
		// lap_flags bits 5 and 6: the headlights and the flash. The car is the
		// frame's last value and the flags its last byte.
		TArray<uint8> Lit(ApexUdpGolden::S_TelemetryCompactGarage, UE_ARRAY_COUNT(ApexUdpGolden::S_TelemetryCompactGarage));
		Lit.Last() = 0x05 | 32 | 64;
		FApexServerMessage Message;
		FString Error;
		if (TestTrue(FString::Printf(TEXT("lit telemetry decodes (%s)"), *Error),
				ApexProtocol::DecodeUdpMessage(Lit, Message, Error))
			&& TestEqual(TEXT("one car"), Message.Telemetry.Cars.Num(), 1))
		{
			const FApexCarTelemetry& Car = Message.Telemetry.Cars[0];
			TestTrue(TEXT("headlights on"), Car.bHeadlights);
			TestTrue(TEXT("flashing"), Car.bHeadlightFlash);
			TestTrue(TEXT("the garage bit still reads"), Car.bInGarage);
			TestFalse(TEXT("DRS untouched"), Car.bDrsAllowed || Car.bDrsOpen);
		}
	}

	{
		// 24 fields: the tank appended after lap_flags, in tenths of a litre.
		FApexServerMessage Message;
		FString Error;
		if (TestTrue(FString::Printf(TEXT("24-field telemetry decodes (%s)"), *Error),
				ApexProtocol::DecodeUdpMessage(ApexUdpGolden::S_TelemetryCompactFuel, Message, Error))
			&& TestEqual(TEXT("one car"), Message.Telemetry.Cars.Num(), 1))
		{
			const FApexCarTelemetry& Car = Message.Telemetry.Cars[0];
			TestEqual(TEXT("pos X"), Car.Position.X, 100.5);
			TestTrue(TEXT("the flags still read"), Car.bLapInvalid);
			TestEqual(TEXT("fuel"), Car.FuelLiters, 42.5f);
		}
		// A 23-field frame from before the tank leaves it unknown.
		if (ApexProtocol::DecodeUdpMessage(ApexUdpGolden::S_TelemetryCompactLapFlags, Message, Error)
			&& Message.Telemetry.Cars.Num() == 1)
		{
			TestTrue(TEXT("no fuel from an older server"), Message.Telemetry.Cars[0].FuelLiters < 0.0f);
		}
	}

	{
		// 26 fields: the tyres' tread temperatures and pressures appended
		// after the tank, a byte each per tyre.
		FApexServerMessage Message;
		FString Error;
		if (TestTrue(FString::Printf(TEXT("26-field telemetry decodes (%s)"), *Error),
				ApexProtocol::DecodeUdpMessage(ApexUdpGolden::S_TelemetryCompactTyres, Message, Error))
			&& TestEqual(TEXT("one car"), Message.Telemetry.Cars.Num(), 1))
		{
			const FApexCarTelemetry& Car = Message.Telemetry.Cars[0];
			TestEqual(TEXT("fuel still reads"), Car.FuelLiters, 42.5f);
			TestTrue(TEXT("tyres known"), Car.HasTyres());
			TestEqual(TEXT("FL tread"), Car.TyreTempC[0], 84.0f);
			TestEqual(TEXT("FR tread"), Car.TyreTempC[1], 92.0f);
			TestEqual(TEXT("RL tread"), Car.TyreTempC[2], 103.0f);
			TestEqual(TEXT("RR tread"), Car.TyreTempC[3], 255.0f);
			TestEqual(TEXT("FL pressure"), Car.TyrePressureKpa[0], 176.0f);
			TestEqual(TEXT("RR pressure"), Car.TyrePressureKpa[3], 1.0f);
		}
		// A 24-field frame from before the tyres leaves them unknown.
		if (ApexProtocol::DecodeUdpMessage(ApexUdpGolden::S_TelemetryCompactFuel, Message, Error)
			&& Message.Telemetry.Cars.Num() == 1)
		{
			TestFalse(TEXT("no tyres from an older server"), Message.Telemetry.Cars[0].HasTyres());
		}
	}

	{
		// 27 fields: the tow appended after the tyres, in percent.
		FApexServerMessage Message;
		FString Error;
		if (TestTrue(FString::Printf(TEXT("27-field telemetry decodes (%s)"), *Error),
				ApexProtocol::DecodeUdpMessage(ApexUdpGolden::S_TelemetryCompactTow, Message, Error))
			&& TestEqual(TEXT("one car"), Message.Telemetry.Cars.Num(), 1))
		{
			const FApexCarTelemetry& Car = Message.Telemetry.Cars[0];
			TestEqual(TEXT("tyres still read"), Car.TyreTempC[2], 103.0f);
			TestEqual(TEXT("tow"), Car.TowShare, 0.17f);
		}
		// A 31-field frame: wear, the compound and the pit lane.
		FApexServerMessage Pit;
		if (TestTrue(TEXT("31-field telemetry decodes"),
				ApexProtocol::DecodeUdpMessage(ApexUdpGolden::S_TelemetryCompactPit, Pit, Error))
			&& Pit.Telemetry.Cars.Num() == 1)
		{
			const FApexCarTelemetry& Car = Pit.Telemetry.Cars[0];
			TestEqual(TEXT("tow still reads"), Car.TowShare, 0.17f);
			TestEqual(TEXT("FL wear"), Car.TyreWearPct[0], 12.0f);
			TestEqual(TEXT("RR wear"), Car.TyreWearPct[3], 100.0f);
			TestEqual(TEXT("softs"), Car.Compound, 0);
			TestEqual(TEXT("letter"), FApexCarTelemetry::CompoundLetter(Car.Compound), FString(TEXT("S")));
			TestTrue(TEXT("in the lane"), Car.bInPitLane);
			TestTrue(TEXT("on the limiter"), Car.bPitLimiter);
			TestTrue(TEXT("in service"), Car.bPitServicing);
			TestEqual(TEXT("service left"), Car.ServiceSecondsLeft, 7.3f);
			TestFalse(TEXT("no autopilot"), Car.bPitAutopilot);
			TestFalse(TEXT("exit open"), Car.bPitExitClosed);
			TestFalse(TEXT("not held"), Car.bPitHeld);
		}
		// The same frame with the autopilot, the red exit light and the hold
		// bits set (pit_flags 63): the byte is the second last.
		TArray<uint8> PitBits(ApexUdpGolden::S_TelemetryCompactPit, UE_ARRAY_COUNT(ApexUdpGolden::S_TelemetryCompactPit));
		if (TestEqual(TEXT("pit_flags where expected"), static_cast<int32>(PitBits[PitBits.Num() - 2]), 7))
		{
			PitBits[PitBits.Num() - 2] = 63;
			FApexServerMessage Held;
			if (TestTrue(TEXT("pit bits decode"), ApexProtocol::DecodeUdpMessage(PitBits, Held, Error))
				&& Held.Telemetry.Cars.Num() == 1)
			{
				const FApexCarTelemetry& Car = Held.Telemetry.Cars[0];
				TestTrue(TEXT("still in service"), Car.bPitServicing);
				TestTrue(TEXT("autopilot"), Car.bPitAutopilot);
				TestTrue(TEXT("exit closed"), Car.bPitExitClosed);
				TestTrue(TEXT("held"), Car.bPitHeld);
			}
		}
		// 33 fields: the brakes and the coolant.
		FApexServerMessage Heat;
		if (TestTrue(TEXT("33-field telemetry decodes"),
				ApexProtocol::DecodeUdpMessage(ApexUdpGolden::S_TelemetryCompactHeat, Heat, Error))
			&& Heat.Telemetry.Cars.Num() == 1)
		{
			const FApexCarTelemetry& Car = Heat.Telemetry.Cars[0];
			TestTrue(TEXT("the pit still reads"), Car.bPitServicing);
			TestEqual(TEXT("FL brake"), Car.BrakeTempC[0], 612.0f);
			TestEqual(TEXT("RR brake"), Car.BrakeTempC[3], 350.0f);
			TestEqual(TEXT("coolant"), Car.WaterTempC, 105.0f);
		}
		// 34 fields: the damage.
		FApexServerMessage Damage;
		if (TestTrue(TEXT("34-field telemetry decodes"),
				ApexProtocol::DecodeUdpMessage(ApexUdpGolden::S_TelemetryCompactDamage, Damage, Error))
			&& Damage.Telemetry.Cars.Num() == 1)
		{
			const FApexCarTelemetry& Car = Damage.Telemetry.Cars[0];
			TestEqual(TEXT("the coolant still reads"), Car.WaterTempC, 105.0f);
			TestTrue(TEXT("damage known"), Car.HasDamage());
			TestEqual(TEXT("front"), Car.DamagePct[0], 23.0f);
			TestEqual(TEXT("left"), Car.DamagePct[2], 8.0f);
			TestEqual(TEXT("engine"), Car.DamagePct[4], 100.0f);
		}
		if (ApexProtocol::DecodeUdpMessage(ApexUdpGolden::S_TelemetryCompactHeat, Damage, Error)
			&& Damage.Telemetry.Cars.Num() == 1)
		{
			TestFalse(TEXT("no damage from an older server"), Damage.Telemetry.Cars[0].HasDamage());
		}
		// 37 fields: the hybrid.
		FApexServerMessage Ers;
		if (TestTrue(TEXT("37-field telemetry decodes"),
				ApexProtocol::DecodeUdpMessage(ApexUdpGolden::S_TelemetryCompactErs, Ers, Error))
			&& Ers.Telemetry.Cars.Num() == 1)
		{
			const FApexCarTelemetry& Car = Ers.Telemetry.Cars[0];
			TestEqual(TEXT("the damage still reads"), Car.DamagePct[4], 100.0f);
			TestTrue(TEXT("a hybrid"), Car.HasHybrid());
			TestEqual(TEXT("charge"), Car.ErsChargePct, 64.0f);
			TestTrue(TEXT("no lap budget"), Car.ErsLapPct < 0.0f);
			TestEqual(TEXT("attack"), Car.ErsMode, 2);
			TestTrue(TEXT("deploying"), Car.bErsDeploying);
			TestFalse(TEXT("not harvesting"), Car.bErsHarvesting);
			TestTrue(TEXT("overtake"), Car.bErsBoost);
		}
		if (ApexProtocol::DecodeUdpMessage(ApexUdpGolden::S_TelemetryCompactDamage, Ers, Error)
			&& Ers.Telemetry.Cars.Num() == 1)
		{
			TestFalse(TEXT("no hybrid from an older server"), Ers.Telemetry.Cars[0].HasHybrid());
			TestEqual(TEXT("no mode from an older server"), Ers.Telemetry.Cars[0].ErsMode, -1);
		}
		// 41 fields: the tread shoulders, brake wear, slide flags and stint budget.
		FApexServerMessage Zones;
		if (TestTrue(TEXT("41-field telemetry decodes"),
				ApexProtocol::DecodeUdpMessage(ApexUdpGolden::S_TelemetryCompactZones, Zones, Error))
			&& Zones.Telemetry.Cars.Num() == 1)
		{
			const FApexCarTelemetry& Car = Zones.Telemetry.Cars[0];
			TestEqual(TEXT("the hybrid still reads"), Car.ErsChargePct, 64.0f);
			TestTrue(TEXT("the overtake button still reads"), Car.bErsBoost);
			// Every shoulder is sent (the RR pair at the byte's ceiling, 255), so the set is known.
			TestTrue(TEXT("every shoulder known"), Car.HasTyreEdges());
			TestEqual(TEXT("FL inner"), Car.TyreInnerC[0], 88.0f);
			TestEqual(TEXT("FL outer"), Car.TyreOuterC[0], 80.0f);
			TestEqual(TEXT("FR inner"), Car.TyreInnerC[1], 90.0f);
			TestEqual(TEXT("FR outer"), Car.TyreOuterC[1], 94.0f);
			TestEqual(TEXT("RL inner"), Car.TyreInnerC[2], 100.0f);
			TestEqual(TEXT("RL outer"), Car.TyreOuterC[2], 105.0f);
			TestEqual(TEXT("RR inner at the ceiling"), Car.TyreInnerC[3], 255.0f);
			TestEqual(TEXT("RR outer at the ceiling"), Car.TyreOuterC[3], 255.0f);
			TestEqual(TEXT("FL brake wear"), Car.BrakeWearPct[0], 12.0f);
			TestEqual(TEXT("RL brake wear"), Car.BrakeWearPct[2], 31.0f);
			TestEqual(TEXT("RR brake wear"), Car.BrakeWearPct[3], 100.0f);
			TestTrue(TEXT("FR sliding"), Car.bTyreSliding[1]);
			TestFalse(TEXT("FL not sliding"), Car.bTyreSliding[0]);
			TestTrue(TEXT("FL locked"), Car.bTyreLocked[0]);
			TestFalse(TEXT("FR not locked"), Car.bTyreLocked[1]);
			TestFalse(TEXT("RR not sliding"), Car.bTyreSliding[3]);
			TestEqual(TEXT("stint budget"), Car.ErsStintPct, 73.0f);
			// Zero one shoulder and the whole set is unknown (0 is "not known",
			// and a half-known set is no use to a display): the blob ends
			// `..., 0xCC, 0xFF, 0xCC, 0xFF, <brake wear: 0x94 + 4 bytes + 1 uint8 marker>, 0x12, 0x49`,
			// each 0xCC a uint8 marker, so the value byte keeps its width.
			TArray<uint8> Cold(ApexUdpGolden::S_TelemetryCompactZones, UE_ARRAY_COUNT(ApexUdpGolden::S_TelemetryCompactZones));
			const int32 End = Cold.Num();
			Cold[End - 8] = 0x00; // outer RR: not known
			FApexServerMessage Patched;
			if (TestTrue(TEXT("the patched frame decodes"), ApexProtocol::DecodeUdpMessage(Cold, Patched, Error))
				&& Patched.Telemetry.Cars.Num() == 1)
			{
				const FApexCarTelemetry& Half = Patched.Telemetry.Cars[0];
				TestFalse(TEXT("a half-known set of shoulders is unknown"), Half.HasTyreEdges());
				TestTrue(TEXT("FL inner unknown with it"), Half.TyreInnerC[0] < 0.0f);
				TestEqual(TEXT("the brake wear after it still reads"), Half.BrakeWearPct[3], 100.0f);
				TestEqual(TEXT("and the stint budget"), Half.ErsStintPct, 73.0f);
			}
		}
		// 42 fields: a recovery's hold.
		FApexServerMessage Recover;
		if (TestTrue(TEXT("42-field telemetry decodes"),
				ApexProtocol::DecodeUdpMessage(ApexUdpGolden::S_TelemetryCompactRecover, Recover, Error))
			&& Recover.Telemetry.Cars.Num() == 1)
		{
			const FApexCarTelemetry& Car = Recover.Telemetry.Cars[0];
			TestEqual(TEXT("the stint budget still reads"), Car.ErsStintPct, 73.0f);
			TestEqual(TEXT("the recovery hold"), Car.RecoverSecondsLeft, 6.3f);
			TestTrue(TEXT("recovering"), Car.IsRecovering());
		}
		// 43 fields: the flat spots.
		FApexServerMessage Spots;
		if (TestTrue(TEXT("43-field telemetry decodes"),
				ApexProtocol::DecodeUdpMessage(ApexUdpGolden::S_TelemetryCompactFlatSpot, Spots, Error))
			&& Spots.Telemetry.Cars.Num() == 1)
		{
			const FApexCarTelemetry& Car = Spots.Telemetry.Cars[0];
			TestEqual(TEXT("the recovery hold still reads"), Car.RecoverSecondsLeft, 6.3f);
			TestEqual(TEXT("FL round"), Car.FlatSpot[0], 0.0f);
			TestEqual(TEXT("FR flat-spotted"), Car.FlatSpot[1], 0.4f);
			TestEqual(TEXT("RR the worst"), Car.FlatSpot[3], 1.0f);
		}
		if (ApexProtocol::DecodeUdpMessage(ApexUdpGolden::S_TelemetryCompactRecover, Spots, Error)
			&& Spots.Telemetry.Cars.Num() == 1)
		{
			TestEqual(TEXT("no flat spots from an older server"), Spots.Telemetry.Cars[0].FlatSpot[3], 0.0f);
		}
		if (ApexProtocol::DecodeUdpMessage(ApexUdpGolden::S_TelemetryCompactZones, Recover, Error)
			&& Recover.Telemetry.Cars.Num() == 1)
		{
			TestFalse(TEXT("no recovery from an older server"), Recover.Telemetry.Cars[0].IsRecovering());
		}
		if (ApexProtocol::DecodeUdpMessage(ApexUdpGolden::S_TelemetryCompactErs, Zones, Error)
			&& Zones.Telemetry.Cars.Num() == 1)
		{
			const FApexCarTelemetry& Car = Zones.Telemetry.Cars[0];
			TestFalse(TEXT("no shoulders from an older server"), Car.HasTyreEdges());
			TestTrue(TEXT("no brake wear from an older server"), Car.BrakeWearPct[0] < 0.0f);
			TestFalse(TEXT("nothing slides on an older server"), Car.bTyreSliding[0] || Car.bTyreSliding[1] || Car.bTyreSliding[2] || Car.bTyreSliding[3]);
			TestFalse(TEXT("nothing locks on an older server"), Car.bTyreLocked[0] || Car.bTyreLocked[3]);
			TestTrue(TEXT("no stint budget from an older server"), Car.ErsStintPct < 0.0f);
		}
		if (ApexProtocol::DecodeUdpMessage(ApexUdpGolden::S_TelemetryCompactPit, Heat, Error)
			&& Heat.Telemetry.Cars.Num() == 1)
		{
			TestTrue(TEXT("no brakes from an older server"), Heat.Telemetry.Cars[0].BrakeTempC[0] < 0.0f);
		}
		// A 27-field frame from before them leaves them unknown.
		FApexServerMessage Tow;
		if (ApexProtocol::DecodeUdpMessage(ApexUdpGolden::S_TelemetryCompactTow, Tow, Error)
			&& Tow.Telemetry.Cars.Num() == 1)
		{
			TestEqual(TEXT("no compound from an older server"), Tow.Telemetry.Cars[0].Compound, -1);
			TestTrue(TEXT("no wear from an older server"), Tow.Telemetry.Cars[0].TyreWearPct[0] < 0.0f);
		}
		// A 26-field frame from before the tow leaves it unknown.
		if (ApexProtocol::DecodeUdpMessage(ApexUdpGolden::S_TelemetryCompactTyres, Message, Error)
			&& Message.Telemetry.Cars.Num() == 1)
		{
			TestTrue(TEXT("no tow from an older server"), Message.Telemetry.Cars[0].TowShare < 0.0f);
		}
	}

	{
		// The blob from before the lap fields: 22 fields, no flags.
		FApexServerMessage Message;
		FString Error;
		if (TestTrue(FString::Printf(TEXT("22-field telemetry still decodes (%s)"), *Error),
				ApexProtocol::DecodeUdpMessage(ApexUdpGolden::S_TelemetryCompact, Message, Error))
			&& TestEqual(TEXT("two cars"), Message.Telemetry.Cars.Num(), 2))
		{
			const FApexCarTelemetry& Car = Message.Telemetry.Cars[0];
			TestEqual(TEXT("pos X survives the shorter car"), Car.Position.X, 100.5);
			TestFalse(TEXT("an old server never strikes a lap"), Car.bLapInvalid);
			TestFalse(TEXT("nor the one before it"), Car.bLastLapInvalid);
		}
	}

	return true;
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexErsModesTest,
	"ApexSim.Net.Ers.Modes",
	ApexUdpTestFlags)

bool FApexErsModesTest::RunTest(const FString& Parameters)
{
	// The ERS key steps Balanced -> Attack -> Harvest -> Balanced, the
	// server's numbering (hybrid.rs ErsMode).
	TestEqual(TEXT("balanced to attack"), ApexErs::NextMode(1), 2);
	TestEqual(TEXT("attack to harvest"), ApexErs::NextMode(2), 0);
	TestEqual(TEXT("harvest to balanced"), ApexErs::NextMode(0), 1);
	TestEqual(TEXT("unknown counts as balanced"), ApexErs::NextMode(-1), 2);
	TestEqual(TEXT("labels"), ApexErs::ShortLabel(0) + ApexErs::ShortLabel(1) + ApexErs::ShortLabel(2),
		FString(TEXT("HARVBALATK")));
	TestTrue(TEXT("no hybrid, no label"), ApexErs::ShortLabel(-1).IsEmpty());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
