#include "Input/ApexInputConfig.h"

#include "Algo/Find.h"
#include "ApexDirectInputTypes.h"
#include "ApexSettingsSave.h"
#include "ApexSettingsSubsystem.h"
#include "ApexSimInputModule.h"
#include "EnhancedActionKeyMapping.h"
#include "EnhancedPlayerInput.h"
#include "Engine/GameInstance.h"
#include "Framework/Application/NavigationConfig.h"
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
			{ Actions::Drs,          0, TEXT("DRS"),          EKeys::Gamepad_FaceButton_Left,  false, EColumn::Gamepad  },
			{ Actions::Drs,          1, TEXT("DRS"),          EKeys::LeftShift,                false, EColumn::Keyboard },
			{ Actions::Headlights,   0, TEXT("Headlights"),   EKeys::Gamepad_DPad_Up,          false, EColumn::Gamepad  },
			{ Actions::Headlights,   1, TEXT("Headlights"),   EKeys::L,                        false, EColumn::Keyboard },
			{ Actions::FlashLights,  0, TEXT("Flash lights"), EKeys::Gamepad_DPad_Down,        false, EColumn::Gamepad  },
			{ Actions::FlashLights,  1, TEXT("Flash lights"), EKeys::H,                        false, EColumn::Keyboard },
			{ Actions::ErsMode,      0, TEXT("ERS mode"),     EKeys::Gamepad_DPad_Right,       false, EColumn::Gamepad  },
			{ Actions::ErsMode,      1, TEXT("ERS mode"),     EKeys::M,                        false, EColumn::Keyboard },
			{ Actions::ErsBoost,     0, TEXT("Overtake"),     EKeys::Gamepad_FaceButton_Bottom, false, EColumn::Gamepad },
			{ Actions::ErsBoost,     1, TEXT("Overtake"),     EKeys::SpaceBar,                 false, EColumn::Keyboard },
			{ Actions::PauseMenu,    0, TEXT("Pause menu"),   EKeys::Gamepad_Special_Right,    false, EColumn::Gamepad  },
			{ Actions::PauseMenu,    1, TEXT("Pause menu"),   EKeys::Escape,                   false, EColumn::Keyboard },
			{ Actions::Handbrake,    0, TEXT("Handbrake"),    EKeys::Gamepad_FaceButton_Right, false, EColumn::Gamepad  },
			{ Actions::Handbrake,    1, TEXT("Handbrake"),    EKeys::LeftControl,              false, EColumn::Keyboard },
			{ Actions::RecoverTrack, 0, TEXT("Back to track"), FKey(),                         false, EColumn::Gamepad  },
			{ Actions::RecoverTrack, 1, TEXT("Back to track"), EKeys::R,                       false, EColumn::Keyboard },
			{ Actions::RecoverPits,  0, TEXT("Back to pits"), FKey(),                          false, EColumn::Gamepad  },
			{ Actions::RecoverPits,  1, TEXT("Back to pits"), FKey(),                          false, EColumn::Keyboard },
			{ Actions::BoxThisLap,   0, TEXT("Box this lap"), EKeys::Gamepad_DPad_Left,        false, EColumn::Gamepad  },
			{ Actions::BoxThisLap,   1, TEXT("Box this lap"), EKeys::P,                        false, EColumn::Keyboard },
			{ Actions::Chat,         0, TEXT("Chat"),         FKey(),                          false, EColumn::Gamepad  },
			{ Actions::Chat,         1, TEXT("Chat"),         EKeys::T,                        false, EColumn::Keyboard },
			{ Actions::Mark,         0, TEXT("Mark"),         FKey(),                          false, EColumn::Gamepad  },
			{ Actions::Mark,         1, TEXT("Mark"),         EKeys::K,                        false, EColumn::Keyboard },

			// The watch view: a race, a replay, a hotlap watched without a car.
			{ Actions::WatchPreviousCar, 0, TEXT("Car ahead"),    EKeys::Gamepad_LeftShoulder,      false, EColumn::Gamepad  },
			{ Actions::WatchPreviousCar, 1, TEXT("Car ahead"),    EKeys::Up,                        false, EColumn::Keyboard },
			{ Actions::WatchNextCar,     0, TEXT("Car behind"),   EKeys::Gamepad_RightShoulder,     false, EColumn::Gamepad  },
			{ Actions::WatchNextCar,     1, TEXT("Car behind"),   EKeys::Down,                      false, EColumn::Keyboard },
			{ Actions::WatchCamera,      0, TEXT("Camera"),       EKeys::Gamepad_FaceButton_Top,    false, EColumn::Gamepad  },
			{ Actions::WatchCamera,      1, TEXT("Camera"),       EKeys::C,                         false, EColumn::Keyboard },
			{ Actions::WatchAuto,        0, TEXT("TV director"),  EKeys::Gamepad_FaceButton_Left,   false, EColumn::Gamepad  },
			{ Actions::WatchAuto,        1, TEXT("TV director"),  EKeys::A,                         false, EColumn::Keyboard },
			{ Actions::WatchTower,       0, TEXT("Timing tower"), EKeys::Gamepad_Special_Left,      false, EColumn::Gamepad  },
			{ Actions::WatchTower,       1, TEXT("Timing tower"), EKeys::T,                         false, EColumn::Keyboard },
			{ Actions::WatchOverlay,     0, TEXT("Hide overlay"), EKeys::Gamepad_RightThumbstick,   false, EColumn::Gamepad  },
			{ Actions::WatchOverlay,     1, TEXT("Hide overlay"), EKeys::H,                         false, EColumn::Keyboard },
			{ Actions::WatchNextRace,    0, TEXT("Next race"),    EKeys::Gamepad_LeftThumbstick,    false, EColumn::Gamepad  },
			{ Actions::WatchNextRace,    1, TEXT("Next race"),    EKeys::N,                         false, EColumn::Keyboard },
			{ Actions::WatchLeave,       0, TEXT("Stop watching"), EKeys::Gamepad_FaceButton_Right, false, EColumn::Gamepad  },
			{ Actions::WatchLeave,       1, TEXT("Stop watching"), EKeys::BackSpace,                false, EColumn::Keyboard },
			{ Actions::WatchPlayPause,   0, TEXT("Pause replay"), EKeys::Gamepad_FaceButton_Bottom, false, EColumn::Gamepad  },
			{ Actions::WatchPlayPause,   1, TEXT("Pause replay"), EKeys::SpaceBar,                  false, EColumn::Keyboard },
			{ Actions::WatchSeekBack,    0, TEXT("Back 10 s"),    EKeys::Gamepad_LeftTrigger,       false, EColumn::Gamepad  },
			{ Actions::WatchSeekBack,    1, TEXT("Back 10 s"),    EKeys::Comma,                     false, EColumn::Keyboard },
			{ Actions::WatchSeekForward, 0, TEXT("On 10 s"),      EKeys::Gamepad_RightTrigger,      false, EColumn::Gamepad  },
			{ Actions::WatchSeekForward, 1, TEXT("On 10 s"),      EKeys::Period,                    false, EColumn::Keyboard },
			{ Actions::WatchSlower,      0, TEXT("Slower"),       EKeys::Gamepad_DPad_Left,         false, EColumn::Gamepad  },
			{ Actions::WatchSlower,      1, TEXT("Slower"),       EKeys::Hyphen,                    false, EColumn::Keyboard },
			{ Actions::WatchFaster,      0, TEXT("Faster"),       EKeys::Gamepad_DPad_Right,        false, EColumn::Gamepad  },
			{ Actions::WatchFaster,      1, TEXT("Faster"),       EKeys::Equals,                    false, EColumn::Keyboard },
			{ Actions::WatchWeather,     1, TEXT("Weather"),      EKeys::W,                         false, EColumn::Keyboard },
			{ Actions::WatchTimeEarlier, 1, TEXT("Hour earlier"), EKeys::LeftBracket,               false, EColumn::Keyboard },
			{ Actions::WatchTimeLater,   1, TEXT("Hour later"),   EKeys::RightBracket,              false, EColumn::Keyboard },
			{ Actions::WatchPreviousTrack, 1, TEXT("Previous circuit"), EKeys::PageUp,              false, EColumn::Keyboard },
			{ Actions::WatchNextTrack,   1, TEXT("Next circuit"), EKeys::PageDown,                  false, EColumn::Keyboard },
			{ Actions::WatchSetup,       0, TEXT("Setup"),        EKeys::Gamepad_DPad_Up,           false, EColumn::Gamepad  },
			{ Actions::WatchSetup,       1, TEXT("Setup"),        EKeys::S,                         false, EColumn::Keyboard },
			{ Actions::WatchReplayLap,   0, TEXT("Replay lap"),   EKeys::Gamepad_DPad_Down,         false, EColumn::Gamepad  },
			{ Actions::WatchReplayLap,   1, TEXT("Replay lap"),   EKeys::R,                         false, EColumn::Keyboard },
			{ Actions::WatchGhost,       1, TEXT("Ghost"),        EKeys::G,                         false, EColumn::Keyboard },
			{ Actions::WatchPhoto,       0, TEXT("Photo mode"),   FKey(),                           false, EColumn::Gamepad  },
			{ Actions::WatchPhoto,       1, TEXT("Photo mode"),   EKeys::P,                         false, EColumn::Keyboard },

			{ Actions::Steer,        Slot::Wheel,      TEXT("Steering"),    FKey(), false, EColumn::Wheel },
			{ Actions::Throttle,     Slot::Wheel,      TEXT("Throttle"),    FKey(), false, EColumn::Wheel },
			{ Actions::Brake,        Slot::Wheel,      TEXT("Brake"),       FKey(), false, EColumn::Wheel },
			{ Actions::Clutch,       Slot::Wheel,      TEXT("Clutch"),      FKey(), false, EColumn::Wheel },
			{ Actions::GearUp,       Slot::Wheel,      TEXT("Shift up"),    FKey(), false, EColumn::Wheel },
			{ Actions::GearDown,     Slot::Wheel,      TEXT("Shift down"),  FKey(), false, EColumn::Wheel },
			{ Actions::ToggleCamera, Slot::Wheel,      TEXT("Camera"),      FKey(), false, EColumn::Wheel },
			{ Actions::Look,         Slot::WheelLow,   TEXT("Look left"),   FKey(), true,  EColumn::Wheel },
			{ Actions::Look,         Slot::WheelHigh,  TEXT("Look right"),  FKey(), false, EColumn::Wheel },
			{ Actions::LookBack,     Slot::Wheel,      TEXT("Look behind"), FKey(), false, EColumn::Wheel },
			{ Actions::Drs,          Slot::Wheel,      TEXT("DRS"),         FKey(), false, EColumn::Wheel },
			{ Actions::Headlights,   Slot::Wheel,      TEXT("Headlights"),  FKey(), false, EColumn::Wheel },
			{ Actions::FlashLights,  Slot::Wheel,      TEXT("Flash lights"), FKey(), false, EColumn::Wheel },
			{ Actions::ErsMode,      Slot::Wheel,      TEXT("ERS mode"),    FKey(), false, EColumn::Wheel },
			{ Actions::ErsBoost,     Slot::Wheel,      TEXT("Overtake"),    FKey(), false, EColumn::Wheel },
			{ Actions::PauseMenu,    Slot::Wheel,      TEXT("Pause menu"),  FKey(), false, EColumn::Wheel },
			{ Actions::Handbrake,    Slot::Wheel,      TEXT("Handbrake"),   FKey(), false, EColumn::Wheel },
			{ Actions::RecoverTrack, Slot::Wheel,      TEXT("Back to track"), FKey(), false, EColumn::Wheel },
			{ Actions::RecoverPits,  Slot::Wheel,      TEXT("Back to pits"), FKey(), false, EColumn::Wheel },
			{ Actions::BoxThisLap,   Slot::Wheel,      TEXT("Box this lap"), FKey(), false, EColumn::Wheel },
			{ Actions::Chat,         Slot::Wheel,      TEXT("Chat"),        FKey(), false, EColumn::Wheel },
			{ Actions::Mark,         Slot::Wheel,      TEXT("Mark"),        FKey(), false, EColumn::Wheel },
			{ Actions::HGearReverse, Slot::Wheel, TEXT("Reverse"), FKey(), false, EColumn::Wheel },
			{ Actions::HGear1, Slot::Wheel, TEXT("Gear 1"), FKey(), false, EColumn::Wheel },
			{ Actions::HGear2, Slot::Wheel, TEXT("Gear 2"), FKey(), false, EColumn::Wheel },
			{ Actions::HGear3, Slot::Wheel, TEXT("Gear 3"), FKey(), false, EColumn::Wheel },
			{ Actions::HGear4, Slot::Wheel, TEXT("Gear 4"), FKey(), false, EColumn::Wheel },
			{ Actions::HGear5, Slot::Wheel, TEXT("Gear 5"), FKey(), false, EColumn::Wheel },
			{ Actions::HGear6, Slot::Wheel, TEXT("Gear 6"), FKey(), false, EColumn::Wheel },
			{ Actions::HGear7, Slot::Wheel, TEXT("Gear 7"), FKey(), false, EColumn::Wheel },
			{ Actions::HGear8, Slot::Wheel, TEXT("Gear 8"), FKey(), false, EColumn::Wheel },
			{ Actions::MenuUp,       Slot::Wheel,      TEXT("Menu up"),     FKey(), false, EColumn::Wheel },
			{ Actions::MenuDown,     Slot::Wheel,      TEXT("Menu down"),   FKey(), false, EColumn::Wheel },
			{ Actions::MenuLeft,     Slot::Wheel,      TEXT("Menu left"),   FKey(), false, EColumn::Wheel },
			{ Actions::MenuRight,    Slot::Wheel,      TEXT("Menu right"),  FKey(), false, EColumn::Wheel },
			{ Actions::MenuAccept,   Slot::Wheel,      TEXT("Menu OK"),     FKey(), false, EColumn::Wheel },
			{ Actions::MenuBack,     Slot::Wheel,      TEXT("Menu back"),   FKey(), false, EColumn::Wheel },
		};
		return Table;
	}

	bool IsWatchAction(FName ActionId)
	{
		static const FString Prefix(TEXT("Watch"));
		return ActionId.ToString().StartsWith(Prefix);
	}

	EContext ContextOf(FName ActionId)
	{
		return IsMenuAction(ActionId) ? EContext::Menu : IsWatchAction(ActionId) ? EContext::Watch : EContext::Driving;
	}

	int32 HGearOfAction(FName ActionId)
	{
		if (ActionId == Actions::HGearReverse) { return -1; }
		static const FName Forward[8] = { Actions::HGear1, Actions::HGear2, Actions::HGear3, Actions::HGear4,
			Actions::HGear5, Actions::HGear6, Actions::HGear7, Actions::HGear8 };
		for (int32 Index = 0; Index < 8; ++Index)
		{
			if (ActionId == Forward[Index]) { return Index + 1; }
		}
		return 0;
	}

	bool IsMenuAction(FName ActionId)
	{
		return ActionId == Actions::MenuUp || ActionId == Actions::MenuDown
			|| ActionId == Actions::MenuLeft || ActionId == Actions::MenuRight
			|| ActionId == Actions::MenuAccept || ActionId == Actions::MenuBack;
	}

	void ApplyMenuNavigation(
		FNavigationConfig& Config, const TArray<FApexKeyBinding>& Bindings, TArray<FKey>& InOutAdded)
	{
		for (const FKey& Key : InOutAdded)
		{
			Config.KeyEventRules.Remove(Key);
			Config.KeyActionRules.Remove(Key);
		}
		InOutAdded.Reset();

		struct FRule
		{
			FName ActionId;
			EUINavigation Direction;
			EUINavigationAction Action;
		};
		static const FRule Rules[] = {
			{ Actions::MenuUp,     EUINavigation::Up,      EUINavigationAction::Invalid },
			{ Actions::MenuDown,   EUINavigation::Down,    EUINavigationAction::Invalid },
			{ Actions::MenuLeft,   EUINavigation::Left,    EUINavigationAction::Invalid },
			{ Actions::MenuRight,  EUINavigation::Right,   EUINavigationAction::Invalid },
			{ Actions::MenuAccept, EUINavigation::Invalid, EUINavigationAction::Accept },
			{ Actions::MenuBack,   EUINavigation::Invalid, EUINavigationAction::Back },
		};

		for (const FApexKeyBinding& Binding : Bindings)
		{
			if (!IsWheelSlot(Binding.Slot) || !Binding.Key.IsValid())
			{
				continue;
			}
			const FRule* Rule = Algo::FindBy(Rules, Binding.ActionId, &FRule::ActionId);
			// Also skips a key bound to two menu slots: the first one wins.
			if (!Rule || Config.KeyEventRules.Contains(Binding.Key) || Config.KeyActionRules.Contains(Binding.Key))
			{
				continue;
			}
			if (Rule->Direction != EUINavigation::Invalid)
			{
				Config.KeyEventRules.Add(Binding.Key, Rule->Direction);
			}
			else
			{
				Config.KeyActionRules.Add(Binding.Key, Rule->Action);
			}
			InOutAdded.Add(Binding.Key);
		}
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
			{ EKeys::Gamepad_Special_Left.GetFName(),     TEXT("VIEW")   },
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

	int32 FindSteeringDevice(const TArray<FApexKeyBinding>& Bindings)
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
		return Device ? Device->Slot : INDEX_NONE;
	}

	int32 FindForceFeedbackDevice(const TArray<FApexKeyBinding>& Bindings)
	{
		const FApexSimInputModule* Input = FApexSimInputModule::Get();
		const int32 DeviceSlot = FindSteeringDevice(Bindings);
		const ApexDirectInput::FDeviceInfo* Device =
			Input && DeviceSlot != INDEX_NONE ? Input->FindDevice(DeviceSlot) : nullptr;
		// What it *can* do, not what it is doing: the device layer only takes a
		// wheel exclusively once this has named it, so asking the other way
		// round would mean no wheel ever played anything.
		return Device && Device->bCanPlayForces ? Device->Slot : INDEX_NONE;
	}

	float WheelSteeringScale(float RotationDeg, float SteeringLockDeg)
	{
		const float Rotation = FMath::Clamp(RotationDeg, WheelRotationMinDeg, WheelRotationMaxDeg);
		const float Lock = FMath::Clamp(SteeringLockDeg, SteeringLockMinDeg, Rotation);
		return Rotation / Lock;
	}

	bool ReadWheelSteering(const TArray<FApexKeyBinding>& Bindings, float& OutAxis)
	{
		const FApexSimInputModule* Input = FApexSimInputModule::Get();
		if (!Input)
		{
			return false;
		}

		auto IsAttached = [Input](int32 DeviceSlot) { return Input->IsAttached(DeviceSlot); };
		const FApexKeyBinding* Steering = FindBinding(Bindings, Actions::Steer, Slot::Wheel, IsAttached);
		const FKey Key = Steering ? Steering->Key : GetWheelDefaultKey(Actions::Steer, Slot::Wheel);

		const ApexDirectInput::FControl Control = ApexDirectInput::ParseKey(Key);
		if (!Control.IsValid() || Control.Kind != ApexDirectInput::EControlKind::Axis || !Input->IsAttached(Control.Slot))
		{
			return false;
		}
		const float Value = Input->GetControlValue(Control);
		OutAxis = Steering && Steering->bInvert ? -Value : Value;
		return true;
	}
}

// --- Modifiers --------------------------------------------------------------

FInputActionValue UApexInputModifierPedal::ModifyRaw_Implementation(
	const UEnhancedPlayerInput* PlayerInput, FInputActionValue CurrentValue, float DeltaTime)
{
	return FInputActionValue(FMath::Clamp(0.5f * (CurrentValue.Get<float>() + 1.0f), 0.0f, 1.0f));
}

FInputActionValue UApexInputModifierWheelSteering::ModifyRaw_Implementation(
	const UEnhancedPlayerInput* PlayerInput, FInputActionValue CurrentValue, float DeltaTime)
{
	const APlayerController* PlayerController = PlayerInput ? Cast<APlayerController>(PlayerInput->GetOuter()) : nullptr;
	const UGameInstance* GameInstance = PlayerController ? PlayerController->GetGameInstance() : nullptr;
	const UApexSettingsSubsystem* Settings = GameInstance ? GameInstance->GetSubsystem<UApexSettingsSubsystem>() : nullptr;

	const float Scale = Settings ? Settings->GetWheelSteeringScale() : 1.0f;
	return FInputActionValue(FMath::Clamp(CurrentValue.Get<float>() * Scale, -1.0f, 1.0f));
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
	Config->Clutch = MakeAction(Config, TEXT("IA_Clutch"), EInputActionValueType::Axis1D);
	Config->Brake = MakeAction(Config, TEXT("IA_Brake"), EInputActionValueType::Axis1D);
	Config->GearUp = MakeAction(Config, TEXT("IA_GearUp"), EInputActionValueType::Boolean);
	Config->GearDown = MakeAction(Config, TEXT("IA_GearDown"), EInputActionValueType::Boolean);
	Config->ToggleCamera =
		MakeAction(Config, TEXT("IA_ToggleCamera"), EInputActionValueType::Boolean);
	Config->Look = MakeAction(Config, TEXT("IA_Look"), EInputActionValueType::Axis1D);
	Config->LookBack = MakeAction(Config, TEXT("IA_LookBack"), EInputActionValueType::Boolean);
	Config->Drs = MakeAction(Config, TEXT("IA_Drs"), EInputActionValueType::Boolean);
	Config->Headlights = MakeAction(Config, TEXT("IA_Headlights"), EInputActionValueType::Boolean);
	Config->FlashLights = MakeAction(Config, TEXT("IA_FlashLights"), EInputActionValueType::Boolean);
	Config->ErsMode = MakeAction(Config, TEXT("IA_ErsMode"), EInputActionValueType::Boolean);
	Config->ErsBoost = MakeAction(Config, TEXT("IA_ErsBoost"), EInputActionValueType::Boolean);
	Config->Handbrake = MakeAction(Config, TEXT("IA_Handbrake"), EInputActionValueType::Boolean);
	Config->RecoverTrack = MakeAction(Config, TEXT("IA_RecoverTrack"), EInputActionValueType::Boolean);
	Config->RecoverPits = MakeAction(Config, TEXT("IA_RecoverPits"), EInputActionValueType::Boolean);
	Config->BoxThisLap = MakeAction(Config, TEXT("IA_BoxThisLap"), EInputActionValueType::Boolean);
	Config->Chat = MakeAction(Config, TEXT("IA_Chat"), EInputActionValueType::Boolean);
	Config->Mark = MakeAction(Config, TEXT("IA_Mark"), EInputActionValueType::Boolean);
	// Reverse, then 1 to 8: one held action each.
	for (int32 Gear = -1; Gear <= 8; ++Gear)
	{
		if (Gear == 0)
		{
			continue;
		}
		Config->HGears.Add(MakeAction(Config,
			*FString::Printf(TEXT("IA_HGear%s"), Gear < 0 ? TEXT("Reverse") : *FString::FromInt(Gear)),
			EInputActionValueType::Boolean));
	}

	Config->DriveContext = NewObject<UInputMappingContext>(Config, TEXT("IMC_Drive"));
	Config->ApplyBindings({});

	return Config;
}

UInputAction* UApexInputConfig::FindAction(FName ActionId) const
{
	if (ActionId == ApexInput::Actions::Throttle)     { return Throttle; }
	if (ActionId == ApexInput::Actions::Brake)        { return Brake; }
	if (ActionId == ApexInput::Actions::Clutch)       { return Clutch; }
	if (ActionId == ApexInput::Actions::Steer)        { return Steer; }
	if (ActionId == ApexInput::Actions::GearUp)       { return GearUp; }
	if (ActionId == ApexInput::Actions::GearDown)     { return GearDown; }
	if (ActionId == ApexInput::Actions::ToggleCamera) { return ToggleCamera; }
	if (ActionId == ApexInput::Actions::Look)         { return Look; }
	if (ActionId == ApexInput::Actions::LookBack)     { return LookBack; }
	if (ActionId == ApexInput::Actions::Drs)          { return Drs; }
	if (ActionId == ApexInput::Actions::Headlights)   { return Headlights; }
	if (ActionId == ApexInput::Actions::FlashLights)  { return FlashLights; }
	if (ActionId == ApexInput::Actions::ErsMode)      { return ErsMode; }
	if (ActionId == ApexInput::Actions::ErsBoost)     { return ErsBoost; }
	if (ActionId == ApexInput::Actions::Handbrake)    { return Handbrake; }
	if (ActionId == ApexInput::Actions::RecoverTrack) { return RecoverTrack; }
	if (ActionId == ApexInput::Actions::RecoverPits)  { return RecoverPits; }
	if (ActionId == ApexInput::Actions::BoxThisLap)   { return BoxThisLap; }
	if (ActionId == ApexInput::Actions::Chat)         { return Chat; }
	if (ActionId == ApexInput::Actions::Mark)         { return Mark; }
	{
		const int32 Gear = ApexInput::HGearOfAction(ActionId);
		if (Gear != 0)
		{
			return HGears.IsValidIndex(Gear < 0 ? 0 : Gear) ? HGears[Gear < 0 ? 0 : Gear].Get() : nullptr;
		}
	}
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
	if (Def.ActionId == ApexInput::Actions::Steer && bDirectInputAxis && ApexInput::IsWheelSlot(Def.Slot))
	{
		// After the negate: the scale is symmetric, but the clamp has to see
		// the value the right way round.
		Mapping.Modifiers.Add(NewObject<UApexInputModifierWheelSteering>(DriveContext));
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
