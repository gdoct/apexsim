#include "UI/ApexPauseMenuWidget.h"

#include "ApexReplayRecorder.h"
#include "ApexMenuFlowSubsystem.h"
#include "ApexNetSubsystem.h"
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
	AddV(Heading, MakeText(*WidgetTree, TEXT("PAUSED"), Font::Display(52.0f, 20), Palette::TextPrimary),
		FMargin(0.0f, 10.0f, 0.0f, 0.0f));
	AddV(Card, MakePanel(*WidgetTree, Heading, FMargin(42.0f, 34.0f), MakeBrush(Palette::Surface)));
	AddV(Card, MakeDivider(*WidgetTree));

	UVerticalBox* Actions = WidgetTree->ConstructWidget<UVerticalBox>();

	// Resume is first and filled: it sits where the cursor is already resting,
	// so a second Escape and a click do the same thing.
	AddRow(Actions, TEXT("Close menu"), FString(), ActionResume, /*bPrimary*/ true, TEXT("ESC"));
	AddRow(Actions, TEXT("SETTINGS"), TEXT("Gameplay · Graphics · Controls"), ActionSettings, false);
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

	AddV(Card, MakePanel(*WidgetTree, Actions, FMargin(22.0f, 22.0f), MakeBrush(Palette::Background)));

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
	SetVisibility(ESlateVisibility::Collapsed);
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
	OnAction.Broadcast(EApexPauseAction::Resume);
	return true;
}

bool UApexPauseMenuWidget::HandleNavigation(EUINavigation Direction, UWidget* Source)
{
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
