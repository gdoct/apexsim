#include "ApexBootSettings.h"
#include "ApexTestCommon.h"
#include "Hud/ApexHudData.h"
#include "Telemetry/ApexTelemetryOutput.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	uint32 TelemetryTestU32(const TArray<uint8>& Bytes, int32 Offset)
	{
		return static_cast<uint32>(Bytes[Offset]) | (static_cast<uint32>(Bytes[Offset + 1]) << 8)
			| (static_cast<uint32>(Bytes[Offset + 2]) << 16) | (static_cast<uint32>(Bytes[Offset + 3]) << 24);
	}

	float TelemetryTestF32(const TArray<uint8>& Bytes, int32 Offset)
	{
		const uint32 Bits = TelemetryTestU32(Bytes, Offset);
		float Value = 0.0f;
		FMemory::Memcpy(&Value, &Bits, sizeof(Value));
		return Value;
	}
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexTelemetryOutputLayoutTest,
	"ApexSim.TelemetryOutput.Layout",
	ApexTestFlags)

bool FApexTelemetryOutputLayoutTest::RunTest(const FString& Parameters)
{
	// The offsets are the public API docs/game/telemetry-output.md lists: an
	// app reads fields by them, so none may move.
	using namespace ApexTelemetryOutput;
	FPacket P;
	P.Sequence = 7;
	P.Flags = CarPresent | DrsOpen;
	P.TimeS = 1.5f;
	P.GameMode = 7;
	P.Position = FVector3f(1.0f, 2.0f, 3.0f);
	P.Attitude = FVector3f(0.1f, 0.2f, 0.3f);
	P.LocalVelocity = FVector3f(50.0f, -1.0f, 0.5f);
	P.LocalAccel = FVector3f(-20.0f, 15.0f, 2.0f);
	P.AngularRate = FVector3f(0.01f, 0.02f, 0.4f);
	P.SpeedMps = 50.0f;
	P.Rpm = 9000.0f;
	P.RedlineRpm = 11000.0f;
	P.MaxRpm = 11500.0f;
	P.RevFraction = 0.78f;
	P.Gear = -1;
	P.Throttle = 0.25f;
	P.Brake = 0.75f;
	P.Steering = -0.5f;
	P.FuelLiters = 42.5f;
	P.ErsCharge = 0.6f;
	P.TyreTempC[0] = 85.0f;
	P.TyreTempC[3] = 92.0f;
	P.TyrePressureKpa[1] = 170.0f;
	P.TyreWearPct[2] = 12.0f;
	P.Place = 3;
	P.CarCount = 20;
	P.Lap = 3;
	P.LapLimit = 10;
	P.LapTimeS = 61.25f;
	P.LastLapS = 90.5f;
	P.BestLapS = 89.75f;
	P.DeltaS = -0.25f;
	P.StationM = 1234.0f;
	P.RaceLeftS = -1.0f;
	const TArray<uint8> Bytes = Encode(P);
	TestEqual(TEXT("216 bytes"), Bytes.Num(), static_cast<int32>(PacketBytes));
	TestEqual(TEXT("magic spells APXT"), FString::Printf(TEXT("%c%c%c%c"), Bytes[0], Bytes[1], Bytes[2], Bytes[3]), FString(TEXT("APXT")));
	TestEqual(TEXT("version at 4"), static_cast<int32>(Bytes[4] | (Bytes[5] << 8)), 1);
	TestEqual(TEXT("size at 6"), static_cast<int32>(Bytes[6] | (Bytes[7] << 8)), 216);
	TestEqual(TEXT("sequence at 8"), TelemetryTestU32(Bytes, 8), 7u);
	TestEqual(TEXT("flags at 12"), TelemetryTestU32(Bytes, 12), static_cast<uint32>(CarPresent | DrsOpen));
	TestEqual(TEXT("time at 16"), TelemetryTestF32(Bytes, 16), 1.5f);
	TestEqual(TEXT("game mode at 20"), TelemetryTestU32(Bytes, 20), 7u);
	TestEqual(TEXT("position z at 32"), TelemetryTestF32(Bytes, 32), 3.0f);
	TestEqual(TEXT("roll at 44"), TelemetryTestF32(Bytes, 44), 0.3f);
	TestEqual(TEXT("forward velocity at 48"), TelemetryTestF32(Bytes, 48), 50.0f);
	TestEqual(TEXT("surge at 60"), TelemetryTestF32(Bytes, 60), -20.0f);
	TestEqual(TEXT("sway at 64"), TelemetryTestF32(Bytes, 64), 15.0f);
	TestEqual(TEXT("yaw rate at 80"), TelemetryTestF32(Bytes, 80), 0.4f);
	TestEqual(TEXT("speed at 84"), TelemetryTestF32(Bytes, 84), 50.0f);
	TestEqual(TEXT("rpm at 88"), TelemetryTestF32(Bytes, 88), 9000.0f);
	TestEqual(TEXT("rev fraction at 100"), TelemetryTestF32(Bytes, 100), 0.78f);
	TestEqual(TEXT("reverse gear at 104"), static_cast<int32>(TelemetryTestU32(Bytes, 104)), -1);
	TestEqual(TEXT("brake at 112"), TelemetryTestF32(Bytes, 112), 0.75f);
	TestEqual(TEXT("fuel at 120"), TelemetryTestF32(Bytes, 120), 42.5f);
	TestEqual(TEXT("FL tread at 128"), TelemetryTestF32(Bytes, 128), 85.0f);
	TestEqual(TEXT("RR tread at 140"), TelemetryTestF32(Bytes, 140), 92.0f);
	TestEqual(TEXT("FR pressure at 148"), TelemetryTestF32(Bytes, 148), 170.0f);
	TestEqual(TEXT("RL wear at 168"), TelemetryTestF32(Bytes, 168), 12.0f);
	TestEqual(TEXT("place at 176"), TelemetryTestU32(Bytes, 176), 3u);
	TestEqual(TEXT("car count at 180"), TelemetryTestU32(Bytes, 180), 20u);
	TestEqual(TEXT("lap limit at 188"), TelemetryTestU32(Bytes, 188), 10u);
	TestEqual(TEXT("best lap at 200"), TelemetryTestF32(Bytes, 200), 89.75f);
	TestEqual(TEXT("station at 208"), TelemetryTestF32(Bytes, 208), 1234.0f);
	TestEqual(TEXT("race left at 212"), TelemetryTestF32(Bytes, 212), -1.0f);
	return true;
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexTelemetryOutputFromHudTest,
	"ApexSim.TelemetryOutput.FromHud",
	ApexTestFlags)

bool FApexTelemetryOutputFromHudTest::RunTest(const FString& Parameters)
{
	// The packet reads the HUD's data points, so the two cannot disagree.
	using namespace ApexTelemetryOutput;
	FApexHudData Hud;
	FPacket Empty;
	FillFromHud(Hud, Empty);
	TestFalse(TEXT("no car: CarPresent clear"), (Empty.Flags & CarPresent) != 0);

	Hud.Set(TEXT("car.present"), true);
	Hud.Set(TEXT("car.speed_kph"), 180.0);
	Hud.Set(TEXT("car.gear"), 5);
	Hud.Set(TEXT("car.rpm"), 10000.0);
	Hud.Set(TEXT("car.rpm_fraction"), 0.9);
	Hud.Set(TEXT("car.brake"), 0.5);
	Hud.Set(TEXT("ers.charge_pct"), 40.0);
	Hud.SetNone(TEXT("fuel.liters"));
	Hud.Set(TEXT("flag.yellow"), 2);
	Hud.Set(TEXT("pit.in_lane"), true);
	Hud.Set(TEXT("race.position"), 4);
	Hud.Set(TEXT("lap.limit"), -1);
	TArray<FApexHudRecord>& Tyres = Hud.Lists.FindOrAdd(TEXT("tyres"));
	for (int32 Tyre = 0; Tyre < 4; ++Tyre)
	{
		FApexHudRecord& Row = Tyres.AddDefaulted_GetRef();
		Row.Add(TEXT("temp_c"), FApexHudValue::Of(80.0f + Tyre));
		Row.Add(TEXT("pressure_kpa"), FApexHudValue());
		Row.Add(TEXT("wear_pct"), FApexHudValue::Of(5.0f));
	}

	FPacket P;
	FillFromHud(Hud, P);
	TestTrue(TEXT("CarPresent"), (P.Flags & CarPresent) != 0);
	TestTrue(TEXT("a double yellow is a yellow"), (P.Flags & YellowFlag) != 0);
	TestTrue(TEXT("in the pit lane"), (P.Flags & InPitLane) != 0);
	TestFalse(TEXT("no blue"), (P.Flags & BlueFlag) != 0);
	TestEqual(TEXT("speed in m/s"), P.SpeedMps, 50.0f, 1e-3f);
	TestEqual(TEXT("gear"), P.Gear, 5);
	TestEqual(TEXT("rev lights"), P.RevFraction, 0.9f, 1e-6f);
	TestEqual(TEXT("ERS as a share"), P.ErsCharge, 0.4f, 1e-6f);
	TestEqual(TEXT("unknown fuel is -1"), P.FuelLiters, -1.0f);
	TestEqual(TEXT("RR tread"), P.TyreTempC[3], 83.0f);
	TestEqual(TEXT("unknown pressure is -1"), P.TyrePressureKpa[0], -1.0f);
	TestEqual(TEXT("place"), P.Place, 4);
	TestEqual(TEXT("no lap limit is 0"), P.LapLimit, 0);
	return true;
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexTelemetryOutputMotionTest,
	"ApexSim.TelemetryOutput.Motion",
	ApexTestFlags)

bool FApexTelemetryOutputMotionTest::RunTest(const FString& Parameters)
{
	using namespace ApexTelemetryOutput;

	// The car's axes: yaw from +x toward +y, nose up lifts forward, a right
	// lean tips the roof to the right.
	FVector3f Forward, Left, Up;
	CarAxes(FVector3f(HALF_PI, 0.0f, 0.0f), Forward, Left, Up);
	TestTrue(TEXT("yaw 90: forward is +y"), Forward.Equals(FVector3f(0.0f, 1.0f, 0.0f), 1e-5f));
	TestTrue(TEXT("yaw 90: left is -x"), Left.Equals(FVector3f(-1.0f, 0.0f, 0.0f), 1e-5f));
	CarAxes(FVector3f(0.0f, 0.1f, 0.0f), Forward, Left, Up);
	TestTrue(TEXT("nose up: forward climbs"), Forward.Z > 0.09f);
	TestTrue(TEXT("nose up: the roof leans back"), Up.X < -0.09f);
	CarAxes(FVector3f(0.0f, 0.0f, 0.1f), Forward, Left, Up);
	TestTrue(TEXT("leaning right: the roof goes right"), Up.Y < -0.09f);
	TestTrue(TEXT("leaning right: the left side lifts"), Left.Z > 0.09f);

	// A left-hander of 100 m at 30 m/s, sampled as the server sends it: every
	// seventh tick of 420 Hz, arriving 60 times a second with some jitter.
	const float Radius = 100.0f;
	const float Speed = 30.0f;
	const double TickS = 1.0 / 420.0;
	FMotionEstimator Motion;
	for (int32 Frame = 0; Frame < 240; ++Frame)
	{
		const int64 Tick = 1000 + Frame * 7;
		const double T = (Tick - 1000) * TickS;
		const float Angle = static_cast<float>(Speed * T / Radius);
		const FVector3f Position(Radius * FMath::Sin(Angle), Radius * (1.0f - FMath::Cos(Angle)), 2.0f);
		const double Jitter = (Frame % 3 == 0 ? 0.004 : -0.002);
		Motion.AddSample(Tick, 50.0 + T + Jitter, Position, FVector3f(FMath::UnwindRadians(Angle), 0.0f, 0.0f));
	}
	TestTrue(TEXT("motion known"), Motion.HasMotion());
	TestEqual(TEXT("the tick's length learnt"), Motion.SecondsPerTick(), TickS, TickS * 0.05);
	FPacket P;
	Motion.Fill(P);
	TestTrue(TEXT("MotionValid"), (P.Flags & MotionValid) != 0);
	TestEqual(TEXT("forward speed"), P.LocalVelocity.X, Speed, 0.5f);
	TestEqual(TEXT("no sideways speed"), P.LocalVelocity.Y, 0.0f, 0.5f);
	TestEqual(TEXT("sway: v^2/r to the left"), P.LocalAccel.Y, Speed * Speed / Radius, 0.6f);
	TestEqual(TEXT("no surge at a steady speed"), P.LocalAccel.X, 0.0f, 0.6f);
	TestEqual(TEXT("yaw rate v/r"), P.AngularRate.Z, Speed / Radius, 0.01f);

	// Braking in a straight line at 2 g, on a fresh estimator.
	FMotionEstimator Braking;
	for (int32 Frame = 0; Frame < 120; ++Frame)
	{
		const double T = Frame * 7 * TickS;
		const float X = static_cast<float>(60.0 * T - 0.5 * 19.62 * T * T);
		Braking.AddSample(Frame * 7, T, FVector3f(X, 0.0f, 0.0f), FVector3f::ZeroVector);
	}
	FPacket B;
	Braking.Fill(B);
	TestEqual(TEXT("surge: -2 g"), B.LocalAccel.X, -19.62f, 0.5f);

	// A teleport (recovery to the pits) starts again rather than spiking.
	Braking.AddSample(120 * 7, 120 * 7 * TickS, FVector3f(5000.0f, 0.0f, 0.0f), FVector3f::ZeroVector);
	TestFalse(TEXT("a 5 km jump restarts the motion"), Braking.HasMotion());
	return true;
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexTelemetryOutputSettingsTest,
	"ApexSim.TelemetryOutput.Settings",
	ApexTestFlags)

bool FApexTelemetryOutputSettingsTest::RunTest(const FString& Parameters)
{
	// telemetry_out survives the game's rewrite of settings.yml, and is off by default.
	const FApexBootSettings Defaults;
	TestFalse(TEXT("off by default"), Defaults.TelemetryOut.bEnabled);
	const FString DefaultText = ApexBootSettingsIo::Serialise(Defaults);
	TestTrue(TEXT("the file has the section"), DefaultText.Contains(TEXT("telemetry_out:\n  #")));
	TestTrue(TEXT("and says it is off"), DefaultText.Contains(TEXT("  enabled: false\n")));

	FApexBootSettings Written;
	Written.TelemetryOut.bEnabled = true;
	Written.TelemetryOut.Host = TEXT("192.168.1.20");
	Written.TelemetryOut.Port = 20777;
	Written.TelemetryOut.RateHz = 120;
	FApexBootSettings Read;
	ApexBootSettingsIo::Parse(ApexBootSettingsIo::Serialise(Written), Read);
	TestTrue(TEXT("enabled survives"), Read.TelemetryOut.bEnabled);
	TestEqual(TEXT("host survives"), Read.TelemetryOut.Host, Written.TelemetryOut.Host);
	TestEqual(TEXT("port survives"), Read.TelemetryOut.Port, 20777);
	TestEqual(TEXT("rate survives"), Read.TelemetryOut.RateHz, 120);
	// And the server's own host is not the telemetry's.
	TestEqual(TEXT("the server host is its own"), Read.ServerHost, Written.ServerHost);

	FApexBootSettings Bad;
	ApexBootSettingsIo::Parse(TEXT("telemetry_out:\n  rate_hz: 5000\n"), Bad);
	TestEqual(TEXT("an out-of-range rate keeps the default"), Bad.TelemetryOut.RateHz, 60);
	return true;
}

#endif
