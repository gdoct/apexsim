#include "Race/ApexStreetLights.h"
#include "ApexTestCommon.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexLightsSpecsTest, "ApexSim.Lights.Specs", ApexTestFlags)

bool FApexLightsSpecsTest::RunTest(const FString& Parameters)
{
	const TArray<ApexLights::FLightSpec> Arm = ApexLights::SpecsFor(TEXT("SM_lamp_arm_twin"));
	if (!TestEqual(TEXT("an arm lamp has two heads"), Arm.Num(), 2))
	{
		return false;
	}
	TestTrue(TEXT("arms reach to either side"), Arm[0].LocalCm.Y * Arm[1].LocalCm.Y < 0.0
		&& FMath::IsNearlyEqual(FMath::Abs(Arm[0].LocalCm.Y), 340.0));
	TestTrue(TEXT("nine metres up"), FMath::IsNearlyEqual(Arm[0].LocalCm.Z, 920.0));
	TestTrue(TEXT("a spot with a wide cone"), Arm[0].bSpot && Arm[0].OuterConeDeg >= 100.0f);
	TestTrue(TEXT("it shines down"), Arm[0].AimLocal.Quaternion().GetForwardVector().Z < -0.95);
	TestTrue(TEXT("and leans toward the road"),
		Arm[0].AimLocal.Quaternion().GetForwardVector().Y * Arm[0].LocalCm.Y < 0.0);
	TestTrue(TEXT("warm white"), Arm[0].Color.R > Arm[0].Color.B);
	TestTrue(TEXT("28 m reach"), FMath::IsNearlyEqual(Arm[0].RadiusCm, 2800.0f));
	// The older lamps keep their old figures.
	const TArray<ApexLights::FLightSpec> Post = ApexLights::SpecsFor(TEXT("SM_lamp_post"));
	TestEqual(TEXT("a post is one spot"), Post.Num(), 1);
	TestTrue(TEXT("the arm lamp is brighter than the post"), Arm[0].Lumens > Post[0].Lumens);
	TestEqual(TEXT("a mast is one spot"), ApexLights::SpecsFor(TEXT("SM_floodlight_tower")).Num(), 1);
	// Points and the truss.
	const TArray<ApexLights::FLightSpec> Globe = ApexLights::SpecsFor(TEXT("SM_lamp_globe_pole"));
	TestTrue(TEXT("a globe is one point light"), Globe.Num() == 1 && !Globe[0].bSpot);
	const TArray<ApexLights::FLightSpec> White = ApexLights::SpecsFor(TEXT("SM_lamp_balloon_tether"));
	const TArray<ApexLights::FLightSpec> Orange = ApexLights::SpecsFor(TEXT("SM_lamp_balloon_tether_orange"));
	TestTrue(TEXT("a balloon is one point light"), White.Num() == 1 && Orange.Num() == 1 && !White[0].bSpot);
	TestTrue(TEXT("the orange balloon is orange"), Orange[0].Color.B < White[0].Color.B);
	TestTrue(TEXT("balloons reach 18 m"), FMath::IsNearlyEqual(White[0].RadiusCm, 1800.0f));
	const TArray<ApexLights::FLightSpec> Truss = ApexLights::SpecsFor(TEXT("SM_pit_light_truss_6m"));
	TestTrue(TEXT("a truss module is one downward spot"),
		Truss.Num() == 1 && Truss[0].bSpot && Truss[0].AimLocal.Quaternion().GetForwardVector().Z < -0.99);
	TestEqual(TEXT("a barrier has no light"), ApexLights::SpecsFor(TEXT("SM_armco_4m")).Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexLightsSelectTest, "ApexSim.Lights.Select", ApexTestFlags)

bool FApexLightsSelectTest::RunTest(const FString& Parameters)
{
	// Ten lights 100 m apart along a road, three allowed, 360 m range.
	TArray<float> Dist;
	for (int32 i = 0; i < 10; ++i)
	{
		Dist.Add(10000.0f * (i + 1));
	}
	TArray<uint8> On;
	int32 Count = ApexLights::Select(Dist, On, 3, 36000.0f);
	TestEqual(TEXT("three on"), Count, 3);
	TestTrue(TEXT("the nearest three"), (On[0] != 0) && (On[1] != 0) && (On[2] != 0) && !(On[3] != 0));

	// The hard range: only two are within it.
	Count = ApexLights::Select(Dist, On, 90, 20500.0f);
	TestEqual(TEXT("range cuts the rest"), Count, 2);

	// Hysteresis: with 0,1,2 on, a light 3 that is a touch nearer than 2 does not swap in.
	On.Init(0, 4);
	On[0] = On[1] = On[2] = 1;
	TArray<float> Edge = {1000.0f, 2000.0f, 3000.0f, 2900.0f};
	ApexLights::Select(Edge, On, 3, 26000.0f);
	TestTrue(TEXT("the incumbent stays"), (On[2] != 0) && !(On[3] != 0));
	// ... but a clearly nearer one does.
	Edge[3] = 1500.0f;
	ApexLights::Select(Edge, On, 3, 26000.0f);
	TestTrue(TEXT("a clearly nearer light swaps in"), (On[3] != 0) && !(On[2] != 0));

	// A light that is on stays a little past the range; one that is off does not come on there.
	TArray<float> Far = {26000.0f * 1.1f, 26000.0f * 1.1f};
	On.Init(0, 2);
	On[0] = 1;
	ApexLights::Select(Far, On, 5, 26000.0f);
	TestTrue(TEXT("sticky range"), (On[0] != 0) && !(On[1] != 0));
	Far[0] = 26000.0f * 1.3f;
	ApexLights::Select(Far, On, 5, 26000.0f);
	TestFalse(TEXT("out of the sticky range"), On[0] != 0);

	// Deterministic ties, no lights, zero budget, a resized flag array.
	TArray<float> Tie = {500.0f, 500.0f, 500.0f};
	TArray<uint8> Fresh;
	ApexLights::Select(Tie, Fresh, 2, 26000.0f);
	TestTrue(TEXT("ties go to the lower index"), Fresh.Num() == 3 && (Fresh[0] != 0) && (Fresh[1] != 0) && !(Fresh[2] != 0));
	TestEqual(TEXT("zero budget"), ApexLights::Select(Tie, Fresh, 0, 26000.0f), 0);
	TArray<float> None;
	TestEqual(TEXT("no lights"), ApexLights::Select(None, Fresh, 5, 26000.0f), 0);
	TestEqual(TEXT("flags follow the count"), Fresh.Num(), 0);
	return true;
}

#endif	  // WITH_DEV_AUTOMATION_TESTS
