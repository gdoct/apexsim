#include "UI/ApexMainMenuWidget.h"

#include "ApexMenuFlowSubsystem.h"
#include "ApexNetSubsystem.h"
#include "ApexSettingsSubsystem.h"
#include "ApexSim.h"
#include "Audio/ApexUiAudioSubsystem.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Image.h"
#include "Components/ScaleBox.h"
#include "Components/ScrollBox.h"
#include "Track/ApexTrackContentSubsystem.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "ApexSettingsSave.h"
#include "Components/SizeBox.h"
#include "Components/Spacer.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Kismet/KismetSystemLibrary.h"
#include "ApexReplayRecorder.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Race/ApexRaceDirector.h"
#include "TimerManager.h"
#include "UI/ApexButtonWidget.h"
#include "UI/ApexCreateSessionModel.h"
#include "UI/ApexNavigation.h"
#include "UI/ApexRootWidget.h"
#include "UI/ApexSettingsWidget.h"
#include "UI/ApexUIStyle.h"

namespace
{
	/** Identifies each button in the single activation handler. */
	namespace Action
	{
		const FName Start(TEXT("Start"));
		const FName ChangeSetup(TEXT("ChangeSetup"));
		const FName Browse(TEXT("Browse"));
		const FName Watch(TEXT("Watch"));
		const FName WatchHotlap(TEXT("WatchHotlap"));
		const FName Replays(TEXT("Replays"));
		const FName Create(TEXT("Create"));
		const FName Garage(TEXT("Garage"));
		const FName Tracks(TEXT("Tracks"));
		const FName Connect(TEXT("Connect"));
		const FName Settings(TEXT("Settings"));
		const FName Quick(TEXT("Quick"));
		const FName Back(TEXT("Back"));
		const FName GoGarage(TEXT("GoGarage"));
		const FName GoDrive(TEXT("GoDrive"));
		const FName GoCars(TEXT("GoCars"));
		const FName Setups(TEXT("Setups"));
		const FName ListItem(TEXT("ListItem"));
		const FName GoTracks(TEXT("GoTracks"));
		const FName GoCreate(TEXT("GoCreate"));
		const FName QuitBack(TEXT("QuitBack"));
		const FName QuitExit(TEXT("QuitExit"));
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
	// Focus lands a tick later, by way of the root's RequestFocusDefault.
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
			{ TEXT("Start"), TEXT("Settings") },
			{ TEXT("Alt+Enter"), TEXT("Display mode") },
			{ TEXT("Esc"), TEXT("Menu") },
		}));

	// The page itself is clear so the backdrop can show through; the solid
	// colour is the bottom layer.
	UBorder* Background = ApexUI::MakePanel(
		*WidgetTree,
		Page,
		FMargin(),
		ApexUI::MakeBrush(FLinearColor::Transparent));

	UOverlay* Stack = WidgetTree->ConstructWidget<UOverlay>();

	UBorder* Base = ApexUI::MakePanel(*WidgetTree, nullptr, FMargin(), ApexUI::MakeBrush(ApexUI::Palette::Background));
	// The root is an overlay now, so name the layer the demo race fades out.
	PageBackground = Base;
	UOverlaySlot* BaseSlot = Stack->AddChildToOverlay(Base);
	BaseSlot->SetHorizontalAlignment(HAlign_Fill);
	BaseSlot->SetVerticalAlignment(VAlign_Fill);

	// Full-bleed art (the focused circuit) under a dark scrim that keeps the
	// text readable; hidden until a page asks for it (SetBackdrop).
	UOverlay* Art = WidgetTree->ConstructWidget<UOverlay>();
	BackdropImage = WidgetTree->ConstructWidget<UImage>();
	BackdropImage->SetColorAndOpacity(FLinearColor(1.0f, 1.0f, 1.0f, 0.0f));
	UScaleBox* Scale = WidgetTree->ConstructWidget<UScaleBox>();
	// Fit, not fill: the previews are small, and filling blew one line up into a blur.
	Scale->SetStretch(EStretch::ScaleToFit);
	Scale->AddChild(BackdropImage);
	UOverlaySlot* ImageSlot = Art->AddChildToOverlay(Scale);
	ImageSlot->SetHorizontalAlignment(HAlign_Fill);
	ImageSlot->SetVerticalAlignment(VAlign_Fill);
	UOverlaySlot* ScrimSlot = Art->AddChildToOverlay(
		ApexUI::MakePanel(*WidgetTree, nullptr, FMargin(), ApexUI::MakeBrush(FLinearColor(0.02f, 0.02f, 0.025f, 0.6f))));
	ScrimSlot->SetHorizontalAlignment(HAlign_Fill);
	ScrimSlot->SetVerticalAlignment(VAlign_Fill);
	Art->SetVisibility(ESlateVisibility::Collapsed);
	BackdropLayer = Art;
	UOverlaySlot* ArtSlot = Stack->AddChildToOverlay(Art);
	ArtSlot->SetHorizontalAlignment(HAlign_Fill);
	ArtSlot->SetVerticalAlignment(VAlign_Fill);

	UOverlaySlot* PageSlot = Stack->AddChildToOverlay(Background);
	PageSlot->SetHorizontalAlignment(HAlign_Fill);
	PageSlot->SetVerticalAlignment(VAlign_Fill);

	QuitOverlay = BuildQuitOverlay();
	QuitOverlay->SetVisibility(ESlateVisibility::Collapsed);
	UOverlaySlot* QuitSlot = Stack->AddChildToOverlay(QuitOverlay);
	QuitSlot->SetHorizontalAlignment(HAlign_Fill);
	QuitSlot->SetVerticalAlignment(VAlign_Fill);

	WidgetTree->RootWidget = Stack;

	ShowPage(EPage::Root, /*bFocus*/ false);
}

UWidget* UApexMainMenuWidget::BuildQuitOverlay()
{
	UVerticalBox* Box = WidgetTree->ConstructWidget<UVerticalBox>();
	ApexUI::AddV(Box, ApexUI::MakeLabel(*WidgetTree, TEXT("Menu")), FMargin(0.0f, 0.0f, 0.0f, 14.0f));

	FApexButtonSpec Spec;
	Spec.Variant = EApexButtonVariant::Panel;
	Spec.bCentreLabel = true;

	const TPair<FName, FString> Rows[] = {
		TPair<FName, FString>(Action::QuitBack, TEXT("Back to main menu")),
		TPair<FName, FString>(Action::QuitExit, TEXT("Exit game")),
	};
	for (const TPair<FName, FString>& Row : Rows)
	{
		Spec.ActionId = Row.Key;
		Spec.Label = Row.Value;
		UApexButtonWidget* Button = WidgetTree->ConstructWidget<UApexButtonWidget>();
		Button->Setup(Spec);
		Button->OnActivated.AddDynamic(this, &UApexMainMenuWidget::HandleButtonActivated);
		ApexUI::AddV(Box, Button, FMargin(0.0f, 0.0f, 0.0f, 6.0f));
		QuitButtons.Add(Button);
	}

	UBorder* Card = ApexUI::MakePanel(
		*WidgetTree,
		ApexUI::MakeSized(*WidgetTree, Box, 420.0f, -1.0f),
		FMargin(28.0f),
		ApexUI::MakeBrush(ApexUI::Palette::Background, ApexUI::Palette::Border, 1.0f));

	// Dims the screen and swallows the mouse, so the rail behind cannot be clicked.
	UBorder* Dim = ApexUI::MakePanel(
		*WidgetTree,
		Card,
		FMargin(),
		ApexUI::MakeBrush(FLinearColor(0.0f, 0.0f, 0.0f, 0.78f)));
	Dim->SetHorizontalAlignment(HAlign_Center);
	Dim->SetVerticalAlignment(VAlign_Center);
	return Dim;
}

void UApexMainMenuWidget::SetQuitOverlayOpen(bool bOpen)
{
	bQuitOpen = bOpen;
	if (QuitOverlay)
	{
		QuitOverlay->SetVisibility(bOpen ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
	}
	ApplyFocus();
}

void UApexMainMenuWidget::QuitGame()
{
	if (UApexNetSubsystem* Net = GetNet())
	{
		Net->Disconnect();
	}
	UKismetSystemLibrary::QuitGame(this, nullptr, EQuitPreference::Quit, false);
}

void UApexMainMenuWidget::CycleDisplayMode()
{
	UApexSettingsSubsystem* Settings =
		GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexSettingsSubsystem>() : nullptr;
	if (!Settings || !Settings->Get())
	{
		return;
	}

	// EWindowMode order: 0 fullscreen, 1 borderless, 2 windowed.
	const int32 Next = (Settings->Get()->DisplayMode + 1) % 3;
	Settings->SetDisplayMode(Next);
	static const TCHAR* Names[] = { TEXT("Fullscreen"), TEXT("Borderless"), TEXT("Windowed") };
	ShowToast(FString::Printf(TEXT("Display mode: %s"), Names[Next]));
}

// ---------------------------------------------------------------------------
// Rail pages
// ---------------------------------------------------------------------------

void UApexMainMenuWidget::ShowPage(EPage Page, bool bFocus)
{
	CurrentPage = Page;
	RailButtons.Reset();

	if (HeroHome && HeroList)
	{
		HeroHome->SetVisibility(IsListPage() ? ESlateVisibility::Collapsed : ESlateVisibility::SelfHitTestInvisible);
		HeroList->SetVisibility(IsListPage() ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);
		SetBackdrop(nullptr);
		if (IsListPage())
		{
			BuildList(Page == EPage::Cars);
		}
	}

	for (int32 i = 0; i < PageBoxes.Num(); ++i)
	{
		const bool bShown = i == static_cast<int32>(Page);
		if (PageBoxes[i])
		{
			PageBoxes[i]->SetVisibility(bShown ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);
			if (bShown)
			{
				for (UWidget* Child : PageBoxes[i]->GetAllChildren())
				{
					if (UApexButtonWidget* Button = Cast<UApexButtonWidget>(Child))
					{
						RailButtons.Add(Button);
					}
				}
			}
		}
	}

	RailIndex = 0;
	for (int32 i = 0; i < RailButtons.Num(); ++i)
	{
		if (ApexNav::CanFocus(RailButtons[i]))
		{
			RailIndex = i;
			break;
		}
	}

	if (bFocus)
	{
		ActiveColumn = 1;
		ApplyFocus();
	}
}

bool UApexMainMenuWidget::GoUp()
{
	switch (CurrentPage)
	{
	case EPage::Garage:
	case EPage::Drive:  ShowPage(EPage::Root);   return true;
	case EPage::Cars:
	case EPage::Tracks: ShowPage(EPage::Garage); return true;
	case EPage::Create: ShowPage(EPage::Drive);  return true;
	default:            return false;
	}
}

UVerticalBox* UApexMainMenuWidget::BuildPage(EPage Page)
{
	UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>();

	// Rows that exist in the plan (docs/game/client.md) but have no game
	// behind them yet: greyed, no focus, listed under "Not implemented yet".
	auto Row = [&](const TCHAR* Label, FName Id)
	{
		FApexButtonSpec Spec;
		Spec.Variant = EApexButtonVariant::Panel;
		Spec.Label = Label;
		Spec.ActionId = Id;
		AddColumnButton(Column, Spec, true);
	};
	auto Unavailable = [&](const TCHAR* Label)
	{
		FApexButtonSpec Locked;
		Locked.Variant = EApexButtonVariant::Locked;
		Locked.Badge = TEXT("Not yet");
		Locked.Label = Label;
		AddColumnButton(Column, Locked, true);
	};
	auto Heading = [&](const TCHAR* Text)
	{
		ApexUI::AddV(Column, ApexUI::MakeLabel(*WidgetTree, Text), FMargin(0.0f, 0.0f, 0.0f, 14.0f));
	};

	switch (Page)
	{
	case EPage::Root:
		Heading(TEXT("Menu"));
		Row(TEXT("Garage"),       Action::GoGarage);
		Row(TEXT("Drive"),        Action::GoDrive);
		Row(TEXT("Watch a race"), Action::Watch);
		Row(TEXT("Settings"),     Action::Settings);
		break;

	case EPage::Garage:
		Heading(TEXT("Garage"));
		Row(TEXT("Garage"),  Action::GoCars);
		Row(TEXT("Replays"), Action::Replays);
		Row(TEXT("Tracks"),  Action::GoTracks);
		break;

	case EPage::Cars:
		Heading(TEXT("Garage / Garage"));
		Row(TEXT("Manage car setups"), Action::Setups);
		Row(TEXT("View car stats"), Action::Garage);
		break;

	case EPage::Tracks:
		Heading(TEXT("Garage / Tracks"));
		Row(TEXT("Watch hotlap"), Action::WatchHotlap);
		Row(TEXT("See track guide"), Action::Tracks);
		break;

	case EPage::Drive:
		Heading(TEXT("Drive"));
		Row(TEXT("Create session"),    Action::GoCreate);
		Row(TEXT("Browse sessions"),   Action::Browse);
		Row(TEXT("Connect to server"), Action::Connect);
		break;

	case EPage::Create:
		Heading(TEXT("Drive / Create session"));
		Row(TEXT("Quick"), Action::Quick);
		Unavailable(TEXT("Race weekend"));
		Row(TEXT("Custom"), Action::Create);
		break;

	default:
		break;
	}

	if (Page != EPage::Root)
	{
		FApexButtonSpec Spec;
		Spec.Variant = EApexButtonVariant::Ghost;
		Spec.Label = TEXT("Back");
		Spec.ActionId = Action::Back;
		AddColumnButton(Column, Spec, true);
	}
	return Column;
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
	// Two faces in one column: the home hero, and the car / circuit list the
	// Garage pages show (ShowPage switches them).
	UVerticalBox* Wrap = WidgetTree->ConstructWidget<UVerticalBox>();
	UVerticalBox* Hero = WidgetTree->ConstructWidget<UVerticalBox>();
	HeroHome = Hero;
	ApexUI::AddV(Wrap, Hero, FMargin(), HAlign_Fill, 1.0f);

	UVerticalBox* ListColumn = WidgetTree->ConstructWidget<UVerticalBox>();
	ListTitleText = ApexUI::MakeText(*WidgetTree, FString(), ApexUI::Font::Mono(11.0f, 200), ApexUI::Palette::Accent);
	ApexUI::AddV(ListColumn, ListTitleText, FMargin(0.0f, 0.0f, 0.0f, 14.0f), HAlign_Left);
	ListBox = WidgetTree->ConstructWidget<UVerticalBox>();
	ListScroll = WidgetTree->ConstructWidget<UScrollBox>();
	ListScroll->AddChild(ListBox);

	// The names on the left, what the focused one looks like beside them.
	UHorizontalBox* ListRow = WidgetTree->ConstructWidget<UHorizontalBox>();
	ApexUI::AddH(ListRow, ApexUI::MakeSized(*WidgetTree, ListScroll, 320.0f, -1.0f), FMargin(), VAlign_Fill);
	ListDetail = WidgetTree->ConstructWidget<UVerticalBox>();
	ApexUI::AddH(ListRow, ListDetail, FMargin(36.0f, 0.0f, 0.0f, 0.0f), VAlign_Top, 1.0f);
	ApexUI::AddV(ListColumn, ListRow, FMargin(), HAlign_Fill, 1.0f);
	ListColumn->SetVisibility(ESlateVisibility::Collapsed);
	HeroList = ListColumn;
	ApexUI::AddV(Wrap, ListColumn, FMargin(), HAlign_Fill, 1.0f);

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

	return Wrap;
}

// ---------------------------------------------------------------------------
// Car / circuit list (Garage pages)
// ---------------------------------------------------------------------------

void UApexMainMenuWidget::BuildList(bool bCars)
{
	UApexNetSubsystem* Net = GetNet();
	UApexMenuFlowSubsystem* Flow = GetFlow();
	if (!ListBox || !Net || !Flow)
	{
		return;
	}

	bListShowsCars = bCars;
	ListBox->ClearChildren();
	ListButtons.Reset();
	ListIds.Reset();

	if (ListTitleText)
	{
		ListTitleText->SetText(FText::FromString(bCars ? TEXT("GARAGE  /  CARS") : TEXT("GARAGE  /  TRACKS")));
	}

	const FApexLobbyState& Lobby = Net->GetCachedLobbyState();
	const FString Pending = bCars ? Flow->GetPendingCarId() : Flow->GetPendingTrackId();

	auto AddItem = [&](const FString& Id, const FString& Name, const FString& Badge)
	{
		FApexButtonSpec Spec;
		Spec.Variant = EApexButtonVariant::Panel;
		Spec.Label = Name;
		// No badge: it ran into long names, and the detail beside the list has the figures.
		Spec.LabelSize = 17.0f;
		Spec.ActionId = Action::ListItem;
		UApexButtonWidget* Button = WidgetTree->ConstructWidget<UApexButtonWidget>();
		Button->Setup(Spec);
		Button->SetSelected(Id == Pending);
		Button->OnActivated.AddDynamic(this, &UApexMainMenuWidget::HandleButtonActivated);
		ApexUI::AddV(ListBox, Button, FMargin(0.0f, 0.0f, 0.0f, 6.0f));
		ListButtons.Add(Button);
		ListIds.Add(Id);
	};

	// Names as the player sees them (the catalog's, else the server's), A to Z.
	TArray<TPair<FString, FString>> Entries; // name, id
	if (bCars)
	{
		for (const FApexCarConfigSummary& Car : Lobby.CarConfigs)
		{
			FApexCarCatalogRow Row;
			const bool bRow = Flow->GetCarCatalogRow(Car.Id, Row) && !Row.DisplayName.IsEmpty();
			Entries.Emplace(bRow ? Row.DisplayName : Car.Name, Car.Id);
		}
	}
	else
	{
		for (const FApexTrackConfigSummary& Track : Lobby.TrackConfigs)
		{
			FApexTrackCatalogRow Row;
			const bool bRow = Flow->GetTrackCatalogRow(Track.Id, Row) && !Row.DisplayName.IsEmpty();
			Entries.Emplace(bRow ? Row.DisplayName : Track.Name, Track.Id);
		}
	}
	Entries.StableSort([](const TPair<FString, FString>& A, const TPair<FString, FString>& B)
	{
		return A.Key.Compare(B.Key, ESearchCase::IgnoreCase) < 0;
	});
	for (const TPair<FString, FString>& Entry : Entries)
	{
		AddItem(Entry.Value, Entry.Key, FString());
	}

	if (ListButtons.Num() == 0)
	{
		ApexUI::AddV(ListBox, ApexUI::MakeText(*WidgetTree,
			Net->IsAuthenticated() ? TEXT("Nothing to show yet.") : TEXT("Connect to a server to see the list."),
			ApexUI::Font::Body(14.0f), ApexUI::Palette::TextMuted));
	}

	ListIndex = FMath::Max(0, ListIds.IndexOfByKey(Pending));
	DetailShownId.Reset();
	RefreshListDetail(ListIndex);
}

void UApexMainMenuWidget::RefreshListDetail(int32 Index)
{
	UApexMenuFlowSubsystem* Flow = GetFlow();
	if (!ListDetail || !Flow)
	{
		return;
	}
	if (!ListIds.IsValidIndex(Index))
	{
		ListDetail->ClearChildren();
		DetailShownId.Reset();
		return;
	}
	if (DetailShownId == ListIds[Index])
	{
		return;
	}
	DetailShownId = ListIds[Index];
	ListDetail->ClearChildren();

	using namespace ApexUI;

	// A title in the hero's style: the distinctive half bright, the generic half grey.
	auto AddTitle = [&](const FString& Name)
	{
		FString Head;
		FString Tail;
		SplitTitle(Name, Head, Tail);
		const int32 Length = Head.Len() + Tail.Len();
		const float Size = Length > 26 ? 34.0f : (Length > 18 ? 42.0f : 52.0f);
		UHorizontalBox* Title = WidgetTree->ConstructWidget<UHorizontalBox>();
		AddH(Title, MakeText(*WidgetTree, Head, Font::Display(Size), Palette::TextPrimary), FMargin(), VAlign_Bottom);
		AddH(Title, MakeText(*WidgetTree, Tail, Font::Display(Size), Palette::TextDisabled), FMargin(), VAlign_Bottom);
		AddV(ListDetail, Title, FMargin(0.0f, 18.0f, 0.0f, 6.0f), HAlign_Left);
	};

	auto AddStats = [&](const TArray<TPair<FString, FString>>& Stats)
	{
		UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();
		for (int32 i = 0; i < Stats.Num(); ++i)
		{
			UTextBlock* Value = nullptr;
			AddH(Row, MakeStat(*WidgetTree, Stats[i].Key, Value), FMargin(i == 0 ? 0.0f : 10.0f, 0.0f, 0.0f, 0.0f), VAlign_Fill, 1.0f);
			Value->SetText(FText::FromString(Stats[i].Value));
		}
		AddV(ListDetail, Row, FMargin(0.0f, 22.0f, 0.0f, 0.0f));
	};

	if (bListShowsCars)
	{
		SetBackdrop(nullptr);
		FApexCarCatalogRow Car;
		const bool bRow = Flow->GetCarCatalogRow(DetailShownId, Car);

		// There is no car render on disk to show, so the class itself is the picture.
		AddV(ListDetail, MakeArtPlaceholder(*WidgetTree,
			bRow ? ApexCatalog::DisplayClass(Car.CarClass).ToUpper() : FString(TEXT("CAR"))));
		AddTitle(bRow ? Car.DisplayName : FString(TEXT("Car")));

		TArray<FString> Sub;
		if (bRow)
		{
			if (!Car.Brand.IsEmpty())               { Sub.Add(Car.Brand.ToUpper()); }
			if (!Car.ManufacturerCountry.IsEmpty()) { Sub.Add(Car.ManufacturerCountry.ToUpper()); }
			if (Car.ModelYear > 0)                  { Sub.Add(FString::FromInt(Car.ModelYear)); }
		}
		AddV(ListDetail, MakeText(*WidgetTree, FString::Join(Sub, TEXT("  /  ")), Font::Mono(11.0f, 120), Palette::TextMuted));
		AddStats({
			{ TEXT("Class"), bRow ? ApexCatalog::DisplayClass(Car.CarClass) : FString(TEXT("—")) },
			{ TEXT("Power"), bRow && Car.MaxPowerKw > 0.0f ? FString::Printf(TEXT("%d kW"), FMath::RoundToInt(Car.MaxPowerKw)) : FString(TEXT("—")) },
			{ TEXT("Mass"),  bRow && Car.MassKg > 0.0f ? FString::Printf(TEXT("%d kg"), FMath::RoundToInt(Car.MassKg)) : FString(TEXT("—")) },
		});
		return;
	}

	FApexTrackCatalogRow Track;
	const bool bRow = Flow->GetTrackCatalogRow(DetailShownId, Track);

	// The circuit itself, large: this is the picture the page is about, and it
	// fills the screen behind the page too.
	UTexture2D* Art = bRow ? UApexTrackContentSubsystem::PreviewOf(Track) : nullptr;
	SetBackdrop(Art);
	AddV(ListDetail, MakePreview(*WidgetTree, Art, TEXT("No preview"), -1.0f, 300.0f));
	AddTitle(bRow ? Track.DisplayName : FString(TEXT("Circuit")));

	TArray<FString> Sub;
	if (bRow)
	{
		if (!Track.Country.IsEmpty())  { Sub.Add(Track.Country.ToUpper()); }
		if (!Track.City.IsEmpty())     { Sub.Add(Track.City.ToUpper()); }
		if (!Track.Category.IsEmpty()) { Sub.Add(ApexCatalog::DisplayClass(Track.Category).ToUpper()); }
	}
	AddV(ListDetail, MakeText(*WidgetTree, FString::Join(Sub, TEXT("  /  ")), Font::Mono(11.0f, 120), Palette::TextMuted));

	if (bRow && !Track.Description.IsEmpty())
	{
		UTextBlock* Description = MakeText(*WidgetTree, Track.Description, Font::Body(13.0f), Palette::TextMuted);
		Description->SetAutoWrapText(true);
		AddV(ListDetail, Description, FMargin(0.0f, 10.0f, 0.0f, 0.0f));
	}

	float Best = 0.0f;
	AddStats({
		{ TEXT("Length"),    bRow && Track.LengthM > 0.0f ? FString::Printf(TEXT("%.2f km"), Track.LengthM / 1000.0f) : FString(TEXT("—")) },
		{ TEXT("Your best"), Flow->GetBestLapSeconds(DetailShownId, Best) ? UApexMenuFlowSubsystem::FormatLapTime(Best) : FString(TEXT("Never driven")) },
	});
}

void UApexMainMenuWidget::RefreshList()
{
	const UApexNetSubsystem* Net = GetNet();
	if (!IsListPage() || !Net)
	{
		return;
	}
	const FApexLobbyState& Lobby = Net->GetCachedLobbyState();
	const int32 Count = bListShowsCars ? Lobby.CarConfigs.Num() : Lobby.TrackConfigs.Num();
	if (Count != ListButtons.Num())
	{
		BuildList(bListShowsCars);
	}
}

void UApexMainMenuWidget::CommitListItem(int32 Index)
{
	UApexMenuFlowSubsystem* Flow = GetFlow();
	if (!Flow || !ListIds.IsValidIndex(Index))
	{
		return;
	}

	ListIndex = Index;
	RefreshListDetail(Index);
	for (int32 i = 0; i < ListButtons.Num(); ++i)
	{
		if (ListButtons[i])
		{
			ListButtons[i]->SetSelected(i == Index);
		}
	}

	if (bListShowsCars)
	{
		if (Flow->GetPendingCarId() != ListIds[Index]) { Flow->SetPendingCar(ListIds[Index]); }
	}
	else
	{
		if (Flow->GetPendingTrackId() != ListIds[Index]) { Flow->SetPendingTrack(ListIds[Index]); }
	}
}

UWidget* UApexMainMenuWidget::BuildRail()
{
	// One column per page, only the current one shown (ShowPage).
	UVerticalBox* Rail = WidgetTree->ConstructWidget<UVerticalBox>();

	PageBoxes.Init(nullptr, static_cast<int32>(EPage::Count));
	for (int32 i = 0; i < static_cast<int32>(EPage::Count); ++i)
	{
		UVerticalBox* Box = BuildPage(static_cast<EPage>(i));
		PageBoxes[i] = Box;
		ApexUI::AddV(Rail, Box);
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
		// ShowPage fills RailButtons with the visible page's rows.
		AllRailButtons.Add(Button);
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
	RefreshList();
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
	case EApexConnectionState::Failed:
	case EApexConnectionState::Reconnecting:                                      DotColour = ApexUI::Palette::Error;  break;
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
				Parts.Add(ApexCatalog::DisplayClass(TrackRow.Category).ToUpper());
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
		SessionValueText->SetText(FText::FromString(Flow->CreateStartingMode == EApexGameMode::Hotlap
			? UApexMenuFlowSubsystem::GetGameModeName(Flow->CreateStartingMode)
			: FString::Printf(
				TEXT("%s · %s · %d AI"),
				*UApexMenuFlowSubsystem::GetGameModeName(Flow->CreateStartingMode),
				*(Flow->bCreateTimedRace ? ApexRaceLength::Describe(Flow->CreateRaceMinutes * 60)
					: FString::Printf(TEXT("%d laps"), Flow->CreateLapLimit)),
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

	for (UApexButtonWidget* Button : AllRailButtons)
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
		else if (Id == Action::Replays)
		{
			// A file count, not a read: this runs with every lobby broadcast.
			TArray<FString> Saved;
			TArray<FString> Recent;
			IFileManager::Get().FindFiles(Saved, *FPaths::Combine(UApexReplayRecorder::ReplayDirectory(), TEXT("*.apxs")), true, false);
			IFileManager::Get().FindFiles(Recent, *FPaths::Combine(UApexReplayRecorder::RecentDirectory(), TEXT("*.apxs")), true, false);
			const int32 Count = Saved.Num() + Recent.Num();
			Button->SetBadge(Count > 0 ? FString::FromInt(Count) : TEXT("None"), ApexUI::Palette::TextMuted);
		}
	}
}

// ---------------------------------------------------------------------------
// Interaction
// ---------------------------------------------------------------------------

void UApexMainMenuWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);
	// The backdrop comes and goes on its own clock, not the lobby's.
	RefreshWatchBadge();

	// The detail follows whichever row has focus or the pointer, however it got
	// there (keys, pad, mouse hover); only a change of row counts.
	if (IsListPage())
	{
		int32 Probe = INDEX_NONE;
		for (int32 i = 0; i < ListButtons.Num() && Probe == INDEX_NONE; ++i)
		{
			if (ListButtons[i] && (ListButtons[i]->HasAnyUserFocus() || ListButtons[i]->IsHovered()))
			{
				Probe = i;
			}
		}
		if (Probe != INDEX_NONE && Probe != LastListProbe)
		{
			ListIndex = Probe;
			RefreshListDetail(Probe);
		}
		LastListProbe = Probe;
	}

	// The circuit art fades in when the focused row changes, and out when it goes.
	if (BackdropImage && !FMath::IsNearlyEqual(BackdropAlpha, BackdropTarget, 0.002f))
	{
		BackdropAlpha = FMath::FInterpTo(BackdropAlpha, BackdropTarget, InDeltaTime, 5.0f);
		BackdropImage->SetColorAndOpacity(FLinearColor(1.0f, 1.0f, 1.0f, BackdropAlpha * 0.5f));
		if (BackdropTarget <= 0.0f && BackdropAlpha < 0.01f && BackdropLayer)
		{
			BackdropLayer->SetVisibility(ESlateVisibility::Collapsed);
		}
	}
}

void UApexMainMenuWidget::SetBackdrop(UTexture2D* Texture)
{
	if (!BackdropImage || !BackdropLayer)
	{
		return;
	}
	if (Texture)
	{
		BackdropImage->SetBrushFromTexture(Texture, /*bMatchSize*/ false);
		BackdropLayer->SetVisibility(ESlateVisibility::HitTestInvisible);
		// Restart the fade so a new circuit arrives rather than snaps.
		BackdropAlpha = 0.0f;
		BackdropTarget = 1.0f;
	}
	else
	{
		BackdropTarget = 0.0f;
	}
}

void UApexMainMenuWidget::RefreshWatchBadge()
{
	const AApexRaceDirector* Director = AApexRaceDirector::Find(this);
	const bool bOn = Director && Director->IsDemoViewActive();
	if (WatchBadgeState == (bOn ? 1 : 0))
	{
		return;
	}
	WatchBadgeState = bOn ? 1 : 0;
	for (UApexButtonWidget* Button : AllRailButtons)
	{
		if (Button && Button->GetActionId() == Action::Watch)
		{
			Button->SetBadge(bOn ? TEXT("On now") : TEXT("None"), bOn ? ApexUI::Palette::Live : ApexUI::Palette::TextMuted);
		}
	}
}

void UApexMainMenuWidget::HandleButtonActivated(UApexButtonWidget* Button)
{
	if (!Button)
	{
		return;
	}

	const FName Id = Button->GetActionId();
	UApexRootWidget* Root = GetRoot();

	if (Id == Action::Start || Id == Action::Quick)
	{
		StartRememberedSession();
	}
	else if (Id == Action::GoGarage)  { ShowPage(EPage::Garage); }
	else if (Id == Action::GoDrive)   { ShowPage(EPage::Drive); }
	else if (Id == Action::GoCars)    { ShowPage(EPage::Cars); }
	else if (Id == Action::GoTracks)  { ShowPage(EPage::Tracks); }
	else if (Id == Action::GoCreate)  { ShowPage(EPage::Create); }
	else if (Id == Action::Back)      { GoUp(); }
	else if (Id == Action::ListItem)
	{
		// Pick it, then on to the rail's actions for it.
		CommitListItem(ApexNav::IndexOf(ListButtons, Button));
		ActiveColumn = 1;
		ApplyFocus();
	}
	else if (Id == Action::QuitBack)
	{
		SetQuitOverlayOpen(false);
		ShowPage(EPage::Root);
	}
	else if (Id == Action::QuitExit)
	{
		QuitGame();
	}
	else if (Id == Action::ChangeSetup)
	{
		ShowScreen(EApexScreen::SessionCreate);
	}
	else if (Id == Action::Browse)
	{
		ShowScreen(EApexScreen::SessionBrowser);
	}
	else if (Id == Action::Replays)
	{
		ShowScreen(EApexScreen::Replays);
	}
	else if (Id == Action::Watch)
	{
		// The race behind the menu, full screen; live sessions are watched from the browser.
		if (Root)
		{
			Root->WatchBackdrop();
		}
	}
	else if (Id == Action::WatchHotlap)
	{
		// One AI car laps the pending circuit alone, full screen; the server
		// works the lap out for the car and the sky as it is asked for.
		if (Root)
		{
			Root->StartHotlapWatch();
		}
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
	else if (Id == Action::Setups)
	{
		if (Root)
		{
			Root->OpenSetupEditor();
		}
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
	else if (Id == Action::Settings)
	{
		OpenSettings();
	}
}

void UApexMainMenuWidget::OpenSettings()
{
	// On the controls page: key and pad bindings are what a player comes here
	// for before a first race, and the wheel page is one shoulder press on.
	if (UApexRootWidget* Root = GetRoot())
	{
		Root->OpenSettings(EApexSettingsTab::Controls);
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

	UE_LOG(LogApexSim, Log, TEXT("Main menu start: track '%s', %d AI, %d laps, %d s, mode %d, %s"),
		*Flow->GetPendingTrackId(), Flow->EffectiveAiCount(), Flow->EffectiveLapLimit(), Flow->EffectiveRaceSeconds(),
		static_cast<int32>(Flow->CreateStartingMode), *Flow->CreateConditions.Describe());

	Net->CreateSessionWithOrder(
		Flow->GetPendingTrackId(),
		Flow->CreateMaxPlayers,
		Flow->EffectiveAiCount(),
		Flow->EffectiveLapLimit(),
		Flow->CreateSessionKind,
		Flow->CreateAllowedAssists,
		Flow->CreateConditions,
		Flow->CreateDamage,
		Flow->CreateAiSkill,
		Flow->EffectiveRaceSeconds(),
		Flow->GridOrderToSend(ApexCreateSession::GridCeiling));
}

void UApexMainMenuWidget::HandleConnectionStateChanged(EApexConnectionState NewState, const FString& Detail)
{
	RefreshAll();

	// Rejected credentials drop the user straight onto the connect dialog so
	// they can fix them rather than staring at a dead menu. An unreachable
	// server is not Failed but Reconnecting: the subsystem keeps retrying and
	// the menu fills in by itself once the server is up.
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
	// A pad's Start, or whatever opens the pause menu on a wheel, opens
	// settings, as it opens the pause menu in a race; inside the overlay the
	// same button closes it again. Never Escape: here that quits.
	const FKey Key = InKeyEvent.GetKey();
	const UApexSettingsSubsystem* Settings =
		GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexSettingsSubsystem>() : nullptr;
	if (Key == EKeys::Gamepad_Special_Right || (Key != EKeys::Escape && Settings && Settings->IsPauseKey(Key)))
	{
		ApexUiAudio::Play(this, EApexUiSound::Accept);
		OpenSettings();
		return FReply::Handled();
	}

	if (Key == EKeys::Enter && InKeyEvent.IsAltDown())
	{
		// Flip the display mode; never an accept on the focused row.
		CycleDisplayMode();
		return FReply::Handled();
	}

	if (Key == EKeys::Escape)
	{
		// Escape opens (and closes) the Back to main menu / Exit game menu. Only
		// the key itself: a pad's B button steps up a page, see HandleBack.
		SetQuitOverlayOpen(!bQuitOpen);
		return FReply::Handled();
	}

	return Super::NativeOnKeyDown(InGeometry, InKeyEvent);
}

void UApexMainMenuWidget::FocusDefault()
{
	ApplyFocus();
}

bool UApexMainMenuWidget::HandleBack()
{
	if (bQuitOpen)
	{
		SetQuitOverlayOpen(false);
	}
	else
	{
		GoUp();
	}
	return true;
}

bool UApexMainMenuWidget::HandleNavigation(EUINavigation Direction, UWidget* Source)
{
	if (bQuitOpen)
	{
		const int32 At = ApexNav::IndexOf(QuitButtons, Source);
		if (At != INDEX_NONE && QuitButtons.Num() > 1)
		{
			const bool bUp = Direction == EUINavigation::Up || Direction == EUINavigation::Previous;
			const bool bDown = Direction == EUINavigation::Down || Direction == EUINavigation::Next;
			if (bUp || bDown)
			{
				ApexNav::Focus(QuitButtons[(At + (bDown ? 1 : QuitButtons.Num() - 1)) % QuitButtons.Num()]);
			}
		}
		return true;
	}

	if (ApexNav::IsSequential(Direction))
	{
		SwitchColumn();
		return true;
	}

	const int32 ListAt = ApexNav::IndexOf(ListButtons, Source);
	if (ListAt != INDEX_NONE && IsListPage())
	{
		ActiveColumn = 0;
		ListIndex = ListAt;
		switch (Direction)
		{
		case EUINavigation::Up:
			ListIndex = FMath::Max(0, ListAt - 1);
			ApplyFocus();
			return true;
		case EUINavigation::Down:
			ListIndex = FMath::Min(ListButtons.Num() - 1, ListAt + 1);
			ApplyFocus();
			return true;
		case EUINavigation::Right:
			// Over to the rail with this row as the pick.
			CommitListItem(ListAt);
			ActiveColumn = 1;
			ApplyFocus();
			return true;
		default:
			return true;
		}
	}

	const int32 HeroAt = ApexNav::IndexOf(HeroButtons, Source);
	if (HeroAt != INDEX_NONE)
	{
		ActiveColumn = 0;
		HeroIndex = HeroAt;

		switch (Direction)
		{
		case EUINavigation::Left:
			HeroIndex = FMath::Max(0, HeroAt - 1);
			ApplyFocus();
			return true;

		case EUINavigation::Right:
			if (HeroAt + 1 < HeroButtons.Num())
			{
				HeroIndex = HeroAt + 1;
			}
			else
			{
				// Off the end of the actions is the rail.
				ActiveColumn = 1;
			}
			ApplyFocus();
			return true;

		default:
			// Nothing above or below the actions worth landing on; Slate's
			// search would pick a rail item at random.
			return true;
		}
	}

	const int32 RailAt = ApexNav::IndexOf(RailButtons, Source);
	if (RailAt != INDEX_NONE)
	{
		ActiveColumn = 1;
		RailIndex = RailAt;

		switch (Direction)
		{
		case EUINavigation::Up:
			MoveRail(-1);
			return true;

		case EUINavigation::Down:
			MoveRail(1);
			return true;

		case EUINavigation::Left:
			ActiveColumn = 0;
			ApplyFocus();
			return true;

		default:
			return true;
		}
	}

	// From the screen itself: the base lands focus on the default.
	return false;
}

void UApexMainMenuWidget::MoveRail(int32 Delta)
{
	if (RailButtons.Num() == 0)
	{
		return;
	}

	// Wrap, and step over anything that cannot be activated (the locked rows).
	for (int32 Attempt = 0; Attempt < RailButtons.Num(); ++Attempt)
	{
		RailIndex = (RailIndex + Delta + RailButtons.Num()) % RailButtons.Num();
		if (ApexNav::CanFocus(RailButtons[RailIndex]))
		{
			break;
		}
	}

	ApplyFocus();
}

void UApexMainMenuWidget::SwitchColumn()
{
	if (ActiveColumn == 0 && IsListPage())
	{
		CommitListItem(ListIndex);
	}
	ActiveColumn = ActiveColumn == 0 ? 1 : 0;
	ApplyFocus();
}

void UApexMainMenuWidget::ApplyFocus()
{
	if (bQuitOpen)
	{
		// The Esc menu owns focus until it is dismissed.
		if (QuitButtons.Num() > 0 && ApexNav::Focus(QuitButtons[0]))
		{
			return;
		}
	}

	const bool bList = ActiveColumn == 0 && IsListPage();
	const TArray<TObjectPtr<UApexButtonWidget>>& Column = ActiveColumn == 0 ? (bList ? ListButtons : HeroButtons) : RailButtons;
	const int32 Index = ActiveColumn == 0 ? (bList ? ListIndex : HeroIndex) : RailIndex;

	if (bList && ListScroll && ListButtons.IsValidIndex(ListIndex))
	{
		ListScroll->ScrollWidgetIntoView(ListButtons[ListIndex], false);
		RefreshListDetail(ListIndex);
	}

	if (Column.IsValidIndex(Index) && ApexNav::Focus(Column[Index]))
	{
		UE_LOG(LogApexSim, Verbose, TEXT("Main menu focus -> column %d index %d (%s)"),
			ActiveColumn, Index, *Column[Index]->GetActionId().ToString());
		return;
	}

	// The remembered button is gone or locked: anything in the column will do,
	// and failing that keep the keys coming to this screen.
	for (UApexButtonWidget* Button : Column)
	{
		if (ApexNav::Focus(Button))
		{
			return;
		}
	}
	SetKeyboardFocus();
}
