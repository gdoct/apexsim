#include "ApexPlayerController.h"

#include "ApexSettingsSubsystem.h"
#include "ApexSim.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/GameInstance.h"
#include "Input/ApexInputConfig.h"
#include "InputActionValue.h"

namespace
{
	/** Priority of the driving context. Nothing else competes for it yet. */
	constexpr int32 kDriveContextPriority = 0;
}	 // namespace

AApexPlayerController::AApexPlayerController()
{
	bShowMouseCursor = true;
}

void AApexPlayerController::BeginPlay()
{
	Super::BeginPlay();
	EnsureInputConfig();
}

void AApexPlayerController::EnsureInputConfig()
{
	// Exactly one config, ever.
	//
	// `SetupInputComponent` runs before `BeginPlay`, so creating a config in
	// each meant the bindings referenced one set of `UInputAction` objects
	// while the mapping context added later referenced a second, freshly
	// built set. Every key press then resolved to an action nothing was
	// listening for, and the controls were silently dead — the mapping
	// context was present and correct, which made it look like an input-mode
	// problem rather than a lifetime one.
	if (!InputConfig)
	{
		InputConfig = UApexInputConfig::Create(this);
		ApplySavedBindings();
	}
}

UApexSettingsSubsystem* AApexPlayerController::GetSettings() const
{
	const UGameInstance* GameInstance = GetGameInstance();
	return GameInstance ? GameInstance->GetSubsystem<UApexSettingsSubsystem>() : nullptr;
}

void AApexPlayerController::ApplySavedBindings()
{
	const UApexSettingsSubsystem* Settings = GetSettings();
	if (InputConfig && Settings && Settings->Get())
	{
		InputConfig->ApplyBindings(Settings->Get()->Bindings);
	}
}

void AApexPlayerController::RebuildBindings()
{
	ApplySavedBindings();

	if (UEnhancedInputLocalPlayerSubsystem* Subsystem =
			ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(GetLocalPlayer()))
	{
		Subsystem->RequestRebuildControlMappings();
	}
}

void AApexPlayerController::SetupInputComponent()
{
	Super::SetupInputComponent();

	UEnhancedInputComponent* Input = Cast<UEnhancedInputComponent>(InputComponent);
	if (!Input)
	{
		UE_LOG(LogApexSim, Error,
			TEXT("InputComponent is not a UEnhancedInputComponent; driving controls are dead. "
				 "Check DefaultInputComponentClass in DefaultInput.ini"));
		return;
	}

	EnsureInputConfig();

	// Triggered fires every frame the axis is non-zero; Completed fires once
	// when it falls back to zero. Both are needed — without Completed the
	// last non-zero value would stick after the key came up, and the car
	// would drive off on its own.
	Input->BindAction(
		InputConfig->Throttle, ETriggerEvent::Triggered, this, &AApexPlayerController::HandleThrottle);
	Input->BindAction(InputConfig->Throttle, ETriggerEvent::Completed, this,
		&AApexPlayerController::HandleThrottleReleased);
	Input->BindAction(
		InputConfig->Brake, ETriggerEvent::Triggered, this, &AApexPlayerController::HandleBrake);
	Input->BindAction(InputConfig->Brake, ETriggerEvent::Completed, this,
		&AApexPlayerController::HandleBrakeReleased);
	Input->BindAction(
		InputConfig->Steer, ETriggerEvent::Triggered, this, &AApexPlayerController::HandleSteer);
	Input->BindAction(InputConfig->Steer, ETriggerEvent::Completed, this,
		&AApexPlayerController::HandleSteerReleased);

	Input->BindAction(
		InputConfig->GearUp, ETriggerEvent::Started, this, &AApexPlayerController::HandleGearUp);
	Input->BindAction(
		InputConfig->GearDown, ETriggerEvent::Started, this, &AApexPlayerController::HandleGearDown);
	Input->BindAction(InputConfig->ToggleCamera, ETriggerEvent::Started, this,
		&AApexPlayerController::HandleToggleCamera);
}

void AApexPlayerController::SetDriveInputEnabled(bool bEnabled)
{
	if (bDriveInputEnabled == bEnabled)
	{
		return;
	}
	bDriveInputEnabled = bEnabled;

	if (UEnhancedInputLocalPlayerSubsystem* Subsystem =
			ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(GetLocalPlayer()))
	{
		if (bEnabled && InputConfig)
		{
			Subsystem->AddMappingContext(InputConfig->DriveContext, kDriveContextPriority);
		}
		else if (InputConfig)
		{
			Subsystem->RemoveMappingContext(InputConfig->DriveContext);
		}
	}
	else
	{
		UE_LOG(LogApexSim, Error, TEXT("No EnhancedInput subsystem; driving controls are dead"));
	}

	if (bEnabled)
	{
		// UI-only input never reaches the viewport's game input path, so the
		// bindings above would receive nothing at all. Game-and-UI keeps the
		// menu clickable and lets Escape through to the root widget, which
		// handles it and passes everything else down to us.
		FInputModeGameAndUI InputMode;
		InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
		InputMode.SetHideCursorDuringCapture(false);
		SetInputMode(InputMode);
	}
	else
	{
		FInputModeUIOnly InputMode;
		InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
		SetInputMode(InputMode);
		// A key still held when the race ended would otherwise never send its
		// release, leaving the throttle latched on for the next one.
		FlushPressedKeys();
		DriveInput = FApexDriveInput();
		PendingGearDelta = 0;
		bPendingCameraToggle = false;
	}

	bShowMouseCursor = true;
	UE_LOG(LogApexSim, Log, TEXT("Driving controls %s"),
		bEnabled ? TEXT("enabled (WASD, Q/E gears, C camera)") : TEXT("disabled"));
}

int32 AApexPlayerController::ConsumeGearDelta()
{
	const int32 Delta = PendingGearDelta;
	PendingGearDelta = 0;
	return Delta;
}

bool AApexPlayerController::ConsumeCameraToggle()
{
	const bool bToggled = bPendingCameraToggle;
	bPendingCameraToggle = false;
	return bToggled;
}

void AApexPlayerController::HandleThrottle(const FInputActionValue& Value)
{
	DriveInput.Throttle = FMath::Clamp(Value.Get<float>(), 0.0f, 1.0f);
}

void AApexPlayerController::HandleThrottleReleased(const FInputActionValue&)
{
	DriveInput.Throttle = 0.0f;
}

void AApexPlayerController::HandleBrake(const FInputActionValue& Value)
{
	DriveInput.Brake = FMath::Clamp(Value.Get<float>(), 0.0f, 1.0f);
}

void AApexPlayerController::HandleBrakeReleased(const FInputActionValue&)
{
	DriveInput.Brake = 0.0f;
}

void AApexPlayerController::HandleSteer(const FInputActionValue& Value)
{
	const float Raw = FMath::Clamp(Value.Get<float>(), -1.0f, 1.0f);

	// Deadzone and sensitivity are applied here rather than as Enhanced Input
	// modifiers: the modifiers live on the mapping, so changing either would
	// mean rebuilding the context on every slider frame.
	const UApexSettingsSubsystem* Settings = GetSettings();
	DriveInput.Steer = Settings ? Settings->ShapeSteering(Raw) : Raw;
}

void AApexPlayerController::HandleSteerReleased(const FInputActionValue&)
{
	DriveInput.Steer = 0.0f;
}

void AApexPlayerController::HandleGearUp(const FInputActionValue&)
{
	++PendingGearDelta;
}

void AApexPlayerController::HandleGearDown(const FInputActionValue&)
{
	--PendingGearDelta;
}

void AApexPlayerController::HandleToggleCamera(const FInputActionValue&)
{
	bPendingCameraToggle = true;
}
