#include "Input/ApexInputConfig.h"

#include "ApexDirectInputTypes.h"
#include "ApexSettingsSave.h"
#include "ApexSettingsSubsystem.h"
#include "ApexSimInputModule.h"
#include "EnhancedActionKeyMapping.h"
#include "EnhancedPlayerInput.h"
#include "Engine/GameInstance.h"
#include "GameFramework/PlayerController.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"

namespace ApexInput
{
	const TArray<FSlotDef>& Slots()
	{
		// Slot 0 is the gamepad column, slot 1 the keyboard one; steering and
		// looking need two more because a keyboard has no axis. Slots 4 and up
		// are the wheel column, which starts empty: what a wheel's axes are
		// called is a different answer on every set, so they are captured
		// rather than guessed — except steering, which falls back to the first
		// attached base's own steering axis (GetWheelDefaultKey).
		//
		// The order here is the order the controls screen draws.
		static const TArray<FSlotDef> Table = {
			{ Actions::Throttle,     0, TEXT("Throttle"),     EKeys::Gamepad_RightTriggerAxis, false, EColumn::Gamepad  },
			{ Actions::Throttle,     1, TEXT("Throttle"),     EKeys::W,                        false, EColumn::Keyboard },
			{ Actions::Brake,        0, TEXT("Brake"),        EKeys::Gamepad_LeftTriggerAxis,  false, EColumn::Gamepad  },
			{ Actions::Brake,        1, TEXT("Brake"),        EKeys::S,                        false, EColumn::Keyboard },
			{ Actions::Steer,        0, TEXT("Steer axis"),   EKeys::Gamepad_LeftX,            false, EColumn::Gamepad  },
			{ Actions::Steer,        2, TEXT("Steer left"),   EKeys::A,                        true,  EColumn::Keyboard },
			{ Actions::Steer,        3, TEXT("Steer right"),  EKeys::D,                        false, EColumn::Keyboard },
			{ Actions::GearUp,       0, TEXT("Shift up"),     EKeys::Gamepad_RightShoulder,    false, EColumn::Gamepad  },
			{ Actions::GearUp,       1, TEXT("Shift up"),     EKeys::E,                        false, EColumn::Keyboard },
			{ Actions::GearDown,     0, TEXT("Shift down"),   EKeys::Gamepad_LeftShoulder,     false, EColumn::Gamepad  },
			{ Actions::GearDown,     1, TEXT("Shift down"),   EKeys::Q,                        false, EColumn::Keyboard },
			{ Actions::ToggleCamera, 0, TEXT("Camera"),       EKeys::Gamepad_FaceButton_Top,   false, EColumn::Gamepad  },
			{ Actions::ToggleCamera, 1, TEXT("Camera"),       EKeys::C,                        false, EColumn::Keyboard },
			{ Actions::Look,         0, TEXT("Look axis"),    EKeys::Gamepad_RightX,           false, EColumn::Gamepad  },
			{ Actions::Look,         2, TEXT("Look left"),    EKeys::Comma,                    true,  EColumn::Keyboard },
			{ Actions::Look,         3, TEXT("Look right"),   EKeys::Period,                   false, EColumn::Keyboard },
			{ Actions::LookBack,     0, TEXT("Look behind"),  EKeys::Gamepad_RightThumbstick,  false, EColumn::Gamepad  },
			{ Actions::LookBack,     1, TEXT("Look behind"),  EKeys::B,                        false, EColumn::Keyboard },
			{ Actions::PauseMenu,    0, TEXT("Pause menu"),   EKeys::Gamepad_Special_Right,    false, EColumn::Gamepad  },
			{ Actions::PauseMenu,    1, TEXT("Pause menu"),   EKeys::Escape,                   false, EColumn::Keyboard },

			{ Actions::Steer,        Slot::Wheel,      TEXT("Steering"),    FKey(), false, EColumn::Wheel },
			{ Actions::Throttle,     Slot::Wheel,      TEXT("Throttle"),    FKey(), false, EColumn::Wheel },
			{ Actions::Brake,        Slot::Wheel,      TEXT("Brake"),       FKey(), false, EColumn::Wheel },
			{ Actions::GearUp,       Slot::Wheel,      TEXT("Shift up"),    FKey(), false, EColumn::Wheel },
			{ Actions::GearDown,     Slot::Wheel,      TEXT("Shift down"),  FKey(), false, EColumn::Wheel },
			{ Actions::ToggleCamera, Slot::Wheel,      TEXT("Camera"),      FKey(), false, EColumn::Wheel },
			{ Actions::Look,         Slot::WheelLow,   TEXT("Look left"),   FKey(), true,  EColumn::Wheel },
			{ Actions::Look,         Slot::WheelHigh,  TEXT("Look right"),  FKey(), false, EColumn::Wheel },
			{ Actions::LookBack,     Slot::Wheel,      TEXT("Look behind"), FKey(), false, EColumn::Wheel },
			{ Actions::PauseMenu,    Slot::Wheel,      TEXT("Pause menu"),  FKey(), false, EColumn::Wheel },
		};
		return Table;
	}

	const FSlotDef* FindSlot(FName ActionId, int32 Slot)
	{
		return Slots().FindByPredicate([ActionId, Slot](const FSlotDef& Def)
		{
			return Def.ActionId == ActionId && Def.Slot == Slot;
		});
	}

	bool IsCentredAxisSlot(FName ActionId, int32 Slot)
	{
		// The halves ("look left") are one-way even on an axis action, and a
		// pedal is one-way whatever it is bound to.
		const bool bFullAxisSlot = Slot == Slot::Gamepad || Slot == Slot::Wheel;
		return bFullAxisSlot && (ActionId == Actions::Steer || ActionId == Actions::Look);
	}

	FString GetKeyDisplayName(const FKey& Key)
	{
		if (!Key.IsValid())
		{
			return TEXT("UNBOUND");
		}

		if (ApexDirectInput::IsDirectInputKey(Key))
		{
			// The device says what it is called: "WHEEL X", "PEDALS RZ".
			if (const FApexSimInputModule* Input = FApexSimInputModule::Get())
			{
				return Input->GetShortName(Key);
			}
		}

		// The engine's long names ("Gamepad Right Shoulder") do not fit a binding
		// chip; the short forms are what the mockups and every other sim use.
		static const TMap<FName, FString> ShortNames = {
			{ EKeys::Gamepad_LeftShoulder.GetFName(),     TEXT("LB")     },
			{ EKeys::Gamepad_RightShoulder.GetFName(),    TEXT("RB")     },
			{ EKeys::Gamepad_LeftTriggerAxis.GetFName(),  TEXT("LT")     },
			{ EKeys::Gamepad_RightTriggerAxis.GetFName(), TEXT("RT")     },
			{ EKeys::Gamepad_LeftX.GetFName(),            TEXT("LS X")   },
			{ EKeys::Gamepad_LeftY.GetFName(),            TEXT("LS Y")   },
			{ EKeys::Gamepad_RightX.GetFName(),           TEXT("RS X")   },
			{ EKeys::Gamepad_RightY.GetFName(),           TEXT("RS Y")   },
			{ EKeys::Gamepad_FaceButton_Bottom.GetFName(),TEXT("A")      },
			{ EKeys::Gamepad_FaceButton_Right.GetFName(), TEXT("B")      },
			{ EKeys::Gamepad_FaceButton_Left.GetFName(),  TEXT("X")      },
			{ EKeys::Gamepad_FaceButton_Top.GetFName(),   TEXT("Y")      },
			{ EKeys::Gamepad_Special_Left.GetFName(),     TEXT("BACK")   },
			{ EKeys::Gamepad_Special_Right.GetFName(),    TEXT("START")  },
			{ EKeys::Gamepad_RightThumbstick.GetFName(),  TEXT("RS")     },
			{ EKeys::Comma.GetFName(),                    TEXT(",")      },
			{ EKeys::Period.GetFName(),                   TEXT(".")      },
			{ EKeys::SpaceBar.GetFName(),                 TEXT("SPACE")  },
			{ EKeys::Escape.GetFName(),                   TEXT("ESC")    },
		};

		if (const FString* Short = ShortNames.Find(Key.GetFName()))
		{
			return *Short;
		}
		return Key.GetDisplayName(/*bLongDisplayName*/ false).ToString().ToUpper();
	}

	FString GetKeyLongName(const FKey& Key)
	{
		if (ApexDirectInput::IsDirectInputKey(Key))
		{
			if (const FApexSimInputModule* Input = FApexSimInputModule::Get())
			{
				return Input->GetLongName(Key);
			}
		}
		return GetKeyDisplayName(Key);
	}

	// --- Bindings ---------------------------------------------------------------

	const FApexKeyBinding* FindBinding(
		const TArray<FApexKeyBinding>& Bindings, FName ActionId, int32 Slot, FDeviceAttached IsAttached)
	{
		const FApexKeyBinding* Unbound = nullptr;
		const FApexKeyBinding* Remembered = nullptr;

		for (const FApexKeyBinding& Binding : Bindings)
		{
			if (Binding.ActionId != ActionId || Binding.Slot != Slot)
			{
				continue;
			}
			if (!IsWheelSlot(Slot))
			{
				// One binding per slot everywhere but the wheel column.
				return &Binding;
			}

			const ApexDirectInput::FControl Control = ApexDirectInput::ParseKey(Binding.Key);
			if (Control.IsValid() && IsAttached(Control.Slot))
			{
				return &Binding;
			}
			if (Binding.Key.IsValid())
			{
				// The last one wins: bindings are appended as they are made.
				Remembered = &Binding;
			}
			else
			{
				Unbound = &Binding;
			}
		}

		return Unbound ? Unbound : Remembered;
	}

	void StoreBinding(
		TArray<FApexKeyBinding>& Bindings, FName ActionId, int32 Slot, const FKey& Key, bool bInvert,
		FDeviceAttached IsAttached)
	{
		const FSlotDef* Def = FindSlot(ActionId, Slot);
		if (!Def)
		{
			return;
		}
		// bNegate comes from the slot, not the key: which half of an action a
		// slot drives is a property of the control, not of what is bound to it.
		const bool bNegate = Def->bNegate;

		if (!IsWheelSlot(Slot))
		{
			if (FApexKeyBinding* Existing = Bindings.FindByPredicate([ActionId, Slot](const FApexKeyBinding& Candidate)
				{
					return Candidate.ActionId == ActionId && Candidate.Slot == Slot;
				}))
			{
				Existing->Key = Key;
				Existing->bNegate = bNegate;
				Existing->bInvert = bInvert;
			}
			else
			{
				Bindings.Emplace(ActionId, Slot, Key, bNegate, bInvert);
			}
			return;
		}

		const int32 Device = ApexDirectInput::ParseKey(Key).Slot;
		Bindings.RemoveAll([ActionId, Slot, Device, &Key, &IsAttached](const FApexKeyBinding& Candidate)
		{
			if (Candidate.ActionId != ActionId || Candidate.Slot != Slot)
			{
				return false;
			}
			const int32 Other = ApexDirectInput::ParseKey(Candidate.Key).Slot;
			// What is being replaced: this device's own binding, any previous
			// unbinding, and — when this is itself an unbinding — every device
			// the player can see, which is what they meant by it.
			return Other == Device || Other == INDEX_NONE || (!Key.IsValid() && IsAttached(Other));
		});

		Bindings.Emplace(ActionId, Slot, Key, bNegate, Key.IsValid() && bInvert);
	}

	void ResetColumn(TArray<FApexKeyBinding>& Bindings, EColumn Column)
	{
		const bool bWheel = Column == EColumn::Wheel;
		Bindings.RemoveAll([bWheel](const FApexKeyBinding& Binding)
		{
			return IsWheelSlot(Binding.Slot) == bWheel;
		});
	}

	FKey GetWheelDefaultKey(FName ActionId, int32 Slot)
	{
		// Only steering: which axis is the throttle is a different answer on
		// every pedal set, and a wrong guess would be a car that drives itself.
		if (ActionId != Actions::Steer || Slot != Slot::Wheel)
		{
			return FKey();
		}

		const FApexSimInputModule* Input = FApexSimInputModule::Get();
		if (!Input)
		{
			return FKey();
		}

		const ApexDirectInput::FDeviceInfo* Base = nullptr;
		for (const ApexDirectInput::FDeviceInfo& Device : Input->GetDevices())
		{
			if (Device.Kind != ApexDirectInput::EDeviceKind::Wheel || !Device.HasAxis(0))
			{
				continue;
			}
			// A base that can play forces is the wheel, if two claim to be.
			if (!Base || (Device.bCanPlayForces && !Base->bCanPlayForces))
			{
				Base = &Device;
			}
		}

		return Base
			? ApexDirectInput::MakeKey({ Base->Slot, ApexDirectInput::EControlKind::Axis, 0 })
			: FKey();
	}

	int32 FindForceFeedbackDevice(const TArray<FApexKeyBinding>& Bindings)
	{
		const FApexSimInputModule* Input = FApexSimInputModule::Get();
		if (!Input)
		{
			return INDEX_NONE;
		}

		auto IsAttached = [Input](int32 DeviceSlot) { return Input->IsAttached(DeviceSlot); };
		const FApexKeyBinding* Steering = FindBinding(Bindings, Actions::Steer, Slot::Wheel, IsAttached);
		const FKey Key = Steering ? Steering->Key : GetWheelDefaultKey(Actions::Steer, Slot::Wheel);

		const ApexDirectInput::FControl Control = ApexDirectInput::ParseKey(Key);
		const ApexDirectInput::FDeviceInfo* Device = Control.IsValid() ? Input->FindDevice(Control.Slot) : nullptr;
		// What it *can* do, not what it is doing: the device layer only takes a
		// wheel exclusively once this has named it, so asking the other way
		// round would mean no wheel ever played anything.
		return Device && Device->bCanPlayForces ? Device->Slot : INDEX_NONE;
	}
}

// --- Modifiers --------------------------------------------------------------

FInputActionValue UApexInputModifierPedal::ModifyRaw_Implementation(
	const UEnhancedPlayerInput* PlayerInput, FInputActionValue CurrentValue, float DeltaTime)
{
	return FInputActionValue(FMath::Clamp(0.5f * (CurrentValue.Get<float>() + 1.0f), 0.0f, 1.0f));
}

FInputActionValue UApexInputModifierPadSteering::ModifyRaw_Implementation(
	const UEnhancedPlayerInput* PlayerInput, FInputActionValue CurrentValue, float DeltaTime)
{
	const APlayerController* PlayerController = PlayerInput ? Cast<APlayerController>(PlayerInput->GetOuter()) : nullptr;
	const UGameInstance* GameInstance = PlayerController ? PlayerController->GetGameInstance() : nullptr;
	const UApexSettingsSubsystem* Settings = GameInstance ? GameInstance->GetSubsystem<UApexSettingsSubsystem>() : nullptr;

	const float Raw = FMath::Clamp(CurrentValue.Get<float>(), -1.0f, 1.0f);
	return FInputActionValue(Settings ? Settings->ShapeSteering(Raw) : Raw);
}

// --- The config -------------------------------------------------------------

namespace
{
	UInputAction* MakeAction(UObject* Outer, const TCHAR* Name, EInputActionValueType ValueType)
	{
		UInputAction* Action = NewObject<UInputAction>(Outer, Name);
		Action->ValueType = ValueType;
		return Action;
	}
}	 // namespace

UApexInputConfig* UApexInputConfig::Create(UObject* Outer)
{
	UApexInputConfig* Config = NewObject<UApexInputConfig>(Outer);

	Config->Steer = MakeAction(Config, TEXT("IA_Steer"), EInputActionValueType::Axis1D);
	Config->Throttle = MakeAction(Config, TEXT("IA_Throttle"), EInputActionValueType::Axis1D);
	Config->Brake = MakeAction(Config, TEXT("IA_Brake"), EInputActionValueType::Axis1D);
	Config->GearUp = MakeAction(Config, TEXT("IA_GearUp"), EInputActionValueType::Boolean);
	Config->GearDown = MakeAction(Config, TEXT("IA_GearDown"), EInputActionValueType::Boolean);
	Config->ToggleCamera =
		MakeAction(Config, TEXT("IA_ToggleCamera"), EInputActionValueType::Boolean);
	Config->Look = MakeAction(Config, TEXT("IA_Look"), EInputActionValueType::Axis1D);
	Config->LookBack = MakeAction(Config, TEXT("IA_LookBack"), EInputActionValueType::Boolean);

	Config->DriveContext = NewObject<UInputMappingContext>(Config, TEXT("IMC_Drive"));
	Config->ApplyBindings({});

	return Config;
}

UInputAction* UApexInputConfig::FindAction(FName ActionId) const
{
	if (ActionId == ApexInput::Actions::Throttle)     { return Throttle; }
	if (ActionId == ApexInput::Actions::Brake)        { return Brake; }
	if (ActionId == ApexInput::Actions::Steer)        { return Steer; }
	if (ActionId == ApexInput::Actions::GearUp)       { return GearUp; }
	if (ActionId == ApexInput::Actions::GearDown)     { return GearDown; }
	if (ActionId == ApexInput::Actions::ToggleCamera) { return ToggleCamera; }
	if (ActionId == ApexInput::Actions::Look)         { return Look; }
	if (ActionId == ApexInput::Actions::LookBack)     { return LookBack; }
	// PauseMenu is handled by the root widget, not by Enhanced Input.
	return nullptr;
}

void UApexInputConfig::MapSlot(const ApexInput::FSlotDef& Def, UInputAction* Action, const FKey& Key, bool bNegate)
{
	if (!DriveContext || !Action || !Key.IsValid())
	{
		return;
	}

	FEnhancedActionKeyMapping& Mapping = DriveContext->MapKey(Action, Key);
	if (bNegate)
	{
		Mapping.Modifiers.Add(NewObject<UInputModifierNegate>(DriveContext));
	}

	const bool bDirectInputAxis = ApexDirectInput::IsAxisKey(Key);
	if (bDirectInputAxis && !ApexInput::IsCentredAxisSlot(Def.ActionId, Def.Slot))
	{
		// After the negate, so an inverted pedal is folded the right way round.
		Mapping.Modifiers.Add(NewObject<UApexInputModifierPedal>(DriveContext));
	}
	if (Def.ActionId == ApexInput::Actions::Steer && !bDirectInputAxis)
	{
		Mapping.Modifiers.Add(NewObject<UApexInputModifierPadSteering>(DriveContext));
	}
}

void UApexInputConfig::ApplyBindings(const TArray<FApexKeyBinding>& Bindings)
{
	if (!DriveContext)
	{
		return;
	}

	DriveContext->UnmapAll();

	const FApexSimInputModule* Input = FApexSimInputModule::Get();
	auto IsAttached = [Input](int32 DeviceSlot) { return Input && Input->IsAttached(DeviceSlot); };

	for (const ApexInput::FSlotDef& Def : ApexInput::Slots())
	{
		UInputAction* Action = FindAction(Def.ActionId);
		if (!Action)
		{
			continue;
		}

		if (Def.Column == ApexInput::EColumn::Wheel)
		{
			// Every remembered device is mapped, not only the attached one: a
			// wheelbase that comes back later drives without a rebind, and a
			// binding on a device that is not here cannot fire anyway.
			bool bMappedAny = false;
			for (const FApexKeyBinding& Binding : Bindings)
			{
				if (Binding.ActionId == Def.ActionId && Binding.Slot == Def.Slot && Binding.Key.IsValid())
				{
					MapSlot(Def, Action, Binding.Key, Def.bNegate != Binding.bInvert);
					bMappedAny = true;
				}
			}
			const bool bUnboundOnPurpose = Bindings.ContainsByPredicate([&Def](const FApexKeyBinding& Binding)
			{
				return Binding.ActionId == Def.ActionId && Binding.Slot == Def.Slot && !Binding.Key.IsValid();
			});
			if (!bMappedAny && !bUnboundOnPurpose)
			{
				MapSlot(Def, Action, ApexInput::GetWheelDefaultKey(Def.ActionId, Def.Slot), Def.bNegate);
			}
			continue;
		}

		const FApexKeyBinding* Override = ApexInput::FindBinding(Bindings, Def.ActionId, Def.Slot, IsAttached);

		// An override that stores an invalid key means "unbound on purpose", which
		// is different from having no override at all.
		const FKey Key = Override ? Override->Key : Def.DefaultKey;
		const bool bNegate = Override ? (Def.bNegate != Override->bInvert) : Def.bNegate;
		MapSlot(Def, Action, Key, bNegate);
	}

	// The arrow keys are not in the slot table: they are a fixed convenience
	// alias for WASD, not something the controls screen offers to rebind.
	static const ApexInput::FSlotDef ArrowThrottle{ ApexInput::Actions::Throttle, ApexInput::Slot::Keyboard, TEXT("Throttle"), FKey(), false, ApexInput::EColumn::Keyboard };
	static const ApexInput::FSlotDef ArrowBrake{ ApexInput::Actions::Brake, ApexInput::Slot::Keyboard, TEXT("Brake"), FKey(), false, ApexInput::EColumn::Keyboard };
	static const ApexInput::FSlotDef ArrowSteer{ ApexInput::Actions::Steer, ApexInput::Slot::KeyboardHigh, TEXT("Steer"), FKey(), false, ApexInput::EColumn::Keyboard };
	MapSlot(ArrowThrottle, Throttle, EKeys::Up, false);
	MapSlot(ArrowBrake, Brake, EKeys::Down, false);
	MapSlot(ArrowSteer, Steer, EKeys::Right, false);
	MapSlot(ArrowSteer, Steer, EKeys::Left, /*bNegate*/ true);
}
