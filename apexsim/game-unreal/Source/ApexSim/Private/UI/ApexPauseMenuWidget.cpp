#include "UI/ApexPauseMenuWidget.h"

#include "ApexReplayRecorder.h"
#include "ApexMenuFlowSubsystem.h"
#include "ApexNetSubsystem.h"
#include "ApexPitPlanSubsystem.h"
#include "ApexSettingsSubsystem.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/HorizontalBox.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Engine/GameInstance.h"
#include "Race/ApexLapTraceSubsystem.h"
#include "Race/ApexRaceCoordinate.h"
#include "UI/ApexButtonWidget.h"
#include "UI/ApexLapTraceWidget.h"
#include "UI/ApexStepperWidget.h"
#include "UI/ApexUIStyle.h"

using namespace ApexUI;

namespace
{
	constexpr float PauseCardWidth = 682.0f;
	constexpr float PauseRowHeight = 82.0f;
	constexpr float PrimaryRowHeight = 92.0f;

	const FName ActionResume   = TEXT("Resume");
	const FName ActionSettings = TEXT("Settings");
	const FName ActionPauseLeave    = TEXT("Leave");
	const FName ActionPauseSaveReplay    = TEXT("SaveReplay");
	const FName ActionGarage   = TEXT("Garage");
	const FName ActionRecoverTrack = TEXT("RecoverTrack");
	const FName ActionRecoverPits  = TEXT("RecoverPits");
	const FName ActionRewind       = TEXT("Rewind");
	const FName ActionPhoto        = TEXT("Photo");
	const FName ActionQuit     = TEXT("Quit");
	const FName ActionPitPlan  = TEXT("PitPlan");
	const FName ActionPitPlanDone = TEXT("PitPlanDone");
	const FName ActionLapTraces = TEXT("LapTraces");
	const FName ActionLapTracesDone = TEXT("LapTracesDone");
	/** The card while the traces are up: room for a lap to read. */
	constexpr float LapTracesCardWidth = 1240.0f;
	constexpr float LapTraceGraphHeight = 460.0f;
	/** The traces' two pickers: the lap drawn, and the one it is drawn against. */
	enum ELapTracePick : int32 { PickLap = 0, PickAgainst, PickCount };
	constexpr float LapTracePickReadoutWidth = 300.0f;

	/** The pit plan's rows, in the order the crew works, then the request. */
	enum EPitPlanRow : int32 { PlanTyres = 0, PlanFuel, PlanRepair, PlanPads, PlanPadSet, PlanBox, PlanRowCount };
	constexpr float PitPlanRowHeight = 52.0f;
	constexpr float PitPlanReadoutWidth = 230.0f;
}

UApexPauseMenuWidget::UApexPauseMenuWidget(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	SetVisibility(ESlateVisibility::Collapsed);
	SetIsFocusable(true);
}

void UApexPauseMenuWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();

	// --- The card -------------------------------------------------------------

	UVerticalBox* Card = WidgetTree->ConstructWidget<UVerticalBox>();

	UVerticalBox* Heading = WidgetTree->ConstructWidget<UVerticalBox>();
	AddV(Heading, MakeLabel(*WidgetTree, TEXT("Session paused"), Palette::Accent));
	HeadingText = MakeText(*WidgetTree, TEXT("PAUSED"), Font::Display(52.0f, 20), Palette::TextPrimary);
	AddV(Heading, HeadingText, FMargin(0.0f, 10.0f, 0.0f, 0.0f));
	AddV(Card, MakePanel(*WidgetTree, Heading, FMargin(42.0f, 34.0f), MakeBrush(Palette::Surface)));
	AddV(Card, MakeDivider(*WidgetTree));

	UVerticalBox* Actions = WidgetTree->ConstructWidget<UVerticalBox>();

	// Resume is first and filled: it sits where the cursor is already resting,
	// so a second Escape and a click do the same thing.
	AddRow(Actions, TEXT("Close menu"), FString(), ActionResume, /*bPrimary*/ true, TEXT("ESC"));
	AddRow(Actions, TEXT("SETTINGS"), TEXT("Gameplay · Graphics · Controls"), ActionSettings, false);
	// The driver's orders for the next stop, and the box-this-lap request.
	PitPlanRow = AddRow(Actions, TEXT("PIT PLAN"), TEXT("Crew's call"), ActionPitPlan, false);
	PitPlanRow->SetVisibility(ESlateVisibility::Collapsed);
	// The last lap against the best, by distance; once a lap is on record.
	LapTracesRow = AddRow(Actions, TEXT("LAP TRACES"), TEXT("Any two laps"), ActionLapTraces, false);
	LapTracesRow->SetVisibility(ESlateVisibility::Collapsed);
	// A hotlap's way back in: the car is parked, repaired and out of the way,
	// with the setup card up. Collapsed in every other mode and in the garage.
	GarageRow = AddRow(Actions, TEXT("BACK TO GARAGE"), TEXT("Tune, replay, go again"), ActionGarage, false);
	GarageRow->SetVisibility(ESlateVisibility::Collapsed);
	// A car stuck against a wall: put back where it is, or towed to its box.
	// The server holds it there (the time cost) and refuses a moving car.
	RecoverTrackRow = AddRow(Actions, TEXT("BACK TO TRACK"), TEXT("Held 8 s · lap struck"), ActionRecoverTrack, false);
	RecoverTrackRow->SetVisibility(ESlateVisibility::Collapsed);
	RecoverPitsRow = AddRow(Actions, TEXT("BACK TO PITS"), TEXT("Towed 30 s · serviced"), ActionRecoverPits, false);
	RecoverPitsRow->SetVisibility(ESlateVisibility::Collapsed);
	// Practice alone: the session goes back, the lap through it struck.
	RewindRow = AddRow(Actions, TEXT("REWIND 10 S"), TEXT("Practice alone · lap struck"), ActionRewind, false);
	RewindRow->SetVisibility(ESlateVisibility::Collapsed);
	// Watching: a free camera over the race held still, and a big shot of it.
	PhotoRow = AddRow(Actions, TEXT("PHOTO MODE"), TEXT("Free camera · lens · shot"), ActionPhoto, false);
	PhotoRow->SetVisibility(ESlateVisibility::Collapsed);
	// Every session is recorded; this keeps it (Saved/Replays) rather than
	// leaving it among the recent ones that make way for newer sessions.
	SaveReplayRow = AddRow(Actions, TEXT("SAVE REPLAY"), TEXT("Watch it again later"), ActionPauseSaveReplay, false);
	SaveReplayRow->SetVisibility(ESlateVisibility::Collapsed);

	// The two exits are separated from the rest and carry a consequence label.
	// Leaving the session and quitting are never adjacent to Resume.
	AddV(Actions, MakeDivider(*WidgetTree), FMargin(0.0f, 12.0f, 0.0f, 12.0f));
	LeaveRow = AddRow(Actions, TEXT("BACK TO MAIN MENU"), TEXT("Session ends"), ActionPauseLeave, false);
	AddRow(Actions, TEXT("EXIT"), TEXT("Quit ApexSim"), ActionQuit, false);

	ActionsPanel = MakePanel(*WidgetTree, Actions, FMargin(22.0f, 22.0f), MakeBrush(Palette::Background));
	AddV(Card, ActionsPanel);

	// --- The pit plan ----------------------------------------------------------
	// Each order a stepper, sent as it moves; the crew's own call at every
	// order's first stop.
	{
		UVerticalBox* Plan = WidgetTree->ConstructWidget<UVerticalBox>();
		static const TCHAR* const Labels[PlanRowCount] = {
			TEXT("Tyres"), TEXT("Fuel"), TEXT("Repairs"), TEXT("Brake pads"), TEXT("Pad set"), TEXT("Box this lap"),
		};
		for (int32 Index = 0; Index < PlanRowCount; ++Index)
		{
			UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();
			AddH(Row, MakeText(*WidgetTree, FString(Labels[Index]).ToUpper(), Font::Display(22.0f), Palette::TextPrimary),
				FMargin(6.0f, 0.0f, 0.0f, 0.0f), VAlign_Center, 1.0f);
			UApexStepperWidget* Stepper = WidgetTree->ConstructWidget<UApexStepperWidget>();
			Stepper->Index = Index;
			Stepper->OnChanged.AddDynamic(this, &UApexPauseMenuWidget::HandlePitPlanChanged);
			AddH(Row, Stepper, FMargin(), VAlign_Center);
			if (Index == PlanBox)
			{
				AddV(Plan, MakeDivider(*WidgetTree), FMargin(0.0f, 8.0f, 0.0f, 4.0f));
			}
			AddV(Plan, MakeSized(*WidgetTree, Row, -1.0f, PitPlanRowHeight), FMargin(0.0f, Index == 0 ? 0.0f : 4.0f, 0.0f, 0.0f));
			PitPlanSteppers.Add(Stepper);
		}
		PitPlanSummaryText = MakeText(*WidgetTree, FString(), Font::Mono(11.0f, 80), Palette::TextSecondary);
		PitPlanSummaryText->SetAutoWrapText(true);
		AddV(Plan, PitPlanSummaryText, FMargin(6.0f, 14.0f, 6.0f, 12.0f));

		FApexButtonSpec Done;
		Done.Label = TEXT("DONE");
		Done.Badge = TEXT("Back to the menu");
		Done.KeyCap = TEXT("ESC");
		Done.ActionId = ActionPitPlanDone;
		Done.Variant = EApexButtonVariant::Panel;
		Done.Height = PauseRowHeight;
		Done.LabelSize = 24.0f;
		Done.Sound = EApexUiSound::Back;
		PitPlanDoneButton = WidgetTree->ConstructWidget<UApexButtonWidget>();
		PitPlanDoneButton->Setup(Done);
		PitPlanDoneButton->OnActivated.AddDynamic(this, &UApexPauseMenuWidget::HandleButtonActivated);
		AddV(Plan, PitPlanDoneButton);

		PitPlanPanel = MakePanel(*WidgetTree, Plan, FMargin(22.0f, 22.0f), MakeBrush(Palette::Background));
		PitPlanPanel->SetVisibility(ESlateVisibility::Collapsed);
		AddV(Card, PitPlanPanel);
	}

	// --- The lap traces --------------------------------------------------------
	{
		UVerticalBox* Traces = WidgetTree->ConstructWidget<UVerticalBox>();
		LapTracesLegend = MakeText(*WidgetTree, FString(), Font::Mono(12.0f, 60), Palette::TextPrimary);
		LapTracesLegend->SetAutoWrapText(true);
		AddV(Traces, LapTracesLegend, FMargin(6.0f, 0.0f, 6.0f, 12.0f));
		// Any two held laps: LAP is drawn in the accent, AGAINST muted.
		UHorizontalBox* Picks = WidgetTree->ConstructWidget<UHorizontalBox>();
		static const TCHAR* const PickLabels[PickCount] = { TEXT("LAP"), TEXT("AGAINST") };
		for (int32 Index = 0; Index < PickCount; ++Index)
		{
			AddH(Picks, MakeText(*WidgetTree, PickLabels[Index], Font::Display(20.0f), Palette::TextPrimary),
				FMargin(6.0f, 0.0f, 12.0f, 0.0f), VAlign_Center);
			UApexStepperWidget* Stepper = WidgetTree->ConstructWidget<UApexStepperWidget>();
			Stepper->Index = Index;
			Stepper->OnChanged.AddDynamic(this, &UApexPauseMenuWidget::HandleLapTracePicked);
			AddH(Picks, Stepper, FMargin(0.0f, 0.0f, 24.0f, 0.0f), VAlign_Center);
			LapTraceSteppers.Add(Stepper);
		}
		AddV(Traces, MakeSized(*WidgetTree, Picks, -1.0f, PitPlanRowHeight), FMargin(0.0f, 0.0f, 0.0f, 10.0f));
		LapTraceGraph = WidgetTree->ConstructWidget<UApexLapTraceWidget>();
		AddV(Traces, MakeSized(*WidgetTree, LapTraceGraph, -1.0f, LapTraceGraphHeight), FMargin(6.0f, 0.0f));
		LapTracesNote = MakeText(*WidgetTree, FString(), Font::Mono(10.0f, 60), Palette::TextMuted);
		LapTracesNote->SetAutoWrapText(true);
		AddV(Traces, LapTracesNote, FMargin(6.0f, 12.0f, 6.0f, 12.0f));

		FApexButtonSpec Done;
		Done.Label = TEXT("DONE");
		Done.Badge = TEXT("Back to the menu");
		Done.KeyCap = TEXT("ESC");
		Done.ActionId = ActionLapTracesDone;
		Done.Variant = EApexButtonVariant::Panel;
		Done.Height = PauseRowHeight;
		Done.LabelSize = 24.0f;
		Done.Sound = EApexUiSound::Back;
		LapTracesDoneButton = WidgetTree->ConstructWidget<UApexButtonWidget>();
		LapTracesDoneButton->Setup(Done);
		LapTracesDoneButton->OnActivated.AddDynamic(this, &UApexPauseMenuWidget::HandleButtonActivated);
		AddV(Traces, LapTracesDoneButton);

		LapTracesPanel = MakePanel(*WidgetTree, Traces, FMargin(22.0f, 22.0f), MakeBrush(Palette::Background));
		LapTracesPanel->SetVisibility(ESlateVisibility::Collapsed);
		AddV(Card, LapTracesPanel);
	}

	AddV(Card, MakeKeyHintBar(
		*WidgetTree,
		{ { TEXT("↑↓"), TEXT("Move") }, { TEXT("Enter"), TEXT("Select") }, { TEXT("Esc"), TEXT("Resume") } },
		{}));

	UBorder* CardPanel = MakeModalCard(*WidgetTree, Card);

	// --- The status strip -----------------------------------------------------

	UHorizontalBox* Strip = WidgetTree->ConstructWidget<UHorizontalBox>();

	StatusBadgeText = MakeText(*WidgetTree, TEXT("LIVE"), Font::Mono(10.0f, 120), Palette::OnAccent);
	StatusBadge = MakePanel(*WidgetTree, StatusBadgeText, FMargin(12.0f, 6.0f), MakeBrush(Palette::Accent));
	AddH(Strip, StatusBadge);

	StatusText = MakeText(*WidgetTree, FString(), Font::Mono(12.0f, 60), Palette::TextSecondary);
	AddH(Strip, StatusText, FMargin(18.0f, 0.0f, 0.0f, 0.0f));

	// --- Assembly -------------------------------------------------------------

	UOverlay* Content = WidgetTree->ConstructWidget<UOverlay>();

	UOverlaySlot* StripSlot = Content->AddChildToOverlay(Strip);
	StripSlot->SetHorizontalAlignment(HAlign_Left);
	StripSlot->SetVerticalAlignment(VAlign_Top);
	StripSlot->SetPadding(FMargin(Metrics::PageGutter, 34.0f, 0.0f, 0.0f));

	CardSize = MakeSized(*WidgetTree, CardPanel, PauseCardWidth, -1.0f);
	UOverlaySlot* CardSlot = Content->AddChildToOverlay(CardSize);
	CardSlot->SetHorizontalAlignment(HAlign_Center);
	CardSlot->SetVerticalAlignment(VAlign_Center);

	WidgetTree->RootWidget = MakeScrim(*WidgetTree, Content);
}

UApexButtonWidget* UApexPauseMenuWidget::AddRow(
	UVerticalBox* Stack,
	const FString& Label,
	const FString& Badge,
	FName ActionId,
	bool bPrimary,
	const FString& KeyCap)
{
	UApexButtonWidget* Button = WidgetTree->ConstructWidget<UApexButtonWidget>();

	FApexButtonSpec Spec;
	Spec.Label = Label;
	Spec.Badge = Badge;
	Spec.KeyCap = KeyCap;
	Spec.ActionId = ActionId;
	Spec.Variant = bPrimary ? EApexButtonVariant::Primary : EApexButtonVariant::Panel;
	Spec.Height = bPrimary ? PrimaryRowHeight : PauseRowHeight;
	Spec.LabelSize = bPrimary ? 27.0f : 24.0f;
	if (ActionId == ActionResume)
	{
		// Closing the menu is the step back its key cap says it is.
		Spec.Sound = EApexUiSound::Back;
	}
	Button->Setup(Spec);
	Button->OnActivated.AddDynamic(this, &UApexPauseMenuWidget::HandleButtonActivated);

	AddV(Stack, Button, FMargin(0.0f, Stack->GetChildrenCount() == 0 ? 0.0f : 2.0f, 0.0f, 0.0f));
	Rows.Add(Button);
	return Button;
}

void UApexPauseMenuWidget::Open()
{
	// Not guarded on bOpen: coming back from settings the menu never went away,
	// and what it needs is the focus back, not a fresh open.
	bOpen = true;

	if (LeaveRow)
	{
		LeaveRow->SetLabel(bWatching ? TEXT("STOP WATCHING") : TEXT("BACK TO MAIN MENU"));
		LeaveRow->SetBadge(bWatching ? TEXT("Back to the menu") : TEXT("Session ends"), Palette::TextMuted);
	}
	if (SaveReplayRow)
	{
		const UApexReplayRecorder* Recorder = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexReplayRecorder>() : nullptr;
		const bool bCanSave = Recorder && Recorder->IsRecording() && Recorder->HasSomethingToSave();
		SaveReplayRow->SetVisibility(bCanSave ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
		if (bCanSave)
		{
			const int32 Seconds = FMath::FloorToInt(Recorder->GetRecordedSeconds());
			SaveReplayRow->SetBadge(FString::Printf(TEXT("%d:%02d recorded"), Seconds / 60, Seconds % 60), Palette::TextMuted);
		}
	}
	SetVisibility(ESlateVisibility::Visible);
	RefreshStatusStrip();
	FocusDefault();
}

void UApexPauseMenuWidget::FocusDefault()
{
	if (bLapTracesOpen)
	{
		UWidget* First = LapTraceSteppers.Num() > 0 ? ApexNav::FindFirstFocusable(LapTraceSteppers[0]) : nullptr;
		if (!ApexNav::Focus(First) && !ApexNav::Focus(LapTracesDoneButton))
		{
			SetKeyboardFocus();
		}
		return;
	}
	if (bPitPlanOpen)
	{
		UWidget* First = PitPlanSteppers.Num() > 0 ? ApexNav::FindFirstFocusable(PitPlanSteppers[0]) : nullptr;
		if (!ApexNav::Focus(First))
		{
			SetKeyboardFocus();
		}
		return;
	}
	if (!(Rows.Num() > 0 && ApexNav::Focus(Rows[0])))
	{
		SetKeyboardFocus();
	}
}

void UApexPauseMenuWidget::Close()
{
	if (!bOpen)
	{
		return;
	}
	bOpen = false;
	ClosePitPlan();
	CloseLapTraces();
	SetVisibility(ESlateVisibility::Collapsed);
}

void UApexPauseMenuWidget::OpenPitPlan()
{
	if (!PitPlanPanel || !ActionsPanel)
	{
		return;
	}
	bPitPlanOpen = true;
	RefreshPitPlan();
	ActionsPanel->SetVisibility(ESlateVisibility::Collapsed);
	PitPlanPanel->SetVisibility(ESlateVisibility::SelfHitTestInvisible);
	if (HeadingText)
	{
		HeadingText->SetText(FText::FromString(TEXT("PIT PLAN")));
	}
	FocusDefault();
}

void UApexPauseMenuWidget::ClosePitPlan()
{
	if (!bPitPlanOpen)
	{
		return;
	}
	bPitPlanOpen = false;
	if (PitPlanPanel)
	{
		PitPlanPanel->SetVisibility(ESlateVisibility::Collapsed);
	}
	if (ActionsPanel)
	{
		ActionsPanel->SetVisibility(ESlateVisibility::SelfHitTestInvisible);
	}
	if (HeadingText)
	{
		HeadingText->SetText(FText::FromString(TEXT("PAUSED")));
	}
	RefreshStatusStrip();
}

void UApexPauseMenuWidget::OpenLapTraces()
{
	const UApexLapTraceSubsystem* Traces = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexLapTraceSubsystem>() : nullptr;
	if (!LapTracesPanel || !ActionsPanel || !Traces || LapTraceSteppers.Num() != PickCount)
	{
		return;
	}
	bLapTracesOpen = true;
	const TArray<FApexLapTrace>& Laps = Traces->GetLaps();
	const int32 BestIndex = Traces->GetBestIndex();
	// The last lap against the best; the best against itself says nothing,
	// so when the last lap is the best, the lap before it is shown.
	int32 LapIndex = Laps.Num() > 1 && BestIndex == 0 ? 1 : 0;
	// LAP is one of the player's own: the record lap is only ever against.
	while (Laps.IsValidIndex(LapIndex + 1) && Laps[LapIndex].bRecord)
	{
		++LapIndex;
	}
	const int32 AgainstIndex = BestIndex != INDEX_NONE ? BestIndex : FMath::Min(1, Laps.Num() - 1);
	auto Time = [](int32 Ms) { return FString::Printf(TEXT("%d:%02d.%03d"), Ms / 60000, (Ms / 1000) % 60, Ms % 1000); };
	for (UApexStepperWidget* Stepper : LapTraceSteppers)
	{
		Stepper->Formatter = [Traces, Time](int32 Value)
		{
			const TArray<FApexLapTrace>& Held = Traces->GetLaps();
			if (!Held.IsValidIndex(Value))
			{
				return FString(TEXT("NO LAP"));
			}
			const FApexLapTrace& Lap = Held[Value];
			if (Lap.bRecord)
			{
				return FString::Printf(TEXT("RECORD  %s"), *Time(Lap.LapMs));
			}
			return FString::Printf(TEXT("LAP %d  %s%s"), Lap.LapNumber, *Time(Lap.LapMs),
				Value == Traces->GetBestIndex() ? TEXT("  BEST") : (Lap.bValid ? TEXT("") : TEXT("  INV")));
		};
		const int32 Max = FMath::Max(Laps.Num() - 1, 0);
		Stepper->Setup(0, Max, Stepper->Index == PickLap ? LapIndex : FMath::Max(AgainstIndex, 0), LapTracePickReadoutWidth);
	}
	ShowPickedLapTraces();
	LapTracesNote->SetText(FText::FromString(FString::Printf(
		TEXT("Accent: the lap; grey: the one against it; red: braking. Every whole lap is saved as CSV in %s"), *Traces->GetFolder())));
	ActionsPanel->SetVisibility(ESlateVisibility::Collapsed);
	LapTracesPanel->SetVisibility(ESlateVisibility::SelfHitTestInvisible);
	if (CardSize)
	{
		CardSize->SetWidthOverride(LapTracesCardWidth);
	}
	if (HeadingText)
	{
		HeadingText->SetText(FText::FromString(TEXT("LAP TRACES")));
	}
	FocusDefault();
}

void UApexPauseMenuWidget::HandleLapTracePicked(UApexStepperWidget* Control, int32 Value)
{
	ShowPickedLapTraces();
}

void UApexPauseMenuWidget::ShowPickedLapTraces()
{
	const UApexLapTraceSubsystem* Traces = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexLapTraceSubsystem>() : nullptr;
	if (!Traces || LapTraceSteppers.Num() != PickCount || !LapTraceGraph || !LapTracesLegend)
	{
		return;
	}
	const TArray<FApexLapTrace>& Laps = Traces->GetLaps();
	static const FApexLapTrace None;
	const int32 LapIndex = LapTraceSteppers[PickLap]->GetValue();
	const int32 AgainstIndex = LapTraceSteppers[PickAgainst]->GetValue();
	const FApexLapTrace& Lap = Laps.IsValidIndex(LapIndex) ? Laps[LapIndex] : None;
	const FApexLapTrace& Against = Laps.IsValidIndex(AgainstIndex) && AgainstIndex != LapIndex ? Laps[AgainstIndex] : None;
	LapTraceGraph->SetTraces(Against, Lap);
	auto Time = [](int32 Ms) { return FString::Printf(TEXT("%d:%02d.%03d"), Ms / 60000, (Ms / 1000) % 60, Ms % 1000); };
	auto Name = [&](const FApexLapTrace& Trace, int32 Index)
	{
		// FDateTime::ToString has no month name: "18 OCT 18:41".
		static const TCHAR* const Months[] = { TEXT("JAN"), TEXT("FEB"), TEXT("MAR"), TEXT("APR"), TEXT("MAY"), TEXT("JUN"),
			TEXT("JUL"), TEXT("AUG"), TEXT("SEP"), TEXT("OCT"), TEXT("NOV"), TEXT("DEC") };
		if (Trace.bRecord)
		{
			return FString::Printf(TEXT("RECORD  %s  (the circuit's record for the car; no pedals)"), *Time(Trace.LapMs));
		}
		const FString When = FString::Printf(TEXT("%d %s %s"), Trace.RecordedAt.GetDay(),
			Months[FMath::Clamp(Trace.RecordedAt.GetMonth(), 1, 12) - 1], *Trace.RecordedAt.ToString(TEXT("%H:%M")));
		return FString::Printf(TEXT("LAP %d  %s  %s%s%s"), Trace.LapNumber, *Time(Trace.LapMs), *When,
			Index == Traces->GetBestIndex() ? TEXT("  BEST") : TEXT(""), Trace.bValid ? TEXT("") : TEXT("  INVALID"));
	};
	TArray<FString> Legend;
	if (!Lap.IsEmpty())
	{
		Legend.Add(Name(Lap, LapIndex));
	}
	if (!Against.IsEmpty())
	{
		Legend.Add(FString::Printf(TEXT("AGAINST %s"), *Name(Against, AgainstIndex)));
	}
	if (!Lap.IsEmpty() && !Against.IsEmpty())
	{
		const int32 Gap = Lap.LapMs - Against.LapMs;
		Legend.Add(FString::Printf(TEXT("%s%.3f"), Gap >= 0 ? TEXT("+") : TEXT("-"), FMath::Abs(Gap) / 1000.0f));
	}
	LapTracesLegend->SetText(FText::FromString(Legend.Num() > 0 ? FString::Join(Legend, TEXT("     ")) : FString(TEXT("NO LAP YET"))));
}

void UApexPauseMenuWidget::CloseLapTraces()
{
	if (!bLapTracesOpen)
	{
		return;
	}
	bLapTracesOpen = false;
	if (LapTracesPanel)
	{
		LapTracesPanel->SetVisibility(ESlateVisibility::Collapsed);
	}
	if (ActionsPanel)
	{
		ActionsPanel->SetVisibility(ESlateVisibility::SelfHitTestInvisible);
	}
	if (CardSize)
	{
		CardSize->SetWidthOverride(PauseCardWidth);
	}
	if (HeadingText)
	{
		HeadingText->SetText(FText::FromString(TEXT("PAUSED")));
	}
	RefreshStatusStrip();
}

void UApexPauseMenuWidget::RefreshPitPlan()
{
	const UApexPitPlanSubsystem* PitPlan = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexPitPlanSubsystem>() : nullptr;
	if (!PitPlan || PitPlanSteppers.Num() != PlanRowCount)
	{
		return;
	}
	const FApexPitPlan& Plan = PitPlan->GetPlan();
	const TArray<FString> Compounds = PitPlan->GetCompounds();

	PitPlanSteppers[PlanTyres]->Formatter = [Compounds](int32 Value) { return ApexPitPlan::TyreChoiceLabel(Value, Compounds); };
	PitPlanSteppers[PlanFuel]->Formatter = [](int32 Value) { return ApexPitPlan::FuelChoiceLabel(Value); };
	PitPlanSteppers[PlanRepair]->Formatter = [](int32 Value) { return ApexPitPlan::RepairChoiceLabel(Value); };
	PitPlanSteppers[PlanPads]->Formatter = [](int32 Value) { return ApexPitPlan::PadsChoiceLabel(Value); };
	PitPlanSteppers[PlanPadSet]->Formatter = [](int32 Value) { return ApexPitPlan::PadSetChoiceLabel(Value); };
	PitPlanSteppers[PlanBox]->Formatter = [](int32 Value) { return Value > 0 ? FString(TEXT("BOX, BOX")) : FString(TEXT("NOT THIS LAP")); };

	// Built afresh each time the page opens: the tyres row's length is the car's.
	PitPlanSteppers[PlanTyres]->Setup(0, ApexPitPlan::TyreChoiceCount(Compounds) - 1, ApexPitPlan::TyreChoice(Plan), PitPlanReadoutWidth);
	PitPlanSteppers[PlanFuel]->Setup(-1, ApexPitPlan::FuelMaxSteps, ApexPitPlan::FuelChoice(Plan), PitPlanReadoutWidth);
	PitPlanSteppers[PlanRepair]->Setup(0, 1, Plan.bNoRepair ? 1 : 0, PitPlanReadoutWidth);
	PitPlanSteppers[PlanPads]->Setup(0, 2, ApexPitPlan::PadsChoice(Plan), PitPlanReadoutWidth);
	PitPlanSteppers[PlanPadSet]->Setup(0, 3, ApexPitPlan::PadSetChoice(Plan), PitPlanReadoutWidth);
	PitPlanSteppers[PlanBox]->Setup(0, 1, Plan.bBoxThisLap ? 1 : 0, PitPlanReadoutWidth);
	RefreshPitPlanSummary();
}

void UApexPauseMenuWidget::RefreshPitPlanSummary()
{
	const UApexPitPlanSubsystem* PitPlan = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexPitPlanSubsystem>() : nullptr;
	if (PitPlan && PitPlanSummaryText)
	{
		PitPlanSummaryText->SetText(FText::FromString(FString::Printf(TEXT("%s  ·  SENT TO THE CREW AS IT CHANGES"),
			*ApexPitPlan::Summary(PitPlan->GetPlan(), PitPlan->GetCompounds()))));
	}
}

void UApexPauseMenuWidget::HandlePitPlanChanged(UApexStepperWidget* Control, int32 Value)
{
	UApexPitPlanSubsystem* PitPlan = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexPitPlanSubsystem>() : nullptr;
	if (!PitPlan || !Control)
	{
		return;
	}
	FApexPitPlan Plan = PitPlan->GetPlan();
	switch (Control->Index)
	{
	case PlanTyres:  ApexPitPlan::SetTyreChoice(Plan, Value); break;
	case PlanFuel:   ApexPitPlan::SetFuelChoice(Plan, Value); break;
	case PlanRepair: Plan.bNoRepair = Value > 0; break;
	case PlanPads:   ApexPitPlan::SetPadsChoice(Plan, Value); break;
	case PlanPadSet: ApexPitPlan::SetPadSetChoice(Plan, Value); break;
	case PlanBox:    Plan.bBoxThisLap = Value > 0; break;
	default: return;
	}
	PitPlan->SetPlan(Plan);
	RefreshPitPlanSummary();
}

void UApexPauseMenuWidget::RefreshStatusStrip()
{
	const UGameInstance* GameInstance = GetGameInstance();
	const UApexNetSubsystem* Net = GameInstance ? GameInstance->GetSubsystem<UApexNetSubsystem>() : nullptr;
	const UApexMenuFlowSubsystem* Flow = GameInstance ? GameInstance->GetSubsystem<UApexMenuFlowSubsystem>() : nullptr;
	if (!Net || !StatusText)
	{
		return;
	}

	TArray<FString> Parts;
	if (bWatching)
	{
		Parts.Add(TEXT("Watching"));
	}

	const int32 LocalIndex = Net->GetLocalCarIndex();
	bool bOnTrackInHotlap = false;
	bool bCanRecover = false;
	bool bCanRewind = false;
	if (const FApexCarTelemetry* Local = Net->GetLatestTelemetry().Cars.FindByPredicate(
			[LocalIndex](const FApexCarTelemetry& Car) { return Car.CarIndex == LocalIndex; }))
	{
		const FApexTelemetryFrame& Frame = Net->GetLatestTelemetry();
		const int32 LapLimit = Frame.HasRaceClock() || !Flow ? 0 : Flow->CreateLapLimit;
		Parts.Add(LapLimit > 0
			? FString::Printf(TEXT("Lap %d / %d"), ApexRace::DisplayLap(Local->CurrentLap, LapLimit), LapLimit)
			: FString::Printf(TEXT("Lap %d"), FMath::Max(1, Local->CurrentLap)));
		// A timed race: the clock, then the lap that ends it.
		if (Frame.HasRaceClock())
		{
			const int32 Left = (Frame.RaceLeftMs + 999) / 1000;
			Parts.Add(Frame.RaceFinalLap > 0
				? FString::Printf(TEXT("Ends on lap %d"), Frame.RaceFinalLap)
				: Left >= 3600
				? FString::Printf(TEXT("%d:%02d:%02d left"), Left / 3600, Left / 60 % 60, Left % 60)
				: FString::Printf(TEXT("%d:%02d left"), Left / 60, Left % 60));
		}
		bOnTrackInHotlap = ApexIsGarageMode(Net->GetGameMode()) && !Local->bInGarage;
		const EApexGameMode Mode = Net->GetGameMode();
		const bool bRunning = Mode == EApexGameMode::FreePractice || ApexIsGarageMode(Mode)
			|| (Mode == EApexGameMode::Race && Net->GetSessionState() == EApexSessionState::Racing);
		bCanRecover = !bWatching && bRunning && !Local->bInGarage && Local->FinishPosition <= 0
			&& !Local->IsRecovering() && !Local->bPitAutopilot && !Local->bPitServicing;
		bCanRewind = !bWatching && Net->CanRewind() && Net->GetSessionState() == EApexSessionState::Racing;
	}
	if (RecoverTrackRow)
	{
		RecoverTrackRow->SetVisibility(bCanRecover ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
	}
	if (RecoverPitsRow)
	{
		RecoverPitsRow->SetVisibility(bCanRecover && !bOnTrackInHotlap ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
	}
	if (PhotoRow)
	{
		PhotoRow->SetVisibility(bWatching ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
	}
	if (RewindRow)
	{
		RewindRow->SetVisibility(bCanRewind ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
	}
	if (GarageRow)
	{
		GarageRow->SetVisibility(bOnTrackInHotlap ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
	}
	if (PitPlanRow)
	{
		// A driver with a car in a session with stops in it: a race, or free practice.
		const EApexGameMode Mode = Net->GetGameMode();
		const bool bPitPlan = !bWatching && Net->IsInSession() && !Net->IsSessionSpectator() && LocalIndex >= 0
			&& (Mode == EApexGameMode::Race || Mode == EApexGameMode::FreePractice);
		PitPlanRow->SetVisibility(bPitPlan ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
		const UApexPitPlanSubsystem* PitPlan = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexPitPlanSubsystem>() : nullptr;
		if (bPitPlan && PitPlan)
		{
			PitPlanRow->SetBadge(ApexPitPlan::Summary(PitPlan->GetPlan(), PitPlan->GetCompounds()),
				PitPlan->GetPlan().bBoxThisLap ? Palette::Live : Palette::TextMuted);
		}
	}

	if (LapTracesRow)
	{
		const UApexLapTraceSubsystem* Traces = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexLapTraceSubsystem>() : nullptr;
		const bool bTraces = !bWatching && Traces && (!Traces->GetLast().IsEmpty() || !Traces->GetBest().IsEmpty());
		LapTracesRow->SetVisibility(bTraces ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
	}

	FApexSessionSummary Session;
	if (Net->FindSessionById(Net->GetCurrentSessionId(), Session) && !Session.TrackName.IsEmpty())
	{
		Parts.Add(Session.TrackName);
	}

	if (Flow && !bWatching)
	{
		FApexCarCatalogRow CarRow;
		if (Flow->GetCarCatalogRow(Flow->GetPendingCarId(), CarRow) && !CarRow.DisplayName.IsEmpty())
		{
			Parts.Add(CarRow.DisplayName);
		}
	}

	StatusText->SetText(FText::FromString(FString::Join(Parts, TEXT("  ·  ")).ToUpper()));

	// The server has no pause: it is authoritative and the rest of the field is
	// still driving. Saying "HELD" here would be a lie the moment anyone else is
	// in the session, so the badge reports what is actually happening.
	const EApexSessionState State = Net->IsInDemoSession() ? Net->GetDemoSessionState() : Net->GetSessionState();
	const bool bSimRunning = State == EApexSessionState::Racing || State == EApexSessionState::Countdown;
	if (StatusBadgeText)
	{
		StatusBadgeText->SetText(FText::FromString(bSimRunning ? TEXT("SIM LIVE") : TEXT("HELD")));
		StatusBadgeText->SetColorAndOpacity(FSlateColor(bSimRunning ? Palette::OnAccent : Palette::TextPrimary));
	}
	if (StatusBadge)
	{
		StatusBadge->SetBrush(MakeBrush(bSimRunning ? Palette::Accent : Palette::Surface, Palette::Border, 1.0f));
	}
}

void UApexPauseMenuWidget::HandleButtonActivated(UApexButtonWidget* Button)
{
	if (!Button)
	{
		return;
	}

	const FName Action = Button->GetActionId();
	if (Action == ActionResume)
	{
		OnAction.Broadcast(EApexPauseAction::Resume);
	}
	else if (Action == ActionSettings)
	{
		OnAction.Broadcast(EApexPauseAction::OpenSettings);
	}
	else if (Action == ActionPitPlan)
	{
		OpenPitPlan();
	}
	else if (Action == ActionPitPlanDone)
	{
		ClosePitPlan();
		ApexNav::Focus(PitPlanRow);
	}
	else if (Action == ActionLapTraces)
	{
		OpenLapTraces();
	}
	else if (Action == ActionLapTracesDone)
	{
		CloseLapTraces();
		ApexNav::Focus(LapTracesRow);
	}
	else if (Action == ActionGarage)
	{
		OnAction.Broadcast(EApexPauseAction::ReturnToGarage);
	}
	else if (Action == ActionRecoverTrack)
	{
		OnAction.Broadcast(EApexPauseAction::RecoverToTrack);
	}
	else if (Action == ActionRecoverPits)
	{
		OnAction.Broadcast(EApexPauseAction::RecoverToPits);
	}
	else if (Action == ActionRewind)
	{
		OnAction.Broadcast(EApexPauseAction::Rewind);
	}
	else if (Action == ActionPhoto)
	{
		OnAction.Broadcast(EApexPauseAction::PhotoMode);
	}
	else if (Action == ActionPauseSaveReplay)
	{
		OnAction.Broadcast(EApexPauseAction::SaveReplay);
	}
	else if (Action == ActionPauseLeave)
	{
		OnAction.Broadcast(EApexPauseAction::LeaveSession);
	}
	else if (Action == ActionQuit)
	{
		OnAction.Broadcast(EApexPauseAction::QuitGame);
	}
}

FReply UApexPauseMenuWidget::NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent)
{
	// The pause key resumes as well as pauses: whichever key opened the menu
	// closes it. Escape and B arrive through HandleBack.
	const UApexSettingsSubsystem* Settings =
		GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexSettingsSubsystem>() : nullptr;
	if (Settings && Settings->IsPauseKey(InKeyEvent.GetKey()) && !InKeyEvent.IsRepeat())
	{
		OnAction.Broadcast(EApexPauseAction::Resume);
		return FReply::Handled();
	}

	return Super::NativeOnKeyDown(InGeometry, InKeyEvent);
}

bool UApexPauseMenuWidget::HandleBack()
{
	if (bLapTracesOpen)
	{
		CloseLapTraces();
		ApexNav::Focus(LapTracesRow);
		return true;
	}
	if (bPitPlanOpen)
	{
		ClosePitPlan();
		ApexNav::Focus(PitPlanRow);
		return true;
	}
	OnAction.Broadcast(EApexPauseAction::Resume);
	return true;
}

bool UApexPauseMenuWidget::HandleNavigation(EUINavigation Direction, UWidget* Source)
{
	if (bLapTracesOpen)
	{
		// The two pickers, then Done: the nearest control that way.
		ApexNav::MoveToward(this, Direction, Source);
		return true;
	}
	if (bPitPlanOpen)
	{
		// Rows of steppers, then Done: the nearest control that way, and
		// nothing beyond the card.
		ApexNav::MoveToward(this, Direction, Source);
		return true;
	}
	if (Rows.Num() == 0)
	{
		return false;
	}

	const int32 Current = ApexNav::IndexOf(Rows, Source);
	// A collapsed row (the garage and recovery rows when they do not apply)
	// is skipped over.
	auto Step = [this](int32 From, int32 Delta)
	{
		int32 Index = From;
		for (int32 Tries = 0; Tries < Rows.Num(); ++Tries)
		{
			Index = (Index + Delta + Rows.Num()) % Rows.Num();
			if (Rows[Index] && Rows[Index]->GetVisibility() != ESlateVisibility::Collapsed)
			{
				return Index;
			}
		}
		return From;
	};
	switch (Direction)
	{
	case EUINavigation::Up:
	case EUINavigation::Previous:
		ApexNav::Focus(Rows[Current == INDEX_NONE ? 0 : Step(Current, -1)]);
		return true;

	case EUINavigation::Down:
	case EUINavigation::Next:
		ApexNav::Focus(Rows[Current == INDEX_NONE ? 0 : Step(Current, 1)]);
		return true;

	default:
		// Nothing to the left or right of the card; stay put rather than let
		// Slate hunt for something under the scrim.
		return Current != INDEX_NONE;
	}
}
