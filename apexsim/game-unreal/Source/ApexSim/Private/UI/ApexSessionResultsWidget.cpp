#include "UI/ApexSessionResultsWidget.h"

#include "ApexMenuFlowSubsystem.h"
#include "ApexNetSubsystem.h"
#include "ApexReplayRecorder.h"
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
#include "HAL/IConsoleManager.h"
#include "Misc/Paths.h"
#include "Race/ApexQualifyingBoard.h"
#include "UI/ApexButtonWidget.h"
#include "UI/ApexChampionshipPanel.h"
#include "UI/ApexIncidentList.h"
#include "UI/ApexNavigation.h"
#include "UI/ApexRaceResultsWidget.h"
#include "UI/ApexRootWidget.h"
#include "UI/ApexUIStyle.h"
#include "UObject/UObjectIterator.h"

namespace
{
	FAutoConsoleCommandWithWorldAndArgs IncidentsCommand(
		TEXT("apexsim.incidents"),
		TEXT("The results screen's incident list: apexsim.incidents [show|hide|log|watch <n>] ")
		TEXT("(show turns the table to it and asks the server again; watch plays this session's replay from just before incident n, 0-based)"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			UApexSessionResultsWidget* Results = nullptr;
			for (TObjectIterator<UApexSessionResultsWidget> It; It; ++It)
			{
				if (It->GetWorld() == World && It->GetCachedWidget().IsValid())
				{
					Results = *It;
					break;
				}
			}
			if (!Results)
			{
				UE_LOG(LogApexSim, Warning, TEXT("apexsim.incidents: no results screen in this world"));
				return;
			}
			const FString Verb = Args.Num() > 0 ? Args[0].ToLower() : FString(TEXT("show"));
			if (Verb == TEXT("show") || Verb == TEXT("hide"))
			{
				Results->ShowIncidents(Verb == TEXT("show"));
			}
			else if (Verb == TEXT("watch") && Args.Num() > 1)
			{
				Results->JumpToIncident(FCString::Atoi(*Args[1]));
			}
			Results->LogIncidents();
		}));

	const FName ActionDriveAgain(TEXT("__again"));
	const FName ActionLobby(TEXT("__lobby"));
	const FName ActionSaveReplay(TEXT("__savereplay"));
	const FName ActionMenu(TEXT("__menu"));
	const FName ActionIncidents(TEXT("__incidents"));
	/** An incident row's action: this, then its index in the list. */
	const FString IncidentActionPrefix(TEXT("__incident_"));

	constexpr float ResultsSidePanelWidth = 470.0f;

	/** How often the provisional table follows the running order. */
	constexpr float LiveRefreshSeconds = 0.5f;

	/** Column widths for the classification table, in slate units. */
	constexpr float ColPos = 60.0f;
	constexpr float ColDriver = 260.0f;
	constexpr float ColCar = 280.0f;
	constexpr float ColBest = 190.0f;
	constexpr float ColGap = 140.0f;

	/** Qualifying's columns: narrower names to make room for the sectors. */
	constexpr float ColQualDriver = 240.0f;
	constexpr float ColQualCar = 200.0f;
	constexpr float ColQualBest = 160.0f;
	constexpr float ColQualGap = 100.0f;
	constexpr float ColSector = 92.0f;

	/** The timing screens' purple: the quickest anyone drove that sector. */
	const FLinearColor SectorPurple = FLinearColor::FromSRGBColor(FColor(0xB0, 0x6C, 0xF5));

	UWidget* MakeCell(UWidgetTree& Tree, const FString& Text, const FSlateFontInfo& Font, const FLinearColor& Colour, float Width)
	{
		return ApexUI::MakeSized(Tree, ApexUI::MakeText(Tree, Text, Font, Colour), Width, -1.0f);
	}

	/** A sector time: seconds to the thousandth, minutes only when a sector runs that long. */
	FString FormatSplit(int32 Ms)
	{
		if (Ms <= 0)
		{
			return TEXT("--.---");
		}
		return Ms >= 60000 ? UApexMenuFlowSubsystem::FormatLapTime(Ms / 1000.0f) : FString::Printf(TEXT("%.3f"), Ms / 1000.0f);
	}

	/** "+0.312", "-0.104": a difference in ms, signed. */
	FString FormatSignedMs(int32 Ms)
	{
		return FString::Printf(TEXT("%s%.3f"), Ms < 0 ? TEXT("−") : TEXT("+"), FMath::Abs(Ms) / 1000.0f);
	}

	int32 ToMs(float Seconds)
	{
		return Seconds > 0.0f ? FMath::RoundToInt(Seconds * 1000.0f) : 0;
	}

	TArray<int32> ToMs(const TArray<float>& Seconds)
	{
		TArray<int32> Out;
		Out.Reserve(Seconds.Num());
		for (const float Value : Seconds)
		{
			Out.Add(ToMs(Value));
		}
		return Out;
	}

	/** The recorded results as the qualifying sheet takes them. */
	TArray<ApexBoard::FSheetRow> QualifyingSheet(const TArray<FApexCarResult>& Results)
	{
		TArray<ApexBoard::FSheetEntry> Entries;
		Entries.Reserve(Results.Num());
		for (const FApexCarResult& Result : Results)
		{
			ApexBoard::FSheetEntry Entry;
			Entry.CarIndex = Result.CarIndex;
			Entry.BestMs = ToMs(Result.BestLapSeconds);
			Entry.BestLapSplitsMs = ToMs(Result.BestLapSplitsSeconds);
			Entry.BestSectorsMs = ToMs(Result.BestSectorsSeconds);
			Entries.Add(MoveTemp(Entry));
		}
		return ApexBoard::Sheet(Entries);
	}

	const FApexCarResult* FindResult(const TArray<FApexCarResult>& Results, int32 CarIndex)
	{
		return Results.FindByPredicate([CarIndex](const FApexCarResult& Result) { return Result.CarIndex == CarIndex; });
	}

	/** A small caption tag beside a driver's name: YOU, POLE. */
	UWidget* MakeTag(UWidgetTree& Tree, const FString& Text, const FLinearColor& Colour)
	{
		return ApexUI::MakeText(Tree, Text, ApexUI::Font::Mono(9.0f, 120), Colour);
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

	if (UApexNetSubsystem* Net = GetNet())
	{
		// A weekend's state (who is out, whether the host may move on) can land
		// after the session's end.
		Net->OnWeekendStateChanged.AddUniqueDynamic(this, &UApexSessionResultsWidget::HandleWeekendStateChanged);
		if (!IncidentsHandle.IsValid())
		{
			IncidentsHandle = Net->OnIncidents.AddUObject(this, &UApexSessionResultsWidget::HandleIncidents);
		}
		if (!ChampionshipsHandle.IsValid())
		{
			// The standings that count this race arrive once the server keeps it.
			ChampionshipsHandle = Net->OnChampionships.AddWeakLambda(this, [this](const FApexChampionships&)
			{
				if (IsVisible())
				{
					RefreshChampionships();
				}
			});
		}
	}

	// A new visit starts on the classification.
	bShowingIncidents = false;
	LiveRefreshCountdown = LiveRefreshSeconds;
	RefreshIncidentsButton();
	RefreshTable();
	RefreshSidePanel();
	RefreshChampionships();
	RefreshSaveReplay();
	RefreshPrimaryAction();
}

void UApexSessionResultsWidget::RefreshChampionships()
{
	const UApexNetSubsystem* Net = GetNet();
	if (!ChampionshipBox || !Net)
	{
		return;
	}
	// A race's results: a qualifying or practice sheet has its own summary.
	const bool bAny = !IsTimesheet() && ApexChampionshipPanel::Fill(*WidgetTree, *ChampionshipBox, *Net, 3);
	if (!bAny)
	{
		ChampionshipBox->ClearChildren();
	}
	ChampionshipBox->SetVisibility(bAny ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
}

void UApexSessionResultsWidget::HandleWeekendStateChanged()
{
	if (!IsVisible())
	{
		return;
	}
	RefreshTable();
	RefreshSidePanel();
	RefreshPrimaryAction();
}

void UApexSessionResultsWidget::HandleIncidents(const FApexIncidents& Incidents)
{
	if (IsVisible() && bShowingIncidents)
	{
		RefreshTable();
	}
}

void UApexSessionResultsWidget::RefreshIncidentsButton()
{
	if (!IncidentsButton)
	{
		return;
	}
	// The list is the server's, of a race: asked for while still in its session.
	const UApexNetSubsystem* Net = GetNet();
	const bool bCan = Net && Net->IsInSession() && !IsTimesheet();
	if (!bCan)
	{
		bShowingIncidents = false;
	}
	IncidentsButton->SetVisibility(bCan ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
	IncidentsButton->SetLabel(bShowingIncidents ? TEXT("Classification") : TEXT("Incidents"));
}

void UApexSessionResultsWidget::RefreshSaveReplay()
{
	if (!SaveReplayButton)
	{
		return;
	}
	const UApexReplayRecorder* Replays = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexReplayRecorder>() : nullptr;
	const bool bKept = Replays && Replays->IsThisSessionKept();
	const bool bCan = Replays && Replays->CanKeepThisSession();
	SaveReplayButton->SetIsEnabled(bCan && !bKept);
	SaveReplayButton->SetBadge(bKept ? (Replays->IsRecording() ? TEXT("At the flag") : TEXT("Saved"))
		: bCan ? FString() : TEXT("Nothing recorded"),
		bKept ? ApexUI::Palette::Live : ApexUI::Palette::TextMuted);
}

void UApexSessionResultsWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);

	const bool bLive = IsLive();
	if (!bLive && !bShowingLive)
	{
		return;
	}
	LiveRefreshCountdown -= InDeltaTime;
	if (bLive && LiveRefreshCountdown > 0.0f)
	{
		return;
	}
	LiveRefreshCountdown = LiveRefreshSeconds;
	if (bShowingIncidents)
	{
		// The list grows while the race runs, and the replay to jump into is
		// written at its end: ask again, it redraws on arrival.
		if (UApexNetSubsystem* Net = GetNet())
		{
			Net->RequestIncidents();
		}
	}
	else
	{
		RefreshTable();
	}
	RefreshSaveReplay();
	RefreshPrimaryAction();
	RefreshIncidentsButton();
	if (!bLive)
	{
		// The session just ended: the recorder has filed the local best by now.
		RefreshSidePanel();
	}
}

bool UApexSessionResultsWidget::IsLive() const
{
	const UApexSessionRecorder* Recorder = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexSessionRecorder>() : nullptr;
	return Recorder && Recorder->IsRecording();
}

bool UApexSessionResultsWidget::IsTimesheet() const
{
	const UApexSessionRecorder* Recorder = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexSessionRecorder>() : nullptr;
	return Recorder && ApexIsOutlapMode(Recorder->GetGameMode());
}

bool UApexSessionResultsWidget::IsPractice() const
{
	const UApexSessionRecorder* Recorder = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexSessionRecorder>() : nullptr;
	return Recorder && Recorder->GetGameMode() == EApexGameMode::Practice;
}

FString UApexSessionResultsWidget::SessionTitle(bool bLive) const
{
	if (!IsTimesheet())
	{
		return bLive ? TEXT("PROVISIONAL · RACE RUNNING") : TEXT("SESSION COMPLETE");
	}
	if (IsPractice())
	{
		return bLive ? TEXT("PROVISIONAL · PRACTICE RUNNING") : TEXT("PRACTICE · TIMESHEET");
	}
	const FApexWeekendState* Knockout = KnockoutRound();
	FString Name = Knockout && Knockout->Rounds > 1 ? FString::Printf(TEXT("Q%d OF %d"), Knockout->Round, Knockout->Rounds) : FString(TEXT("QUALIFYING"));
	if (IsShootout())
	{
		Name += TEXT(" SHOOT-OUT");
	}
	return bLive ? FString::Printf(TEXT("PROVISIONAL · %s RUNNING"), *Name)
		: Knockout && !Knockout->IsFinalRound() ? FString::Printf(TEXT("%s · CLASSIFICATION"), *Name)
		: IsShootout() ? FString(TEXT("QUALIFYING SHOOT-OUT · THE GRID"))
		: FString(TEXT("QUALIFYING · THE GRID"));
}

bool UApexSessionResultsWidget::IsShootout() const
{
	const UApexNetSubsystem* Net = GetNet();
	if (!Net || !IsTimesheet() || IsPractice())
	{
		return false;
	}
	const FApexWeekendState* Knockout = KnockoutRound();
	return ApexBoard::IsShootoutRound(Net->GetSessionQualifyingFormat(), Net->IsWeekendSession(),
		Knockout ? Knockout->Round : 1, Knockout ? Knockout->Rounds : 1);
}

bool UApexSessionResultsWidget::IsSessionHost() const
{
	const UApexNetSubsystem* Net = GetNet();
	FApexSessionSummary Session;
	return Net && Net->FindSessionById(Net->GetCurrentSessionId(), Session)
		&& Session.HostName.Equals(Net->GetPlayerName(), ESearchCase::IgnoreCase);
}

void UApexSessionResultsWidget::FocusDefault()
{
	if (!ApexNav::Focus(DriveAgainButton) && !ApexNav::Focus(BackToLobbyButton))
	{
		Super::FocusDefault();
	}
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

	// Filled by RefreshTableHeader: a race and qualifying have different columns.
	TableHeaderRow = WidgetTree->ConstructWidget<UHorizontalBox>();
	ApexUI::AddV(TableColumn, ApexUI::MakePanel(*WidgetTree, TableHeaderRow, FMargin(18.0f, 0.0f, 18.0f, 14.0f), ApexUI::MakeBrush(FLinearColor::Transparent)));

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

	HeaderStatusText = ApexUI::MakeText(*WidgetTree, TEXT("SESSION COMPLETE"), ApexUI::Font::Mono(10.0f, 160), ApexUI::Palette::Accent);
	ApexUI::AddH(Row, HeaderStatusText);
	ApexUI::AddH(Row, ApexUI::MakeSized(*WidgetTree, ApexUI::MakeDivider(*WidgetTree, true), 1.0f, 22.0f), FMargin(18.0f, 0.0f));

	HeaderTrackText = ApexUI::MakeText(*WidgetTree, FString(), ApexUI::Font::Display(22.0f), ApexUI::Palette::TextPrimary);
	ApexUI::AddH(Row, HeaderTrackText);

	HeaderFormatText = ApexUI::MakeText(*WidgetTree, FString(), ApexUI::Font::Mono(10.0f, 100), ApexUI::Palette::TextMuted);
	ApexUI::AddH(Row, HeaderFormatText, FMargin(16.0f, 0.0f, 0.0f, 0.0f));

	UVerticalBox* Stack = WidgetTree->ConstructWidget<UVerticalBox>();
	UBorder* Bar = ApexUI::MakePanel(*WidgetTree, Row, FMargin(ApexUI::Metrics::PageGutter, 0.0f), ApexUI::MakeBrush(ApexUI::Palette::Background));
	ApexUI::AlignContent(*Bar, HAlign_Fill, VAlign_Center);
	ApexUI::AddV(Stack, ApexUI::MakeSized(*WidgetTree, Bar, -1.0f, ApexUI::Metrics::TopBarHeight));
	ApexUI::AddV(Stack, ApexUI::MakeDivider(*WidgetTree));
	return Stack;
}

UWidget* UApexSessionResultsWidget::BuildSidePanel()
{
	UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>();

	SummaryBox = WidgetTree->ConstructWidget<UVerticalBox>();
	ApexUI::AddV(Column, SummaryBox);

	ApexUI::AddV(Column, ApexUI::MakeLabel(*WidgetTree, TEXT("Lap by lap")), FMargin(0.0f, 26.0f, 0.0f, 12.0f));
	LapChartBox = WidgetTree->ConstructWidget<UVerticalBox>();
	ApexUI::AddV(Column, LapChartBox);

	ChampionshipBox = WidgetTree->ConstructWidget<UVerticalBox>();
	ApexUI::AddV(Column, ChampionshipBox, FMargin(0.0f, 26.0f, 0.0f, 0.0f));

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

	// Keeps the whole session's replay: at once when it has ended, else when
	// it does (UApexReplayRecorder::KeepThisSession).
	FApexButtonSpec ReplaySpec;
	ReplaySpec.Label = TEXT("Save replay");
	ReplaySpec.Variant = EApexButtonVariant::Ghost;
	ReplaySpec.LabelSize = 17.0f;
	ReplaySpec.Height = 52.0f;
	ReplaySpec.ActionId = ActionSaveReplay;

	SaveReplayButton = WidgetTree->ConstructWidget<UApexButtonWidget>();
	SaveReplayButton->Setup(ReplaySpec);
	SaveReplayButton->OnActivated.AddDynamic(this, &UApexSessionResultsWidget::HandleButtonActivated);
	ApexUI::AddV(Column, SaveReplayButton, FMargin(0.0f, 0.0f, 0.0f, 8.0f));

	// The race's incident list in the table, and back (RefreshIncidentsButton).
	FApexButtonSpec IncidentsSpec;
	IncidentsSpec.Label = TEXT("Incidents");
	IncidentsSpec.Variant = EApexButtonVariant::Ghost;
	IncidentsSpec.LabelSize = 17.0f;
	IncidentsSpec.Height = 52.0f;
	IncidentsSpec.ActionId = ActionIncidents;

	IncidentsButton = WidgetTree->ConstructWidget<UApexButtonWidget>();
	IncidentsButton->Setup(IncidentsSpec);
	IncidentsButton->OnActivated.AddDynamic(this, &UApexSessionResultsWidget::HandleButtonActivated);
	ApexUI::AddV(Column, IncidentsButton);

	return ApexUI::MakePanel(
		*WidgetTree,
		Column,
		FMargin(28.0f, 24.0f, ApexUI::Metrics::PageGutter, 26.0f),
		ApexUI::MakeBrush(FLinearColor::Transparent));
}

// ---------------------------------------------------------------------------
// Data
// ---------------------------------------------------------------------------

void UApexSessionResultsWidget::RefreshTableHeader(bool bQualifying)
{
	if (!TableHeaderRow)
	{
		return;
	}
	TableHeaderRow->ClearChildren();
	const FSlateFontInfo Font = ApexUI::Font::Mono(10.0f, 120);
	const FLinearColor Colour = ApexUI::Palette::TextMuted;
	auto Add = [&](const TCHAR* Caption, float Width)
	{
		ApexUI::AddH(TableHeaderRow, Width > 0.0f
			? MakeCell(*WidgetTree, Caption, Font, Colour, Width)
			: static_cast<UWidget*>(ApexUI::MakeText(*WidgetTree, Caption, Font, Colour)));
	};

	if (bQualifying)
	{
		Add(TEXT("POS"), ColPos);
		Add(TEXT("DRIVER"), ColQualDriver);
		Add(TEXT("CAR"), ColQualCar);
		Add(TEXT("BEST LAP"), ColQualBest);
		Add(TEXT("GAP"), ColQualGap);
		Add(TEXT("INT"), ColQualGap);
		Add(TEXT("S1"), ColSector);
		Add(TEXT("S2"), ColSector);
		Add(TEXT("S3"), ColSector);
		Add(TEXT("LAPS"), -1.0f);
		return;
	}
	Add(TEXT("POS"), ColPos);
	Add(TEXT("DRIVER"), ColDriver);
	Add(TEXT("CAR"), ColCar);
	Add(TEXT("BEST LAP"), ColBest);
	Add(TEXT("GAP"), ColGap);
	Add(TEXT("LAPS"), -1.0f);
}

void UApexSessionResultsWidget::RefreshTable()
{
	UApexSessionRecorder* Recorder = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexSessionRecorder>() : nullptr;
	const UApexNetSubsystem* Net = GetNet();
	UApexMenuFlowSubsystem* Flow = GetFlow();
	if (!Recorder || !Net || !Flow || !TableBox)
	{
		return;
	}

	const bool bLive = Recorder->IsRecording();
	const bool bQualifying = IsTimesheet();
	bShowingLive = bLive;

	// Header.
	if (HeaderStatusText)
	{
		HeaderStatusText->SetText(FText::FromString(SessionTitle(bLive)));
		HeaderStatusText->SetColorAndOpacity(FSlateColor(bLive ? ApexUI::Palette::Live : ApexUI::Palette::Accent));
	}

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
		const UApexNetSubsystem* WeekendNet = GetNet();
		const bool bWeekend = WeekendNet && WeekendNet->IsWeekendSession();
		const FString Length = IsShootout()
			? FString(TEXT("SHOOT-OUT · ONE FLYING LAP EACH, IN RUN ORDER"))
			: bWeekend && bQualifying
			? FString(TEXT("RACE WEEKEND"))
			: bQualifying
			? (Recorder->GetQualifyingSeconds() > 0 ? ApexRaceLength::Describe(Recorder->GetQualifyingSeconds()).ToUpper() : FString(TEXT("OPEN")))
			: Recorder->GetRaceSeconds() > 0 ? ApexRaceLength::Describe(Recorder->GetRaceSeconds()).ToUpper()
			: FString::Printf(TEXT("%d LAPS"), Recorder->GetLapLimit());
		HeaderFormatText->SetText(FText::FromString(FString::Printf(
			TEXT("%s · %s · %d DRIVERS"),
			*UApexMenuFlowSubsystem::GetGameModeName(Recorder->GetGameMode()).ToUpper(),
			*Length,
			Recorder->GetResults().Num())));
	}

	if (bShowingIncidents && !bQualifying)
	{
		FillIncidentRows();
		return;
	}

	RefreshTableHeader(bQualifying);
	TableBox->ClearChildren();

	if (Recorder->GetResults().Num() == 0)
	{
		ApexUI::AddV(TableBox, ApexUI::MakeText(
			*WidgetTree,
			TEXT("No session has been recorded yet."),
			ApexUI::Font::Body(14.0f),
			ApexUI::Palette::TextMuted));
		return;
	}

	if (bQualifying)
	{
		FillQualifyingRows();
	}
	else
	{
		FillRaceRows(bLive);
	}
}

void UApexSessionResultsWidget::FillIncidentRows()
{
	const UApexNetSubsystem* Net = GetNet();
	if (!Net || !TableBox || !TableHeaderRow)
	{
		return;
	}
	TableHeaderRow->ClearChildren();
	ApexUI::AddH(TableHeaderRow, ApexUI::MakeText(*WidgetTree,
		TEXT("RACE INCIDENTS · ENTER WATCHES THE REPLAY FROM JUST BEFORE ONE (LEAVES THE SESSION)"),
		ApexUI::Font::Mono(10.0f, 120), ApexUI::Palette::TextMuted));
	TableBox->ClearChildren();

	const FApexIncidents& List = Net->GetIncidents();
	if (!List.bValid || List.Incidents.Num() == 0)
	{
		ApexUI::AddV(TableBox, ApexUI::MakeText(*WidgetTree,
			!List.bValid ? TEXT("Asking the server for the race's incidents…") : TEXT("No incidents: a clean race."),
			ApexUI::Font::Body(14.0f), ApexUI::Palette::TextMuted));
		return;
	}

	const UApexReplayRecorder* Replays = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexReplayRecorder>() : nullptr;
	const int32 Own = Net->GetLocalCarIndex();
	for (int32 Index = 0; Index < List.Incidents.Num(); ++Index)
	{
		const FApexIncident& Incident = List.Incidents[Index];
		FString Path;
		double ReplaySeconds = 0.0;
		const bool bWatchable = Replays && Replays->ReplaySecondsOf(List.SessionId, Incident.Tick, Path, ReplaySeconds);
		const bool bMine = Own >= 0 && (Incident.Car == Own || Incident.Other == Own);

		FApexButtonSpec Row;
		Row.Label = FString::Printf(TEXT("%s · %s"), *ApexIncidentList::What(Incident), *ApexIncidentList::Cars(Incident));
		Row.SubLabel = FString::Printf(TEXT("%s · lap %d · %s"),
			*ApexIncidentList::RaceClock(List.RaceSeconds(Incident)), Incident.Lap, *ApexIncidentList::Detail(Incident));
		Row.Badge = bWatchable ? TEXT("WATCH") : FString();
		Row.BadgeColour = bMine ? ApexUI::Palette::Accent : ApexUI::Palette::TextSecondary;
		Row.LabelSize = 16.0f;
		Row.ActionId = FName(*(IncidentActionPrefix + FString::FromInt(Index)));
		UApexButtonWidget* Button = WidgetTree->ConstructWidget<UApexButtonWidget>();
		Button->Setup(Row);
		Button->OnActivated.AddDynamic(this, &UApexSessionResultsWidget::HandleButtonActivated);
		ApexUI::AddV(TableBox, Button, FMargin(0.0f, 0.0f, 0.0f, 4.0f));
	}
}

void UApexSessionResultsWidget::ShowIncidents(bool bShow)
{
	bShowingIncidents = bShow;
	if (bShow)
	{
		if (UApexNetSubsystem* Net = GetNet())
		{
			Net->RequestIncidents();
		}
	}
	RefreshIncidentsButton();
	RefreshTable();
}

void UApexSessionResultsWidget::LogIncidents() const
{
	const UApexNetSubsystem* Net = GetNet();
	if (!Net || !Net->GetIncidents().bValid)
	{
		UE_LOG(LogApexSim, Display, TEXT("apexsim.incidents: no list yet"));
		return;
	}
	const FApexIncidents& List = Net->GetIncidents();
	const UApexReplayRecorder* Replays = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexReplayRecorder>() : nullptr;
	UE_LOG(LogApexSim, Display, TEXT("apexsim.incidents: %d"), List.Incidents.Num());
	for (int32 Index = 0; Index < List.Incidents.Num(); ++Index)
	{
		const FApexIncident& Incident = List.Incidents[Index];
		FString Path;
		double Seconds = -1.0;
		const bool bWatchable = Replays && Replays->ReplaySecondsOf(List.SessionId, Incident.Tick, Path, Seconds);
		UE_LOG(LogApexSim, Display, TEXT("  %d: %s lap %d %s · %s · %s%s"), Index,
			*ApexIncidentList::RaceClock(List.RaceSeconds(Incident)), Incident.Lap, *ApexIncidentList::What(Incident),
			*ApexIncidentList::Cars(Incident), *ApexIncidentList::Detail(Incident),
			bWatchable ? *FString::Printf(TEXT(" (replay %.1f s)"), Seconds) : TEXT(""));
	}
}

void UApexSessionResultsWidget::JumpToIncident(int32 Index)
{
	const UApexNetSubsystem* Net = GetNet();
	const UApexReplayRecorder* Replays = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexReplayRecorder>() : nullptr;
	if (!Net || !Replays || !Net->GetIncidents().Incidents.IsValidIndex(Index))
	{
		return;
	}
	const FApexIncidents& List = Net->GetIncidents();
	FString Path;
	double Seconds = 0.0;
	if (!Replays->ReplaySecondsOf(List.SessionId, List.Incidents[Index].Tick, Path, Seconds))
	{
		ShowToast(Replays->IsRecording()
			? TEXT("The replay is written when the session ends: wait for the flag")
			: TEXT("This machine's replay of the race does not reach back to that incident"), true);
		return;
	}
	if (UApexRootWidget* Root = GetRoot())
	{
		// The watch camera on the incident's car, not wherever the director looks.
		const int32 Car = List.Incidents[Index].Car;
		Root->WatchReplayAt(Path, ApexIncidentList::JumpSeconds(Seconds), Car == ApexIncident::NoCar ? INDEX_NONE : Car);
	}
}

namespace
{
	/** The car a result drove, by its catalog row; false when unknown. */
	bool CarRowOf(const UApexNetSubsystem& Net, UApexMenuFlowSubsystem& Flow, const FApexCarResult& Result, FApexCarCatalogRow& CarRow)
	{
		// The roster names every car's model (AI included); a lobby player's
		// pick stands in for a roster from an older server.
		FString CarId;
		for (const FApexRosterEntry& Entry : Net.GetSessionRoster().Entries)
		{
			if (Entry.CarIndex == Result.CarIndex && !Entry.CarConfigId.IsEmpty())
			{
				CarId = Entry.CarConfigId;
				break;
			}
		}
		for (const FApexLobbyPlayer& Player : Net.GetCachedLobbyState().PlayersInLobby)
		{
			if (CarId.IsEmpty() && Player.Id.Equals(Result.PlayerId, ESearchCase::IgnoreCase) && Player.HasSelectedCar())
			{
				CarId = Player.SelectedCar;
				break;
			}
		}
		return !CarId.IsEmpty() && Flow.GetCarCatalogRow(CarId, CarRow);
	}

	/** The car a result drove, by its catalog display name; empty when unknown. */
	FString CarNameOf(const UApexNetSubsystem& Net, UApexMenuFlowSubsystem& Flow, const FApexCarResult& Result)
	{
		FApexCarCatalogRow CarRow;
		return CarRowOf(Net, Flow, Result, CarRow) ? CarRow.DisplayName : FString();
	}

	/** The car's catalog class (raw); empty when unknown. */
	FString CarClassOf(const UApexNetSubsystem& Net, UApexMenuFlowSubsystem& Flow, const FApexCarResult& Result)
	{
		FApexCarCatalogRow CarRow;
		return CarRowOf(Net, Flow, Result, CarRow) ? CarRow.CarClass.TrimStartAndEnd() : FString();
	}

	bool IsLocalResult(const UApexNetSubsystem& Net, const FApexCarResult& Result)
	{
		return !Result.bIsAi && Result.PlayerId.Equals(Net.GetPlayerId(), ESearchCase::IgnoreCase);
	}
}

void UApexSessionResultsWidget::FillRaceRows(bool bLive)
{
	UApexSessionRecorder* Recorder = GetGameInstance()->GetSubsystem<UApexSessionRecorder>();
	const UApexNetSubsystem* Net = GetNet();
	UApexMenuFlowSubsystem* Flow = GetFlow();
	const TArray<FApexCarResult>& Results = Recorder->GetResults();

	// The gap column is the gap at the flag, as the finish overlay reads it:
	// the winner's race time, a finisher's time (or laps) behind it, and ON
	// TRACK or DNF for a car without a flag.
	TSet<int32> MissedStops;
	for (const FApexRosterEntry& Entry : Net->GetSessionRoster().Entries)
	{
		if (Entry.bMissedStops)
		{
			MissedStops.Add(Entry.CarIndex);
		}
	}
	TMap<int32, FString> GapByCar;
	for (const ApexRaceResults::FRow& Line : ApexRaceResults::BuildRows(Results, Net->GetPlayerId(), bLive, TSet<int32>(), MissedStops))
	{
		GapByCar.Add(Line.CarIndex, Line.Time);
	}
	// A multi-class race: each car's place in its class, counted down the
	// overall order, shown beside the car.
	TMap<int32, FString> ClassByCar;
	TMap<int32, int32> ClassPlaceByCar;
	TMap<FString, int32> ClassCounts;
	for (const FApexCarResult& Result : Results)
	{
		const FString Class = CarClassOf(*Net, *Flow, Result);
		ClassByCar.Add(Result.CarIndex, Class);
		ClassPlaceByCar.Add(Result.CarIndex, ++ClassCounts.FindOrAdd(Class.ToLower()));
	}
	const bool bMultiClass = ClassCounts.Num() > 1;

	for (int32 Index = 0; Index < Results.Num(); ++Index)
	{
		const FApexCarResult& Result = Results[Index];
		const bool bIsLocal = IsLocalResult(*Net, Result);
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
			ApexUI::AddH(NameCell, MakeTag(*WidgetTree, TEXT("YOU"), ApexUI::Palette::Accent), FMargin(10.0f, 0.0f, 0.0f, 0.0f));
		}
		// Past the flag owing the race's pit rule: classified behind every car that met it.
		if (MissedStops.Contains(Result.CarIndex) && Result.FinishPosition > 0)
		{
			ApexUI::AddH(NameCell, MakeTag(*WidgetTree, TEXT("PEN"), ApexUI::Palette::Error), FMargin(10.0f, 0.0f, 0.0f, 0.0f));
		}
		ApexUI::AddH(Row, ApexUI::MakeSized(*WidgetTree, NameCell, ColDriver, -1.0f));

		FString CarText = CarNameOf(*Net, *Flow, Result).ToUpper();
		if (bMultiClass)
		{
			const FString Class = ClassByCar.FindRef(Result.CarIndex);
			CarText = FString::Printf(TEXT("%s P%d%s%s"),
				*(Class.IsEmpty() ? FString(TEXT("—")) : ApexCatalog::DisplayClass(Class).ToUpper()),
				ClassPlaceByCar.FindRef(Result.CarIndex),
				CarText.IsEmpty() ? TEXT("") : TEXT(" · "), *CarText);
		}
		ApexUI::AddH(Row, MakeCell(*WidgetTree, CarText, ApexUI::Font::Mono(10.0f, 60), ApexUI::Palette::TextMuted, ColCar));

		ApexUI::AddH(Row, MakeCell(
			*WidgetTree,
			bHasLap ? UApexMenuFlowSubsystem::FormatLapTime(Result.BestLapSeconds) : TEXT("NO TIME"),
			ApexUI::Font::Display(bHasLap ? 24.0f : 18.0f),
			RowColour,
			ColBest));

		const FString* GapText = GapByCar.Find(Result.CarIndex);
		const FString Gap = GapText ? *GapText : FString(TEXT("—"));
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

void UApexSessionResultsWidget::FillQualifyingRows()
{
	UApexSessionRecorder* Recorder = GetGameInstance()->GetSubsystem<UApexSessionRecorder>();
	const UApexNetSubsystem* Net = GetNet();
	UApexMenuFlowSubsystem* Flow = GetFlow();
	const TArray<FApexCarResult>& Results = Recorder->GetResults();
	const bool bPractice = IsPractice();
	const FApexWeekendState* Knockout = KnockoutRound();

	// A knockout round's table is the cars that ran in it; those out in an
	// earlier round are listed after it, where they start.
	TArray<FApexCarResult> InRound;
	for (const FApexCarResult& Result : Results)
	{
		const FApexKnockedOut* Out = Knockout ? Knockout->FindEliminated(Result.CarIndex) : nullptr;
		if (!Out || Out->Round >= Knockout->Round)
		{
			InRound.Add(Result);
		}
	}
	const int32 Advancing = Knockout ? Knockout->Advancing : 0;
	const bool bShootout = IsShootout();
	const TArray<ApexBoard::FSheetRow> Sheet = QualifyingSheet(InRound);

	// Once the round is over the server's list is the cut (it orders cars
	// with no time by grid slot); while it runs, the top Advancing by time.
	auto IsOut = [&](const ApexBoard::FSheetRow& Line)
	{
		if (Advancing <= 0)
		{
			return false;
		}
		if (const FApexKnockedOut* Out = Knockout->FindEliminated(Line.CarIndex))
		{
			return Out->Round == Knockout->Round;
		}
		return !Knockout->bFinished && (Line.Position <= 0 || Line.Position > Advancing);
	};

	bool bNoTimeCaptioned = false;
	auto AddRow = [&](const ApexBoard::FSheetRow& Line, int32 Place)
	{
		const FApexCarResult* Result = FindResult(Results, Line.CarIndex);
		if (!Result)
		{
			return;
		}
		const bool bIsLocal = IsLocalResult(*Net, *Result);
		const bool bHasTime = Line.Position > 0;
		const bool bTop = Line.Position == 1;
		const FApexKnockedOut* Out = Knockout ? Knockout->FindEliminated(Line.CarIndex) : nullptr;
		const FLinearColor RowColour = bHasTime && !Out ? ApexUI::Palette::TextPrimary
			: bHasTime ? ApexUI::Palette::TextSecondary : ApexUI::Palette::TextDisabled;

		// The cars with no time start behind everyone who set one.
		if (!bHasTime && !bNoTimeCaptioned && !Out)
		{
			bNoTimeCaptioned = true;
			ApexUI::AddV(TableBox, ApexUI::MakeLabel(*WidgetTree, bPractice ? TEXT("No time") : TEXT("No time · start behind the times")),
				FMargin(18.0f, 10.0f, 0.0f, 8.0f));
		}

		UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();

		// Practice has no grid, so a car with no time has no place; in
		// qualifying every car has its slot, timed or not.
		const bool bPlaced = bHasTime || (!bPractice && Place > 0);
		ApexUI::AddH(Row, MakeCell(
			*WidgetTree,
			bPlaced ? FString::FromInt(Out ? Out->GridPosition : Place) : FString(TEXT("—")),
			ApexUI::Font::Display(22.0f),
			bTop ? ApexUI::Palette::Accent : bHasTime ? ApexUI::Palette::TextSecondary : ApexUI::Palette::TextMuted,
			ColPos));

		UHorizontalBox* NameCell = WidgetTree->ConstructWidget<UHorizontalBox>();
		ApexUI::AddH(NameCell, ApexUI::MakeText(
			*WidgetTree,
			Result->bIsAi ? FString::Printf(TEXT("AI · %s"), *Result->DriverName) : Result->DriverName,
			ApexUI::Font::Display(19.0f),
			RowColour));
		if (bTop)
		{
			ApexUI::AddH(NameCell, MakeTag(*WidgetTree, bPractice ? TEXT("FASTEST") : Advancing > 0 ? TEXT("TOP") : TEXT("POLE"), ApexUI::Palette::Accent),
				FMargin(10.0f, 0.0f, 0.0f, 0.0f));
		}
		// A shoot-out: where the car ran in the order.
		if (const int32 Ran = bShootout ? Net->GetShootoutState().PlaceOf(Line.CarIndex) : 0; Ran > 0)
		{
			ApexUI::AddH(NameCell, MakeTag(*WidgetTree, FString::Printf(TEXT("RAN %s"), *ApexBoard::Ordinal(Ran).ToUpper()), ApexUI::Palette::TextMuted),
				FMargin(10.0f, 0.0f, 0.0f, 0.0f));
		}
		if (bIsLocal)
		{
			ApexUI::AddH(NameCell, MakeTag(*WidgetTree, TEXT("YOU"), ApexUI::Palette::Accent), FMargin(10.0f, 0.0f, 0.0f, 0.0f));
		}
		ApexUI::AddH(Row, ApexUI::MakeSized(*WidgetTree, NameCell, ColQualDriver, -1.0f));

		ApexUI::AddH(Row, MakeCell(*WidgetTree, CarNameOf(*Net, *Flow, *Result).ToUpper(), ApexUI::Font::Mono(10.0f, 60), ApexUI::Palette::TextMuted, ColQualCar));

		ApexUI::AddH(Row, MakeCell(
			*WidgetTree,
			bHasTime ? UApexMenuFlowSubsystem::FormatLapTime(Line.BestMs / 1000.0f) : TEXT("NO TIME"),
			ApexUI::Font::Display(bHasTime ? 24.0f : 18.0f),
			bTop ? ApexUI::Palette::Accent : RowColour,
			ColQualBest));

		ApexUI::AddH(Row, MakeCell(
			*WidgetTree,
			!bHasTime ? FString(TEXT("—")) : bTop ? FString(bPractice ? TEXT("FASTEST") : Advancing > 0 ? TEXT("TOP") : TEXT("POLE")) : FormatSignedMs(Line.GapMs),
			ApexUI::Font::Mono(11.0f),
			bTop ? ApexUI::Palette::Accent : ApexUI::Palette::TextSecondary,
			ColQualGap));
		ApexUI::AddH(Row, MakeCell(
			*WidgetTree,
			bHasTime && !bTop ? FormatSignedMs(Line.IntervalMs) : FString(),
			ApexUI::Font::Mono(11.0f),
			ApexUI::Palette::TextMuted,
			ColQualGap));

		// The sectors the best lap was made of, purple where nobody did better.
		for (int32 Sector = 0; Sector < 3; ++Sector)
		{
			const bool bTimed = Line.SplitsMs.IsValidIndex(Sector) && Line.SplitsMs[Sector] > 0;
			const bool bPurple = bTimed && Line.FastestSplit.IsValidIndex(Sector) && Line.FastestSplit[Sector];
			ApexUI::AddH(Row, MakeCell(
				*WidgetTree,
				bTimed ? FormatSplit(Line.SplitsMs[Sector]) : FString(TEXT("—")),
				ApexUI::Font::Mono(11.0f),
				bPurple ? SectorPurple : ApexUI::Palette::TextMuted,
				ColSector));
		}

		ApexUI::AddH(Row, ApexUI::MakeText(
			*WidgetTree,
			FString::Printf(TEXT("%d/%d"), Result->ValidLaps, Result->LapsCompleted()),
			ApexUI::Font::Mono(12.0f),
			ApexUI::Palette::TextSecondary));

		UBorder* RowPanel = ApexUI::MakePanel(
			*WidgetTree,
			Row,
			FMargin(18.0f, 14.0f),
			ApexUI::MakeBrush(
				Out ? ApexUI::Palette::Background : ApexUI::Palette::Surface,
				bIsLocal ? ApexUI::Palette::Accent : ApexUI::Palette::Border,
				bIsLocal ? 2.0f : 1.0f));
		ApexUI::AddV(TableBox, RowPanel, FMargin(0.0f, 0.0f, 0.0f, 6.0f));
	};

	// The cars still in it (all of them outside a knockout), then the cut.
	int32 Place = 0;
	for (const ApexBoard::FSheetRow& Line : Sheet)
	{
		if (!IsOut(Line))
		{
			AddRow(Line, ++Place);
		}
	}
	TArray<const ApexBoard::FSheetRow*> Cut;
	for (const ApexBoard::FSheetRow& Line : Sheet)
	{
		if (IsOut(Line))
		{
			Cut.Add(&Line);
		}
	}
	if (Cut.Num() > 0)
	{
		Cut.StableSort([&](const ApexBoard::FSheetRow& A, const ApexBoard::FSheetRow& B)
		{
			const FApexKnockedOut* OutA = Knockout->FindEliminated(A.CarIndex);
			const FApexKnockedOut* OutB = Knockout->FindEliminated(B.CarIndex);
			return (OutA ? OutA->GridPosition : MAX_int32) < (OutB ? OutB->GridPosition : MAX_int32);
		});
		ApexUI::AddV(TableBox, ApexUI::MakeText(*WidgetTree,
			FString::Printf(TEXT("KNOCKED OUT IN Q%d · THE TOP %d GO THROUGH"), Knockout->Round, Advancing),
			ApexUI::Font::Mono(10.0f, 120), ApexUI::Palette::Error), FMargin(18.0f, 10.0f, 0.0f, 8.0f));
		for (const ApexBoard::FSheetRow* Line : Cut)
		{
			AddRow(*Line, ++Place);
		}
	}

	// --- Out in an earlier round: where they start, and the time that put them there.
	if (!Knockout)
	{
		return;
	}
	bool bEarlierCaptioned = false;
	for (const FApexKnockedOut& Out : Knockout->Eliminated)
	{
		if (Out.Round >= Knockout->Round)
		{
			continue;
		}
		const FApexCarResult* Result = FindResult(Results, Out.CarIndex);
		if (!bEarlierCaptioned)
		{
			bEarlierCaptioned = true;
			ApexUI::AddV(TableBox, ApexUI::MakeLabel(*WidgetTree, TEXT("Out in an earlier round")), FMargin(18.0f, 16.0f, 0.0f, 8.0f));
		}
		const bool bIsLocal = Result && IsLocalResult(*Net, *Result);
		UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();
		ApexUI::AddH(Row, MakeCell(*WidgetTree, FString::Printf(TEXT("P%d"), Out.GridPosition), ApexUI::Font::Display(18.0f),
			ApexUI::Palette::TextMuted, ColPos));
		UHorizontalBox* NameCell = WidgetTree->ConstructWidget<UHorizontalBox>();
		ApexUI::AddH(NameCell, ApexUI::MakeText(*WidgetTree,
			!Result ? FString::Printf(TEXT("Car %d"), Out.CarIndex)
				: Result->bIsAi ? FString::Printf(TEXT("AI · %s"), *Result->DriverName) : Result->DriverName,
			ApexUI::Font::Display(17.0f), ApexUI::Palette::TextSecondary));
		ApexUI::AddH(NameCell, MakeTag(*WidgetTree, Out.bLate ? FString(TEXT("LATE ENTRY")) : FString::Printf(TEXT("OUT IN Q%d"), Out.Round),
			ApexUI::Palette::TextMuted), FMargin(10.0f, 0.0f, 0.0f, 0.0f));
		if (bIsLocal)
		{
			ApexUI::AddH(NameCell, MakeTag(*WidgetTree, TEXT("YOU"), ApexUI::Palette::Accent), FMargin(10.0f, 0.0f, 0.0f, 0.0f));
		}
		ApexUI::AddH(Row, ApexUI::MakeSized(*WidgetTree, NameCell, ColQualDriver, -1.0f));
		ApexUI::AddH(Row, MakeCell(*WidgetTree, Result ? CarNameOf(*Net, *Flow, *Result).ToUpper() : FString(),
			ApexUI::Font::Mono(10.0f, 60), ApexUI::Palette::TextMuted, ColQualCar));
		ApexUI::AddH(Row, ApexUI::MakeText(*WidgetTree,
			Out.LapTimeMs > 0 ? UApexMenuFlowSubsystem::FormatLapTime(Out.LapTimeMs / 1000.0f) : FString(TEXT("NO TIME")),
			ApexUI::Font::Display(18.0f), ApexUI::Palette::TextSecondary));
		ApexUI::AddV(TableBox, ApexUI::MakePanel(*WidgetTree, Row, FMargin(18.0f, 10.0f),
			ApexUI::MakeBrush(ApexUI::Palette::Background, bIsLocal ? ApexUI::Palette::Accent : ApexUI::Palette::Border, 1.0f)),
			FMargin(0.0f, 0.0f, 0.0f, 4.0f));
	}
}

void UApexSessionResultsWidget::RefreshSidePanel()
{
	UApexSessionRecorder* Recorder = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexSessionRecorder>() : nullptr;
	const UApexNetSubsystem* Net = GetNet();
	if (!Recorder || !Net || !SummaryBox || !LapChartBox)
	{
		return;
	}

	SummaryBox->ClearChildren();
	LapChartBox->ClearChildren();

	const bool bQualifying = IsTimesheet();
	ApexUI::AddV(SummaryBox, ApexUI::MakeLabel(*WidgetTree,
		IsPractice() ? TEXT("Your practice") : bQualifying ? TEXT("Your qualifying") : TEXT("Your session")), FMargin(0.0f, 0.0f, 0.0f, 14.0f));

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

	if (bQualifying)
	{
		FillQualifyingSummary(*Local);
	}
	else
	{
		// --- Best lap, large --------------------------------------------------
		ApexUI::AddV(SummaryBox, ApexUI::MakeLabel(*WidgetTree, TEXT("Best lap")));
		ApexUI::AddV(
			SummaryBox,
			ApexUI::MakeText(
				*WidgetTree,
				Local->BestLapSeconds > 0.0f ? UApexMenuFlowSubsystem::FormatLapTime(Local->BestLapSeconds) : TEXT("NO TIME"),
				ApexUI::Font::Display(52.0f),
				ApexUI::Palette::Accent),
			FMargin(0.0f, 8.0f, 0.0f, 0.0f));
	}

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

	// --- The splits that best lap was made of --------------------------------
	//
	// Timed by the server (`crate::laps`) and carried in `LapTiming`; the
	// client only lays them out. Qualifying lays them out against pole.
	if (!bQualifying && Local->BestLapSplitsSeconds.Num() > 0)
	{
		UHorizontalBox* Splits = WidgetTree->ConstructWidget<UHorizontalBox>();
		for (int32 Index = 0; Index < Local->BestLapSplitsSeconds.Num(); ++Index)
		{
			const float Split = Local->BestLapSplitsSeconds[Index];
			ApexUI::AddH(
				Splits,
				ApexUI::MakeText(
					*WidgetTree,
					FString::Printf(TEXT("S%d %s"), Index + 1,
						Split > 0.0f ? *FString::Printf(TEXT("%.3f"), Split) : TEXT("--.---")),
					ApexUI::Font::Mono(11.0f),
					ApexUI::Palette::TextSecondary),
				FMargin(Index == 0 ? 0.0f : 16.0f, 0.0f, 0.0f, 0.0f));
		}
		ApexUI::AddV(SummaryBox, Splits, FMargin(0.0f, 10.0f, 0.0f, 0.0f));
	}

	// --- The records this lap was measured against ---------------------------
	//
	// Kept by the server across sessions and restarts, so this is the driver's
	// best here ever, not their best today.
	const FApexLapRecord& Record = Net->GetLapRecord();
	if (Record.LapTimeMs > 0)
	{
		ApexUI::AddV(
			SummaryBox,
			ApexUI::MakeText(
				*WidgetTree,
				FString::Printf(TEXT("PERSONAL RECORD %s"),
					*UApexMenuFlowSubsystem::FormatLapTime(Record.LapTimeMs / 1000.0f)),
				ApexUI::Font::Mono(10.0f, 100),
				ApexUI::Palette::TextMuted),
			FMargin(0.0f, 12.0f, 0.0f, 0.0f));
	}
	if (Record.TrackRecordMs > 0 && !Record.TrackRecordHolder.Equals(Record.PlayerName))
	{
		ApexUI::AddV(
			SummaryBox,
			ApexUI::MakeText(
				*WidgetTree,
				FString::Printf(TEXT("TRACK RECORD %s · %s"),
					*UApexMenuFlowSubsystem::FormatLapTime(Record.TrackRecordMs / 1000.0f),
					*Record.TrackRecordHolder.ToUpper()),
				ApexUI::Font::Mono(10.0f, 100),
				ApexUI::Palette::TextMuted),
			FMargin(0.0f, 4.0f, 0.0f, 0.0f));
	}

	// --- Four numbers ---------------------------------------------------------
	UTextBlock* TopSpeed = nullptr;
	UTextBlock* SecondStat = nullptr;
	UTextBlock* ValidLaps = nullptr;
	UTextBlock* Distance = nullptr;

	// Consistency means little over a handful of flying laps; qualifying
	// shows how many it took instead.
	UHorizontalBox* StatRowOne = WidgetTree->ConstructWidget<UHorizontalBox>();
	ApexUI::AddH(StatRowOne, ApexUI::MakeStat(*WidgetTree, TEXT("Top speed"), TopSpeed), FMargin(), VAlign_Fill, 1.0f);
	ApexUI::AddH(StatRowOne, ApexUI::MakeStat(*WidgetTree, bQualifying ? TEXT("Laps") : TEXT("Consistency"), SecondStat), FMargin(10.0f, 0.0f, 0.0f, 0.0f), VAlign_Fill, 1.0f);
	ApexUI::AddV(SummaryBox, StatRowOne, FMargin(0.0f, 22.0f, 0.0f, 0.0f));

	UHorizontalBox* StatRowTwo = WidgetTree->ConstructWidget<UHorizontalBox>();
	ApexUI::AddH(StatRowTwo, ApexUI::MakeStat(*WidgetTree, TEXT("Valid laps"), ValidLaps), FMargin(), VAlign_Fill, 1.0f);
	ApexUI::AddH(StatRowTwo, ApexUI::MakeStat(*WidgetTree, TEXT("Distance"), Distance), FMargin(10.0f, 0.0f, 0.0f, 0.0f), VAlign_Fill, 1.0f);
	ApexUI::AddV(SummaryBox, StatRowTwo, FMargin(0.0f, 10.0f, 0.0f, 0.0f));

	TopSpeed->SetText(FText::FromString(Local->TopSpeedMps > 0.0f
		? FString::Printf(TEXT("%.0f km/h"), Local->TopSpeedMps * 3.6f)
		: TEXT("—")));

	if (bQualifying)
	{
		SecondStat->SetText(FText::AsNumber(Local->LapsCompleted()));
	}
	else
	{
		const float ConsistencyValue = Recorder->GetConsistency();
		SecondStat->SetText(FText::FromString(ConsistencyValue > 0.0f
			? FString::Printf(TEXT("%.0f%%"), ConsistencyValue * 100.0f)
			: TEXT("—")));
	}

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

	// One lap is nothing to chart: its time, and whether it stood.
	if (Local->LapTimes.Num() == 1)
	{
		const bool bValid = !Local->LapValid.IsValidIndex(0) || Local->LapValid[0];
		ApexUI::AddV(LapChartBox, ApexUI::MakeText(
			*WidgetTree,
			FString::Printf(TEXT("LAP 1   %s%s"), *UApexMenuFlowSubsystem::FormatLapTime(Local->LapTimes[0]), bValid ? TEXT("") : TEXT("   INVALID")),
			ApexUI::Font::Mono(13.0f),
			bValid ? ApexUI::Palette::TextPrimary : ApexUI::Palette::Error));
		return;
	}

	float Slowest = 0.0f;
	for (float Lap : Local->LapTimes)
	{
		Slowest = FMath::Max(Slowest, Lap);
	}

	// Bars are relative to the slowest lap, with the best one picked out and a
	// lap struck for track limits in red. Short bar means quick lap, which is
	// the way round people read a lap chart.
	constexpr float LapBarWidth = 26.0f;
	const bool bFillChart = Local->LapTimes.Num() > 12;
	UHorizontalBox* Chart = WidgetTree->ConstructWidget<UHorizontalBox>();
	for (int32 Index = 0; Index < Local->LapTimes.Num(); ++Index)
	{
		const float Lap = Local->LapTimes[Index];
		const bool bValid = !Local->LapValid.IsValidIndex(Index) || Local->LapValid[Index];
		const bool bIsBest = bValid && FMath::IsNearlyEqual(Lap, Local->BestLapSeconds, 0.0005f);
		const float Ratio = Slowest > 0.0f ? Lap / Slowest : 1.0f;

		UVerticalBox* Bar = WidgetTree->ConstructWidget<UVerticalBox>();
		ApexUI::AddV(Bar, WidgetTree->ConstructWidget<USpacer>(), FMargin(), HAlign_Fill, FMath::Max(0.01f, 1.0f - Ratio));
		ApexUI::AddV(
			Bar,
			ApexUI::MakePanel(
				*WidgetTree,
				nullptr,
				FMargin(),
				ApexUI::MakeBrush(bIsBest ? ApexUI::Palette::Accent
					: bValid ? ApexUI::Palette::SurfaceHover
					: ApexUI::Palette::Error.CopyWithNewOpacity(0.55f))),
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

		// A short race's bars keep a bar's width rather than filling the
		// column as blocks; a long one's share it.
		if (bFillChart)
		{
			ApexUI::AddH(Chart, Cell, FMargin(Index == 0 ? 0.0f : 6.0f, 0.0f, 0.0f, 0.0f), VAlign_Fill, 1.0f);
		}
		else
		{
			ApexUI::AddH(Chart, ApexUI::MakeSized(*WidgetTree, Cell, LapBarWidth, -1.0f), FMargin(Index == 0 ? 0.0f : 6.0f, 0.0f, 0.0f, 0.0f), VAlign_Fill);
		}
	}
	ApexUI::AddV(LapChartBox, Chart, FMargin(), bFillChart ? HAlign_Fill : HAlign_Left);
}

void UApexSessionResultsWidget::FillQualifyingSummary(const FApexCarResult& Local)
{
	const UApexSessionRecorder* Recorder = GetGameInstance()->GetSubsystem<UApexSessionRecorder>();
	const TArray<FApexCarResult>& Results = Recorder->GetResults();
	const bool bPractice = IsPractice();
	const FApexWeekendState* Knockout = KnockoutRound();
	const FApexKnockedOut* MyOut = Knockout ? Knockout->FindEliminated(Local.CarIndex) : nullptr;

	// Knocked out in an earlier round: the driver did not run in this one.
	if (MyOut && MyOut->Round < Knockout->Round)
	{
		ApexUI::AddV(SummaryBox, ApexUI::MakeText(*WidgetTree, FString::Printf(TEXT("P%d"), MyOut->GridPosition),
			ApexUI::Font::Display(64.0f), ApexUI::Palette::TextSecondary));
		ApexUI::AddV(SummaryBox, ApexUI::MakeLabel(*WidgetTree, MyOut->bLate
			? FString(TEXT("On the grid · a late entry, from the back"))
			: FString::Printf(TEXT("On the grid · out in Q%d"), MyOut->Round)));
		return;
	}

	TArray<FApexCarResult> InRound;
	for (const FApexCarResult& Result : Results)
	{
		const FApexKnockedOut* Out = Knockout ? Knockout->FindEliminated(Result.CarIndex) : nullptr;
		if (!Out || Out->Round >= Knockout->Round)
		{
			InRound.Add(Result);
		}
	}
	const TArray<ApexBoard::FSheetRow> Sheet = QualifyingSheet(InRound);
	const int32 MineAt = Sheet.IndexOfByPredicate([&Local](const ApexBoard::FSheetRow& Row) { return Row.CarIndex == Local.CarIndex; });
	if (MineAt == INDEX_NONE)
	{
		return;
	}
	const ApexBoard::FSheetRow& Mine = Sheet[MineAt];
	const bool bHasTime = Mine.Position > 0;
	const bool bThrough = Knockout && Knockout->Advancing > 0 && !MyOut && bHasTime && Mine.Position <= Knockout->Advancing;

	// --- Where the driver stands ------------------------------------------------
	// Practice: the place on the timesheet. A knockout round that is not the
	// last: through, or out and where from. Otherwise the grid slot: the
	// server grids the race from this classification, the cars with no time
	// behind in seating order, so the sheet's order is the grid's.
	FString Big = FString::Printf(TEXT("P%d"), MineAt + 1);
	FString Caption = TEXT("On the grid");
	FString Under = bHasTime ? FString::Printf(TEXT("OF %d CARS"), Sheet.Num()) : FString(TEXT("NO TIME · FROM THE BACK"));
	bool bGood = bHasTime;
	if (bPractice)
	{
		Caption = Mine.Position == 1 ? TEXT("Fastest in practice") : TEXT("In practice");
		Under = bHasTime ? FString::Printf(TEXT("OF %d CARS"), Sheet.Num()) : FString(TEXT("NO TIME"));
	}
	else if (MyOut)
	{
		Big = FString::Printf(TEXT("P%d"), MyOut->GridPosition);
		Caption = MyOut->bLate ? FString(TEXT("A late entry")) : FString::Printf(TEXT("Knocked out in Q%d"), MyOut->Round);
		Under = TEXT("YOUR GRID SLOT");
		bGood = false;
	}
	else if (bThrough)
	{
		Caption = FString::Printf(TEXT("Through to Q%d"), Knockout->Round + 1);
		Under = FString::Printf(TEXT("P%d IN Q%d · TOP %d GO THROUGH"), Mine.Position, Knockout->Round, Knockout->Advancing);
	}
	else if (Mine.Position == 1)
	{
		Caption = TEXT("Pole position");
	}

	UHorizontalBox* GridSlot = WidgetTree->ConstructWidget<UHorizontalBox>();
	ApexUI::AddH(GridSlot, ApexUI::MakeText(*WidgetTree, Big, ApexUI::Font::Display(64.0f),
		bGood ? ApexUI::Palette::Accent : ApexUI::Palette::TextSecondary), FMargin(), VAlign_Center);
	UVerticalBox* SlotCaption = WidgetTree->ConstructWidget<UVerticalBox>();
	ApexUI::AddV(SlotCaption, ApexUI::MakeLabel(*WidgetTree, Caption, MyOut ? ApexUI::Palette::Error : ApexUI::Palette::TextMuted));
	ApexUI::AddV(SlotCaption, ApexUI::MakeText(*WidgetTree, Under, ApexUI::Font::Mono(10.0f, 100), ApexUI::Palette::TextMuted),
		FMargin(0.0f, 4.0f, 0.0f, 0.0f));
	ApexUI::AddH(GridSlot, SlotCaption, FMargin(18.0f, 0.0f, 0.0f, 0.0f), VAlign_Center);
	ApexUI::AddV(SummaryBox, GridSlot);

	if (!bHasTime)
	{
		return;
	}

	// --- Best lap and the gap -------------------------------------------------
	const TCHAR* Top = bPractice ? TEXT("THE FASTEST") : Knockout && Knockout->Advancing > 0 ? TEXT("THE TOP") : TEXT("POLE");
	UHorizontalBox* Best = WidgetTree->ConstructWidget<UHorizontalBox>();
	ApexUI::AddH(Best, ApexUI::MakeText(
		*WidgetTree,
		UApexMenuFlowSubsystem::FormatLapTime(Mine.BestMs / 1000.0f),
		ApexUI::Font::Display(32.0f),
		ApexUI::Palette::TextPrimary), FMargin(), VAlign_Bottom);
	ApexUI::AddH(Best, ApexUI::MakeText(
		*WidgetTree,
		Mine.Position == 1
			? (Sheet.IsValidIndex(1) && Sheet[1].Position > 0 ? FString::Printf(TEXT("%s TO P2"), *FormatSignedMs(-Sheet[1].GapMs)) : FString(Top))
			: FString::Printf(TEXT("%s TO %s"), *FormatSignedMs(Mine.GapMs), Top),
		ApexUI::Font::Mono(11.0f, 100),
		Mine.Position == 1 ? ApexUI::Palette::Live : ApexUI::Palette::TextSecondary), FMargin(14.0f, 0.0f, 0.0f, 6.0f), VAlign_Bottom);
	ApexUI::AddV(SummaryBox, Best, FMargin(0.0f, 12.0f, 0.0f, 0.0f));

	// --- Sector by sector, against the reference ------------------------------
	// The top time for everyone else; for the top, the car it beat.
	const ApexBoard::FSheetRow* Reference = Mine.Position == 1
		? (Sheet.IsValidIndex(1) && Sheet[1].Position > 0 ? &Sheet[1] : nullptr)
		: &Sheet[0];
	if (Mine.SplitsMs.Num() > 0)
	{
		ApexUI::AddV(SummaryBox, ApexUI::MakeLabel(*WidgetTree, !Reference ? FString(TEXT("Sectors"))
			: Mine.Position == 1 ? FString(TEXT("Sectors · against P2"))
			: FString::Printf(TEXT("Sectors · against %s"), *FString(Top).ToLower())), FMargin(0.0f, 18.0f, 0.0f, 8.0f));
		for (int32 Sector = 0; Sector < Mine.SplitsMs.Num(); ++Sector)
		{
			const int32 Split = Mine.SplitsMs[Sector];
			const bool bPurple = Mine.FastestSplit.IsValidIndex(Sector) && Mine.FastestSplit[Sector];
			UHorizontalBox* Line = WidgetTree->ConstructWidget<UHorizontalBox>();
			ApexUI::AddH(Line, MakeCell(*WidgetTree, FString::Printf(TEXT("S%d"), Sector + 1), ApexUI::Font::Mono(11.0f, 100), ApexUI::Palette::TextMuted, 44.0f));
			ApexUI::AddH(Line, MakeCell(*WidgetTree, FormatSplit(Split), ApexUI::Font::Mono(13.0f),
				bPurple ? SectorPurple : ApexUI::Palette::TextPrimary, 110.0f));
			if (Reference && Reference->SplitsMs.IsValidIndex(Sector) && Reference->SplitsMs[Sector] > 0 && Split > 0)
			{
				const int32 Diff = Split - Reference->SplitsMs[Sector];
				ApexUI::AddH(Line, ApexUI::MakeText(*WidgetTree, FormatSignedMs(Diff), ApexUI::Font::Mono(12.0f),
					Diff <= 0 ? ApexUI::Palette::Live : ApexUI::Palette::TextSecondary));
			}
			ApexUI::AddV(SummaryBox, Line, FMargin(0.0f, 0.0f, 0.0f, 4.0f));
		}
	}

	// --- What was left out there ---------------------------------------------
	if (Mine.IdealMs > 0 && Mine.IdealMs < Mine.BestMs)
	{
		ApexUI::AddV(SummaryBox, ApexUI::MakeText(
			*WidgetTree,
			FString::Printf(TEXT("IDEAL LAP %s · %.3f IN YOUR BEST SECTORS"),
				*UApexMenuFlowSubsystem::FormatLapTime(Mine.IdealMs / 1000.0f),
				(Mine.BestMs - Mine.IdealMs) / 1000.0f),
			ApexUI::Font::Mono(10.0f, 100),
			ApexUI::Palette::TextSecondary), FMargin(0.0f, 10.0f, 0.0f, 0.0f));
	}
}

void UApexSessionResultsWidget::RefreshPrimaryAction()
{
	if (!DriveAgainButton)
	{
		return;
	}
	const UApexNetSubsystem* Net = GetNet();
	const bool bHost = IsSessionHost();
	// A race weekend moves on at the host's word, to whatever comes next.
	if (Net && Net->IsInSession() && Net->IsWeekendSession() && !NextWeekendSession().IsEmpty())
	{
		const FApexWeekendState& Weekend = Net->GetWeekendState();
		DriveAgainButton->SetLabel(NextWeekendSession());
		DriveAgainButton->SetIsEnabled(bHost && Weekend.bFinished && !IsLive());
		DriveAgainButton->SetBadge(bHost ? FString() : FString(TEXT("Waiting for the host")), ApexUI::Palette::TextMuted);
		return;
	}
	if (!IsTimesheet() || IsPractice() || !Net || !Net->IsInSession())
	{
		DriveAgainButton->SetLabel(TEXT("Drive again"));
		DriveAgainButton->SetIsEnabled(true);
		DriveAgainButton->SetBadge(FString(), ApexUI::Palette::TextMuted);
		return;
	}
	// After qualifying the next thing is the race, and it is the host's to
	// start: the server refuses anyone else's countdown.
	DriveAgainButton->SetLabel(TEXT("Start the race"));
	DriveAgainButton->SetIsEnabled(bHost && !IsLive());
	DriveAgainButton->SetBadge(bHost ? FString() : FString(TEXT("Waiting for the host")), ApexUI::Palette::TextMuted);
}

FString UApexSessionResultsWidget::NextWeekendSession() const
{
	const UApexNetSubsystem* Net = GetNet();
	if (!Net || !Net->GetWeekendState().bValid)
	{
		return FString();
	}
	const FApexWeekendState& Weekend = Net->GetWeekendState();
	switch (Weekend.Phase)
	{
	case EApexWeekendPhase::Practice:
		return Weekend.Rounds > 1 ? TEXT("Start Q1") : TEXT("Start qualifying");
	case EApexWeekendPhase::Qualifying:
		return Weekend.Round < Weekend.Rounds ? FString::Printf(TEXT("Start Q%d"), Weekend.Round + 1) : FString(TEXT("Start the race"));
	default:
		return FString();
	}
}

const FApexWeekendState* UApexSessionResultsWidget::KnockoutRound() const
{
	const UApexNetSubsystem* Net = GetNet();
	if (!Net || IsPractice() || !IsTimesheet())
	{
		return nullptr;
	}
	const FApexWeekendState& Weekend = Net->GetWeekendState();
	return Weekend.bValid && Weekend.Phase == EApexWeekendPhase::Qualifying ? &Weekend : nullptr;
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
		if (IsLive())
		{
			// Starting again now would put everyone still racing back on the grid.
			ShowToast(IsTimesheet() ? TEXT("The session is still running — wait for the flag")
				: TEXT("The race is still running — wait for the field to finish"), true);
			return;
		}
		if (!Net->IsUdpReady())
		{
			ShowToast(TEXT("Still binding the telemetry channel — try again in a moment"), true);
			return;
		}
		// A race weekend's next session is the server's to pick.
		if (Net->IsWeekendSession() && !NextWeekendSession().IsEmpty())
		{
			Net->AdvanceWeekend();
			return;
		}
		// After qualifying: the race, gridded by the server from the result.
		Net->BeginSession(5, IsTimesheet() && !IsPractice() ? EApexGameMode::Race : Flow->CreateStartingMode);
		return;
	}

	if (Id == ActionIncidents)
	{
		ShowIncidents(!bShowingIncidents);
		return;
	}

	if (Id.ToString().StartsWith(IncidentActionPrefix))
	{
		JumpToIncident(FCString::Atoi(*Id.ToString().RightChop(IncidentActionPrefix.Len())));
		return;
	}

	if (Id == ActionSaveReplay)
	{
		UApexReplayRecorder* Replays = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexReplayRecorder>() : nullptr;
		FString Path;
		FString Error;
		bool bWhenFinished = false;
		if (Replays && Replays->KeepThisSession(Path, bWhenFinished, Error))
		{
			ShowToast(bWhenFinished ? FString(TEXT("The replay will be saved when the session ends"))
				: FString::Printf(TEXT("Replay saved: %s"), *FPaths::GetBaseFilename(Path)));
		}
		else
		{
			ShowToast(FString::Printf(TEXT("No replay saved: %s"), Replays ? *Error : TEXT("no recorder")), true);
		}
		RefreshSaveReplay();
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
