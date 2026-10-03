#include "ApexTestCommon.h"
#include "Race/ApexSpectatorView.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexSpectateOrderTest, "ApexSim.Spectate.Order", ApexTestFlags)

bool FApexSpectateOrderTest::RunTest(const FString& Parameters)
{
	using namespace ApexSpectate;
	// Car 3 has finished second, car 1 won; of the cars still racing car 0
	// is a lap ahead of car 2 and car 4, who sit level (index breaks the tie).
	const TArray<FRunner> Runners = {
		{0, 0, 9000.0f},
		{1, 1, 9500.0f},
		{2, 0, 4000.0f},
		{3, 2, 9400.0f},
		{4, 0, 4000.0f},
	};
	const TArray<int32> Order = RaceOrder(Runners);
	TestEqual(TEXT("finishers first, then distance, then index"), Order, TArray<int32>({1, 3, 0, 2, 4}));

	TestEqual(TEXT("next behind"), Step(Order, 0, 1), 2);
	TestEqual(TEXT("next ahead"), Step(Order, 0, -1), 3);
	TestEqual(TEXT("wraps past last"), Step(Order, 4, 1), 1);
	TestEqual(TEXT("wraps past the leader"), Step(Order, 1, -1), 4);
	TestEqual(TEXT("a car not in the order starts from the leader"), Step(Order, 42, 1), 1);
	TestEqual(TEXT("no order, no car"), Step(TArray<int32>(), 0, 1), (int32)INDEX_NONE);

	TestEqual(TEXT("P1"), AtPosition(Order, 1), 1);
	TestEqual(TEXT("P5"), AtPosition(Order, 5), 4);
	TestEqual(TEXT("P6 is nobody"), AtPosition(Order, 6), (int32)INDEX_NONE);
	TestEqual(TEXT("P0 is nobody"), AtPosition(Order, 0), (int32)INDEX_NONE);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexSpectateCyclesTest, "ApexSim.Spectate.Cycles", ApexTestFlags)

bool FApexSpectateCyclesTest::RunTest(const FString& Parameters)
{
	using namespace ApexSpectate;
	TestEqual(TEXT("broadcast -> chase"), (int32)NextCamera(ECamera::Broadcast), (int32)ECamera::Chase);
	TestEqual(TEXT("chase -> onboard"), (int32)NextCamera(ECamera::Chase), (int32)ECamera::Onboard);
	TestEqual(TEXT("onboard -> broadcast"), (int32)NextCamera(ECamera::Onboard), (int32)ECamera::Broadcast);
	TestEqual(TEXT("camera name"), FString(CameraName(ECamera::Onboard)), FString(TEXT("ONBOARD")));

	// Every tower column comes round once, each with its own key.
	TSet<FString> Keys;
	ETowerMode Mode = ETowerMode::Interval;
	for (int32 Step = 0; Step < static_cast<int32>(ETowerMode::Count); ++Step)
	{
		Keys.Add(TowerModeKey(Mode));
		Mode = NextTowerMode(Mode);
	}
	TestEqual(TEXT("back to the interval"), (int32)Mode, (int32)ETowerMode::Interval);
	TestEqual(TEXT("five columns"), Keys.Num(), 5);
	TestTrue(TEXT("tyres column"), Keys.Contains(TEXT("tyres")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexSpectateKeysTest, "ApexSim.Spectate.Keys", ApexTestFlags)

bool FApexSpectateKeysTest::RunTest(const FString& Parameters)
{
	using namespace ApexSpectate;
	auto ActionOf = [](const FKey& Key) { return static_cast<int32>(CommandFor(Key).Action); };
	TestEqual(TEXT("up is the car ahead"), ActionOf(EKeys::Up), (int32)EAction::PreviousCar);
	TestEqual(TEXT("right is the car behind"), ActionOf(EKeys::Right), (int32)EAction::NextCar);
	TestEqual(TEXT("left shoulder"), ActionOf(EKeys::Gamepad_LeftShoulder), (int32)EAction::PreviousCar);
	TestEqual(TEXT("right shoulder"), ActionOf(EKeys::Gamepad_RightShoulder), (int32)EAction::NextCar);
	TestEqual(TEXT("C camera"), ActionOf(EKeys::C), (int32)EAction::Camera);
	TestEqual(TEXT("Y camera"), ActionOf(EKeys::Gamepad_FaceButton_Top), (int32)EAction::Camera);
	TestEqual(TEXT("A auto"), ActionOf(EKeys::A), (int32)EAction::Auto);
	TestEqual(TEXT("T tower"), ActionOf(EKeys::T), (int32)EAction::Tower);
	TestEqual(TEXT("H overlay"), ActionOf(EKeys::H), (int32)EAction::Overlay);
	TestEqual(TEXT("N next race"), ActionOf(EKeys::N), (int32)EAction::NextRace);
	TestEqual(TEXT("B leaves"), ActionOf(EKeys::Gamepad_FaceButton_Right), (int32)EAction::Leave);
	// The pause key is not the watch view's.
	TestEqual(TEXT("Escape is the pause menu's"), ActionOf(EKeys::Escape), (int32)EAction::None);
	TestEqual(TEXT("Space pauses a replay"), ActionOf(EKeys::SpaceBar), (int32)EAction::PlayPause);
	TestEqual(TEXT("pad A pauses a replay"), ActionOf(EKeys::Gamepad_FaceButton_Bottom), (int32)EAction::PlayPause);
	TestEqual(TEXT("comma seeks back"), ActionOf(EKeys::Comma), (int32)EAction::SeekBack);
	TestEqual(TEXT("right trigger seeks on"), ActionOf(EKeys::Gamepad_RightTrigger), (int32)EAction::SeekForward);
	TestEqual(TEXT("= is faster"), ActionOf(EKeys::Equals), (int32)EAction::Faster);

	TestEqual(TEXT("faster from real time"), StepPlaybackRate(1.0f, 1), 2.0f);
	TestEqual(TEXT("slower from real time"), StepPlaybackRate(1.0f, -1), 0.5f);
	TestEqual(TEXT("no faster than 4x"), StepPlaybackRate(4.0f, 1), 4.0f);
	TestEqual(TEXT("no slower than a quarter"), StepPlaybackRate(0.25f, -1), 0.25f);

	const FCommand Third = CommandFor(EKeys::Three);
	TestTrue(TEXT("3 is P3"), Third.Action == EAction::Position && Third.Position == 3);
	const FCommand Tenth = CommandFor(EKeys::Zero);
	TestTrue(TEXT("0 is P10"), Tenth.Action == EAction::Position && Tenth.Position == 10);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
