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
 * its own (`SessionConditions` in server/sim/src/data.rs) so the preview says
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
	// when the race counts in (`server/sim/src/grid_order.rs`): `@host`, `@ai:N`
	// (the Nth AI car, 1-based) or a driver's name, which is how a loaded
	// qualifying result names whoever set a lap. A driver the list leaves out
	// starts behind the listed ones. The create screen keeps the list
	// *normalised* once it is edited: the references that can match, each
	// once, then everyone the field holds that it left out, so what the
	// preview shows is what the server will seat.

	/** What the server reads as the host's own car. */
	inline const TCHAR* HostRef() { return TEXT("@host"); }

	inline FString AiRef(int32 Number) { return FString::Printf(TEXT("@ai:%d"), Number); }

	/** The driver whose profile has this id (`grid_order::driver_ref`), whatever name they join under. */
	inline FString DriverRef(const FString& DriverId) { return TEXT("@driver:") + DriverId; }

	/** The profile id a `@driver:` reference names, empty when it is not one. */
	inline FString DriverIdOf(const FString& Ref)
	{
		return Ref.StartsWith(TEXT("@driver:")) ? Ref.Mid(8) : FString();
	}

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
			if (bAi ? (AiNumberOf(Ref) >= 1 && AiNumberOf(Ref) <= Ai)
				: (Ref == HostRef() || !DriverIdOf(Ref).IsEmpty() || !Ref.StartsWith(TEXT("@"))))
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

	/**
	 * The occupied slots, first to last, for a normalised order. A `@driver:`
	 * slot is labelled with the name `DriverNames` (profile id -> name, as
	 * DriverNames() reads them off the stored results) has for it.
	 */
	inline TArray<FGridEntry> Entries(const TArray<FString>& Normalised, const TMap<FString, FString>& DriverNames = TMap<FString, FString>())
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
			else if (const FString Id = DriverIdOf(Ref); !Id.IsEmpty())
			{
				const FString* Name = DriverNames.Find(Id.ToLower());
				Entry.Label = Name ? *Name : FString(TEXT("DRIVER"));
			}
			else
			{
				Entry.Label = Ref;
			}
			Out.Add(Entry);
		}
		return Out;
	}

	/** Profile id (lower case) -> the name it qualified under, newest result first wins. */
	inline TMap<FString, FString> DriverNames(const TArray<FApexQualifyingResult>& Results)
	{
		TMap<FString, FString> Names;
		for (const FApexQualifyingResult& Result : Results)
		{
			for (const FApexQualifyingEntry& Entry : Result.Entries)
			{
				if (!Entry.DriverId.IsEmpty() && !Names.Contains(Entry.DriverId.ToLower()))
				{
					Names.Add(Entry.DriverId.ToLower(), Entry.Name);
				}
			}
		}
		return Names;
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
	 * with the host's profile id (`HostDriverId`), or for a line with no id
	 * the one named `HostName`, is the host; the result's AI take this
	 * session's AI in the order they qualified (the Nth quickest AI is
	 * `@ai:N`; the ones past this session's `Ai` are dropped), and any other
	 * human is referred to by profile id (`@driver:`), else by name, for the
	 * server to find if they join. Normalise it for the field after.
	 */
	inline TArray<FString> OrderFromResult(const FApexQualifyingResult& Result, const FString& HostName, int32 Ai,
		const FString& HostDriverId = FString())
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
			else if (!Entry.DriverId.IsEmpty()
				? Entry.DriverId.Equals(HostDriverId, ESearchCase::IgnoreCase)
				: !HostName.IsEmpty() && Entry.Name == HostName)
			{
				Out.AddUnique(HostRef());
			}
			else if (!Entry.DriverId.IsEmpty())
			{
				Out.AddUnique(DriverRef(Entry.DriverId.ToLower()));
			}
			else if (!Entry.Name.IsEmpty() && !Entry.Name.StartsWith(TEXT("@")))
			{
				Out.AddUnique(Entry.Name);
			}
		}
		return Out;
	}

	// --- The air, as the server resolves it ----------------------------------

	/** `data::WARMEST_DAY_OF_YEAR`: the air is warmest on 19 July in the north. */
	constexpr float WarmestDayOfYear = 200.0f;
	/** `data::SEASON_RANGE_PER_DEG_C` / `SEASON_RANGE_MAX_C`: the year's range of air by latitude. */
	constexpr float SeasonRangePerDegC = 0.35f;
	constexpr float SeasonRangeMaxC = 20.0f;
	/** `Weather::SNOW_AIR_DROP_C`: snow's own air under overcast's on the model day. */
	constexpr float SnowAirDropBaseC = 17.0f;

	/** `data::season_air_c`: the season's air on day `Day` over a circuit
	 * `LatitudeDeg` north against the year's mean, a cosine over the year
	 * warmest on WarmestDayOfYear in the north and half a year on in the south. */
	inline float SeasonAirC(int32 Day, float LatitudeDeg)
	{
		const float Days = static_cast<float>(FApexSessionConditions::DaysPerYear);
		const float Range = FMath::Min(SeasonRangePerDegC * FMath::Min(FMath::Abs(LatitudeDeg), 90.0f), SeasonRangeMaxC);
		const float Warmest = LatitudeDeg < 0.0f ? WarmestDayOfYear + 0.5f * Days : WarmestDayOfYear;
		const float Phase = (static_cast<float>(Day) - Warmest) / Days * UE_TWO_PI;
		return 0.5f * Range * FMath::Cos(Phase);
	}

	/**
	 * `climate::ClimateSite`: where a circuit is as its weather sees it, its
	 * latitude, its longitude when the track names one, its height. A bare
	 * latitude is a site on the sea with the reference's climate.
	 */
	struct FClimateSite
	{
		float LatitudeDeg = ApexSky::DefaultLatitudeDeg;
		bool bHasLongitude = false;
		float LongitudeDeg = 0.0f;
		float AltitudeM = 0.0f;

		FClimateSite() = default;
		FClimateSite(float InLatitudeDeg) : LatitudeDeg(InLatitudeDeg) {}
		FClimateSite(float InLatitudeDeg, float InLongitudeDeg, float InAltitudeM)
			: LatitudeDeg(InLatitudeDeg), bHasLongitude(true), LongitudeDeg(InLongitudeDeg), AltitudeM(InAltitudeM) {}
	};

	/** `climate::Climate`: a region's annual range over its latitude's, its mean over the latitude's, its wet season's share. */
	struct FClimate
	{
		float RangeFactor = 1.0f;
		float MeanOffsetC = 0.0f;
		float WetSeason = 1.0f;
	};

	/** `climate::EQUATOR_MEAN_C`, `MEAN_DROP_AT_50_C`, `LAPSE_C_PER_KM`. */
	constexpr float EquatorMeanC = 27.0f;
	constexpr float MeanDropAt50C = 17.0f;
	constexpr float LapseCPerKm = 5.0f;

	/** `climate::sea_level_mean_c`: the annual mean at sea level over a latitude. */
	inline float SeaLevelMeanC(float LatitudeDeg)
	{
		const float X = FMath::Min(FMath::Abs(LatitudeDeg), 90.0f) / 50.0f;
		return EquatorMeanC - MeanDropAt50C * X * X;
	}

	/** `climate::climate_of`: the coarse map of where racing happens; the reference's without a longitude. */
	inline FClimate ClimateOf(const FClimateSite& Site)
	{
		if (!Site.bHasLongitude || !FMath::IsFinite(Site.LongitudeDeg))
		{
			return FClimate();
		}
		const float Lat = Site.LatitudeDeg;
		const float Lon = Site.LongitudeDeg;
		auto Within = [](float Lo, float Hi, float V) { return V >= Lo && V < Hi; };
		auto Region = [](float Range, float Mean, float Wet) { return FClimate{Range, Mean, Wet}; };
		if ((Within(35.0f, 44.5f, Lat) && Within(-11.0f, -6.5f, Lon)) || (Within(44.5f, 62.0f, Lat) && Within(-11.0f, 7.0f, Lon)))
		{
			return Region(0.7f, 1.5f, 1.0f);   // the Atlantic edge of Europe
		}
		if (Within(35.0f, 44.5f, Lat) && Within(-6.5f, 26.0f, Lon))
		{
			return Region(1.15f, 0.0f, 1.0f);  // the Mediterranean
		}
		if (Within(44.5f, 62.0f, Lat) && Within(7.0f, 40.0f, Lon))
		{
			return Region(1.2f, 0.0f, 1.0f);   // central and eastern Europe
		}
		if (Within(12.0f, 32.0f, Lat) && Within(34.0f, 60.0f, Lon))
		{
			return Region(1.8f, 4.0f, 0.0f);   // the Gulf and Arabia
		}
		if (Within(32.0f, 45.0f, Lat) && Within(26.0f, 60.0f, Lon))
		{
			return Region(1.4f, -2.0f, 1.0f);  // Anatolia and the Caspian
		}
		if (Within(20.0f, 55.0f, Lat) && Within(100.0f, 146.0f, Lon))
		{
			return Region(1.8f, -3.0f, 1.0f);  // East Asia under the winter monsoon
		}
		if (Within(24.0f, 31.0f, Lat) && Within(-88.0f, -79.0f, Lon))
		{
			return Region(1.15f, 0.0f, 1.0f);  // Florida
		}
		if (Within(25.0f, 60.0f, Lat) && Within(-105.0f, -52.0f, Lon))
		{
			return Region(1.7f, -0.4f * FMath::Max(Lat - 30.0f, 0.0f), 1.0f);  // North America east of the Rockies
		}
		if (Within(30.0f, 50.0f, Lat) && Within(-125.0f, -117.0f, Lon))
		{
			return Region(0.7f, -2.0f, 1.0f);  // the Pacific coast
		}
		if (Within(30.0f, 50.0f, Lat) && Within(-117.0f, -105.0f, Lon))
		{
			return Region(1.8f, 0.0f, 1.0f);   // the interior west
		}
		if (FMath::Abs(Lat) < 25.0f)
		{
			return Region(0.6f, 0.0f, 1.0f);   // the rest of the tropics
		}
		if (Lat < -25.0f)
		{
			return Region(0.8f, 0.0f, 1.0f);   // the southern mid-latitudes
		}
		return FClimate();
	}

	/** `climate::season_c`: the season's air on `Day` against the year's mean, scaled by the climate's range. */
	inline float SeasonC(int32 Day, const FClimateSite& Site)
	{
		return ClimateOf(Site).RangeFactor * SeasonAirC(Day, Site.LatitudeDeg);
	}

	/** `climate::mean_air_c`: the latitude's sea-level mean, the region's offset, the season, the lapse with height. */
	inline float MeanAirC(int32 Day, const FClimateSite& Site)
	{
		return SeaLevelMeanC(Site.LatitudeDeg) + ClimateOf(Site).MeanOffsetC + SeasonC(Day, Site)
			- LapseCPerKm * FMath::Max(Site.AltitudeM, 0.0f) / 1000.0f;
	}

	/** `SessionConditions::climate_offset_c`: how much warmer the date at a
	 * site is than the reference day the weather's own air is filed at (the
	 * model day at 50 N on the sea); exactly 0 with no date. */
	inline float ClimateOffsetC(const FApexSessionConditions& C, const FClimateSite& Site)
	{
		if (!C.HasDate())
		{
			return 0.0f;
		}
		const int32 Day = FMath::Clamp(C.DayOfYear, 1, FApexSessionConditions::DaysPerYear);
		return MeanAirC(Day, Site) - MeanAirC(ApexSky::ModelDayOfYear, FClimateSite());
	}

	/** `SessionConditions::auto_air_temperature_c`: 13 °C before dawn to 23 °C
	 * two hours after solar noon (15:00 where noon is 13:00) on a clear day,
	 * the swing damped and the whole curve lowered under cloud and rain. The
	 * model day's: the season is `AutoAirTempCAt`'s. */
	inline float AutoAirTempC(const FApexSessionConditions& C)
	{
		constexpr float CoolestC = 13.0f;
		constexpr float SwingC = 10.0f;
		constexpr float WarmestAfterNoonHours = 2.0f;
		const float WarmestHours = ApexSky::SolarNoonOf(C) + WarmestAfterNoonHours;
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

	/** `SessionConditions::auto_air_temperature_c_at`: the model day's air
	 * moved to the date in the circuit's climate (ClimateOffsetC). Snow's
	 * own air is the same near freezing in any month and place. */
	inline float AutoAirTempCAt(const FApexSessionConditions& C, const FClimateSite& Site)
	{
		const float Own = AutoAirTempC(C);
		return C.IsSnow() ? Own : Own + ClimateOffsetC(C, Site);
	}

	/** `SessionConditions::snow_air_drop_c`: how far snow's air lies under
	 * overcast's on the date at the site (the snow line), never under 0. */
	inline float SnowAirDropC(const FApexSessionConditions& C, const FClimateSite& Site)
	{
		return FMath::Max(SnowAirDropBaseC + ClimateOffsetC(C, Site), 0.0f);
	}

	/** The air in whole degrees as the server resolves it over a circuit at
	 * `Site` (`resolve_at`): the host's figure, else the weather's on the
	 * date in the circuit's climate (Rust's `round`, half away from zero, as
	 * `RoundHalfFromZero`, held to the host's range). The site only matters
	 * with a date. */
	inline int32 AirTempC(const FApexSessionConditions& C, const FClimateSite& Site = FClimateSite())
	{
		return C.HasAirTemp()
			? FMath::Clamp(C.AirTempC, FApexSessionConditions::MinAirTempC, FApexSessionConditions::MaxAirTempC)
			: FMath::Clamp(static_cast<int32>(FMath::RoundHalfFromZero(AutoAirTempCAt(C, Site))),
				FApexSessionConditions::MinAirTempC, FApexSessionConditions::MaxAirTempC);
	}

	/** `SessionConditions::track_temperature_c_at`: the air plus up to 20 °C
	 * of sun through the cloud, under the circuit's sun on the session's
	 * date; a wet track a degree under the air. */
	inline float TrackTempC(const FApexSessionConditions& C, const FClimateSite& Site = FClimateSite())
	{
		const float LatitudeDeg = Site.LatitudeDeg;
		const float Air = static_cast<float>(AirTempC(C, Site));
		if (C.IsWet() || C.IsSnow())
		{
			return Air - 1.0f;
		}
		const float Elevation = ApexSky::SunAt(C.Hours(), LatitudeDeg, ApexSky::SolarNoonOf(C), ApexSky::DeclinationOf(C)).ElevationDeg;
		const float Sun = FMath::Max(0.0f, FMath::Sin(FMath::DegreesToRadians(Elevation)));
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
	 * (the air as the server resolves it over a circuit at `Site`:
	 * the host's figure, else the weather's in the date's season). A joined
	 * session's conditions name their air, so the latitude is the create
	 * screen's business only. */
	inline bool IsWinter(const FApexSessionConditions& In, const FClimateSite& Site = FClimateSite())
	{
		const FApexSessionConditions C = In.Clamped();
		return C.IsSnow() || (C.Weather == EApexWeather::Overcast && static_cast<float>(AirTempC(C, Site)) < WinterBelowC);
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
	inline TArray<FForecastChange> Forecast(const FApexSessionConditions& In, uint64 Seed, const FClimateSite& Site = FClimateSite())
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
		if (IsWinter(C, Site))
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

	// --- A forecast the host writes ----------------------------------------------
	//
	// A list of steps, each a session minute and a weather, that the server
	// follows instead of the one it lays out from the seed, whatever the
	// Weather changes row says (`HostForecast`, data.rs). Held to its rules
	// as it is edited: ascending, each step a change at least
	// ForecastMinGapMin after the last (and the start), none past the
	// horizon, at most MaxForecastSteps.

	/** A written forecast's steps as forecast changes, seconds of session. */
	inline TArray<FForecastChange> WrittenForecast(const TArray<FApexForecastStep>& Steps)
	{
		TArray<FForecastChange> Out;
		for (const FApexForecastStep& Step : Steps)
		{
			Out.Add({ static_cast<float>(Step.AtMin) * 60.0f, Step.Weather });
		}
		return Out;
	}

	/**
	 * The written forecast as the server will simulate and echo it
	 * (`resolve_at`): a winter sky's rain falls as snow, a summer sky's snow
	 * as light rain, then made valid again from the start's weather.
	 */
	inline TArray<FApexForecastStep> KeptToSky(const FApexSessionConditions& In, const FClimateSite& Site = FClimateSite())
	{
		const FApexSessionConditions C = In.Clamped();
		const bool bWinter = IsWinter(C, Site);
		TArray<FApexForecastStep> Mapped = C.Forecast;
		for (FApexForecastStep& Step : Mapped)
		{
			if (bWinter && (Step.Weather == EApexWeather::LightRain || Step.Weather == EApexWeather::HeavyRain))
			{
				Step.Weather = EApexWeather::Snow;
			}
			else if (!bWinter && Step.Weather == EApexWeather::Snow)
			{
				Step.Weather = EApexWeather::LightRain;
			}
		}
		return FApexSessionConditions::ClampedForecast(C.Weather, Mapped);
	}

	/**
	 * The forecast a session follows: the host's written one when it has
	 * one (`HasForecast`, kept to its sky), else the one laid out from
	 * `Seed` (empty when the sky holds).
	 */
	inline TArray<FForecastChange> SessionForecast(const FApexSessionConditions& C, uint64 Seed, const FClimateSite& Site = FClimateSite())
	{
		return C.HasForecast() ? WrittenForecast(KeptToSky(C, Site)) : Forecast(C, Seed, Site);
	}

	/** A new step's minute: ForecastMinGapMin's steps of 4, 30 after the last step (or the start). */
	constexpr int32 ForecastStepGapMin = 30;

	/** The minutes a step may be moved to: one gap after the step before it up to one gap before the next. */
	inline void StepBounds(const TArray<FApexForecastStep>& Steps, int32 Index, int32& OutLo, int32& OutHi)
	{
		const int32 Gap = FApexSessionConditions::ForecastMinGapMin;
		OutLo = (Steps.IsValidIndex(Index - 1) ? Steps[Index - 1].AtMin : 0) + Gap;
		OutHi = Steps.IsValidIndex(Index + 1) ? Steps[Index + 1].AtMin - Gap : FApexSessionConditions::ForecastHorizonMin;
	}

	/**
	 * Adds a step after the last: ForecastStepGapMin on (held to the
	 * horizon), the next weather on from the one before it so it is a
	 * change. False when the list is full or there is no room.
	 */
	inline bool AddForecastStep(TArray<FApexForecastStep>& Steps, EApexWeather Start)
	{
		if (Steps.Num() >= FApexSessionConditions::MaxForecastSteps)
		{
			return false;
		}
		const int32 Last = Steps.IsEmpty() ? 0 : Steps.Last().AtMin;
		const int32 At = FMath::Min(Last + ForecastStepGapMin, FApexSessionConditions::ForecastHorizonMin);
		if (At < Last + FApexSessionConditions::ForecastMinGapMin)
		{
			return false;
		}
		const EApexWeather Before = Steps.IsEmpty() ? Start : Steps.Last().Weather;
		FApexForecastStep Step;
		Step.AtMin = At;
		Step.Weather = static_cast<EApexWeather>((static_cast<int32>(Before) + 1) % FApexSessionConditions::WeatherCount);
		Steps.Add(Step);
		return true;
	}

	/** Moves step `Index` by `DeltaMin`, held between its neighbours. False when it cannot move. */
	inline bool MoveForecastStep(TArray<FApexForecastStep>& Steps, int32 Index, int32 DeltaMin)
	{
		if (!Steps.IsValidIndex(Index))
		{
			return false;
		}
		int32 Lo = 0;
		int32 Hi = 0;
		StepBounds(Steps, Index, Lo, Hi);
		const int32 At = FMath::Clamp(Steps[Index].AtMin + DeltaMin, Lo, FMath::Max(Lo, Hi));
		if (At == Steps[Index].AtMin)
		{
			return false;
		}
		Steps[Index].AtMin = At;
		return true;
	}

	/**
	 * Steps the weather of step `Index` on (+1) or back (-1), skipping the
	 * weather of the step before it (or the start) so it stays a change.
	 * A step after it left repeating the new weather is dropped.
	 */
	inline bool CycleForecastWeather(TArray<FApexForecastStep>& Steps, int32 Index, int32 Direction, EApexWeather Start)
	{
		if (!Steps.IsValidIndex(Index) || Direction == 0)
		{
			return false;
		}
		const EApexWeather Before = Index > 0 ? Steps[Index - 1].Weather : Start;
		const int32 Count = FApexSessionConditions::WeatherCount;
		int32 Weather = static_cast<int32>(Steps[Index].Weather);
		do
		{
			Weather = ((Weather + (Direction > 0 ? 1 : -1)) % Count + Count) % Count;
		}
		while (static_cast<EApexWeather>(Weather) == Before);
		Steps[Index].Weather = static_cast<EApexWeather>(Weather);
		if (Steps.IsValidIndex(Index + 1) && Steps[Index + 1].Weather == Steps[Index].Weather)
		{
			Steps.RemoveAt(Index + 1);
		}
		return true;
	}

	/** Removes step `Index`, and the step after it if that now repeats the weather before. */
	inline bool RemoveForecastStep(TArray<FApexForecastStep>& Steps, int32 Index, EApexWeather Start)
	{
		if (!Steps.IsValidIndex(Index))
		{
			return false;
		}
		Steps.RemoveAt(Index);
		const EApexWeather Before = Index > 0 ? Steps[Index - 1].Weather : Start;
		if (Steps.IsValidIndex(Index) && Steps[Index].Weather == Before)
		{
			Steps.RemoveAt(Index);
		}
		return true;
	}

	/** "+20 min", "+1 h 05": a step's minute of session. */
	inline FString StepMinuteLabel(int32 AtMin)
	{
		return AtMin < 60 ? FString::Printf(TEXT("+%d min"), AtMin)
			: FString::Printf(TEXT("+%d h %02d"), AtMin / 60, AtMin % 60);
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

	// --- The session's date ------------------------------------------------------

	/** The Date row's steps: the model day (-1, late May), then the middle of each month. */
	inline const TArray<int32>& DateChoices()
	{
		static const TArray<int32> Days = { -1, 15, 46, 74, 105, 135, 166, 196, 227, 258, 288, 319, 349 };
		return Days;
	}

	/** "15 Dec" for a day of the year, "Late May" for the model day (none set). */
	inline FString DateLabel(int32 DayOfYear)
	{
		if (DayOfYear < 1)
		{
			return TEXT("Late May");
		}
		static const int32 MonthStarts[] = { 1, 32, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335, 366 };
		static const TCHAR* Months[] = { TEXT("Jan"), TEXT("Feb"), TEXT("Mar"), TEXT("Apr"), TEXT("May"), TEXT("Jun"),
			TEXT("Jul"), TEXT("Aug"), TEXT("Sep"), TEXT("Oct"), TEXT("Nov"), TEXT("Dec") };
		const int32 Day = FMath::Clamp(DayOfYear, 1, FApexSessionConditions::DaysPerYear);
		int32 Month = 0;
		while (Month < 11 && Day >= MonthStarts[Month + 1])
		{
			++Month;
		}
		return FString::Printf(TEXT("%d %s"), Day - MonthStarts[Month] + 1, Months[Month]);
	}

	/** The next (Step +1) or previous (-1) Date step from `DayOfYear`, held at the ends. */
	inline int32 StepDate(int32 DayOfYear, int32 Step)
	{
		const TArray<int32>& Days = DateChoices();
		int32 Index = 0;
		for (int32 I = 0; I < Days.Num(); ++I)
		{
			if (Days[I] <= FMath::Max(DayOfYear, -1))
			{
				Index = I;
			}
		}
		return Days[FMath::Clamp(Index + Step, 0, Days.Num() - 1)];
	}

	// --- A race weekend's days ------------------------------------------------

	/** The hours a weekend day can start at, minutes after midnight; the picker's -1 is the race day's. */
	inline const TArray<int32>& DayHourChoices()
	{
		static const TArray<int32> Hours = { 7 * 60, 9 * 60, 11 * 60, 13 * 60, 15 * 60, 17 * 60, 19 * 60 };
		return Hours;
	}

	/** A weekend day: practice (0) or the qualifying day (1). */
	enum class EWeekendDay : uint8 { Practice = 0, Qualifying = 1 };

	/**
	 * A weekend day's own sky: the race's conditions under the day's weather
	 * (`Weather`, an EApexWeather, or -1 for the race's) and starting hour
	 * (`Minutes`, or -1 for the race's), its forecast from a seed of its own
	 * (the race's mixed with the day) so the forecast shown is the one the
	 * server lays out. False, and `Out` the race's, when the day picks
	 * neither: it then runs under the race day's sky.
	 */
	inline bool DaySky(const FApexSessionConditions& Race, int32 Weather, int32 Minutes, EWeekendDay Day, FApexSessionConditions& Out)
	{
		Out = Race.Clamped();
		const bool bWeather = Weather >= 0 && Weather < FApexSessionConditions::WeatherCount;
		const bool bHour = Minutes >= 0;
		if (!bWeather && !bHour)
		{
			return false;
		}
		if (bWeather)
		{
			Out.Weather = static_cast<EApexWeather>(Weather);
		}
		if (bHour)
		{
			Out.TimeOfDayMinutes = FMath::Clamp(Minutes, 0, 24 * 60 - 1);
		}
		if (Out.HasSkySeed())
		{
			Out.SkySeed = (Out.SkySeed ^ (0x9E3779B97F4A7C15ull * (static_cast<uint64>(Day) + 1))) * 0xBF58476D1CE4E5B9ull;
		}
		Out = Out.Clamped();
		return true;
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
	 * The sun stands as it will over the circuit (`LatitudeDeg`) on the
	 * session's date at its solar noon (`C.SolarNoonMin`, which the screen
	 * fills in from `ApexSky::SolarNoonMinutes`; 13:00 without one).
	 */
	inline FSkyLook Sky(const FApexSessionConditions& C, float LatitudeDeg = ApexSky::DefaultLatitudeDeg)
	{
		const FColor NightTop(11, 16, 32), NightBottom(26, 34, 56);
		const FColor DawnTop(42, 58, 102), DawnBottom(227, 138, 74);
		const FColor DayTop(61, 120, 196), DayBottom(169, 203, 230);

		const float Hours = C.Hours();
		const float NoonHours = ApexSky::SolarNoonOf(C);
		const float Declination = ApexSky::DeclinationOf(C);
		const float Elevation = ApexSky::SunAt(Hours, LatitudeDeg, NoonHours, Declination).ElevationDeg;

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

		// Half the sun's day, hours, from the sunrise hour angle: 7.71 on
		// the model day at 50 N (05:17 to 20:43 round a 13:00 noon).
		const float CosRise = -FMath::Tan(FMath::DegreesToRadians(FMath::Clamp(LatitudeDeg, -89.0f, 89.0f)))
			* FMath::Tan(FMath::DegreesToRadians(Declination));
		const float HalfDayHours = FMath::Max(FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(CosRise, -1.0f, 1.0f))) / 15.0f, 0.5f);
		Out.SunX = FMath::Clamp((Hours - (NoonHours - HalfDayHours)) / (2.0f * HalfDayHours), 0.0f, 1.0f);
		Out.SunHeight = Elevation / 62.0f;
		Out.SunOpacity = Elevation > -3.0f ? 1.0f - Weather.Cloud * 0.85f : 0.0f;
		Out.bLowSun = Elevation < 12.0f;
		Out.Phase = Elevation < -6.0f ? TEXT("NIGHT")
			: Elevation < 10.0f ? (Hours < NoonHours ? TEXT("DAWN") : TEXT("DUSK"))
			: TEXT("DAY");
		return Out;
	}

	// --- Qualifying formats -----------------------------------------------------

	/**
	 * A weekend's qualifying as the menus say it: "Q1-Q3 10 min each", "Q1-Q2
	 * 10 min · Q3 shoot-out", "Qualifying 10 min" or "Shoot-out" (a
	 * shoot-out has no clock, so the round it takes has no length).
	 */
	inline FString WeekendRoundsLabel(int32 Rounds, int32 RoundMinutes, bool bShootout)
	{
		const int32 N = FMath::Clamp(Rounds, 1, FApexWeekendFormat::MaxRounds);
		const FString Length = ApexRaceLength::Describe(RoundMinutes * 60);
		if (!bShootout)
		{
			return N > 1 ? FString::Printf(TEXT("Q1-Q%d %s each"), N, *Length) : FString::Printf(TEXT("Qualifying %s"), *Length);
		}
		if (N == 1)
		{
			return TEXT("Shoot-out");
		}
		const FString Timed = N > 2 ? FString::Printf(TEXT("Q1-Q%d %s"), N - 1, *Length) : FString::Printf(TEXT("Q1 %s"), *Length);
		return FString::Printf(TEXT("%s · Q%d shoot-out"), *Timed, N);
	}

	/** The lap allowance's stepper: None, then 1 to 30, held at the ends. */
	inline int32 StepLapAllowance(int32 Laps, int32 Step)
	{
		return ApexQualifyingLapAllowance::Clamp(ApexQualifyingLapAllowance::Clamp(Laps) + Step);
	}

	/** "None", "1 lap", "3 laps". */
	inline FString LapAllowanceLabel(int32 Laps)
	{
		const int32 N = ApexQualifyingLapAllowance::Clamp(Laps);
		return N == ApexQualifyingLapAllowance::None ? FString(TEXT("None"))
			: N == 1 ? FString(TEXT("1 lap")) : FString::Printf(TEXT("%d laps"), N);
	}

	/** A weekend phase's damage chips: the race's (-1), then each EApexDamageLevel. */
	inline constexpr int32 PhaseDamageChips = 5;

	inline const TCHAR* PhaseDamageChipLabel(int32 Chip)
	{
		static const TCHAR* Labels[PhaseDamageChips] = { TEXT("Race's"), TEXT("Off"), TEXT("Reduced"), TEXT("Full"), TEXT("Visual") };
		return Labels[FMath::Clamp(Chip, 0, PhaseDamageChips - 1)];
	}

	/** The chip for a phase's pick (-1 the race's, else a level) and back. */
	inline int32 PhaseDamageChip(int32 Damage) { return FMath::Clamp(Damage, -1, PhaseDamageChips - 2) + 1; }
	inline int32 PhaseDamageOfChip(int32 Chip) { return FMath::Clamp(Chip, 0, PhaseDamageChips - 1) - 1; }

	// --- Race control ------------------------------------------------------------

	/** The Reverse grid row's chips: off, the top 6, 8 or 10, or the whole field. */
	inline constexpr int32 ReverseGridChoices[] = { 0, 6, 8, 10, FApexRaceRules::ReverseWholeField };

	/** The chip a reverse grid shows on; a count the row does not offer shows on the next larger one. */
	inline int32 ReverseGridChip(int32 Reverse)
	{
		if (Reverse <= 1)
		{
			return 0;
		}
		for (int32 Chip = 1; Chip < UE_ARRAY_COUNT(ReverseGridChoices); ++Chip)
		{
			if (Reverse <= ReverseGridChoices[Chip])
			{
				return Chip;
			}
		}
		return static_cast<int32>(UE_ARRAY_COUNT(ReverseGridChoices)) - 1;
	}

	// --- Tyres, parc fermé and stints --------------------------------------------

	/** Qualifying sets: none, or 1 to 4 new sets. */
	inline constexpr int32 QualifyingTyreSetChoices[] = { 0, 1, 2, 3, 4 };
	/** Race on qualifying tyres: off, the top 10, every car with a time. */
	inline constexpr int32 QualifyingTyreStartChoices[] = { 0, 10, FApexRaceRules::QualifyingTyreStartAll };
	/** A weekend's sets: none, or 6 to 13 for practice and qualifying together. */
	inline constexpr int32 WeekendTyreSetChoices[] = { 0, 6, 8, 10, 13 };
	/** Stint lengths: none, or so many laps; none, or so many minutes. */
	inline constexpr int32 StintLapChoices[] = { 0, 10, 15, 20, 30 };
	inline constexpr int32 StintMinuteChoices[] = { 0, 20, 30, 45, 60 };

	/**
	 * The chip a value shows on in a row of choices whose first is "none"
	 * (0): 0 on the first, else the first choice at least the value, a value
	 * past the last on the last.
	 */
	template <int32 N>
	inline int32 ChoiceChip(const int32 (&Choices)[N], int32 Value)
	{
		if (Value <= 0)
		{
			return 0;
		}
		for (int32 Chip = 1; Chip < N; ++Chip)
		{
			if (Value <= Choices[Chip])
			{
				return Chip;
			}
		}
		return N - 1;
	}

	/** The choice under a chip, the last for one past the end. */
	template <int32 N>
	inline int32 ChoiceOfChip(const int32 (&Choices)[N], int32 Chip)
	{
		return Choices[FMath::Clamp(Chip, 0, N - 1)];
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

	/** "PRO ASSISTS" for a preset's set, "3/5 ASSISTS" for any other. */
	inline FString AssistsSummary(const FApexAllowedAssists& Assists)
	{
		const int32 Preset = MatchingPreset(Assists);
		return Preset != INDEX_NONE
			? FString::Printf(TEXT("%s ASSISTS"), *FString(PresetName(static_cast<EAssistPreset>(Preset))).ToUpper())
			: FString::Printf(TEXT("%d/%d ASSISTS"), FApexAllowedAssists::Count - Assists.CountLocked(), FApexAllowedAssists::Count);
	}

	/** A weekend phase's assist chips: the race's (-1), then each EAssistPreset. */
	inline constexpr int32 PhaseAssistChips = 1 + static_cast<int32>(EAssistPreset::Count);

	inline FString PhaseAssistChipLabel(int32 Chip)
	{
		return Chip <= 0 ? FString(TEXT("Race's"))
			: FString(PresetName(static_cast<EAssistPreset>(FMath::Min(Chip, PhaseAssistChips - 1) - 1)));
	}

	/** The chip for a phase's pick (-1 the race's, else a preset) and back. */
	inline int32 PhaseAssistChip(int32 Pick) { return FMath::Clamp(Pick, -1, PhaseAssistChips - 2) + 1; }
	inline int32 PhaseAssistOfChip(int32 Chip) { return FMath::Clamp(Chip, 0, PhaseAssistChips - 1) - 1; }

	/** ", PRO ASSISTS" after a weekend phase in a lobby line when it has aids of its own, else nothing. */
	inline FString PhaseAssistsNote(bool bHasOwn, const FApexAllowedAssists& Assists)
	{
		return bHasOwn ? FString::Printf(TEXT(", %s"), *AssistsSummary(Assists)) : FString();
	}

	/** The aids a phase's pick allows; everything for the race's (unused: the format leaves it off). */
	inline FApexAllowedAssists PhaseAssists(int32 Pick)
	{
		return Pick < 0 ? FApexAllowedAssists()
			: AssistPreset(static_cast<EAssistPreset>(FMath::Min(Pick, PhaseAssistChips - 2)));
	}
}
