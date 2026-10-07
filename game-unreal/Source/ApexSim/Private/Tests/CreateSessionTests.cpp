#include "ApexTestCommon.h"
#include "UI/ApexCreateSessionModel.h"

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

#endif
