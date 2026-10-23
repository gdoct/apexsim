#include "ApexRepoPaths.h"
#include "ApexTestCommon.h"
#include "Audio/ApexRadio.h"
#include "Hud/ApexHudData.h"
#include "Internationalization/Regex.h"
#include "Misc/FileHelper.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace ApexRadioTest
{
	/** A car at racing speed in a race, nothing around it. */
	FApexRadioInputs Racing()
	{
		FApexRadioInputs In;
		In.bDriving = true;
		In.Mode = TEXT("race");
		In.SpeedKph = 180.0f;
		In.FuelState = TEXT("ok");
		In.FuelLaps = 20.0f;
		In.TyreWearMaxPct = 5.0f;
		In.Lap = 2;
		In.LapsLeft = 9;
		return In;
	}

	/** Steps the triggers at 60 Hz from From to To seconds, collecting what they say. */
	TArray<FName> Run(FApexRadioTriggers& Triggers, const FApexRadioInputs& In, double From, double To)
	{
		TArray<FName> Out;
		for (double Now = From; Now < To - 1e-9; Now += 1.0 / 60.0)
		{
			Triggers.Step(In, Now, Out);
		}
		return Out;
	}

	FString Join(const TArray<FName>& Lines)
	{
		TArray<FString> Parts;
		for (const FName Line : Lines)
		{
			Parts.Add(Line.ToString());
		}
		return FString::Join(Parts, TEXT(" "));
	}

	/** A primed set of triggers: the first frame takes stock. */
	FApexRadioTriggers Primed(const FApexRadioInputs& In)
	{
		FApexRadioTriggers Triggers;
		TArray<FName> Ignored;
		Triggers.Step(In, 0.0, Ignored);
		return Triggers;
	}

	TArray<uint8> MakeWav(int32 Rate, int32 Channels, int32 Bits, const TArray<int16>& Samples)
	{
		TArray<uint8> B;
		auto U32 = [&B](uint32 V) { for (int32 I = 0; I < 4; ++I) { B.Add(uint8(V >> (8 * I))); } };
		auto U16 = [&B](uint16 V) { B.Add(uint8(V)); B.Add(uint8(V >> 8)); };
		auto Tag = [&B](const char* T) { for (int32 I = 0; I < 4; ++I) { B.Add(uint8(T[I])); } };
		const uint32 DataBytes = Samples.Num() * 2;
		Tag("RIFF"); U32(4 + 8 + 16 + 8 + 3 + 1 + 8 + DataBytes); Tag("WAVE");
		Tag("fmt "); U32(16); U16(1); U16(uint16(Channels)); U32(uint32(Rate)); U32(uint32(Rate * Channels * 2)); U16(uint16(Channels * 2)); U16(uint16(Bits));
		// An odd-sized chunk the reader must skip, padding and all.
		Tag("LIST"); U32(3); B.Add('a'); B.Add('b'); B.Add('c'); B.Add(0);
		Tag("data"); U32(DataBytes);
		for (const int16 S : Samples) { U16(uint16(S)); }
		return B;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexRadioSpotterTest, "ApexSim.Audio.Radio.Spotter", ApexTestFlags)

bool FApexRadioSpotterTest::RunTest(const FString& Parameters)
{
	using namespace ApexRadioTest;
	FApexRadioInputs In = Racing();

	// Taking stock: a car already alongside when the radio comes on is not called.
	{
		FApexRadioInputs Alongside = In;
		Alongside.bLeft = true;
		FApexRadioTriggers Triggers = Primed(Alongside);
		TestEqual(TEXT("primed alongside says nothing"), Join(Run(Triggers, Alongside, 0.0, 2.0)), FString());
	}

	FApexRadioTriggers Triggers = Primed(In);
	// A car drawing alongside on the left: called once it has held there.
	In.bLeft = true;
	TestEqual(TEXT("not before it holds"), Join(Run(Triggers, In, 0.0, 0.1)), FString());
	TestEqual(TEXT("car left"), Join(Run(Triggers, In, 0.1, 1.0)), FString(TEXT("car_left")));
	// A second car on the right: three wide.
	In.bRight = true;
	TestEqual(TEXT("three wide"), Join(Run(Triggers, In, 1.0, 2.0)), FString(TEXT("three_wide")));
	// The left one drops back: clear left (the right one is still there).
	In.bLeft = false;
	TestEqual(TEXT("not clear at once"), Join(Run(Triggers, In, 2.0, 2.4)), FString());
	TestEqual(TEXT("clear left"), Join(Run(Triggers, In, 2.4, 3.5)), FString(TEXT("clear_left")));
	In.bRight = false;
	TestEqual(TEXT("clear right"), Join(Run(Triggers, In, 3.5, 5.0)), FString(TEXT("clear_right")));

	// The radar's edge flickering: a blip shorter than the hold is never called.
	for (int32 Blip = 0; Blip < 5; ++Blip)
	{
		In.bRight = true;
		TestEqual(TEXT("blip on"), Join(Run(Triggers, In, 5.0 + Blip, 5.1 + Blip)), FString());
		In.bRight = false;
		TestEqual(TEXT("blip off"), Join(Run(Triggers, In, 5.1 + Blip, 6.0 + Blip)), FString());
	}
	// Both sides at once, then both gone at once.
	In.bLeft = In.bRight = true;
	TestEqual(TEXT("both at once"), Join(Run(Triggers, In, 20.0, 21.0)), FString(TEXT("three_wide")));
	In.bLeft = In.bRight = false;
	TestEqual(TEXT("both gone"), Join(Run(Triggers, In, 21.0, 22.0)), FString(TEXT("clear")));

	// Slow, in the pit lane or on the grid: nothing is called alongside.
	for (int32 Case = 0; Case < 3; ++Case)
	{
		FApexRadioInputs Quiet = Racing();
		if (Case == 0) { Quiet.SpeedKph = 20.0f; }
		if (Case == 1) { Quiet.bInPitLane = true; }
		if (Case == 2) { Quiet.Mode = TEXT("grid"); }
		FApexRadioTriggers QuietTriggers = Primed(Quiet);
		Quiet.bLeft = true;
		TestEqual(FString::Printf(TEXT("quiet case %d"), Case), Join(Run(QuietTriggers, Quiet, 0.0, 2.0)), FString());
	}

	// Not driving any more forgets everything; the next frame takes stock again.
	In.bLeft = true;
	FApexRadioInputs Away = In;
	Away.bDriving = false;
	Run(Triggers, Away, 30.0, 30.1);
	TestEqual(TEXT("after a reset"), Join(Run(Triggers, In, 30.1, 32.0)), FString());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexRadioFlagsTest, "ApexSim.Audio.Radio.Flags", ApexTestFlags)

bool FApexRadioFlagsTest::RunTest(const FString& Parameters)
{
	using namespace ApexRadioTest;
	FApexRadioInputs In = Racing();
	FApexRadioTriggers Triggers = Primed(In);

	In.Yellow = 1;
	TestEqual(TEXT("yellow"), Join(Run(Triggers, In, 0.0, 1.0)), FString(TEXT("yellow_ahead")));
	In.Yellow = 2;
	TestEqual(TEXT("single to double"), Join(Run(Triggers, In, 1.0, 2.0)), FString(TEXT("double_yellow")));
	In.Yellow = 1;
	TestEqual(TEXT("double to single is not news"), Join(Run(Triggers, In, 2.0, 3.0)), FString());
	In.Yellow = 0;
	TestEqual(TEXT("not clear at once"), Join(Run(Triggers, In, 3.0, 4.0)), FString());
	TestEqual(TEXT("track clear"), Join(Run(Triggers, In, 4.0, 6.0)), FString(TEXT("yellow_clear")));

	// Out for under the hold and back: no clear between.
	In.Yellow = 1;
	Run(Triggers, In, 6.0, 7.0);
	In.Yellow = 0;
	Run(Triggers, In, 7.0, 7.5);
	In.Yellow = 1;
	TestEqual(TEXT("back before clear"), Join(Run(Triggers, In, 7.5, 9.0)), FString(TEXT("yellow_ahead")));

	In.Yellow = 0;
	Run(Triggers, In, 9.0, 12.0);
	In.bBlue = true;
	TestEqual(TEXT("blue"), Join(Run(Triggers, In, 12.0, 13.0)), FString(TEXT("blue_flag")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexRadioEngineerTest, "ApexSim.Audio.Radio.Engineer", ApexTestFlags)

bool FApexRadioEngineerTest::RunTest(const FString& Parameters)
{
	using namespace ApexRadioTest;
	FApexRadioInputs In = Racing();
	FApexRadioTriggers Triggers = Primed(In);

	// The race's phases.
	In.Phase = TEXT("safety_car");
	TestEqual(TEXT("safety car"), Join(Run(Triggers, In, 0.0, 1.0)), FString(TEXT("safety_car")));
	In.Phase = TEXT("sc_in_this_lap");
	TestEqual(TEXT("in this lap"), Join(Run(Triggers, In, 1.0, 2.0)), FString(TEXT("sc_in")));
	In.Phase = FString();
	TestEqual(TEXT("green"), Join(Run(Triggers, In, 2.0, 3.0)), FString(TEXT("green_green")));
	In.Phase = TEXT("vsc");
	TestEqual(TEXT("vsc"), Join(Run(Triggers, In, 3.0, 4.0)), FString(TEXT("vsc")));
	In.Phase = TEXT("vsc_ending");
	TestEqual(TEXT("vsc ending"), Join(Run(Triggers, In, 4.0, 5.0)), FString());
	In.Phase = FString();
	TestEqual(TEXT("green after vsc"), Join(Run(Triggers, In, 5.0, 6.0)), FString(TEXT("green_green")));
	{
		// The formation lap ending is the start, not a restart.
		FApexRadioInputs Formation = Racing();
		Formation.Phase = TEXT("formation");
		FApexRadioTriggers Start = Primed(Formation);
		Formation.Phase = FString();
		TestEqual(TEXT("formation over"), Join(Run(Start, Formation, 0.0, 1.0)), FString());
	}

	// The driver asks for the stop.
	In.bPitRequested = true;
	TestEqual(TEXT("box confirmed"), Join(Run(Triggers, In, 6.0, 7.0)), FString(TEXT("box_confirmed")));
	In.bPitRequested = false;
	Run(Triggers, In, 7.0, 8.0);

	// Fuel short of the flag: at the line, the laps it has left.
	In.FuelState = TEXT("short");
	In.FuelLaps = 3.6f;
	TestEqual(TEXT("mid-lap fuel says nothing"), Join(Run(Triggers, In, 8.0, 9.0)), FString());
	In.Lap = 3;
	In.LapsLeft = 8;
	TestEqual(TEXT("fuel laps at the line"), Join(Run(Triggers, In, 9.0, 10.0)), FString(TEXT("fuel_laps_3")));
	In.FuelState = TEXT("critical");
	In.FuelLaps = 0.8f;
	TestEqual(TEXT("fuel critical"), Join(Run(Triggers, In, 10.0, 11.0)), FString(TEXT("fuel_critical")));
	In.FuelState = TEXT("ok");
	In.FuelLaps = 20.0f;

	// Worn tyres once a set; a fresh set re-arms the call.
	In.TyreWearMaxPct = 75.0f;
	TestEqual(TEXT("tyres worn"), Join(Run(Triggers, In, 11.0, 12.0)), FString(TEXT("tyres_worn")));
	In.TyreWearMaxPct = 80.0f;
	TestEqual(TEXT("said once"), Join(Run(Triggers, In, 12.0, 13.0)), FString());
	In.TyreWearMaxPct = 2.0f;
	Run(Triggers, In, 13.0, 14.0);
	In.TyreWearMaxPct = 71.0f;
	TestEqual(TEXT("the next set"), Join(Run(Triggers, In, 14.0, 15.0)), FString(TEXT("tyres_worn")));

	// A stop still owed late in the race.
	In.StopsOwed = 1;
	In.Lap = 9;
	In.LapsLeft = 2;
	TestEqual(TEXT("box this lap"), Join(Run(Triggers, In, 15.0, 16.0)), FString(TEXT("box_this_lap")));

	// The last lap, the flag and the place.
	In.StopsOwed = 0;
	In.Lap = 10;
	In.LapsLeft = 1;
	In.bFinalLap = true;
	TestEqual(TEXT("last lap"), Join(Run(Triggers, In, 16.0, 17.0)), FString(TEXT("last_lap")));
	In.Lap = 11;
	In.FinishPosition = 3;
	TestEqual(TEXT("chequered and place"), Join(Run(Triggers, In, 17.0, 18.0)), FString(TEXT("chequered position_3")));
	{
		// A place without a line: the flag alone.
		FApexRadioInputs Back = Racing();
		FApexRadioTriggers Finish = Primed(Back);
		Back.FinishPosition = ApexRadio::MaxPositionLine + 1;
		TestEqual(TEXT("past the lines"), Join(Run(Finish, Back, 0.0, 1.0)), FString(TEXT("chequered")));
	}
	{
		// Practice has no race calls: no last lap, no flag.
		FApexRadioInputs Practice = Racing();
		Practice.Mode = TEXT("practice");
		FApexRadioTriggers PracticeTriggers = Primed(Practice);
		Practice.bFinalLap = true;
		Practice.FinishPosition = 1;
		Practice.Phase = TEXT("safety_car");
		TestEqual(TEXT("practice"), Join(Run(PracticeTriggers, Practice, 0.0, 1.0)), FString());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexRadioQueueTest, "ApexSim.Audio.Radio.Queue", ApexTestFlags)

bool FApexRadioQueueTest::RunTest(const FString& Parameters)
{
	bool bInterrupt = false;
	{
		// The most urgent first; equal ones in order.
		FApexRadioQueue Queue;
		Queue.Push(TEXT("tyres_worn"), 0.0);
		Queue.Push(TEXT("chequered"), 0.0);
		Queue.Push(TEXT("position_2"), 0.0);
		Queue.Push(TEXT("yellow_ahead"), 0.0);
		TestEqual(TEXT("yellow first"), Queue.Pop(0.0, false, bInterrupt), FName(TEXT("yellow_ahead")));
		Queue.Started(TEXT("yellow_ahead"), 0.0, 2.0);
		TestEqual(TEXT("busy"), Queue.Pop(1.0, false, bInterrupt), FName());
		TestEqual(TEXT("chequered"), Queue.Pop(2.5, false, bInterrupt), FName(TEXT("chequered")));
		Queue.Started(TEXT("chequered"), 2.5, 1.0);
		TestEqual(TEXT("then the place"), Queue.Pop(4.0, false, bInterrupt), FName(TEXT("position_2")));
		Queue.Started(TEXT("position_2"), 4.0, 1.0);
		TestEqual(TEXT("then tyres"), Queue.Pop(6.0, false, bInterrupt), FName(TEXT("tyres_worn")));
	}
	{
		// A side call replaces the one waiting; a stale one is dropped.
		FApexRadioQueue Queue;
		Queue.Started(TEXT("yellow_ahead"), 0.0, 3.0);
		Queue.Push(TEXT("car_left"), 0.5);
		Queue.Push(TEXT("three_wide"), 0.6);
		TestEqual(TEXT("one waits"), Queue.Num(), 1);
		TestEqual(TEXT("stale side call"), Queue.Pop(3.5, false, bInterrupt), FName());
		TestEqual(TEXT("dropped"), Queue.Num(), 0);
	}
	{
		// A spotter call cuts an engineer line short.
		FApexRadioQueue Queue;
		Queue.Push(TEXT("safety_car"), 0.0);
		TestEqual(TEXT("engineer"), Queue.Pop(0.0, false, bInterrupt), FName(TEXT("safety_car")));
		Queue.Started(TEXT("safety_car"), 0.0, 3.0);
		Queue.Push(TEXT("car_right"), 1.0);
		TestEqual(TEXT("cuts in"), Queue.Pop(1.0, false, bInterrupt), FName(TEXT("car_right")));
		TestTrue(TEXT("interrupt"), bInterrupt);
		Queue.Started(TEXT("car_right"), 1.0, 0.6);
		// ...but not another spotter call, and a flag does not cut in.
		Queue.Push(TEXT("clear_right"), 1.2);
		TestEqual(TEXT("waits for the spotter"), Queue.Pop(1.2, false, bInterrupt), FName());
		TestEqual(TEXT("after"), Queue.Pop(1.9, false, bInterrupt), FName(TEXT("clear_right")));
		TestFalse(TEXT("not an interrupt"), bInterrupt);
		Queue.Started(TEXT("clear_right"), 1.9, 0.6);
		Queue.Push(TEXT("green_green"), 3.0);
		Queue.Started(TEXT("green_green"), 3.0, 3.0);
		Queue.Push(TEXT("blue_flag"), 3.5);
		TestEqual(TEXT("a flag waits"), Queue.Pop(3.5, false, bInterrupt), FName());
	}
	{
		// Engineer lines hold while a car is alongside; the spotter's do not.
		FApexRadioQueue Queue;
		Queue.Push(TEXT("fuel_laps_2"), 0.0);
		Queue.Push(TEXT("blue_flag"), 0.0);
		TestEqual(TEXT("blue goes"), Queue.Pop(0.0, true, bInterrupt), FName(TEXT("blue_flag")));
		Queue.Started(TEXT("blue_flag"), 0.0, 1.0);
		TestEqual(TEXT("fuel holds"), Queue.Pop(2.0, true, bInterrupt), FName());
		TestEqual(TEXT("fuel after"), Queue.Pop(3.0, false, bInterrupt), FName(TEXT("fuel_laps_2")));
	}
	{
		// The cool-down: a flag said a moment ago is not said again.
		FApexRadioQueue Queue;
		Queue.Push(TEXT("blue_flag"), 0.0);
		Queue.Pop(0.0, false, bInterrupt);
		Queue.Started(TEXT("blue_flag"), 0.0, 1.0);
		Queue.Push(TEXT("blue_flag"), 5.0);
		TestEqual(TEXT("in the cool-down"), Queue.Num(), 0);
		Queue.Push(TEXT("blue_flag"), 25.0);
		TestEqual(TEXT("after it"), Queue.Num(), 1);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexRadioHudTest, "ApexSim.Audio.Radio.FromHud", ApexTestFlags)

bool FApexRadioHudTest::RunTest(const FString& Parameters)
{
	FApexHudData Data;
	Data.Set(TEXT("session.mode"), TEXT("race"));
	Data.Set(TEXT("car.speed_kph"), 212.0f);
	Data.Set(TEXT("radar.left"), true);
	Data.Set(TEXT("flag.yellow"), 2);
	Data.Set(TEXT("race.phase"), TEXT("vsc"));
	Data.Set(TEXT("fuel.state"), TEXT("short"));
	Data.Set(TEXT("fuel.laps"), 2.5f);
	Data.Set(TEXT("lap.current"), 7);
	Data.SetNone(TEXT("lap.laps_left"));
	Data.SetNone(TEXT("tyre.wear_max_pct"));

	FApexRadioInputs In = ApexRadio::InputsFromHud(Data, true);
	TestTrue(TEXT("driving"), In.bDriving);
	TestEqual(TEXT("mode"), In.Mode, FString(TEXT("race")));
	TestEqual(TEXT("speed"), In.SpeedKph, 212.0f);
	TestTrue(TEXT("left"), In.bLeft);
	TestFalse(TEXT("right"), In.bRight);
	TestEqual(TEXT("yellow"), In.Yellow, 2);
	TestEqual(TEXT("phase"), In.Phase, FString(TEXT("vsc")));
	TestEqual(TEXT("fuel laps"), In.FuelLaps, 2.5f);
	TestEqual(TEXT("lap"), In.Lap, 7);
	TestEqual(TEXT("no distance"), In.LapsLeft, -1);
	TestTrue(TEXT("no wear"), In.TyreWearMaxPct < 0.0f);

	TestFalse(TEXT("not the caller's car"), ApexRadio::InputsFromHud(Data, false).bDriving);
	Data.SetNone(TEXT("car.speed_kph"));
	TestFalse(TEXT("no car"), ApexRadio::InputsFromHud(Data, true).bDriving);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexRadioWavTest, "ApexSim.Audio.Radio.Wav", ApexTestFlags)

bool FApexRadioWavTest::RunTest(const FString& Parameters)
{
	using namespace ApexRadioTest;
	TArray<float> Samples;
	int32 Rate = 0;
	TestTrue(TEXT("mono"), ApexRadio::ParseWav(MakeWav(16000, 1, 16, { 0, 16384, -32768 }), Samples, Rate));
	TestEqual(TEXT("rate"), Rate, 16000);
	TestEqual(TEXT("count"), Samples.Num(), 3);
	if (Samples.Num() == 3)
	{
		TestEqual(TEXT("half"), Samples[1], 0.5f);
		TestEqual(TEXT("floor"), Samples[2], -1.0f);
	}
	TestTrue(TEXT("stereo"), ApexRadio::ParseWav(MakeWav(22050, 2, 16, { 16384, 0, -16384, -16384 }), Samples, Rate));
	TestEqual(TEXT("stereo frames"), Samples.Num(), 2);
	if (Samples.Num() == 2)
	{
		TestEqual(TEXT("averaged"), Samples[0], 0.25f);
		TestEqual(TEXT("averaged low"), Samples[1], -0.5f);
	}
	TestFalse(TEXT("8-bit"), ApexRadio::ParseWav(MakeWav(16000, 1, 8, { 0, 1 }), Samples, Rate));
	TArray<uint8> Junk = { 'R', 'I', 'F', 'F', 0, 0, 0, 0, 'A', 'V', 'I', ' ' };
	TestFalse(TEXT("not a wave"), ApexRadio::ParseWav(Junk, Samples, Rate));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexRadioLinesTest, "ApexSim.Audio.Radio.Lines", ApexTestFlags)

bool FApexRadioLinesTest::RunTest(const FString& Parameters)
{
	// Every line the triggers can say is in lines.yml on the same channel,
	// and rendered: a WAV that reads, under four seconds.
	const FString Dir = ApexRepoPaths::ContentPath(TEXT("audio/radio"));
	FString Yaml;
	if (!FFileHelper::LoadFileToString(Yaml, *FPaths::Combine(Dir, TEXT("lines.yml"))))
	{
		AddError(FString::Printf(TEXT("no lines.yml in %s"), *Dir));
		return false;
	}
	TMap<FString, FString> Channels;
	const FRegexPattern Pattern(TEXT("^\\s+([a-z0-9_]+):\\s*\\{\\s*channel:\\s*([a-z]+)"));
	TArray<FString> YamlLines;
	Yaml.ParseIntoArrayLines(YamlLines);
	for (const FString& Line : YamlLines)
	{
		FRegexMatcher Match(Pattern, Line);
		if (Match.FindNext())
		{
			Channels.Add(Match.GetCaptureGroup(1), Match.GetCaptureGroup(2));
		}
	}
	TestEqual(TEXT("lines.yml names every line"), Channels.Num(), ApexRadio::AllLines().Num());

	for (const FName Line : ApexRadio::AllLines())
	{
		const FString Id = Line.ToString();
		const FString* Channel = Channels.Find(Id);
		if (!Channel)
		{
			AddError(FString::Printf(TEXT("%s is not in lines.yml"), *Id));
			continue;
		}
		const bool bSpotter = ApexRadio::SpecOf(Line).Channel == EApexRadioChannel::Spotter;
		TestEqual(FString::Printf(TEXT("%s channel"), *Id), *Channel, FString(bSpotter ? TEXT("spotter") : TEXT("engineer")));

		TArray<uint8> Bytes;
		TArray<float> Samples;
		int32 Rate = 0;
		if (!FFileHelper::LoadFileToArray(Bytes, *FPaths::Combine(Dir, Id + TEXT(".wav")))
			|| !ApexRadio::ParseWav(Bytes, Samples, Rate))
		{
			AddError(FString::Printf(TEXT("%s.wav does not read"), *Id));
			continue;
		}
		const double Seconds = double(Samples.Num()) / Rate;
		TestTrue(FString::Printf(TEXT("%s length %.2f s"), *Id, Seconds), Seconds > 0.3 && Seconds < 4.0);
	}
	return true;
}

#endif
