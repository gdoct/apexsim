#include "ApexDirectInputTypes.h"
#include "ApexSettingsSave.h"
#include "ApexTestCommon.h"
#include "EnhancedActionKeyMapping.h"
#include "Framework/Application/NavigationConfig.h"
#include "Input/ApexInputConfig.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	// Not `using namespace`: in a unity blob it would drag MaxAxes & co into
	// every later file's global scope, where they shadow Chaos's parameters.
	using ApexDirectInput::EControlKind;
	using ApexDirectInput::HatDirections;
	using ApexDirectInput::MakeKey;

	/** The steering axis of a device, as a wheel binding names it. */
	FKey WheelAxis(int32 Device, int32 Axis = 0)
	{
		return MakeKey({ Device, EControlKind::Axis, Axis });
	}

	FKey WheelButton(int32 Device, int32 Button)
	{
		return MakeKey({ Device, EControlKind::Button, Button });
	}

	/** A hat direction: 0 up, 1 right, 2 down, 3 left, as the device names them. */
	FKey WheelHat(int32 Device, int32 Hat, int32 Direction)
	{
		return MakeKey({ Device, EControlKind::Hat, Hat * HatDirections + Direction });
	}

	/** Which devices the rules should believe are plugged in. */
	TSet<int32> WheelTestAttached;

	bool WheelTestIsAttached(int32 DeviceSlot)
	{
		return WheelTestAttached.Contains(DeviceSlot);
	}

	FKey ResolvedKey(const TArray<FApexKeyBinding>& Bindings, FName ActionId, int32 Slot)
	{
		const FApexKeyBinding* Found = ApexInput::FindBinding(Bindings, ActionId, Slot, &WheelTestIsAttached);
		return Found ? Found->Key : FKey();
	}

	/** The mapping a key ended up with in the drive context, or null. */
	const FEnhancedActionKeyMapping* FindMapping(const UApexInputConfig& Config, const FKey& Key)
	{
		return Config.DriveContext->GetMappings().FindByPredicate(
			[&Key](const FEnhancedActionKeyMapping& Mapping) { return Mapping.Key == Key; });
	}

	template <typename TModifier>
	bool HasModifier(const FEnhancedActionKeyMapping& Mapping)
	{
		return Mapping.Modifiers.ContainsByPredicate(
			[](const UInputModifier* Modifier) { return Cast<TModifier>(Modifier) != nullptr; });
	}
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexWheelBindingsTest,
	"ApexSim.Input.Wheel.Bindings",
	ApexTestFlags)

bool FApexWheelBindingsTest::RunTest(const FString& Parameters)
{
	using namespace ApexInput;

	TArray<FApexKeyBinding> Bindings;
	WheelTestAttached = { 0 };

	// The keyboard column is one binding per slot, replaced in place.
	StoreBinding(Bindings, Actions::Throttle, Slot::Keyboard, EKeys::T, false, &WheelTestIsAttached);
	StoreBinding(Bindings, Actions::Throttle, Slot::Keyboard, EKeys::Y, false, &WheelTestIsAttached);
	TestEqual(TEXT("a keyboard rebind replaces rather than piles up"), Bindings.Num(), 1);
	TestTrue(TEXT("and holds the newest key"), ResolvedKey(Bindings, Actions::Throttle, Slot::Keyboard) == EKeys::Y);

	// The wheel column keeps one binding per device. Two wheelbases, each
	// mapped while it was the one plugged in.
	StoreBinding(Bindings, Actions::Throttle, Slot::Wheel, WheelAxis(0, 1), /*bInvert*/ true, &WheelTestIsAttached);
	WheelTestAttached = { 1 };
	StoreBinding(Bindings, Actions::Throttle, Slot::Wheel, WheelAxis(1, 2), /*bInvert*/ false, &WheelTestIsAttached);

	TestTrue(TEXT("the attached wheel's binding is the one that resolves"),
		ResolvedKey(Bindings, Actions::Throttle, Slot::Wheel) == WheelAxis(1, 2));

	// Nothing attached: the most recent binding is what the screen shows,
	// rather than an empty slot that suggests the mapping was lost.
	WheelTestAttached.Reset();
	TestTrue(TEXT("a remembered binding is shown with nothing plugged in"),
		ResolvedKey(Bindings, Actions::Throttle, Slot::Wheel) == WheelAxis(1, 2));

	WheelTestAttached = { 0 };
	TestTrue(TEXT("plug the other one in and its own binding comes back"),
		ResolvedKey(Bindings, Actions::Throttle, Slot::Wheel) == WheelAxis(0, 1));

	const FApexKeyBinding* First = ApexInput::FindBinding(Bindings, Actions::Throttle, Slot::Wheel, &WheelTestIsAttached);
	TestTrue(TEXT("with the way round its own pedal runs"), First && First->bInvert);

	// Rebinding the attached device leaves the other device's mapping alone.
	StoreBinding(Bindings, Actions::Throttle, Slot::Wheel, WheelAxis(0, 3), false, &WheelTestIsAttached);
	TestTrue(TEXT("the rebind took"), ResolvedKey(Bindings, Actions::Throttle, Slot::Wheel) == WheelAxis(0, 3));
	WheelTestAttached = { 1 };
	TestTrue(TEXT("and the other wheel still has its own"),
		ResolvedKey(Bindings, Actions::Throttle, Slot::Wheel) == WheelAxis(1, 2));

	// Unbinding is about the device in front of the player, not every device
	// they have ever owned.
	StoreBinding(Bindings, Actions::Throttle, Slot::Wheel, FKey(), false, &WheelTestIsAttached);
	TestFalse(TEXT("the attached wheel is unbound"), ResolvedKey(Bindings, Actions::Throttle, Slot::Wheel).IsValid());
	WheelTestAttached = { 0 };
	TestTrue(TEXT("the one that was not here kept its binding"),
		ResolvedKey(Bindings, Actions::Throttle, Slot::Wheel) == WheelAxis(0, 3));

	// An unbinding is the player's last word on the slot, so it is what shows
	// when the device it was made on is not here to speak for itself.
	WheelTestAttached.Reset();
	TestFalse(TEXT("an unbinding outlasts the device it was made on"),
		ResolvedKey(Bindings, Actions::Throttle, Slot::Wheel).IsValid());

	// Resetting one column leaves the other alone: a wheel user who resets the
	// pad page must not lose the mapping they spent ten minutes on.
	StoreBinding(Bindings, Actions::PauseMenu, Slot::Wheel, WheelButton(0, 7), false, &WheelTestIsAttached);
	ResetColumn(Bindings, EColumn::Keyboard);
	TestFalse(TEXT("the keyboard binding is gone"),
		ResolvedKey(Bindings, Actions::Throttle, Slot::Keyboard).IsValid());
	TestTrue(TEXT("the wheel's is not"), ResolvedKey(Bindings, Actions::PauseMenu, Slot::Wheel) == WheelButton(0, 7));

	ResetColumn(Bindings, EColumn::Wheel);
	TestEqual(TEXT("and resetting the wheel column empties it"), Bindings.Num(), 0);

	// Which axes are read from the middle and which from a stop: a pedal bound
	// to the throttle has to fold into 0..1, a steering axis must not.
	TestTrue(TEXT("steering is centred"), IsCentredAxisSlot(Actions::Steer, Slot::Wheel));
	TestTrue(TEXT("so is the pad's look axis"), IsCentredAxisSlot(Actions::Look, Slot::Gamepad));
	TestFalse(TEXT("a pedal is not"), IsCentredAxisSlot(Actions::Throttle, Slot::Wheel));
	TestFalse(TEXT("nor is half an axis"), IsCentredAxisSlot(Actions::Look, Slot::WheelLow));
	return true;
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexWheelMappingTest,
	"ApexSim.Input.Wheel.Mapping",
	ApexTestFlags)

bool FApexWheelMappingTest::RunTest(const FString& Parameters)
{
	using namespace ApexInput;

	// What the bindings actually become: the modifiers are where a wheel is
	// told apart from a thumbstick, and getting them wrong is a car that
	// drives itself (a pedal read from its centre) or a wheel with a
	// thumbstick's deadzone.
	UApexInputConfig* Config = UApexInputConfig::Create(GetTransientPackage());
	if (!Config || !Config->DriveContext)
	{
		AddError(TEXT("no input config"));
		return false;
	}

	TArray<FApexKeyBinding> Bindings;
	// A pedal that rests at the top of its travel, and a wheel's steering axis.
	Bindings.Emplace(Actions::Throttle, Slot::Wheel, WheelAxis(0, 1), false, /*bInvert*/ true);
	Bindings.Emplace(Actions::Steer, Slot::Wheel, WheelAxis(0, 0), false, false);
	Bindings.Emplace(Actions::GearUp, Slot::Wheel, WheelButton(0, 4), false, false);
	Config->ApplyBindings(Bindings);

	if (const FEnhancedActionKeyMapping* Pedal = FindMapping(*Config, WheelAxis(0, 1)))
	{
		TestTrue(TEXT("the pedal drives the throttle"), Pedal->Action == Config->Throttle);
		TestTrue(TEXT("and is folded into 0..1"), HasModifier<UApexInputModifierPedal>(*Pedal));
		TestTrue(TEXT("the right way round for a pedal that rests at the top"),
			HasModifier<UInputModifierNegate>(*Pedal));
		TestFalse(TEXT("with none of the pad's steering shaping"),
			HasModifier<UApexInputModifierPadSteering>(*Pedal));
	}
	else
	{
		AddError(TEXT("the pedal was not mapped"));
	}

	if (const FEnhancedActionKeyMapping* Steering = FindMapping(*Config, WheelAxis(0, 0)))
	{
		TestTrue(TEXT("the wheel steers"), Steering->Action == Config->Steer);
		TestFalse(TEXT("read from the centre, not folded"), HasModifier<UApexInputModifierPedal>(*Steering));
		TestFalse(TEXT("and with no deadzone or curve of the pad's"),
			HasModifier<UApexInputModifierPadSteering>(*Steering));
		TestFalse(TEXT("nor inverted"), HasModifier<UInputModifierNegate>(*Steering));
		TestTrue(TEXT("but geared to the steering lock"), HasModifier<UApexInputModifierWheelSteering>(*Steering));
	}
	else
	{
		AddError(TEXT("the wheel's steering was not mapped"));
	}

	if (const FEnhancedActionKeyMapping* Paddle = FindMapping(*Config, WheelButton(0, 4)))
	{
		TestTrue(TEXT("a paddle shifts up"), Paddle->Action == Config->GearUp);
		TestFalse(TEXT("and is not treated as a pedal"), HasModifier<UApexInputModifierPedal>(*Paddle));
	}
	else
	{
		AddError(TEXT("the paddle was not mapped"));
	}

	// The pad keeps its shaping, and the keyboard keeps working.
	if (const FEnhancedActionKeyMapping* Stick = FindMapping(*Config, EKeys::Gamepad_LeftX))
	{
		TestTrue(TEXT("the stick is shaped"), HasModifier<UApexInputModifierPadSteering>(*Stick));
		TestFalse(TEXT("and not geared like a wheel"), HasModifier<UApexInputModifierWheelSteering>(*Stick));
	}
	else
	{
		AddError(TEXT("the pad's steering was not mapped"));
	}
	TestTrue(TEXT("W still means throttle"), FindMapping(*Config, EKeys::W) != nullptr);

	// The maths the modifier does, which is the whole of "resting is no throttle".
	const UApexInputModifierPedal* PedalModifier = NewObject<UApexInputModifierPedal>();
	auto Fold = [PedalModifier](float Raw)
	{
		return PedalModifier->ModifyRaw(nullptr, FInputActionValue(Raw), 0.0f).Get<float>();
	};
	TestEqual(TEXT("a pedal at rest is nothing"), Fold(-1.0f), 0.0f);
	TestEqual(TEXT("half pressed"), Fold(0.0f), 0.5f);
	TestEqual(TEXT("all the way down"), Fold(1.0f), 1.0f);

	// The steering lock: full lock at that many degrees of rim, whatever the
	// base's rotation, and never a lock that cannot be reached.
	TestEqual(TEXT("a 900 degree base at a 450 degree lock doubles the steering"), WheelSteeringScale(900.0f, 450.0f), 2.0f);
	TestEqual(TEXT("a lock as long as the rotation is the rim as it is"), WheelSteeringScale(900.0f, 900.0f), 1.0f);
	TestEqual(TEXT("a lock longer than the rotation is the rotation"), WheelSteeringScale(540.0f, 900.0f), 1.0f);
	TestEqual(TEXT("1080 at 480"), WheelSteeringScale(1080.0f, 480.0f), 2.25f);
	TestTrue(TEXT("absurd values stay finite"), FMath::IsFinite(WheelSteeringScale(0.0f, 0.0f))
		&& WheelSteeringScale(0.0f, 0.0f) >= 1.0f);
	return true;
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexWheelMenuNavigationTest,
	"ApexSim.Input.Wheel.MenuNavigation",
	ApexTestFlags)

bool FApexWheelMenuNavigationTest::RunTest(const FString& Parameters)
{
	using namespace ApexInput;

	// A wheel's thumb stick drives the menus by becoming Slate navigation
	// rules, beside the D-pad's: the config is what every widget asks,
	// Slate's own sliders and dropdowns included.
	FNavigationConfig Config;
	const int32 EngineRules = Config.KeyEventRules.Num();
	const int32 EngineActions = Config.KeyActionRules.Num();

	auto Direction = [&Config](const FKey& Key)
	{
		return Config.GetNavigationDirectionFromKey(FKeyEvent(Key, FModifierKeysState(), 0, false, 0, 0));
	};
	auto Action = [&Config](const FKey& Key)
	{
		return Config.GetNavigationActionFromKey(FKeyEvent(Key, FModifierKeysState(), 0, false, 0, 0));
	};

	TArray<FApexKeyBinding> Bindings;
	Bindings.Emplace(Actions::MenuUp, Slot::Wheel, WheelHat(0, 0, 0));
	Bindings.Emplace(Actions::MenuRight, Slot::Wheel, WheelHat(0, 0, 1));
	Bindings.Emplace(Actions::MenuDown, Slot::Wheel, WheelHat(0, 0, 2));
	Bindings.Emplace(Actions::MenuLeft, Slot::Wheel, WheelHat(0, 0, 3));
	Bindings.Emplace(Actions::MenuAccept, Slot::Wheel, WheelButton(0, 1));
	Bindings.Emplace(Actions::MenuBack, Slot::Wheel, WheelButton(0, 2));
	// The same hat also looks about while driving: not a menu rule.
	Bindings.Emplace(Actions::LookBack, Slot::Wheel, WheelButton(0, 9));
	// A second wheelbase remembered but not plugged in still navigates.
	Bindings.Emplace(Actions::MenuUp, Slot::Wheel, WheelHat(1, 0, 0));
	// Two menu slots on one key: the first one wins, the second is skipped.
	Bindings.Emplace(Actions::MenuDown, Slot::Wheel, WheelButton(0, 1));
	// A pad key in a menu slot (not a wheel slot) is never a rule.
	Bindings.Emplace(Actions::MenuUp, Slot::Gamepad, EKeys::Gamepad_FaceButton_Top);

	TArray<FKey> Added;
	ApplyMenuNavigation(Config, Bindings, Added);

	TestTrue(TEXT("hat up is up"), Direction(WheelHat(0, 0, 0)) == EUINavigation::Up);
	TestTrue(TEXT("hat right is right"), Direction(WheelHat(0, 0, 1)) == EUINavigation::Right);
	TestTrue(TEXT("hat down is down"), Direction(WheelHat(0, 0, 2)) == EUINavigation::Down);
	TestTrue(TEXT("hat left is left"), Direction(WheelHat(0, 0, 3)) == EUINavigation::Left);
	TestTrue(TEXT("the other base's hat too"), Direction(WheelHat(1, 0, 0)) == EUINavigation::Up);
	TestTrue(TEXT("OK accepts"), Action(WheelButton(0, 1)) == EUINavigationAction::Accept);
	TestTrue(TEXT("Back backs out"), Action(WheelButton(0, 2)) == EUINavigationAction::Back);
	TestTrue(TEXT("a key on two menu slots keeps the first"), Direction(WheelButton(0, 1)) == EUINavigation::Invalid);
	TestTrue(TEXT("a driving binding is not navigation"), Direction(WheelButton(0, 9)) == EUINavigation::Invalid);
	TestFalse(TEXT("a pad key in a menu slot is ignored"), Added.Contains(EKeys::Gamepad_FaceButton_Top));
	TestTrue(TEXT("the D-pad is untouched"), Direction(EKeys::Gamepad_DPad_Up) == EUINavigation::Up);
	TestEqual(TEXT("seven keys added"), Added.Num(), 7);

	// Rebinding: the old set comes out, the new one goes in.
	TArray<FApexKeyBinding> Rebound;
	Rebound.Emplace(Actions::MenuUp, Slot::Wheel, WheelButton(0, 5));
	ApplyMenuNavigation(Config, Rebound, Added);
	TestTrue(TEXT("the old hat no longer navigates"), Direction(WheelHat(0, 0, 0)) == EUINavigation::Invalid);
	TestTrue(TEXT("nor does the old OK accept"), Action(WheelButton(0, 1)) == EUINavigationAction::Invalid);
	TestTrue(TEXT("the new key does"), Direction(WheelButton(0, 5)) == EUINavigation::Up);

	// A player cannot take over one of Slate's keys, even by binding it.
	TArray<FApexKeyBinding> Hostile;
	Hostile.Emplace(Actions::MenuBack, Slot::Wheel, EKeys::Enter);
	ApplyMenuNavigation(Config, Hostile, Added);
	TestTrue(TEXT("Enter still accepts"), Action(EKeys::Enter) == EUINavigationAction::Accept);

	// And clearing leaves the config as the engine made it.
	ApplyMenuNavigation(Config, {}, Added);
	TestEqual(TEXT("no rules left behind"), Config.KeyEventRules.Num(), EngineRules);
	TestEqual(TEXT("no actions left behind"), Config.KeyActionRules.Num(), EngineActions);
	TestTrue(TEXT("Enter survived the clear"), Action(EKeys::Enter) == EUINavigationAction::Accept);
	TestTrue(TEXT("menu slots are menu actions"), IsMenuAction(Actions::MenuAccept) && !IsMenuAction(Actions::Look));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
