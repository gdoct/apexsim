#include "ApexTestCommon.h"
#include "Race/ApexCarDriver.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexDriverVisibilityTest, "ApexSim.Driver.Visibility", ApexTestFlags)

bool FApexDriverVisibilityTest::RunTest(const FString& Parameters)
{
	// Another car, or the player's own seen from outside: drawn.
	TestTrue(TEXT("drawn on a shown car"), ApexDriver::IsDrawn(true, true, true));
	TestFalse(TEXT("no shadow of his own needed when drawn"), ApexDriver::CastsHiddenShadow(true, true, true));
	// The car the cockpit camera rides: the camera is his eyes.
	TestFalse(TEXT("hidden from his own eyes"), ApexDriver::IsDrawn(true, true, false));
	TestTrue(TEXT("but still shades the cockpit"), ApexDriver::CastsHiddenShadow(true, true, false));
	// The bodywork switched off (or a demo hiding the world): no driver, no shadow.
	TestFalse(TEXT("goes with the bodywork"), ApexDriver::IsDrawn(true, false, true));
	TestFalse(TEXT("no shadow without the bodywork"), ApexDriver::CastsHiddenShadow(true, false, false));
	// A car without a [driver] table.
	TestFalse(TEXT("nothing to draw"), ApexDriver::IsDrawn(false, true, true));
	TestFalse(TEXT("nothing to shade"), ApexDriver::CastsHiddenShadow(false, true, false));
	return true;
}

#endif
