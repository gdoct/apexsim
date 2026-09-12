#pragma once

#include "CoreMinimal.h"
#include "InputCoreTypes.h"
#include "InputModifiers.h"
#include "UObject/Object.h"

#include "ApexInputConfig.generated.h"

class UInputAction;
class UInputMappingContext;
struct FApexKeyBinding;

/**
 * The bindable controls, as a flat table of slots.
 *
 * A slot is one key on one action. Steering needs four — a gamepad axis, the
 * two keyboard halves and the wheel's axis — so the settings screen edits
 * slots rather than actions, and the mapping context is rebuilt from whatever
 * the slots hold.
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
		/** Head turn, an axis: a stick, or a key either side. */
		inline const FName Look         = TEXT("Look");
		/** Held: look straight behind. */
		inline const FName LookBack     = TEXT("LookBack");
		/**
		 * Not an Enhanced Input action: the pause key has to work while the race
		 * view owns input, so the root widget tests it directly. Listed here so
		 * it is rebindable and appears in the controls screen with the rest.
		 */
		inline const FName PauseMenu    = TEXT("PauseMenu");
	}

	/** Which screen a slot is edited on, which is also which device it is for. */
	enum class EColumn : uint8
	{
		Gamepad,
		Keyboard,
		/** A wheel, its pedals, its shifter: anything that arrives through DirectInput. */
		Wheel,
	};

	/**
	 * Slot numbers. Stable — they are written into the settings save, so a
	 * number here is never reused for something else.
	 */
	namespace Slot
	{
		inline constexpr int32 Gamepad = 0;
		inline constexpr int32 Keyboard = 1;
		/** The keyboard halves of an axis: "steer left" and "steer right". */
		inline constexpr int32 KeyboardLow = 2;
		inline constexpr int32 KeyboardHigh = 3;
		inline constexpr int32 Wheel = 4;
		inline constexpr int32 WheelLow = 5;
		inline constexpr int32 WheelHigh = 6;
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
		EColumn Column;
	};

	/** Every bindable slot, in the order the controls screen shows them. */
	APEXSIM_API const TArray<FSlotDef>& Slots();

	/** The slot's definition, or null if nothing declares it. */
	APEXSIM_API const FSlotDef* FindSlot(FName ActionId, int32 Slot);

	/** "LEFT SHIFT", "RB", "WHEEL X" — short enough for a binding chip. */
	APEXSIM_API FString GetKeyDisplayName(const FKey& Key);

	/** The device's own name and the control on it: "FANATEC CSL DD · X". */
	APEXSIM_API FString GetKeyLongName(const FKey& Key);

	inline bool IsWheelSlot(int32 Slot) { return Slot >= Slot::Wheel; }

	/**
	 * An axis slot that runs both ways from a centre (steering, looking), as
	 * against one that runs one way from a rest position (a pedal).
	 *
	 * It decides how a DirectInput axis is read: a pedal reports its whole
	 * travel as -1..1 and has to be folded into 0..1, or resting on the stop
	 * would be half throttle.
	 */
	APEXSIM_API bool IsCentredAxisSlot(FName ActionId, int32 Slot);

	/** Whether a DirectInput device is attached, as the binding rules ask it. */
	using FDeviceAttached = TFunctionRef<bool(int32 DeviceSlot)>;

	/**
	 * The binding a slot resolves to, or null when nothing is stored for it.
	 *
	 * The wheel column holds one binding per device, so that a player with two
	 * wheelbases keeps both mappings and plugging one in is all it takes to
	 * drive: the attached device's binding wins, then an explicit unbinding,
	 * then the most recently bound device that is not here right now.
	 */
	APEXSIM_API const FApexKeyBinding* FindBinding(
		const TArray<FApexKeyBinding>& Bindings, FName ActionId, int32 Slot, FDeviceAttached IsAttached);

	/**
	 * Stores a captured key, following the same per-device rules: a wheel
	 * binding replaces the one for its own device and leaves other devices'
	 * alone, and unbinding only unbinds the device in front of the player.
	 * An invalid key is an unbinding, which is not the same as never having
	 * touched the slot — the default does not come back.
	 */
	APEXSIM_API void StoreBinding(
		TArray<FApexKeyBinding>& Bindings, FName ActionId, int32 Slot, const FKey& Key, bool bInvert,
		FDeviceAttached IsAttached);

	/** Drops every binding of one column, leaving the others as they are. */
	APEXSIM_API void ResetColumn(TArray<FApexKeyBinding>& Bindings, EColumn Column);

	/**
	 * What an unbound wheel slot falls back to: the steering axis of the first
	 * attached wheelbase, so a wheel steers the car the moment it is plugged
	 * in. Nothing else can be guessed — which pedal is the throttle is a
	 * different answer on every set.
	 */
	APEXSIM_API FKey GetWheelDefaultKey(FName ActionId, int32 Slot);

	/**
	 * The DirectInput device slot that should play the forces: the one the
	 * steering is bound to, when it is attached and can play them.
	 */
	APEXSIM_API int32 FindForceFeedbackDevice(const TArray<FApexKeyBinding>& Bindings);
}

/**
 * A pedal's travel as 0..1.
 *
 * A DirectInput axis reports its whole range, so a pedal at rest reads -1 and
 * fully pressed +1 (or the other way round, which the binding's own invert
 * has already put right by the time this runs). Folding that into 0..1 is
 * what makes "resting" mean "no throttle" — and it has to be a modifier
 * rather than something the handler does, because the same action is also fed
 * by a trigger and a key, which are 0..1 already.
 */
UCLASS(NotBlueprintable, HideDropdown)
class APEXSIM_API UApexInputModifierPedal : public UInputModifier
{
	GENERATED_BODY()

protected:
	virtual FInputActionValue ModifyRaw_Implementation(
		const UEnhancedPlayerInput* PlayerInput, FInputActionValue CurrentValue, float DeltaTime) override;
};

/**
 * The gamepad's steering curve: deadzone, then sensitivity.
 *
 * On the mapping rather than in the handler because a wheel must not get it —
 * a deadzone that is sensible for a thumbstick is several degrees of a wheel
 * that has none — and the handler cannot tell which device a value came from.
 * It reads the settings as it runs, so dragging the slider is felt at once
 * without the mapping context being rebuilt under the player's hands.
 */
UCLASS(NotBlueprintable, HideDropdown)
class APEXSIM_API UApexInputModifierPadSteering : public UInputModifier
{
	GENERATED_BODY()

protected:
	virtual FInputActionValue ModifyRaw_Implementation(
		const UEnhancedPlayerInput* PlayerInput, FInputActionValue CurrentValue, float DeltaTime) override;
};

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

	/**
	 * Maps one key for a slot, with the modifiers that slot's device needs:
	 * the negate of a half-axis or an inverted pedal, a DirectInput axis
	 * folded into a pedal's 0..1, and the pad's steering curve.
	 */
	void MapSlot(const ApexInput::FSlotDef& Def, UInputAction* Action, const FKey& Key, bool bNegate);

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

	/** Axis1D, `+1` looks right. Local only. */
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> Look;

	/** Digital, held. Local only. */
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> LookBack;
};
