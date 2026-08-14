#include "Input/ApexInputConfig.h"

#include "ApexSettingsSave.h"
#include "EnhancedActionKeyMapping.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"

namespace ApexInput
{
	const TArray<FSlotDef>& Slots()
	{
		// Slot 0 is the device column, slot 1 the keyboard one; steering needs two
		// more because a keyboard has no axis. The order here is the order the
		// controls screen draws.
		static const TArray<FSlotDef> Table = {
			{ Actions::Throttle,     0, TEXT("Throttle"),     EKeys::Gamepad_RightTriggerAxis, false, true  },
			{ Actions::Throttle,     1, TEXT("Throttle"),     EKeys::W,                        false, false },
			{ Actions::Brake,        0, TEXT("Brake"),        EKeys::Gamepad_LeftTriggerAxis,  false, true  },
			{ Actions::Brake,        1, TEXT("Brake"),        EKeys::S,                        false, false },
			{ Actions::Steer,        0, TEXT("Steer axis"),   EKeys::Gamepad_LeftX,            false, true  },
			{ Actions::Steer,        2, TEXT("Steer left"),   EKeys::A,                        true,  false },
			{ Actions::Steer,        3, TEXT("Steer right"),  EKeys::D,                        false, false },
			{ Actions::GearUp,       0, TEXT("Shift up"),     EKeys::Gamepad_RightShoulder,    false, true  },
			{ Actions::GearUp,       1, TEXT("Shift up"),     EKeys::E,                        false, false },
			{ Actions::GearDown,     0, TEXT("Shift down"),   EKeys::Gamepad_LeftShoulder,     false, true  },
			{ Actions::GearDown,     1, TEXT("Shift down"),   EKeys::Q,                        false, false },
			{ Actions::ToggleCamera, 0, TEXT("Camera"),       EKeys::Gamepad_FaceButton_Top,   false, true  },
			{ Actions::ToggleCamera, 1, TEXT("Camera"),       EKeys::C,                        false, false },
			{ Actions::PauseMenu,    0, TEXT("Pause menu"),   EKeys::Gamepad_Special_Right,    false, true  },
			{ Actions::PauseMenu,    1, TEXT("Pause menu"),   EKeys::Escape,                   false, false },
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

	FString GetKeyDisplayName(const FKey& Key)
	{
		if (!Key.IsValid())
		{
			return TEXT("UNBOUND");
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
			{ EKeys::SpaceBar.GetFName(),                 TEXT("SPACE")  },
			{ EKeys::Escape.GetFName(),                   TEXT("ESC")    },
		};

		if (const FString* Short = ShortNames.Find(Key.GetFName()))
		{
			return *Short;
		}
		return Key.GetDisplayName(/*bLongDisplayName*/ false).ToString().ToUpper();
	}
}

namespace
{
	UInputAction* MakeAction(UObject* Outer, const TCHAR* Name, EInputActionValueType ValueType)
	{
		UInputAction* Action = NewObject<UInputAction>(Outer, Name);
		Action->ValueType = ValueType;
		return Action;
	}

	/** Map a key to an action, optionally inverted. */
	void MapKey(UInputMappingContext* Context, UInputAction* Action, const FKey& Key,
		bool bNegate = false)
	{
		if (!Context || !Action || !Key.IsValid())
		{
			return;
		}

		FEnhancedActionKeyMapping& Mapping = Context->MapKey(Action, Key);
		if (bNegate)
		{
			Mapping.Modifiers.Add(NewObject<UInputModifierNegate>(Context));
		}
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
	// PauseMenu is handled by the root widget, not by Enhanced Input.
	return nullptr;
}

void UApexInputConfig::ApplyBindings(const TArray<FApexKeyBinding>& Bindings)
{
	if (!DriveContext)
	{
		return;
	}

	DriveContext->UnmapAll();

	for (const ApexInput::FSlotDef& Def : ApexInput::Slots())
	{
		UInputAction* Action = FindAction(Def.ActionId);
		if (!Action)
		{
			continue;
		}

		const FApexKeyBinding* Override = Bindings.FindByPredicate([&Def](const FApexKeyBinding& Binding)
		{
			return Binding.ActionId == Def.ActionId && Binding.Slot == Def.Slot;
		});

		// An override that stores an invalid key means "unbound on purpose", which
		// is different from having no override at all.
		const FKey Key = Override ? Override->Key : Def.DefaultKey;
		const bool bNegate = Override ? Override->bNegate : Def.bNegate;
		MapKey(DriveContext, Action, Key, bNegate);
	}

	// The arrow keys are not in the slot table: they are a fixed convenience
	// alias for WASD, not something the controls screen offers to rebind.
	MapKey(DriveContext, Throttle, EKeys::Up);
	MapKey(DriveContext, Brake, EKeys::Down);
	MapKey(DriveContext, Steer, EKeys::Right);
	MapKey(DriveContext, Steer, EKeys::Left, /*bNegate*/ true);
}
