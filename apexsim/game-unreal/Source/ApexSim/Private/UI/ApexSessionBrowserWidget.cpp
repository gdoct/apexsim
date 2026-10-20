#include "UI/ApexSessionBrowserWidget.h"

#include "ApexMenuFlowSubsystem.h"
#include "ApexNetSubsystem.h"
#include "ApexReplayRecorder.h"
#include "ApexSim.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UI/ApexSegmentedWidget.h"
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
		case EApexSessionKind::Multiplayer: return TEXT("Multiplayer");
		default:                            return TEXT("Session");
		}
	}

	/** The badge: what a driver can do with the session, coloured to match. */
	FString StateBadge(const FApexSessionSummary& Session, FLinearColor& OutColour)
	{
		using namespace ApexUI;
		if (Session.State == EApexSessionState::Finished && !Session.bWeekendWaiting)
		{
			OutColour = Palette::TextMuted;
			return TEXT("Finished");
		}
		if (Session.PlayerCount >= Session.MaxPlayers)
		{
			OutColour = Palette::Error;
			return TEXT("Full");
		}
		if (Session.bWeekendWaiting)
		{
			// A weekend between phases, waiting for its host: drivers can still join.
			OutColour = Palette::Live;
			return TEXT("Between sessions");
		}
		switch (Session.State)
		{
		case EApexSessionState::Countdown: OutColour = Palette::Accent; return TEXT("Starting");
		case EApexSessionState::Racing:    OutColour = Palette::Accent; return TEXT("Racing");
		default:                           OutColour = Palette::Live;   return TEXT("Lobby");
		}
	}
}

namespace ApexBrowser
{
	bool Passes(const FApexSessionSummary& Session, EFilter Filter)
	{
		switch (Filter)
		{
		case EFilter::Joinable:
			return Session.IsJoinable();
		case EFilter::Racing:
			return Session.IsWatchable();
		case EFilter::Recorded:
			return false;
		default:
			return true;
		}
	}

	FString DamageWords(EApexDamageLevel Damage)
	{
		switch (Damage)
		{
		case EApexDamageLevel::Off:        return TEXT("no damage");
		case EApexDamageLevel::Reduced:    return TEXT("reduced damage");
		case EApexDamageLevel::VisualOnly: return TEXT("visual damage");
		default:                           return FString();
		}
	}

	bool ParseRecordings(const FString& Json, TArray<FRecording>& Out)
	{
		Out.Reset();
		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		const TArray<TSharedPtr<FJsonValue>>* Sessions = nullptr;
		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid() || !Root->TryGetArrayField(TEXT("sessions"), Sessions))
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Sessions)
		{
			const TSharedPtr<FJsonObject> Row = Value.IsValid() ? Value->AsObject() : nullptr;
			if (!Row.IsValid())
			{
				continue;
			}
			FRecording& R = Out.AddDefaulted_GetRef();
			Row->TryGetStringField(TEXT("id"), R.Id);
			Row->TryGetStringField(TEXT("file"), R.File);
			Row->TryGetStringField(TEXT("track"), R.Track);
			Row->TryGetStringField(TEXT("track_id"), R.TrackId);
			Row->TryGetStringField(TEXT("kind"), R.Kind);
			Row->TryGetStringField(TEXT("winner"), R.Winner);
			Row->TryGetNumberField(TEXT("recorded_at"), R.RecordedAt);
			Row->TryGetNumberField(TEXT("duration_s"), R.DurationS);
			Row->TryGetNumberField(TEXT("cars"), R.Cars);
			Row->TryGetNumberField(TEXT("humans"), R.Humans);
			Row->TryGetNumberField(TEXT("laps"), R.Laps);
		}
		return true;
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

	// What the list shows: every live session, the joinable ones, the races
	// under way, or the finished races the server recorded.
	FilterPills = WidgetTree->ConstructWidget<UApexSegmentedWidget>();
	FilterPills->ControlId = TEXT("Browser.Filter");
	FilterPills->Setup({ TEXT("ALL"), TEXT("JOINABLE"), TEXT("RACING"), TEXT("RECORDED") }, 0, 132.0f);
	FilterPills->OnChosen.AddDynamic(this, &UApexSessionBrowserWidget::HandleFilterChosen);
	AddV(Body, FilterPills, FMargin(0.0f, 22.0f, 0.0f, 0.0f), HAlign_Left);

	StatusLine = MakeText(*WidgetTree, FString(), Font::Mono(12.0f, 60), Palette::TextSecondary);
	AddV(Body, StatusLine, FMargin(0.0f, 18.0f, 0.0f, 12.0f));

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
		{ { TEXT("Enter"), TEXT("Join") }, { TEXT("→"), TEXT("Watch") }, { TEXT("F"), TEXT("Filter") }, { TEXT("R"), TEXT("Refresh") } },
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
	// Damage is on by default: only another rule is worth the words.
	if (const FString Damage = ApexBrowser::DamageWords(Session.Damage); !Damage.IsEmpty())
	{
		Parts.Add(Damage);
	}
	// The race's pit rule, when it has one: "1 stop · two compounds".
	if (const FString PitRule = ApexMandatoryStops::Describe(Session.MandatoryStops, Session.bCompoundChange); !PitRule.IsEmpty())
	{
		Parts.Add(PitRule);
	}
	// Its race-control rules, when off their defaults: "formation lap ·
	// 2 penalties · safety car".
	if (const FString Rules = Session.RaceRules.Normalised().Describe(); !Rules.IsEmpty())
	{
		Parts.Add(Rules);
	}
	// A server that balances its cars drives them unlike their car.toml.
	if (const UApexNetSubsystem* Net = GetNet(); Net && Net->GetCachedLobbyState().bBalanced)
	{
		Parts.Add(TEXT("balanced cars"));
	}

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

void UApexSessionBrowserWidget::SetFilter(ApexBrowser::EFilter InFilter)
{
	Filter = InFilter;
	if (FilterPills)
	{
		FilterPills->SetSelectedIndex(static_cast<int32>(Filter));
	}
	// A different set of rows, from the top.
	Shown.Reset();
	LastFocused = 0;
	if (Filter == ApexBrowser::EFilter::Recorded)
	{
		RebuildRecordingRows();
		FetchRecordings();
	}
	else
	{
		List->ClearChildren();
		Rows.Reset();
		WatchButtons.Reset();
		RefreshList();
	}
}

void UApexSessionBrowserWidget::HandleFilterChosen(UApexSegmentedWidget* Control, int32 Index)
{
	SetFilter(static_cast<ApexBrowser::EFilter>(FMath::Clamp(Index, 0, static_cast<int32>(ApexBrowser::EFilter::Count) - 1)));
}

void UApexSessionBrowserWidget::FetchRecordings()
{
	const UApexNetSubsystem* Net = GetNet();
	const int32 Port = Net ? Net->GetCachedLobbyState().RecordingsPort : 0;
	if (!Net || Net->GetConnectionState() != EApexConnectionState::Authenticated)
	{
		RecordingsNote = TEXT("NOT CONNECTED: CONNECT TO A SERVER FROM THE MAIN MENU");
		RebuildRecordingRows();
		return;
	}
	if (Port <= 0)
	{
		RecordingsNote = TEXT("THIS SERVER DOES NOT SHARE ITS RECORDINGS");
		RebuildRecordingRows();
		return;
	}
	if (bFetchingRecordings)
	{
		return;
	}
	bFetchingRecordings = true;
	RecordingsNote = TEXT("ASKING THE SERVER FOR ITS RECORDINGS");
	RebuildRecordingRows();
	const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(FString::Printf(TEXT("http://%s:%d/recordings"), *Net->GetServerHost(), Port));
	Request->SetVerb(TEXT("GET"));
	Request->SetTimeout(10.0f);
	TWeakObjectPtr<UApexSessionBrowserWidget> Weak(this);
	Request->OnProcessRequestComplete().BindLambda([Weak](FHttpRequestPtr, FHttpResponsePtr Response, bool bOk)
	{
		UApexSessionBrowserWidget* Self = Weak.Get();
		if (!Self)
		{
			return;
		}
		Self->bFetchingRecordings = false;
		TArray<ApexBrowser::FRecording> Parsed;
		if (bOk && Response.IsValid() && Response->GetResponseCode() == 200
			&& ApexBrowser::ParseRecordings(Response->GetContentAsString(), Parsed))
		{
			Self->Recordings = MoveTemp(Parsed);
			Self->RecordingsNote = Self->Recordings.IsEmpty() ? TEXT("NO RECORDED SESSIONS ON THIS SERVER YET")
				: TEXT("ENTER WATCHES A RECORDING: IT IS FETCHED INTO YOUR REPLAYS FIRST");
		}
		else
		{
			Self->RecordingsNote = TEXT("THE SERVER'S RECORDINGS COULD NOT BE FETCHED (ITS HEALTH PORT MAY BE LOCAL ONLY)");
		}
		if (Self->Filter == ApexBrowser::EFilter::Recorded)
		{
			Self->RebuildRecordingRows();
		}
	});
	Request->ProcessRequest();
}

void UApexSessionBrowserWidget::RebuildRecordingRows()
{
	using namespace ApexUI;
	if (!List)
	{
		return;
	}
	const bool bHadFocus = HasFocusedDescendants();
	List->ClearChildren();
	Rows.Reset();
	WatchButtons.Reset();
	const UApexMenuFlowSubsystem* Flow = GetFlow();
	for (int32 Index = 0; Index < Recordings.Num(); ++Index)
	{
		const ApexBrowser::FRecording& R = Recordings[Index];
		// The catalog's display name; the file's `name` is the real circuit's
		// and never goes on screen (apexsim-content/docs/naming.md).
		FString TrackName = TEXT("A circuit not installed here");
		FApexTrackCatalogRow TrackRow;
		if (Flow && !R.TrackId.IsEmpty() && Flow->GetTrackCatalogRow(R.TrackId, TrackRow) && !TrackRow.DisplayName.IsEmpty())
		{
			TrackName = TrackRow.DisplayName;
		}
		TArray<FString> Parts;
		Parts.Add(FDateTime::FromUnixTimestamp(R.RecordedAt).ToString(TEXT("%Y-%m-%d %H:%M")));
		Parts.Add(R.Kind.IsEmpty() ? FString(TEXT("Session")) : R.Kind);
		Parts.Add(FString::Printf(TEXT("%d car%s"), R.Cars, R.Cars == 1 ? TEXT("") : TEXT("s")));
		if (R.Laps > 0)
		{
			Parts.Add(FString::Printf(TEXT("%d laps"), R.Laps));
		}
		Parts.Add(UApexMenuFlowSubsystem::FormatLapTime(static_cast<float>(R.DurationS)));
		if (!R.Winner.IsEmpty())
		{
			Parts.Add(FString::Printf(TEXT("won by %s"), *R.Winner));
		}
		const bool bHere = !R.File.IsEmpty() && IFileManager::Get().FileExists(*(UApexReplayRecorder::ReplayDirectory() / R.File));

		UApexButtonWidget* Row = WidgetTree->ConstructWidget<UApexButtonWidget>();
		Row->OnActivated.AddDynamic(this, &UApexSessionBrowserWidget::HandleButtonActivated);
		FApexButtonSpec Spec;
		Spec.Label = TrackName;
		Spec.SubLabel = FString::Join(Parts, TEXT("  ·  "));
		Spec.Badge = R.File.IsEmpty() ? TEXT("Old") : bHere ? TEXT("In replays") : TEXT("Recorded");
		Spec.BadgeColour = R.File.IsEmpty() ? Palette::TextMuted : bHere ? Palette::Live : Palette::Accent;
		Spec.Variant = R.File.IsEmpty() ? EApexButtonVariant::Locked : EApexButtonVariant::Panel;
		Spec.LabelSize = 20.0f;
		Spec.Height = RowHeight;
		Spec.ActionId = ActionBrowserRow;
		Row->Setup(Spec);
		Rows.Add(Row);
		List->AddChild(Row);
		if (UScrollBoxSlot* LineSlot = Cast<UScrollBoxSlot>(Row->Slot))
		{
			LineSlot->SetPadding(FMargin(0.0f, Index == 0 ? 0.0f : 6.0f, 0.0f, 0.0f));
		}
	}
	LastFocused = Rows.Num() > 0 ? FMath::Clamp(LastFocused, 0, Rows.Num() - 1) : 0;
	if (CountText)
	{
		CountText->SetText(FText::FromString(FString::Printf(TEXT("%d RECORDING%s"), Recordings.Num(), Recordings.Num() == 1 ? TEXT("") : TEXT("S"))));
	}
	if (StatusLine)
	{
		StatusLine->SetText(FText::FromString(bDownloading ? FString(TEXT("FETCHING THE RECORDING")) : RecordingsNote));
		StatusLine->SetVisibility(RecordingsNote.IsEmpty() && !bDownloading ? ESlateVisibility::Collapsed : ESlateVisibility::HitTestInvisible);
	}
	if (EmptyText)
	{
		EmptyText->SetVisibility(ESlateVisibility::Collapsed);
	}
	// Onto the first row once there are rows, as a filter switched by key
	// leaves the screen itself focused.
	if (IsActiveScreen() && (bHadFocus || HasAnyUserFocus()))
	{
		FocusDefault();
	}
}

void UApexSessionBrowserWidget::WatchRecording(int32 Index)
{
	const UApexNetSubsystem* Net = GetNet();
	if (!Recordings.IsValidIndex(Index) || bDownloading || !Net)
	{
		return;
	}
	const ApexBrowser::FRecording& R = Recordings[Index];
	if (R.File.IsEmpty())
	{
		ShowToast(TEXT("A recording from before streams: it cannot be watched here"), true);
		return;
	}
	const FString Path = UApexReplayRecorder::ReplayDirectory() / R.File;
	if (IFileManager::Get().FileExists(*Path))
	{
		if (UApexRootWidget* Root = GetRoot())
		{
			Root->WatchReplay(Path);
		}
		return;
	}
	bDownloading = true;
	RebuildRecordingRows();
	const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(FString::Printf(TEXT("http://%s:%d/recordings/%s"), *Net->GetServerHost(),
		Net->GetCachedLobbyState().RecordingsPort, *R.File));
	Request->SetVerb(TEXT("GET"));
	Request->SetTimeout(120.0f);
	TWeakObjectPtr<UApexSessionBrowserWidget> Weak(this);
	Request->OnProcessRequestComplete().BindLambda([Weak, Path](FHttpRequestPtr, FHttpResponsePtr Response, bool bOk)
	{
		UApexSessionBrowserWidget* Self = Weak.Get();
		if (!Self)
		{
			return;
		}
		Self->bDownloading = false;
		const bool bGot = bOk && Response.IsValid() && Response->GetResponseCode() == 200 && Response->GetContent().Num() > 8
			&& FFileHelper::SaveArrayToFile(Response->GetContent(), *Path);
		Self->RebuildRecordingRows();
		if (!bGot)
		{
			Self->ShowToast(TEXT("The recording could not be fetched"), true);
			return;
		}
		UE_LOG(LogApexSim, Log, TEXT("Session browser: fetched %s (%d bytes)"), *Path, Response->GetContent().Num());
		if (UApexRootWidget* Root = Self->GetRoot())
		{
			Root->WatchReplay(Path);
		}
	});
	Request->ProcessRequest();
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
	if (Filter == ApexBrowser::EFilter::Recorded)
	{
		// The recordings are fetched on asking, not with every lobby broadcast.
		return;
	}

	TArray<FApexSessionSummary> Sessions;
	for (const FApexSessionSummary& Session : Net->GetCachedLobbyState().AvailableSessions)
	{
		if (ApexBrowser::Passes(Session, Filter))
		{
			Sessions.Add(Session);
		}
	}
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
		EmptyText->SetText(FText::FromString(Filter == ApexBrowser::EFilter::All
			? TEXT("No sessions on this server yet. Create one, and drivers who connect will find it here.")
			: TEXT("No session here matches the filter.")));
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
			// (A recording's row has none.)
			if (WatchButtons.IsValidIndex(RowAt))
			{
				ApexNav::Focus(WatchButtons[RowAt]);
			}
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
	if (!InKeyEvent.IsRepeat() && (Key == EKeys::F || Key == EKeys::Gamepad_FaceButton_Left))
	{
		const int32 Count = static_cast<int32>(ApexBrowser::EFilter::Count);
		SetFilter(static_cast<ApexBrowser::EFilter>((static_cast<int32>(Filter) + (InKeyEvent.IsShiftDown() ? Count - 1 : 1)) % Count));
		return FReply::Handled();
	}
	return Super::NativeOnKeyDown(InGeometry, InKeyEvent);
}

void UApexSessionBrowserWidget::Refresh()
{
	if (Filter == ApexBrowser::EFilter::Recorded)
	{
		FetchRecordings();
		return;
	}
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
	if (Session.IsOver())
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
		if (Filter == ApexBrowser::EFilter::Recorded)
		{
			WatchRecording(RowAt);
		}
		else
		{
			Join(RowAt);
		}
	}
	else if (const int32 WatchAt = ApexNav::IndexOf(WatchButtons, Button); WatchAt != INDEX_NONE)
	{
		LastFocused = WatchAt;
		Watch(WatchAt);
	}
}
