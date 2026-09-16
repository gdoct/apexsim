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
 * The circuit is nowhere in particular: one latitude and one date serve
 * every track, a late-spring day in the north where the sun is up from a
 * little after six to a little after nine in the evening. Solar noon is
 * 13:00, which is what a clock on summer time says.
 */
namespace ApexSky
{
	/** Latitude the sun is computed for, degrees north. */
	inline constexpr float LatitudeDeg = 50.0f;
	/** Solar declination, degrees: a day in late May. */
	inline constexpr float DeclinationDeg = 20.0f;
	/** Hour of solar noon, local clock (summer time). */
	inline constexpr float SolarNoonHours = 13.0f;
	/** Full sun, lux, at the zenith. */
	inline constexpr float ZenithLux = 100000.0f;
	/** Below this elevation the sun is gone and the moon takes the light. */
	inline constexpr float MoonBelowElevationDeg = -8.0f;

	struct FSunPosition
	{
		/** Degrees above the horizon; negative at night. */
		float ElevationDeg = 0.0f;
		/** Compass azimuth, degrees clockwise from north (+X). */
		float AzimuthDeg = 180.0f;
	};

	/** Where the sun is at `Hours` after midnight on the model day. */
	inline FSunPosition SunAt(float Hours)
	{
		const float Lat = FMath::DegreesToRadians(LatitudeDeg);
		const float Dec = FMath::DegreesToRadians(DeclinationDeg);
		const float HourAngle = FMath::DegreesToRadians((Hours - SolarNoonHours) * 15.0f);

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
		/** The road's material roughness; 0.9 is the baked dry value. */
		float RoadRoughness = 0.9f;
		bool bWetRoad = false;

		bool bHeadlights = false;
		bool bFloodlights = false;
		/** Emissive strength of the floodlight lamp faces (nits). */
		float LampGlow = 0.0f;
	};

	/** 0 at full night, 1 in full day, smooth through twilight. */
	inline float Daylight(float ElevationDeg)
	{
		return FMath::SmoothStep(-6.0f, 10.0f, ElevationDeg);
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
		default:                      return 1.0f;
		}
	}

	inline FSkyState Derive(const FApexSessionConditions& Conditions)
	{
		FSkyState S;
		const FApexSessionConditions C = Conditions.Clamped();
		S.Sun = SunAt(C.Hours());
		const float Elev = S.Sun.ElevationDeg;
		const float Day = Daylight(Elev);
		const float Direct = DirectSunFactor(C.Weather);
		const bool bOvercast = C.Weather == EApexWeather::Overcast || C.IsWet();

		S.bMoon = Elev < MoonBelowElevationDeg;
		if (S.bMoon)
		{
			// The moon stands where the sun is not: opposite it, well up. The
			// light shines away from its source, so its yaw is the sun's azimuth.
			S.LightRotation = FRotator(-35.0f, S.Sun.AzimuthDeg, 0.0f);
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
			S.LightRotation = FRotator(-FMath::Max(Elev, 0.5f), S.Sun.AzimuthDeg + 180.0f, 0.0f);
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

		// Fog: thicker with the weather, its colour the sky's, dark at night.
		switch (C.Weather)
		{
		case EApexWeather::Sunny:     S.FogDensity = 0.0015f; S.FogColor = FLinearColor(0.45f, 0.52f, 0.62f); break;
		case EApexWeather::Cloudy:    S.FogDensity = 0.0022f; S.FogColor = FLinearColor(0.50f, 0.54f, 0.60f); break;
		case EApexWeather::Overcast:  S.FogDensity = 0.0035f; S.FogColor = FLinearColor(0.52f, 0.54f, 0.57f); break;
		case EApexWeather::LightRain: S.FogDensity = 0.0055f; S.FogColor = FLinearColor(0.46f, 0.48f, 0.51f); break;
		case EApexWeather::HeavyRain: S.FogDensity = 0.0095f; S.FogColor = FLinearColor(0.40f, 0.42f, 0.45f); break;
		default: break;
		}
		S.FogStartDistanceCm = C.IsWet() ? 1000.0f : 3000.0f;
		S.FogHeightFalloff = 0.2f;
		S.FogColor *= FMath::Lerp(0.012f, 1.0f, Day);

		// Exposure: the baked daylight clamp (10..15 EV) would hold a night
		// scene black, so the floor follows the light that is there.
		const float NightMin = -2.0f;
		const float DayMin = bOvercast ? 8.0f : 10.0f;
		S.ExposureMinEv = FMath::Lerp(NightMin, DayMin, Day);
		S.ExposureMaxEv = FMath::Lerp(8.0f, 15.0f, Day);
		S.ExposureBias = -0.4f + (C.IsWet() ? -0.15f : 0.0f);
		S.Saturation = C.IsWet() ? 0.82f : (bOvercast ? 0.9f : 1.0f);
		S.Contrast = C.IsWet() ? 0.96f : 1.0f;

		S.RainIntensity = C.Weather == EApexWeather::LightRain ? 0.4f : (C.Weather == EApexWeather::HeavyRain ? 1.0f : 0.0f);
		S.bWetRoad = C.IsWet();
		S.RoadRoughness = S.bWetRoad ? 0.3f : 0.9f;

		// Lights come on as the sun goes, and in the rain as they do on a race day.
		S.bHeadlights = Elev < 6.0f || C.IsWet();
		S.bFloodlights = Elev < 4.0f;
		S.LampGlow = S.bFloodlights ? FMath::Lerp(60.0f, 6.0f, Day) : 0.0f;
		return S;
	}
}
