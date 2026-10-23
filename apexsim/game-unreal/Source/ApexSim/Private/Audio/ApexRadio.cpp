#include "Audio/ApexRadio.h"

#include "Hud/ApexHudData.h"

namespace
{
	const FName GroupSide(TEXT("side"));
	const FName GroupYellow(TEXT("yellow"));
	const FName GroupPhase(TEXT("phase"));
	const FName GroupFuel(TEXT("fuel"));
	const FName GroupPosition(TEXT("position"));

	FName PositionLine(int32 Place)
	{
		return FName(*FString::Printf(TEXT("position_%d"), Place));
	}

	FName FuelLapsLine(int32 Laps)
	{
		return FName(*FString::Printf(TEXT("fuel_laps_%d"), Laps));
	}

	bool IsSafetyPhase(const FString& Phase)
	{
		return Phase == TEXT("safety_car") || Phase == TEXT("sc_in_this_lap") || Phase == TEXT("vsc") || Phase == TEXT("vsc_ending");
	}

	/** The modes with cars at speed on track: not the grid, the countdown, the lobby or a replay. */
	bool IsOnTrackMode(const FString& Mode)
	{
		return !(Mode.IsEmpty() || Mode == TEXT("grid") || Mode == TEXT("countdown") || Mode == TEXT("lobby")
			|| Mode == TEXT("replay") || Mode == TEXT("demo_lap") || Mode == TEXT("unknown"));
	}

	uint32 ReadU32(const TArray<uint8>& B, int32 At)
	{
		return uint32(B[At]) | (uint32(B[At + 1]) << 8) | (uint32(B[At + 2]) << 16) | (uint32(B[At + 3]) << 24);
	}

	uint16 ReadU16(const TArray<uint8>& B, int32 At)
	{
		return uint16(B[At]) | uint16(B[At + 1] << 8);
	}
}

const TArray<FName>& ApexRadio::AllLines()
{
	static const TArray<FName> Lines = []
	{
		TArray<FName> Out;
		for (const TCHAR* Id : {
				 TEXT("car_left"), TEXT("car_right"), TEXT("three_wide"), TEXT("clear_left"), TEXT("clear_right"), TEXT("clear"),
				 TEXT("yellow_ahead"), TEXT("double_yellow"), TEXT("yellow_clear"), TEXT("blue_flag"),
				 TEXT("safety_car"), TEXT("vsc"), TEXT("sc_in"), TEXT("green_green"),
				 TEXT("box_this_lap"), TEXT("box_confirmed"),
				 TEXT("fuel_laps_1"), TEXT("fuel_laps_2"), TEXT("fuel_laps_3"), TEXT("fuel_laps_4"), TEXT("fuel_laps_5"),
				 TEXT("fuel_critical"), TEXT("tyres_worn"), TEXT("last_lap"), TEXT("chequered") })
		{
			Out.Add(FName(Id));
		}
		for (int32 Place = 1; Place <= MaxPositionLine; ++Place)
		{
			Out.Add(PositionLine(Place));
		}
		return Out;
	}();
	return Lines;
}

ApexRadio::FLineSpec ApexRadio::SpecOf(FName Line)
{
	auto Make = [](EApexRadioChannel Channel, int32 Priority, double MaxWaitS, double CooldownS, FName Group)
	{
		FLineSpec Spec;
		Spec.Channel = Channel;
		Spec.Priority = Priority;
		Spec.MaxWaitS = MaxWaitS;
		Spec.CooldownS = CooldownS;
		Spec.Group = Group;
		return Spec;
	};
	using enum EApexRadioChannel;
	static const TMap<FName, FLineSpec> Specs = [&Make]
	{
		TMap<FName, FLineSpec> Out;
		// The spotter's calls are only true for a second: the newest replaces
		// any still waiting, and they cut an engineer short.
		for (const TCHAR* Id : { TEXT("car_left"), TEXT("car_right"), TEXT("three_wide"), TEXT("clear_left"), TEXT("clear_right"), TEXT("clear") })
		{
			Out.Add(FName(Id), Make(Spotter, SpotterInterruptPriority, 1.0, 0.0, GroupSide));
		}
		Out.Add(TEXT("double_yellow"), Make(Spotter, 90, 4.0, 6.0, GroupYellow));
		Out.Add(TEXT("yellow_ahead"), Make(Spotter, 90, 4.0, 6.0, GroupYellow));
		Out.Add(TEXT("yellow_clear"), Make(Spotter, 40, 4.0, 6.0, GroupYellow));
		Out.Add(TEXT("blue_flag"), Make(Spotter, 80, 4.0, 20.0, FName(TEXT("blue_flag"))));
		Out.Add(TEXT("safety_car"), Make(Engineer, 70, 8.0, 5.0, GroupPhase));
		Out.Add(TEXT("vsc"), Make(Engineer, 70, 8.0, 5.0, GroupPhase));
		Out.Add(TEXT("sc_in"), Make(Engineer, 70, 8.0, 5.0, GroupPhase));
		Out.Add(TEXT("green_green"), Make(Engineer, 75, 4.0, 5.0, GroupPhase));
		Out.Add(TEXT("box_confirmed"), Make(Engineer, 60, 10.0, 10.0, FName(TEXT("box_confirmed"))));
		Out.Add(TEXT("box_this_lap"), Make(Engineer, 45, 15.0, 0.0, FName(TEXT("box_this_lap"))));
		Out.Add(TEXT("fuel_critical"), Make(Engineer, 55, 15.0, 60.0, GroupFuel));
		for (int32 Laps = 1; Laps <= 5; ++Laps)
		{
			Out.Add(FuelLapsLine(Laps), Make(Engineer, 30, 20.0, 0.0, GroupFuel));
		}
		Out.Add(TEXT("tyres_worn"), Make(Engineer, 30, 20.0, 0.0, FName(TEXT("tyres_worn"))));
		Out.Add(TEXT("last_lap"), Make(Engineer, 50, 10.0, 0.0, FName(TEXT("last_lap"))));
		Out.Add(TEXT("chequered"), Make(Engineer, 50, 20.0, 0.0, FName(TEXT("chequered"))));
		for (int32 Place = 1; Place <= MaxPositionLine; ++Place)
		{
			Out.Add(PositionLine(Place), Make(Engineer, 50, 20.0, 0.0, GroupPosition));
		}
		return Out;
	}();
	if (const FLineSpec* Found = Specs.Find(Line))
	{
		return *Found;
	}
	return Make(Engineer, 0, 10.0, 0.0, Line);
}

FApexRadioInputs ApexRadio::InputsFromHud(const FApexHudData& Data, bool bDriving)
{
	auto Value = [&Data](const TCHAR* Name) -> const FApexHudValue*
	{
		const FApexHudValue* Found = Data.Find(FName(Name));
		return Found && !Found->IsNone() ? Found : nullptr;
	};
	auto Number = [&Value](const TCHAR* Name, double Default)
	{
		const FApexHudValue* Found = Value(Name);
		return Found ? Found->AsNumber() : Default;
	};
	auto Bool = [&Value](const TCHAR* Name)
	{
		const FApexHudValue* Found = Value(Name);
		return Found && Found->AsBool();
	};
	auto Text = [&Value](const TCHAR* Name)
	{
		const FApexHudValue* Found = Value(Name);
		return Found ? Found->AsString() : FString();
	};

	FApexRadioInputs In;
	// No car, no radio: a frame the HUD could not fill has no speed.
	In.bDriving = bDriving && Value(TEXT("car.speed_kph")) != nullptr;
	In.Mode = Text(TEXT("session.mode"));
	In.SpeedKph = static_cast<float>(Number(TEXT("car.speed_kph"), 0.0));
	In.bInPitLane = Bool(TEXT("pit.in_lane"));
	In.bInGarage = Bool(TEXT("car.in_garage"));
	In.bLeft = Bool(TEXT("radar.left"));
	In.bRight = Bool(TEXT("radar.right"));
	In.Yellow = FMath::RoundToInt(Number(TEXT("flag.yellow"), 0.0));
	In.bBlue = Bool(TEXT("flag.blue"));
	In.Phase = Text(TEXT("race.phase"));
	In.bPitRequested = Bool(TEXT("pit.requested"));
	In.StopsOwed = FMath::RoundToInt(Number(TEXT("pit.stops_owed"), -1.0));
	In.LapsLeft = FMath::RoundToInt(Number(TEXT("lap.laps_left"), -1.0));
	In.FuelState = Text(TEXT("fuel.state"));
	In.FuelLaps = static_cast<float>(Number(TEXT("fuel.laps"), -1.0));
	In.TyreWearMaxPct = static_cast<float>(Number(TEXT("tyre.wear_max_pct"), -1.0));
	In.Lap = FMath::RoundToInt(Number(TEXT("lap.current"), 0.0));
	In.bFinalLap = Bool(TEXT("lap.final"));
	In.FinishPosition = FMath::RoundToInt(Number(TEXT("car.finish_position"), 0.0));
	return In;
}

bool ApexRadio::ParseWav(const TArray<uint8>& Bytes, TArray<float>& OutSamples, int32& OutSampleRate)
{
	OutSamples.Reset();
	OutSampleRate = 0;
	if (Bytes.Num() < 12 || FMemory::Memcmp(Bytes.GetData(), "RIFF", 4) != 0 || FMemory::Memcmp(Bytes.GetData() + 8, "WAVE", 4) != 0)
	{
		return false;
	}
	int32 Channels = 0;
	int32 Bits = 0;
	int32 At = 12;
	while (At + 8 <= Bytes.Num())
	{
		const uint8* Id = Bytes.GetData() + At;
		const int64 Size = ReadU32(Bytes, At + 4);
		const int32 Body = At + 8;
		if (Body + Size > Bytes.Num())
		{
			return false;
		}
		if (FMemory::Memcmp(Id, "fmt ", 4) == 0 && Size >= 16)
		{
			const int32 Format = ReadU16(Bytes, Body);
			Channels = ReadU16(Bytes, Body + 2);
			OutSampleRate = static_cast<int32>(ReadU32(Bytes, Body + 4));
			Bits = ReadU16(Bytes, Body + 14);
			if (Format != 1 || Bits != 16 || Channels < 1 || OutSampleRate <= 0)
			{
				return false;
			}
		}
		else if (FMemory::Memcmp(Id, "data", 4) == 0)
		{
			if (Channels < 1)
			{
				return false;
			}
			const int32 Frames = static_cast<int32>(Size / (2 * Channels));
			OutSamples.SetNumUninitialized(Frames);
			for (int32 Frame = 0; Frame < Frames; ++Frame)
			{
				float Sum = 0.0f;
				for (int32 Channel = 0; Channel < Channels; ++Channel)
				{
					const int16 Sample = static_cast<int16>(ReadU16(Bytes, Body + 2 * (Frame * Channels + Channel)));
					Sum += Sample / 32768.0f;
				}
				OutSamples[Frame] = Sum / Channels;
			}
			return Frames > 0;
		}
		// Chunks are padded to an even length.
		At = Body + static_cast<int32>(Size + (Size & 1));
	}
	return false;
}

// ---------------------------------------------------------------------------
// FApexRadioTriggers
// ---------------------------------------------------------------------------

void FApexRadioTriggers::Reset()
{
	*this = FApexRadioTriggers();
}

bool FApexRadioTriggers::StepSide(FSide& Side, bool bRaw, double Now)
{
	if (bRaw == Side.bOn)
	{
		Side.ChangedAt = -1.0;
		return false;
	}
	if (Side.ChangedAt < 0.0)
	{
		Side.ChangedAt = Now;
	}
	if (Now - Side.ChangedAt + KINDA_SMALL_NUMBER >= (bRaw ? ApexRadio::SideOnS : ApexRadio::SideOffS))
	{
		Side.bOn = bRaw;
		Side.ChangedAt = -1.0;
		return true;
	}
	return false;
}

void FApexRadioTriggers::Step(const FApexRadioInputs& In, double Now, TArray<FName>& Out)
{
	using namespace ApexRadio;
	if (!In.bDriving)
	{
		Reset();
		return;
	}

	const bool bOnTrack = IsOnTrackMode(In.Mode);
	const bool bRace = In.Mode == TEXT("race");
	const bool bSpotting = bOnTrack && In.SpeedKph >= SpotterMinKph && !In.bInPitLane && !In.bInGarage;
	const bool bRawLeft = bSpotting && In.bLeft;
	const bool bRawRight = bSpotting && In.bRight;
	const int32 YellowNow = bOnTrack ? In.Yellow : 0;
	const bool bBlueNow = bOnTrack && In.bBlue;

	if (!bPrimed)
	{
		// Take stock: what is already the case when the radio comes on is not news.
		bPrimed = true;
		Left.bOn = bRawLeft;
		Right.bOn = bRawRight;
		Yellow = YellowNow;
		bBlue = bBlueNow;
		Phase = In.Phase;
		bPitRequested = In.bPitRequested;
		FuelState = In.FuelState;
		bTyresCalled = In.TyreWearMaxPct >= TyreWornPct;
		Lap = In.Lap;
		bFinalLap = In.bFinalLap;
		FinishPosition = In.FinishPosition;
		return;
	}

	// --- The spotter: cars alongside -----------------------------------------
	if (!bSpotting)
	{
		// Off the racing (slow, in the lane): nothing is alongside, and
		// nothing is called clear either.
		Left = FSide();
		Right = FSide();
	}
	else
	{
		const bool bLeftFlip = StepSide(Left, bRawLeft, Now);
		const bool bRightFlip = StepSide(Right, bRawRight, Now);
		if (bLeftFlip || bRightFlip)
		{
			if (Left.bOn && Right.bOn)
			{
				Out.Add(TEXT("three_wide"));
			}
			else if (bLeftFlip && bRightFlip)
			{
				Out.Add(Left.bOn ? TEXT("car_left") : Right.bOn ? TEXT("car_right") : TEXT("clear"));
			}
			else if (bLeftFlip)
			{
				Out.Add(Left.bOn ? TEXT("car_left") : TEXT("clear_left"));
			}
			else
			{
				Out.Add(Right.bOn ? TEXT("car_right") : TEXT("clear_right"));
			}
		}
	}

	// --- The spotter: race control's flags -----------------------------------
	if (YellowNow > Yellow)
	{
		Out.Add(YellowNow >= 2 ? TEXT("double_yellow") : TEXT("yellow_ahead"));
	}
	if (YellowNow > 0)
	{
		YellowGoneAt = -1.0;
	}
	else if (Yellow > 0)
	{
		YellowGoneAt = Now;
	}
	if (YellowGoneAt >= 0.0 && Now - YellowGoneAt >= YellowClearS)
	{
		Out.Add(TEXT("yellow_clear"));
		YellowGoneAt = -1.0;
	}
	Yellow = YellowNow;
	if (bBlueNow && !bBlue)
	{
		Out.Add(TEXT("blue_flag"));
	}
	bBlue = bBlueNow;

	// --- The engineer: the race's phases -------------------------------------
	if (In.Phase != Phase)
	{
		if (bRace)
		{
			if (In.Phase == TEXT("safety_car"))
			{
				Out.Add(TEXT("safety_car"));
			}
			else if (In.Phase == TEXT("vsc"))
			{
				Out.Add(TEXT("vsc"));
			}
			else if (In.Phase == TEXT("sc_in_this_lap"))
			{
				Out.Add(TEXT("sc_in"));
			}
			else if (In.Phase.IsEmpty() && IsSafetyPhase(Phase))
			{
				Out.Add(TEXT("green_green"));
			}
		}
		Phase = In.Phase;
	}

	// --- The engineer: the stop, fuel and tyres ------------------------------
	if (In.bPitRequested && !bPitRequested)
	{
		Out.Add(TEXT("box_confirmed"));
	}
	bPitRequested = In.bPitRequested;

	if (In.FuelState == TEXT("critical") && FuelState != TEXT("critical"))
	{
		Out.Add(TEXT("fuel_critical"));
	}
	FuelState = In.FuelState;

	if (In.TyreWearMaxPct >= TyreWornPct && !bTyresCalled)
	{
		Out.Add(TEXT("tyres_worn"));
		bTyresCalled = true;
	}
	else if (In.TyreWearMaxPct >= 0.0f && In.TyreWearMaxPct < TyreFreshPct)
	{
		bTyresCalled = false;
	}

	// At the line: what the lap just done says about the ones to come.
	if (In.Lap > Lap && Lap >= 1)
	{
		const int32 FuelLapsWhole = In.FuelLaps >= 0.0f ? FMath::FloorToInt(In.FuelLaps) : -1;
		const bool bShort = In.FuelState == TEXT("short") || (In.LapsLeft < 0 && In.FuelState == TEXT("ok") && FuelLapsWhole <= 3);
		if (bShort && FuelLapsWhole >= 1 && FuelLapsWhole <= 5)
		{
			Out.Add(FuelLapsLine(FuelLapsWhole));
		}
		// A stop still owed with this lap the last chance to make it.
		if (bRace && In.StopsOwed > 0 && !In.bPitRequested && In.LapsLeft >= 2 && In.LapsLeft <= 3)
		{
			Out.Add(TEXT("box_this_lap"));
		}
	}
	Lap = In.Lap;

	// --- The engineer: the end -------------------------------------------------
	if (bRace && In.bFinalLap && !bFinalLap && In.FinishPosition == 0)
	{
		Out.Add(TEXT("last_lap"));
	}
	bFinalLap = In.bFinalLap;
	if (bRace && In.FinishPosition > 0 && FinishPosition == 0)
	{
		Out.Add(TEXT("chequered"));
		if (In.FinishPosition <= MaxPositionLine)
		{
			Out.Add(PositionLine(In.FinishPosition));
		}
	}
	FinishPosition = In.FinishPosition;
}

// ---------------------------------------------------------------------------
// FApexRadioQueue
// ---------------------------------------------------------------------------

void FApexRadioQueue::Reset()
{
	*this = FApexRadioQueue();
}

void FApexRadioQueue::Push(FName Line, double Now)
{
	const ApexRadio::FLineSpec Spec = ApexRadio::SpecOf(Line);
	if (const double* Said = LastSaid.Find(Line); Said && Now - *Said < Spec.CooldownS)
	{
		return;
	}
	Entries.RemoveAll([&Spec](const FEntry& Entry) { return ApexRadio::SpecOf(Entry.Line).Group == Spec.Group; });
	Entries.Add({ Line, Now, NextSeq++ });
}

FName FApexRadioQueue::Pop(double Now, bool bHoldEngineer, bool& bOutInterrupt)
{
	bOutInterrupt = false;
	Entries.RemoveAll([Now](const FEntry& Entry) { return Now - Entry.QueuedAt > ApexRadio::SpecOf(Entry.Line).MaxWaitS; });

	const bool bBusy = IsBusy(Now);
	const bool bPlayingEngineer = bBusy && ApexRadio::SpecOf(Playing).Channel == EApexRadioChannel::Engineer;
	int32 Best = INDEX_NONE;
	ApexRadio::FLineSpec BestSpec;
	for (int32 Index = 0; Index < Entries.Num(); ++Index)
	{
		const ApexRadio::FLineSpec Spec = ApexRadio::SpecOf(Entries[Index].Line);
		if (bHoldEngineer && Spec.Channel == EApexRadioChannel::Engineer)
		{
			continue;
		}
		if (bBusy && !(bPlayingEngineer && Spec.Priority >= ApexRadio::SpotterInterruptPriority))
		{
			continue;
		}
		if (Best == INDEX_NONE || Spec.Priority > BestSpec.Priority
			|| (Spec.Priority == BestSpec.Priority && Entries[Index].Seq < Entries[Best].Seq))
		{
			Best = Index;
			BestSpec = Spec;
		}
	}
	if (Best == INDEX_NONE)
	{
		return NAME_None;
	}
	const FName Line = Entries[Best].Line;
	Entries.RemoveAt(Best);
	LastSaid.Add(Line, Now);
	bOutInterrupt = bBusy;
	return Line;
}

void FApexRadioQueue::Started(FName Line, double Now, double DurationS)
{
	Playing = Line;
	BusyUntil = Now + FMath::Max(0.0, DurationS) + ApexRadio::GapS;
}
