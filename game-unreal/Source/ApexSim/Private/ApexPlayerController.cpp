#include "ApexPlayerController.h"

#include "ApexNetSubsystem.h"
#include "ApexSettingsSave.h"
#include "ApexSettingsSubsystem.h"
#include "ApexSim.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "GenericPlatform/IInputInterface.h"
#include "HAL/IConsoleManager.h"
#include "Input/ApexInputConfig.h"
#include "InputActionValue.h"
#include "Misc/App.h"
#include "Widgets/SViewport.h"

namespace
{
	/** Priority of the driving context. Nothing else competes for it yet. */
	constexpr int32 kDriveContextPriority = 0;

	/**
	 * Feedback older than this is a car that no longer exists (session left,
	 * connection lost) or a stalled stream; the rumble lets go rather than
	 * holding the last slide forever. Several 60 Hz messages' worth.
	 */
	constexpr double kFeedbackStaleSeconds = 0.25;

	/** How long the settings slider's preview rumble lasts after each change. */
	constexpr float kPreviewSeconds = 0.25f;

	TAutoConsoleVariable<bool> CVarFeedbackDebug(
		TEXT("apexsim.ffb.Debug"),
		false,
		TEXT("Print the force-feedback signals and the pad's motor levels on screen every frame."));
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

	// Look is an axis like steering, and a held button for straight behind.
	Input->BindAction(InputConfig->Look, ETriggerEvent::Triggered, this, &AApexPlayerController::HandleLook);
	Input->BindAction(InputConfig->Look, ETriggerEvent::Completed, this, &AApexPlayerController::HandleLookReleased);
	Input->BindAction(InputConfig->LookBack, ETriggerEvent::Started, this, &AApexPlayerController::HandleLookBack);
	Input->BindAction(InputConfig->LookBack, ETriggerEvent::Completed, this, &AApexPlayerController::HandleLookBackReleased);
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
		//
		// Focus has to be moved explicitly: game-and-UI without a widget to
		// focus leaves it wherever the menu had it, and a focus path through
		// the shell ends on the root widget, where Slate's default handler
		// answers the left stick, D-pad and arrows with menu navigation and
		// consumes them. Throttle and the shoulder buttons still got through,
		// which is what made it look like a binding problem.
		FInputModeGameAndUI InputMode;
		InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
		InputMode.SetHideCursorDuringCapture(false);
		if (const UGameViewportClient* GameViewport = GetWorld() ? GetWorld()->GetGameViewport() : nullptr)
		{
			InputMode.SetWidgetToFocus(GameViewport->GetGameViewportWidget());
		}
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
		bEnabled ? TEXT("enabled (WASD, Q/E gears, C camera, ,/. look, B behind)") : TEXT("disabled"));
}

void AApexPlayerController::PreviewForceFeedback()
{
	FeedbackPreviewSeconds = kPreviewSeconds;
}

void AApexPlayerController::UpdateForceFeedback(IInputInterface* InputInterface, const int32 ControllerId)
{
	FForceFeedbackValues Values = ForceFeedbackValues;
	const ApexFfb::FRumble Driving = TickDrivingFeedback(static_cast<float>(FApp::GetDeltaTime()));

	// XInput, the engine's pad backend on Windows, drives the heavy motor from
	// the larger of the two Large channels and the light one from the Small
	// channels. Under the GameInput plugin the Small channels are the trigger
	// motors instead; it is not enabled in this project.
	Values.LeftLarge = FMath::Max(Values.LeftLarge, Driving.Low);
	Values.LeftSmall = FMath::Max(Values.LeftSmall, Driving.High);

	InputInterface->SetForceFeedbackChannelValues(ControllerId, bForceFeedbackEnabled ? Values : FForceFeedbackValues());
}

ApexFfb::FRumble AApexPlayerController::TickDrivingFeedback(float DeltaSeconds)
{
	const UApexSettingsSubsystem* Settings = GetSettings();
	const float Strength = Settings && Settings->Get() ? Settings->Get()->Vibration : 0.0f;
	const float Gain = ApexFfb::GainFromStrength(Strength);

	// Only the car being driven: not in menus, not behind the pause menu, and
	// only while the server is actually sending feedback for it.
	ApexFfb::FSignals Signals;
	const UGameInstance* GameInstance = GetGameInstance();
	const UApexNetSubsystem* Net = GameInstance ? GameInstance->GetSubsystem<UApexNetSubsystem>() : nullptr;

	// Tracked while paused too, or resuming would replay the last message's hits.
	const uint32 Serial = Net ? Net->GetDriverFeedbackSerial() : 0;
	const bool bNewMessage = Serial != LastFeedbackSerial;
	LastFeedbackSerial = Serial;

	if (bDriveInputEnabled && Net
		&& FPlatformTime::Seconds() - Net->GetDriverFeedbackTime() < kFeedbackStaleSeconds)
	{
		// Speed and gear are telemetry's; the feedback carries only what it adds.
		float SpeedMps = 0.0f;
		int32 Gear = 0;
		const int32 CarIndex = Net->GetLocalCarIndex();
		for (const FApexCarTelemetry& Car : Net->GetLatestTelemetry().Cars)
		{
			if (Car.CarIndex == CarIndex)
			{
				SpeedMps = Car.SpeedMps;
				Gear = Car.Gear;
				break;
			}
		}
		Signals = ApexFfb::MakeSignals(Net->GetDriverFeedback(), SpeedMps, Gear, bNewMessage);
	}

	ApexFfb::FRumble Rumble = ApexFfb::MixGamepad(Signals, FeedbackState, DeltaSeconds, Gain);

	if (FeedbackPreviewSeconds > 0.0f)
	{
		FeedbackPreviewSeconds -= DeltaSeconds;
		const float Preview = FMath::Clamp(0.5f * Gain, 0.0f, 1.0f);
		Rumble.Low = FMath::Max(Rumble.Low, Preview);
		Rumble.High = FMath::Max(Rumble.High, Preview);
	}

	if (CVarFeedbackDebug.GetValueOnGameThread() && GEngine)
	{
		GEngine->AddOnScreenDebugMessage(static_cast<uint64>(0xFFB0), 0.0f, FColor::Cyan,
			FString::Printf(TEXT("FFB %s  gain %.2f  low %.2f  high %.2f\n")
				TEXT("  speed %.1f  torque %+.2f  front %.2f  rear %.2f  lock %.2f  spin %.2f  abs %d  tc %d\n")
				TEXT("  curb L %.1f R %.1f  off %.2f  bump %.2f  impact %.1f"),
				Signals.bActive ? TEXT("live") : TEXT("idle"), Gain, Rumble.Low, Rumble.High,
				Signals.SpeedMps, Signals.SteerTorque, Signals.FrontSlide, Signals.RearSlide,
				Signals.Lockup, Signals.Wheelspin, Signals.bAbs ? 1 : 0, Signals.bTractionControl ? 1 : 0,
				Signals.CurbLeft, Signals.CurbRight, Signals.OffTrack, Signals.BumpMps, Signals.ImpactMps));
	}

	return Rumble;
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

void AApexPlayerController::HandleLook(const FInputActionValue& Value)
{
	DriveInput.Look = FMath::Clamp(Value.Get<float>(), -1.0f, 1.0f);
}

void AApexPlayerController::HandleLookReleased(const FInputActionValue&)
{
	DriveInput.Look = 0.0f;
}

void AApexPlayerController::HandleLookBack(const FInputActionValue&)
{
	DriveInput.bLookBack = true;
}

void AApexPlayerController::HandleLookBackReleased(const FInputActionValue&)
{
	DriveInput.bLookBack = false;
}
