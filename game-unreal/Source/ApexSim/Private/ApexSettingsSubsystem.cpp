#include "ApexSettingsSubsystem.h"

#include "ApexBootSettings.h"
#include "ApexDirectInputTypes.h"
#include "ApexPlayerController.h"
#include "ApexSim.h"
#include "ApexSimInputModule.h"
#include "AudioDevice.h"
#include "Engine/Engine.h"
#include "GameFramework/GameUserSettings.h"
#include "HAL/IConsoleManager.h"
#include "Input/ApexInputConfig.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Race/ApexRaceDirector.h"

namespace
{
	/** Scalability bucket a preset puts every quality row into. */
	int32 PresetBucket(EApexGraphicsPreset Preset)
	{
		switch (Preset)
		{
		case EApexGraphicsPreset::Low:    return 0;
		case EApexGraphicsPreset::Medium: return 1;
		case EApexGraphicsPreset::High:   return 2;
		case EApexGraphicsPreset::Ultra:  return 3;
		default:                          return -1;
		}
	}
}

void UApexSettingsSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// settings.yml is the authority on the display block, so it has to be read
	// before Load() decides what to apply.
	Collection.InitializeDependency<UApexBootSettingsSubsystem>();

	Load();

	ApplyGraphics();
	// Gameplay too, because one of its effects is a console variable rather than
	// a widget: the minimap needs centerline parsing switched on before the first
	// LobbyState arrives, which is well before any HUD exists to ask for it.
	// Controls are left out — the player controller does not exist yet, and it
	// reads the bindings itself when it builds its input config.
	ApplyGameplay();
	// The audio device exists by now, and the first menu cue plays before any
	// overlay could have applied the volume.
	ApplyAudio();

	// A wheel plugged in mid-session has to reach the mapping context: its
	// bindings name device slots, and the steering default is whichever
	// wheelbase is attached.
	if (FApexSimInputModule* Input = FApexSimInputModule::Get())
	{
		DevicesChangedHandle = Input->OnDevicesChanged().AddUObject(this, &UApexSettingsSubsystem::HandleInputDevicesChanged);
	}
}

void UApexSettingsSubsystem::Deinitialize()
{
	if (DevicesChangedHandle.IsValid())
	{
		if (FApexSimInputModule* Input = FApexSimInputModule::Get())
		{
			Input->OnDevicesChanged().Remove(DevicesChangedHandle);
		}
		DevicesChangedHandle.Reset();
	}

	Save();
	Super::Deinitialize();
}

void UApexSettingsSubsystem::HandleInputDevicesChanged()
{
	// Not a change the player made: no change count, and nothing to save.
	ApplyControls();
	OnInputDevicesChanged.Broadcast();
	OnSettingsChanged.Broadcast(EApexSettingsGroup::Wheel);
}

void UApexSettingsSubsystem::Load()
{
	if (UGameplayStatics::DoesSaveGameExist(UApexSettingsSave::SlotName, 0))
	{
		Settings = Cast<UApexSettingsSave>(
			UGameplayStatics::LoadGameFromSlot(UApexSettingsSave::SlotName, 0));
	}

	if (!Settings)
	{
		Settings = Cast<UApexSettingsSave>(
			UGameplayStatics::CreateSaveGameObject(UApexSettingsSave::StaticClass()));
	}

	UApexBootSettingsSubsystem* Boot = GetBoot();
	if (!Settings || !Boot)
	{
		return;
	}

	if (Boot->DidFileExist())
	{
		// The file the player can edit wins: it exists precisely so that a game
		// which opens at an unusable resolution can be fixed from outside it.
		const FApexBootSettings& Values = Boot->Get();
		Settings->Resolution = Values.Resolution;
		Settings->DisplayMode = Values.WindowMode;
		Settings->bVSync = Values.bVSync;
		Settings->FrameLimit = Values.FrameLimit;
	}
	else
	{
		// The file was created this run. Seeding it from the slot means an
		// existing install keeps the display it was already using rather than
		// being reset by a file it never had. On a genuine first run the slot
		// holds its own defaults, and the file already describes this display.
		if (!UGameplayStatics::DoesSaveGameExist(UApexSettingsSave::SlotName, 0))
		{
			const FApexBootSettings& Values = Boot->Get();
			Settings->Resolution = Values.Resolution;
			Settings->DisplayMode = Values.WindowMode;
		}
		PushToBootSettings();
	}
}

void UApexSettingsSubsystem::Save()
{
	if (Settings)
	{
		UGameplayStatics::SaveGameToSlot(Settings, UApexSettingsSave::SlotName, 0);
		// Same moment, both stores: a display change made in the overlay has to
		// reach settings.yml, or the next launch would silently undo it.
		PushToBootSettings();
	}
}

UApexBootSettingsSubsystem* UApexSettingsSubsystem::GetBoot() const
{
	const UGameInstance* GameInstance = GetGameInstance();
	return GameInstance ? GameInstance->GetSubsystem<UApexBootSettingsSubsystem>() : nullptr;
}

void UApexSettingsSubsystem::PushToBootSettings()
{
	if (UApexBootSettingsSubsystem* Boot = GetBoot())
	{
		Boot->SetDisplay(Settings->Resolution, Settings->DisplayMode, Settings->bVSync, Settings->FrameLimit);
	}
}

void UApexSettingsSubsystem::Changed(EApexSettingsGroup Group)
{
	if (bApplyingPreset)
	{
		return;
	}

	++ChangeCount;
	ApplyGroup(Group);
	OnSettingsChanged.Broadcast(Group);
}

void UApexSettingsSubsystem::ApplyGroup(EApexSettingsGroup Group)
{
	switch (Group)
	{
	case EApexSettingsGroup::Gameplay: ApplyGameplay(); break;
	case EApexSettingsGroup::Assists:  ApplyAssists();  break;
	case EApexSettingsGroup::Graphics: ApplyGraphics(); break;
	case EApexSettingsGroup::Camera:   ApplyCamera();   break;
	case EApexSettingsGroup::Controls: ApplyControls(); break;
	// The wheel's forces are read every frame by the player controller, and its
	// bindings go through the same mapping context as everything else.
	case EApexSettingsGroup::Wheel:    ApplyControls(); break;
	case EApexSettingsGroup::Audio:    ApplyAudio();    break;
	// Nothing local: the setup exists only on the server, which the root
	// widget forwards it to (UApexRootWidget::SendCarSetup).
	case EApexSettingsGroup::CarSetup: break;
	}
}

// --- Gameplay ---------------------------------------------------------------

// --- Assists ----------------------------------------------------------------
//
// Every one of these is a server-side aid (UApexRootWidget::SendDriverAids
// forwards the group) except the racing line, which the server builds and the
// client draws. The session's allowed set is applied by the server, so a
// locked aid is still stored here — it comes back when the player is in a
// session that allows it.

void UApexSettingsSubsystem::SetTractionControl(EApexAssistLevel Level)
{
	if (!Settings || Settings->TractionControl == Level) { return; }
	Settings->TractionControl = Level;
	Changed(EApexSettingsGroup::Assists);
}

void UApexSettingsSubsystem::SetAbs(bool bEnabled)
{
	if (!Settings || Settings->bAbs == bEnabled) { return; }
	Settings->bAbs = bEnabled;
	Changed(EApexSettingsGroup::Assists);
}

void UApexSettingsSubsystem::SetAutoGearbox(bool bAuto)
{
	if (!Settings || Settings->bAutoGearbox == bAuto) { return; }
	Settings->bAutoGearbox = bAuto;
	Changed(EApexSettingsGroup::Assists);
}

void UApexSettingsSubsystem::SetSteeringAssist(bool bAssist)
{
	if (!Settings || Settings->bSteeringAssist == bAssist) { return; }
	Settings->bSteeringAssist = bAssist;
	Changed(EApexSettingsGroup::Assists);
}

void UApexSettingsSubsystem::SetRacingLine(EApexRacingLine Line)
{
	if (!Settings || Settings->RacingLine == Line) { return; }
	Settings->RacingLine = Line;
	Changed(EApexSettingsGroup::Assists);
}

void UApexSettingsSubsystem::SetGhostCar(bool bOn)
{
	if (!Settings || Settings->bGhostCar == bOn) { return; }
	Settings->bGhostCar = bOn;
	Changed(EApexSettingsGroup::Gameplay);
}

// --- Gameplay ---------------------------------------------------------------

void UApexSettingsSubsystem::SetAiSkill(float Skill01)
{
	const float Clamped = FMath::Clamp(Skill01, 0.0f, 1.0f);
	if (!Settings || FMath::IsNearlyEqual(Settings->AiSkill, Clamped)) { return; }
	Settings->AiSkill = Clamped;
	Changed(EApexSettingsGroup::Gameplay);
}

void UApexSettingsSubsystem::SetUnits(EApexUnits InUnits)
{
	if (!Settings || Settings->Units == InUnits) { return; }
	Settings->Units = InUnits;
	Changed(EApexSettingsGroup::Gameplay);
}

void UApexSettingsSubsystem::SetHudDetail(EApexHudDetail Detail)
{
	if (!Settings || Settings->HudDetail == Detail) { return; }
	Settings->HudDetail = Detail;
	Changed(EApexSettingsGroup::Gameplay);
}

void UApexSettingsSubsystem::ApplyGameplay()
{
	if (!Settings)
	{
		return;
	}

	// The minimap is drawn from TrackConfigSummary.Centerline, which the codec
	// skips by default because parsing ~14k points twice a minute is not free.
	// Turning it on here means the cost is paid only when a minimap will use it.
	if (IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(TEXT("apexsim.net.ParseCenterline")))
	{
		CVar->Set(Settings->HudDetail == EApexHudDetail::All ? 1 : 0, ECVF_SetByGameSetting);
	}
}

void UApexSettingsSubsystem::ApplyAssists()
{
	if (!Settings)
	{
		return;
	}

	// The racing line only exists while racing; the director reads the
	// setting again when the next race starts.
	if (AApexRaceDirector* Director = AApexRaceDirector::Find(this))
	{
		Director->ApplyRacingLineSetting();
	}
}

// --- Graphics ---------------------------------------------------------------

void UApexSettingsSubsystem::SetGraphicsPreset(EApexGraphicsPreset Preset)
{
	if (!Settings)
	{
		return;
	}

	Settings->Preset = Preset;

	const int32 Bucket = PresetBucket(Preset);
	if (Bucket >= 0)
	{
		// One change, not four: the rows are moving because the preset moved.
		bApplyingPreset = true;
		Settings->ShadowQuality = Bucket;
		Settings->AntiAliasingQuality = Bucket;
		Settings->TextureQuality = Bucket;
		bApplyingPreset = false;
	}

	Changed(EApexSettingsGroup::Graphics);
}

void UApexSettingsSubsystem::SetDisplayMode(int32 WindowMode)
{
	if (!Settings || Settings->DisplayMode == WindowMode) { return; }
	Settings->DisplayMode = WindowMode;
	Changed(EApexSettingsGroup::Graphics);
}

void UApexSettingsSubsystem::SetResolution(FIntPoint Resolution)
{
	if (!Settings || Settings->Resolution == Resolution) { return; }
	Settings->Resolution = Resolution;
	Changed(EApexSettingsGroup::Graphics);
}

void UApexSettingsSubsystem::SetFrameLimit(int32 Fps)
{
	if (!Settings || Settings->FrameLimit == Fps) { return; }
	Settings->FrameLimit = Fps;
	Changed(EApexSettingsGroup::Graphics);
}

void UApexSettingsSubsystem::SetVSync(bool bEnabled)
{
	if (!Settings || Settings->bVSync == bEnabled) { return; }
	Settings->bVSync = bEnabled;
	Changed(EApexSettingsGroup::Graphics);
}

void UApexSettingsSubsystem::SetShadowQuality(int32 Bucket)
{
	if (!Settings || Settings->ShadowQuality == Bucket) { return; }
	Settings->ShadowQuality = Bucket;
	ReconcilePreset();
	Changed(EApexSettingsGroup::Graphics);
}

void UApexSettingsSubsystem::SetAntiAliasingQuality(int32 Bucket)
{
	if (!Settings || Settings->AntiAliasingQuality == Bucket) { return; }
	Settings->AntiAliasingQuality = Bucket;
	ReconcilePreset();
	Changed(EApexSettingsGroup::Graphics);
}

void UApexSettingsSubsystem::SetTextureQuality(int32 Bucket)
{
	if (!Settings || Settings->TextureQuality == Bucket) { return; }
	Settings->TextureQuality = Bucket;
	ReconcilePreset();
	Changed(EApexSettingsGroup::Graphics);
}

void UApexSettingsSubsystem::SetMotionBlur(float Amount01)
{
	const float Clamped = FMath::Clamp(Amount01, 0.0f, 1.0f);
	if (!Settings || FMath::IsNearlyEqual(Settings->MotionBlur, Clamped)) { return; }
	Settings->MotionBlur = Clamped;
	Changed(EApexSettingsGroup::Graphics);
}

void UApexSettingsSubsystem::ReconcilePreset()
{
	if (!Settings || bApplyingPreset)
	{
		return;
	}

	const int32 Bucket = PresetBucket(Settings->Preset);
	if (Bucket >= 0
		&& (Settings->ShadowQuality != Bucket
			|| Settings->AntiAliasingQuality != Bucket
			|| Settings->TextureQuality != Bucket))
	{
		Settings->Preset = EApexGraphicsPreset::Custom;
	}
}

const TArray<FIntPoint>& UApexSettingsSubsystem::GetAvailableResolutions() const
{
	if (AvailableResolutions.Num() == 0)
	{
		UKismetSystemLibrary::GetSupportedFullscreenResolutions(AvailableResolutions);

		// A headless or software-rendered run reports nothing; offering an empty
		// list would leave the row unusable rather than merely inaccurate.
		if (AvailableResolutions.Num() == 0)
		{
			AvailableResolutions = { FIntPoint(1280, 720), FIntPoint(1920, 1080), FIntPoint(2560, 1440), FIntPoint(3840, 2160) };
		}

		AvailableResolutions.Sort([](const FIntPoint& A, const FIntPoint& B)
		{
			return A.X * A.Y > B.X * B.Y;
		});
	}
	return AvailableResolutions;
}

void UApexSettingsSubsystem::ApplyGraphics()
{
	UGameUserSettings* User = GEngine ? GEngine->GetGameUserSettings() : nullptr;
	if (!Settings || !User)
	{
		return;
	}

	User->SetFullscreenMode(static_cast<EWindowMode::Type>(Settings->DisplayMode));
	User->SetScreenResolution(Settings->Resolution);
	User->SetVSyncEnabled(Settings->bVSync);
	User->SetFrameRateLimit(static_cast<float>(Settings->FrameLimit));
	User->SetShadowQuality(Settings->ShadowQuality);
	User->SetAntiAliasingQuality(Settings->AntiAliasingQuality);
	User->SetTextureQuality(Settings->TextureQuality);

	// bCheckForCommandLineOverrides off: a -ResX on the command line would
	// otherwise silently win over the resolution the player just picked.
	User->ApplySettings(/*bCheckForCommandLineOverrides*/ false);

	// Motion blur has no GameUserSettings row; it is a scalability cvar plus the
	// post-process amount, and the design exposes it as a continuous slider
	// rather than a bucket.
	if (IConsoleVariable* Quality = IConsoleManager::Get().FindConsoleVariable(TEXT("r.MotionBlurQuality")))
	{
		Quality->Set(Settings->MotionBlur > 0.01f ? 4 : 0, ECVF_SetByGameSetting);
	}
	if (IConsoleVariable* Amount = IConsoleManager::Get().FindConsoleVariable(TEXT("r.MotionBlur.Amount")))
	{
		Amount->Set(Settings->MotionBlur, ECVF_SetByGameSetting);
	}
}

// --- Camera -----------------------------------------------------------------

void UApexSettingsSubsystem::SetFieldOfView(float Degrees)
{
	const float Clamped = FMath::Clamp(Degrees, 60.0f, 120.0f);
	if (!Settings || FMath::IsNearlyEqual(Settings->FieldOfView, Clamped)) { return; }
	Settings->FieldOfView = Clamped;
	Changed(EApexSettingsGroup::Camera);
}

void UApexSettingsSubsystem::SetStartInCockpit(bool bCockpit)
{
	if (!Settings || Settings->bStartInCockpit == bCockpit) { return; }
	Settings->bStartInCockpit = bCockpit;
	Changed(EApexSettingsGroup::Camera);
}

void UApexSettingsSubsystem::SetChaseLevel(int32 Level)
{
	// The race director owns the ladder and clamps before it gets here; this
	// only keeps the value, so a change never re-enters the camera code.
	if (!Settings || Settings->ChaseViewLevel == Level) { return; }
	Settings->ChaseViewLevel = Level;
	Changed(EApexSettingsGroup::Camera);
}

void UApexSettingsSubsystem::SetSeatForward(float Cm)
{
	const float Clamped = FMath::Clamp(Cm, -30.0f, 30.0f);
	if (!Settings || FMath::IsNearlyEqual(Settings->SeatForwardCm, Clamped)) { return; }
	Settings->SeatForwardCm = Clamped;
	Changed(EApexSettingsGroup::Camera);
}

void UApexSettingsSubsystem::SetSeatHeight(float Cm)
{
	const float Clamped = FMath::Clamp(Cm, -15.0f, 15.0f);
	if (!Settings || FMath::IsNearlyEqual(Settings->SeatHeightCm, Clamped)) { return; }
	Settings->SeatHeightCm = Clamped;
	Changed(EApexSettingsGroup::Camera);
}

void UApexSettingsSubsystem::SetViewPitch(float Degrees)
{
	const float Clamped = FMath::Clamp(Degrees, -10.0f, 10.0f);
	if (!Settings || FMath::IsNearlyEqual(Settings->ViewPitchDeg, Clamped)) { return; }
	Settings->ViewPitchDeg = Clamped;
	Changed(EApexSettingsGroup::Camera);
}

void UApexSettingsSubsystem::SetHorizonLock(float Value01)
{
	const float Clamped = FMath::Clamp(Value01, 0.0f, 1.0f);
	if (!Settings || FMath::IsNearlyEqual(Settings->HorizonLock, Clamped)) { return; }
	Settings->HorizonLock = Clamped;
	Changed(EApexSettingsGroup::Camera);
}

void UApexSettingsSubsystem::SetHeadMotion(float Value01)
{
	const float Clamped = FMath::Clamp(Value01, 0.0f, 1.0f);
	if (!Settings || FMath::IsNearlyEqual(Settings->HeadMotion, Clamped)) { return; }
	Settings->HeadMotion = Clamped;
	Changed(EApexSettingsGroup::Camera);
}

void UApexSettingsSubsystem::SetLookToApex(float Value01)
{
	const float Clamped = FMath::Clamp(Value01, 0.0f, 1.0f);
	if (!Settings || FMath::IsNearlyEqual(Settings->LookToApex, Clamped)) { return; }
	Settings->LookToApex = Clamped;
	Changed(EApexSettingsGroup::Camera);
}

void UApexSettingsSubsystem::SetCockpitShowCar(bool bShow)
{
	if (!Settings || Settings->bCockpitShowCar == bShow) { return; }
	Settings->bCockpitShowCar = bShow;
	Changed(EApexSettingsGroup::Camera);
}

void UApexSettingsSubsystem::SetCockpitWheel(bool bShow)
{
	if (!Settings || Settings->bCockpitWheel == bShow) { return; }
	Settings->bCockpitWheel = bShow;
	Changed(EApexSettingsGroup::Camera);
}

void UApexSettingsSubsystem::SetCockpitMirrors(bool bShow)
{
	if (!Settings || Settings->bCockpitMirrors == bShow) { return; }
	Settings->bCockpitMirrors = bShow;
	Changed(EApexSettingsGroup::Camera);
}

void UApexSettingsSubsystem::SetVirtualMirror(bool bShow)
{
	if (!Settings || Settings->bVirtualMirror == bShow) { return; }
	Settings->bVirtualMirror = bShow;
	Changed(EApexSettingsGroup::Camera);
}

void UApexSettingsSubsystem::SetMirrorQuality(int32 Bucket)
{
	const int32 Clamped = FMath::Clamp(Bucket, 0, 2);
	if (!Settings || Settings->MirrorQuality == Clamped) { return; }
	Settings->MirrorQuality = Clamped;
	Changed(EApexSettingsGroup::Camera);
}

void UApexSettingsSubsystem::ApplyCamera()
{
	// The cameras only exist while racing; the director reads the whole
	// block again at the start of every race.
	if (AApexRaceDirector* Director = AApexRaceDirector::Find(this))
	{
		Director->ApplyCameraSettings();
	}
}

// --- Controls ---------------------------------------------------------------

void UApexSettingsSubsystem::SetSteeringSensitivity(float Value01)
{
	const float Clamped = FMath::Clamp(Value01, 0.0f, 1.0f);
	if (!Settings || FMath::IsNearlyEqual(Settings->SteeringSensitivity, Clamped)) { return; }
	Settings->SteeringSensitivity = Clamped;
	Changed(EApexSettingsGroup::Controls);
}

void UApexSettingsSubsystem::SetDeadzone(float Value01)
{
	const float Clamped = FMath::Clamp(Value01, 0.0f, 0.5f);
	if (!Settings || FMath::IsNearlyEqual(Settings->Deadzone, Clamped)) { return; }
	Settings->Deadzone = Clamped;
	Changed(EApexSettingsGroup::Controls);
}

void UApexSettingsSubsystem::SetVibration(float Value01)
{
	const float Clamped = FMath::Clamp(Value01, 0.0f, 1.0f);
	if (!Settings || FMath::IsNearlyEqual(Settings->Vibration, Clamped)) { return; }
	Settings->Vibration = Clamped;
	Changed(EApexSettingsGroup::Controls);
}

namespace
{
	/** The binding rules ask this; nothing is attached when the module is not loaded. */
	bool IsDeviceAttached(int32 DeviceSlot)
	{
		const FApexSimInputModule* Input = FApexSimInputModule::Get();
		return Input && Input->IsAttached(DeviceSlot);
	}
}

FKey UApexSettingsSubsystem::GetBoundKey(FName ActionId, int32 Slot) const
{
	if (Settings)
	{
		if (const FApexKeyBinding* Binding = ApexInput::FindBinding(Settings->Bindings, ActionId, Slot, &IsDeviceAttached))
		{
			return Binding->Key;
		}
	}

	if (ApexInput::IsWheelSlot(Slot))
	{
		return ApexInput::GetWheelDefaultKey(ActionId, Slot);
	}

	const ApexInput::FSlotDef* Def = ApexInput::FindSlot(ActionId, Slot);
	return Def ? Def->DefaultKey : FKey();
}

bool UApexSettingsSubsystem::IsBindingDetached(FName ActionId, int32 Slot) const
{
	const FKey Key = GetBoundKey(ActionId, Slot);
	const ApexDirectInput::FControl Control = ApexDirectInput::ParseKey(Key);
	return Control.IsValid() && !IsDeviceAttached(Control.Slot);
}

void UApexSettingsSubsystem::SetBoundKey(FName ActionId, int32 Slot, const FKey& Key, bool bInvert)
{
	if (!Settings)
	{
		return;
	}

	if (!ApexInput::FindSlot(ActionId, Slot))
	{
		UE_LOG(LogApexSim, Warning, TEXT("Rebind of unknown slot %s/%d ignored"), *ActionId.ToString(), Slot);
		return;
	}

	ApexInput::StoreBinding(Settings->Bindings, ActionId, Slot, Key, bInvert, &IsDeviceAttached);
	Changed(ApexInput::IsWheelSlot(Slot) ? EApexSettingsGroup::Wheel : EApexSettingsGroup::Controls);
}

TArray<const ApexInput::FSlotDef*> UApexSettingsSubsystem::FindConflicts(
	const FKey& Key, FName ExceptAction, int32 ExceptSlot) const
{
	TArray<const ApexInput::FSlotDef*> Conflicts;
	if (!Key.IsValid())
	{
		return Conflicts;
	}

	for (const ApexInput::FSlotDef& Def : ApexInput::Slots())
	{
		if (Def.ActionId == ExceptAction && Def.Slot == ExceptSlot)
		{
			continue;
		}
		if (GetBoundKey(Def.ActionId, Def.Slot) == Key)
		{
			Conflicts.Add(&Def);
		}
	}
	return Conflicts;
}

void UApexSettingsSubsystem::ResetBindings()
{
	if (!Settings || Settings->Bindings.Num() == 0)
	{
		return;
	}
	Settings->Bindings.Reset();
	Changed(EApexSettingsGroup::Controls);
}

bool UApexSettingsSubsystem::IsPauseKey(const FKey& Key) const
{
	if (!Key.IsValid())
	{
		return false;
	}
	if (GetBoundKey(ApexInput::Actions::PauseMenu, ApexInput::Slot::Gamepad) == Key
		|| GetBoundKey(ApexInput::Actions::PauseMenu, ApexInput::Slot::Keyboard) == Key)
	{
		return true;
	}

	// Every wheel's pause button, not only the attached one's: the binding that
	// resolves is the attached device's, and this is asked with a key in hand.
	return Settings && Settings->Bindings.ContainsByPredicate([&Key](const FApexKeyBinding& Binding)
	{
		return Binding.ActionId == ApexInput::Actions::PauseMenu
			&& ApexInput::IsWheelSlot(Binding.Slot)
			&& Binding.Key == Key;
	});
}

// --- Wheel ------------------------------------------------------------------

void UApexSettingsSubsystem::SetWheelForce(float Value01)
{
	const float Clamped = FMath::Clamp(Value01, 0.0f, 1.0f);
	if (!Settings || FMath::IsNearlyEqual(Settings->WheelForce, Clamped)) { return; }
	Settings->WheelForce = Clamped;
	Changed(EApexSettingsGroup::Wheel);
}

void UApexSettingsSubsystem::SetWheelRoadEffects(float Value01)
{
	const float Clamped = FMath::Clamp(Value01, 0.0f, 1.0f);
	if (!Settings || FMath::IsNearlyEqual(Settings->WheelRoadEffects, Clamped)) { return; }
	Settings->WheelRoadEffects = Clamped;
	Changed(EApexSettingsGroup::Wheel);
}

void UApexSettingsSubsystem::SetWheelDamping(float Value01)
{
	const float Clamped = FMath::Clamp(Value01, 0.0f, 1.0f);
	if (!Settings || FMath::IsNearlyEqual(Settings->WheelDamping, Clamped)) { return; }
	Settings->WheelDamping = Clamped;
	Changed(EApexSettingsGroup::Wheel);
}

void UApexSettingsSubsystem::SetWheelInvertForce(bool bInvert)
{
	if (!Settings || Settings->bWheelInvertForce == bInvert) { return; }
	Settings->bWheelInvertForce = bInvert;
	Changed(EApexSettingsGroup::Wheel);
}

// --- Car setup --------------------------------------------------------------

void UApexSettingsSubsystem::SetCarSetupClick(int32 Knob, int32 Clicks)
{
	if (!Settings) { return; }
	// A slot from before the field, or a short one, is brought to size here
	// rather than at every read.
	if (Settings->CarSetup.Clicks.Num() != FApexCarSetup::KnobCount)
	{
		Settings->CarSetup.Clamp();
	}
	if (!Settings->CarSetup.SetClick(Knob, Clicks)) { return; }
	Changed(EApexSettingsGroup::CarSetup);
}

int32 UApexSettingsSubsystem::GetWheelDeviceSlot() const
{
	return Settings ? ApexInput::FindForceFeedbackDevice(Settings->Bindings) : INDEX_NONE;
}

float UApexSettingsSubsystem::ShapeSteering(float RawAxis) const
{
	if (!Settings)
	{
		return RawAxis;
	}

	const float Magnitude = FMath::Abs(RawAxis);
	if (Magnitude <= Settings->Deadzone)
	{
		return 0.0f;
	}

	// Rescale so the axis still reaches 1 after the deadzone is cut out —
	// otherwise a large deadzone quietly costs the player full lock.
	const float Rescaled = (Magnitude - Settings->Deadzone) / FMath::Max(KINDA_SMALL_NUMBER, 1.0f - Settings->Deadzone);

	// 0.5 is linear; below it the curve is gentler around centre, above it
	// sharper. An exponent in [0.5, 2] either side of 1 gives that either way.
	const float Exponent = FMath::Lerp(2.0f, 0.5f, Settings->SteeringSensitivity);
	return FMath::Sign(RawAxis) * FMath::Pow(Rescaled, Exponent);
}

void UApexSettingsSubsystem::ApplyControls()
{
	if (AApexPlayerController* PlayerController =
			Cast<AApexPlayerController>(UGameplayStatics::GetPlayerController(this, 0)))
	{
		PlayerController->RebuildBindings();
	}
}

// --- Audio ------------------------------------------------------------------

void UApexSettingsSubsystem::SetMasterVolume(float Value01)
{
	const float Clamped = FMath::Clamp(Value01, 0.0f, 1.0f);
	if (!Settings || FMath::IsNearlyEqual(Settings->MasterVolume, Clamped)) { return; }
	Settings->MasterVolume = Clamped;
	Changed(EApexSettingsGroup::Audio);
}

void UApexSettingsSubsystem::SetUiVolume(float Value01)
{
	const float Clamped = FMath::Clamp(Value01, 0.0f, 1.0f);
	if (!Settings || FMath::IsNearlyEqual(Settings->UiVolume, Clamped)) { return; }
	Settings->UiVolume = Clamped;
	Changed(EApexSettingsGroup::Audio);
}

void UApexSettingsSubsystem::SetEngineVolume(float Value01)
{
	const float Clamped = FMath::Clamp(Value01, 0.0f, 1.0f);
	if (!Settings || FMath::IsNearlyEqual(Settings->EngineVolume, Clamped)) { return; }
	Settings->EngineVolume = Clamped;
	Changed(EApexSettingsGroup::Audio);
}

void UApexSettingsSubsystem::SetOtherCarsVolume(float Value01)
{
	const float Clamped = FMath::Clamp(Value01, 0.0f, 1.0f);
	if (!Settings || FMath::IsNearlyEqual(Settings->OtherCarsVolume, Clamped)) { return; }
	Settings->OtherCarsVolume = Clamped;
	Changed(EApexSettingsGroup::Audio);
}

void UApexSettingsSubsystem::SetRoadVolume(float Value01)
{
	const float Clamped = FMath::Clamp(Value01, 0.0f, 1.0f);
	if (!Settings || FMath::IsNearlyEqual(Settings->RoadVolume, Clamped)) { return; }
	Settings->RoadVolume = Clamped;
	Changed(EApexSettingsGroup::Audio);
}

void UApexSettingsSubsystem::ApplyAudio()
{
	if (!Settings || !GEngine)
	{
		return;
	}

	// The device's transient primary volume is the one knob that scales every
	// sound the game makes without a sound mix or sound class asset to hold
	// it. The call marshals itself to the audio thread. Menu-sound volume is
	// not applied here: UApexUiAudioSubsystem reads it at play time.
	if (FAudioDevice* Device = GEngine->GetMainAudioDeviceRaw())
	{
		Device->SetTransientPrimaryVolume(Settings->MasterVolume);
	}

	// The cars only exist while racing; the director reads the block again
	// for every car it spawns.
	if (AApexRaceDirector* Director = AApexRaceDirector::Find(this))
	{
		Director->ApplyAudioSettings();
	}
}

// --- Defaults ---------------------------------------------------------------

void UApexSettingsSubsystem::ResetToDefaults(EApexSettingsGroup Group)
{
	if (!Settings)
	{
		return;
	}

	const UApexSettingsSave* Defaults = GetDefault<UApexSettingsSave>();

	switch (Group)
	{
	case EApexSettingsGroup::Gameplay:
		Settings->AiSkill = Defaults->AiSkill;
		Settings->Units = Defaults->Units;
		Settings->HudDetail = Defaults->HudDetail;
		Settings->bGhostCar = Defaults->bGhostCar;
		break;

	case EApexSettingsGroup::Assists:
		Settings->TractionControl = Defaults->TractionControl;
		Settings->bAbs = Defaults->bAbs;
		Settings->bAutoGearbox = Defaults->bAutoGearbox;
		Settings->bSteeringAssist = Defaults->bSteeringAssist;
		Settings->RacingLine = Defaults->RacingLine;
		break;

	case EApexSettingsGroup::Graphics:
		Settings->Preset = Defaults->Preset;
		Settings->FrameLimit = Defaults->FrameLimit;
		Settings->bVSync = Defaults->bVSync;
		Settings->ShadowQuality = Defaults->ShadowQuality;
		Settings->AntiAliasingQuality = Defaults->AntiAliasingQuality;
		Settings->TextureQuality = Defaults->TextureQuality;
		Settings->MotionBlur = Defaults->MotionBlur;
		// Display mode and resolution are left alone on purpose: they describe
		// this machine's monitor, not a preference that has a shipped default.
		break;

	case EApexSettingsGroup::Camera:
		Settings->FieldOfView = Defaults->FieldOfView;
		Settings->bStartInCockpit = Defaults->bStartInCockpit;
		Settings->ChaseViewLevel = Defaults->ChaseViewLevel;
		Settings->SeatForwardCm = Defaults->SeatForwardCm;
		Settings->SeatHeightCm = Defaults->SeatHeightCm;
		Settings->ViewPitchDeg = Defaults->ViewPitchDeg;
		Settings->HorizonLock = Defaults->HorizonLock;
		Settings->HeadMotion = Defaults->HeadMotion;
		Settings->LookToApex = Defaults->LookToApex;
		Settings->bCockpitShowCar = Defaults->bCockpitShowCar;
		Settings->bCockpitWheel = Defaults->bCockpitWheel;
		Settings->bCockpitMirrors = Defaults->bCockpitMirrors;
		Settings->bVirtualMirror = Defaults->bVirtualMirror;
		Settings->MirrorQuality = Defaults->MirrorQuality;
		break;

	case EApexSettingsGroup::Controls:
		Settings->SteeringSensitivity = Defaults->SteeringSensitivity;
		Settings->Deadzone = Defaults->Deadzone;
		Settings->Vibration = Defaults->Vibration;
		// Only the pad's and the keyboard's: a wheel user resetting the pad
		// page must not lose the mapping they spent ten minutes on.
		ApexInput::ResetColumn(Settings->Bindings, ApexInput::EColumn::Keyboard);
		break;

	case EApexSettingsGroup::Wheel:
		Settings->WheelForce = Defaults->WheelForce;
		Settings->WheelRoadEffects = Defaults->WheelRoadEffects;
		Settings->WheelDamping = Defaults->WheelDamping;
		Settings->bWheelInvertForce = Defaults->bWheelInvertForce;
		ApexInput::ResetColumn(Settings->Bindings, ApexInput::EColumn::Wheel);
		break;

	case EApexSettingsGroup::Audio:
		Settings->MasterVolume = Defaults->MasterVolume;
		Settings->UiVolume = Defaults->UiVolume;
		Settings->EngineVolume = Defaults->EngineVolume;
		Settings->OtherCarsVolume = Defaults->OtherCarsVolume;
		Settings->RoadVolume = Defaults->RoadVolume;
		break;

	case EApexSettingsGroup::CarSetup:
		Settings->CarSetup = FApexCarSetup();
		break;
	}

	Changed(Group);
}
