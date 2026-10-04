#include "ApexTestCommon.h"
#include "Guide/ApexGuidePlayer.h"
#include "Guide/ApexTrackGuide.h"
#include "Race/ApexReplayCamera.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace ApexGuideTest
{
	/** A guide as `apexsim-replay guide` writes it (docs/TRACK_GUIDE.md), with a key from the future. */
	const TCHAR* FullGuide = TEXT(R"({
		"version": 1,
		"class": "GT3",
		"display_class": "GT3",
		"recording": "Spa.GT3.guide.apxs",
		"source_crc": 1234567890,
		"car": { "id": "uuid-car", "folder": "posh-gt3rs", "name": "Posh GT3 RS" },
		"car_gap_s": 2.0,
		"some_future_key": { "nested": [1, 2, 3] },
		"track": {
			"stem": "Spa", "track_id": "uuid-track", "display_name": "Spa-Frankenchamps",
			"description": "Modelled on the circuit in the Ardennes.", "country": "Belgium", "city": "Stavelot",
			"category": "F1", "year_built": 1921,
			"length_m": 7004.0, "altitude_m": 401.0,
			"latitude_deg": 50.43, "longitude_deg": 5.97,
			"elevation_min_m": 0.0, "elevation_max_m": 102.0,
			"climb_m": 180.0,
			"corners": 19, "left": 9, "right": 10,
			"direction": "clockwise",
			"longest_straight_m": 1900.0,
			"drs_zones": 2,
			"lap_time_s": 138.4, "top_speed_kph": 268.0,
			"notes": ["A line of the circuit's character.", ""]
		},
		"overview": {
			"time_s": 14.2,
			"cameras": [ { "name": "Grandstand", "kind": "fixed", "eye": [10, 20, 5], "look": [0, 0, 0], "fov_deg": 40.0 } ]
		},
		"corners": [
			{
				"number": 2, "turn_from": 3, "turn_to": 4, "name": "Eau Rouge", "direction": "left-right",
				"entry_m": 900, "apex_m": 1000, "exit_m": 1100, "turn_deg": 80, "min_radius_m": 120,
				"clip": { "from_s": 40.0, "to_s": 50.0, "slow_from_s": 43.0, "slow_to_s": 47.0, "slow_rate": 0.5, "apex_s": 45.0 },
				"min_speed_kph": 240, "apex_gear": 6, "entry_speed_kph": 250, "exit_speed_kph": 245,
				"brake_m": null, "flat_out": true, "elevation_change_m": 35.0, "banking_deg": 0.0,
				"gotchas": ["Generated."], "notes": [], "cameras": []
			},
			{
				"number": 1, "turn_from": 1, "turn_to": 1, "name": "Sauce-Source", "direction": "right",
				"entry_m": 300.0, "apex_m": 410.0, "exit_m": 480.0, "turn_deg": 160.0, "min_radius_m": 25.0,
				"clip": { "from_s": 20.1, "to_s": 30.1, "slow_from_s": 23.5, "slow_to_s": 27.5, "slow_rate": 0.25, "apex_s": 25.4 },
				"min_speed_kph": 72.0, "apex_gear": 2, "entry_speed_kph": 245.0, "exit_speed_kph": 160.0,
				"brake_m": 120.0, "flat_out": false, "elevation_change_m": -8.0, "banking_deg": 1.5,
				"gotchas": ["Generated line.", "Hand line."],
				"notes": ["Hand line."],
				"cameras": [
					{ "name": "Trackside", "kind": "fixed", "eye": [400, 30, 3], "look": [410, 0, 0], "fov_deg": 35.0 },
					{ "name": "Broken", "kind": "fixed", "eye": [1, 2, 3] },
					{ "name": "Chase", "kind": "chase" }
				]
			}
		]
	})");
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexGuideParseFullTest, "ApexSim.Guide.Parse.Full", ApexTestFlags)

bool FApexGuideParseFullTest::RunTest(const FString& Parameters)
{
	FApexTrackGuide Guide;
	FString Error;
	if (!ApexTrackGuide::Parse(ApexGuideTest::FullGuide, Guide, Error))
	{
		AddError(FString::Printf(TEXT("the guide did not parse: %s"), *Error));
		return false;
	}
	TestEqual(TEXT("version"), Guide.Version, 1);
	TestEqual(TEXT("class"), Guide.Class, FString(TEXT("GT3")));
	TestEqual(TEXT("recording"), Guide.Recording, FString(TEXT("Spa.GT3.guide.apxs")));
	TestEqual(TEXT("crc"), Guide.SourceCrc, static_cast<int64>(1234567890));
	TestEqual(TEXT("car"), Guide.CarName, FString(TEXT("Posh GT3 RS")));
	TestEqual(TEXT("car folder"), Guide.CarFolder, FString(TEXT("posh-gt3rs")));
	TestEqual(TEXT("display name"), Guide.Track.DisplayName, FString(TEXT("Spa-Frankenchamps")));
	TestEqual(TEXT("turns"), Guide.Track.Corners, 19);
	TestTrue(TEXT("altitude"), Guide.Track.bHasAltitude && FMath::IsNearlyEqual(Guide.Track.AltitudeM, 401.0f));
	TestEqual(TEXT("year"), Guide.Track.YearBuilt, 1921);
	TestEqual(TEXT("empty notes dropped"), Guide.Track.Notes.Num(), 1);
	TestNearlyEqual(TEXT("overview time"), Guide.OverviewTimeS, 14.2, 1e-9);
	TestEqual(TEXT("overview camera"), Guide.OverviewCameras.Num(), 1);

	if (!TestEqual(TEXT("corners"), Guide.Corners.Num(), 2))
	{
		return false;
	}
	// In the order the lap runs, not the file's.
	const FApexGuideCorner& First = Guide.Corners[0];
	const FApexGuideCorner& Second = Guide.Corners[1];
	TestEqual(TEXT("sorted by clip"), First.Name, FString(TEXT("Sauce-Source")));
	TestEqual(TEXT("turn label"), ApexTrackGuide::TurnLabel(First), FString(TEXT("TURN 1")));
	TestEqual(TEXT("turns label"), ApexTrackGuide::TurnLabel(Second), FString(TEXT("TURNS 3\u20134")));
	TestEqual(TEXT("chicane"), ApexTrackGuide::DirectionLabel(Second.Direction), FString(TEXT("LEFT-RIGHT")));
	TestTrue(TEXT("brake given"), First.bHasBrake && FMath::IsNearlyEqual(First.BrakeM, 120.0f));
	TestFalse(TEXT("null brake"), Second.bHasBrake);
	TestTrue(TEXT("flat out"), Second.bFlatOut);
	TestEqual(TEXT("brake text"), ApexTrackGuide::BrakeText(First), FString(TEXT("Brake 120 m before the apex")));
	TestEqual(TEXT("flat text"), ApexTrackGuide::BrakeText(Second), FString(TEXT("Flat out")));
	TestNearlyEqual(TEXT("slow from"), First.SlowFromS, 23.5, 1e-9);
	TestNearlyEqual(TEXT("slow rate"), First.SlowRate, 0.25f, 1e-6f);
	TestNearlyEqual(TEXT("apex time"), First.ApexS, 25.4, 1e-9);
	TestEqual(TEXT("gear"), First.ApexGear, 2);
	// A tripod with no look point, and a car camera (the client's own), are dropped.
	if (TestEqual(TEXT("cameras"), First.Cameras.Num(), 1))
	{
		TestEqual(TEXT("camera name"), First.Cameras[0].Name, FString(TEXT("Trackside")));
		TestEqual(TEXT("camera eye"), First.Cameras[0].EyeM, FVector(400.0, 30.0, 3.0));
		TestNearlyEqual(TEXT("camera fov"), First.Cameras[0].FovDeg, 35.0f, 1e-6f);
	}
	// Notes first, and a line in both lists once.
	const TArray<FString> Lines = ApexTrackGuide::CornerLines(First);
	TestEqual(TEXT("lines"), Lines.Num(), 2);
	TestEqual(TEXT("notes first"), Lines[0], FString(TEXT("Hand line.")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexGuideParseMinimalTest, "ApexSim.Guide.Parse.Minimal", ApexTestFlags)

bool FApexGuideParseMinimalTest::RunTest(const FString& Parameters)
{
	FApexTrackGuide Guide;
	FString Error;
	if (!TestTrue(TEXT("bare guide"), ApexTrackGuide::Parse(TEXT(R"({"version": 1, "recording": "x.apxs"})"), Guide, Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("no corners"), Guide.Corners.Num(), 0);
	TestFalse(TEXT("no altitude"), Guide.Track.bHasAltitude);
	TestEqual(TEXT("no lap time"), ApexTrackGuide::LapTimeText(Guide.Track.LapTimeS), FString(TEXT("\u2014")));

	// Missing names, turns and slow parts; a clip given backwards.
	const TCHAR* Sparse = TEXT(R"({"version": 2, "recording": "y.apxs", "corners": [
		{"number": 3, "clip": {"from_s": 30, "to_s": 20, "slow_from_s": 5, "slow_to_s": 99}},
		{"number": 4, "turn_from": 5, "turn_to": 6, "clip": {"from_s": 40, "to_s": 50}},
		{"number": 5}
	]})");
	if (!TestTrue(TEXT("sparse guide"), ApexTrackGuide::Parse(Sparse, Guide, Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("a newer version still reads"), Guide.Version, 2);
	if (!TestEqual(TEXT("corners"), Guide.Corners.Num(), 3))
	{
		return false;
	}
	const FApexGuideCorner* Three = Guide.Corners.FindByPredicate([](const FApexGuideCorner& C) { return C.Number == 3; });
	const FApexGuideCorner* Four = Guide.Corners.FindByPredicate([](const FApexGuideCorner& C) { return C.Number == 4; });
	if (!Three || !Four)
	{
		AddError(TEXT("corners lost"));
		return false;
	}
	TestEqual(TEXT("fallback name"), Three->Name, FString(TEXT("Turn 3")));
	TestEqual(TEXT("fallback chicane name"), Four->Name, FString(TEXT("Turns 5-6")));
	TestNearlyEqual(TEXT("from before to"), Three->FromS, 20.0, 1e-9);
	TestNearlyEqual(TEXT("to"), Three->ToS, 30.0, 1e-9);
	TestTrue(TEXT("slow part inside the loop"), Three->SlowFromS >= Three->FromS && Three->SlowToS <= Three->ToS);
	TestNearlyEqual(TEXT("no slow part"), Four->SlowToS - Four->SlowFromS, 0.0, 1e-9);
	TestEqual(TEXT("turns from the stops"), Guide.Track.Corners, 6);
	TestEqual(TEXT("no brake, not flat"), ApexTrackGuide::BrakeText(*Three), FString());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexGuideParseBadTest, "ApexSim.Guide.Parse.Bad", ApexTestFlags)

bool FApexGuideParseBadTest::RunTest(const FString& Parameters)
{
	FApexTrackGuide Guide;
	FString Error;
	TestFalse(TEXT("not JSON"), ApexTrackGuide::Parse(TEXT("nope"), Guide, Error));
	TestFalse(TEXT("empty"), ApexTrackGuide::Parse(FString(), Guide, Error));
	TestFalse(TEXT("no version"), ApexTrackGuide::Parse(TEXT(R"({"recording": "x.apxs"})"), Guide, Error));
	TestFalse(TEXT("version zero"), ApexTrackGuide::Parse(TEXT(R"({"version": 0, "recording": "x.apxs"})"), Guide, Error));
	TestFalse(TEXT("no recording"), ApexTrackGuide::Parse(TEXT(R"({"version": 1})"), Guide, Error));
	TestFalse(TEXT("an array"), ApexTrackGuide::Parse(TEXT("[1, 2]"), Guide, Error));
	TestTrue(TEXT("a reason"), !Error.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexGuideFilesTest, "ApexSim.Guide.Files", ApexTestFlags)

bool FApexGuideFilesTest::RunTest(const FString& Parameters)
{
	FString Stem;
	FString Class;
	TestTrue(TEXT("a guide"), ApexTrackGuide::ParseFileName(TEXT("C:/x/build/guide/Spa.GT3.guide.json"), Stem, Class));
	TestEqual(TEXT("stem"), Stem, FString(TEXT("Spa")));
	TestEqual(TEXT("class"), Class, FString(TEXT("GT3")));
	TestTrue(TEXT("any case"), ApexTrackGuide::ParseFileName(TEXT("RedBullRing.Hypercar.GUIDE.JSON"), Stem, Class));
	TestEqual(TEXT("class 2"), Class, FString(TEXT("Hypercar")));
	TestFalse(TEXT("the recording"), ApexTrackGuide::ParseFileName(TEXT("Spa.GT3.guide.apxs"), Stem, Class));
	TestFalse(TEXT("no class"), ApexTrackGuide::ParseFileName(TEXT("Spa.guide.json"), Stem, Class));
	TestFalse(TEXT("a track export"), ApexTrackGuide::ParseFileName(TEXT("Spa.uescene.json"), Stem, Class));

	const TArray<FString> All = { TEXT("GT3"), TEXT("LMP2"), TEXT("F1"), TEXT("Hypercar") };
	TestEqual(TEXT("the picked car's class"), ApexTrackGuide::ChooseClass(All, TEXT("lmp2")), FString(TEXT("LMP2")));
	TestEqual(TEXT("F1 first"), ApexTrackGuide::ChooseClass(All, FString()), FString(TEXT("F1")));
	TestEqual(TEXT("a class the track lacks"), ApexTrackGuide::ChooseClass({ TEXT("GT3"), TEXT("Hypercar") }, TEXT("F1")), FString(TEXT("Hypercar")));
	TestEqual(TEXT("then LMP2"), ApexTrackGuide::ChooseClass({ TEXT("GT3"), TEXT("LMP2") }, FString()), FString(TEXT("LMP2")));
	TestEqual(TEXT("then GT3"), ApexTrackGuide::ChooseClass({ TEXT("Zeta"), TEXT("GT3") }, FString()), FString(TEXT("GT3")));
	TestEqual(TEXT("then by name"), ApexTrackGuide::ChooseClass({ TEXT("Zeta"), TEXT("Alpha") }, FString()), FString(TEXT("Alpha")));
	TestEqual(TEXT("none"), ApexTrackGuide::ChooseClass({}, TEXT("GT3")), FString());
	return true;
}

namespace ApexGuideTest
{
	/** Two corners on a 120 s recording: one at 20-30 s (slow 23-27 at 0.25x), one far round at 90-100 s. */
	void SetupTwo(ApexGuide::FPlayer& Player)
	{
		ApexGuide::FLoop A;
		A.FromS = 20.0;
		A.ToS = 30.0;
		A.SlowFromS = 23.0;
		A.SlowToS = 27.0;
		A.SlowRate = 0.25f;
		ApexGuide::FLoop B;
		B.FromS = 90.0;
		B.ToS = 100.0;
		B.SlowFromS = 94.0;
		B.SlowToS = 96.0;
		B.SlowRate = 0.5f;
		Player.Setup(10.0, { A, B }, 120.0);
	}

	void Run(ApexGuide::FPlayer& Player, double Seconds, double Step = 1.0 / 60.0)
	{
		for (double T = 0.0; T < Seconds - 1e-9; T += Step)
		{
			Player.Tick(Step);
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexGuidePlayerLoopTest, "ApexSim.Guide.Player.Loop", ApexTestFlags)

bool FApexGuidePlayerLoopTest::RunTest(const FString& Parameters)
{
	using namespace ApexGuide;
	FPlayer Player;
	ApexGuideTest::SetupTwo(Player);
	TestEqual(TEXT("opens on the overview"), (int32)(Player.GetPhase()), (int32)EPhase::Overview);
	TestNearlyEqual(TEXT("frozen at the line"), Player.GetTime(), 10.0, 1e-9);
	TestTrue(TEXT("the first frame is a cut"), Player.ConsumeCut());
	TestFalse(TEXT("once"), Player.ConsumeCut());
	ApexGuideTest::Run(Player, 2.0);
	TestNearlyEqual(TEXT("the overview holds"), Player.GetTime(), 10.0, 1e-9);
	TestEqual(TEXT("still"), Player.GetRate(), 0.0f);

	// Next: 10 s of clip to corner 1 at 6x is under 4 s, so it runs.
	Player.Next();
	TestEqual(TEXT("travelling"), (int32)(Player.GetPhase()), (int32)EPhase::Travel);
	TestEqual(TEXT("to corner 1"), Player.GetCorner(), 0);
	TestFalse(TEXT("a run, not a cut"), Player.ConsumeCut());
	TestEqual(TEXT("fast"), Player.GetRate(), 6.0f);
	ApexGuideTest::Run(Player, 1.0);
	TestNearlyEqual(TEXT("six seconds of clip in one"), Player.GetTime(), 16.0, 0.05);
	ApexGuideTest::Run(Player, 1.0);
	TestEqual(TEXT("arrived"), (int32)(Player.GetPhase()), (int32)EPhase::Normal);
	TestNearlyEqual(TEXT("at the loop's start"), Player.GetTime(), 20.0 + (2.0 - 10.0 / 6.0), 0.05);
	TestFalse(TEXT("arriving is not a cut"), Player.ConsumeCut());

	// The normal pass ends at 30 s and the slow part takes over at 23 s.
	ApexGuideTest::Run(Player, 10.0 - (2.0 - 10.0 / 6.0) + 0.05);
	TestEqual(TEXT("slow motion"), (int32)(Player.GetPhase()), (int32)EPhase::Slow);
	TestTrue(TEXT("the seam is a cut"), Player.ConsumeCut());
	TestNearlyEqual(TEXT("from the slow part's start"), Player.GetTime(), 23.0, 0.05);
	TestEqual(TEXT("quarter speed"), Player.GetRate(), 0.25f);
	ApexGuideTest::Run(Player, 4.0);
	TestNearlyEqual(TEXT("a second of clip in four"), Player.GetTime(), 24.0, 0.05);

	// Paused, nothing moves.
	Player.TogglePause();
	const double Held = Player.GetTime();
	ApexGuideTest::Run(Player, 3.0);
	TestNearlyEqual(TEXT("paused"), Player.GetTime(), Held, 1e-9);
	TestEqual(TEXT("paused rate"), Player.GetRate(), 0.0f);
	Player.TogglePause();

	// 3 s of clip left at 0.25x, then round again from the top.
	ApexGuideTest::Run(Player, 12.1);
	TestEqual(TEXT("normal again"), (int32)(Player.GetPhase()), (int32)EPhase::Normal);
	TestEqual(TEXT("one loop"), Player.GetLoopCount(), 1);
	TestTrue(TEXT("a cut back to the start"), Player.ConsumeCut());
	TestTrue(TEXT("near the start"), Player.GetTime() < 20.2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexGuidePlayerTravelTest, "ApexSim.Guide.Player.Travel", ApexTestFlags)

bool FApexGuidePlayerTravelTest::RunTest(const FString& Parameters)
{
	using namespace ApexGuide;
	FPlayer Player;
	ApexGuideTest::SetupTwo(Player);
	TestFalse(TEXT("nothing before the overview"), Player.Previous());

	Player.GoToCorner(0, /*bCut*/ true);
	Player.ConsumeCut();
	TestEqual(TEXT("cut straight in"), (int32)(Player.GetPhase()), (int32)EPhase::Normal);

	// Corner 2 is 70 s on: jumped to 24 s short of it (6x for 4 s), then run.
	Player.Next();
	TestEqual(TEXT("travelling"), (int32)(Player.GetPhase()), (int32)EPhase::Travel);
	TestTrue(TEXT("a long way is cut short"), Player.ConsumeCut());
	TestNearlyEqual(TEXT("24 s short"), Player.GetTime(), 90.0 - 24.0, 1e-6);
	ApexGuideTest::Run(Player, 4.05);
	TestEqual(TEXT("there in four seconds"), (int32)(Player.GetPhase()), (int32)EPhase::Normal);
	TestEqual(TEXT("corner 2"), Player.GetCorner(), 1);

	// Back is always a cut, to the corner's own start.
	TestTrue(TEXT("back"), Player.Previous());
	TestTrue(TEXT("a cut"), Player.ConsumeCut());
	TestEqual(TEXT("corner 1"), Player.GetCorner(), 0);
	TestNearlyEqual(TEXT("its start"), Player.GetTime(), 20.0, 1e-9);
	TestEqual(TEXT("playing"), (int32)(Player.GetPhase()), (int32)EPhase::Normal);
	TestTrue(TEXT("back from corner 1"), Player.Previous());
	TestEqual(TEXT("the overview"), (int32)(Player.GetPhase()), (int32)EPhase::Overview);

	// A corner just ahead is a cut rather than a blink of fast-forward.
	FPlayer Near;
	{
		ApexGuide::FLoop A;
		A.FromS = 20.0;
		A.ToS = 30.0;
		ApexGuide::FLoop B;
		B.FromS = 31.0;
		B.ToS = 40.0;
		Near.Setup(0.0, { A, B }, 60.0);
		Near.GoToCorner(0, true);
		Near.ConsumeCut();
		ApexGuideTest::Run(Near, 9.5);
		Near.Next();
		TestEqual(TEXT("next door: straight there"), (int32)(Near.GetPhase()), (int32)EPhase::Normal);
		TestTrue(TEXT("by a cut"), Near.ConsumeCut());
		TestFalse(TEXT("no slow part, no slow phase"), Near.GetPhase() == EPhase::Slow);
		ApexGuideTest::Run(Near, 9.2);
		TestEqual(TEXT("loops without one"), Near.GetLoopCount(), 1);
		TestEqual(TEXT("still normal"), (int32)(Near.GetPhase()), (int32)EPhase::Normal);
	}

	// Past the last corner is the overview again.
	Player.GoToCorner(1, true);
	Player.Next();
	TestEqual(TEXT("round to the overview"), (int32)(Player.GetPhase()), (int32)EPhase::Overview);
	TestEqual(TEXT("no corner"), Player.GetCorner(), INDEX_NONE);
	// Navigating plays: a paused guide does not move to a frozen corner.
	Player.SetPaused(true);
	Player.Next();
	TestFalse(TEXT("unpaused by moving on"), Player.IsPaused());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexGuideKeysTest, "ApexSim.Guide.Keys", ApexTestFlags)

bool FApexGuideKeysTest::RunTest(const FString& Parameters)
{
	using namespace ApexGuide;
	TestEqual(TEXT("left"), (int32)(CommandFor(EKeys::Left).Action), (int32)EAction::Previous);
	TestEqual(TEXT("page up"), (int32)(CommandFor(EKeys::PageUp).Action), (int32)EAction::Previous);
	TestEqual(TEXT("pad left shoulder"), (int32)(CommandFor(EKeys::Gamepad_LeftShoulder).Action), (int32)EAction::Previous);
	TestEqual(TEXT("right"), (int32)(CommandFor(EKeys::Right).Action), (int32)EAction::Next);
	TestEqual(TEXT("d-pad right"), (int32)(CommandFor(EKeys::Gamepad_DPad_Right).Action), (int32)EAction::Next);
	TestEqual(TEXT("C"), (int32)(CommandFor(EKeys::C).Action), (int32)EAction::Camera);
	TestEqual(TEXT("shift C"), (int32)(CommandFor(EKeys::C, true).Action), (int32)EAction::CameraBack);
	TestEqual(TEXT("pad Y"), (int32)(CommandFor(EKeys::Gamepad_FaceButton_Top).Action), (int32)EAction::Camera);
	TestEqual(TEXT("space"), (int32)(CommandFor(EKeys::SpaceBar).Action), (int32)EAction::Pause);
	TestEqual(TEXT("pad A"), (int32)(CommandFor(EKeys::Gamepad_FaceButton_Bottom).Action), (int32)EAction::Pause);
	TestEqual(TEXT("escape"), (int32)(CommandFor(EKeys::Escape).Action), (int32)EAction::Leave);
	TestEqual(TEXT("backspace"), (int32)(CommandFor(EKeys::BackSpace).Action), (int32)EAction::Leave);
	TestEqual(TEXT("pad B"), (int32)(CommandFor(EKeys::Gamepad_FaceButton_Right).Action), (int32)EAction::Leave);
	TestEqual(TEXT("home"), (int32)(CommandFor(EKeys::Home).Action), (int32)EAction::Overview);
	const FCommand Three = CommandFor(EKeys::Three);
	TestEqual(TEXT("3"), (int32)(Three.Action), (int32)EAction::Corner);
	TestEqual(TEXT("corner 3"), Three.Corner, 3);
	TestEqual(TEXT("0 is ten"), CommandFor(EKeys::Zero).Corner, 10);
	TestEqual(TEXT("anything else"), (int32)(CommandFor(EKeys::W).Action), (int32)EAction::None);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexGuideTextTest, "ApexSim.Guide.Text", ApexTestFlags)

bool FApexGuideTextTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("km/h"), ApexTrackGuide::SpeedText(72.4f, true), FString(TEXT("72 km/h")));
	TestEqual(TEXT("mph"), ApexTrackGuide::SpeedText(100.0f, false), FString(TEXT("62 mph")));
	TestEqual(TEXT("km"), ApexTrackGuide::DistanceText(7004.0f, true), FString(TEXT("7.004 km")));
	TestEqual(TEXT("m"), ApexTrackGuide::DistanceText(420.0f, true), FString(TEXT("420 m")));
	TestEqual(TEXT("signed up"), ApexTrackGuide::HeightText(8.0f, true, true), FString(TEXT("+8 m")));
	TestEqual(TEXT("signed down"), ApexTrackGuide::HeightText(-12.2f, true, true), FString(TEXT("-12 m")));
	TestEqual(TEXT("lap"), ApexTrackGuide::LapTimeText(138.4f), FString(TEXT("2:18.400")));
	TestEqual(TEXT("clockwise"), ApexTrackGuide::TrackDirectionLabel(TEXT("clockwise")), FString(TEXT("Clockwise")));
	TestEqual(TEXT("anticlockwise"), ApexTrackGuide::TrackDirectionLabel(TEXT("anticlockwise")), FString(TEXT("Anticlockwise")));

	// A tripod on its corner turns toward the car, but no further than it may.
	const FVector Eye(0.0, 0.0, 0.0);
	const FVector Look(1000.0, 0.0, 0.0);
	const FRotator Near = ApexReplayCam::ClampedPanRotation(Eye, Look, FVector(1000.0, 100.0, 0.0), 10.0f);
	TestNearlyEqual(TEXT("follows a car near the corner"), Near.Yaw, FMath::RadiansToDegrees(FMath::Atan2(100.0, 1000.0)), 0.01);
	const FRotator Far = ApexReplayCam::ClampedPanRotation(Eye, Look, FVector(0.0, 1000.0, 0.0), 10.0f);
	TestNearlyEqual(TEXT("lets go of a car far off"), Far.Yaw, 10.0, 0.01);
	const FRotator Behind = ApexReplayCam::ClampedPanRotation(Eye, Look, FVector(-1000.0, 10.0, 0.0), 10.0f);
	TestTrue(TEXT("never past the clamp"), FMath::Abs(Behind.Yaw) <= 10.01);
	TestEqual(TEXT("level"), Near.Roll, 0.0);

	// Clear of the card: a tenth of a 60-degree lens puts the subject 6 degrees right, by turning left.
	TestNearlyEqual(TEXT("frame bias"), ApexReplayCam::FrameYawOffset(0.10f, 60.0f), -6.0f, 1e-4f);
	TestNearlyEqual(TEXT("centred"), ApexReplayCam::FrameYawOffset(0.0f, 60.0f), 0.0f, 1e-6f);
	return true;
}

#endif
