#include "ApexSettingsSubsystem.h"

#include "ApexPlayerController.h"
#include "ApexSim.h"
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

	Load();

	ApplyGraphics();
	// Gameplay too, because one of its effects is a console variable rather than
	// a widget: the minimap needs centerline parsing switched on before the first
	// LobbyState arrives, which is well before any HUD exists to ask for it.
	// Controls are left out — the player controller does not exist yet, and it
	// reads the bindings itself when it builds its input config.
	ApplyGameplay();
}

void UApexSettingsSubsystem::Deinitialize()
{
	Save();
	Super::Deinitialize();
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

		// A first run has no stored resolution, and 1920x1080 is a guess that is
		// wrong on most machines. The desktop's own mode is the only sane default.
		if (Settings)
		{
			if (const UGameUserSettings* User = GEngine ? GEngine->GetGameUserSettings() : nullptr)
			{
				Settings->Resolution = User->GetScreenResolution();
				Settings->DisplayMode = static_cast<int32>(User->GetFullscreenMode());
			}
		}
	}
}

void UApexSettingsSubsystem::Save()
{
	if (Settings)
	{
		UGameplayStatics::SaveGameToSlot(Settings, UApexSettingsSave::SlotName, 0);
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
	case EApexSettingsGroup::Graphics: ApplyGraphics(); break;
	case EApexSettingsGroup::Controls: ApplyControls(); break;
	}
}

// --- Gameplay ---------------------------------------------------------------

void UApexSettingsSubsystem::SetTractionControl(EApexAssistLevel Level)
{
	if (!Settings || Settings->TractionControl == Level) { return; }
	Settings->TractionControl = Level;
	Changed(EApexSettingsGroup::Gameplay);
}

void UApexSettingsSubsystem::SetAbs(bool bEnabled)
{
	if (!Settings || Settings->bAbs == bEnabled) { return; }
	Settings->bAbs = bEnabled;
	Changed(EApexSettingsGroup::Gameplay);
}

void UApexSettingsSubsystem::SetAutoGearbox(bool bAuto)
{
	if (!Settings || Settings->bAutoGearbox == bAuto) { return; }
	Settings->bAutoGearbox = bAuto;
	Changed(EApexSettingsGroup::Gameplay);
}

void UApexSettingsSubsystem::SetRacingLine(EApexRacingLine Line)
{
	if (!Settings || Settings->RacingLine == Line) { return; }
	Settings->RacingLine = Line;
	Changed(EApexSettingsGroup::Gameplay);
}

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

void UApexSettingsSubsystem::SetFieldOfView(float Degrees)
{
	const float Clamped = FMath::Clamp(Degrees, 60.0f, 120.0f);
	if (!Settings || FMath::IsNearlyEqual(Settings->FieldOfView, Clamped)) { return; }
	Settings->FieldOfView = Clamped;
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

	if (AApexRaceDirector* Director = AApexRaceDirector::Find(this))
	{
		Director->SetFieldOfView(Settings->FieldOfView);
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

FKey UApexSettingsSubsystem::GetBoundKey(FName ActionId, int32 Slot) const
{
	if (Settings)
	{
		if (const FApexKeyBinding* Binding = Settings->Bindings.FindByPredicate(
				[ActionId, Slot](const FApexKeyBinding& Candidate)
				{
					return Candidate.ActionId == ActionId && Candidate.Slot == Slot;
				}))
		{
			return Binding->Key;
		}
	}

	const ApexInput::FSlotDef* Def = ApexInput::FindSlot(ActionId, Slot);
	return Def ? Def->DefaultKey : FKey();
}

void UApexSettingsSubsystem::SetBoundKey(FName ActionId, int32 Slot, const FKey& Key)
{
	if (!Settings)
	{
		return;
	}

	const ApexInput::FSlotDef* Def = ApexInput::FindSlot(ActionId, Slot);
	if (!Def)
	{
		UE_LOG(LogApexSim, Warning, TEXT("Rebind of unknown slot %s/%d ignored"), *ActionId.ToString(), Slot);
		return;
	}

	FApexKeyBinding* Existing = Settings->Bindings.FindByPredicate(
		[ActionId, Slot](const FApexKeyBinding& Candidate)
		{
			return Candidate.ActionId == ActionId && Candidate.Slot == Slot;
		});

	if (Existing)
	{
		Existing->Key = Key;
	}
	else
	{
		// bNegate comes from the slot, not the key: which half of an axis a slot
		// drives is a property of the control, not of what is bound to it.
		Settings->Bindings.Emplace(ActionId, Slot, Key, Def->bNegate);
	}

	Changed(EApexSettingsGroup::Controls);
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
	return Key.IsValid()
		&& (GetBoundKey(ApexInput::Actions::PauseMenu, 0) == Key
			|| GetBoundKey(ApexInput::Actions::PauseMenu, 1) == Key);
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
		Settings->TractionControl = Defaults->TractionControl;
		Settings->bAbs = Defaults->bAbs;
		Settings->bAutoGearbox = Defaults->bAutoGearbox;
		Settings->RacingLine = Defaults->RacingLine;
		Settings->AiSkill = Defaults->AiSkill;
		Settings->Units = Defaults->Units;
		Settings->HudDetail = Defaults->HudDetail;
		break;

	case EApexSettingsGroup::Graphics:
		Settings->Preset = Defaults->Preset;
		Settings->FrameLimit = Defaults->FrameLimit;
		Settings->bVSync = Defaults->bVSync;
		Settings->ShadowQuality = Defaults->ShadowQuality;
		Settings->AntiAliasingQuality = Defaults->AntiAliasingQuality;
		Settings->TextureQuality = Defaults->TextureQuality;
		Settings->MotionBlur = Defaults->MotionBlur;
		Settings->FieldOfView = Defaults->FieldOfView;
		// Display mode and resolution are left alone on purpose: they describe
		// this machine's monitor, not a preference that has a shipped default.
		break;

	case EApexSettingsGroup::Controls:
		Settings->SteeringSensitivity = Defaults->SteeringSensitivity;
		Settings->Deadzone = Defaults->Deadzone;
		Settings->Vibration = Defaults->Vibration;
		Settings->Bindings.Reset();
		break;
	}

	Changed(Group);
}
