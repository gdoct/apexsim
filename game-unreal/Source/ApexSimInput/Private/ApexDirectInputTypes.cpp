#include "ApexDirectInputTypes.h"

#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace ApexDirectInput
{
	namespace
	{
		const FName KeyCategory = TEXT("ApexDirectInput");

		const TCHAR* const AxisNames[MaxAxes] = {
			TEXT("X"), TEXT("Y"), TEXT("Z"), TEXT("RX"), TEXT("RY"), TEXT("RZ"), TEXT("S1"), TEXT("S2") };
		const TCHAR* const HatDirectionNames[HatDirections] = { TEXT("Up"), TEXT("Right"), TEXT("Down"), TEXT("Left") };
		const TCHAR* const HatArrows[HatDirections] = { TEXT("↑"), TEXT("→"), TEXT("↓"), TEXT("←") };

		/** Every key name, built once, and the way back from a name to its control. */
		struct FKeyTable
		{
			FName Axes[MaxDevices][MaxAxes];
			FName Buttons[MaxDevices][MaxButtons];
			FName Hats[MaxDevices][MaxHats * HatDirections];
			TMap<FName, FControl> ByName;
		};

		const FKeyTable& KeyTable()
		{
			static const FKeyTable Table = []
			{
				FKeyTable Built;
				for (int32 Slot = 0; Slot < MaxDevices; ++Slot)
				{
					for (int32 Axis = 0; Axis < MaxAxes; ++Axis)
					{
						Built.Axes[Slot][Axis] = FName(*FString::Printf(TEXT("DInput%d_%s"), Slot + 1, AxisNames[Axis]));
						Built.ByName.Add(Built.Axes[Slot][Axis], FControl{ Slot, EControlKind::Axis, Axis });
					}
					for (int32 Button = 0; Button < MaxButtons; ++Button)
					{
						Built.Buttons[Slot][Button] = FName(*FString::Printf(TEXT("DInput%d_Button%d"), Slot + 1, Button + 1));
						Built.ByName.Add(Built.Buttons[Slot][Button], FControl{ Slot, EControlKind::Button, Button });
					}
					for (int32 Hat = 0; Hat < MaxHats * HatDirections; ++Hat)
					{
						Built.Hats[Slot][Hat] = FName(*FString::Printf(TEXT("DInput%d_Hat%d%s"),
							Slot + 1, Hat / HatDirections + 1, HatDirectionNames[Hat % HatDirections]));
						Built.ByName.Add(Built.Hats[Slot][Hat], FControl{ Slot, EControlKind::Hat, Hat });
					}
				}
				return Built;
			}();
			return Table;
		}

		int32 ControlCount(EControlKind Kind)
		{
			switch (Kind)
			{
			case EControlKind::Axis:   return MaxAxes;
			case EControlKind::Button: return MaxButtons;
			case EControlKind::Hat:    return MaxHats * HatDirections;
			}
			return 0;
		}
	}

	FKey MakeKey(const FControl& Control)
	{
		if (Control.Slot < 0 || Control.Slot >= MaxDevices || Control.Index < 0 || Control.Index >= ControlCount(Control.Kind))
		{
			return FKey();
		}

		const FKeyTable& Table = KeyTable();
		switch (Control.Kind)
		{
		case EControlKind::Axis:   return FKey(Table.Axes[Control.Slot][Control.Index]);
		case EControlKind::Button: return FKey(Table.Buttons[Control.Slot][Control.Index]);
		case EControlKind::Hat:    return FKey(Table.Hats[Control.Slot][Control.Index]);
		}
		return FKey();
	}

	FControl ParseKey(const FKey& Key)
	{
		const FControl* Found = KeyTable().ByName.Find(Key.GetFName());
		return Found ? *Found : FControl();
	}

	const TCHAR* AxisName(int32 Axis)
	{
		return Axis >= 0 && Axis < MaxAxes ? AxisNames[Axis] : TEXT("?");
	}

	FString ControlName(const FControl& Control)
	{
		switch (Control.Kind)
		{
		case EControlKind::Axis:
			return AxisName(Control.Index);
		case EControlKind::Button:
			return FString::Printf(TEXT("B%d"), Control.Index + 1);
		case EControlKind::Hat:
			return FString::Printf(TEXT("H%d%s"), Control.Index / HatDirections + 1, HatArrows[Control.Index % HatDirections]);
		}
		return FString();
	}

	void RegisterKeys()
	{
		// The editor's live coding can restart the module; EKeys ensures on a
		// duplicate, and the keys it already holds are the same ones.
		static bool bRegistered = false;
		if (bRegistered)
		{
			return;
		}
		bRegistered = true;

		EKeys::AddMenuCategoryDisplayInfo(KeyCategory, NSLOCTEXT("ApexDirectInput", "KeyCategory", "DirectInput"),
			TEXT("GraphEditor.PadEvent_16x"));

		const FKeyTable& Table = KeyTable();
		for (int32 Slot = 0; Slot < MaxDevices; ++Slot)
		{
			// Axes are Axis1D so Enhanced Input reads their value rather than a
			// press. GamepadKey on everything: to the engine these are
			// controller inputs, which keeps them out of mouse-and-keyboard
			// logic such as Slate's modifier-key handling.
			for (int32 Axis = 0; Axis < MaxAxes; ++Axis)
			{
				EKeys::AddKey(FKeyDetails(FKey(Table.Axes[Slot][Axis]),
					FText::FromString(FString::Printf(TEXT("Device %d %s axis"), Slot + 1, AxisNames[Axis])),
					FKeyDetails::GamepadKey | FKeyDetails::Axis1D, KeyCategory));
			}
			for (int32 Button = 0; Button < MaxButtons; ++Button)
			{
				EKeys::AddKey(FKeyDetails(FKey(Table.Buttons[Slot][Button]),
					FText::FromString(FString::Printf(TEXT("Device %d button %d"), Slot + 1, Button + 1)),
					FKeyDetails::GamepadKey, KeyCategory));
			}
			for (int32 Hat = 0; Hat < MaxHats * HatDirections; ++Hat)
			{
				EKeys::AddKey(FKeyDetails(FKey(Table.Hats[Slot][Hat]),
					FText::FromString(FString::Printf(TEXT("Device %d hat %d %s"),
						Slot + 1, Hat / HatDirections + 1, HatDirectionNames[Hat % HatDirections])),
					FKeyDetails::GamepadKey, KeyCategory));
			}
		}
	}

	// --- Devices ----------------------------------------------------------------

	EDeviceKind ClassifyDevice(const FString& ProductName, const FDeviceTraits& Traits)
	{
		const FString Name = ProductName.ToLower();
		auto Has = [&Name](const TCHAR* Word) { return Name.Contains(Word); };

		if (Has(TEXT("pedal")) || Has(TEXT("lcm")))
		{
			return EDeviceKind::Pedals;
		}
		if (Has(TEXT("shifter")) || Has(TEXT("sequential")) || Has(TEXT("h-pattern")) || Has(TEXT("th8")))
		{
			return EDeviceKind::Shifter;
		}
		if (Has(TEXT("handbrake")) || Has(TEXT("hand brake")) || Has(TEXT("e-brake")))
		{
			return EDeviceKind::Handbrake;
		}
		if (Has(TEXT("button box")) || Has(TEXT("buttonbox")) || Has(TEXT("button-box")))
		{
			return EDeviceKind::ButtonBox;
		}
		// Heusinkveld names its shifter and handbrake, but not its pedal sets.
		if (Has(TEXT("heusinkveld")))
		{
			return EDeviceKind::Pedals;
		}
		if (Traits.bForceFeedback || Traits.bDrivingType || Has(TEXT("wheel")) || Has(TEXT("racing")))
		{
			return EDeviceKind::Wheel;
		}
		if (Traits.bGamepadType || Has(TEXT("gamepad")))
		{
			return EDeviceKind::Gamepad;
		}
		return Traits.bJoystickType ? EDeviceKind::Joystick : EDeviceKind::Other;
	}

	const TCHAR* KindTag(EDeviceKind Kind)
	{
		switch (Kind)
		{
		case EDeviceKind::Wheel:     return TEXT("WHEEL");
		case EDeviceKind::Pedals:    return TEXT("PEDALS");
		case EDeviceKind::Shifter:   return TEXT("SHIFTER");
		case EDeviceKind::Handbrake: return TEXT("HBRAKE");
		case EDeviceKind::ButtonBox: return TEXT("BOX");
		case EDeviceKind::Joystick:  return TEXT("STICK");
		case EDeviceKind::Gamepad:   return TEXT("PAD");
		case EDeviceKind::Other:     break;
		}
		return TEXT("DEV");
	}

	const TCHAR* KindName(EDeviceKind Kind)
	{
		switch (Kind)
		{
		case EDeviceKind::Wheel:     return TEXT("Wheel");
		case EDeviceKind::Pedals:    return TEXT("Pedals");
		case EDeviceKind::Shifter:   return TEXT("Shifter");
		case EDeviceKind::Handbrake: return TEXT("Handbrake");
		case EDeviceKind::ButtonBox: return TEXT("Button box");
		case EDeviceKind::Joystick:  return TEXT("Joystick");
		case EDeviceKind::Gamepad:   return TEXT("Gamepad");
		case EDeviceKind::Other:     break;
		}
		return TEXT("Controller");
	}

	int32 AssignSlot(TArray<FSlotRecord>& Records, const FGuid& Instance, const FGuid& Product, uint32 BusySlots)
	{
		auto IsFree = [BusySlots](int32 Slot)
		{
			return Slot >= 0 && Slot < MaxDevices && (BusySlots & (1u << Slot)) == 0;
		};

		// Most recently attached last, so the last rule evicts from the front.
		auto Promote = [&Records](int32 Index)
		{
			const FSlotRecord Record = Records[Index];
			Records.RemoveAt(Index);
			Records.Add(Record);
			return Record.Slot;
		};

		for (int32 Index = 0; Index < Records.Num(); ++Index)
		{
			if (Records[Index].Instance == Instance && IsFree(Records[Index].Slot))
			{
				return Promote(Index);
			}
		}

		if (Product.IsValid())
		{
			for (int32 Index = 0; Index < Records.Num(); ++Index)
			{
				if (Records[Index].Product == Product && Records[Index].Instance != Instance && IsFree(Records[Index].Slot))
				{
					Records[Index].Instance = Instance;
					return Promote(Index);
				}
			}
		}

		for (int32 Slot = 0; Slot < MaxDevices; ++Slot)
		{
			const bool bEverHeld = Records.ContainsByPredicate([Slot](const FSlotRecord& Record) { return Record.Slot == Slot; });
			if (IsFree(Slot) && !bEverHeld)
			{
				FSlotRecord& Record = Records.AddDefaulted_GetRef();
				Record.Slot = Slot;
				Record.Instance = Instance;
				Record.Product = Product;
				return Slot;
			}
		}

		for (int32 Index = 0; Index < Records.Num(); ++Index)
		{
			if (IsFree(Records[Index].Slot))
			{
				FSlotRecord& Record = Records[Index];
				Record.Instance = Instance;
				Record.Product = Product;
				Record.Name.Reset();
				Record.Kind = EDeviceKind::Other;
				return Promote(Index);
			}
		}

		return INDEX_NONE;
	}

	FString RecordsToJson(const TArray<FSlotRecord>& Records)
	{
		TArray<TSharedPtr<FJsonValue>> Devices;
		for (const FSlotRecord& Record : Records)
		{
			TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetNumberField(TEXT("slot"), Record.Slot);
			Entry->SetStringField(TEXT("instance"), Record.Instance.ToString(EGuidFormats::DigitsWithHyphensInBraces));
			Entry->SetStringField(TEXT("product"), Record.Product.ToString(EGuidFormats::DigitsWithHyphensInBraces));
			Entry->SetStringField(TEXT("name"), Record.Name);
			Entry->SetNumberField(TEXT("kind"), static_cast<int32>(Record.Kind));
			Devices.Add(MakeShared<FJsonValueObject>(Entry));
		}

		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetNumberField(TEXT("version"), 1);
		Root->SetArrayField(TEXT("devices"), Devices);

		FString Out;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Root, Writer);
		return Out;
	}

	TArray<FSlotRecord> RecordsFromJson(const FString& Json)
	{
		TArray<FSlotRecord> Records;

		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		const TArray<TSharedPtr<FJsonValue>>* Devices = nullptr;
		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid() || !Root->TryGetArrayField(TEXT("devices"), Devices))
		{
			return Records;
		}

		for (const TSharedPtr<FJsonValue>& Value : *Devices)
		{
			const TSharedPtr<FJsonObject> Entry = Value.IsValid() ? Value->AsObject() : nullptr;
			if (!Entry.IsValid())
			{
				continue;
			}

			FSlotRecord Record;
			int32 Kind = 0;
			FString Instance;
			FString Product;
			if (!Entry->TryGetNumberField(TEXT("slot"), Record.Slot)
				|| Record.Slot < 0 || Record.Slot >= MaxDevices
				|| !Entry->TryGetStringField(TEXT("instance"), Instance)
				|| !FGuid::Parse(Instance, Record.Instance))
			{
				continue;
			}
			// A slot held twice would hand one device's bindings to another.
			if (Records.ContainsByPredicate([&Record](const FSlotRecord& Other) { return Other.Slot == Record.Slot; }))
			{
				continue;
			}
			if (Entry->TryGetStringField(TEXT("product"), Product))
			{
				FGuid::Parse(Product, Record.Product);
			}
			Entry->TryGetStringField(TEXT("name"), Record.Name);
			if (Entry->TryGetNumberField(TEXT("kind"), Kind) && Kind >= 0 && Kind <= static_cast<int32>(EDeviceKind::Other))
			{
				Record.Kind = static_cast<EDeviceKind>(Kind);
			}
			Records.Add(Record);
		}
		return Records;
	}

	// --- Readings ---------------------------------------------------------------

	float NormaliseAxis(int32 Raw, int32 Min, int32 Max)
	{
		if (Max <= Min)
		{
			return 0.0f;
		}
		const double Alpha = (static_cast<double>(Raw) - Min) / (static_cast<double>(Max) - Min);
		return FMath::Clamp(static_cast<float>(Alpha * 2.0 - 1.0), -1.0f, 1.0f);
	}

	uint8 HatBitsFromPov(uint32 Pov)
	{
		// Centred is 0xFFFF in the low word; some drivers report the whole
		// DWORD as -1, which has the same low word.
		if ((Pov & 0xFFFF) == 0xFFFF)
		{
			return 0;
		}

		// Eight ways: each direction owns its own 45 degrees and half of each
		// neighbouring diagonal, so a diagonal holds two directions at once.
		const uint32 Angle = Pov % 36000;
		uint8 Bits = 0;
		if (Angle >= 29250 || Angle <= 6750)     { Bits |= 1; }
		if (Angle >= 2250 && Angle <= 15750)     { Bits |= 2; }
		if (Angle >= 11250 && Angle <= 24750)    { Bits |= 4; }
		if (Angle >= 20250 && Angle <= 33750)    { Bits |= 8; }
		return Bits;
	}
}
