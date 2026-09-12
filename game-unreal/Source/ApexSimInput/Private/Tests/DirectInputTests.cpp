#include "ApexDirectInputTypes.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

using namespace ApexDirectInput;

namespace
{
	constexpr EAutomationTestFlags ApexInputTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter;

	FGuid MakeGuid(uint32 Seed)
	{
		return FGuid(Seed, 0x1111, 0x2222, 0x3333);
	}

	uint32 SlotBit(int32 Slot)
	{
		return 1u << Slot;
	}
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexDirectInputKeysTest,
	"ApexSim.Input.DirectInput.Keys",
	ApexInputTestFlags)

bool FApexDirectInputKeysTest::RunTest(const FString& Parameters)
{
	const FControl Axis{ 0, EControlKind::Axis, 5 };
	const FControl Button{ 3, EControlKind::Button, 41 };
	const FControl Hat{ 7, EControlKind::Hat, 2 };

	for (const FControl& Control : { Axis, Button, Hat })
	{
		const FKey Key = MakeKey(Control);
		TestTrue(FString::Printf(TEXT("%s is a key"), *Key.ToString()), Key.IsValid());
		TestTrue(TEXT("and it is one of ours"), IsDirectInputKey(Key));
		TestTrue(TEXT("and it names the control it was made from"), ParseKey(Key) == Control);
	}

	TestEqual(TEXT("the names are stable"), MakeKey(Axis).ToString(), TEXT("DInput1_RZ"));
	TestEqual(TEXT("buttons count from one"), MakeKey(Button).ToString(), TEXT("DInput4_Button42"));
	TestEqual(TEXT("hats name their direction"), MakeKey(Hat).ToString(), TEXT("DInput8_Hat1Down"));

	TestTrue(TEXT("an axis key reads as an axis"), IsAxisKey(MakeKey(Axis)));
	TestFalse(TEXT("a button does not"), IsAxisKey(MakeKey(Button)));

	// A keyboard key is nobody's device control, and out-of-range asks for
	// nothing rather than a key that was never registered.
	TestFalse(TEXT("an ordinary key is not one of ours"), IsDirectInputKey(EKeys::A));
	TestFalse(TEXT("nor is an invalid one"), IsDirectInputKey(FKey()));
	TestFalse(TEXT("a device past the last slot has no key"), MakeKey({ MaxDevices, EControlKind::Axis, 0 }).IsValid());
	TestFalse(TEXT("nor a button past the last"), MakeKey({ 0, EControlKind::Button, MaxButtons }).IsValid());

	TestEqual(TEXT("a chip's control name"), ControlName(Button), TEXT("B42"));
	return true;
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexDirectInputSlotsTest,
	"ApexSim.Input.DirectInput.Slots",
	ApexInputTestFlags)

bool FApexDirectInputSlotsTest::RunTest(const FString& Parameters)
{
	const FGuid WheelInstance = MakeGuid(1);
	const FGuid WheelProduct = MakeGuid(100);
	const FGuid PedalsInstance = MakeGuid(2);
	const FGuid PedalsProduct = MakeGuid(200);

	// First run: two devices, in the order they happen to enumerate.
	TArray<FSlotRecord> Records;
	const int32 WheelSlot = AssignSlot(Records, WheelInstance, WheelProduct, 0);
	const int32 PedalsSlot = AssignSlot(Records, PedalsInstance, PedalsProduct, SlotBit(WheelSlot));
	TestEqual(TEXT("the first device takes the first slot"), WheelSlot, 0);
	TestEqual(TEXT("the second takes the next"), PedalsSlot, 1);

	// Second run, enumerated the other way round: each keeps its own slot, or
	// every binding would move to the other device.
	TestEqual(TEXT("the pedals keep their slot whatever the order"),
		AssignSlot(Records, PedalsInstance, PedalsProduct, 0), 1);
	TestEqual(TEXT("and so does the wheel"),
		AssignSlot(Records, WheelInstance, WheelProduct, SlotBit(1)), 0);

	// The same wheel on another USB port: some devices mint a new instance GUID
	// for that, and only the model is left to recognise it by.
	const FGuid MovedInstance = MakeGuid(3);
	TestEqual(TEXT("the same model takes over the slot it used to hold"),
		AssignSlot(Records, MovedInstance, WheelProduct, 0), 0);
	TestEqual(TEXT("and the record now names the device that is here"),
		Records[Records.IndexOfByPredicate([](const FSlotRecord& Record) { return Record.Slot == 0; })].Instance,
		MovedInstance);

	// Two identical wheels at once: the second cannot take the first's slot.
	const int32 TwinSlot = AssignSlot(Records, MakeGuid(4), WheelProduct, SlotBit(0) | SlotBit(1));
	TestEqual(TEXT("an identical twin gets a slot of its own"), TwinSlot, 2);

	// Every slot spoken for: the device that has not been seen for longest
	// gives up its slot rather than the new one going unusable.
	TArray<FSlotRecord> Full;
	for (int32 Index = 0; Index < MaxDevices; ++Index)
	{
		AssignSlot(Full, MakeGuid(10 + Index), MakeGuid(50 + Index), 0);
	}
	const FGuid Newcomer = MakeGuid(99);
	TestEqual(TEXT("the ninth device evicts the oldest"), AssignSlot(Full, Newcomer, MakeGuid(999), 0), 0);
	TestEqual(TEXT("still one record per slot"), Full.Num(), MaxDevices);

	// Nothing to give: a ninth device attached at the same time is ignored
	// rather than stealing a slot from something in use.
	uint32 AllBusy = 0;
	for (int32 Index = 0; Index < MaxDevices; ++Index)
	{
		AllBusy |= SlotBit(Index);
	}
	TestEqual(TEXT("no slot left"), AssignSlot(Full, MakeGuid(1000), MakeGuid(1001), AllBusy), INDEX_NONE);
	return true;
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexDirectInputRecordsFileTest,
	"ApexSim.Input.DirectInput.RecordsFile",
	ApexInputTestFlags)

bool FApexDirectInputRecordsFileTest::RunTest(const FString& Parameters)
{
	TArray<FSlotRecord> Records;
	FSlotRecord& Wheel = Records.AddDefaulted_GetRef();
	Wheel.Slot = 0;
	Wheel.Instance = MakeGuid(1);
	Wheel.Product = MakeGuid(2);
	Wheel.Name = TEXT("FANATEC CSL DD");
	Wheel.Kind = EDeviceKind::Wheel;

	FSlotRecord& Pedals = Records.AddDefaulted_GetRef();
	Pedals.Slot = 3;
	Pedals.Instance = MakeGuid(3);
	Pedals.Product = MakeGuid(4);
	Pedals.Name = TEXT("Heusinkveld Sprint");
	Pedals.Kind = EDeviceKind::Pedals;

	const TArray<FSlotRecord> Read = RecordsFromJson(RecordsToJson(Records));
	if (!TestEqual(TEXT("both devices survive the round trip"), Read.Num(), 2))
	{
		return false;
	}
	TestEqual(TEXT("slot"), Read[1].Slot, 3);
	TestEqual(TEXT("instance"), Read[1].Instance, Pedals.Instance);
	TestEqual(TEXT("product"), Read[1].Product, Pedals.Product);
	TestEqual(TEXT("name"), Read[1].Name, Pedals.Name);
	TestTrue(TEXT("kind"), Read[1].Kind == EDeviceKind::Pedals);

	// A file that has been edited or half-written must not take bindings with
	// it: bad entries are dropped, the rest still load.
	const TArray<FSlotRecord> Salvaged = RecordsFromJson(TEXT(R"({"devices":[
		{"slot":0,"instance":"{00000001-1111-2222-3333-333333333333}","name":"Good"},
		{"slot":99,"instance":"{00000002-1111-2222-3333-333333333333}"},
		{"slot":1,"instance":"not a guid"},
		{"slot":0,"instance":"{00000003-1111-2222-3333-333333333333}"}]})"));
	TestEqual(TEXT("only the usable entry survives"), Salvaged.Num(), 1);
	TestEqual(TEXT("and it is the first one for its slot"), Salvaged[0].Name, TEXT("Good"));
	TestEqual(TEXT("nothing at all is not a failure"), RecordsFromJson(TEXT("nonsense")).Num(), 0);
	return true;
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexDirectInputReadingsTest,
	"ApexSim.Input.DirectInput.Readings",
	ApexInputTestFlags)

bool FApexDirectInputReadingsTest::RunTest(const FString& Parameters)
{
	// The whole travel, whatever range the driver reports it in.
	TestEqual(TEXT("the bottom of the range"), NormaliseAxis(0, 0, 65535), -1.0f);
	TestEqual(TEXT("the top"), NormaliseAxis(65535, 0, 65535), 1.0f);
	TestTrue(TEXT("the middle"), FMath::IsNearlyEqual(NormaliseAxis(32767, 0, 65535), 0.0f, 0.001f));
	TestEqual(TEXT("a signed range works the same"), NormaliseAxis(-32768, -32768, 32767), -1.0f);
	TestTrue(TEXT("a reading past the range is clamped"), NormaliseAxis(70000, 0, 65535) == 1.0f);
	TestEqual(TEXT("a range that makes no sense reads as centred"), NormaliseAxis(10, 100, 100), 0.0f);

	// Hats: eight ways, so a diagonal holds both of its directions.
	TestEqual(TEXT("centred"), static_cast<int32>(HatBitsFromPov(0xFFFF)), 0);
	TestEqual(TEXT("centred, reported as -1"), static_cast<int32>(HatBitsFromPov(0xFFFFFFFF)), 0);
	TestEqual(TEXT("up"), static_cast<int32>(HatBitsFromPov(0)), 1);
	TestEqual(TEXT("up and right"), static_cast<int32>(HatBitsFromPov(4500)), 1 | 2);
	TestEqual(TEXT("right"), static_cast<int32>(HatBitsFromPov(9000)), 2);
	TestEqual(TEXT("down"), static_cast<int32>(HatBitsFromPov(18000)), 4);
	TestEqual(TEXT("left"), static_cast<int32>(HatBitsFromPov(27000)), 8);
	TestEqual(TEXT("up and left"), static_cast<int32>(HatBitsFromPov(31500)), 1 | 8);
	return true;
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexDirectInputClassifyTest,
	"ApexSim.Input.DirectInput.Classify",
	ApexInputTestFlags)

bool FApexDirectInputClassifyTest::RunTest(const FString& Parameters)
{
	FDeviceTraits Driving;
	Driving.bDrivingType = true;

	FDeviceTraits ForceFeedbackJoystick;
	ForceFeedbackJoystick.bJoystickType = true;
	ForceFeedbackJoystick.bForceFeedback = true;

	// The name wins over the type: pedals, shifters and handbrakes all report
	// themselves as driving devices.
	TestTrue(TEXT("pedals"), ClassifyDevice(TEXT("Fanatec ClubSport Pedals V3"), Driving) == EDeviceKind::Pedals);
	TestTrue(TEXT("a shifter"), ClassifyDevice(TEXT("Thrustmaster TH8A Shifter"), Driving) == EDeviceKind::Shifter);
	TestTrue(TEXT("a handbrake"), ClassifyDevice(TEXT("Heusinkveld Sim Handbrake"), Driving) == EDeviceKind::Handbrake);
	TestTrue(TEXT("pedals without the word"), ClassifyDevice(TEXT("Heusinkveld Sprint"), Driving) == EDeviceKind::Pedals);
	TestTrue(TEXT("a base"), ClassifyDevice(TEXT("FANATEC CSL DD"), Driving) == EDeviceKind::Wheel);

	// A direct drive base that calls itself a joystick is still a wheel: it can
	// play forces, and nothing else in a sim rig can.
	TestTrue(TEXT("a base that reports as a joystick"),
		ClassifyDevice(TEXT("Simucube 2 Pro"), ForceFeedbackJoystick) == EDeviceKind::Wheel);

	FDeviceTraits Plain;
	Plain.bJoystickType = true;
	TestTrue(TEXT("a button box"), ClassifyDevice(TEXT("Ascher Racing Button Box"), Plain) == EDeviceKind::ButtonBox);
	TestTrue(TEXT("anything else"), ClassifyDevice(TEXT("Some Stick"), Plain) == EDeviceKind::Joystick);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
