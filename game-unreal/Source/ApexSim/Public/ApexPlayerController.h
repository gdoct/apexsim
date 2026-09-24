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
 * What the wheel was asked for while driving, summed over a window and
 * logged when it closes: the one record of how hard the base was driven
 * that a test session leaves behind, which "it feels weak" can be read
 * against.
 */
struct FApexWheelForceStats
{
	double Seconds = 0.0;
	double SumTorque = 0.0;
	double SumConstant = 0.0;
	double SumCorrection = 0.0;
	double SumRoad = 0.0;
	double SumLimit = 0.0;
	double SumVibration = 0.0;
	double SaturatedSeconds = 0.0;
	float PeakConstant = 0.0f;
	float PeakTorque = 0.0f;
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

	/**
	 * Pushes the wheel to the right for a moment.
	 *
	 * Which way a force turns a rim is the device's business — DirectInput does
	 * not promise it — so the Wheel page offers this and asks the player what
	 * happened, rather than the game guessing and a car fighting its driver.
	 */
	void TestWheelForce();

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
	/**
	 * What the car is doing this frame, as force-feedback signals: the server's
	 * DriverFeedback plus the speed and gear from telemetry. Inactive in the
	 * menus, behind the pause menu, and whenever the stream has gone quiet.
	 */
	ApexFfb::FSignals ReadDrivingSignals();

	/** This frame's rumble from the car, through the gamepad mixer. */
	ApexFfb::FRumble TickDrivingFeedback(const ApexFfb::FSignals& Signals, float DeltaSeconds);

	/** The same signals through the wheel mixer, onto whichever wheelbase steers. */
	void TickWheelFeedback(const ApexFfb::FSignals& Signals, float DeltaSeconds);

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
	ApexFfb::FWheelState WheelState;
	/** The last frame's forces, for `apexsim.ffb.Debug`. */
	FApexWheelEffects LastWheelEffects;
	/** The net subsystem's feedback serial last mixed, so each message's hits fire once. */
	uint32 LastFeedbackSerial = 0;
	float FeedbackPreviewSeconds = 0.0f;
	/** Counts down while the Wheel page's test force is pushing. */
	float WheelTestSeconds = 0.0f;

	FApexWheelForceStats WheelStats;
	void AccumulateWheelStats(const ApexFfb::FSignals& Signals, const FApexWheelEffects& Effects, float DeltaSeconds);
};
