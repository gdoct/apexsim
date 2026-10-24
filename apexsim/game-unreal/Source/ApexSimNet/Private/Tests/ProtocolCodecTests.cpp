#include "Misc/AutomationTest.h"

#include "ApexDriverKey.h"
#include "ApexLiveryPackage.h"
#include "ApexProtocolCodec.h"
#include "ApexProtocolTypes.h"
#include "ApexServerErrors.h"
#include "HAL/IConsoleManager.h"
#include "Tests/ApexGoldenBlobs.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	constexpr EAutomationTestFlags ApexTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter;

	const FString CarId    = TEXT("11111111-2222-3333-4444-555555555555");
	const FString TrackId  = TEXT("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee");
	const FString SessId   = TEXT("01234567-89ab-cdef-0123-456789abcdef");
	const FString PlayerId = TEXT("deadbeef-0000-1111-2222-333344445555");

	/** Renders a mismatch as a hex diff so a failure names the offending byte. */
	FString DescribeMismatch(TArrayView<const uint8> Actual, TArrayView<const uint8> Expected)
	{
		if (Actual.Num() != Expected.Num())
		{
			return FString::Printf(TEXT("length %d, expected %d"), Actual.Num(), Expected.Num());
		}
		for (int32 i = 0; i < Actual.Num(); ++i)
		{
			if (Actual[i] != Expected[i])
			{
				return FString::Printf(
					TEXT("first difference at byte %d: got 0x%02X, expected 0x%02X"),
					i, Actual[i], Expected[i]);
			}
		}
		return TEXT("identical");
	}
}

// -----------------------------------------------------------------------------
// Encode: the client must produce exactly the bytes rmp_serde does.
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexProtocolGoldenEncodeTest,
	"ApexSim.Net.Protocol.GoldenEncode",
	ApexTestFlags)

bool FApexProtocolGoldenEncodeTest::RunTest(const FString& Parameters)
{
	auto CheckBytes = [this](const TCHAR* Label, const TArray<uint8>& Actual, TArrayView<const uint8> Expected)
	{
		const bool bMatches = Actual.Num() == Expected.Num()
			&& FMemory::Memcmp(Actual.GetData(), Expected.GetData(), Expected.Num()) == 0;
		TestTrue(
			FString::Printf(TEXT("%s encodes byte-for-byte (%s)"),
				Label, *DescribeMismatch(Actual, Expected)),
			bMatches);
	};

	CheckBytes(TEXT("Authenticate"),
		ApexProtocol::EncodeAuthenticate(TEXT("dev-token"), TEXT("Player")),
		ApexGolden::C_Authenticate);
	// A reconnect presents the resume token; without one the bytes above hold.
	CheckBytes(TEXT("Authenticate with a resume token"),
		ApexProtocol::EncodeAuthenticate(TEXT("dev-token"), TEXT("Player"), TEXT("resume-tok")),
		ApexGolden::C_AuthenticateResume);
	// A driver profile sends its id beside the name.
	CheckBytes(TEXT("Authenticate with a driver profile"),
		ApexProtocol::EncodeAuthenticate(TEXT("dev-token"), TEXT("Player"), FString(), TEXT("dddddddd-0000-4000-8000-000000000001")),
		ApexGolden::C_AuthenticateProfile);
	// A profile with a key sends its public half too, and proves it.
	CheckBytes(TEXT("Authenticate with a driver key"),
		ApexProtocol::EncodeAuthenticate(TEXT("dev-token"), TEXT("Player"), FString(), TEXT("dddddddd-0000-4000-8000-000000000001"),
			FString::ChrN(32, 'a').Replace(TEXT("a"), TEXT("ab"))),
		ApexGolden::C_AuthenticateKey);
	CheckBytes(TEXT("ProveDriverKey"),
		ApexProtocol::EncodeProveDriverKey(FString::ChrN(64, 'c').Replace(TEXT("c"), TEXT("cd"))),
		ApexGolden::C_ProveDriverKey);

	// The unit variants are the sharpest trap: a one-key map with no "data".
	// Emitting `"data": {}` fails from_slice on the server, and each parse
	// failure costs 10 rate-limit violations (200 => forced disconnect).
	CheckBytes(TEXT("RequestLobbyState"),
		ApexProtocol::EncodeRequestLobbyState(),
		ApexGolden::C_RequestLobbyState);

	CheckBytes(TEXT("LeaveSession"),
		ApexProtocol::EncodeLeaveSession(),
		ApexGolden::C_LeaveSession);

	CheckBytes(TEXT("Disconnect"),
		ApexProtocol::EncodeDisconnect(),
		ApexGolden::C_Disconnect);

	CheckBytes(TEXT("Heartbeat"),
		ApexProtocol::EncodeHeartbeat(300),
		ApexGolden::C_Heartbeat);

	// A measured ping rides along as `ping_ms`; without one (above) the bytes
	// are those of every earlier client.
	CheckBytes(TEXT("Heartbeat with ping"),
		ApexProtocol::EncodeHeartbeat(300, 42),
		ApexGolden::C_HeartbeatPing);

	CheckBytes(TEXT("SelectCar"),
		ApexProtocol::EncodeSelectCar(CarId),
		ApexGolden::C_SelectCar);

	CheckBytes(TEXT("Chat"),
		ApexProtocol::EncodeChat(TEXT("good luck")),
		ApexGolden::C_Chat);

	CheckBytes(TEXT("Rewind"),
		ApexProtocol::EncodeRewind(10),
		ApexGolden::C_Rewind);

	CheckBytes(TEXT("RequestIncidents"),
		ApexProtocol::EncodeRequestIncidents(),
		ApexGolden::C_RequestIncidents);

	CheckBytes(TEXT("RequestEventSheets"),
		ApexProtocol::EncodeRequestEventSheets(TEXT("autumn-cup")),
		ApexGolden::C_RequestEventSheets);

	CheckBytes(TEXT("RequestChampionships"),
		ApexProtocol::EncodeRequestChampionships(),
		ApexGolden::C_RequestChampionships);

	FApexAllowedAssists CreateAssists;
	CreateAssists.bTractionControl = false;
	CreateAssists.bSteeringAssist = false;
	FApexSessionConditions CreateConditions;
	CreateConditions.Weather = EApexWeather::LightRain;
	CreateConditions.TimeOfDayMinutes = 21 * 60 + 30;
	CheckBytes(TEXT("CreateSession"),
		ApexProtocol::EncodeCreateSession(TrackId, 8, 3, 5, EApexSessionKind::Multiplayer, CreateAssists, CreateConditions),
		ApexGolden::C_CreateSession);

	{
		// The air named in full; an absent figure is left off, as above.
		FApexSessionConditions Air;
		Air.Weather = EApexWeather::Overcast;
		Air.TimeOfDayMinutes = 8 * 60;
		Air.AirTempC = -3;
		Air.HumidityPct = 80;
		Air.WindKph = 22;
		Air.WindFromDeg = 270;
		CheckBytes(TEXT("CreateSession with the air"),
			ApexProtocol::EncodeCreateSession(TrackId, 8, 3, 5, EApexSessionKind::Multiplayer, FApexAllowedAssists(), Air),
			ApexGolden::C_CreateSessionAir);
	}

	{
		// A changing sky: the three follow the air's keys, the air unnamed.
		FApexSessionConditions Sky;
		Sky.Weather = EApexWeather::Cloudy;
		Sky.TimeOfDayMinutes = 15 * 60;
		Sky.TimeScale = 24;
		Sky.Changeable = 2;
		Sky.TrackRubberPct = 90;
		CheckBytes(TEXT("CreateSession with a changing sky"),
			ApexProtocol::EncodeCreateSession(TrackId, 8, 3, 5, EApexSessionKind::Multiplayer, FApexAllowedAssists(), Sky),
			ApexGolden::C_CreateSessionSky);
		// Out of range is held to what the server takes, as it would.
		FApexSessionConditions Wild = Sky;
		Wild.TimeScale = 200;
		Wild.Changeable = 9;
		Wild.TrackRubberPct = 300;
		const FApexSessionConditions Held = Wild.Clamped();
		TestEqual(TEXT("Sky.TimeScale clamped"), Held.TimeScale, FApexSessionConditions::MaxTimeScale);
		TestEqual(TEXT("Sky.Changeable clamped"), Held.Changeable, FApexSessionConditions::MaxChangeable);
		TestEqual(TEXT("Sky.TrackRubberPct clamped"), Held.TrackRubberPct, 100);
		TestFalse(TEXT("Sky is not the default"), Sky.IsDefault());

		// The host's own seed for the forecast rides last among the sky's keys.
		FApexSessionConditions Seeded = Sky;
		Seeded.SkySeed = 0x0123456789ABCDEFull;
		Seeded.bHasSkySeed = true;
		CheckBytes(TEXT("CreateSession with a seeded sky"),
			ApexProtocol::EncodeCreateSession(TrackId, 8, 3, 5, EApexSessionKind::Multiplayer, FApexAllowedAssists(), Seeded),
			ApexGolden::C_CreateSessionSkySeed);
		TestTrue(TEXT("a seed is not the sky it was rolled for"), Seeded != Sky);

		// The host's date rides after the seed; solar noon is the server's,
		// so a create never sends one even when the conditions carry it.
		FApexSessionConditions Dated = Seeded;
		Dated.DayOfYear = 200;
		Dated.SolarNoonMin = 13 * 60 + 33;
		CheckBytes(TEXT("CreateSession with a date"),
			ApexProtocol::EncodeCreateSession(TrackId, 8, 3, 5, EApexSessionKind::Multiplayer, FApexAllowedAssists(), Dated),
			ApexGolden::C_CreateSessionDate);
		FApexSessionConditions Late = Sky;
		Late.DayOfYear = 999;
		TestEqual(TEXT("Sky.DayOfYear clamped"), Late.Clamped().DayOfYear, FApexSessionConditions::DaysPerYear);

		// The host's own forecast rides last, `[at_min, weather]` pairs; a
		// step too soon after the last is dropped as the server drops it.
		auto Step = [](int32 AtMin, EApexWeather Weather)
		{
			FApexForecastStep Out;
			Out.AtMin = AtMin;
			Out.Weather = Weather;
			return Out;
		};
		FApexSessionConditions Written = Sky;
		Written.Forecast = { Step(45, EApexWeather::HeavyRain), Step(20, EApexWeather::LightRain),
			Step(22, EApexWeather::Overcast), Step(90, EApexWeather::Sunny) };
		CheckBytes(TEXT("CreateSession with a written forecast"),
			ApexProtocol::EncodeCreateSession(TrackId, 8, 3, 5, EApexSessionKind::Multiplayer, FApexAllowedAssists(), Written),
			ApexGolden::C_CreateSessionForecast);
		TestEqual(TEXT("Forecast clamped"), Written.Clamped().Forecast.Num(), 3);
		TestFalse(TEXT("a written forecast is not the default"), Written.IsDefault());
	}

	CheckBytes(TEXT("JoinSession"),
		ApexProtocol::EncodeJoinSession(SessId),
		ApexGolden::C_JoinSession);

	CheckBytes(TEXT("SetGameMode"),
		ApexProtocol::EncodeSetGameMode(EApexGameMode::FreePractice),
		ApexGolden::C_SetGameMode);

	CheckBytes(TEXT("StartCountdown"),
		ApexProtocol::EncodeStartCountdown(10, EApexGameMode::Race),
		ApexGolden::C_StartCountdown);

	CheckBytes(TEXT("OpenGrid"), ApexProtocol::EncodeOpenGrid(10), ApexGolden::C_OpenGrid);
	CheckBytes(TEXT("SetReady"), ApexProtocol::EncodeSetReady(true), ApexGolden::C_SetReady);
	CheckBytes(TEXT("SetLivery"), ApexProtocol::EncodeSetLivery(2), ApexGolden::C_SetLivery);
	{
		FApexPitPlan Plan;
		Plan.bKeepTyres = true;
		Plan.Compound = 2;
		Plan.FuelL = 5.0f;
		Plan.bNoRepair = true;
		Plan.Pads = 1;
		Plan.bChoosePadSet = true;
		Plan.PadSet = -1;
		Plan.bBoxThisLap = true;
		CheckBytes(TEXT("SetPitPlan"), ApexProtocol::EncodeSetPitPlan(Plan), ApexGolden::C_SetPitPlan);
		CheckBytes(TEXT("SetPitPlan empty"), ApexProtocol::EncodeSetPitPlan(FApexPitPlan()), ApexGolden::C_SetPitPlanEmpty);
	}

	CheckBytes(TEXT("SetDriverAids"),
		ApexProtocol::EncodeSetDriverAids(true, true, false, EApexTractionControl::High, false),
		ApexGolden::C_SetDriverAids);
	CheckBytes(TEXT("CreateSession with damage"),
		ApexProtocol::EncodeCreateSession(
			TEXT("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"), 8, 3, 5, EApexSessionKind::Multiplayer,
			FApexAllowedAssists(), FApexSessionConditions(), EApexDamageLevel::Reduced),
		ApexGolden::C_CreateSessionDamage);
	CheckBytes(TEXT("CreateSession with an AI level"),
		ApexProtocol::EncodeCreateSession(
			TEXT("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"), 8, 3, 5, EApexSessionKind::Multiplayer,
			FApexAllowedAssists(), FApexSessionConditions(), EApexDamageLevel::Full, 95),
		ApexGolden::C_CreateSessionAiSkill);
	{
		// The mixed field is left off: a create that names no level keeps
		// the bytes of one from before the field.
		const TArray<uint8> Mixed = ApexProtocol::EncodeCreateSession(
			TEXT("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"), 8, 3, 5, EApexSessionKind::Multiplayer,
			FApexAllowedAssists(), FApexSessionConditions(), EApexDamageLevel::Reduced, ApexAiSkill::Mixed);
		CheckBytes(TEXT("CreateSession, mixed AI, has no ai_skill"), Mixed, ApexGolden::C_CreateSessionDamage);
	}
	CheckBytes(TEXT("CreateSession for a timed race"),
		ApexProtocol::EncodeCreateSession(
			TEXT("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"), 8, 3, 5, EApexSessionKind::Multiplayer,
			FApexAllowedAssists(), FApexSessionConditions(), EApexDamageLevel::Full, ApexAiSkill::Mixed, 7200),
		ApexGolden::C_CreateSessionRaceTime);
	{
		// A race over laps is left off: no race_seconds in its bytes.
		const TArray<uint8> Laps = ApexProtocol::EncodeCreateSession(
			TEXT("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"), 8, 3, 5, EApexSessionKind::Multiplayer,
			FApexAllowedAssists(), FApexSessionConditions(), EApexDamageLevel::Reduced, ApexAiSkill::Mixed, 0);
		CheckBytes(TEXT("CreateSession over laps has no race_seconds"), Laps, ApexGolden::C_CreateSessionDamage);
	}
	CheckBytes(TEXT("CreateSession with timed qualifying"),
		ApexProtocol::EncodeCreateSession(
			TEXT("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"), 8, 3, 5, EApexSessionKind::Multiplayer,
			FApexAllowedAssists(), FApexSessionConditions(), EApexDamageLevel::Full, ApexAiSkill::Mixed, 0,
			TArray<FString>(), 900),
		ApexGolden::C_CreateSessionQualifyingTime);
	{
		FApexWeekendFormat Weekend;
		Weekend.PracticeSeconds = 1800;
		Weekend.QualifyingSeconds = { 1080, 900, 720 };
		CheckBytes(TEXT("CreateSession for a race weekend"),
			ApexProtocol::EncodeCreateSession(
				TEXT("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"), 8, 3, 5, EApexSessionKind::Multiplayer,
				FApexAllowedAssists(), FApexSessionConditions(), EApexDamageLevel::Full, ApexAiSkill::Mixed, 0,
				TArray<FString>(), 0, Weekend),
			ApexGolden::C_CreateSessionWeekend);

		// Practice under its own sky: heavy rain at 07:00; qualifying under the session's.
		Weekend.bHasPracticeConditions = true;
		Weekend.PracticeConditions.Weather = EApexWeather::HeavyRain;
		Weekend.PracticeConditions.TimeOfDayMinutes = 7 * 60;
		CheckBytes(TEXT("CreateSession for a race weekend with a day's sky"),
			ApexProtocol::EncodeCreateSession(
				TEXT("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"), 8, 3, 5, EApexSessionKind::Multiplayer,
				FApexAllowedAssists(), FApexSessionConditions(), EApexDamageLevel::Full, ApexAiSkill::Mixed, 0,
				TArray<FString>(), 0, Weekend),
			ApexGolden::C_CreateSessionWeekendDays);
	}
	{
		// A hotlap watched as a stream, on a setup (`watch`).
		ApexProtocol::FHotlapWatchOptions Watch;
		Watch.bStream = true;
		Watch.bHasSetup = true;
		for (int32 Index = 0; Index < FApexCarSetup::KnobCount; ++Index)
		{
			const FString Key = ApexCarSetup::Knob(Index).Key;
			Watch.Setup.Clicks[Index] = Key == TEXT("tyre_pressure_front") ? -2 : Key == TEXT("rear_wing") ? 3 : 0;
		}
		CheckBytes(TEXT("CreateSession for a hotlap watched as a stream"),
			ApexProtocol::EncodeCreateSession(
				TEXT("00000000-0000-0000-0000-000000000007"), 1, 1, 3, EApexSessionKind::HotlapWatch,
				FApexAllowedAssists(), FApexSessionConditions(), EApexDamageLevel::Full, ApexAiSkill::Mixed, 0,
				TArray<FString>(), 0, FApexWeekendFormat(), &Watch),
			ApexGolden::C_CreateSessionHotlapWatchStream);
	}
	CheckBytes(TEXT("CreateSession with the 107% rule"),
		ApexProtocol::EncodeCreateSession(
			TEXT("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"), 8, 3, 5, EApexSessionKind::Multiplayer,
			FApexAllowedAssists(), FApexSessionConditions(), EApexDamageLevel::Full, ApexAiSkill::Mixed, 0,
			TArray<FString>(), 0, FApexWeekendFormat(), nullptr, ApexQualifyingCutoff::Rule107),
		ApexGolden::C_CreateSessionQualifyingCutoff);
	TestEqual(TEXT("no cut-off stays none"), ApexQualifyingCutoff::Clamp(0), 0);
	TestEqual(TEXT("a cut-off is at least 101%"), ApexQualifyingCutoff::Clamp(100), 101);
	TestEqual(TEXT("and at most 150%"), ApexQualifyingCutoff::Clamp(300), 150);
	CheckBytes(TEXT("CreateSession with the pit rule"),
		ApexProtocol::EncodeCreateSession(
			TEXT("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"), 8, 3, 5, EApexSessionKind::Multiplayer,
			FApexAllowedAssists(), FApexSessionConditions(), EApexDamageLevel::Full, ApexAiSkill::Mixed, 0,
			TArray<FString>(), 0, FApexWeekendFormat(), nullptr, ApexQualifyingCutoff::None, 2, true),
		ApexGolden::C_CreateSessionPitRule);
	TestEqual(TEXT("mandatory stops are at most three"), ApexMandatoryStops::Clamp(9), 3);
	CheckBytes(TEXT("CreateSession for a shoot-out with a lap allowance"),
		ApexProtocol::EncodeCreateSession(
			TEXT("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"), 8, 3, 5, EApexSessionKind::Multiplayer,
			FApexAllowedAssists(), FApexSessionConditions(), EApexDamageLevel::Full, ApexAiSkill::Mixed, 0,
			TArray<FString>(), 0, FApexWeekendFormat(), nullptr, ApexQualifyingCutoff::None, 0, false,
			EApexQualifyingFormat::Shootout, 3),
		ApexGolden::C_CreateSessionShootout);
	TestEqual(TEXT("a lap allowance is at most thirty"), ApexQualifyingLapAllowance::Clamp(99), 30);
	{
		// The race-control rules: the table and each key only when set.
		auto CreateWithRules = [](const FApexRaceRules& Rules)
		{
			return ApexProtocol::EncodeCreateSession(
				TEXT("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"), 8, 3, 5, EApexSessionKind::Multiplayer,
				FApexAllowedAssists(), FApexSessionConditions(), EApexDamageLevel::Full, ApexAiSkill::Mixed, 0,
				TArray<FString>(), 0, FApexWeekendFormat(), nullptr, ApexQualifyingCutoff::None, 0, false,
				EApexQualifyingFormat::Open, 0, Rules);
		};
		FApexRaceRules Start;
		Start.Start = EApexStartType::Rolling;
		Start.bFormationLap = true;
		Start.bJumpStart = true;
		CheckBytes(TEXT("CreateSession with a rolling start, a formation lap and false starts"), CreateWithRules(Start),
			ApexGolden::C_CreateSessionRaceRules);
		{
			// A multi-class field: each class's AI count and its own level
			// only when it has one; a field of one class leaves the list off.
			TArray<FApexClassEntry> Classes;
			FApexClassEntry Lmp2;
			Lmp2.Class = TEXT("LMP2");
			Lmp2.AiCount = 4;
			Lmp2.AiSkill = 100;
			FApexClassEntry Gt3;
			Gt3.Class = TEXT("GT3");
			Gt3.AiCount = 6;
			Classes.Add(Lmp2);
			Classes.Add(Gt3);
			CheckBytes(TEXT("CreateSession with two classes"),
				ApexProtocol::EncodeCreateSession(
					TEXT("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"), 12, 10, 5, EApexSessionKind::Multiplayer,
					FApexAllowedAssists(), FApexSessionConditions(), EApexDamageLevel::Full, ApexAiSkill::Mixed, 0,
					TArray<FString>(), 0, FApexWeekendFormat(), nullptr, ApexQualifyingCutoff::None, 0, false,
					EApexQualifyingFormat::Open, 0, FApexRaceRules(), Classes),
				ApexGolden::C_CreateSessionClasses);
			const TArray<FApexClassEntry> One = { Gt3 };
			CheckBytes(TEXT("CreateSession with one class has no classes"),
				ApexProtocol::EncodeCreateSession(
					TEXT("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"), 8, 3, 5, EApexSessionKind::Multiplayer,
					FApexAllowedAssists(), FApexSessionConditions(), EApexDamageLevel::Reduced, ApexAiSkill::Mixed, 0,
					TArray<FString>(), 0, FApexWeekendFormat(), nullptr, ApexQualifyingCutoff::None, 0, false,
					EApexQualifyingFormat::Open, 0, FApexRaceRules(), One),
				ApexGolden::C_CreateSessionDamage);
		}
		FApexRaceRules Penalties;
		Penalties.bYellowPenalty = true;
		Penalties.bPitExitPenalty = true;
		CheckBytes(TEXT("CreateSession penalising yellow passes and the pit exit light"), CreateWithRules(Penalties),
			ApexGolden::C_CreateSessionPenalties);
		CheckBytes(TEXT("CreateSession under the default rules has no race_rules"),
			ApexProtocol::EncodeCreateSession(
				TEXT("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"), 8, 3, 5, EApexSessionKind::Multiplayer,
				FApexAllowedAssists(), FApexSessionConditions(), EApexDamageLevel::Full, ApexAiSkill::Mixed, 0,
				TArray<FString>(), 0, FApexWeekendFormat(), nullptr, ApexQualifyingCutoff::None, 2, true,
				EApexQualifyingFormat::Open, 0, FApexRaceRules()),
			ApexGolden::C_CreateSessionPitRule);
		TestTrue(TEXT("default rules are default"), FApexRaceRules().IsDefault());
		TestEqual(TEXT("default rules name nothing"), FApexRaceRules().Describe(), FString());
		FApexRaceRules All;
		All.Start = EApexStartType::Rolling;
		All.SafetyCar = EApexSafetyCar::Full;
		All.ReverseGrid = FApexRaceRules::ReverseWholeField;
		TestEqual(TEXT("rules described"), All.Describe(), FString(TEXT("rolling start · safety car · reverse grid")));
		TestEqual(TEXT("a reverse of eight"), FApexRaceRules::DescribeReverse(8), FString(TEXT("reverse 8")));
		FApexRaceRules One;
		One.ReverseGrid = 1;
		TestTrue(TEXT("a reverse of one changes nothing"), One.Normalised().IsDefault());

		// The tyre, parc fermé and stint rules: each key after the others,
		// in the server's order.
		FApexRaceRules Tyres;
		Tyres.QualifyingTyreSets = 3;
		Tyres.QualifyingTyreStart = 10;
		Tyres.bParcFerme = true;
		Tyres.WeekendTyreSets = 8;
		Tyres.MaxStintLaps = 25;
		Tyres.MaxStintMinutes = 65;
		CheckBytes(TEXT("CreateSession with the tyre, parc ferme and stint rules"), CreateWithRules(Tyres),
			ApexGolden::C_CreateSessionTyreStintRules);
		TestEqual(TEXT("tyre rules described"), Tyres.Describe(),
			FString(TEXT("3 qualifying sets · top 10 start on qualifying tyres · 8 weekend sets · parc fermé · stints 25 laps or 65 min")));
		TestEqual(TEXT("minutes alone"), FApexRaceRules::DescribeStints(0, 45), FString(TEXT("stints 45 min")));

		// Ballast by driver: after the classes; a blank or empty entry is
		// not sent.
		const TArray<FApexDriverBallast> Ballast = { {TEXT("@host"), 30}, {TEXT(" "), 10}, {TEXT("@ai:1"), 15}, {TEXT("@ai:2"), 0} };
		CheckBytes(TEXT("CreateSession with ballast by driver"),
			ApexProtocol::EncodeCreateSession(
				TEXT("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"), 8, 3, 5, EApexSessionKind::Practice,
				FApexAllowedAssists(), FApexSessionConditions(), EApexDamageLevel::Full, ApexAiSkill::Mixed, 0,
				TArray<FString>(), 0, FApexWeekendFormat(), nullptr, ApexQualifyingCutoff::None, 0, false,
				EApexQualifyingFormat::Open, 0, FApexRaceRules(), TArray<FApexClassEntry>(), Ballast),
			ApexGolden::C_CreateSessionBallast);
	}
	{
		// A weekend whose practice and qualifying name their own damage rule.
		FApexWeekendFormat Weekend;
		Weekend.PracticeSeconds = 600;
		Weekend.QualifyingSeconds = { 600 };
		Weekend.bHasPracticeDamage = true;
		Weekend.PracticeDamage = EApexDamageLevel::Off;
		Weekend.bHasQualifyingDamage = true;
		Weekend.QualifyingDamage = EApexDamageLevel::Reduced;
		CheckBytes(TEXT("CreateSession for a weekend with each phase's damage"),
			ApexProtocol::EncodeCreateSession(
				TEXT("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"), 8, 3, 5, EApexSessionKind::Multiplayer,
				FApexAllowedAssists(), FApexSessionConditions(), EApexDamageLevel::Full, ApexAiSkill::Mixed, 0,
				TArray<FString>(), 0, Weekend),
			ApexGolden::C_CreateSessionWeekendDamage);
	}
	{
		// A weekend whose practice and qualifying name their own allowed aids.
		FApexWeekendFormat Weekend;
		Weekend.PracticeSeconds = 600;
		Weekend.QualifyingSeconds = { 600 };
		Weekend.bHasPracticeAssists = true;
		Weekend.PracticeAssists.bAbs = true;
		Weekend.PracticeAssists.bTractionControl = false;
		Weekend.PracticeAssists.bAutoGearbox = false;
		Weekend.PracticeAssists.bSteeringAssist = false;
		Weekend.PracticeAssists.bRacingLine = false;
		Weekend.bHasQualifyingAssists = true;
		Weekend.QualifyingAssists.bRacingLine = false;
		CheckBytes(TEXT("CreateSession for a weekend with each phase's aids"),
			ApexProtocol::EncodeCreateSession(
				TEXT("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"), 8, 3, 5, EApexSessionKind::Multiplayer,
				FApexAllowedAssists(), FApexSessionConditions(), EApexDamageLevel::Full, ApexAiSkill::Mixed, 0,
				TArray<FString>(), 0, Weekend),
			ApexGolden::C_CreateSessionWeekendAssists);

	}
	CheckBytes(TEXT("AdvanceWeekend"), ApexProtocol::EncodeAdvanceWeekend(), ApexGolden::C_AdvanceWeekend);
	CheckBytes(TEXT("GiveUpSeat"), ApexProtocol::EncodeGiveUpSeat(), ApexGolden::C_GiveUpSeat);

	{
		FApexCarSetup Setup;
		const int32 Clicks[] = { 1, -2, -3, 4, -5, 5, -1, 2, 3, -3, 0, 1, -4, 4, -2, 2, -1, -3, 1, 1, 2, -1, 2, 1, -2, 3, 1, -4, 1, -3, 2 };
		static_assert(UE_ARRAY_COUNT(Clicks) == FApexCarSetup::KnobCount, "the golden setup has a click per knob");
		for (int32 Index = 0; Index < FApexCarSetup::KnobCount; ++Index)
		{
			Setup.Clicks[Index] = Clicks[Index];
		}
		CheckBytes(TEXT("SetCarSetup"), ApexProtocol::EncodeSetCarSetup(Setup), ApexGolden::C_SetCarSetup);
	}

	CheckBytes(TEXT("HotlapRelocate"),
		ApexProtocol::EncodeHotlapRelocate(EApexHotlapDestination::Track),
		ApexGolden::C_HotlapRelocate);
	CheckBytes(TEXT("HotlapRelocate cold"),
		ApexProtocol::EncodeHotlapRelocate(EApexHotlapDestination::Track, true),
		ApexGolden::C_HotlapRelocateCold);
	CheckBytes(TEXT("RecoverCar track"),
		ApexProtocol::EncodeRecoverCar(EApexRecoverDestination::Track),
		ApexGolden::C_RecoverCarTrack);
	CheckBytes(TEXT("RecoverCar pits"),
		ApexProtocol::EncodeRecoverCar(EApexRecoverDestination::Pits),
		ApexGolden::C_RecoverCarPits);
	CheckBytes(TEXT("RequestGhost"), ApexProtocol::EncodeRequestGhost(), ApexGolden::C_RequestGhost);
	CheckBytes(TEXT("RequestTrackBundle"), ApexProtocol::EncodeRequestTrackBundle(0xC0FFEE01u, 3),
		ApexGolden::C_RequestTrackBundle);
	CheckBytes(TEXT("CreateSession with a start order"),
		ApexProtocol::EncodeCreateSession(
			TEXT("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"), 8, 2, 5, EApexSessionKind::Multiplayer,
			FApexAllowedAssists(), FApexSessionConditions(), EApexDamageLevel::Full, ApexAiSkill::Mixed, 0,
			{ TEXT("@ai:2"), TEXT("@host"), TEXT("Luna Swift") }),
		ApexGolden::C_CreateSessionGrid);
	CheckBytes(TEXT("RequestLeaderboard"),
		ApexProtocol::EncodeRequestLeaderboard(TEXT("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"), TEXT("99999999-8888-7777-6666-555555555555")),
		ApexGolden::C_RequestLeaderboard);
	CheckBytes(TEXT("RequestQualifyingResults"),
		ApexProtocol::EncodeRequestQualifyingResults(TEXT("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee")),
		ApexGolden::C_RequestQualifyingResults);

	return true;
}

// -----------------------------------------------------------------------------
// The setup's clicks: ranges, one-sided knobs, and the read-out.
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexCarSetupParcFermeTest,
	"ApexSim.Net.CarSetup.ParcFerme",
	ApexTestFlags)

bool FApexCarSetupParcFermeTest::RunTest(const FString& Parameters)
{
	// The knobs the server leaves free (car_setup.rs under_parc_ferme).
	TestTrue(TEXT("brake bias is free"), ApexCarSetup::FreeUnderParcFerme(ApexCarSetup::BrakeBias));
	TestTrue(TEXT("the front wing is free"), ApexCarSetup::FreeUnderParcFerme(ApexCarSetup::FrontWing));
	TestFalse(TEXT("the rear wing is held"), ApexCarSetup::FreeUnderParcFerme(ApexCarSetup::RearWing));
	TestFalse(TEXT("a spring is held"), ApexCarSetup::FreeUnderParcFerme(ApexCarSetup::SpringFront));
	FApexCarSetup Held;
	Held.SetClick(ApexCarSetup::SpringFront, 2);
	Held.SetClick(ApexCarSetup::BrakeBias, 1);
	FApexCarSetup Asked;
	Asked.SetClick(ApexCarSetup::SpringFront, -3);
	Asked.SetClick(ApexCarSetup::BrakeBias, -2);
	const FApexCarSetup Applied = Asked.UnderParcFerme(Held);
	TestEqual(TEXT("the spring stays held"), Applied.GetClick(ApexCarSetup::SpringFront), 2);
	TestEqual(TEXT("the bias is the driver's"), Applied.GetClick(ApexCarSetup::BrakeBias), -2);

	// Held from the first run in qualifying to the end of the race.
	FApexParcFerme Rule;
	Rule.Update(true, EApexGameMode::Qualification, false, Held);
	TestFalse(TEXT("not held in the garage"), Rule.bHeld);
	Rule.Update(true, EApexGameMode::Qualification, true, Held);
	TestTrue(TEXT("held once out"), Rule.bHeld);
	Rule.Update(true, EApexGameMode::Qualification, false, Asked);
	TestEqual(TEXT("at the setup it went out on"), Rule.Held.GetClick(ApexCarSetup::SpringFront), 2);
	Rule.Update(true, EApexGameMode::Grid, false, Asked);
	Rule.Update(true, EApexGameMode::Countdown, false, Asked);
	Rule.Update(true, EApexGameMode::Race, true, Asked);
	TestTrue(TEXT("held through the race"), Rule.bHeld);
	Rule.Update(true, EApexGameMode::Lobby, false, Asked);
	TestFalse(TEXT("released after it"), Rule.bHeld);
	Rule.Update(true, EApexGameMode::Race, true, Asked);
	TestFalse(TEXT("a race without qualifying holds nothing"), Rule.bHeld);
	Rule.Update(false, EApexGameMode::Qualification, true, Asked);
	TestFalse(TEXT("no rule, no hold"), Rule.bHeld);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexCarSetupClicksTest,
	"ApexSim.Net.CarSetup.Clicks",
	ApexTestFlags)

bool FApexCarSetupClicksTest::RunTest(const FString& Parameters)
{
	FApexCarSetup Setup;
	TestTrue(TEXT("a new setup is stock"), Setup.IsStock());
	TestEqual(TEXT("a new setup has every knob"), Setup.Clicks.Num(), FApexCarSetup::KnobCount);

	TestTrue(TEXT("a click within range moves the knob"), Setup.SetClick(ApexCarSetup::SpringFront, 2));
	TestFalse(TEXT("the same value again is no change"), Setup.SetClick(ApexCarSetup::SpringFront, 2));
	TestEqual(TEXT("one knob changed"), Setup.CountChanged(), 1);

	Setup.SetClick(ApexCarSetup::SpringRear, 40);
	TestEqual(TEXT("a wild click is pinned to the top of the range"), Setup.GetClick(ApexCarSetup::SpringRear), 5);
	Setup.SetClick(ApexCarSetup::RevLimiter, 3);
	TestEqual(TEXT("the rev limiter cannot be raised"), Setup.GetClick(ApexCarSetup::RevLimiter), 0);
	Setup.SetClick(ApexCarSetup::TorqueMap, -9);
	TestEqual(TEXT("the torque map bottoms out at five clicks"), Setup.GetClick(ApexCarSetup::TorqueMap), -5);

	// A slot from an older build may hold too few knobs; Clamp fills it in.
	FApexCarSetup Short;
	Short.Clicks.SetNum(3);
	Short.Clicks[0] = 99;
	Short.Clamp();
	TestEqual(TEXT("clamp restores the knob count"), Short.Clicks.Num(), FApexCarSetup::KnobCount);
	TestEqual(TEXT("clamp pins the values"), Short.GetClick(0), 5);
	TestEqual(TEXT("clamp zeroes the missing knobs"), Short.GetClick(FApexCarSetup::KnobCount - 1), 0);

	TestEqual(TEXT("stock reads as 0"), ApexCarSetup::Describe(ApexCarSetup::SpringFront, 0), FString(TEXT("0")));
	TestEqual(TEXT("clicks carry their effect"),
		ApexCarSetup::Describe(ApexCarSetup::SpringFront, 2), FString(TEXT("+2  (+8%)")));
	TestEqual(TEXT("the limiter reads in rpm"),
		ApexCarSetup::Describe(ApexCarSetup::RevLimiter, -3), FString(TEXT("-3  (-300 rpm)")));
	TestEqual(TEXT("pressures read in kPa"),
		ApexCarSetup::Describe(ApexCarSetup::TyrePressureRear, -1), FString(TEXT("-1  (-5 kPa)")));

	// The wire key table must line up with the enum it is indexed by.
	TestEqual(TEXT("first key"), FString(ApexCarSetup::Knob(ApexCarSetup::TyrePressureFront).Key), FString(TEXT("tyre_pressure_front")));
	TestEqual(TEXT("anti-roll key"), FString(ApexCarSetup::Knob(ApexCarSetup::AntiRollRear).Key), FString(TEXT("anti_roll_rear")));
	TestEqual(TEXT("fuel key"), FString(ApexCarSetup::Knob(ApexCarSetup::FuelLoad).Key), FString(TEXT("fuel_load")));
	TestEqual(TEXT("fuel reads in laps"),
		ApexCarSetup::Describe(ApexCarSetup::FuelLoad, 2), FString(TEXT("+2  (+2 laps)")));
	TestEqual(TEXT("wing key"), FString(ApexCarSetup::Knob(ApexCarSetup::FrontWing).Key), FString(TEXT("front_wing")));
	TestEqual(TEXT("compound key"), FString(ApexCarSetup::Knob(ApexCarSetup::TyreCompound).Key), FString(TEXT("tyre_compound")));
	TestEqual(TEXT("ducts key"), FString(ApexCarSetup::Knob(ApexCarSetup::BrakeDucts).Key), FString(TEXT("brake_ducts")));
	TestEqual(TEXT("rear ducts key"), FString(ApexCarSetup::Knob(ApexCarSetup::BrakeDuctsRear).Key), FString(TEXT("brake_ducts_rear")));
	TestEqual(TEXT("pads key"), FString(ApexCarSetup::Knob(ApexCarSetup::BrakePads).Key), FString(TEXT("brake_pads")));
	TestEqual(TEXT("last key"), FString(ApexCarSetup::Knob(ApexCarSetup::Radiator).Key), FString(TEXT("radiator")));
	TestEqual(TEXT("pads read by name"), ApexCarSetup::Describe(ApexCarSetup::BrakePads, 1), FString(TEXT("Sprint")));
	TestEqual(TEXT("standard pads"), ApexCarSetup::Describe(ApexCarSetup::BrakePads, 0), FString(TEXT("Standard")));
	TestEqual(TEXT("endurance pads"), ApexCarSetup::Describe(ApexCarSetup::BrakePads, -1), FString(TEXT("Endurance")));
	Setup.SetClick(ApexCarSetup::BrakePads, 3);
	TestEqual(TEXT("the pads stop at sprint"), Setup.GetClick(ApexCarSetup::BrakePads), 1);
	TestEqual(TEXT("radiator reads in percent"), ApexCarSetup::Describe(ApexCarSetup::Radiator, -2), FString(TEXT("-2  (-16%)")));
	TestEqual(TEXT("ducts read in air"), ApexCarSetup::Describe(ApexCarSetup::BrakeDucts, 2), FString(TEXT("+2  (+20% air)")));
	TestEqual(TEXT("compound reads by name"), ApexCarSetup::Describe(ApexCarSetup::TyreCompound, 1), FString(TEXT("Soft")));
	TestEqual(TEXT("hard"), ApexCarSetup::Describe(ApexCarSetup::TyreCompound, -1), FString(TEXT("Hard")));
	TestEqual(TEXT("inters"), ApexCarSetup::Describe(ApexCarSetup::TyreCompound, -2), FString(TEXT("Intermediate")));
	TestEqual(TEXT("wets"), ApexCarSetup::Describe(ApexCarSetup::TyreCompound, -3), FString(TEXT("Wet")));
	TestTrue(TEXT("the compound reaches the wets, and a car's longer list"), ApexCarSetup::Knob(ApexCarSetup::TyreCompound).Min <= -3);
	TestEqual(TEXT("ride height reads in mm"),
		ApexCarSetup::Describe(ApexCarSetup::RideHeightFront, -3), FString(TEXT("-3  (-6 mm)")));
	TestEqual(TEXT("start mode by name"), ApexCarSetup::Describe(ApexCarSetup::ErsStartMode, -1), FString(TEXT("Harvest")));
	TestEqual(TEXT("deploy map by lean"), ApexCarSetup::Describe(ApexCarSetup::ErsDeployMap, 2), FString(TEXT("Early 2")));
	TestEqual(TEXT("deploy map even"), ApexCarSetup::Describe(ApexCarSetup::ErsDeployMap, 0), FString(TEXT("Even")));
	TestEqual(TEXT("regen key"), FString(ApexCarSetup::Knob(ApexCarSetup::ErsRegen).Key), FString(TEXT("ers_regen")));
	return true;
}

// -----------------------------------------------------------------------------
// Decode: every server message, against bytes the server actually produces.
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexProtocolGoldenDecodeTest,
	"ApexSim.Net.Protocol.GoldenDecode",
	ApexTestFlags)

bool FApexProtocolGoldenDecodeTest::RunTest(const FString& Parameters)
{
	auto Decode = [this](const TCHAR* Label, TArrayView<const uint8> Bytes, FApexServerMessage& Out) -> bool
	{
		FString Error;
		const bool bOk = ApexProtocol::DecodeServerMessage(Bytes, Out, Error);
		TestTrue(FString::Printf(TEXT("%s decodes (%s)"), Label, *Error), bOk);
		return bOk;
	};

	{
		FApexServerMessage Message;
		if (Decode(TEXT("AuthSuccess"), ApexGolden::S_AuthSuccess, Message))
		{
			TestEqual(TEXT("AuthSuccess type"), Message.Type, EApexServerMessageType::AuthSuccess);
			TestEqual(TEXT("AuthSuccess.PlayerId"), Message.AuthSuccess.PlayerId, PlayerId);
			TestEqual(TEXT("AuthSuccess.ServerVersion"), Message.AuthSuccess.ServerVersion, static_cast<int64>(1));
			TestEqual(TEXT("AuthSuccess.ProtocolVersion"), Message.AuthSuccess.ProtocolVersion, 2);
			TestEqual(TEXT("AuthSuccess.UdpToken"), Message.AuthSuccess.UdpToken, FString(TEXT("udp-tok")));
			TestEqual(TEXT("AuthSuccess.UdpPort"), Message.AuthSuccess.UdpPort, 9001);
		}
	}

	{
		FApexServerMessage Message;
		if (Decode(TEXT("AuthSuccess with a resume token"), ApexGolden::S_AuthSuccessResume, Message))
		{
			TestEqual(TEXT("AuthSuccess.ResumeToken"), Message.AuthSuccess.ResumeToken, FString(TEXT("resume-tok")));
			TestEqual(TEXT("AuthSuccess.UdpPort (resume)"), Message.AuthSuccess.UdpPort, 9001);
			TestTrue(TEXT("no key, no challenge"), Message.AuthSuccess.KeyChallenge.IsEmpty());
		}
	}

	{
		FApexServerMessage Message;
		if (Decode(TEXT("AuthSuccess with a key challenge"), ApexGolden::S_AuthSuccessChallenge, Message))
		{
			TestEqual(TEXT("AuthSuccess.KeyChallenge"), Message.AuthSuccess.KeyChallenge, FString::ChrN(32, 'e').Replace(TEXT("e"), TEXT("ef")));
		}
	}

	{
		FApexServerMessage Message;
		if (Decode(TEXT("RejoinAvailable"), ApexGolden::S_RejoinAvailable, Message))
		{
			TestEqual(TEXT("RejoinAvailable type"), Message.Type, EApexServerMessageType::RejoinAvailable);
			TestEqual(TEXT("RejoinAvailable.SessionId"), Message.SessionId, FString(TEXT("01234567-89ab-cdef-0123-456789abcdef")));
			TestEqual(TEXT("RejoinAvailable.TrackId"), Message.TrackId, FString(TEXT("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee")));
			TestEqual(TEXT("RejoinAvailable.SessionKind"), Message.SessionKind, EApexSessionKind::Multiplayer);
		}
	}

	{
		FApexServerMessage Message;
		if (Decode(TEXT("AuthFailure"), ApexGolden::S_AuthFailure, Message))
		{
			TestEqual(TEXT("AuthFailure type"), Message.Type, EApexServerMessageType::AuthFailure);
			TestEqual(TEXT("AuthFailure.reason"), Message.Reason, FString(TEXT("protocol version mismatch")));
		}
	}

	{
		// A unit variant: a one-key envelope with no "data" at all.
		FApexServerMessage Message;
		if (Decode(TEXT("SessionLeft"), ApexGolden::S_SessionLeft, Message))
		{
			TestEqual(TEXT("SessionLeft type"), Message.Type, EApexServerMessageType::SessionLeft);
		}
	}

	{
		FApexServerMessage Message;
		if (Decode(TEXT("SessionJoined"), ApexGolden::S_SessionJoined, Message))
		{
			TestEqual(TEXT("SessionJoined type"), Message.Type, EApexServerMessageType::SessionJoined);
			TestEqual(TEXT("SessionJoined.SessionId"), Message.SessionId, SessId);
			TestEqual(TEXT("SessionJoined.YourGridPosition"), Message.GridPosition, 3);
			TestTrue(TEXT("SessionJoined from an older server allows every assist"),
				Message.AllowedAssists.AllowsEverything());
		}
	}

	{
		FApexServerMessage Message;
		if (Decode(TEXT("SessionJoined with assists"), ApexGolden::S_SessionJoinedAssists, Message))
		{
			TestEqual(TEXT("SessionJoinedAssists type"), Message.Type, EApexServerMessageType::SessionJoined);
			TestEqual(TEXT("SessionJoinedAssists.SessionId"), Message.SessionId, SessId);
			TestEqual(TEXT("SessionJoinedAssists.YourGridPosition"), Message.GridPosition, 3);
			TestEqual(TEXT("SessionJoinedAssists.SessionKind"), Message.SessionKind, EApexSessionKind::Practice);
			// The snake_case keys under the PascalCase parent: get one wrong and
			// the field silently keeps its default (true), which is why each is
			// pinned, and the false ones in particular.
			TestFalse(TEXT("AllowedAssists.abs"), Message.AllowedAssists.bAbs);
			TestTrue(TEXT("AllowedAssists.traction_control"), Message.AllowedAssists.bTractionControl);
			TestFalse(TEXT("AllowedAssists.auto_gearbox"), Message.AllowedAssists.bAutoGearbox);
			TestTrue(TEXT("AllowedAssists.steering_assist"), Message.AllowedAssists.bSteeringAssist);
			TestFalse(TEXT("AllowedAssists.racing_line"), Message.AllowedAssists.bRacingLine);
			TestEqual(TEXT("Damage absent is full"), Message.Damage, EApexDamageLevel::Full);
			TestEqual(TEXT("AllowedAssists.CountLocked"), Message.AllowedAssists.CountLocked(), 3);
			TestEqual(TEXT("Conditions.weather"), Message.Conditions.Weather, EApexWeather::HeavyRain);
			TestEqual(TEXT("Conditions.time_of_day_minutes"), Message.Conditions.TimeOfDayMinutes, 6 * 60 + 15);
			TestEqual(TEXT("Conditions.Describe"), Message.Conditions.Describe(), FString(TEXT("Heavy rain · 06:15")));
		}
	}

	{
		// A driver's join names the session's tick rate and road for the
		// prediction, and the sky comes with its bytes as the server wrote
		// them; a join without Physics (above) leaves it not valid.
		FApexServerMessage Message;
		if (Decode(TEXT("SessionJoined with physics"), ApexGolden::S_SessionJoinedPhysics, Message))
		{
			TestEqual(TEXT("Physics.GridPosition"), Message.GridPosition, 2);
			TestTrue(TEXT("Physics.bValid"), Message.Physics.bValid);
			TestEqual(TEXT("Physics.TickRateHz"), Message.Physics.TickRateHz, 420);
			TestEqual(TEXT("Physics.RoadContact"), Message.Physics.RoadContact, 1);
			// {weather: 0, time_of_day_minutes: 780}, named: 33 bytes.
			TestEqual(TEXT("ConditionsBytes size"), Message.ConditionsBytes.Num(), 33);
			TestTrue(TEXT("ConditionsBytes is the map"), Message.ConditionsBytes.Num() > 0 && Message.ConditionsBytes[0] == 0x82);
			TestTrue(TEXT("ConditionsBytes ends at 13:00"), Message.ConditionsBytes.Num() == 33 && Message.ConditionsBytes.Last() == 0x0C);
		}
		FApexServerMessage Older;
		if (Decode(TEXT("SessionJoined with physics, no bundle"), ApexGolden::S_SessionJoinedPhysics, Older))
		{
			TestEqual(TEXT("No bundle named"), Older.Physics.BundleCrc, 0u);
		}
		if (Decode(TEXT("SessionJoined without physics"), ApexGolden::S_SessionJoinedAssists, Older))
		{
			TestFalse(TEXT("No Physics from an older server"), Older.Physics.bValid);
		}
	}

	{
		// The track's physics bundle: named in the join, asked for a chunk at
		// a time, answered with the chunk or with none.
		FApexServerMessage Message;
		if (Decode(TEXT("SessionJoined with a bundle"), ApexGolden::S_SessionJoinedPhysicsBundle, Message))
		{
			TestEqual(TEXT("Bundle.TickRateHz"), Message.Physics.TickRateHz, 420);
			TestEqual(TEXT("Bundle.BundleCrc"), Message.Physics.BundleCrc, 0xC0FFEE01u);
			TestEqual(TEXT("Bundle.BundleBytes"), Message.Physics.BundleBytes, static_cast<int64>(2190000));
			TestEqual(TEXT("Bundle.BundleChunks"), Message.Physics.BundleChunks, 9);
		}
		FApexServerMessage Balanced;
		if (Decode(TEXT("SessionJoined with a balanced car"), ApexGolden::S_SessionJoinedPhysicsBalance, Balanced))
		{
			TestTrue(TEXT("Balance named"), Balanced.Physics.bBalanced);
			TestEqual(TEXT("Balance.BallastKg"), Balanced.Physics.BallastKg, 25.0f);
			TestEqual(TEXT("Balance.Power"), Balanced.Physics.Power, 0.95f);
			TestEqual(TEXT("Balance.Downforce"), Balanced.Physics.Downforce, 1.1f);
			TestEqual(TEXT("Balance: the rest as before"), Balanced.Physics.TickRateHz, 420);
		}
		TestFalse(TEXT("No balance without the key"), Message.Physics.bBalanced);
		FApexServerMessage Chunk;
		if (Decode(TEXT("TrackBundleChunk"), ApexGolden::S_TrackBundleChunk, Chunk))
		{
			TestEqual(TEXT("Chunk type"), Chunk.Type, EApexServerMessageType::TrackBundleChunk);
			TestEqual(TEXT("Chunk.Crc"), Chunk.TrackBundleChunk.Crc, 0xC0FFEE01u);
			TestEqual(TEXT("Chunk.Chunk"), Chunk.TrackBundleChunk.Chunk, 3);
			TestEqual(TEXT("Chunk.Chunks"), Chunk.TrackBundleChunk.Chunks, 9);
			TestEqual(TEXT("Chunk.Bytes"), Chunk.TrackBundleChunk.Bytes, TArray<uint8>({0x41, 0x50, 0x58, 0x54, 0x01}));
		}
		FApexServerMessage Missing;
		if (Decode(TEXT("TrackBundleChunk missing"), ApexGolden::S_TrackBundleChunkMissing, Missing))
		{
			TestEqual(TEXT("Missing.Chunks"), Missing.TrackBundleChunk.Chunks, 0);
			TestEqual(TEXT("Missing.Bytes"), Missing.TrackBundleChunk.Bytes.Num(), 0);
		}
	}

	{
		// The session's damage rule is named only when it is not full.
		FApexServerMessage Message;
		if (Decode(TEXT("SessionJoined without damage"), ApexGolden::S_SessionJoinedNoDamage, Message))
		{
			TestEqual(TEXT("Damage"), Message.Damage, EApexDamageLevel::Off);
			TestEqual(TEXT("NoDamage.CountLocked"), Message.AllowedAssists.CountLocked(), 0);
			TestEqual(TEXT("NoDamage.AiSkill (mixed when absent)"), Message.AiSkill, ApexAiSkill::Mixed);
		}
	}

	{
		// The 107% rule is named only when the session has one.
		FApexServerMessage Message;
		if (Decode(TEXT("SessionJoined with the 107% rule"), ApexGolden::S_SessionJoinedQualifyingCutoff, Message))
		{
			TestEqual(TEXT("QualifyingCutoffPct"), Message.QualifyingCutoffPct, 107);
		}
		FApexServerMessage Plain;
		if (Decode(TEXT("SessionJoined without a cut-off"), ApexGolden::S_SessionJoinedNoDamage, Plain))
		{
			TestEqual(TEXT("QualifyingCutoffPct (none when absent)"), Plain.QualifyingCutoffPct, 0);
			TestEqual(TEXT("MandatoryStops (none when absent)"), Plain.MandatoryStops, 0);
			TestFalse(TEXT("CompoundChange (false when absent)"), Plain.bCompoundChange);
		}
	}

	{
		// The pit rule: the stops and the second compound, and a roster
		// row that took the flag owing them.
		FApexServerMessage Message;
		if (Decode(TEXT("SessionJoined with the pit rule"), ApexGolden::S_SessionJoinedPitRule, Message))
		{
			TestEqual(TEXT("MandatoryStops"), Message.MandatoryStops, 2);
			TestTrue(TEXT("CompoundChange"), Message.bCompoundChange);
			TestEqual(TEXT("QualifyingFormat (open when absent)"), Message.QualifyingFormat, EApexQualifyingFormat::Open);
			TestEqual(TEXT("QualifyingLapAllowance (none when absent)"), Message.QualifyingLapAllowance, 0);
			TestTrue(TEXT("RaceRules (the defaults when absent)"), Message.RaceRules.IsDefault());
		}
	}

	{
		// The classes the server echoes, the quickest first; none for a field of one class.
		FApexServerMessage Joined;
		if (Decode(TEXT("SessionJoined with classes"), ApexGolden::S_SessionJoinedClasses, Joined))
		{
			TestEqual(TEXT("Classes.Num"), Joined.Classes.Num(), 2);
			if (Joined.Classes.Num() == 2)
			{
				TestEqual(TEXT("Classes[0].Class"), Joined.Classes[0].Class, FString(TEXT("LMP2")));
				TestEqual(TEXT("Classes[0].AiCount"), Joined.Classes[0].AiCount, 4);
				TestEqual(TEXT("Classes[0].AiSkill"), Joined.Classes[0].AiSkill, 100);
				TestEqual(TEXT("Classes[1].Class"), Joined.Classes[1].Class, FString(TEXT("GT3")));
				TestEqual(TEXT("Classes[1].AiCount"), Joined.Classes[1].AiCount, 6);
				TestEqual(TEXT("Classes[1].AiSkill"), Joined.Classes[1].AiSkill, ApexAiSkill::Mixed);
			}
		}
		FApexServerMessage Plain;
		if (Decode(TEXT("SessionJoined, one class"), ApexGolden::S_SessionJoinedAiSkill, Plain))
		{
			TestEqual(TEXT("Classes (none when absent)"), Plain.Classes.Num(), 0);
		}
	}

	{
		// The race event the session is run for; none for a session that is no event's.
		FApexServerMessage Joined;
		if (Decode(TEXT("SessionJoined with an event"), ApexGolden::S_SessionJoinedEvent, Joined))
		{
			TestTrue(TEXT("Event.IsEvent"), Joined.Event.IsEvent());
			TestEqual(TEXT("Event.Id"), Joined.Event.Id, FString(TEXT("autumn-cup")));
			TestEqual(TEXT("Event.Name"), Joined.Event.Name, FString(TEXT("Autumn Cup")));
			TestEqual(TEXT("Event.Phase"), Joined.Event.Phase, static_cast<int32>(EApexWeekendPhase::Race));
			TestFalse(TEXT("Event.IsDone"), Joined.Event.IsDone());
		}
		FApexServerMessage Plain;
		if (Decode(TEXT("SessionJoined, no event"), ApexGolden::S_SessionJoinedClasses, Plain))
		{
			TestFalse(TEXT("Event (none when absent)"), Plain.Event.IsEvent());
		}
	}

	{
		// The race-control rules the server echoes.
		FApexServerMessage Joined;
		if (Decode(TEXT("SessionJoined with race rules"), ApexGolden::S_SessionJoinedRaceRules, Joined))
		{
			TestEqual(TEXT("SessionJoined type"), Joined.Type, EApexServerMessageType::SessionJoined);
			TestEqual(TEXT("RaceRules.Start"), Joined.RaceRules.Start, EApexStartType::Rolling);
			TestTrue(TEXT("RaceRules.bFormationLap"), Joined.RaceRules.bFormationLap);
			TestTrue(TEXT("RaceRules.bJumpStart"), Joined.RaceRules.bJumpStart);
			TestFalse(TEXT("RaceRules.bYellowPenalty"), Joined.RaceRules.bYellowPenalty);
			TestFalse(TEXT("RaceRules.bPitExitPenalty"), Joined.RaceRules.bPitExitPenalty);
			TestEqual(TEXT("RaceRules.SafetyCar"), Joined.RaceRules.SafetyCar, EApexSafetyCar::Off);
			TestEqual(TEXT("RaceRules.ReverseGrid"), static_cast<int32>(Joined.RaceRules.ReverseGrid), 0);
		}
	}

	{
		// Ballast by driver, echoed.
		FApexServerMessage Joined;
		if (Decode(TEXT("SessionJoined with ballast"), ApexGolden::S_SessionJoinedBallast, Joined)
			&& TestEqual(TEXT("two drivers ballasted"), Joined.Ballast.Num(), 2))
		{
			TestEqual(TEXT("the host"), Joined.Ballast[0].Driver, FString(TEXT("@host")));
			TestEqual(TEXT("30 kg"), Joined.Ballast[0].Kg, 30);
			TestEqual(TEXT("the first AI"), Joined.Ballast[1].Driver, FString(TEXT("@ai:1")));
			TestEqual(TEXT("15 kg"), Joined.Ballast[1].Kg, 15);
		}
	}

	{
		// A one-lap shoot-out with a lap allowance, and its state.
		FApexServerMessage Joined;
		if (Decode(TEXT("SessionJoined for a shoot-out"), ApexGolden::S_SessionJoinedShootout, Joined))
		{
			TestEqual(TEXT("QualifyingFormat"), Joined.QualifyingFormat, EApexQualifyingFormat::Shootout);
			TestEqual(TEXT("QualifyingLapAllowance"), Joined.QualifyingLapAllowance, 3);
		}
		FApexServerMessage State;
		if (Decode(TEXT("ShootoutState"), ApexGolden::S_ShootoutState, State))
		{
			TestEqual(TEXT("ShootoutState type"), State.Type, EApexServerMessageType::ShootoutState);
			const FApexShootoutState& S = State.ShootoutState;
			TestTrue(TEXT("ShootoutState.bValid"), S.bValid);
			TestTrue(TEXT("ShootoutState.Order"), S.Order == TArray<int32>({ 2, 0, 1 }));
			TestEqual(TEXT("ShootoutState.Running"), S.Running, 2);
			TestEqual(TEXT("ShootoutState.Done"), S.Done, 1);
			TestTrue(TEXT("ShootoutState.bFlying"), S.bFlying);
			TestEqual(TEXT("car 1 runs third"), S.PlaceOf(1), 3);
			TestEqual(TEXT("car 5 does not run"), S.PlaceOf(5), 0);
		}
		FApexServerMessage Phase;
		if (Decode(TEXT("WeekendState under a phase's damage rule"), ApexGolden::S_WeekendStateDamage, Phase))
		{
			TestEqual(TEXT("WeekendState.Damage"), Phase.WeekendState.Damage, EApexDamageLevel::Off);
		}
		TestFalse(TEXT("WeekendState without a day's sky"), Phase.WeekendState.bHasConditions);
		FApexServerMessage Late;
		if (Decode(TEXT("WeekendState without a damage rule"), ApexGolden::S_WeekendStateLate, Late))
		{
			TestEqual(TEXT("WeekendState.Damage (full when absent)"), Late.WeekendState.Damage, EApexDamageLevel::Full);
		}
	}
	{
		// A weekend whose practice and qualifying name their own allowed aids.
		FApexServerMessage Joined;
		if (Decode(TEXT("SessionJoined echoing each phase's aids"), ApexGolden::S_SessionJoinedWeekendAssists, Joined))
		{
			const FApexWeekendFormat& Echo = Joined.Weekend;
			TestTrue(TEXT("Weekend.bHasPracticeAssists"), Echo.bHasPracticeAssists);
			TestTrue(TEXT("practice allows ABS"), Echo.PracticeAssists.bAbs);
			TestEqual(TEXT("and nothing else"), Echo.PracticeAssists.CountLocked(), 4);
			TestTrue(TEXT("Weekend.bHasQualifyingAssists"), Echo.bHasQualifyingAssists);
			TestFalse(TEXT("qualifying without the line"), Echo.QualifyingAssists.bRacingLine);
			TestEqual(TEXT("qualifying locks one"), Echo.QualifyingAssists.CountLocked(), 1);
		}
		FApexServerMessage State;
		if (Decode(TEXT("WeekendState under a phase's aids"), ApexGolden::S_WeekendStateAssists, State))
		{
			TestTrue(TEXT("WeekendState.bHasAllowedAssists"), State.WeekendState.bHasAllowedAssists);
			TestTrue(TEXT("WeekendState.AllowedAssists.bAbs"), State.WeekendState.AllowedAssists.bAbs);
			TestEqual(TEXT("WeekendState.AllowedAssists locks four"), State.WeekendState.AllowedAssists.CountLocked(), 4);
		}
		FApexServerMessage Plain;
		if (Decode(TEXT("WeekendState with no aids of its own"), ApexGolden::S_WeekendStateDamage, Plain))
		{
			TestFalse(TEXT("WeekendState without aids"), Plain.WeekendState.bHasAllowedAssists);
		}
	}
	{
		FApexServerMessage Day;
		if (Decode(TEXT("WeekendState under a day's own sky"), ApexGolden::S_WeekendStateConditions, Day))
		{
			TestTrue(TEXT("WeekendState.bHasConditions"), Day.WeekendState.bHasConditions);
			TestEqual(TEXT("WeekendState.Conditions.Weather"), static_cast<int32>(Day.WeekendState.Conditions.Weather), 4);
			TestEqual(TEXT("WeekendState.Conditions.TimeOfDayMinutes"), Day.WeekendState.Conditions.TimeOfDayMinutes, 540);
		}
		FApexServerMessage Roster;
		if (Decode(TEXT("SessionRoster with a missed pit rule"), ApexGolden::S_SessionRosterMissedStops, Roster))
		{
			TestEqual(TEXT("roster entries"), Roster.Roster.Entries.Num(), 1);
			if (Roster.Roster.Entries.Num() == 1)
			{
				TestTrue(TEXT("MissedStops"), Roster.Roster.Entries[0].bMissedStops);
				TestFalse(TEXT("Dnq"), Roster.Roster.Entries[0].bDnq);
				TestEqual(TEXT("no penalties"), Roster.Roster.Entries[0].Penalties.Num(), 0);
			}
		}
		FApexServerMessage Penalised;
		if (Decode(TEXT("SessionRoster with penalties"), ApexGolden::S_SessionRosterPenalties, Penalised)
			&& TestEqual(TEXT("one entry"), Penalised.Roster.Entries.Num(), 1))
		{
			const FApexRosterEntry& Entry = Penalised.Roster.Entries[0];
			if (TestEqual(TEXT("two penalties"), Entry.Penalties.Num(), 2))
			{
				TestEqual(TEXT("a jump start"), Entry.Penalties[0].Reason, ApexPenalty::JumpStart);
				TestEqual(TEXT("a drive-through"), Entry.Penalties[0].Kind, ApexPenalty::DriveThrough);
				TestEqual(TEXT("served"), Entry.Penalties[0].Status, ApexPenalty::Served);
				TestEqual(TEXT("a pass under a yellow"), Entry.Penalties[1].Reason, ApexPenalty::YellowPass);
				TestEqual(TEXT("5 s"), Entry.Penalties[1].Seconds, 5);
				TestEqual(TEXT("on lap 4"), Entry.Penalties[1].Lap, 4);
			}
			TestEqual(TEXT("5 s added"), Entry.PenaltySeconds(), 5);
			TestNull(TEXT("none owed"), Entry.FirstOwed());
		}
	}

	{
		// Visual-only damage is the next integer.
		FApexServerMessage Message;
		if (Decode(TEXT("SessionJoined with visual damage"), ApexGolden::S_SessionJoinedVisualDamage, Message))
		{
			TestEqual(TEXT("Damage"), Message.Damage, EApexDamageLevel::VisualOnly);
		}
	}

	{
		// The AI field's level is named only when the host picked one.
		FApexServerMessage Message;
		if (Decode(TEXT("SessionJoined with an AI level"), ApexGolden::S_SessionJoinedAiSkill, Message))
		{
			TestEqual(TEXT("AiSkill"), Message.AiSkill, 95);
			TestEqual(TEXT("AiSkill.Damage"), Message.Damage, EApexDamageLevel::Full);
		}
	}

	{
		// A timed race's length is named only when the host picked one.
		FApexServerMessage Message;
		if (Decode(TEXT("SessionJoined for a timed race"), ApexGolden::S_SessionJoinedRaceTime, Message))
		{
			TestEqual(TEXT("RaceSeconds"), Message.RaceSeconds, 7200);
			TestEqual(TEXT("RaceSeconds.AiSkill"), Message.AiSkill, ApexAiSkill::Mixed);
		}
		FApexServerMessage Laps;
		if (Decode(TEXT("SessionJoined over laps"), ApexGolden::S_SessionJoinedAiSkill, Laps))
		{
			TestEqual(TEXT("RaceSeconds (0 when absent)"), Laps.RaceSeconds, 0);
		}
	}

	{
		// Timed qualifying's length is named only when the host picked one.
		FApexServerMessage Message;
		if (Decode(TEXT("SessionJoined with timed qualifying"), ApexGolden::S_SessionJoinedQualifyingTime, Message))
		{
			TestEqual(TEXT("QualifyingSeconds"), Message.QualifyingSeconds, 900);
			TestEqual(TEXT("QualifyingSeconds.RaceSeconds"), Message.RaceSeconds, 0);
		}
		FApexServerMessage Open;
		if (Decode(TEXT("SessionJoined, open qualifying"), ApexGolden::S_SessionJoinedAiSkill, Open))
		{
			TestEqual(TEXT("QualifyingSeconds (0 when absent)"), Open.QualifyingSeconds, 0);
		}
	}

	{
		// A race weekend's format, and where it stands.
		FApexServerMessage Message;
		if (Decode(TEXT("SessionJoined for a race weekend"), ApexGolden::S_SessionJoinedWeekend, Message))
		{
			TestTrue(TEXT("Weekend"), Message.Weekend.IsWeekend());
			TestEqual(TEXT("Weekend.PracticeSeconds"), Message.Weekend.PracticeSeconds, 1800);
			TestEqual(TEXT("Weekend.Rounds"), Message.Weekend.Rounds(), 3);
			if (Message.Weekend.Rounds() == 3)
			{
				TestEqual(TEXT("Weekend.Q1"), Message.Weekend.QualifyingSeconds[0], 1080);
				TestEqual(TEXT("Weekend.Q3"), Message.Weekend.QualifyingSeconds[2], 720);
			}
		}
		FApexServerMessage Plain;
		if (Decode(TEXT("SessionJoined, no weekend"), ApexGolden::S_SessionJoinedAiSkill, Plain))
		{
			TestFalse(TEXT("no weekend when absent"), Plain.Weekend.IsWeekend());
		}
		FApexServerMessage State;
		if (Decode(TEXT("WeekendState"), ApexGolden::S_WeekendState, State))
		{
			TestEqual(TEXT("WeekendState type"), State.Type, EApexServerMessageType::WeekendState);
			const FApexWeekendState& W = State.WeekendState;
			TestTrue(TEXT("WeekendState.bValid"), W.bValid);
			TestEqual(TEXT("WeekendState.Phase"), W.Phase, EApexWeekendPhase::Qualifying);
			TestEqual(TEXT("WeekendState.Round"), W.Round, 2);
			TestEqual(TEXT("WeekendState.Rounds"), W.Rounds, 3);
			TestFalse(TEXT("WeekendState.bFinished"), W.bFinished);
			TestEqual(TEXT("WeekendState.Advancing"), W.Advancing, 10);
			if (TestEqual(TEXT("WeekendState.Eliminated"), W.Eliminated.Num(), 1))
			{
				TestEqual(TEXT("Eliminated.CarIndex"), W.Eliminated[0].CarIndex, 7);
				TestEqual(TEXT("Eliminated.Round"), W.Eliminated[0].Round, 1);
				TestEqual(TEXT("Eliminated.GridPosition"), W.Eliminated[0].GridPosition, 16);
				TestEqual(TEXT("Eliminated.LapTimeMs"), W.Eliminated[0].LapTimeMs, 91234);
				TestFalse(TEXT("Eliminated.bLate (not named)"), W.Eliminated[0].bLate);
			}
		}
		FApexServerMessage Late;
		if (Decode(TEXT("WeekendState with a late entry"), ApexGolden::S_WeekendStateLate, Late)
			&& TestEqual(TEXT("one late entry"), Late.WeekendState.Eliminated.Num(), 1))
		{
			TestTrue(TEXT("Eliminated.bLate"), Late.WeekendState.Eliminated[0].bLate);
			TestEqual(TEXT("with no time"), Late.WeekendState.Eliminated[0].LapTimeMs, 0);
		}
		FApexServerMessage Lobby;
		if (Decode(TEXT("LobbyState with a weekend between phases"), ApexGolden::S_LobbyStateWeekendWaiting, Lobby)
			&& TestEqual(TEXT("one session"), Lobby.LobbyState.AvailableSessions.Num(), 1))
		{
			const FApexSessionSummary& Waiting = Lobby.LobbyState.AvailableSessions[0];
			TestEqual(TEXT("listed finished"), Waiting.State, EApexSessionState::Finished);
			TestTrue(TEXT("WeekendWaiting"), Waiting.bWeekendWaiting);
			TestTrue(TEXT("and joinable"), Waiting.IsJoinable());
		}
	}

	{
		// The create screen's ladder and read-out.
		TestEqual(TEXT("RaceLength.Step up"), ApexRaceLength::StepMinutes(60, 1), 75);
		TestEqual(TEXT("RaceLength.Step down"), ApexRaceLength::StepMinutes(60, -1), 45);
		TestEqual(TEXT("RaceLength.Step off the ladder"), ApexRaceLength::StepMinutes(100, 1), 120);
		TestEqual(TEXT("RaceLength.Step past the top"), ApexRaceLength::StepMinutes(1440, 1), 1440);
		TestEqual(TEXT("RaceLength.Step below the bottom"), ApexRaceLength::StepMinutes(5, -1), 5);
		TestEqual(TEXT("RaceLength.Clamp"), ApexRaceLength::Clamp(30), ApexRaceLength::MinSeconds);
		TestEqual(TEXT("RaceLength.Clamp laps"), ApexRaceLength::Clamp(0), 0);
		TestEqual(TEXT("RaceLength.Describe minutes"), ApexRaceLength::Describe(45 * 60), FString(TEXT("45 min")));
		TestEqual(TEXT("RaceLength.Describe hours"), ApexRaceLength::Describe(24 * 3600), FString(TEXT("24 h")));
		TestEqual(TEXT("RaceLength.Describe mixed"), ApexRaceLength::Describe(90 * 60), FString(TEXT("1 h 30")));
	}

	{
		// The stepper's walk: Mixed below the bottom, the top held.
		TestEqual(TEXT("AiSkill.Step up from mixed"), ApexAiSkill::Step(ApexAiSkill::Mixed, 1), ApexAiSkill::Min);
		TestEqual(TEXT("AiSkill.Step down from mixed"), ApexAiSkill::Step(ApexAiSkill::Mixed, -1), ApexAiSkill::Mixed);
		TestEqual(TEXT("AiSkill.Step below the bottom"), ApexAiSkill::Step(ApexAiSkill::Min, -1), ApexAiSkill::Mixed);
		TestEqual(TEXT("AiSkill.Step past the top"), ApexAiSkill::Step(ApexAiSkill::Max, 1), ApexAiSkill::Max);
		TestEqual(TEXT("AiSkill.Clamp"), ApexAiSkill::Clamp(150), ApexAiSkill::Max);
		TestEqual(TEXT("AiSkill.Label"), FString(ApexAiSkill::Label(95)), FString(TEXT("Pro")));
		TestEqual(TEXT("AiSkill.Label mixed"), FString(ApexAiSkill::Label(ApexAiSkill::Mixed)), FString(TEXT("Mixed")));
	}

	{
		// A resolved session names its air, and the read-out says it.
		FApexServerMessage Message;
		if (Decode(TEXT("SessionJoined with the air"), ApexGolden::S_SessionJoinedAir, Message))
		{
			TestEqual(TEXT("Air.weather"), Message.Conditions.Weather, EApexWeather::Overcast);
			TestEqual(TEXT("Air.air_temp_c (signed)"), Message.Conditions.AirTempC, -3);
			TestEqual(TEXT("Air.humidity_pct"), Message.Conditions.HumidityPct, 80);
			TestEqual(TEXT("Air.wind_kph"), Message.Conditions.WindKph, 22);
			TestEqual(TEXT("Air.wind_from_deg"), Message.Conditions.WindFromDeg, 270);
			TestEqual(TEXT("Air.Describe"), Message.Conditions.Describe(),
				FString(TEXT("Overcast · 08:00 · -3°C · wind 22 km/h from the right")));
			TestFalse(TEXT("Air: no clock speed named"), Message.Conditions.HasTimeScale());
			TestFalse(TEXT("Air: no forecast named"), Message.Conditions.HasChangeable());
			TestFalse(TEXT("Air: no rubber named"), Message.Conditions.HasTrackRubber());
		}
	}

	{
		// A changing sky's echo, and the read-out that names it.
		FApexServerMessage Message;
		if (Decode(TEXT("SessionJoined with a changing sky"), ApexGolden::S_SessionJoinedSky, Message))
		{
			TestEqual(TEXT("Sky.weather"), Message.Conditions.Weather, EApexWeather::Cloudy);
			TestEqual(TEXT("Sky.time_of_day"), Message.Conditions.TimeOfDayMinutes, 15 * 60);
			TestEqual(TEXT("Sky.time_scale"), Message.Conditions.TimeScale, 24);
			TestEqual(TEXT("Sky.changeable"), Message.Conditions.Changeable, 2);
			TestEqual(TEXT("Sky.track_rubber_pct"), Message.Conditions.TrackRubberPct, 90);
			TestEqual(TEXT("Sky.Describe"), Message.Conditions.Describe(),
				FString(TEXT("Cloudy · 15:00 · clock x24 · changeable weather · track 90% rubber")));
		}
		FApexSessionConditions Green;
		Green.TrackRubberPct = 0;
		Green.TimeScale = 0;
		Green.Changeable = 0;
		TestEqual(TEXT("A frozen clock and a fixed sky say nothing; a green track does"), Green.Describe(),
			FString(TEXT("Sunny · 13:00 · green track")));
	}

	{
		// The echo names the date and the circuit's solar noon; an older
		// echo names neither (13:00 on the model day).
		FApexServerMessage Message;
		if (Decode(TEXT("SessionJoined with the sun"), ApexGolden::S_SessionJoinedSun, Message))
		{
			TestEqual(TEXT("Sun.day_of_year"), Message.Conditions.DayOfYear, 200);
			TestEqual(TEXT("Sun.solar_noon_min"), Message.Conditions.SolarNoonMin, 13 * 60 + 33);
			TestTrue(TEXT("Sun.sky_seed kept"), Message.Conditions.HasSkySeed());
		}
		FApexServerMessage Older;
		if (Decode(TEXT("SessionJoined without the sun"), ApexGolden::S_SessionJoinedSky, Older))
		{
			TestFalse(TEXT("no date"), Older.Conditions.HasDate());
			TestFalse(TEXT("no solar noon"), Older.Conditions.HasSolarNoon());
			TestFalse(TEXT("no forecast"), Older.Conditions.HasForecast());
		}
		// The echo of a host's own forecast.
		FApexServerMessage Written;
		if (Decode(TEXT("SessionJoined with a written forecast"), ApexGolden::S_SessionJoinedForecast, Written))
		{
			const TArray<FApexForecastStep>& Steps = Written.Conditions.Forecast;
			if (TestEqual(TEXT("Forecast steps"), Steps.Num(), 3))
			{
				TestEqual(TEXT("Forecast[0].AtMin"), Steps[0].AtMin, 20);
				TestEqual(TEXT("Forecast[0].Weather"), Steps[0].Weather, EApexWeather::LightRain);
				TestEqual(TEXT("Forecast[1].Weather"), Steps[1].Weather, EApexWeather::HeavyRain);
				TestEqual(TEXT("Forecast[2].AtMin"), Steps[2].AtMin, 90);
				TestEqual(TEXT("Forecast[2].Weather"), Steps[2].Weather, EApexWeather::Sunny);
			}
			TestTrue(TEXT("Describe names the written forecast"),
				Written.Conditions.Describe().Contains(TEXT(" · forecast light rain at 20 min, heavy rain at ")));
		}
	}

	{
		// A message from a server without the field is a sunny afternoon.
		FApexServerMessage Message;
		if (Decode(TEXT("SessionJoined without conditions"), ApexGolden::S_SessionJoined, Message))
		{
			TestTrue(TEXT("Conditions default"), Message.Conditions.IsDefault());
			TestEqual(TEXT("Conditions default clock"), Message.Conditions.ClockText(), FString(TEXT("13:00")));
		}
	}

	{
		FApexServerMessage Message;
		if (Decode(TEXT("Error"), ApexGolden::S_Error, Message))
		{
			TestEqual(TEXT("Error type"), Message.Type, EApexServerMessageType::Error);
			TestEqual(TEXT("Error.code"), Message.ErrorCode, 404);
			TestEqual(TEXT("Error.message"), Message.Reason, FString(TEXT("session not found")));
		}
	}

	{
		FApexServerMessage Message;
		if (Decode(TEXT("HeartbeatAck"), ApexGolden::S_HeartbeatAck, Message))
		{
			TestEqual(TEXT("HeartbeatAck type"), Message.Type, EApexServerMessageType::HeartbeatAck);
			TestEqual(TEXT("HeartbeatAck.server_tick"), Message.ServerTick, static_cast<int64>(4242));
		}
	}

	{
		FApexServerMessage Message;
		if (Decode(TEXT("ServerNotice"), ApexGolden::S_ServerNotice, Message))
		{
			TestEqual(TEXT("ServerNotice type"), Message.Type, EApexServerMessageType::ServerNotice);
			TestEqual(TEXT("ServerNotice.message"), Message.Reason, FString(TEXT("Race starts in 5 minutes")));
			TestEqual(TEXT("ServerNotice carries no error code"), Message.ErrorCode, 0);
		}
	}

	{
		FApexServerMessage Message;
		if (Decode(TEXT("Chat (lobby)"), ApexGolden::S_ChatLobby, Message))
		{
			TestEqual(TEXT("Chat type"), Message.Type, EApexServerMessageType::Chat);
			TestTrue(TEXT("Chat decoded"), Message.Chat.bValid);
			TestEqual(TEXT("Chat.PlayerId"), Message.Chat.PlayerId, FString(TEXT("12345678-1234-5678-1234-567812345678")));
			TestEqual(TEXT("Chat.From"), Message.Chat.From, FString(TEXT("Racer")));
			TestEqual(TEXT("Chat.Text"), Message.Chat.Text, FString(TEXT("good luck")));
			TestFalse(TEXT("said in the lobby"), Message.Chat.bInSession);
		}
		FApexServerMessage InSession;
		if (Decode(TEXT("Chat (session)"), ApexGolden::S_ChatSession, InSession))
		{
			TestTrue(TEXT("said in a session"), InSession.Chat.bInSession);
		}
	}

	{
		FApexServerMessage Message;
		if (Decode(TEXT("Rewound"), ApexGolden::S_Rewound, Message))
		{
			TestEqual(TEXT("Rewound type"), Message.Type, EApexServerMessageType::Rewound);
			TestTrue(TEXT("Rewound decoded"), Message.Rewound.bValid);
			TestEqual(TEXT("Rewound.Tick"), Message.Rewound.Tick, 4200u);
			TestEqual(TEXT("Rewound.FromTick"), Message.Rewound.FromTick, 8610u);
			TestEqual(TEXT("Rewound.HoldMs"), Message.Rewound.HoldMs, 3000u);
		}
	}

	{
		// The championships: rounds and standings, best first.
		FApexServerMessage Message;
		if (Decode(TEXT("Championships"), ApexGolden::S_Championships, Message))
		{
			const FApexChampionships& All = Message.Championships;
			TestEqual(TEXT("Championships type"), Message.Type, EApexServerMessageType::Championships);
			TestTrue(TEXT("Championships decoded"), All.bValid);
			if (TestEqual(TEXT("one championship"), All.Championships.Num(), 1))
			{
				const FApexChampionship& Series = All.Championships[0];
				TestEqual(TEXT("Id"), Series.Id, FString(TEXT("autumn-series")));
				TestEqual(TEXT("rounds"), Series.Rounds.Num(), 2);
				TestTrue(TEXT("round 1 raced"), Series.Rounds[0].bRaced);
				TestFalse(TEXT("round 2 not"), Series.Rounds[1].bRaced);
				TestEqual(TEXT("round 2 at practice"), Series.Rounds[1].Phase, 1);
				if (TestEqual(TEXT("standings"), Series.Standings.Num(), 2))
				{
					TestEqual(TEXT("leader"), Series.Standings[0].Name, FString(TEXT("Ada")));
					TestEqual(TEXT("points"), Series.Standings[0].Points, 25);
					TestEqual(TEXT("wins"), Series.Standings[0].Wins, 1);
					TestFalse(TEXT("a human"), Series.Standings[0].bAi);
					TestTrue(TEXT("an AI"), Series.Standings[1].bAi);
					TestEqual(TEXT("results"), Series.Standings[1].Results.Num(), 2);
					TestEqual(TEXT("second in round 1"), Series.Standings[1].Results[0], 2);
				}
				TestEqual(TEXT("no fastest-lap points"), Series.FastestLapPoints, 0);
				TestTrue(TEXT("no fastest lap named"), Series.Rounds[0].FastestLap.IsEmpty());
			}
		}
	}

	{
		// The fastest-lap rule and who scored it: a point at any place, the AI driver's lap in round 1.
		FApexServerMessage Message;
		if (Decode(TEXT("ChampionshipsFastestLap"), ApexGolden::S_ChampionshipsFastestLap, Message)
			&& TestEqual(TEXT("one championship"), Message.Championships.Championships.Num(), 1))
		{
			const FApexChampionship& Series = Message.Championships.Championships[0];
			TestEqual(TEXT("FastestLapPoints"), Series.FastestLapPoints, 1);
			TestTrue(TEXT("FastestLapAnyPlace"), Series.bFastestLapAnyPlace);
			if (TestEqual(TEXT("rounds"), Series.Rounds.Num(), 2) && TestEqual(TEXT("standings"), Series.Standings.Num(), 2))
			{
				TestEqual(TEXT("FastestLap"), Series.Rounds[0].FastestLap, FString(TEXT("Bot")));
				TestEqual(TEXT("FastestLapMs"), Series.Rounds[0].FastestLapMs, 81234);
				TestTrue(TEXT("FastestLapScored"), Series.Rounds[0].bFastestLapScored);
				TestFalse(TEXT("round 2 none"), Series.Rounds[1].bFastestLapScored);
				TestEqual(TEXT("Bot's points"), Series.Standings[1].Points, 19);
				TestEqual(TEXT("FastestLaps"), Series.Standings[1].FastestLaps, 1);
				TestEqual(TEXT("Ada none"), Series.Standings[0].FastestLaps, 0);
			}
		}
	}

	{
		// A race event's sheets: practice, the combined qualifying sheet, the race.
		FApexServerMessage Message;
		if (Decode(TEXT("EventSheets"), ApexGolden::S_EventSheets, Message))
		{
			const FApexEventSheets& Sheets = Message.EventSheets;
			TestEqual(TEXT("EventSheets type"), Message.Type, EApexServerMessageType::EventSheets);
			TestTrue(TEXT("EventSheets decoded"), Sheets.bValid);
			TestEqual(TEXT("Id"), Sheets.Id, FString(TEXT("autumn-cup")));
			TestEqual(TEXT("TrackId"), Sheets.TrackId, FString(TEXT("01234567-89ab-cdef-0123-456789abcdef")));
			TestEqual(TEXT("Phase"), Sheets.Phase, 4);
			if (TestEqual(TEXT("Phases.Num"), Sheets.Phases.Num(), 3))
			{
				TestEqual(TEXT("practice"), Sheets.Phases[0].Phase, 1);
				TestEqual(TEXT("practice best"), Sheets.Phases[0].Lines[0].BestLapMs, 81234);
				TestEqual(TEXT("no practice replay"), Sheets.Phases[0].Replays.Num(), 0);
				const FApexEventSheet& Quali = Sheets.Phases[1];
				TestEqual(TEXT("a replay a round"), Quali.Replays.Num(), 2);
				TestEqual(TEXT("Q2's replay"), Quali.Replays[1], FString(TEXT("replay_q2.apxs")));
				TestTrue(TEXT("pole: AI"), Quali.Lines[0].bAi);
				TestEqual(TEXT("out in Q1"), Quali.Lines[1].KnockedOutIn, 1);
				TestEqual(TEXT("CarId"), Quali.Lines[1].CarId, FString(TEXT("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee")));
				const FApexEventSheet& Race = Sheets.Phases[2];
				TestEqual(TEXT("FinishedAt"), Race.FinishedAt, static_cast<int64>(1793180000));
				TestEqual(TEXT("race time"), Race.Lines[0].RaceTimeMs, 812345);
				TestEqual(TEXT("finished"), Race.Lines[0].Status, FString(TEXT("finished")));
				TestEqual(TEXT("retired"), Race.Lines[1].Status, FString(TEXT("retired")));
				TestEqual(TEXT("laps"), Race.Lines[1].Laps, 6);
			}
		}
	}

	{
		// The race's incident list: cars by roster index and name, a car gone as NoCar.
		FApexServerMessage Message;
		if (Decode(TEXT("Incidents"), ApexGolden::S_Incidents, Message))
		{
			const FApexIncidents& List = Message.Incidents;
			TestEqual(TEXT("Incidents type"), Message.Type, EApexServerMessageType::Incidents);
			TestTrue(TEXT("Incidents decoded"), List.bValid);
			TestEqual(TEXT("TickRate"), List.TickRate, 420);
			TestEqual(TEXT("RaceStartTick"), List.RaceStartTick, static_cast<int64>(2520));
			if (TestEqual(TEXT("Incidents.Num"), List.Incidents.Num(), 3))
			{
				const FApexIncident& Contact = List.Incidents[0];
				TestEqual(TEXT("contact kind"), Contact.Kind, ApexIncident::Contact);
				TestEqual(TEXT("contact cars"), Contact.Car * 10 + Contact.Other, 3);
				TestEqual(TEXT("contact names"), Contact.Name + Contact.OtherName, FString(TEXT("AdaBea")));
				TestEqual(TEXT("contact severity"), Contact.Severity, ApexIncident::Heavy);
				TestEqual(TEXT("contact closing"), Contact.ClosingMps, 9.5f);
				TestEqual(TEXT("contact second of the race"), List.RaceSeconds(Contact), 6.0);
				const FApexIncident& Cut = List.Incidents[1];
				TestEqual(TEXT("cut kind"), Cut.Kind, ApexIncident::TrackLimits);
				TestEqual(TEXT("cut car gone"), Cut.Car, ApexIncident::NoCar);
				TestTrue(TEXT("cut"), Cut.bCut);
				TestEqual(TEXT("cut other (none)"), Cut.Other, ApexIncident::NoCar);
				const FApexIncident& Penalty = List.Incidents[2];
				TestEqual(TEXT("penalty kind"), Penalty.Kind, ApexIncident::Penalty);
				TestEqual(TEXT("penalty offence"), Penalty.Offence, ApexPenalty::YellowPass);
				TestEqual(TEXT("penalty paid by"), Penalty.Penalty, ApexPenalty::DriveThrough);
			}
		}
	}

	{
		FApexServerMessage Message;
		if (Decode(TEXT("GameModeChanged"), ApexGolden::S_GameModeChanged, Message))
		{
			TestEqual(TEXT("GameModeChanged type"), Message.Type, EApexServerMessageType::GameModeChanged);
			// Serialize_repr means this arrives as the integer 2, not "Countdown".
			TestEqual(TEXT("GameModeChanged.mode"), Message.GameMode, EApexGameMode::Countdown);
		}
	}

	{
		FApexServerMessage Message;
		if (Decode(TEXT("GameModeChanged to the grid"), ApexGolden::S_GameModeChangedGrid, Message))
		{
			TestEqual(TEXT("GameModeChanged.mode is the grid"), Message.GameMode, EApexGameMode::Grid);
		}
	}

	{
		FApexServerMessage Message;
		if (Decode(TEXT("CountdownUpdate"), ApexGolden::S_CountdownUpdate, Message))
		{
			TestEqual(TEXT("CountdownUpdate type"), Message.Type, EApexServerMessageType::CountdownUpdate);
			TestEqual(TEXT("CountdownUpdate.seconds_remaining"), Message.CountdownSeconds, 7);
		}
	}

	{
		FApexServerMessage Message;
		if (Decode(TEXT("PlayerDisconnected"), ApexGolden::S_PlayerDisconnected, Message))
		{
			TestEqual(TEXT("PlayerDisconnected type"), Message.Type, EApexServerMessageType::PlayerDisconnected);
			TestEqual(TEXT("PlayerDisconnected.PlayerId"), Message.PlayerId, PlayerId);
		}
	}

	return true;
}

// -----------------------------------------------------------------------------
// LobbyState gets its own test: it is the message the whole UI is built on,
// and the one where a PascalCase slip degrades silently into empty lists.
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexProtocolLobbyStateDecodeTest,
	"ApexSim.Net.Protocol.LobbyStateDecode",
	ApexTestFlags)

bool FApexProtocolLobbyStateDecodeTest::RunTest(const FString& Parameters)
{
	FApexServerMessage Message;
	FString Error;
	if (!TestTrue(
			FString::Printf(TEXT("LobbyState decodes (%s)"), *Error),
			ApexProtocol::DecodeServerMessage(ApexGolden::S_LobbyState, Message, Error)))
	{
		return false;
	}

	TestEqual(TEXT("LobbyState type"), Message.Type, EApexServerMessageType::LobbyState);

	const FApexLobbyState& Lobby = Message.LobbyState;
	TestEqual(TEXT("no recordings port from an older lobby"), Lobby.RecordingsPort, 0);
	{
		FApexServerMessage WithPort;
		TestTrue(TEXT("a lobby with its recordings port decodes"),
			ApexProtocol::DecodeServerMessage(ApexGolden::S_LobbyStateRecordings, WithPort, Error));
		TestEqual(TEXT("its recordings port"), WithPort.LobbyState.RecordingsPort, 9002);
		TestTrue(TEXT("and the live stream"), WithPort.LobbyState.bLiveStream);
	}
	TestFalse(TEXT("an older lobby does not balance its cars"), Lobby.bBalanced);
	TestEqual(TEXT("an older lobby lists no events"), Lobby.Events.Num(), 0);
	{
		FApexServerMessage Events;
		if (TestTrue(TEXT("a lobby listing race events decodes"),
				ApexProtocol::DecodeServerMessage(ApexGolden::S_LobbyStateEvents, Events, Error))
			&& TestEqual(TEXT("two events"), Events.LobbyState.Events.Num(), 2))
		{
			const FApexLobbyEvent& Cup = Events.LobbyState.Events[0];
			TestEqual(TEXT("Id"), Cup.Id, FString(TEXT("autumn-cup")));
			TestEqual(TEXT("Name"), Cup.Name, FString(TEXT("Autumn Cup")));
			TestEqual(TEXT("Phase"), Cup.Phase, 3);
			TestEqual(TEXT("TrackId"), Cup.TrackId, FString(TEXT("01234567-89ab-cdef-0123-456789abcdef")));
			TestEqual(TEXT("StartsAt"), Cup.StartsAt, static_cast<int64>(1793000000));
			TestEqual(TEXT("SessionId"), Cup.SessionId, FString(TEXT("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee")));
			TestEqual(TEXT("Entries / Slots"), Cup.Entries * 100 + Cup.Slots, 212);
			const FApexLobbyEvent& Spring = Events.LobbyState.Events[1];
			TestTrue(TEXT("Spring is over"), Spring.IsDone());
			TestEqual(TEXT("no start time"), Spring.StartsAt, static_cast<int64>(0));
			TestFalse(TEXT("no session"), Spring.HasSession());
			TestTrue(TEXT("FindEvent"), Events.LobbyState.FindEvent(TEXT("spring")) == &Spring);
		}
	}
	{
		FApexServerMessage Rules;
		if (TestTrue(TEXT("a lobby listing race rules decodes"),
				ApexProtocol::DecodeServerMessage(ApexGolden::S_LobbyStateRaceRules, Rules, Error))
			&& TestEqual(TEXT("one session"), Rules.LobbyState.AvailableSessions.Num(), 1))
		{
			const FApexRaceRules& Race = Rules.LobbyState.AvailableSessions[0].RaceRules;
			TestTrue(TEXT("the server balances its cars"), Rules.LobbyState.bBalanced);
			TestTrue(TEXT("a formation lap"), Race.bFormationLap);
			TestTrue(TEXT("jump starts"), Race.bJumpStart);
			TestTrue(TEXT("yellow passes"), Race.bYellowPenalty);
			TestFalse(TEXT("no pit exit penalty"), Race.bPitExitPenalty);
			TestEqual(TEXT("the safety car"), Race.SafetyCar, EApexSafetyCar::Full);
			TestEqual(TEXT("reverse grid"), static_cast<int32>(Race.ReverseGrid), 6);
			TestEqual(TEXT("the browser's words"), Race.Normalised().Describe(),
				FString(TEXT("formation lap · 2 penalties · safety car · reverse 6")));
		}
	}
	{
		FApexServerMessage Site;
		if (TestTrue(TEXT("a lobby with a track's site decodes"),
				ApexProtocol::DecodeServerMessage(ApexGolden::S_LobbyStateTrackSite, Site, Error))
			&& TestEqual(TEXT("one track"), Site.LobbyState.TrackConfigs.Num(), 1))
		{
			const FApexTrackConfigSummary& Track = Site.LobbyState.TrackConfigs[0];
			TestTrue(TEXT("it has a site"), Track.bHasSite);
			TestTrue(TEXT("its latitude"), FMath::IsNearlyEqual(Track.LatitudeDeg, 52.0786f, 1e-4f));
			TestTrue(TEXT("its longitude"), FMath::IsNearlyEqual(Track.LongitudeDeg, -1.0169f, 1e-4f));
		}
	}

	// Non-empty arrays are the actual assertion: a mis-cased key yields an
	// empty list with no error, which is exactly the failure this pins down.
	if (TestEqual(TEXT("one player in lobby"), Lobby.PlayersInLobby.Num(), 1))
	{
		const FApexLobbyPlayer& Player = Lobby.PlayersInLobby[0];
		TestEqual(TEXT("player Id"), Player.Id, FString(TEXT("deadbeef-0000-1111-2222-333344445555")));
		// UTF-8 through a fixstr whose byte length exceeds its character count.
		TestEqual(TEXT("player Name decodes UTF-8"), Player.Name, FString(TEXT("Nürburgring Nils")));
		TestEqual(TEXT("player SelectedCar"), Player.SelectedCar, CarId);
		// InSession is None on the wire -> nil -> empty string, not a failure.
		TestTrue(TEXT("player InSession is empty for a nil"), Player.InSession.IsEmpty());
		TestTrue(TEXT("player has selected a car"), Player.HasSelectedCar());
	}

	if (TestEqual(TEXT("one available session"), Lobby.AvailableSessions.Num(), 1))
	{
		const FApexSessionSummary& Session = Lobby.AvailableSessions[0];
		TestEqual(TEXT("session Id"), Session.Id, SessId);
		TestEqual(TEXT("session TrackName decodes UTF-8"), Session.TrackName, FString(TEXT("São Paulo")));
		TestEqual(TEXT("session TrackFile"), Session.TrackFile, FString(TEXT("tracks/real/SaoPaulo.yaml")));
		TestEqual(TEXT("session HostName"), Session.HostName, FString(TEXT("Player")));
		// Both of these are u8 integers on the wire, never strings.
		TestEqual(TEXT("session SessionKind"), Session.SessionKind, EApexSessionKind::Practice);
		TestEqual(TEXT("session State"), Session.State, EApexSessionState::Lobby);
		TestEqual(TEXT("session PlayerCount"), Session.PlayerCount, 1);
		TestEqual(TEXT("session MaxPlayers"), Session.MaxPlayers, 8);
		TestTrue(TEXT("session is joinable"), Session.IsJoinable());
	}

	if (TestEqual(TEXT("one car config"), Lobby.CarConfigs.Num(), 1))
	{
		const FApexCarConfigSummary& Car = Lobby.CarConfigs[0];
		TestEqual(TEXT("car Id"), Car.Id, CarId);
		TestEqual(TEXT("car Name"), Car.Name, FString(TEXT("Red Horse RB20")));
		TestEqual(TEXT("car MassKg"), Car.MassKg, 798.0f);
		TestEqual(TEXT("car MaxEngineForceN"), Car.MaxEngineForceN, 12000.5f);
	}

	if (TestEqual(TEXT("one track config"), Lobby.TrackConfigs.Num(), 1))
	{
		const FApexTrackConfigSummary& Track = Lobby.TrackConfigs[0];
		TestEqual(TEXT("track Id"), Track.Id, TrackId);
		TestEqual(TEXT("track Name"), Track.Name, FString(TEXT("Circuit of The Americas")));
		// Centerline parsing is off by default, so it must be skipped cleanly
		// rather than partially consumed.
		TestEqual(TEXT("centerline is skipped by default"), Track.Centerline.Num(), 0);
	}

	// Turn centerline parsing on and confirm the lowercase x/y keys work — the
	// one struct in the payload that is NOT PascalCase.
	{
		IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(TEXT("apexsim.net.ParseCenterline"));
		if (TestNotNull(TEXT("apexsim.net.ParseCenterline exists"), CVar))
		{
			const int32 Previous = CVar->GetInt();
			CVar->Set(1, ECVF_SetByCode);

			FApexServerMessage WithCenterline;
			FString CenterlineError;
			if (TestTrue(
					FString::Printf(TEXT("LobbyState decodes with centerline on (%s)"), *CenterlineError),
					ApexProtocol::DecodeServerMessage(ApexGolden::S_LobbyState, WithCenterline, CenterlineError)))
			{
				if (TestEqual(TEXT("track configs still present"), WithCenterline.LobbyState.TrackConfigs.Num(), 1))
				{
					const TArray<FVector2D>& Points = WithCenterline.LobbyState.TrackConfigs[0].Centerline;
					if (TestEqual(TEXT("two centerline points"), Points.Num(), 2))
					{
						TestEqual(TEXT("point 0 x (lowercase key)"), Points[0].X, 1.5);
						TestEqual(TEXT("point 0 y (lowercase key)"), Points[0].Y, -2.25);
						TestEqual(TEXT("point 1 x"), Points[1].X, 3.0);
						TestEqual(TEXT("point 1 y"), Points[1].Y, 4.0);
					}
				}
			}

			CVar->Set(Previous, ECVF_SetByCode);
		}
	}

	return true;
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexProtocolRobustnessTest,
	"ApexSim.Net.Protocol.Robustness",
	ApexTestFlags)

bool FApexProtocolRobustnessTest::RunTest(const FString& Parameters)
{
	// Every truncation of the largest real message must be rejected, never
	// crash, and never half-populate.
	{
		TArrayView<const uint8> Full(ApexGolden::S_LobbyState);
		for (int32 PrefixLength = 0; PrefixLength < Full.Num(); ++PrefixLength)
		{
			FApexServerMessage Message;
			FString Error;
			const bool bDecoded = ApexProtocol::DecodeServerMessage(
				TArrayView<const uint8>(Full.GetData(), PrefixLength), Message, Error);
			TestFalse(
				FString::Printf(TEXT("a %d byte prefix of LobbyState is rejected"), PrefixLength),
				bDecoded);
		}
	}

	// An unknown variant must decode successfully as Unknown — a newer server
	// adding a message must not break an older client.
	{
		// {"type": "SomeFutureMessage", "data": {"whatever": 1}}
		const uint8 FutureMessage[] = {
			0x82,
			0xA4, 't', 'y', 'p', 'e',
			0xB1, 'S', 'o', 'm', 'e', 'F', 'u', 't', 'u', 'r', 'e', 'M', 'e', 's', 's', 'a', 'g', 'e',
			0xA4, 'd', 'a', 't', 'a',
			0x81, 0xA8, 'w', 'h', 'a', 't', 'e', 'v', 'e', 'r', 0x01
		};
		FApexServerMessage Message;
		FString Error;
		TestTrue(
			FString::Printf(TEXT("an unknown variant decodes without error (%s)"), *Error),
			ApexProtocol::DecodeServerMessage(
				TArrayView<const uint8>(FutureMessage, UE_ARRAY_COUNT(FutureMessage)), Message, Error));
		TestEqual(TEXT("an unknown variant is reported as Unknown"), Message.Type, EApexServerMessageType::Unknown);
		TestEqual(TEXT("the unknown variant name is preserved"), Message.VariantName, FString(TEXT("SomeFutureMessage")));
	}

	// A known variant carrying an unknown extra field must still decode: this
	// is what makes the client forward-compatible with server-side additions.
	{
		// {"type": "SessionJoined", "data": {"SessionId": "...", "YourGridPosition": 3, "NewField": true}}
		const uint8 ExtraField[] = {
			0x82,
			0xA4, 't', 'y', 'p', 'e',
			0xAD, 'S', 'e', 's', 's', 'i', 'o', 'n', 'J', 'o', 'i', 'n', 'e', 'd',
			0xA4, 'd', 'a', 't', 'a',
			0x83,
			0xA9, 'S', 'e', 's', 's', 'i', 'o', 'n', 'I', 'd',
			0xA4, 'a', 'b', 'c', 'd',
			0xB0, 'Y', 'o', 'u', 'r', 'G', 'r', 'i', 'd', 'P', 'o', 's', 'i', 't', 'i', 'o', 'n', 0x03,
			0xA8, 'N', 'e', 'w', 'F', 'i', 'e', 'l', 'd', 0xC3
		};
		FApexServerMessage Message;
		FString Error;
		TestTrue(
			FString::Printf(TEXT("an unknown field is skipped (%s)"), *Error),
			ApexProtocol::DecodeServerMessage(
				TArrayView<const uint8>(ExtraField, UE_ARRAY_COUNT(ExtraField)), Message, Error));
		TestEqual(TEXT("the known variant still resolves"), Message.Type, EApexServerMessageType::SessionJoined);
		TestEqual(TEXT("known fields survive an unknown neighbour"), Message.SessionId, FString(TEXT("abcd")));
		TestEqual(TEXT("grid position survives an unknown neighbour"), Message.GridPosition, 3);
		TestEqual(TEXT("a join without a kind is an ordinary session"), Message.SessionKind, EApexSessionKind::Multiplayer);
	}

	// The menu's demo session is told apart by the kind on the join itself.
	{
		// {"type": "SessionJoined", "data": {"SessionId": "abcd", "YourGridPosition": 0, "SessionKind": 3}}
		const uint8 DemoJoin[] = {
			0x82,
			0xA4, 't', 'y', 'p', 'e',
			0xAD, 'S', 'e', 's', 's', 'i', 'o', 'n', 'J', 'o', 'i', 'n', 'e', 'd',
			0xA4, 'd', 'a', 't', 'a',
			0x83,
			0xA9, 'S', 'e', 's', 's', 'i', 'o', 'n', 'I', 'd',
			0xA4, 'a', 'b', 'c', 'd',
			0xB0, 'Y', 'o', 'u', 'r', 'G', 'r', 'i', 'd', 'P', 'o', 's', 'i', 't', 'i', 'o', 'n', 0x00,
			0xAB, 'S', 'e', 's', 's', 'i', 'o', 'n', 'K', 'i', 'n', 'd', 0x03
		};
		FApexServerMessage Message;
		FString Error;
		TestTrue(
			FString::Printf(TEXT("a demo join decodes (%s)"), *Error),
			ApexProtocol::DecodeServerMessage(
				TArrayView<const uint8>(DemoJoin, UE_ARRAY_COUNT(DemoJoin)), Message, Error));
		TestEqual(TEXT("the join says it is a demo"), Message.SessionKind, EApexSessionKind::Demo);
	}

	// Garbage must be rejected rather than misread.
	{
		const uint8 Garbage[] = {0xC1, 0xC1, 0xC1};
		FApexServerMessage Message;
		FString Error;
		TestFalse(TEXT("garbage is rejected"),
			ApexProtocol::DecodeServerMessage(
				TArrayView<const uint8>(Garbage, UE_ARRAY_COUNT(Garbage)), Message, Error));
	}

	// An empty payload is rejected.
	{
		FApexServerMessage Message;
		FString Error;
		TestFalse(TEXT("an empty payload is rejected"),
			ApexProtocol::DecodeServerMessage(TArrayView<const uint8>(), Message, Error));
	}

	return true;
}

// -----------------------------------------------------------------------------
// RacingLine: parallel X/Y/Z/Phase arrays zipped into points.
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexProtocolRacingLineDecodeTest,
	"ApexSim.Net.Protocol.RacingLineDecode",
	ApexTestFlags)

bool FApexProtocolRacingLineDecodeTest::RunTest(const FString& Parameters)
{
	FApexServerMessage Message;
	FString Error;
	if (!TestTrue(FString::Printf(TEXT("RacingLine decodes (%s)"), *Error),
			ApexProtocol::DecodeServerMessage(ApexGolden::S_RacingLine, Message, Error)))
	{
		return false;
	}

	TestEqual(TEXT("RacingLine type"), Message.Type, EApexServerMessageType::RacingLine);
	const FApexRacingLineData& Line = Message.RacingLine;
	TestEqual(TEXT("session id"), Line.SessionId, SessId);
	TestEqual(TEXT("spacing"), Line.SpacingM, 2.5f);
	if (TestEqual(TEXT("two points"), Line.Points.Num(), 2) && TestEqual(TEXT("two phases"), Line.Phases.Num(), 2))
	{
		TestEqual(TEXT("point 0"), Line.Points[0], FVector(1.5, 0.25, 0.0));
		TestEqual(TEXT("point 1"), Line.Points[1], FVector(-2.0, 3.0, 1.0));
		TestEqual(TEXT("phase 0 is throttle"), Line.Phases[0], EApexLinePhase::Throttle);
		TestEqual(TEXT("phase 1 is brake"), Line.Phases[1], EApexLinePhase::Brake);
	}
	return true;
}


// -----------------------------------------------------------------------------
// Lap timing: the sector lines, the splits and the records.
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexProtocolLapTimingDecodeTest,
	"ApexSim.Net.Protocol.LapTimingDecode",
	ApexTestFlags)

bool FApexProtocolLapTimingDecodeTest::RunTest(const FString& Parameters)
{
	FString Error;

	FApexServerMessage SectorsMessage;
	if (TestTrue(TEXT("TrackSectors decodes"),
			ApexProtocol::DecodeServerMessage(ApexGolden::S_TrackSectors, SectorsMessage, Error)))
	{
		TestEqual(TEXT("TrackSectors type"), SectorsMessage.Type, EApexServerMessageType::TrackSectors);
		const FApexTrackSectors& Sectors = SectorsMessage.TrackSectors;
		TestEqual(TEXT("session id"), Sectors.SessionId, SessId);
		TestEqual(TEXT("track length"), Sectors.TrackLengthM, 5793.0f);
		TestTrue(TEXT("sectors are usable"), Sectors.IsValid());
		if (TestEqual(TEXT("two boundaries"), Sectors.BoundariesM.Num(), 2))
		{
			TestEqual(TEXT("boundary 1"), Sectors.BoundariesM[0], 1931.0f);
			TestEqual(TEXT("boundary 2"), Sectors.BoundariesM[1], 3862.0f);
		}
		TestEqual(TEXT("three sectors"), Sectors.SectorCount(), 3);
		// Sector 1 starts at the line, and the boundary itself belongs to the
		// sector it opens.
		TestEqual(TEXT("at the line"), Sectors.SectorAt(0.0f), 0);
		TestEqual(TEXT("just before the first line"), Sectors.SectorAt(1930.0f), 0);
		TestEqual(TEXT("on the first line"), Sectors.SectorAt(1931.0f), 1);
		TestEqual(TEXT("past the second"), Sectors.SectorAt(5000.0f), 2);
	}

	FApexServerMessage CornersMessage;
	if (TestTrue(TEXT("TrackCorners decodes"),
			ApexProtocol::DecodeServerMessage(ApexGolden::S_TrackCorners, CornersMessage, Error)))
	{
		TestEqual(TEXT("TrackCorners type"), CornersMessage.Type, EApexServerMessageType::TrackCorners);
		const FApexTrackCorners& Corners = CornersMessage.TrackCorners;
		TestEqual(TEXT("session id"), Corners.SessionId, SessId);
		TestEqual(TEXT("lap length"), Corners.TrackLengthM, 5793.0f);
		TestTrue(TEXT("corners are usable"), Corners.IsValid());
		if (TestEqual(TEXT("two corners"), Corners.Corners.Num(), 2))
		{
			const FApexTrackCorner& First = Corners.Corners[0];
			TestEqual(TEXT("number"), First.Number, 1);
			TestEqual(TEXT("name"), First.Name, FString(TEXT("Variante del Rettifilo")));
			TestEqual(TEXT("entry"), First.EntryM, 5700.0f);
			TestEqual(TEXT("apex"), First.ApexM, 120.0f);
			TestEqual(TEXT("exit"), First.ExitM, 210.0f);
			TestFalse(TEXT("turns right"), First.bLeft);
			const FApexTrackCorner& Second = Corners.Corners[1];
			TestEqual(TEXT("no name, so Turn N"), Second.Label(), FString(TEXT("Turn 2")));
			TestTrue(TEXT("turns left"), Second.bLeft);

			// The first corner's entry is before the line and its exit past
			// it: the car is in it on both sides of the line, not between
			// the exit and the next entry.
			TestTrue(TEXT("inside, before the line"), Corners.IsInside(First, 5750.0f));
			TestTrue(TEXT("inside, past the line"), Corners.IsInside(First, 100.0f));
			TestFalse(TEXT("out of it after the exit"), Corners.IsInside(First, 300.0f));
			TestFalse(TEXT("out of it before the entry"), Corners.IsInside(First, 5600.0f));

			float Distance = 0.0f;
			bool bInside = false;
			TestEqual(TEXT("in the first"), Corners.CornerAt(150.0f, Distance, bInside), 0);
			TestTrue(TEXT("and inside it"), bInside && Distance == 0.0f);
			TestEqual(TEXT("coming to the second"), Corners.CornerAt(500.0f, Distance, bInside), 1);
			TestTrue(TEXT("400 m from its entry"), !bInside && FMath::IsNearlyEqual(Distance, 400.0f));
			TestEqual(TEXT("past the second, the first comes round"), Corners.CornerAt(1500.0f, Distance, bInside), 0);
			TestTrue(TEXT("through the line"), !bInside && FMath::IsNearlyEqual(Distance, 4200.0f));
		}
	}

	FApexServerMessage TimingMessage;
	if (TestTrue(TEXT("LapTiming decodes"),
			ApexProtocol::DecodeServerMessage(ApexGolden::S_LapTiming, TimingMessage, Error)))
	{
		TestEqual(TEXT("LapTiming type"), TimingMessage.Type, EApexServerMessageType::LapTiming);
		const FApexLapTiming& Timing = TimingMessage.LapTiming;
		TestEqual(TEXT("car index"), Timing.CarIndex, 2);
		TestEqual(TEXT("lap"), Timing.Lap, 4);
		TestEqual(TEXT("sector"), Timing.Sector, 2);
		TestEqual(TEXT("sector time"), Timing.SectorTimeMs, 27431);
		TestEqual(TEXT("lap time"), Timing.LapTimeMs, 82615);
		TestTrue(TEXT("closes the lap"), Timing.bIsLapEnd);
		TestTrue(TEXT("legal"), Timing.bValid);
		// Flags 0b1001: the driver's best lap and the session's best sector.
		TestTrue(TEXT("personal best lap"), Timing.bPersonalBestLap);
		TestFalse(TEXT("not the session's best lap"), Timing.bSessionBestLap);
		TestFalse(TEXT("not a personal best sector"), Timing.bPersonalBestSector);
		TestTrue(TEXT("session best sector"), Timing.bSessionBestSector);
	}

	FApexServerMessage RecordMessage;
	if (TestTrue(TEXT("LapRecord decodes"),
			ApexProtocol::DecodeServerMessage(ApexGolden::S_LapRecord, RecordMessage, Error)))
	{
		TestEqual(TEXT("LapRecord type"), RecordMessage.Type, EApexServerMessageType::LapRecord);
		const FApexLapRecord& Record = RecordMessage.LapRecord;
		TestEqual(TEXT("player"), Record.PlayerName, FString(TEXT("Ayrton")));
		TestEqual(TEXT("track id"), Record.TrackId, TrackId);
		TestEqual(TEXT("car id"), Record.CarConfigId, CarId);
		TestEqual(TEXT("lap time"), Record.LapTimeMs, 82615);
		TestTrue(TEXT("new record"), Record.bIsNew);
		TestTrue(TEXT("has a ghost"), Record.bHasGhost);
		TestEqual(TEXT("track record"), Record.TrackRecordMs, 81900);
		TestEqual(TEXT("track record holder"), Record.TrackRecordHolder, FString(TEXT("Alain")));
		if (TestEqual(TEXT("three splits"), Record.SplitsMs.Num(), 3))
		{
			int32 Total = 0;
			for (int32 Split : Record.SplitsMs)
			{
				Total += Split;
			}
			TestEqual(TEXT("splits add up to the lap"), Total, Record.LapTimeMs);
		}
	}

	FApexServerMessage RoadMessage;
	if (TestTrue(TEXT("RoadState decodes"),
			ApexProtocol::DecodeServerMessage(ApexGolden::S_RoadState, RoadMessage, Error)))
	{
		TestEqual(TEXT("RoadState type"), RoadMessage.Type, EApexServerMessageType::RoadState);
		const FApexRoadState& Road = RoadMessage.RoadState;
		TestEqual(TEXT("session id"), Road.SessionId, SessId);
		TestEqual(TEXT("lap"), Road.LapM, 5793.0f);
		TestEqual(TEXT("cell"), Road.CellM, 10.0f);
		TestEqual(TEXT("bins"), Road.Bins, 4);
		TestEqual(TEXT("half span"), Road.HalfSpanM, 16.0f);
		TestEqual(TEXT("first cell"), Road.FirstCell, 7);
		TestEqual(TEXT("a row is depth and three bytes a bin"), Road.RowBytes(), 13);
		TestEqual(TEXT("two cells"), Road.NumCells(), 2);
		TestEqual(TEXT("580 cells round the lap"), Road.LapCells(), 580);
		if (TestEqual(TEXT("26 row bytes"), Road.Rows.Num(), 26))
		{
			TestEqual(TEXT("first cell's depth"), (int32)Road.Rows[0], 80);
			// Cell 0, bin 2: rubber 255, marbles 20, dry 40.
			TestEqual(TEXT("bin rubber"), (int32)Road.Rows[1 + 3 * 2], 255);
			TestEqual(TEXT("bin dry"), (int32)Road.Rows[1 + 3 * 2 + 2], 40);
			TestEqual(TEXT("second cell dry"), (int32)Road.Rows[13], 0);
		}
		TestTrue(TEXT("debris"), Road.Debris.Num() == 2 && Road.Debris[1].Equals(FVector2D(900.0, 4.0), 1e-3));
		TestTrue(TEXT("geometry"), Road.Geometry.Num() == 2
			&& Road.Geometry[0] == FVector4f(100.0f, -20.0f, 0.0f, 1.0f)
			&& Road.Geometry[1] == FVector4f(110.0f, -20.0f, 0.0f, 1.0f));
		TestTrue(TEXT("usable"), Road.IsValid());
		TestFalse(TEXT("no snow rows without snow"), Road.HasSnow());
	}

	// A session listed with the damage rule its host picked (a Full rule is left
	// off the wire, and reads as Full).
	{
		const uint8 Listed[] = {
			0x82, 0xA4, 't', 'y', 'p', 'e', 0xAA, 'L', 'o', 'b', 'b', 'y', 'S', 't', 'a', 't', 'e',
			0xA4, 'd', 'a', 't', 'a', 0x81, 0xB1, 'A', 'v', 'a', 'i', 'l', 'a', 'b', 'l', 'e', 'S', 'e', 's', 's', 'i', 'o', 'n', 's',
			0x92, 0x81, 0xA6, 'D', 'a', 'm', 'a', 'g', 'e', 0x01, 0x80 };
		FApexServerMessage Listing;
		if (TestTrue(TEXT("a lobby listing with a damage rule decodes"),
				ApexProtocol::DecodeServerMessage(TArrayView<const uint8>(Listed, UE_ARRAY_COUNT(Listed)), Listing, Error)))
		{
			if (TestEqual(TEXT("two sessions"), Listing.LobbyState.AvailableSessions.Num(), 2))
			{
				TestEqual(TEXT("reduced damage"), (int32)Listing.LobbyState.AvailableSessions[0].Damage, (int32)EApexDamageLevel::Reduced);
				TestEqual(TEXT("a session without the field is full damage"), (int32)Listing.LobbyState.AvailableSessions[1].Damage, (int32)EApexDamageLevel::Full);
			}
		}
	}

	FApexServerMessage SnowMessage;
	if (TestTrue(TEXT("RoadState with snow decodes"),
			ApexProtocol::DecodeServerMessage(ApexGolden::S_RoadStateSnow, SnowMessage, Error)))
	{
		const FApexRoadState& Road = SnowMessage.RoadState;
		TestEqual(TEXT("the rows are read as before"), Road.Rows.Num(), 26);
		TestEqual(TEXT("a snow row is the verge and two bytes a bin"), Road.SnowRowBytes(), 9);
		if (TestTrue(TEXT("snow rows"), Road.HasSnow() && Road.SnowRows.Num() == 18))
		{
			TestEqual(TEXT("cell 0 verge"), (int32)Road.SnowRows[0], 255);
			// Bin 2 of cell 0: cover 128, ice 0.
			TestEqual(TEXT("bin cover"), (int32)Road.SnowRows[1 + 2 * 2], 128);
			TestEqual(TEXT("bin ice"), (int32)Road.SnowRows[1 + 2 * 2 + 1], 0);
			TestEqual(TEXT("the last bin's ice"), (int32)Road.SnowRows[1 + 2 * 3 + 1], 90);
			TestEqual(TEXT("cell 1 is bare"), (int32)Road.SnowRows[9], 0);
		}
		TestFalse(TEXT("no water rows without a puddle"), Road.HasWaterRows());
	}

	FApexServerMessage PuddleMessage;
	if (TestTrue(TEXT("RoadState with a puddle decodes"),
			ApexProtocol::DecodeServerMessage(ApexGolden::S_RoadStatePuddle, PuddleMessage, Error)))
	{
		const FApexRoadState& Road = PuddleMessage.RoadState;
		TestEqual(TEXT("the rows are read as before"), Road.Rows.Num(), 26);
		if (TestTrue(TEXT("water rows, a byte a bin"), Road.HasWaterRows() && Road.WaterRows.Num() == 8))
		{
			TestEqual(TEXT("the puddle on cell 0's second bin"), (int32)Road.WaterRows[1], 160);
			TestEqual(TEXT("the flat beside it"), (int32)Road.WaterRows[0], 80);
			TestEqual(TEXT("cell 1 is dry"), (int32)Road.WaterRows[5], 0);
		}
		TestFalse(TEXT("and no snow"), Road.HasSnow());
		TestEqual(TEXT("and no lane snow"), Road.LaneSnow.Num(), 0);
	}

	FApexServerMessage LaneMessage;
	if (TestTrue(TEXT("RoadState with the lane's snow decodes"),
			ApexProtocol::DecodeServerMessage(ApexGolden::S_RoadStateLaneSnow, LaneMessage, Error)))
	{
		const FApexRoadState& Road = LaneMessage.RoadState;
		if (TestEqual(TEXT("a byte a lane cell"), Road.LaneSnow.Num(), 3))
		{
			TestEqual(TEXT("the packed cell"), (int32)Road.LaneSnow[0], 128);
			TestEqual(TEXT("the rest white"), (int32)Road.LaneSnow[2], 255);
		}
		TestTrue(TEXT("with the road's snow"), Road.HasSnow());
	}

	return true;
}

/**
 * The timing board is the client's whole memory of a session's laps, so the
 * bests it files have to survive the order the messages arrive in.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexTimingBoardTest,
	"ApexSim.Net.Protocol.TimingBoard",
	ApexTestFlags)

bool FApexTimingBoardTest::RunTest(const FString& Parameters)
{
	FApexTimingBoard Board;
	Board.Reset(3);

	auto Cross = [](int32 Car, int32 Lap, int32 Sector, int32 SectorMs, int32 LapMs, bool bValid)
	{
		FApexLapTiming Timing;
		Timing.CarIndex = Car;
		Timing.Lap = Lap;
		Timing.Sector = Sector;
		Timing.SectorTimeMs = SectorMs;
		Timing.LapTimeMs = LapMs;
		Timing.bIsLapEnd = LapMs > 0;
		Timing.bValid = bValid;
		return Timing;
	};

	// Car 0 sets a clean 1:22.615 — its first lap, so every part of it is a
	// personal best, and with nobody else out, the session's too.
	for (int32 Sector = 0; Sector < 3; ++Sector)
	{
		const int32 Splits[3] = { 27100, 28084, 27431 };
		FApexLapTiming Timing = Cross(0, 1, Sector, Splits[Sector], Sector == 2 ? 82615 : 0, true);
		Timing.bPersonalBestSector = true;
		Timing.bSessionBestSector = true;
		Timing.bPersonalBestLap = Sector == 2;
		Timing.bSessionBestLap = Sector == 2;
		Board.Apply(Timing);
	}

	const FApexCarTiming* Car = Board.Find(0);
	if (!TestNotNull(TEXT("car 0 is on the board"), Car))
	{
		return false;
	}
	TestEqual(TEXT("best lap"), Car->BestLapMs, 82615);
	TestEqual(TEXT("last lap"), Car->LastLapMs, 82615);
	TestTrue(TEXT("last lap counted"), Car->bLastLapValid);
	TestEqual(TEXT("session best lap"), Board.SessionBestLapMs, 82615);
	TestEqual(TEXT("session best holder"), Board.SessionBestLapCarIndex, 0);
	TestEqual(TEXT("optimal lap is the three bests"), Car->OptimalLapMs(), 82615);
	TestEqual(TEXT("the lap in progress is empty again"), Car->CurrentSplitsMs[0], 0);

	// A struck lap: the server sends no best flags, so nothing on the board
	// moves however quick the time was.
	Board.Apply(Cross(0, 2, 0, 20000, 0, false));
	Board.Apply(Cross(0, 2, 1, 20000, 0, false));
	Board.Apply(Cross(0, 2, 2, 20000, 60000, false));
	Car = Board.Find(0);
	TestEqual(TEXT("best lap unchanged by a struck lap"), Car->BestLapMs, 82615);
	TestEqual(TEXT("last lap is still recorded"), Car->LastLapMs, 60000);
	TestFalse(TEXT("last lap did not count"), Car->bLastLapValid);
	TestEqual(TEXT("best sector unchanged"), Car->BestSplitsMs[0], 27100);

	// A second car takes a sector but not the lap.
	FApexLapTiming Purple = Cross(1, 1, 0, 26500, 0, true);
	Purple.bPersonalBestSector = true;
	Purple.bSessionBestSector = true;
	Board.Apply(Purple);
	TestEqual(TEXT("session best sector 1"), Board.SessionBestSplitsMs[0], 26500);
	TestEqual(TEXT("session best lap still car 0"), Board.SessionBestLapCarIndex, 0);

	// And a board that never heard the sector count still files a crossing.
	FApexTimingBoard Fresh;
	Fresh.Apply(Cross(3, 1, 1, 30000, 0, true));
	TestEqual(TEXT("sector count grows to fit"), Fresh.SectorCount, 2);
	TestNotNull(TEXT("car filed"), Fresh.Find(3));
	return true;
}

// -----------------------------------------------------------------------------
// Stored qualifying results, asked for per track.
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// The driver key: OpenSSL's Ed25519 makes the server's own known answer
// (driver_keys.rs, a_known_seed_signs_a_known_proof), and a fresh seed signs.
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexDriverKeyTest,
	"ApexSim.Net.DriverKey",
	ApexTestFlags)

bool FApexDriverKeyTest::RunTest(const FString& Parameters)
{
	TArray<uint8> Seed;
	Seed.Init(3, ApexDriverKey::KeyBytes);
	TestEqual(TEXT("the public key"), ApexDriverKey::PublicKeyHex(Seed), FString(TEXT("ed4928c628d1c2c6eae90338905995612959273a5c63f93636c14614ac8737d1")));
	const TArray<uint8> Message = ApexDriverKey::ProofMessage(TEXT("abcd"), TEXT("00000000-0000-0000-0000-000000000007"));
	TestEqual(TEXT("the signature the server checks"), ApexDriverKey::SignHex(Seed, Message),
		FString(TEXT("d1d7215720771c6c9167b0b43456f67e3c1a0f153d1b031d0ced996beb0bcc6132bca87bbeb7dc859a69a70e34785e08d38dbaac09b880890ceeae6a28fdf707")));
	TArray<uint8> Fresh;
	TestTrue(TEXT("a seed is made"), ApexDriverKey::Generate(Fresh) && Fresh.Num() == ApexDriverKey::KeyBytes);
	TestEqual(TEXT("its public key"), ApexDriverKey::PublicKeyHex(Fresh).Len(), 64);
	TestEqual(TEXT("its signature"), ApexDriverKey::SignHex(Fresh, Message).Len(), 128);
	TestTrue(TEXT("no seed, no key"), ApexDriverKey::PublicKeyHex(TArray<uint8>()).IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexProtocolLeaderboardTest,
	"ApexSim.Net.Protocol.Leaderboard",
	ApexTestFlags)

bool FApexProtocolLeaderboardTest::RunTest(const FString& Parameters)
{
	FApexServerMessage Message;
	FString Error;
	if (!TestTrue(FString::Printf(TEXT("Leaderboard decodes (%s)"), *Error),
			ApexProtocol::DecodeServerMessage(ApexGolden::S_Leaderboard, Message, Error)))
	{
		return false;
	}
	TestEqual(TEXT("type"), Message.Type, EApexServerMessageType::Leaderboard);
	TestEqual(TEXT("track"), Message.LeaderboardTrackId, FString(TEXT("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee")));
	TestEqual(TEXT("car"), Message.LeaderboardCarId, FString(TEXT("99999999-8888-7777-6666-555555555555")));
	if (!TestEqual(TEXT("two laps"), Message.Leaderboard.Num(), 2))
	{
		return false;
	}
	TestEqual(TEXT("first driver"), Message.Leaderboard[0].PlayerName, FString(TEXT("Luna Swift")));
	TestEqual(TEXT("first lap"), Message.Leaderboard[0].LapTimeMs, 91234);
	TestTrue(TEXT("splits"), Message.Leaderboard[0].SplitsMs == TArray<int32>({30100, 31000, 30134}));
	TestEqual(TEXT("second driver"), Message.Leaderboard[1].PlayerName, FString(TEXT("Guido")));
	TestTrue(TEXT("an AI's lap has no profile"), Message.Leaderboard[0].DriverId.IsEmpty());
	TestEqual(TEXT("the profile's lap"), Message.Leaderboard[1].DriverId, FString(TEXT("dddddddd-0000-4000-8000-000000000001")));
	TestEqual(TEXT("when"), Message.Leaderboard[1].RecordedAt, FString(TEXT("2026-10-07T09:30:00Z")));
	// A request for every car leaves the car off the wire.
	const TArray<uint8> All = ApexProtocol::EncodeRequestLeaderboard(TEXT("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"), FString());
	TestTrue(TEXT("every car has no car field"), All.Num() > 0 && All.Num() < UE_ARRAY_COUNT(ApexGolden::C_RequestLeaderboard));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexProtocolQualifyingResultsTest,
	"ApexSim.Net.Protocol.QualifyingResults",
	ApexTestFlags)

bool FApexProtocolQualifyingResultsTest::RunTest(const FString& Parameters)
{
	FString Error;
	FApexServerMessage Message;
	if (!TestTrue(FString::Printf(TEXT("QualifyingResults decodes (%s)"), *Error),
			ApexProtocol::DecodeServerMessage(ApexGolden::S_QualifyingResults, Message, Error)))
	{
		return false;
	}
	TestEqual(TEXT("type"), Message.Type, EApexServerMessageType::QualifyingResults);
	TestEqual(TEXT("track"), Message.QualifyingTrackId, TrackId);
	if (!TestEqual(TEXT("one result"), Message.QualifyingResults.Num(), 1))
	{
		return false;
	}
	const FApexQualifyingResult& Result = Message.QualifyingResults[0];
	TestEqual(TEXT("id"), Result.Id, FString(TEXT("11111111-2222-3333-4444-555555555555")));
	TestEqual(TEXT("class"), Result.Class, FString(TEXT("GT3")));
	TestEqual(TEXT("date"), Result.RecordedAt, FString(TEXT("2026-10-06T12:00:00Z")));
	if (!TestEqual(TEXT("two drivers"), Result.Entries.Num(), 2))
	{
		return false;
	}
	TestEqual(TEXT("pole"), Result.Entries[0].Name, FString(TEXT("Luna Swift")));
	TestEqual(TEXT("pole time"), Result.Entries[0].LapTimeMs, 91234);
	TestTrue(TEXT("pole is an AI"), Result.Entries[0].bIsAi);
	TestEqual(TEXT("second"), Result.Entries[1].Name, FString(TEXT("Guido")));
	TestFalse(TEXT("second is human"), Result.Entries[1].bIsAi);
	TestTrue(TEXT("the AI has no profile"), Result.Entries[0].DriverId.IsEmpty());
	TestEqual(TEXT("the human's profile"), Result.Entries[1].DriverId, FString(TEXT("dddddddd-0000-4000-8000-000000000001")));
	return true;
}

// -----------------------------------------------------------------------------
// The ghost lap: struct-of-arrays on the wire, one sample per moment here,
// and a pose blended between samples for a puppet car.
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexProtocolGhostLapTest,
	"ApexSim.Net.Protocol.GhostLap",
	ApexTestFlags)

bool FApexProtocolGhostLapTest::RunTest(const FString& Parameters)
{
	FString Error;
	FApexServerMessage Message;
	if (!TestTrue(FString::Printf(TEXT("GhostLap decodes (%s)"), *Error),
			ApexProtocol::DecodeServerMessage(ApexGolden::S_GhostLap, Message, Error)))
	{
		return false;
	}
	TestEqual(TEXT("GhostLap type"), Message.Type, EApexServerMessageType::GhostLap);
	const FApexGhostLap& Lap = Message.GhostLap;
	TestEqual(TEXT("track id"), Lap.TrackId, TrackId);
	TestEqual(TEXT("car id"), Lap.CarConfigId, CarId);
	TestEqual(TEXT("lap time"), Lap.LapTimeMs, 82615);
	TestEqual(TEXT("sample rate"), Lap.SampleHz, 20.0f);
	TestTrue(TEXT("a lap with two samples is playable"), Lap.IsValid());
	if (!TestEqual(TEXT("two samples"), Lap.Samples.Num(), 2))
	{
		return false;
	}
	TestEqual(TEXT("t0"), Lap.Samples[0].TimeMs, 0);
	TestEqual(TEXT("t1"), Lap.Samples[1].TimeMs, 50);
	TestEqual(TEXT("position 0"), Lap.Samples[0].Position, FVector(1.5, -2.0, 0.25));
	TestEqual(TEXT("position 1"), Lap.Samples[1].Position, FVector(4.5, -2.5, 0.25));
	TestEqual(TEXT("roll 0"), Lap.Samples[0].RollRad, -0.125f);
	TestEqual(TEXT("speed 1"), Lap.Samples[1].SpeedMps, 61.0f);
	TestEqual(TEXT("steering 1"), Lap.Samples[1].Steering, -0.2f);
	TestEqual(TEXT("gear 0"), Lap.Samples[0].Gear, 4);
	TestEqual(TEXT("a negative gear is signed on the wire"), Lap.Samples[1].Gear, -1);
	TestEqual(TEXT("rpm 1"), Lap.Samples[1].EngineRpm, 9100.0f);

	// Halfway between the samples: position and speed blend, the gear steps.
	FApexGhostSample Mid;
	TestTrue(TEXT("mid-lap sample is inside the lap"), Lap.SampleAt(25.0f, Mid));
	TestEqual(TEXT("blended position"), Mid.Position, FVector(3.0, -2.25, 0.25));
	TestEqual(TEXT("blended speed"), Mid.SpeedMps, 60.5f);
	TestEqual(TEXT("blended roll"), Mid.RollRad, -0.0625f);
	TestEqual(TEXT("gear steps at the second sample"), Mid.Gear, -1);
	FApexGhostSample Before;
	TestTrue(TEXT("before the line is the first sample"), Lap.SampleAt(-10.0f, Before));
	TestEqual(TEXT("first sample"), Before.Position, Lap.Samples[0].Position);
	FApexGhostSample After;
	TestFalse(TEXT("past the last sample the lap is over"), Lap.SampleAt(1000.0f, After));
	TestEqual(TEXT("held at the last sample"), After.Position, Lap.Samples[1].Position);

	// The yaw blends the short way round the seam.
	FApexGhostLap Seam;
	Seam.LapTimeMs = 100;
	FApexGhostSample A;
	A.TimeMs = 0;
	A.YawRad = 3.0f;
	FApexGhostSample B;
	B.TimeMs = 100;
	B.YawRad = -3.0f;
	Seam.Samples = { A, B };
	FApexGhostSample AtSeam;
	Seam.SampleAt(50.0f, AtSeam);
	TestTrue(TEXT("yaw crosses the seam, not the long way"), FMath::Abs(FMath::Abs(AtSeam.YawRad) - PI) < 0.01f);

	TestFalse(TEXT("an empty reply is not a lap"), FApexGhostLap().IsValid());
	return true;
}

// -----------------------------------------------------------------------------
// The garage's setup sheet: every knob's stock value and click in real units.
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexProtocolCarSetupSheetTest,
	"ApexSim.Net.Protocol.CarSetupSheet",
	ApexTestFlags)

bool FApexProtocolCarSetupSheetTest::RunTest(const FString& Parameters)
{
	FString Error;
	FApexServerMessage Message;
	if (!TestTrue(FString::Printf(TEXT("CarSetupSheet decodes (%s)"), *Error),
			ApexProtocol::DecodeServerMessage(ApexGolden::S_CarSetupSheet, Message, Error)))
	{
		return false;
	}
	TestEqual(TEXT("type"), Message.Type, EApexServerMessageType::CarSetupSheet);
	const FApexCarSetupSheet& Sheet = Message.CarSetupSheet;
	TestEqual(TEXT("car id"), Sheet.CarConfigId, FString(TEXT("00000000-0000-0000-0000-000000000002")));
	if (!TestEqual(TEXT("two knobs"), Sheet.Knobs.Num(), 2))
	{
		return false;
	}
	TestEqual(TEXT("stock"), Sheet.Knobs[0].Stock, 26.0f);
	TestEqual(TEXT("step"), Sheet.Knobs[0].Step, 0.5f);
	TestEqual(TEXT("unbounded below"), Sheet.Knobs[0].Lo, -MAX_flt);
	TestEqual(TEXT("decimals"), Sheet.Knobs[0].Decimals, 1);
	TestEqual(TEXT("unit"), Sheet.Knobs[0].Unit, FString(TEXT("psi")));
	TestEqual(TEXT("floor"), Sheet.Knobs[1].Lo, 10.0f);
	TestEqual(TEXT("mm"), Sheet.Knobs[1].Unit, FString(TEXT("mm")));
	TestEqual(TEXT("gears"), Sheet.GearRatios.Num(), 2);
	TestEqual(TEXT("final drive"), Sheet.FinalDrive, 3.75f);
	TestEqual(TEXT("wheel"), Sheet.WheelRadiusM, 0.25f);
	TestEqual(TEXT("optimum"), Sheet.TyreOptimalPsi, 27.5f);
	TestEqual(TEXT("fuel per lap"), Sheet.LapFuelL, 2.5f);
	TestEqual(TEXT("density"), Sheet.FuelKgPerL, 0.75f);
	TestEqual(TEXT("fill"), Sheet.FillLaps, 3.0f);
	TestTrue(TEXT("camber"), Sheet.bCamberModelled);
	TestEqual(TEXT("rake balance"), Sheet.RakeBalancePerMm, 0.001f);
	if (TestEqual(TEXT("two compounds"), Sheet.Compounds.Num(), 2))
	{
		TestEqual(TEXT("first compound"), Sheet.Compounds[0], FString(TEXT("soft")));
		TestEqual(TEXT("upper-cased for the garage"), Sheet.CompoundNameAt(1), FString(TEXT("MEDIUM")));
	}
	TestEqual(TEXT("reference"), Sheet.ReferenceCompound, 1);
	TestTrue(TEXT("a hybrid"), Sheet.HasHybrid());
	TestEqual(TEXT("battery"), Sheet.HybridBatteryKwh, 4.0f);
	TestEqual(TEXT("motor"), Sheet.HybridMotorKw, 350.0f);
	TestEqual(TEXT("no lap budget"), Sheet.HybridLapBudgetKj, 0.0f);
	TestEqual(TEXT("deploys from"), Sheet.HybridDeployMinKph, 190.0f);
	TestEqual(TEXT("the reference is click 0"), Sheet.CompoundIndexForClicks(0), 1);
	TestEqual(TEXT("one click up is the first"), Sheet.ClicksForCompoundIndex(0), 1);
	TestEqual(TEXT("a name from the sheet"), FApexCarTelemetry::CompoundNameFrom(Sheet.Compounds, 0), FString(TEXT("SOFT")));
	TestEqual(TEXT("a letter from the sheet"), FApexCarTelemetry::CompoundLetterFrom(Sheet.Compounds, 1), FString(TEXT("M")));
	TestEqual(TEXT("a snow tyre is SN, not a soft"),
		FApexCarTelemetry::CompoundLetterFrom({ TEXT("gravel"), TEXT("snow") }, 1), FString(TEXT("SN")));
	TestEqual(TEXT("past the sheet's list is unknown"), FApexCarTelemetry::CompoundNameFrom(Sheet.Compounds, 4), FString());
	TestEqual(TEXT("no sheet falls back to the defaults"), FApexCarTelemetry::CompoundNameFrom(TArray<FString>(), 4), FString(TEXT("WET")));
	FApexCarSetupSheet Older;
	TestFalse(TEXT("an older sheet names no compounds"), Older.HasCompounds());
	TestEqual(TEXT("and its reference is the medium"), Older.ReferenceCompound, 1);
	return true;
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexProtocolPitServiceTest,
	"ApexSim.Net.Protocol.PitService",
	ApexTestFlags)

bool FApexProtocolPitServiceTest::RunTest(const FString& Parameters)
{
	FString Error;
	FApexServerMessage Message;
	if (!TestTrue(FString::Printf(TEXT("PitService decodes (%s)"), *Error),
			ApexProtocol::DecodeServerMessage(ApexGolden::S_PitService, Message, Error)))
	{
		return false;
	}
	TestEqual(TEXT("type"), Message.Type, EApexServerMessageType::PitService);
	const FApexPitService& Stop = Message.PitService;
	TestEqual(TEXT("car"), Stop.CarIndex, 3);
	TestEqual(TEXT("box"), Stop.PitBox, 7);
	TestEqual(TEXT("tyres"), Stop.TyresS, 9.0f);
	TestEqual(TEXT("soft"), Stop.Compound, 0);
	TestEqual(TEXT("fuel"), Stop.FuelS, 12.5f);
	TestEqual(TEXT("litres"), Stop.FuelL, 25.0f);
	TestEqual(TEXT("repairs"), Stop.RepairS, 2.0f);
	TestEqual(TEXT("repaired"), Stop.RepairPct, 50.0f);
	TestEqual(TEXT("total"), Stop.TotalS, 23.5f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexProtocolPitBoxTest,
	"ApexSim.Net.Protocol.PitBox",
	ApexTestFlags)

bool FApexProtocolPitBoxTest::RunTest(const FString& Parameters)
{
	FString Error;
	FApexServerMessage Message;
	if (!TestTrue(FString::Printf(TEXT("PitBox decodes (%s)"), *Error),
			ApexProtocol::DecodeServerMessage(ApexGolden::S_PitBox, Message, Error)))
	{
		return false;
	}
	TestEqual(TEXT("type"), Message.Type, EApexServerMessageType::PitBox);
	const FApexPitBox& Box = Message.PitBox;
	TestTrue(TEXT("valid"), Box.bValid);
	TestEqual(TEXT("box"), Box.PitBox, 4);
	TestEqual(TEXT("spot"), Box.Position, FVector(120.5, -36.25, 2.0));
	TestEqual(TEXT("yaw"), Box.YawRad, 1.5f);
	TestEqual(TEXT("lateral"), Box.LateralM, -2.5f);
	TestEqual(TEXT("width"), Box.LaneWidthM, 12.0f);
	TestEqual(TEXT("radius"), Box.RadiusM, 2.5f);
	TestFalse(TEXT("on the autopilot (left off the wire)"), Box.bManual);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexServerErrorsTest,
	"ApexSim.Net.ServerErrors",
	ApexTestFlags)

bool FApexServerErrorsTest::RunTest(const FString& Parameters)
{
	// A race event's refusals read as a sentence, whatever the server's words.
	for (const TCHAR* Reason : { TEXT("This event takes drivers by their proved driver id"), TEXT("Not entered in this event"),
		TEXT("This event's entry list is full"), TEXT("The livery is fixed for this event") })
	{
		const FString Text = ApexServerError::Describe(403, Reason);
		TestFalse(FString::Printf(TEXT("%s: no bare code"), Reason), Text.Contains(TEXT("403")));
		TestTrue(FString::Printf(TEXT("%s: a sentence"), Reason), Text.EndsWith(TEXT(".")));
		TestTrue(FString::Printf(TEXT("%s: names the event"), Reason), Text.Contains(TEXT("race event")));
	}
	// Any other refusal is the server's sentence; other codes keep theirs.
	TestEqual(TEXT("another 403"), ApexServerError::Describe(403, TEXT("Rewind needs you driving alone")),
		FString(TEXT("Rewind needs you driving alone.")));
	TestEqual(TEXT("a 409"), ApexServerError::Describe(409, TEXT("Race under way")),
		FString(TEXT("Server error 409: Race under way")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexProtocolOwnLiveryTest,
	"ApexSim.Net.Protocol.OwnLivery",
	ApexTestFlags)

bool FApexProtocolOwnLiveryTest::RunTest(const FString& Parameters)
{
	// The wire (network.rs `own_livery_wire_format`, livery.rs
	// `livery_package_wire_format`): the upload, the request, the chunk, the
	// roster naming the hash, and the package as the client packs it.
	auto CheckBytes = [this](const TCHAR* Label, const TArray<uint8>& Actual, TArrayView<const uint8> Expected)
	{
		TestEqual(FString::Printf(TEXT("%s: length"), Label), Actual.Num(), Expected.Num());
		TestTrue(FString::Printf(TEXT("%s: bytes"), Label),
			Actual.Num() == Expected.Num() && FMemory::Memcmp(Actual.GetData(), Expected.GetData(), Actual.Num()) == 0);
	};
	const FString Hash = FString::ChrN(64, TEXT('a')).Replace(TEXT("aa"), TEXT("ab"));
	const TArray<uint8> Png4 = {0x89, 0x50, 0x4E, 0x47};
	CheckBytes(TEXT("UploadLivery"), ApexProtocol::EncodeUploadLivery(Hash, 1, 2, Png4), ApexGolden::C_UploadLivery);
	CheckBytes(TEXT("RequestLivery"), ApexProtocol::EncodeRequestLivery(Hash, 1), ApexGolden::C_RequestLivery);

	FApexServerMessage Chunk;
	FString Error;
	if (TestTrue(TEXT("LiveryChunk decodes"), ApexProtocol::DecodeServerMessage(ApexGolden::S_LiveryChunk, Chunk, Error)))
	{
		TestEqual(TEXT("Chunk type"), Chunk.Type, EApexServerMessageType::LiveryChunk);
		TestEqual(TEXT("Chunk.Hash"), Chunk.LiveryChunk.Hash, Hash);
		TestEqual(TEXT("Chunk.Chunk"), Chunk.LiveryChunk.Chunk, 1);
		TestEqual(TEXT("Chunk.Chunks"), Chunk.LiveryChunk.Chunks, 2);
		TestEqual(TEXT("Chunk.Bytes"), Chunk.LiveryChunk.Bytes, Png4);
	}
	FApexServerMessage Missing;
	if (TestTrue(TEXT("LiveryChunk missing decodes"), ApexProtocol::DecodeServerMessage(ApexGolden::S_LiveryChunkMissing, Missing, Error)))
	{
		TestEqual(TEXT("Missing.Chunks"), Missing.LiveryChunk.Chunks, 0);
		TestEqual(TEXT("Missing.Bytes"), Missing.LiveryChunk.Bytes.Num(), 0);
	}
	FApexServerMessage Roster;
	if (TestTrue(TEXT("Roster decodes"), ApexProtocol::DecodeServerMessage(ApexGolden::S_SessionRosterOwnLivery, Roster, Error)))
	{
		TestEqual(TEXT("one entry"), Roster.Roster.Entries.Num(), 1);
		TestEqual(TEXT("OwnLivery"), Roster.Roster.Entries.Num() == 1 ? Roster.Roster.Entries[0].OwnLivery : FString(), Hash);
	}
	FApexServerMessage Plain;
	if (ApexProtocol::DecodeServerMessage(ApexGolden::S_SessionRosterPenalties, Plain, Error) && Plain.Roster.Entries.Num() > 0)
	{
		TestTrue(TEXT("no OwnLivery from a roster without it"), Plain.Roster.Entries[0].OwnLivery.IsEmpty());
		TestEqual(TEXT("no tyre-set rule: NewSetsLeft -1"), Plain.Roster.Entries[0].NewSetsLeft, -1);
		TestEqual(TEXT("no tyre-set rule: UsedSetWear -1"), Plain.Roster.Entries[0].UsedSetWear, -1);
	}
	FApexServerMessage Sets;
	if (TestTrue(TEXT("Roster with tyre sets decodes"), ApexProtocol::DecodeServerMessage(ApexGolden::S_SessionRosterTyreSets, Sets, Error))
		&& TestEqual(TEXT("one entry"), Sets.Roster.Entries.Num(), 1))
	{
		TestEqual(TEXT("NewSetsLeft"), Sets.Roster.Entries[0].NewSetsLeft, 0);
		TestEqual(TEXT("UsedSetWear"), Sets.Roster.Entries[0].UsedSetWear, 37);
	}

	// The package: packed to livery.rs's bytes, and read back.
	auto PngHeader = [](uint32 W, uint32 H)
	{
		TArray<uint8> B = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 13, 'I', 'H', 'D', 'R'};
		for (const uint32 V : {W, H})
		{
			B.Append({uint8(V >> 24), uint8(V >> 16), uint8(V >> 8), uint8(V)});
		}
		B.Append({8, 6, 0, 0, 0});
		return B;
	};
	FApexLiveryPackage Package;
	Package.Car = TEXT("99999999-8888-7777-6666-555555555555");
	Package.Name = TEXT("Mine");
	Package.Paint = FLinearColor(0.5f, 0.25f, 0.0f, 1.0f);
	Package.Accent = FLinearColor(1.0f, 1.0f, 1.0f, 1.0f);
	Package.Metallic = 0.5f;
	Package.Logo = PngHeader(2, 1);
	Package.Skin = {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x04, 0x4A, 0x46, 0xFF, 0xC0, 0x00, 0x11, 0x08, 0x00, 0x01, 0x00, 0x02,
		3, 3, 3, 3, 3, 3, 3, 3, 3, 3};
	Package.Textures.Emplace(TEXT("EXT_RIM"), PngHeader(1, 1));
	const TArray<uint8> Packed = ApexLiveryPackage::Encode(Package);
	CheckBytes(TEXT("LiveryPackage"), Packed, ApexGolden::C_LiveryPackage);
	FApexLiveryPackage Back;
	if (TestTrue(TEXT("the package reads back"), ApexLiveryPackage::Decode(Packed, Back, Error)))
	{
		TestEqual(TEXT("its car"), Back.Car, Package.Car);
		TestEqual(TEXT("its name"), Back.Name, Package.Name);
		TestEqual(TEXT("its paint"), Back.Paint, Package.Paint);
		TestEqual(TEXT("its metallic"), Back.Metallic, 0.5f);
		TestEqual(TEXT("its skin"), Back.Skin, Package.Skin);
		TestEqual(TEXT("its texture"), Back.Textures.Num() == 1 ? Back.Textures[0].Key : FString(), FString(TEXT("EXT_RIM")));
	}
	int32 W = 0;
	int32 H = 0;
	TestTrue(TEXT("a JPEG's size"), ApexLiveryPackage::ImageSize(Package.Skin, W, H) && W == 2 && H == 1);
	TestFalse(TEXT("a PNG past the side"), ApexLiveryPackage::ImageSize(PngHeader(8192, 4), W, H));

	// What the server refuses, the client refuses too.
	auto Refused = [&](TFunctionRef<void(FApexLiveryPackage&)> Edit)
	{
		FApexLiveryPackage Bad = Package;
		Edit(Bad);
		FApexLiveryPackage Out;
		FString Why;
		return !ApexLiveryPackage::Decode(ApexLiveryPackage::Encode(Bad), Out, Why) && !Why.IsEmpty();
	};
	TestTrue(TEXT("no car id"), Refused([](FApexLiveryPackage& P) { P.Car = TEXT("a car"); }));
	TestTrue(TEXT("no name"), Refused([](FApexLiveryPackage& P) { P.Name = TEXT("  "); }));
	TestTrue(TEXT("a colour past 1"), Refused([](FApexLiveryPackage& P) { P.Paint.R = 1.5f; }));
	TestTrue(TEXT("not an image"), Refused([](FApexLiveryPackage& P) { P.Logo = {'G', 'I', 'F', '8', '9', 'a'}; }));
	TestTrue(TEXT("nothing to paint"), Refused([](FApexLiveryPackage& P) { P.Paint.A = 0.0f; P.Skin.Reset(); P.Textures.Reset(); }));
	TestTrue(TEXT("a slot twice"), Refused([&](FApexLiveryPackage& P) { P.Textures.Emplace(TEXT("ext_rim"), PngHeader(1, 1)); }));
	TestTrue(TEXT("over the cap"), Refused([](FApexLiveryPackage& P) { P.Skin.SetNumZeroed(ApexLiveryPackage::MaxBytes); P.Skin[0] = 0xFF; }));
	FApexLiveryPackage Out;
	TestFalse(TEXT("not a package"), ApexLiveryPackage::Decode(TArray<uint8>({1, 2, 3}), Out, Error));

	// The hash is the server's: SHA-256, lowercase hex.
	const TArray<uint8> Abc = {'a', 'b', 'c'};
	TestEqual(TEXT("SHA-256 of abc"), ApexLiveryPackage::Hash(Abc),
		FString(TEXT("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")));
	TestEqual(TEXT("chunks of a package"), ApexLiveryPackage::ChunkCount(ApexLiveryPackage::MaxBytes), 4);
	TestEqual(TEXT("one chunk at least"), ApexLiveryPackage::ChunkCount(0), 1);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
