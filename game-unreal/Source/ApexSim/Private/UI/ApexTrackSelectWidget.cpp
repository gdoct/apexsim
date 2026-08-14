#include "UI/ApexTrackSelectWidget.h"

#include "ApexMenuFlowSubsystem.h"
#include "ApexNetSubsystem.h"
#include "ApexSim.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/EditableTextBox.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/ScrollBox.h"
#include "Components/SizeBox.h"
#include "Components/Spacer.h"
#include "Components/TextBlock.h"
#include "Components/UniformGridPanel.h"
#include "Components/UniformGridSlot.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "TimerManager.h"
#include "UI/ApexButtonWidget.h"
#include "UI/ApexContentCardWidget.h"
#include "UI/ApexRootWidget.h"
#include "UI/ApexUIStyle.h"

namespace
{
	const FName ActionTrackSelectBack(TEXT("__back"));
	const FName ActionUse(TEXT("__use"));
	const FName ActionDemo(TEXT("__demo"));

	/** Filter key for "tracks I have a lap time on". Not a catalog category. */
	const FString DrivenFilter(TEXT("__driven"));

	constexpr float DetailWidth = 470.0f;
	constexpr float CardPreviewHeight = 140.0f;
}

UApexTrackSelectWidget::UApexTrackSelectWidget(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	SetIsFocusable(true);
}

void UApexTrackSelectWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	BuildLayout();
}

void UApexTrackSelectWidget::NativeConstruct()
{
	Super::NativeConstruct();

	if (UApexNetSubsystem* Net = GetNet())
	{
		Net->OnLobbyStateUpdated.AddDynamic(this, &UApexTrackSelectWidget::HandleLobbyStateUpdated);
	}
}

void UApexTrackSelectWidget::NativeDestruct()
{
	if (UApexNetSubsystem* Net = GetNet())
	{
		Net->OnLobbyStateUpdated.RemoveDynamic(this, &UApexTrackSelectWidget::HandleLobbyStateUpdated);
	}
	Super::NativeDestruct();
}

void UApexTrackSelectWidget::OnScreenActivated()
{
	Super::OnScreenActivated();

	RebuildCards();

	if (const UApexMenuFlowSubsystem* Flow = GetFlow())
	{
		if (Flow->HasPendingTrack())
		{
			SelectTrack(Flow->GetPendingTrackId());
		}
	}

	if (SelectedTrackId.IsEmpty() && VisibleCards.Num() > 0)
	{
		SelectTrack(VisibleCards[0]->GetCardId());
	}

	RequestCardFocus();
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

void UApexTrackSelectWidget::BuildLayout()
{
	UVerticalBox* Page = WidgetTree->ConstructWidget<UVerticalBox>();
	ApexUI::AddV(Page, BuildHeader());

	CardGrid = WidgetTree->ConstructWidget<UUniformGridPanel>();
	CardGrid->SetSlotPadding(FMargin(8.0f));

	CardScroll = WidgetTree->ConstructWidget<UScrollBox>();
	CardScroll->AddChild(CardGrid);

	UBorder* GridPanel = ApexUI::MakePanel(
		*WidgetTree,
		CardScroll,
		FMargin(ApexUI::Metrics::PageGutter - 8.0f, 20.0f, 20.0f, 20.0f),
		ApexUI::MakeBrush(FLinearColor::Transparent));

	UHorizontalBox* Columns = WidgetTree->ConstructWidget<UHorizontalBox>();
	ApexUI::AddH(Columns, GridPanel, FMargin(), VAlign_Fill, 1.0f);
	ApexUI::AddH(Columns, ApexUI::MakeDivider(*WidgetTree, true), FMargin(), VAlign_Fill);
	ApexUI::AddH(Columns, ApexUI::MakeSized(*WidgetTree, BuildDetailPanel(), DetailWidth, -1.0f), FMargin(), VAlign_Fill);

	ApexUI::AddV(Page, Columns, FMargin(), HAlign_Fill, 1.0f);

	WidgetTree->RootWidget = ApexUI::MakePanel(
		*WidgetTree,
		Page,
		FMargin(),
		ApexUI::MakeBrush(ApexUI::Palette::Background));
}

UWidget* UApexTrackSelectWidget::BuildHeader()
{
	FApexButtonSpec BackSpec;
	BackSpec.Label = TEXT("Back");
	BackSpec.KeyCap = TEXT("Esc");
	BackSpec.bKeyCapLeading = true;
	BackSpec.Variant = EApexButtonVariant::Bare;
	BackSpec.LabelSize = 15.0f;
	BackSpec.ActionId = ActionTrackSelectBack;

	UApexButtonWidget* BackButton = WidgetTree->ConstructWidget<UApexButtonWidget>();
	BackButton->Setup(BackSpec);
	BackButton->OnActivated.AddDynamic(this, &UApexTrackSelectWidget::HandleButtonActivated);

	UHorizontalBox* Right = WidgetTree->ConstructWidget<UHorizontalBox>();

	CountText = ApexUI::MakeText(*WidgetTree, FString(), ApexUI::Font::Mono(10.0f, 120), ApexUI::Palette::TextMuted);
	ApexUI::AddH(Right, CountText, FMargin(0.0f, 0.0f, 20.0f, 0.0f));

	SearchField = ApexUI::MakeSearchBox(*WidgetTree, TEXT("Search tracks…"));
	SearchField->OnTextChanged.AddDynamic(this, &UApexTrackSelectWidget::HandleSearchChanged);
	ApexUI::AddH(Right, ApexUI::MakeSized(*WidgetTree, SearchField, 300.0f, 34.0f), FMargin(0.0f, 0.0f, 16.0f, 0.0f));

	// Filled by RebuildFilterChips once the catalog is known.
	UHorizontalBox* Chips = WidgetTree->ConstructWidget<UHorizontalBox>();
	FilterChipBox = WidgetTree->ConstructWidget<UVerticalBox>();
	ApexUI::AddV(FilterChipBox, Chips, FMargin(), HAlign_Right);
	ApexUI::AddH(Right, FilterChipBox);

	return ApexUI::MakeScreenHeader(*WidgetTree, BackButton, TEXT("Select track"), Right);
}

UWidget* UApexTrackSelectWidget::BuildDetailPanel()
{
	UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>();

	DetailBox = WidgetTree->ConstructWidget<UVerticalBox>();
	ApexUI::AddV(Column, DetailBox, FMargin(), HAlign_Fill, 1.0f);

	FApexButtonSpec UseSpec;
	UseSpec.Label = TEXT("Use this track");
	UseSpec.KeyCap = TEXT("Enter");
	UseSpec.Variant = EApexButtonVariant::Primary;
	UseSpec.LabelSize = 22.0f;
	UseSpec.Height = 62.0f;
	UseSpec.ActionId = ActionUse;

	UseButton = WidgetTree->ConstructWidget<UApexButtonWidget>();
	UseButton->Setup(UseSpec);
	UseButton->OnActivated.AddDynamic(this, &UApexTrackSelectWidget::HandleButtonActivated);
	ApexUI::AddV(Column, UseButton, FMargin(0.0f, 12.0f, 0.0f, 8.0f));

	// Locked, not missing: the server's DemoLap mode removes human players from
	// the session's participants (game_session.rs:409-425), so whoever asks for
	// a demo lap becomes a spectator, stops receiving telemetry, and is left
	// watching a countdown that never ends. Re-enable when that is fixed.
	FApexButtonSpec DemoSpec;
	DemoSpec.Label = TEXT("Preview demo lap");
	DemoSpec.Badge = TEXT("Locked");
	DemoSpec.Variant = EApexButtonVariant::Locked;
	DemoSpec.bCentreLabel = false;
	DemoSpec.LabelSize = 18.0f;
	DemoSpec.Height = 52.0f;
	DemoSpec.ActionId = ActionDemo;

	DemoButton = WidgetTree->ConstructWidget<UApexButtonWidget>();
	DemoButton->Setup(DemoSpec);
	DemoButton->OnActivated.AddDynamic(this, &UApexTrackSelectWidget::HandleButtonActivated);
	ApexUI::AddV(Column, DemoButton);

	return ApexUI::MakePanel(*WidgetTree, Column, FMargin(26.0f, 20.0f, 26.0f, 26.0f), ApexUI::MakeBrush(FLinearColor::Transparent));
}

// ---------------------------------------------------------------------------
// Data
// ---------------------------------------------------------------------------

void UApexTrackSelectWidget::RebuildCards(bool bForce)
{
	const UApexNetSubsystem* Net = GetNet();
	UApexMenuFlowSubsystem* Flow = GetFlow();
	if (!Net || !CardGrid)
	{
		return;
	}

	const TArray<FApexTrackConfigSummary>& Tracks = Net->GetCachedLobbyState().TrackConfigs;

	TArray<FString> IncomingIds;
	IncomingIds.Reserve(Tracks.Num());
	for (const FApexTrackConfigSummary& Track : Tracks)
	{
		IncomingIds.Add(Track.Id);
	}
	if (!bForce && IncomingIds == BuiltTrackIds)
	{
		return;
	}
	BuiltTrackIds = MoveTemp(IncomingIds);

	TrackCards.Reset();
	CardGrid->ClearChildren();

	for (const FApexTrackConfigSummary& Track : Tracks)
	{
		FApexTrackCatalogRow Row;
		const bool bHasRow = Flow && Flow->GetTrackCatalogRow(Track.Id, Row);

		FApexCardSpec Spec;
		Spec.Id = Track.Id;
		Spec.Title = bHasRow && !Row.DisplayName.IsEmpty() ? Row.DisplayName : Track.Name;
		Spec.PreviewHeight = CardPreviewHeight;
		Spec.PlaceholderCaption = TEXT("No preview");

		TArray<FString> MetaParts;
		if (bHasRow)
		{
			if (!Row.Country.IsEmpty())  { MetaParts.Add(Row.Country); }
			if (Row.LengthM > 0.0f)      { MetaParts.Add(FString::Printf(TEXT("%.2f km"), Row.LengthM / 1000.0f)); }
			if (!Row.Category.IsEmpty()) { MetaParts.Add(Row.Category); }
			Spec.Preview = Row.PreviewImage.LoadSynchronous();
		}
		Spec.Meta = FString::Join(MetaParts, TEXT(" · "));

		float BestSeconds = 0.0f;
		if (Flow && Flow->GetBestLapSeconds(Track.Id, BestSeconds))
		{
			Spec.Footnote = FString::Printf(TEXT("Best %s"), *UApexMenuFlowSubsystem::FormatLapTime(BestSeconds));
			Spec.FootnoteColour = ApexUI::Palette::Accent;
		}
		else
		{
			Spec.Footnote = TEXT("Never driven");
			Spec.FootnoteColour = ApexUI::Palette::TextDisabled;
		}

		UApexContentCardWidget* Card = WidgetTree->ConstructWidget<UApexContentCardWidget>();
		Card->Setup(Spec);
		Card->OnActivated.AddDynamic(this, &UApexTrackSelectWidget::HandleCardActivated);
		TrackCards.Add(Card);
	}

	RebuildFilterChips();
	ApplyFilter();

	// The grid is usually built before the first lobby snapshot lands, so these
	// cards are new objects: the selection made against the old, empty grid has
	// to be applied to them or nothing is outlined.
	if (SelectedTrackId.IsEmpty())
	{
		if (Flow && Flow->HasPendingTrack())
		{
			SelectedTrackId = Flow->GetPendingTrackId();
		}
		else if (VisibleCards.Num() > 0)
		{
			SelectedTrackId = VisibleCards[0]->GetCardId();
		}
	}
	if (!SelectedTrackId.IsEmpty())
	{
		SelectTrack(SelectedTrackId);
		RequestCardFocus();
	}
}

void UApexTrackSelectWidget::RequestCardFocus()
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().SetTimerForNextTick(
			FTimerDelegate::CreateWeakLambda(this, [this]() { FocusCard(FocusedCardIndex); }));
	}
}

void UApexTrackSelectWidget::RebuildFilterChips()
{
	UHorizontalBox* Chips = FilterChipBox && FilterChipBox->GetChildrenCount() > 0
		? Cast<UHorizontalBox>(FilterChipBox->GetChildAt(0))
		: nullptr;
	if (!Chips)
	{
		return;
	}

	Chips->ClearChildren();
	FilterChips.Reset();

	// Categories come from the catalog rather than a hardcoded list, so a new
	// class of track shows up here the moment it has rows.
	TArray<FString> Categories;
	const UApexMenuFlowSubsystem* Flow = GetFlow();
	if (Flow)
	{
		for (const FString& Id : BuiltTrackIds)
		{
			FApexTrackCatalogRow Row;
			if (Flow->GetTrackCatalogRow(Id, Row) && !Row.Category.IsEmpty())
			{
				Categories.AddUnique(Row.Category);
			}
		}
	}
	Categories.Sort();

	auto AddChip = [this, Chips](const FString& Key, const FString& Label)
	{
		FApexButtonSpec Spec;
		Spec.Label = Label;
		Spec.Variant = ActiveFilter == Key ? EApexButtonVariant::Primary : EApexButtonVariant::Ghost;
		Spec.bCentreLabel = true;
		Spec.LabelSize = 14.0f;
		Spec.Height = 34.0f;
		Spec.ActionId = FName(*Key);

		UApexButtonWidget* Chip = WidgetTree->ConstructWidget<UApexButtonWidget>();
		Chip->Setup(Spec);
		Chip->OnActivated.AddDynamic(this, &UApexTrackSelectWidget::HandleButtonActivated);

		ApexUI::AddH(Chips, ApexUI::MakeSized(*WidgetTree, Chip, -1.0f, 34.0f), FMargin(6.0f, 0.0f, 0.0f, 0.0f));
		FilterChips.Add(Chip);
	};

	AddChip(FString(), TEXT("All"));
	for (const FString& Category : Categories)
	{
		AddChip(Category, Category);
	}
	AddChip(DrivenFilter, TEXT("Driven"));
}

void UApexTrackSelectWidget::ApplyFilter()
{
	if (!CardGrid)
	{
		return;
	}

	const FString Search = SearchField ? SearchField->GetText().ToString().TrimStartAndEnd() : FString();
	const UApexMenuFlowSubsystem* Flow = GetFlow();

	VisibleCards.Reset();
	CardGrid->ClearChildren();

	for (UApexContentCardWidget* Card : TrackCards)
	{
		if (!Card)
		{
			continue;
		}

		bool bMatches = Search.IsEmpty() || Card->GetSearchText().Contains(Search, ESearchCase::IgnoreCase);

		if (bMatches && !ActiveFilter.IsEmpty())
		{
			if (ActiveFilter == DrivenFilter)
			{
				float Unused = 0.0f;
				bMatches = Flow && Flow->GetBestLapSeconds(Card->GetCardId(), Unused);
			}
			else
			{
				FApexTrackCatalogRow Row;
				bMatches = Flow
					&& Flow->GetTrackCatalogRow(Card->GetCardId(), Row)
					&& Row.Category.Equals(ActiveFilter, ESearchCase::IgnoreCase);
			}
		}

		if (!bMatches)
		{
			continue;
		}

		// Cards are re-slotted rather than hidden: a collapsed child in a uniform
		// grid still occupies its cell, which would leave holes in the layout.
		const int32 Index = VisibleCards.Num();
		UUniformGridSlot* GridSlot = CardGrid->AddChildToUniformGrid(Card, Index / GridColumns, Index % GridColumns);
		// Without this the cells are uniform but the cards inside them are not:
		// each keeps its own desired width and the grid looks ragged.
		GridSlot->SetHorizontalAlignment(HAlign_Fill);
		GridSlot->SetVerticalAlignment(VAlign_Fill);
		VisibleCards.Add(Card);
	}

	if (CountText)
	{
		CountText->SetText(FText::FromString(VisibleCards.Num() == TrackCards.Num()
			? FString::Printf(TEXT("%d TRACKS"), TrackCards.Num())
			: FString::Printf(TEXT("%d OF %d"), VisibleCards.Num(), TrackCards.Num())));
	}

	FocusedCardIndex = FMath::Clamp(FocusedCardIndex, 0, FMath::Max(0, VisibleCards.Num() - 1));
}

void UApexTrackSelectWidget::SelectTrack(const FString& TrackId)
{
	SelectedTrackId = TrackId;

	for (int32 Index = 0; Index < VisibleCards.Num(); ++Index)
	{
		const bool bIsSelected = VisibleCards[Index]->GetCardId().Equals(TrackId, ESearchCase::IgnoreCase);
		VisibleCards[Index]->SetSelected(bIsSelected);
		if (bIsSelected)
		{
			FocusedCardIndex = Index;
		}
	}

	// Cards filtered out of view still need clearing, or a stale selection
	// outline reappears when the filter is removed.
	for (UApexContentCardWidget* Card : TrackCards)
	{
		if (Card && !VisibleCards.Contains(Card))
		{
			Card->SetSelected(false);
		}
	}

	RefreshDetail();
}

void UApexTrackSelectWidget::RefreshDetail()
{
	if (!DetailBox)
	{
		return;
	}

	DetailBox->ClearChildren();

	const UApexNetSubsystem* Net = GetNet();
	UApexMenuFlowSubsystem* Flow = GetFlow();

	if (SelectedTrackId.IsEmpty())
	{
		ApexUI::AddV(DetailBox, ApexUI::MakeText(
			*WidgetTree,
			TEXT("Pick a track to see its details."),
			ApexUI::Font::Body(14.0f),
			ApexUI::Palette::TextMuted));

		if (UseButton)  { UseButton->SetIsEnabled(false); }
		if (DemoButton) { DemoButton->SetIsEnabled(false); }
		return;
	}

	if (UseButton)  { UseButton->SetIsEnabled(true); }
	if (DemoButton) { DemoButton->SetIsEnabled(true); }

	FApexTrackCatalogRow Row;
	const bool bHasRow = Flow && Flow->GetTrackCatalogRow(SelectedTrackId, Row);

	FString Name = bHasRow ? Row.DisplayName : FString();
	if (Name.IsEmpty() && Net)
	{
		FApexTrackConfigSummary Summary;
		if (Net->FindTrackById(SelectedTrackId, Summary))
		{
			Name = Summary.Name;
		}
	}

	ApexUI::AddV(DetailBox, ApexUI::MakePreview(
		*WidgetTree,
		bHasRow ? Row.PreviewImage.LoadSynchronous() : nullptr,
		TEXT("No preview"),
		-1.0f,
		200.0f));

	UTextBlock* Title = ApexUI::MakeText(*WidgetTree, Name, ApexUI::Font::Display(30.0f), ApexUI::Palette::TextPrimary);
	Title->SetAutoWrapText(true);
	ApexUI::AddV(DetailBox, Title, FMargin(0.0f, 18.0f, 0.0f, 0.0f));

	TArray<FString> SubtitleParts;
	if (bHasRow)
	{
		if (!Row.Country.IsEmpty())  { SubtitleParts.Add(Row.Country.ToUpper()); }
		if (!Row.City.IsEmpty())     { SubtitleParts.Add(Row.City.ToUpper()); }
		if (!Row.Category.IsEmpty()) { SubtitleParts.Add(Row.Category.ToUpper()); }
	}
	ApexUI::AddV(
		DetailBox,
		ApexUI::MakeText(*WidgetTree, FString::Join(SubtitleParts, TEXT(" · ")), ApexUI::Font::Mono(10.0f, 120), ApexUI::Palette::TextMuted),
		FMargin(0.0f, 8.0f, 0.0f, 0.0f));

	// Two rows of two: everything the client actually knows about a circuit.
	UTextBlock* LengthValue = nullptr;
	UTextBlock* BestValue = nullptr;
	UTextBlock* CategoryValue = nullptr;
	UTextBlock* EnvironmentValue = nullptr;

	UHorizontalBox* StatRowOne = WidgetTree->ConstructWidget<UHorizontalBox>();
	ApexUI::AddH(StatRowOne, ApexUI::MakeStat(*WidgetTree, TEXT("Length"), LengthValue), FMargin(), VAlign_Fill, 1.0f);
	ApexUI::AddH(StatRowOne, ApexUI::MakeStat(*WidgetTree, TEXT("Your best"), BestValue), FMargin(10.0f, 0.0f, 0.0f, 0.0f), VAlign_Fill, 1.0f);
	ApexUI::AddV(DetailBox, StatRowOne, FMargin(0.0f, 20.0f, 0.0f, 0.0f));

	UHorizontalBox* StatRowTwo = WidgetTree->ConstructWidget<UHorizontalBox>();
	ApexUI::AddH(StatRowTwo, ApexUI::MakeStat(*WidgetTree, TEXT("Category"), CategoryValue), FMargin(), VAlign_Fill, 1.0f);
	ApexUI::AddH(StatRowTwo, ApexUI::MakeStat(*WidgetTree, TEXT("Environment"), EnvironmentValue), FMargin(10.0f, 0.0f, 0.0f, 0.0f), VAlign_Fill, 1.0f);
	ApexUI::AddV(DetailBox, StatRowTwo, FMargin(0.0f, 10.0f, 0.0f, 0.0f));

	LengthValue->SetText(FText::FromString(
		bHasRow && Row.LengthM > 0.0f ? FString::Printf(TEXT("%.2f km"), Row.LengthM / 1000.0f) : TEXT("—")));
	CategoryValue->SetText(FText::FromString(bHasRow && !Row.Category.IsEmpty() ? Row.Category : TEXT("—")));
	EnvironmentValue->SetText(FText::FromString(bHasRow && !Row.EnvironmentType.IsEmpty() ? Row.EnvironmentType : TEXT("—")));

	float BestSeconds = 0.0f;
	if (Flow && Flow->GetBestLapSeconds(SelectedTrackId, BestSeconds))
	{
		BestValue->SetText(FText::FromString(UApexMenuFlowSubsystem::FormatLapTime(BestSeconds)));
		BestValue->SetColorAndOpacity(FSlateColor(ApexUI::Palette::Accent));
	}
	else
	{
		BestValue->SetText(FText::FromString(TEXT("—")));
		BestValue->SetColorAndOpacity(FSlateColor(ApexUI::Palette::TextDisabled));
	}

	if (!bHasRow)
	{
		// Say why the panel is mostly dashes, rather than looking broken.
		UTextBlock* Note = ApexUI::MakeText(
			*WidgetTree,
			TEXT("This circuit has no entry in the local track catalog, so its details and preview are missing."),
			ApexUI::Font::Body(13.0f),
			ApexUI::Palette::TextMuted);
		Note->SetAutoWrapText(true);
		ApexUI::AddV(DetailBox, Note, FMargin(0.0f, 18.0f, 0.0f, 0.0f));
	}

	ApexUI::AddV(DetailBox, WidgetTree->ConstructWidget<USpacer>(), FMargin(), HAlign_Fill, 1.0f);
}

// ---------------------------------------------------------------------------
// Interaction
// ---------------------------------------------------------------------------

void UApexTrackSelectWidget::HandleLobbyStateUpdated(const FApexLobbyState& LobbyState)
{
	RebuildCards();
}

void UApexTrackSelectWidget::HandleCardActivated(UApexContentCardWidget* Card)
{
	if (Card)
	{
		SelectTrack(Card->GetCardId());
	}
}

void UApexTrackSelectWidget::HandleSearchChanged(const FText& Text)
{
	ApplyFilter();
	// Keep the description in step with what is on screen.
	if (!SelectedTrackId.IsEmpty())
	{
		SelectTrack(SelectedTrackId);
	}
}

void UApexTrackSelectWidget::HandleButtonActivated(UApexButtonWidget* Button)
{
	if (!Button)
	{
		return;
	}

	const FName Id = Button->GetActionId();

	if (Id == ActionTrackSelectBack)
	{
		GoBack();
		return;
	}

	if (Id == ActionUse)
	{
		if (SelectedTrackId.IsEmpty())
		{
			ShowToast(TEXT("Pick a track first"), true);
			return;
		}
		if (UApexMenuFlowSubsystem* Flow = GetFlow())
		{
			Flow->SetPendingTrack(SelectedTrackId);
		}
		if (UApexRootWidget* Root = GetRoot())
		{
			Root->ReplaceScreen(Root->ScreenAfterTrackSelect);
		}
		return;
	}

	if (Id == ActionDemo)
	{
		UApexNetSubsystem* Net = GetNet();
		UApexMenuFlowSubsystem* Flow = GetFlow();
		if (!Net || !Flow || SelectedTrackId.IsEmpty())
		{
			return;
		}
		if (!Net->IsAuthenticated())
		{
			ShowToast(TEXT("Not connected to a server"), true);
			return;
		}

		// A demo lap is an ordinary session the server drives itself, so this is
		// the same create-then-count-in path the main menu's start button uses.
		Flow->SetPendingTrack(SelectedTrackId);
		Flow->bAutoStartOnJoin = true;
		Flow->AutoStartMode = EApexGameMode::DemoLap;

		UE_LOG(LogApexSim, Log, TEXT("Demo lap requested on track '%s'"), *SelectedTrackId);
		Net->CreateSession(SelectedTrackId, Flow->CreateMaxPlayers, 0, Flow->CreateLapLimit, EApexSessionKind::Practice);
		return;
	}

	// Anything else is a filter chip; its action id is the filter key.
	ActiveFilter = Id.IsNone() ? FString() : Id.ToString();
	RebuildFilterChips();
	ApplyFilter();
	if (!SelectedTrackId.IsEmpty())
	{
		SelectTrack(SelectedTrackId);
	}
}

// ---------------------------------------------------------------------------
// Keyboard
// ---------------------------------------------------------------------------

FReply UApexTrackSelectWidget::NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent)
{
	const FKey Key = InKeyEvent.GetKey();

	if (Key == EKeys::Left)  { MoveCardFocus(-1);            return FReply::Handled(); }
	if (Key == EKeys::Right) { MoveCardFocus(1);             return FReply::Handled(); }
	if (Key == EKeys::Up)    { MoveCardFocus(-GridColumns);  return FReply::Handled(); }
	if (Key == EKeys::Down)  { MoveCardFocus(GridColumns);   return FReply::Handled(); }

	if (Key == EKeys::Tab && UseButton)
	{
		UseButton->SetKeyboardFocus();
		return FReply::Handled();
	}

	return Super::NativeOnKeyDown(InGeometry, InKeyEvent);
}

void UApexTrackSelectWidget::MoveCardFocus(int32 Delta)
{
	if (VisibleCards.Num() == 0)
	{
		return;
	}
	FocusCard(FMath::Clamp(FocusedCardIndex + Delta, 0, VisibleCards.Num() - 1));
}

void UApexTrackSelectWidget::FocusCard(int32 Index)
{
	if (!VisibleCards.IsValidIndex(Index))
	{
		return;
	}

	FocusedCardIndex = Index;
	VisibleCards[Index]->SetKeyboardFocus();

	if (CardScroll)
	{
		CardScroll->ScrollWidgetIntoView(VisibleCards[Index], true);
	}

	// Moving the keyboard through the grid re-describes what it lands on; that
	// is the whole point of the detail panel.
	SelectTrack(VisibleCards[Index]->GetCardId());
}
