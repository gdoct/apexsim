#pragma once

#include "CoreMinimal.h"
#include "InputCoreTypes.h"
#include "UObject/Object.h"

#include "ApexInputConfig.generated.h"

class UInputAction;
class UInputMappingContext;
struct FApexKeyBinding;

/**
 * The bindable controls, as a flat table of slots.
 *
 * A slot is one key on one action. Steering needs three — a gamepad axis and
 * the two keyboard halves — so the settings screen edits slots rather than
 * actions, and the mapping context is rebuilt from whatever the slots hold.
 *
 * The table is the single source of truth for both sides: the input config maps
 * from it, and the controls screen lists from it. Adding a control is one entry
 * here and nothing else.
 */
namespace ApexInput
{
	/** Action ids. Stable — they are written into the settings save. */
	namespace Actions
	{
		inline const FName Throttle     = TEXT("Throttle");
		inline const FName Brake        = TEXT("Brake");
		inline const FName Steer        = TEXT("Steer");
		inline const FName GearUp       = TEXT("GearUp");
		inline const FName GearDown     = TEXT("GearDown");
		inline const FName ToggleCamera = TEXT("ToggleCamera");
		/**
		 * Not an Enhanced Input action: the pause key has to work while the race
		 * view owns input, so the root widget tests it directly. Listed here so
		 * it is rebindable and appears in the controls screen with the rest.
		 */
		inline const FName PauseMenu    = TEXT("PauseMenu");
	}

	/** One row of the controls screen: an action, a slot, and what it defaults to. */
	struct FSlotDef
	{
		FName ActionId;
		int32 Slot;
		/** Player-facing name, e.g. "Steer left". */
		const TCHAR* Label;
		FKey DefaultKey;
		/** See FApexKeyBinding::bNegate. */
		bool bNegate;
		/** True for the gamepad/wheel column, false for the keyboard column. */
		bool bDeviceColumn;
	};

	/** Every bindable slot, in the order the controls screen shows them. */
	APEXSIM_API const TArray<FSlotDef>& Slots();

	/** The slot's definition, or null if nothing declares it. */
	APEXSIM_API const FSlotDef* FindSlot(FName ActionId, int32 Slot);

	/** "LEFT SHIFT", "RB", "LS →" — short enough for a binding chip. */
	APEXSIM_API FString GetKeyDisplayName(const FKey& Key);
}

/**
 * The driving controls, as Enhanced Input actions and a mapping context.
 *
 * Built in code rather than authored as `.uasset`s. Every other piece of this
 * client is defined in C++ — widgets, catalogs, the track pipeline — and
 * binary input assets would be the one part of the control scheme you could
 * not read in a diff or change without the editor open. Nothing is lost by
 * doing it here: these are ordinary `UInputAction` objects, so rebinding at
 * runtime works exactly as it would with assets.
 *
 * Actions are deliberately limited to what the wire protocol can actually
 * carry (`FApexPlayerInput`: throttle, brake, steering, gear) plus the local
 * camera toggle. There is no handbrake action because there is nowhere to
 * send a handbrake.
 */
UCLASS()
class APEXSIM_API UApexInputConfig : public UObject
{
	GENERATED_BODY()

public:
	/** Build the actions and the mapping context with their default keys. */
	static UApexInputConfig* Create(UObject* Outer);

	/**
	 * Rebuild the mapping context from a binding table, with slots the table
	 * does not mention falling back to their defaults.
	 *
	 * The context object is kept and refilled rather than replaced: the player
	 * controller has already handed this exact object to the Enhanced Input
	 * subsystem, and swapping it out would leave the old mappings live.
	 */
	void ApplyBindings(const TArray<FApexKeyBinding>& Bindings);

	/** The action a slot drives, or null for slots with no Enhanced Input action. */
	UInputAction* FindAction(FName ActionId) const;

	/** Mapping context added while driving and removed on the way out. */
	UPROPERTY(Transient)
	TObjectPtr<UInputMappingContext> DriveContext;

	/**
	 * Axis1D, `+1` is right.
	 *
	 * Screen convention on purpose: the server's frame has positive steering
	 * to the *left*, and that flip belongs at the network boundary next to
	 * every other handedness conversion, not spread through the bindings.
	 */
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> Steer;

	/** Axis1D, 0..1. An axis rather than a button so a pedal can map to it. */
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> Throttle;

	/** Axis1D, 0..1. */
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> Brake;

	/** Digital, fires once per press. */
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> GearUp;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> GearDown;

	/** Digital. Local only — never reaches the server. */
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> ToggleCamera;
};
