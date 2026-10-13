#pragma once

#include "CoreMinimal.h"
#include "ApexProtocolTypes.h"
#include "Race/ApexSkyModel.h"

/**
 * What the create-session screen shows, worked out from the choices on it.
 *
 * Pure maths, like `ApexSky`: the grid the server will line up, the air it
 * will resolve for a pick left on auto, the grip the weather leaves on the
 * road, and the colours of the sky preview. The server figures are ports of
 * its own (`SessionConditions` in server/src/data.rs) so the preview says
 * what the session will be, not a guess; `ApexSim.UI.CreateSession.*` pins
 * them against values worked out from the Rust.
 */
namespace ApexCreateSession
{
	/** The most cars a starting grid holds (the server's 20 slots). */
	constexpr int32 GridCeiling = 20;

	/** What sits in one slot of the starting grid. */
	enum class ESlot : uint8
	{
		/** Past the end of the grid. */
		None,
		Ai,
		You,
		/** A seat another player can take. */
		Open,
	};

	/**
	 * The grid a session lines up, the way the server fills it: the AI are
	 * added with the session and take the first slots, the host joins
	 * behind them, anyone else after (`GameSession::with_ai_profiles`, then
	 * `add_player` takes the first free position). A single-player session
	 * has no open seats, so its field is the AI and the player.
	 */
	struct FGrid
	{
		int32 Field = 1;
		int32 Ai = 0;
		int32 Open = 0;
		/** 1-based. */
		int32 YourSlot = 1;
	};

	inline FGrid Grid(bool bMultiplayer, int32 MaxPlayers, int32 AiCount, int32 Ceiling)
	{
		FGrid Out;
		Out.Field = bMultiplayer
			? FMath::Clamp(MaxPlayers, 1, Ceiling)
			: FMath::Clamp(AiCount + 1, 1, Ceiling);
		Out.Ai = FMath::Clamp(AiCount, 0, Out.Field - 1);
		Out.Open = bMultiplayer ? Out.Field - 1 - Out.Ai : 0;
		Out.YourSlot = Out.Ai + 1;
		return Out;
	}

	/** The slot at grid position Position (1-based). */
	inline ESlot SlotAt(const FGrid& G, int32 Position)
	{
		if (Position < 1 || Position > G.Field)
		{
			return ESlot::None;
		}
		if (Position <= G.Ai)
		{
			return ESlot::Ai;
		}
		return Position == G.YourSlot ? ESlot::You : ESlot::Open;
	}

	// --- The start order ------------------------------------------------------
	//
	// The host's order is a list of driver references the server resolves
	// when the race counts in (`server/src/grid_order.rs`): `@host`, `@ai:N`
	// (the Nth AI car, 1-based) or a driver's name, which is how a loaded
	// qualifying result names whoever set a lap. A driver the list leaves out
	// starts behind the listed ones. The create screen keeps the list
	// *normalised* once it is edited: the references that can match, each
	// once, then everyone the field holds that it left out, so what the
	// preview shows is what the server will seat.

	/** What the server reads as the host's own car. */
	inline const TCHAR* HostRef() { return TEXT("@host"); }

	inline FString AiRef(int32 Number) { return FString::Printf(TEXT("@ai:%d"), Number); }

	/** The AI number a reference names (1-based), 0 when it is not one. */
	inline int32 AiNumberOf(const FString& Ref)
	{
		return Ref.StartsWith(TEXT("@ai:")) ? FCString::Atoi(*Ref.Mid(4)) : 0;
	}

	/** The order cars are seated in: the AI first, the host behind them. */
	inline TArray<FString> DefaultOrder(int32 Ai)
	{
		TArray<FString> Out;
		for (int32 Number = 1; Number <= Ai; ++Number)
		{
			Out.Add(AiRef(Number));
		}
		Out.Add(HostRef());
		return Out;
	}

	/**
	 * `Order` made complete for a field of `Field` cars with `Ai` of them AI:
	 * references that cannot match (an AI past the count, a blank) dropped,
	 * repeats dropped, then the AI and the host the list left out behind it,
	 * and the whole cut to the field with the host kept.
	 */
	inline TArray<FString> Normalise(const TArray<FString>& Order, int32 Ai, int32 Field)
	{
		TArray<FString> Out;
		auto Add = [&Out](const FString& Ref)
		{
			if (!Ref.IsEmpty() && !Out.Contains(Ref))
			{
				Out.Add(Ref);
			}
		};
		for (const FString& Ref : Order)
		{
			const bool bAi = Ref.StartsWith(TEXT("@ai:"));
			if (bAi ? (AiNumberOf(Ref) >= 1 && AiNumberOf(Ref) <= Ai) : (Ref == HostRef() || !Ref.StartsWith(TEXT("@"))))
			{
				Add(Ref);
			}
		}
		for (const FString& Ref : DefaultOrder(Ai))
		{
			Add(Ref);
		}
		const int32 Cars = FMath::Max(1, Field);
		if (Out.Num() > Cars)
		{
			const bool bHostKept = Out.IndexOfByKey(HostRef()) < Cars;
			Out.SetNum(Cars);
			if (!bHostKept)
			{
				Out.Last() = HostRef();
			}
		}
		return Out;
	}

	/** Whether `Order` is just the seating order, so nothing need be sent. */
	inline bool IsDefaultOrder(const TArray<FString>& Order, int32 Ai, int32 Field)
	{
		return Order.IsEmpty() || Normalise(Order, Ai, Field) == Normalise(DefaultOrder(Ai), Ai, Field);
	}

	/** One occupied slot of the starting grid as the preview draws it. */
	struct FGridEntry
	{
		FString Ref;
		/** `YOU`, `AI 2`, or the driver's own name. */
		FString Label;
		bool bYou = false;
		bool bAi = false;
	};

	/** The occupied slots, first to last, for a normalised order. */
	inline TArray<FGridEntry> Entries(const TArray<FString>& Normalised)
	{
		TArray<FGridEntry> Out;
		for (const FString& Ref : Normalised)
		{
			FGridEntry Entry;
			Entry.Ref = Ref;
			if (Ref == HostRef())
			{
				Entry.Label = TEXT("YOU");
				Entry.bYou = true;
			}
			else if (const int32 Number = AiNumberOf(Ref); Number > 0)
			{
				Entry.Label = FString::Printf(TEXT("AI %d"), Number);
				Entry.bAi = true;
			}
			else
			{
				Entry.Label = Ref;
			}
			Out.Add(Entry);
		}
		return Out;
	}

	/** Moves the entry at `From` so it sits at `To` (the others shift). False when nothing moved. */
	inline bool MoveEntry(TArray<FString>& Normalised, int32 From, int32 To)
	{
		if (!Normalised.IsValidIndex(From) || From == To)
		{
			return false;
		}
		To = FMath::Clamp(To, 0, Normalised.Num() - 1);
		if (From == To)
		{
			return false;
		}
		const FString Moved = Normalised[From];
		Normalised.RemoveAt(From);
		Normalised.Insert(Moved, To);
		return true;
	}

	/**
	 * The order a stored qualifying result grids, fastest first: the driver
	 * named `HostName` is the host, the result's AI take this session's AI
	 * in the order they qualified (the Nth quickest AI is `@ai:N`; the ones
	 * past this session's `Ai` are dropped), and any other human is named,
	 * for the server to find if they join. Normalise it for the field after.
	 */
	inline TArray<FString> OrderFromResult(const FApexQualifyingResult& Result, const FString& HostName, int32 Ai)
	{
		TArray<FString> Out;
		int32 NextAi = 1;
		for (const FApexQualifyingEntry& Entry : Result.Entries)
		{
			if (Entry.bIsAi)
			{
				if (NextAi <= Ai)
				{
					Out.Add(AiRef(NextAi++));
				}
			}
			else if (!HostName.IsEmpty() && Entry.Name == HostName)
			{
				Out.AddUnique(HostRef());
			}
			else if (!Entry.Name.IsEmpty() && !Entry.Name.StartsWith(TEXT("@")))
			{
				Out.AddUnique(Entry.Name);
			}
		}
		return Out;
	}

	// --- The air, as the server resolves it ----------------------------------

	/** `SessionConditions::auto_air_temperature_c`: 13 °C before dawn to 23 °C
	 * at 15:00 on a clear day, the swing damped and the whole curve lowered
	 * under cloud and rain. */
	inline float AutoAirTempC(const FApexSessionConditions& C)
	{
		constexpr float CoolestC = 13.0f;
		constexpr float SwingC = 10.0f;
		constexpr float WarmestHours = 15.0f;
		const float Hours = C.Hours();
		const float Day = 0.5f + 0.5f * FMath::Cos((Hours - WarmestHours) / 24.0f * UE_TWO_PI);
		float Swing = 1.0f;
		float Offset = 0.0f;
		switch (C.Weather)
		{
		case EApexWeather::Cloudy:    Swing = 0.8f; Offset = -0.5f; break;
		case EApexWeather::Overcast:  Swing = 0.5f; Offset = -1.5f; break;
		case EApexWeather::LightRain: Swing = 0.4f; Offset = -3.0f; break;
		case EApexWeather::HeavyRain: Swing = 0.3f; Offset = -4.0f; break;
		// Overcast's own day, 17 degrees colder (`Weather::SNOW_AIR_DROP_C`).
		case EApexWeather::Snow:      Swing = 0.4f; Offset = -2.5f - 17.0f; break;
		default: break;
		}
		return CoolestC + SwingC * (0.5f + (Day - 0.5f) * Swing) + Offset;
	}

	/** The air in whole degrees: the host's figure, else the weather's
	 * (Rust's `round`, half away from zero, as `RoundHalfFromZero`). */
	inline int32 AirTempC(const FApexSessionConditions& C)
	{
		return C.HasAirTemp()
			? FMath::Clamp(C.AirTempC, FApexSessionConditions::MinAirTempC, FApexSessionConditions::MaxAirTempC)
			: static_cast<int32>(FMath::RoundHalfFromZero(AutoAirTempC(C)));
	}

	/** `SessionConditions::track_temperature_c`: the air plus up to 20 °C of
	 * sun through the cloud; a wet track a degree under the air. */
	inline float TrackTempC(const FApexSessionConditions& C)
	{
		const float Air = static_cast<float>(AirTempC(C));
		if (C.IsWet() || C.IsSnow())
		{
			return Air - 1.0f;
		}
		const float Sun = FMath::Max(0.0f, FMath::Sin(FMath::DegreesToRadians(ApexSky::SunAt(C.Hours()).ElevationDeg)));
		const float ThroughCloud = C.Weather == EApexWeather::Sunny ? 1.0f
			: C.Weather == EApexWeather::Cloudy ? 0.6f
			: 0.25f;
		return Air + 20.0f * Sun * ThroughCloud;
	}

	/** `SessionConditions::auto_wind_kph`. */
	inline int32 AutoWindKph(EApexWeather Weather)
	{
		switch (Weather)
		{
		case EApexWeather::Cloudy:    return 12;
		case EApexWeather::Overcast:  return 15;
		case EApexWeather::LightRain: return 18;
			case EApexWeather::HeavyRain: return 28;
		case EApexWeather::Snow:      return 10;
		default:                      return 8;
		}
	}

	inline int32 WindKph(const FApexSessionConditions& C)
	{
		return C.HasWind() ? FMath::Min(C.WindKph, FApexSessionConditions::MaxWindKph) : AutoWindKph(C.Weather);
	}

	// --- The sky through the session ------------------------------------------

	/** How fast the day's clock may run, the Clock row's chips: frozen, then
	 * real time to an hour a minute. */
	inline constexpr int32 ClockScales[] = {0, 1, 2, 4, 8, 24, 60};
	inline constexpr int32 ClockScaleCount = UE_ARRAY_COUNT(ClockScales);

	/** "Frozen", "1x", "24x". */
	inline FString ClockScaleLabel(int32 Scale)
	{
		return Scale <= 0 ? FString(TEXT("Frozen")) : FString::Printf(TEXT("%dx"), Scale);
	}

	/** The Clock row's chip for a pick: an unset scale is frozen. */
	inline int32 ClockScaleIndex(const FApexSessionConditions& C)
	{
		const int32 Scale = C.ClockRuns() ? C.TimeScale : 0;
		for (int32 Index = 0; Index < ClockScaleCount; ++Index)
		{
			if (ClockScales[Index] == Scale)
			{
				return Index;
			}
		}
		return INDEX_NONE;
	}

	/** A chip's scale onto the conditions: frozen is left off the wire. */
	inline void SetClockScale(FApexSessionConditions& C, int32 Scale)
	{
		C.TimeScale = Scale > 0 ? FMath::Min(Scale, FApexSessionConditions::MaxTimeScale) : FApexSessionConditions::Auto;
	}

	/** The Weather changes row: Fixed (0, off the wire), Settled, Changeable, Stormy. */
	inline constexpr int32 ChangeableCount = FApexSessionConditions::MaxChangeable + 1;

	inline int32 ChangeableIndex(const FApexSessionConditions& C)
	{
		return C.WeatherChanges() ? FMath::Min(C.Changeable, FApexSessionConditions::MaxChangeable) : 0;
	}

	inline void SetChangeable(FApexSessionConditions& C, int32 Level)
	{
		C.Changeable = Level > 0 ? FMath::Min(Level, FApexSessionConditions::MaxChangeable) : FApexSessionConditions::Auto;
	}

	/** The Track row: Green (0), Normal (unset: the calibrated 50, off the
	 * wire) and Rubbered (100). */
	inline constexpr int32 TrackRubberPicks[] = {0, FApexSessionConditions::Auto, 100};
	inline constexpr int32 TrackRubberCount = UE_ARRAY_COUNT(TrackRubberPicks);

	inline const TCHAR* TrackRubberChipLabel(int32 Index)
	{
		static const TCHAR* Labels[TrackRubberCount] = {TEXT("Green"), TEXT("Normal"), TEXT("Rubbered")};
		return Labels[FMath::Clamp(Index, 0, TrackRubberCount - 1)];
	}

	/** The chip for a pick; a figure between them (a profile edited by hand) shows none. */
	inline int32 TrackRubberIndex(const FApexSessionConditions& C)
	{
		if (!C.HasTrackRubber() || C.TrackRubberPct == FApexSessionConditions::ReferenceRubberPct)
		{
			return 1;
		}
		return C.TrackRubberPct == 0 ? 0 : C.TrackRubberPct >= 100 ? 2 : INDEX_NONE;
	}

	inline void SetTrackRubber(FApexSessionConditions& C, int32 Index)
	{
		C.TrackRubberPct = TrackRubberPicks[FMath::Clamp(Index, 0, TrackRubberCount - 1)];
	}

	// --- The forecast ---------------------------------------------------------
	//
	// The server lays the weather's changes out from a seed
	// (`crate::conditions::forecast`): the host's `sky_seed`, else the
	// session's id. With a seed of its own the create screen shows the same
	// list before the session exists, and the HUD the whole of it once in
	// one (the sky's telemetry carries the next three). A port, to the bit:
	// `ApexSim.UI.CreateSession.Forecast` pins it against the Rust's own
	// `the_forecast_reference`.

	/** `wind::hash01`: a hash of (seed, salt) onto 0..1, splitmix64. */
	inline float Hash01(uint64 Seed, uint64 Salt)
	{
		uint64 Z = Seed ^ (Salt * 0x9E3779B97F4A7C15ull);
		Z += 0x9E3779B97F4A7C15ull;
		Z = (Z ^ (Z >> 30)) * 0xBF58476D1CE4E5B9ull;
		Z = (Z ^ (Z >> 27)) * 0x94D049BB133111EBull;
		Z ^= Z >> 31;
		return static_cast<float>(Z >> 40) / 16777216.0f;
	}

	/** `conditions::WINTER_BELOW_C`: an overcast start under this air is a winter sky. */
	constexpr float WinterBelowC = 1.0f;
	/** `conditions::MIN_GAP_S`: no change sooner than this, s of session. */
	constexpr float ForecastMinGapS = 240.0f;
	/** `conditions::HORIZON_S`: how far ahead the forecast is laid out, s of session. */
	constexpr float ForecastHorizonS = 26.0f * 3600.0f;
	constexpr int32 ForecastMaxChanges = 400;

	/** `conditions::is_winter`: snow, or overcast with the air under WinterBelowC
	 * (the air as the server resolves it: the host's figure, else the weather's). */
	inline bool IsWinter(const FApexSessionConditions& In)
	{
		const FApexSessionConditions C = In.Clamped();
		return C.IsSnow() || (C.Weather == EApexWeather::Overcast && static_cast<float>(AirTempC(C)) < WinterBelowC);
	}

	/** One change: from `AtS` seconds into the session the weather is `Weather`. */
	struct FForecastChange
	{
		float AtS = 0.0f;
		EApexWeather Weather = EApexWeather::Sunny;
	};

	/**
	 * `conditions::forecast` for a session starting under `In` from `Seed`:
	 * empty unless the weather changes. Each step lasts a stretch of the day
	 * (40 to 120 minutes settled, 25 to 75 changeable, 15 to 45 stormy; run
	 * through the clock's scale, never under ForecastMinGapS) and moves the
	 * sky one step wetter or drier (two now and then above settled), drawn
	 * back toward the host's pick; a winter sky swings between overcast and
	 * snow instead.
	 */
	inline TArray<FForecastChange> Forecast(const FApexSessionConditions& In, uint64 Seed)
	{
		TArray<FForecastChange> Out;
		const FApexSessionConditions C = In.Clamped();
		const int32 Level = C.WeatherChanges() ? C.Changeable : 0;
		if (Level == 0)
		{
			return Out;
		}
		const float GapLo = Level == 1 ? 40.0f : Level == 2 ? 25.0f : 15.0f;
		const float GapHi = Level == 1 ? 120.0f : Level == 2 ? 75.0f : 45.0f;
		const float Scale = static_cast<float>(FMath::Max(C.ClockRuns() ? C.TimeScale : 0, 1));
		auto Gap = [&](uint64 K)
		{
			const float GapMin = GapLo + (GapHi - GapLo) * Hash01(Seed, 0xF00000ull + 3 * K);
			return FMath::Max(GapMin * 60.0f / Scale, ForecastMinGapS);
		};
		if (IsWinter(C))
		{
			const float HomeLean = C.IsSnow() ? 0.15f : -0.15f;
			const float PSnow = FMath::Clamp(0.5f + 0.1f * (static_cast<float>(Level) - 2.0f) + HomeLean, 0.1f, 0.9f);
			EApexWeather Weather = C.Weather;
			float AtS = 0.0f;
			for (uint64 K = 0; K < static_cast<uint64>(ForecastMaxChanges); ++K)
			{
				AtS += Gap(K);
				if (AtS > ForecastHorizonS)
				{
					break;
				}
				const EApexWeather Next = Hash01(Seed, 0xF00001ull + 3 * K) < PSnow ? EApexWeather::Snow : EApexWeather::Overcast;
				if (Next != Weather)
				{
					Weather = Next;
					Out.Add({AtS, Weather});
				}
			}
			return Out;
		}
		const int32 Home = static_cast<int32>(C.Weather);
		int32 Weather = Home;
		float AtS = 0.0f;
		for (uint64 K = 0; K < static_cast<uint64>(ForecastMaxChanges); ++K)
		{
			AtS += Gap(K);
			if (AtS > ForecastHorizonS)
			{
				break;
			}
			const float Lean = 0.1f * (static_cast<float>(Level) - 2.0f) - 0.15f * static_cast<float>(Weather - Home);
			const bool bWetter = Hash01(Seed, 0xF00001ull + 3 * K) < FMath::Clamp(0.5f + Lean, 0.1f, 0.9f);
			const bool bTwo = Level > 1 && Hash01(Seed, 0xF00002ull + 3 * K) < 0.15f * static_cast<float>(Level - 1);
			const int32 Step = bTwo ? 2 : 1;
			int32 Next = bWetter ? Weather + Step : Weather - Step;
			if (Next < 0 || Next > 4)
			{
				Next = bWetter ? Weather - 1 : Weather + 1;
			}
			Weather = FMath::Clamp(Next, 0, 4);
			Out.Add({AtS, static_cast<EApexWeather>(Weather)});
		}
		return Out;
	}

	/**
	 * `Uuid::as_u64_pair().0` of a session id ("01234567-89ab-cdef-..."): its
	 * first eight bytes as a big-endian integer, the seed a session with no
	 * `sky_seed` is laid out from. 0 for anything that is not a UUID.
	 */
	inline uint64 SeedOfSessionId(const FString& SessionId)
	{
		uint64 Seed = 0;
		int32 Digits = 0;
		for (int32 Index = 0; Index < SessionId.Len() && Digits < 16; ++Index)
		{
			const TCHAR Ch = SessionId[Index];
			if (Ch == TEXT('-'))
			{
				continue;
			}
			if (!FChar::IsHexDigit(Ch))
			{
				return 0;
			}
			Seed = (Seed << 4) | static_cast<uint64>(FParse::HexDigit(Ch));
			++Digits;
		}
		return Digits == 16 ? Seed : 0;
	}

	/** The seed a joined session's sky is laid out from: the host's, else its id's. */
	inline uint64 SessionSeed(const FApexSessionConditions& C, const FString& SessionId)
	{
		return C.HasSkySeed() ? C.SkySeed : SeedOfSessionId(SessionId);
	}

	/** A fresh seed for the create screen (never 0, so it reads as set). */
	inline uint64 NewSkySeed()
	{
		const FGuid Guid = FGuid::NewGuid();
		const uint64 Seed = (static_cast<uint64>(Guid.A) << 32) | static_cast<uint64>(Guid.B);
		return Seed == 0 ? 1 : Seed;
	}

	/** The day's clock at `AtS` seconds into a session starting under `C`, "14:12". */
	inline FString ClockAt(const FApexSessionConditions& C, float AtS)
	{
		const int32 Scale = C.ClockRuns() ? C.TimeScale : 0;
		const int32 Minutes = C.Clamped().TimeOfDayMinutes + FMath::FloorToInt(AtS * static_cast<float>(Scale) / 60.0f);
		const int32 Wrapped = ((Minutes % FApexSessionConditions::MinutesPerDay) + FApexSessionConditions::MinutesPerDay)
			% FApexSessionConditions::MinutesPerDay;
		return FString::Printf(TEXT("%02d:%02d"), Wrapped / 60, Wrapped % 60);
	}

	/**
	 * The forecast as the create screen prints it: "14:12 Cloudy / 14:40
	 * Light rain / 15:03 Overcast ...", the first `Shown` changes with the
	 * day's clock when it runs (else the session's "+12 min"); empty when
	 * the sky holds or the forecast is empty.
	 */
	inline FString ForecastLine(const FApexSessionConditions& C, const TArray<FForecastChange>& Changes, int32 Shown = 8)
	{
		TArray<FString> Parts;
		for (int32 Index = 0; Index < Changes.Num() && Index < Shown; ++Index)
		{
			const FForecastChange& Change = Changes[Index];
			const FString When = C.ClockRuns() ? ClockAt(C, Change.AtS)
				: FString::Printf(TEXT("+%d min"), FMath::Max(1, FMath::RoundToInt(Change.AtS / 60.0f)));
			Parts.Add(FString::Printf(TEXT("%s %s"), *When, *FApexSessionConditions::WeatherLabel(Change.Weather)));
		}
		FString Line = FString::Join(Parts, TEXT("  /  "));
		if (Changes.Num() > Shown)
		{
			Line += TEXT("  ...");
		}
		return Line;
	}

	/** The road's grip under this weather, percent: `Weather::road_grip_factor`. */
	inline int32 GripPercent(EApexWeather Weather)
	{
		switch (Weather)
		{
		case EApexWeather::Overcast:  return 98;
		case EApexWeather::LightRain: return 86;
		case EApexWeather::HeavyRain: return 74;
		case EApexWeather::Snow:      return 55;
		default:                      return 100;
		}
	}

	// --- The sky preview ------------------------------------------------------

	/** How a weather looks on its tile and over the preview's sky. */
	struct FWeatherLook
	{
		FColor Top;
		FColor Bottom;
		/** 0 clear .. 1 a lid of cloud. */
		float Cloud = 0.0f;
		/** 0 dry .. 1 heavy rain. */
		float Rain = 0.0f;
	};

	inline FWeatherLook WeatherLook(EApexWeather Weather)
	{
		switch (Weather)
		{
		case EApexWeather::Cloudy:    return { FColor(0x5D, 0x78, 0x96), FColor(0xB3, 0xC1, 0xCC), 0.35f, 0.0f };
		case EApexWeather::Overcast:  return { FColor(0x5D, 0x64, 0x6B), FColor(0x9A, 0xA1, 0xA7), 0.7f, 0.0f };
		case EApexWeather::LightRain: return { FColor(0x45, 0x4C, 0x54), FColor(0x7B, 0x83, 0x8B), 0.8f, 0.45f };
		case EApexWeather::HeavyRain: return { FColor(0x2B, 0x31, 0x37), FColor(0x5A, 0x61, 0x68), 0.9f, 1.0f };
		case EApexWeather::Snow:      return { FColor(0x7C, 0x8B, 0x9E), FColor(0xD9, 0xDF, 0xE6), 0.85f, 0.0f };
		default:                      return { FColor(0x3D, 0x78, 0xC4), FColor(0xA9, 0xCB, 0xE6), 0.0f, 0.0f };
		}
	}

	/** Byte-wise blend, the way the design mixes its sky stops. */
	inline FColor Mix(const FColor& A, const FColor& B, float T)
	{
		const float U = FMath::Clamp(T, 0.0f, 1.0f);
		return FColor(
			static_cast<uint8>(FMath::RoundToInt(FMath::Lerp(static_cast<float>(A.R), static_cast<float>(B.R), U))),
			static_cast<uint8>(FMath::RoundToInt(FMath::Lerp(static_cast<float>(A.G), static_cast<float>(B.G), U))),
			static_cast<uint8>(FMath::RoundToInt(FMath::Lerp(static_cast<float>(A.B), static_cast<float>(B.B), U))));
	}

	struct FSkyLook
	{
		FColor Top;
		FColor Bottom;
		/** Across the preview, 0 left (sunrise) .. 1 right (sunset). */
		float SunX = 0.5f;
		/** Above the horizon as a share of the sky over it; negative below. */
		float SunHeight = 0.5f;
		float SunOpacity = 1.0f;
		/** A low sun is orange. */
		bool bLowSun = false;
		/** "NIGHT", "DAWN", "DAY", "DUSK". */
		const TCHAR* Phase = TEXT("DAY");
	};

	/**
	 * The preview's sky at this hour under this weather, from the same sun
	 * the race is lit by (`ApexSky::SunAt`): night blue under -8°, a dawn
	 * glow round the horizon, day blue from 14° up, all greyed by the cloud.
	 */
	inline FSkyLook Sky(const FApexSessionConditions& C)
	{
		const FColor NightTop(11, 16, 32), NightBottom(26, 34, 56);
		const FColor DawnTop(42, 58, 102), DawnBottom(227, 138, 74);
		const FColor DayTop(61, 120, 196), DayBottom(169, 203, 230);

		const float Hours = C.Hours();
		const float Elevation = ApexSky::SunAt(Hours).ElevationDeg;

		FSkyLook Out;
		if (Elevation <= -8.0f)
		{
			Out.Top = NightTop;
			Out.Bottom = NightBottom;
		}
		else if (Elevation < 2.0f)
		{
			const float T = (Elevation + 8.0f) / 10.0f;
			Out.Top = Mix(NightTop, DawnTop, T);
			Out.Bottom = Mix(NightBottom, DawnBottom, T);
		}
		else if (Elevation < 14.0f)
		{
			const float T = (Elevation - 2.0f) / 12.0f;
			Out.Top = Mix(DawnTop, DayTop, T);
			Out.Bottom = Mix(DawnBottom, DayBottom, T);
		}
		else
		{
			Out.Top = DayTop;
			Out.Bottom = DayBottom;
		}

		const FWeatherLook Weather = WeatherLook(C.Weather);
		Out.Top = Mix(Out.Top, FColor(92, 99, 106), Weather.Cloud * 0.7f);
		Out.Bottom = Mix(Out.Bottom, FColor(140, 148, 156), Weather.Cloud * 0.7f);

		// The sun's day on the model's date runs from about 05:17 to 20:43
		// either side of the 13:00 solar noon (50 N, late May).
		constexpr float HalfDayHours = 7.71f;
		Out.SunX = FMath::Clamp((Hours - (ApexSky::SolarNoonHours - HalfDayHours)) / (2.0f * HalfDayHours), 0.0f, 1.0f);
		Out.SunHeight = Elevation / 62.0f;
		Out.SunOpacity = Elevation > -3.0f ? 1.0f - Weather.Cloud * 0.85f : 0.0f;
		Out.bLowSun = Elevation < 12.0f;
		Out.Phase = Elevation < -6.0f ? TEXT("NIGHT")
			: Elevation < 10.0f ? (Hours < ApexSky::SolarNoonHours ? TEXT("DAWN") : TEXT("DUSK"))
			: TEXT("DAY");
		return Out;
	}

	// --- Assists --------------------------------------------------------------

	enum class EAssistPreset : uint8
	{
		/** Everything allowed. */
		Novice,
		/** The aids a club race allows: ABS, traction control, the line. */
		Club,
		/** Nothing. */
		Pro,
		Count,
	};

	inline const TCHAR* PresetName(EAssistPreset Preset)
	{
		switch (Preset)
		{
		case EAssistPreset::Novice: return TEXT("Novice");
		case EAssistPreset::Club:   return TEXT("Club");
		default:                    return TEXT("Pro");
		}
	}

	inline FApexAllowedAssists AssistPreset(EAssistPreset Preset)
	{
		FApexAllowedAssists Out;
		if (Preset == EAssistPreset::Novice)
		{
			return Out;
		}
		const bool bClub = Preset == EAssistPreset::Club;
		Out.bAbs = bClub;
		Out.bTractionControl = bClub;
		Out.bRacingLine = bClub;
		Out.bAutoGearbox = false;
		Out.bSteeringAssist = false;
		return Out;
	}

	/** The preset these assists are exactly, or INDEX_NONE. */
	inline int32 MatchingPreset(const FApexAllowedAssists& Assists)
	{
		for (int32 Index = 0; Index < static_cast<int32>(EAssistPreset::Count); ++Index)
		{
			if (AssistPreset(static_cast<EAssistPreset>(Index)) == Assists)
			{
				return Index;
			}
		}
		return INDEX_NONE;
	}
}
