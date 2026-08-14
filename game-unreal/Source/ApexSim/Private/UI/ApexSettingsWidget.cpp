#include "UI/ApexSettingsWidget.h"

#include "ApexSettingsSave.h"
#include "ApexSim.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/ComboBoxString.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/ProgressBar.h"
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
#include "UI/ApexSegmentedWidget.h"
#include "UI/ApexUIStyle.h"

using namespace ApexUI;

// The reset button casts the tab straight to a settings group, so the two
// enums have to stay in step.
static_assert(
	static_cast<int32>(EApexSettingsTab::Gameplay) == static_cast<int32>(EApexSettingsGroup::Gameplay)
		&& static_cast<int32>(EApexSettingsTab::Graphics) == static_cast<int32>(EApexSettingsGroup::Graphics)
		&& static_cast<int32>(EApexSettingsTab::Controls) == static_cast<int32>(EApexSettingsGroup::Controls),
	"EApexSettingsTab and EApexSettingsGroup must stay aligned");

namespace
{
	constexpr float PanelWidth = 1370.0f;
	constexpr float RailWidth = 274.0f;
	constexpr float RowHeight = 68.0f;
	constexpr float ControlWidth = 480.0f;
	/** Dropdowns and the sliders beside them share a width so the grid lines up. */
	constexpr float DropdownWidth = 250.0f;
	/** Wider, because the AI-skill row has a whole page column to itself. */
	constexpr float SliderCellWidth = 400.0f;

	// Rail and footer actions.
	const FName ActionTabGameplay = TEXT("Tab.Gameplay");
	const FName ActionTabGraphics = TEXT("Tab.Graphics");
	const FName ActionTabControls = TEXT("Tab.Controls");
	const FName ActionBack        = TEXT("Back");
	const FName ActionReset       = TEXT("Reset");

	// Segmented-control ids.
	const FName SegTraction   = TEXT("Traction");
	const FName SegAbs        = TEXT("Abs");
	const FName SegGearbox    = TEXT("Gearbox");
	const FName SegRacingLine = TEXT("RacingLine");
	const FName SegUnits      = TEXT("Units");
	const FName SegHud        = TEXT("Hud");
	const FName SegPreset     = TEXT("Preset");
	const FName SegVSync      = TEXT("VSync");

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

void UApexSettingsWidget::BuildOverlay()
{
	UVerticalBox* Panel = WidgetTree->ConstructWidget<UVerticalBox>();

	AddV(Panel, BuildHeader());
	AddV(Panel, MakeDivider(*WidgetTree));

	UHorizontalBox* Body = WidgetTree->ConstructWidget<UHorizontalBox>();
	AddH(Body, BuildRail(), FMargin(), VAlign_Fill);
	AddH(Body, MakeDivider(*WidgetTree, /*bVertical*/ true), FMargin(), VAlign_Fill);

	PageHost = WidgetTree->ConstructWidget<UWidgetSwitcher>();
	PageHost->AddChild(BuildGameplayPage());
	PageHost->AddChild(BuildGraphicsPage());
	PageHost->AddChild(BuildControlsPage());

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
	AddH(Row, HeaderContextText, FMargin(20.0f, 6.0f, 0.0f, 0.0f), VAlign_Center);

	AddH(Row, WidgetTree->ConstructWidget<UHorizontalBox>(), FMargin(), VAlign_Center, 1.0f);

	UApexButtonWidget* Back = WidgetTree->ConstructWidget<UApexButtonWidget>();
	FApexButtonSpec BackSpec;
	BackSpec.Label = TEXT("Back");
	BackSpec.KeyCap = TEXT("ESC");
	BackSpec.Variant = EApexButtonVariant::Bare;
	BackSpec.LabelSize = 13.0f;
	BackSpec.ActionId = ActionBack;
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
	AddTab(TEXT("Graphics"), ActionTabGraphics);
	AddTab(TEXT("Controls"), ActionTabControls);

	AddV(Stack, WidgetTree->ConstructWidget<UVerticalBox>(), FMargin(), HAlign_Fill, 1.0f);

	UBorder* Rail = MakePanel(*WidgetTree, Stack, FMargin(0.0f, 22.0f, 0.0f, 0.0f), MakeBrush(Palette::Background));
	return MakeSized(*WidgetTree, Rail, RailWidth, -1.0f);
}

UWidget* UApexSettingsWidget::BuildFooter()
{
	UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();

	FooterStatusText = MakeLabel(*WidgetTree, FString(), Palette::TextMuted);
	AddH(Row, FooterStatusText);
	AddH(Row, WidgetTree->ConstructWidget<UHorizontalBox>(), FMargin(), VAlign_Center, 1.0f);

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
	RaceSpec.ActionId = ActionBack;
	BackToRace->Setup(RaceSpec);
	BackToRace->OnActivated.AddDynamic(this, &UApexSettingsWidget::HandleFooterActivated);
	AddH(Row, MakeSized(*WidgetTree, BackToRace, 200.0f, 56.0f));

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
	const FString& Label, const FString& Description, UWidget* Control, const FString& PendingNote)
{
	UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();
	const bool bPending = !PendingNote.IsEmpty();

	UVerticalBox* Text = WidgetTree->ConstructWidget<UVerticalBox>();

	UHorizontalBox* TitleRow = WidgetTree->ConstructWidget<UHorizontalBox>();
	AddH(TitleRow, MakeText(*WidgetTree, Label, Font::Body(17.0f, true),
		bPending ? Palette::TextSecondary : Palette::TextPrimary));
	if (bPending)
	{
		// A badge, not a tooltip: the row has to read as "saved but inert" at a
		// glance, without the player having to hover it to find out.
		AddH(TitleRow,
			MakePanel(*WidgetTree, MakeLabel(*WidgetTree, TEXT("Not yet wired"), Palette::Accent),
				FMargin(8.0f, 3.0f), MakeBrush(FLinearColor::Transparent, Palette::Accent, 1.0f, 2.0f)),
			FMargin(12.0f, 0.0f, 0.0f, 0.0f));
	}
	AddV(Text, TitleRow);

	const FString SubLine = bPending
		? (Description.IsEmpty() ? PendingNote : Description + TEXT("  ") + PendingNote)
		: Description;
	if (!SubLine.IsEmpty())
	{
		AddV(Text, MakeText(*WidgetTree, SubLine, Font::Body(12.0f),
			bPending ? Palette::TextDisabled : Palette::TextMuted),
			FMargin(0.0f, 5.0f, 0.0f, 0.0f));
	}
	AddH(Row, Text, FMargin(), VAlign_Center, 1.0f);

	if (Control)
	{
		// Controls are right-aligned to a fixed width so the pills and dropdowns
		// on every row line up down one edge regardless of how wide each is.
		UHorizontalBox* ControlCell = WidgetTree->ConstructWidget<UHorizontalBox>();
		if (UHorizontalBoxSlot* CellSlot = AddH(ControlCell, Control, FMargin(), VAlign_Center, 1.0f))
		{
			CellSlot->SetHorizontalAlignment(HAlign_Right);
		}
		AddH(Row, MakeSized(*WidgetTree, ControlCell, ControlWidth, -1.0f), FMargin(24.0f, 0.0f, 0.0f, 0.0f), VAlign_Center);
	}

	UBorder* Panel = MakePanel(*WidgetTree, Row, FMargin(22.0f, 0.0f), MakeBrush(Palette::Surface));
	Panel->SetVerticalAlignment(VAlign_Center);
	return MakeSized(*WidgetTree, Panel, -1.0f, RowHeight);
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

// --- Gameplay ---------------------------------------------------------------

UWidget* UApexSettingsWidget::BuildGameplayPage()
{
	UVerticalBox* Page = WidgetTree->ConstructWidget<UVerticalBox>();

	AddV(Page, MakeSectionLabel(TEXT("Driving aids")), FMargin(0.0f, 0.0f, 0.0f, 14.0f));

	// Traction control, ABS and the racing line are stored and saved but nothing
	// consumes them: the aids are server-side physics and the protocol has no
	// field for them, and there is no raceline renderer in the client yet.
	static const FString AidNote = TEXT("Saved, but the server has no field for it yet.");

	AddV(Page, MakeRow(
		TEXT("Traction control"),
		TEXT("Cuts torque when the rear axle slips."),
		MakeSegment(SegTraction, { TEXT("OFF"), TEXT("LOW"), TEXT("HIGH") }, 1),
		AidNote));

	AddV(Page, MakeRow(
		TEXT("ABS"),
		TEXT("Releases brake pressure at lock-up."),
		MakeSegment(SegAbs, { TEXT("OFF"), TEXT("ON") }, 1, 178.0f),
		AidNote), FMargin(0.0f, 2.0f, 0.0f, 0.0f));

	AddV(Page, MakeRow(
		TEXT("Gearbox"),
		TEXT("Automatic shifting is done on this machine, from the car's revs."),
		MakeSegment(SegGearbox, { TEXT("MANUAL"), TEXT("AUTO") }, 1, 178.0f)), FMargin(0.0f, 2.0f, 0.0f, 0.0f));

	AddV(Page, MakeRow(
		TEXT("Racing line"),
		TEXT("Drawn from the track's measured raceline."),
		MakeSegment(SegRacingLine, { TEXT("OFF"), TEXT("BRAKING ONLY"), TEXT("FULL") }, 1),
		TEXT("Saved, but nothing draws the line yet.")),
		FMargin(0.0f, 2.0f, 0.0f, 0.0f));

	AddV(Page, MakeSectionLabel(TEXT("Session")), FMargin(0.0f, 26.0f, 0.0f, 14.0f));

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
		Slider->OnValueChanged.AddDynamic(this, &UApexSettingsWidget::HandleAiSkillChanged);

		// An explicit width, because MakeRow right-aligns its control cell and a
		// fill-width slider right-aligned to its own desired size is a stub.

		// CreateSession carries a track, a player cap, an AI count, a lap limit
		// and a session kind — and no skill. The server's own [ai] config decides.
		AddV(Page, MakeRow(
			TEXT("AI skill"),
			TEXT("Would apply to every AI car in a session you create."),
			MakeSized(*WidgetTree, Cell, SliderCellWidth, -1.0f),
			TEXT("CreateSession has no skill field.")));
	}

	AddV(Page, MakeRow(
		TEXT("Units"),
		TEXT("Speed, distance and temperature."),
		MakeSegment(SegUnits, { TEXT("METRIC"), TEXT("IMPERIAL") }, 0, 178.0f)), FMargin(0.0f, 2.0f, 0.0f, 0.0f));

	AddV(Page, MakeRow(
		TEXT("HUD elements"),
		TEXT("Minimap and pedal telemetry."),
		MakeSegment(SegHud, { TEXT("ALL"), TEXT("ESSENTIAL"), TEXT("HIDDEN") }, 0)), FMargin(0.0f, 2.0f, 0.0f, 0.0f));

	AddV(Page, WidgetTree->ConstructWidget<UVerticalBox>(), FMargin(), HAlign_Fill, 1.0f);
	return Page;
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

	AddSliderRow(Right, TEXT("Field of view"), TEXT(""), FovSlider, FovFill, FovValue, false);
	FovSlider->OnValueChanged.AddDynamic(this, &UApexSettingsWidget::HandleFovChanged);

	AddH(Grid, Left, FMargin(0.0f, 0.0f, 14.0f, 0.0f), VAlign_Top, 1.0f);
	AddH(Grid, Right, FMargin(), VAlign_Top, 1.0f);
	AddV(Page, Grid, FMargin(0.0f, 14.0f, 0.0f, 0.0f));

	// The note the mockup carries, and it is true here: the pause menu leaves
	// the scene rendering behind the panel, so a change is visible at once.
	{
		UHorizontalBox* Note = WidgetTree->ConstructWidget<UHorizontalBox>();
		AddH(Note, MakeLabel(*WidgetTree, TEXT("Live preview"), Palette::Accent));
		AddH(Note,
			MakeText(*WidgetTree,
				TEXT("Changes apply to the frame behind this panel, so the effect is visible before returning to the race."),
				Font::Body(13.0f), Palette::TextSecondary),
			FMargin(18.0f, 0.0f, 0.0f, 0.0f));
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

	// Two cards, because two is what the platform can actually tell us about:
	// the keyboard, which is always there, and whether a gamepad is attached.
	// Enumerating wheels and pedals by name needs a device layer this client
	// does not have yet.
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

		AddSliderCell(TEXT("Steering sensitivity"), SteeringSlider, SteeringFill, SteeringValue, true);
		AddSliderCell(TEXT("Deadzone"), DeadzoneSlider, DeadzoneFill, DeadzoneValue, false);
		// Stored and saved, but nothing plays force feedback yet — there is no
		// force-feedback path from the telemetry stream to the pad.
		AddSliderCell(TEXT("Vibration (not yet wired)"), VibrationSlider, VibrationFill, VibrationValue, false);

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
	};

	static const TArray<FRowSpec> RowSpecs = {
		{ TEXT("Throttle"),    ApexInput::Actions::Throttle,      0,  1 },
		{ TEXT("Brake"),       ApexInput::Actions::Brake,         0,  1 },
		{ TEXT("Steer axis"),  ApexInput::Actions::Steer,         0, -1 },
		{ TEXT("Steer left"),  ApexInput::Actions::Steer,        -1,  2 },
		{ TEXT("Steer right"), ApexInput::Actions::Steer,        -1,  3 },
		{ TEXT("Shift up"),    ApexInput::Actions::GearUp,        0,  1 },
		{ TEXT("Shift down"),  ApexInput::Actions::GearDown,      0,  1 },
		{ TEXT("Camera"),      ApexInput::Actions::ToggleCamera,  0,  1 },
		{ TEXT("Pause menu"),  ApexInput::Actions::PauseMenu,     0,  1 },
	};

	UHorizontalBox* Grid = WidgetTree->ConstructWidget<UHorizontalBox>();
	UVerticalBox* Left = WidgetTree->ConstructWidget<UVerticalBox>();
	UVerticalBox* Right = WidgetTree->ConstructWidget<UVerticalBox>();

	const int32 Split = FMath::DivideAndRoundUp(RowSpecs.Num(), 2);

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

		UVerticalBox* Column = Index < Split ? Left : Right;
		AddV(Column, MakeRow(RowSpec.Label, FString(), Chips),
			FMargin(0.0f, Column->GetChildrenCount() == 0 ? 0.0f : 2.0f, 0.0f, 0.0f));
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

	if (UApexSettingsSubsystem* Settings = GetSettings())
	{
		Settings->ResetChangeCount();
	}

	ShowTab(Tab);
	RefreshFromSettings();
	SetKeyboardFocus();
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
	SetSegment(SegRacingLine, static_cast<int32>(Values->RacingLine));
	SetSegment(SegUnits, static_cast<int32>(Values->Units));
	SetSegment(SegHud, static_cast<int32>(Values->HudDetail));
	SetSegment(SegPreset, static_cast<int32>(Values->Preset));
	SetSegment(SegVSync, Values->bVSync ? 1 : 0);

	auto SetSlider = [](USlider* Slider, UProgressBar* Fill, UTextBlock* Text,
		float Value, float Min, float Max, const FString& Display)
	{
		const float Alpha = FMath::Clamp((Value - Min) / FMath::Max(KINDA_SMALL_NUMBER, Max - Min), 0.0f, 1.0f);
		if (Slider) { Slider->SetValue(Alpha); }
		if (Fill)   { Fill->SetPercent(Alpha); }
		if (Text)   { Text->SetText(FText::FromString(Display)); }
	};

	SetSlider(AiSkillSlider, AiSkillFill, AiSkillValue, Values->AiSkill, 0.0f, 1.0f,
		FString::Printf(TEXT("%d %%"), FMath::RoundToInt(Values->AiSkill * 100.0f)));
	SetSlider(MotionBlurSlider, MotionBlurFill, MotionBlurValue, Values->MotionBlur, 0.0f, 1.0f,
		FString::FromInt(FMath::RoundToInt(Values->MotionBlur * 100.0f)));
	SetSlider(FovSlider, FovFill, FovValue, Values->FieldOfView, 60.0f, 120.0f,
		FString::Printf(TEXT("%d°"), FMath::RoundToInt(Values->FieldOfView)));
	SetSlider(SteeringSlider, SteeringFill, SteeringValue, Values->SteeringSensitivity, 0.0f, 1.0f,
		FString::FromInt(FMath::RoundToInt(Values->SteeringSensitivity * 100.0f)));
	SetSlider(DeadzoneSlider, DeadzoneFill, DeadzoneValue, Values->Deadzone, 0.0f, 0.5f,
		FString::FromInt(FMath::RoundToInt(Values->Deadzone * 100.0f)));
	SetSlider(VibrationSlider, VibrationFill, VibrationValue, Values->Vibration, 0.0f, 1.0f,
		FString::FromInt(FMath::RoundToInt(Values->Vibration * 100.0f)));

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

	RefreshBindingChips();
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

	case EApexSettingsTab::Graphics:
		if (const UWorld* World = GetWorld())
		{
			const float Delta = World->GetDeltaSeconds();
			const int32 Fps = Delta > KINDA_SMALL_NUMBER ? FMath::RoundToInt(1.0f / Delta) : 0;
			Context = FString::Printf(TEXT("%d fps"), Fps);
		}
		break;

	case EApexSettingsTab::Controls:
	{
		const bool bAttached = FSlateApplication::IsInitialized() && FSlateApplication::Get().IsGamepadAttached();
		Context = bAttached ? TEXT("2 devices detected") : TEXT("1 device detected");
		break;
	}
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
	else if (Action == ActionTabGraphics) { ShowTab(EApexSettingsTab::Graphics); }
	else if (Action == ActionTabControls) { ShowTab(EApexSettingsTab::Controls); }
}

void UApexSettingsWidget::HandleFooterActivated(UApexButtonWidget* Button)
{
	if (!Button)
	{
		return;
	}

	if (Button->GetActionId() == ActionBack)
	{
		Close();
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
	if (Id == SegTraction)        { Settings->SetTractionControl(static_cast<EApexAssistLevel>(Index)); }
	else if (Id == SegAbs)        { Settings->SetAbs(Index == 1); }
	else if (Id == SegGearbox)    { Settings->SetAutoGearbox(Index == 1); }
	else if (Id == SegRacingLine) { Settings->SetRacingLine(static_cast<EApexRacingLine>(Index)); }
	else if (Id == SegUnits)      { Settings->SetUnits(static_cast<EApexUnits>(Index)); }
	else if (Id == SegHud)        { Settings->SetHudDetail(static_cast<EApexHudDetail>(Index)); }
	else if (Id == SegVSync)      { Settings->SetVSync(Index == 1); }
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
	if (AiSkillFill) { AiSkillFill->SetPercent(Value); }
	if (AiSkillValue) { AiSkillValue->SetText(FText::FromString(FString::Printf(TEXT("%d %%"), FMath::RoundToInt(Value * 100.0f)))); }
	if (UApexSettingsSubsystem* Settings = GetSettings()) { Settings->SetAiSkill(Value); }
	RefreshFooter();
}

void UApexSettingsWidget::HandleMotionBlurChanged(float Value)
{
	if (bRefreshing) { return; }
	if (MotionBlurFill) { MotionBlurFill->SetPercent(Value); }
	if (MotionBlurValue) { MotionBlurValue->SetText(FText::FromString(FString::FromInt(FMath::RoundToInt(Value * 100.0f)))); }
	if (UApexSettingsSubsystem* Settings = GetSettings()) { Settings->SetMotionBlur(Value); }
	RefreshFooter();
}

void UApexSettingsWidget::HandleFovChanged(float Value)
{
	if (bRefreshing) { return; }
	const float Degrees = FMath::Lerp(60.0f, 120.0f, Value);
	if (FovFill) { FovFill->SetPercent(Value); }
	if (FovValue) { FovValue->SetText(FText::FromString(FString::Printf(TEXT("%d°"), FMath::RoundToInt(Degrees)))); }
	if (UApexSettingsSubsystem* Settings = GetSettings()) { Settings->SetFieldOfView(Degrees); }
	RefreshFooter();
}

void UApexSettingsWidget::HandleSteeringChanged(float Value)
{
	if (bRefreshing) { return; }
	if (SteeringFill) { SteeringFill->SetPercent(Value); }
	if (SteeringValue) { SteeringValue->SetText(FText::FromString(FString::FromInt(FMath::RoundToInt(Value * 100.0f)))); }
	if (UApexSettingsSubsystem* Settings = GetSettings()) { Settings->SetSteeringSensitivity(Value); }
	RefreshFooter();
}

void UApexSettingsWidget::HandleDeadzoneChanged(float Value)
{
	if (bRefreshing) { return; }
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
	if (VibrationFill) { VibrationFill->SetPercent(Value); }
	if (VibrationValue) { VibrationValue->SetText(FText::FromString(FString::FromInt(FMath::RoundToInt(Value * 100.0f)))); }
	if (UApexSettingsSubsystem* Settings = GetSettings()) { Settings->SetVibration(Value); }
	RefreshFooter();
}

void UApexSettingsWidget::HandleDisplayModeChanged(FString Item, ESelectInfo::Type SelectType)
{
	if (bRefreshing || !DisplayModeBox) { return; }
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
	if (UApexSettingsSubsystem* Settings = GetSettings())
	{
		Settings->SetAntiAliasingQuality(AntiAliasingBox->GetSelectedIndex());
	}
	RefreshFromSettings();
}

void UApexSettingsWidget::HandleTexturesChanged(FString Item, ESelectInfo::Type SelectType)
{
	if (bRefreshing || !TexturesBox) { return; }
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

	if (ListenTitleText)
	{
		const ApexInput::FSlotDef* Def = ApexInput::FindSlot(ActionId, ListenSlot);
		ListenTitleText->SetText(FText::FromString(
			Def ? FString::Printf(TEXT("Press any key or move an axis for %s"), Def->Label)
				: TEXT("Press any key or move an axis")));
	}
	if (ListenOverlay)
	{
		ListenOverlay->SetVisibility(ESlateVisibility::Visible);
	}

	// Focus has to come back to this widget: the chip that was clicked would
	// otherwise eat Enter and Space before the capture sees them.
	SetKeyboardFocus();
}

void UApexSettingsWidget::FinishListening(const FKey& Key, bool bCancelled)
{
	bListening = false;
	if (ListenOverlay)
	{
		ListenOverlay->SetVisibility(ESlateVisibility::Collapsed);
	}

	if (!bCancelled)
	{
		if (UApexSettingsSubsystem* Settings = GetSettings())
		{
			Settings->SetBoundKey(ListeningAction, ListeningSlot, Key);
		}
		RefreshBindingChips();
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

FReply UApexSettingsWidget::NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent)
{
	const FKey Key = InKeyEvent.GetKey();

	if (bListening)
	{
		if (Key == EKeys::Escape)
		{
			FinishListening(FKey(), /*bCancelled*/ true);
		}
		else if (Key == EKeys::Delete || Key == EKeys::BackSpace)
		{
			// An explicitly empty binding, which is not the same as never having
			// touched the slot — the default does not come back.
			FinishListening(FKey(), /*bCancelled*/ false);
		}
		else if (!IsRejectedBindingKey(Key))
		{
			FinishListening(Key, /*bCancelled*/ false);
		}
		// Every key while listening belongs to the capture, including the ones
		// that were rejected — otherwise a click on Shift would close the screen.
		return FReply::Handled();
	}

	if (Key == EKeys::Escape || Key == EKeys::Gamepad_Special_Right)
	{
		Close();
		return FReply::Handled();
	}

	// Tab walks the pages, which is the only way to reach them from a gamepad
	// until the rail is properly navigable.
	if (Key == EKeys::Tab || Key == EKeys::Gamepad_LeftShoulder || Key == EKeys::Gamepad_RightShoulder)
	{
		const int32 Direction = (Key == EKeys::Gamepad_LeftShoulder || InKeyEvent.IsShiftDown()) ? 2 : 1;
		ShowTab(static_cast<EApexSettingsTab>((static_cast<int32>(CurrentTab) + Direction) % 3));
		return FReply::Handled();
	}

	return Super::NativeOnKeyDown(InGeometry, InKeyEvent);
}

FReply UApexSettingsWidget::NativeOnAnalogValueChanged(const FGeometry& InGeometry, const FAnalogInputEvent& InAnalogEvent)
{
	// An axis binding is made by moving the axis, which never produces a key
	// event — only this one. The threshold keeps a resting stick from binding
	// itself the moment the prompt opens.
	if (bListening && FMath::Abs(InAnalogEvent.GetAnalogValue()) > 0.5f)
	{
		FinishListening(InAnalogEvent.GetKey(), /*bCancelled*/ false);
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
