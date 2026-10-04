#include "Hud/ApexHudData.h"

#include "ApexErs.h"
#include "Race/ApexRaceCoordinate.h"

namespace
{
	const TCHAR* const HudTyreKeys[4] = {TEXT("fl"), TEXT("fr"), TEXT("rl"), TEXT("rr")};
	const TCHAR* const HudTyreNames[4] = {TEXT("FL"), TEXT("FR"), TEXT("RL"), TEXT("RR")};
	/** The order of FApexCarTelemetry::DamagePct. */
	const TCHAR* const HudDamageKeys[5] = {TEXT("front"), TEXT("rear"), TEXT("left"), TEXT("right"), TEXT("engine")};
	const TCHAR* const HudDamageNames[5] = {TEXT("Front"), TEXT("Rear"), TEXT("Left"), TEXT("Right"), TEXT("Engine")};

	/** How long a zone that has just been hurt flashes, seconds. */
	constexpr double HudDamageFlashSeconds = 0.8;

	/** The running order and its gaps are taken at most this often, seconds. */
	constexpr double HudStandingsRefreshSeconds = 0.5;

	/**
	 * A tread against the car's working window: cold under it, ok in it, hot
	 * over it and over once 15 °C past the edge, where the grip goes fast.
	 */
	const TCHAR* HudTyreState(float TempC, float OptimalC, float WindowC)
	{
		if (TempC < 0.0f)
		{
			return TEXT("unknown");
		}
		if (TempC < OptimalC - WindowC)
		{
			return TEXT("cold");
		}
		if (TempC <= OptimalC + WindowC)
		{
			return TEXT("ok");
		}
		return TempC <= OptimalC + WindowC + 15.0f ? TEXT("hot") : TEXT("over");
	}

	/**
	 * A brake's working range is its material's, which the client does not
	 * know, so it is judged from how hot it runs: carbon works from 300 °C,
	 * steel below that, and both are fading by 850 °C.
	 */
	const TCHAR* HudBrakeState(float TempC)
	{
		return TempC < 0.0f ? TEXT("unknown")
			: TempC >= 1000.0f ? TEXT("over")
			: TempC >= 850.0f ? TEXT("hot")
			: TempC < 150.0f ? TEXT("cold")
			: TEXT("ok");
	}

	/** The engine protects itself past 112 °C (server engine_heat.rs). */
	const TCHAR* HudWaterState(float TempC)
	{
		return TempC < 0.0f ? TEXT("unknown") : TempC > 112.0f ? TEXT("over") : TempC > 105.0f ? TEXT("hot") : TEXT("ok");
	}

	const TCHAR* HudModeKey(EApexGameMode Mode)
	{
		switch (Mode)
		{
		case EApexGameMode::Lobby: return TEXT("lobby");
		case EApexGameMode::Sandbox: return TEXT("sandbox");
		case EApexGameMode::Countdown: return TEXT("countdown");
		case EApexGameMode::DemoLap: return TEXT("demo_lap");
		case EApexGameMode::FreePractice: return TEXT("practice");
		case EApexGameMode::Replay: return TEXT("replay");
		case EApexGameMode::Qualification: return TEXT("qualifying");
		case EApexGameMode::Race: return TEXT("race");
		case EApexGameMode::Hotlap: return TEXT("hotlap");
		default: return TEXT("unknown");
		}
	}

	/** One car's place in the order, before it becomes a record. */
	struct FHudStanding
	{
		const FApexCarTelemetry* Car = nullptr;
		FString Name;
		/** Race distance in metres; negative on the grid behind the line. */
		float Progress = 0.0f;
		/** Speed the gaps are read at, m/s. */
		float Speed = 0.0f;
		bool bIsLocal = false;
	};

	/** Milliseconds as seconds, null when the server has no time. */
	FApexHudValue HudSeconds(int32 Ms)
	{
		return Ms > 0 ? FApexHudValue::Of(Ms / 1000.0) : FApexHudValue();
	}

	FApexHudValue HudKnown(float Value, bool bKnown)
	{
		return bKnown ? FApexHudValue::Of(Value) : FApexHudValue();
	}

	/** Out of the race: a damage zone has reached 100% (server damage.rs). */
	bool HudRetired(const FApexCarTelemetry& Car)
	{
		if (!Car.HasDamage())
		{
			return false;
		}
		for (const float Pct : Car.DamagePct)
		{
			if (Pct >= 100.0f)
			{
				return true;
			}
		}
		return false;
	}

	/** The most worn of the four, percent; null when the wear is not sent. */
	FApexHudValue HudWorstWear(const FApexCarTelemetry& Car)
	{
		float Worst = -1.0f;
		for (const float Wear : Car.TyreWearPct)
		{
			Worst = FMath::Max(Worst, Wear);
		}
		return Worst >= 0.0f ? FApexHudValue::Of(Worst) : FApexHudValue();
	}

	/**
	 * Laps to the flag for a car, the one it is on included; -1 when there
	 * is no telling (no distance, or a timed race before the car has a lap
	 * time). A race over laps counts down its distance. A timed race: one
	 * once anyone has the flag (every car takes it at its next crossing),
	 * to the final lap once the clock has run out, and before that as many
	 * of the car's last laps as the clock still holds, rounded up.
	 */
	int32 HudLapsToFlag(const FApexHudInputs& In, const FApexTelemetryFrame& Frame, const FApexCarTelemetry& Car)
	{
		if (Car.FinishPosition > 0)
		{
			return 0;
		}
		if (Frame.HasRaceClock())
		{
			const bool bFlagOut = Frame.Cars.ContainsByPredicate(
				[](const FApexCarTelemetry& Other) { return Other.FinishPosition > 0; });
			if (bFlagOut)
			{
				return 1;
			}
			if (Frame.RaceFinalLap > 0)
			{
				return FMath::Max(1, Frame.RaceFinalLap - FMath::Max(1, Car.CurrentLap) + 1);
			}
			return Car.LastLapTimeMs > 0
				? FMath::Max(1, FMath::CeilToInt(static_cast<double>(Frame.RaceLeftMs) / Car.LastLapTimeMs))
				: -1;
		}
		return In.LapLimit > 0 ? FMath::Max(0, In.LapLimit - FMath::Max(0, Car.CurrentLap - 1)) : -1;
	}
}

void FApexHudMemory::SampleLap(const FApexCarTelemetry& Local, float TrackLengthM)
{
	const float LapSeconds = Local.CurrentLapTimeMs / 1000.0f;

	// Samples are keyed by fraction of the lap so a reference lap can be looked
	// up by position; the wire's TrackProgress is a station in metres.
	const float Progress = TrackLengthM > 0.0f ? FMath::Clamp(Local.TrackProgress / TrackLengthM, 0.0f, 1.0f) : 0.0f;

	if (Local.CurrentLap != LastSeenLap)
	{
		// The lap counter moved on, and the lap it left behind is on the wire
		// as `LastLapTimeMs`. A lap that left the track is no reference:
		// chasing a delta against a lap that cut a chicane would ask the
		// driver to cut it too.
		const float Completed = Local.LastLapTimeMs / 1000.0f;
		if (LastSeenLap > 0 && LapSamples.Num() > 1 && Completed > 0.0f && !Local.bLastLapInvalid)
		{
			if (ReferenceLapSeconds <= 0.0f || Completed < ReferenceLapSeconds)
			{
				ReferenceLapSeconds = Completed;
				ReferenceLap = LapSamples;
			}
		}
		LastSeenLap = Local.CurrentLap;
		LapSamples.Reset();
	}

	// Samples must stay monotonic in progress for the lookup to work; a frame
	// that arrives out of order (or the wrap at the line) is dropped.
	if (LapSamples.Num() == 0 || Progress > LapSamples.Last().Key)
	{
		LapSamples.Emplace(Progress, LapSeconds);
	}
}

void FApexHudMemory::SampleField(const FApexTelemetryFrame& Frame)
{
	for (const FApexCarTelemetry& Car : Frame.Cars)
	{
		FCarHistory* History = Cars.Find(Car.CarIndex);
		if (!History)
		{
			// First seen: the set on it is taken to be the one it started on.
			History = &Cars.Add(Car.CarIndex);
			History->Compound = Car.Compound;
		}
		// A stop is counted as the car comes to rest in its box; the new set
		// is on when the crew is done.
		if (Car.bPitServicing && !History->bWasServicing)
		{
			++History->PitStops;
		}
		if ((!Car.bPitServicing && History->bWasServicing) || (Car.Compound >= 0 && History->Compound >= 0 && Car.Compound != History->Compound))
		{
			History->TyresFromLap = FMath::Max(1, Car.CurrentLap);
		}
		History->bWasServicing = Car.bPitServicing;
		if (Car.Compound >= 0)
		{
			History->Compound = Car.Compound;
		}
	}
}

float FApexHudMemory::ReferenceTimeAt(float Fraction) const
{
	if (ReferenceLap.Num() < 2)
	{
		return -1.0f;
	}
	// Linear scan is fine: a lap holds a few thousand samples at most and this
	// runs once a frame.
	if (Fraction <= ReferenceLap[0].Key)
	{
		return ReferenceLap[0].Value;
	}
	for (int32 Index = 1; Index < ReferenceLap.Num(); ++Index)
	{
		if (Fraction <= ReferenceLap[Index].Key)
		{
			const TPair<float, float>& Before = ReferenceLap[Index - 1];
			const TPair<float, float>& After = ReferenceLap[Index];
			const float Span = After.Key - Before.Key;
			const float Alpha = Span > KINDA_SMALL_NUMBER ? (Fraction - Before.Key) / Span : 0.0f;
			return FMath::Lerp(Before.Value, After.Value, Alpha);
		}
	}
	return ReferenceLap.Last().Value;
}

const TMap<FName, TArray<FName>>& ApexHudData::ListFields()
{
	static const TMap<FName, TArray<FName>> Fields = {
		{TEXT("standings"), {TEXT("position"), TEXT("car_index"), TEXT("name"), TEXT("is_local"), TEXT("is_player"),
			TEXT("finished"), TEXT("finish_position"), TEXT("lap"), TEXT("gap_leader_s"), TEXT("gap_s"), TEXT("interval_s"),
			TEXT("laps_down"), TEXT("best_lap_s"), TEXT("last_lap_s"), TEXT("last_lap_invalid"), TEXT("is_session_best"),
			TEXT("speed_kph"), TEXT("in_pit"), TEXT("on_track"), TEXT("servicing"), TEXT("retired"), TEXT("car_name"),
			TEXT("compound"), TEXT("tyre_age_laps"), TEXT("tyre_wear_pct"), TEXT("pit_stops")}},
		{TEXT("sectors"), {TEXT("number"), TEXT("time_s"), TEXT("best_s"), TEXT("session_best_s"), TEXT("state"),
			TEXT("is_current")}},
		{TEXT("tyres"), {TEXT("key"), TEXT("name"), TEXT("temp_c"), TEXT("pressure_kpa"), TEXT("wear_pct"),
			TEXT("brake_c"), TEXT("state"), TEXT("brake_state")}},
		{TEXT("damage"), {TEXT("key"), TEXT("name"), TEXT("pct"), TEXT("flash")}},
	};
	return Fields;
}

TArray<FName> ApexHudData::ScalarNames()
{
	FApexHudInputs Empty;
	FApexHudMemory Memory;
	FApexHudData Data;
	Build(Empty, Memory, Data);
	TArray<FName> Names;
	Data.Values.GetKeys(Names);
	Names.Sort(FNameLexicalLess());
	return Names;
}

FApexPitStopProgress ApexHudData::PitStopProgress(const FApexPitService& Stop, float SecondsLeft)
{
	FApexPitStopProgress P;
	P.PartSeconds[FApexPitStopProgress::Tyres] = FMath::Max(0.0f, Stop.TyresS);
	P.PartSeconds[FApexPitStopProgress::Fuel] = FMath::Max(0.0f, Stop.FuelS);
	P.PartSeconds[FApexPitStopProgress::Repair] = FMath::Max(0.0f, Stop.RepairS);
	const float Parts = P.PartSeconds[0] + P.PartSeconds[1] + P.PartSeconds[2];
	// The server's total is the three added up; the sum stands in should it
	// ever not send one.
	P.TotalS = Stop.TotalS > 0.0f ? Stop.TotalS : Parts;
	if (Parts <= 0.0f || P.TotalS <= 0.0f)
	{
		return P;
	}
	P.bValid = true;
	// Telemetry counts the whole stop down; what has gone is the total less it.
	P.ElapsedS = FMath::Clamp(P.TotalS - FMath::Max(0.0f, SecondsLeft), 0.0f, P.TotalS);
	P.Progress = P.ElapsedS / P.TotalS;

	// The parts one after the other, tyres first. The part in hand is the
	// first not yet finished; at a boundary the next one has started.
	float Start = 0.0f;
	for (int32 Part = 0; Part < FApexPitStopProgress::PartCount; ++Part)
	{
		const float Seconds = P.PartSeconds[Part];
		if (Seconds <= 0.0f)
		{
			continue;
		}
		P.PartShare[Part] = Seconds / Parts;
		P.PartFill[Part] = FMath::Clamp((P.ElapsedS - Start) / Seconds, 0.0f, 1.0f);
		if (P.Phase == INDEX_NONE && P.ElapsedS < Start + Seconds)
		{
			P.Phase = Part;
			P.PhaseLeftS = Start + Seconds - P.ElapsedS;
			P.PhaseProgress = P.PartFill[Part];
		}
		Start += Seconds;
	}
	if (P.Phase == INDEX_NONE)
	{
		// All done, the last moment before the car is let go: the last part, full.
		for (int32 Part = FApexPitStopProgress::PartCount - 1; Part >= 0; --Part)
		{
			if (P.PartSeconds[Part] > 0.0f)
			{
				P.Phase = Part;
				P.PhaseLeftS = 0.0f;
				P.PhaseProgress = 1.0f;
				break;
			}
		}
	}

	const TCHAR* const Dot = TEXT(" · ");
	switch (P.Phase)
	{
	case FApexPitStopProgress::Tyres:
	{
		P.PhaseKey = TEXT("tyres");
		const FString Compound = FApexCarTelemetry::CompoundName(Stop.Compound);
		P.PhaseLabel = Compound.IsEmpty() ? FString(TEXT("CHANGING TYRES")) : FString(TEXT("CHANGING TYRES")) + Dot + Compound;
		break;
	}
	case FApexPitStopProgress::Fuel:
		P.PhaseKey = TEXT("fuel");
		P.PhaseLabel = Stop.FuelL > 0.0f ? FString::Printf(TEXT("REFUELLING%s+%.1f L"), Dot, Stop.FuelL) : FString(TEXT("REFUELLING"));
		break;
	case FApexPitStopProgress::Repair:
		P.PhaseKey = TEXT("repair");
		P.PhaseLabel = Stop.RepairPct > 0.0f ? FString::Printf(TEXT("REPAIRING%s%.0f%%"), Dot, Stop.RepairPct) : FString(TEXT("REPAIRING"));
		break;
	default:
		break;
	}
	return P;
}

void ApexHudData::Build(const FApexHudInputs& In, FApexHudMemory& Memory, FApexHudData& Out)
{
	static const FApexTelemetryFrame NoFrame;
	static const FApexSessionRoster NoRoster;
	static const FApexTimingBoard NoTiming;
	static const FApexTrackSectors NoSectors;
	const FApexTelemetryFrame& Frame = In.Frame ? *In.Frame : NoFrame;
	const FApexSessionRoster& Roster = In.Roster ? *In.Roster : NoRoster;
	const FApexTimingBoard& Timing = In.Timing ? *In.Timing : NoTiming;
	const FApexTrackSectors& Sectors = In.Sectors ? *In.Sectors : NoSectors;

	const FApexCarTelemetry* Local = Frame.Cars.FindByPredicate(
		[&In](const FApexCarTelemetry& Car) { return Car.CarIndex == In.LocalCarIndex; });
	const FApexCarTiming* LocalTiming = Local ? Timing.Find(Local->CarIndex) : nullptr;

	// --- HUD and session ----------------------------------------------------

	Out.Set(TEXT("hud.full"), In.bFullDetail);
	Out.Set(TEXT("hud.time_s"), In.TimeSeconds);
	Out.Set(TEXT("hud.imperial"), In.bImperial);
	Out.Set(TEXT("hud.speed_unit"), In.bImperial ? TEXT("MPH") : TEXT("KM/H"));
	Out.Set(TEXT("hud.gamepad"), In.bGamepad);

	Out.Set(TEXT("spectate.active"), In.bSpectating);
	Out.Set(TEXT("spectate.live"), In.bSpectating && In.SpectateSource == TEXT("live"));
	Out.Set(TEXT("spectate.source"), In.bSpectating && !In.SpectateSource.IsEmpty() ? FApexHudValue::Of(In.SpectateSource) : FApexHudValue());
	Out.Set(TEXT("spectate.camera"), In.bSpectating && !In.SpectateCamera.IsEmpty() ? FApexHudValue::Of(In.SpectateCamera) : FApexHudValue());
	Out.Set(TEXT("spectate.auto"), In.bSpectating && In.bSpectateAuto);
	Out.Set(TEXT("spectate.tower_mode"), In.SpectateTowerMode);
	Out.Set(TEXT("spectate.waiting"), In.bSpectating && Frame.Cars.Num() == 0);

	Out.Set(TEXT("replay.active"), In.bReplay);
	Out.Set(TEXT("replay.time_s"), In.bReplay ? FApexHudValue::Of(In.ReplaySeconds) : FApexHudValue());
	Out.Set(TEXT("replay.duration_s"), In.bReplay ? FApexHudValue::Of(In.ReplayDurationSeconds) : FApexHudValue());
	Out.Set(TEXT("replay.progress"), In.bReplay && In.ReplayDurationSeconds > 0.0
		? FApexHudValue::Of(FMath::Clamp(In.ReplaySeconds / In.ReplayDurationSeconds, 0.0, 1.0))
		: FApexHudValue());
	Out.Set(TEXT("replay.rate"), In.bReplay ? FApexHudValue::Of(In.ReplayRate) : FApexHudValue());
	Out.Set(TEXT("replay.paused"), In.bReplay && In.bReplayPaused);
	Out.Set(TEXT("replay.ended"), In.bReplay && In.bReplayEnded);

	Out.Set(TEXT("session.track_name"), In.TrackName);
	Out.Set(TEXT("session.car_name"), In.CarName);
	Out.Set(TEXT("session.mode"), HudModeKey(In.GameMode));
	Out.Set(TEXT("session.mode_name"), In.ModeName);
	Out.Set(TEXT("session.is_race"), In.GameMode == EApexGameMode::Race);
	Out.Set(TEXT("session.is_hotlap"), In.GameMode == EApexGameMode::Hotlap);
	if (In.bHasConditions)
	{
		Out.Set(TEXT("session.conditions"), In.Conditions.Describe());
		Out.Set(TEXT("session.weather"), FApexSessionConditions::WeatherLabel(In.Conditions.Weather));
		Out.Set(TEXT("session.clock"), In.Conditions.ClockText());
		Out.Values.FindOrAdd(TEXT("session.air_temp_c")) = In.Conditions.HasAirTemp() ? FApexHudValue::Of(In.Conditions.AirTempC) : FApexHudValue();
		Out.Values.FindOrAdd(TEXT("session.wind_kph")) = In.Conditions.HasWind() ? FApexHudValue::Of(In.Conditions.WindKph) : FApexHudValue();
	}
	else
	{
		Out.SetNone(TEXT("session.conditions"));
		Out.SetNone(TEXT("session.weather"));
		Out.SetNone(TEXT("session.clock"));
		Out.SetNone(TEXT("session.air_temp_c"));
		Out.SetNone(TEXT("session.wind_kph"));
	}
	Out.Values.FindOrAdd(TEXT("net.ping_ms")) = In.PingMs >= 0 ? FApexHudValue::Of(In.PingMs) : FApexHudValue();

	// --- Standings ----------------------------------------------------------

	Memory.SampleField(Frame);

	// Without a catalog length the stations still order cars within a lap; the
	// nominal length only has to dwarf any real station so a completed lap
	// always outranks a partial one. Gaps stay null in that case: they need
	// the real length to mean anything.
	const float RankLength = In.TrackLengthM > 0.0f ? In.TrackLengthM : 100000.0f;
	TArray<FHudStanding> Order;
	Order.Reserve(Frame.Cars.Num());
	for (const FApexCarTelemetry& Car : Frame.Cars)
	{
		FHudStanding Entry;
		Entry.Car = &Car;
		// The wire's TrackProgress is a station in metres, not a lap fraction;
		// RaceDistanceM also folds in the grid sitting behind the line.
		Entry.Progress = ApexRace::RaceDistanceM(Car.CurrentLap, Car.TrackProgress, RankLength);
		Entry.Speed = FMath::Max(Car.SpeedMps, 5.0f);
		Entry.bIsLocal = Car.CarIndex == In.LocalCarIndex;
		if (const FApexRosterEntry* Row = Roster.Entries.FindByPredicate(
				[&Car](const FApexRosterEntry& Candidate) { return Candidate.CarIndex == Car.CarIndex; }))
		{
			Entry.Name = Row->PlayerName;
		}
		if (Entry.Name.IsEmpty())
		{
			Entry.Name = FString::Printf(TEXT("CAR %d"), Car.CarIndex);
		}
		Order.Add(MoveTemp(Entry));
	}
	// Finishers first, in classified order; then furthest round the race. Ties
	// break on car index so the order does not flicker between two cars sitting
	// on the grid.
	Order.Sort([](const FHudStanding& A, const FHudStanding& B)
	{
		if (A.Car->FinishPosition != B.Car->FinishPosition || !FMath::IsNearlyEqual(A.Progress, B.Progress))
		{
			return ApexRace::RanksAhead(A.Car->FinishPosition, A.Progress, B.Car->FinishPosition, B.Progress);
		}
		return A.Car->CarIndex < B.Car->CarIndex;
	});

	// Two cars side by side swap places on the road every few frames, and
	// every place, gap and lit row flickered with them: the order and the
	// distances the gaps are read from are taken at most twice a second and
	// held in between. A change in the field, a clock that ran back (a replay
	// seek) or no race clock at all takes them at once.
	bool bTakeOrder = In.TimeSeconds <= 0.0 || Memory.StandingsTakenAt < 0.0
		|| In.TimeSeconds < Memory.StandingsTakenAt
		|| In.TimeSeconds - Memory.StandingsTakenAt >= HudStandingsRefreshSeconds
		|| Memory.Standings.Num() != Order.Num();
	if (!bTakeOrder)
	{
		TArray<FHudStanding> Held;
		Held.Reserve(Order.Num());
		for (const FApexHudMemory::FStanding& Was : Memory.Standings)
		{
			const FHudStanding* Now = Order.FindByPredicate(
				[&Was](const FHudStanding& Entry) { return Entry.Car->CarIndex == Was.CarIndex; });
			if (!Now)
			{
				bTakeOrder = true;
				break;
			}
			FHudStanding& Entry = Held.Add_GetRef(*Now);
			Entry.Progress = Was.Progress;
			Entry.Speed = Was.Speed;
		}
		if (!bTakeOrder)
		{
			Order = MoveTemp(Held);
		}
	}
	if (bTakeOrder)
	{
		Memory.Standings.Reset(Order.Num());
		for (const FHudStanding& Entry : Order)
		{
			Memory.Standings.Add({Entry.Car->CarIndex, Entry.Progress, Entry.Speed});
		}
		Memory.StandingsTakenAt = In.TimeSeconds;
	}

	const int32 LocalPlace = Order.IndexOfByPredicate([](const FHudStanding& Entry) { return Entry.bIsLocal; });
	const bool bLengthKnown = In.TrackLengthM > 0.0f;

	TArray<FApexHudRecord>& Standings = Out.Lists.FindOrAdd(TEXT("standings"));
	Standings.Reset(Order.Num());
	for (int32 Place = 0; Place < Order.Num(); ++Place)
	{
		const FHudStanding& Entry = Order[Place];
		const FApexCarTelemetry& Car = *Entry.Car;
		const FApexCarTiming* CarTiming = Timing.Find(Car.CarIndex);
		FApexHudRecord& Row = Standings.AddDefaulted_GetRef();
		Row.Add(TEXT("position"), FApexHudValue::Of(Place + 1));
		Row.Add(TEXT("car_index"), FApexHudValue::Of(Car.CarIndex));
		Row.Add(TEXT("name"), FApexHudValue::Of(Entry.Name));
		Row.Add(TEXT("is_local"), FApexHudValue::Of(Entry.bIsLocal));
		Row.Add(TEXT("is_player"), FApexHudValue::Of(Entry.bIsLocal && !In.bSpectating));
		Row.Add(TEXT("finished"), FApexHudValue::Of(Car.FinishPosition > 0));
		Row.Add(TEXT("finish_position"), FApexHudValue::Of(Car.FinishPosition));
		Row.Add(TEXT("lap"), FApexHudValue::Of(ApexRace::DisplayLap(Car.CurrentLap, In.LapLimit)));
		// Gaps are a time, not a distance: how long the car behind would take,
		// at its current speed, to cover the ground between them.
		Row.Add(TEXT("gap_leader_s"), bLengthKnown && Place > 0
			? FApexHudValue::Of((Order[0].Progress - Entry.Progress) / Entry.Speed)
			: FApexHudValue());
		Row.Add(TEXT("gap_s"), bLengthKnown && LocalPlace >= 0
			? FApexHudValue::Of((Order[LocalPlace].Progress - Entry.Progress) / Order[LocalPlace].Speed)
			: FApexHudValue());
		// The car directly ahead on the road of the order, the way a timing
		// tower reads: the leader has none.
		Row.Add(TEXT("interval_s"), bLengthKnown && Place > 0
			? FApexHudValue::Of((Order[Place - 1].Progress - Entry.Progress) / Entry.Speed)
			: FApexHudValue());
		Row.Add(TEXT("laps_down"), FApexHudValue::Of(bLengthKnown && Place > 0 && Car.FinishPosition <= 0
			? FMath::Max(0, FMath::FloorToInt((Order[0].Progress - Entry.Progress) / In.TrackLengthM))
			: 0));
		const int32 BestMs = CarTiming && CarTiming->BestLapMs > 0 ? CarTiming->BestLapMs : Car.BestLapTimeMs;
		Row.Add(TEXT("best_lap_s"), HudSeconds(BestMs));
		Row.Add(TEXT("last_lap_s"), HudSeconds(Car.LastLapTimeMs));
		Row.Add(TEXT("last_lap_invalid"), FApexHudValue::Of(Car.LastLapTimeMs > 0 && Car.bLastLapInvalid));
		Row.Add(TEXT("is_session_best"), FApexHudValue::Of(Timing.SessionBestLapMs > 0 && Car.CarIndex == Timing.SessionBestLapCarIndex));
		Row.Add(TEXT("speed_kph"), FApexHudValue::Of(ApexRace::MpsToKph(Car.SpeedMps)));
		Row.Add(TEXT("in_pit"), FApexHudValue::Of(Car.bInPitLane));
		Row.Add(TEXT("on_track"), FApexHudValue::Of(Car.bIsOnTrack));
		Row.Add(TEXT("servicing"), FApexHudValue::Of(Car.bPitServicing));
		Row.Add(TEXT("retired"), FApexHudValue::Of(HudRetired(Car)));
		const FString* Model = In.CarNames ? In.CarNames->Find(Car.CarIndex) : nullptr;
		Row.Add(TEXT("car_name"), Model && !Model->IsEmpty() ? FApexHudValue::Of(*Model) : FApexHudValue());
		const FString Letter = FApexCarTelemetry::CompoundLetter(Car.Compound);
		Row.Add(TEXT("compound"), Letter.IsEmpty() ? FApexHudValue() : FApexHudValue::Of(Letter));
		const FApexHudMemory::FCarHistory* History = Memory.Cars.Find(Car.CarIndex);
		Row.Add(TEXT("tyre_age_laps"), History && Letter.Len() > 0
			? FApexHudValue::Of(FMath::Max(0, Car.CurrentLap - History->TyresFromLap))
			: FApexHudValue());
		Row.Add(TEXT("tyre_wear_pct"), HudWorstWear(Car));
		Row.Add(TEXT("pit_stops"), FApexHudValue::Of(History ? History->PitStops : 0));
	}

	Out.Values.FindOrAdd(TEXT("race.position")) = LocalPlace >= 0 ? FApexHudValue::Of(LocalPlace + 1) : FApexHudValue();
	Out.Set(TEXT("race.car_count"), Order.Num());
	// The race's lap is the leader's, whoever the HUD is about.
	Out.Set(TEXT("race.leader_lap"), Order.Num() > 0
		? FApexHudValue::Of(ApexRace::DisplayLap(Order[0].Car->CurrentLap, In.LapLimit))
		: FApexHudValue());

	auto SetGap = [&](const TCHAR* Prefix, int32 OtherPlace)
	{
		const bool bThere = LocalPlace >= 0 && Order.IsValidIndex(OtherPlace);
		Out.Values.FindOrAdd(FName(FString(Prefix) + TEXT("_name"))) = bThere ? FApexHudValue::Of(Order[OtherPlace].Name) : FApexHudValue();
		FApexHudValue Gap;
		if (bThere && bLengthKnown)
		{
			Gap = FApexHudValue::Of(FMath::Abs(Order[OtherPlace].Progress - Order[LocalPlace].Progress) / Order[LocalPlace].Speed);
		}
		Out.Values.FindOrAdd(FName(FString(Prefix) + TEXT("_s"))) = Gap;
	};
	SetGap(TEXT("gap.ahead"), LocalPlace - 1);
	SetGap(TEXT("gap.behind"), LocalPlace + 1);

	// --- Laps and timing ----------------------------------------------------

	Out.Set(TEXT("lap.limit"), In.LapLimit);
	// A timed race's clock is the server's: the time left, then the lap the
	// leader takes the flag on.
	const bool bTimed = Frame.HasRaceClock();
	Out.Set(TEXT("race.timed"), bTimed);
	Out.Set(TEXT("race.time_left_s"), bTimed ? FApexHudValue::Of(Frame.RaceLeftMs / 1000.0) : FApexHudValue());
	Out.Set(TEXT("race.final_lap"), bTimed && Frame.RaceFinalLap > 0 ? FApexHudValue::Of(Frame.RaceFinalLap) : FApexHudValue());
	if (Local)
	{
		Out.Set(TEXT("lap.current"), Local->CurrentLap);
		// The counter keeps stepping on the cool-down lap after the flag.
		Out.Set(TEXT("lap.display"), ApexRace::DisplayLap(Local->CurrentLap, In.LapLimit));
		const int32 LapsToFlag = HudLapsToFlag(In, Frame, *Local);
		Out.Values.FindOrAdd(TEXT("lap.laps_left")) = LapsToFlag >= 0 ? FApexHudValue::Of(LapsToFlag) : FApexHudValue();
		// Certain only once a timed race's clock has run out.
		const bool bLastKnown = !bTimed || Frame.RaceFinalLap > 0;
		Out.Set(TEXT("lap.final"), LapsToFlag == 1 && bLastKnown);
		Out.Set(TEXT("lap.time_s"), Local->CurrentLapTimeMs / 1000.0);
		Out.Set(TEXT("lap.invalid"), Local->bLapInvalid);
		Out.Values.FindOrAdd(TEXT("lap.last_s")) = HudSeconds(Local->LastLapTimeMs);
		Out.Set(TEXT("lap.last_invalid"), Local->LastLapTimeMs > 0 && Local->bLastLapInvalid);
		Out.Values.FindOrAdd(TEXT("lap.best_s")) = HudSeconds(Local->BestLapTimeMs);
	}
	else
	{
		for (const TCHAR* Name : {TEXT("lap.current"), TEXT("lap.display"), TEXT("lap.laps_left"), TEXT("lap.time_s"),
				 TEXT("lap.last_s"), TEXT("lap.best_s")})
		{
			Out.SetNone(Name);
		}
		Out.Set(TEXT("lap.final"), false);
		Out.Set(TEXT("lap.invalid"), false);
		Out.Set(TEXT("lap.last_invalid"), false);
	}

	// The delta against the quickest legal lap this HUD has watched.
	{
		const float Fraction = Local && bLengthKnown ? FMath::Clamp(Local->TrackProgress / In.TrackLengthM, 0.0f, 1.0f) : 0.0f;
		const float ReferenceNow = Local ? Memory.ReferenceTimeAt(Fraction) : -1.0f;
		Out.Values.FindOrAdd(TEXT("timing.delta_s")) = ReferenceNow >= 0.0f
			? FApexHudValue::Of(Local->CurrentLapTimeMs / 1000.0 - ReferenceNow)
			: FApexHudValue();
		Out.Values.FindOrAdd(TEXT("timing.reference_s")) = Memory.ReferenceLapSeconds > 0.0f
			? FApexHudValue::Of(Memory.ReferenceLapSeconds)
			: FApexHudValue();
	}
	// The session's fastest lap. The server names it: only a lap inside track
	// limits can hold it.
	{
		Out.Values.FindOrAdd(TEXT("timing.session_best_s")) = HudSeconds(Timing.SessionBestLapMs);
		const FHudStanding* Holder = Order.FindByPredicate(
			[&Timing](const FHudStanding& Entry) { return Entry.Car->CarIndex == Timing.SessionBestLapCarIndex; });
		Out.Values.FindOrAdd(TEXT("timing.session_best_name")) = Holder ? FApexHudValue::Of(Holder->Name) : FApexHudValue();
		Out.Set(TEXT("timing.session_best_is_local"), Holder && Holder->bIsLocal);
		Out.Values.FindOrAdd(TEXT("timing.optimal_s")) = HudSeconds(LocalTiming ? LocalTiming->OptimalLapMs() : 0);
	}

	// --- Sectors --------------------------------------------------------------

	{
		const int32 Current = (Local && Sectors.IsValid()) ? Sectors.SectorAt(Local->TrackProgress) : 0;
		Out.Set(TEXT("sector.current"), Current + 1);
		// The server splits a lap into three unless the track names its own.
		const int32 Count = Timing.SectorCount > 0 ? Timing.SectorCount : Sectors.IsValid() ? Sectors.SectorCount() : 3;
		Out.Set(TEXT("sector.count"), Count);

		// The lap in progress, or (in the moments after the line, before the
		// first sector of the new lap is done) the lap that just ended, so the
		// driver gets to read their final split.
		const bool bShowLastLap = LocalTiming
			&& !LocalTiming->CurrentSplitsMs.ContainsByPredicate([](int32 Split) { return Split > 0; });
		const TArray<int32>* Splits = LocalTiming
			? (bShowLastLap ? &LocalTiming->LastSplitsMs : &LocalTiming->CurrentSplitsMs)
			: nullptr;

		TArray<FApexHudRecord>& List = Out.Lists.FindOrAdd(TEXT("sectors"));
		List.Reset(Count);
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const int32 Mine = Splits && Splits->IsValidIndex(Index) ? (*Splits)[Index] : 0;
			const int32 MyBest = LocalTiming && LocalTiming->BestSplitsMs.IsValidIndex(Index) ? LocalTiming->BestSplitsMs[Index] : 0;
			const int32 SessionBest = Timing.SessionBestSplitsMs.IsValidIndex(Index) ? Timing.SessionBestSplitsMs[Index] : 0;
			// Purple is the session's best, green the driver's own, amber a
			// sector they have done quicker before.
			const TCHAR* State = Mine <= 0 ? TEXT("none")
				: (SessionBest > 0 && Mine <= SessionBest) ? TEXT("session_best")
				: (MyBest <= 0 || Mine <= MyBest) ? TEXT("personal_best")
				: TEXT("slower");
			FApexHudRecord& Row = List.AddDefaulted_GetRef();
			Row.Add(TEXT("number"), FApexHudValue::Of(Index + 1));
			Row.Add(TEXT("time_s"), HudSeconds(Mine));
			Row.Add(TEXT("best_s"), HudSeconds(MyBest));
			Row.Add(TEXT("session_best_s"), HudSeconds(SessionBest));
			Row.Add(TEXT("state"), FApexHudValue::Of(State));
			Row.Add(TEXT("is_current"), FApexHudValue::Of(Index == Current));
		}
	}

	// --- The car ----------------------------------------------------------------

	Out.Set(TEXT("car.present"), Local != nullptr);
	{
		Out.Set(TEXT("car.driver_name"), Order.IsValidIndex(LocalPlace) ? FApexHudValue::Of(Order[LocalPlace].Name) : FApexHudValue());
		const FApexHudMemory::FCarHistory* History = Local ? Memory.Cars.Find(Local->CarIndex) : nullptr;
		Out.Set(TEXT("car.pit_stops"), History ? FApexHudValue::Of(History->PitStops) : FApexHudValue());
		Out.Set(TEXT("car.retired"), Local && HudRetired(*Local));
		Out.Set(TEXT("tyre.age_laps"), History && Local && Local->Compound >= 0
			? FApexHudValue::Of(FMath::Max(0, Local->CurrentLap - History->TyresFromLap))
			: FApexHudValue());
		Out.Set(TEXT("tyre.wear_max_pct"), Local ? HudWorstWear(*Local) : FApexHudValue());
	}
	Out.Set(TEXT("car.redline_rpm"), In.RedlineRpm > 0.0f ? FApexHudValue::Of(In.RedlineRpm) : FApexHudValue());
	if (Local)
	{
		const float Kph = ApexRace::MpsToKph(Local->SpeedMps);
		const float Mph = Local->SpeedMps * 2.236936f;
		Out.Set(TEXT("car.index"), Local->CarIndex);
		Out.Set(TEXT("car.speed_kph"), Kph);
		Out.Set(TEXT("car.speed_mph"), Mph);
		Out.Set(TEXT("car.speed"), In.bImperial ? Mph : Kph);
		Out.Set(TEXT("car.gear"), Local->Gear);
		Out.Set(TEXT("car.gear_text"), Local->Gear < 0 ? FString(TEXT("R")) : Local->Gear == 0 ? FString(TEXT("N")) : FString::FromInt(Local->Gear));
		Out.Set(TEXT("car.rpm"), Local->EngineRpm);

		// The scale is the car's rev limiter when its car.toml names one;
		// otherwise the highest reading so far, which only ever grows and so
		// never rescales the strip under the driver.
		Memory.ObservedMaxRpm = FMath::Max(Memory.ObservedMaxRpm, Local->EngineRpm);
		const float Ceiling = In.LimiterRpm > 0.0f ? FMath::Max(In.LimiterRpm, Local->EngineRpm)
			: In.RedlineRpm > 0.0f ? FMath::Max(In.RedlineRpm, Local->EngineRpm)
			: Memory.ObservedMaxRpm;
		Out.Set(TEXT("car.rpm_max"), Ceiling);
		Out.Set(TEXT("car.rpm_fraction"), FMath::Clamp(Local->EngineRpm / FMath::Max(Ceiling, 1.0f), 0.0f, 1.0f));

		Out.Set(TEXT("car.throttle"), FMath::Clamp(Local->Throttle, 0.0f, 1.0f));
		Out.Set(TEXT("car.brake"), FMath::Clamp(Local->Brake, 0.0f, 1.0f));
		Out.Set(TEXT("car.steering"), Local->Steering);
		Out.Set(TEXT("car.in_garage"), Local->bInGarage);
		Out.Set(TEXT("car.on_track"), Local->bIsOnTrack);
		Out.Set(TEXT("car.colliding"), Local->bIsColliding);
		Out.Set(TEXT("car.headlights"), Local->bHeadlights);
		Out.Set(TEXT("car.finish_position"), Local->FinishPosition);
		Out.Set(TEXT("car.drs_allowed"), Local->bDrsAllowed);
		Out.Set(TEXT("car.drs_open"), Local->bDrsOpen);
		Out.Set(TEXT("car.tow"), HudKnown(Local->TowShare, Local->TowShare >= 0.0f));
		Out.Set(TEXT("car.x"), Local->Position.X);
		Out.Set(TEXT("car.y"), Local->Position.Y);
		Out.Set(TEXT("car.z"), Local->Position.Z);
		Out.Set(TEXT("car.yaw_deg"), FMath::RadiansToDegrees(Local->YawRad));
		Out.Set(TEXT("car.station_m"), Local->TrackProgress);

		Out.Set(TEXT("pit.in_lane"), Local->bInPitLane);
		Out.Set(TEXT("pit.limiter"), Local->bPitLimiter);
		Out.Set(TEXT("pit.servicing"), Local->bPitServicing);
		Out.Set(TEXT("pit.service_s"), Local->ServiceSecondsLeft);
		Out.Set(TEXT("pit.autopilot"), Local->bPitAutopilot);
		Out.Set(TEXT("pit.exit_closed"), Local->bPitExitClosed);
		Out.Set(TEXT("pit.held"), Local->bPitHeld);
	}
	else
	{
		for (const TCHAR* Name : {TEXT("car.index"), TEXT("car.speed_kph"), TEXT("car.speed_mph"), TEXT("car.speed"),
				 TEXT("car.gear"), TEXT("car.gear_text"), TEXT("car.rpm"), TEXT("car.rpm_max"), TEXT("car.rpm_fraction"),
				 TEXT("car.throttle"), TEXT("car.brake"), TEXT("car.steering"), TEXT("car.in_garage"), TEXT("car.on_track"),
				 TEXT("car.colliding"), TEXT("car.headlights"), TEXT("car.finish_position"), TEXT("car.drs_allowed"),
				 TEXT("car.drs_open"), TEXT("car.tow"), TEXT("car.x"), TEXT("car.y"), TEXT("car.z"), TEXT("car.yaw_deg"),
				 TEXT("car.station_m"), TEXT("pit.in_lane"), TEXT("pit.limiter"), TEXT("pit.servicing"), TEXT("pit.service_s"),
				 TEXT("pit.autopilot"), TEXT("pit.exit_closed"), TEXT("pit.held")})
		{
			Out.SetNone(Name);
		}
	}

	// --- The pit stop -------------------------------------------------------------

	{
		// The crew's plan arrives once, as the car stops (`PitService`); the
		// countdown is telemetry's. The plan outlives the stop, so it is only
		// read while the car is being serviced.
		const FApexPitService* Stop = Local && In.PitServices ? In.PitServices->Find(Local->CarIndex) : nullptr;
		Out.Set(TEXT("pit.box"), Stop ? FApexHudValue::Of(Stop->PitBox + 1) : FApexHudValue());

		const FApexPitStopProgress P = Stop && Local->bPitServicing
			? ApexHudData::PitStopProgress(*Stop, Local->ServiceSecondsLeft)
			: FApexPitStopProgress();
		auto Known = [&P](float Value) { return P.bValid ? FApexHudValue::Of(Value) : FApexHudValue(); };
		Out.Set(TEXT("pit.service_total_s"), Known(P.TotalS));
		Out.Set(TEXT("pit.service_elapsed_s"), Known(P.ElapsedS));
		Out.Set(TEXT("pit.service_progress"), Known(P.Progress));
		const bool bPhase = P.bValid && !P.PhaseKey.IsEmpty();
		Out.Set(TEXT("pit.service_phase"), bPhase ? FApexHudValue::Of(P.PhaseKey) : FApexHudValue());
		Out.Set(TEXT("pit.service_phase_label"), bPhase ? FApexHudValue::Of(P.PhaseLabel) : FApexHudValue());
		Out.Set(TEXT("pit.service_phase_left_s"), bPhase ? FApexHudValue::Of(P.PhaseLeftS) : FApexHudValue());
		Out.Set(TEXT("pit.service_phase_progress"), bPhase ? FApexHudValue::Of(P.PhaseProgress) : FApexHudValue());

		static const TCHAR* const PartKeys[FApexPitStopProgress::PartCount] = {TEXT("tyres"), TEXT("fuel"), TEXT("repair")};
		for (int32 Part = 0; Part < FApexPitStopProgress::PartCount; ++Part)
		{
			Out.Values.FindOrAdd(FName(FString::Printf(TEXT("pit.service_%s_s"), PartKeys[Part]))) = Known(P.PartSeconds[Part]);
			Out.Values.FindOrAdd(FName(FString::Printf(TEXT("pit.service_%s_share"), PartKeys[Part]))) = Known(P.PartShare[Part]);
			Out.Values.FindOrAdd(FName(FString::Printf(TEXT("pit.service_%s_fill"), PartKeys[Part]))) = Known(P.PartFill[Part]);
		}
		Out.Set(TEXT("pit.service_fuel_l"), P.bValid ? FApexHudValue::Of(Stop->FuelL) : FApexHudValue());
		Out.Set(TEXT("pit.service_repair_pct"), P.bValid ? FApexHudValue::Of(Stop->RepairPct) : FApexHudValue());
		const FString Compound = P.bValid && P.PartSeconds[FApexPitStopProgress::Tyres] > 0.0f
			? FApexCarTelemetry::CompoundName(Stop->Compound)
			: FString();
		Out.Set(TEXT("pit.service_compound"), Compound.IsEmpty() ? FApexHudValue() : FApexHudValue::Of(Compound));
	}

	// --- Fuel -------------------------------------------------------------------

	{
		FApexHudValue Liters;
		FApexHudValue Laps;
		const TCHAR* State = TEXT("unknown");
		if (Local && Local->FuelLiters >= 0.0f)
		{
			// What a lap costs is measured at the line: the tank there against
			// the tank one lap earlier. A car filled up (the hotlap garage)
			// starts over.
			if (Local->CurrentLap != Memory.FuelLap)
			{
				if (Memory.FuelLap > 0 && Local->CurrentLap == Memory.FuelLap + 1 && Memory.FuelAtLapStart > Local->FuelLiters)
				{
					Memory.FuelPerLap = Memory.FuelAtLapStart - Local->FuelLiters;
				}
				Memory.FuelLap = Local->CurrentLap;
				Memory.FuelAtLapStart = Local->FuelLiters;
			}
			else if (Local->FuelLiters > Memory.FuelAtLapStart + 0.5f)
			{
				Memory.FuelAtLapStart = Local->FuelLiters;
				Memory.FuelPerLap = -1.0f;
			}

			Liters = FApexHudValue::Of(Local->FuelLiters);
			State = TEXT("ok");
			const int32 LapsLeft = HudLapsToFlag(In, Frame, *Local);
			if (Local->FuelLiters <= 0.0f)
			{
				State = TEXT("empty");
			}
			else if (Memory.FuelPerLap > 0.0f)
			{
				const float LapsOfFuel = Local->FuelLiters / Memory.FuelPerLap;
				Laps = FApexHudValue::Of(LapsOfFuel);
				State = LapsOfFuel < 1.0f ? TEXT("critical") : (LapsLeft > 0 && LapsOfFuel < LapsLeft) ? TEXT("short") : TEXT("ok");
			}
		}
		Out.Set(TEXT("fuel.liters"), Liters);
		Out.Set(TEXT("fuel.per_lap"), Memory.FuelPerLap > 0.0f && Local ? FApexHudValue::Of(Memory.FuelPerLap) : FApexHudValue());
		Out.Set(TEXT("fuel.laps"), Laps);
		Out.Set(TEXT("fuel.state"), State);
	}

	// --- Hybrid -----------------------------------------------------------------

	{
		const bool bHybrid = Local && Local->HasHybrid();
		Out.Set(TEXT("ers.present"), bHybrid);
		Out.Set(TEXT("ers.charge_pct"), bHybrid ? FApexHudValue::Of(Local->ErsChargePct) : FApexHudValue());
		Out.Set(TEXT("ers.lap_pct"), bHybrid && Local->ErsLapPct >= 0.0f ? FApexHudValue::Of(Local->ErsLapPct) : FApexHudValue());
		Out.Set(TEXT("ers.mode"), bHybrid ? FApexHudValue::Of(ApexErs::ShortLabel(Local->ErsMode)) : FApexHudValue());
		Out.Set(TEXT("ers.deploying"), bHybrid && Local->bErsDeploying);
		Out.Set(TEXT("ers.harvesting"), bHybrid && Local->bErsHarvesting);
		Out.Set(TEXT("ers.boost"), bHybrid && Local->bErsBoost);
	}

	// --- Tyres, brakes and engine -------------------------------------------------

	{
		const bool bTyres = Local && Local->HasTyres();
		Out.Set(TEXT("tyre.known"), bTyres);
		const FString Letter = Local ? FApexCarTelemetry::CompoundLetter(Local->Compound) : FString();
		Out.Set(TEXT("tyre.compound"), Letter.IsEmpty() ? FApexHudValue() : FApexHudValue::Of(Letter));
		Out.Set(TEXT("tyre.optimal_c"), In.TyreOptimalC);
		Out.Set(TEXT("tyre.window_c"), In.TyreWindowC);

		TArray<FApexHudRecord>& List = Out.Lists.FindOrAdd(TEXT("tyres"));
		List.Reset(4);
		for (int32 Tyre = 0; Tyre < 4; ++Tyre)
		{
			const float TempC = bTyres ? Local->TyreTempC[Tyre] : -1.0f;
			const float Kpa = Local ? Local->TyrePressureKpa[Tyre] : -1.0f;
			const float Wear = Local ? Local->TyreWearPct[Tyre] : -1.0f;
			const float BrakeC = Local ? Local->BrakeTempC[Tyre] : -1.0f;

			FApexHudRecord& Row = List.AddDefaulted_GetRef();
			Row.Add(TEXT("key"), FApexHudValue::Of(HudTyreKeys[Tyre]));
			Row.Add(TEXT("name"), FApexHudValue::Of(HudTyreNames[Tyre]));
			Row.Add(TEXT("temp_c"), HudKnown(TempC, TempC >= 0.0f));
			Row.Add(TEXT("pressure_kpa"), HudKnown(Kpa, bTyres && Kpa >= 0.0f));
			Row.Add(TEXT("wear_pct"), HudKnown(Wear, Wear >= 0.0f));
			Row.Add(TEXT("brake_c"), HudKnown(BrakeC, BrakeC >= 0.0f));
			Row.Add(TEXT("state"), FApexHudValue::Of(HudTyreState(TempC, In.TyreOptimalC, In.TyreWindowC)));
			Row.Add(TEXT("brake_state"), FApexHudValue::Of(HudBrakeState(BrakeC)));

			// The same figures by name, for a layout that places each tyre itself.
			for (const TPair<FName, FApexHudValue>& Field : Row)
			{
				if (Field.Key != TEXT("key") && Field.Key != TEXT("name"))
				{
					Out.Values.FindOrAdd(FName(FString::Printf(TEXT("tyre.%s.%s"), HudTyreKeys[Tyre], *Field.Key.ToString()))) = Field.Value;
				}
			}
		}

		const float Water = Local ? Local->WaterTempC : -1.0f;
		Out.Set(TEXT("engine.water_c"), HudKnown(Water, Water >= 0.0f));
		Out.Set(TEXT("engine.water_state"), HudWaterState(Water));
	}

	// --- Damage -----------------------------------------------------------------------

	{
		const bool bDamage = Local && Local->HasDamage();
		Out.Set(TEXT("damage.known"), bDamage);
		Out.Set(TEXT("damage.level"), In.DamageLevel == EApexDamageLevel::Off ? TEXT("off")
			: In.DamageLevel == EApexDamageLevel::Reduced ? TEXT("reduced") : TEXT("full"));

		TArray<FApexHudRecord>& List = Out.Lists.FindOrAdd(TEXT("damage"));
		List.Reset(5);
		for (int32 Zone = 0; Zone < 5; ++Zone)
		{
			FApexHudValue Pct;
			float Flash = 0.0f;
			if (bDamage)
			{
				const float D = FMath::Clamp(Local->DamagePct[Zone], 0.0f, 100.0f);
				// A fresh hit (a whole percent at once; overheating creeps in by
				// fractions and does not flash) lights the zone for a moment.
				if (D >= Memory.LastDamagePct[Zone] + 1.0f)
				{
					Memory.DamageFlashUntil[Zone] = In.TimeSeconds + HudDamageFlashSeconds;
				}
				else if (D < Memory.LastDamagePct[Zone])
				{
					// Repaired (a pit stop, the garage): no flash to finish.
					Memory.DamageFlashUntil[Zone] = 0.0;
				}
				Memory.LastDamagePct[Zone] = D;
				Pct = FApexHudValue::Of(D);
				Flash = static_cast<float>(FMath::Clamp((Memory.DamageFlashUntil[Zone] - In.TimeSeconds) / HudDamageFlashSeconds, 0.0, 1.0));
			}
			FApexHudRecord& Row = List.AddDefaulted_GetRef();
			Row.Add(TEXT("key"), FApexHudValue::Of(HudDamageKeys[Zone]));
			Row.Add(TEXT("name"), FApexHudValue::Of(HudDamageNames[Zone]));
			Row.Add(TEXT("pct"), Pct);
			Row.Add(TEXT("flash"), FApexHudValue::Of(Flash));
			Out.Values.FindOrAdd(FName(FString::Printf(TEXT("damage.%s"), HudDamageKeys[Zone]))) = Pct;
			Out.Values.FindOrAdd(FName(FString::Printf(TEXT("damage.%s_flash"), HudDamageKeys[Zone]))) = FApexHudValue::Of(Flash);
		}
	}
}

FApexHudPreview::FApexHudPreview()
{
	constexpr float Length = 5793.0f;
	static const TCHAR* const Names[] = {TEXT("Rex Thunder"), TEXT("Nova Blaze"), TEXT("Kai Storm"), TEXT("Player"),
		TEXT("Luna Swift"), TEXT("Max Voltage"), TEXT("Zara Vortex"), TEXT("Atlas Fury"), TEXT("Ivy Comet"), TEXT("Jett Rider")};
	constexpr int32 LocalIndex = 3;

	// A loop with a long straight and a few bends, for the minimap and the blips.
	constexpr int32 Points = 240;
	auto LoopAt = [](float Fraction)
	{
		const float T = Fraction * 2.0f * PI;
		return FVector2D(900.0f * FMath::Cos(T) + 160.0f * FMath::Cos(3.0f * T), 430.0f * FMath::Sin(T) + 90.0f * FMath::Sin(2.0f * T));
	};
	for (int32 Point = 0; Point < Points; ++Point)
	{
		Outline.Add(LoopAt(static_cast<float>(Point) / Points));
	}

	// The field strung out over a quarter of a lap, the player fourth.
	const float LocalFraction = 0.4f;
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(Names); ++Index)
	{
		const float Fraction = LocalFraction + (LocalIndex - Index) * 0.018f;
		FApexCarTelemetry Car;
		Car.CarIndex = Index;
		Car.CurrentLap = 6;
		Car.TrackProgress = Fraction * Length;
		Car.SpeedMps = 68.0f + (Index % 3) * 2.0f;
		const FVector2D At = LoopAt(Fraction);
		Car.Position = FVector(At.X, At.Y, 0.0f);
		Car.BestLapTimeMs = 81500 + Index * 230;
		Car.LastLapTimeMs = 82100 + Index * 310;
		Frame.Cars.Add(Car);

		FApexRosterEntry& Row = Roster.Entries.AddDefaulted_GetRef();
		Row.CarIndex = Index;
		Row.PlayerName = Names[Index];
	}

	FApexCarTelemetry& Local = Frame.Cars[LocalIndex];
	Local.SpeedMps = 63.0f;
	Local.Gear = 5;
	Local.EngineRpm = 9800.0f;
	Local.Throttle = 0.82f;
	Local.Brake = 0.0f;
	Local.CurrentLapTimeMs = 32600;
	Local.LastLapTimeMs = 82345;
	Local.BestLapTimeMs = 81902;
	Local.bDrsAllowed = true;
	Local.TowShare = 0.06f;
	Local.FuelLiters = 38.4f;
	Local.Compound = 1;
	const float Tread[4] = {88.0f, 91.0f, 95.0f, 97.0f};
	const float Pressure[4] = {176.0f, 178.0f, 181.0f, 183.0f};
	const float Wear[4] = {14.0f, 16.0f, 12.0f, 13.0f};
	const float Brakes[4] = {520.0f, 545.0f, 380.0f, 395.0f};
	for (int32 Tyre = 0; Tyre < 4; ++Tyre)
	{
		Local.TyreTempC[Tyre] = Tread[Tyre];
		Local.TyrePressureKpa[Tyre] = Pressure[Tyre];
		Local.TyreWearPct[Tyre] = Wear[Tyre];
		Local.BrakeTempC[Tyre] = Brakes[Tyre];
	}
	Local.WaterTempC = 92.0f;
	const float Damage[5] = {8.0f, 0.0f, 3.0f, 0.0f, 1.0f};
	for (int32 Zone = 0; Zone < 5; ++Zone)
	{
		Local.DamagePct[Zone] = Damage[Zone];
	}
	Local.ErsChargePct = 64.0f;
	Local.ErsLapPct = 72.0f;
	Local.ErsMode = 1;
	// A stop twelve seconds in: the tyres done, the fuel going in, repairs
	// to come. Nobody stops at racing speed, but the panels are the point.
	Local.bInPitLane = true;
	Local.bPitServicing = true;
	Local.ServiceSecondsLeft = 11.5f;
	FApexPitService& Stop = PitServices.Add(LocalIndex);
	Stop.CarIndex = LocalIndex;
	Stop.PitBox = 3;
	Stop.TyresS = 9.0f;
	Stop.Compound = 0;
	Stop.FuelS = 12.5f;
	Stop.FuelL = 25.0f;
	Stop.RepairS = 2.0f;
	Stop.RepairPct = 12.0f;
	Stop.TotalS = 23.5f;

	Timing.SectorCount = 3;
	Timing.SessionBestLapMs = 81500;
	Timing.SessionBestLapCarIndex = 0;
	Timing.SessionBestSplitsMs = {28200, 27100, 26200};
	FApexCarTiming& Mine = Timing.Cars.Add(LocalIndex);
	Mine.CurrentSplitsMs = {28412, 0, 0};
	Mine.BestSplitsMs = {28480, 27300, 26122};
	Mine.BestLapMs = 81902;

	Sectors.TrackLengthM = Length;
	Sectors.BoundariesM = {Length / 3.0f, Length * 2.0f / 3.0f};

	// A reference lap that puts the player two tenths up at this point.
	Memory.ReferenceLap.Emplace(0.0f, 0.0f);
	Memory.ReferenceLap.Emplace(1.0f, 82.0f);
	Memory.ReferenceLapSeconds = 82.0f;
	Memory.LastSeenLap = 6;

	Inputs.Frame = &Frame;
	Inputs.LocalCarIndex = LocalIndex;
	Inputs.Roster = &Roster;
	Inputs.Timing = &Timing;
	Inputs.Sectors = &Sectors;
	Inputs.PitServices = &PitServices;
	Inputs.TrackLengthM = Length;
	Inputs.LapLimit = 12;
	Inputs.GameMode = EApexGameMode::Race;
	Inputs.ModeName = TEXT("Race");
	Inputs.TrackName = TEXT("Preview circuit");
	Inputs.CarName = TEXT("Your car");
	Inputs.PingMs = 24;
	Inputs.LimiterRpm = 11000.0f;
	Inputs.RedlineRpm = 10500.0f;
	Inputs.bHasConditions = true;
}
