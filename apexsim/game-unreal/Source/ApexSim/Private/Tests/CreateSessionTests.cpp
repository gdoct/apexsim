#include "ApexTestCommon.h"
#include "UI/ApexCreateSessionModel.h"
#include "ApexMenuFlowSubsystem.h"
#include "Engine/GameInstance.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	FApexSessionConditions CreateAt(EApexWeather Weather, int32 Hour, int32 Minute = 0)
	{
		FApexSessionConditions C;
		C.Weather = Weather;
		C.TimeOfDayMinutes = Hour * 60 + Minute;
		return C;
	}
}

// -----------------------------------------------------------------------------
// The start order: references the server resolves, normalised so the preview
// shows what it will seat.
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexCreateSessionStartOrderTest,
	"ApexSim.UI.CreateSession.StartOrder",
	ApexTestFlags)

bool FApexCreateSessionStartOrderTest::RunTest(const FString& Parameters)
{
	using namespace ApexCreateSession;

	const TArray<FString> Seating = DefaultOrder(3);
	TestEqual(TEXT("the seating order: the AI, then the host"),
		FString::Join(Seating, TEXT(",")), FString(TEXT("@ai:1,@ai:2,@ai:3,@host")));
	TestTrue(TEXT("nothing asked is the seating order"), IsDefaultOrder({}, 3, 8));
	TestTrue(TEXT("the seating order spelled out is too"), IsDefaultOrder(Seating, 3, 8));

	// A listed driver leads; the others follow in seating order.
	TArray<FString> Custom = Normalise({ TEXT("@host"), TEXT("@ai:2") }, 3, 8);
	TestEqual(TEXT("normalised"), FString::Join(Custom, TEXT(",")), FString(TEXT("@host,@ai:2,@ai:1,@ai:3")));
	TestFalse(TEXT("a changed order is not the default"), IsDefaultOrder(Custom, 3, 8));

	// What cannot match is dropped, repeats too; unknown @ references too.
	TestEqual(TEXT("junk is dropped"),
		FString::Join(Normalise({ TEXT("@ai:9"), TEXT(""), TEXT("@ai:1"), TEXT("@ai:1"), TEXT("@nope") }, 2, 8), TEXT(",")),
		FString(TEXT("@ai:1,@ai:2,@host")));

	// A named driver (from a loaded result) keeps its place.
	TestEqual(TEXT("a name leads"),
		FString::Join(Normalise({ TEXT("Alice"), TEXT("@host") }, 1, 8), TEXT(",")),
		FString(TEXT("Alice,@host,@ai:1")));

	// The field cuts the list, and the host is never cut.
	TestEqual(TEXT("cut to the field"),
		FString::Join(Normalise({ TEXT("@ai:1"), TEXT("@ai:2"), TEXT("@ai:3") }, 3, 2), TEXT(",")),
		FString(TEXT("@ai:1,@host")));

	const TArray<FGridEntry> Slots = ApexCreateSession::Entries(Custom);
	TestTrue(TEXT("P1 is you"), Slots[0].bYou);
	TestEqual(TEXT("P2's label"), Slots[1].Label, FString(TEXT("AI 2")));
	TestTrue(TEXT("P2 is AI"), Slots[1].bAi);
	TestEqual(TEXT("a name is its own label"), ApexCreateSession::Entries({ TEXT("Alice") })[0].Label, FString(TEXT("Alice")));

	// Moving an entry shifts the ones between.
	TArray<FString> Moving = Seating;
	TestTrue(TEXT("the host to pole"), MoveEntry(Moving, 3, 0));
	TestEqual(TEXT("moved"), FString::Join(Moving, TEXT(",")), FString(TEXT("@host,@ai:1,@ai:2,@ai:3")));
	TestTrue(TEXT("down the grid"), MoveEntry(Moving, 0, 2));
	TestEqual(TEXT("moved down"), FString::Join(Moving, TEXT(",")), FString(TEXT("@ai:1,@ai:2,@host,@ai:3")));
	TestFalse(TEXT("to where it is"), MoveEntry(Moving, 2, 2));
	TestFalse(TEXT("from nowhere"), MoveEntry(Moving, 9, 0));
	TestTrue(TEXT("past the end clamps"), MoveEntry(Moving, 0, 99));
	TestEqual(TEXT("clamped to last"), Moving.Last(), FString(TEXT("@ai:1")));

	// A stored result: the host by name, its AI by rank, other humans by name.
	FApexQualifyingResult Result;
	auto Add = [&Result](const TCHAR* Name, bool bAi)
	{
		FApexQualifyingEntry& Entry = Result.Entries.AddDefaulted_GetRef();
		Entry.Name = Name;
		Entry.bIsAi = bAi;
	};
	Add(TEXT("Luna Swift"), true);
	Add(TEXT("Guido"), false);
	Add(TEXT("Max Voltage"), true);
	Add(TEXT("Kai Storm"), true);
	Add(TEXT("Bob"), false);
	TestEqual(TEXT("from a result"),
		FString::Join(OrderFromResult(Result, TEXT("Guido"), 2), TEXT(",")),
		FString(TEXT("@ai:1,@host,@ai:2,Bob")));
	TestEqual(TEXT("normalised for the field"),
		FString::Join(Normalise(OrderFromResult(Result, TEXT("Guido"), 2), 2, 3), TEXT(",")),
		FString(TEXT("@ai:1,@host,@ai:2")));
	TestEqual(TEXT("a host who did not qualify starts behind"),
		FString::Join(Normalise(OrderFromResult(Result, TEXT("Nobody"), 2), 2, 8), TEXT(",")),
		FString(TEXT("@ai:1,Guido,@ai:2,Bob,@host")));

	// Drivers with a profile are known by its id: the host by theirs under
	// any name, the others by `@driver:`, labelled with the name they had.
	Result.Entries[1].DriverId = TEXT("AAAA-1");
	Result.Entries[4].DriverId = TEXT("bbbb-2");
	TestEqual(TEXT("the host by profile, renamed"),
		FString::Join(OrderFromResult(Result, TEXT("Renamed"), 2, TEXT("aaaa-1")), TEXT(",")),
		FString(TEXT("@ai:1,@host,@ai:2,@driver:bbbb-2")));
	const TArray<FString> ByProfile = Normalise(OrderFromResult(Result, TEXT("Guido"), 2, TEXT("cccc-3")), 2, 8);
	TestEqual(TEXT("a name is not the host's profile"),
		FString::Join(ByProfile, TEXT(",")), FString(TEXT("@ai:1,@driver:aaaa-1,@ai:2,@driver:bbbb-2,@host")));
	const TArray<ApexCreateSession::FGridEntry> Named = ApexCreateSession::Entries(ByProfile, ApexCreateSession::DriverNames({ Result }));
	TestEqual(TEXT("a profile's slot shows its name"), Named[3].Label, FString(TEXT("Bob")));
	return true;
}

// -----------------------------------------------------------------------------
// The grid: the AI take the front, the host lines up behind them, the rest
// are open seats (the server spawns the AI before the host joins).
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexCreateSessionGridTest,
	"ApexSim.UI.CreateSession.Grid",
	ApexTestFlags)

bool FApexCreateSessionGridTest::RunTest(const FString& Parameters)
{
	using namespace ApexCreateSession;

	const FGrid Multi = Grid(true, 8, 3, 20);
	TestEqual(TEXT("multiplayer field is the grid size"), Multi.Field, 8);
	TestEqual(TEXT("multiplayer AI"), Multi.Ai, 3);
	TestEqual(TEXT("multiplayer open seats"), Multi.Open, 4);
	TestEqual(TEXT("host behind the AI"), Multi.YourSlot, 4);
	TestTrue(TEXT("P1 is AI"), SlotAt(Multi, 1) == ESlot::Ai);
	TestTrue(TEXT("P4 is you"), SlotAt(Multi, 4) == ESlot::You);
	TestTrue(TEXT("P5 is open"), SlotAt(Multi, 5) == ESlot::Open);
	TestTrue(TEXT("P9 is past the grid"), SlotAt(Multi, 9) == ESlot::None);

	const FGrid Single = Grid(false, 8, 3, 20);
	TestEqual(TEXT("single player field is the AI and you"), Single.Field, 4);
	TestEqual(TEXT("no open seats alone"), Single.Open, 0);
	TestTrue(TEXT("you start last"), SlotAt(Single, 4) == ESlot::You);
	TestTrue(TEXT("nothing past you"), SlotAt(Single, 5) == ESlot::None);

	const FGrid Crowded = Grid(true, 5, 9, 20);
	TestEqual(TEXT("AI never take the host's seat"), Crowded.Ai, 4);
	TestEqual(TEXT("a full grid has no open seats"), Crowded.Open, 0);

	const FGrid Alone = Grid(false, 1, 0, 20);
	TestEqual(TEXT("alone is one car"), Alone.Field, 1);
	TestTrue(TEXT("alone on pole"), SlotAt(Alone, 1) == ESlot::You);
	return true;
}

// -----------------------------------------------------------------------------
// The air on auto is what the server resolves (SessionConditions in data.rs).
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexCreateSessionAirTest,
	"ApexSim.UI.CreateSession.Air",
	ApexTestFlags)

bool FApexCreateSessionAirTest::RunTest(const FString& Parameters)
{
	using namespace ApexCreateSession;

	// 13 °C before dawn to 23 °C at 15:00 on a clear day.
	TestEqual(TEXT("sunny 15:00 is the warmest"), AirTempC(CreateAt(EApexWeather::Sunny, 15)), 23);
	TestEqual(TEXT("sunny 03:00 is the coolest"), AirTempC(CreateAt(EApexWeather::Sunny, 3)), 13);
	// 13 + 10 x (0.5 + 0.5 x 0.3) - 4 = 15.5, rounded away from zero as Rust does.
	TestEqual(TEXT("heavy rain damps and lowers the day"), AirTempC(CreateAt(EApexWeather::HeavyRain, 15)), 16);
	// 13 + 10 x 0.5 - 1.5 = 16.5.
	TestEqual(TEXT("overcast morning"), AirTempC(CreateAt(EApexWeather::Overcast, 9)), 17);

	FApexSessionConditions Fixed = CreateAt(EApexWeather::Sunny, 15);
	Fixed.AirTempC = 4;
	TestEqual(TEXT("a picked figure wins"), AirTempC(Fixed), 4);

	// The asphalt: the air plus the sun through the cloud, a degree under the air when wet.
	TestEqual(TEXT("a wet track is under the air"),
		FMath::RoundToInt(TrackTempC(CreateAt(EApexWeather::HeavyRain, 15))), 15);
	TestEqual(TEXT("no sun at midnight"),
		FMath::RoundToInt(TrackTempC(CreateAt(EApexWeather::Sunny, 0))), AirTempC(CreateAt(EApexWeather::Sunny, 0)));
	const float Noon = TrackTempC(CreateAt(EApexWeather::Sunny, 13));
	// 22 °C air under a 60° sun: 22 + 20 sin 60°.
	TestTrue(TEXT("a clear noon heats the asphalt"), Noon > 38.5f && Noon < 40.5f);
	TestTrue(TEXT("cloud keeps it cooler"), TrackTempC(CreateAt(EApexWeather::Cloudy, 13)) < Noon);

	TestEqual(TEXT("auto wind in the sun"), WindKph(CreateAt(EApexWeather::Sunny, 13)), 8);
	TestEqual(TEXT("auto wind in a storm"), WindKph(CreateAt(EApexWeather::HeavyRain, 13)), 28);
	FApexSessionConditions Calm = CreateAt(EApexWeather::HeavyRain, 13);
	Calm.WindKph = 0;
	TestEqual(TEXT("a picked calm is calm"), WindKph(Calm), 0);

	TestEqual(TEXT("dry grip"), GripPercent(EApexWeather::Sunny), 100);
	TestEqual(TEXT("overcast grip"), GripPercent(EApexWeather::Overcast), 98);
	TestEqual(TEXT("heavy rain grip"), GripPercent(EApexWeather::HeavyRain), 74);
	TestEqual(TEXT("snow grip"), GripPercent(EApexWeather::Snow), 55);
	TestEqual(TEXT("auto wind in snow"), WindKph(CreateAt(EApexWeather::Snow, 13)), 10);
	TestEqual(TEXT("snow at its warmest hour"), AirTempC(CreateAt(EApexWeather::Snow, 15)), 1);
	TestEqual(TEXT("snow before dawn"), AirTempC(CreateAt(EApexWeather::Snow, 3)), -4);
	TestEqual(TEXT("a snowy track is a degree under the air"),
		FMath::RoundToInt(TrackTempC(CreateAt(EApexWeather::Snow, 13))), AirTempC(CreateAt(EApexWeather::Snow, 13)) - 1);
	return true;
}

// -----------------------------------------------------------------------------
// The sky through the session: the Clock, Weather changes and Track chips, and
// what each leaves on the wire (the stock pick is left off it).
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexCreateSessionSkyChipsTest,
	"ApexSim.UI.CreateSession.SkyChips",
	ApexTestFlags)

bool FApexCreateSessionSkyChipsTest::RunTest(const FString& Parameters)
{
	using namespace ApexCreateSession;

	FApexSessionConditions C;
	TestEqual(TEXT("an unset clock is the frozen chip"), ClockScaleIndex(C), 0);
	TestEqual(TEXT("an unset forecast is fixed"), ChangeableIndex(C), 0);
	TestEqual(TEXT("an unset track is normal"), TrackRubberIndex(C), 1);

	SetClockScale(C, 24);
	TestEqual(TEXT("24x"), C.TimeScale, 24);
	TestEqual(TEXT("24x chip"), ClockScales[ClockScaleIndex(C)], 24);
	TestEqual(TEXT("chip label"), ClockScaleLabel(24), FString(TEXT("24x")));
	SetClockScale(C, 0);
	TestFalse(TEXT("frozen is left off the wire"), C.HasTimeScale());

	SetChangeable(C, 3);
	TestEqual(TEXT("stormy"), C.Changeable, 3);
	SetChangeable(C, 0);
	TestFalse(TEXT("fixed is left off the wire"), C.HasChangeable());

	SetTrackRubber(C, 0);
	TestEqual(TEXT("green"), C.TrackRubberPct, 0);
	TestEqual(TEXT("green chip"), TrackRubberIndex(C), 0);
	SetTrackRubber(C, 2);
	TestEqual(TEXT("rubbered"), C.TrackRubberPct, 100);
	SetTrackRubber(C, 1);
	TestFalse(TEXT("normal is left off the wire"), C.HasTrackRubber());
	TestTrue(TEXT("all stock is the default sky"), C.IsDefault());

	C.TrackRubberPct = 70;
	TestEqual(TEXT("a figure between the chips shows none"), TrackRubberIndex(C), INDEX_NONE);
	return true;
}

// -----------------------------------------------------------------------------
// The sky preview follows the race's sun and greys under cloud.
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexCreateSessionSkyTest,
	"ApexSim.UI.CreateSession.Sky",
	ApexTestFlags)

bool FApexCreateSessionSkyTest::RunTest(const FString& Parameters)
{
	using namespace ApexCreateSession;

	const FSkyLook Night = Sky(CreateAt(EApexWeather::Sunny, 1));
	TestEqual(TEXT("01:00 is night"), FString(Night.Phase), FString(TEXT("NIGHT")));
	TestEqual(TEXT("no sun at night"), Night.SunOpacity, 0.0f);

	const FSkyLook Noon = Sky(CreateAt(EApexWeather::Sunny, 13));
	TestEqual(TEXT("13:00 is day"), FString(Noon.Phase), FString(TEXT("DAY")));
	TestTrue(TEXT("the noon sun is high"), Noon.SunHeight > 0.9f);
	TestTrue(TEXT("the noon sun is mid-sky"), FMath::Abs(Noon.SunX - 0.5f) < 0.02f);
	TestFalse(TEXT("the noon sun is white"), Noon.bLowSun);

	TestEqual(TEXT("06:15 is dawn"), FString(Sky(CreateAt(EApexWeather::Sunny, 6, 15)).Phase), FString(TEXT("DAWN")));
	TestEqual(TEXT("20:30 is dusk"), FString(Sky(CreateAt(EApexWeather::Sunny, 20, 30)).Phase), FString(TEXT("DUSK")));
	TestTrue(TEXT("a dusk sun is low"), Sky(CreateAt(EApexWeather::Sunny, 20, 30)).bLowSun);

	const FSkyLook Overcast = Sky(CreateAt(EApexWeather::Overcast, 13));
	TestTrue(TEXT("cloud greys the zenith"), Overcast.Top != Noon.Top);
	TestTrue(TEXT("cloud dims the sun"), Overcast.SunOpacity < Noon.SunOpacity);
	return true;
}

// -----------------------------------------------------------------------------
// Assist presets.
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexCreateSessionAssistPresetsTest,
	"ApexSim.UI.CreateSession.AssistPresets",
	ApexTestFlags)

bool FApexCreateSessionAssistPresetsTest::RunTest(const FString& Parameters)
{
	using namespace ApexCreateSession;

	TestEqual(TEXT("everything allowed is Novice"), MatchingPreset(FApexAllowedAssists()), static_cast<int32>(EAssistPreset::Novice));
	TestEqual(TEXT("Pro allows nothing"), AssistPreset(EAssistPreset::Pro).CountLocked(), FApexAllowedAssists::Count);
	const FApexAllowedAssists Club = AssistPreset(EAssistPreset::Club);
	TestTrue(TEXT("Club keeps ABS, TC and the line"), Club.bAbs && Club.bTractionControl && Club.bRacingLine);
	TestFalse(TEXT("Club drops the gearbox and steering aids"), Club.bAutoGearbox || Club.bSteeringAssist);
	TestEqual(TEXT("Club is recognised"), MatchingPreset(Club), static_cast<int32>(EAssistPreset::Club));

	FApexAllowedAssists Mixed;
	Mixed.bAbs = false;
	TestEqual(TEXT("a mix is no preset"), MatchingPreset(Mixed), static_cast<int32>(INDEX_NONE));
	return true;
}

// -----------------------------------------------------------------------------
// The forecast: a port of the server's, laid out from the create's seed.
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexCreateSessionForecastTest,
	"ApexSim.UI.CreateSession.Forecast",
	ApexTestFlags)

bool FApexCreateSessionForecastTest::RunTest(const FString& Parameters)
{
	using namespace ApexCreateSession;

	// The Rust's `the_forecast_reference` (server/sim/src/conditions.rs): the
	// same picks and seed give the same changes to the bit, or one side
	// shows a sky the other will not run.
	constexpr uint64 Seed = 0x0123456789ABCDEFull;
	FApexSessionConditions Summer = CreateAt(EApexWeather::Cloudy, 15);
	Summer.TimeScale = 4;
	Summer.Changeable = 2;
	const TArray<FForecastChange> S = Forecast(Summer, Seed);
	TestTrue(TEXT("a changeable summer sky has changes"), S.Num() >= 5);
	if (S.Num() >= 5)
	{
		const float AtS[5] = {979.83295f, 1437.4385f, 2386.9404f, 3194.0688f, 3735.432f};
		const EApexWeather W[5] = {EApexWeather::Sunny, EApexWeather::Cloudy, EApexWeather::Overcast, EApexWeather::LightRain, EApexWeather::Overcast};
		for (int32 Index = 0; Index < 5; ++Index)
		{
			TestTrue(*FString::Printf(TEXT("summer change %d at %.5f (got %.5f)"), Index, AtS[Index], S[Index].AtS), S[Index].AtS == AtS[Index]);
			TestEqual(*FString::Printf(TEXT("summer change %d weather"), Index), S[Index].Weather, W[Index]);
		}
	}

	FApexSessionConditions Winter = CreateAt(EApexWeather::Snow, 9);
	Winter.TimeScale = 1;
	Winter.Changeable = 3;
	TestTrue(TEXT("snow is a winter sky"), IsWinter(Winter));
	TestFalse(TEXT("a cloudy afternoon is not"), IsWinter(Summer));
	const TArray<FForecastChange> Wi = Forecast(Winter, Seed);
	TestTrue(TEXT("a stormy winter sky has changes"), Wi.Num() >= 3);
	if (Wi.Num() >= 3)
	{
		const float AtS[3] = {21565.688f, 23245.703f, 24461.414f};
		const EApexWeather W[3] = {EApexWeather::Overcast, EApexWeather::Snow, EApexWeather::Overcast};
		for (int32 Index = 0; Index < 3; ++Index)
		{
			TestTrue(*FString::Printf(TEXT("winter change %d at %.3f (got %.3f)"), Index, AtS[Index], Wi[Index].AtS), Wi[Index].AtS == AtS[Index]);
			TestEqual(*FString::Printf(TEXT("winter change %d weather"), Index), Wi[Index].Weather, W[Index]);
		}
	}

	// A sky that holds has no forecast.
	TestEqual(TEXT("a held sky"), Forecast(CreateAt(EApexWeather::Cloudy, 15), Seed).Num(), 0);

	// The seed a session with no sky_seed uses: the first half of its id.
	TestEqual(TEXT("a session id's seed"), SeedOfSessionId(TEXT("01234567-89ab-cdef-0123-456789abcdef")), Seed);
	TestEqual(TEXT("not a uuid"), SeedOfSessionId(TEXT("nope")), static_cast<uint64>(0));
	FApexSessionConditions Seeded = Summer;
	Seeded.SkySeed = 42;
	Seeded.bHasSkySeed = true;
	TestEqual(TEXT("the host's seed wins"), SessionSeed(Seeded, TEXT("01234567-89ab-cdef-0123-456789abcdef")), static_cast<uint64>(42));
	TestEqual(TEXT("else the id's"), SessionSeed(Summer, TEXT("01234567-89ab-cdef-0123-456789abcdef")), Seed);
	TestTrue(TEXT("a new seed is set"), NewSkySeed() != 0);

	// The screen's line: the day's clock at each change (15:00 + 979.8 s x 4).
	TestEqual(TEXT("the clock at the first change"), ClockAt(Summer, S.IsEmpty() ? 0.0f : S[0].AtS), FString(TEXT("16:05")));
	const FString Line = ForecastLine(Summer, S, 2);
	TestTrue(*FString::Printf(TEXT("the line starts with the first change: %s"), *Line), Line.StartsWith(TEXT("16:05 Sunny")));
	TestTrue(TEXT("and says there is more"), Line.EndsWith(TEXT("...")));
	TestTrue(TEXT("a held clock counts session minutes"), ForecastLine(CreateAt(EApexWeather::Cloudy, 15), S, 1).StartsWith(TEXT("+16 min Sunny")));
	return true;
}

// -----------------------------------------------------------------------------
// A race weekend's days: practice and qualifying under a sky of their own.
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexCreateSessionWeekendDaysTest,
	"ApexSim.UI.CreateSession.WeekendDays",
	ApexTestFlags)

bool FApexCreateSessionWeekendDaysTest::RunTest(const FString& Parameters)
{
	using namespace ApexCreateSession;
	FApexSessionConditions Race = CreateAt(EApexWeather::Cloudy, 15);
	Race.Changeable = 2;
	Race.SkySeed = 0x0123456789ABCDEFull;
	Race.bHasSkySeed = true;

	FApexSessionConditions Day;
	TestFalse(TEXT("a day that picks nothing runs under the race day's sky"), DaySky(Race, -1, -1, EWeekendDay::Practice, Day));

	TestTrue(TEXT("a day's weather makes a sky of its own"), DaySky(Race, static_cast<int32>(EApexWeather::HeavyRain), -1, EWeekendDay::Practice, Day));
	TestEqual(TEXT("its weather"), Day.Weather, EApexWeather::HeavyRain);
	TestEqual(TEXT("the race's hour"), Day.TimeOfDayMinutes, Race.TimeOfDayMinutes);
	TestEqual(TEXT("and the race's changes"), Day.Changeable, Race.Changeable);
	TestTrue(TEXT("its own seed"), Day.HasSkySeed() && Day.SkySeed != Race.SkySeed);

	FApexSessionConditions Qualifying;
	TestTrue(TEXT("an hour alone is a sky of its own"), DaySky(Race, -1, 9 * 60, EWeekendDay::Qualifying, Qualifying));
	TestEqual(TEXT("the race's weather at that hour"), Qualifying.Weather, Race.Weather);
	TestEqual(TEXT("the day's hour"), Qualifying.TimeOfDayMinutes, 9 * 60);
	FApexSessionConditions PracticeAtNine;
	DaySky(Race, -1, 9 * 60, EWeekendDay::Practice, PracticeAtNine);
	TestTrue(TEXT("each day lays out its own forecast"), PracticeAtNine.SkySeed != Qualifying.SkySeed);
	FApexSessionConditions Again;
	DaySky(Race, -1, 9 * 60, EWeekendDay::Qualifying, Again);
	TestTrue(TEXT("the same picks give the same seed"), Again.SkySeed == Qualifying.SkySeed);

	FApexSessionConditions Unseeded = Race;
	Unseeded.bHasSkySeed = false;
	DaySky(Unseeded, static_cast<int32>(EApexWeather::Sunny), -1, EWeekendDay::Practice, Day);
	TestFalse(TEXT("a race with no seed leaves the day's to the server"), Day.HasSkySeed());
	TestTrue(TEXT("the hours run through the day"), DayHourChoices().Num() > 3 && DayHourChoices()[0] < DayHourChoices().Last());
	return true;
}

// -----------------------------------------------------------------------------
// The session's date: the Date row's steps, and the preview's sun over the
// circuit on that date at its own solar noon.
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexCreateSessionDateTest,
	"ApexSim.UI.CreateSession.Date",
	ApexTestFlags)

bool FApexCreateSessionDateTest::RunTest(const FString& Parameters)
{
	using namespace ApexCreateSession;
	TestEqual(TEXT("no date is the model day"), DateLabel(-1), FString(TEXT("Late May")));
	TestEqual(TEXT("day 15"), DateLabel(15), FString(TEXT("15 Jan")));
	TestEqual(TEXT("day 349"), DateLabel(349), FString(TEXT("15 Dec")));
	TestEqual(TEXT("day 60"), DateLabel(60), FString(TEXT("1 Mar")));
	TestEqual(TEXT("a step on from none is January"), StepDate(-1, 1), 15);
	TestEqual(TEXT("a step back from January is none"), StepDate(15, -1), -1);
	TestEqual(TEXT("December is the last"), StepDate(349, 1), 349);
	TestEqual(TEXT("a day between steps steps from the one before it"), StepDate(100, 1), 105);

	// The roadmap's check: a December create at Silverstone at 16:30 lights
	// a dusk; the same clock in late May is still day.
	constexpr float SilverstoneLat = 52.0786f;
	constexpr float SilverstoneLon = -1.0169f;
	FApexSessionConditions December = CreateAt(EApexWeather::Sunny, 16, 30);
	December.DayOfYear = 349;
	December.SolarNoonMin = ApexSky::SolarNoonMinutes(SilverstoneLat, SilverstoneLon, 349);
	TestTrue(TEXT("Silverstone's December noon is about midday GMT"), FMath::Abs(December.SolarNoonMin - 12 * 60) <= 10);
	const FSkyLook Dusk = Sky(December, SilverstoneLat);
	TestEqual(*FString::Printf(TEXT("16:30 in December is dusk (sun x %.2f)"), Dusk.SunX), FString(Dusk.Phase), FString(TEXT("DUSK")));
	TestTrue(TEXT("and the sun is low"), Dusk.bLowSun);
	FApexSessionConditions May = CreateAt(EApexWeather::Sunny, 16, 30);
	May.SolarNoonMin = ApexSky::SolarNoonMinutes(SilverstoneLat, SilverstoneLon, ApexSky::ModelDayOfYear);
	TestEqual(TEXT("16:30 in late May is day"), FString(Sky(May, SilverstoneLat).Phase), FString(TEXT("DAY")));
	TestTrue(TEXT("a December sun sets earlier across the preview"), Dusk.SunX > Sky(May, SilverstoneLat).SunX);
	return true;
}

// -----------------------------------------------------------------------------
// Solar noon: the server's `solar_noon_minutes`, port for port.
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexCreateSessionSolarNoonTest,
	"ApexSim.UI.CreateSession.SolarNoon",
	ApexTestFlags)

bool FApexCreateSessionSolarNoonTest::RunTest(const FString& Parameters)
{
	// The almanac's late-May noons the server's own test holds itself to
	// (conditions.rs), within three minutes.
	struct FCase { const TCHAR* Name; float Lat; float Lon; int32 Almanac; };
	const FCase Cases[] = {
		{ TEXT("Barcelona"), 41.57f, 2.26f, 13 * 60 + 48 },
		{ TEXT("Monzarella"), 45.62f, 9.27f, 13 * 60 + 20 },
		{ TEXT("Sudoku"), 34.84f, 136.54f, 11 * 60 + 51 },
		{ TEXT("Armadillos"), 30.13f, -97.64f, 13 * 60 + 28 },
		{ TEXT("Mexico City"), 19.40f, -99.09f, 12 * 60 + 33 },
		{ TEXT("Interlagos"), -23.70f, -46.70f, 12 * 60 + 4 },
		{ TEXT("AlbertParkour"), -37.85f, 144.97f, 12 * 60 + 17 },
		{ TEXT("Marina Bay"), 1.29f, 103.86f, 13 * 60 },
		{ TEXT("Yas Marina"), 24.47f, 54.60f, 12 * 60 + 18 },
	};
	for (const FCase& Case : Cases)
	{
		const int32 Noon = ApexSky::SolarNoonMinutes(Case.Lat, Case.Lon, ApexSky::ModelDayOfYear);
		TestTrue(*FString::Printf(TEXT("%s at %02d:%02d against %02d:%02d"), Case.Name, Noon / 60, Noon % 60, Case.Almanac / 60, Case.Almanac % 60),
			FMath::Abs(Noon - Case.Almanac) <= 3);
	}
	// Summer time goes with the season.
	TestEqual(TEXT("Silverstone in January"), ApexSky::SolarNoonMinutes(52.07f, -1.02f, 15) / 60, 12);
	TestEqual(TEXT("Melbourne in its summer"), ApexSky::SolarNoonMinutes(-37.85f, 144.97f, 15) / 60, 13);
	return true;
}

// -----------------------------------------------------------------------------
// The season's air: `season_air_c`, `snow_air_drop_c` and the resolved air
// over a circuit, against figures printed by the Rust (the conditions behind
// `a_date_moves_the_air_with_the_season_and_no_date_is_late_may`).
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexCreateSessionSeasonAirTest,
	"ApexSim.UI.CreateSession.SeasonAir",
	ApexTestFlags)

bool FApexCreateSessionSeasonAirTest::RunTest(const FString& Parameters)
{
	using namespace ApexCreateSession;

	// Weather, minutes, latitude, longitude (999 none), height, day (-1 none),
	// solar noon (-1 none), then the Rust's `auto_air_temperature_c_at`,
	// `resolve_at`'s air, `snow_air_drop_c` and `is_winter` of the resolved
	// conditions: `cargo test --lib client_season_air_figures -- --ignored
	// --nocapture` in server/sim prints them.
	struct FFigure { int32 Weather; int32 Minutes; float Lat; float Lon; float Alt; int32 Day; int32 Noon; float Auto; int32 Air; float Drop; bool bWinter; };
	static const FFigure Figures[] = {
		{0, 240, 52.07f, 999.0f, 0.0f, -1, -1, 13.1704f, 13, 17.0000f, false},
		{0, 240, 52.07f, 999.0f, 0.0f, -1, 726, 13.6059f, 14, 17.0000f, false},
		{0, 240, 52.07f, 999.0f, 0.0f, 15, -1, -2.4828f, -2, 1.3468f, false},
		{0, 240, 52.07f, 999.0f, 0.0f, 15, 726, -2.0472f, -2, 1.3468f, false},
		{0, 240, 52.07f, 999.0f, 0.0f, 196, -1, 15.7117f, 16, 19.5413f, false},
		{0, 240, 52.07f, 999.0f, 0.0f, 196, 726, 16.1472f, 16, 19.5413f, false},
		{0, 240, -37.85f, 999.0f, 0.0f, -1, -1, 13.1704f, 13, 17.0000f, false},
		{0, 240, -37.85f, 999.0f, 0.0f, -1, 726, 13.6059f, 14, 17.0000f, false},
		{0, 240, -37.85f, 999.0f, 0.0f, 15, -1, 21.9335f, 22, 25.7632f, false},
		{0, 240, -37.85f, 999.0f, 0.0f, 15, 726, 22.3691f, 22, 25.7632f, false},
		{0, 240, -37.85f, 999.0f, 0.0f, 196, -1, 8.7079f, 9, 12.5375f, false},
		{0, 240, -37.85f, 999.0f, 0.0f, 196, 726, 9.1434f, 9, 12.5375f, false},
		{0, 240, 50.00f, 999.0f, 0.0f, -1, -1, 13.1704f, 13, 17.0000f, false},
		{0, 240, 50.00f, 999.0f, 0.0f, -1, 726, 13.6059f, 14, 17.0000f, false},
		{0, 240, 50.00f, 999.0f, 0.0f, 15, -1, -0.6841f, -1, 3.1455f, false},
		{0, 240, 50.00f, 999.0f, 0.0f, 15, 726, -0.2486f, 0, 3.1455f, false},
		{0, 240, 50.00f, 999.0f, 0.0f, 196, -1, 16.7870f, 17, 20.6167f, false},
		{0, 240, 50.00f, 999.0f, 0.0f, 196, 726, 17.2226f, 17, 20.6167f, false},
		{0, 240, 52.07f, -1.02f, 150.0f, -1, -1, 13.1704f, 13, 17.0000f, false},
		{0, 240, 52.07f, -1.02f, 150.0f, -1, 726, 13.6059f, 14, 17.0000f, false},
		{0, 240, 52.07f, -1.02f, 150.0f, 15, -1, 0.9984f, 1, 4.8280f, false},
		{0, 240, 52.07f, -1.02f, 150.0f, 15, 726, 1.4339f, 1, 4.8280f, false},
		{0, 240, 52.07f, -1.02f, 150.0f, 196, -1, 13.7345f, 14, 17.5641f, false},
		{0, 240, 52.07f, -1.02f, 150.0f, 196, 726, 14.1700f, 14, 17.5641f, false},
		{0, 240, -37.85f, 144.97f, 0.0f, -1, -1, 13.1704f, 13, 17.0000f, false},
		{0, 240, -37.85f, 144.97f, 0.0f, -1, 726, 13.6059f, 14, 17.0000f, false},
		{0, 240, -37.85f, 144.97f, 0.0f, 15, -1, 20.6100f, 21, 24.4397f, false},
		{0, 240, -37.85f, 144.97f, 0.0f, 15, 726, 21.0456f, 21, 24.4397f, false},
		{0, 240, -37.85f, 144.97f, 0.0f, 196, -1, 10.0295f, 10, 13.8591f, false},
		{0, 240, -37.85f, 144.97f, 0.0f, 196, 726, 10.4650f, 10, 13.8591f, false},
		{0, 240, 19.40f, -99.09f, 2240.0f, -1, -1, 13.1704f, 13, 17.0000f, false},
		{0, 240, 19.40f, -99.09f, 2240.0f, -1, 726, 13.6059f, 14, 17.0000f, false},
		{0, 240, 19.40f, -99.09f, 2240.0f, 15, -1, 9.2634f, 9, 13.0930f, false},
		{0, 240, 19.40f, -99.09f, 2240.0f, 15, 726, 9.6989f, 10, 13.0930f, false},
		{0, 240, 19.40f, -99.09f, 2240.0f, 196, -1, 13.3307f, 13, 17.1603f, false},
		{0, 240, 19.40f, -99.09f, 2240.0f, 196, 726, 13.7662f, 14, 17.1603f, false},
		{0, 240, 26.03f, 50.51f, 0.0f, -1, -1, 13.1704f, 13, 17.0000f, false},
		{0, 240, 26.03f, 50.51f, 0.0f, -1, 726, 13.6059f, 14, 17.0000f, false},
		{0, 240, 26.03f, 50.51f, 0.0f, 15, -1, 16.2585f, 16, 20.0881f, false},
		{0, 240, 26.03f, 50.51f, 0.0f, 15, 726, 16.6940f, 17, 20.0881f, false},
		{0, 240, 26.03f, 50.51f, 0.0f, 196, -1, 32.6304f, 33, 36.4600f, false},
		{0, 240, 26.03f, 50.51f, 0.0f, 196, 726, 33.0659f, 33, 36.4600f, false},
		{0, 900, 52.07f, 999.0f, 0.0f, -1, -1, 23.0000f, 23, 17.0000f, false},
		{0, 900, 52.07f, 999.0f, 0.0f, -1, 726, 22.8619f, 23, 17.0000f, false},
		{0, 900, 52.07f, 999.0f, 0.0f, 15, -1, 7.3468f, 7, 1.3468f, false},
		{0, 900, 52.07f, 999.0f, 0.0f, 15, 726, 7.2087f, 7, 1.3468f, false},
		{0, 900, 52.07f, 999.0f, 0.0f, 196, -1, 25.5413f, 26, 19.5413f, false},
		{0, 900, 52.07f, 999.0f, 0.0f, 196, 726, 25.4032f, 25, 19.5413f, false},
		{0, 900, -37.85f, 999.0f, 0.0f, -1, -1, 23.0000f, 23, 17.0000f, false},
		{0, 900, -37.85f, 999.0f, 0.0f, -1, 726, 22.8619f, 23, 17.0000f, false},
		{0, 900, -37.85f, 999.0f, 0.0f, 15, -1, 31.7632f, 32, 25.7632f, false},
		{0, 900, -37.85f, 999.0f, 0.0f, 15, 726, 31.6250f, 32, 25.7632f, false},
		{0, 900, -37.85f, 999.0f, 0.0f, 196, -1, 18.5375f, 19, 12.5375f, false},
		{0, 900, -37.85f, 999.0f, 0.0f, 196, 726, 18.3994f, 18, 12.5375f, false},
		{0, 900, 50.00f, 999.0f, 0.0f, -1, -1, 23.0000f, 23, 17.0000f, false},
		{0, 900, 50.00f, 999.0f, 0.0f, -1, 726, 22.8619f, 23, 17.0000f, false},
		{0, 900, 50.00f, 999.0f, 0.0f, 15, -1, 9.1455f, 9, 3.1455f, false},
		{0, 900, 50.00f, 999.0f, 0.0f, 15, 726, 9.0073f, 9, 3.1455f, false},
		{0, 900, 50.00f, 999.0f, 0.0f, 196, -1, 26.6167f, 27, 20.6167f, false},
		{0, 900, 50.00f, 999.0f, 0.0f, 196, 726, 26.4785f, 26, 20.6167f, false},
		{0, 900, 52.07f, -1.02f, 150.0f, -1, -1, 23.0000f, 23, 17.0000f, false},
		{0, 900, 52.07f, -1.02f, 150.0f, -1, 726, 22.8619f, 23, 17.0000f, false},
		{0, 900, 52.07f, -1.02f, 150.0f, 15, -1, 10.8280f, 11, 4.8280f, false},
		{0, 900, 52.07f, -1.02f, 150.0f, 15, 726, 10.6898f, 11, 4.8280f, false},
		{0, 900, 52.07f, -1.02f, 150.0f, 196, -1, 23.5641f, 24, 17.5641f, false},
		{0, 900, 52.07f, -1.02f, 150.0f, 196, 726, 23.4260f, 23, 17.5641f, false},
		{0, 900, -37.85f, 144.97f, 0.0f, -1, -1, 23.0000f, 23, 17.0000f, false},
		{0, 900, -37.85f, 144.97f, 0.0f, -1, 726, 22.8619f, 23, 17.0000f, false},
		{0, 900, -37.85f, 144.97f, 0.0f, 15, -1, 30.4397f, 30, 24.4397f, false},
		{0, 900, -37.85f, 144.97f, 0.0f, 15, 726, 30.3015f, 30, 24.4397f, false},
		{0, 900, -37.85f, 144.97f, 0.0f, 196, -1, 19.8591f, 20, 13.8591f, false},
		{0, 900, -37.85f, 144.97f, 0.0f, 196, 726, 19.7210f, 20, 13.8591f, false},
		{0, 900, 19.40f, -99.09f, 2240.0f, -1, -1, 23.0000f, 23, 17.0000f, false},
		{0, 900, 19.40f, -99.09f, 2240.0f, -1, 726, 22.8619f, 23, 17.0000f, false},
		{0, 900, 19.40f, -99.09f, 2240.0f, 15, -1, 19.0930f, 19, 13.0930f, false},
		{0, 900, 19.40f, -99.09f, 2240.0f, 15, 726, 18.9549f, 19, 13.0930f, false},
		{0, 900, 19.40f, -99.09f, 2240.0f, 196, -1, 23.1603f, 23, 17.1603f, false},
		{0, 900, 19.40f, -99.09f, 2240.0f, 196, 726, 23.0222f, 23, 17.1603f, false},
		{0, 900, 26.03f, 50.51f, 0.0f, -1, -1, 23.0000f, 23, 17.0000f, false},
		{0, 900, 26.03f, 50.51f, 0.0f, -1, 726, 22.8619f, 23, 17.0000f, false},
		{0, 900, 26.03f, 50.51f, 0.0f, 15, -1, 26.0881f, 26, 20.0881f, false},
		{0, 900, 26.03f, 50.51f, 0.0f, 15, 726, 25.9500f, 26, 20.0881f, false},
		{0, 900, 26.03f, 50.51f, 0.0f, 196, -1, 42.4600f, 42, 36.4600f, false},
		{0, 900, 26.03f, 50.51f, 0.0f, 196, 726, 42.3218f, 42, 36.4600f, false},
		{4, 240, 52.07f, 999.0f, 0.0f, -1, -1, 12.5511f, 13, 17.0000f, false},
		{4, 240, 52.07f, 999.0f, 0.0f, -1, 726, 12.6818f, 13, 17.0000f, false},
		{4, 240, 52.07f, 999.0f, 0.0f, 15, -1, -3.1020f, -3, 1.3468f, false},
		{4, 240, 52.07f, 999.0f, 0.0f, 15, 726, -2.9714f, -3, 1.3468f, false},
		{4, 240, 52.07f, 999.0f, 0.0f, 196, -1, 15.0924f, 15, 19.5413f, false},
		{4, 240, 52.07f, 999.0f, 0.0f, 196, 726, 15.2231f, 15, 19.5413f, false},
		{4, 240, -37.85f, 999.0f, 0.0f, -1, -1, 12.5511f, 13, 17.0000f, false},
		{4, 240, -37.85f, 999.0f, 0.0f, -1, 726, 12.6818f, 13, 17.0000f, false},
		{4, 240, -37.85f, 999.0f, 0.0f, 15, -1, 21.3143f, 21, 25.7632f, false},
		{4, 240, -37.85f, 999.0f, 0.0f, 15, 726, 21.4450f, 21, 25.7632f, false},
		{4, 240, -37.85f, 999.0f, 0.0f, 196, -1, 8.0886f, 8, 12.5375f, false},
		{4, 240, -37.85f, 999.0f, 0.0f, 196, 726, 8.2193f, 8, 12.5375f, false},
		{4, 240, 50.00f, 999.0f, 0.0f, -1, -1, 12.5511f, 13, 17.0000f, false},
		{4, 240, 50.00f, 999.0f, 0.0f, -1, 726, 12.6818f, 13, 17.0000f, false},
		{4, 240, 50.00f, 999.0f, 0.0f, 15, -1, -1.3034f, -1, 3.1455f, false},
		{4, 240, 50.00f, 999.0f, 0.0f, 15, 726, -1.1727f, -1, 3.1455f, false},
		{4, 240, 50.00f, 999.0f, 0.0f, 196, -1, 16.1678f, 16, 20.6167f, false},
		{4, 240, 50.00f, 999.0f, 0.0f, 196, 726, 16.2984f, 16, 20.6167f, false},
		{4, 240, 52.07f, -1.02f, 150.0f, -1, -1, 12.5511f, 13, 17.0000f, false},
		{4, 240, 52.07f, -1.02f, 150.0f, -1, 726, 12.6818f, 13, 17.0000f, false},
		{4, 240, 52.07f, -1.02f, 150.0f, 15, -1, 0.3791f, 0, 4.8280f, false},
		{4, 240, 52.07f, -1.02f, 150.0f, 15, 726, 0.5098f, 1, 4.8280f, false},
		{4, 240, 52.07f, -1.02f, 150.0f, 196, -1, 13.1152f, 13, 17.5641f, false},
		{4, 240, 52.07f, -1.02f, 150.0f, 196, 726, 13.2459f, 13, 17.5641f, false},
		{4, 240, -37.85f, 144.97f, 0.0f, -1, -1, 12.5511f, 13, 17.0000f, false},
		{4, 240, -37.85f, 144.97f, 0.0f, -1, 726, 12.6818f, 13, 17.0000f, false},
		{4, 240, -37.85f, 144.97f, 0.0f, 15, -1, 19.9908f, 20, 24.4397f, false},
		{4, 240, -37.85f, 144.97f, 0.0f, 15, 726, 20.1214f, 20, 24.4397f, false},
		{4, 240, -37.85f, 144.97f, 0.0f, 196, -1, 9.4102f, 9, 13.8591f, false},
		{4, 240, -37.85f, 144.97f, 0.0f, 196, 726, 9.5409f, 10, 13.8591f, false},
		{4, 240, 19.40f, -99.09f, 2240.0f, -1, -1, 12.5511f, 13, 17.0000f, false},
		{4, 240, 19.40f, -99.09f, 2240.0f, -1, 726, 12.6818f, 13, 17.0000f, false},
		{4, 240, 19.40f, -99.09f, 2240.0f, 15, -1, 8.6441f, 9, 13.0930f, false},
		{4, 240, 19.40f, -99.09f, 2240.0f, 15, 726, 8.7748f, 9, 13.0930f, false},
		{4, 240, 19.40f, -99.09f, 2240.0f, 196, -1, 12.7114f, 13, 17.1603f, false},
		{4, 240, 19.40f, -99.09f, 2240.0f, 196, 726, 12.8421f, 13, 17.1603f, false},
		{4, 240, 26.03f, 50.51f, 0.0f, -1, -1, 12.5511f, 13, 17.0000f, false},
		{4, 240, 26.03f, 50.51f, 0.0f, -1, 726, 12.6818f, 13, 17.0000f, false},
		{4, 240, 26.03f, 50.51f, 0.0f, 15, -1, 15.6392f, 16, 20.0881f, false},
		{4, 240, 26.03f, 50.51f, 0.0f, 15, 726, 15.7699f, 16, 20.0881f, false},
		{4, 240, 26.03f, 50.51f, 0.0f, 196, -1, 32.0111f, 32, 36.4600f, false},
		{4, 240, 26.03f, 50.51f, 0.0f, 196, 726, 32.1418f, 32, 36.4600f, false},
		{4, 900, 52.07f, 999.0f, 0.0f, -1, -1, 15.5000f, 16, 17.0000f, false},
		{4, 900, 52.07f, 999.0f, 0.0f, -1, 726, 15.4586f, 15, 17.0000f, false},
		{4, 900, 52.07f, 999.0f, 0.0f, 15, -1, -0.1532f, 0, 1.3468f, false},
		{4, 900, 52.07f, 999.0f, 0.0f, 15, 726, -0.1946f, 0, 1.3468f, false},
		{4, 900, 52.07f, 999.0f, 0.0f, 196, -1, 18.0413f, 18, 19.5413f, false},
		{4, 900, 52.07f, 999.0f, 0.0f, 196, 726, 17.9999f, 18, 19.5413f, false},
		{4, 900, -37.85f, 999.0f, 0.0f, -1, -1, 15.5000f, 16, 17.0000f, false},
		{4, 900, -37.85f, 999.0f, 0.0f, -1, 726, 15.4586f, 15, 17.0000f, false},
		{4, 900, -37.85f, 999.0f, 0.0f, 15, -1, 24.2632f, 24, 25.7632f, false},
		{4, 900, -37.85f, 999.0f, 0.0f, 15, 726, 24.2217f, 24, 25.7632f, false},
		{4, 900, -37.85f, 999.0f, 0.0f, 196, -1, 11.0375f, 11, 12.5375f, false},
		{4, 900, -37.85f, 999.0f, 0.0f, 196, 726, 10.9961f, 11, 12.5375f, false},
		{4, 900, 50.00f, 999.0f, 0.0f, -1, -1, 15.5000f, 16, 17.0000f, false},
		{4, 900, 50.00f, 999.0f, 0.0f, -1, 726, 15.4586f, 15, 17.0000f, false},
		{4, 900, 50.00f, 999.0f, 0.0f, 15, -1, 1.6455f, 2, 3.1455f, false},
		{4, 900, 50.00f, 999.0f, 0.0f, 15, 726, 1.6040f, 2, 3.1455f, false},
		{4, 900, 50.00f, 999.0f, 0.0f, 196, -1, 19.1167f, 19, 20.6167f, false},
		{4, 900, 50.00f, 999.0f, 0.0f, 196, 726, 19.0752f, 19, 20.6167f, false},
		{4, 900, 52.07f, -1.02f, 150.0f, -1, -1, 15.5000f, 16, 17.0000f, false},
		{4, 900, 52.07f, -1.02f, 150.0f, -1, 726, 15.4586f, 15, 17.0000f, false},
		{4, 900, 52.07f, -1.02f, 150.0f, 15, -1, 3.3280f, 3, 4.8280f, false},
		{4, 900, 52.07f, -1.02f, 150.0f, 15, 726, 3.2865f, 3, 4.8280f, false},
		{4, 900, 52.07f, -1.02f, 150.0f, 196, -1, 16.0641f, 16, 17.5641f, false},
		{4, 900, 52.07f, -1.02f, 150.0f, 196, 726, 16.0227f, 16, 17.5641f, false},
		{4, 900, -37.85f, 144.97f, 0.0f, -1, -1, 15.5000f, 16, 17.0000f, false},
		{4, 900, -37.85f, 144.97f, 0.0f, -1, 726, 15.4586f, 15, 17.0000f, false},
		{4, 900, -37.85f, 144.97f, 0.0f, 15, -1, 22.9397f, 23, 24.4397f, false},
		{4, 900, -37.85f, 144.97f, 0.0f, 15, 726, 22.8982f, 23, 24.4397f, false},
		{4, 900, -37.85f, 144.97f, 0.0f, 196, -1, 12.3591f, 12, 13.8591f, false},
		{4, 900, -37.85f, 144.97f, 0.0f, 196, 726, 12.3177f, 12, 13.8591f, false},
		{4, 900, 19.40f, -99.09f, 2240.0f, -1, -1, 15.5000f, 16, 17.0000f, false},
		{4, 900, 19.40f, -99.09f, 2240.0f, -1, 726, 15.4586f, 15, 17.0000f, false},
		{4, 900, 19.40f, -99.09f, 2240.0f, 15, -1, 11.5930f, 12, 13.0930f, false},
		{4, 900, 19.40f, -99.09f, 2240.0f, 15, 726, 11.5516f, 12, 13.0930f, false},
		{4, 900, 19.40f, -99.09f, 2240.0f, 196, -1, 15.6603f, 16, 17.1603f, false},
		{4, 900, 19.40f, -99.09f, 2240.0f, 196, 726, 15.6189f, 16, 17.1603f, false},
		{4, 900, 26.03f, 50.51f, 0.0f, -1, -1, 15.5000f, 16, 17.0000f, false},
		{4, 900, 26.03f, 50.51f, 0.0f, -1, 726, 15.4586f, 15, 17.0000f, false},
		{4, 900, 26.03f, 50.51f, 0.0f, 15, -1, 18.5881f, 19, 20.0881f, false},
		{4, 900, 26.03f, 50.51f, 0.0f, 15, 726, 18.5467f, 19, 20.0881f, false},
		{4, 900, 26.03f, 50.51f, 0.0f, 196, -1, 34.9600f, 35, 36.4600f, false},
		{4, 900, 26.03f, 50.51f, 0.0f, 196, 726, 34.9185f, 35, 36.4600f, false},
		{5, 240, 52.07f, 999.0f, 0.0f, -1, -1, -3.4319f, -3, 17.0000f, true},
		{5, 240, 52.07f, 999.0f, 0.0f, -1, 726, -3.2576f, -3, 17.0000f, true},
		{5, 240, 52.07f, 999.0f, 0.0f, 15, -1, -3.4319f, -3, 1.3468f, true},
		{5, 240, 52.07f, 999.0f, 0.0f, 15, 726, -3.2576f, -3, 1.3468f, true},
		{5, 240, 52.07f, 999.0f, 0.0f, 196, -1, -3.4319f, -3, 19.5413f, true},
		{5, 240, 52.07f, 999.0f, 0.0f, 196, 726, -3.2576f, -3, 19.5413f, true},
		{5, 240, -37.85f, 999.0f, 0.0f, -1, -1, -3.4319f, -3, 17.0000f, true},
		{5, 240, -37.85f, 999.0f, 0.0f, -1, 726, -3.2576f, -3, 17.0000f, true},
		{5, 240, -37.85f, 999.0f, 0.0f, 15, -1, -3.4319f, -3, 25.7632f, true},
		{5, 240, -37.85f, 999.0f, 0.0f, 15, 726, -3.2576f, -3, 25.7632f, true},
		{5, 240, -37.85f, 999.0f, 0.0f, 196, -1, -3.4319f, -3, 12.5375f, true},
		{5, 240, -37.85f, 999.0f, 0.0f, 196, 726, -3.2576f, -3, 12.5375f, true},
		{5, 240, 50.00f, 999.0f, 0.0f, -1, -1, -3.4319f, -3, 17.0000f, true},
		{5, 240, 50.00f, 999.0f, 0.0f, -1, 726, -3.2576f, -3, 17.0000f, true},
		{5, 240, 50.00f, 999.0f, 0.0f, 15, -1, -3.4319f, -3, 3.1455f, true},
		{5, 240, 50.00f, 999.0f, 0.0f, 15, 726, -3.2576f, -3, 3.1455f, true},
		{5, 240, 50.00f, 999.0f, 0.0f, 196, -1, -3.4319f, -3, 20.6167f, true},
		{5, 240, 50.00f, 999.0f, 0.0f, 196, 726, -3.2576f, -3, 20.6167f, true},
		{5, 240, 52.07f, -1.02f, 150.0f, -1, -1, -3.4319f, -3, 17.0000f, true},
		{5, 240, 52.07f, -1.02f, 150.0f, -1, 726, -3.2576f, -3, 17.0000f, true},
		{5, 240, 52.07f, -1.02f, 150.0f, 15, -1, -3.4319f, -3, 4.8280f, true},
		{5, 240, 52.07f, -1.02f, 150.0f, 15, 726, -3.2576f, -3, 4.8280f, true},
		{5, 240, 52.07f, -1.02f, 150.0f, 196, -1, -3.4319f, -3, 17.5641f, true},
		{5, 240, 52.07f, -1.02f, 150.0f, 196, 726, -3.2576f, -3, 17.5641f, true},
		{5, 240, -37.85f, 144.97f, 0.0f, -1, -1, -3.4319f, -3, 17.0000f, true},
		{5, 240, -37.85f, 144.97f, 0.0f, -1, 726, -3.2576f, -3, 17.0000f, true},
		{5, 240, -37.85f, 144.97f, 0.0f, 15, -1, -3.4319f, -3, 24.4397f, true},
		{5, 240, -37.85f, 144.97f, 0.0f, 15, 726, -3.2576f, -3, 24.4397f, true},
		{5, 240, -37.85f, 144.97f, 0.0f, 196, -1, -3.4319f, -3, 13.8591f, true},
		{5, 240, -37.85f, 144.97f, 0.0f, 196, 726, -3.2576f, -3, 13.8591f, true},
		{5, 240, 19.40f, -99.09f, 2240.0f, -1, -1, -3.4319f, -3, 17.0000f, true},
		{5, 240, 19.40f, -99.09f, 2240.0f, -1, 726, -3.2576f, -3, 17.0000f, true},
		{5, 240, 19.40f, -99.09f, 2240.0f, 15, -1, -3.4319f, -3, 13.0930f, true},
		{5, 240, 19.40f, -99.09f, 2240.0f, 15, 726, -3.2576f, -3, 13.0930f, true},
		{5, 240, 19.40f, -99.09f, 2240.0f, 196, -1, -3.4319f, -3, 17.1603f, true},
		{5, 240, 19.40f, -99.09f, 2240.0f, 196, 726, -3.2576f, -3, 17.1603f, true},
		{5, 240, 26.03f, 50.51f, 0.0f, -1, -1, -3.4319f, -3, 17.0000f, true},
		{5, 240, 26.03f, 50.51f, 0.0f, -1, 726, -3.2576f, -3, 17.0000f, true},
		{5, 240, 26.03f, 50.51f, 0.0f, 15, -1, -3.4319f, -3, 20.0881f, true},
		{5, 240, 26.03f, 50.51f, 0.0f, 15, 726, -3.2576f, -3, 20.0881f, true},
		{5, 240, 26.03f, 50.51f, 0.0f, 196, -1, -3.4319f, -3, 36.4600f, true},
		{5, 240, 26.03f, 50.51f, 0.0f, 196, 726, -3.2576f, -3, 36.4600f, true},
		{5, 900, 52.07f, 999.0f, 0.0f, -1, -1, 0.5000f, 1, 17.0000f, true},
		{5, 900, 52.07f, 999.0f, 0.0f, -1, 726, 0.4447f, 0, 17.0000f, true},
		{5, 900, 52.07f, 999.0f, 0.0f, 15, -1, 0.5000f, 1, 1.3468f, true},
		{5, 900, 52.07f, 999.0f, 0.0f, 15, 726, 0.4447f, 0, 1.3468f, true},
		{5, 900, 52.07f, 999.0f, 0.0f, 196, -1, 0.5000f, 1, 19.5413f, true},
		{5, 900, 52.07f, 999.0f, 0.0f, 196, 726, 0.4447f, 0, 19.5413f, true},
		{5, 900, -37.85f, 999.0f, 0.0f, -1, -1, 0.5000f, 1, 17.0000f, true},
		{5, 900, -37.85f, 999.0f, 0.0f, -1, 726, 0.4447f, 0, 17.0000f, true},
		{5, 900, -37.85f, 999.0f, 0.0f, 15, -1, 0.5000f, 1, 25.7632f, true},
		{5, 900, -37.85f, 999.0f, 0.0f, 15, 726, 0.4447f, 0, 25.7632f, true},
		{5, 900, -37.85f, 999.0f, 0.0f, 196, -1, 0.5000f, 1, 12.5375f, true},
		{5, 900, -37.85f, 999.0f, 0.0f, 196, 726, 0.4447f, 0, 12.5375f, true},
		{5, 900, 50.00f, 999.0f, 0.0f, -1, -1, 0.5000f, 1, 17.0000f, true},
		{5, 900, 50.00f, 999.0f, 0.0f, -1, 726, 0.4447f, 0, 17.0000f, true},
		{5, 900, 50.00f, 999.0f, 0.0f, 15, -1, 0.5000f, 1, 3.1455f, true},
		{5, 900, 50.00f, 999.0f, 0.0f, 15, 726, 0.4447f, 0, 3.1455f, true},
		{5, 900, 50.00f, 999.0f, 0.0f, 196, -1, 0.5000f, 1, 20.6167f, true},
		{5, 900, 50.00f, 999.0f, 0.0f, 196, 726, 0.4447f, 0, 20.6167f, true},
		{5, 900, 52.07f, -1.02f, 150.0f, -1, -1, 0.5000f, 1, 17.0000f, true},
		{5, 900, 52.07f, -1.02f, 150.0f, -1, 726, 0.4447f, 0, 17.0000f, true},
		{5, 900, 52.07f, -1.02f, 150.0f, 15, -1, 0.5000f, 1, 4.8280f, true},
		{5, 900, 52.07f, -1.02f, 150.0f, 15, 726, 0.4447f, 0, 4.8280f, true},
		{5, 900, 52.07f, -1.02f, 150.0f, 196, -1, 0.5000f, 1, 17.5641f, true},
		{5, 900, 52.07f, -1.02f, 150.0f, 196, 726, 0.4447f, 0, 17.5641f, true},
		{5, 900, -37.85f, 144.97f, 0.0f, -1, -1, 0.5000f, 1, 17.0000f, true},
		{5, 900, -37.85f, 144.97f, 0.0f, -1, 726, 0.4447f, 0, 17.0000f, true},
		{5, 900, -37.85f, 144.97f, 0.0f, 15, -1, 0.5000f, 1, 24.4397f, true},
		{5, 900, -37.85f, 144.97f, 0.0f, 15, 726, 0.4447f, 0, 24.4397f, true},
		{5, 900, -37.85f, 144.97f, 0.0f, 196, -1, 0.5000f, 1, 13.8591f, true},
		{5, 900, -37.85f, 144.97f, 0.0f, 196, 726, 0.4447f, 0, 13.8591f, true},
		{5, 900, 19.40f, -99.09f, 2240.0f, -1, -1, 0.5000f, 1, 17.0000f, true},
		{5, 900, 19.40f, -99.09f, 2240.0f, -1, 726, 0.4447f, 0, 17.0000f, true},
		{5, 900, 19.40f, -99.09f, 2240.0f, 15, -1, 0.5000f, 1, 13.0930f, true},
		{5, 900, 19.40f, -99.09f, 2240.0f, 15, 726, 0.4447f, 0, 13.0930f, true},
		{5, 900, 19.40f, -99.09f, 2240.0f, 196, -1, 0.5000f, 1, 17.1603f, true},
		{5, 900, 19.40f, -99.09f, 2240.0f, 196, 726, 0.4447f, 0, 17.1603f, true},
		{5, 900, 26.03f, 50.51f, 0.0f, -1, -1, 0.5000f, 1, 17.0000f, true},
		{5, 900, 26.03f, 50.51f, 0.0f, -1, 726, 0.4447f, 0, 17.0000f, true},
		{5, 900, 26.03f, 50.51f, 0.0f, 15, -1, 0.5000f, 1, 20.0881f, true},
		{5, 900, 26.03f, 50.51f, 0.0f, 15, 726, 0.4447f, 0, 20.0881f, true},
		{5, 900, 26.03f, 50.51f, 0.0f, 196, -1, 0.5000f, 1, 36.4600f, true},
		{5, 900, 26.03f, 50.51f, 0.0f, 196, 726, 0.4447f, 0, 36.4600f, true},
	};
	for (const FFigure& F : Figures)
	{
		FApexSessionConditions C;
		C.Weather = static_cast<EApexWeather>(F.Weather);
		C.TimeOfDayMinutes = F.Minutes;
		C.DayOfYear = F.Day;
		C.SolarNoonMin = F.Noon;
		const FClimateSite Site = F.Lon > 900.0f ? FClimateSite(F.Lat) : FClimateSite(F.Lat, F.Lon, F.Alt);
		const FString What = FString::Printf(TEXT("%s %02d:%02d at %.2f %.2f %.0f m, day %d, noon %d"),
			*FApexSessionConditions::WeatherLabel(C.Weather), F.Minutes / 60, F.Minutes % 60, F.Lat, F.Lon, F.Alt, F.Day, F.Noon);
		TestTrue(*FString::Printf(TEXT("%s: auto air %.4f against %.4f"), *What, AutoAirTempCAt(C, Site), F.Auto),
			FMath::IsNearlyEqual(AutoAirTempCAt(C, Site), F.Auto, 0.002f));
		TestEqual(*FString::Printf(TEXT("%s: the air"), *What), AirTempC(C, Site), F.Air);
		TestTrue(*FString::Printf(TEXT("%s: snow drop %.4f against %.4f"), *What, SnowAirDropC(C, Site), F.Drop),
			FMath::IsNearlyEqual(SnowAirDropC(C, Site), F.Drop, 0.002f));
		TestEqual(*FString::Printf(TEXT("%s: winter sky"), *What), IsWinter(C, Site), F.bWinter);
	}

	// The Rust test's own claims: Silverstone's mid-January 15:00 near 10 °C,
	// July warmer than late May; no date is late May everywhere; the equator
	// has no seasons; a January overcast night at 50° N is a winter sky.
	auto At = [](EApexWeather Weather, int32 Hour, int32 Day)
	{
		FApexSessionConditions C;
		C.Weather = Weather;
		C.TimeOfDayMinutes = Hour * 60;
		C.DayOfYear = Day;
		return C;
	};
	// Silverstone's own site: the Atlantic edge keeps its January mild.
	const FClimateSite Silverstone(52.07f, -1.02f, 150.0f);
	const int32 Jan = AirTempC(At(EApexWeather::Sunny, 15, 15), Silverstone);
	TestTrue(*FString::Printf(TEXT("Silverstone in January, %d °C"), Jan), Jan >= 8 && Jan <= 12);
	TestTrue(TEXT("July warmer than May"), AirTempC(At(EApexWeather::Sunny, 15, 196), Silverstone) > AirTempC(At(EApexWeather::Sunny, 15, 145), Silverstone));
	// Up a plateau the air is thinner and colder: Mexico City's 2240 m take 11 °C off a sea-level twin.
	TestTrue(TEXT("height cools the air"),
		AirTempC(At(EApexWeather::Sunny, 15, 196), FClimateSite(19.40f, -99.09f, 2240.0f)) + 10
			<= AirTempC(At(EApexWeather::Sunny, 15, 196), FClimateSite(19.40f, -99.09f, 0.0f)));
	TestEqual(TEXT("no date is late May"), AirTempC(At(EApexWeather::Sunny, 15, -1), 52.07f), AirTempC(At(EApexWeather::Sunny, 15, -1)));
	TestEqual(TEXT("the equator has no seasons"), AirTempC(At(EApexWeather::Sunny, 15, 15), 1.29f), AirTempC(At(EApexWeather::Sunny, 15, 196), 1.29f));
	TestTrue(TEXT("a January overcast night is a winter sky"), IsWinter(At(EApexWeather::Overcast, 4, 15), 50.0f));
	TestFalse(TEXT("the same night in late May is not"), IsWinter(At(EApexWeather::Overcast, 4, -1), 50.0f));
	TestEqual(TEXT("snow's own air is the same in any month"),
		AutoAirTempCAt(At(EApexWeather::Snow, 13, 15), 50.0f), AutoAirTempCAt(At(EApexWeather::Snow, 13, 145), 50.0f));

	// A winter sky lays out a winter forecast: overcast and snow, no rain.
	FApexSessionConditions Night = At(EApexWeather::Overcast, 4, 15);
	Night.Changeable = 2;
	Night.TimeScale = 1;
	const TArray<FForecastChange> NightForecast = Forecast(Night, 3, 50.0f);
	bool bOnlyWinter = NightForecast.Num() > 0;
	for (const FForecastChange& Change : NightForecast)
	{
		bOnlyWinter &= Change.Weather == EApexWeather::Overcast || Change.Weather == EApexWeather::Snow;
	}
	TestTrue(TEXT("a January night's forecast is overcast and snow"), bOnlyWinter);
	return true;
}

// -----------------------------------------------------------------------------
// A forecast the host writes, held to the server's rules as it is edited.
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexCreateSessionWrittenForecastTest,
	"ApexSim.UI.CreateSession.WrittenForecast",
	ApexTestFlags)

bool FApexCreateSessionWrittenForecastTest::RunTest(const FString& Parameters)
{
	using namespace ApexCreateSession;

	auto Valid = [](EApexWeather Start, const TArray<FApexForecastStep>& Steps)
	{
		return FApexSessionConditions::ClampedForecast(Start, Steps) == Steps;
	};
	auto Step = [](int32 AtMin, EApexWeather Weather)
	{
		FApexForecastStep Out;
		Out.AtMin = AtMin;
		Out.Weather = Weather;
		return Out;
	};

	TArray<FApexForecastStep> Steps;
	TestTrue(TEXT("a first step"), AddForecastStep(Steps, EApexWeather::Sunny));
	TestEqual(TEXT("30 min in"), Steps[0].AtMin, ForecastStepGapMin);
	TestEqual(TEXT("a change from the start"), Steps[0].Weather, EApexWeather::Cloudy);
	while (AddForecastStep(Steps, EApexWeather::Sunny)) {}
	TestEqual(TEXT("ten steps at most"), Steps.Num(), FApexSessionConditions::MaxForecastSteps);
	TestTrue(TEXT("every step added keeps the rules"), Valid(EApexWeather::Sunny, Steps));

	// Moving is held a gap from the neighbours.
	MoveForecastStep(Steps, 0, -60);
	TestEqual(TEXT("held at 4 min"), Steps[0].AtMin, FApexSessionConditions::ForecastMinGapMin);
	MoveForecastStep(Steps, 0, 500);
	TestEqual(TEXT("held 4 min before the next"), Steps[0].AtMin, Steps[1].AtMin - FApexSessionConditions::ForecastMinGapMin);
	MoveForecastStep(Steps, Steps.Num() - 1, 100000);
	TestEqual(TEXT("the last held at the horizon"), Steps.Last().AtMin, FApexSessionConditions::ForecastHorizonMin);
	TestTrue(TEXT("moves keep the rules"), Valid(EApexWeather::Sunny, Steps));

	// The weather steps past the one before it, and a repeat after it goes.
	TArray<FApexForecastStep> Three;
	AddForecastStep(Three, EApexWeather::Sunny);
	AddForecastStep(Three, EApexWeather::Sunny);
	AddForecastStep(Three, EApexWeather::Sunny);
	// Cloudy, Overcast, Light rain.
	CycleForecastWeather(Three, 0, -1, EApexWeather::Sunny);
	TestEqual(TEXT("back from cloudy skips sunny (the start)"), Three[0].Weather, EApexWeather::Snow);
	CycleForecastWeather(Three, 1, 1, EApexWeather::Sunny);
	TestEqual(TEXT("overcast on"), Three[1].Weather, EApexWeather::LightRain);
	TestEqual(TEXT("the light rain after it, now a repeat, went"), Three.Num(), 2);
	TestTrue(TEXT("cycling keeps the rules"), Valid(EApexWeather::Sunny, Three));

	// Removing a step drops one after it that now repeats the weather before.
	TArray<FApexForecastStep> Swing = { Step(30, EApexWeather::LightRain), Step(60, EApexWeather::Sunny), Step(90, EApexWeather::Cloudy) };
	RemoveForecastStep(Swing, 0, EApexWeather::Sunny);
	TestEqual(TEXT("sunny after a sunny start went too"), Swing.Num(), 1);
	TestEqual(TEXT("cloudy stays"), Swing.Num() > 0 ? Swing[0].Weather : EApexWeather::Sunny, EApexWeather::Cloudy);

	// Kept to its sky: a winter sky's rain is snow, a summer sky's snow rain.
	FApexSessionConditions Winter;
	Winter.Weather = EApexWeather::Overcast;
	Winter.TimeOfDayMinutes = 4 * 60;
	Winter.DayOfYear = 15;
	Winter.Forecast = { Step(20, EApexWeather::HeavyRain), Step(50, EApexWeather::Overcast) };
	const TArray<FApexForecastStep> Kept = KeptToSky(Winter, 50.0f);
	TestEqual(TEXT("winter rain falls as snow"), Kept.Num() > 0 ? Kept[0].Weather : EApexWeather::Sunny, EApexWeather::Snow);
	FApexSessionConditions Summer;
	Summer.Forecast = { Step(20, EApexWeather::Snow) };
	const TArray<FApexForecastStep> KeptSummer = KeptToSky(Summer);
	TestEqual(TEXT("summer snow falls as light rain"), KeptSummer.Num() > 0 ? KeptSummer[0].Weather : EApexWeather::Sunny, EApexWeather::LightRain);

	// The forecast a session follows: the written one, whatever Changeable says.
	FApexSessionConditions Written;
	Written.Changeable = 3;
	Written.Forecast = { Step(20, EApexWeather::LightRain), Step(45, EApexWeather::HeavyRain) };
	const TArray<FForecastChange> Follows = SessionForecast(Written, 7);
	TestEqual(TEXT("the written steps"), Follows.Num(), 2);
	TestEqual(TEXT("in seconds"), Follows.Num() > 1 ? Follows[1].AtS : 0.0f, 45.0f * 60.0f);
	FApexSessionConditions Seeded = Written;
	Seeded.Forecast.Reset();
	TestEqual(TEXT("without one, the seed's"), SessionForecast(Seeded, 7).Num(), Forecast(Seeded, 7).Num());
	TestEqual(TEXT("a step's minute"), StepMinuteLabel(65), FString(TEXT("+1 h 05")));
	TestTrue(TEXT("Describe names it"), Written.Describe().Contains(TEXT("forecast light rain at 20 min, heavy rain at 45 min")));
	return true;
}

// -----------------------------------------------------------------------------
// Qualifying's format, lap allowance and a weekend phase's damage and aids.
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexCreateSessionQualifyingFormatTest,
	"ApexSim.UI.CreateSession.QualifyingFormat",
	ApexTestFlags)

bool FApexCreateSessionQualifyingFormatTest::RunTest(const FString& Parameters)
{
	using namespace ApexCreateSession;
	TestEqual(TEXT("three timed rounds"), WeekendRoundsLabel(3, 10, false), FString(TEXT("Q1-Q3 10 min each")));
	TestEqual(TEXT("Q3 a shoot-out"), WeekendRoundsLabel(3, 10, true), FString(TEXT("Q1-Q2 10 min · Q3 shoot-out")));
	TestEqual(TEXT("Q2 a shoot-out"), WeekendRoundsLabel(2, 12, true), FString(TEXT("Q1 12 min · Q2 shoot-out")));
	TestEqual(TEXT("one round, a shoot-out, no length"), WeekendRoundsLabel(1, 10, true), FString(TEXT("Shoot-out")));
	TestEqual(TEXT("none below"), StepLapAllowance(0, -1), 0);
	TestEqual(TEXT("one up"), StepLapAllowance(0, 1), 1);
	TestEqual(TEXT("held at 30"), StepLapAllowance(30, 1), ApexQualifyingLapAllowance::MaxLaps);
	TestEqual(TEXT("None"), LapAllowanceLabel(0), FString(TEXT("None")));
	TestEqual(TEXT("1 lap"), LapAllowanceLabel(1), FString(TEXT("1 lap")));
	TestEqual(TEXT("3 laps"), LapAllowanceLabel(3), FString(TEXT("3 laps")));
	TestEqual(TEXT("the race's is the first chip"), PhaseDamageChip(-1), 0);
	TestEqual(TEXT("visual the last"), PhaseDamageChip(static_cast<int32>(EApexDamageLevel::VisualOnly)), PhaseDamageChips - 1);
	for (int32 Chip = 0; Chip < PhaseDamageChips; ++Chip)
	{
		TestEqual(*FString::Printf(TEXT("chip %d round trip"), Chip), PhaseDamageChip(PhaseDamageOfChip(Chip)), Chip);
	}

	// A weekend phase's own aids: the race's, or a preset.
	TestEqual(TEXT("assists: the race's first"), PhaseAssistChip(-1), 0);
	TestEqual(TEXT("then each preset"), PhaseAssistChips, 1 + static_cast<int32>(EAssistPreset::Count));
	TestEqual(TEXT("the race's chip"), PhaseAssistChipLabel(0), FString(TEXT("Race's")));
	TestEqual(TEXT("Pro the last chip"), PhaseAssistChipLabel(PhaseAssistChips - 1), FString(TEXT("Pro")));
	for (int32 Chip = 0; Chip < PhaseAssistChips; ++Chip)
	{
		TestEqual(*FString::Printf(TEXT("assist chip %d round trip"), Chip), PhaseAssistChip(PhaseAssistOfChip(Chip)), Chip);
	}
	TestTrue(TEXT("Pro allows nothing"), PhaseAssists(static_cast<int32>(EAssistPreset::Pro)) == AssistPreset(EAssistPreset::Pro));
	TestEqual(TEXT("a preset summarised"), AssistsSummary(AssistPreset(EAssistPreset::Club)), FString(TEXT("CLUB ASSISTS")));
	FApexAllowedAssists NoLine;
	NoLine.bRacingLine = false;
	TestEqual(TEXT("a mix counted"), AssistsSummary(NoLine), FString(TEXT("4/5 ASSISTS")));
	TestEqual(TEXT("no note without its own"), PhaseAssistsNote(false, NoLine), FString());
	TestEqual(TEXT("a lobby note"), PhaseAssistsNote(true, AssistPreset(EAssistPreset::Pro)), FString(TEXT(", PRO ASSISTS")));
	{
		UGameInstance* Instance = NewObject<UGameInstance>(GetTransientPackage());
		UApexMenuFlowSubsystem* Flow = NewObject<UApexMenuFlowSubsystem>(Instance);
		Flow->CreateStartingMode = EApexGameMode::Race;
		Flow->bCreateWeekend = true;
		Flow->CreateWeekendPracticeMinutes = 0;
		Flow->CreateWeekendPracticeAssists = static_cast<int32>(EAssistPreset::Pro);
		Flow->CreateWeekendQualifyingAssists = static_cast<int32>(EAssistPreset::Club);
		FApexWeekendFormat Sent = Flow->WeekendToSend();
		TestFalse(TEXT("no practice, no practice aids"), Sent.bHasPracticeAssists);
		TestTrue(TEXT("qualifying sends its own"), Sent.bHasQualifyingAssists);
		TestTrue(TEXT("as Club"), Sent.QualifyingAssists == AssistPreset(EAssistPreset::Club));
		Flow->CreateWeekendPracticeMinutes = 10;
		Flow->CreateWeekendQualifyingAssists = -1;
		Sent = Flow->WeekendToSend();
		TestTrue(TEXT("practice sends its own"), Sent.bHasPracticeAssists && Sent.PracticeAssists == AssistPreset(EAssistPreset::Pro));
		TestFalse(TEXT("the race's sends none"), Sent.bHasQualifyingAssists);
	}
	TestEqual(TEXT("no allowance, no count"), ApexQualifyingLapAllowance::LapsLeft(0, 3), -1);
	TestEqual(TEXT("two of three run"), ApexQualifyingLapAllowance::LapsLeft(3, 2), 1);
	TestEqual(TEXT("used"), ApexQualifyingLapAllowance::LapsLeft(3, 5), 0);
	return true;
}

// -----------------------------------------------------------------------------
// The race-control rules: the reverse grid's chips and what a create sends.
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexCreateSessionRaceControlTest,
	"ApexSim.UI.CreateSession.RaceControl",
	ApexTestFlags)

bool FApexCreateSessionRaceControlTest::RunTest(const FString& Parameters)
{
	using namespace ApexCreateSession;
	TestEqual(TEXT("off"), ReverseGridChip(0), 0);
	TestEqual(TEXT("a reverse of one is off"), ReverseGridChip(1), 0);
	TestEqual(TEXT("top 8"), ReverseGridChip(8), 2);
	TestEqual(TEXT("an unoffered 7 shows on 8"), ReverseGridChip(7), 2);
	TestEqual(TEXT("the whole field"), ReverseGridChip(FApexRaceRules::ReverseWholeField), 4);
	for (int32 Chip = 0; Chip < UE_ARRAY_COUNT(ReverseGridChoices); ++Chip)
	{
		TestEqual(*FString::Printf(TEXT("reverse chip %d round trip"), Chip), ReverseGridChip(ReverseGridChoices[Chip]), Chip);
	}

	// A subsystem lives within a game instance; not initialised, so no profile is read.
	UGameInstance* Instance = NewObject<UGameInstance>(GetTransientPackage());
	UApexMenuFlowSubsystem* Flow = NewObject<UApexMenuFlowSubsystem>(Instance);
	Flow->CreateRaceRules.Start = EApexStartType::Rolling;
	Flow->CreateRaceRules.bFormationLap = true;
	Flow->CreateRaceRules.bYellowPenalty = true;
	Flow->CreateRaceRules.SafetyCar = EApexSafetyCar::Virtual;
	Flow->CreateRaceRules.ReverseGrid = 8;
	Flow->CreateStartingMode = EApexGameMode::Race;
	const FApexRaceRules Sent = Flow->RaceRulesToSend();
	TestEqual(TEXT("a race sends its start"), Sent.Start, EApexStartType::Rolling);
	TestFalse(TEXT("a rolling start sends no formation lap"), Sent.bFormationLap);
	TestTrue(TEXT("a race sends its penalties"), Sent.bYellowPenalty);
	TestEqual(TEXT("and its safety car"), Sent.SafetyCar, EApexSafetyCar::Virtual);
	TestEqual(TEXT("and its reverse grid"), static_cast<int32>(Sent.ReverseGrid), 8);
	TestEqual(TEXT("named in the summary"), Sent.Describe(),
		FString(TEXT("rolling start · 1 penalty · VSC · reverse 8")));
	Flow->CreateStartingMode = EApexGameMode::Qualification;
	TestFalse(TEXT("qualifying before a race sends them"), Flow->RaceRulesToSend().IsDefault());
	Flow->CreateStartingMode = EApexGameMode::Hotlap;
	TestTrue(TEXT("a hotlap sends none"), Flow->RaceRulesToSend().IsDefault());
	Flow->CreateStartingMode = EApexGameMode::FreePractice;
	TestTrue(TEXT("practice sends none"), Flow->RaceRulesToSend().IsDefault());

	// The tyre, parc fermé and stint rules: the qualifying ones only where
	// the session qualifies, the weekend's sets only in a weekend.
	TestEqual(TEXT("no sets"), ChoiceChip(QualifyingTyreSetChoices, 0), 0);
	TestEqual(TEXT("two sets"), ChoiceChip(QualifyingTyreSetChoices, 2), 2);
	TestEqual(TEXT("an unoffered 12 shows on 13"), ChoiceChip(WeekendTyreSetChoices, 12), 4);
	TestEqual(TEXT("every car on its set"), ChoiceChip(QualifyingTyreStartChoices, FApexRaceRules::QualifyingTyreStartAll), 2);
	TestEqual(TEXT("past the last"), ChoiceOfChip(StintMinuteChoices, 9), 60);
	FApexRaceRules& Rules = Flow->CreateRaceRules;
	Rules = FApexRaceRules();
	Rules.QualifyingTyreSets = 2;
	Rules.QualifyingTyreStart = 10;
	Rules.bParcFerme = true;
	Rules.WeekendTyreSets = 8;
	Rules.MaxStintLaps = 20;
	Rules.MaxStintMinutes = 45;
	Flow->bCreateWeekend = false;
	Flow->CreateStartingMode = EApexGameMode::Race;
	const FApexRaceRules RaceOnly = Flow->RaceRulesToSend();
	TestEqual(TEXT("a race without qualifying: no qualifying sets"), static_cast<int32>(RaceOnly.QualifyingTyreSets), 0);
	TestEqual(TEXT("nor the start on them"), static_cast<int32>(RaceOnly.QualifyingTyreStart), 0);
	TestFalse(TEXT("nor parc ferme"), RaceOnly.bParcFerme);
	TestEqual(TEXT("nor a weekend's sets"), static_cast<int32>(RaceOnly.WeekendTyreSets), 0);
	TestEqual(TEXT("but its stint laps"), RaceOnly.MaxStintLaps, 20);
	TestEqual(TEXT("and minutes"), RaceOnly.MaxStintMinutes, 45);
	Flow->CreateStartingMode = EApexGameMode::Qualification;
	const FApexRaceRules Qualifying = Flow->RaceRulesToSend();
	TestEqual(TEXT("qualifying sends its sets"), static_cast<int32>(Qualifying.QualifyingTyreSets), 2);
	TestTrue(TEXT("and parc ferme"), Qualifying.bParcFerme);
	TestEqual(TEXT("but no weekend's sets"), static_cast<int32>(Qualifying.WeekendTyreSets), 0);
	Flow->CreateStartingMode = EApexGameMode::Race;
	Flow->bCreateWeekend = true;
	const FApexRaceRules Weekend = Flow->RaceRulesToSend();
	TestEqual(TEXT("a weekend sends its allocation"), static_cast<int32>(Weekend.WeekendTyreSets), 8);
	TestEqual(TEXT("and the start on qualifying sets"), static_cast<int32>(Weekend.QualifyingTyreStart), 10);
	return true;
}

#endif
