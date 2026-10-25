#include "Misc/AutomationTest.h"

#include "ApexNetSubsystem.h"
#include "Engine/GameInstance.h"
#include "Tests/ApexGoldenBlobs.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	constexpr EAutomationTestFlags NetSubsystemTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexLocalWatchCornersTest, "ApexSim.Net.LocalWatchCorners", NetSubsystemTestFlags)

bool FApexLocalWatchCornersTest::RunTest(const FString& Parameters)
{
	UApexNetSubsystem* Net = NewObject<UApexNetSubsystem>(NewObject<UGameInstance>(GetTransientPackage()));
	FApexSessionConditions Sky;

	// A hotlap simulated here takes its corners as it begins; the backdrop
	// feed that carries its frames begins after and must not wipe them.
	Net->BeginLocalHotlapWatch(TEXT("track-id"), TEXT("Zandervoort"), TEXT("Zandervoort"), Sky, ApexGolden::S_TrackCorners);
	TestTrue(TEXT("the corners decoded"), Net->GetTrackCorners().IsValid());
	Net->BeginBackdropFeed(Sky);
	TestTrue(TEXT("the corners outlast the feed's start"), Net->GetTrackCorners().IsValid());
	TestEqual(TEXT("both corners"), Net->GetTrackCorners().Corners.Num(), 2);

	Net->EndLocalHotlapWatch();
	TestFalse(TEXT("gone with the watch"), Net->GetTrackCorners().IsValid());
	Net->EndBackdropFeed();

	// Any other backdrop starts from none.
	Net->BeginBackdropFeed(Sky);
	TestFalse(TEXT("a showcase starts without corners"), Net->GetTrackCorners().IsValid());
	Net->EndBackdropFeed();
	return true;
}

#endif
