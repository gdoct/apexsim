#include "ApexTestCommon.h"
#include "ApexPortraitSubsystem.h"
#include "ApexProfileSave.h"
#include "ApexProfiles.h"
#include "ApexSettingsSave.h"
#include "HAL/FileManager.h"
#include "UI/ApexProfileWidget.h"

#if WITH_DEV_AUTOMATION_TESTS

// What one session adds to a driver's stats: laps, distance, a race only
// when classified, a win only against someone, the best per circuit and car.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexProfileStatsTest, "ApexSim.Profile.Stats", ApexTestFlags)

bool FApexProfileStatsTest::RunTest(const FString& Parameters)
{
	FApexDriverStats Stats;
	FApexSessionStats Practice;
	Practice.Mode = EApexGameMode::FreePractice;
	Practice.TrackId = TEXT("track-a");
	Practice.CarId = TEXT("car-a");
	Practice.Laps = 5;
	Practice.DistanceM = 21000.0;
	Practice.BestLapSeconds = 90.5f;
	Practice.Incidents = 1;
	ApexProfiles::FileSession(Stats, Practice);
	TestEqual(TEXT("laps"), Stats.Laps, 5);
	TestEqual(TEXT("sessions"), Stats.Sessions, 1);
	TestEqual(TEXT("practice is no race"), Stats.Races, 0);
	TestEqual(TEXT("incidents"), Stats.Incidents, 1);

	FApexSessionStats Race = Practice;
	Race.Mode = EApexGameMode::Race;
	Race.FinishPosition = 1;
	Race.FieldSize = 8;
	Race.BestLapSeconds = 91.0f;
	ApexProfiles::FileSession(Stats, Race);
	TestEqual(TEXT("a race"), Stats.Races, 1);
	TestEqual(TEXT("a win"), Stats.Wins, 1);
	TestEqual(TEXT("a podium"), Stats.Podiums, 1);
	TestEqual(TEXT("the faster best stays"), Stats.CarBests.FindRef(ApexProfiles::CarBestKey(TEXT("track-a"), TEXT("car-a"))), 90.5f);

	FApexSessionStats Alone = Race;
	Alone.FieldSize = 1;
	Alone.CarId = TEXT("car-b");
	ApexProfiles::FileSession(Stats, Alone);
	TestEqual(TEXT("a race alone is a race"), Stats.Races, 2);
	TestEqual(TEXT("but no win"), Stats.Wins, 1);
	TestEqual(TEXT("one best per car"), Stats.CarBests.Num(), 2);

	FApexSessionStats Retired = Race;
	Retired.FinishPosition = 0;
	ApexProfiles::FileSession(Stats, Retired);
	TestEqual(TEXT("not classified, not a race"), Stats.Races, 2);
	TestEqual(TEXT("distance adds up"), Stats.DistanceM, 84000.0);

	const TArray<UApexProfileWidget::FBestRow> Rows = UApexProfileWidget::BestRows(Stats);
	if (TestEqual(TEXT("two PB rows"), Rows.Num(), 2))
	{
		TestEqual(TEXT("by track then car"), Rows[0].CarId, FString(TEXT("car-a")));
		TestEqual(TEXT("its time"), Rows[1].Seconds, 91.0f);
	}
	const TArray<TPair<FString, FString>> Lines = UApexProfileWidget::StatLines(Stats);
	TestEqual(TEXT("six stats"), Lines.Num(), 6);
	TestEqual(TEXT("distance in km"), Lines[1].Value, FString(TEXT("84 km")));
	return true;
}

// The profile list: the pre-profile slots become the first profile, a new
// one gets slots of its own, and the launch picks by -ApexProfile= (a name
// or a place in the list) before the one used last.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexProfileIndexTest, "ApexSim.Profile.Index", ApexTestFlags)

bool FApexProfileIndexTest::RunTest(const FString& Parameters)
{
	UApexProfileIndexSave* Index = NewObject<UApexProfileIndexSave>();
	const FGuid First = FGuid::NewGuid();
	ApexProfiles::SeedFirstProfile(*Index, TEXT("  Guido  "), First);
	TestEqual(TEXT("first on the old profile slot"), Index->Profiles[0].ProfileSlot, FString(UApexProfileSave::SlotName));
	TestEqual(TEXT("first on the old settings slot"), Index->Profiles[0].SettingsSlot, FString(UApexSettingsSave::SlotName));
	TestEqual(TEXT("name trimmed"), Index->Profiles[0].Name, FString(TEXT("Guido")));

	const FApexProfileEntry Second = ApexProfiles::MakeEntry(TEXT(""), 3);
	Index->Profiles.Add(Second);
	TestEqual(TEXT("an empty name is Player"), Second.Name, FString(TEXT("Player")));
	TestNotEqual(TEXT("own profile slot"), Second.ProfileSlot, Index->Profiles[0].ProfileSlot);
	TestNotEqual(TEXT("own settings slot"), Second.SettingsSlot, Index->Profiles[0].SettingsSlot);
	TestEqual(TEXT("a long name is cut"), ApexProfiles::CleanName(FString::ChrN(40, 'x')).Len(), ApexProfiles::MaxNameLength);

	TestEqual(TEXT("the last used"), ApexProfiles::Chosen(*Index, TEXT(""))->Id, First);
	Index->Current = Second.Id;
	TestEqual(TEXT("the last used, moved"), ApexProfiles::Chosen(*Index, TEXT(""))->Id, Second.Id);
	TestEqual(TEXT("by name"), ApexProfiles::Chosen(*Index, TEXT("-ApexProfile=guido"))->Id, First);
	TestEqual(TEXT("by place"), ApexProfiles::Chosen(*Index, TEXT("-ApexProfile=1"))->Id, First);
	TestEqual(TEXT("an unknown name: the last used"), ApexProfiles::Chosen(*Index, TEXT("-ApexProfile=nobody"))->Id, Second.Id);
	TestTrue(TEXT("the command line chose"), ApexProfiles::CommandLineChoosesProfile(TEXT("-game -ApexProfile=2")));
	TestFalse(TEXT("the command line did not"), ApexProfiles::CommandLineChoosesProfile(TEXT("-game -ApexAutoRace")));

	const FString Wire = ApexProfiles::WireId(First);
	TestEqual(TEXT("wire id is a UUID's text"), Wire.Len(), 36);
	TestEqual(TEXT("lower case"), Wire, Wire.ToLower());
	TestTrue(TEXT("no id, nothing on the wire"), ApexProfiles::WireId(FGuid()).IsEmpty());
	return true;
}

// The pictures are content: face_1.png to face_16.png in drivers/portraits.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexProfilePortraitsTest, "ApexSim.Profile.Portraits", ApexTestFlags)

bool FApexProfilePortraitsTest::RunTest(const FString& Parameters)
{
	const FString Dir = UApexPortraitSubsystem::Folder();
	for (int32 Index = 1; Index <= 16; ++Index)
	{
		TestTrue(*FString::Printf(TEXT("face_%d.png"), Index),
			IFileManager::Get().FileExists(*FPaths::Combine(Dir, FString::Printf(TEXT("face_%d.png"), Index))));
	}
	return true;
}

#endif
