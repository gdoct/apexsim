#pragma once

#include "CoreMinimal.h"
#include "ApexProtocolTypes.h"

/**
 * The sky a session races under, worked out from its conditions.
 *
 * Pure maths over `FApexSessionConditions`: where the sun is at that hour,
 * how bright and how warm it is, how much the cloud takes off it, how the
 * fog, the exposure and the road change, and which lights the circuit and
 * the cars need. `AApexRaceDirector::ApplyRaceEnvironment` turns the result
 * into light, fog and post-process settings; nothing here touches an actor,
 * so `ApexSim.Sky.*` can pin the numbers.
 *
 * The day is the session's date (`DayOfYear`), else a late-spring model
 * day where, at the default 50° N, the sun is up from a little after six
 * to a little after nine in the evening. Solar noon is the server's for the
 * circuit (`SolarNoonMin`, from its longitude and time zone), else 13:00,
 * what a clock on summer time says. Where the circuit is comes from its export (`FSkySite`: the
 * manifest's `latitude_deg` and `north_yaw_deg`); a track exported before
 * those keys is at 50° N with true north down the frame's +X, which is
 * how every circuit was lit until then. The server's `sun_elevation_deg`
 * (data.rs) is `SunAt` to the letter, so its idea of dusk (the headlights,
 * the asphalt's heat) is the one on screen.
 */
namespace ApexSky
{
	/** Latitude the sun is computed for when a track does not say, degrees north. */
	inline constexpr float DefaultLatitudeDeg = 50.0f;
	/** Solar declination, degrees: a day in late May. */
	inline constexpr float DeclinationDeg = 20.0f;
	/** Hour of solar noon, local clock (summer time). */
	inline constexpr float SolarNoonHours = 13.0f;
	/** Full sun, lux, at the zenith. */
	inline constexpr float ZenithLux = 100000.0f;
	/** Below this elevation the sun is gone and the moon takes the light. */
	inline constexpr float MoonBelowElevationDeg = -8.0f;
	/** The road family's roughness, baked dry and at the full wet sheen. */
	inline constexpr float DryRoadRoughness = 0.9f;
	inline constexpr float WetRoadRoughness = 0.3f;
	/** The road's RoughnessNoise at the full wet sheen. */
	inline constexpr float WetRoadRoughnessNoise = 0.08f;

	/**
	 * Where on Earth the circuit is, as far as the sky cares: its latitude,
	 * and which way true north lies in the track's (server) frame, degrees
	 * counter-clockwise from +X. Unreal's world is that frame with Y flipped,
	 * so north is at Unreal yaw `-NorthYawDeg`.
	 */
	struct FSkySite
	{
		float LatitudeDeg = DefaultLatitudeDeg;
		float NorthYawDeg = 0.0f;

		/** Unreal world yaw of a compass bearing (degrees clockwise from
		 * north): both turn clockwise seen from above, so it is the bearing
		 * less north's counter-clockwise server yaw. */
		float WorldYawOfBearing(float BearingDeg) const { return BearingDeg - NorthYawDeg; }
	};

	struct FSunPosition
	{
		/** Degrees above the horizon; negative at night. */
		float ElevationDeg = 0.0f;
		/** Compass azimuth, degrees clockwise from true north. */
		float AzimuthDeg = 180.0f;
	};

	/** The sun's declination on day `Day` of the year, degrees (Cooper's
	 * formula): the server's `data::sun_declination_deg` to the letter. */
	inline float DeclinationOn(int32 Day)
	{
		const float N = static_cast<float>(FMath::Clamp(Day, 1, FApexSessionConditions::DaysPerYear));
		return 23.44f * FMath::Sin((284.0f + N) / static_cast<float>(FApexSessionConditions::DaysPerYear) * UE_TWO_PI);
	}

	/** The session's solar noon, hours on its clock: the server's
	 * `solar_noon_min` from where the circuit is, else 13:00. */
	inline float SolarNoonOf(const FApexSessionConditions& C)
	{
		return C.HasSolarNoon()
			? static_cast<float>(C.SolarNoonMin % FApexSessionConditions::MinutesPerDay) / 60.0f
			: SolarNoonHours;
	}

	/** The session's declination: its date's, else the model day's. */
	inline float DeclinationOf(const FApexSessionConditions& C)
	{
		return C.HasDate() ? DeclinationOn(C.DayOfYear) : DeclinationDeg;
	}

	/** The model day of the year (late May) a session with no date runs on: the server's `MODEL_DAY_OF_YEAR`. */
	inline constexpr int32 ModelDayOfYear = 145;

	/** The equation of time on day `Day`, minutes: the server's `equation_of_time_min` to the letter. */
	inline float EquationOfTimeMin(int32 Day)
	{
		const float B = (static_cast<float>(FMath::Clamp(Day, 1, FApexSessionConditions::DaysPerYear)) - 81.0f) / 364.0f * UE_TWO_PI;
		return 9.87f * FMath::Sin(2.0f * B) - 7.53f * FMath::Cos(B) - 1.5f * FMath::Sin(B);
	}

	/**
	 * Solar noon on a circuit's legal clock, minutes after midnight, on day
	 * `Day`: the server's `solar_noon_minutes` and its coarse map of time
	 * zones and summer time (`legal_clock`), ported line for line so the
	 * create screen's preview stands the sun where the session will.
	 */
	inline int32 SolarNoonMinutes(float LatitudeDeg, float LongitudeDeg, int32 Day)
	{
		const float Lat = LatitudeDeg;
		const float Lon = LongitudeDeg;
		auto Within = [Lat, Lon](float LatLo, float LatHi, float LonLo, float LonHi)
		{
			return Lat >= LatLo && Lat < LatHi && Lon >= LonLo && Lon < LonHi;
		};
		enum class ESummer : uint8 { None, Europe, America, South };
		float ZoneH = FMath::RoundHalfFromZero(Lon / 15.0f);
		ESummer Summer = ESummer::None;
		if (Within(49.8f, 61.0f, -11.0f, 1.8f) || Within(36.8f, 42.2f, -9.6f, -6.2f))
		{
			ZoneH = 0.0f; Summer = ESummer::Europe;
		}
		else if (Within(35.0f, 71.0f, -9.6f, 24.0f))
		{
			ZoneH = 1.0f; Summer = ESummer::Europe;
		}
		else if (Within(34.5f, 71.0f, 24.0f, 30.0f))
		{
			ZoneH = 2.0f; Summer = ESummer::Europe;
		}
		else if (Within(-2.0f, 8.0f, 99.0f, 120.0f))
		{
			ZoneH = 8.0f;
		}
		else if (Within(14.0f, 25.8f, -118.0f, -86.0f))
		{
			ZoneH = -6.0f;
		}
		else if (Within(-56.0f, -21.0f, -74.0f, -53.0f))
		{
			ZoneH = -3.0f;
		}
		else if (Within(25.0f, 72.0f, -170.0f, -50.0f))
		{
			ZoneH = Lon >= -67.0f && Lat >= 43.0f ? -4.0f
				: Lon >= -87.5f ? -5.0f
				: Lon >= -102.0f ? -6.0f
				: Lon >= -114.0f ? -7.0f
				: -8.0f;
			Summer = ESummer::America;
		}
		else if (Within(-44.0f, -10.0f, 113.0f, 154.0f))
		{
			ZoneH = Lon >= 141.0f ? 10.0f : Lon >= 129.0f ? 9.5f : 8.0f;
			Summer = Lat < -28.2f && Lon >= 129.0f ? ESummer::South : ESummer::None;
		}
		const int32 D = FMath::Clamp(Day, 1, FApexSessionConditions::DaysPerYear);
		const bool bSummer = Summer == ESummer::Europe ? (D >= 88 && D < 300)
			: Summer == ESummer::America ? (D >= 69 && D < 307)
			: Summer == ESummer::South ? !(D >= 95 && D < 278)
			: false;
		const float Hours = 12.0f + ZoneH + (bSummer ? 1.0f : 0.0f) - Lon / 15.0f - EquationOfTimeMin(D) / 60.0f;
		const int32 Minutes = FMath::RoundHalfFromZero(Hours * 60.0f);
		return ((Minutes % FApexSessionConditions::MinutesPerDay) + FApexSessionConditions::MinutesPerDay) % FApexSessionConditions::MinutesPerDay;
	}

	/** Where the sun is at `Hours` after midnight at `LatitudeDeg` north
	 * (south negative), the sun at its height at `NoonHours` on a day of
	 * `DecDeg` declination (the model day's by default). */
	inline FSunPosition SunAt(float Hours, float LatitudeDeg = DefaultLatitudeDeg,
		float NoonHours = SolarNoonHours, float DecDeg = DeclinationDeg)
	{
		const float Lat = FMath::DegreesToRadians(FMath::Clamp(LatitudeDeg, -89.0f, 89.0f));
		const float Dec = FMath::DegreesToRadians(DecDeg);
		const float HourAngle = FMath::DegreesToRadians((Hours - NoonHours) * 15.0f);

		const float SinElev = FMath::Sin(Lat) * FMath::Sin(Dec) + FMath::Cos(Lat) * FMath::Cos(Dec) * FMath::Cos(HourAngle);
		const float Elev = FMath::Asin(FMath::Clamp(SinElev, -1.0f, 1.0f));

		// Azimuth from north, east positive: the standard formula with the
		// sign taken from the hour angle (afternoon is west).
		const float CosAz = (FMath::Sin(Dec) - FMath::Sin(Elev) * FMath::Sin(Lat)) / FMath::Max(1e-4f, FMath::Cos(Elev) * FMath::Cos(Lat));
		float Az = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(CosAz, -1.0f, 1.0f)));
		if (HourAngle > 0.0f)
		{
			Az = 360.0f - Az;
		}

		FSunPosition Out;
		Out.ElevationDeg = FMath::RadiansToDegrees(Elev);
		Out.AzimuthDeg = Az;
		return Out;
	}

	/** Everything the director sets, from one set of conditions. */
	struct FSkyState
	{
		FSunPosition Sun;
		/** Whether the directional light is the sun or, at night, the moon. */
		bool bMoon = false;
		/** Rotation for the directional light (it shines along its +X). */
		FRotator LightRotation = FRotator::ZeroRotator;
		/** Lux. */
		float LightIntensity = 50000.0f;
		FLinearColor LightColor = FLinearColor::White;
		/** Whether the light drives the atmosphere (the moon does not). */
		bool bAtmosphereLight = true;
		bool bCastShadows = true;
		/** Degrees; a wide source is what cloud does to shadows. */
		float LightSourceAngleDeg = 0.5f;
		/** Multiplier on the sky light's real-time capture. */
		float SkyLightIntensity = 1.0f;

		/** Exponential height fog. */
		float FogDensity = 0.0015f;
		float FogHeightFalloff = 0.2f;
		float FogStartDistanceCm = 3000.0f;
		FLinearColor FogColor = FLinearColor(0.45f, 0.52f, 0.62f);

		/** Auto-exposure clamp and bias, EV100. */
		float ExposureMinEv = 10.0f;
		float ExposureMaxEv = 15.0f;
		float ExposureBias = -0.4f;
		/** Colour grading. */
		float Saturation = 1.0f;
		float Contrast = 1.0f;

		/** 0 dry .. 1 a downpour. */
		float RainIntensity = 0.0f;
		/** 0 none .. 1 steady snowfall: the flakes in the air. */
		float SnowIntensity = 0.0f;
		/** 0 bare .. 1 white: how far the ground has gone to snow. A fixed
		 * snow sky is fully covered; a live one follows the server's cover. */
		float SnowCover = 0.0f;
		/** Cloud cover, 0..1 (`Weather::cloud`: cloudy 0.5, overcast 0.85; a
		 * live sky's is the server's eased figure): what the cloud dome draws. */
		float CloudCover = 0.0f;
		/** The cloud's colour as lit now, and how bright, cd/m². */
		FLinearColor CloudColor = FLinearColor(0.7f, 0.72f, 0.76f);
		float CloudBrightness = 0.0f;
		/**
		 * The thin high cloud over a fair sky, 0..1: a cirrus veil the dome
		 * draws far above the cumulus (sunny 0.7, cloudy 0.35; the sheet
		 * hides it from overcast on), so a clear day is not a flat blue.
		 */
		float CirrusVeil = 0.0f;
		/** The road's material roughness; 0.9 is the baked dry value. */
		float RoadRoughness = 0.9f;
		/** How far the road has gone from its baked dry look toward the wet
		 * sheen, 0..1: the director lerps each road material's own roughness
		 * toward WetRoadRoughness by it. */
		float RoadWetness = 0.0f;
		bool bWetRoad = false;

		bool bHeadlights = false;
		bool bFloodlights = false;
		/** Emissive strength of the floodlight lamp faces (nits). */
		float LampGlow = 0.0f;
		/**
		 * 0 by day, 1 at night, smooth through twilight: how far the lit windows,
		 * the wheel's light strips and the lit panels have come on
		 * (`ApexProps::NightGlowOf`).
		 */
		float WindowGlow = 0.0f;
	};

	/** 0 at full night, 1 in full day, smooth through twilight. */
	/**
	 * How bright the haze a far hill sinks into is, against the cloud the sky
	 * lights now: the horizon sky is a little brighter than a lit cloud.
	 */
	inline constexpr float HazeGain = 1.4f;

	/**
	 * The height fog's inscattering as a luminance: the fog's colour lit as
	 * the sky lights the clouds. A bare colour (about 0.5) is near-black at
	 * daylight exposure, so far land sank into a dark blue band read as sea.
	 */
	inline FLinearColor FogLuminance(const FSkyState& S)
	{
		return S.FogColor * (FMath::Max(S.CloudBrightness, 0.05f) * HazeGain);
	}

	/**
	 * The height fog's density for a meteorological visibility, m. The
	 * engine divides the density by 1000 into a per-centimetre figure and
	 * fades by powers of two, so its extinction is density x ln 2 / 10 per
	 * metre; Koschmieder's visibility is 3.912 / extinction. A density of
	 * 0.0015 is 37 km of air, the clearest of days, where 3.9 / visibility
	 * read straight as a density drew a 1.6 km dawn mist as that.
	 */
	inline float FogDensityForVisibility(float VisibilityM)
	{
		constexpr float DensityPerExtinction = 10.0f / UE_LN2;
		return 3.912f / FMath::Max(VisibilityM, 1.0f) * DensityPerExtinction;
	}

	/**
	 * Each weather's own haze as the visibility it leaves, m: a fair day's
	 * 8 km softens a circuit's far end and its horizon (a far grandstand at
	 * a kilometre loses about a third of its contrast); rain and snow close
	 * it in.
	 */
	inline float HazeVisibilityM(EApexWeather Weather)
	{
		switch (Weather)
		{
		case EApexWeather::Sunny:     return 8000.0f;
		case EApexWeather::Cloudy:    return 6000.0f;
		case EApexWeather::Overcast:  return 4500.0f;
		case EApexWeather::LightRain: return 3500.0f;
		case EApexWeather::HeavyRain: return 2200.0f;
		case EApexWeather::Snow:      return 2800.0f;
		default:                      return 8000.0f;
		}
	}

	/** The thin high cloud each weather carries (`FSkyState::CirrusVeil`). */
	inline float CirrusVeilOf(EApexWeather Weather)
	{
		switch (Weather)
		{
		case EApexWeather::Sunny:  return 0.7f;
		case EApexWeather::Cloudy: return 0.35f;
		default:                   return 0.0f;
		}
	}

	inline float Daylight(float ElevationDeg)
	{
		return FMath::SmoothStep(-6.0f, 10.0f, ElevationDeg);
	}

	/**
	 * The night pass's level for a daylight figure: lit from the golden hour
	 * (daylight under 0.5) to full at dark, exactly 0 in full day.
	 */
	inline float WindowGlowAt(float Day)
	{
		return 1.0f - FMath::SmoothStep(0.0f, 0.5f, FMath::Clamp(Day, 0.0f, 1.0f));
	}

	/** `Weather::cloud`: the cover each weather settles at. */
	inline float CloudCoverOf(EApexWeather Weather)
	{
		switch (Weather)
		{
		case EApexWeather::Sunny:     return 0.0f;
		case EApexWeather::Cloudy:    return 0.5f;
		case EApexWeather::Overcast:  return 0.85f;
		case EApexWeather::LightRain: return 0.9f;
		case EApexWeather::HeavyRain: return 1.0f;
		case EApexWeather::Snow:      return 1.0f;
		default:                      return 0.0f;
		}
	}

	/**
	 * What a cloud is lit to by the sun at `ElevationDeg`, cd/m²: a few
	 * thousand under a high sun, down with the airmass toward the horizon,
	 * near nothing at night. Scattered cloud is lit white; a sheet is darker
	 * and bluer, and the dimmer the thicker.
	 */
	inline void CloudLook(float ElevationDeg, float Cover, FLinearColor& OutColor, float& OutBrightness)
	{
		const float Day = Daylight(ElevationDeg);
		const float Sun = FMath::Max(0.15f, FMath::Sin(FMath::DegreesToRadians(FMath::Max(ElevationDeg, 0.0f))));
		OutBrightness = FMath::Lerp(0.02f, 4500.0f * Sun, Day);
		OutColor = FMath::Lerp(FLinearColor(0.93f, 0.94f, 0.96f), FLinearColor(0.66f, 0.68f, 0.72f), FMath::Clamp(Cover, 0.0f, 1.0f));
	}

	/** Sunlight through the cloud: what is left of the direct beam. */
	inline float DirectSunFactor(EApexWeather Weather)
	{
		switch (Weather)
		{
		case EApexWeather::Sunny:     return 1.0f;
		case EApexWeather::Cloudy:    return 0.65f;
		case EApexWeather::Overcast:  return 0.22f;
		case EApexWeather::LightRain: return 0.14f;
		case EApexWeather::HeavyRain: return 0.08f;
		case EApexWeather::Snow:      return 0.2f;
		default:                      return 1.0f;
		}
	}

	inline FSkyState Derive(const FApexSessionConditions& Conditions, const FSkySite& Site = FSkySite())
	{
		FSkyState S;
		const FApexSessionConditions C = Conditions.Clamped();
		S.Sun = SunAt(C.Hours(), Site.LatitudeDeg, SolarNoonOf(C), DeclinationOf(C));
		// The light's yaw is in the world; the sun's azimuth is from true north.
		const float SunYawDeg = Site.WorldYawOfBearing(S.Sun.AzimuthDeg);
		const float Elev = S.Sun.ElevationDeg;
		const float Day = Daylight(Elev);
		const float Direct = DirectSunFactor(C.Weather);
		const bool bOvercast = C.Weather == EApexWeather::Overcast || C.IsWet() || C.IsSnow();
		const bool bFalling = C.IsWet() || C.IsSnow();

		S.bMoon = Elev < MoonBelowElevationDeg;
		if (S.bMoon)
		{
			// The moon stands where the sun is not: opposite it, well up. The
			// light shines away from its source, so its yaw is the sun's azimuth.
			S.LightRotation = FRotator(-35.0f, SunYawDeg, 0.0f);
			S.LightIntensity = 1.0f * FMath::Lerp(1.0f, 0.35f, 1.0f - Direct);
			S.LightColor = FLinearColor(0.62f, 0.72f, 1.0f);
			S.bAtmosphereLight = false;
			S.bCastShadows = !bOvercast;
			S.LightSourceAngleDeg = 0.5f;
		}
		else
		{
			// The light shines along its +X: yaw it to point away from the
			// sun's azimuth, pitch it down by the elevation. A sun still under
			// the horizon keeps aiming the atmosphere, for the twilight.
			S.LightRotation = FRotator(-FMath::Max(Elev, 0.5f), SunYawDeg + 180.0f, 0.0f);
			// Airmass takes the beam down toward the horizon; the atmosphere
			// colours it, this only dims it.
			const float Airmass = FMath::Pow(FMath::Max(0.0f, FMath::Sin(FMath::DegreesToRadians(FMath::Max(Elev, 0.0f)))), 0.6f);
			S.LightIntensity = ZenithLux * FMath::Max(Airmass, 0.02f) * Direct;
			// Warm at the horizon, white overhead, and grey when the cloud
			// has eaten the disc.
			const float Warmth = 1.0f - FMath::Clamp(Elev / 25.0f, 0.0f, 1.0f);
			const FLinearColor Warm = FLinearColor::LerpUsingHSV(FLinearColor(1.0f, 0.93f, 0.84f), FLinearColor(1.0f, 0.62f, 0.32f), Warmth);
			const FLinearColor Grey(0.86f, 0.88f, 0.92f);
			S.LightColor = FLinearColor::LerpUsingHSV(Warm, Grey, 1.0f - Direct);
			S.bAtmosphereLight = true;
			// Cloudy keeps a soft shadow; overcast and rain have none.
			S.bCastShadows = Direct > 0.3f;
			S.LightSourceAngleDeg = FMath::Lerp(0.5f, 6.0f, 1.0f - Direct);
		}

		// Cloud makes the sky itself the lamp: the capture follows the dimmer
		// sun, so the ambient is scaled back up to a flat, bright overcast.
		S.SkyLightIntensity = FMath::Lerp(1.0f, 2.6f, 1.0f - Direct);
		// The cloud itself, for the dome.
		S.CloudCover = CloudCoverOf(C.Weather);
		S.CirrusVeil = CirrusVeilOf(C.Weather);
		CloudLook(Elev, S.CloudCover, S.CloudColor, S.CloudBrightness);

		// Fog: thicker with the weather, its colour the sky's, dark at night.
		S.FogDensity = FogDensityForVisibility(HazeVisibilityM(C.Weather));
		switch (C.Weather)
		{
		case EApexWeather::Sunny:     S.FogColor = FLinearColor(0.45f, 0.52f, 0.62f); break;
		case EApexWeather::Cloudy:    S.FogColor = FLinearColor(0.50f, 0.54f, 0.60f); break;
		case EApexWeather::Overcast:  S.FogColor = FLinearColor(0.52f, 0.54f, 0.57f); break;
		case EApexWeather::LightRain: S.FogColor = FLinearColor(0.46f, 0.48f, 0.51f); break;
		case EApexWeather::HeavyRain: S.FogColor = FLinearColor(0.40f, 0.42f, 0.45f); break;
		// A white, cool haze, between light and heavy rain's.
		case EApexWeather::Snow:      S.FogColor = FLinearColor(0.60f, 0.64f, 0.70f); break;
		default: break;
		}
		S.FogStartDistanceCm = bFalling ? 1000.0f : 3000.0f;
		S.FogHeightFalloff = 0.2f;
		S.FogColor *= FMath::Lerp(0.012f, 1.0f, Day);

		// Exposure: the baked daylight clamp (10..15 EV) would hold a night
		// scene black, so the floor follows the light that is there.
		const float NightMin = -2.0f;
		const float DayMin = bOvercast ? 8.0f : 10.0f;
		S.ExposureMinEv = FMath::Lerp(NightMin, DayMin, Day);
		S.ExposureMaxEv = FMath::Lerp(8.0f, 15.0f, Day);
		// Snow fills the frame with a bright, flat white that the metering
		// would take for the whole scene's key: the bias comes up from rain's
		// and the colour goes out of the scene.
		S.ExposureBias = -0.4f + (C.IsWet() ? -0.15f : 0.0f) + (C.IsSnow() ? 0.25f : 0.0f);
		S.Saturation = C.IsSnow() ? 0.7f : C.IsWet() ? 0.82f : (bOvercast ? 0.9f : 1.0f);
		S.Contrast = bFalling ? 0.96f : 1.0f;

		S.RainIntensity = C.Weather == EApexWeather::LightRain ? 0.4f : (C.Weather == EApexWeather::HeavyRain ? 1.0f : 0.0f);
		S.SnowIntensity = C.IsSnow() ? 1.0f : 0.0f;
		S.SnowCover = S.SnowIntensity;
		// The slush on a snowy road is the server's 0.3 water: a wet sheen
		// of 0.6 (`DeriveLive` reads it off the road water).
		S.bWetRoad = C.IsWet() || C.IsSnow();
		S.RoadWetness = C.IsWet() ? 1.0f : (C.IsSnow() ? 0.6f : 0.0f);
		S.RoadRoughness = FMath::Lerp(DryRoadRoughness, WetRoadRoughness, S.RoadWetness);

		// Lights come on as the sun goes, and in the rain as they do on a race day.
		S.bHeadlights = Elev < 6.0f || bFalling;
		S.bFloodlights = Elev < 4.0f;
		S.LampGlow = S.bFloodlights ? FMath::Lerp(60.0f, 6.0f, Day) : 0.0f;
		S.WindowGlow = WindowGlowAt(Day);
		return S;
	}

	/**
	 * Where a live sky stands on the weather ladder (0 sunny, 1 cloudy, 2
	 * overcast, 3 light rain, 4 heavy rain), from the cloud and the rain
	 * the server eases in rather than the forecast's step: the cloud carries
	 * it to overcast at the server's own covers (`Weather::cloud`: 0, 0.5,
	 * 0.85), the rain on from there by its water units (`Weather::water`: a
	 * light rain is half a heavy one). Each weather's own figures land on
	 * its rung exactly, so a sky that holds is lit as it always was.
	 */
	inline float WeatherLadder(float Cloud01, float Rain01)
	{
		const float Cloud = FMath::Clamp(Cloud01, 0.0f, 1.0f);
		const float CloudRung = Cloud <= 0.5f ? Cloud / 0.5f
			: Cloud <= 0.85f ? 1.0f + (Cloud - 0.5f) / 0.35f
			: 2.0f;
		return FMath::Clamp(CloudRung + 2.0f * FMath::Clamp(Rain01, 0.0f, 1.0f), 0.0f, 4.0f);
	}

	/** Two skies at the same clock, `T` of the way from `A` to `B`: every
	 * figure lerped, every switch from the nearer. T = 0 is A to the bit. */
	inline FSkyState Blend(const FSkyState& A, const FSkyState& B, float T)
	{
		if (T <= 0.0f)
		{
			return A;
		}
		if (T >= 1.0f)
		{
			return B;
		}
		FSkyState S = T < 0.5f ? A : B;
		S.LightIntensity = FMath::Lerp(A.LightIntensity, B.LightIntensity, T);
		S.LightColor = FMath::Lerp(A.LightColor, B.LightColor, T);
		S.LightSourceAngleDeg = FMath::Lerp(A.LightSourceAngleDeg, B.LightSourceAngleDeg, T);
		S.SkyLightIntensity = FMath::Lerp(A.SkyLightIntensity, B.SkyLightIntensity, T);
		S.FogDensity = FMath::Lerp(A.FogDensity, B.FogDensity, T);
		S.FogHeightFalloff = FMath::Lerp(A.FogHeightFalloff, B.FogHeightFalloff, T);
		S.FogStartDistanceCm = FMath::Lerp(A.FogStartDistanceCm, B.FogStartDistanceCm, T);
		S.FogColor = FMath::Lerp(A.FogColor, B.FogColor, T);
		S.ExposureMinEv = FMath::Lerp(A.ExposureMinEv, B.ExposureMinEv, T);
		S.ExposureMaxEv = FMath::Lerp(A.ExposureMaxEv, B.ExposureMaxEv, T);
		S.ExposureBias = FMath::Lerp(A.ExposureBias, B.ExposureBias, T);
		S.Saturation = FMath::Lerp(A.Saturation, B.Saturation, T);
		S.Contrast = FMath::Lerp(A.Contrast, B.Contrast, T);
		S.RainIntensity = FMath::Lerp(A.RainIntensity, B.RainIntensity, T);
		S.SnowIntensity = FMath::Lerp(A.SnowIntensity, B.SnowIntensity, T);
		S.SnowCover = FMath::Lerp(A.SnowCover, B.SnowCover, T);
		S.CloudCover = FMath::Lerp(A.CloudCover, B.CloudCover, T);
		S.CirrusVeil = FMath::Lerp(A.CirrusVeil, B.CirrusVeil, T);
		S.CloudColor = FMath::Lerp(A.CloudColor, B.CloudColor, T);
		S.CloudBrightness = FMath::Lerp(A.CloudBrightness, B.CloudBrightness, T);
		S.LampGlow = FMath::Lerp(A.LampGlow, B.LampGlow, T);
		S.WindowGlow = FMath::Lerp(A.WindowGlow, B.WindowGlow, T);
		return S;
	}

	/**
	 * The sky a newer server says it is now (`FApexSkyNow`): the clock it
	 * has run to, lit between the two weathers its cloud and rain stand
	 * between (`WeatherLadder`), the streaks as heavy as the rain falling
	 * (none at all when it has stopped), and the road as wet as the water
	 * on it: a light rain's half of a heavy one is the full sheen, as the
	 * fixed sky always drew it, and the sheen goes as the road dries. The
	 * clock is read to the second (the director re-derives on a minute).
	 */
	/**
	 * The height fog's density for the server's visibility through fog
	 * (`FogDensityForVisibility`), never thinner than the weather's own
	 * haze; 0 (not said) or a clear 10 km keeps the haze.
	 */
	inline float FogDensityFor(float WeatherDensity, int32 VisibilityM)
	{
		if (VisibilityM <= 0)
		{
			return WeatherDensity;
		}
		return FMath::Max(WeatherDensity, FogDensityForVisibility(static_cast<float>(VisibilityM)));
	}

	inline FSkyState DeriveLive(const FApexSessionConditions& Session, const FApexSkyNow& Now, const FSkySite& Site = FSkySite())
	{
		FApexSessionConditions C = Now.AsConditions(Session);
		// A winter sky (snow in the air, or the forecast's snow just ended)
		// is lit between overcast and snow by how hard it is snowing; the
		// ground is as white as the road's line.
		if (Now.Weather == EApexWeather::Snow || Now.SnowfallPct > 0 || Now.LineSnowPct > 0)
		{
			const float Fall = FMath::Clamp(static_cast<float>(Now.SnowfallPct) / 100.0f, 0.0f, 1.0f);
			C.Weather = EApexWeather::Overcast;
			const FSkyState Plain = Derive(C, Site);
			C.Weather = EApexWeather::Snow;
			const FSkyState Snowy = Derive(C, Site);
			FSkyState W = Blend(Plain, Snowy, Fall);
			W.SnowIntensity = Fall;
			W.SnowCover = FMath::Clamp(static_cast<float>(Now.LineSnowPct) / 100.0f, 0.0f, 1.0f);
			W.CloudCover = FMath::Clamp(static_cast<float>(Now.CloudPct) / 100.0f, 0.0f, 1.0f);
			CloudLook(W.Sun.ElevationDeg, W.CloudCover, W.CloudColor, W.CloudBrightness);
			W.RainIntensity = 0.0f;
			W.RoadWetness = FMath::Clamp(static_cast<float>(Now.RoadWaterPct) / 50.0f, 0.0f, 1.0f);
			W.bWetRoad = Now.RoadWaterPct >= 5;
			W.RoadRoughness = FMath::Lerp(DryRoadRoughness, WetRoadRoughness, W.RoadWetness);
			W.FogDensity = FogDensityFor(W.FogDensity, Now.VisibilityM);
			return W;
		}
		const float Rung = WeatherLadder(Now.CloudPct / 100.0f, Now.RainPct / 100.0f);
		const int32 Low = FMath::Clamp(FMath::FloorToInt(Rung), 0, FApexSessionConditions::WeatherCount - 1);
		const int32 High = FMath::Min(Low + 1, FApexSessionConditions::WeatherCount - 1);
		C.Weather = static_cast<EApexWeather>(Low);
		const FSkyState A = Derive(C, Site);
		C.Weather = static_cast<EApexWeather>(High);
		const FSkyState B = Derive(C, Site);
		FSkyState S = Blend(A, B, Rung - static_cast<float>(Low));
		if (Now.RainPct <= 0)
		{
			S.RainIntensity = 0.0f;
		}
		// The cloud as the server eases it, not the rung's.
		S.CloudCover = FMath::Clamp(static_cast<float>(Now.CloudPct) / 100.0f, 0.0f, 1.0f);
		CloudLook(S.Sun.ElevationDeg, S.CloudCover, S.CloudColor, S.CloudBrightness);
		// The water: a road counts as wet from 5% of a heavy rain's (the
		// server's `WET_FROM_WATER`), the sheen full by a light rain's.
		S.RoadWetness = FMath::Clamp(static_cast<float>(Now.RoadWaterPct) / 50.0f, 0.0f, 1.0f);
		S.bWetRoad = Now.RoadWaterPct >= 5;
		S.RoadRoughness = FMath::Lerp(DryRoadRoughness, WetRoadRoughness, S.RoadWetness);
		// Dawn's radiation fog (the server's dew and fog, conditions.md).
		S.FogDensity = FogDensityFor(S.FogDensity, Now.VisibilityM);
		return S;
	}

	/**
	 * Whether a live sky has moved far enough from the one the world was
	 * last lit by to light it again: a minute of the day, a step of the
	 * weather, or a few percent of rain, cloud or road water. Lighting is
	 * not free (the sky light recaptures, the road materials are written),
	 * and the server's figures move in whole percents every frame.
	 */
	/**
	 * Whether the sun has to be turned from where it stands to where the
	 * sky wants it. Virtual shadow maps key their whole cache on the light's
	 * direction, so every turn, however small, re-renders every shadow page
	 * on the next frame (and is what overflows the non-Nanite marking queue:
	 * docs/proposals/nanite-shadows.md). The sky relights for a game
	 * minute (a quarter of a degree of sun) and for every few percent of
	 * cloud or rain, which would turn it each time; held until it is
	 * `StepDeg` out, it turns every few game minutes and never for the
	 * weather. A step of zero or less turns it every time.
	 */
	inline bool SunNeedsTurning(const FQuat& Current, const FQuat& Wanted, float StepDeg)
	{
		if (StepDeg <= 0.0f)
		{
			return !Current.Equals(Wanted, 0.0);
		}
		// The light shines down its X axis; that is all the shadows see.
		const FVector From = Current.GetForwardVector();
		const FVector To = Wanted.GetForwardVector();
		const double CosAngle = FMath::Clamp(FVector::DotProduct(From, To), -1.0, 1.0);
		return FMath::RadiansToDegrees(FMath::Acos(CosAngle)) >= StepDeg;
	}

	/**
	 * Whether the fog's sight moved enough to draw again: a tenth of it (a
	 * dawn fog thickens and lifts over minutes), or into or out of none.
	 */
	inline bool VisibilityMoved(int32 LitM, int32 NowM)
	{
		if ((LitM > 0) != (NowM > 0))
		{
			return true;
		}
		return FMath::Abs(NowM - LitM) * 10 >= FMath::Max(LitM, 1);
	}

	inline bool LiveSkyMoved(const FApexSkyNow& Lit, const FApexSkyNow& Now)
	{
		if (Lit.bValid != Now.bValid)
		{
			return true;
		}
		if (!Now.bValid)
		{
			return false;
		}
		int32 ClockStep = FMath::Abs(Now.ClockS - Lit.ClockS);
		ClockStep = FMath::Min(ClockStep, 86400 - ClockStep);
		return ClockStep >= 60 || Now.Weather != Lit.Weather
			|| FMath::Abs(Now.RainPct - Lit.RainPct) >= 3
			|| FMath::Abs(Now.CloudPct - Lit.CloudPct) >= 3
			|| FMath::Abs(Now.RoadWaterPct - Lit.RoadWaterPct) >= 3
			|| FMath::Abs(Now.SnowfallPct - Lit.SnowfallPct) >= 3
			|| FMath::Abs(Now.LineSnowPct - Lit.LineSnowPct) >= 3
			|| ((Now.RainPct == 0) != (Lit.RainPct == 0))
			|| ((Now.SnowfallPct == 0) != (Lit.SnowfallPct == 0))
			|| VisibilityMoved(Lit.VisibilityM, Now.VisibilityM);
	}
}
