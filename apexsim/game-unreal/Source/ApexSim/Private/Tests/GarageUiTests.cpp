#include "ApexTestCommon.h"
#include "Blueprint/WidgetTree.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Spacer.h"
#include "UI/ApexHotlapWidget.h"
#include "UI/ApexUIStyle.h"

#include "UI/ApexSessionBrowserWidget.h"

#if WITH_DEV_AUTOMATION_TESTS

// -----------------------------------------------------------------------------
// The garage's compound cards: the server sends a list for every car, and the
// default five must keep their hand-written notes and distinct bars rather
// than five cards saying "a compound of this car".
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexGarageCompoundCardsTest,
	"ApexSim.UI.Garage.CompoundCards",
	ApexTestFlags)

bool FApexGarageCompoundCardsTest::RunTest(const FString& Parameters)
{
	FApexCarSetupSheet Defaults;
	Defaults.Compounds = { TEXT("soft"), TEXT("medium"), TEXT("hard"), TEXT("intermediate"), TEXT("wet") };
	Defaults.ReferenceCompound = 1;
	const TArray<FApexGarageCompound> Cards = UApexHotlapWidget::CompoundCardsFor(&Defaults);
	if (!TestEqual(TEXT("five default cards"), Cards.Num(), 5))
	{
		return false;
	}
	TestEqual(TEXT("soft is +1"), Cards[0].Clicks, 1);
	TestEqual(TEXT("wet is -3"), Cards[4].Clicks, -3);
	TestTrue(TEXT("the soft grips more than the medium"), Cards[0].Grip > Cards[1].Grip);
	TestTrue(TEXT("the hard lasts longer than the medium"), Cards[2].Life > Cards[1].Life);
	TestFalse(TEXT("the soft keeps its own note"), Cards[0].Note.Contains(TEXT("compound of this car")));
	TestTrue(TEXT("the soft says its figures"), Cards[0].Figures.Contains(TEXT("WEAR")));

	// A car's own list names its cards and marks its reference.
	FApexCarSetupSheet Own;
	Own.Compounds = { TEXT("supersoft"), TEXT("soft"), TEXT("medium"), TEXT("wet") };
	Own.ReferenceCompound = 2;
	const TArray<FApexGarageCompound> OwnCards = UApexHotlapWidget::CompoundCardsFor(&Own);
	if (!TestEqual(TEXT("four own cards"), OwnCards.Num(), 4))
	{
		return false;
	}
	TestEqual(TEXT("named from the list"), OwnCards[0].Name, FString(TEXT("SUPERSOFT")));
	TestEqual(TEXT("the reference is click 0"), OwnCards[2].Clicks, 0);
	TestTrue(TEXT("softest grips most"), OwnCards[0].Grip > OwnCards[2].Grip);
	TestEqual(TEXT("the wet is a wet"), OwnCards[3].Note, FString(TEXT("Standing water.")));

	// A rally car's snow tyre is its own card, not a slick on the soft-to-hard ramp.
	FApexCarSetupSheet Rally;
	Rally.Compounds = { TEXT("gravel"), TEXT("snow") };
	Rally.ReferenceCompound = 0;
	const TArray<FApexGarageCompound> RallyCards = UApexHotlapWidget::CompoundCardsFor(&Rally);
	if (TestEqual(TEXT("two rally cards"), RallyCards.Num(), 2))
	{
		TestTrue(TEXT("the snow tyre says it is studded"), RallyCards[1].Note.Contains(TEXT("Studded")));
		TestTrue(TEXT("in ice white"), RallyCards[1].Colour.R > 0.5f && RallyCards[1].Colour.B > 0.5f);
		TestEqual(TEXT("the only slick is the reference's own"), RallyCards[0].Note, FString(TEXT("The car's own tyre.")));
	}
	return true;
}

// -----------------------------------------------------------------------------
// ApexUI::AddH / AddV fill at the weight they are given. They used to fill at
// weight 1 whatever was asked, so every share bar in the garage read half full.
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexUiFillWeightTest,
	"ApexSim.UI.Style.FillWeight",
	ApexTestFlags)

bool FApexUiFillWeightTest::RunTest(const FString& Parameters)
{
	UWidgetTree* Tree = NewObject<UWidgetTree>(GetTransientPackage());
	UHorizontalBox* Row = Tree->ConstructWidget<UHorizontalBox>();
	UHorizontalBoxSlot* Filled = ApexUI::AddH(Row, Tree->ConstructWidget<USpacer>(), FMargin(), VAlign_Fill, 0.8f);
	UHorizontalBoxSlot* Rest = ApexUI::AddH(Row, Tree->ConstructWidget<USpacer>(), FMargin(), VAlign_Fill, 0.2f);
	UHorizontalBoxSlot* Auto = ApexUI::AddH(Row, Tree->ConstructWidget<USpacer>());
	TestEqual(TEXT("fills"), Filled->GetSize().SizeRule, ESlateSizeRule::Fill);
	TestEqual(TEXT("at 0.8"), Filled->GetSize().Value, 0.8f);
	TestEqual(TEXT("the rest at 0.2"), Rest->GetSize().Value, 0.2f);
	TestEqual(TEXT("no weight is automatic"), Auto->GetSize().SizeRule, ESlateSizeRule::Automatic);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexSessionBrowserFilterTest, "ApexSim.UI.SessionBrowser", ApexTestFlags)

bool FApexSessionBrowserFilterTest::RunTest(const FString& Parameters)
{
	using namespace ApexBrowser;
	FApexSessionSummary Lobby;
	Lobby.State = EApexSessionState::Lobby;
	Lobby.PlayerCount = 2;
	Lobby.MaxPlayers = 8;
	FApexSessionSummary Full = Lobby;
	Full.PlayerCount = 8;
	FApexSessionSummary Racing = Lobby;
	Racing.State = EApexSessionState::Racing;
	FApexSessionSummary Over = Lobby;
	Over.State = EApexSessionState::Finished;

	TestTrue(TEXT("all shows a full one"), Passes(Full, EFilter::All));
	TestTrue(TEXT("a lobby with room is joinable"), Passes(Lobby, EFilter::Joinable));
	TestFalse(TEXT("a full one is not"), Passes(Full, EFilter::Joinable));
	TestFalse(TEXT("nor one that is over"), Passes(Over, EFilter::Joinable));
	FApexSessionSummary Waiting = Over;
	Waiting.bWeekendWaiting = true;
	TestTrue(TEXT("a weekend between phases is, though listed finished"), Passes(Waiting, EFilter::Joinable));
	TestFalse(TEXT("and is not over"), Waiting.IsOver());
	Waiting.PlayerCount = Waiting.MaxPlayers;
	TestFalse(TEXT("unless it is full"), Passes(Waiting, EFilter::Joinable));
	TestTrue(TEXT("a race under way is racing"), Passes(Racing, EFilter::Racing));
	TestFalse(TEXT("a lobby is not"), Passes(Lobby, EFilter::Racing));
	TestFalse(TEXT("the recorded list has no live session"), Passes(Racing, EFilter::Recorded));
	TestFalse(TEXT("nor has the events list"), Passes(Racing, EFilter::Events));

	// The damage rule in the row's line: only one that is not the default.
	TestTrue(TEXT("full damage goes unsaid"), DamageWords(EApexDamageLevel::Full).IsEmpty());
	TestEqual(TEXT("reduced"), DamageWords(EApexDamageLevel::Reduced), FString(TEXT("reduced damage")));
	TestEqual(TEXT("off"), DamageWords(EApexDamageLevel::Off), FString(TEXT("no damage")));
	TestEqual(TEXT("visual only"), DamageWords(EApexDamageLevel::VisualOnly), FString(TEXT("visual damage")));

	TArray<FRecording> Rows;
	TestTrue(TEXT("the server's list parses"), ParseRecordings(TEXT(
		"{\"sessions\":[{\"id\":\"77\",\"file\":\"replay_77_1760000000.apxs\",\"track\":\"Monzarella\","
		"\"kind\":\"Race\",\"recorded_at\":1760000000,\"duration_s\":612.5,\"cars\":12,\"humans\":1,"
		"\"laps\":5,\"winner\":\"Nils\",\"fastest\":null,\"results\":[]},{\"id\":\"old\",\"file\":null}]}"), Rows));
	if (TestEqual(TEXT("two rows"), Rows.Num(), 2))
	{
		TestEqual(TEXT("its file"), Rows[0].File, FString(TEXT("replay_77_1760000000.apxs")));
		TestEqual(TEXT("its cars"), Rows[0].Cars, 12);
		TestEqual(TEXT("its winner"), Rows[0].Winner, FString(TEXT("Nils")));
		TestTrue(TEXT("its length"), FMath::IsNearlyEqual(Rows[0].DurationS, 612.5));
		TestTrue(TEXT("one from before streams has no file"), Rows[1].File.IsEmpty());
	}
	TestFalse(TEXT("anything else is refused"), ParseRecordings(TEXT("Not Found"), Rows));
	return true;
}

#endif
