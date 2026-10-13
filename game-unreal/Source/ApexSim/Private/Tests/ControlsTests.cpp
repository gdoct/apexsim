#include "ApexTestCommon.h"
#include "Input/ApexInputConfig.h"

#if WITH_DEV_AUTOMATION_TESTS

// -----------------------------------------------------------------------------
// The driving controls added with the handbrake, the H-pattern shifter and the
// recovery keys: each has its slots and its Enhanced Input action, the defaults
// do not clash with a driving key already in use, and a shifter's gates map
// onto absolute gears.
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexControlsSlotsTest, "ApexSim.Input.Controls.Slots", ApexTestFlags)

bool FApexControlsSlotsTest::RunTest(const FString& Parameters)
{
	using namespace ApexInput;

	TestEqual(TEXT("reverse is -1"), HGearOfAction(Actions::HGearReverse), -1);
	TestEqual(TEXT("first is 1"), HGearOfAction(Actions::HGear1), 1);
	TestEqual(TEXT("eighth is 8"), HGearOfAction(Actions::HGear8), 8);
	TestEqual(TEXT("any other action is 0"), HGearOfAction(Actions::GearUp), 0);

	// Every H-pattern gate, the handbrake and the recovery keys have a wheel slot.
	for (const FName& Id : {Actions::Handbrake, Actions::RecoverTrack, Actions::RecoverPits, Actions::HGearReverse,
			 Actions::HGear1, Actions::HGear2, Actions::HGear3, Actions::HGear4, Actions::HGear5, Actions::HGear6,
			 Actions::HGear7, Actions::HGear8})
	{
		TestTrue(*FString::Printf(TEXT("%s has a wheel slot"), *Id.ToString()), FindSlot(Id, Slot::Wheel) != nullptr);
		TestEqual(*FString::Printf(TEXT("%s is a driving control"), *Id.ToString()),
			(int32)ContextOf(Id), (int32)EContext::Driving);
	}
	// A pad and a keyboard handbrake, and the recovery keys on the keyboard.
	TestTrue(TEXT("pad handbrake"), FindSlot(Actions::Handbrake, 0) && FindSlot(Actions::Handbrake, 0)->DefaultKey == EKeys::Gamepad_FaceButton_Right);
	TestTrue(TEXT("keyboard handbrake"), FindSlot(Actions::Handbrake, 1) && FindSlot(Actions::Handbrake, 1)->DefaultKey == EKeys::LeftControl);
	TestTrue(TEXT("R is back to track"), FindSlot(Actions::RecoverTrack, 1) && FindSlot(Actions::RecoverTrack, 1)->DefaultKey == EKeys::R);
	TestTrue(TEXT("back to pits starts unbound"), FindSlot(Actions::RecoverPits, 1) && !FindSlot(Actions::RecoverPits, 1)->DefaultKey.IsValid());
	TestFalse(TEXT("the H-pattern has no default key"), FindSlot(Actions::HGear3, Slot::Wheel)->DefaultKey.IsValid());
	// The clutch pedal: a wheel slot of its own, unbound, and no pad or keyboard one.
	TestTrue(TEXT("clutch has a wheel slot"), FindSlot(Actions::Clutch, Slot::Wheel) != nullptr);
	TestTrue(TEXT("the clutch starts unbound"), FindSlot(Actions::Clutch, Slot::Wheel) && !FindSlot(Actions::Clutch, Slot::Wheel)->DefaultKey.IsValid());
	TestTrue(TEXT("no pad clutch"), FindSlot(Actions::Clutch, 0) == nullptr);

	// No two driving actions share a default key in a column.
	for (EColumn Column : {EColumn::Gamepad, EColumn::Keyboard})
	{
		TMap<FKey, FName> Seen;
		for (const FSlotDef& Def : Slots())
		{
			if (!Def.DefaultKey.IsValid() || ContextOf(Def.ActionId) != EContext::Driving || Def.Column != Column)
			{
				continue;
			}
			if (const FName* Other = Seen.Find(Def.DefaultKey))
			{
				AddError(FString::Printf(TEXT("%s and %s both default to %s"), *Other->ToString(), *Def.ActionId.ToString(), *Def.DefaultKey.ToString()));
			}
			Seen.Add(Def.DefaultKey, Def.ActionId);
		}
	}

	// The config makes an action for each, and finds them by id.
	UApexInputConfig* Config = UApexInputConfig::Create(GetTransientPackage());
	if (TestNotNull(TEXT("a config"), Config))
	{
		TestEqual(TEXT("nine gates: reverse and eight gears"), Config->HGears.Num(), 9);
		TestNotNull(TEXT("handbrake action"), Config->FindAction(Actions::Handbrake));
		TestNotNull(TEXT("clutch action"), Config->FindAction(Actions::Clutch));
		TestNotNull(TEXT("back to track action"), Config->FindAction(Actions::RecoverTrack));
		TestNotNull(TEXT("back to pits action"), Config->FindAction(Actions::RecoverPits));
		TestTrue(TEXT("reverse is the first gate"), Config->FindAction(Actions::HGearReverse) == Config->HGears[0].Get());
		TestTrue(TEXT("sixth gear is the sixth after it"), Config->FindAction(Actions::HGear6) == Config->HGears[6].Get());
		TestNull(TEXT("an unknown id is none"), Config->FindAction(FName(TEXT("HGear9"))));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
