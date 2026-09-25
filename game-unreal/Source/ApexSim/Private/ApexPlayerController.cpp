#include "ApexPlayerController.h"

#include "ApexNetSubsystem.h"
#include "ApexSettingsSave.h"
#include "ApexSettingsSubsystem.h"
#include "ApexSim.h"
#include "ApexSimInputModule.h"
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

	/** How long the wheel's direction test pushes, and how hard. */
	constexpr float kWheelTestSeconds = 0.7f;
	constexpr float kWheelTestForce = 0.35f;

	/** The shortest headlight flash: a tap still shows for this long. */
	constexpr double kMinFlashSeconds = 0.2;

	/** Driving seconds summed into each line of the wheel's force log. */
	constexpr double kWheelStatsWindowSeconds = 15.0;
	/** A constant force this near the base's peak is at its limit. */
	constexpr float kWheelSaturated = 0.95f;

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
	Input->BindAction(InputConfig->Drs, ETriggerEvent::Started, this, &AApexPlayerController::HandleDrs);
	Input->BindAction(InputConfig->Drs, ETriggerEvent::Completed, this, &AApexPlayerController::HandleDrsReleased);
	Input->BindAction(InputConfig->Headlights, ETriggerEvent::Started, this, &AApexPlayerController::HandleHeadlights);
	Input->BindAction(InputConfig->FlashLights, ETriggerEvent::Started, this, &AApexPlayerController::HandleFlashLights);
	Input->BindAction(InputConfig->FlashLights, ETriggerEvent::Completed, this, &AApexPlayerController::HandleFlashLightsReleased);
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
		PendingHeadlightToggles = 0;
		FlashPressedAt = -1.0e9;
	}

	bShowMouseCursor = true;
	UE_LOG(LogApexSim, Log, TEXT("Driving controls %s"),
		bEnabled ? TEXT("enabled (WASD, Q/E gears, C camera, ,/. look, B behind, L lights, H flash)") : TEXT("disabled"));
}

void AApexPlayerController::PreviewForceFeedback()
{
	FeedbackPreviewSeconds = kPreviewSeconds;
}

void AApexPlayerController::TestWheelForce()
{
	WheelTestSeconds = kWheelTestSeconds;
}

void AApexPlayerController::UpdateForceFeedback(IInputInterface* InputInterface, const int32 ControllerId)
{
	const float DeltaSeconds = static_cast<float>(FApp::GetDeltaTime());
	const ApexFfb::FSignals Signals = ReadDrivingSignals();

	FForceFeedbackValues Values = ForceFeedbackValues;
	const ApexFfb::FRumble Driving = TickDrivingFeedback(Signals, DeltaSeconds);
	// The wheel rides on the same hook: it is the one place that runs every
	// frame whether or not there is a pawn, a race or a pause menu in front.
	TickWheelFeedback(Signals, DeltaSeconds);

	// XInput, the engine's pad backend on Windows, drives the heavy motor from
	// the larger of the two Large channels and the light one from the Small
	// channels. Under the GameInput plugin the Small channels are the trigger
	// motors instead; it is not enabled in this project.
	Values.LeftLarge = FMath::Max(Values.LeftLarge, Driving.Low);
	Values.LeftSmall = FMath::Max(Values.LeftSmall, Driving.High);

	InputInterface->SetForceFeedbackChannelValues(ControllerId, bForceFeedbackEnabled ? Values : FForceFeedbackValues());
}

ApexFfb::FSignals AApexPlayerController::ReadDrivingSignals()
{
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
		const FApexCarTelemetry* Local = nullptr;
		const int32 CarIndex = Net->GetLocalCarIndex();
		for (const FApexCarTelemetry& Car : Net->GetLatestTelemetry().Cars)
		{
			if (Car.CarIndex == CarIndex)
			{
				Local = &Car;
				SpeedMps = Car.SpeedMps;
				Gear = Car.Gear;
				break;
			}
		}
		Signals = ApexFfb::MakeSignals(Net->GetDriverFeedback(), SpeedMps, Gear, bNewMessage);
		// What is being sent this frame, which the server's torque is a round
		// trip behind (ApexRaceDirector sends the server -Steer).
		Signals.bHasLocalSteer = true;
		Signals.LocalSteer = DriveInput.Steer;
		if (Local)
		{
			Signals.bHasStation = true;
			Signals.StationM = Local->TrackProgress;
		}
	}
	return Signals;
}

void AApexPlayerController::TickWheelFeedback(const ApexFfb::FSignals& Signals, float DeltaSeconds)
{
	FApexSimInputModule* Input = FApexSimInputModule::Get();
	UApexSettingsSubsystem* Settings = GetSettings();
	const UApexSettingsSave* Values = Settings ? Settings->Get() : nullptr;
	if (!Input || !Values)
	{
		return;
	}

	// Which wheelbase gets them is the steering binding's answer, worked out
	// every frame because a wheel can be plugged in mid-session.
	const int32 Slot = Settings->GetWheelDeviceSlot();
	if (Slot == INDEX_NONE)
	{
		LastWheelEffects = FApexWheelEffects();
		Input->SetWheelEffects(INDEX_NONE, LastWheelEffects);
		return;
	}

	ApexFfb::FWheelTuning Tuning;
	Tuning.Force = Values->WheelForce;
	Tuning.RoadEffects = Values->WheelRoadEffects;
	Tuning.Damping = Values->WheelDamping;
	Tuning.bInvert = Values->bWheelInvertForce;
	// A lock shorter than the base's rotation gets a stop at its ends; one as
	// long has the base's own.
	const float Scale = Settings->GetWheelSteeringScale();
	const float Rotation = FMath::Clamp(Values->WheelRotationDeg, ApexInput::WheelRotationMinDeg, ApexInput::WheelRotationMaxDeg);
	Tuning.SteeringLockDeg = Scale > 1.001f ? Rotation / Scale : 0.0f;
	Tuning.RimDegreesPerInput = 0.5f * Rotation / FMath::Max(Scale, 1.0f);

	// Where the rim is, for centring it and for the stop: the device's own
	// reading, which the server never sees.
	ApexFfb::FSignals WheelSignals = Signals;
	float Axis = 0.0f;
	if (ApexInput::ReadWheelSteering(Values->Bindings, Axis))
	{
		WheelSignals.bHasRim = true;
		WheelSignals.RimDegrees = Axis * 0.5f * Rotation;
	}

	FApexWheelEffects Effects = ApexFfb::MixWheel(WheelSignals, WheelState, DeltaSeconds, Tuning);

	if (WheelTestSeconds > 0.0f)
	{
		WheelTestSeconds -= DeltaSeconds;
		// Deliberately not the player's strength: the test has to be felt even
		// with the force slider down, and it is what tells them which way
		// "positive" turns their rim.
		const float Push = Values->bWheelInvertForce ? -kWheelTestForce : kWheelTestForce;
		Effects.Constant = FMath::Clamp(Effects.Constant + Push, -1.0f, 1.0f);
		Effects.Spring = 0.0f;
	}

	LastWheelEffects = Effects;
	Input->SetWheelEffects(Slot, Effects);
	AccumulateWheelStats(WheelSignals, Effects, DeltaSeconds);
}

void AApexPlayerController::AccumulateWheelStats(
	const ApexFfb::FSignals& Signals, const FApexWheelEffects& Effects, float DeltaSeconds)
{
	if (!Signals.bActive || Signals.SpeedMps < 5.0f || DeltaSeconds <= 0.0f)
	{
		return;
	}
	FApexWheelForceStats& S = WheelStats;
	const double Dt = DeltaSeconds;
	S.Seconds += Dt;
	S.SumTorque += FMath::Abs(Signals.SteerTorque) * Dt;
	S.SumConstant += FMath::Abs(Effects.Constant) * Dt;
	S.SumCorrection += FMath::Abs(WheelState.Correction) * Dt;
	S.SumRoad += FMath::Abs(WheelState.Road) * Dt;
	S.SumLimit += WheelState.StiffnessLimit * Dt;
	S.SumVibration += Effects.VibrationAmplitude * Dt;
	S.SaturatedSeconds += FMath::Abs(Effects.Constant) >= kWheelSaturated ? Dt : 0.0;
	S.PeakConstant = FMath::Max(S.PeakConstant, FMath::Abs(Effects.Constant));
	S.PeakTorque = FMath::Max(S.PeakTorque, FMath::Abs(Signals.SteerTorque));

	if (S.Seconds >= kWheelStatsWindowSeconds)
	{
		const UApexSettingsSubsystem* Settings = GetSettings();
		const UApexSettingsSave* Values = Settings ? Settings->Get() : nullptr;
		UE_LOG(LogApexSim, Log,
			TEXT("Wheel forces over %.0f s driving: torque |mean| %.2f peak %.2f -> constant |mean| %.2f peak %.2f, ")
			TEXT("at the base's limit %.0f%% of the time; rim correction |mean| %.3f, stiffness limit mean %.2f, road |mean| %.3f, vibration mean %.2f ")
			TEXT("(force %.2f, road %.2f, damping %.2f, invert %d)"),
			S.Seconds, S.SumTorque / S.Seconds, S.PeakTorque, S.SumConstant / S.Seconds, S.PeakConstant,
			100.0 * S.SaturatedSeconds / S.Seconds, S.SumCorrection / S.Seconds, S.SumLimit / S.Seconds, S.SumRoad / S.Seconds,
			S.SumVibration / S.Seconds,
			Values ? Values->WheelForce : -1.0f, Values ? Values->WheelRoadEffects : -1.0f,
			Values ? Values->WheelDamping : -1.0f, Values && Values->bWheelInvertForce ? 1 : 0);
		S = FApexWheelForceStats();
	}
}

ApexFfb::FRumble AApexPlayerController::TickDrivingFeedback(const ApexFfb::FSignals& Signals, float DeltaSeconds)
{
	const UApexSettingsSubsystem* Settings = GetSettings();
	const float Strength = Settings && Settings->Get() ? Settings->Get()->Vibration : 0.0f;
	const float Gain = ApexFfb::GainFromStrength(Strength);

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
		const int32 WheelSlot = Settings ? Settings->GetWheelDeviceSlot() : INDEX_NONE;
		GEngine->AddOnScreenDebugMessage(static_cast<uint64>(0xFFB0), 0.0f, FColor::Cyan,
			FString::Printf(TEXT("FFB %s  gain %.2f  low %.2f  high %.2f\n")
				TEXT("  speed %.1f  torque %+.2f  front %.2f  rear %.2f  lock %.2f  spin %.2f  abs %d  tc %d\n")
				TEXT("  curb L %.1f R %.1f  off %.2f  bump %.2f  impact %.1f\n")
				TEXT("  wheel %s  force %+.2f  vibration %.2f @ %.0f Hz  damper %.2f  spring %.2f\n")
				TEXT("  rim correction %+.3f (slope %+.1f, input %+.3f vs server %+.3f)  road %+.3f  front load %.2f  brake slip %.2f  station %.0f"),
				Signals.bActive ? TEXT("live") : TEXT("idle"), Gain, Rumble.Low, Rumble.High,
				Signals.SpeedMps, Signals.SteerTorque, Signals.FrontSlide, Signals.RearSlide,
				Signals.Lockup, Signals.Wheelspin, Signals.bAbs ? 1 : 0, Signals.bTractionControl ? 1 : 0,
				Signals.CurbLeft, Signals.CurbRight, Signals.OffTrack, Signals.BumpMps, Signals.ImpactMps,
				WheelSlot == INDEX_NONE ? TEXT("none") : *FString::Printf(TEXT("device %d"), WheelSlot + 1),
				LastWheelEffects.Constant, LastWheelEffects.VibrationAmplitude, LastWheelEffects.VibrationHz,
				LastWheelEffects.Damper, LastWheelEffects.Spring,
				WheelState.Correction, WheelState.Stiffness, Signals.LocalSteer, WheelState.ServerSteer, WheelState.Road,
				Signals.FrontLoad, Signals.FrontBrakeSlip, WheelState.RoadStation));
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

int32 AApexPlayerController::ConsumeHeadlightToggles()
{
	const int32 Toggles = PendingHeadlightToggles;
	PendingHeadlightToggles = 0;
	return Toggles;
}

bool AApexPlayerController::IsFlashingLights() const
{
	return DriveInput.bFlashLights || FPlatformTime::Seconds() - FlashPressedAt < kMinFlashSeconds;
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
	// Already shaped, and only where shaping belongs: the pad's deadzone and
	// curve are a modifier on the pad's own mappings
	// (UApexInputModifierPadSteering), because a wheel must not be given a
	// thumbstick's deadzone and this handler cannot tell the two apart.
	DriveInput.Steer = FMath::Clamp(Value.Get<float>(), -1.0f, 1.0f);
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

void AApexPlayerController::HandleDrs(const FInputActionValue&)
{
	DriveInput.bDrs = true;
}

void AApexPlayerController::HandleDrsReleased(const FInputActionValue&)
{
	DriveInput.bDrs = false;
}

void AApexPlayerController::HandleHeadlights(const FInputActionValue&)
{
	++PendingHeadlightToggles;
}

void AApexPlayerController::HandleFlashLights(const FInputActionValue&)
{
	DriveInput.bFlashLights = true;
	FlashPressedAt = FPlatformTime::Seconds();
}

void AApexPlayerController::HandleFlashLightsReleased(const FInputActionValue&)
{
	DriveInput.bFlashLights = false;
}
