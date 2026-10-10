#include "UI/ApexRaceResultsWidget.h"

#include "ApexMenuFlowSubsystem.h"
#include "ApexNetSubsystem.h"
#include "ApexReplayRecorder.h"
#include "ApexSessionRecorder.h"
#include "Blueprint/WidgetTree.h"
#include "Catalog/ApexCatalogRows.h"
#include "Components/Border.h"
#include "Components/HorizontalBox.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/ScrollBox.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Engine/GameInstance.h"
#include "Misc/Paths.h"
#include "UI/ApexButtonWidget.h"
#include "UI/ApexRootWidget.h"
#include "UI/ApexUIStyle.h"

using namespace ApexUI;

namespace
{
	const FName ActionRaceResultsRow   = TEXT("RaceResults.Row");
	const FName ActionRaceResultsAgain = TEXT("RaceResults.Again");
	const FName ActionRaceResultsLobby = TEXT("RaceResults.Lobby");
	const FName ActionRaceResultsSave  = TEXT("RaceResults.Save");
	const FName ActionRaceResultsHide  = TEXT("RaceResults.Hide");
	const FName ActionRaceResultsShow  = TEXT("RaceResults.Show");

	constexpr float ResultsCardWidth = 980.0f;
	/** Past this the table scrolls: the race has to show above and below the card. */
	constexpr float ResultsTableMaxHeight = 470.0f;
	constexpr float ResultsRowHeight = 44.0f;
	constexpr float ResultsRefreshSeconds = 0.5f;

	constexpr float ColPos = 52.0f;
	constexpr float ColLaps = 70.0f;
	constexpr float ColTime = 150.0f;
	constexpr float ColBest = 130.0f;
	constexpr float ColState = 150.0f;

	/** Translucent: the race goes on behind it. */
	const FLinearColor ResultsCardFill = Palette::Background.CopyWithNewOpacity(0.74f);

	/** The shell this card sits in, for its toasts. */
	UApexRootWidget* FindRoot(const UWidget* Widget)
	{
		for (UWidget* Parent = Widget ? Widget->GetParent() : nullptr; Parent; Parent = Parent->GetParent())
		{
			if (UApexRootWidget* Root = Cast<UApexRootWidget>(Parent))
			{
				return Root;
			}
		}
		return nullptr;
	}

	UOverlay* RowWithContent(UWidgetTree& Tree, UApexButtonWidget* Button, UWidget* Content)
	{
		UOverlay* Stack = Tree.ConstructWidget<UOverlay>();
		UOverlaySlot* ButtonSlot = Stack->AddChildToOverlay(Button);
		ButtonSlot->SetHorizontalAlignment(HAlign_Fill);
		ButtonSlot->SetVerticalAlignment(VAlign_Fill);
		Content->SetVisibility(ESlateVisibility::HitTestInvisible);
		UOverlaySlot* ContentSlot = Stack->AddChildToOverlay(Content);
		ContentSlot->SetHorizontalAlignment(HAlign_Fill);
		ContentSlot->SetVerticalAlignment(VAlign_Center);
		ContentSlot->SetPadding(FMargin(14.0f, 0.0f));
		return Stack;
	}

	UApexButtonWidget* MakeButton(UWidgetTree& Tree, const FString& Label, FName Action, EApexButtonVariant Variant,
		const FString& KeyCap = FString())
	{
		FApexButtonSpec Spec;
		Spec.Label = Label;
		Spec.KeyCap = KeyCap;
		Spec.ActionId = Action;
		Spec.Variant = Variant;
		Spec.bCentreLabel = true;
		Spec.LabelSize = 17.0f;
		Spec.Height = 52.0f;
		UApexButtonWidget* Button = Tree.ConstructWidget<UApexButtonWidget>();
		Button->Setup(Spec);
		return Button;
	}
}

// ---------------------------------------------------------------------------
// Rows
// ---------------------------------------------------------------------------

FString ApexRaceResults::FormatRaceTime(float Seconds)
{
	const int32 Millis = FMath::Max(0, FMath::RoundToInt(Seconds * 1000.0f));
	const int32 Hours = Millis / 3600000;
	const int32 Minutes = Millis / 60000 % 60;
	const int32 Secs = Millis / 1000 % 60;
	return Hours > 0
		? FString::Printf(TEXT("%d:%02d:%02d.%03d"), Hours, Minutes, Secs, Millis % 1000)
		: FString::Printf(TEXT("%d:%02d.%03d"), Minutes, Secs, Millis % 1000);
}

TArray<ApexRaceResults::FRow> ApexRaceResults::BuildRows(TConstArrayView<FApexCarResult> Results, const FString& LocalPlayerId, bool bLive)
{
	// Finishers by the server's classification, then the rest as the
	// recorder has them (the running order).
	TArray<const FApexCarResult*> Order;
	for (const FApexCarResult& Result : Results)
	{
		Order.Add(&Result);
	}
	Order.StableSort([](const FApexCarResult& A, const FApexCarResult& B)
	{
		const bool bA = A.FinishPosition > 0;
		const bool bB = B.FinishPosition > 0;
		if (bA != bB)
		{
			return bA;
		}
		return bA && A.FinishPosition < B.FinishPosition;
	});

	auto RaceTime = [](const FApexCarResult& Result)
	{
		float Total = 0.0f;
		for (float Lap : Result.LapTimes)
		{
			Total += Lap;
		}
		return Total;
	};
	const FApexCarResult* Winner = Order.Num() > 0 && Order[0]->FinishPosition > 0 ? Order[0] : nullptr;

	TArray<FRow> Rows;
	for (int32 Index = 0; Index < Order.Num(); ++Index)
	{
		const FApexCarResult& Result = *Order[Index];
		FRow& Row = Rows.AddDefaulted_GetRef();
		Row.CarIndex = Result.CarIndex;
		Row.Position = Index + 1;
		Row.Name = Result.DriverName;
		Row.bAi = Result.bIsAi;
		Row.bYou = !Result.bIsAi && !LocalPlayerId.IsEmpty() && Result.PlayerId.Equals(LocalPlayerId, ESearchCase::IgnoreCase);
		Row.bFinished = Result.FinishPosition > 0;
		Row.Laps = Result.LapsCompleted();
		Row.BestLapSeconds = Result.BestLapSeconds;
		if (!Row.bFinished)
		{
			Row.Time = bLive ? TEXT("ON TRACK") : TEXT("DNF");
		}
		else if (&Result == Winner)
		{
			Row.Time = FormatRaceTime(RaceTime(Result));
		}
		else if (Winner && Row.Laps < Winner->LapsCompleted())
		{
			const int32 Down = Winner->LapsCompleted() - Row.Laps;
			Row.Time = FString::Printf(TEXT("+%d LAP%s"), Down, Down == 1 ? TEXT("") : TEXT("S"));
		}
		else
		{
			Row.Time = FString::Printf(TEXT("+%.3f"), FMath::Max(0.0f, RaceTime(Result) - (Winner ? RaceTime(*Winner) : 0.0f)));
		}
		// The ones worth a camera: still racing. Our own row puts the panorama back.
		Row.bWatchable = Row.bYou || (bLive && !Row.bFinished);
	}
	return Rows;
}

// ---------------------------------------------------------------------------
// Widget
// ---------------------------------------------------------------------------

UApexRaceResultsWidget::UApexRaceResultsWidget(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	SetVisibility(ESlateVisibility::Collapsed);
	SetIsFocusable(true);
}

void UApexRaceResultsWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();

	UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>();

	// --- Heading --------------------------------------------------------------
	UVerticalBox* Heading = WidgetTree->ConstructWidget<UVerticalBox>();
	StatusText = MakeText(*WidgetTree, FString(), Font::Mono(10.0f, 160), Palette::Live);
	AddV(Heading, StatusText);
	TitleText = MakeText(*WidgetTree, FString(), Font::Display(40.0f, 10), Palette::TextPrimary);
	AddV(Heading, TitleText, FMargin(0.0f, 6.0f, 0.0f, 0.0f));
	FormatText = MakeText(*WidgetTree, FString(), Font::Mono(10.0f, 100), Palette::TextSecondary);
	AddV(Heading, FormatText, FMargin(0.0f, 4.0f, 0.0f, 0.0f));
	AddV(Column, MakePanel(*WidgetTree, Heading, FMargin(28.0f, 22.0f, 28.0f, 16.0f), MakeBrush(FLinearColor::Transparent)));
	AddV(Column, MakeDivider(*WidgetTree));

	// --- Table ----------------------------------------------------------------
	UHorizontalBox* Captions = WidgetTree->ConstructWidget<UHorizontalBox>();
	AddH(Captions, MakeSized(*WidgetTree, MakeLabel(*WidgetTree, TEXT("Pos")), ColPos, -1.0f));
	AddH(Captions, MakeLabel(*WidgetTree, TEXT("Driver")), FMargin(), VAlign_Center, 1.0f);
	AddH(Captions, MakeSized(*WidgetTree, MakeLabel(*WidgetTree, TEXT("Laps")), ColLaps, -1.0f));
	AddH(Captions, MakeSized(*WidgetTree, MakeLabel(*WidgetTree, TEXT("Time")), ColTime, -1.0f));
	AddH(Captions, MakeSized(*WidgetTree, MakeLabel(*WidgetTree, TEXT("Best lap")), ColBest, -1.0f));
	AddH(Captions, MakeSized(*WidgetTree, MakeLabel(*WidgetTree, FString()), ColState, -1.0f));
	AddV(Column, MakePanel(*WidgetTree, Captions, FMargin(42.0f, 12.0f, 42.0f, 6.0f), MakeBrush(FLinearColor::Transparent)));

	RowBox = WidgetTree->ConstructWidget<UVerticalBox>();
	UScrollBox* Scroll = WidgetTree->ConstructWidget<UScrollBox>();
	Scroll->AddChild(RowBox);
	USizeBox* TableSize = WidgetTree->ConstructWidget<USizeBox>();
	TableSize->SetMaxDesiredHeight(ResultsTableMaxHeight);
	TableSize->AddChild(Scroll);
	AddV(Column, MakePanel(*WidgetTree, TableSize, FMargin(28.0f, 0.0f), MakeBrush(FLinearColor::Transparent)));

	// --- Foot -----------------------------------------------------------------
	AddV(Column, MakeDivider(*WidgetTree), FMargin(0.0f, 12.0f, 0.0f, 0.0f));
	UVerticalBox* Foot = WidgetTree->ConstructWidget<UVerticalBox>();
	HintText = MakeText(*WidgetTree, FString(), Font::Mono(10.0f, 60), Palette::TextMuted);
	AddV(Foot, HintText, FMargin(0.0f, 0.0f, 0.0f, 12.0f));

	UHorizontalBox* Buttons = WidgetTree->ConstructWidget<UHorizontalBox>();
	DriveAgainButton = MakeButton(*WidgetTree, TEXT("Drive again"), ActionRaceResultsAgain, EApexButtonVariant::Primary);
	LobbyButton = MakeButton(*WidgetTree, TEXT("Back to lobby"), ActionRaceResultsLobby, EApexButtonVariant::Ghost);
	SaveReplayButton = MakeButton(*WidgetTree, TEXT("Save replay"), ActionRaceResultsSave, EApexButtonVariant::Ghost);
	HideButton = MakeButton(*WidgetTree, TEXT("Hide"), ActionRaceResultsHide, EApexButtonVariant::Ghost, TEXT("H"));
	int32 ButtonCount = 0;
	for (UApexButtonWidget* Button : { DriveAgainButton.Get(), LobbyButton.Get(), SaveReplayButton.Get(), HideButton.Get() })
	{
		Button->OnActivated.AddDynamic(this, &UApexRaceResultsWidget::HandleButtonActivated);
		AddH(Buttons, Button, FMargin(ButtonCount++ == 0 ? 0.0f : 8.0f, 0.0f, 0.0f, 0.0f), VAlign_Fill, Button == DriveAgainButton ? 1.4f : 1.0f);
	}
	AddV(Foot, Buttons);
	AddV(Column, MakePanel(*WidgetTree, Foot, FMargin(28.0f, 14.0f, 28.0f, 22.0f), MakeBrush(FLinearColor::Transparent)));

	Card = MakeSized(*WidgetTree, MakePanel(*WidgetTree, Column, FMargin(), MakeBrush(ResultsCardFill, Palette::Border, 1.0f)),
		ResultsCardWidth, -1.0f);

	// --- The folded tab -------------------------------------------------------
	FApexButtonSpec TabSpec;
	TabSpec.Label = TEXT("RESULTS");
	TabSpec.KeyCap = TEXT("H");
	TabSpec.ActionId = ActionRaceResultsShow;
	TabSpec.Variant = EApexButtonVariant::Panel;
	TabSpec.LabelSize = 17.0f;
	TabSpec.Height = 52.0f;
	TabButton = WidgetTree->ConstructWidget<UApexButtonWidget>();
	TabButton->Setup(TabSpec);
	TabButton->OnActivated.AddDynamic(this, &UApexRaceResultsWidget::HandleButtonActivated);
	Tab = MakeSized(*WidgetTree, TabButton, 220.0f, -1.0f);
	Tab->SetVisibility(ESlateVisibility::Collapsed);

	UOverlay* Root = WidgetTree->ConstructWidget<UOverlay>();
	UOverlaySlot* CardSlot = Root->AddChildToOverlay(Card);
	CardSlot->SetHorizontalAlignment(HAlign_Center);
	CardSlot->SetVerticalAlignment(VAlign_Center);
	UOverlaySlot* TabSlot = Root->AddChildToOverlay(Tab);
	TabSlot->SetHorizontalAlignment(HAlign_Right);
	TabSlot->SetVerticalAlignment(VAlign_Bottom);
	TabSlot->SetPadding(FMargin(0.0f, 0.0f, Metrics::PageGutter, 40.0f));
	WidgetTree->RootWidget = Root;
}

void UApexRaceResultsWidget::AddRow()
{
	FApexButtonSpec Spec;
	Spec.ActionId = ActionRaceResultsRow;
	Spec.Variant = EApexButtonVariant::Panel;
	Spec.Height = ResultsRowHeight;
	UApexButtonWidget* Button = WidgetTree->ConstructWidget<UApexButtonWidget>();
	Button->Setup(Spec);
	Button->OnActivated.AddDynamic(this, &UApexRaceResultsWidget::HandleButtonActivated);

	UHorizontalBox* Cells = WidgetTree->ConstructWidget<UHorizontalBox>();
	UTextBlock* Pos = MakeText(*WidgetTree, FString(), Font::Display(20.0f), Palette::TextSecondary);
	UTextBlock* Name = MakeText(*WidgetTree, FString(), Font::Display(18.0f), Palette::TextPrimary);
	UTextBlock* Laps = MakeText(*WidgetTree, FString(), Font::Mono(12.0f), Palette::TextSecondary);
	UTextBlock* Time = MakeText(*WidgetTree, FString(), Font::Mono(13.0f), Palette::TextPrimary);
	UTextBlock* Best = MakeText(*WidgetTree, FString(), Font::Mono(12.0f), Palette::TextSecondary);
	UTextBlock* State = MakeText(*WidgetTree, FString(), Font::Mono(9.0f, 100), Palette::TextMuted);
	AddH(Cells, MakeSized(*WidgetTree, Pos, ColPos, -1.0f), FMargin(), VAlign_Center);
	AddH(Cells, Name, FMargin(), VAlign_Center, 1.0f);
	AddH(Cells, MakeSized(*WidgetTree, Laps, ColLaps, -1.0f), FMargin(), VAlign_Center);
	AddH(Cells, MakeSized(*WidgetTree, Time, ColTime, -1.0f), FMargin(), VAlign_Center);
	AddH(Cells, MakeSized(*WidgetTree, Best, ColBest, -1.0f), FMargin(), VAlign_Center);
	AddH(Cells, MakeSized(*WidgetTree, State, ColState, -1.0f), FMargin(), VAlign_Center);

	UOverlay* Row = RowWithContent(*WidgetTree, Button, Cells);
	AddV(RowBox, MakeSized(*WidgetTree, Row, -1.0f, ResultsRowHeight), FMargin(0.0f, 0.0f, 0.0f, 3.0f));

	RowButtons.Add(Button);
	RowRoots.Add(Row);
	RowPos.Add(Pos);
	RowName.Add(Name);
	RowLaps.Add(Laps);
	RowTime.Add(Time);
	RowBest.Add(Best);
	RowState.Add(State);
	RowCars.Add(INDEX_NONE);
	RowWatchable.Add(false);
}

void UApexRaceResultsWidget::Open()
{
	bOpen = true;
	SetVisibility(ESlateVisibility::SelfHitTestInvisible);
	bShowedLive = IsLive();
	SetMinimised(false);
	RefreshCountdown = ResultsRefreshSeconds;
	Refresh();
	FocusDefault();
}

void UApexRaceResultsWidget::Close()
{
	if (!bOpen)
	{
		return;
	}
	bOpen = false;
	WatchedCar = INDEX_NONE;
	SetVisibility(ESlateVisibility::Collapsed);
}

void UApexRaceResultsWidget::SetMinimised(bool bInMinimised)
{
	bMinimised = bInMinimised;
	if (Card)
	{
		Card->SetVisibility(bMinimised ? ESlateVisibility::Collapsed : ESlateVisibility::Visible);
	}
	if (Tab)
	{
		Tab->SetVisibility(bMinimised ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
	}
	if (bOpen)
	{
		FocusDefault();
	}
}

void UApexRaceResultsWidget::SetWatchedCar(int32 CarIndex)
{
	WatchedCar = CarIndex;
	Refresh();
}

bool UApexRaceResultsWidget::IsLive() const
{
	const UApexSessionRecorder* Recorder = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexSessionRecorder>() : nullptr;
	return Recorder && Recorder->IsRecording();
}

void UApexRaceResultsWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);
	if (!bOpen)
	{
		return;
	}
	RefreshCountdown -= InDeltaTime;
	if (RefreshCountdown > 0.0f)
	{
		return;
	}
	RefreshCountdown = ResultsRefreshSeconds;
	const bool bLive = IsLive();
	Refresh();
	if (bShowedLive && !bLive)
	{
		// The last car is in: the next race is the thing to offer.
		bShowedLive = false;
		if (!bMinimised)
		{
			FocusDefault();
		}
	}
}

void UApexRaceResultsWidget::Refresh()
{
	const UGameInstance* GameInstance = GetGameInstance();
	const UApexSessionRecorder* Recorder = GameInstance ? GameInstance->GetSubsystem<UApexSessionRecorder>() : nullptr;
	const UApexNetSubsystem* Net = GameInstance ? GameInstance->GetSubsystem<UApexNetSubsystem>() : nullptr;
	UApexMenuFlowSubsystem* Flow = GameInstance ? GameInstance->GetSubsystem<UApexMenuFlowSubsystem>() : nullptr;
	if (!Recorder || !Net || !RowBox)
	{
		return;
	}
	const bool bLive = Recorder->IsRecording();
	const TArray<ApexRaceResults::FRow> Rows = ApexRaceResults::BuildRows(Recorder->GetResults(), Net->GetPlayerId(), bLive);

	int32 Running = 0;
	const ApexRaceResults::FRow* You = nullptr;
	for (const ApexRaceResults::FRow& Row : Rows)
	{
		Running += Row.bFinished ? 0 : 1;
		You = Row.bYou ? &Row : You;
	}

	if (StatusText)
	{
		StatusText->SetText(FText::FromString(bLive
			? FString::Printf(TEXT("PROVISIONAL · %d STILL RACING"), Running)
			: FString(TEXT("FINAL RESULT"))));
		StatusText->SetColorAndOpacity(FSlateColor(bLive ? Palette::Live : Palette::Accent));
	}
	if (TitleText)
	{
		TitleText->SetText(FText::FromString(!You ? FString(TEXT("CHEQUERED FLAG"))
			: !You->bFinished ? FString(TEXT("YOUR RACE IS OVER"))
			: You->Position == 1 ? FString(TEXT("YOU WIN"))
			: FString::Printf(TEXT("YOU FINISHED P%d"), You->Position)));
		TitleText->SetColorAndOpacity(FSlateColor(You && You->Position == 1 && You->bFinished ? Palette::Accent : Palette::TextPrimary));
	}
	if (FormatText && Flow)
	{
		FString TrackName;
		FApexTrackCatalogRow TrackRow;
		if (Flow->GetTrackCatalogRow(Recorder->GetTrackId(), TrackRow))
		{
			TrackName = TrackRow.DisplayName.ToUpper();
		}
		const FString Length = Recorder->GetRaceSeconds() > 0
			? ApexRaceLength::Describe(Recorder->GetRaceSeconds()).ToUpper()
			: FString::Printf(TEXT("%d LAPS"), Recorder->GetLapLimit());
		FormatText->SetText(FText::FromString(TrackName.IsEmpty()
			? FString::Printf(TEXT("%s · %d DRIVERS"), *Length, Rows.Num())
			: FString::Printf(TEXT("%s · %s · %d DRIVERS"), *TrackName, *Length, Rows.Num())));
	}
	if (HintText)
	{
		FString Watched;
		for (const ApexRaceResults::FRow& Row : Rows)
		{
			if (Row.CarIndex == WatchedCar && !Row.bYou)
			{
				Watched = Row.Name;
			}
		}
		HintText->SetText(FText::FromString(!Watched.IsEmpty()
			? FString::Printf(TEXT("WATCHING %s · CLICK YOUR NAME FOR THE PANORAMA"), *Watched.ToUpper())
			: bLive && Running > 0 ? FString(TEXT("CLICK A DRIVER STILL RACING TO WATCH THEM · H HIDES THE RESULTS"))
			: FString(TEXT("H HIDES THE RESULTS"))));
	}

	while (RowButtons.Num() < Rows.Num())
	{
		AddRow();
	}
	for (int32 Index = 0; Index < RowButtons.Num(); ++Index)
	{
		const bool bUsed = Rows.IsValidIndex(Index);
		RowRoots[Index]->SetVisibility(bUsed ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
		RowCars[Index] = bUsed ? Rows[Index].CarIndex : INDEX_NONE;
		RowWatchable[Index] = bUsed && Rows[Index].bWatchable;
		if (!bUsed)
		{
			continue;
		}
		const ApexRaceResults::FRow& Row = Rows[Index];
		const FLinearColor RowInk = Row.bFinished || bLive ? Palette::TextPrimary : Palette::TextDisabled;
		RowPos[Index]->SetText(FText::AsNumber(Row.Position));
		RowPos[Index]->SetColorAndOpacity(FSlateColor(Row.bFinished && Row.Position == 1 ? Palette::Accent : Palette::TextSecondary));
		RowName[Index]->SetText(FText::FromString(Row.bYou ? Row.Name + TEXT("  (YOU)") : Row.bAi ? TEXT("AI · ") + Row.Name : Row.Name));
		RowName[Index]->SetColorAndOpacity(FSlateColor(Row.bYou ? Palette::Accent : RowInk));
		RowLaps[Index]->SetText(FText::AsNumber(Row.Laps));
		RowTime[Index]->SetText(FText::FromString(Row.Time));
		RowTime[Index]->SetColorAndOpacity(FSlateColor(Row.bFinished ? Palette::TextPrimary : bLive ? Palette::Live : Palette::TextDisabled));
		RowBest[Index]->SetText(FText::FromString(Row.BestLapSeconds > 0.0f
			? UApexMenuFlowSubsystem::FormatLapTime(Row.BestLapSeconds) : FString(TEXT("—"))));
		const bool bWatched = Row.CarIndex == WatchedCar && !Row.bYou;
		RowState[Index]->SetText(FText::FromString(bWatched ? TEXT("WATCHING")
			: Row.bWatchable && !Row.bYou ? TEXT("WATCH ›")
			: Row.bFinished ? TEXT("FLAG") : FString()));
		RowState[Index]->SetColorAndOpacity(FSlateColor(bWatched ? Palette::Accent : Row.bWatchable ? Palette::TextSecondary : Palette::TextMuted));
		RowButtons[Index]->SetSelected(Row.CarIndex == WatchedCar || (Row.bYou && WatchedCar == INDEX_NONE));
		// A finished car or a race that is over has nothing to film: not a control.
		RowButtons[Index]->SetIsEnabled(Row.bWatchable);
	}

	if (DriveAgainButton)
	{
		DriveAgainButton->SetIsEnabled(!bLive);
		DriveAgainButton->SetBadge(bLive ? TEXT("After the field is in") : FString(),
			bLive ? Palette::OnAccent : Palette::TextMuted);
	}
	RefreshSaveReplay();
}

void UApexRaceResultsWidget::RefreshSaveReplay()
{
	if (!SaveReplayButton)
	{
		return;
	}
	const UApexReplayRecorder* Replays = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexReplayRecorder>() : nullptr;
	const bool bKept = Replays && Replays->IsThisSessionKept();
	const bool bCan = Replays && Replays->CanKeepThisSession();
	SaveReplayButton->SetIsEnabled(bCan && !bKept);
	SaveReplayButton->SetLabel(bKept ? (Replays->IsRecording() ? TEXT("Saved at the end") : TEXT("Replay saved")) : TEXT("Save replay"));
}

void UApexRaceResultsWidget::FocusDefault()
{
	if (bMinimised)
	{
		if (!ApexNav::Focus(TabButton))
		{
			SetKeyboardFocus();
		}
		return;
	}
	if (!IsLive() && ApexNav::Focus(DriveAgainButton))
	{
		return;
	}
	for (int32 Index = 0; Index < RowButtons.Num(); ++Index)
	{
		if (RowWatchable[Index] && RowCars[Index] != INDEX_NONE && ApexNav::Focus(RowButtons[Index]))
		{
			return;
		}
	}
	if (!ApexNav::Focus(LobbyButton))
	{
		SetKeyboardFocus();
	}
}

bool UApexRaceResultsWidget::HandleNavigation(EUINavigation Direction, UWidget* Source)
{
	// Rows of a scroll box and the buttons under them: the nearest that way.
	ApexNav::MoveToward(this, Direction, Source);
	return true;
}

bool UApexRaceResultsWidget::HandleBack()
{
	SetMinimised(!bMinimised);
	return true;
}

FReply UApexRaceResultsWidget::NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent)
{
	const FKey Key = InKeyEvent.GetKey();
	if (bOpen && !InKeyEvent.IsRepeat() && (Key == EKeys::H || Key == EKeys::Gamepad_FaceButton_Top))
	{
		SetMinimised(!bMinimised);
		return FReply::Handled();
	}
	return Super::NativeOnKeyDown(InGeometry, InKeyEvent);
}

void UApexRaceResultsWidget::HandleButtonActivated(UApexButtonWidget* Button)
{
	if (!Button)
	{
		return;
	}
	const FName Id = Button->GetActionId();
	if (Id == ActionRaceResultsRow)
	{
		const int32 Row = ApexNav::IndexOf(RowButtons, Button);
		if (RowCars.IsValidIndex(Row) && RowCars[Row] != INDEX_NONE && RowWatchable[Row])
		{
			OnWatchCar.Broadcast(RowCars[Row]);
		}
		return;
	}
	if (Id == ActionRaceResultsAgain)
	{
		OnAction.Broadcast(EApexRaceResultsAction::DriveAgain);
		return;
	}
	if (Id == ActionRaceResultsLobby)
	{
		OnAction.Broadcast(EApexRaceResultsAction::BackToLobby);
		return;
	}
	if (Id == ActionRaceResultsHide || Id == ActionRaceResultsShow)
	{
		SetMinimised(Id == ActionRaceResultsHide);
		return;
	}
	if (Id == ActionRaceResultsSave)
	{
		UApexReplayRecorder* Replays = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexReplayRecorder>() : nullptr;
		UApexRootWidget* Root = FindRoot(this);
		FString Path;
		FString Error;
		bool bWhenFinished = false;
		if (Replays && Replays->KeepThisSession(Path, bWhenFinished, Error))
		{
			if (Root)
			{
				Root->ShowToast(bWhenFinished ? FString(TEXT("The replay will be saved when the session ends"))
					: FString::Printf(TEXT("Replay saved: %s"), *FPaths::GetBaseFilename(Path)));
			}
		}
		else if (Root)
		{
			Root->ShowToast(FString::Printf(TEXT("No replay saved: %s"), Replays ? *Error : TEXT("no recorder")), true);
		}
		RefreshSaveReplay();
	}
}
