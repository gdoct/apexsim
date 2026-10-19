#include "ApexTestCommon.h"
#include "UI/ApexLeaderboardWidget.h"

#if WITH_DEV_AUTOMATION_TESTS

// The leaderboard's lines: ranks in the server's order, the gap to the fastest,
// the player's own laps flagged, the car named through the catalog.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexLeaderboardRowsTest, "ApexSim.Leaderboard.Rows", ApexTestFlags)

bool FApexLeaderboardRowsTest::RunTest(const FString& Parameters)
{
	auto Entry = [](const TCHAR* Name, const TCHAR* Car, int32 Ms, const TCHAR* When)
	{
		FApexLeaderboardEntry E;
		E.PlayerName = Name;
		E.CarConfigId = Car;
		E.LapTimeMs = Ms;
		E.RecordedAt = When;
		return E;
	};
	TArray<FApexLeaderboardEntry> Entries;
	Entries.Add(Entry(TEXT("Luna Swift"), TEXT("gt3"), 91234, TEXT("2026-10-06T12:00:00Z")));
	Entries.Add(Entry(TEXT("Guido"), TEXT("gt3"), 91876, TEXT("2026-10-07T09:30:00Z")));
	Entries.Add(Entry(TEXT("Guido"), TEXT("f1"), 120001, TEXT("")));

	const TArray<UApexLeaderboardWidget::FRow> Rows = UApexLeaderboardWidget::MakeRows(Entries, TEXT("guido"),
		[](const FString& Id) { return Id == TEXT("gt3") ? FString(TEXT("GT3 Car")) : FString(TEXT("-")); });
	if (!TestEqual(TEXT("a row per lap"), Rows.Num(), 3))
	{
		return false;
	}
	TestEqual(TEXT("first rank"), Rows[0].Rank, 1);
	TestEqual(TEXT("the leader's time"), Rows[0].Time, FString(TEXT("1:31.234")));
	TestEqual(TEXT("the leader has no gap"), Rows[0].Gap, FString());
	TestEqual(TEXT("the gap to the leader"), Rows[1].Gap, FString(TEXT("+0.642")));
	TestEqual(TEXT("a longer gap"), Rows[2].Gap, FString(TEXT("+28.767")));
	TestEqual(TEXT("the day"), Rows[1].When, FString(TEXT("2026-10-07")));
	TestEqual(TEXT("no date"), Rows[2].When, FString());
	TestEqual(TEXT("the car's name"), Rows[0].Car, FString(TEXT("GT3 Car")));
	TestEqual(TEXT("an unknown car"), Rows[2].Car, FString(TEXT("-")));
	TestTrue(TEXT("the player's laps are theirs, whatever the case"), !Rows[0].bMine && Rows[1].bMine && Rows[2].bMine);
	TestEqual(TEXT("no laps, no rows"), UApexLeaderboardWidget::MakeRows(TArray<FApexLeaderboardEntry>(), TEXT("guido"),
		[](const FString&) { return FString(); }).Num(), 0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
