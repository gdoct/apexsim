#include "UI/ApexPhotoModeWidget.h"

#include "ApexSim.h"
#include "ApexSpectatorSubsystem.h"
#include "Audio/ApexUiAudioSubsystem.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/HorizontalBox.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HighResScreenshot.h"
#include "Race/ApexRaceDirector.h"
#include "RHI.h"
#include "UI/ApexRootWidget.h"
#include "UI/ApexUIStyle.h"
#include "UObject/UObjectIterator.h"
#include "UnrealClient.h"

using namespace ApexUI;

// Named, not anonymous: this module is unity-built.
namespace ApexPhotoUi
{
	constexpr float CardWidth = 340.0f;
	const FLinearColor CardFill = Palette::Surface.CopyWithNewOpacity(0.86f);
	/** Below this a stick is at rest. */
	constexpr float StickDeadZone = 0.15f;
	/** Degrees of turn per pixel of mouse, with a button held. */
	constexpr float MouseDegPerPixel = 0.12f;

	float StickValue(float Value)
	{
		return FMath::Abs(Value) < StickDeadZone ? 0.0f : Value;
	}

	UApexRootWidget* FindShell(UWorld* World)
	{
		for (TObjectIterator<UApexRootWidget> It; It; ++It)
		{
			if (It->GetWorld() == World && It->GetCachedWidget().IsValid())
			{
				return *It;
			}
		}
		return nullptr;
	}

	bool SettingFromName(const FString& Name, ApexPhoto::ESetting& Out)
	{
		using ApexPhoto::ESetting;
		if (Name == TEXT("focal"))         { Out = ESetting::FocalLength; return true; }
		if (Name == TEXT("focus"))         { Out = ESetting::Focus; return true; }
		if (Name == TEXT("aperture"))      { Out = ESetting::Aperture; return true; }
		if (Name == TEXT("ev"))            { Out = ESetting::Exposure; return true; }
		if (Name == TEXT("res"))           { Out = ESetting::Resolution; return true; }
		return false;
	}

	FAutoConsoleCommandWithWorldAndArgs PhotoCommand(
		TEXT("apexsim.photo"),
		TEXT("Photo mode over a watched race: apexsim.photo [on|off|shot|orbit|hide|reset|pause|info] ")
		TEXT("| move <forward> <right> <up> <seconds> | look <yaw> <pitch> ")
		TEXT("| focal <mm> | focus <m, 0 on the car> | aperture <f-number, 0 off> | ev <stops> | res <1|2|4>"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			UApexRootWidget* Root = FindShell(World);
			if (!Root)
			{
				UE_LOG(LogApexSim, Warning, TEXT("apexsim.photo: no shell in this world"));
				return;
			}
			const FString Verb = Args.Num() > 0 ? Args[0].ToLower() : FString(TEXT("on"));
			if (Verb == TEXT("on"))
			{
				Root->BeginPhotoMode();
				return;
			}
			if (Verb == TEXT("off"))
			{
				Root->EndPhotoMode();
				return;
			}
			UApexPhotoModeWidget* Photo = Root->GetPhotoMode();
			if (!Photo || !Photo->IsActive())
			{
				UE_LOG(LogApexSim, Warning, TEXT("apexsim.photo %s: photo mode is off (apexsim.photo on)"), *Verb);
				return;
			}
			auto Number = [&Args](int32 Index) { return Args.IsValidIndex(Index) ? FCString::Atof(*Args[Index]) : 0.0f; };
			ApexPhoto::ESetting Setting;
			if (Verb == TEXT("shot"))         { Photo->TakeShot(); }
			else if (Verb == TEXT("orbit"))   { Photo->RunCommand(ApexPhoto::ECommand::Orbit); }
			else if (Verb == TEXT("hide"))    { Photo->RunCommand(ApexPhoto::ECommand::HidePanel); }
			else if (Verb == TEXT("reset"))   { Photo->RunCommand(ApexPhoto::ECommand::Reset); }
			else if (Verb == TEXT("pause"))   { Photo->RunCommand(ApexPhoto::ECommand::PlayPause); }
			else if (Verb == TEXT("move"))    { Photo->ScriptMove(FVector(Number(1), Number(2), Number(3)), Args.Num() > 4 ? Number(4) : 1.0f); }
			else if (Verb == TEXT("look"))    { Photo->ScriptLook(Number(1), Number(2)); }
			else if (SettingFromName(Verb, Setting) && Args.Num() > 1) { Photo->SetLensValue(Setting, Number(1)); }
			else if (Verb != TEXT("info"))
			{
				UE_LOG(LogApexSim, Warning, TEXT("apexsim.photo: unknown \"%s\""), *Verb);
				return;
			}
			UE_LOG(LogApexSim, Display, TEXT("apexsim.photo %s: %s"), *Verb, *Photo->Describe());
		}));
}

UApexPhotoModeWidget::UApexPhotoModeWidget(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	SetVisibility(ESlateVisibility::Collapsed);
}

void UApexPhotoModeWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	using namespace ApexPhotoUi;

	UOverlay* Layers = WidgetTree->ConstructWidget<UOverlay>();

	// The chip at the top: what the camera is doing and whether the race is held.
	ModeText = MakeText(*WidgetTree, FString(), Font::Mono(12.0f, 140), Palette::OnAccent);
	UOverlaySlot* ChipSlot = Layers->AddChildToOverlay(MakePanel(*WidgetTree, ModeText, FMargin(14.0f, 7.0f), MakeBrush(Palette::Accent)));
	ChipSlot->SetHorizontalAlignment(HAlign_Center);
	ChipSlot->SetVerticalAlignment(VAlign_Top);
	ChipSlot->SetPadding(FMargin(0.0f, 36.0f, 0.0f, 0.0f));

	// The lens on the right, clear of the middle of the picture.
	UOverlaySlot* CardSlot = Layers->AddChildToOverlay(BuildCard());
	CardSlot->SetHorizontalAlignment(HAlign_Right);
	CardSlot->SetVerticalAlignment(VAlign_Center);
	CardSlot->SetPadding(FMargin(0.0f, 0.0f, Metrics::PageGutter, 0.0f));

	// The keys along the bottom.
	HintText = MakeText(*WidgetTree, FString(), Font::Mono(11.0f, 60), Palette::TextSecondary);
	UOverlaySlot* HintSlot = Layers->AddChildToOverlay(MakePanel(*WidgetTree, HintText, FMargin(16.0f, 9.0f), MakeBrush(CardFill)));
	HintSlot->SetHorizontalAlignment(HAlign_Center);
	HintSlot->SetVerticalAlignment(VAlign_Bottom);
	HintSlot->SetPadding(FMargin(0.0f, 0.0f, 0.0f, 30.0f));

	Layers->SetVisibility(ESlateVisibility::HitTestInvisible);
	Layer = Layers;
	WidgetTree->RootWidget = Layers;
}

UWidget* UApexPhotoModeWidget::BuildCard()
{
	using namespace ApexPhotoUi;
	UVerticalBox* Body = WidgetTree->ConstructWidget<UVerticalBox>();
	AddV(Body, MakeLabel(*WidgetTree, TEXT("Photo mode"), Palette::Accent), FMargin(4.0f, 0.0f, 0.0f, 12.0f));
	for (int32 Index = 0; Index < static_cast<int32>(ApexPhoto::ESetting::Count); ++Index)
	{
		const ApexPhoto::ESetting Setting = static_cast<ApexPhoto::ESetting>(Index);
		UHorizontalBox* RowBox = WidgetTree->ConstructWidget<UHorizontalBox>();
		UTextBlock* Label = MakeText(*WidgetTree, ApexPhoto::SettingName(Setting), Font::Mono(11.0f, 120), Palette::TextMuted);
		UTextBlock* Value = MakeText(*WidgetTree, FString(), Font::Display(20.0f), Palette::TextPrimary);
		AddH(RowBox, Label, FMargin(), VAlign_Center, 1.0f);
		AddH(RowBox, Value, FMargin(12.0f, 0.0f, 0.0f, 0.0f), VAlign_Center);
		UBorder* Panel = MakePanel(*WidgetTree, RowBox, FMargin(12.0f, 8.0f), MakeBrush(FLinearColor::Transparent));
		AddV(Body, Panel, FMargin(0.0f, Index == 0 ? 0.0f : 2.0f, 0.0f, 0.0f));
		RowPanels.Add(Panel);
		RowLabels.Add(Label);
		RowValues.Add(Value);
	}
	return MakeSized(*WidgetTree, MakePanel(*WidgetTree, Body, FMargin(18.0f, 18.0f), MakeBrush(CardFill, Palette::Border, 1.0f)),
		CardWidth, -1.0f);
}

bool UApexPhotoModeWidget::Begin()
{
	AApexRaceDirector* Director = AApexRaceDirector::Find(this);
	if (bActive)
	{
		return true;
	}
	if (!Director || !Director->BeginPhotoMode())
	{
		return false;
	}
	FVector Location;
	FRotator Rotation;
	float Fov = 70.0f;
	Director->GetViewPose(Location, Rotation, Fov);
	Camera = ApexPhoto::FreeAt(Location, Rotation);
	StartCamera = Camera;
	Lens = ApexPhoto::FLens();
	// The lens the view had, so nothing jumps; the first step lands on the panel's steps.
	Lens.FocalLengthMm = ApexPhoto::FocalLengthForFov(Fov);
	Row = ApexPhoto::ESetting::FocalLength;
	HeldKeys.Reset();
	Axes.Reset();
	PendingLookDeg = FVector2D::ZeroVector;
	ScriptedSeconds = 0.0f;
	bPanelHidden = false;
	bActive = true;
	ShownRow = -1;
	ShownState.Reset();
	SetVisibility(ESlateVisibility::HitTestInvisible);
	RefreshPanel();
	return true;
}

void UApexPhotoModeWidget::End()
{
	if (!bActive)
	{
		return;
	}
	bActive = false;
	HeldKeys.Reset();
	Axes.Reset();
	if (AApexRaceDirector* Director = AApexRaceDirector::Find(this))
	{
		Director->EndPhotoMode();
	}
	SetVisibility(ESlateVisibility::Collapsed);
}

void UApexPhotoModeWidget::HandleKeyDown(const FKeyEvent& InKeyEvent)
{
	const FKey Key = InKeyEvent.GetKey();
	FVector Move;
	FVector2D Look;
	if (ApexPhoto::HeldAxisFor(Key, Move, Look) || Key == EKeys::LeftShift || Key == EKeys::RightShift
		|| Key == EKeys::LeftControl || Key == EKeys::RightControl || Key == EKeys::Gamepad_LeftThumbstick)
	{
		HeldKeys.Add(Key);
		return;
	}
	if (InKeyEvent.IsRepeat())
	{
		// A held arrow steps a value on, as a menu's does; the rest act once.
		const ApexPhoto::ECommand Command = ApexPhoto::CommandFor(Key);
		if (Command == ApexPhoto::ECommand::Decrease || Command == ApexPhoto::ECommand::Increase)
		{
			RunCommand(Command);
		}
		return;
	}
	RunCommand(ApexPhoto::CommandFor(Key));
}

void UApexPhotoModeWidget::HandleKeyUp(const FKeyEvent& InKeyEvent)
{
	HeldKeys.Remove(InKeyEvent.GetKey());
}

void UApexPhotoModeWidget::HandleAnalog(const FKey& Key, float Value)
{
	Axes.Add(Key, Value);
}

void UApexPhotoModeWidget::HandleMouseMove(const FVector2D& DeltaPixels, bool bButtonHeld)
{
	if (bButtonHeld)
	{
		// Screen y grows downwards: a pull back tilts the view down, as a hand on a tripod.
		PendingLookDeg += FVector2D(DeltaPixels.X, -DeltaPixels.Y) * ApexPhotoUi::MouseDegPerPixel;
	}
}

void UApexPhotoModeWidget::HandleMouseWheel(float Delta)
{
	if (Delta != 0.0f)
	{
		RunCommand(Delta > 0.0f ? ApexPhoto::ECommand::ZoomIn : ApexPhoto::ECommand::ZoomOut);
	}
}

void UApexPhotoModeWidget::RunCommand(ApexPhoto::ECommand Command)
{
	using ApexPhoto::ECommand;
	if (!bActive)
	{
		return;
	}
	UApexRootWidget* Root = Shell.Get();
	switch (Command)
	{
	case ECommand::TakeShot:
		TakeShot();
		break;
	case ECommand::Exit:
		if (Root)
		{
			Root->EndPhotoMode();
		}
		else
		{
			End();
		}
		break;
	case ECommand::Orbit:
	{
		FVector Pivot = Camera.LocationCm;
		const AApexRaceDirector* Director = AApexRaceDirector::Find(this);
		if (!Director || !Director->GetPhotoSubject(Pivot))
		{
			ApexUiAudio::Play(this, EApexUiSound::Denied);
			break;
		}
		ApexPhoto::SetOrbit(Camera, !Camera.bOrbit, Pivot);
		ApexUiAudio::Play(this, EApexUiSound::Adjust);
		break;
	}
	case ECommand::HidePanel:
		bPanelHidden = !bPanelHidden;
		ShownState.Reset();
		ApexUiAudio::Play(this, EApexUiSound::Adjust);
		break;
	case ECommand::PreviousRow:
	case ECommand::NextRow:
		Row = ApexPhoto::StepRow(Row, Command == ECommand::NextRow ? 1 : -1);
		ApexUiAudio::Play(this, EApexUiSound::Move);
		break;
	case ECommand::Decrease:
	case ECommand::Increase:
		ApexPhoto::StepSetting(Lens, Row, Command == ECommand::Increase ? 1 : -1);
		ApexUiAudio::Play(this, EApexUiSound::Adjust);
		break;
	case ECommand::ZoomOut:
	case ECommand::ZoomIn:
		ApexPhoto::StepSetting(Lens, ApexPhoto::ESetting::FocalLength, Command == ECommand::ZoomIn ? 1 : -1);
		ApexUiAudio::Play(this, EApexUiSound::Adjust);
		break;
	case ECommand::PlayPause:
		if (Root)
		{
			ApexSpectate::FCommand Pause;
			Pause.Action = ApexSpectate::EAction::PlayPause;
			Root->RunWatchCommand(Pause);
		}
		break;
	case ECommand::Reset:
		Camera = StartCamera;
		ApexUiAudio::Play(this, EApexUiSound::Adjust);
		break;
	default:
		break;
	}
	RefreshPanel();
}

void UApexPhotoModeWidget::ScriptMove(const FVector& Move, float Seconds)
{
	ScriptedMove = Move.BoundToCube(1.0f);
	ScriptedSeconds = FMath::Max(Seconds, 0.0f);
}

void UApexPhotoModeWidget::ScriptLook(float YawDeg, float PitchDeg)
{
	PendingLookDeg += FVector2D(YawDeg, PitchDeg);
}

void UApexPhotoModeWidget::SetLensValue(ApexPhoto::ESetting Setting, float Value)
{
	using ApexPhoto::ESetting;
	switch (Setting)
	{
	case ESetting::FocalLength: Lens.FocalLengthMm = FMath::Clamp(Value, ApexPhoto::MinFocalLengthMm, ApexPhoto::MaxFocalLengthMm); break;
	case ESetting::Focus:       Lens.FocusM = FMath::Max(Value, 0.0f); break;
	case ESetting::Aperture:    Lens.Fstop = Value <= 0.0f ? 0.0f : FMath::Clamp(Value, 1.2f, 32.0f); break;
	case ESetting::Exposure:    Lens.ExposureEv = FMath::Clamp(Value, ApexPhoto::MinExposureEv, ApexPhoto::MaxExposureEv); break;
	case ESetting::Resolution:  Lens.Resolution = Value >= 4.0f ? 4 : Value >= 2.0f ? 2 : 1; break;
	default: break;
	}
	Row = Setting;
	RefreshPanel();
}

ApexPhoto::FInput UApexPhotoModeWidget::GatherInput(float DeltaSeconds)
{
	using namespace ApexPhotoUi;
	ApexPhoto::FInput Input;
	for (const FKey& Key : HeldKeys)
	{
		FVector Move;
		FVector2D Look;
		if (ApexPhoto::HeldAxisFor(Key, Move, Look))
		{
			Input.Move += Move;
			Input.Look += Look;
		}
	}
	Input.bFast = HeldKeys.Contains(EKeys::LeftShift) || HeldKeys.Contains(EKeys::RightShift)
		|| HeldKeys.Contains(EKeys::Gamepad_LeftThumbstick);
	Input.bSlow = HeldKeys.Contains(EKeys::LeftControl) || HeldKeys.Contains(EKeys::RightControl);

	auto Axis = [this](const FKey& Key) { const float* Value = Axes.Find(Key); return Value ? StickValue(*Value) : 0.0f; };
	Input.Move.X += Axis(EKeys::Gamepad_LeftY);
	Input.Move.Y += Axis(EKeys::Gamepad_LeftX);
	Input.Move.Z += Axis(EKeys::Gamepad_RightTriggerAxis) - Axis(EKeys::Gamepad_LeftTriggerAxis);
	Input.Look.X += Axis(EKeys::Gamepad_RightX);
	Input.Look.Y += Axis(EKeys::Gamepad_RightY);

	if (ScriptedSeconds > 0.0f)
	{
		Input.Move += ScriptedMove;
		ScriptedSeconds -= DeltaSeconds;
	}
	Input.LookDeltaDeg = PendingLookDeg;
	PendingLookDeg = FVector2D::ZeroVector;
	return Input;
}

void UApexPhotoModeWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);
	if (!bActive)
	{
		return;
	}
	AApexRaceDirector* Director = AApexRaceDirector::Find(this);
	if (!Director || !Director->IsPhotoModeActive())
	{
		// The race went (the replay ended and was left, the view changed): out with it.
		if (UApexRootWidget* Root = Shell.Get())
		{
			Root->EndPhotoMode();
		}
		else
		{
			End();
		}
		return;
	}

	FVector Pivot = Camera.LocationCm;
	const bool bSubject = Director->GetPhotoSubject(Pivot);
	if (Camera.bOrbit && !bSubject)
	{
		ApexPhoto::SetOrbit(Camera, false, Pivot);
	}
	ApexPhoto::Advance(Camera, GatherInput(InDeltaTime), InDeltaTime, Pivot);
	double GroundZ = 0.0;
	if (Director->FindGroundBelow(Camera.LocationCm + FVector(0.0, 0.0, 200.0), GroundZ))
	{
		ApexPhoto::KeepAboveGround(Camera, GroundZ);
	}

	// Focus on the car (the middle of its body) unless a distance is set.
	const float FocusCm = Lens.FocusM > 0.0f ? Lens.FocusM * 100.0f
		: bSubject ? static_cast<float>(FVector::Dist(Camera.LocationCm, Pivot)) : 0.0f;
	Director->SetPhotoCamera(Camera.LocationCm, ApexPhoto::Rotation(Camera), ApexPhoto::FovForFocalLength(Lens.FocalLengthMm),
		FocusCm, Lens.Fstop, Lens.ExposureEv);
	RefreshPanel();
}

void UApexPhotoModeWidget::RefreshPanel()
{
	if (!Layer)
	{
		return;
	}
	Layer->SetVisibility(bPanelHidden ? ESlateVisibility::Collapsed : ESlateVisibility::HitTestInvisible);
	if (bPanelHidden)
	{
		return;
	}

	for (int32 Index = 0; Index < RowValues.Num(); ++Index)
	{
		RowValues[Index]->SetText(FText::FromString(ApexPhoto::DescribeSetting(Lens, static_cast<ApexPhoto::ESetting>(Index))));
	}
	const int32 Picked = static_cast<int32>(Row);
	if (Picked != ShownRow)
	{
		ShownRow = Picked;
		for (int32 Index = 0; Index < RowPanels.Num(); ++Index)
		{
			const bool bPicked = Index == Picked;
			RowPanels[Index]->SetBrush(MakeBrush(bPicked ? Palette::Accent.CopyWithNewOpacity(0.22f) : FLinearColor::Transparent,
				bPicked ? Palette::Accent : FLinearColor::Transparent, bPicked ? 1.0f : 0.0f));
			RowLabels[Index]->SetColorAndOpacity(FSlateColor(bPicked ? Palette::Accent : Palette::TextMuted));
		}
	}

	const UApexSpectatorSubsystem* Spectator = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexSpectatorSubsystem>() : nullptr;
	const bool bHeld = Spectator && Spectator->IsPlaying() && Spectator->IsPaused();
	const FString State = FString::Printf(TEXT("PHOTO MODE  ·  %s  ·  %s"), Camera.bOrbit ? TEXT("ORBIT") : TEXT("FREE CAMERA"),
		bHeld ? TEXT("HELD") : TEXT("RUNNING"));
	const UApexRootWidget* Root = Shell.Get();
	const bool bGamepad = Root && Root->IsGamepadHints();
	if (State != ShownState || bGamepad != bShownGamepad)
	{
		ShownState = State;
		bShownGamepad = bGamepad;
		ModeText->SetText(FText::FromString(State));
		HintText->SetText(FText::FromString(bGamepad
			? TEXT("L STICK move  ·  R STICK look  ·  LT/RT down/up  ·  LB/RB zoom  ·  D-PAD lens  ·  A shot  ·  X pause  ·  Y orbit  ·  VIEW hide  ·  B back")
			: TEXT("WASD move  ·  Q/E down/up  ·  MOUSE (held) look  ·  WHEEL zoom  ·  ARROWS lens  ·  ENTER shot  ·  SPACE pause  ·  O orbit  ·  H hide  ·  R reset  ·  ESC back")));
	}
}

void UApexPhotoModeWidget::TakeShot()
{
	if (!bActive)
	{
		return;
	}
	UWorld* World = GetWorld();
	UGameViewportClient* ViewportClient = World ? World->GetGameViewport() : nullptr;
	FViewport* Viewport = ViewportClient ? ViewportClient->Viewport : nullptr;
	if (!Viewport)
	{
		return;
	}
	const FIntPoint Size = Viewport->GetSizeXY();
	const int32 Multiplier = ApexPhoto::FitMultiplier(Size, Lens.Resolution, static_cast<int32>(GetMax2DTextureDimension()));
	const AApexRaceDirector* Director = AApexRaceDirector::Find(this);
	const FString Folder = ApexPhoto::ShotFolder();
	IFileManager::Get().MakeDirectory(*Folder, /*Tree*/ true);
	const FString Base = ApexPhoto::MakeShotName(FDateTime::Now(), Director ? Director->GetShotTrackStem() : FString(), Multiplier);
	// Two in one second: the second gets a number.
	FString Name = Base;
	for (int32 Copy = 2; IFileManager::Get().FileExists(*(Folder / Name + TEXT(".png"))); ++Copy)
	{
		Name = FString::Printf(TEXT("%s_%d"), *Base, Copy);
	}
	const FString Path = Folder / Name;

	// The engine's high-resolution shot: the scene alone (no HUD, no panel)
	// rendered at the multiple and written as a PNG at the next frame.
	FHighResScreenshotConfig& Config = GetHighResScreenshotConfig();
	Config.SetHDRCapture(false);
	Config.SetFilename(Path);
	if (!Config.SetResolution(Size.X, Size.Y, static_cast<float>(Multiplier)))
	{
		if (UApexRootWidget* Root = Shell.Get())
		{
			Root->ShowToast(TEXT("The shot is too large for this GPU"), true);
		}
		return;
	}
	UE_LOG(LogApexSim, Log, TEXT("Photo mode: shot %d x %d to %s.png (%s)"), Size.X * Multiplier, Size.Y * Multiplier, *Path, *Describe());
	if (UApexRootWidget* Root = Shell.Get())
	{
		Root->ShowToast(FString::Printf(TEXT("Saved %s.png  ·  %d x %d  ·  Saved/Screenshots/ApexSim"), *Name,
			Size.X * Multiplier, Size.Y * Multiplier));
	}
}

FString UApexPhotoModeWidget::Describe() const
{
	return FString::Printf(TEXT("%s at (%.0f, %.0f, %.0f) cm, yaw %.1f, pitch %.1f; %s, focus %s, %s, %s, %s"),
		Camera.bOrbit ? TEXT("orbit") : TEXT("free"), Camera.LocationCm.X, Camera.LocationCm.Y, Camera.LocationCm.Z,
		Camera.YawDeg, Camera.PitchDeg,
		*ApexPhoto::DescribeSetting(Lens, ApexPhoto::ESetting::FocalLength),
		*ApexPhoto::DescribeSetting(Lens, ApexPhoto::ESetting::Focus),
		*ApexPhoto::DescribeSetting(Lens, ApexPhoto::ESetting::Aperture),
		*ApexPhoto::DescribeSetting(Lens, ApexPhoto::ESetting::Exposure),
		*ApexPhoto::DescribeSetting(Lens, ApexPhoto::ESetting::Resolution));
}
