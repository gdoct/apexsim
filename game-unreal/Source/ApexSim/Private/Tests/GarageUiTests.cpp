#include "ApexTestCommon.h"
#include "Blueprint/WidgetTree.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Spacer.h"
#include "UI/ApexHotlapWidget.h"
#include "UI/ApexUIStyle.h"

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

#endif
