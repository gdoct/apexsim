#include "ApexTestCommon.h"
#include "Race/ApexRaceCoordinate.h"
#include "Race/ApexReplayCamera.h"
#include "Race/ApexReplayClip.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace ApexReplayTest
{
	/** One car's positional array, as `apexsim-replay cut` writes it. */
	FString CarArray(float PosX, float PosY, float Heading, float Mps, int32 GearNo, int32 LapNo, float StationM)
	{
		return FString::Printf(TEXT("[%g,%g,0.5,%g,0,0,%g,1,0,0.1,%d,9000,%d,%g,1,0]"),
			PosX, PosY, Heading, Mps, GearNo, LapNo, StationM);
	}

	/**
	 * Two cars on a 1 km lap, three frames 4 ticks apart (60 Hz at 240):
	 * car 0 crosses the line and the yaw seam between frames 1 and 2.
	 */
	FString TwoCarClip()
	{
		const float Pi = PI;
		return FString::Printf(TEXT(R"({
			"format": "apexsim-clip", "version": 1,
			"track_name": "Ring", "track_stem": "Ring", "track_id": "abc",
			"track_length_m": 1000, "weather": 3, "time_of_day_minutes": 1290,
			"tick_rate": 240, "race_start_tick": 100,
			"cars": [
				{"index": 0, "name": "Luna Swift", "car_config_id": "car-a", "livery": 2},
				{"index": 1, "name": "Kai Storm", "car_config_id": "car-b", "livery": 0}
			],
			"centerline": [[0,0],[10,0],[20,5]],
			"frames": [
				{"tick": 1000, "state": 2, "countdown_ms": null, "cars": [%s, %s]},
				{"tick": 1004, "state": 2, "countdown_ms": null, "cars": [%s, %s]},
				{"tick": 1008, "state": 2, "countdown_ms": null, "cars": [%s, %s]}
			]
		})"),
			*CarArray(0, 0, Pi - 0.1f, 50, 4, 1, 990), *CarArray(-20, 0, 0, 48, 4, 1, 970),
			*CarArray(1, 0, Pi - 0.05f, 51, 4, 1, 999), *CarArray(-19, 0, 0, 48, 4, 1, 971),
			*CarArray(2, 0, -Pi + 0.05f, 52, 5, 2, 8), *CarArray(-18, 0, 0, 48, 4, 1, 972));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexReplayClipParseTest, "ApexSim.Replay.Clip.Parse", ApexTestFlags)

bool FApexReplayClipParseTest::RunTest(const FString& Parameters)
{
	FApexReplayClip Clip;
	FString Error;
	if (!Clip.LoadFromString(ApexReplayTest::TwoCarClip(), Error))
	{
		AddError(FString::Printf(TEXT("the clip did not load: %s"), *Error));
		return false;
	}
	TestEqual(TEXT("stem"), Clip.GetTrackStem(), FString(TEXT("Ring")));
	TestEqual(TEXT("frames"), Clip.NumFrames(), 3);
	TestEqual(TEXT("cars"), Clip.GetCars().Num(), 2);
	TestEqual(TEXT("centerline"), Clip.GetCenterline().Num(), 3);
	TestNearlyEqual(TEXT("8 ticks at 240 Hz"), Clip.GetDurationSeconds(), 8.0 / 240.0, 1e-9);
	TestEqual(TEXT("weather"), Clip.GetConditions().Weather, EApexWeather::LightRain);
	TestEqual(TEXT("time of day"), Clip.GetConditions().TimeOfDayMinutes, 1290);

	const FApexSessionRoster Roster = Clip.MakeRoster();
	TestEqual(TEXT("roster size"), Roster.Entries.Num(), 2);
	TestEqual(TEXT("car id"), Roster.Entries[0].CarConfigId, FString(TEXT("car-a")));
	TestEqual(TEXT("livery"), Roster.Entries[0].Livery, 2);
	TestEqual(TEXT("name"), Roster.Entries[1].PlayerName, FString(TEXT("Kai Storm")));
	TestTrue(TEXT("AI"), Roster.Entries[1].bIsAi);

	FApexReplayClip Bad;
	TestFalse(TEXT("not a clip"), Bad.LoadFromString(TEXT("{\"format\": \"something-else\"}"), Error));
	TestFalse(TEXT("not JSON"), Bad.LoadFromString(TEXT("nope"), Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexReplayClipSampleTest, "ApexSim.Replay.Clip.Sample", ApexTestFlags)

bool FApexReplayClipSampleTest::RunTest(const FString& Parameters)
{
	FApexReplayClip Clip;
	FString Error;
	if (!Clip.LoadFromString(ApexReplayTest::TwoCarClip(), Error))
	{
		AddError(Error);
		return false;
	}
	const double FrameSeconds = 4.0 / 240.0;

	FApexTelemetryFrame Frame;
	Clip.SampleAt(0.0, Frame);
	TestEqual(TEXT("both cars"), Frame.Cars.Num(), 2);
	TestEqual(TEXT("racing"), Frame.SessionState, EApexSessionState::Racing);
	TestNearlyEqual(TEXT("first frame x"), Frame.Cars[0].Position.X, 0.0, 1e-4);

	// Halfway between frames 0 and 1.
	Clip.SampleAt(FrameSeconds * 0.5, Frame);
	TestNearlyEqual(TEXT("position blends"), Frame.Cars[0].Position.X, 0.5, 1e-4);
	TestNearlyEqual(TEXT("speed blends"), Frame.Cars[0].SpeedMps, 50.5f, 1e-4f);
	TestNearlyEqual(TEXT("second car too"), Frame.Cars[1].Position.X, -19.5, 1e-4);

	// Halfway between frames 1 and 2: across the yaw seam and the line.
	Clip.SampleAt(FrameSeconds * 1.5, Frame);
	const FApexCarTelemetry& Car = Frame.Cars[0];
	TestTrue(FString::Printf(TEXT("yaw goes the short way (%f)"), Car.YawRad), FMath::Abs(FMath::Abs(Car.YawRad) - PI) < 0.01f);
	TestTrue(FString::Printf(TEXT("station blends across the line (%f)"), Car.TrackProgress),
		Car.TrackProgress > 999.0f || Car.TrackProgress < 8.0f);
	TestEqual(TEXT("gear is the earlier frame's"), Car.Gear, 4);
	TestEqual(TEXT("past the line, the lap is the new one"), Car.CurrentLap, 2);

	// 999 m to 8 m crosses the line a ninth of the way: a twentieth is short of it.
	Clip.SampleAt(FrameSeconds * 1.05, Frame);
	TestTrue(FString::Printf(TEXT("short of the line (%f)"), Frame.Cars[0].TrackProgress), Frame.Cars[0].TrackProgress > 999.0f);
	TestEqual(TEXT("so still lap 1"), Frame.Cars[0].CurrentLap, 1);
	TestFalse(TEXT("a car with a row is drawn"), Frame.Cars[0].bInGarage);

	// Past the end holds the last frame.
	Clip.SampleAt(10.0, Frame);
	TestNearlyEqual(TEXT("clamped to the end"), Frame.Cars[0].Position.X, 2.0, 1e-4);
	TestEqual(TEXT("last gear"), Frame.Cars[0].Gear, 5);

	TestEqual(TEXT("car 0 leads (lap 2 at the end)"), Clip.LeaderAt(10.0), 0);
	TestEqual(TEXT("nearest to (-18, 0)"), Clip.NearestCarTo(FVector(-18.0, 0.0, 0.0), 10.0), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexReplayCameraTest, "ApexSim.Replay.Camera", ApexTestFlags)

bool FApexReplayCameraTest::RunTest(const FString& Parameters)
{
	using namespace ApexReplayCam;

	EMode Mode = EMode::Tv;
	TestTrue(TEXT("pan parses"), ParseMode(TEXT("Pan"), Mode) && Mode == EMode::Pan);
	TestTrue(TEXT("onboard is the cockpit"), ParseMode(TEXT("onboard"), Mode) && Mode == EMode::Cockpit);
	TestFalse(TEXT("nonsense"), ParseMode(TEXT("drone"), Mode));

	EFollow Follow = EFollow::Leader;
	int32 Index = INDEX_NONE;
	TestTrue(TEXT("index"), ParseFollow(TEXT("3"), Follow, Index) && Follow == EFollow::Index && Index == 3);
	TestTrue(TEXT("nearest"), ParseFollow(TEXT("nearest"), Follow, Index) && Follow == EFollow::Nearest);
	ApexTv::EShot Shot = ApexTv::EShot::None;
	TestTrue(TEXT("tv shot by name"), ParseTvShot(TEXT("helicopter"), Shot) && Shot == ApexTv::EShot::Helicopter);

	// A pan with no bias looks at the car, led by its velocity.
	const FVector Eye(0.0, 0.0, 0.0);
	const FVector Car(10000.0, 0.0, 0.0);
	const FRotator Straight = PanRotation(Eye, Car, FVector::ZeroVector, nullptr, 0.0f, 0.2f);
	TestNearlyEqual(TEXT("at the car"), Straight.Yaw, 0.0, 1e-3);
	const FRotator Led = PanRotation(Eye, Car, FVector(0.0, 5000.0, 0.0), nullptr, 0.0f, 0.2f);
	TestTrue(TEXT("leads a car crossing the frame"), Led.Yaw > 4.0 && Led.Yaw < 7.0);

	// Biased halfway toward a landmark 90 degrees round: 45 degrees.
	const FVector Landmark(0.0, 10000.0, 0.0);
	const FRotator Half = PanRotation(Eye, Car, FVector::ZeroVector, &Landmark, 0.5f, 0.0f);
	TestNearlyEqual(TEXT("halfway to the landmark"), Half.Yaw, 45.0, 0.01);
	const FRotator All = PanRotation(Eye, Car, FVector::ZeroVector, &Landmark, 1.0f, 0.0f);
	TestNearlyEqual(TEXT("all the way"), All.Yaw, 90.0, 0.01);

	// 10 m across the frame 100 m away is 5.72 degrees; clamped at both ends.
	TestNearlyEqual(TEXT("framing"), FramingFov(10.0f, 10000.0, 1.0f, 90.0f), 5.72f, 0.01f);
	TestNearlyEqual(TEXT("no zoom"), FramingFov(0.0f, 10000.0, 1.0f, 50.0f), 50.0f, 1e-4f);
	TestNearlyEqual(TEXT("widest"), FramingFov(10.0f, 100.0, 1.0f, 60.0f), 60.0f, 1e-4f);
	TestNearlyEqual(TEXT("longest"), FramingFov(10.0f, 1.0e7, 3.0f, 60.0f), 3.0f, 1e-4f);

	// Easing is frame-rate independent: two half steps are one whole one.
	const float Whole = EaseAlpha(5.0f, 0.1f);
	const float HalfStep = EaseAlpha(5.0f, 0.05f);
	TestNearlyEqual(TEXT("ease composes"), 1.0f - (1.0f - HalfStep) * (1.0f - HalfStep), Whole, 1e-5f);
	return true;
}

#endif
