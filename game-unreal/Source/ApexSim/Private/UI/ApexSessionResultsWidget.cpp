#include "UI/ApexSessionResultsWidget.h"

#include "ApexMenuFlowSubsystem.h"
#include "ApexNetSubsystem.h"
#include "ApexSessionRecorder.h"
#include "ApexSim.h"
#include "Blueprint/WidgetTree.h"
#include "Catalog/ApexCatalogRows.h"
#include "Components/Border.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/ScrollBox.h"
#include "Components/SizeBox.h"
#include "Components/Spacer.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Engine/GameInstance.h"
#include "UI/ApexButtonWidget.h"
#include "UI/ApexRootWidget.h"
#include "UI/ApexUIStyle.h"

namespace
{
	const FName ActionDriveAgain(TEXT("__again"));
	const FName ActionLobby(TEXT("__lobby"));
	const FName ActionMenu(TEXT("__menu"));

	constexpr float ResultsSidePanelWidth = 470.0f;

	/** Column widths for the classification table, in slate units. */
	constexpr float ColPos = 60.0f;
	constexpr float ColDriver = 260.0f;
	constexpr float ColCar = 280.0f;
	constexpr float ColBest = 190.0f;
	constexpr float ColGap = 140.0f;

	UWidget* MakeCell(UWidgetTree& Tree, const FString& Text, const FSlateFontInfo& Font, const FLinearColor& Colour, float Width)
	{
		return ApexUI::MakeSized(Tree, ApexUI::MakeText(Tree, Text, Font, Colour), Width, -1.0f);
	}
}

UApexSessionResultsWidget::UApexSessionResultsWidget(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	SetIsFocusable(true);
}

void UApexSessionResultsWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	BuildLayout();
}

void UApexSessionResultsWidget::OnScreenActivated()
{
	Super::OnScreenActivated();

	RefreshTable();
	RefreshSidePanel();
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

void UApexSessionResultsWidget::BuildLayout()
{
	UVerticalBox* Page = WidgetTree->ConstructWidget<UVerticalBox>();
	ApexUI::AddV(Page, BuildHeader());

	// --- Classification -------------------------------------------------------
	UVerticalBox* TableColumn = WidgetTree->ConstructWidget<UVerticalBox>();

	UHorizontalBox* HeaderRow = WidgetTree->ConstructWidget<UHorizontalBox>();
	ApexUI::AddH(HeaderRow, MakeCell(*WidgetTree, TEXT("POS"), ApexUI::Font::Mono(10.0f, 120), ApexUI::Palette::TextMuted, ColPos));
	ApexUI::AddH(HeaderRow, MakeCell(*WidgetTree, TEXT("DRIVER"), ApexUI::Font::Mono(10.0f, 120), ApexUI::Palette::TextMuted, ColDriver));
	ApexUI::AddH(HeaderRow, MakeCell(*WidgetTree, TEXT("CAR"), ApexUI::Font::Mono(10.0f, 120), ApexUI::Palette::TextMuted, ColCar));
	ApexUI::AddH(HeaderRow, MakeCell(*WidgetTree, TEXT("BEST LAP"), ApexUI::Font::Mono(10.0f, 120), ApexUI::Palette::TextMuted, ColBest));
	ApexUI::AddH(HeaderRow, MakeCell(*WidgetTree, TEXT("GAP"), ApexUI::Font::Mono(10.0f, 120), ApexUI::Palette::TextMuted, ColGap));
	ApexUI::AddH(HeaderRow, ApexUI::MakeText(*WidgetTree, TEXT("LAPS"), ApexUI::Font::Mono(10.0f, 120), ApexUI::Palette::TextMuted));
	ApexUI::AddV(TableColumn, ApexUI::MakePanel(*WidgetTree, HeaderRow, FMargin(18.0f, 0.0f, 18.0f, 14.0f), ApexUI::MakeBrush(FLinearColor::Transparent)));

	TableBox = WidgetTree->ConstructWidget<UVerticalBox>();
	UScrollBox* TableScroll = WidgetTree->ConstructWidget<UScrollBox>();
	TableScroll->AddChild(TableBox);
	ApexUI::AddV(TableColumn, TableScroll, FMargin(), HAlign_Fill, 1.0f);

	UBorder* TablePanel = ApexUI::MakePanel(
		*WidgetTree,
		TableColumn,
		FMargin(ApexUI::Metrics::PageGutter, 24.0f, 30.0f, 24.0f),
		ApexUI::MakeBrush(FLinearColor::Transparent));

	UHorizontalBox* Columns = WidgetTree->ConstructWidget<UHorizontalBox>();
	ApexUI::AddH(Columns, TablePanel, FMargin(), VAlign_Fill, 1.0f);
	ApexUI::AddH(Columns, ApexUI::MakeDivider(*WidgetTree, true), FMargin(), VAlign_Fill);
	ApexUI::AddH(Columns, ApexUI::MakeSized(*WidgetTree, BuildSidePanel(), ResultsSidePanelWidth, -1.0f), FMargin(), VAlign_Fill);

	ApexUI::AddV(Page, Columns, FMargin(), HAlign_Fill, 1.0f);

	WidgetTree->RootWidget = ApexUI::MakePanel(
		*WidgetTree,
		Page,
		FMargin(),
		ApexUI::MakeBrush(ApexUI::Palette::Background));
}

UWidget* UApexSessionResultsWidget::BuildHeader()
{
	UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();

	ApexUI::AddH(Row, ApexUI::MakeText(*WidgetTree, TEXT("SESSION COMPLETE"), ApexUI::Font::Mono(10.0f, 160), ApexUI::Palette::Accent));
	ApexUI::AddH(Row, ApexUI::MakeSized(*WidgetTree, ApexUI::MakeDivider(*WidgetTree, true), 1.0f, 22.0f), FMargin(18.0f, 0.0f));

	HeaderTrackText = ApexUI::MakeText(*WidgetTree, FString(), ApexUI::Font::Display(22.0f), ApexUI::Palette::TextPrimary);
	ApexUI::AddH(Row, HeaderTrackText);

	HeaderFormatText = ApexUI::MakeText(*WidgetTree, FString(), ApexUI::Font::Mono(10.0f, 100), ApexUI::Palette::TextMuted);
	ApexUI::AddH(Row, HeaderFormatText, FMargin(16.0f, 0.0f, 0.0f, 0.0f));

	UVerticalBox* Stack = WidgetTree->ConstructWidget<UVerticalBox>();
	UBorder* Bar = ApexUI::MakePanel(*WidgetTree, Row, FMargin(ApexUI::Metrics::PageGutter, 0.0f), ApexUI::MakeBrush(ApexUI::Palette::Background));
	Bar->SetVerticalAlignment(VAlign_Center);
	ApexUI::AddV(Stack, ApexUI::MakeSized(*WidgetTree, Bar, -1.0f, ApexUI::Metrics::TopBarHeight));
	ApexUI::AddV(Stack, ApexUI::MakeDivider(*WidgetTree));
	return Stack;
}

UWidget* UApexSessionResultsWidget::BuildSidePanel()
{
	UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>();

	ApexUI::AddV(Column, ApexUI::MakeLabel(*WidgetTree, TEXT("Your session")), FMargin(0.0f, 0.0f, 0.0f, 14.0f));

	SummaryBox = WidgetTree->ConstructWidget<UVerticalBox>();
	ApexUI::AddV(Column, SummaryBox);

	ApexUI::AddV(Column, ApexUI::MakeLabel(*WidgetTree, TEXT("Lap by lap")), FMargin(0.0f, 26.0f, 0.0f, 12.0f));
	LapChartBox = WidgetTree->ConstructWidget<UVerticalBox>();
	ApexUI::AddV(Column, LapChartBox);

	ApexUI::AddV(Column, WidgetTree->ConstructWidget<USpacer>(), FMargin(), HAlign_Fill, 1.0f);

	FApexButtonSpec AgainSpec;
	AgainSpec.Label = TEXT("Drive again");
	AgainSpec.KeyCap = TEXT("Enter");
	AgainSpec.Variant = EApexButtonVariant::Primary;
	AgainSpec.LabelSize = 22.0f;
	AgainSpec.Height = 62.0f;
	AgainSpec.ActionId = ActionDriveAgain;

	DriveAgainButton = WidgetTree->ConstructWidget<UApexButtonWidget>();
	DriveAgainButton->Setup(AgainSpec);
	DriveAgainButton->OnActivated.AddDynamic(this, &UApexSessionResultsWidget::HandleButtonActivated);
	ApexUI::AddV(Column, DriveAgainButton, FMargin(0.0f, 0.0f, 0.0f, 8.0f));

	FApexButtonSpec LobbySpec;
	LobbySpec.Label = TEXT("Back to lobby");
	LobbySpec.Variant = EApexButtonVariant::Ghost;
	LobbySpec.bCentreLabel = true;
	LobbySpec.LabelSize = 17.0f;
	LobbySpec.Height = 52.0f;
	LobbySpec.ActionId = ActionLobby;

	BackToLobbyButton = WidgetTree->ConstructWidget<UApexButtonWidget>();
	BackToLobbyButton->Setup(LobbySpec);
	BackToLobbyButton->OnActivated.AddDynamic(this, &UApexSessionResultsWidget::HandleButtonActivated);
	ApexUI::AddV(Column, BackToLobbyButton, FMargin(0.0f, 0.0f, 0.0f, 8.0f));

	// No replay system exists; the button says so rather than misleading.
	FApexButtonSpec ReplaySpec;
	ReplaySpec.Label = TEXT("Save replay");
	ReplaySpec.Badge = TEXT("Locked");
	ReplaySpec.Variant = EApexButtonVariant::Locked;
	ReplaySpec.LabelSize = 17.0f;
	ReplaySpec.Height = 52.0f;

	UApexButtonWidget* ReplayButton = WidgetTree->ConstructWidget<UApexButtonWidget>();
	ReplayButton->Setup(ReplaySpec);
	ApexUI::AddV(Column, ReplayButton);

	return ApexUI::MakePanel(
		*WidgetTree,
		Column,
		FMargin(28.0f, 24.0f, ApexUI::Metrics::PageGutter, 26.0f),
		ApexUI::MakeBrush(FLinearColor::Transparent));
}

// ---------------------------------------------------------------------------
// Data
// ---------------------------------------------------------------------------

void UApexSessionResultsWidget::RefreshTable()
{
	UApexSessionRecorder* Recorder = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexSessionRecorder>() : nullptr;
	const UApexNetSubsystem* Net = GetNet();
	UApexMenuFlowSubsystem* Flow = GetFlow();
	if (!Recorder || !Net || !Flow || !TableBox)
	{
		return;
	}

	// Header.
	FString TrackName;
	FApexTrackCatalogRow TrackRow;
	if (Flow->GetTrackCatalogRow(Recorder->GetTrackId(), TrackRow))
	{
		TrackName = TrackRow.DisplayName;
	}
	if (HeaderTrackText)
	{
		HeaderTrackText->SetText(FText::FromString(TrackName));
	}
	if (HeaderFormatText)
	{
		HeaderFormatText->SetText(FText::FromString(FString::Printf(
			TEXT("%s · %d LAPS · %d DRIVERS"),
			*UApexMenuFlowSubsystem::GetGameModeName(Recorder->GetGameMode()).ToUpper(),
			Recorder->GetLapLimit(),
			Recorder->GetResults().Num())));
	}

	TableBox->ClearChildren();

	const TArray<FApexCarResult>& Results = Recorder->GetResults();
	if (Results.Num() == 0)
	{
		ApexUI::AddV(TableBox, ApexUI::MakeText(
			*WidgetTree,
			TEXT("No session has been recorded yet."),
			ApexUI::Font::Body(14.0f),
			ApexUI::Palette::TextMuted));
		return;
	}

	// The gap column is measured against the quickest lap set by anyone.
	float LeaderBest = 0.0f;
	for (const FApexCarResult& Result : Results)
	{
		if (Result.BestLapSeconds > 0.0f && (LeaderBest <= 0.0f || Result.BestLapSeconds < LeaderBest))
		{
			LeaderBest = Result.BestLapSeconds;
		}
	}

	for (int32 Index = 0; Index < Results.Num(); ++Index)
	{
		const FApexCarResult& Result = Results[Index];
		const bool bIsLocal = !Result.bIsAi && Result.PlayerId.Equals(Net->GetPlayerId(), ESearchCase::IgnoreCase);
		const bool bHasLap = Result.BestLapSeconds > 0.0f;
		const FLinearColor RowColour = bHasLap ? ApexUI::Palette::TextPrimary : ApexUI::Palette::TextDisabled;

		UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();

		ApexUI::AddH(Row, MakeCell(
			*WidgetTree,
			FString::FromInt(Index + 1),
			ApexUI::Font::Display(22.0f),
			Index == 0 ? ApexUI::Palette::Accent : ApexUI::Palette::TextSecondary,
			ColPos));

		UHorizontalBox* NameCell = WidgetTree->ConstructWidget<UHorizontalBox>();
		ApexUI::AddH(NameCell, ApexUI::MakeText(
			*WidgetTree,
			Result.bIsAi ? FString::Printf(TEXT("AI · %s"), *Result.DriverName) : Result.DriverName,
			ApexUI::Font::Display(19.0f),
			RowColour));
		if (bIsLocal)
		{
			ApexUI::AddH(NameCell, ApexUI::MakeText(*WidgetTree, TEXT("YOU"), ApexUI::Font::Mono(9.0f, 120), ApexUI::Palette::Accent), FMargin(10.0f, 0.0f, 0.0f, 0.0f));
		}
		ApexUI::AddH(Row, ApexUI::MakeSized(*WidgetTree, NameCell, ColDriver, -1.0f));

		// The protocol never says which car an AI is in, and a human's choice is
		// only known while they are in the lobby list.
		FString CarName;
		for (const FApexLobbyPlayer& Player : Net->GetCachedLobbyState().PlayersInLobby)
		{
			if (Player.Id.Equals(Result.PlayerId, ESearchCase::IgnoreCase) && Player.HasSelectedCar())
			{
				FApexCarCatalogRow CarRow;
				CarName = Flow->GetCarCatalogRow(Player.SelectedCar, CarRow) ? CarRow.DisplayName : FString();
				break;
			}
		}
		ApexUI::AddH(Row, MakeCell(*WidgetTree, CarName.ToUpper(), ApexUI::Font::Mono(10.0f, 60), ApexUI::Palette::TextMuted, ColCar));

		ApexUI::AddH(Row, MakeCell(
			*WidgetTree,
			bHasLap ? UApexMenuFlowSubsystem::FormatLapTime(Result.BestLapSeconds) : TEXT("NO TIME"),
			ApexUI::Font::Display(bHasLap ? 24.0f : 18.0f),
			RowColour,
			ColBest));

		FString Gap = TEXT("—");
		if (bHasLap && LeaderBest > 0.0f)
		{
			const float Delta = Result.BestLapSeconds - LeaderBest;
			Gap = Delta <= 0.0f ? TEXT("—") : FString::Printf(TEXT("+%.3f"), Delta);
		}
		else if (!bHasLap)
		{
			Gap = TEXT("DNF");
		}
		ApexUI::AddH(Row, MakeCell(*WidgetTree, Gap, ApexUI::Font::Mono(11.0f), ApexUI::Palette::TextMuted, ColGap));

		ApexUI::AddH(Row, ApexUI::MakeText(*WidgetTree, FString::FromInt(Result.LapsCompleted()), ApexUI::Font::Mono(12.0f), ApexUI::Palette::TextSecondary));

		UBorder* RowPanel = ApexUI::MakePanel(
			*WidgetTree,
			Row,
			FMargin(18.0f, 14.0f),
			ApexUI::MakeBrush(
				ApexUI::Palette::Surface,
				bIsLocal ? ApexUI::Palette::Accent : ApexUI::Palette::Border,
				bIsLocal ? 2.0f : 1.0f));
		ApexUI::AddV(TableBox, RowPanel, FMargin(0.0f, 0.0f, 0.0f, 6.0f));
	}
}

void UApexSessionResultsWidget::RefreshSidePanel()
{
	UApexSessionRecorder* Recorder = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexSessionRecorder>() : nullptr;
	if (!Recorder || !SummaryBox || !LapChartBox)
	{
		return;
	}

	SummaryBox->ClearChildren();
	LapChartBox->ClearChildren();

	const FApexCarResult* Local = Recorder->FindLocalResult();
	if (!Local)
	{
		ApexUI::AddV(SummaryBox, ApexUI::MakeText(
			*WidgetTree,
			TEXT("You did not drive in this session."),
			ApexUI::Font::Body(14.0f),
			ApexUI::Palette::TextMuted));
		return;
	}

	// --- Best lap, large ------------------------------------------------------
	ApexUI::AddV(SummaryBox, ApexUI::MakeLabel(*WidgetTree, TEXT("Best lap")));
	ApexUI::AddV(
		SummaryBox,
		ApexUI::MakeText(
			*WidgetTree,
			Local->BestLapSeconds > 0.0f ? UApexMenuFlowSubsystem::FormatLapTime(Local->BestLapSeconds) : TEXT("NO TIME"),
			ApexUI::Font::Display(52.0f),
			ApexUI::Palette::Accent),
		FMargin(0.0f, 8.0f, 0.0f, 0.0f));

	const float Delta = Recorder->GetPersonalBestDelta();
	if (Delta > 0.0f)
	{
		ApexUI::AddV(
			SummaryBox,
			ApexUI::MakeText(
				*WidgetTree,
				FString::Printf(TEXT("−%.3f ON YOUR PREVIOUS BEST"), Delta),
				ApexUI::Font::Mono(10.0f, 100),
				ApexUI::Palette::Live),
			FMargin(0.0f, 6.0f, 0.0f, 0.0f));
	}

	// --- Four numbers ---------------------------------------------------------
	UTextBlock* TopSpeed = nullptr;
	UTextBlock* Consistency = nullptr;
	UTextBlock* ValidLaps = nullptr;
	UTextBlock* Distance = nullptr;

	UHorizontalBox* StatRowOne = WidgetTree->ConstructWidget<UHorizontalBox>();
	ApexUI::AddH(StatRowOne, ApexUI::MakeStat(*WidgetTree, TEXT("Top speed"), TopSpeed), FMargin(), VAlign_Fill, 1.0f);
	ApexUI::AddH(StatRowOne, ApexUI::MakeStat(*WidgetTree, TEXT("Consistency"), Consistency), FMargin(10.0f, 0.0f, 0.0f, 0.0f), VAlign_Fill, 1.0f);
	ApexUI::AddV(SummaryBox, StatRowOne, FMargin(0.0f, 22.0f, 0.0f, 0.0f));

	UHorizontalBox* StatRowTwo = WidgetTree->ConstructWidget<UHorizontalBox>();
	ApexUI::AddH(StatRowTwo, ApexUI::MakeStat(*WidgetTree, TEXT("Valid laps"), ValidLaps), FMargin(), VAlign_Fill, 1.0f);
	ApexUI::AddH(StatRowTwo, ApexUI::MakeStat(*WidgetTree, TEXT("Distance"), Distance), FMargin(10.0f, 0.0f, 0.0f, 0.0f), VAlign_Fill, 1.0f);
	ApexUI::AddV(SummaryBox, StatRowTwo, FMargin(0.0f, 10.0f, 0.0f, 0.0f));

	TopSpeed->SetText(FText::FromString(Local->TopSpeedMps > 0.0f
		? FString::Printf(TEXT("%.0f km/h"), Local->TopSpeedMps * 3.6f)
		: TEXT("—")));

	const float ConsistencyValue = Recorder->GetConsistency();
	Consistency->SetText(FText::FromString(ConsistencyValue > 0.0f
		? FString::Printf(TEXT("%.0f%%"), ConsistencyValue * 100.0f)
		: TEXT("—")));

	ValidLaps->SetText(FText::FromString(FString::Printf(TEXT("%d / %d"), Local->ValidLaps, Local->LapsCompleted())));

	Distance->SetText(FText::FromString(Local->DistanceM > 0.0f
		? FString::Printf(TEXT("%.1f km"), Local->DistanceM / 1000.0f)
		: TEXT("—")));

	// --- Lap by lap -----------------------------------------------------------
	if (Local->LapTimes.Num() == 0)
	{
		ApexUI::AddV(LapChartBox, ApexUI::MakeText(
			*WidgetTree,
			TEXT("No completed laps."),
			ApexUI::Font::Body(13.0f),
			ApexUI::Palette::TextMuted));
		return;
	}

	float Slowest = 0.0f;
	for (float Lap : Local->LapTimes)
	{
		Slowest = FMath::Max(Slowest, Lap);
	}

	// Bars are relative to the slowest lap, with the best one picked out. Short
	// bar means quick lap, which is the way round people read a lap chart.
	UHorizontalBox* Chart = WidgetTree->ConstructWidget<UHorizontalBox>();
	for (int32 Index = 0; Index < Local->LapTimes.Num(); ++Index)
	{
		const float Lap = Local->LapTimes[Index];
		const bool bIsBest = FMath::IsNearlyEqual(Lap, Local->BestLapSeconds, 0.0005f);
		const float Ratio = Slowest > 0.0f ? Lap / Slowest : 1.0f;

		UVerticalBox* Bar = WidgetTree->ConstructWidget<UVerticalBox>();
		ApexUI::AddV(Bar, WidgetTree->ConstructWidget<USpacer>(), FMargin(), HAlign_Fill, FMath::Max(0.01f, 1.0f - Ratio));
		ApexUI::AddV(
			Bar,
			ApexUI::MakePanel(
				*WidgetTree,
				nullptr,
				FMargin(),
				ApexUI::MakeBrush(bIsBest ? ApexUI::Palette::Accent : ApexUI::Palette::SurfaceHover)),
			FMargin(),
			HAlign_Fill,
			FMath::Max(0.01f, Ratio));

		UVerticalBox* Cell = WidgetTree->ConstructWidget<UVerticalBox>();
		ApexUI::AddV(Cell, ApexUI::MakeSized(*WidgetTree, Bar, -1.0f, 90.0f));
		ApexUI::AddV(
			Cell,
			ApexUI::MakeText(*WidgetTree, FString::FromInt(Index + 1), ApexUI::Font::Mono(9.0f), ApexUI::Palette::TextMuted),
			FMargin(0.0f, 6.0f, 0.0f, 0.0f),
			HAlign_Center);

		ApexUI::AddH(Chart, Cell, FMargin(Index == 0 ? 0.0f : 6.0f, 0.0f, 0.0f, 0.0f), VAlign_Fill, 1.0f);
	}
	ApexUI::AddV(LapChartBox, Chart);
}

// ---------------------------------------------------------------------------
// Interaction
// ---------------------------------------------------------------------------

void UApexSessionResultsWidget::HandleButtonActivated(UApexButtonWidget* Button)
{
	if (!Button)
	{
		return;
	}

	UApexNetSubsystem* Net = GetNet();
	UApexMenuFlowSubsystem* Flow = GetFlow();
	const FName Id = Button->GetActionId();

	if (Id == ActionDriveAgain)
	{
		if (!Net || !Flow)
		{
			return;
		}
		if (!Net->IsInSession())
		{
			// The session is gone; the main menu's start button rebuilds one from
			// the same setup.
			ShowScreen(EApexScreen::MainMenu);
			return;
		}
		if (!Net->IsUdpReady())
		{
			ShowToast(TEXT("Still binding the telemetry channel — try again in a moment"), true);
			return;
		}
		Net->StartCountdown(5, Flow->CreateStartingMode);
		return;
	}

	if (Id == ActionLobby || Id == ActionMenu)
	{
		if (UApexRootWidget* Root = GetRoot())
		{
			Root->ReplaceScreen(Net && Net->IsInSession() ? EApexScreen::SessionLobby : EApexScreen::MainMenu);
		}
	}
}
