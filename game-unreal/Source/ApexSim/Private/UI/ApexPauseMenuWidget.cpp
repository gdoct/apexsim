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
#include "Race/ApexRaceCoordinate.h"
#include "UI/ApexButtonWidget.h"
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
	const FName ActionQuit     = TEXT("Quit");
	const FName ActionPitPlan  = TEXT("PitPlan");
	const FName ActionPitPlanDone = TEXT("PitPlanDone");

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

	UOverlaySlot* CardSlot = Content->AddChildToOverlay(MakeSized(*WidgetTree, CardPanel, PauseCardWidth, -1.0f));
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
	}
	if (RecoverTrackRow)
	{
		RecoverTrackRow->SetVisibility(bCanRecover ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
	}
	if (RecoverPitsRow)
	{
		RecoverPitsRow->SetVisibility(bCanRecover && !bOnTrackInHotlap ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
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
