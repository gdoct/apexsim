#include "ApexTestCommon.h"
#include "Race/ApexSpectatorView.h"
#include "Input/ApexInputConfig.h"

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

	TestEqual(TEXT("W the weather"), ActionOf(EKeys::W), (int32)EAction::Weather);
	TestEqual(TEXT("[ an hour earlier"), ActionOf(EKeys::LeftBracket), (int32)EAction::TimeEarlier);
	TestEqual(TEXT("] an hour later"), ActionOf(EKeys::RightBracket), (int32)EAction::TimeLater);
	TestEqual(TEXT("page up the previous circuit"), ActionOf(EKeys::PageUp), (int32)EAction::PreviousTrack);
	TestEqual(TEXT("page down the next circuit"), ActionOf(EKeys::PageDown), (int32)EAction::NextTrack);

	// The pad's D-pad sets a replay's speed; up and down still step through the field.
	TestEqual(TEXT("D-pad left is slower"), ActionOf(EKeys::Gamepad_DPad_Left), (int32)EAction::Slower);
	TestEqual(TEXT("D-pad right is faster"), ActionOf(EKeys::Gamepad_DPad_Right), (int32)EAction::Faster);
	TestEqual(TEXT("D-pad up is the car ahead"), ActionOf(EKeys::Gamepad_DPad_Up), (int32)EAction::PreviousCar);
	TestEqual(TEXT("D-pad down is the car behind"), ActionOf(EKeys::Gamepad_DPad_Down), (int32)EAction::NextCar);
	TestEqual(TEXT("the left arrow still steps"), ActionOf(EKeys::Left), (int32)EAction::PreviousCar);

	// A rebound key is the new one's, and the old one is nobody's.
	{
		auto Rebound = [](FName ActionId, int32 Slot) -> FKey
		{
			if (ActionId == ApexInput::Actions::WatchTower && Slot == ApexInput::Slot::Keyboard)
			{
				return EKeys::J;
			}
			const ApexInput::FSlotDef* Def = ApexInput::FindSlot(ActionId, Slot);
			return Def ? Def->DefaultKey : FKey();
		};
		TestEqual(TEXT("J is the tower now"), (int32)CommandFor(EKeys::J, Rebound).Action, (int32)EAction::Tower);
		TestEqual(TEXT("T no longer is"), (int32)CommandFor(EKeys::T, Rebound).Action, (int32)EAction::None);
		TestEqual(TEXT("the pad's tower key stays"), (int32)CommandFor(EKeys::Gamepad_Special_Left, Rebound).Action, (int32)EAction::Tower);
		TestEqual(TEXT("the others stay"), (int32)CommandFor(EKeys::C, Rebound).Action, (int32)EAction::Camera);
		TestEqual(TEXT("an unbound slot owns nothing"), (int32)CommandFor(FKey(), Rebound).Action, (int32)EAction::None);
	}
	// The slot table has a slot for every command and none of its keys clashes with another's.
	{
		TSet<FKey> Seen;
		for (const ApexInput::FSlotDef& Def : ApexInput::Slots())
		{
			if (ApexInput::IsWatchAction(Def.ActionId) && Def.DefaultKey.IsValid())
			{
				TestFalse(*FString::Printf(TEXT("%s has its own default key"), *Def.ActionId.ToString()), Seen.Contains(Def.DefaultKey));
				Seen.Add(Def.DefaultKey);
			}
		}
		for (uint8 Raw = static_cast<uint8>(EAction::PreviousCar); Raw <= static_cast<uint8>(EAction::NextTrack); ++Raw)
		{
			if (Raw == static_cast<uint8>(EAction::Position))
			{
				continue;
			}
			const FName Id = ActionIdOf(static_cast<EAction>(Raw));
			TestTrue(*FString::Printf(TEXT("action %d has a slot"), Raw), !Id.IsNone() && ApexInput::FindSlot(Id, ApexInput::Slot::Keyboard) != nullptr);
		}
		TestEqual(TEXT("a watch key is no clash with a driving key"),
			(int32)ApexInput::ContextOf(ApexInput::Actions::WatchCamera), (int32)ApexInput::EContext::Watch);
		TestEqual(TEXT("the camera key is a driving key too"),
			(int32)ApexInput::ContextOf(ApexInput::Actions::ToggleCamera), (int32)ApexInput::EContext::Driving);
	}

	const FCommand Third = CommandFor(EKeys::Three);
	TestTrue(TEXT("3 is P3"), Third.Action == EAction::Position && Third.Position == 3);
	const FCommand Tenth = CommandFor(EKeys::Zero);
	TestTrue(TEXT("0 is P10"), Tenth.Action == EAction::Position && Tenth.Position == 10);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexSpectateHotlapChoicesTest, "ApexSim.Spectate.HotlapChoices", ApexTestFlags)

bool FApexSpectateHotlapChoicesTest::RunTest(const FString& Parameters)
{
	using namespace ApexSpectate;
	TestEqual(TEXT("on from the last wraps to the first"), StepIndex(4, 3, 1), 0);
	TestEqual(TEXT("back from the first wraps to the last"), StepIndex(4, 0, -1), 3);
	TestEqual(TEXT("an empty list"), StepIndex(0, 2, 1), 0);

	TestEqual(TEXT("sunny, then cloudy"), (int32)StepWeather(EApexWeather::Sunny, 1), (int32)EApexWeather::Cloudy);
	TestEqual(TEXT("heavy rain, then sunny again"), (int32)StepWeather(EApexWeather::HeavyRain, 1), (int32)EApexWeather::Sunny);
	TestEqual(TEXT("before sunny is heavy rain"), (int32)StepWeather(EApexWeather::Sunny, -1), (int32)EApexWeather::HeavyRain);

	TestEqual(TEXT("an hour on"), StepTimeOfDay(13 * 60, 1), 14 * 60);
	TestEqual(TEXT("an hour earlier keeps the quarter hour"), StepTimeOfDay(13 * 60 + 15, -1), 12 * 60 + 15);
	TestEqual(TEXT("past midnight"), StepTimeOfDay(23 * 60 + 30, 1), 30);
	TestEqual(TEXT("before midnight"), StepTimeOfDay(30, -1), 23 * 60 + 30);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
