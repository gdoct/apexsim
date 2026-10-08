#include "ApexTestCommon.h"
#include "ApexDemoModeSubsystem.h"
#include "Race/ApexSkyModel.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	FApexSessionConditions At(EApexWeather Weather, int32 Hour, int32 Minute = 0)
	{
		FApexSessionConditions C;
		C.Weather = Weather;
		C.TimeOfDayMinutes = Hour * 60 + Minute;
		return C;
	}
}

// -----------------------------------------------------------------------------
// The sun: up in the day, down at night, highest at solar noon, east then west.
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexSkySunPathTest,
	"ApexSim.Sky.SunPath",
	ApexTestFlags)

bool FApexSkySunPathTest::RunTest(const FString& Parameters)
{
	using namespace ApexSky;

	const FSunPosition Noon = SunAt(SolarNoonHours);
	const FSunPosition Morning = SunAt(9.0f);
	const FSunPosition Evening = SunAt(18.0f);
	const FSunPosition Midnight = SunAt(0.5f);

	TestTrue(TEXT("noon is the highest sun"), Noon.ElevationDeg > Morning.ElevationDeg && Noon.ElevationDeg > Evening.ElevationDeg);
	// 50 N in late May: about 60 degrees at noon.
	TestTrue(TEXT("noon elevation plausible"), Noon.ElevationDeg > 55.0f && Noon.ElevationDeg < 65.0f);
	TestTrue(TEXT("noon is due south"), FMath::Abs(Noon.AzimuthDeg - 180.0f) < 1.0f);
	TestTrue(TEXT("morning sun is in the east"), Morning.AzimuthDeg > 60.0f && Morning.AzimuthDeg < 180.0f);
	TestTrue(TEXT("evening sun is in the west"), Evening.AzimuthDeg > 180.0f && Evening.AzimuthDeg < 300.0f);
	TestTrue(TEXT("midnight sun is below the horizon"), Midnight.ElevationDeg < -10.0f);

	// Sunrise and sunset bracket the day: down at 5, up at 7, up at 20, down at 22.
	TestTrue(TEXT("05:00 dark"), SunAt(5.0f).ElevationDeg < 0.0f);
	TestTrue(TEXT("07:00 light"), SunAt(7.0f).ElevationDeg > 0.0f);
	TestTrue(TEXT("20:00 light"), SunAt(20.0f).ElevationDeg > 0.0f);
	TestTrue(TEXT("22:00 dark"), SunAt(22.0f).ElevationDeg < 0.0f);
	return true;
}

// -----------------------------------------------------------------------------
// What the director gets: day, dusk and night; sun and cloud; rain.
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexSkyDeriveDayNightTest,
	"ApexSim.Sky.DayAndNight",
	ApexTestFlags)

bool FApexSkyDeriveDayNightTest::RunTest(const FString& Parameters)
{
	using namespace ApexSky;

	const FSkyState Day = Derive(At(EApexWeather::Sunny, 13));
	TestFalse(TEXT("day: the sun, not the moon"), Day.bMoon);
	TestTrue(TEXT("day: atmosphere sun"), Day.bAtmosphereLight);
	TestTrue(TEXT("day: tens of thousands of lux"), Day.LightIntensity > 50000.0f);
	TestTrue(TEXT("day: shadows"), Day.bCastShadows);
	TestTrue(TEXT("day: light points down"), Day.LightRotation.Pitch < -50.0f);
	TestTrue(TEXT("day: the baked exposure clamp"), FMath::IsNearlyEqual(Day.ExposureMinEv, 10.0f) && FMath::IsNearlyEqual(Day.ExposureMaxEv, 15.0f));
	TestFalse(TEXT("day: no headlights"), Day.bHeadlights);
	TestFalse(TEXT("day: no floodlights"), Day.bFloodlights);
	TestTrue(TEXT("day: no rain"), Day.RainIntensity == 0.0f && !Day.bWetRoad);
	TestTrue(TEXT("day: dry road roughness as baked"), FMath::IsNearlyEqual(Day.RoadRoughness, 0.9f));

	const FSkyState Night = Derive(At(EApexWeather::Sunny, 23, 30));
	TestTrue(TEXT("night: the moon"), Night.bMoon);
	TestFalse(TEXT("night: the moon does not light the atmosphere"), Night.bAtmosphereLight);
	TestTrue(TEXT("night: about a lux"), Night.LightIntensity < 2.0f && Night.LightIntensity > 0.1f);
	TestTrue(TEXT("night: exposure floor goes down"), Night.ExposureMinEv < 0.0f);
	TestTrue(TEXT("night: exposure ceiling comes down too"), Night.ExposureMaxEv < 10.0f);
	TestTrue(TEXT("night: headlights"), Night.bHeadlights);
	TestTrue(TEXT("night: floodlights"), Night.bFloodlights);
	TestTrue(TEXT("night: lamps glow"), Night.LampGlow > 0.0f);
	TestTrue(TEXT("night: fog is dark"), Night.FogColor.GetLuminance() < 0.05f);
	TestTrue(TEXT("night: light still points down"), Night.LightRotation.Pitch < 0.0f);

	// Dusk sits between: sun still the light, dim and warm, lights on.
	const FSkyState Dusk = Derive(At(EApexWeather::Sunny, 20, 45));
	TestFalse(TEXT("dusk: still the sun"), Dusk.bMoon);
	TestTrue(TEXT("dusk: dimmer than noon"), Dusk.LightIntensity < Day.LightIntensity * 0.5f);
	TestTrue(TEXT("dusk: warmer than noon"), Dusk.LightColor.B / Dusk.LightColor.R < Day.LightColor.B / Day.LightColor.R);
	TestTrue(TEXT("dusk: headlights on"), Dusk.bHeadlights);
	TestTrue(TEXT("dusk: exposure between"), Dusk.ExposureMinEv > Night.ExposureMinEv && Dusk.ExposureMinEv < Day.ExposureMinEv);

	// The clock wraps: 25:00 is 01:00.
	FApexSessionConditions Wrapped = At(EApexWeather::Sunny, 25);
	TestTrue(TEXT("wrapped clock is night"), Derive(Wrapped).bMoon);
	TestEqual(TEXT("wrapped clock text"), Wrapped.ClockText(), FString(TEXT("01:00")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexSkyDeriveWeatherTest,
	"ApexSim.Sky.Weather",
	ApexTestFlags)

bool FApexSkyDeriveWeatherTest::RunTest(const FString& Parameters)
{
	using namespace ApexSky;

	const FSkyState Sunny = Derive(At(EApexWeather::Sunny, 14));
	const FSkyState Cloudy = Derive(At(EApexWeather::Cloudy, 14));
	const FSkyState Overcast = Derive(At(EApexWeather::Overcast, 14));
	const FSkyState Light = Derive(At(EApexWeather::LightRain, 14));
	const FSkyState Heavy = Derive(At(EApexWeather::HeavyRain, 14));

	// Each step takes more off the direct sun and puts more into the sky and the fog.
	TestTrue(TEXT("sun dims with cloud"), Sunny.LightIntensity > Cloudy.LightIntensity && Cloudy.LightIntensity > Overcast.LightIntensity
		&& Overcast.LightIntensity > Light.LightIntensity && Light.LightIntensity > Heavy.LightIntensity);
	TestTrue(TEXT("sky light scales up under cloud"), Overcast.SkyLightIntensity > Cloudy.SkyLightIntensity && Cloudy.SkyLightIntensity > Sunny.SkyLightIntensity);
	TestTrue(TEXT("fog thickens"), Sunny.FogDensity < Cloudy.FogDensity && Cloudy.FogDensity < Overcast.FogDensity
		&& Overcast.FogDensity < Light.FogDensity && Light.FogDensity < Heavy.FogDensity);
	TestTrue(TEXT("sunny casts shadows"), Sunny.bCastShadows && Cloudy.bCastShadows);
	TestFalse(TEXT("overcast casts none"), Overcast.bCastShadows || Light.bCastShadows || Heavy.bCastShadows);
	TestTrue(TEXT("cloud widens the source"), Overcast.LightSourceAngleDeg > Sunny.LightSourceAngleDeg);
	TestTrue(TEXT("overcast lowers the exposure floor"), Overcast.ExposureMinEv < Sunny.ExposureMinEv);

	// Rain: streaks, a wet road, lights on in daylight, and a greyer grade.
	TestTrue(TEXT("dry has no rain"), Sunny.RainIntensity == 0.0f && Overcast.RainIntensity == 0.0f);
	TestTrue(TEXT("light rain"), Light.RainIntensity > 0.0f && Light.RainIntensity < Heavy.RainIntensity);
	TestTrue(TEXT("heavy rain is a downpour"), FMath::IsNearlyEqual(Heavy.RainIntensity, 1.0f));
	TestTrue(TEXT("rain wets the road"), Light.bWetRoad && Heavy.bWetRoad && !Overcast.bWetRoad);
	TestTrue(TEXT("wet road is glossier"), Light.RoadRoughness < Sunny.RoadRoughness);
	TestTrue(TEXT("rain turns the headlights on by day"), Light.bHeadlights && !Overcast.bHeadlights);
	TestTrue(TEXT("rain desaturates"), Light.Saturation < Sunny.Saturation);
	TestTrue(TEXT("rain fog starts nearer"), Light.FogStartDistanceCm < Sunny.FogStartDistanceCm);

	// The weather labels and the describe string the browser shows.
	TestEqual(TEXT("label"), FApexSessionConditions::WeatherLabel(EApexWeather::HeavyRain), FString(TEXT("Heavy rain")));
	TestEqual(TEXT("describe"), At(EApexWeather::LightRain, 21, 30).Describe(), FString(TEXT("Light rain · 21:30")));
	TestTrue(TEXT("default is a sunny afternoon"), FApexSessionConditions().IsDefault());
	return true;
}

// -----------------------------------------------------------------------------
// Where the circuit is: the latitude lifts or lowers the sun, true north turns
// it in the world. The server's `sun_elevation_deg` is the same formula.
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexSkySiteTest,
	"ApexSim.Sky.Site",
	ApexTestFlags)

bool FApexSkySiteTest::RunTest(const FString& Parameters)
{
	using namespace ApexSky;

	// Solar noon's elevation is 90 - latitude + declination, to the letter.
	TestTrue(TEXT("50 N noon is 60 degrees"), FMath::IsNearlyEqual(SunAt(SolarNoonHours).ElevationDeg, 60.0f, 0.01f));
	TestTrue(TEXT("the default is 50 N"), FMath::IsNearlyEqual(SunAt(9.0f).ElevationDeg, SunAt(9.0f, 50.0f).ElevationDeg));
	const FSunPosition YasNoon = SunAt(SolarNoonHours, 24.5f);
	TestTrue(TEXT("Yas Marina's noon sun is higher than 50 N's"), YasNoon.ElevationDeg > SunAt(SolarNoonHours).ElevationDeg);
	TestTrue(TEXT("24.5 N noon is 85.5 degrees"), FMath::IsNearlyEqual(YasNoon.ElevationDeg, 85.5f, 0.05f));
	// Further south the evening is shorter: at 20:30 the sun is down at
	// 24.5 N and still up at 50 N.
	TestTrue(TEXT("tropical dusk comes early"), SunAt(20.5f, 24.5f).ElevationDeg < 0.0f && SunAt(20.5f).ElevationDeg > 0.0f);
	// South of the declination's latitude the noon sun is to the north;
	// in the south it always is.
	TestTrue(TEXT("a southern noon sun is due north"), FMath::Abs(FRotator::NormalizeAxis(SunAt(SolarNoonHours, -34.0f).AzimuthDeg)) < 1.0f);
	TestTrue(TEXT("at 50 N it is due south"), FMath::Abs(SunAt(SolarNoonHours).AzimuthDeg - 180.0f) < 1.0f);

	// True north turns the light in the world, not the sun in the sky.
	const FApexSessionConditions Afternoon = At(EApexWeather::Sunny, 16);
	FSkySite Turned;
	Turned.NorthYawDeg = 92.3f;	// Spa's
	const FSkyState Plain = Derive(Afternoon);
	const FSkyState Spa = Derive(Afternoon, Turned);
	TestTrue(TEXT("the same sun in the sky"), FMath::IsNearlyEqual(Plain.Sun.AzimuthDeg, Spa.Sun.AzimuthDeg)
		&& FMath::IsNearlyEqual(Plain.Sun.ElevationDeg, Spa.Sun.ElevationDeg));
	TestTrue(TEXT("north turned counter-clockwise turns the light's yaw back by as much"),
		FMath::IsNearlyEqual(FRotator::NormalizeAxis(Plain.LightRotation.Yaw - Spa.LightRotation.Yaw), 92.3f, 0.01f));
	TestTrue(TEXT("no site is the default site"), Plain.LightRotation.Equals(Derive(Afternoon, FSkySite()).LightRotation));
	// A bearing: north is at Unreal yaw -NorthYawDeg (the server frame's
	// counter-clockwise yaw negated by the Y flip).
	TestTrue(TEXT("north in the world"), FMath::IsNearlyEqual(Turned.WorldYawOfBearing(0.0f), -92.3f));
	TestTrue(TEXT("east is a quarter turn clockwise of it"), FMath::IsNearlyEqual(Turned.WorldYawOfBearing(90.0f), -2.3f, 1e-3f));
	return true;
}

// -----------------------------------------------------------------------------
// A sky that moves: the server's clock, cloud, rain and road water light the
// world between the weathers, and a sky that holds is lit as it always was.
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexSkyLiveTest,
	"ApexSim.Sky.Live",
	ApexTestFlags)

bool FApexSkyLiveTest::RunTest(const FString& Parameters)
{
	using namespace ApexSky;

	auto Now = [](EApexWeather Weather, int32 Hour, int32 Rain, int32 Cloud, int32 Water)
	{
		FApexSkyNow S;
		S.bValid = true;
		S.ClockS = Hour * 3600;
		S.Weather = Weather;
		S.RainPct = Rain;
		S.CloudPct = Cloud;
		S.RoadWaterPct = Water;
		return S;
	};

	// The weathers' own cloud and rain stand on their own rungs.
	TestTrue(TEXT("sunny rung"), FMath::IsNearlyEqual(WeatherLadder(0.0f, 0.0f), 0.0f));
	TestTrue(TEXT("cloudy rung"), FMath::IsNearlyEqual(WeatherLadder(0.5f, 0.0f), 1.0f));
	TestTrue(TEXT("overcast rung"), FMath::IsNearlyEqual(WeatherLadder(0.85f, 0.0f), 2.0f));
	TestTrue(TEXT("light rain rung"), FMath::IsNearlyEqual(WeatherLadder(0.9f, 0.5f), 3.0f));
	TestTrue(TEXT("heavy rain rung"), FMath::IsNearlyEqual(WeatherLadder(1.0f, 1.0f), 4.0f));

	// A sky that holds is the fixed sky, figure for figure.
	const FApexSessionConditions Session = At(EApexWeather::LightRain, 14);
	const FSkyState Fixed = Derive(Session);
	const FSkyState Held = DeriveLive(Session, Now(EApexWeather::LightRain, 14, 50, 90, 50));
	TestTrue(TEXT("held: same light"), FMath::IsNearlyEqual(Fixed.LightIntensity, Held.LightIntensity));
	TestTrue(TEXT("held: same fog"), FMath::IsNearlyEqual(Fixed.FogDensity, Held.FogDensity));
	TestTrue(TEXT("held: same rain"), FMath::IsNearlyEqual(Fixed.RainIntensity, Held.RainIntensity));
	TestTrue(TEXT("held: same road"), FMath::IsNearlyEqual(Fixed.RoadRoughness, Held.RoadRoughness) && Held.bWetRoad);
	TestTrue(TEXT("held: same sun"), Fixed.LightRotation.Equals(Held.LightRotation));

	// Rain arriving: between overcast and light rain, the streaks part way.
	const FSkyState Arriving = DeriveLive(Session, Now(EApexWeather::LightRain, 14, 20, 88, 4));
	const FSkyState Overcast = Derive(At(EApexWeather::Overcast, 14));
	TestTrue(TEXT("arriving: some rain"), Arriving.RainIntensity > 0.0f && Arriving.RainIntensity < Fixed.RainIntensity);
	TestTrue(TEXT("arriving: darker than overcast, lighter than the rain"),
		Arriving.LightIntensity < Overcast.LightIntensity && Arriving.LightIntensity > Fixed.LightIntensity);
	TestFalse(TEXT("arriving: the road not wet yet"), Arriving.bWetRoad);
	TestTrue(TEXT("arriving: dry roughness"), FMath::IsNearlyEqual(Arriving.RoadRoughness, DryRoadRoughness, 0.1f));

	// Rain stopped, road still wet: no streaks, a sheen part way.
	const FSkyState Drying = DeriveLive(Session, Now(EApexWeather::Cloudy, 14, 0, 60, 25));
	TestTrue(TEXT("drying: no streaks"), Drying.RainIntensity == 0.0f);
	TestTrue(TEXT("drying: still wet"), Drying.bWetRoad);
	TestTrue(TEXT("drying: half the sheen"), FMath::IsNearlyEqual(Drying.RoadWetness, 0.5f)
		&& Drying.RoadRoughness < DryRoadRoughness && Drying.RoadRoughness > WetRoadRoughness);

	// The clock is the server's, not the session's pick.
	const FSkyState Late = DeriveLive(At(EApexWeather::Sunny, 13), Now(EApexWeather::Sunny, 23, 0, 0, 0));
	TestTrue(TEXT("the running clock brings the night"), Late.bMoon && Late.bFloodlights);

	// Relighting is on a minute, a weather step or a few percent.
	const FApexSkyNow Base = Now(EApexWeather::Cloudy, 14, 0, 50, 0);
	FApexSkyNow Moved = Base;
	Moved.ClockS += 30;
	TestFalse(TEXT("half a minute is nothing"), LiveSkyMoved(Base, Moved));
	Moved.ClockS += 30;
	TestTrue(TEXT("a minute relights"), LiveSkyMoved(Base, Moved));
	Moved = Base;
	Moved.CloudPct += 2;
	TestFalse(TEXT("two percent of cloud is nothing"), LiveSkyMoved(Base, Moved));
	Moved.RainPct = 1;
	TestTrue(TEXT("the first drop relights"), LiveSkyMoved(Base, Moved));
	FApexSkyNow Midnight = Base;
	Midnight.ClockS = 86399;
	FApexSkyNow After = Base;
	After.ClockS = 20;
	TestFalse(TEXT("midnight is not a day's jump"), LiveSkyMoved(Midnight, After));
	TestTrue(TEXT("losing the sky relights"), LiveSkyMoved(Base, FApexSkyNow()));
	return true;
}

// -----------------------------------------------------------------------------
// The demo's sky: every weather and all three parts of the day come up, on
// quarter hours, mostly dry daylight.
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexSkyDemoRollTest,
	"ApexSim.Sky.DemoRoll",
	ApexTestFlags)

bool FApexSkyDemoRollTest::RunTest(const FString& Parameters)
{
	FRandomStream Random(1234);
	constexpr int32 Rolls = 4000;
	int32 WeatherSeen[FApexSessionConditions::WeatherCount] = {};
	int32 Day = 0, LowSun = 0, Night = 0, Dry = 0;
	bool bQuarterHours = true;
	for (int32 Index = 0; Index < Rolls; ++Index)
	{
		const FApexSessionConditions C = UApexDemoModeSubsystem::RollConditions(Random);
		++WeatherSeen[static_cast<int32>(C.Weather)];
		Dry += C.IsWet() ? 0 : 1;
		bQuarterHours &= C.TimeOfDayMinutes % 15 == 0 && C.TimeOfDayMinutes >= 0 && C.TimeOfDayMinutes < FApexSessionConditions::MinutesPerDay;
		const int32 Hour = C.TimeOfDayMinutes / 60;
		if (Hour >= 8 && Hour < 18) { ++Day; }
		else if ((Hour >= 6 && Hour < 8) || (Hour >= 18 && Hour < 21)) { ++LowSun; }
		else { ++Night; }
	}

	for (int32 Index = 0; Index < FApexSessionConditions::WeatherCount; ++Index)
	{
		TestTrue(FString::Printf(TEXT("weather %d comes up"), Index), WeatherSeen[Index] > 0);
	}
	TestTrue(TEXT("on quarter hours within the day"), bQuarterHours);
	TestTrue(TEXT("rain one race in ten"), Dry > Rolls * 87 / 100 && Dry < Rolls * 93 / 100);
	TestTrue(TEXT("mostly daylight"), Day > Rolls * 65 / 100 && Day < Rolls * 75 / 100);
	TestTrue(TEXT("some low sun"), LowSun > Rolls * 15 / 100 && LowSun < Rolls * 25 / 100);
	TestTrue(TEXT("night one race in ten"), Night > Rolls * 7 / 100 && Night < Rolls * 13 / 100);
	return true;
}

// -----------------------------------------------------------------------------
// The sun is turned only once it has drifted past the step: a game minute of
// the sky's own sun is not enough, a few are, and the weather never is.
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexSkySunStepTest,
	"ApexSim.Sky.SunStep",
	ApexTestFlags)

bool FApexSkySunStepTest::RunTest(const FString& Parameters)
{
	using namespace ApexSky;

	const FQuat At1500 = Derive(At(EApexWeather::Sunny, 15, 0)).LightRotation.Quaternion();
	const FQuat At1501 = Derive(At(EApexWeather::Sunny, 15, 1)).LightRotation.Quaternion();
	const FQuat At1505 = Derive(At(EApexWeather::Sunny, 15, 5)).LightRotation.Quaternion();
	const FQuat Cloudy1500 = Derive(At(EApexWeather::Cloudy, 15, 0)).LightRotation.Quaternion();

	TestFalse(TEXT("a game minute stays under half a degree"), SunNeedsTurning(At1500, At1501, 0.5f));
	TestTrue(TEXT("five game minutes pass it"), SunNeedsTurning(At1500, At1505, 0.5f));
	TestFalse(TEXT("cloud alone does not turn the sun"), SunNeedsTurning(At1500, Cloudy1500, 0.5f));
	TestTrue(TEXT("a step of zero turns it for a minute"), SunNeedsTurning(At1500, At1501, 0.0f));
	TestFalse(TEXT("a step of zero leaves a sun that is there"), SunNeedsTurning(At1500, At1500, 0.0f));
	// A roll about the beam is not a turn: the shadows cannot see it.
	const FQuat Rolled = At1500 * FQuat(FVector::ForwardVector, FMath::DegreesToRadians(30.0f));
	TestFalse(TEXT("roll about the beam"), SunNeedsTurning(At1500, Rolled, 0.5f));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
