#include "Track/ApexPropLibrary.h"
#include "Race/ApexSkyModel.h"
#include "ApexTestCommon.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** A corner of a bay in the stand's frame: the bay's placement applied to a local point. */
	FVector StandBayCorner(const FTransform& Bay, double LocalX)
	{
		return Bay.TransformPosition(FVector(LocalX, 0.0, 0.0));
	}
}	 // namespace

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexPropAliasTest, "ApexSim.Props.Aliases", ApexTestFlags)

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexPropKindsTest, "ApexSim.Props.Kinds", ApexTestFlags)

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
	TestTrue(TEXT("a tent's open front looks at the road"), ApexProps::FacesRoad(TEXT("attraction"), TEXT("tent_6m")));
	TestFalse(TEXT("a camera tower has no front"), ApexProps::FacesRoad(TEXT("attraction"), TEXT("camera_tower")));
	TestTrue(TEXT("buildings have a front"), ApexProps::FacesRoad(TEXT("building"), TEXT("control_tower")));
	TestFalse(TEXT("parked cars are left as placed"), ApexProps::FacesRoad(TEXT("vehicle"), TEXT("car_b")));

	// Distance boards and light panels are read from the car, so they are
	// turned up the course rather than across it (and never flipped by side).
	TestTrue(TEXT("a braking marker looks up the course"),
		ApexProps::FacesUpCourse(TEXT("board"), TEXT("braking_marker")));
	TestFalse(TEXT("and so is not turned at the road"),
		ApexProps::FacesRoad(TEXT("board"), TEXT("braking_marker")));
	TestTrue(TEXT("a light panel looks up the course"),
		ApexProps::FacesUpCourse(TEXT("board"), TEXT("light_panel")));
	TestFalse(TEXT("a hoarding faces the road"),
		ApexProps::FacesUpCourse(TEXT("board"), TEXT("hoarding_3m")));
	TestTrue(TEXT("a hoarding is turned at the road"),
		ApexProps::FacesRoad(TEXT("board"), TEXT("hoarding_3m")));
	TestTrue(TEXT("a chevron board looks up the course"),
		ApexProps::FacesUpCourse(TEXT("board"), TEXT("chevron_left")));
	TestTrue(TEXT("so does a kilometre board"),
		ApexProps::FacesUpCourse(TEXT("board"), TEXT("km_marker")));
	TestTrue(TEXT("and a German road sign"),
		ApexProps::FacesUpCourse(TEXT("board"), TEXT("de_curve_right")));
	TestFalse(TEXT("a marshal post is not a board"),
		ApexProps::FacesUpCourse(TEXT("sign"), TEXT("braking_marker")));
	TestTrue(TEXT("the pit exit light looks up the lane"),
		ApexProps::FacesUpCourse(TEXT("sign"), TEXT("pit_exit_light")));
	TestTrue(TEXT("and so does the pit speed limit"),
		ApexProps::FacesUpCourse(TEXT("sign"), TEXT("pit_speed_limit")));
	TestEqual(TEXT("sky default"), ApexProps::DefaultAssetFor(TEXT("sky")), FString(TEXT("blimp")));
	TestEqual(TEXT("barrier default"), ApexProps::DefaultAssetFor(TEXT("barrier")), FString(TEXT("armco_4m")));
	TestEqual(TEXT("sign default"), ApexProps::DefaultAssetFor(TEXT("sign")), FString(TEXT("marshal_post")));
	TestEqual(TEXT("vehicle default"), ApexProps::DefaultAssetFor(TEXT("vehicle")), FString(TEXT("car_a")));
	TestTrue(TEXT("cones have no default"), ApexProps::DefaultAssetFor(TEXT("cone")).IsEmpty());
	TestTrue(TEXT("unknown kinds have no default"), ApexProps::DefaultAssetFor(TEXT("spaceship")).IsEmpty());
	TestNull(TEXT("unknown kind"), ApexProps::FindKind(TEXT("spaceship")));

	TestEqual(TEXT("mesh path"), ApexProps::MeshObjectPath(TEXT("/Game/Props"), TEXT("barrier"), TEXT("armco_4m")),
		FString(TEXT("/Game/Props/barrier/SM_armco_4m.SM_armco_4m")));
	TestEqual(TEXT("brand path"), ApexProps::BrandTextureObjectPath(TEXT("/Game/Props"), TEXT("piretti")),
		FString(TEXT("/Game/Props/board/Brands/T_brand_piretti.T_brand_piretti")));
	TestTrue(TEXT("brand slot"), ApexProps::IsBrandSlot(FName(TEXT("bridge_brand_piretti"))));
	TestFalse(TEXT("a post is not a brand slot"), ApexProps::IsBrandSlot(FName(TEXT("board_post"))));
	TestTrue(TEXT("foliage is masked"), ApexProps::IsMaskedSlot(FName(TEXT("tree_foliage_conifer"))));
	TestTrue(TEXT("crowd cards are masked"), ApexProps::IsMaskedSlot(FName(TEXT("crowd_cards"))));
	TestTrue(TEXT("a balloon carries a brand"), ApexProps::IsBrandSlot(FName(TEXT("balloon_envelope"))));
	TestTrue(TEXT("flag cloth"), ApexProps::IsFlagSlot(FName(TEXT("flag_cloth"))));
	TestFalse(TEXT("a flag is not a brand"), ApexProps::IsBrandSlot(FName(TEXT("flag_cloth"))));
	TestEqual(TEXT("flag path"), ApexProps::FlagTextureObjectPath(TEXT("/Game/Props"), TEXT("nl")),
		FString(TEXT("/Game/Props/sign/Flags/T_flag_nl.T_flag_nl")));
	TestTrue(TEXT("pit lights glow"), ApexProps::IsEmissiveSlot(FName(TEXT("pit_light_green"))));
	TestTrue(TEXT("both of them"), ApexProps::IsEmissiveSlot(FName(TEXT("pit_light_red"))));
	// The pit lane's signs are kit meshes of the sign kind, found by name: no alias.
	for (const TCHAR* Asset : {TEXT("pit_exit_light"), TEXT("pit_speed_limit")})
	{
		FString Kind = TEXT("sign");
		FString Key = Asset;
		FString Text = TEXT("80");
		TestFalse(*FString::Printf(TEXT("%s is not an alias"), Asset), ApexProps::ResolveAlias(Kind, Key, Text));
		TestTrue(*FString::Printf(TEXT("%s keeps its key"), Asset), Kind == TEXT("sign") && Key == Asset);
		TestEqual(*FString::Printf(TEXT("%s mesh path"), Asset), ApexProps::MeshObjectPath(TEXT("/Game/Props"), Kind, Key),
			FString::Printf(TEXT("/Game/Props/sign/SM_%s.SM_%s"), Asset, Asset));
	}

	// The dressing's variants: every stand module has a crowd twin, the
	// caps do not; broadleaf trees turn, conifers stay green.
	TestEqual(TEXT("bay crowd"), ApexProps::CrowdVariant(TEXT("grandstand"), TEXT("bay_10m_curve6_roof")),
		FString(TEXT("bay_10m_curve6_roof_crowd")));
	TestEqual(TEXT("scaffold crowd"), ApexProps::CrowdVariant(TEXT("grandstand"), TEXT("scaffold_10m")),
		FString(TEXT("scaffold_10m_crowd")));
	TestTrue(TEXT("caps have no crowd"), ApexProps::CrowdVariant(TEXT("grandstand"), TEXT("end_cap")).IsEmpty());
	TestTrue(TEXT("crowd is not doubled"),
		ApexProps::CrowdVariant(TEXT("grandstand"), TEXT("bay_10m_crowd")).IsEmpty());
	TestTrue(TEXT("only stands have crowds"), ApexProps::CrowdVariant(TEXT("pit"), TEXT("bay_10m")).IsEmpty());
	TestEqual(TEXT("broadleaf autumn"), ApexProps::AutumnVariant(TEXT("tree"), TEXT("broadleaf_l")),
		FString(TEXT("broadleaf_l_autumn")));
	TestEqual(TEXT("poplar autumn"), ApexProps::AutumnVariant(TEXT("tree"), TEXT("poplar")),
		FString(TEXT("poplar_autumn")));
	TestTrue(TEXT("conifers stay"), ApexProps::AutumnVariant(TEXT("tree"), TEXT("conifer_l")).IsEmpty());
	TestTrue(TEXT("autumn is not doubled"),
		ApexProps::AutumnVariant(TEXT("tree"), TEXT("poplar_autumn")).IsEmpty());

	TestEqual(TEXT("15 m road keeps the span"), ApexProps::BridgeSpanScale(15.0f), 1.0f);
	TestEqual(TEXT("12 m road shortens it"), ApexProps::BridgeSpanScale(12.0f), 0.8f);
	TestEqual(TEXT("no width leaves it"), ApexProps::BridgeSpanScale(0.0f), 1.0f);
	return true;
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexPropStraightStandTest, "ApexSim.Props.StraightStand", ApexTestFlags)

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

	// A club stand is the module repeated: no caps, never a wedge, even on
	// a tight bend.
	TestFalse(TEXT("scaffold is not a bay family"), ApexProps::IsBayFamily(TEXT("scaffold_10m")));
	TestTrue(TEXT("roofed bays are"), ApexProps::IsBayFamily(TEXT("bay_10m_large_roof")));
	bWedge = true;
	const ApexProps::FStandLayout Scaffold =
		ApexProps::LayoutGrandstand(TEXT("scaffold_10m"), 20.0f, TOptional<float>(48.0f), &bWedge);
	TestFalse(TEXT("scaffold stays straight"), bWedge);
	TestEqual(TEXT("scaffold module"), Scaffold.BayAsset, FString(TEXT("scaffold_10m")));
	TestTrue(TEXT("scaffold has no caps"), Scaffold.CapAsset.IsEmpty() && Scaffold.Caps.IsEmpty());
	TestEqual(TEXT("two modules"), Scaffold.Bays.Num(), 2);
	if (Scaffold.Bays.Num() == 2)
	{
		TestEqual(TEXT("module 0 x"), Scaffold.Bays[0].GetLocation().X, -500.0);
		TestEqual(TEXT("module 1 x"), Scaffold.Bays[1].GetLocation().X, 500.0);
	}
	return true;
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexPropWedgeStandTest, "ApexSim.Props.WedgeStand", ApexTestFlags)

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
			const FVector Right = StandBayCorner(Layout.Bays[i], 500.0);
			const FVector Left = StandBayCorner(Layout.Bays[i + 1], -500.0);
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
				Layout.Caps[0].GetLocation().Equals(StandBayCorner(Layout.Bays[0], -550.0), 0.01));
			TestTrue(What + TEXT(": right cap"),
				Layout.Caps[1].GetLocation().Equals(StandBayCorner(Layout.Bays[3], 550.0), 0.01));
		}
	}
	return true;
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexPropNightGlowTest, "ApexSim.Props.NightGlow", ApexTestFlags)

bool FApexPropNightGlowTest::RunTest(const FString& Parameters)
{
	// Every textured window slot of the Marina Bay pass, and the older pit one.
	const TCHAR* Windows[] = {TEXT("mb_glass_blue"), TEXT("mb_glass_teal"), TEXT("mb_glass_bronze"),
		TEXT("mb_glass_grey"), TEXT("mb_glass_clear"), TEXT("mb_classic"), TEXT("mb_colonial"), TEXT("mb_deco"),
		TEXT("pit_glass"), TEXT("pit_interior")};
	for (const TCHAR* Name : Windows)
	{
		ApexProps::FNightGlow Glow;
		TestTrue(FString(Name) + TEXT(" lights at night"), ApexProps::NightGlowOf(FName(Name), Glow));
		TestTrue(FString(Name) + TEXT(" has a level"), Glow.PeakNits > 0.0f);
		TestFalse(FString(Name) + TEXT(" keeps the imported material"), Glow.bEmissiveParent);
		TestFalse(FString(Name) + TEXT(" is not a fixed lamp"), ApexProps::IsEmissiveSlot(FName(Name)));
	}
	// The wheel's light strips, on both wheels' slots.
	for (const TCHAR* Name : {TEXT("ferris_lights"), TEXT("ferris_lights_rim"), TEXT("ferris_lights_hub")})
	{
		ApexProps::FNightGlow Glow;
		TestTrue(FString(Name) + TEXT(" lights at night"), ApexProps::NightGlowOf(FName(Name), Glow));
		TestFalse(FString(Name) + TEXT(" keeps the imported material"), Glow.bEmissiveParent);
	}
	// Start/finish straight: stand LED lines, globe lamp, balloons, step lights.
	for (const TCHAR* Name : {TEXT("stand_led_blue"), TEXT("stand_led_green"), TEXT("globe_lamp"),
			 TEXT("balloon_lamp_white"), TEXT("balloon_lamp_orange"), TEXT("step_light_orange")})
	{
		ApexProps::FNightGlow Glow;
		TestTrue(FString(Name) + TEXT(" lights at night"), ApexProps::NightGlowOf(FName(Name), Glow));
		TestTrue(FString(Name) + TEXT(" has a level"), Glow.PeakNits > 0.0f);
		TestFalse(FString(Name) + TEXT(" keeps the imported material"), Glow.bEmissiveParent);
	}
	ApexProps::FNightGlow Step;
	ApexProps::FNightGlow Globe;
	ApexProps::NightGlowOf(FName(TEXT("step_light_orange")), Step);
	ApexProps::NightGlowOf(FName(TEXT("globe_lamp")), Globe);
	TestTrue(TEXT("step nosings are dimmer than the globe lamps"), Step.PeakNits < Globe.PeakNits);
	// The other lit slots of the new props ride the existing paths.
	for (const TCHAR* Name : {TEXT("led_panel"), TEXT("led_screen"), TEXT("floodlight_lamp")})
	{
		TestTrue(FString(Name) + TEXT(" is an emissive slot"), ApexProps::IsEmissiveSlot(FName(Name)));
	}
	// A plain emissive panel is a builder-made emissive instance driven at night.
	ApexProps::FNightGlow Panel;
	TestTrue(TEXT("mb_lit_panel lights at night"), ApexProps::NightGlowOf(FName(TEXT("mb_lit_panel")), Panel));
	TestTrue(TEXT("mb_lit_panel is an emissive instance"), Panel.bEmissiveParent);
	TestTrue(TEXT("mb_lit_panel is an emissive slot"), ApexProps::IsEmissiveSlot(FName(TEXT("mb_lit_panel"))));
	// The signal heads are emissive lamps, but not night-driven.
	for (const TCHAR* Name : {TEXT("signal_red"), TEXT("signal_amber"), TEXT("signal_green")})
	{
		ApexProps::FNightGlow Glow;
		TestTrue(FString(Name) + TEXT(" is an emissive slot"), ApexProps::IsEmissiveSlot(FName(Name)));
		TestFalse(FString(Name) + TEXT(" is lit by day too"), ApexProps::NightGlowOf(FName(Name), Glow));
	}
	// Ordinary slots are left alone.
	ApexProps::FNightGlow None;
	TestFalse(TEXT("asphalt does not glow"), ApexProps::NightGlowOf(FName(TEXT("armco_galv")), None));
	TestFalse(TEXT("floodlight lamps are the lamp path"), ApexProps::NightGlowOf(FName(TEXT("floodlight_lamp")), None));

	// The sky: off in full day, on in full night, rising through twilight.
	TestEqual(TEXT("full day"), ApexSky::WindowGlowAt(1.0f), 0.0f);
	TestEqual(TEXT("full night"), ApexSky::WindowGlowAt(0.0f), 1.0f);
	TestTrue(TEXT("dusk is part way"), ApexSky::WindowGlowAt(0.25f) > 0.0f && ApexSky::WindowGlowAt(0.25f) < 1.0f);
	FApexSessionConditions Noon;
	Noon.TimeOfDayMinutes = 13 * 60;
	FApexSessionConditions Midnight = Noon;
	Midnight.TimeOfDayMinutes = 0;
	TestEqual(TEXT("sky at 13:00"), ApexSky::Derive(Noon).WindowGlow, 0.0f);
	TestEqual(TEXT("sky at midnight"), ApexSky::Derive(Midnight).WindowGlow, 1.0f);
	return true;
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexPropRotorTest, "ApexSim.Props.Rotor", ApexTestFlags)

bool FApexPropRotorTest::RunTest(const FString& Parameters)
{
	const ApexProps::FRotorSpec* Ferris = ApexProps::FindRotorSpec(TEXT("attraction"), TEXT("ferris_wheel"));
	if (!TestNotNull(TEXT("the ferris wheel turns"), Ferris))
	{
		return false;
	}
	TestEqual(TEXT("ferris rpm"), Ferris->Rpm, 0.5f);
	TestEqual(TEXT("ferris rotor mesh"), FString(Ferris->RotorAsset), FString(ApexProps::FerrisRotorAsset));
	TestTrue(TEXT("ferris hub 35 m up"), Ferris->HubOffsetCm.Equals(ApexProps::FerrisHubOffsetCm));

	const ApexProps::FRotorSpec* Xl = ApexProps::FindRotorSpec(TEXT("attraction"), TEXT("landmark_big_wheel_xl"));
	if (!TestNotNull(TEXT("the observation wheel turns"), Xl))
	{
		return false;
	}
	TestEqual(TEXT("xl rotor mesh"), FString(Xl->RotorAsset), FString(TEXT("landmark_big_wheel_xl_rotor")));
	// One turn in about half an hour.
	TestTrue(TEXT("xl turns once in 25-35 minutes"), Xl->Rpm > 1.0f / 35.0f && Xl->Rpm < 1.0f / 25.0f);
	// glTF translation [0, 90, 8] m -> Unreal (x, z, y) cm.
	TestTrue(TEXT("xl hub"), Xl->HubOffsetCm.Equals(FVector(0.0, 800.0, 9000.0)));

	TestNull(TEXT("a bridge does not turn"), ApexProps::FindRotorSpec(TEXT("bridge"), TEXT("start_gantry")));
	TestNull(TEXT("the rotor mesh is not itself a rotor asset"),
		ApexProps::FindRotorSpec(TEXT("attraction"), TEXT("ferris_wheel_rotor")));
	TestEqual(TEXT("two rotor assets"), ApexProps::AllRotorSpecs().Num(), 2);
	TestFalse(TEXT("the observation wheel has no front"),
		ApexProps::FacesRoad(TEXT("attraction"), TEXT("landmark_big_wheel_xl")));
	return true;
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexPropStreetStandTest, "ApexSim.Props.StreetStand", ApexTestFlags)

bool FApexPropStreetStandTest::RunTest(const FString& Parameters)
{
	// 50 m of stand: five bays, a cap at +-(25 + 2) m, even on a bend.
	for (TOptional<float> Radius : {TOptional<float>(), TOptional<float>(60.0f), TOptional<float>(-60.0f)})
	{
		bool bWedge = true;
		const ApexProps::FStandLayout Layout =
			ApexProps::LayoutGrandstand(TEXT("street_stand_tier_10m"), 50.0f, Radius, &bWedge);
		TestFalse(TEXT("never a wedge"), bWedge);
		TestEqual(TEXT("bay asset"), Layout.BayAsset, FString(TEXT("street_stand_tier_10m")));
		TestEqual(TEXT("cap asset"), Layout.CapAsset, FString(TEXT("street_stand_tier_end")));
		TestEqual(TEXT("five bays"), Layout.Bays.Num(), 5);
		TestEqual(TEXT("two caps"), Layout.Caps.Num(), 2);
		if (Layout.Bays.Num() == 5 && Layout.Caps.Num() == 2)
		{
			TestTrue(TEXT("bays straight at 10 m"), Layout.Bays[0].GetLocation().Equals(FVector(-2000, 0, 0), 0.01)
				&& Layout.Bays[4].GetLocation().Equals(FVector(2000, 0, 0), 0.01) && Layout.Bays[2].GetRotation().IsIdentity());
			TestTrue(TEXT("caps at +-(L/2 + 2)"), Layout.Caps[0].GetLocation().Equals(FVector(-2700, 0, 0), 0.01)
				&& Layout.Caps[1].GetLocation().Equals(FVector(2700, 0, 0), 0.01));
		}
	}
	// Rounded to whole bays, at least one.
	TestEqual(TEXT("a 4 m stand is one bay"),
		ApexProps::LayoutGrandstand(TEXT("street_stand_tier_10m"), 4.0f, TOptional<float>()).Bays.Num(), 1);
	// The roofed variant is the asset the scene names; the cap is shared.
	const ApexProps::FStandLayout Roof =
		ApexProps::LayoutGrandstand(TEXT("street_stand_tier_10m_roof"), 30.0f, TOptional<float>());
	TestEqual(TEXT("roof bay"), Roof.BayAsset, FString(TEXT("street_stand_tier_10m_roof")));
	TestEqual(TEXT("roof cap"), Roof.CapAsset, FString(TEXT("street_stand_tier_end")));
	TestEqual(TEXT("three roofed bays"), Roof.Bays.Num(), 3);
	// Crowd twins for the bays only.
	TestEqual(TEXT("crowd twin"), ApexProps::CrowdVariant(TEXT("grandstand"), TEXT("street_stand_tier_10m")),
		FString(TEXT("street_stand_tier_10m_crowd")));
	TestEqual(TEXT("roof crowd twin"), ApexProps::CrowdVariant(TEXT("grandstand"), TEXT("street_stand_tier_10m_roof")),
		FString(TEXT("street_stand_tier_10m_roof_crowd")));
	TestTrue(TEXT("the cap has no crowd"),
		ApexProps::CrowdVariant(TEXT("grandstand"), TEXT("street_stand_tier_end")).IsEmpty());
	TestTrue(TEXT("the deck has no crowd"),
		ApexProps::CrowdVariant(TEXT("grandstand"), TEXT("street_stand_deck_10m")).IsEmpty());
	// The deck is a plain tiled module with no caps.
	const ApexProps::FStandLayout Deck =
		ApexProps::LayoutGrandstand(TEXT("street_stand_deck_10m"), 40.0f, TOptional<float>(60.0f));
	TestEqual(TEXT("deck bay"), Deck.BayAsset, FString(TEXT("street_stand_deck_10m")));
	TestEqual(TEXT("four deck bays"), Deck.Bays.Num(), 4);
	TestEqual(TEXT("no deck caps"), Deck.Caps.Num(), 0);
	// The marina decal set rides with the graffiti.
	TestTrue(TEXT("marina decals imported"), ApexProps::DecalSets().Contains(TEXT("marina")));
	FString Set;
	FString Name;
	TestTrue(TEXT("marina decal key"), ApexProps::ParseDecalKey(TEXT("decal_marina_edge_yellow_blue_r"), Set, Name));
	TestEqual(TEXT("decal set"), Set, FString(TEXT("marina")));
	TestEqual(TEXT("decal name"), Name, FString(TEXT("edge_yellow_blue_r")));
	TestEqual(TEXT("decal texture"), ApexProps::DecalTextureObjectPath(TEXT("/Game/Props"), Set, Name),
		FString(TEXT("/Game/Props/decal/Marina/T_marina_edge_yellow_blue_r.T_marina_edge_yellow_blue_r")));
	// Road-facing pivots and the bridge scale the new props rely on.
	TestTrue(TEXT("pit wall gantry faces the road"), ApexProps::FacesRoad(TEXT("pit"), TEXT("pit_wall_gantry_6m")));
	TestTrue(TEXT("roof deck faces the road"), ApexProps::FacesRoad(TEXT("building"), TEXT("pit_building_roofdeck")));
	TestTrue(TEXT("banner gantry spans 20 m"), FMath::IsNearlyEqual(ApexProps::BridgeSpanScale(20.0f), 20.0f / 15.0f));
	return true;
}

#endif	  // WITH_DEV_AUTOMATION_TESTS
