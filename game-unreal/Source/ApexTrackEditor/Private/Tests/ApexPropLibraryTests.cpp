#include "ApexPropLibrary.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	constexpr EAutomationTestFlags ApexPropTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter;

	/** A corner of a bay in the stand's frame: the bay's placement applied to a local point. */
	FVector Corner(const FTransform& Bay, double LocalX)
	{
		return Bay.TransformPosition(FVector(LocalX, 0.0, 0.0));
	}
}	 // namespace

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexPropAliasTest, "ApexSim.Props.Aliases", ApexPropTestFlags)

bool FApexPropAliasTest::RunTest(const FString& Parameters)
{
	// The groomer's distance boards: a kind change, and the text the old
	// key implied when the prop carries none.
	FString Kind = TEXT("sign");
	FString Asset = TEXT("board_200m");
	FString Text;
	TestTrue(TEXT("board alias applies"), ApexProps::ResolveAlias(Kind, Asset, Text));
	TestEqual(TEXT("board kind"), Kind, FString(TEXT("board")));
	TestEqual(TEXT("board asset"), Asset, FString(TEXT("braking_marker")));
	TestEqual(TEXT("board text"), Text, FString(TEXT("200")));

	Kind = TEXT("sign");
	Asset = TEXT("board_50m");
	Text = TEXT("60");
	ApexProps::ResolveAlias(Kind, Asset, Text);
	TestEqual(TEXT("authored text wins"), Text, FString(TEXT("60")));

	Kind = TEXT("tree");
	Asset = TEXT("tree_generic");
	Text.Empty();
	TestTrue(TEXT("tree alias applies"), ApexProps::ResolveAlias(Kind, Asset, Text));
	TestEqual(TEXT("tree asset"), Asset, FString(TEXT("broadleaf_m")));
	TestTrue(TEXT("no text implied"), Text.IsEmpty());

	Kind = TEXT("barrier");
	Asset = TEXT("tecpro_2m");
	TestFalse(TEXT("authored keys pass through"), ApexProps::ResolveAlias(Kind, Asset, Text));
	TestEqual(TEXT("untouched"), Asset, FString(TEXT("tecpro_2m")));
	return true;
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexPropKindsTest, "ApexSim.Props.Kinds", ApexPropTestFlags)

bool FApexPropKindsTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("barriers are instanced"), ApexProps::IsInstancedKind(TEXT("barrier")));
	TestTrue(TEXT("barriers face the road"), ApexProps::FacesRoad(TEXT("barrier"), TEXT("armco_4m")));
	TestFalse(TEXT("barriers are not Nanite"), ApexProps::IsNaniteKind(TEXT("barrier")));
	TestTrue(TEXT("stands are Nanite"), ApexProps::IsNaniteKind(TEXT("grandstand")));
	TestFalse(TEXT("stands are not instanced per prop"), ApexProps::IsInstancedKind(TEXT("grandstand")));
	TestFalse(TEXT("bridges are never flipped"), ApexProps::FacesRoad(TEXT("bridge"), TEXT("start_gantry")));
	TestFalse(TEXT("trees are never flipped"), ApexProps::FacesRoad(TEXT("tree"), TEXT("poplar")));
	TestTrue(TEXT("a video screen has a front"), ApexProps::FacesRoad(TEXT("attraction"), TEXT("video_screen")));
	TestFalse(TEXT("a ferris wheel has none"), ApexProps::FacesRoad(TEXT("attraction"), TEXT("ferris_wheel")));
	TestEqual(TEXT("sky default"), ApexProps::DefaultAssetFor(TEXT("sky")), FString(TEXT("blimp")));
	TestEqual(TEXT("barrier default"), ApexProps::DefaultAssetFor(TEXT("barrier")), FString(TEXT("armco_4m")));
	TestTrue(TEXT("signs have no default"), ApexProps::DefaultAssetFor(TEXT("sign")).IsEmpty());
	TestTrue(TEXT("unknown kinds have no default"), ApexProps::DefaultAssetFor(TEXT("spaceship")).IsEmpty());
	TestNull(TEXT("unknown kind"), ApexProps::FindKind(TEXT("spaceship")));

	TestEqual(TEXT("mesh path"), ApexProps::MeshObjectPath(TEXT("/Game/Props"), TEXT("barrier"), TEXT("armco_4m")),
		FString(TEXT("/Game/Props/barrier/SM_armco_4m.SM_armco_4m")));
	TestEqual(TEXT("brand path"), ApexProps::BrandTextureObjectPath(TEXT("/Game/Props"), TEXT("piretti")),
		FString(TEXT("/Game/Props/board/Brands/T_brand_piretti.T_brand_piretti")));
	TestTrue(TEXT("brand slot"), ApexProps::IsBrandSlot(FName(TEXT("bridge_brand_piretti"))));
	TestFalse(TEXT("a post is not a brand slot"), ApexProps::IsBrandSlot(FName(TEXT("board_post"))));
	TestTrue(TEXT("foliage is masked"), ApexProps::IsMaskedSlot(FName(TEXT("tree_foliage_conifer"))));

	TestEqual(TEXT("15 m road keeps the span"), ApexProps::BridgeSpanScale(15.0f), 1.0f);
	TestEqual(TEXT("12 m road shortens it"), ApexProps::BridgeSpanScale(12.0f), 0.8f);
	TestEqual(TEXT("no width leaves it"), ApexProps::BridgeSpanScale(0.0f), 1.0f);
	return true;
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexPropStraightStandTest, "ApexSim.Props.StraightStand", ApexPropTestFlags)

bool FApexPropStraightStandTest::RunTest(const FString& Parameters)
{
	bool bWedge = true;
	const ApexProps::FStandLayout Layout =
		ApexProps::LayoutGrandstand(TEXT("bay_10m_large_roof"), 30.0f, TOptional<float>(), &bWedge);
	TestFalse(TEXT("straight"), bWedge);
	TestEqual(TEXT("family kept"), Layout.BayAsset, FString(TEXT("bay_10m_large_roof")));
	TestEqual(TEXT("large cap"), Layout.CapAsset, FString(TEXT("end_cap_large")));
	TestEqual(TEXT("three bays"), Layout.Bays.Num(), 3);
	TestEqual(TEXT("two caps"), Layout.Caps.Num(), 2);
	if (Layout.Bays.Num() == 3 && Layout.Caps.Num() == 2)
	{
		TestEqual(TEXT("bay 0 x"), Layout.Bays[0].GetLocation().X, -1000.0);
		TestEqual(TEXT("bay 1 x"), Layout.Bays[1].GetLocation().X, 0.0);
		TestEqual(TEXT("bay 2 x"), Layout.Bays[2].GetLocation().X, 1000.0);
		TestEqual(TEXT("left cap"), Layout.Caps[0].GetLocation().X, -1550.0);
		TestEqual(TEXT("right cap"), Layout.Caps[1].GetLocation().X, 1550.0);
		TestTrue(TEXT("no turn"), Layout.Bays[2].GetRotation().IsIdentity(1e-6));
	}

	// The large family never curves, and a stand keeps at least one bay.
	const ApexProps::FStandLayout Large =
		ApexProps::LayoutGrandstand(TEXT("bay_10m_large"), 4.0f, TOptional<float>(60.0f), &bWedge);
	TestFalse(TEXT("large stays straight"), bWedge);
	TestEqual(TEXT("at least one bay"), Large.Bays.Num(), 1);

	// A gentle bend is straight too.
	ApexProps::LayoutGrandstand(TEXT("bay_10m"), 30.0f, TOptional<float>(400.0f), &bWedge);
	TestFalse(TEXT("400 m radius is straight"), bWedge);
	return true;
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexPropWedgeStandTest, "ApexSim.Props.WedgeStand", ApexPropTestFlags)

bool FApexPropWedgeStandTest::RunTest(const FString& Parameters)
{
	struct FCase
	{
		const TCHAR* Family;
		float RadiusM;
		const TCHAR* ExpectedBay;
		float ThetaDeg;
	};
	const FCase Cases[] = {
		{TEXT("bay_10m"), 48.0f, TEXT("bay_10m_curve12"), 12.0f},
		{TEXT("bay_10m_roof"), 95.0f, TEXT("bay_10m_curve6_roof"), 6.0f},
		{TEXT("bay_10m"), 140.0f, TEXT("bay_10m_curve6"), 6.0f},
		{TEXT("bay_10m_roof"), -60.0f, TEXT("bay_10m_curve6_in_roof"), 6.0f},
	};
	for (const FCase& Case : Cases)
	{
		bool bWedge = false;
		const ApexProps::FStandLayout Layout =
			ApexProps::LayoutGrandstand(Case.Family, 40.0f, TOptional<float>(Case.RadiusM), &bWedge);
		const FString What = FString::Printf(TEXT("%s at %.0f m"), Case.Family, Case.RadiusM);
		TestTrue(What + TEXT(": wedge"), bWedge);
		TestEqual(What + TEXT(": bay asset"), Layout.BayAsset, FString(Case.ExpectedBay));
		TestEqual(What + TEXT(": four bays"), Layout.Bays.Num(), 4);
		if (Layout.Bays.Num() != 4)
		{
			continue;
		}
		// Neighbours are turned by the wedge angle and share a front corner
		// exactly, so a run of wedges tiles without a gap or an overlap.
		for (int32 i = 0; i + 1 < Layout.Bays.Num(); ++i)
		{
			const FQuat Step = Layout.Bays[i].GetRotation().Inverse() * Layout.Bays[i + 1].GetRotation();
			TestTrue(What + TEXT(": neighbours turn by theta"),
				FMath::IsNearlyEqual(FMath::Abs(Step.Rotator().Yaw), Case.ThetaDeg, 0.01f));
			const FVector Right = Corner(Layout.Bays[i], 500.0);
			const FVector Left = Corner(Layout.Bays[i + 1], -500.0);
			TestTrue(What + TEXT(": shared corner"), Right.Equals(Left, 0.5));
		}
		// Bays run along +X in order, whichever way they turn.
		TestTrue(What + TEXT(": ordered along x"),
			Layout.Bays[0].GetLocation().X < Layout.Bays[1].GetLocation().X
				&& Layout.Bays[2].GetLocation().X < Layout.Bays[3].GetLocation().X);
		// The curve bulges toward the road (+Y) outside a corner and away
		// from it inside, and is symmetric about the stand's centre.
		const double Bulge = Layout.Bays[0].GetLocation().Y;
		TestTrue(What + TEXT(": bulge side"), Case.RadiusM > 0.0f ? Bulge > 0.0 : Bulge < 0.0);
		TestTrue(What + TEXT(": symmetric"),
			FMath::IsNearlyEqual(Layout.Bays[0].GetLocation().Y, Layout.Bays[3].GetLocation().Y, 0.01));
		// Caps sit half a metre past the outer bays' edges, turned with them.
		TestEqual(What + TEXT(": two caps"), Layout.Caps.Num(), 2);
		if (Layout.Caps.Num() == 2)
		{
			TestTrue(What + TEXT(": left cap"),
				Layout.Caps[0].GetLocation().Equals(Corner(Layout.Bays[0], -550.0), 0.01));
			TestTrue(What + TEXT(": right cap"),
				Layout.Caps[1].GetLocation().Equals(Corner(Layout.Bays[3], 550.0), 0.01));
		}
	}
	return true;
}

#endif	  // WITH_DEV_AUTOMATION_TESTS
