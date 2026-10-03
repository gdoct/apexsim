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
		if (C.IsWet())
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
		default:                      return 8;
		}
	}

	inline int32 WindKph(const FApexSessionConditions& C)
	{
		return C.HasWind() ? FMath::Min(C.WindKph, FApexSessionConditions::MaxWindKph) : AutoWindKph(C.Weather);
	}

	/** The road's grip under this weather, percent: `Weather::road_grip_factor`. */
	inline int32 GripPercent(EApexWeather Weather)
	{
		switch (Weather)
		{
		case EApexWeather::Overcast:  return 98;
		case EApexWeather::LightRain: return 86;
		case EApexWeather::HeavyRain: return 74;
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
