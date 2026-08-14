#include "UI/ApexPauseMenuWidget.h"

#include "ApexMenuFlowSubsystem.h"
#include "ApexNetSubsystem.h"
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
#include "UI/ApexUIStyle.h"

using namespace ApexUI;

namespace
{
	constexpr float CardWidth = 682.0f;
	constexpr float RowHeight = 82.0f;
	constexpr float PrimaryRowHeight = 92.0f;

	const FName ActionResume   = TEXT("Resume");
	const FName ActionSettings = TEXT("Settings");
	const FName ActionLeave    = TEXT("Leave");
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

	// The two exits are separated from the rest and carry a consequence label.
	// Leaving the session and quitting are never adjacent to Resume.
	AddV(Actions, MakeDivider(*WidgetTree), FMargin(0.0f, 12.0f, 0.0f, 12.0f));
	AddRow(Actions, TEXT("BACK TO MAIN MENU"), TEXT("Session ends"), ActionLeave, false);
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

	UOverlaySlot* CardSlot = Content->AddChildToOverlay(MakeSized(*WidgetTree, CardPanel, CardWidth, -1.0f));
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
	Spec.Height = bPrimary ? PrimaryRowHeight : RowHeight;
	Spec.LabelSize = bPrimary ? 27.0f : 24.0f;
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

	SetVisibility(ESlateVisibility::Visible);
	RefreshStatusStrip();

	// Focus lands on Resume so Enter is always the safe answer.
	if (Rows.Num() > 0 && Rows[0])
	{
		Rows[0]->SetKeyboardFocus();
	}
	else
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

	const int32 LocalIndex = Net->GetLocalCarIndex();
	if (const FApexCarTelemetry* Local = Net->GetLatestTelemetry().Cars.FindByPredicate(
			[LocalIndex](const FApexCarTelemetry& Car) { return Car.CarIndex == LocalIndex; }))
	{
		const int32 LapLimit = Flow ? Flow->CreateLapLimit : 0;
		Parts.Add(LapLimit > 0
			? FString::Printf(TEXT("Lap %d / %d"), FMath::Max(1, Local->CurrentLap), LapLimit)
			: FString::Printf(TEXT("Lap %d"), FMath::Max(1, Local->CurrentLap)));
	}

	FApexSessionSummary Session;
	if (Net->FindSessionById(Net->GetCurrentSessionId(), Session) && !Session.TrackName.IsEmpty())
	{
		Parts.Add(Session.TrackName);
	}

	if (Flow)
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
	const bool bSimRunning = Net->GetSessionState() == EApexSessionState::Racing
		|| Net->GetSessionState() == EApexSessionState::Countdown;
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
	else if (Action == ActionLeave)
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
	const FKey Key = InKeyEvent.GetKey();

	// Escape resumes, matching what the footer promises. It is handled here
	// rather than by the root widget so the menu closes even when focus has
	// moved down onto one of the rows.
	if (Key == EKeys::Escape || Key == EKeys::Gamepad_Special_Right)
	{
		OnAction.Broadcast(EApexPauseAction::Resume);
		return FReply::Handled();
	}

	// Arrow keys walk the rows. Slate's own navigation would work too, but the
	// rows are separated by a divider that it happily focuses past.
	const bool bUp = Key == EKeys::Up || Key == EKeys::Gamepad_DPad_Up;
	const bool bDown = Key == EKeys::Down || Key == EKeys::Gamepad_DPad_Down;
	if ((bUp || bDown) && Rows.Num() > 0)
	{
		int32 Current = Rows.IndexOfByPredicate([](const TObjectPtr<UApexButtonWidget>& Row)
		{
			return Row && Row->HasAnyUserFocus();
		});
		if (Current == INDEX_NONE)
		{
			Current = 0;
		}
		else
		{
			Current = (Current + (bDown ? 1 : Rows.Num() - 1)) % Rows.Num();
		}
		if (Rows[Current])
		{
			Rows[Current]->SetKeyboardFocus();
		}
		return FReply::Handled();
	}

	return Super::NativeOnKeyDown(InGeometry, InKeyEvent);
}
