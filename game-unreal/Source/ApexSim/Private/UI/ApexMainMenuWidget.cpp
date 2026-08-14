#include "UI/ApexMainMenuWidget.h"

#include "ApexMenuFlowSubsystem.h"
#include "ApexNetSubsystem.h"
#include "ApexSim.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/SizeBox.h"
#include "Components/Spacer.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Kismet/KismetSystemLibrary.h"
#include "TimerManager.h"
#include "UI/ApexButtonWidget.h"
#include "UI/ApexRootWidget.h"
#include "UI/ApexUIStyle.h"

namespace
{
	/** Identifies each button in the single activation handler. */
	namespace Action
	{
		const FName Start(TEXT("Start"));
		const FName ChangeSetup(TEXT("ChangeSetup"));
		const FName Browse(TEXT("Browse"));
		const FName Create(TEXT("Create"));
		const FName Garage(TEXT("Garage"));
		const FName Tracks(TEXT("Tracks"));
		const FName Connect(TEXT("Connect"));
	}

	/**
	 * Splits a circuit name so the distinctive half can be drawn bright and the
	 * generic half grey, as in "HOCKENHEIM|RING" and "SILVERSTONE |CIRCUIT".
	 */
	void SplitTitle(const FString& Name, FString& OutHead, FString& OutTail)
	{
		static const TCHAR* GenericSuffixes[] = {
			TEXT("RING"), TEXT("CIRCUIT"), TEXT("RACEWAY"), TEXT("SPEEDWAY"), TEXT("PARK"), TEXT("INTERNATIONAL")
		};

		const FString Upper = Name.ToUpper();
		for (const TCHAR* Suffix : GenericSuffixes)
		{
			const int32 SuffixLen = FCString::Strlen(Suffix);
			// The remaining head has to be long enough to still be a name.
			if (Upper.EndsWith(Suffix) && Upper.Len() > SuffixLen + 2)
			{
				OutHead = Upper.LeftChop(SuffixLen);
				OutTail = Suffix;
				return;
			}
		}

		int32 LastSpace = INDEX_NONE;
		if (Upper.FindLastChar(TEXT(' '), LastSpace) && LastSpace > 0)
		{
			OutHead = Upper.Left(LastSpace + 1);
			OutTail = Upper.Mid(LastSpace + 1);
			return;
		}

		OutHead = Upper;
		OutTail.Reset();
	}

	/** Project version from DefaultGame.ini, read directly to avoid an EngineSettings dependency. */
	FString GetClientVersion()
	{
		FString Version;
		GConfig->GetString(TEXT("/Script/EngineSettings.GeneralProjectSettings"), TEXT("ProjectVersion"), Version, GGameIni);
		return Version;
	}

	/** A labelled value box — the CAR and SESSION tiles under the hero title. */
	UWidget* MakeTile(UWidgetTree& Tree, const FString& Label, UTextBlock*& OutValue)
	{
		UVerticalBox* Box = Tree.ConstructWidget<UVerticalBox>();
		ApexUI::AddV(Box, ApexUI::MakeLabel(Tree, Label));

		OutValue = ApexUI::MakeText(Tree, FString(), ApexUI::Font::Display(21.0f), ApexUI::Palette::TextPrimary);
		ApexUI::AddV(Box, OutValue, FMargin(0.0f, 9.0f, 0.0f, 0.0f));

		return ApexUI::MakePanel(
			Tree,
			Box,
			FMargin(20.0f, 15.0f),
			ApexUI::MakeBrush(ApexUI::Palette::Surface, ApexUI::Palette::Border, 1.0f));
	}
}

UApexMainMenuWidget::UApexMainMenuWidget(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// Needed to receive the arrow/Tab/Escape keys the footer advertises.
	SetIsFocusable(true);
}

void UApexMainMenuWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	BuildLayout();
}

void UApexMainMenuWidget::NativeConstruct()
{
	Super::NativeConstruct();

	if (UApexNetSubsystem* Net = GetNet())
	{
		Net->OnConnectionStateChanged.AddDynamic(this, &UApexMainMenuWidget::HandleConnectionStateChanged);
		Net->OnLobbyStateUpdated.AddDynamic(this, &UApexMainMenuWidget::HandleLobbyStateUpdated);
	}
	if (UApexMenuFlowSubsystem* Flow = GetFlow())
	{
		Flow->OnPendingCarChanged.AddDynamic(this, &UApexMainMenuWidget::HandlePendingCarChanged);
		Flow->OnPendingTrackChanged.AddDynamic(this, &UApexMainMenuWidget::HandlePendingTrackChanged);
	}

	RefreshAll();
}

void UApexMainMenuWidget::NativeDestruct()
{
	if (UApexNetSubsystem* Net = GetNet())
	{
		Net->OnConnectionStateChanged.RemoveDynamic(this, &UApexMainMenuWidget::HandleConnectionStateChanged);
		Net->OnLobbyStateUpdated.RemoveDynamic(this, &UApexMainMenuWidget::HandleLobbyStateUpdated);
	}
	if (UApexMenuFlowSubsystem* Flow = GetFlow())
	{
		Flow->OnPendingCarChanged.RemoveDynamic(this, &UApexMainMenuWidget::HandlePendingCarChanged);
		Flow->OnPendingTrackChanged.RemoveDynamic(this, &UApexMainMenuWidget::HandlePendingTrackChanged);
	}

	Super::NativeDestruct();
}

void UApexMainMenuWidget::OnScreenActivated()
{
	Super::OnScreenActivated();

	RefreshAll();

	// Not this frame: on the first activation the shell is still being added to
	// the viewport, and the game mode's SetInputMode runs immediately after and
	// hands focus to the viewport widget. A tick later the tree is live and the
	// focus sticks.
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().SetTimerForNextTick(
			FTimerDelegate::CreateWeakLambda(this, [this]() { ApplyFocus(); }));
	}
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

void UApexMainMenuWidget::BuildLayout()
{
	UVerticalBox* Page = WidgetTree->ConstructWidget<UVerticalBox>();

	ApexUI::AddV(Page, BuildTopBar());

	// Hairline under the bar. A one-pixel size box is cheaper than a border with
	// asymmetric outline settings and reads identically.
	ApexUI::AddV(Page, ApexUI::MakeSized(
		*WidgetTree,
		ApexUI::MakePanel(*WidgetTree, nullptr, FMargin(), ApexUI::MakeBrush(ApexUI::Palette::Border)),
		-1.0f, 1.0f));

	UHorizontalBox* Columns = WidgetTree->ConstructWidget<UHorizontalBox>();
	ApexUI::AddH(Columns, BuildHero(), FMargin(), VAlign_Fill, 1.0f);
	ApexUI::AddH(Columns, ApexUI::MakeSized(*WidgetTree, BuildRail(), ApexUI::Metrics::RailWidth, -1.0f), FMargin(56.0f, 0.0f, 0.0f, 0.0f), VAlign_Fill);

	UBorder* Content = ApexUI::MakePanel(
		*WidgetTree,
		Columns,
		FMargin(ApexUI::Metrics::PageGutter, 96.0f, ApexUI::Metrics::PageGutter, 24.0f),
		ApexUI::MakeBrush(FLinearColor::Transparent));
	ApexUI::AddV(Page, Content, FMargin(), HAlign_Fill, 1.0f);

	ApexUI::AddV(Page, ApexUI::MakeKeyHintBar(
		*WidgetTree,
		{
			{ TEXT("↑ ↓"), TEXT("Navigate") },
			{ TEXT("Enter"), TEXT("Select") },
			{ TEXT("Tab"), TEXT("Switch column") },
		},
		{
			{ TEXT("Esc"), TEXT("Quit") },
		}));

	WidgetTree->RootWidget = ApexUI::MakePanel(
		*WidgetTree,
		Page,
		FMargin(),
		ApexUI::MakeBrush(ApexUI::Palette::Background));
}

UWidget* UApexMainMenuWidget::BuildTopBar()
{
	UHorizontalBox* Bar = WidgetTree->ConstructWidget<UHorizontalBox>();

	ApexUI::AddH(Bar, ApexUI::MakeText(*WidgetTree, TEXT("APEXSIM"), ApexUI::Font::Display(23.0f, 90), ApexUI::Palette::TextPrimary));
	ApexUI::AddH(Bar, ApexUI::MakeText(*WidgetTree, TEXT("/"), ApexUI::Font::Display(23.0f), ApexUI::Palette::TextDisabled), FMargin(12.0f, 0.0f));

	const FString Version = GetClientVersion();
	if (!Version.IsEmpty())
	{
		ApexUI::AddH(Bar, ApexUI::MakeText(*WidgetTree, Version, ApexUI::Font::Mono(11.0f), ApexUI::Palette::TextMuted));
	}

	ApexUI::AddH(Bar, WidgetTree->ConstructWidget<UHorizontalBox>(), FMargin(), VAlign_Center, 1.0f);

	UBorder* Dot = nullptr;
	UWidget* DotBox = ApexUI::MakeDot(*WidgetTree, ApexUI::Palette::TextMuted, 9.0f, &Dot);
	ConnectionDot = Dot;
	ApexUI::AddH(Bar, DotBox, FMargin(0.0f, 0.0f, 9.0f, 0.0f));

	ServerText = ApexUI::MakeText(*WidgetTree, FString(), ApexUI::Font::Mono(11.0f), ApexUI::Palette::TextSecondary);
	ApexUI::AddH(Bar, ServerText);

	PingText = ApexUI::MakeText(*WidgetTree, FString(), ApexUI::Font::Mono(11.0f), ApexUI::Palette::TextMuted);
	ApexUI::AddH(Bar, PingText, FMargin(18.0f, 0.0f, 0.0f, 0.0f));

	// Placeholder for the driver's avatar; there is no account system to fill it.
	ApexUI::AddH(Bar,
		ApexUI::MakeSized(*WidgetTree,
			ApexUI::MakePanel(*WidgetTree, nullptr, FMargin(), ApexUI::MakeBrush(ApexUI::Palette::SurfaceHover)),
			22.0f, 22.0f),
		FMargin(24.0f, 0.0f, 10.0f, 0.0f));

	DriverText = ApexUI::MakeText(*WidgetTree, FString(), ApexUI::Font::Body(13.0f), ApexUI::Palette::TextPrimary);
	ApexUI::AddH(Bar, DriverText);

	UBorder* Panel = ApexUI::MakePanel(
		*WidgetTree,
		Bar,
		FMargin(ApexUI::Metrics::PageGutter, 0.0f),
		ApexUI::MakeBrush(ApexUI::Palette::Background));
	Panel->SetVerticalAlignment(VAlign_Center);

	return ApexUI::MakeSized(*WidgetTree, Panel, -1.0f, ApexUI::Metrics::TopBarHeight);
}

UWidget* UApexMainMenuWidget::BuildHero()
{
	UVerticalBox* Hero = WidgetTree->ConstructWidget<UVerticalBox>();

	EyebrowText = ApexUI::MakeText(*WidgetTree, FString(), ApexUI::Font::Mono(11.0f, 200), ApexUI::Palette::Accent);
	ApexUI::AddV(Hero, EyebrowText, FMargin(0.0f, 0.0f, 0.0f, 14.0f), HAlign_Left);

	UHorizontalBox* Title = WidgetTree->ConstructWidget<UHorizontalBox>();
	TitleHeadText = ApexUI::MakeText(*WidgetTree, FString(), ApexUI::Font::Display(66.0f), ApexUI::Palette::TextPrimary);
	TitleTailText = ApexUI::MakeText(*WidgetTree, FString(), ApexUI::Font::Display(66.0f), ApexUI::Palette::TextDisabled);
	ApexUI::AddH(Title, TitleHeadText, FMargin(), VAlign_Bottom);
	ApexUI::AddH(Title, TitleTailText, FMargin(), VAlign_Bottom);
	ApexUI::AddV(Hero, Title, FMargin(0.0f, 0.0f, 0.0f, 10.0f), HAlign_Left);

	MetaText = ApexUI::MakeText(*WidgetTree, FString(), ApexUI::Font::Mono(12.0f, 60), ApexUI::Palette::TextSecondary);
	ApexUI::AddV(Hero, MetaText, FMargin(0.0f, 0.0f, 0.0f, 36.0f), HAlign_Left);

	UHorizontalBox* Tiles = WidgetTree->ConstructWidget<UHorizontalBox>();
	UTextBlock* CarValue = nullptr;
	UTextBlock* SessionValue = nullptr;
	ApexUI::AddH(Tiles, MakeTile(*WidgetTree, TEXT("Car"), CarValue), FMargin(), VAlign_Fill, 1.0f);
	ApexUI::AddH(Tiles, MakeTile(*WidgetTree, TEXT("Session"), SessionValue), FMargin(18.0f, 0.0f, 0.0f, 0.0f), VAlign_Fill, 1.0f);
	CarValueText = CarValue;
	SessionValueText = SessionValue;
	ApexUI::AddV(Hero, ApexUI::MakeSized(*WidgetTree, Tiles, 840.0f, -1.0f), FMargin(0.0f, 0.0f, 0.0f, 26.0f), HAlign_Left);

	UHorizontalBox* Actions = WidgetTree->ConstructWidget<UHorizontalBox>();

	FApexButtonSpec StartSpec;
	StartSpec.Label = TEXT("Start session");
	StartSpec.KeyCap = TEXT("Enter");
	StartSpec.Variant = EApexButtonVariant::Primary;
	StartSpec.ActionId = Action::Start;
	StartSpec.bCentreLabel = false;

	StartButton = WidgetTree->ConstructWidget<UApexButtonWidget>();
	StartButton->Setup(StartSpec);
	StartButton->OnActivated.AddDynamic(this, &UApexMainMenuWidget::HandleButtonActivated);
	HeroButtons.Add(StartButton);
	ApexUI::AddH(Actions, ApexUI::MakeSized(*WidgetTree, StartButton, 470.0f, -1.0f));

	FApexButtonSpec SetupSpec;
	SetupSpec.Label = TEXT("Change setup");
	SetupSpec.Variant = EApexButtonVariant::Ghost;
	SetupSpec.ActionId = Action::ChangeSetup;
	SetupSpec.bCentreLabel = true;
	SetupSpec.Height = 78.0f;

	UApexButtonWidget* SetupButton = WidgetTree->ConstructWidget<UApexButtonWidget>();
	SetupButton->Setup(SetupSpec);
	SetupButton->OnActivated.AddDynamic(this, &UApexMainMenuWidget::HandleButtonActivated);
	HeroButtons.Add(SetupButton);
	ApexUI::AddH(Actions, ApexUI::MakeSized(*WidgetTree, SetupButton, 230.0f, -1.0f), FMargin(18.0f, 0.0f, 0.0f, 0.0f));

	ApexUI::AddV(Hero, Actions, FMargin(), HAlign_Left);

	// Pushes everything above to the top of the column.
	ApexUI::AddV(Hero, WidgetTree->ConstructWidget<USpacer>(), FMargin(), HAlign_Fill, 1.0f);

	return Hero;
}

UWidget* UApexMainMenuWidget::BuildRail()
{
	UVerticalBox* Rail = WidgetTree->ConstructWidget<UVerticalBox>();

	ApexUI::AddV(Rail, ApexUI::MakeLabel(*WidgetTree, TEXT("Elsewhere")), FMargin(0.0f, 0.0f, 0.0f, 14.0f));

	FApexButtonSpec Spec;
	Spec.Variant = EApexButtonVariant::Panel;

	Spec.Label = TEXT("Browse sessions"); Spec.ActionId = Action::Browse;  AddColumnButton(Rail, Spec, true);
	Spec.Label = TEXT("Create session");  Spec.ActionId = Action::Create;  AddColumnButton(Rail, Spec, true);
	Spec.Label = TEXT("Garage");          Spec.ActionId = Action::Garage;  AddColumnButton(Rail, Spec, true);
	Spec.Label = TEXT("Tracks");          Spec.ActionId = Action::Tracks;  AddColumnButton(Rail, Spec, true);
	Spec.Label = TEXT("Connect to server");Spec.ActionId = Action::Connect; AddColumnButton(Rail, Spec, true);

	ApexUI::AddV(Rail, ApexUI::MakeLabel(*WidgetTree, TEXT("In development")), FMargin(0.0f, 30.0f, 0.0f, 14.0f));

	// Announced but not built. Kept in the rail so the shape of the finished
	// product is visible; they take no focus and do nothing.
	FApexButtonSpec LockedSpec;
	LockedSpec.Variant = EApexButtonVariant::Locked;
	LockedSpec.Badge = TEXT("Locked");
	for (const TCHAR* Label : { TEXT("Race weekend"), TEXT("Qualifying"), TEXT("Replay") })
	{
		LockedSpec.Label = Label;
		AddColumnButton(Rail, LockedSpec, true);
	}

	ApexUI::AddV(Rail, WidgetTree->ConstructWidget<USpacer>(), FMargin(), HAlign_Fill, 1.0f);

	return Rail;
}

UApexButtonWidget* UApexMainMenuWidget::AddColumnButton(UVerticalBox* Column, const FApexButtonSpec& Spec, bool bIsRail)
{
	UApexButtonWidget* Button = WidgetTree->ConstructWidget<UApexButtonWidget>();
	Button->Setup(Spec);
	Button->OnActivated.AddDynamic(this, &UApexMainMenuWidget::HandleButtonActivated);

	ApexUI::AddV(Column, Button, FMargin(0.0f, 0.0f, 0.0f, 6.0f));

	if (bIsRail)
	{
		RailButtons.Add(Button);
	}
	else
	{
		HeroButtons.Add(Button);
	}
	return Button;
}

// ---------------------------------------------------------------------------
// Data
// ---------------------------------------------------------------------------

void UApexMainMenuWidget::RefreshAll()
{
	RefreshHeader();
	RefreshHero();
	RefreshRail();
}

void UApexMainMenuWidget::RefreshHeader()
{
	const UApexNetSubsystem* Net = GetNet();
	const UApexMenuFlowSubsystem* Flow = GetFlow();
	if (!Net || !Flow)
	{
		return;
	}

	FLinearColor DotColour = ApexUI::Palette::TextMuted;
	switch (Net->GetConnectionState())
	{
	case EApexConnectionState::Authenticated:                                     DotColour = ApexUI::Palette::Live;   break;
	case EApexConnectionState::Connecting:
	case EApexConnectionState::Authenticating:                                    DotColour = ApexUI::Palette::Accent; break;
	case EApexConnectionState::Failed:                                            DotColour = ApexUI::Palette::Error;  break;
	default:                                                                      break;
	}

	ApexUI::SetDotColour(ConnectionDot, DotColour);

	if (ServerText)
	{
		ServerText->SetText(FText::FromString(FString::Printf(TEXT("%s:%d"), *Flow->ServerHost, Flow->ServerPort)));
	}

	if (PingText)
	{
		const int32 Ping = Net->GetPingMs();
		PingText->SetText(FText::FromString(Ping >= 0 ? FString::Printf(TEXT("%d ms"), Ping) : TEXT("— ms")));
	}

	if (DriverText)
	{
		DriverText->SetText(FText::FromString(Flow->PlayerName));
	}
}

void UApexMainMenuWidget::RefreshHero()
{
	UApexMenuFlowSubsystem* Flow = GetFlow();
	const UApexNetSubsystem* Net = GetNet();
	if (!Flow)
	{
		return;
	}

	const FString TrackId = Flow->GetPendingTrackId();

	FString TrackName;
	FApexTrackCatalogRow TrackRow;
	const bool bHasTrackRow = Flow->GetTrackCatalogRow(TrackId, TrackRow);
	if (bHasTrackRow)
	{
		TrackName = TrackRow.DisplayName;
	}
	if (TrackName.IsEmpty() && Net)
	{
		// No catalog row: the server's name is better than nothing.
		FApexTrackConfigSummary Summary;
		if (Net->FindTrackById(TrackId, Summary))
		{
			TrackName = Summary.Name;
		}
	}

	const bool bHasTrack = !TrackName.IsEmpty();

	if (EyebrowText)
	{
		EyebrowText->SetText(FText::FromString(bHasTrack
			? TEXT("Continue where you left off")
			: TEXT("Nothing selected yet")));
	}

	if (TitleHeadText && TitleTailText)
	{
		FString Head;
		FString Tail;
		SplitTitle(bHasTrack ? TrackName : TEXT("Choose a track"), Head, Tail);

		// Circuit names run from "MONZA" to "AUTODROMO NAZIONALE DI MONZA"; step
		// the size down so the long ones stay inside the hero column.
		const int32 Length = Head.Len() + Tail.Len();
		const float TitleSize = Length > 26 ? 46.0f : (Length > 18 ? 56.0f : 66.0f);

		TitleHeadText->SetText(FText::FromString(Head));
		TitleHeadText->SetFont(ApexUI::Font::Display(TitleSize));
		TitleHeadText->SetColorAndOpacity(FSlateColor(bHasTrack ? ApexUI::Palette::TextPrimary : ApexUI::Palette::TextDisabled));

		TitleTailText->SetText(FText::FromString(Tail));
		TitleTailText->SetFont(ApexUI::Font::Display(TitleSize));
	}

	if (MetaText)
	{
		TArray<FString> Parts;
		if (bHasTrackRow)
		{
			if (!TrackRow.Country.IsEmpty())
			{
				Parts.Add(TrackRow.Country.ToUpper());
			}
			if (TrackRow.LengthM > 0.0f)
			{
				Parts.Add(FString::Printf(TEXT("%.2f km"), TrackRow.LengthM / 1000.0f));
			}
			if (!TrackRow.Category.IsEmpty())
			{
				Parts.Add(TrackRow.Category.ToUpper());
			}
		}

		float BestSeconds = 0.0f;
		if (bHasTrack && Flow->GetBestLapSeconds(TrackId, BestSeconds))
		{
			Parts.Add(FString::Printf(TEXT("BEST %s"), *UApexMenuFlowSubsystem::FormatLapTime(BestSeconds)));
		}
		else if (bHasTrack)
		{
			Parts.Add(TEXT("NEVER DRIVEN"));
		}

		MetaText->SetText(FText::FromString(FString::Join(Parts, TEXT("   /   "))));
	}

	if (CarValueText)
	{
		FString CarName;
		FApexCarCatalogRow CarRow;
		if (Flow->GetCarCatalogRow(Flow->GetPendingCarId(), CarRow))
		{
			CarName = CarRow.DisplayName;
		}
		if (CarName.IsEmpty() && Net)
		{
			FApexCarConfigSummary Summary;
			if (Net->FindCarById(Flow->GetPendingCarId(), Summary))
			{
				CarName = Summary.Name;
			}
		}

		const bool bHasCar = !CarName.IsEmpty();
		CarValueText->SetText(FText::FromString(bHasCar ? CarName : TEXT("No car selected")));
		CarValueText->SetColorAndOpacity(FSlateColor(bHasCar ? ApexUI::Palette::TextPrimary : ApexUI::Palette::TextDisabled));
	}

	if (SessionValueText)
	{
		SessionValueText->SetText(FText::FromString(FString::Printf(
			TEXT("%s · %d laps · %d AI"),
			*UApexMenuFlowSubsystem::GetGameModeName(Flow->CreateStartingMode),
			Flow->CreateLapLimit,
			Flow->CreateAiCount)));
	}

	if (StartButton)
	{
		// With no track there is nothing to start, so the primary action becomes
		// the step that is actually missing rather than a dead button.
		StartButton->SetLabel(bHasTrack ? TEXT("Start session") : TEXT("Select a track"));
		StartButton->SetActionId(bHasTrack ? Action::Start : Action::Tracks);
	}
}

void UApexMainMenuWidget::RefreshRail()
{
	const UApexNetSubsystem* Net = GetNet();
	const UApexMenuFlowSubsystem* Flow = GetFlow();
	if (!Net || !Flow)
	{
		return;
	}

	const FApexLobbyState& Lobby = Net->GetCachedLobbyState();
	const bool bOnline = Net->IsAuthenticated();

	for (UApexButtonWidget* Button : RailButtons)
	{
		if (!Button)
		{
			continue;
		}

		const FName Id = Button->GetActionId();
		if (Id == Action::Browse)
		{
			const int32 Live = Lobby.AvailableSessions.Num();
			Button->SetBadge(
				bOnline ? (Live > 0 ? FString::Printf(TEXT("%d live"), Live) : TEXT("none")) : TEXT("offline"),
				Live > 0 && bOnline ? ApexUI::Palette::Live : ApexUI::Palette::TextMuted);
		}
		else if (Id == Action::Create)
		{
			Button->SetBadge(TEXT("Host"), ApexUI::Palette::TextMuted);
		}
		else if (Id == Action::Garage)
		{
			Button->SetBadge(
				bOnline ? FString::Printf(TEXT("%d cars"), Lobby.CarConfigs.Num()) : TEXT("offline"),
				ApexUI::Palette::TextMuted);
		}
		else if (Id == Action::Tracks)
		{
			Button->SetBadge(
				bOnline ? FString::FromInt(Lobby.TrackConfigs.Num()) : TEXT("offline"),
				ApexUI::Palette::TextMuted);
		}
		else if (Id == Action::Connect)
		{
			Button->SetBadge(Flow->ServerHost, ApexUI::Palette::TextMuted);
		}
	}
}

// ---------------------------------------------------------------------------
// Interaction
// ---------------------------------------------------------------------------

void UApexMainMenuWidget::HandleButtonActivated(UApexButtonWidget* Button)
{
	if (!Button)
	{
		return;
	}

	const FName Id = Button->GetActionId();
	UApexRootWidget* Root = GetRoot();

	if (Id == Action::Start)
	{
		StartRememberedSession();
	}
	else if (Id == Action::ChangeSetup)
	{
		ShowScreen(EApexScreen::SessionCreate);
	}
	else if (Id == Action::Browse)
	{
		ShowScreen(EApexScreen::SessionBrowser);
	}
	else if (Id == Action::Create)
	{
		ShowScreen(EApexScreen::SessionCreate);
	}
	else if (Id == Action::Garage)
	{
		if (Root)
		{
			Root->ScreenAfterCarSelect = EApexScreen::MainMenu;
		}
		ShowScreen(EApexScreen::CarSelect);
	}
	else if (Id == Action::Tracks)
	{
		if (Root)
		{
			Root->ScreenAfterTrackSelect = EApexScreen::MainMenu;
		}
		ShowScreen(EApexScreen::TrackSelect);
	}
	else if (Id == Action::Connect)
	{
		ShowScreen(EApexScreen::ConnectDialog);
	}
}

void UApexMainMenuWidget::StartRememberedSession()
{
	UApexNetSubsystem* Net = GetNet();
	UApexMenuFlowSubsystem* Flow = GetFlow();
	if (!Net || !Flow)
	{
		return;
	}

	if (!Net->IsAuthenticated())
	{
		ShowToast(TEXT("Not connected to a server yet"), true);
		ShowScreen(EApexScreen::ConnectDialog);
		return;
	}

	if (!Flow->HasPendingTrack())
	{
		ShowScreen(EApexScreen::TrackSelect);
		return;
	}

	if (Flow->HasPendingCar())
	{
		// SelectCar before CreateSession, the order the rest of the shell uses.
		Net->SelectCar(Flow->GetPendingCarId());
	}

	// The shell counts the session in on join rather than parking in the lobby:
	// this button promises a session, not a waiting room.
	Flow->bAutoStartOnJoin = true;
	Flow->AutoStartMode = Flow->CreateStartingMode;

	UE_LOG(LogApexSim, Log, TEXT("Main menu start: track '%s', %d AI, %d laps, mode %d"),
		*Flow->GetPendingTrackId(), Flow->CreateAiCount, Flow->CreateLapLimit,
		static_cast<int32>(Flow->CreateStartingMode));

	Net->CreateSession(
		Flow->GetPendingTrackId(),
		Flow->CreateMaxPlayers,
		Flow->CreateAiCount,
		Flow->CreateLapLimit,
		Flow->CreateSessionKind);
}

void UApexMainMenuWidget::HandleConnectionStateChanged(EApexConnectionState NewState, const FString& Detail)
{
	RefreshAll();

	// A failed auto-connect drops the user straight onto the connect dialog so
	// they can fix the address rather than staring at a dead menu.
	if (NewState == EApexConnectionState::Failed && GetRoot() && GetRoot()->GetCurrentScreen() == EApexScreen::MainMenu)
	{
		ShowScreen(EApexScreen::ConnectDialog);
	}
}

void UApexMainMenuWidget::HandleLobbyStateUpdated(const FApexLobbyState& LobbyState)
{
	if (UApexMenuFlowSubsystem* Flow = GetFlow())
	{
		Flow->ReportUnmatchedCatalogIds(LobbyState);
	}
	RefreshAll();
}

void UApexMainMenuWidget::HandlePendingCarChanged(const FString& CarId)
{
	RefreshHero();
}

void UApexMainMenuWidget::HandlePendingTrackChanged(const FString& TrackId)
{
	RefreshHero();
}

// ---------------------------------------------------------------------------
// Keyboard
// ---------------------------------------------------------------------------

FReply UApexMainMenuWidget::NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent)
{
	const FKey Key = InKeyEvent.GetKey();

	if (Key == EKeys::Up)
	{
		MoveSelection(-1);
		return FReply::Handled();
	}
	if (Key == EKeys::Down)
	{
		MoveSelection(1);
		return FReply::Handled();
	}
	if (Key == EKeys::Tab)
	{
		SwitchColumn();
		return FReply::Handled();
	}
	if (Key == EKeys::Escape)
	{
		// The footer promises Escape quits here. Elsewhere the root widget takes
		// it as "back", but the main menu is the bottom of the stack.
		if (UApexNetSubsystem* Net = GetNet())
		{
			Net->Disconnect();
		}
		UKismetSystemLibrary::QuitGame(this, nullptr, EQuitPreference::Quit, false);
		return FReply::Handled();
	}

	return Super::NativeOnKeyDown(InGeometry, InKeyEvent);
}

void UApexMainMenuWidget::MoveSelection(int32 Delta)
{
	TArray<TObjectPtr<UApexButtonWidget>>& Column = ActiveColumn == 0 ? HeroButtons : RailButtons;
	if (Column.Num() == 0)
	{
		return;
	}

	int32& Index = ActiveColumn == 0 ? HeroIndex : RailIndex;

	// Wrap, and step over anything that cannot be activated (the locked rows).
	for (int32 Attempt = 0; Attempt < Column.Num(); ++Attempt)
	{
		Index = (Index + Delta + Column.Num()) % Column.Num();
		if (Column[Index] && Column[Index]->IsInteractive())
		{
			break;
		}
	}

	ApplyFocus();
}

void UApexMainMenuWidget::SwitchColumn()
{
	ActiveColumn = ActiveColumn == 0 ? 1 : 0;
	ApplyFocus();
}

void UApexMainMenuWidget::ApplyFocus()
{
	const TArray<TObjectPtr<UApexButtonWidget>>& Column = ActiveColumn == 0 ? HeroButtons : RailButtons;
	const int32 Index = ActiveColumn == 0 ? HeroIndex : RailIndex;

	if (Column.IsValidIndex(Index) && Column[Index])
	{
		Column[Index]->SetKeyboardFocus();
		UE_LOG(LogApexSim, Verbose, TEXT("Main menu focus -> column %d index %d (%s), took focus: %s"),
			ActiveColumn, Index, *Column[Index]->GetActionId().ToString(),
			Column[Index]->HasKeyboardFocus() ? TEXT("yes") : TEXT("no"));
	}
	else
	{
		// Nothing to focus — keep the keys coming to this screen anyway.
		SetKeyboardFocus();
	}
}
