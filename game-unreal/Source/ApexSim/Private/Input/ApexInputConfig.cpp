#include "Input/ApexInputConfig.h"

#include "EnhancedActionKeyMapping.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"

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

	UInputMappingContext* Context = NewObject<UInputMappingContext>(Config, TEXT("IMC_Drive"));

	MapKey(Context, Config->Throttle, EKeys::W);
	MapKey(Context, Config->Throttle, EKeys::Up);
	MapKey(Context, Config->Brake, EKeys::S);
	MapKey(Context, Config->Brake, EKeys::Down);

	// A steers left, so it is the negated half of the same axis.
	MapKey(Context, Config->Steer, EKeys::D);
	MapKey(Context, Config->Steer, EKeys::Right);
	MapKey(Context, Config->Steer, EKeys::A, /*bNegate*/ true);
	MapKey(Context, Config->Steer, EKeys::Left, /*bNegate*/ true);

	MapKey(Context, Config->GearUp, EKeys::E);
	MapKey(Context, Config->GearDown, EKeys::Q);
	MapKey(Context, Config->ToggleCamera, EKeys::C);

	Config->DriveContext = Context;
	return Config;
}
