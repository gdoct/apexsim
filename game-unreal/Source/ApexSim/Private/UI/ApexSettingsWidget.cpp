#include "UI/ApexSettingsWidget.h"

#include "Race/ApexChaseView.h"

#include "ApexDirectInputTypes.h"
#include "ApexNetSubsystem.h"
#include "ApexPlayerController.h"
#include "ApexSettingsSave.h"
#include "ApexSim.h"
#include "ApexSimInputModule.h"
#include "ApexWheelProfiles.h"
#include "Audio/ApexUiAudioSubsystem.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/ComboBoxString.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/ProgressBar.h"
#include "Components/ScrollBox.h"
#include "Components/SizeBox.h"
#include "Components/Slider.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/WidgetSwitcher.h"
#include "Engine/GameInstance.h"
#include "Framework/Application/SlateApplication.h"
#include "GenericPlatform/GenericApplication.h"
#include "Input/ApexInputConfig.h"
#include "UI/ApexButtonWidget.h"
#include "UI/ApexRootWidget.h"
#include "UI/ApexSegmentedWidget.h"
#include "UI/ApexStepperWidget.h"
#include "UI/ApexUIStyle.h"

using namespace ApexUI;

// The reset button casts the tab straight to a settings group, so the two
// enums have to stay in step.
static_assert(
	static_cast<int32>(EApexSettingsTab::Gameplay) == static_cast<int32>(EApexSettingsGroup::Gameplay)
		&& static_cast<int32>(EApexSettingsTab::Assists) == static_cast<int32>(EApexSettingsGroup::Assists)
		&& static_cast<int32>(EApexSettingsTab::Graphics) == static_cast<int32>(EApexSettingsGroup::Graphics)
		&& static_cast<int32>(EApexSettingsTab::Camera) == static_cast<int32>(EApexSettingsGroup::Camera)
		&& static_cast<int32>(EApexSettingsTab::Controls) == static_cast<int32>(EApexSettingsGroup::Controls)
		&& static_cast<int32>(EApexSettingsTab::Wheel) == static_cast<int32>(EApexSettingsGroup::Wheel)
		&& static_cast<int32>(EApexSettingsTab::Audio) == static_cast<int32>(EApexSettingsGroup::Audio),
	"EApexSettingsTab and EApexSettingsGroup must stay aligned (the car setup group has no tab: it lives in the hotlap garage)");

namespace
{
	constexpr float PanelWidth = 1370.0f;
	constexpr float RailWidth = 274.0f;
	constexpr float SettingsRowHeight = 68.0f;

	/** Text that wraps inside whatever width its slot gives it, instead of running past it. */
	UTextBlock* Wrapped(UTextBlock* Block)
	{
		Block->SetAutoWrapText(true);
		return Block;
	}
	/** The bindings grid is the tallest column on any page; its rows are cut down to fit. */
	constexpr float BindingRowHeight = 58.0f;
	/** The wheel page's rows sit above devices and three sliders, so they are shorter still. */
	constexpr float WheelRowHeight = 46.0f;
	/**
	 * The wheel bindings' two blocks, driving and menu, as fill weights. The
	 * header row splits by the same weights so the MENU caption sits over its
	 * column; inside the driving block the axes' column (meters) is the wider.
	 */
	constexpr float WheelDrivingWeight = 2.3f;
	constexpr float WheelMenuWeight = 0.75f;
	constexpr float WheelAxesWeight = 1.3f;
	constexpr float WheelColumnGap = 14.0f;

	/**
	 * A fill slot with a weight. ApexUI::AddH only switches a slot to Fill
	 * (every fill weighs 1), so the wheel grid's uneven split is set here.
	 */
	void AddWeighted(UHorizontalBox* Box, UWidget* Child, const FMargin& Padding, EVerticalAlignment VAlign, float Weight)
	{
		if (UHorizontalBoxSlot* Slot = ApexUI::AddH(Box, Child, Padding, VAlign, Weight))
		{
			FSlateChildSize Size(ESlateSizeRule::Fill);
			Size.Value = Weight;
			Slot->SetSize(Size);
		}
	}
	/** Dropdowns and the sliders beside them share a width so the grid lines up. */
	constexpr float DropdownWidth = 250.0f;
	/** Wider, because the AI-skill row has a whole page column to itself. */
	constexpr float SliderCellWidth = 400.0f;

	// Rail and footer actions.
	const FName ActionTabGameplay = TEXT("Tab.Gameplay");
	const FName ActionTabAssists  = TEXT("Tab.Assists");
	const FName ActionTabGraphics = TEXT("Tab.Graphics");
	const FName ActionTabCamera   = TEXT("Tab.Camera");
	const FName ActionTabControls = TEXT("Tab.Controls");
	const FName ActionTabWheel    = TEXT("Tab.Wheel");
	const FName ActionTabAudio    = TEXT("Tab.Audio");
	/** For wrapping the page cycle; the last tab is the count minus one. */
	constexpr int32 TabCount = static_cast<int32>(EApexSettingsTab::Audio) + 1;
	const FName ActionSettingsBack        = TEXT("Back");
	const FName ActionReset       = TEXT("Reset");
	const FName ActionEditHud     = TEXT("EditHud");

	// Segmented-control ids.
	const FName SegTraction   = TEXT("Traction");
	const FName SegAbs        = TEXT("Abs");
	const FName SegGearbox    = TEXT("Gearbox");
	const FName SegSteering   = TEXT("Steering");
	const FName SegRacingLine = TEXT("RacingLine");
	const FName SegAssistPreset = TEXT("AssistPreset");

	/** One assist preset: what each of the five assists is set to. */
	struct FAssistPreset
	{
		EApexAssistLevel Traction;
		bool bAbs;
		bool bAutoGearbox;
		bool bSteeringAssist;
		EApexRacingLine RacingLine;
	};

	/** Beginner, Pro, Elite: everything on, braking aids only, nothing. */
	const FAssistPreset AssistPresets[] = {
		{ EApexAssistLevel::High, true,  true,  true,  EApexRacingLine::Full },
		{ EApexAssistLevel::Low,  true,  false, true,  EApexRacingLine::BrakingOnly },
		{ EApexAssistLevel::Off,  false, false, false, EApexRacingLine::Off },
	};
	constexpr int32 AssistPresetCustom = 3;

	/** The AI skill slider's steps: the mixed field, then one per level 70-110. */
	constexpr float AiSkillSteps = static_cast<float>(ApexAiSkill::Max - ApexAiSkill::Min + 1);

	int32 AiSkillStep(int32 Level)
	{
		return Level == ApexAiSkill::Mixed ? 0 : Level - ApexAiSkill::Min + 1;
	}

	FString AiSkillText(int32 Level)
	{
		return Level == ApexAiSkill::Mixed
			? FString(TEXT("MIX"))
			: FString::Printf(TEXT("%d  %s"), Level, ApexAiSkill::Label(Level));
	}
	const FName SegUnits      = TEXT("Units");
	const FName SegHud        = TEXT("Hud");
	const FName SegPreset     = TEXT("Preset");
	const FName SegVSync      = TEXT("VSync");
	const FName SegScreens    = TEXT("Screens");
	const FName SegStartView  = TEXT("StartView");
	const FName SegChaseView  = TEXT("ChaseView");
	const FName SegCockpitCar = TEXT("CockpitCar");
	const FName SegCockpitWheel   = TEXT("CockpitWheel");
	const FName SegCockpitMirrors = TEXT("CockpitMirrors");
	const FName SegVirtualMirror  = TEXT("VirtualMirror");
	const FName SegMirrorQuality  = TEXT("MirrorQuality");
	const FName SegWheelDirection = TEXT("WheelDirection");
	const FName ActionWheelTest   = TEXT("WheelTest");

	/** How far an axis has to travel from where it started before it is a binding. */
	constexpr float AxisCaptureTravel = 0.5f;

	/** The wheel page's meters: the axes a player has to see to trust a mapping. */
	struct FMeterSpec
	{
		const TCHAR* Label;
		FName ActionId;
		/** A steering axis reads -1..1 and draws from the middle; a pedal reads 0..1. */
		bool bCentred;
	};

	// Camera slider ranges; the sliders themselves run 0..1.
	constexpr float FovMin = 60.0f, FovMax = 120.0f;
	constexpr float SeatForwardRange = 30.0f;
	constexpr float SeatHeightRange = 15.0f;
	constexpr float ViewPitchRange = 10.0f;

	// Wheel steering slider steps, in degrees.
	constexpr float WheelRotationStepDeg = 30.0f;
	constexpr float SteeringLockStepDeg = 20.0f;

	/** A 0..1 slider position as degrees on a range, snapped to its step. */
	float SliderDegrees(float Alpha, float Min, float Max, float Step)
	{
		return FMath::Clamp(Min + FMath::RoundToFloat(Alpha * (Max - Min) / Step) * Step, Min, Max);
	}

	/** The steering lock as the player thinks of it: lock to lock, and how far each way. */
	FString SteeringLockText(float LockDeg, float RotationDeg)
	{
		const float Effective = FMath::Min(LockDeg, RotationDeg);
		return FString::Printf(TEXT("%d° (±%d°)"), FMath::RoundToInt(Effective), FMath::RoundToInt(Effective / 2.0f));
	}

	/**
	 * The steering lock slider's range: one step below the shortest lock is
	 * Auto, the car's own lock (UApexSettingsSave::bWheelSteeringLockAuto).
	 */
	constexpr float SteeringLockSliderMinDeg = ApexInput::SteeringLockMinDeg - SteeringLockStepDeg;

	FString SteeringLockSettingText(bool bAuto, float LockDeg, float RotationDeg)
	{
		return bAuto ? FString(TEXT("Auto (per car)")) : SteeringLockText(LockDeg, RotationDeg);
	}

	FString SignedCm(float Cm)
	{
		const int32 Whole = FMath::RoundToInt(Cm);
		return Whole == 0 ? FString(TEXT("0 cm")) : FString::Printf(TEXT("%+d cm"), Whole);
	}

	FString SignedDegrees(float Degrees)
	{
		const int32 Whole = FMath::RoundToInt(Degrees);
		return Whole == 0 ? FString(TEXT("0°")) : FString::Printf(TEXT("%+d°"), Whole);
	}

	FString Percent(float Value01)
	{
		return FString::Printf(TEXT("%d %%"), FMath::RoundToInt(Value01 * 100.0f));
	}

	FString Centimetres(float Cm)
	{
		return FString::Printf(TEXT("%d cm"), FMath::RoundToInt(Cm));
	}

	FString TenthCentimetres(float Cm)
	{
		return FString::Printf(TEXT("%.1f cm"), Cm);
	}

	FString Degrees(float Deg)
	{
		return FString::Printf(TEXT("%d°"), FMath::RoundToInt(Deg));
	}

	const TArray<FString> QualityNames = { TEXT("LOW"), TEXT("MEDIUM"), TEXT("HIGH"), TEXT("ULTRA") };
	// GameUserSettings exposes anti-aliasing as a quality bucket, not as a choice
	// of method, so the row names buckets rather than promising TAA or TSR.
	const TArray<FString> AntiAliasingNames = QualityNames;
	const TArray<FString> DisplayModeNames = { TEXT("FULLSCREEN"), TEXT("WINDOWED FULLSCREEN"), TEXT("WINDOWED") };
	/** Matching EWindowMode: Fullscreen, WindowedFullscreen, Windowed. */
	const TArray<int32> DisplayModeValues = { 0, 1, 2 };
	const TArray<int32> FrameLimitValues = { 0, 30, 60, 90, 120, 144, 165, 240 };

	FString FrameLimitName(int32 Fps)
	{
		return Fps <= 0 ? FString(TEXT("UNLIMITED")) : FString::Printf(TEXT("%d FPS"), Fps);
	}

	FString ResolutionName(const FIntPoint& Resolution)
	{
		return FString::Printf(TEXT("%d × %d"), Resolution.X, Resolution.Y);
	}

	/** Index of Value in Values, or 0 — a stored value the machine no longer offers. */
	int32 IndexOfOr0(const TArray<int32>& Values, int32 Value)
	{
		const int32 Found = Values.IndexOfByKey(Value);
		return Found == INDEX_NONE ? 0 : Found;
	}

	/** "Throttle|1" — a binding chip's action id has to name both parts of a slot. */
	FName MakeBindingId(FName ActionId, int32 Slot)
	{
		return FName(*FString::Printf(TEXT("%s|%d"), *ActionId.ToString(), Slot));
	}

	bool ParseBindingId(FName BindingId, FName& OutAction, int32& OutSlot)
	{
		FString Left;
		FString Right;
		if (!BindingId.ToString().Split(TEXT("|"), &Left, &Right))
		{
			return false;
		}
		OutAction = FName(*Left);
		OutSlot = FCString::Atoi(*Right);
		return true;
	}
}

UApexSettingsWidget::UApexSettingsWidget(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	SetVisibility(ESlateVisibility::Collapsed);
	SetIsFocusable(true);
}

UApexSettingsSubsystem* UApexSettingsWidget::GetSettings() const
{
	const UGameInstance* GameInstance = GetGameInstance();
	return GameInstance ? GameInstance->GetSubsystem<UApexSettingsSubsystem>() : nullptr;
}

// --- Construction -----------------------------------------------------------

void UApexSettingsWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	BuildOverlay();
}

void UApexSettingsWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);

	// Only the wheel page ticks, and only while it is the one on screen: the
	// meters are live readings and the device cards follow what is plugged in.
	if (!bOpen || CurrentTab != EApexSettingsTab::Wheel)
	{
		return;
	}

	const FApexSimInputModule* Input = FApexSimInputModule::Get();
	if (Input && Input->GetDevicesSerial() != WheelDevicesSerial)
	{
		RefreshWheelDevices();
		RefreshBindingChips();
	}
	RefreshWheelMeters();
}

void UApexSettingsWidget::BuildOverlay()
{
	UVerticalBox* Panel = WidgetTree->ConstructWidget<UVerticalBox>();

	AddV(Panel, BuildHeader());
	AddV(Panel, MakeDivider(*WidgetTree));

	UHorizontalBox* Body = WidgetTree->ConstructWidget<UHorizontalBox>();
	AddH(Body, BuildRail(), FMargin(), VAlign_Fill);
	AddH(Body, MakeDivider(*WidgetTree, /*bVertical*/ true), FMargin(), VAlign_Fill);

	// Every page scrolls: one taller than the panel (the bindings, the wheel's)
	// used to run down behind the footer buttons.
	auto ScrollPage = [this](UWidget* Page)
	{
		UScrollBox* Scroll = WidgetTree->ConstructWidget<UScrollBox>();
		Scroll->SetScrollBarVisibility(ESlateVisibility::Collapsed);
		Scroll->SetConsumeMouseWheel(EConsumeMouseWheel::WhenScrollingPossible);
		Scroll->AddChild(Page);
		return Scroll;
	};

	PageHost = WidgetTree->ConstructWidget<UWidgetSwitcher>();
	PageHost->AddChild(ScrollPage(BuildGameplayPage()));
	PageHost->AddChild(ScrollPage(BuildAssistsPage()));
	PageHost->AddChild(ScrollPage(BuildGraphicsPage()));
	PageHost->AddChild(ScrollPage(BuildCameraPage()));
	PageHost->AddChild(ScrollPage(BuildControlsPage()));
	PageHost->AddChild(ScrollPage(BuildWheelPage()));
	PageHost->AddChild(ScrollPage(BuildAudioPage()));

	// The page area is darker than the card it sits in, so that the rows — which
	// are Surface — read as cards rather than dissolving into the panel.
	AddH(Body,
		MakePanel(*WidgetTree, PageHost, FMargin(34.0f, 28.0f), MakeBrush(Palette::Background)),
		FMargin(), VAlign_Fill, 1.0f);

	AddV(Panel, Body, FMargin(), HAlign_Fill, 1.0f);
	AddV(Panel, MakeDivider(*WidgetTree));
	AddV(Panel, BuildFooter());

	UBorder* Card = MakeModalCard(*WidgetTree, Panel);

	// The listening card sits over the whole panel: while a rebind is running,
	// nothing behind it may be clicked.
	{
		UVerticalBox* Prompt = WidgetTree->ConstructWidget<UVerticalBox>();
		AddV(Prompt, MakeLabel(*WidgetTree, TEXT("Binding"), Palette::Accent), FMargin(), HAlign_Center);
		ListenTitleText = MakeText(*WidgetTree, TEXT("Press any key or move an axis"), Font::Body(24.0f, true), Palette::TextPrimary);
		AddV(Prompt, ListenTitleText, FMargin(0.0f, 12.0f, 0.0f, 0.0f), HAlign_Center);
		AddV(Prompt, MakeLabel(*WidgetTree, TEXT("Esc cancel  ·  Del unbind")), FMargin(0.0f, 14.0f, 0.0f, 0.0f), HAlign_Center);

		UBorder* PromptCard = MakePanel(
			*WidgetTree, Prompt, FMargin(54.0f, 34.0f),
			MakeBrush(Palette::Background, Palette::Accent, 1.0f));

		UOverlay* Layer = WidgetTree->ConstructWidget<UOverlay>();
		FLinearColor Dim = Palette::Background;
		Dim.A = 0.72f;
		UOverlaySlot* DimSlot = Layer->AddChildToOverlay(MakePanel(*WidgetTree, nullptr, FMargin(), MakeBrush(Dim)));
		DimSlot->SetHorizontalAlignment(HAlign_Fill);
		DimSlot->SetVerticalAlignment(VAlign_Fill);
		UOverlaySlot* CardSlot = Layer->AddChildToOverlay(PromptCard);
		CardSlot->SetHorizontalAlignment(HAlign_Center);
		CardSlot->SetVerticalAlignment(VAlign_Center);

		Layer->SetVisibility(ESlateVisibility::Collapsed);
		ListenOverlay = Layer;
	}

	UOverlay* Content = WidgetTree->ConstructWidget<UOverlay>();
	UOverlaySlot* PanelSlot = Content->AddChildToOverlay(MakeSized(*WidgetTree, Card, PanelWidth, -1.0f));
	PanelSlot->SetHorizontalAlignment(HAlign_Center);
	PanelSlot->SetVerticalAlignment(VAlign_Center);
	PanelSlot->SetPadding(FMargin(0.0f, 60.0f));

	UOverlaySlot* ListenSlot = Content->AddChildToOverlay(ListenOverlay);
	ListenSlot->SetHorizontalAlignment(HAlign_Fill);
	ListenSlot->SetVerticalAlignment(VAlign_Fill);

	WidgetTree->RootWidget = MakeScrim(*WidgetTree, Content);
}

UWidget* UApexSettingsWidget::BuildHeader()
{
	UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();

	AddH(Row, MakeText(*WidgetTree, TEXT("SETTINGS"), Font::Display(38.0f, 20), Palette::TextPrimary));

	// The context line changes per page: what a page needs to warn about is
	// different on each, and none of it belongs in the page's own scroll area.
	HeaderContextText = MakeLabel(*WidgetTree, FString(), Palette::TextMuted);
	Wrapped(HeaderContextText);
	AddH(Row, HeaderContextText, FMargin(20.0f, 6.0f, 20.0f, 0.0f), VAlign_Center, 1.0f);

	UApexButtonWidget* Back = WidgetTree->ConstructWidget<UApexButtonWidget>();
	FApexButtonSpec BackSpec;
	BackSpec.Label = TEXT("Back");
	BackSpec.KeyCap = TEXT("ESC");
	BackSpec.Variant = EApexButtonVariant::Bare;
	BackSpec.LabelSize = 13.0f;
	BackSpec.ActionId = ActionSettingsBack;
	Back->Setup(BackSpec);
	Back->OnActivated.AddDynamic(this, &UApexSettingsWidget::HandleFooterActivated);
	AddH(Row, Back);

	UBorder* Bar = MakePanel(*WidgetTree, Row, FMargin(38.0f, 0.0f), MakeBrush(Palette::Surface));
	Bar->SetVerticalAlignment(VAlign_Center);
	return MakeSized(*WidgetTree, Bar, -1.0f, 98.0f);
}

UWidget* UApexSettingsWidget::BuildRail()
{
	UVerticalBox* Stack = WidgetTree->ConstructWidget<UVerticalBox>();

	auto AddTab = [this, Stack](const TCHAR* Label, FName ActionId)
	{
		UApexButtonWidget* Button = WidgetTree->ConstructWidget<UApexButtonWidget>();
		FApexButtonSpec Spec;
		Spec.Label = Label;
		Spec.Variant = EApexButtonVariant::Panel;
		Spec.ActionId = ActionId;
		Spec.Height = 62.0f;
		Spec.LabelSize = 21.0f;
		Button->Setup(Spec);
		Button->OnActivated.AddDynamic(this, &UApexSettingsWidget::HandleRailActivated);
		AddV(Stack, Button);
		RailButtons.Add(Button);
	};

	AddTab(TEXT("Gameplay"), ActionTabGameplay);
	AddTab(TEXT("Assists"), ActionTabAssists);
	AddTab(TEXT("Graphics"), ActionTabGraphics);
	AddTab(TEXT("Camera"), ActionTabCamera);
	AddTab(TEXT("Controls"), ActionTabControls);
	AddTab(TEXT("Wheel"), ActionTabWheel);
	AddTab(TEXT("Audio"), ActionTabAudio);

	AddV(Stack, WidgetTree->ConstructWidget<UVerticalBox>(), FMargin(), HAlign_Fill, 1.0f);

	UBorder* Rail = MakePanel(*WidgetTree, Stack, FMargin(0.0f, 22.0f, 0.0f, 0.0f), MakeBrush(Palette::Background));
	return MakeSized(*WidgetTree, Rail, RailWidth, -1.0f);
}

UWidget* UApexSettingsWidget::BuildFooter()
{
	UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();

	FooterStatusText = MakeLabel(*WidgetTree, FString(), Palette::TextMuted);
	Wrapped(FooterStatusText);
	AddH(Row, FooterStatusText, FMargin(0.0f, 0.0f, 24.0f, 0.0f), VAlign_Center, 1.0f);

	UApexButtonWidget* Reset = WidgetTree->ConstructWidget<UApexButtonWidget>();
	FApexButtonSpec ResetSpec;
	ResetSpec.Label = TEXT("Reset to defaults");
	ResetSpec.Variant = EApexButtonVariant::Ghost;
	ResetSpec.bCentreLabel = true;
	ResetSpec.Height = 56.0f;
	ResetSpec.LabelSize = 19.0f;
	ResetSpec.ActionId = ActionReset;
	Reset->Setup(ResetSpec);
	Reset->OnActivated.AddDynamic(this, &UApexSettingsWidget::HandleFooterActivated);
	AddH(Row, MakeSized(*WidgetTree, Reset, 230.0f, 56.0f), FMargin(0.0f, 0.0f, 14.0f, 0.0f));

	UApexButtonWidget* BackToRace = WidgetTree->ConstructWidget<UApexButtonWidget>();
	FApexButtonSpec RaceSpec;
	RaceSpec.Label = TEXT("Back to race");
	RaceSpec.Variant = EApexButtonVariant::Primary;
	RaceSpec.bCentreLabel = true;
	RaceSpec.Height = 56.0f;
	RaceSpec.LabelSize = 19.0f;
	RaceSpec.ActionId = ActionSettingsBack;
	BackToRace->Setup(RaceSpec);
	BackToRace->OnActivated.AddDynamic(this, &UApexSettingsWidget::HandleFooterActivated);
	AddH(Row, MakeSized(*WidgetTree, BackToRace, 200.0f, 56.0f));
	FooterBackButton = BackToRace;

	UBorder* Bar = MakePanel(*WidgetTree, Row, FMargin(38.0f, 0.0f), MakeBrush(Palette::Surface));
	Bar->SetVerticalAlignment(VAlign_Center);
	return MakeSized(*WidgetTree, Bar, -1.0f, 92.0f);
}

// --- Row primitives ---------------------------------------------------------

UWidget* UApexSettingsWidget::MakeSectionLabel(const FString& Text)
{
	return MakeLabel(*WidgetTree, Text);
}

UWidget* UApexSettingsWidget::MakeRow(
	const FString& Label, const FString& Description, UWidget* Control, const FString& PendingNote, float Height,
	UWidget* TitleBadge)
{
	UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();
	const bool bPending = !PendingNote.IsEmpty();

	UVerticalBox* Text = WidgetTree->ConstructWidget<UVerticalBox>();

	UHorizontalBox* TitleRow = WidgetTree->ConstructWidget<UHorizontalBox>();
	AddH(TitleRow, Wrapped(MakeText(*WidgetTree, Label, Font::Body(17.0f, true),
		bPending ? Palette::TextSecondary : Palette::TextPrimary)), FMargin(), VAlign_Center, 1.0f);
	if (bPending)
	{
		// A badge, not a tooltip: the row has to read as "saved but inert" at a
		// glance, without the player having to hover it to find out.
		AddH(TitleRow,
			MakePanel(*WidgetTree, MakeLabel(*WidgetTree, TEXT("Not yet wired"), Palette::Accent),
				FMargin(8.0f, 3.0f), MakeBrush(FLinearColor::Transparent, Palette::Accent, 1.0f, 2.0f)),
			FMargin(12.0f, 0.0f, 0.0f, 0.0f));
	}
	if (TitleBadge)
	{
		AddH(TitleRow, TitleBadge, FMargin(12.0f, 0.0f, 0.0f, 0.0f));
	}
	AddV(Text, TitleRow);

	const FString SubLine = bPending
		? (Description.IsEmpty() ? PendingNote : Description + TEXT("  ") + PendingNote)
		: Description;
	if (!SubLine.IsEmpty())
	{
		AddV(Text, Wrapped(MakeText(*WidgetTree, SubLine, Font::Body(12.0f),
			bPending ? Palette::TextDisabled : Palette::TextMuted)),
			FMargin(0.0f, 5.0f, 0.0f, 0.0f));
	}
	AddH(Row, Text, FMargin(), VAlign_Center, 1.0f);

	if (Control)
	{
		// The text fills, so every control — pill, dropdown or slider — sits
		// against the row's right edge and they line up down that edge
		// whatever their widths. No fixed cell: a two-column page's rows are
		// narrower than one, and a cell sized for the wide page overflowed them.
		AddH(Row, Control, FMargin(24.0f, 0.0f, 0.0f, 0.0f), VAlign_Center);
	}

	UBorder* Panel = MakePanel(*WidgetTree, Row, FMargin(22.0f, 10.0f), MakeBrush(Palette::Surface));
	Panel->SetVerticalAlignment(VAlign_Center);
	// A minimum, not a fixed height: a description that wraps makes the row taller.
	USizeBox* Sized = WidgetTree->ConstructWidget<USizeBox>();
	Sized->SetMinDesiredHeight(Height > 0.0f ? Height : SettingsRowHeight);
	Sized->SetContent(Panel);
	return Sized;
}

UApexSegmentedWidget* UApexSettingsWidget::MakeSegment(
	FName ControlId, const TArray<FString>& Options, int32 Selected, float Width)
{
	UApexSegmentedWidget* Control = WidgetTree->ConstructWidget<UApexSegmentedWidget>();
	Control->ControlId = ControlId;
	Control->Setup(Options, Selected, Width);
	Control->OnChosen.AddDynamic(this, &UApexSettingsWidget::HandleSegmentChosen);
	Segments.Add(ControlId, Control);
	return Control;
}

UWidget* UApexSettingsWidget::MakeSliderCell(
	TObjectPtr<USlider>& OutSlider, TObjectPtr<UProgressBar>& OutFill, TObjectPtr<UTextBlock>& OutValue, float Width)
{
	// MakeSliderTrack fills raw pointers; a TObjectPtr member does not bind to
	// a T*&, so the members are assigned from locals afterwards.
	USlider* Slider = nullptr;
	UProgressBar* Fill = nullptr;

	UHorizontalBox* Cell = WidgetTree->ConstructWidget<UHorizontalBox>();
	AddH(Cell, MakeSliderTrack(*WidgetTree, Slider, Fill), FMargin(), VAlign_Center, 1.0f);
	UTextBlock* Value = MakeText(*WidgetTree, FString(), Font::Mono(13.0f, 40), Palette::TextPrimary);
	AddH(Cell, Value, FMargin(16.0f, 0.0f, 0.0f, 0.0f));

	OutSlider = Slider;
	OutFill = Fill;
	OutValue = Value;

	// An explicit width, because MakeRow right-aligns its control cell and a
	// fill-width slider right-aligned to its own desired size is a stub.
	return MakeSized(*WidgetTree, Cell, Width, -1.0f);
}

void UApexSettingsWidget::ReflectSlider(UProgressBar* Fill, UTextBlock* Value, float Alpha, const FString& Display)
{
	ApexUiAudio::Play(this, EApexUiSound::Adjust);
	if (Fill) { Fill->SetPercent(Alpha); }
	if (Value) { Value->SetText(FText::FromString(Display)); }
	RefreshFooter();
}

// --- Gameplay ---------------------------------------------------------------

UWidget* UApexSettingsWidget::BuildGameplayPage()
{
	UVerticalBox* Page = WidgetTree->ConstructWidget<UVerticalBox>();

	// The driving aids have a page of their own (BuildAssistsPage).
	AddV(Page, MakeSectionLabel(TEXT("Session")), FMargin(0.0f, 0.0f, 0.0f, 14.0f));

	// AI skill: the track fills the control cell, with the percentage after it.
	{
		// MakeSliderTrack fills raw pointers; a TObjectPtr member does not bind to
		// a T*&, so the members are assigned from locals afterwards.
		USlider* Slider = nullptr;
		UProgressBar* Fill = nullptr;

		UHorizontalBox* Cell = WidgetTree->ConstructWidget<UHorizontalBox>();
		AddH(Cell, MakeSliderTrack(*WidgetTree, Slider, Fill), FMargin(), VAlign_Center, 1.0f);
		AiSkillValue = MakeText(*WidgetTree, TEXT("0 %"), Font::Mono(13.0f, 40), Palette::TextPrimary);
		AddH(Cell, AiSkillValue, FMargin(16.0f, 0.0f, 0.0f, 0.0f));

		AiSkillSlider = Slider;
		AiSkillFill = Fill;
		// One step per level: the leftmost is the mixed field, then 70-110.
		Slider->SetStepSize(1.0f / AiSkillSteps);
		Slider->OnValueChanged.AddDynamic(this, &UApexSettingsWidget::HandleAiSkillChanged);

		// An explicit width, because MakeRow right-aligns its control cell and a
		// fill-width slider right-aligned to its own desired size is a stub.

		// The same level the create screen's AI stepper edits
		// (UApexMenuFlowSubsystem::CreateAiSkill, sent as CreateSession.ai_skill).
		AddV(Page, MakeRow(
			TEXT("AI skill"),
			TEXT("The level of the AI field in sessions you create (MIX: every level). The create screen can change it per session."),
			MakeSized(*WidgetTree, Cell, SliderCellWidth, -1.0f)));
	}

	AddV(Page, MakeRow(
		TEXT("Units"),
		TEXT("Speed, distance and temperature."),
		MakeSegment(SegUnits, { TEXT("METRIC"), TEXT("IMPERIAL") }, 0, 178.0f)), FMargin(0.0f, 2.0f, 0.0f, 0.0f));

	AddV(Page, MakeRow(
		TEXT("HUD elements"),
		TEXT("Minimap and pedal telemetry."),
		MakeSegment(SegHud, { TEXT("ALL"), TEXT("ESSENTIAL"), TEXT("HIDDEN") }, 0)), FMargin(0.0f, 2.0f, 0.0f, 0.0f));

	// The HUD editor: the overlay steps aside while it runs (UApexRootWidget).
	{
		UApexButtonWidget* Edit = WidgetTree->ConstructWidget<UApexButtonWidget>();
		FApexButtonSpec EditSpec;
		EditSpec.Label = TEXT("Edit layout");
		EditSpec.Variant = EApexButtonVariant::Ghost;
		EditSpec.bCentreLabel = true;
		EditSpec.Height = 44.0f;
		EditSpec.LabelSize = 15.0f;
		EditSpec.ActionId = ActionEditHud;
		Edit->Setup(EditSpec);
		Edit->OnActivated.AddDynamic(this, &UApexSettingsWidget::HandleFooterActivated);
		AddV(Page, MakeRow(
			TEXT("HUD layout"),
			TEXT("Move, resize, add and remove the HUD's panels."),
			MakeSized(*WidgetTree, Edit, 178.0f, 44.0f)), FMargin(0.0f, 2.0f, 0.0f, 0.0f));
	}

	AddV(Page, WidgetTree->ConstructWidget<UVerticalBox>(), FMargin(), HAlign_Fill, 1.0f);
	return Page;
}

// --- Assists ----------------------------------------------------------------

UWidget* UApexSettingsWidget::BuildAssistsPage()
{
	UVerticalBox* Page = WidgetTree->ConstructWidget<UVerticalBox>();

	// Every aid here runs on the server: UApexRootWidget::SendDriverAids
	// forwards the group on join and on any change. The session's host picks
	// which are allowed when creating it, and the server forces the rest off,
	// so each row carries a lock badge for that case (RefreshAssistLocks).
	AddV(Page, MakeSectionLabel(TEXT("Preset")), FMargin(0.0f, 0.0f, 0.0f, 14.0f));

	AddV(Page, MakeRow(
		TEXT("Assist level"),
		TEXT("Beginner: every aid on. Pro: ABS, light traction control, steering aid, braking line. Elite: no aids. Changing any aid below makes it Custom."),
		MakeSegment(SegAssistPreset, { TEXT("BEGINNER"), TEXT("PRO"), TEXT("ELITE"), TEXT("CUSTOM") }, AssistPresetCustom, 118.0f),
		FString(), 0.0f));

	AddV(Page, MakeSectionLabel(TEXT("Driving aids")), FMargin(0.0f, 26.0f, 0.0f, 14.0f));

	AddV(Page, MakeRow(
		TEXT("ABS"),
		TEXT("Releases brake pressure at lock-up, so a stamped pedal still steers."),
		MakeSegment(SegAbs, { TEXT("OFF"), TEXT("ON") }, 1, 178.0f),
		FString(), 0.0f, MakeAssistLockBadge(SegAbs)));

	AddV(Page, MakeRow(
		TEXT("Traction control"),
		TEXT("LOW stops the driven wheels spinning; HIGH also keeps grip in hand for cornering."),
		MakeSegment(SegTraction, { TEXT("OFF"), TEXT("LOW"), TEXT("HIGH") }, 1),
		FString(), 0.0f, MakeAssistLockBadge(SegTraction)), FMargin(0.0f, 2.0f, 0.0f, 0.0f));

	AddV(Page, MakeRow(
		TEXT("Gearbox"),
		TEXT("Automatic shifting by the server, from the car's torque curve."),
		MakeSegment(SegGearbox, { TEXT("MANUAL"), TEXT("AUTO") }, 1, 178.0f),
		FString(), 0.0f, MakeAssistLockBadge(SegGearbox)), FMargin(0.0f, 2.0f, 0.0f, 0.0f));

	// Also server-side: the lock worth having depends on the car's grip,
	// downforce and wheelbase, none of which the protocol sends.
	AddV(Page, MakeRow(
		TEXT("Steering"),
		TEXT("Less lock as speed rises, so the stick can't overdrive the tyres. Pad and keyboard only: a wheel always steers directly."),
		MakeSegment(SegSteering, { TEXT("FULL LOCK"), TEXT("SPEED SENSITIVE") }, 1, 178.0f),
		FString(), 0.0f, MakeAssistLockBadge(SegSteering)), FMargin(0.0f, 2.0f, 0.0f, 0.0f));

	// Damage is not here: it is the session's rule, picked by the host on
	// the create screen and the same for every car.

	AddV(Page, MakeSectionLabel(TEXT("Guides")), FMargin(0.0f, 26.0f, 0.0f, 14.0f));

	// The server works the line out for the car being driven, so the braking
	// points are that car's; see AApexRacingLineActor. A session that forbids
	// it sends no line at all.
	AddV(Page, MakeRow(
		TEXT("Racing line"),
		TEXT("Dots on the road: green flat out, amber at the limit, red braking."),
		MakeSegment(SegRacingLine, { TEXT("OFF"), TEXT("BRAKING ONLY"), TEXT("FULL") }, 0),
		FString(), 0.0f, MakeAssistLockBadge(SegRacingLine)), FMargin(0.0f, 2.0f, 0.0f, 0.0f));

	AddV(Page, MakeText(*WidgetTree,
		TEXT("The session's host chooses which assists are allowed. A locked assist is off for everyone in that session; your choice is kept for the next one."),
		Font::Body(12.0f), Palette::TextMuted), FMargin(4.0f, 18.0f, 0.0f, 0.0f));

	AddV(Page, WidgetTree->ConstructWidget<UVerticalBox>(), FMargin(), HAlign_Fill, 1.0f);
	return Page;
}

void UApexSettingsWidget::ApplyAssistPreset(int32 Index)
{
	UApexSettingsSubsystem* Settings = GetSettings();
	if (!Settings || Index < 0 || Index >= UE_ARRAY_COUNT(AssistPresets))
	{
		return;
	}

	const FAssistPreset& Preset = AssistPresets[Index];
	Settings->SetTractionControl(Preset.Traction);
	Settings->SetAbs(Preset.bAbs);
	Settings->SetAutoGearbox(Preset.bAutoGearbox);
	Settings->SetSteeringAssist(Preset.bSteeringAssist);
	Settings->SetRacingLine(Preset.RacingLine);

	// Move the five rows to match.
	RefreshFromSettings();
}

void UApexSettingsWidget::RefreshAssistPreset()
{
	const UApexSettingsSubsystem* Settings = GetSettings();
	const UApexSettingsSave* Values = Settings ? Settings->Get() : nullptr;
	const TObjectPtr<UApexSegmentedWidget>* Segment = Segments.Find(SegAssistPreset);
	if (!Values || !Segment || !*Segment)
	{
		return;
	}

	int32 Match = AssistPresetCustom;
	for (int32 i = 0; i < UE_ARRAY_COUNT(AssistPresets); ++i)
	{
		const FAssistPreset& Preset = AssistPresets[i];
		if (Values->TractionControl == Preset.Traction && Values->bAbs == Preset.bAbs
			&& Values->bAutoGearbox == Preset.bAutoGearbox && Values->bSteeringAssist == Preset.bSteeringAssist
			&& Values->RacingLine == Preset.RacingLine)
		{
			Match = i;
			break;
		}
	}

	TGuardValue<bool> Guard(bRefreshing, true);
	(*Segment)->SetSelectedIndex(Match);
}

UWidget* UApexSettingsWidget::MakeAssistLockBadge(FName ControlId)
{
	// The same shape as the "Not yet wired" badge, in the muted colour: this
	// is a rule of the session, not a gap in the game.
	UBorder* Badge = MakePanel(*WidgetTree, MakeLabel(*WidgetTree, TEXT("Off in this session"), Palette::TextMuted),
		FMargin(8.0f, 3.0f), MakeBrush(FLinearColor::Transparent, Palette::TextMuted, 1.0f, 2.0f));
	Badge->SetVisibility(ESlateVisibility::Collapsed);
	AssistLocks.Add(ControlId, Badge);
	return Badge;
}

void UApexSettingsWidget::RefreshAssistLocks()
{
	const UGameInstance* GameInstance = GetGameInstance();
	const UApexNetSubsystem* Net = GameInstance ? GameInstance->GetSubsystem<UApexNetSubsystem>() : nullptr;
	// Outside a session nothing is locked: the page edits the preference that
	// the next session will get.
	const FApexAllowedAssists Allowed = Net && Net->IsInSession() ? Net->GetAllowedAssists() : FApexAllowedAssists();

	const TPair<FName, bool> Rows[] = {
		{ SegAbs,        Allowed.bAbs },
		{ SegTraction,   Allowed.bTractionControl },
		{ SegGearbox,    Allowed.bAutoGearbox },
		{ SegSteering,   Allowed.bSteeringAssist },
		{ SegRacingLine, Allowed.bRacingLine },
	};
	for (const TPair<FName, bool>& Row : Rows)
	{
		if (const TObjectPtr<UWidget>* Badge = AssistLocks.Find(Row.Key))
		{
			if (*Badge)
			{
				(*Badge)->SetVisibility(Row.Value ? ESlateVisibility::Collapsed : ESlateVisibility::HitTestInvisible);
			}
		}
		if (const TObjectPtr<UApexSegmentedWidget>* Segment = Segments.Find(Row.Key))
		{
			// The pills keep showing the stored choice, dimmed: it is what the
			// player will get back in a session that allows the aid.
			if (*Segment)
			{
				(*Segment)->SetIsEnabled(Row.Value);
			}
		}
	}
}

// --- Graphics ---------------------------------------------------------------

UWidget* UApexSettingsWidget::BuildGraphicsPage()
{
	UVerticalBox* Page = WidgetTree->ConstructWidget<UVerticalBox>();

	// Preset sits above the grid: it moves several rows at once, so it is not
	// one of them.
	{
		UVerticalBox* Box = WidgetTree->ConstructWidget<UVerticalBox>();
		AddV(Box, MakeSectionLabel(TEXT("Preset")));
		AddV(Box, MakeSegment(
			SegPreset,
			{ TEXT("LOW"), TEXT("MEDIUM"), TEXT("HIGH"), TEXT("ULTRA"), TEXT("CUSTOM") },
			2,
			190.0f),
			FMargin(0.0f, 12.0f, 0.0f, 0.0f), HAlign_Left);

		AddV(Page, MakePanel(*WidgetTree, Box, FMargin(22.0f, 18.0f), MakeBrush(Palette::Surface)));
	}

	UHorizontalBox* Grid = WidgetTree->ConstructWidget<UHorizontalBox>();
	UVerticalBox* Left = WidgetTree->ConstructWidget<UVerticalBox>();
	UVerticalBox* Right = WidgetTree->ConstructWidget<UVerticalBox>();

	auto AddSliderRow = [this](UVerticalBox* Column, const TCHAR* Label, const TCHAR* Description,
		TObjectPtr<USlider>& OutSlider, TObjectPtr<UProgressBar>& OutFill, TObjectPtr<UTextBlock>& OutValue, bool bFirst)
	{
		USlider* Slider = nullptr;
		UProgressBar* Fill = nullptr;

		UHorizontalBox* Cell = WidgetTree->ConstructWidget<UHorizontalBox>();
		AddH(Cell, MakeSliderTrack(*WidgetTree, Slider, Fill), FMargin(), VAlign_Center, 1.0f);
		UTextBlock* Value = MakeText(*WidgetTree, FString(), Font::Mono(13.0f, 40), Palette::TextPrimary);
		AddH(Cell, Value, FMargin(16.0f, 0.0f, 0.0f, 0.0f));
		AddV(Column, MakeRow(Label, Description, MakeSized(*WidgetTree, Cell, DropdownWidth, -1.0f)),
			FMargin(0.0f, bFirst ? 0.0f : 2.0f, 0.0f, 0.0f));

		OutSlider = Slider;
		OutFill = Fill;
		OutValue = Value;
	};

	// Left column: the window and the frame.
	DisplayModeBox = MakeDropdown(*WidgetTree, DisplayModeNames, 0);
	DisplayModeBox->OnSelectionChanged.AddDynamic(this, &UApexSettingsWidget::HandleDisplayModeChanged);
	AddV(Left, MakeRow(TEXT("Display mode"), FString(), MakeSized(*WidgetTree, DisplayModeBox, DropdownWidth, -1.0f)));

	FrameLimitBox = MakeDropdown(*WidgetTree, {}, 0);
	for (int32 Fps : FrameLimitValues)
	{
		FrameLimitBox->AddOption(FrameLimitName(Fps));
	}
	FrameLimitBox->OnSelectionChanged.AddDynamic(this, &UApexSettingsWidget::HandleFrameLimitChanged);
	AddV(Left, MakeRow(TEXT("Frame limit"), FString(), MakeSized(*WidgetTree, FrameLimitBox, DropdownWidth, -1.0f)),
		FMargin(0.0f, 2.0f, 0.0f, 0.0f));

	ShadowsBox = MakeDropdown(*WidgetTree, QualityNames, 2);
	ShadowsBox->OnSelectionChanged.AddDynamic(this, &UApexSettingsWidget::HandleShadowsChanged);
	AddV(Left, MakeRow(TEXT("Shadows"), FString(), MakeSized(*WidgetTree, ShadowsBox, DropdownWidth, -1.0f)),
		FMargin(0.0f, 2.0f, 0.0f, 0.0f));

	TexturesBox = MakeDropdown(*WidgetTree, QualityNames, 3);
	TexturesBox->OnSelectionChanged.AddDynamic(this, &UApexSettingsWidget::HandleTexturesChanged);
	AddV(Left, MakeRow(TEXT("Textures"), FString(), MakeSized(*WidgetTree, TexturesBox, DropdownWidth, -1.0f)),
		FMargin(0.0f, 2.0f, 0.0f, 0.0f));

	AddSliderRow(Left, TEXT("Motion blur"), TEXT(""), MotionBlurSlider, MotionBlurFill, MotionBlurValue, false);
	MotionBlurSlider->OnValueChanged.AddDynamic(this, &UApexSettingsWidget::HandleMotionBlurChanged);

	// Right column: the image itself.
	ResolutionBox = MakeDropdown(*WidgetTree, {}, 0);
	if (const UApexSettingsSubsystem* Settings = GetSettings())
	{
		for (const FIntPoint& Mode : Settings->GetAvailableResolutions())
		{
			ResolutionBox->AddOption(ResolutionName(Mode));
		}
	}
	ResolutionBox->OnSelectionChanged.AddDynamic(this, &UApexSettingsWidget::HandleResolutionChanged);
	AddV(Right, MakeRow(TEXT("Resolution"), FString(), MakeSized(*WidgetTree, ResolutionBox, DropdownWidth, -1.0f)));

	AddV(Right, MakeRow(TEXT("V-Sync"), FString(),
		MakeSegment(SegVSync, { TEXT("OFF"), TEXT("ON") }, 0, 120.0f)), FMargin(0.0f, 2.0f, 0.0f, 0.0f));

	AntiAliasingBox = MakeDropdown(*WidgetTree, AntiAliasingNames, 2);
	AntiAliasingBox->OnSelectionChanged.AddDynamic(this, &UApexSettingsWidget::HandleAntiAliasingChanged);
	AddV(Right, MakeRow(TEXT("Anti-aliasing"), FString(), MakeSized(*WidgetTree, AntiAliasingBox, DropdownWidth, -1.0f)),
		FMargin(0.0f, 2.0f, 0.0f, 0.0f));
	// Field of view lives on the camera page with the seat it belongs to.

	AddH(Grid, Left, FMargin(0.0f, 0.0f, 14.0f, 0.0f), VAlign_Top, 1.0f);
	AddH(Grid, Right, FMargin(), VAlign_Top, 1.0f);
	AddV(Page, Grid, FMargin(0.0f, 14.0f, 0.0f, 0.0f));

	// Screens: one monitor, or a triple-monitor rig with a view per monitor
	// (UApexMultiViewSubsystem). The four measurements are what the side
	// views are worked out from, so they are here with the switch rather
	// than on the camera page, and they only mean anything on TRIPLE.
	{
		UVerticalBox* Box = WidgetTree->ConstructWidget<UVerticalBox>();

		UHorizontalBox* Head = WidgetTree->ConstructWidget<UHorizontalBox>();
		AddH(Head, MakeSectionLabel(TEXT("Screens")), FMargin(), VAlign_Center);
		AddH(Head, MakeSegment(SegScreens, { TEXT("SINGLE"), TEXT("TRIPLE") }, 0, 140.0f),
			FMargin(24.0f, 0.0f, 0.0f, 0.0f), VAlign_Center);
		AddV(Box, Head);

		TripleNoteText = Wrapped(MakeText(*WidgetTree, FString(), Font::Body(13.0f), Palette::TextSecondary));
		AddV(Box, TripleNoteText, FMargin(0.0f, 8.0f, 0.0f, 0.0f));

		UHorizontalBox* TripleGrid = WidgetTree->ConstructWidget<UHorizontalBox>();
		UVerticalBox* TripleLeft = WidgetTree->ConstructWidget<UVerticalBox>();
		UVerticalBox* TripleRight = WidgetTree->ConstructWidget<UVerticalBox>();

		AddSliderRow(TripleLeft, TEXT("Screen width"), TEXT("The picture of one monitor, edge to edge."),
			TripleWidthSlider, TripleWidthFill, TripleWidthValue, true);
		TripleWidthSlider->OnValueChanged.AddDynamic(this, &UApexSettingsWidget::HandleTripleWidthChanged);
		AddSliderRow(TripleLeft, TEXT("Eye distance"), TEXT("From your eyes to the middle of the centre monitor."),
			TripleDistanceSlider, TripleDistanceFill, TripleDistanceValue, false);
		TripleDistanceSlider->OnValueChanged.AddDynamic(this, &UApexSettingsWidget::HandleTripleDistanceChanged);

		AddSliderRow(TripleRight, TEXT("Bezel"), TEXT("The gap between two pictures: both bezels together."),
			TripleBezelSlider, TripleBezelFill, TripleBezelValue, true);
		TripleBezelSlider->OnValueChanged.AddDynamic(this, &UApexSettingsWidget::HandleTripleBezelChanged);
		AddSliderRow(TripleRight, TEXT("Side angle"), TEXT("How far each side monitor is turned toward you; 0 is a flat row."),
			TripleAngleSlider, TripleAngleFill, TripleAngleValue, false);
		TripleAngleSlider->OnValueChanged.AddDynamic(this, &UApexSettingsWidget::HandleTripleAngleChanged);

		AddH(TripleGrid, TripleLeft, FMargin(0.0f, 0.0f, 14.0f, 0.0f), VAlign_Top, 1.0f);
		AddH(TripleGrid, TripleRight, FMargin(), VAlign_Top, 1.0f);
		AddV(Box, TripleGrid, FMargin(0.0f, 12.0f, 0.0f, 0.0f));
		TripleGeometryBox = TripleGrid;

		AddV(Page, MakePanel(*WidgetTree, Box, FMargin(22.0f, 18.0f), MakeBrush(Palette::Surface)),
			FMargin(0.0f, 18.0f, 0.0f, 0.0f));
	}

	// The note the mockup carries, and it is true here: the pause menu leaves
	// the scene rendering behind the panel, so a change is visible at once.
	{
		UHorizontalBox* Note = WidgetTree->ConstructWidget<UHorizontalBox>();
		AddH(Note, MakeLabel(*WidgetTree, TEXT("Live preview"), Palette::Accent));
		AddH(Note,
			Wrapped(MakeText(*WidgetTree,
				TEXT("Changes apply to the frame behind this panel, so the effect is visible before returning to the race."),
				Font::Body(13.0f), Palette::TextSecondary)),
			FMargin(18.0f, 0.0f, 0.0f, 0.0f), VAlign_Center, 1.0f);
		AddV(Page, MakePanel(*WidgetTree, Note, FMargin(22.0f, 16.0f), MakeBrush(Palette::Surface)),
			FMargin(0.0f, 18.0f, 0.0f, 0.0f));
	}

	AddV(Page, WidgetTree->ConstructWidget<UVerticalBox>(), FMargin(), HAlign_Fill, 1.0f);
	return Page;
}

// --- Camera -----------------------------------------------------------------

UWidget* UApexSettingsWidget::BuildCameraPage()
{
	UVerticalBox* Page = WidgetTree->ConstructWidget<UVerticalBox>();

	UHorizontalBox* Grid = WidgetTree->ConstructWidget<UHorizontalBox>();
	UVerticalBox* Left = WidgetTree->ConstructWidget<UVerticalBox>();
	UVerticalBox* Right = WidgetTree->ConstructWidget<UVerticalBox>();

	const FMargin RowGap(0.0f, 2.0f, 0.0f, 0.0f);

	// Left: the seat. Everything here moves the eye.
	AddV(Left, MakeSectionLabel(TEXT("Seat")), FMargin(0.0f, 0.0f, 0.0f, 12.0f));

	AddV(Left, MakeRow(
		TEXT("Start in"),
		TEXT("The view a race opens in. C steps through them at any time."),
		MakeSegment(SegStartView, { TEXT("CHASE"), TEXT("COCKPIT") }, 1, 130.0f)));

	AddV(Left, MakeRow(
		TEXT("Chase distance"),
		TEXT("How far back the chase camera sits. C steps through these, then back to the cockpit."),
		MakeSegment(SegChaseView, { TEXT("ROOF"), TEXT("CLOSE"), TEXT("NEAR"), TEXT("FAR") },
			ApexChase::DefaultLevel(), 84.0f)), RowGap);

	AddV(Left, MakeRow(TEXT("Field of view"), TEXT("Horizontal, cockpit. Chase sits 15° narrower."),
		MakeSliderCell(FovSlider, FovFill, FovValue, DropdownWidth)), RowGap);
	FovSlider->OnValueChanged.AddDynamic(this, &UApexSettingsWidget::HandleFovChanged);

	AddV(Left, MakeRow(TEXT("Seat forward"), TEXT("Slide from the car's own driving position."),
		MakeSliderCell(SeatForwardSlider, SeatForwardFill, SeatForwardValue, DropdownWidth)), RowGap);
	SeatForwardSlider->OnValueChanged.AddDynamic(this, &UApexSettingsWidget::HandleSeatForwardChanged);

	AddV(Left, MakeRow(TEXT("Seat height"), TEXT("Raise or lower the eye."),
		MakeSliderCell(SeatHeightSlider, SeatHeightFill, SeatHeightValue, DropdownWidth)), RowGap);
	SeatHeightSlider->OnValueChanged.AddDynamic(this, &UApexSettingsWidget::HandleSeatHeightChanged);

	AddV(Left, MakeRow(TEXT("View pitch"), TEXT("Resting gaze, up or down."),
		MakeSliderCell(ViewPitchSlider, ViewPitchFill, ViewPitchValue, DropdownWidth)), RowGap);
	ViewPitchSlider->OnValueChanged.AddDynamic(this, &UApexSettingsWidget::HandleViewPitchChanged);

	// Right: how the head behaves, and what of the cockpit is drawn.
	AddV(Right, MakeSectionLabel(TEXT("Head")), FMargin(0.0f, 0.0f, 0.0f, 12.0f));

	AddV(Right, MakeRow(TEXT("Horizon lock"), TEXT("0 rides every bump and bank; 100 keeps the horizon level."),
		MakeSliderCell(HorizonLockSlider, HorizonLockFill, HorizonLockValue, DropdownWidth)));
	HorizonLockSlider->OnValueChanged.AddDynamic(this, &UApexSettingsWidget::HandleHorizonLockChanged);

	AddV(Right, MakeRow(TEXT("Head motion"), TEXT("How far braking and cornering throw the head."),
		MakeSliderCell(HeadMotionSlider, HeadMotionFill, HeadMotionValue, DropdownWidth)), RowGap);
	HeadMotionSlider->OnValueChanged.AddDynamic(this, &UApexSettingsWidget::HandleHeadMotionChanged);

	AddV(Right, MakeRow(TEXT("Look to apex"), TEXT("Turn the head into the corner with the steering."),
		MakeSliderCell(LookToApexSlider, LookToApexFill, LookToApexValue, DropdownWidth)), RowGap);
	LookToApexSlider->OnValueChanged.AddDynamic(this, &UApexSettingsWidget::HandleLookToApexChanged);

	AddV(Right, MakeSectionLabel(TEXT("Cockpit")), FMargin(0.0f, 18.0f, 0.0f, 12.0f));

	AddV(Right, MakeRow(TEXT("Car body"), TEXT("Draw your own car from inside."),
		MakeSegment(SegCockpitCar, { TEXT("OFF"), TEXT("ON") }, 1, 120.0f)));
	AddV(Right, MakeRow(TEXT("Wheel & display"), TEXT("Steering wheel with gear, speed and revs."),
		MakeSegment(SegCockpitWheel, { TEXT("OFF"), TEXT("ON") }, 1, 120.0f)), RowGap);
	AddV(Right, MakeRow(TEXT("Mirrors"), TEXT("On the car, in the cockpit view."),
		MakeSegment(SegCockpitMirrors, { TEXT("OFF"), TEXT("ON") }, 1, 120.0f)), RowGap);
	AddV(Right, MakeRow(TEXT("Virtual mirror"), TEXT("Rear view at the top of the HUD, in either view."),
		MakeSegment(SegVirtualMirror, { TEXT("OFF"), TEXT("ON") }, 0, 120.0f)), RowGap);
	AddV(Right, MakeRow(TEXT("Mirror quality"), TEXT("Each mirror is another render of the scene."),
		MakeSegment(SegMirrorQuality, { TEXT("LOW"), TEXT("MEDIUM"), TEXT("HIGH") }, 1)), RowGap);

	AddH(Grid, Left, FMargin(0.0f, 0.0f, 14.0f, 0.0f), VAlign_Top, 1.0f);
	AddH(Grid, Right, FMargin(), VAlign_Top, 1.0f);
	AddV(Page, Grid);

	// The seat moves behind the panel as the slider does, which is the only
	// honest way to set one; and the look keys are not on the controls page's
	// first screen, so say where they are.
	{
		UHorizontalBox* Note = WidgetTree->ConstructWidget<UHorizontalBox>();
		AddH(Note, MakeLabel(*WidgetTree, TEXT("Live preview"), Palette::Accent));
		AddH(Note,
			Wrapped(MakeText(*WidgetTree,
				TEXT("The seat and mirrors move behind this panel as you drag. In the race: , and . look aside, B looks behind, C swaps views."),
				Font::Body(13.0f), Palette::TextSecondary)),
			FMargin(18.0f, 0.0f, 0.0f, 0.0f), VAlign_Center, 1.0f);
		AddV(Page, MakePanel(*WidgetTree, Note, FMargin(22.0f, 16.0f), MakeBrush(Palette::Surface)),
			FMargin(0.0f, 18.0f, 0.0f, 0.0f));
	}

	AddV(Page, WidgetTree->ConstructWidget<UVerticalBox>(), FMargin(), HAlign_Fill, 1.0f);
	return Page;
}

// --- Controls ---------------------------------------------------------------

UWidget* UApexSettingsWidget::BuildControlsPage()
{
	UVerticalBox* Page = WidgetTree->ConstructWidget<UVerticalBox>();

	AddV(Page, MakeSectionLabel(TEXT("Device")), FMargin(0.0f, 0.0f, 0.0f, 12.0f));

	// The keyboard, which is always there, and whether a gamepad is attached.
	// Wheels and pedals have a page of their own, because there can be several
	// of them and each has a name, axes and forces worth showing.
	{
		UHorizontalBox* Devices = WidgetTree->ConstructWidget<UHorizontalBox>();

		auto AddDeviceCard = [this, Devices](const TCHAR* Name, TObjectPtr<UTextBlock>& OutState, const TCHAR* InitialState, bool bFirst)
		{
			UVerticalBox* Box = WidgetTree->ConstructWidget<UVerticalBox>();
			AddV(Box, MakeText(*WidgetTree, Name, Font::Body(17.0f, true), Palette::TextPrimary));
			UTextBlock* State = MakeLabel(*WidgetTree, InitialState);
			OutState = State;
			AddV(Box, State, FMargin(0.0f, 7.0f, 0.0f, 0.0f));

			AddH(Devices, MakePanel(*WidgetTree, Box, FMargin(20.0f, 14.0f),
				MakeBrush(Palette::Surface, Palette::Border, 1.0f)),
				FMargin(bFirst ? 0.0f : 12.0f, 0.0f, 0.0f, 0.0f), VAlign_Fill, 1.0f);
		};

		TObjectPtr<UTextBlock> KeyboardState;
		AddDeviceCard(TEXT("Keyboard"), KeyboardState, TEXT("Always available"), true);
		AddDeviceCard(TEXT("Gamepad"), GamepadStateText, TEXT("Not detected"), false);
		AddDeviceCard(TEXT("Wheel & pedals"), DeviceCountText, TEXT("See the Wheel page"), false);

		AddV(Page, Devices);
	}

	// The three continuous controls, side by side as in the design.
	{
		UHorizontalBox* Sliders = WidgetTree->ConstructWidget<UHorizontalBox>();

		auto AddSliderCell = [this, Sliders](const TCHAR* Label, TObjectPtr<USlider>& OutSlider,
			TObjectPtr<UProgressBar>& OutFill, TObjectPtr<UTextBlock>& OutValue, bool bFirst)
		{
			USlider* Slider = nullptr;
			UProgressBar* Fill = nullptr;

			UVerticalBox* Cell = WidgetTree->ConstructWidget<UVerticalBox>();

			UHorizontalBox* Head = WidgetTree->ConstructWidget<UHorizontalBox>();
			AddH(Head, MakeLabel(*WidgetTree, Label));
			AddH(Head, WidgetTree->ConstructWidget<UHorizontalBox>(), FMargin(), VAlign_Center, 1.0f);
			UTextBlock* Value = MakeText(*WidgetTree, FString(), Font::Mono(13.0f, 40), Palette::TextPrimary);
			AddH(Head, Value);
			AddV(Cell, Head);

			AddV(Cell, MakeSliderTrack(*WidgetTree, Slider, Fill), FMargin(0.0f, 8.0f, 0.0f, 0.0f));

			AddH(Sliders, Cell, FMargin(bFirst ? 0.0f : 26.0f, 0.0f, 0.0f, 0.0f), VAlign_Center, 1.0f);

			OutSlider = Slider;
			OutFill = Fill;
			OutValue = Value;
		};

		// All three are the pad's and the keyboard's: a wheel gets none of this
		// shaping (a thumbstick's deadzone is several degrees of a rim) and its
		// forces are on the Wheel page.
		AddSliderCell(TEXT("Steering sensitivity"), SteeringSlider, SteeringFill, SteeringValue, true);
		AddSliderCell(TEXT("Deadzone"), DeadzoneSlider, DeadzoneFill, DeadzoneValue, false);
		// The pad's rumble from the server's DriverFeedback: tyres past their
		// grip, curbs, grass, bumps and contact. Dragging it plays a pulse.
		AddSliderCell(TEXT("Pad vibration"), VibrationSlider, VibrationFill, VibrationValue, false);

		SteeringSlider->OnValueChanged.AddDynamic(this, &UApexSettingsWidget::HandleSteeringChanged);
		DeadzoneSlider->OnValueChanged.AddDynamic(this, &UApexSettingsWidget::HandleDeadzoneChanged);
		VibrationSlider->OnValueChanged.AddDynamic(this, &UApexSettingsWidget::HandleVibrationChanged);

		AddV(Page, Sliders, FMargin(0.0f, 26.0f, 0.0f, 0.0f));
	}

	UHorizontalBox* BindingsHead = WidgetTree->ConstructWidget<UHorizontalBox>();
	AddH(BindingsHead, MakeSectionLabel(TEXT("Bindings")));
	AddH(BindingsHead, WidgetTree->ConstructWidget<UHorizontalBox>(), FMargin(), VAlign_Center, 1.0f);
	AddH(BindingsHead, MakeLabel(*WidgetTree, TEXT("Click a slot, then press a key or button")));
	AddV(Page, BindingsHead, FMargin(0.0f, 28.0f, 0.0f, 12.0f));

	AddV(Page, BuildBindingsGrid());

	UHorizontalBox* Legend = WidgetTree->ConstructWidget<UHorizontalBox>();
	AddH(Legend, MakeLabel(*WidgetTree, TEXT("Column 1 · Device")));
	AddH(Legend, MakeLabel(*WidgetTree, TEXT("Column 2 · Keyboard")), FMargin(26.0f, 0.0f, 0.0f, 0.0f));
	AddH(Legend, MakeLabel(*WidgetTree, TEXT("Conflicts are flagged in red"), Palette::Error),
		FMargin(26.0f, 0.0f, 0.0f, 0.0f));
	AddV(Page, Legend, FMargin(0.0f, 14.0f, 0.0f, 0.0f));

	AddV(Page, WidgetTree->ConstructWidget<UVerticalBox>(), FMargin(), HAlign_Fill, 1.0f);
	return Page;
}

// --- Wheel page -------------------------------------------------------------

UWidget* UApexSettingsWidget::BuildWheelPage()
{
	UVerticalBox* Page = WidgetTree->ConstructWidget<UVerticalBox>();

	AddV(Page, MakeSectionLabel(TEXT("Devices")), FMargin(0.0f, 0.0f, 0.0f, 12.0f));

	// Filled by RefreshWheelDevices: what is attached changes while this page
	// is open, which is exactly when a player is plugging things in.
	WheelDeviceRow = WidgetTree->ConstructWidget<UHorizontalBox>();
	AddV(Page, WheelDeviceRow);

	AddV(Page, MakeSectionLabel(TEXT("Force feedback")), FMargin(0.0f, 24.0f, 0.0f, 12.0f));

	// A row of labelled sliders, each with its value at the top right and a
	// line of explanation under it.
	auto AddSliderCell = [this](UHorizontalBox* Sliders, const TCHAR* Label, const TCHAR* Note,
		TObjectPtr<USlider>& OutSlider, TObjectPtr<UProgressBar>& OutFill, TObjectPtr<UTextBlock>& OutValue, bool bFirst)
	{
		USlider* Slider = nullptr;
		UProgressBar* Fill = nullptr;

		UVerticalBox* Cell = WidgetTree->ConstructWidget<UVerticalBox>();

		UHorizontalBox* Head = WidgetTree->ConstructWidget<UHorizontalBox>();
		AddH(Head, MakeLabel(*WidgetTree, Label));
		AddH(Head, WidgetTree->ConstructWidget<UHorizontalBox>(), FMargin(), VAlign_Center, 1.0f);
		UTextBlock* Value = MakeText(*WidgetTree, FString(), Font::Mono(13.0f, 40), Palette::TextPrimary);
		AddH(Head, Value);
		AddV(Cell, Head);

		AddV(Cell, MakeSliderTrack(*WidgetTree, Slider, Fill), FMargin(0.0f, 8.0f, 0.0f, 0.0f));
		AddV(Cell, Wrapped(MakeText(*WidgetTree, Note, Font::Body(12.0f), Palette::TextMuted)), FMargin(0.0f, 8.0f, 0.0f, 0.0f));

		AddH(Sliders, Cell, FMargin(bFirst ? 0.0f : 26.0f, 0.0f, 0.0f, 0.0f), VAlign_Top, 1.0f);

		OutSlider = Slider;
		OutFill = Fill;
		OutValue = Value;
	};

	{
		UHorizontalBox* Sliders = WidgetTree->ConstructWidget<UHorizontalBox>();

		AddSliderCell(Sliders, TEXT("Force"), TEXT("The steering torque the car is actually making."),
			WheelForceSlider, WheelForceFill, WheelForceValue, true);
		AddSliderCell(Sliders, TEXT("Road effects"), TEXT("Curbs, grass, ABS and impacts on top of it."),
			WheelRoadSlider, WheelRoadFill, WheelRoadValue, false);
		AddSliderCell(Sliders, TEXT("Damping"), TEXT("Weight in the rim. Steadies it on a network."),
			WheelDampingSlider, WheelDampingFill, WheelDampingValue, false);

		WheelForceSlider->OnValueChanged.AddDynamic(this, &UApexSettingsWidget::HandleWheelForceChanged);
		WheelRoadSlider->OnValueChanged.AddDynamic(this, &UApexSettingsWidget::HandleWheelRoadChanged);
		WheelDampingSlider->OnValueChanged.AddDynamic(this, &UApexSettingsWidget::HandleWheelDampingChanged);

		AddV(Page, Sliders);
	}

	// Which base this is, and how strong: what turns the sliders' shares of
	// the base into newton-metres (ApexWheelProfiles). Detected from the
	// base's name; the player can name another or give the peak their base's
	// own software is set to.
	{
		UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();

		auto AddStepperCell = [this, Row](const TCHAR* Label, TObjectPtr<UApexStepperWidget>& OutStepper, FName Id,
			int32 Max, float ReadoutWidth, bool bFirst)
		{
			UVerticalBox* Cell = WidgetTree->ConstructWidget<UVerticalBox>();
			AddV(Cell, MakeLabel(*WidgetTree, Label));
			UApexStepperWidget* Stepper = WidgetTree->ConstructWidget<UApexStepperWidget>();
			Stepper->ControlId = Id;
			Stepper->Setup(0, Max, 0, ReadoutWidth);
			Stepper->OnChanged.AddDynamic(this, &UApexSettingsWidget::HandleWheelProfileStepped);
			AddV(Cell, Stepper, FMargin(0.0f, 8.0f, 0.0f, 0.0f), HAlign_Left);
			AddH(Row, Cell, FMargin(bFirst ? 0.0f : 26.0f, 0.0f, 0.0f, 0.0f), VAlign_Top);
			OutStepper = Stepper;
		};

		AddStepperCell(TEXT("Wheelbase"), WheelProfileStepper, TEXT("WheelProfile"),
			ApexWheelProfiles::All().Num(), 300.0f, true);
		WheelProfileStepper->Formatter = [this](int32 Value)
		{
			const UApexSettingsSubsystem* Settings = GetSettings();
			const UApexSettingsSubsystem::FWheelProfileState State = Settings
				? Settings->GetWheelProfile() : UApexSettingsSubsystem::FWheelProfileState();
			if (!State.bHasBase)
			{
				return FString(TEXT("No force wheel"));
			}
			const TConstArrayView<ApexWheelProfiles::FProfile> Profiles = ApexWheelProfiles::All();
			return Value <= 0 || Value > Profiles.Num()
				? FString::Printf(TEXT("Auto: %s"), State.Detected->DisplayName)
				: FString(Profiles[Value - 1].DisplayName);
		};

		AddStepperCell(TEXT("Peak torque"), WheelPeakStepper, TEXT("WheelPeak"),
			FMath::RoundToInt(ApexWheelProfiles::MaxPeakTorqueNm), 150.0f, false);
		WheelPeakStepper->Formatter = [this](int32 Value)
		{
			if (Value > 0)
			{
				return FString::Printf(TEXT("%d Nm"), Value);
			}
			const UApexSettingsSubsystem* Settings = GetSettings();
			const UApexSettingsSubsystem::FWheelProfileState State = Settings
				? Settings->GetWheelProfile() : UApexSettingsSubsystem::FWheelProfileState();
			return State.Profile->PeakTorqueNm > 0.0f
				? FString::Printf(TEXT("Auto: %s Nm"), *FString::SanitizeFloat(State.Profile->PeakTorqueNm, 0))
				: FString(TEXT("Auto: unknown"));
		};

		UVerticalBox* NoteCell = WidgetTree->ConstructWidget<UVerticalBox>();
		WheelProfileNote = MakeText(*WidgetTree, FString(), Font::Mono(12.0f, 40), Palette::TextSecondary);
		AddV(NoteCell, WheelProfileNote);
		AddV(NoteCell, Wrapped(MakeText(*WidgetTree,
			TEXT("Turned the base down in its own software? Give the peak it is set to."),
			Font::Body(12.0f), Palette::TextMuted)), FMargin(0.0f, 8.0f, 0.0f, 0.0f));
		AddH(Row, NoteCell, FMargin(26.0f, 22.0f, 0.0f, 0.0f), VAlign_Top, 1.0f);

		AddV(Page, Row, FMargin(0.0f, 20.0f, 0.0f, 0.0f));
	}

	// Steering and direction share a row: how far the rim turns for the car's
	// full lock (the base's rotation has to be said, because DirectInput only
	// reports where the rim is between its two ends), and which way a positive
	// force turns it, with the test that answers that. DirectInput does not say
	// which way, so the player is asked rather than told. One row, because the
	// bindings below have to fit on the page without scrolling.
	{
		UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();

		AddSliderCell(Row, TEXT("Wheel rotation"), TEXT("Lock to lock, as set in the wheel's driver."),
			WheelRotationSlider, WheelRotationFill, WheelRotationValue, true);
		AddSliderCell(Row, TEXT("Steering lock"), TEXT("Rim turn for full lock. Auto takes each car's own."),
			WheelSteeringLockSlider, WheelSteeringLockFill, WheelSteeringLockValue, false);

		WheelRotationSlider->SetStepSize(WheelRotationStepDeg / (ApexInput::WheelRotationMaxDeg - ApexInput::WheelRotationMinDeg));
		WheelSteeringLockSlider->SetStepSize(SteeringLockStepDeg / (ApexInput::SteeringLockMaxDeg - SteeringLockSliderMinDeg));
		WheelRotationSlider->OnValueChanged.AddDynamic(this, &UApexSettingsWidget::HandleWheelRotationChanged);
		WheelSteeringLockSlider->OnValueChanged.AddDynamic(this, &UApexSettingsWidget::HandleWheelSteeringLockChanged);

		UVerticalBox* Direction = WidgetTree->ConstructWidget<UVerticalBox>();
		AddV(Direction, MakeLabel(*WidgetTree, TEXT("Direction")));

		UHorizontalBox* Controls = WidgetTree->ConstructWidget<UHorizontalBox>();
		AddH(Controls, MakeSegment(SegWheelDirection, { TEXT("NORMAL"), TEXT("INVERTED") }, 0));

		UApexButtonWidget* Test = WidgetTree->ConstructWidget<UApexButtonWidget>();
		FApexButtonSpec TestSpec;
		TestSpec.Label = TEXT("Test");
		TestSpec.Variant = EApexButtonVariant::Ghost;
		TestSpec.bCentreLabel = true;
		TestSpec.Height = 38.0f;
		TestSpec.LabelSize = 13.0f;
		TestSpec.ActionId = ActionWheelTest;
		Test->Setup(TestSpec);
		Test->OnActivated.AddDynamic(this, &UApexSettingsWidget::HandleWheelTestActivated);
		AddH(Controls, MakeSized(*WidgetTree, Test, 70.0f, 38.0f), FMargin(8.0f, 0.0f, 0.0f, 0.0f));
		AddV(Direction, Controls, FMargin(0.0f, 8.0f, 0.0f, 0.0f));

		AddV(Direction, Wrapped(MakeText(*WidgetTree, TEXT("Test pushes right. Went left? Invert."), Font::Body(12.0f), Palette::TextMuted)),
			FMargin(0.0f, 8.0f, 0.0f, 0.0f));
		AddH(Row, Direction, FMargin(26.0f, 0.0f, 0.0f, 0.0f), VAlign_Top, 1.0f);

		AddV(Page, Row, FMargin(0.0f, 20.0f, 0.0f, 0.0f));
	}

	UHorizontalBox* DrivingHead = WidgetTree->ConstructWidget<UHorizontalBox>();
	AddH(DrivingHead, MakeSectionLabel(TEXT("Wheel bindings")));
	AddH(DrivingHead, WidgetTree->ConstructWidget<UHorizontalBox>(), FMargin(), VAlign_Center, 1.0f);
	AddH(DrivingHead, MakeLabel(*WidgetTree,
		TEXT("Click a slot, then move that control  ·  kept per device")));

	UHorizontalBox* BindingsHead = WidgetTree->ConstructWidget<UHorizontalBox>();
	AddWeighted(BindingsHead, DrivingHead, FMargin(0.0f, 0.0f, WheelColumnGap, 0.0f), VAlign_Center, WheelDrivingWeight);
	AddWeighted(BindingsHead, MakeSectionLabel(TEXT("Menu")), FMargin(), VAlign_Center, WheelMenuWeight);
	AddV(Page, BindingsHead, FMargin(0.0f, 20.0f, 0.0f, 10.0f));

	AddV(Page, BuildWheelBindings());

	AddV(Page, WidgetTree->ConstructWidget<UVerticalBox>(), FMargin(), HAlign_Fill, 1.0f);
	return Page;
}

UWidget* UApexSettingsWidget::BuildWheelBindings()
{
	// The axes come first and carry a meter each: a binding that reads
	// backwards, or not at all, is the whole of what goes wrong with a wheel,
	// and a bar says so at a glance. The driving slots split down two columns;
	// the menu's six have a narrow third one of their own, under the MENU
	// caption, which is why their rows can be called just "Up" and "OK".
	struct FWheelRowSpec
	{
		const TCHAR* Label;
		FName ActionId;
		int32 Slot;
		/** Meters are only on the three that decide whether the car is drivable. */
		bool bMeter;
		/** 0 and 1 are the driving block's columns, 2 the menu's. */
		int32 Column;
	};
	static const TArray<FWheelRowSpec> RowSpecs = {
		{ TEXT("Steering"),    ApexInput::Actions::Steer,        ApexInput::Slot::Wheel,     true,  0 },
		{ TEXT("Throttle"),    ApexInput::Actions::Throttle,     ApexInput::Slot::Wheel,     true,  0 },
		{ TEXT("Brake"),       ApexInput::Actions::Brake,        ApexInput::Slot::Wheel,     true,  0 },
		{ TEXT("Shift up"),    ApexInput::Actions::GearUp,       ApexInput::Slot::Wheel,     false, 0 },
		{ TEXT("Shift down"),  ApexInput::Actions::GearDown,     ApexInput::Slot::Wheel,     false, 0 },
		{ TEXT("Camera"),      ApexInput::Actions::ToggleCamera, ApexInput::Slot::Wheel,     false, 1 },
		{ TEXT("Look left"),   ApexInput::Actions::Look,         ApexInput::Slot::WheelLow,  false, 1 },
		{ TEXT("Look right"),  ApexInput::Actions::Look,         ApexInput::Slot::WheelHigh, false, 1 },
		{ TEXT("Look behind"), ApexInput::Actions::LookBack,     ApexInput::Slot::Wheel,     false, 1 },
		{ TEXT("DRS"),         ApexInput::Actions::Drs,          ApexInput::Slot::Wheel,     false, 1 },
		{ TEXT("Headlights"),  ApexInput::Actions::Headlights,   ApexInput::Slot::Wheel,     false, 1 },
		{ TEXT("Flash lights"), ApexInput::Actions::FlashLights, ApexInput::Slot::Wheel,     false, 1 },
		{ TEXT("ERS mode"),    ApexInput::Actions::ErsMode,      ApexInput::Slot::Wheel,     false, 1 },
		{ TEXT("Overtake"),    ApexInput::Actions::ErsBoost,     ApexInput::Slot::Wheel,     false, 1 },
		{ TEXT("Pause menu"),  ApexInput::Actions::PauseMenu,    ApexInput::Slot::Wheel,     false, 1 },
		{ TEXT("Up"),          ApexInput::Actions::MenuUp,       ApexInput::Slot::Wheel,     false, 2 },
		{ TEXT("Down"),        ApexInput::Actions::MenuDown,     ApexInput::Slot::Wheel,     false, 2 },
		{ TEXT("Left"),        ApexInput::Actions::MenuLeft,     ApexInput::Slot::Wheel,     false, 2 },
		{ TEXT("Right"),       ApexInput::Actions::MenuRight,    ApexInput::Slot::Wheel,     false, 2 },
		{ TEXT("OK"),          ApexInput::Actions::MenuAccept,   ApexInput::Slot::Wheel,     false, 2 },
		{ TEXT("Back"),        ApexInput::Actions::MenuBack,     ApexInput::Slot::Wheel,     false, 2 },
	};

	UVerticalBox* Columns[3] = {
		WidgetTree->ConstructWidget<UVerticalBox>(),
		WidgetTree->ConstructWidget<UVerticalBox>(),
		WidgetTree->ConstructWidget<UVerticalBox>(),
	};

	WheelMeterBars.Reset();
	WheelMeterValues.Reset();

	for (int32 Index = 0; Index < RowSpecs.Num(); ++Index)
	{
		const FWheelRowSpec& RowSpec = RowSpecs[Index];
		UHorizontalBox* Cell = WidgetTree->ConstructWidget<UHorizontalBox>();

		if (RowSpec.bMeter)
		{
			UProgressBar* Bar = WidgetTree->ConstructWidget<UProgressBar>();
			FProgressBarStyle BarStyle = Bar->GetWidgetStyle();
			BarStyle.SetBackgroundImage(MakeBrush(Palette::Border));
			BarStyle.SetFillImage(MakeBrush(Palette::Accent));
			Bar->SetWidgetStyle(BarStyle);
			Bar->SetPercent(0.0f);
			AddH(Cell, MakeSized(*WidgetTree, Bar, 76.0f, 6.0f), FMargin(0.0f, 0.0f, 12.0f, 0.0f), VAlign_Center);

			UTextBlock* Value = MakeText(*WidgetTree, TEXT("—"), Font::Mono(12.0f, 40), Palette::TextMuted);
			AddH(Cell, MakeSized(*WidgetTree, Value, 46.0f, -1.0f), FMargin(0.0f, 0.0f, 10.0f, 0.0f), VAlign_Center);

			WheelMeterBars.Add(Bar);
			WheelMeterValues.Add(Value);
		}

		AddH(Cell, MakeSized(*WidgetTree, MakeBindingChip(RowSpec.ActionId, RowSpec.Slot), 116.0f, 36.0f));

		UVerticalBox* Column = Columns[RowSpec.Column];
		AddV(Column, MakeRow(RowSpec.Label, FString(), Cell, FString(), WheelRowHeight),
			FMargin(0.0f, Column->GetChildrenCount() == 0 ? 0.0f : 2.0f, 0.0f, 0.0f));
	}

	// Nested the way the header is, so the menu column lines up under its caption.
	UHorizontalBox* Driving = WidgetTree->ConstructWidget<UHorizontalBox>();
	AddWeighted(Driving, Columns[0], FMargin(0.0f, 0.0f, WheelColumnGap, 0.0f), VAlign_Top, WheelAxesWeight);
	AddWeighted(Driving, Columns[1], FMargin(), VAlign_Top, 1.0f);

	UHorizontalBox* Grid = WidgetTree->ConstructWidget<UHorizontalBox>();
	AddWeighted(Grid, Driving, FMargin(0.0f, 0.0f, WheelColumnGap, 0.0f), VAlign_Top, WheelDrivingWeight);
	AddWeighted(Grid, Columns[2], FMargin(), VAlign_Top, WheelMenuWeight);
	return Grid;
}

void UApexSettingsWidget::RefreshWheelDevices()
{
	if (!WheelDeviceRow)
	{
		return;
	}

	const FApexSimInputModule* Input = FApexSimInputModule::Get();
	WheelDevicesSerial = Input ? Input->GetDevicesSerial() : 0;
	WheelDeviceRow->ClearChildren();
	// A base arriving or leaving changes which profile the steppers are about.
	RefreshWheelProfile();

	const TArray<ApexDirectInput::FDeviceInfo> Devices = Input ? Input->GetDevices() : TArray<ApexDirectInput::FDeviceInfo>();
	if (Devices.IsEmpty())
	{
		UVerticalBox* Empty = WidgetTree->ConstructWidget<UVerticalBox>();
		AddV(Empty, MakeText(*WidgetTree, TEXT("No wheel detected"), Font::Body(17.0f, true), Palette::TextSecondary));
		AddV(Empty,
			Wrapped(MakeText(*WidgetTree,
				TEXT("Plug a wheel, pedals or a button box in and it appears here — the game does not need restarting. Devices already in use by another program can still be driven, but cannot play forces."),
				Font::Body(12.0f), Palette::TextMuted)),
			FMargin(0.0f, 7.0f, 0.0f, 0.0f));
		AddH(WheelDeviceRow, MakePanel(*WidgetTree, Empty, FMargin(20.0f, 14.0f),
			MakeBrush(Palette::Surface, Palette::Border, 1.0f)), FMargin(), VAlign_Fill, 1.0f);
		return;
	}

	const UApexSettingsSubsystem* Settings = GetSettings();
	const int32 ForceSlot = Settings ? Settings->GetWheelDeviceSlot() : INDEX_NONE;

	for (const ApexDirectInput::FDeviceInfo& Device : Devices)
	{
		UVerticalBox* Card = WidgetTree->ConstructWidget<UVerticalBox>();

		UTextBlock* Name = MakeText(*WidgetTree, Device.Name, Font::Body(15.0f, true), Palette::TextPrimary);
		Name->SetTextOverflowPolicy(ETextOverflowPolicy::Ellipsis);
		AddV(Card, Name);

		// The tag is what its bindings are called on the chips, so the card is
		// where a player learns to read them.
		AddV(Card, MakeText(*WidgetTree,
			FString::Printf(TEXT("%s · %d axes · %d buttons"),
				ApexDirectInput::KindTag(Device.Kind), FMath::CountBits(Device.AxisMask), Device.NumButtons),
			Font::Mono(11.0f, 40), Palette::TextMuted), FMargin(0.0f, 6.0f, 0.0f, 0.0f));

		// "Force feedback" is the wheel the steering is bound to; a second base
		// left plugged in can play forces but is not being asked to.
		const bool bDrivingForces = Device.Slot == ForceSlot && Device.bCanPlayForces;
		const TCHAR* State = bDrivingForces ? TEXT("FORCE FEEDBACK")
			: (Device.bCanPlayForces ? TEXT("FORCES AVAILABLE") : TEXT("INPUT ONLY"));
		AddV(Card, MakeLabel(*WidgetTree, State, bDrivingForces ? Palette::Live : Palette::TextSecondary),
			FMargin(0.0f, 6.0f, 0.0f, 0.0f));

		AddH(WheelDeviceRow, MakePanel(*WidgetTree, Card, FMargin(18.0f, 13.0f),
			MakeBrush(Palette::Surface, bDrivingForces ? Palette::Accent : Palette::Border, 1.0f)),
			FMargin(WheelDeviceRow->GetChildrenCount() == 0 ? 0.0f : 12.0f, 0.0f, 0.0f, 0.0f), VAlign_Fill, 1.0f);
	}
}

void UApexSettingsWidget::RefreshWheelProfile()
{
	if (!WheelProfileStepper || !WheelPeakStepper)
	{
		return;
	}
	const UApexSettingsSubsystem* Settings = GetSettings();
	const UApexSettingsSubsystem::FWheelProfileState State = Settings
		? Settings->GetWheelProfile() : UApexSettingsSubsystem::FWheelProfileState();

	WheelProfileStepper->SetValue(State.bChosen ? ApexWheelProfiles::IndexOf(*State.Profile) + 1 : 0);
	WheelPeakStepper->SetValue(State.bPeakGiven ? FMath::RoundToInt(State.PeakTorqueNm) : 0);
	WheelProfileStepper->RefreshReadout();
	WheelPeakStepper->RefreshReadout();
	WheelProfileStepper->SetIsEnabled(State.bHasBase);
	WheelPeakStepper->SetIsEnabled(State.bHasBase);

	if (WheelProfileNote)
	{
		// What the profile does, in the terms the sliders above are in.
		FString Note = TEXT("—");
		if (State.bHasBase)
		{
			const float Scale = ApexWheelProfiles::OutputScale(State.PeakTorqueNm);
			const float MinForce = ApexWheelProfiles::MinimumForce(State.Profile->Drive);
			Note = FString::Printf(TEXT("%s · forces at %d %% of the base"),
				ApexWheelProfiles::DriveName(State.Profile->Drive), FMath::RoundToInt(Scale * 100.0f));
			if (MinForce > 0.0f)
			{
				Note += FString::Printf(TEXT(" · minimum %d %%"), FMath::RoundToInt(MinForce * 100.0f));
			}
		}
		WheelProfileNote->SetText(FText::FromString(Note));
	}
}

void UApexSettingsWidget::HandleWheelProfileStepped(UApexStepperWidget* Stepper, int32 Value)
{
	if (bRefreshing || !Stepper) { return; }
	UApexSettingsSubsystem* Settings = GetSettings();
	if (!Settings) { return; }

	if (Stepper == WheelProfileStepper)
	{
		const TConstArrayView<ApexWheelProfiles::FProfile> Profiles = ApexWheelProfiles::All();
		Settings->SetWheelProfile(Value > 0 && Value <= Profiles.Num() ? FString(Profiles[Value - 1].Id) : FString());
	}
	else if (Stepper == WheelPeakStepper)
	{
		Settings->SetWheelPeakTorque(static_cast<float>(Value));
	}
	// The peak's Auto read-out follows the profile, and the note follows both.
	RefreshWheelProfile();
}

void UApexSettingsWidget::RefreshWheelMeters()
{
	static const TArray<FMeterSpec> Meters = {
		{ TEXT("Steering"), ApexInput::Actions::Steer,    true  },
		{ TEXT("Throttle"), ApexInput::Actions::Throttle, false },
		{ TEXT("Brake"),    ApexInput::Actions::Brake,    false },
	};

	const UApexSettingsSubsystem* Settings = GetSettings();
	const FApexSimInputModule* Input = FApexSimInputModule::Get();
	if (!Settings || !Input)
	{
		return;
	}

	for (int32 Index = 0; Index < Meters.Num(); ++Index)
	{
		if (!WheelMeterBars.IsValidIndex(Index) || !WheelMeterBars[Index] || !WheelMeterValues[Index])
		{
			continue;
		}

		const FMeterSpec& Meter = Meters[Index];
		const FKey Key = Settings->GetBoundKey(Meter.ActionId, ApexInput::Slot::Wheel);
		const ApexDirectInput::FControl Control = ApexDirectInput::ParseKey(Key);
		const bool bLive = Control.IsValid() && Input->IsAttached(Control.Slot);

		// What the car will be given, not what the device reports: the
		// binding's own inversion and a pedal's fold into 0..1 are exactly
		// what a meter is here to prove.
		float Value = bLive ? Input->GetControlValue(Control) : 0.0f;
		if (bLive && Settings->Get())
		{
			const FApexKeyBinding* Binding = ApexInput::FindBinding(
				Settings->Get()->Bindings, Meter.ActionId, ApexInput::Slot::Wheel,
				[Input](int32 DeviceSlot) { return Input->IsAttached(DeviceSlot); });
			if (Binding && Binding->bInvert)
			{
				Value = -Value;
			}
		}

		// A pedal is folded into 0..1 the way the car will read it; steering
		// stays signed and draws from the middle of the bar. Unbound reads
		// empty — a pedal sitting at half would look like a stuck throttle.
		float Fill = Meter.bCentred ? 0.5f : 0.0f;
		FString Text = TEXT("—");
		if (bLive)
		{
			if (Meter.bCentred)
			{
				Fill = FMath::Clamp(0.5f * (Value + 1.0f), 0.0f, 1.0f);
				Text = FString::Printf(TEXT("%+d %%"), FMath::RoundToInt(Value * 100.0f));
			}
			else
			{
				Fill = FMath::Clamp(0.5f * (Value + 1.0f), 0.0f, 1.0f);
				Text = FString::Printf(TEXT("%d %%"), FMath::RoundToInt(Fill * 100.0f));
			}
		}

		WheelMeterBars[Index]->SetPercent(Fill);
		WheelMeterValues[Index]->SetText(FText::FromString(Text));
		WheelMeterValues[Index]->SetColorAndOpacity(FSlateColor(bLive ? Palette::TextPrimary : Palette::TextMuted));
	}
}

// --- Audio page -------------------------------------------------------------

UWidget* UApexSettingsWidget::BuildAudioPage()
{
	UVerticalBox* Page = WidgetTree->ConstructWidget<UVerticalBox>();

	AddV(Page, MakeSectionLabel(TEXT("Volume")), FMargin(0.0f, 0.0f, 0.0f, 14.0f));

	// The AI-skill row's shape: the track fills the control cell with the
	// percentage after it. One lambda for both rows so they cannot drift.
	auto MakeVolumeCell = [this](
		TObjectPtr<USlider>& OutSlider, TObjectPtr<UProgressBar>& OutFill, TObjectPtr<UTextBlock>& OutValue) -> UWidget*
	{
		USlider* Slider = nullptr;
		UProgressBar* Fill = nullptr;

		UHorizontalBox* Cell = WidgetTree->ConstructWidget<UHorizontalBox>();
		AddH(Cell, MakeSliderTrack(*WidgetTree, Slider, Fill), FMargin(), VAlign_Center, 1.0f);
		OutValue = MakeText(*WidgetTree, TEXT("0 %"), Font::Mono(13.0f, 40), Palette::TextPrimary);
		AddH(Cell, OutValue, FMargin(16.0f, 0.0f, 0.0f, 0.0f));

		OutSlider = Slider;
		OutFill = Fill;
		return MakeSized(*WidgetTree, Cell, SliderCellWidth, -1.0f);
	};

	UWidget* MasterCell = MakeVolumeCell(MasterVolumeSlider, MasterVolumeFill, MasterVolumeValue);
	MasterVolumeSlider->OnValueChanged.AddDynamic(this, &UApexSettingsWidget::HandleMasterVolumeChanged);
	AddV(Page, MakeRow(
		TEXT("Master volume"),
		TEXT("Everything the game plays."),
		MasterCell));

	UWidget* UiCell = MakeVolumeCell(UiVolumeSlider, UiVolumeFill, UiVolumeValue);
	UiVolumeSlider->OnValueChanged.AddDynamic(this, &UApexSettingsWidget::HandleUiVolumeChanged);
	AddV(Page, MakeRow(
		TEXT("Menu sounds"),
		TEXT("The ticks and chimes as you move through the menus. Moving this plays one."),
		UiCell), FMargin(0.0f, 2.0f, 0.0f, 0.0f));

	AddV(Page, MakeSectionLabel(TEXT("In the car")), FMargin(0.0f, 26.0f, 0.0f, 14.0f));

	UWidget* EngineCell = MakeVolumeCell(EngineVolumeSlider, EngineVolumeFill, EngineVolumeValue);
	EngineVolumeSlider->OnValueChanged.AddDynamic(this, &UApexSettingsWidget::HandleEngineVolumeChanged);
	AddV(Page, MakeRow(
		TEXT("Engines"),
		TEXT("Your engine and everybody else's: exhaust, induction, gearbox whine, the pops on the overrun."),
		EngineCell));

	UWidget* OthersCell = MakeVolumeCell(OtherCarsVolumeSlider, OtherCarsVolumeFill, OtherCarsVolumeValue);
	OtherCarsVolumeSlider->OnValueChanged.AddDynamic(this, &UApexSettingsWidget::HandleOtherCarsVolumeChanged);
	AddV(Page, MakeRow(
		TEXT("Other cars"),
		TEXT("Everybody else's engine against your own. They fade with distance either way; this is the car alongside."),
		OthersCell), FMargin(0.0f, 2.0f, 0.0f, 0.0f));

	UWidget* RoadCell = MakeVolumeCell(RoadVolumeSlider, RoadVolumeFill, RoadVolumeValue);
	RoadVolumeSlider->OnValueChanged.AddDynamic(this, &UApexSettingsWidget::HandleRoadVolumeChanged);
	AddV(Page, MakeRow(
		TEXT("Tyres and road"),
		TEXT("Tyre squeal, kerbs, grass and gravel, bumps, contact and the wind. Turn it up to hear the grip go."),
		RoadCell), FMargin(0.0f, 2.0f, 0.0f, 0.0f));

	AddV(Page, WidgetTree->ConstructWidget<UVerticalBox>(), FMargin(), HAlign_Fill, 1.0f);
	return Page;
}

UWidget* UApexSettingsWidget::BuildBindingsGrid()
{
	// One row per label, carrying whichever of its slots exist. Steering has a
	// device axis and two keyboard halves, so the rows are not all the same
	// shape — grouping by label is what makes that readable.
	struct FRowSpec
	{
		const TCHAR* Label;
		FName ActionId;
		/** -1 for a cell this control does not have. */
		int32 DeviceSlot;
		int32 KeyboardSlot;
		/** Heading shown above the first row of each group. */
		const TCHAR* Group;
		/** Which of the two columns the group sits in. */
		bool bRightColumn;
	};

	static const TArray<FRowSpec> RowSpecs = {
		{ TEXT("Throttle"),     ApexInput::Actions::Throttle,      0,  1, TEXT("Driving"),         false },
		{ TEXT("Brake"),        ApexInput::Actions::Brake,         0,  1, TEXT("Driving"),         false },
		{ TEXT("Steer axis"),   ApexInput::Actions::Steer,         0, -1, TEXT("Driving"),         false },
		{ TEXT("Steer left"),   ApexInput::Actions::Steer,        -1,  2, TEXT("Driving"),         false },
		{ TEXT("Steer right"),  ApexInput::Actions::Steer,        -1,  3, TEXT("Driving"),         false },
		{ TEXT("Shift up"),     ApexInput::Actions::GearUp,        0,  1, TEXT("Gears"),           false },
		{ TEXT("Shift down"),   ApexInput::Actions::GearDown,      0,  1, TEXT("Gears"),           false },
		{ TEXT("DRS"),          ApexInput::Actions::Drs,           0,  1, TEXT("Energy and DRS"),  false },
		{ TEXT("ERS mode"),     ApexInput::Actions::ErsMode,       0,  1, TEXT("Energy and DRS"),  false },
		{ TEXT("Overtake"),     ApexInput::Actions::ErsBoost,      0,  1, TEXT("Energy and DRS"),  false },
		{ TEXT("Camera"),       ApexInput::Actions::ToggleCamera,  0,  1, TEXT("View"),            true  },
		{ TEXT("Look axis"),    ApexInput::Actions::Look,          0, -1, TEXT("View"),            true  },
		{ TEXT("Look left"),    ApexInput::Actions::Look,         -1,  2, TEXT("View"),            true  },
		{ TEXT("Look right"),   ApexInput::Actions::Look,         -1,  3, TEXT("View"),            true  },
		{ TEXT("Look behind"),  ApexInput::Actions::LookBack,      0,  1, TEXT("View"),            true  },
		{ TEXT("Headlights"),   ApexInput::Actions::Headlights,    0,  1, TEXT("Lights"),          true  },
		{ TEXT("Flash lights"), ApexInput::Actions::FlashLights,   0,  1, TEXT("Lights"),          true  },
		{ TEXT("Pause menu"),   ApexInput::Actions::PauseMenu,     0,  1, TEXT("Menu"),            true  },
	};

	UHorizontalBox* Grid = WidgetTree->ConstructWidget<UHorizontalBox>();
	UVerticalBox* Left = WidgetTree->ConstructWidget<UVerticalBox>();
	UVerticalBox* Right = WidgetTree->ConstructWidget<UVerticalBox>();

	const TCHAR* LeftGroup = nullptr;
	const TCHAR* RightGroup = nullptr;

	for (int32 Index = 0; Index < RowSpecs.Num(); ++Index)
	{
		const FRowSpec& RowSpec = RowSpecs[Index];

		UHorizontalBox* Chips = WidgetTree->ConstructWidget<UHorizontalBox>();
		auto AddChip = [&](int32 ChipSlot, bool bFirst)
		{
			UWidget* Cell = ChipSlot >= 0
				? static_cast<UWidget*>(MakeBindingChip(RowSpec.ActionId, ChipSlot))
				// An empty cell still occupies its column, or the keyboard chips
				// on the steering rows would slide left under the device column.
				: static_cast<UWidget*>(MakePanel(*WidgetTree, nullptr, FMargin(), MakeBrush(FLinearColor::Transparent)));

			AddH(Chips, MakeSized(*WidgetTree, Cell, 116.0f, 38.0f),
				FMargin(bFirst ? 0.0f : 6.0f, 0.0f, 0.0f, 0.0f));
		};
		AddChip(RowSpec.DeviceSlot, true);
		AddChip(RowSpec.KeyboardSlot, false);

		UVerticalBox* Column = RowSpec.bRightColumn ? Right : Left;
		const TCHAR*& CurrentGroup = RowSpec.bRightColumn ? RightGroup : LeftGroup;
		if (!CurrentGroup || FCString::Strcmp(CurrentGroup, RowSpec.Group) != 0)
		{
			CurrentGroup = RowSpec.Group;
			AddV(Column, MakeSectionLabel(RowSpec.Group),
				FMargin(0.0f, Column->GetChildrenCount() == 0 ? 0.0f : 22.0f, 0.0f, 8.0f));
		}
		AddV(Column, MakeRow(RowSpec.Label, FString(), Chips, FString(), BindingRowHeight),
			FMargin(0.0f, 2.0f, 0.0f, 0.0f));
	}

	AddH(Grid, Left, FMargin(0.0f, 0.0f, 14.0f, 0.0f), VAlign_Top, 1.0f);
	AddH(Grid, Right, FMargin(), VAlign_Top, 1.0f);
	return Grid;
}

UApexButtonWidget* UApexSettingsWidget::MakeBindingChip(FName ActionId, int32 ChipSlot)
{
	UApexButtonWidget* Chip = WidgetTree->ConstructWidget<UApexButtonWidget>();

	FApexButtonSpec Spec;
	Spec.Label = TEXT("—");
	Spec.Variant = EApexButtonVariant::Ghost;
	Spec.bCentreLabel = true;
	Spec.Height = 38.0f;
	Spec.LabelSize = 13.0f;
	Spec.ActionId = MakeBindingId(ActionId, ChipSlot);
	Chip->Setup(Spec);
	Chip->OnActivated.AddDynamic(this, &UApexSettingsWidget::HandleBindingActivated);

	BindingChips.Add(Chip);
	return Chip;
}

// --- Open / close -----------------------------------------------------------

void UApexSettingsWidget::Open(EApexSettingsTab Tab)
{
	bOpen = true;
	SetVisibility(ESlateVisibility::Visible);

	// Opened from the main menu there is no race to go back to.
	if (FooterBackButton)
	{
		const UApexRootWidget* Root = GetTypedOuter<UApexRootWidget>();
		FooterBackButton->SetLabel(Root && Root->IsRaceViewActive() ? TEXT("Back to race") : TEXT("Done"));
	}

	if (UApexSettingsSubsystem* Settings = GetSettings())
	{
		Settings->ResetChangeCount();
	}

	ShowTab(Tab);
	RefreshFromSettings();
	FocusDefault();
}

void UApexSettingsWidget::FocusDefault()
{
	const int32 TabIndex = static_cast<int32>(CurrentTab);
	if (!(RailButtons.IsValidIndex(TabIndex) && ApexNav::Focus(RailButtons[TabIndex])))
	{
		SetKeyboardFocus();
	}
}

bool UApexSettingsWidget::HandleNavigation(EUINavigation Direction, UWidget* Source)
{
	// Tab and the shoulders walk the pages from anywhere on the panel, which
	// is the only way to reach them without crossing back to the rail.
	if (ApexNav::IsSequential(Direction))
	{
		const int32 Step = Direction == EUINavigation::Next ? 1 : TabCount - 1;
		ShowTab(static_cast<EApexSettingsTab>((static_cast<int32>(CurrentTab) + Step) % TabCount));
		FocusDefault();
		return true;
	}

	// The pages are rows of pills, sliders and bindings in a scroll box, where
	// Slate's own search stops at a row of pills and the box's edge.
	return ApexNav::MoveToward(this, Direction, Source);
}

bool UApexSettingsWidget::HandleBack()
{
	Close();
	return true;
}

void UApexSettingsWidget::Close()
{
	if (!bOpen)
	{
		return;
	}
	bOpen = false;

	// A rebind left listening would swallow the first key of whatever comes next.
	if (bListening)
	{
		FinishListening(FKey(), /*bCancelled*/ true);
	}

	// "Saved on close", as the header promises: every change has already been
	// applied, and this is the only point the slot is written.
	if (UApexSettingsSubsystem* Settings = GetSettings())
	{
		Settings->Save();
	}

	SetVisibility(ESlateVisibility::Collapsed);
	OnClosed.Broadcast();
}

void UApexSettingsWidget::ShowTab(EApexSettingsTab Tab)
{
	CurrentTab = Tab;

	if (PageHost)
	{
		PageHost->SetActiveWidgetIndex(static_cast<int32>(Tab));
	}
	for (int32 Index = 0; Index < RailButtons.Num(); ++Index)
	{
		if (RailButtons[Index])
		{
			RailButtons[Index]->SetSelected(Index == static_cast<int32>(Tab));
		}
	}

	RefreshHeaderContext();
	RefreshFooter();
}

// --- Refresh ----------------------------------------------------------------

void UApexSettingsWidget::RefreshFromSettings()
{
	const UApexSettingsSubsystem* Subsystem = GetSettings();
	const UApexSettingsSave* Values = Subsystem ? Subsystem->Get() : nullptr;
	if (!Values)
	{
		return;
	}

	// Every setter below is a write that would otherwise be reported as a change
	// the player made.
	TGuardValue<bool> Guard(bRefreshing, true);

	auto SetSegment = [this](FName Id, int32 Index)
	{
		if (TObjectPtr<UApexSegmentedWidget>* Found = Segments.Find(Id))
		{
			if (*Found)
			{
				(*Found)->SetSelectedIndex(Index);
			}
		}
	};

	SetSegment(SegTraction, static_cast<int32>(Values->TractionControl));
	SetSegment(SegAbs, Values->bAbs ? 1 : 0);
	SetSegment(SegGearbox, Values->bAutoGearbox ? 1 : 0);
	SetSegment(SegSteering, Values->bSteeringAssist ? 1 : 0);
	SetSegment(SegRacingLine, static_cast<int32>(Values->RacingLine));
	RefreshAssistPreset();
	RefreshAssistLocks();
	SetSegment(SegUnits, static_cast<int32>(Values->Units));
	SetSegment(SegHud, static_cast<int32>(Values->HudDetail));
	SetSegment(SegPreset, static_cast<int32>(Values->Preset));
	SetSegment(SegVSync, Values->bVSync ? 1 : 0);
	SetSegment(SegStartView, Values->bStartInCockpit ? 1 : 0);
	SetSegment(SegChaseView, FMath::Clamp(Values->ChaseViewLevel, 0, ApexChase::Num() - 1));
	SetSegment(SegCockpitCar, Values->bCockpitShowCar ? 1 : 0);
	SetSegment(SegCockpitWheel, Values->bCockpitWheel ? 1 : 0);
	SetSegment(SegCockpitMirrors, Values->bCockpitMirrors ? 1 : 0);
	SetSegment(SegVirtualMirror, Values->bVirtualMirror ? 1 : 0);
	SetSegment(SegMirrorQuality, FMath::Clamp(Values->MirrorQuality, 0, 2));
	SetSegment(SegWheelDirection, Values->bWheelInvertForce ? 1 : 0);

	auto SetSlider = [](USlider* Slider, UProgressBar* Fill, UTextBlock* Text,
		float Value, float Min, float Max, const FString& Display)
	{
		const float Alpha = FMath::Clamp((Value - Min) / FMath::Max(KINDA_SMALL_NUMBER, Max - Min), 0.0f, 1.0f);
		if (Slider) { Slider->SetValue(Alpha); }
		if (Fill)   { Fill->SetPercent(Alpha); }
		if (Text)   { Text->SetText(FText::FromString(Display)); }
	};

	{
		const UApexMenuFlowSubsystem* Flow = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexMenuFlowSubsystem>() : nullptr;
		const int32 Level = Flow ? ApexAiSkill::Clamp(Flow->CreateAiSkill) : ApexAiSkill::Mixed;
		SetSlider(AiSkillSlider, AiSkillFill, AiSkillValue, static_cast<float>(AiSkillStep(Level)), 0.0f, AiSkillSteps,
			AiSkillText(Level));
	}
	SetSlider(MotionBlurSlider, MotionBlurFill, MotionBlurValue, Values->MotionBlur, 0.0f, 1.0f,
		FString::FromInt(FMath::RoundToInt(Values->MotionBlur * 100.0f)));
	SetSlider(FovSlider, FovFill, FovValue, Values->FieldOfView, FovMin, FovMax,
		FString::Printf(TEXT("%d°"), FMath::RoundToInt(Values->FieldOfView)));
	SetSlider(SeatForwardSlider, SeatForwardFill, SeatForwardValue, Values->SeatForwardCm, -SeatForwardRange, SeatForwardRange,
		SignedCm(Values->SeatForwardCm));
	SetSlider(SeatHeightSlider, SeatHeightFill, SeatHeightValue, Values->SeatHeightCm, -SeatHeightRange, SeatHeightRange,
		SignedCm(Values->SeatHeightCm));
	SetSlider(ViewPitchSlider, ViewPitchFill, ViewPitchValue, Values->ViewPitchDeg, -ViewPitchRange, ViewPitchRange,
		SignedDegrees(Values->ViewPitchDeg));
	SetSlider(HorizonLockSlider, HorizonLockFill, HorizonLockValue, Values->HorizonLock, 0.0f, 1.0f, Percent(Values->HorizonLock));
	SetSlider(HeadMotionSlider, HeadMotionFill, HeadMotionValue, Values->HeadMotion, 0.0f, 1.0f, Percent(Values->HeadMotion));
	SetSlider(LookToApexSlider, LookToApexFill, LookToApexValue, Values->LookToApex, 0.0f, 1.0f, Percent(Values->LookToApex));
	SetSlider(SteeringSlider, SteeringFill, SteeringValue, Values->SteeringSensitivity, 0.0f, 1.0f,
		FString::FromInt(FMath::RoundToInt(Values->SteeringSensitivity * 100.0f)));
	SetSlider(DeadzoneSlider, DeadzoneFill, DeadzoneValue, Values->Deadzone, 0.0f, 0.5f,
		FString::FromInt(FMath::RoundToInt(Values->Deadzone * 100.0f)));
	SetSlider(VibrationSlider, VibrationFill, VibrationValue, Values->Vibration, 0.0f, 1.0f,
		FString::FromInt(FMath::RoundToInt(Values->Vibration * 100.0f)));
	SetSlider(WheelForceSlider, WheelForceFill, WheelForceValue, Values->WheelForce, 0.0f, 1.0f,
		Percent(Values->WheelForce));
	SetSlider(WheelRoadSlider, WheelRoadFill, WheelRoadValue, Values->WheelRoadEffects, 0.0f, 1.0f,
		Percent(Values->WheelRoadEffects));
	SetSlider(WheelDampingSlider, WheelDampingFill, WheelDampingValue, Values->WheelDamping, 0.0f, 1.0f,
		Percent(Values->WheelDamping));
	SetSlider(WheelRotationSlider, WheelRotationFill, WheelRotationValue, Values->WheelRotationDeg,
		ApexInput::WheelRotationMinDeg, ApexInput::WheelRotationMaxDeg,
		FString::Printf(TEXT("%d°"), FMath::RoundToInt(Values->WheelRotationDeg)));
	SetSlider(WheelSteeringLockSlider, WheelSteeringLockFill, WheelSteeringLockValue,
		Values->bWheelSteeringLockAuto ? SteeringLockSliderMinDeg : Values->WheelSteeringLockDeg,
		SteeringLockSliderMinDeg, ApexInput::SteeringLockMaxDeg,
		SteeringLockSettingText(Values->bWheelSteeringLockAuto, Values->WheelSteeringLockDeg, Values->WheelRotationDeg));
	RefreshWheelProfile();
	SetSlider(MasterVolumeSlider, MasterVolumeFill, MasterVolumeValue, Values->MasterVolume, 0.0f, 1.0f,
		FString::Printf(TEXT("%d %%"), FMath::RoundToInt(Values->MasterVolume * 100.0f)));
	SetSlider(UiVolumeSlider, UiVolumeFill, UiVolumeValue, Values->UiVolume, 0.0f, 1.0f,
		FString::Printf(TEXT("%d %%"), FMath::RoundToInt(Values->UiVolume * 100.0f)));
	SetSlider(EngineVolumeSlider, EngineVolumeFill, EngineVolumeValue, Values->EngineVolume, 0.0f, 1.0f,
		FString::Printf(TEXT("%d %%"), FMath::RoundToInt(Values->EngineVolume * 100.0f)));
	SetSlider(OtherCarsVolumeSlider, OtherCarsVolumeFill, OtherCarsVolumeValue, Values->OtherCarsVolume, 0.0f, 1.0f,
		FString::Printf(TEXT("%d %%"), FMath::RoundToInt(Values->OtherCarsVolume * 100.0f)));
	SetSlider(RoadVolumeSlider, RoadVolumeFill, RoadVolumeValue, Values->RoadVolume, 0.0f, 1.0f,
		FString::Printf(TEXT("%d %%"), FMath::RoundToInt(Values->RoadVolume * 100.0f)));

	SetSegment(SegScreens, Values->Screens == 3 ? 1 : 0);
	SetSlider(TripleWidthSlider, TripleWidthFill, TripleWidthValue, Values->TripleScreenWidthCm,
		ApexMultiView::MinScreenWidthCm, ApexMultiView::MaxScreenWidthCm, Centimetres(Values->TripleScreenWidthCm));
	SetSlider(TripleBezelSlider, TripleBezelFill, TripleBezelValue, Values->TripleBezelCm,
		ApexMultiView::MinBezelCm, ApexMultiView::MaxBezelCm, TenthCentimetres(Values->TripleBezelCm));
	SetSlider(TripleDistanceSlider, TripleDistanceFill, TripleDistanceValue, Values->TripleEyeDistanceCm,
		ApexMultiView::MinEyeDistanceCm, ApexMultiView::MaxEyeDistanceCm, Centimetres(Values->TripleEyeDistanceCm));
	SetSlider(TripleAngleSlider, TripleAngleFill, TripleAngleValue, Values->TripleSideAngleDeg,
		ApexMultiView::MinSideAngleDeg, ApexMultiView::MaxSideAngleDeg, Degrees(Values->TripleSideAngleDeg));
	if (TripleGeometryBox)
	{
		TripleGeometryBox->SetIsEnabled(Values->Screens == 3);
	}
	RefreshTripleNote();

	if (DisplayModeBox)
	{
		DisplayModeBox->SetSelectedIndex(IndexOfOr0(DisplayModeValues, Values->DisplayMode));
	}
	if (ResolutionBox && Subsystem)
	{
		const int32 Index = Subsystem->GetAvailableResolutions().IndexOfByKey(Values->Resolution);
		ResolutionBox->SetSelectedOption(ResolutionName(Values->Resolution));
		if (Index == INDEX_NONE)
		{
			// The stored mode is not one the display offers any more. Showing it
			// anyway is better than silently jumping to something else.
			ResolutionBox->AddOption(ResolutionName(Values->Resolution));
			ResolutionBox->SetSelectedOption(ResolutionName(Values->Resolution));
		}
	}
	if (FrameLimitBox)
	{
		FrameLimitBox->SetSelectedIndex(IndexOfOr0(FrameLimitValues, Values->FrameLimit));
	}
	if (ShadowsBox)
	{
		ShadowsBox->SetSelectedIndex(FMath::Clamp(Values->ShadowQuality, 0, QualityNames.Num() - 1));
	}
	if (AntiAliasingBox)
	{
		AntiAliasingBox->SetSelectedIndex(FMath::Clamp(Values->AntiAliasingQuality, 0, AntiAliasingNames.Num() - 1));
	}
	if (TexturesBox)
	{
		TexturesBox->SetSelectedIndex(FMath::Clamp(Values->TextureQuality, 0, QualityNames.Num() - 1));
	}

	if (GamepadStateText)
	{
		const bool bAttached = FSlateApplication::IsInitialized() && FSlateApplication::Get().IsGamepadAttached();
		GamepadStateText->SetText(FText::FromString(bAttached ? TEXT("CONNECTED") : TEXT("NOT DETECTED")));
		GamepadStateText->SetColorAndOpacity(FSlateColor(bAttached ? Palette::Live : Palette::TextMuted));
	}

	if (DeviceCountText)
	{
		const FApexSimInputModule* Input = FApexSimInputModule::Get();
		const int32 Count = Input ? Input->GetDevices().Num() : 0;
		DeviceCountText->SetText(FText::FromString(Count == 0
			? TEXT("NOT DETECTED")
			: FString::Printf(TEXT("%d ATTACHED · WHEEL PAGE"), Count)));
		DeviceCountText->SetColorAndOpacity(FSlateColor(Count > 0 ? Palette::Live : Palette::TextMuted));
	}

	RefreshBindingChips();
	RefreshWheelDevices();
	RefreshWheelMeters();
	RefreshHeaderContext();
	RefreshFooter();
}

void UApexSettingsWidget::RefreshBindingChips()
{
	const UApexSettingsSubsystem* Settings = GetSettings();
	if (!Settings)
	{
		return;
	}

	for (UApexButtonWidget* Chip : BindingChips)
	{
		if (!Chip)
		{
			continue;
		}

		FName ActionId;
		int32 ChipSlot = 0;
		if (!ParseBindingId(Chip->GetActionId(), ActionId, ChipSlot))
		{
			continue;
		}

		const FKey Key = Settings->GetBoundKey(ActionId, ChipSlot);
		const bool bConflicted = Settings->FindConflicts(Key, ActionId, ChipSlot).Num() > 0;

		FApexButtonSpec Spec;
		Spec.Label = ApexInput::GetKeyDisplayName(Key);
		Spec.Variant = EApexButtonVariant::Ghost;
		Spec.bCentreLabel = true;
		Spec.Height = 38.0f;
		Spec.LabelSize = 13.0f;
		Spec.ActionId = Chip->GetActionId();
		// A conflict is not an error the screen refuses — two controls can share
		// a key deliberately — so it is flagged rather than blocked.
		Spec.LabelColour = bConflicted ? Palette::Error : (Key.IsValid() ? Palette::TextPrimary : Palette::TextMuted);
		Chip->Setup(Spec);
	}
}

void UApexSettingsWidget::RefreshHeaderContext()
{
	if (!HeaderContextText)
	{
		return;
	}

	FString Context;
	switch (CurrentTab)
	{
	case EApexSettingsTab::Gameplay:
		Context = TEXT("Applies immediately · saved on close");
		break;

	case EApexSettingsTab::Assists:
	{
		const UGameInstance* GameInstance = GetGameInstance();
		const UApexNetSubsystem* Net = GameInstance ? GameInstance->GetSubsystem<UApexNetSubsystem>() : nullptr;
		const int32 Locked = Net && Net->IsInSession() ? Net->GetAllowedAssists().CountLocked() : 0;
		Context = Locked == 0
			? TEXT("Applies immediately · saved on close")
			: Locked == 1 ? TEXT("1 assist locked by the host · saved on close")
			              : FString::Printf(TEXT("%d assists locked by the host · saved on close"), Locked);
		break;
	}

	case EApexSettingsTab::Graphics:
		if (const UWorld* World = GetWorld())
		{
			const float Delta = World->GetDeltaSeconds();
			const int32 Fps = Delta > KINDA_SMALL_NUMBER ? FMath::RoundToInt(1.0f / Delta) : 0;
			Context = FString::Printf(TEXT("%d fps"), Fps);
		}
		break;

	case EApexSettingsTab::Camera:
		Context = TEXT("Live preview · saved on close");
		break;

	case EApexSettingsTab::Controls:
	{
		const bool bAttached = FSlateApplication::IsInitialized() && FSlateApplication::Get().IsGamepadAttached();
		Context = bAttached ? TEXT("2 devices detected") : TEXT("1 device detected");
		break;
	}

	case EApexSettingsTab::Wheel:
	{
		const FApexSimInputModule* Input = FApexSimInputModule::Get();
		const int32 Count = Input ? Input->GetDevices().Num() : 0;
		Context = Count == 1 ? TEXT("1 device attached") : FString::Printf(TEXT("%d devices attached"), Count);
		break;
	}

	case EApexSettingsTab::Audio:
		Context = TEXT("Applies immediately · saved on close");
		break;

	}

	HeaderContextText->SetText(FText::FromString(Context.ToUpper()));
}

void UApexSettingsWidget::RefreshFooter()
{
	const UApexSettingsSubsystem* Settings = GetSettings();
	if (!FooterStatusText || !Settings)
	{
		return;
	}

	FString Status;
	if (CurrentTab == EApexSettingsTab::Graphics)
	{
		// Worth saying on the page it applies to: the resolution row is the one
		// control here whose effect is not immediate.
		Status = TEXT("Resolution changes take effect on the next launch");
	}
	else
	{
		const int32 Changes = Settings->GetChangeCount();
		Status = Changes == 1
			? TEXT("1 change this session")
			: FString::Printf(TEXT("%d changes this session"), Changes);
	}

	FooterStatusText->SetText(FText::FromString(Status.ToUpper()));
}

// --- Handlers ---------------------------------------------------------------

void UApexSettingsWidget::HandleRailActivated(UApexButtonWidget* Button)
{
	if (!Button)
	{
		return;
	}

	const FName Action = Button->GetActionId();
	if (Action == ActionTabGameplay)      { ShowTab(EApexSettingsTab::Gameplay); }
	else if (Action == ActionTabAssists)  { ShowTab(EApexSettingsTab::Assists); }
	else if (Action == ActionTabGraphics) { ShowTab(EApexSettingsTab::Graphics); }
	else if (Action == ActionTabCamera)   { ShowTab(EApexSettingsTab::Camera); }
	else if (Action == ActionTabControls) { ShowTab(EApexSettingsTab::Controls); }
	else if (Action == ActionTabWheel)    { ShowTab(EApexSettingsTab::Wheel); }
	else if (Action == ActionTabAudio)    { ShowTab(EApexSettingsTab::Audio); }
}

void UApexSettingsWidget::HandleFooterActivated(UApexButtonWidget* Button)
{
	if (!Button)
	{
		return;
	}

	if (Button->GetActionId() == ActionSettingsBack)
	{
		Close();
		return;
	}

	if (Button->GetActionId() == ActionEditHud)
	{
		OnEditHudLayout.Broadcast();
		return;
	}

	if (Button->GetActionId() == ActionReset)
	{
		if (UApexSettingsSubsystem* Settings = GetSettings())
		{
			// Only the page in front of the player: a reset button under the
			// controls page must not throw away their graphics settings.
			Settings->ResetToDefaults(static_cast<EApexSettingsGroup>(CurrentTab));
		}
		if (CurrentTab == EApexSettingsTab::Gameplay)
		{
			// The AI level lives on the profile, not in the settings slot.
			if (UApexMenuFlowSubsystem* Flow = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexMenuFlowSubsystem>() : nullptr)
			{
				Flow->CreateAiSkill = ApexAiSkill::Mixed;
				Flow->SaveProfile();
			}
		}
		RefreshFromSettings();
	}
}

void UApexSettingsWidget::HandleSegmentChosen(UApexSegmentedWidget* Control, int32 Index)
{
	UApexSettingsSubsystem* Settings = GetSettings();
	if (bRefreshing || !Control || !Settings)
	{
		return;
	}

	const FName Id = Control->ControlId;
	if (Id == SegAssistPreset)
	{
		// CUSTOM is a read-out, not a choice: put the pills back where they were.
		if (Index == AssistPresetCustom)
		{
			RefreshAssistPreset();
		}
		else
		{
			ApplyAssistPreset(Index);
		}
		return;
	}
	else if (Id == SegTraction)   { Settings->SetTractionControl(static_cast<EApexAssistLevel>(Index)); RefreshAssistPreset(); }
	else if (Id == SegAbs)        { Settings->SetAbs(Index == 1); RefreshAssistPreset(); }
	else if (Id == SegGearbox)    { Settings->SetAutoGearbox(Index == 1); RefreshAssistPreset(); }
	else if (Id == SegSteering)   { Settings->SetSteeringAssist(Index == 1); RefreshAssistPreset(); }
	else if (Id == SegRacingLine) { Settings->SetRacingLine(static_cast<EApexRacingLine>(Index)); RefreshAssistPreset(); }
	else if (Id == SegUnits)      { Settings->SetUnits(static_cast<EApexUnits>(Index)); }
	else if (Id == SegHud)        { Settings->SetHudDetail(static_cast<EApexHudDetail>(Index)); }
	else if (Id == SegVSync)      { Settings->SetVSync(Index == 1); }
	else if (Id == SegScreens)
	{
		Settings->SetScreens(Index == 1 ? 3 : 1);
		// The rig's rows wake up or dim with it.
		RefreshFromSettings();
		RefreshFooter();
		return;
	}
	else if (Id == SegStartView)      { Settings->SetStartInCockpit(Index == 1); }
	// Stored, not switched to: the race director adopts it with the rest of
	// the camera group, so picking a distance while sitting in the cockpit
	// does not throw the player out of it.
	else if (Id == SegChaseView)     { Settings->SetChaseLevel(FMath::Clamp(Index, 0, ApexChase::Num() - 1)); }
	else if (Id == SegCockpitCar)     { Settings->SetCockpitShowCar(Index == 1); }
	else if (Id == SegCockpitWheel)   { Settings->SetCockpitWheel(Index == 1); }
	else if (Id == SegCockpitMirrors) { Settings->SetCockpitMirrors(Index == 1); }
	else if (Id == SegVirtualMirror)  { Settings->SetVirtualMirror(Index == 1); }
	else if (Id == SegMirrorQuality)  { Settings->SetMirrorQuality(Index); }
	else if (Id == SegWheelDirection)
	{
		Settings->SetWheelInvertForce(Index == 1);
		// Push again the moment it is flipped, so the answer to "which way?" is
		// immediate rather than one more button press away.
		if (AApexPlayerController* PlayerController = Cast<AApexPlayerController>(GetOwningPlayer()))
		{
			PlayerController->TestWheelForce();
		}
	}
	else if (Id == SegPreset)
	{
		Settings->SetGraphicsPreset(static_cast<EApexGraphicsPreset>(Index));
		// A preset moves the quality dropdowns under it.
		RefreshFromSettings();
		return;
	}

	RefreshFooter();
}

void UApexSettingsWidget::HandleAiSkillChanged(float Value)
{
	if (bRefreshing) { return; }
	ApexUiAudio::Play(this, EApexUiSound::Adjust);

	const int32 Step = FMath::RoundToInt(Value * AiSkillSteps);
	const int32 Level = Step <= 0 ? ApexAiSkill::Mixed : ApexAiSkill::Min + Step - 1;
	if (AiSkillFill) { AiSkillFill->SetPercent(Value); }
	if (AiSkillValue) { AiSkillValue->SetText(FText::FromString(AiSkillText(Level))); }
	if (UApexMenuFlowSubsystem* Flow = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexMenuFlowSubsystem>() : nullptr)
	{
		if (Flow->CreateAiSkill != Level)
		{
			Flow->CreateAiSkill = Level;
			Flow->SaveProfile();
		}
	}
	RefreshFooter();
}

void UApexSettingsWidget::HandleMotionBlurChanged(float Value)
{
	if (bRefreshing) { return; }
	ApexUiAudio::Play(this, EApexUiSound::Adjust);
	if (MotionBlurFill) { MotionBlurFill->SetPercent(Value); }
	if (MotionBlurValue) { MotionBlurValue->SetText(FText::FromString(FString::FromInt(FMath::RoundToInt(Value * 100.0f)))); }
	if (UApexSettingsSubsystem* Settings = GetSettings()) { Settings->SetMotionBlur(Value); }
	RefreshFooter();
}

void UApexSettingsWidget::HandleTripleWidthChanged(float Value)
{
	if (bRefreshing) { return; }
	const float Cm = FMath::Lerp(ApexMultiView::MinScreenWidthCm, ApexMultiView::MaxScreenWidthCm, Value);
	if (UApexSettingsSubsystem* Settings = GetSettings()) { Settings->SetTripleScreenWidth(Cm); }
	ReflectSlider(TripleWidthFill, TripleWidthValue, Value, Centimetres(Cm));
	RefreshTripleNote();
}

void UApexSettingsWidget::HandleTripleBezelChanged(float Value)
{
	if (bRefreshing) { return; }
	const float Cm = FMath::Lerp(ApexMultiView::MinBezelCm, ApexMultiView::MaxBezelCm, Value);
	if (UApexSettingsSubsystem* Settings = GetSettings()) { Settings->SetTripleBezel(Cm); }
	ReflectSlider(TripleBezelFill, TripleBezelValue, Value, TenthCentimetres(Cm));
	RefreshTripleNote();
}

void UApexSettingsWidget::HandleTripleDistanceChanged(float Value)
{
	if (bRefreshing) { return; }
	const float Cm = FMath::Lerp(ApexMultiView::MinEyeDistanceCm, ApexMultiView::MaxEyeDistanceCm, Value);
	if (UApexSettingsSubsystem* Settings = GetSettings()) { Settings->SetTripleEyeDistance(Cm); }
	ReflectSlider(TripleDistanceFill, TripleDistanceValue, Value, Centimetres(Cm));
	RefreshTripleNote();
}

void UApexSettingsWidget::HandleTripleAngleChanged(float Value)
{
	if (bRefreshing) { return; }
	const float Deg = FMath::Lerp(ApexMultiView::MinSideAngleDeg, ApexMultiView::MaxSideAngleDeg, Value);
	if (UApexSettingsSubsystem* Settings = GetSettings()) { Settings->SetTripleSideAngle(Deg); }
	ReflectSlider(TripleAngleFill, TripleAngleValue, Value, Degrees(Deg));
	RefreshTripleNote();
}

void UApexSettingsWidget::RefreshTripleNote()
{
	const UApexSettingsSubsystem* Settings = GetSettings();
	const UApexSettingsSave* Values = Settings ? Settings->Get() : nullptr;
	if (!TripleNoteText || !Values)
	{
		return;
	}
	FString Note;
	if (Values->Screens == 3)
	{
		// What the measurements come to: the field the centre monitor gets,
		// and how far the side views are turned. The Camera page's field of
		// view does nothing while this is on, and this line says why.
		const ApexMultiView::FTripleGeometry Geometry = Settings->GetTripleGeometry();
		Note = FString::Printf(
			TEXT("Three monitors in a row, the window across all of them. The centre view is %d° wide from these "
				 "measurements (the Camera page's field of view is not used); each side view is turned %d°."),
			FMath::RoundToInt(ApexMultiView::CentreFovDeg(Geometry)), FMath::RoundToInt(Geometry.SideAngleDeg));
	}
	else
	{
		Note = TEXT("One monitor. TRIPLE stretches the window across three monitors standing in a row and "
			"draws each its own view, worked out from the measurements below.");
	}
	TripleNoteText->SetText(FText::FromString(Note));
}

void UApexSettingsWidget::HandleFovChanged(float Value)
{
	if (bRefreshing) { return; }
	const float Degrees = FMath::Lerp(FovMin, FovMax, Value);
	if (UApexSettingsSubsystem* Settings = GetSettings()) { Settings->SetFieldOfView(Degrees); }
	ReflectSlider(FovFill, FovValue, Value, FString::Printf(TEXT("%d°"), FMath::RoundToInt(Degrees)));
}

void UApexSettingsWidget::HandleSeatForwardChanged(float Value)
{
	if (bRefreshing) { return; }
	const float Cm = FMath::Lerp(-SeatForwardRange, SeatForwardRange, Value);
	if (UApexSettingsSubsystem* Settings = GetSettings()) { Settings->SetSeatForward(Cm); }
	ReflectSlider(SeatForwardFill, SeatForwardValue, Value, SignedCm(Cm));
}

void UApexSettingsWidget::HandleSeatHeightChanged(float Value)
{
	if (bRefreshing) { return; }
	const float Cm = FMath::Lerp(-SeatHeightRange, SeatHeightRange, Value);
	if (UApexSettingsSubsystem* Settings = GetSettings()) { Settings->SetSeatHeight(Cm); }
	ReflectSlider(SeatHeightFill, SeatHeightValue, Value, SignedCm(Cm));
}

void UApexSettingsWidget::HandleViewPitchChanged(float Value)
{
	if (bRefreshing) { return; }
	const float Degrees = FMath::Lerp(-ViewPitchRange, ViewPitchRange, Value);
	if (UApexSettingsSubsystem* Settings = GetSettings()) { Settings->SetViewPitch(Degrees); }
	ReflectSlider(ViewPitchFill, ViewPitchValue, Value, SignedDegrees(Degrees));
}

void UApexSettingsWidget::HandleHorizonLockChanged(float Value)
{
	if (bRefreshing) { return; }
	if (UApexSettingsSubsystem* Settings = GetSettings()) { Settings->SetHorizonLock(Value); }
	ReflectSlider(HorizonLockFill, HorizonLockValue, Value, Percent(Value));
}

void UApexSettingsWidget::HandleHeadMotionChanged(float Value)
{
	if (bRefreshing) { return; }
	if (UApexSettingsSubsystem* Settings = GetSettings()) { Settings->SetHeadMotion(Value); }
	ReflectSlider(HeadMotionFill, HeadMotionValue, Value, Percent(Value));
}

void UApexSettingsWidget::HandleLookToApexChanged(float Value)
{
	if (bRefreshing) { return; }
	if (UApexSettingsSubsystem* Settings = GetSettings()) { Settings->SetLookToApex(Value); }
	ReflectSlider(LookToApexFill, LookToApexValue, Value, Percent(Value));
}

void UApexSettingsWidget::HandleSteeringChanged(float Value)
{
	if (bRefreshing) { return; }
	ApexUiAudio::Play(this, EApexUiSound::Adjust);
	if (SteeringFill) { SteeringFill->SetPercent(Value); }
	if (SteeringValue) { SteeringValue->SetText(FText::FromString(FString::FromInt(FMath::RoundToInt(Value * 100.0f)))); }
	if (UApexSettingsSubsystem* Settings = GetSettings()) { Settings->SetSteeringSensitivity(Value); }
	RefreshFooter();
}

void UApexSettingsWidget::HandleDeadzoneChanged(float Value)
{
	if (bRefreshing) { return; }
	ApexUiAudio::Play(this, EApexUiSound::Adjust);
	if (DeadzoneFill) { DeadzoneFill->SetPercent(Value); }
	if (DeadzoneValue) { DeadzoneValue->SetText(FText::FromString(FString::FromInt(FMath::RoundToInt(Value * 100.0f)))); }
	// The slider runs 0..1 but a deadzone over half the axis is not a setting,
	// it is a broken wheel; the subsystem's range is 0..0.5.
	if (UApexSettingsSubsystem* Settings = GetSettings()) { Settings->SetDeadzone(Value * 0.5f); }
	RefreshFooter();
}

void UApexSettingsWidget::HandleVibrationChanged(float Value)
{
	if (bRefreshing) { return; }
	ApexUiAudio::Play(this, EApexUiSound::Adjust);
	if (VibrationFill) { VibrationFill->SetPercent(Value); }
	if (VibrationValue) { VibrationValue->SetText(FText::FromString(FString::FromInt(FMath::RoundToInt(Value * 100.0f)))); }
	if (UApexSettingsSubsystem* Settings = GetSettings()) { Settings->SetVibration(Value); }
	// Rumble at the new strength while the slider moves: the only honest way
	// to pick one.
	if (AApexPlayerController* PlayerController = Cast<AApexPlayerController>(GetOwningPlayer()))
	{
		PlayerController->PreviewForceFeedback();
	}
	RefreshFooter();
}

void UApexSettingsWidget::HandleWheelForceChanged(float Value)
{
	if (bRefreshing) { return; }
	if (UApexSettingsSubsystem* Settings = GetSettings()) { Settings->SetWheelForce(Value); }
	ReflectSlider(WheelForceFill, WheelForceValue, Value, Percent(Value));
}

void UApexSettingsWidget::HandleWheelRoadChanged(float Value)
{
	if (bRefreshing) { return; }
	if (UApexSettingsSubsystem* Settings = GetSettings()) { Settings->SetWheelRoadEffects(Value); }
	ReflectSlider(WheelRoadFill, WheelRoadValue, Value, Percent(Value));
}

void UApexSettingsWidget::HandleWheelDampingChanged(float Value)
{
	if (bRefreshing) { return; }
	if (UApexSettingsSubsystem* Settings = GetSettings()) { Settings->SetWheelDamping(Value); }
	ReflectSlider(WheelDampingFill, WheelDampingValue, Value, Percent(Value));
}

void UApexSettingsWidget::HandleWheelRotationChanged(float Value)
{
	if (bRefreshing) { return; }
	const float Degrees = SliderDegrees(Value, ApexInput::WheelRotationMinDeg, ApexInput::WheelRotationMaxDeg, WheelRotationStepDeg);
	UApexSettingsSubsystem* Settings = GetSettings();
	if (Settings) { Settings->SetWheelRotation(Degrees); }
	ReflectSlider(WheelRotationFill, WheelRotationValue, Value, FString::Printf(TEXT("%d°"), FMath::RoundToInt(Degrees)));
	// The lock cannot be longer than the rotation, so its read-out follows.
	if (Settings && Settings->Get() && WheelSteeringLockValue)
	{
		const UApexSettingsSave* Values = Settings->Get();
		WheelSteeringLockValue->SetText(FText::FromString(
			SteeringLockSettingText(Values->bWheelSteeringLockAuto, Values->WheelSteeringLockDeg, Degrees)));
	}
}

void UApexSettingsWidget::HandleWheelSteeringLockChanged(float Value)
{
	if (bRefreshing) { return; }
	const float Degrees = SliderDegrees(Value, SteeringLockSliderMinDeg, ApexInput::SteeringLockMaxDeg, SteeringLockStepDeg);
	const bool bAuto = Degrees < ApexInput::SteeringLockMinDeg;
	UApexSettingsSubsystem* Settings = GetSettings();
	if (Settings)
	{
		if (bAuto) { Settings->SetWheelSteeringLockAuto(true); }
		else { Settings->SetWheelSteeringLock(Degrees); }
	}
	const float Rotation = Settings && Settings->Get() ? Settings->Get()->WheelRotationDeg : ApexInput::WheelRotationMaxDeg;
	ReflectSlider(WheelSteeringLockFill, WheelSteeringLockValue, Value, SteeringLockSettingText(bAuto, Degrees, Rotation));
}

void UApexSettingsWidget::HandleWheelTestActivated(UApexButtonWidget* Button)
{
	if (AApexPlayerController* PlayerController = Cast<AApexPlayerController>(GetOwningPlayer()))
	{
		PlayerController->TestWheelForce();
	}
}

void UApexSettingsWidget::HandleMasterVolumeChanged(float Value)
{
	if (bRefreshing) { return; }
	if (MasterVolumeFill) { MasterVolumeFill->SetPercent(Value); }
	if (MasterVolumeValue) { MasterVolumeValue->SetText(FText::FromString(FString::Printf(TEXT("%d %%"), FMath::RoundToInt(Value * 100.0f)))); }
	if (UApexSettingsSubsystem* Settings = GetSettings()) { Settings->SetMasterVolume(Value); }
	// After the level is applied, so the tick previews it.
	ApexUiAudio::Play(this, EApexUiSound::Adjust);
	RefreshFooter();
}

void UApexSettingsWidget::HandleUiVolumeChanged(float Value)
{
	if (bRefreshing) { return; }
	if (UiVolumeFill) { UiVolumeFill->SetPercent(Value); }
	if (UiVolumeValue) { UiVolumeValue->SetText(FText::FromString(FString::Printf(TEXT("%d %%"), FMath::RoundToInt(Value * 100.0f)))); }
	if (UApexSettingsSubsystem* Settings = GetSettings()) { Settings->SetUiVolume(Value); }
	ApexUiAudio::Play(this, EApexUiSound::Adjust);
	RefreshFooter();
}

void UApexSettingsWidget::HandleEngineVolumeChanged(float Value)
{
	if (bRefreshing) { return; }
	if (EngineVolumeFill) { EngineVolumeFill->SetPercent(Value); }
	if (EngineVolumeValue) { EngineVolumeValue->SetText(FText::FromString(FString::Printf(TEXT("%d %%"), FMath::RoundToInt(Value * 100.0f)))); }
	if (UApexSettingsSubsystem* Settings = GetSettings()) { Settings->SetEngineVolume(Value); }
	RefreshFooter();
}

void UApexSettingsWidget::HandleOtherCarsVolumeChanged(float Value)
{
	if (bRefreshing) { return; }
	if (OtherCarsVolumeFill) { OtherCarsVolumeFill->SetPercent(Value); }
	if (OtherCarsVolumeValue) { OtherCarsVolumeValue->SetText(FText::FromString(FString::Printf(TEXT("%d %%"), FMath::RoundToInt(Value * 100.0f)))); }
	if (UApexSettingsSubsystem* Settings = GetSettings()) { Settings->SetOtherCarsVolume(Value); }
	RefreshFooter();
}

void UApexSettingsWidget::HandleRoadVolumeChanged(float Value)
{
	if (bRefreshing) { return; }
	if (RoadVolumeFill) { RoadVolumeFill->SetPercent(Value); }
	if (RoadVolumeValue) { RoadVolumeValue->SetText(FText::FromString(FString::Printf(TEXT("%d %%"), FMath::RoundToInt(Value * 100.0f)))); }
	if (UApexSettingsSubsystem* Settings = GetSettings()) { Settings->SetRoadVolume(Value); }
	RefreshFooter();
}

void UApexSettingsWidget::HandleDisplayModeChanged(FString Item, ESelectInfo::Type SelectType)
{
	if (bRefreshing || !DisplayModeBox) { return; }
	ApexUiAudio::Play(this, EApexUiSound::Adjust);
	const int32 Index = DisplayModeBox->GetSelectedIndex();
	if (UApexSettingsSubsystem* Settings = GetSettings())
	{
		Settings->SetDisplayMode(DisplayModeValues.IsValidIndex(Index) ? DisplayModeValues[Index] : 0);
	}
	RefreshFooter();
}

void UApexSettingsWidget::HandleResolutionChanged(FString Item, ESelectInfo::Type SelectType)
{
	UApexSettingsSubsystem* Settings = GetSettings();
	if (bRefreshing || !ResolutionBox || !Settings) { return; }
	ApexUiAudio::Play(this, EApexUiSound::Adjust);
	const int32 Index = ResolutionBox->GetSelectedIndex();
	const TArray<FIntPoint>& Modes = Settings->GetAvailableResolutions();
	if (Modes.IsValidIndex(Index))
	{
		Settings->SetResolution(Modes[Index]);
	}
	RefreshFooter();
}

void UApexSettingsWidget::HandleFrameLimitChanged(FString Item, ESelectInfo::Type SelectType)
{
	if (bRefreshing || !FrameLimitBox) { return; }
	ApexUiAudio::Play(this, EApexUiSound::Adjust);
	const int32 Index = FrameLimitBox->GetSelectedIndex();
	if (UApexSettingsSubsystem* Settings = GetSettings())
	{
		Settings->SetFrameLimit(FrameLimitValues.IsValidIndex(Index) ? FrameLimitValues[Index] : 0);
	}
	RefreshFooter();
}

void UApexSettingsWidget::HandleShadowsChanged(FString Item, ESelectInfo::Type SelectType)
{
	if (bRefreshing || !ShadowsBox) { return; }
	ApexUiAudio::Play(this, EApexUiSound::Adjust);
	if (UApexSettingsSubsystem* Settings = GetSettings())
	{
		Settings->SetShadowQuality(ShadowsBox->GetSelectedIndex());
	}
	// A quality row moving off the preset turns the preset to Custom.
	RefreshFromSettings();
}

void UApexSettingsWidget::HandleAntiAliasingChanged(FString Item, ESelectInfo::Type SelectType)
{
	if (bRefreshing || !AntiAliasingBox) { return; }
	ApexUiAudio::Play(this, EApexUiSound::Adjust);
	if (UApexSettingsSubsystem* Settings = GetSettings())
	{
		Settings->SetAntiAliasingQuality(AntiAliasingBox->GetSelectedIndex());
	}
	RefreshFromSettings();
}

void UApexSettingsWidget::HandleTexturesChanged(FString Item, ESelectInfo::Type SelectType)
{
	if (bRefreshing || !TexturesBox) { return; }
	ApexUiAudio::Play(this, EApexUiSound::Adjust);
	if (UApexSettingsSubsystem* Settings = GetSettings())
	{
		Settings->SetTextureQuality(TexturesBox->GetSelectedIndex());
	}
	RefreshFromSettings();
}

// --- Rebinding --------------------------------------------------------------

void UApexSettingsWidget::HandleBindingActivated(UApexButtonWidget* Button)
{
	FName ActionId;
	int32 ChipSlot = 0;
	if (Button && ParseBindingId(Button->GetActionId(), ActionId, ChipSlot))
	{
		BeginListening(ActionId, ChipSlot);
	}
}

void UApexSettingsWidget::BeginListening(FName ActionId, int32 ListenSlot)
{
	bListening = true;
	ListeningAction = ActionId;
	ListeningSlot = ListenSlot;
	ListenBaselines.Reset();

	if (ListenTitleText)
	{
		const ApexInput::FSlotDef* Def = ApexInput::FindSlot(ActionId, ListenSlot);
		const TCHAR* Label = Def ? Def->Label : TEXT("this control");
		FString Prompt;
		if (!ApexInput::IsWheelSlot(ListenSlot))
		{
			Prompt = FString::Printf(TEXT("Press any key or move an axis for %s"), Label);
		}
		else if (ApexInput::IsCentredAxisSlot(ActionId, ListenSlot))
		{
			// Which way it is moved is the answer to which way round it runs.
			Prompt = FString::Printf(TEXT("Turn the wheel RIGHT for %s"), Label);
		}
		else if (ActionId == ApexInput::Actions::MenuUp || ActionId == ApexInput::Actions::MenuDown
			|| ActionId == ApexInput::Actions::MenuLeft || ActionId == ApexInput::Actions::MenuRight)
		{
			Prompt = FString::Printf(TEXT("Push the wheel's joystick or hat for %s"), Label);
		}
		else if (ActionId == ApexInput::Actions::Throttle || ActionId == ApexInput::Actions::Brake)
		{
			Prompt = FString::Printf(TEXT("Press the %s pedal all the way down"), *FString(Label).ToLower());
		}
		else
		{
			Prompt = FString::Printf(TEXT("Press a button on the wheel for %s"), Label);
		}
		ListenTitleText->SetText(FText::FromString(Prompt));
	}
	if (ListenOverlay)
	{
		ListenOverlay->SetVisibility(ESlateVisibility::Visible);
	}

	// Focus has to come back to this widget: the chip that was clicked would
	// otherwise eat Enter and Space before the capture sees them.
	SetKeyboardFocus();
}

void UApexSettingsWidget::FinishListening(const FKey& Key, bool bCancelled, bool bInvert)
{
	bListening = false;
	ListenBaselines.Reset();
	if (ListenOverlay)
	{
		ListenOverlay->SetVisibility(ESlateVisibility::Collapsed);
	}

	if (!bCancelled)
	{
		if (UApexSettingsSubsystem* Settings = GetSettings())
		{
			Settings->SetBoundKey(ListeningAction, ListeningSlot, Key, bInvert);
		}
		RefreshBindingChips();
		RefreshWheelDevices();
		RefreshFooter();
	}

	ListeningAction = NAME_None;
	ListeningSlot = 0;
}

bool UApexSettingsWidget::IsRejectedBindingKey(const FKey& Key)
{
	// Binding a control to the mouse would make the menu unusable, and the
	// modifier keys on their own are not a binding anyone means to make.
	return Key.IsMouseButton()
		|| Key == EKeys::LeftShift || Key == EKeys::RightShift
		|| Key == EKeys::LeftControl || Key == EKeys::RightControl
		|| Key == EKeys::LeftAlt || Key == EKeys::RightAlt;
}

bool UApexSettingsWidget::IsKeyForListeningSlot(const FKey& Key) const
{
	return ApexDirectInput::IsDirectInputKey(Key) == ApexInput::IsWheelSlot(ListeningSlot);
}

FReply UApexSettingsWidget::NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent)
{
	const FKey Key = InKeyEvent.GetKey();

	if (bListening)
	{
		if (Key == EKeys::Escape || Key == EKeys::Gamepad_FaceButton_Right)
		{
			// The pad's Back cancels too: a wheel capture ignores pad keys, so
			// without it there would be no way out but the keyboard.
			FinishListening(FKey(), /*bCancelled*/ true);
		}
		else if (Key == EKeys::Delete || Key == EKeys::BackSpace)
		{
			// An explicitly empty binding, which is not the same as never having
			// touched the slot — the default does not come back.
			FinishListening(FKey(), /*bCancelled*/ false);
		}
		else if (!IsRejectedBindingKey(Key) && IsKeyForListeningSlot(Key))
		{
			FinishListening(Key, /*bCancelled*/ false);
		}
		// Every key while listening belongs to the capture, including the ones
		// that were rejected — otherwise a click on Shift would close the screen.
		return FReply::Handled();
	}

	// The pause key closes the whole stack's top layer, the same as it does on
	// the pause menu underneath. Escape and B arrive through HandleBack; Tab
	// and the shoulders through HandleNavigation.
	// A wheel's own pause button counts too; Escape is left to HandleBack.
	const UApexSettingsSubsystem* Settings = GetSettings();
	if (Key == EKeys::Gamepad_Special_Right || (Key != EKeys::Escape && Settings && Settings->IsPauseKey(Key)))
	{
		ApexUiAudio::Play(this, EApexUiSound::Back);
		Close();
		return FReply::Handled();
	}

	return Super::NativeOnKeyDown(InGeometry, InKeyEvent);
}

FReply UApexSettingsWidget::NativeOnAnalogValueChanged(const FGeometry& InGeometry, const FAnalogInputEvent& InAnalogEvent)
{
	// An axis binding is made by moving the axis, which never produces a key
	// event — only this one.
	if (bListening)
	{
		const FKey Key = InAnalogEvent.GetKey();
		if (!IsKeyForListeningSlot(Key))
		{
			return FReply::Handled();
		}

		// How far it MOVED, not where it sits: a pedal rests at one end of its
		// travel and reports that every frame, so a threshold on the value
		// alone would bind whichever pedal spoke first. The first reading of
		// each axis is only a baseline.
		const float Value = InAnalogEvent.GetAnalogValue();
		const float* Baseline = ListenBaselines.Find(Key);
		if (!Baseline)
		{
			ListenBaselines.Add(Key, Value);
			return FReply::Handled();
		}

		const float Travel = Value - *Baseline;
		if (FMath::Abs(Travel) > AxisCaptureTravel)
		{
			// Moved the way the prompt asked for: the right way round. Moved
			// the other way: the axis runs backwards, and saying so here saves
			// the player an inversion setting they would have to find.
			FinishListening(Key, /*bCancelled*/ false, /*bInvert*/ Travel < 0.0f);
		}
		return FReply::Handled();
	}
	return Super::NativeOnAnalogValueChanged(InGeometry, InAnalogEvent);
}

FReply UApexSettingsWidget::NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	// Clicking outside the prompt cancels the rebind rather than doing nothing,
	// which is what every other modal on this screen does.
	if (bListening)
	{
		FinishListening(FKey(), /*bCancelled*/ true);
		return FReply::Handled();
	}
	return Super::NativeOnMouseButtonDown(InGeometry, InMouseEvent);
}
