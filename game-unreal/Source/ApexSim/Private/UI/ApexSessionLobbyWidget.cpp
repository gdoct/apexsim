#include "UI/ApexSessionLobbyWidget.h"

#include "ApexMenuFlowSubsystem.h"
#include "ApexNetSubsystem.h"
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
#include "UI/ApexButtonWidget.h"
#include "UI/ApexRootWidget.h"
#include "UI/ApexUIStyle.h"

namespace
{
	const FName ActionStart(TEXT("__start"));
	const FName ActionLeave(TEXT("__leave"));
	const FName ActionChangeCar(TEXT("__changecar"));
	const FName ActionCycleMode(TEXT("__cyclemode"));
	const FName ActionCycleCountdown(TEXT("__cyclecountdown"));

	constexpr float SidePanelWidth = 470.0f;

	/**
	 * The modes a host can count a session into, in cycle order.
	 *
	 * DemoLap is deliberately absent: the server removes human players from the
	 * session when it starts one (game_session.rs:409-425), so the host would
	 * strand everyone — including themselves — as spectators with no telemetry.
	 */
	const EApexGameMode StartableModes[] = {
		EApexGameMode::FreePractice,
		EApexGameMode::Race,
		EApexGameMode::Sandbox,
	};

	const int32 CountdownChoices[] = { 3, 5, 10, 20 };
}

UApexSessionLobbyWidget::UApexSessionLobbyWidget(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	SetIsFocusable(true);
}

void UApexSessionLobbyWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	BuildLayout();
}

void UApexSessionLobbyWidget::NativeConstruct()
{
	Super::NativeConstruct();

	if (UApexNetSubsystem* Net = GetNet())
	{
		Net->OnLobbyStateUpdated.AddDynamic(this, &UApexSessionLobbyWidget::HandleLobbyStateUpdated);
		Net->OnSessionRosterUpdated.AddDynamic(this, &UApexSessionLobbyWidget::HandleRosterUpdated);
		Net->OnCountdownUpdate.AddDynamic(this, &UApexSessionLobbyWidget::HandleCountdownUpdate);
		Net->OnGameModeChanged.AddDynamic(this, &UApexSessionLobbyWidget::HandleGameModeChanged);
	}
}

void UApexSessionLobbyWidget::NativeDestruct()
{
	if (UApexNetSubsystem* Net = GetNet())
	{
		Net->OnLobbyStateUpdated.RemoveDynamic(this, &UApexSessionLobbyWidget::HandleLobbyStateUpdated);
		Net->OnSessionRosterUpdated.RemoveDynamic(this, &UApexSessionLobbyWidget::HandleRosterUpdated);
		Net->OnCountdownUpdate.RemoveDynamic(this, &UApexSessionLobbyWidget::HandleCountdownUpdate);
		Net->OnGameModeChanged.RemoveDynamic(this, &UApexSessionLobbyWidget::HandleGameModeChanged);
	}
	Super::NativeDestruct();
}

void UApexSessionLobbyWidget::OnScreenActivated()
{
	Super::OnScreenActivated();

	CountdownRemaining = 0;
	if (const UApexMenuFlowSubsystem* Flow = GetFlow())
	{
		// The lobby starts from the setup the session was created with.
		CountdownSeconds = 10;
	}

	RefreshAll();
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

void UApexSessionLobbyWidget::BuildLayout()
{
	UVerticalBox* Page = WidgetTree->ConstructWidget<UVerticalBox>();
	ApexUI::AddV(Page, BuildHeader());

	GridBox = WidgetTree->ConstructWidget<UVerticalBox>();

	UVerticalBox* GridColumn = WidgetTree->ConstructWidget<UVerticalBox>();
	ApexUI::AddV(GridColumn, ApexUI::MakeLabel(*WidgetTree, TEXT("Grid")), FMargin(0.0f, 0.0f, 0.0f, 14.0f));

	UScrollBox* GridScroll = WidgetTree->ConstructWidget<UScrollBox>();
	GridScroll->AddChild(GridBox);
	ApexUI::AddV(GridColumn, GridScroll, FMargin(), HAlign_Fill, 1.0f);

	UBorder* GridPanel = ApexUI::MakePanel(
		*WidgetTree,
		GridColumn,
		FMargin(ApexUI::Metrics::PageGutter, 24.0f, 30.0f, 24.0f),
		ApexUI::MakeBrush(FLinearColor::Transparent));

	UHorizontalBox* Columns = WidgetTree->ConstructWidget<UHorizontalBox>();
	ApexUI::AddH(Columns, GridPanel, FMargin(), VAlign_Fill, 1.0f);
	ApexUI::AddH(Columns, ApexUI::MakeDivider(*WidgetTree, true), FMargin(), VAlign_Fill);
	ApexUI::AddH(Columns, ApexUI::MakeSized(*WidgetTree, BuildSidePanel(), SidePanelWidth, -1.0f), FMargin(), VAlign_Fill);

	ApexUI::AddV(Page, Columns, FMargin(), HAlign_Fill, 1.0f);

	WidgetTree->RootWidget = ApexUI::MakePanel(
		*WidgetTree,
		Page,
		FMargin(),
		ApexUI::MakeBrush(ApexUI::Palette::Background));
}

UWidget* UApexSessionLobbyWidget::BuildHeader()
{
	UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();

	UBorder* Dot = nullptr;
	ApexUI::AddH(Row, ApexUI::MakeDot(*WidgetTree, ApexUI::Palette::Live, 9.0f, &Dot), FMargin(0.0f, 0.0f, 10.0f, 0.0f));
	StateDot = Dot;

	StateText = ApexUI::MakeText(*WidgetTree, FString(), ApexUI::Font::Mono(10.0f, 140), ApexUI::Palette::Live);
	ApexUI::AddH(Row, StateText);

	ApexUI::AddH(Row, ApexUI::MakeSized(*WidgetTree, ApexUI::MakeDivider(*WidgetTree, true), 1.0f, 22.0f), FMargin(18.0f, 0.0f));

	TrackText = ApexUI::MakeText(*WidgetTree, FString(), ApexUI::Font::Display(22.0f), ApexUI::Palette::TextPrimary);
	ApexUI::AddH(Row, TrackText);

	FormatText = ApexUI::MakeText(*WidgetTree, FString(), ApexUI::Font::Mono(10.0f, 100), ApexUI::Palette::TextMuted);
	ApexUI::AddH(Row, FormatText, FMargin(16.0f, 0.0f, 0.0f, 0.0f));

	ApexUI::AddH(Row, WidgetTree->ConstructWidget<UHorizontalBox>(), FMargin(), VAlign_Center, 1.0f);

	SlotsText = ApexUI::MakeText(*WidgetTree, FString(), ApexUI::Font::Mono(10.0f, 100), ApexUI::Palette::TextMuted);
	ApexUI::AddH(Row, SlotsText);

	UVerticalBox* Stack = WidgetTree->ConstructWidget<UVerticalBox>();
	UBorder* Bar = ApexUI::MakePanel(*WidgetTree, Row, FMargin(ApexUI::Metrics::PageGutter, 0.0f), ApexUI::MakeBrush(ApexUI::Palette::Background));
	Bar->SetVerticalAlignment(VAlign_Center);
	ApexUI::AddV(Stack, ApexUI::MakeSized(*WidgetTree, Bar, -1.0f, ApexUI::Metrics::TopBarHeight));
	ApexUI::AddV(Stack, ApexUI::MakeDivider(*WidgetTree));
	return Stack;
}

UWidget* UApexSessionLobbyWidget::BuildSidePanel()
{
	UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>();

	ApexUI::AddV(Column, ApexUI::MakeLabel(*WidgetTree, TEXT("Your car")), FMargin(0.0f, 0.0f, 0.0f, 12.0f));
	CarPanelBox = WidgetTree->ConstructWidget<UVerticalBox>();
	ApexUI::AddV(Column, CarPanelBox);

	// --- Host controls --------------------------------------------------------
	HostControlsBox = WidgetTree->ConstructWidget<UVerticalBox>();
	ApexUI::AddV(HostControlsBox, ApexUI::MakeLabel(*WidgetTree, TEXT("Host controls")), FMargin(0.0f, 26.0f, 0.0f, 12.0f));

	FApexButtonSpec ModeSpec;
	ModeSpec.Label = TEXT("Mode");
	ModeSpec.Variant = EApexButtonVariant::Panel;
	ModeSpec.LabelSize = 17.0f;
	ModeSpec.Height = 54.0f;
	ModeSpec.ActionId = ActionCycleMode;
	ModeSpec.BadgeColour = ApexUI::Palette::TextSecondary;

	ModeButton = WidgetTree->ConstructWidget<UApexButtonWidget>();
	ModeButton->Setup(ModeSpec);
	ModeButton->OnActivated.AddDynamic(this, &UApexSessionLobbyWidget::HandleButtonActivated);
	ApexUI::AddV(HostControlsBox, ModeButton, FMargin(0.0f, 0.0f, 0.0f, 8.0f));

	FApexButtonSpec CountdownSpec = ModeSpec;
	CountdownSpec.Label = TEXT("Countdown");
	CountdownSpec.ActionId = ActionCycleCountdown;

	CountdownButton = WidgetTree->ConstructWidget<UApexButtonWidget>();
	CountdownButton->Setup(CountdownSpec);
	CountdownButton->OnActivated.AddDynamic(this, &UApexSessionLobbyWidget::HandleButtonActivated);
	ApexUI::AddV(HostControlsBox, CountdownButton);

	ApexUI::AddV(Column, HostControlsBox);
	ApexUI::AddV(Column, WidgetTree->ConstructWidget<USpacer>(), FMargin(), HAlign_Fill, 1.0f);

	NotReadyText = ApexUI::MakeText(*WidgetTree, FString(), ApexUI::Font::Mono(10.0f, 100), ApexUI::Palette::Accent);
	NotReadyText->SetAutoWrapText(true);
	ApexUI::AddV(Column, NotReadyText, FMargin(0.0f, 0.0f, 0.0f, 10.0f));

	FApexButtonSpec StartSpec;
	StartSpec.Label = TEXT("Start countdown");
	StartSpec.KeyCap = TEXT("Enter");
	StartSpec.Variant = EApexButtonVariant::Primary;
	StartSpec.LabelSize = 22.0f;
	StartSpec.Height = 62.0f;
	StartSpec.ActionId = ActionStart;

	StartAction = WidgetTree->ConstructWidget<UApexButtonWidget>();
	StartAction->Setup(StartSpec);
	StartAction->OnActivated.AddDynamic(this, &UApexSessionLobbyWidget::HandleButtonActivated);
	ApexUI::AddV(Column, StartAction, FMargin(0.0f, 0.0f, 0.0f, 8.0f));

	FApexButtonSpec LeaveSpec;
	LeaveSpec.Label = TEXT("Leave session");
	LeaveSpec.Variant = EApexButtonVariant::Ghost;
	LeaveSpec.bCentreLabel = true;
	LeaveSpec.LabelSize = 17.0f;
	LeaveSpec.Height = 52.0f;
	LeaveSpec.ActionId = ActionLeave;

	UApexButtonWidget* LeaveAction = WidgetTree->ConstructWidget<UApexButtonWidget>();
	LeaveAction->Setup(LeaveSpec);
	LeaveAction->OnActivated.AddDynamic(this, &UApexSessionLobbyWidget::HandleButtonActivated);
	ApexUI::AddV(Column, LeaveAction);

	return ApexUI::MakePanel(
		*WidgetTree,
		Column,
		FMargin(28.0f, 24.0f, ApexUI::Metrics::PageGutter, 26.0f),
		ApexUI::MakeBrush(FLinearColor::Transparent));
}

// ---------------------------------------------------------------------------
// Data
// ---------------------------------------------------------------------------

bool UApexSessionLobbyWidget::IsHost() const
{
	const UApexNetSubsystem* Net = GetNet();
	if (!Net)
	{
		return false;
	}

	// The protocol has no host id — SessionSummary carries a host *name*, which
	// is what the server also gave us at auth.
	FApexSessionSummary Session;
	if (!Net->FindSessionById(Net->GetCurrentSessionId(), Session))
	{
		return false;
	}
	return Session.HostName.Equals(Net->GetPlayerName(), ESearchCase::IgnoreCase);
}

void UApexSessionLobbyWidget::RefreshAll()
{
	RefreshHeader();
	RefreshGrid();
	RefreshSidePanel();
}

void UApexSessionLobbyWidget::RefreshHeader()
{
	const UApexNetSubsystem* Net = GetNet();
	const UApexMenuFlowSubsystem* Flow = GetFlow();
	if (!Net || !Flow)
	{
		return;
	}

	FApexSessionSummary Session;
	const bool bHasSession = Net->FindSessionById(Net->GetCurrentSessionId(), Session);

	FString State;
	FLinearColor Colour = ApexUI::Palette::Live;
	if (CountdownRemaining > 0)
	{
		State = FString::Printf(TEXT("STARTING IN %d"), CountdownRemaining);
		Colour = ApexUI::Palette::Accent;
	}
	else if (IsHost())
	{
		State = TEXT("LOBBY · YOU ARE THE HOST");
	}
	else
	{
		State = TEXT("LOBBY · WAITING FOR HOST");
	}

	if (StateText)
	{
		StateText->SetText(FText::FromString(State));
		StateText->SetColorAndOpacity(FSlateColor(Colour));
	}
	ApexUI::SetDotColour(StateDot, Colour);

	if (TrackText)
	{
		FString TrackName = bHasSession ? Session.TrackName : FString();
		if (TrackName.IsEmpty())
		{
			FApexTrackCatalogRow Row;
			if (Flow->GetTrackCatalogRow(Flow->GetPendingTrackId(), Row))
			{
				TrackName = Row.DisplayName;
			}
		}
		TrackText->SetText(FText::FromString(TrackName));
	}

	if (FormatText)
	{
		FormatText->SetText(FText::FromString(FString::Printf(
			TEXT("%s · %d LAPS"),
			*UApexMenuFlowSubsystem::GetGameModeName(Flow->CreateStartingMode).ToUpper(),
			Flow->CreateLapLimit)));
	}

	if (SlotsText)
	{
		const int32 Filled = Net->GetSessionRoster().Entries.Num();
		const int32 Max = bHasSession ? Session.MaxPlayers : Flow->CreateMaxPlayers;
		const int32 Ping = Net->GetPingMs();
		SlotsText->SetText(FText::FromString(Ping >= 0
			? FString::Printf(TEXT("%d / %d SLOTS FILLED · %d MS"), Filled, Max, Ping)
			: FString::Printf(TEXT("%d / %d SLOTS FILLED"), Filled, Max)));
	}
}

void UApexSessionLobbyWidget::RefreshGrid()
{
	const UApexNetSubsystem* Net = GetNet();
	const UApexMenuFlowSubsystem* Flow = GetFlow();
	if (!Net || !Flow || !GridBox)
	{
		return;
	}

	GridBox->ClearChildren();

	FApexSessionSummary Session;
	const bool bHasSession = Net->FindSessionById(Net->GetCurrentSessionId(), Session);
	const int32 MaxSlots = bHasSession ? Session.MaxPlayers : Flow->CreateMaxPlayers;

	TArray<FApexRosterEntry> Entries = Net->GetSessionRoster().Entries;
	Entries.Sort([](const FApexRosterEntry& A, const FApexRosterEntry& B) { return A.CarIndex < B.CarIndex; });

	const FApexLobbyState& Lobby = Net->GetCachedLobbyState();

	for (int32 GridSlot = 0; GridSlot < FMath::Max(MaxSlots, Entries.Num()); ++GridSlot)
	{
		UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();

		const bool bOccupied = Entries.IsValidIndex(GridSlot);
		const FLinearColor RowText = bOccupied ? ApexUI::Palette::TextPrimary : ApexUI::Palette::TextDisabled;

		ApexUI::AddH(
			Row,
			ApexUI::MakeSized(
				*WidgetTree,
				ApexUI::MakeText(*WidgetTree, FString::FromInt(GridSlot + 1), ApexUI::Font::Mono(13.0f), ApexUI::Palette::TextMuted),
				42.0f, -1.0f),
			FMargin(18.0f, 0.0f, 0.0f, 0.0f));

		if (!bOccupied)
		{
			// Nothing in the protocol adds a driver to a session after it is
			// created, so an empty slot is just an empty slot.
			ApexUI::AddH(Row, ApexUI::MakeText(*WidgetTree, TEXT("Open slot"), ApexUI::Font::Display(18.0f), RowText), FMargin(), VAlign_Center, 1.0f);

			UBorder* EmptyPanel = ApexUI::MakePanel(
				*WidgetTree,
				Row,
				FMargin(0.0f, 14.0f),
				ApexUI::MakeBrush(FLinearColor::Transparent, ApexUI::Palette::Border * 0.6f, 1.0f));
			ApexUI::AddV(GridBox, EmptyPanel, FMargin(0.0f, 0.0f, 0.0f, 6.0f));
			continue;
		}

		const FApexRosterEntry& Entry = Entries[GridSlot];
		const bool bIsLocal = Entry.PlayerId.Equals(Net->GetPlayerId(), ESearchCase::IgnoreCase);

		UHorizontalBox* NameRow = WidgetTree->ConstructWidget<UHorizontalBox>();
		ApexUI::AddH(NameRow, ApexUI::MakeText(
			*WidgetTree,
			Entry.bIsAi ? FString::Printf(TEXT("AI · %s"), *Entry.PlayerName) : Entry.PlayerName,
			ApexUI::Font::Display(19.0f),
			RowText));

		if (bIsLocal)
		{
			ApexUI::AddH(NameRow, ApexUI::MakeText(*WidgetTree, TEXT("YOU"), ApexUI::Font::Mono(9.0f, 120), ApexUI::Palette::Accent), FMargin(10.0f, 0.0f, 0.0f, 0.0f));
		}
		if (bHasSession && Session.HostName.Equals(Entry.PlayerName, ESearchCase::IgnoreCase) && !Entry.bIsAi)
		{
			ApexUI::AddH(NameRow, ApexUI::MakeText(*WidgetTree, TEXT("HOST"), ApexUI::Font::Mono(9.0f, 120), ApexUI::Palette::TextSecondary), FMargin(10.0f, 0.0f, 0.0f, 0.0f));
		}

		ApexUI::AddH(Row, ApexUI::MakeSized(*WidgetTree, NameRow, 300.0f, -1.0f), FMargin(), VAlign_Center);

		// Which car, when the lobby state knows.
		FString CarName;
		bool bPickedCar = Entry.bIsAi;
		for (const FApexLobbyPlayer& Player : Lobby.PlayersInLobby)
		{
			if (!Player.Id.Equals(Entry.PlayerId, ESearchCase::IgnoreCase))
			{
				continue;
			}
			bPickedCar = Player.HasSelectedCar();
			if (bPickedCar)
			{
				FApexCarCatalogRow CarRow;
				if (Flow->GetCarCatalogRow(Player.SelectedCar, CarRow))
				{
					CarName = CarRow.DisplayName;
				}
				else
				{
					FApexCarConfigSummary CarSummary;
					if (Net->FindCarById(Player.SelectedCar, CarSummary))
					{
						CarName = CarSummary.Name;
					}
				}
			}
			break;
		}

		ApexUI::AddH(
			Row,
			ApexUI::MakeText(
				*WidgetTree,
				bPickedCar ? CarName.ToUpper() : TEXT("PICKING A CAR…"),
				ApexUI::Font::Mono(10.0f, 60),
				bPickedCar ? ApexUI::Palette::TextSecondary : ApexUI::Palette::TextMuted),
			FMargin(), VAlign_Center, 1.0f);

		ApexUI::AddH(
			Row,
			ApexUI::MakeText(
				*WidgetTree,
				Entry.bIsAi ? TEXT("AI") : (bPickedCar ? TEXT("READY") : TEXT("NOT READY")),
				ApexUI::Font::Mono(10.0f, 120),
				Entry.bIsAi ? ApexUI::Palette::TextMuted : (bPickedCar ? ApexUI::Palette::Live : ApexUI::Palette::Accent)),
			FMargin(0.0f, 0.0f, 18.0f, 0.0f));

		UBorder* RowPanel = ApexUI::MakePanel(
			*WidgetTree,
			Row,
			FMargin(0.0f, 14.0f),
			ApexUI::MakeBrush(
				ApexUI::Palette::Surface,
				bIsLocal ? ApexUI::Palette::Accent : ApexUI::Palette::Border,
				bIsLocal ? 2.0f : 1.0f));
		ApexUI::AddV(GridBox, RowPanel, FMargin(0.0f, 0.0f, 0.0f, 6.0f));
	}
}

void UApexSessionLobbyWidget::RefreshSidePanel()
{
	const UApexNetSubsystem* Net = GetNet();
	UApexMenuFlowSubsystem* Flow = GetFlow();
	if (!Net || !Flow || !CarPanelBox)
	{
		return;
	}

	// --- Your car ------------------------------------------------------------
	CarPanelBox->ClearChildren();
	{
		FApexCarCatalogRow Row;
		const bool bHasRow = Flow->GetCarCatalogRow(Flow->GetPendingCarId(), Row);

		FString Name = bHasRow ? Row.DisplayName : FString();
		if (Name.IsEmpty())
		{
			FApexCarConfigSummary Summary;
			if (Net->FindCarById(Flow->GetPendingCarId(), Summary))
			{
				Name = Summary.Name;
			}
		}

		ApexUI::AddV(
			CarPanelBox,
			ApexUI::MakeSized(*WidgetTree, ApexUI::MakeArtPlaceholder(*WidgetTree, TEXT("Car")), -1.0f, 150.0f));

		UHorizontalBox* NameRow = WidgetTree->ConstructWidget<UHorizontalBox>();
		UTextBlock* NameBlock = ApexUI::MakeText(
			*WidgetTree,
			Name.IsEmpty() ? TEXT("No car selected") : Name,
			ApexUI::Font::Display(22.0f),
			Name.IsEmpty() ? ApexUI::Palette::TextDisabled : ApexUI::Palette::TextPrimary);
		NameBlock->SetAutoWrapText(true);
		ApexUI::AddH(NameRow, NameBlock, FMargin(), VAlign_Center, 1.0f);

		FApexButtonSpec ChangeSpec;
		ChangeSpec.Label = TEXT("Change");
		ChangeSpec.Variant = EApexButtonVariant::Bare;
		ChangeSpec.LabelSize = 15.0f;
		ChangeSpec.LabelColour = ApexUI::Palette::Accent;
		ChangeSpec.ActionId = ActionChangeCar;

		UApexButtonWidget* Change = WidgetTree->ConstructWidget<UApexButtonWidget>();
		Change->Setup(ChangeSpec);
		Change->OnActivated.AddDynamic(this, &UApexSessionLobbyWidget::HandleButtonActivated);
		ApexUI::AddH(NameRow, Change, FMargin(12.0f, 0.0f, 0.0f, 0.0f));

		ApexUI::AddV(CarPanelBox, NameRow, FMargin(0.0f, 16.0f, 0.0f, 0.0f));
	}

	// --- Host controls -------------------------------------------------------
	const bool bHost = IsHost();
	if (HostControlsBox)
	{
		HostControlsBox->SetVisibility(bHost ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
	}

	if (ModeButton)
	{
		ModeButton->SetBadge(UApexMenuFlowSubsystem::GetGameModeName(Flow->CreateStartingMode), ApexUI::Palette::TextSecondary);
	}
	if (CountdownButton)
	{
		CountdownButton->SetBadge(FString::Printf(TEXT("%d s"), CountdownSeconds), ApexUI::Palette::TextSecondary);
	}

	// --- Start ---------------------------------------------------------------
	int32 NotReady = 0;
	for (const FApexRosterEntry& Entry : Net->GetSessionRoster().Entries)
	{
		if (Entry.bIsAi)
		{
			continue;
		}
		for (const FApexLobbyPlayer& Player : Net->GetCachedLobbyState().PlayersInLobby)
		{
			if (Player.Id.Equals(Entry.PlayerId, ESearchCase::IgnoreCase) && !Player.HasSelectedCar())
			{
				++NotReady;
			}
		}
	}

	if (NotReadyText)
	{
		if (CountdownRemaining > 0)
		{
			NotReadyText->SetText(FText::FromString(FString::Printf(TEXT("STARTING IN %d…"), CountdownRemaining)));
		}
		else if (!bHost)
		{
			NotReadyText->SetText(FText::FromString(TEXT("WAITING FOR THE HOST TO START")));
		}
		else if (NotReady > 0)
		{
			NotReadyText->SetText(FText::FromString(FString::Printf(
				TEXT("%d DRIVER%s STILL PICKING A CAR — START ANYWAY?"), NotReady, NotReady == 1 ? TEXT("") : TEXT("S"))));
		}
		else
		{
			NotReadyText->SetText(FText::GetEmpty());
		}
	}

	if (StartAction)
	{
		StartAction->SetIsEnabled(bHost && CountdownRemaining == 0);
		StartAction->SetVisibility(bHost ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
	}
}

// ---------------------------------------------------------------------------
// Interaction
// ---------------------------------------------------------------------------

void UApexSessionLobbyWidget::HandleLobbyStateUpdated(const FApexLobbyState& LobbyState)
{
	RefreshAll();
}

void UApexSessionLobbyWidget::HandleRosterUpdated(const FApexSessionRoster& Roster)
{
	RefreshAll();
}

void UApexSessionLobbyWidget::HandleCountdownUpdate(int32 SecondsRemaining)
{
	CountdownRemaining = SecondsRemaining;
	RefreshHeader();
	RefreshSidePanel();
}

void UApexSessionLobbyWidget::HandleGameModeChanged(EApexGameMode NewMode)
{
	// The shell hands the view to the race director on a driving mode; all this
	// screen has to do is stop claiming a countdown is running.
	CountdownRemaining = 0;
	RefreshAll();
}

void UApexSessionLobbyWidget::HandleButtonActivated(UApexButtonWidget* Button)
{
	UApexNetSubsystem* Net = GetNet();
	UApexMenuFlowSubsystem* Flow = GetFlow();
	if (!Button || !Net || !Flow)
	{
		return;
	}

	const FName Id = Button->GetActionId();

	if (Id == ActionLeave)
	{
		Net->LeaveSession();
		return;
	}

	if (Id == ActionChangeCar)
	{
		if (UApexRootWidget* Root = GetRoot())
		{
			Root->ScreenAfterCarSelect = EApexScreen::SessionLobby;
		}
		ShowScreen(EApexScreen::CarSelect);
		return;
	}

	if (Id == ActionCycleMode)
	{
		int32 Index = 0;
		for (int32 I = 0; I < UE_ARRAY_COUNT(StartableModes); ++I)
		{
			if (StartableModes[I] == Flow->CreateStartingMode)
			{
				Index = I;
				break;
			}
		}
		Flow->CreateStartingMode = StartableModes[(Index + 1) % UE_ARRAY_COUNT(StartableModes)];
		Flow->SaveProfile();
		RefreshHeader();
		RefreshSidePanel();
		return;
	}

	if (Id == ActionCycleCountdown)
	{
		int32 Index = 0;
		for (int32 I = 0; I < UE_ARRAY_COUNT(CountdownChoices); ++I)
		{
			if (CountdownChoices[I] == CountdownSeconds)
			{
				Index = I;
				break;
			}
		}
		CountdownSeconds = CountdownChoices[(Index + 1) % UE_ARRAY_COUNT(CountdownChoices)];
		RefreshSidePanel();
		return;
	}

	if (Id == ActionStart)
	{
		if (!Net->IsUdpReady())
		{
			ShowToast(TEXT("Still binding the telemetry channel — try again in a moment"), true);
			return;
		}

		// StartCountdown, not StartSession: StartSession sets a countdown but
		// stores no mode to transition into, so the session freezes when it
		// expires and can never reach racing.
		UE_LOG(LogApexSim, Log, TEXT("Lobby start: %d s countdown into mode %d"),
			CountdownSeconds, static_cast<int32>(Flow->CreateStartingMode));
		Net->StartCountdown(CountdownSeconds, Flow->CreateStartingMode);
		return;
	}
}
