#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "ApexInputConfig.generated.h"

class UInputAction;
class UInputMappingContext;

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
