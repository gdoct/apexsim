#include "UI/ApexSessionBrowserWidget.h"

#include "ApexMenuFlowSubsystem.h"
#include "ApexNetSubsystem.h"
#include "Blueprint/WidgetTree.h"
#include "Catalog/ApexCatalogRows.h"
#include "Components/Border.h"
#include "Components/HorizontalBox.h"
#include "Components/ScrollBox.h"
#include "Components/ScrollBoxSlot.h"
#include "Components/SizeBox.h"
#include "Components/Spacer.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Track/ApexTrackContentSubsystem.h"
#include "UI/ApexButtonWidget.h"
#include "UI/ApexNavigation.h"
#include "UI/ApexRootWidget.h"
#include "UI/ApexUIStyle.h"

namespace
{
	const FName ActionBrowserBack(TEXT("Browser.Back"));
	const FName ActionBrowserRefresh(TEXT("Browser.Refresh"));
	const FName ActionBrowserCar(TEXT("Browser.Car"));
	const FName ActionBrowserCreate(TEXT("Browser.Create"));
	const FName ActionBrowserRow(TEXT("Browser.Row"));
	const FName ActionBrowserWatch(TEXT("Browser.Watch"));

	constexpr float PreviewWidth = 128.0f;
	constexpr float PreviewHeight = 72.0f;
	constexpr float RowHeight = 76.0f;
	constexpr float WatchWidth = 150.0f;

	const TCHAR* KindName(EApexSessionKind Kind)
	{
		switch (Kind)
		{
		case EApexSessionKind::Practice:    return TEXT("Practice");
		case EApexSessionKind::Sandbox:     return TEXT("Sandbox");
		case EApexSessionKind::Multiplayer: return TEXT("Multiplayer");
		default:                            return TEXT("Session");
		}
	}

	/** The badge: what a driver can do with the session, coloured to match. */
	FString StateBadge(const FApexSessionSummary& Session, FLinearColor& OutColour)
	{
		using namespace ApexUI;
		if (Session.State == EApexSessionState::Finished)
		{
			OutColour = Palette::TextMuted;
			return TEXT("Finished");
		}
		if (Session.PlayerCount >= Session.MaxPlayers)
		{
			OutColour = Palette::Error;
			return TEXT("Full");
		}
		switch (Session.State)
		{
		case EApexSessionState::Countdown: OutColour = Palette::Accent; return TEXT("Starting");
		case EApexSessionState::Racing:    OutColour = Palette::Accent; return TEXT("Racing");
		default:                           OutColour = Palette::Live;   return TEXT("Lobby");
		}
	}
}

UApexSessionBrowserWidget::UApexSessionBrowserWidget(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	SetIsFocusable(true);
}

void UApexSessionBrowserWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	BuildLayout();
}

void UApexSessionBrowserWidget::BuildLayout()
{
	using namespace ApexUI;

	auto MakeButton = [this](const FApexButtonSpec& Spec)
	{
		UApexButtonWidget* Button = WidgetTree->ConstructWidget<UApexButtonWidget>();
		Button->Setup(Spec);
		Button->OnActivated.AddDynamic(this, &UApexSessionBrowserWidget::HandleButtonActivated);
		return Button;
	};

	// --- Header: back, title, the count and a refresh -----------------------
	FApexButtonSpec BackSpec;
	BackSpec.Label = TEXT("Back");
	BackSpec.KeyCap = TEXT("Esc");
	BackSpec.bKeyCapLeading = true;
	BackSpec.Sound = EApexUiSound::Back;
	BackSpec.Variant = EApexButtonVariant::Bare;
	BackSpec.LabelSize = 15.0f;
	BackSpec.ActionId = ActionBrowserBack;
	HeaderBackButton = MakeButton(BackSpec);

	UHorizontalBox* Right = WidgetTree->ConstructWidget<UHorizontalBox>();
	CountText = MakeText(*WidgetTree, FString(), Font::Mono(10.0f, 120), Palette::TextMuted);
	AddH(Right, CountText, FMargin(0.0f, 0.0f, 20.0f, 0.0f), VAlign_Center);
	FApexButtonSpec RefreshSpec;
	RefreshSpec.Label = TEXT("Refresh");
	RefreshSpec.KeyCap = TEXT("R");
	RefreshSpec.Variant = EApexButtonVariant::Ghost;
	RefreshSpec.bCentreLabel = true;
	RefreshSpec.LabelSize = 15.0f;
	RefreshSpec.Height = 40.0f;
	RefreshSpec.ActionId = ActionBrowserRefresh;
	RefreshAction = MakeButton(RefreshSpec);
	AddH(Right, MakeSized(*WidgetTree, RefreshAction, 150.0f, -1.0f), FMargin(), VAlign_Center);

	UVerticalBox* Page = WidgetTree->ConstructWidget<UVerticalBox>();
	AddV(Page, MakeScreenHeader(*WidgetTree, HeaderBackButton, TEXT("Browse sessions"), Right));

	// --- The car the player would join in, and the way to make a session ---
	UHorizontalBox* Bar = WidgetTree->ConstructWidget<UHorizontalBox>();
	UVerticalBox* CarColumn = WidgetTree->ConstructWidget<UVerticalBox>();
	AddV(CarColumn, MakeLabel(*WidgetTree, TEXT("Joining in")));
	CarText = MakeText(*WidgetTree, FString(), Font::Display(24.0f), Palette::TextPrimary);
	AddV(CarColumn, CarText, FMargin(0.0f, 4.0f, 0.0f, 0.0f));
	AddH(Bar, CarColumn, FMargin(), VAlign_Center);

	FApexButtonSpec CarSpec;
	CarSpec.Label = TEXT("Change car");
	CarSpec.Variant = EApexButtonVariant::Ghost;
	CarSpec.bCentreLabel = true;
	CarSpec.LabelSize = 16.0f;
	CarSpec.Height = 52.0f;
	CarSpec.ActionId = ActionBrowserCar;
	ChangeCarButton = MakeButton(CarSpec);
	AddH(Bar, MakeSized(*WidgetTree, ChangeCarButton, 190.0f, -1.0f), FMargin(24.0f, 0.0f, 0.0f, 0.0f), VAlign_Center);
	AddH(Bar, WidgetTree->ConstructWidget<USpacer>(), FMargin(), VAlign_Fill, 1.0f);

	FApexButtonSpec CreateSpec;
	CreateSpec.Label = TEXT("Create session");
	CreateSpec.Variant = EApexButtonVariant::Primary;
	CreateSpec.bCentreLabel = true;
	CreateSpec.LabelSize = 18.0f;
	CreateSpec.Height = 52.0f;
	CreateSpec.ActionId = ActionBrowserCreate;
	CreateButton = MakeButton(CreateSpec);
	AddH(Bar, MakeSized(*WidgetTree, CreateButton, 240.0f, -1.0f), FMargin(), VAlign_Center);

	UVerticalBox* Body = WidgetTree->ConstructWidget<UVerticalBox>();
	AddV(Body, Bar);
	StatusLine = MakeText(*WidgetTree, FString(), Font::Mono(12.0f, 60), Palette::TextSecondary);
	AddV(Body, StatusLine, FMargin(0.0f, 22.0f, 0.0f, 12.0f));

	EmptyText = MakeText(*WidgetTree,
		TEXT("No sessions on this server yet. Create one, and drivers who connect will find it here."),
		Font::Body(16.0f), Palette::TextMuted);
	EmptyText->SetAutoWrapText(true);
	AddV(Body, EmptyText, FMargin(0.0f, 6.0f, 0.0f, 0.0f));

	List = WidgetTree->ConstructWidget<UScrollBox>();
	AddV(Body, List, FMargin(), HAlign_Fill, 1.0f);

	AddV(Page, MakePanel(*WidgetTree, Body, FMargin(Metrics::PageGutter, 28.0f, Metrics::PageGutter, 12.0f),
		MakeBrush(FLinearColor::Transparent)), FMargin(), HAlign_Fill, 1.0f);
	AddV(Page, MakeKeyHintBar(*WidgetTree,
		{ { TEXT("Enter"), TEXT("Join") }, { TEXT("→"), TEXT("Watch") }, { TEXT("R"), TEXT("Refresh") } },
		{ { TEXT("Esc"), TEXT("Back") } }));

	WidgetTree->RootWidget = MakePanel(*WidgetTree, Page, FMargin(), MakeBrush(Palette::Background));
}

void UApexSessionBrowserWidget::NativeConstruct()
{
	Super::NativeConstruct();
	if (UApexNetSubsystem* Net = GetNet())
	{
		Net->OnLobbyStateUpdated.AddDynamic(this, &UApexSessionBrowserWidget::HandleLobbyStateUpdated);
	}
}

void UApexSessionBrowserWidget::NativeDestruct()
{
	if (UApexNetSubsystem* Net = GetNet())
	{
		Net->OnLobbyStateUpdated.RemoveDynamic(this, &UApexSessionBrowserWidget::HandleLobbyStateUpdated);
	}
	Super::NativeDestruct();
}

void UApexSessionBrowserWidget::OnScreenActivated()
{
	Super::OnScreenActivated();
	RefreshList();
}

void UApexSessionBrowserWidget::HandleLobbyStateUpdated(const FApexLobbyState& LobbyState)
{
	RefreshList();
}

void UApexSessionBrowserWidget::ApplyRow(int32 Index, const FApexSessionSummary& Session)
{
	const UApexMenuFlowSubsystem* Flow = GetFlow();

	FApexTrackCatalogRow Track;
	const bool bHasTrack = Flow && Flow->GetTrackCatalogRow(Session.TrackId, Track);
	// Screens show the catalog's display name; the server's own name only
	// when the client has no row for the track.
	const FString TrackName = bHasTrack && !Track.DisplayName.IsEmpty() ? Track.DisplayName : Session.TrackName;

	TArray<FString> Parts;
	Parts.Add(Session.HostName.IsEmpty() ? FString(TEXT("Unknown host")) : Session.HostName);
	Parts.Add(FString::Printf(TEXT("%d / %d drivers"), Session.PlayerCount, Session.MaxPlayers));
	Parts.Add(KindName(Session.SessionKind));
	if (Session.RaceSeconds > 0)
	{
		Parts.Add(ApexRaceLength::Describe(Session.RaceSeconds));
	}
	else if (Session.LapLimit > 0)
	{
		Parts.Add(FString::Printf(TEXT("%d laps"), Session.LapLimit));
	}
	Parts.Add(FString::Printf(TEXT("%s · %s"),
		*FApexSessionConditions::WeatherLabel(Session.Conditions.Weather), *Session.Conditions.ClockText()));

	FLinearColor BadgeColour;
	FApexButtonSpec Spec;
	Spec.Label = TrackName;
	Spec.SubLabel = FString::Join(Parts, TEXT("  ·  "));
	Spec.Badge = StateBadge(Session, BadgeColour);
	Spec.BadgeColour = BadgeColour;
	Spec.Variant = EApexButtonVariant::Panel;
	Spec.LabelSize = 20.0f;
	Spec.Height = RowHeight;
	Spec.ActionId = ActionBrowserRow;
	Rows[Index]->Setup(Spec);

	// Watching needs a race being driven; otherwise the button is there but
	// dim, so the column stays put.
	FApexButtonSpec WatchSpec;
	WatchSpec.Label = TEXT("Watch");
	WatchSpec.Variant = Session.IsWatchable() ? EApexButtonVariant::Ghost : EApexButtonVariant::Locked;
	WatchSpec.bCentreLabel = true;
	WatchSpec.LabelSize = 16.0f;
	WatchSpec.Height = RowHeight;
	WatchSpec.ActionId = ActionBrowserWatch;
	WatchButtons[Index]->Setup(WatchSpec);
}

void UApexSessionBrowserWidget::RefreshList()
{
	using namespace ApexUI;
	const UApexNetSubsystem* Net = GetNet();
	const UApexMenuFlowSubsystem* Flow = GetFlow();
	if (!Net || !List)
	{
		return;
	}

	const TArray<FApexSessionSummary>& Sessions = Net->GetCachedLobbyState().AvailableSessions;
	bool bSameSet = Sessions.Num() == Shown.Num();
	for (int32 Index = 0; bSameSet && Index < Sessions.Num(); ++Index)
	{
		bSameSet = Sessions[Index].Id == Shown[Index].Id;
	}
	Shown = Sessions;

	if (!bSameSet)
	{
		// Rebuilt only when sessions come or go; counts and states are pushed
		// into the rows below, so a broadcast never moves the pad's place.
		const bool bHadFocus = HasFocusedDescendants();
		List->ClearChildren();
		Rows.Reset();
		WatchButtons.Reset();
		for (int32 Index = 0; Index < Sessions.Num(); ++Index)
		{
			FApexTrackCatalogRow Track;
			const bool bHasTrack = Flow && Flow->GetTrackCatalogRow(Sessions[Index].TrackId, Track);

			UApexButtonWidget* Row = WidgetTree->ConstructWidget<UApexButtonWidget>();
			Row->OnActivated.AddDynamic(this, &UApexSessionBrowserWidget::HandleButtonActivated);
			UApexButtonWidget* WatchButton = WidgetTree->ConstructWidget<UApexButtonWidget>();
			WatchButton->OnActivated.AddDynamic(this, &UApexSessionBrowserWidget::HandleButtonActivated);
			Rows.Add(Row);
			WatchButtons.Add(WatchButton);

			UHorizontalBox* Line = WidgetTree->ConstructWidget<UHorizontalBox>();
			AddH(Line, MakePreview(*WidgetTree, bHasTrack ? UApexTrackContentSubsystem::PreviewOf(Track) : nullptr,
				TEXT("No preview"), PreviewWidth, PreviewHeight), FMargin(0.0f, 0.0f, 12.0f, 0.0f), VAlign_Center);
			AddH(Line, Row, FMargin(), VAlign_Center, 1.0f);
			AddH(Line, MakeSized(*WidgetTree, WatchButton, WatchWidth, -1.0f), FMargin(8.0f, 0.0f, 0.0f, 0.0f), VAlign_Center);
			List->AddChild(Line);
			if (UScrollBoxSlot* LineSlot = Cast<UScrollBoxSlot>(Line->Slot))
			{
				LineSlot->SetPadding(FMargin(0.0f, Index == 0 ? 0.0f : 6.0f, 0.0f, 0.0f));
			}
		}
		LastFocused = Rows.Num() > 0 ? FMath::Clamp(LastFocused, 0, Rows.Num() - 1) : 0;
		if (bHadFocus && IsActiveScreen())
		{
			FocusDefault();
		}
	}
	for (int32 Index = 0; Index < Rows.Num(); ++Index)
	{
		ApplyRow(Index, Sessions[Index]);
	}

	const bool bHasCar = Flow && Flow->HasPendingCar();
	FApexCarCatalogRow Car;
	const FString CarName = bHasCar && Flow->GetCarCatalogRow(Flow->GetPendingCarId(), Car) && !Car.DisplayName.IsEmpty()
		? Car.DisplayName : bHasCar ? Flow->GetPendingCarId() : FString(TEXT("No car picked"));
	if (CarText)
	{
		CarText->SetText(FText::FromString(CarName));
		CarText->SetColorAndOpacity(FSlateColor(bHasCar ? Palette::TextPrimary : Palette::TextMuted));
	}
	if (ChangeCarButton)
	{
		ChangeCarButton->SetLabel(bHasCar ? TEXT("Change car") : TEXT("Pick a car"));
	}

	int32 Live = 0;
	for (const FApexSessionSummary& Session : Sessions)
	{
		Live += Session.IsWatchable() ? 1 : 0;
	}
	if (CountText)
	{
		CountText->SetText(FText::FromString(FString::Printf(TEXT("%d SESSION%s  ·  %d LIVE"),
			Sessions.Num(), Sessions.Num() == 1 ? TEXT("") : TEXT("S"), Live)));
	}
	if (StatusLine)
	{
		const FString Status = Net->GetConnectionState() != EApexConnectionState::Authenticated ? TEXT("NOT CONNECTED: CONNECT TO A SERVER FROM THE MAIN MENU")
			: Sessions.Num() == 0 ? FString()
			: !bHasCar ? TEXT("PICK A CAR BEFORE JOINING; WATCHING NEEDS NONE")
			: TEXT("ENTER JOINS IN YOUR CAR  ·  WATCH SPECTATES A RACE UNDER WAY");
		StatusLine->SetText(FText::FromString(Status));
		StatusLine->SetVisibility(Status.IsEmpty() ? ESlateVisibility::Collapsed : ESlateVisibility::HitTestInvisible);
	}
	if (EmptyText)
	{
		EmptyText->SetVisibility(Sessions.Num() == 0 ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
	}
}

void UApexSessionBrowserWidget::FocusDefault()
{
	if (Rows.IsValidIndex(LastFocused) && ApexNav::Focus(Rows[LastFocused]))
	{
		return;
	}
	if (!(CreateButton && ApexNav::Focus(CreateButton)))
	{
		SetKeyboardFocus();
	}
}

bool UApexSessionBrowserWidget::HandleNavigation(EUINavigation Direction, UWidget* Source)
{
	const int32 RowAt = ApexNav::IndexOf(Rows, Source);
	if (RowAt != INDEX_NONE)
	{
		LastFocused = RowAt;
		// Right from a row is its Watch button, whether or not it is lit: a
		// dim one is not focusable, and the move then stays on the row.
		if (Direction == EUINavigation::Right)
		{
			ApexNav::Focus(WatchButtons[RowAt]);
			return true;
		}
	}
	const int32 WatchAt = ApexNav::IndexOf(WatchButtons, Source);
	if (WatchAt != INDEX_NONE)
	{
		LastFocused = WatchAt;
		if (Direction == EUINavigation::Left)
		{
			ApexNav::Focus(Rows[WatchAt]);
			return true;
		}
	}
	if (ApexNav::IsSequential(Direction))
	{
		// No pages here: stay put rather than let Slate's tab order wander.
		return true;
	}
	// The rows sit in a scroll box, where Slate's own search stops at its edge.
	return ApexNav::MoveToward(this, Direction, Source);
}

FReply UApexSessionBrowserWidget::NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent)
{
	const FKey Key = InKeyEvent.GetKey();
	if (!InKeyEvent.IsRepeat() && (Key == EKeys::R || Key == EKeys::Gamepad_FaceButton_Top))
	{
		Refresh();
		return FReply::Handled();
	}
	return Super::NativeOnKeyDown(InGeometry, InKeyEvent);
}

void UApexSessionBrowserWidget::Refresh()
{
	// RequestLobbyState debounces: the server allows ten control messages a
	// second, and broadcasts a fresh snapshot every two seconds anyway.
	if (UApexNetSubsystem* Net = GetNet())
	{
		if (!Net->RequestLobbyState())
		{
			ShowToast(TEXT("Refreshing: the server sends the list every couple of seconds"), false);
		}
	}
}

void UApexSessionBrowserWidget::Join(int32 Index)
{
	UApexNetSubsystem* Net = GetNet();
	UApexMenuFlowSubsystem* Flow = GetFlow();
	if (!Net || !Flow || !Shown.IsValidIndex(Index))
	{
		return;
	}
	const FApexSessionSummary& Session = Shown[Index];
	if (Session.State == EApexSessionState::Finished)
	{
		ShowToast(TEXT("That session is over"), true);
		return;
	}
	if (Session.PlayerCount >= Session.MaxPlayers)
	{
		ShowToast(Session.IsWatchable() ? TEXT("That session is full: watch it instead") : TEXT("That session is full"), true);
		return;
	}
	if (!Flow->HasPendingCar())
	{
		ShowToast(TEXT("Pick a car first"), true);
		if (UApexRootWidget* Root = GetRoot())
		{
			Root->ScreenAfterCarSelect = EApexScreen::SessionBrowser;
		}
		ShowScreen(EApexScreen::CarSelect);
		return;
	}
	// SelectCar before JoinSession: the server records the car against the
	// player, so joining first would put them on the grid without one.
	Net->SelectCar(Flow->GetPendingCarId());
	Net->JoinSession(Session.Id);
}

void UApexSessionBrowserWidget::Watch(int32 Index)
{
	// No car needed: the race view opens on the session's cars once joined.
	if (UApexRootWidget* Root = Shown.IsValidIndex(Index) && Shown[Index].IsWatchable() ? GetRoot() : nullptr)
	{
		Root->WatchSession(Shown[Index].Id);
	}
}

void UApexSessionBrowserWidget::HandleButtonActivated(UApexButtonWidget* Button)
{
	if (!Button)
	{
		return;
	}
	if (Button == HeaderBackButton)
	{
		GoBack();
	}
	else if (Button == RefreshAction)
	{
		Refresh();
	}
	else if (Button == ChangeCarButton)
	{
		if (UApexRootWidget* Root = GetRoot())
		{
			Root->ScreenAfterCarSelect = EApexScreen::SessionBrowser;
		}
		ShowScreen(EApexScreen::CarSelect);
	}
	else if (Button == CreateButton)
	{
		ShowScreen(EApexScreen::SessionCreate);
	}
	else if (const int32 RowAt = ApexNav::IndexOf(Rows, Button); RowAt != INDEX_NONE)
	{
		LastFocused = RowAt;
		Join(RowAt);
	}
	else if (const int32 WatchAt = ApexNav::IndexOf(WatchButtons, Button); WatchAt != INDEX_NONE)
	{
		LastFocused = WatchAt;
		Watch(WatchAt);
	}
}
