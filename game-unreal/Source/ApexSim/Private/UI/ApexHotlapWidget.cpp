#include "UI/ApexHotlapWidget.h"

#include "ApexMenuFlowSubsystem.h"
#include "ApexNetSubsystem.h"
#include "ApexSettingsSave.h"
#include "ApexSettingsSubsystem.h"
#include "ApexSim.h"
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
#include "UI/ApexButtonWidget.h"
#include "UI/ApexStepperWidget.h"
#include "UI/ApexUIStyle.h"

using namespace ApexUI;

namespace
{
	constexpr float GarageCardWidth = 1180.0f;
	constexpr float SetupRowHeight = 54.0f;
	constexpr float ActionColumnWidth = 280.0f;
	constexpr float SheetWidth = 330.0f;
	/** Laps kept on the sheet; older ones scroll off the top. */
	constexpr int32 SheetRowCount = 10;

	const FName ActionGoOut  = TEXT("Hotlap.GoOut");
	const FName ActionReplay = TEXT("Hotlap.Replay");
	const FName ActionGhost  = TEXT("Hotlap.Ghost");
	const FName ActionResetSetup  = TEXT("Hotlap.Reset");

	/** "1:32.104", or dashes for no time. */
	FString FormatMs(int32 Ms)
	{
		if (Ms <= 0)
		{
			return TEXT("--:--.---");
		}
		const int32 Minutes = Ms / 60000;
		const int32 Seconds = (Ms / 1000) % 60;
		const int32 Millis = Ms % 1000;
		return FString::Printf(TEXT("%d:%02d.%03d"), Minutes, Seconds, Millis);
	}

	/** "+0.412" / "-1.020" against a reference, in seconds. */
	FString FormatDelta(int32 Ms, int32 ReferenceMs)
	{
		const int32 Delta = Ms - ReferenceMs;
		return FString::Printf(TEXT("%s%d.%03d"), Delta < 0 ? TEXT("-") : TEXT("+"), FMath::Abs(Delta) / 1000, FMath::Abs(Delta) % 1000);
	}

	/** The timing screen's purple: the session's best. */
	const FLinearColor Purple = FLinearColor::FromSRGBColor(FColor(0xB3, 0x7C, 0xFF));
}

UApexHotlapWidget::UApexHotlapWidget(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	SetVisibility(ESlateVisibility::Collapsed);
	SetIsFocusable(true);
}

void UApexHotlapWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();

	UOverlay* Layers = WidgetTree->ConstructWidget<UOverlay>();

	GarageCard = BuildGarageCard();
	UOverlaySlot* CardSlot = Layers->AddChildToOverlay(GarageCard);
	CardSlot->SetHorizontalAlignment(HAlign_Left);
	CardSlot->SetVerticalAlignment(VAlign_Center);
	CardSlot->SetPadding(FMargin(Metrics::PageGutter, 0.0f, 0.0f, 0.0f));

	TimingPanel = BuildTimingPanel();
	UOverlaySlot* SheetSlot = Layers->AddChildToOverlay(TimingPanel);
	SheetSlot->SetHorizontalAlignment(HAlign_Right);
	SheetSlot->SetVerticalAlignment(VAlign_Center);
	SheetSlot->SetPadding(FMargin(0.0f, 0.0f, Metrics::PageGutter, 0.0f));

	ReplayStrip = BuildReplayStrip();
	UOverlaySlot* StripSlot = Layers->AddChildToOverlay(ReplayStrip);
	StripSlot->SetHorizontalAlignment(HAlign_Center);
	StripSlot->SetVerticalAlignment(VAlign_Top);
	StripSlot->SetPadding(FMargin(0.0f, 34.0f, 0.0f, 0.0f));

	// On the track a one-line reminder of the way back in, under the HUD's top bar.
	TrackHint = MakePanel(*WidgetTree,
		MakeText(*WidgetTree, TEXT("ESC  ·  BACK TO GARAGE"), Font::Mono(11.0f, 100), Palette::TextSecondary),
		FMargin(14.0f, 7.0f), MakeBrush(Palette::Surface.CopyWithNewOpacity(0.72f)));
	UOverlaySlot* HintSlot = Layers->AddChildToOverlay(TrackHint);
	HintSlot->SetHorizontalAlignment(HAlign_Center);
	HintSlot->SetVerticalAlignment(VAlign_Top);
	HintSlot->SetPadding(FMargin(0.0f, Metrics::TopBarHeight + 52.0f, 0.0f, 0.0f));

	WidgetTree->RootWidget = Layers;
	ApplyView();
}

void UApexHotlapWidget::NativeConstruct()
{
	Super::NativeConstruct();
	UGameInstance* GameInstance = GetGameInstance();
	if (UApexNetSubsystem* Net = GameInstance ? GameInstance->GetSubsystem<UApexNetSubsystem>() : nullptr)
	{
		Net->OnLapTiming.AddDynamic(this, &UApexHotlapWidget::HandleLapTiming);
		Net->OnGhostLap.AddDynamic(this, &UApexHotlapWidget::HandleGhostLap);
		Net->OnLapRecord.AddDynamic(this, &UApexHotlapWidget::HandleLapRecord);
	}
	if (UApexSettingsSubsystem* Settings = GetSettings())
	{
		Settings->OnSettingsChanged.AddDynamic(this, &UApexHotlapWidget::HandleSettingsChanged);
	}
}

void UApexHotlapWidget::NativeDestruct()
{
	UGameInstance* GameInstance = GetGameInstance();
	if (UApexNetSubsystem* Net = GameInstance ? GameInstance->GetSubsystem<UApexNetSubsystem>() : nullptr)
	{
		Net->OnLapTiming.RemoveDynamic(this, &UApexHotlapWidget::HandleLapTiming);
		Net->OnGhostLap.RemoveDynamic(this, &UApexHotlapWidget::HandleGhostLap);
		Net->OnLapRecord.RemoveDynamic(this, &UApexHotlapWidget::HandleLapRecord);
	}
	if (UApexSettingsSubsystem* Settings = GetSettings())
	{
		Settings->OnSettingsChanged.RemoveDynamic(this, &UApexHotlapWidget::HandleSettingsChanged);
	}
	Super::NativeDestruct();
}

UApexSettingsSubsystem* UApexHotlapWidget::GetSettings() const
{
	return GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexSettingsSubsystem>() : nullptr;
}

// --- Construction -----------------------------------------------------------

UApexButtonWidget* UApexHotlapWidget::MakeCardButton(
	UVerticalBox* Stack, const FString& Label, const FString& Badge, FName ActionId, bool bPrimary)
{
	FApexButtonSpec Spec;
	Spec.Label = Label;
	Spec.Badge = Badge;
	Spec.ActionId = ActionId;
	Spec.Variant = bPrimary ? EApexButtonVariant::Primary : EApexButtonVariant::Panel;
	Spec.Height = bPrimary ? 66.0f : 54.0f;
	Spec.LabelSize = bPrimary ? 22.0f : 18.0f;
	UApexButtonWidget* Button = WidgetTree->ConstructWidget<UApexButtonWidget>();
	Button->Setup(Spec);
	Button->OnActivated.AddDynamic(this, &UApexHotlapWidget::HandleButtonActivated);
	AddV(Stack, Button, FMargin(0.0f, Stack->GetChildrenCount() == 0 ? 0.0f : 4.0f, 0.0f, 0.0f));
	return Button;
}

void UApexHotlapWidget::MakeSetupRow(UVerticalBox* Column, int32 Knob, const TCHAR* Label, const TCHAR* Description)
{
	UApexStepperWidget* Stepper = WidgetTree->ConstructWidget<UApexStepperWidget>();
	Stepper->Index = Knob;
	Stepper->ControlId = FName(ApexCarSetup::Knob(Knob).Key);
	Stepper->Formatter = [Knob](int32 Value) { return ApexCarSetup::Describe(Knob, Value); };
	const ApexCarSetup::FKnob& Spec = ApexCarSetup::Knob(Knob);
	Stepper->Setup(Spec.Min, Spec.Max, 0, 96.0f);
	Stepper->OnChanged.AddDynamic(this, &UApexHotlapWidget::HandleStepperChanged);
	SetupSteppers.Add(Knob, Stepper);

	UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();
	UVerticalBox* Text = WidgetTree->ConstructWidget<UVerticalBox>();
	AddV(Text, MakeText(*WidgetTree, Label, Font::Body(15.0f, true), Palette::TextPrimary));
	// The columns are narrow: the note wraps rather than running under the stepper.
	UTextBlock* Note = MakeText(*WidgetTree, Description, Font::Body(11.0f), Palette::TextMuted);
	Note->SetAutoWrapText(true);
	AddV(Text, Note, FMargin(0.0f, 2.0f, 0.0f, 0.0f));
	AddH(Row, Text, FMargin(), VAlign_Center, 1.0f);
	AddH(Row, Stepper, FMargin(12.0f, 0.0f, 0.0f, 0.0f), VAlign_Center);

	UBorder* Panel = MakePanel(*WidgetTree, Row, FMargin(16.0f, 0.0f), MakeBrush(Palette::Surface));
	Panel->SetVerticalAlignment(VAlign_Center);
	AddV(Column, MakeSized(*WidgetTree, Panel, -1.0f, SetupRowHeight), FMargin(0.0f, Column->GetChildrenCount() == 0 ? 0.0f : 2.0f, 0.0f, 0.0f));
}

UWidget* UApexHotlapWidget::BuildGarageCard()
{
	UVerticalBox* Card = WidgetTree->ConstructWidget<UVerticalBox>();

	// Heading.
	UVerticalBox* Heading = WidgetTree->ConstructWidget<UVerticalBox>();
	AddV(Heading, MakeLabel(*WidgetTree, TEXT("Hotlap"), Palette::Accent));
	AddV(Heading, MakeText(*WidgetTree, TEXT("GARAGE"), Font::Display(44.0f, 18), Palette::TextPrimary), FMargin(0.0f, 8.0f, 0.0f, 0.0f));
	GarageSubtitle = MakeText(*WidgetTree, FString(), Font::Mono(12.0f, 60), Palette::TextSecondary);
	AddV(Heading, GarageSubtitle, FMargin(0.0f, 8.0f, 0.0f, 0.0f));
	AddV(Card, MakePanel(*WidgetTree, Heading, FMargin(34.0f, 26.0f), MakeBrush(Palette::Surface)));
	AddV(Card, MakeDivider(*WidgetTree));

	// Body: actions on the left, the setup in two columns on the right.
	UHorizontalBox* Body = WidgetTree->ConstructWidget<UHorizontalBox>();

	UVerticalBox* Actions = WidgetTree->ConstructWidget<UVerticalBox>();
	GoOutButton = MakeCardButton(Actions, TEXT("GO OUT ON TRACK"), FString(), ActionGoOut, true);
	ReplayButton = MakeCardButton(Actions, TEXT("REPLAY LAP"), TEXT("No lap yet"), ActionReplay, false);
	GhostButton = MakeCardButton(Actions, TEXT("GHOST CAR"), TEXT("On"), ActionGhost, false);
	AddV(Actions, MakeDivider(*WidgetTree), FMargin(0.0f, 12.0f, 0.0f, 12.0f));
	ResetButton = MakeCardButton(Actions, TEXT("RESET SETUP"), TEXT("Every knob to stock"), ActionResetSetup, false);
	UTextBlock* Note = MakeText(*WidgetTree,
		TEXT("Out on the track the car spawns on the run-up before the line, so the first lap is a flying one. Each setup click is a fixed step off the car's own file; the server applies a change at once, so the next run drives it."),
		Font::Body(12.0f), Palette::TextMuted);
	Note->SetAutoWrapText(true);
	AddV(Actions, Note, FMargin(2.0f, 16.0f, 0.0f, 0.0f));
	AddH(Body, MakeSized(*WidgetTree, Actions, ActionColumnWidth, -1.0f), FMargin(), VAlign_Top);

	UVerticalBox* Left = WidgetTree->ConstructWidget<UVerticalBox>();
	UVerticalBox* Right = WidgetTree->ConstructWidget<UVerticalBox>();
	const FMargin SectionGap(0.0f, 14.0f, 0.0f, 8.0f);
	const FMargin FirstSection(0.0f, 0.0f, 0.0f, 8.0f);

	AddV(Left, MakeLabel(*WidgetTree, TEXT("Tyres")), FirstSection);
	// Notes are two short lines at most: the column is narrow.
	MakeSetupRow(Left, ApexCarSetup::TyrePressureFront, TEXT("Front pressure"), TEXT("Grip falls off the optimum."));
	MakeSetupRow(Left, ApexCarSetup::TyrePressureRear, TEXT("Rear pressure"), TEXT("Grip falls off the optimum."));
	AddV(Left, MakeLabel(*WidgetTree, TEXT("Engine")), SectionGap);
	MakeSetupRow(Left, ApexCarSetup::RevLimiter, TEXT("Rev limiter"), TEXT("Redline down only."));
	MakeSetupRow(Left, ApexCarSetup::EngineBraking, TEXT("Engine braking"), TEXT("Drag off throttle."));
	AddV(Left, MakeLabel(*WidgetTree, TEXT("Transmission")), SectionGap);
	MakeSetupRow(Left, ApexCarSetup::FinalDrive, TEXT("Final drive"), TEXT("Shorter (+) pulls harder."));
	MakeSetupRow(Left, ApexCarSetup::GearSpread, TEXT("Gear spread"), TEXT("Top gear closer (+)."));
	AddV(Left, MakeLabel(*WidgetTree, TEXT("Torque")), SectionGap);
	MakeSetupRow(Left, ApexCarSetup::TorqueMap, TEXT("Torque map"), TEXT("Whole curve. Down only."));
	MakeSetupRow(Left, ApexCarSetup::BrakeBias, TEXT("Brake bias"), TEXT("Front share of the braking."));

	AddV(Right, MakeLabel(*WidgetTree, TEXT("Suspension")), FirstSection);
	MakeSetupRow(Right, ApexCarSetup::SpringFront, TEXT("Front springs"), TEXT("Stiffer: less dive and roll."));
	MakeSetupRow(Right, ApexCarSetup::SpringRear, TEXT("Rear springs"), TEXT("Softer puts power down."));
	MakeSetupRow(Right, ApexCarSetup::DamperFront, TEXT("Front dampers"), TEXT("Bump and rebound together."));
	MakeSetupRow(Right, ApexCarSetup::DamperRear, TEXT("Rear dampers"), TEXT("Bump and rebound together."));
	MakeSetupRow(Right, ApexCarSetup::AntiRollFront, TEXT("Front anti-roll bar"), TEXT("Stiffer: more understeer."));
	MakeSetupRow(Right, ApexCarSetup::AntiRollRear, TEXT("Rear anti-roll bar"), TEXT("Stiffer: more oversteer."));

	AddH(Body, Left, FMargin(22.0f, 0.0f, 0.0f, 0.0f), VAlign_Top, 1.0f);
	AddH(Body, Right, FMargin(12.0f, 0.0f, 0.0f, 0.0f), VAlign_Top, 1.0f);
	AddV(Card, MakePanel(*WidgetTree, Body, FMargin(22.0f, 22.0f), MakeBrush(Palette::Background)));

	AddV(Card, MakeKeyHintBar(
		*WidgetTree,
		{ { TEXT("↑↓←→"), TEXT("Move") }, { TEXT("Enter"), TEXT("Select") }, { TEXT("Esc"), TEXT("Menu") } },
		{}));

	UBorder* CardPanel = MakeModalCard(*WidgetTree, Card);
	return MakeSized(*WidgetTree, CardPanel, GarageCardWidth, -1.0f);
}

UWidget* UApexHotlapWidget::BuildTimingPanel()
{
	UVerticalBox* Sheet = WidgetTree->ConstructWidget<UVerticalBox>();

	UHorizontalBox* Head = WidgetTree->ConstructWidget<UHorizontalBox>();
	AddH(Head, MakeLabel(*WidgetTree, TEXT("Hotlap"), Palette::Accent), FMargin(), VAlign_Center, 1.0f);
	SheetLiveLabel = MakeText(*WidgetTree, TEXT("IN GARAGE"), Font::Mono(10.0f, 120), Palette::TextMuted);
	AddH(Head, SheetLiveLabel, FMargin(), VAlign_Center);
	AddV(Sheet, Head);

	SheetLiveText = MakeText(*WidgetTree, TEXT("--:--.---"), Font::Mono(30.0f, 20), Palette::TextPrimary);
	AddV(Sheet, SheetLiveText, FMargin(0.0f, 8.0f, 0.0f, 12.0f));
	AddV(Sheet, MakeDivider(*WidgetTree));

	SheetRows = WidgetTree->ConstructWidget<UVerticalBox>();
	AddV(Sheet, SheetRows, FMargin(0.0f, 10.0f, 0.0f, 10.0f));

	AddV(Sheet, MakeDivider(*WidgetTree));
	UHorizontalBox* Best = WidgetTree->ConstructWidget<UHorizontalBox>();
	AddH(Best, MakeText(*WidgetTree, TEXT("BEST"), Font::Mono(10.0f, 120), Palette::TextMuted), FMargin(), VAlign_Center, 1.0f);
	SheetBestText = MakeText(*WidgetTree, TEXT("--:--.---"), Font::Mono(14.0f), Palette::TextPrimary);
	AddH(Best, SheetBestText, FMargin(), VAlign_Center);
	AddV(Sheet, Best, FMargin(0.0f, 10.0f, 0.0f, 0.0f));
	UHorizontalBox* Record = WidgetTree->ConstructWidget<UHorizontalBox>();
	AddH(Record, MakeText(*WidgetTree, TEXT("RECORD"), Font::Mono(10.0f, 120), Palette::TextMuted), FMargin(), VAlign_Center, 1.0f);
	SheetRecordText = MakeText(*WidgetTree, TEXT("--:--.---"), Font::Mono(14.0f), Palette::TextSecondary);
	AddH(Record, SheetRecordText, FMargin(), VAlign_Center);
	AddV(Sheet, Record, FMargin(0.0f, 4.0f, 0.0f, 0.0f));

	UBorder* Panel = MakePanel(*WidgetTree, Sheet, FMargin(20.0f, 16.0f), MakeBrush(Palette::Surface.CopyWithNewOpacity(0.82f)));
	return MakeSized(*WidgetTree, Panel, SheetWidth, -1.0f);
}

UWidget* UApexHotlapWidget::BuildReplayStrip()
{
	UHorizontalBox* Strip = WidgetTree->ConstructWidget<UHorizontalBox>();
	AddH(Strip, MakePanel(*WidgetTree,
		MakeText(*WidgetTree, TEXT("REPLAY"), Font::Mono(10.0f, 120), Palette::OnAccent),
		FMargin(12.0f, 6.0f), MakeBrush(Palette::Accent)), FMargin(), VAlign_Center);
	ReplayText = MakeText(*WidgetTree, FString(), Font::Mono(12.0f, 60), Palette::TextSecondary);
	AddH(Strip, ReplayText, FMargin(18.0f, 0.0f, 0.0f, 0.0f), VAlign_Center);
	AddH(Strip, MakeText(*WidgetTree, TEXT("ESC  ·  STOP"), Font::Mono(11.0f, 100), Palette::TextMuted), FMargin(24.0f, 0.0f, 0.0f, 0.0f), VAlign_Center);
	return MakePanel(*WidgetTree, Strip, FMargin(14.0f, 8.0f), MakeBrush(Palette::Surface.CopyWithNewOpacity(0.82f)));
}

// --- State --------------------------------------------------------------------

void UApexHotlapWidget::SetActive(bool bInActive)
{
	bActive = bInActive;
	Laps.Reset();
	BestMs = 0;
	LiveLap = 0;
	LiveLapTimeMs = 0;
	bLiveInvalid = false;
	if (!bActive)
	{
		SetView(EApexHotlapView::Hidden);
	}
	RefreshSheet();
	RefreshGhostRows();
	RefreshSetup();
}

void UApexHotlapWidget::SetView(EApexHotlapView InView)
{
	if (View == InView)
	{
		return;
	}
	View = InView;
	ApplyView();
	if (View == EApexHotlapView::Garage)
	{
		RefreshSetup();
		RefreshGhostRows();
		const UGameInstance* GameInstance = GetGameInstance();
		const UApexNetSubsystem* Net = GameInstance ? GameInstance->GetSubsystem<UApexNetSubsystem>() : nullptr;
		const UApexMenuFlowSubsystem* Flow = GameInstance ? GameInstance->GetSubsystem<UApexMenuFlowSubsystem>() : nullptr;
		TArray<FString> Parts;
		FApexSessionSummary Session;
		if (Net && Net->FindSessionById(Net->GetCurrentSessionId(), Session) && !Session.TrackName.IsEmpty())
		{
			Parts.Add(Session.TrackName);
		}
		FApexCarCatalogRow CarRow;
		if (Flow && Flow->GetCarCatalogRow(Flow->GetPendingCarId(), CarRow) && !CarRow.DisplayName.IsEmpty())
		{
			Parts.Add(CarRow.DisplayName);
		}
		if (GarageSubtitle)
		{
			GarageSubtitle->SetText(FText::FromString(FString::Join(Parts, TEXT("  ·  ")).ToUpper()));
		}
	}
	RefreshSheet();
}

void UApexHotlapWidget::ApplyView()
{
	const bool bShown = View != EApexHotlapView::Hidden;
	SetVisibility(bShown
		? (View == EApexHotlapView::Garage ? ESlateVisibility::Visible : ESlateVisibility::HitTestInvisible)
		: ESlateVisibility::Collapsed);
	if (GarageCard)
	{
		GarageCard->SetVisibility(View == EApexHotlapView::Garage ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
	}
	if (TimingPanel)
	{
		TimingPanel->SetVisibility(View == EApexHotlapView::Garage || View == EApexHotlapView::Track
			? ESlateVisibility::HitTestInvisible
			: ESlateVisibility::Collapsed);
	}
	if (ReplayStrip)
	{
		ReplayStrip->SetVisibility(View == EApexHotlapView::Replay ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
	}
	if (TrackHint)
	{
		TrackHint->SetVisibility(View == EApexHotlapView::Track ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
	}
}

void UApexHotlapWidget::FocusDefault()
{
	if (View != EApexHotlapView::Garage || !(GoOutButton && ApexNav::Focus(GoOutButton)))
	{
		SetKeyboardFocus();
	}
}

void UApexHotlapWidget::SetLiveLap(int32 Lap, int32 LapTimeMs, bool bInvalid, int32 BestLapTimeMs)
{
	LiveLap = Lap;
	LiveLapTimeMs = LapTimeMs;
	bLiveInvalid = bInvalid;
	if (BestLapTimeMs > 0 && (BestMs == 0 || BestLapTimeMs < BestMs))
	{
		BestMs = BestLapTimeMs;
	}
	if (!SheetLiveText || !SheetLiveLabel)
	{
		return;
	}
	if (View == EApexHotlapView::Garage)
	{
		SheetLiveLabel->SetText(FText::FromString(TEXT("IN GARAGE")));
		SheetLiveLabel->SetColorAndOpacity(Palette::TextMuted);
		SheetLiveText->SetText(FText::FromString(TEXT("--:--.---")));
		SheetLiveText->SetColorAndOpacity(Palette::TextMuted);
	}
	else if (Lap <= 0)
	{
		SheetLiveLabel->SetText(FText::FromString(TEXT("OUT LAP")));
		SheetLiveLabel->SetColorAndOpacity(Palette::TextSecondary);
		SheetLiveText->SetText(FText::FromString(TEXT("--:--.---")));
		SheetLiveText->SetColorAndOpacity(Palette::TextSecondary);
	}
	else
	{
		SheetLiveLabel->SetText(FText::FromString(bInvalid ? TEXT("LAP INVALID") : FString::Printf(TEXT("LAP %d"), Lap)));
		SheetLiveLabel->SetColorAndOpacity(bInvalid ? Palette::Error : Palette::Live);
		SheetLiveText->SetText(FText::FromString(FormatMs(LapTimeMs)));
		SheetLiveText->SetColorAndOpacity(bInvalid ? Palette::TextMuted : Palette::TextPrimary);
	}
}

void UApexHotlapWidget::SetReplayTime(float ReplayMs, int32 LapTimeMs)
{
	if (ReplayText)
	{
		ReplayText->SetText(FText::FromString(FString::Printf(TEXT("BEST LAP  %s  /  %s"),
			*FormatMs(FMath::Max(0, FMath::RoundToInt(ReplayMs))), *FormatMs(LapTimeMs))));
	}
}

void UApexHotlapWidget::HandleLapTiming(const FApexLapTiming& Timing)
{
	const UGameInstance* GameInstance = GetGameInstance();
	const UApexNetSubsystem* Net = GameInstance ? GameInstance->GetSubsystem<UApexNetSubsystem>() : nullptr;
	if (!bActive || !Net || !Timing.bIsLapEnd || Timing.CarIndex != Net->GetLocalCarIndex())
	{
		return;
	}
	FLapEntry Entry;
	Entry.Lap = Timing.Lap;
	Entry.TimeMs = Timing.LapTimeMs;
	Entry.bValid = Timing.bValid;
	Entry.bPersonalBest = Timing.bPersonalBestLap;
	Entry.bSessionBest = Timing.bSessionBestLap;
	Laps.Add(Entry);
	if (Entry.bValid && Entry.TimeMs > 0 && (BestMs == 0 || Entry.TimeMs < BestMs))
	{
		BestMs = Entry.TimeMs;
	}
	RefreshSheet();
}

void UApexHotlapWidget::RefreshSheet()
{
	if (!SheetRows)
	{
		return;
	}
	SheetRows->ClearChildren();
	const int32 First = FMath::Max(0, Laps.Num() - SheetRowCount);
	if (Laps.Num() == 0)
	{
		AddV(SheetRows, MakeText(*WidgetTree, TEXT("No laps yet"), Font::Body(12.0f), Palette::TextMuted));
	}
	for (int32 Index = First; Index < Laps.Num(); ++Index)
	{
		const FLapEntry& Entry = Laps[Index];
		UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();
		const FLinearColor Colour = !Entry.bValid ? Palette::TextDisabled
			: Entry.bSessionBest ? Purple
			: Entry.bPersonalBest ? Palette::Live
			: Palette::TextPrimary;
		AddH(Row, MakeText(*WidgetTree, FString::Printf(TEXT("L%d"), Entry.Lap), Font::Mono(12.0f), Palette::TextMuted),
			FMargin(), VAlign_Center);
		AddH(Row, MakeText(*WidgetTree, FormatMs(Entry.TimeMs), Font::Mono(14.0f), Colour),
			FMargin(14.0f, 0.0f, 0.0f, 0.0f), VAlign_Center, 1.0f);
		FString Right;
		FLinearColor RightColour = Palette::TextMuted;
		if (!Entry.bValid)
		{
			Right = TEXT("INVALID");
			RightColour = Palette::Error;
		}
		else if (BestMs > 0 && Entry.TimeMs != BestMs)
		{
			Right = FormatDelta(Entry.TimeMs, BestMs);
		}
		else if (BestMs > 0)
		{
			Right = TEXT("BEST");
			RightColour = Colour;
		}
		AddH(Row, MakeText(*WidgetTree, Right, Font::Mono(11.0f, 40), RightColour), FMargin(), VAlign_Center);
		AddV(SheetRows, Row, FMargin(0.0f, Index == First ? 0.0f : 5.0f, 0.0f, 0.0f));
	}
	if (SheetBestText)
	{
		SheetBestText->SetText(FText::FromString(FormatMs(BestMs)));
	}
	if (SheetRecordText)
	{
		const UGameInstance* GameInstance = GetGameInstance();
		const UApexNetSubsystem* Net = GameInstance ? GameInstance->GetSubsystem<UApexNetSubsystem>() : nullptr;
		const int32 RecordMs = Net ? Net->GetLapRecord().LapTimeMs : 0;
		SheetRecordText->SetText(FText::FromString(FormatMs(RecordMs)));
	}
	SetLiveLap(LiveLap, LiveLapTimeMs, bLiveInvalid, 0);
}

void UApexHotlapWidget::RefreshGhostRows()
{
	const UGameInstance* GameInstance = GetGameInstance();
	const UApexNetSubsystem* Net = GameInstance ? GameInstance->GetSubsystem<UApexNetSubsystem>() : nullptr;
	const UApexSettingsSubsystem* Settings = GetSettings();
	const bool bHasGhost = Net && Net->GetGhostLap().IsValid();
	if (ReplayButton)
	{
		// The button stays; without a lap it says so rather than vanishing,
		// which is what tells a new driver a replay exists at all.
		ReplayButton->SetBadge(bHasGhost
			? FormatMs(Net->GetGhostLap().LapTimeMs)
			: (Net && Net->GetLapRecord().bHasGhost ? TEXT("Loading") : TEXT("No lap yet")),
			bHasGhost ? Palette::TextPrimary : Palette::TextMuted);
	}
	if (GhostButton)
	{
		const bool bOn = Settings && Settings->Get() ? Settings->Get()->bGhostCar : true;
		GhostButton->SetBadge(bHasGhost ? (bOn ? TEXT("On") : TEXT("Off")) : TEXT("No lap yet"),
			bOn && bHasGhost ? Palette::Live : Palette::TextMuted);
	}
}

void UApexHotlapWidget::RefreshSetup()
{
	const UApexSettingsSubsystem* Settings = GetSettings();
	const UApexSettingsSave* Values = Settings ? Settings->Get() : nullptr;
	if (!Values)
	{
		return;
	}
	bRefreshing = true;
	for (const TPair<int32, TObjectPtr<UApexStepperWidget>>& Entry : SetupSteppers)
	{
		if (Entry.Value)
		{
			Entry.Value->SetValue(Values->CarSetup.GetClick(Entry.Key));
		}
	}
	bRefreshing = false;
	if (ResetButton)
	{
		const int32 Changed = Values->CarSetup.CountChanged();
		ResetButton->SetBadge(Changed == 0 ? TEXT("Stock") : FString::Printf(TEXT("%d off stock"), Changed),
			Changed == 0 ? Palette::TextMuted : Palette::Accent);
	}
}

void UApexHotlapWidget::HandleButtonActivated(UApexButtonWidget* Button)
{
	if (!Button)
	{
		return;
	}
	const FName Id = Button->GetActionId();
	if (Id == ActionGoOut)
	{
		OnAction.Broadcast(EApexHotlapAction::GoOut);
	}
	else if (Id == ActionReplay)
	{
		OnAction.Broadcast(EApexHotlapAction::ReplayBestLap);
	}
	else if (Id == ActionGhost)
	{
		OnAction.Broadcast(EApexHotlapAction::ToggleGhost);
	}
	else if (Id == ActionResetSetup)
	{
		OnAction.Broadcast(EApexHotlapAction::ResetSetup);
	}
}

void UApexHotlapWidget::HandleStepperChanged(UApexStepperWidget* Control, int32 Value)
{
	UApexSettingsSubsystem* Settings = GetSettings();
	if (bRefreshing || !Control || !Settings)
	{
		return;
	}
	ApexUiAudio::Play(this, EApexUiSound::Adjust);
	Settings->SetCarSetupClick(Control->Index, Value);
}

void UApexHotlapWidget::HandleSettingsChanged(EApexSettingsGroup Group)
{
	if (Group == EApexSettingsGroup::CarSetup)
	{
		RefreshSetup();
	}
	else if (Group == EApexSettingsGroup::Gameplay)
	{
		RefreshGhostRows();
	}
}

void UApexHotlapWidget::HandleGhostLap(const FApexGhostLap& Lap)
{
	RefreshGhostRows();
}

void UApexHotlapWidget::HandleLapRecord(const FApexLapRecord& Record)
{
	RefreshGhostRows();
	RefreshSheet();
}

void UApexHotlapWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);
}
