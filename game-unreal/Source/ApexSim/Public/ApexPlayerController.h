#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "Input/ApexForceFeedback.h"

#include "ApexPlayerController.generated.h"

class UApexInputConfig;
class UApexSettingsSubsystem;

/** Driving controls as of this frame, in screen convention. */
USTRUCT(BlueprintType)
struct APEXSIM_API FApexDriveInput
{
	GENERATED_BODY()

	/** 0..1 */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Input")
	float Throttle = 0.0f;

	/** 0..1 */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Input")
	float Brake = 0.0f;

	/** -1..1, positive is right. The server's frame is the other way round. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Input")
	float Steer = 0.0f;

	/** Head turn, -1..1, positive is right. Never sent; the camera's alone. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Input")
	float Look = 0.0f;

	/** Held: look straight behind. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Input")
	bool bLookBack = false;
};

/**
 * Owns the driving controls.
 *
 * The menu shell has no pawn, so the controller is where input lives. It
 * holds an Enhanced Input mapping context that is added only while a race is
 * running — outside a race the driving keys stay unbound, so nothing has to
 * guess whether a keypress meant "throttle" or "navigate the menu".
 *
 * Input mode matters as much as the bindings: the shell runs in UI-only
 * input, where the viewport discards game input entirely and no amount of
 * correct binding produces a single event. `SetDriveInputEnabled` moves to
 * game-and-UI for the duration, which still lets Escape reach the menu.
 */
UCLASS()
class APEXSIM_API AApexPlayerController : public APlayerController
{
	GENERATED_BODY()

public:
	AApexPlayerController();

	/** Bind or unbind the driving controls and switch input mode to match. */
	UFUNCTION(BlueprintCallable, Category = "ApexSim|Input")
	void SetDriveInputEnabled(bool bEnabled);

	UFUNCTION(BlueprintPure, Category = "ApexSim|Input")
	bool IsDriveInputEnabled() const { return bDriveInputEnabled; }

	/** Current control positions. Zeroed while driving input is disabled. */
	const FApexDriveInput& GetDriveInput() const { return DriveInput; }

	/**
	 * Gear changes requested since the last call, and clears the tally.
	 *
	 * Shifts are discrete events but input is sent on a tick, so they have to
	 * be latched — polling key state would drop a shift pressed and released
	 * between two sends.
	 */
	int32 ConsumeGearDelta();

	/** True on the frame the camera-toggle key went down. */
	bool ConsumeCameraToggle();

	/**
	 * Rebuild the mapping context from the saved bindings.
	 *
	 * The context object is reused, so the Enhanced Input subsystem is holding
	 * the right pointer already — but it caches the resolved mappings, and
	 * without asking for a rebuild a rebind does nothing until the context is
	 * removed and re-added.
	 */
	UFUNCTION(BlueprintCallable, Category = "ApexSim|Input")
	void RebuildBindings();

	/**
	 * A short rumble at the saved strength, so the settings slider can be
	 * felt while it is dragged. Plays in menus too.
	 */
	void PreviewForceFeedback();

protected:
	virtual void SetupInputComponent() override;
	virtual void BeginPlay() override;

	/**
	 * Where the driving rumble joins the pad. The engine calls this every tick
	 * with whatever its own force-feedback effects add up to; the car's
	 * feedback is mixed in on top.
	 */
	virtual void UpdateForceFeedback(IInputInterface* InputInterface, const int32 ControllerId) override;

private:
	/** This frame's rumble from the car: the server's DriverFeedback through the gamepad mixer. */
	ApexFfb::FRumble TickDrivingFeedback(float DeltaSeconds);

	/** Create the input config once, whichever init step gets there first. */
	void EnsureInputConfig();

	UApexSettingsSubsystem* GetSettings() const;

	/** Fills the mapping context from the settings slot. */
	void ApplySavedBindings();

	void HandleThrottle(const struct FInputActionValue& Value);
	void HandleBrake(const struct FInputActionValue& Value);
	void HandleSteer(const struct FInputActionValue& Value);
	void HandleThrottleReleased(const struct FInputActionValue& Value);
	void HandleBrakeReleased(const struct FInputActionValue& Value);
	void HandleSteerReleased(const struct FInputActionValue& Value);
	void HandleGearUp(const struct FInputActionValue& Value);
	void HandleGearDown(const struct FInputActionValue& Value);
	void HandleToggleCamera(const struct FInputActionValue& Value);
	void HandleLook(const struct FInputActionValue& Value);
	void HandleLookReleased(const struct FInputActionValue& Value);
	void HandleLookBack(const struct FInputActionValue& Value);
	void HandleLookBackReleased(const struct FInputActionValue& Value);

	UPROPERTY(Transient)
	TObjectPtr<UApexInputConfig> InputConfig;

	FApexDriveInput DriveInput;
	int32 PendingGearDelta = 0;
	bool bPendingCameraToggle = false;
	bool bDriveInputEnabled = false;

	ApexFfb::FGamepadState FeedbackState;
	/** The net subsystem's feedback serial last mixed, so each message's hits fire once. */
	uint32 LastFeedbackSerial = 0;
	float FeedbackPreviewSeconds = 0.0f;
};
