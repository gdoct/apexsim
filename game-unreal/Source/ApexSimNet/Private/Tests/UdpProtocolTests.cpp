#include "Misc/AutomationTest.h"

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

	FApexPlayerInput Input;
	Input.Throttle = 1.0f;
	Input.Brake = 0.0f;
	Input.Steering = -0.5f;
	Input.Gear = 4;
	Input.bDrs = false;
	Input.Headlights = -1;
	Input.bFlash = false;
	CheckBytes(TEXT("PlayerInput"),
		ApexProtocol::EncodePlayerInput(4242, Input),
		ApexUdpGolden::C_PlayerInput);

	// Every bool and the nil switch is one byte, so a held button or a set
	// switch is the same message with that byte changed (server: `cargo test
	// player_input_headlights_wire_format`). From the end: flash, then
	// `a5 "flash"`, the switch, `aa "headlights"`, drs.
	{
		const int32 Len = (int32)UE_ARRAY_COUNT(ApexUdpGolden::C_PlayerInput);
		const int32 FlashAt = Len - 1;
		const int32 SwitchAt = FlashAt - 7;
		const int32 DrsAt = SwitchAt - 12;
		TestEqual(TEXT("the switch left to the sky is nil"), ApexUdpGolden::C_PlayerInput[SwitchAt], (uint8)0xC0);

		Input.bDrs = true;
		Input.Headlights = 0;
		Input.bFlash = true;
		const TArray<uint8> Held = ApexProtocol::EncodePlayerInput(4242, Input);
		if (TestEqual(TEXT("PlayerInput with every button set is the same length"), Held.Num(), Len))
		{
			TestEqual(TEXT("DRS held is true"), Held[DrsAt], (uint8)0xC3);
			TestEqual(TEXT("headlights switched off is false"), Held[SwitchAt], (uint8)0xC2);
			TestEqual(TEXT("flash held is true"), Held[FlashAt], (uint8)0xC3);
		}
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

#endif // WITH_DEV_AUTOMATION_TESTS
