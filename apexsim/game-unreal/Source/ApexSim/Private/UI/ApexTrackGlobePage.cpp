#include "UI/ApexTrackGlobePage.h"

#include "ApexMenuFlowSubsystem.h"
#include "ApexNetSubsystem.h"
#include "ApexSim.h"
#include "Blueprint/WidgetTree.h"
#include "Catalog/ApexCatalogRows.h"
#include "Components/Border.h"
#include "Components/HorizontalBox.h"
#include "Components/Overlay.h"
#include "Components/ScaleBox.h"
#include "Components/ScaleBoxSlot.h"
#include "Components/OverlaySlot.h"
#include "Components/ScrollBox.h"
#include "Components/SizeBox.h"
#include "Components/Spacer.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Guide/ApexTrackGuideSubsystem.h"
#include "Track/ApexTrackContentSubsystem.h"
#include "UI/ApexButtonWidget.h"
#include "UI/ApexGlobeWidget.h"
#include "UI/ApexUIStyle.h"

const FName UApexTrackGlobePage::ActionHotlap(TEXT("Hotlap"));
const FName UApexTrackGlobePage::ActionGuide(TEXT("Tracks"));
const FName UApexTrackGlobePage::ActionWatchHotlap(TEXT("WatchHotlap"));
const FName UApexTrackGlobePage::ActionLeaderboard(TEXT("Leaderboard"));
const FName UApexTrackGlobePage::ActionBack(TEXT("Back"));

namespace
{
	namespace GlobePageAction
	{
		const FName Row(TEXT("GlobeRow"));
		const FName ZoomIn(TEXT("GlobeZoomIn"));
		const FName ZoomOut(TEXT("GlobeZoomOut"));
		const FName Home(TEXT("GlobeHome"));
		const FName Close(TEXT("GlobeClose"));
	}

	constexpr float GlobeListWidth = 300.0f;
	constexpr float GlobePanelWidth = 420.0f;

	/** The page the console drives: the last one built (the main menu has one). */
	TWeakObjectPtr<UApexTrackGlobePage> GGlobePage;

	FAutoConsoleCommandWithWorldAndArgs GGlobeCommand(
		TEXT("apexsim.ui.Globe"),
		TEXT("Drive Garage > Tracks' globe (open the page first): apexsim.ui.Globe select <stem|id> | close | zoom <z> | view <lon> <lat> [zoom] | home | spin 0|1"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			UApexTrackGlobePage* Page = GGlobePage.Get();
			UApexGlobeWidget* Globe = Page ? Page->GetGlobe() : nullptr;
			if (!Globe || Args.Num() == 0)
			{
				UE_LOG(LogApexSim, Warning, TEXT("apexsim.ui.Globe: %s"), Globe ? TEXT("no command") : TEXT("no Tracks page"));
				return;
			}
			const FString Verb = Args[0].ToLower();
			ApexGlobe::FView& View = Globe->GetView();
			if (Verb == TEXT("select") && Args.Num() > 1)
			{
				// A stem (Hockeyheimring) or the track id itself.
				FString Id = Args[1];
				if (const UGameInstance* Game = Page->GetGameInstance())
				{
					if (const UApexTrackContentSubsystem* Tracks = Game->GetSubsystem<UApexTrackContentSubsystem>())
					{
						const FString ById = Tracks->FindRuntimeTrackIdByStem(Args[1]);
						Id = ById.IsEmpty() ? Id : ById;
					}
				}
				Page->Choose(Id);
			}
			else if (Verb == TEXT("close"))
			{
				Page->Choose(FString());
			}
			else if (Verb == TEXT("zoom") && Args.Num() > 1)
			{
				View.TargetZoom = FMath::Clamp(FCString::Atod(*Args[1]), ApexGlobe::MinZoom, ApexGlobe::MaxZoom);
			}
			else if (Verb == TEXT("view") && Args.Num() > 2)
			{
				ApexGlobe::FlyTo(View, FCString::Atod(*Args[1]), FCString::Atod(*Args[2]),
					Args.Num() > 3 ? FCString::Atod(*Args[3]) : View.TargetZoom);
			}
			else if (Verb == TEXT("home"))
			{
				Globe->ResetView();
				Page->Choose(FString());
			}
			else if (Verb == TEXT("spin") && Args.Num() > 1)
			{
				// Off holds the view still for a screenshot, as a hover does.
				Globe->SetIdleSpin(Args[1] != TEXT("0"));
			}
			UE_LOG(LogApexSim, Log, TEXT("apexsim.ui.Globe %s: lon %.1f lat %.1f zoom %.2f, chosen '%s'"),
				*FString::Join(Args, TEXT(" ")), View.LonDeg, View.LatDeg, View.TargetZoom, *Globe->GetSelectedId());
		}));
}

void UApexTrackGlobePage::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	BuildLayout();
	GGlobePage = this;
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

void UApexTrackGlobePage::BuildLayout()
{
	using namespace ApexUI;

	UHorizontalBox* Columns = WidgetTree->ConstructWidget<UHorizontalBox>();

	// The list: the quick jump, and the keys' and pad's way to every circuit.
	UVerticalBox* List = WidgetTree->ConstructWidget<UVerticalBox>();
	AddV(List, MakeText(*WidgetTree, TEXT("GARAGE  /  TRACKS"), Font::Mono(11.0f, 200), Palette::Accent), FMargin(0.0f, 0.0f, 0.0f, 14.0f), HAlign_Left);
	FApexButtonSpec BackSpec;
	BackSpec.Variant = EApexButtonVariant::Ghost;
	BackSpec.Label = TEXT("Back");
	BackSpec.ActionId = ActionBack;
	BackSpec.Height = 44.0f;
	BackButton = WidgetTree->ConstructWidget<UApexButtonWidget>();
	BackButton->Setup(BackSpec);
	BackButton->OnActivated.AddDynamic(this, &UApexTrackGlobePage::HandleButtonActivated);
	AddV(List, BackButton, FMargin(0.0f, 0.0f, 0.0f, 12.0f));
	ListBox = WidgetTree->ConstructWidget<UVerticalBox>();
	ListScroll = WidgetTree->ConstructWidget<UScrollBox>();
	ListScroll->AddChild(ListBox);
	AddV(List, ListScroll, FMargin(), HAlign_Fill, 1.0f);
	AddH(Columns, MakeSized(*WidgetTree, List, GlobeListWidth, -1.0f), FMargin(0.0f, 0.0f, 24.0f, 0.0f), VAlign_Fill);

	AddH(Columns, BuildStage(), FMargin(), VAlign_Fill, 1.0f);

	// The panel, shut until a circuit is chosen; its content is clipped while it slides.
	PanelContent = WidgetTree->ConstructWidget<UVerticalBox>();
	UBorder* PanelSurface = MakePanel(*WidgetTree, MakeSized(*WidgetTree, PanelContent, GlobePanelWidth, -1.0f),
		FMargin(), MakeBrush(Palette::Surface, Palette::Border, 1.0f));
	PanelSurface->SetHorizontalAlignment(HAlign_Left);
	PanelBox = MakeSized(*WidgetTree, PanelSurface, 0.0f, -1.0f);
	PanelBox->SetClipping(EWidgetClipping::ClipToBounds);
	PanelBox->SetVisibility(ESlateVisibility::Collapsed);
	AddH(Columns, PanelBox, FMargin(), VAlign_Fill);

	WidgetTree->RootWidget = Columns;
}

UWidget* UApexTrackGlobePage::BuildStage()
{
	using namespace ApexUI;

	UOverlay* Stage = WidgetTree->ConstructWidget<UOverlay>();
	UOverlaySlot* BackSlot = Stage->AddChildToOverlay(
		MakePanel(*WidgetTree, nullptr, FMargin(), MakeBrush(FLinearColor::FromSRGBColor(FColor(0x0B, 0x0B, 0x0D)))));
	BackSlot->SetHorizontalAlignment(HAlign_Fill);
	BackSlot->SetVerticalAlignment(VAlign_Fill);

	Globe = WidgetTree->ConstructWidget<UApexGlobeWidget>();
	Globe->OnSelectionChanged.AddDynamic(this, &UApexTrackGlobePage::HandleGlobeSelection);
	UOverlaySlot* GlobeSlot = Stage->AddChildToOverlay(Globe);
	GlobeSlot->SetHorizontalAlignment(HAlign_Fill);
	GlobeSlot->SetVerticalAlignment(VAlign_Fill);

	UTextBlock* Hint = MakeText(*WidgetTree, TEXT("DRAG: ROTATE    SCROLL: ZOOM    CLICK A PIN: DETAILS    STICK / TRIGGERS: ROTATE / ZOOM    Y: RESET"),
		Font::Mono(9.0f, 140), Palette::TextSecondary);
	UOverlaySlot* HintSlot = Stage->AddChildToOverlay(Hint);
	HintSlot->SetHorizontalAlignment(HAlign_Left);
	HintSlot->SetVerticalAlignment(VAlign_Bottom);
	HintSlot->SetPadding(FMargin(20.0f, 0.0f, 80.0f, 18.0f));

	// Zoom and reset for the mouse; keys and pad have their own.
	UVerticalBox* Controls = WidgetTree->ConstructWidget<UVerticalBox>();
	auto AddControl = [&](const TCHAR* Label, FName Id)
	{
		FApexButtonSpec Spec;
		Spec.Variant = EApexButtonVariant::Panel;
		Spec.Label = Label;
		Spec.ActionId = Id;
		Spec.bCentreLabel = true;
		Spec.bMouseOnly = true;
		Spec.Height = 40.0f;
		Spec.LabelSize = 14.0f;
		UApexButtonWidget* Button = WidgetTree->ConstructWidget<UApexButtonWidget>();
		Button->Setup(Spec);
		Button->OnActivated.AddDynamic(this, &UApexTrackGlobePage::HandleButtonActivated);
		AddV(Controls, MakeSized(*WidgetTree, Button, 64.0f, -1.0f), FMargin(0.0f, 0.0f, 0.0f, 6.0f));
		StageButtons.Add(Button);
	};
	AddControl(TEXT("+"), GlobePageAction::ZoomIn);
	AddControl(TEXT("-"), GlobePageAction::ZoomOut);
	AddControl(TEXT("Reset"), GlobePageAction::Home);
	UOverlaySlot* ControlsSlot = Stage->AddChildToOverlay(Controls);
	ControlsSlot->SetHorizontalAlignment(HAlign_Right);
	ControlsSlot->SetVerticalAlignment(VAlign_Bottom);
	ControlsSlot->SetPadding(FMargin(0.0f, 0.0f, 18.0f, 12.0f));
	return Stage;
}

// ---------------------------------------------------------------------------
// Data
// ---------------------------------------------------------------------------

void UApexTrackGlobePage::Refresh()
{
	UGameInstance* Game = GetGameInstance();
	const UApexNetSubsystem* Net = Game ? Game->GetSubsystem<UApexNetSubsystem>() : nullptr;
	const UApexMenuFlowSubsystem* Flow = Game ? Game->GetSubsystem<UApexMenuFlowSubsystem>() : nullptr;
	if (!Net || !Flow || !ListBox || !Globe)
	{
		return;
	}

	// The circuits the server offers, by the names the player sees (the
	// catalog's, else the server's), A to Z; a pin for each the catalog places.
	struct FEntry
	{
		FString Id;
		FString Name;
		FApexTrackCatalogRow Row;
		bool bRow = false;
	};
	TArray<FEntry> Entries;
	for (const FApexTrackConfigSummary& Track : Net->GetCachedLobbyState().TrackConfigs)
	{
		FEntry Entry;
		Entry.Id = Track.Id;
		Entry.bRow = Flow->GetTrackCatalogRow(Track.Id, Entry.Row);
		Entry.Name = Entry.bRow && !Entry.Row.DisplayName.IsEmpty() ? Entry.Row.DisplayName : Track.Name;
		Entries.Add(MoveTemp(Entry));
	}
	Entries.StableSort([](const FEntry& A, const FEntry& B) { return A.Name.Compare(B.Name, ESearchCase::IgnoreCase) < 0; });

	FString Key = Net->IsAuthenticated() ? TEXT("+") : TEXT("-");
	for (const FEntry& Entry : Entries)
	{
		Key += Entry.Id + Entry.Name + (Entry.bRow && Entry.Row.bHasGlobePosition ? TEXT("@") : TEXT(";"));
	}
	if (Key == ListKey)
	{
		return;
	}
	ListKey = Key;

	ListBox->ClearChildren();
	ListButtons.Reset();
	ListIds.Reset();
	TArray<FApexGlobePin> Pins;
	for (const FEntry& Entry : Entries)
	{
		FApexButtonSpec Spec;
		Spec.Variant = EApexButtonVariant::Panel;
		Spec.Label = Entry.Name;
		Spec.LabelSize = 17.0f;
		Spec.ActionId = GlobePageAction::Row;
		UApexButtonWidget* Button = WidgetTree->ConstructWidget<UApexButtonWidget>();
		Button->Setup(Spec);
		Button->SetSelected(Entry.Id == OpenId);
		Button->OnActivated.AddDynamic(this, &UApexTrackGlobePage::HandleButtonActivated);
		ApexUI::AddV(ListBox, Button, FMargin(0.0f, 0.0f, 0.0f, 6.0f));
		ListButtons.Add(Button);
		ListIds.Add(Entry.Id);

		if (Entry.bRow && Entry.Row.bHasGlobePosition)
		{
			FApexGlobePin Pin;
			Pin.Id = Entry.Id;
			Pin.Name = Entry.Name;
			Pin.LatDeg = Entry.Row.LatitudeDeg;
			Pin.LonDeg = Entry.Row.LongitudeDeg;
			Pins.Add(MoveTemp(Pin));
		}
	}
	if (ListButtons.Num() == 0)
	{
		ApexUI::AddV(ListBox, ApexUI::MakeText(*WidgetTree,
			Net->IsAuthenticated() ? TEXT("Nothing to show yet.") : TEXT("Connect to a server to see the circuits."),
			ApexUI::Font::Body(14.0f), ApexUI::Palette::TextMuted));
	}
	Globe->SetPins(Pins);

	if (!OpenId.IsEmpty() && !ListIds.Contains(OpenId))
	{
		Choose(FString());
	}
	else if (!OpenId.IsEmpty())
	{
		Globe->Select(OpenId, /*bFly*/ false);
		RebuildPanel();
	}
}

void UApexTrackGlobePage::Choose(const FString& TrackId, bool bFly)
{
	OpenId = TrackId;
	for (int32 i = 0; i < ListButtons.Num(); ++i)
	{
		ListButtons[i]->SetSelected(ListIds[i] == OpenId);
	}
	if (OpenId.IsEmpty())
	{
		if (Globe && !Globe->GetSelectedId().IsEmpty())
		{
			Globe->ClearSelection();
		}
		RebuildPanel();
		return;
	}

	// The circuit the rest of the menu acts on, as the old list made it.
	if (UApexMenuFlowSubsystem* Flow = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexMenuFlowSubsystem>() : nullptr)
	{
		if (Flow->GetPendingTrackId() != OpenId)
		{
			Flow->SetPendingTrack(OpenId);
		}
	}
	if (Globe && Globe->GetSelectedId() != OpenId)
	{
		Globe->Select(OpenId, bFly);
	}
	RebuildPanel();
}

void UApexTrackGlobePage::RebuildPanel()
{
	using namespace ApexUI;

	PanelContent->ClearChildren();
	PanelButtons.Reset();
	if (OpenId.IsEmpty())
	{
		return;
	}

	UGameInstance* Game = GetGameInstance();
	UApexNetSubsystem* Net = Game ? Game->GetSubsystem<UApexNetSubsystem>() : nullptr;
	const UApexMenuFlowSubsystem* Flow = Game ? Game->GetSubsystem<UApexMenuFlowSubsystem>() : nullptr;
	const UApexTrackGuideSubsystem* Guides = Game ? Game->GetSubsystem<UApexTrackGuideSubsystem>() : nullptr;
	FApexTrackCatalogRow Track;
	const bool bRow = Flow && Flow->GetTrackCatalogRow(OpenId, Track);
	FApexTrackConfigSummary Summary;
	const bool bSummary = Net && Net->FindTrackById(OpenId, Summary);

	UVerticalBox* Body = WidgetTree->ConstructWidget<UVerticalBox>();

	UHorizontalBox* Head = WidgetTree->ConstructWidget<UHorizontalBox>();
	AddH(Head, MakeText(*WidgetTree, TEXT("TRACK"), Font::Mono(11.0f, 200), Palette::Accent), FMargin(), VAlign_Center, 1.0f);
	FApexButtonSpec CloseSpec;
	CloseSpec.Variant = EApexButtonVariant::Bare;
	CloseSpec.Label = TEXT("Close");
	CloseSpec.ActionId = GlobePageAction::Close;
	CloseSpec.bMouseOnly = true;
	CloseSpec.Height = 36.0f;
	CloseSpec.LabelSize = 14.0f;
	UApexButtonWidget* Close = WidgetTree->ConstructWidget<UApexButtonWidget>();
	Close->Setup(CloseSpec);
	Close->OnActivated.AddDynamic(this, &UApexTrackGlobePage::HandleButtonActivated);
	AddH(Head, Close);
	AddV(Body, Head, FMargin(0.0f, 0.0f, 0.0f, 16.0f));

	// The circuit's map: its preview, the line drawn from its export by
	// build_track_catalog.py.
	AddV(Body, MakePreview(*WidgetTree, bRow ? UApexTrackContentSubsystem::PreviewOf(Track) : nullptr, TEXT("No map"), -1.0f, 190.0f));

	const FString Name = bRow && !Track.DisplayName.IsEmpty() ? Track.DisplayName : (bSummary ? Summary.Name : FString(TEXT("Circuit")));
	FString TitleHead;
	FString TitleTail;
	SplitTitle(Name, TitleHead, TitleTail);
	const int32 Length = TitleHead.Len() + TitleTail.Len();
	const float Size = Length > 22 ? 30.0f : (Length > 14 ? 38.0f : 48.0f);
	UHorizontalBox* TitleRow = WidgetTree->ConstructWidget<UHorizontalBox>();
	AddH(TitleRow, MakeText(*WidgetTree, TitleHead, Font::Display(Size), Palette::TextPrimary), FMargin(), VAlign_Bottom);
	AddH(TitleRow, MakeText(*WidgetTree, TitleTail, Font::Display(Size), Palette::TextDisabled), FMargin(), VAlign_Bottom);
	// A long name shrinks to the panel rather than running out of it.
	UScaleBox* TitleFit = WidgetTree->ConstructWidget<UScaleBox>();
	TitleFit->SetStretch(EStretch::ScaleToFit);
	TitleFit->SetStretchDirection(EStretchDirection::DownOnly);
	if (UScaleBoxSlot* FitSlot = Cast<UScaleBoxSlot>(TitleFit->AddChild(TitleRow)))
	{
		FitSlot->SetHorizontalAlignment(HAlign_Left);
	}
	AddV(Body, TitleFit, FMargin(0.0f, 22.0f, 0.0f, 8.0f), HAlign_Fill);

	TArray<FString> Meta;
	if (bRow)
	{
		if (!Track.Country.IsEmpty())  { Meta.Add(Track.Country.ToUpper()); }
		if (!Track.City.IsEmpty())     { Meta.Add(Track.City.ToUpper()); }
		if (!Track.Category.IsEmpty()) { Meta.Add(ApexCatalog::DisplayClass(Track.Category).ToUpper()); }
	}
	AddV(Body, MakeText(*WidgetTree, FString::Join(Meta, TEXT("  /  ")), Font::Mono(11.0f, 120), Palette::TextMuted));
	if (bRow && !Track.Description.IsEmpty())
	{
		UTextBlock* Description = MakeText(*WidgetTree, Track.Description, Font::Body(13.0f), Palette::TextSecondary);
		Description->SetAutoWrapText(true);
		AddV(Body, Description, FMargin(0.0f, 10.0f, 0.0f, 0.0f));
	}

	float Best = 0.0f;
	const float LengthM = bRow ? Track.LengthM : 0.0f;
	UHorizontalBox* Tiles = WidgetTree->ConstructWidget<UHorizontalBox>();
	const TPair<FString, FString> Stats[] = {
		{ TEXT("Length"), LengthM > 0.0f ? FString::Printf(TEXT("%.2f km"), LengthM / 1000.0f) : FString(TEXT("—")) },
		{ TEXT("Your best"), Flow && Flow->GetBestLapSeconds(OpenId, Best) ? UApexMenuFlowSubsystem::FormatLapTime(Best) : FString(TEXT("Never driven")) },
	};
	for (int32 i = 0; i < UE_ARRAY_COUNT(Stats); ++i)
	{
		UTextBlock* Value = nullptr;
		UWidget* Stat = MakeStat(*WidgetTree, Stats[i].Key, Value);
		Value->SetText(FText::FromString(Stats[i].Value));
		AddH(Tiles, MakePanel(*WidgetTree, Stat, FMargin(16.0f, 12.0f), MakeBrush(Palette::Background, Palette::Border, 1.0f)),
			FMargin(i == 0 ? 0.0f : 8.0f, 0.0f, 0.0f, 0.0f), VAlign_Fill, 1.0f);
	}
	AddV(Body, Tiles, FMargin(0.0f, 22.0f, 0.0f, 0.0f));

	// The rail's actions for this circuit: drive it, learn it, watch it, see its times.
	const bool bGuide = bRow && Guides && Guides->HasGuide(Track.YamlBaseName);
	auto MakeAction = [&](const TCHAR* Label, FName Id, EApexButtonVariant Variant)
	{
		FApexButtonSpec Spec;
		Spec.Variant = Variant;
		Spec.Label = Label;
		Spec.ActionId = Id;
		Spec.bCentreLabel = true;
		Spec.Height = 56.0f;
		UApexButtonWidget* Button = WidgetTree->ConstructWidget<UApexButtonWidget>();
		Button->Setup(Spec);
		Button->OnActivated.AddDynamic(this, &UApexTrackGlobePage::HandleButtonActivated);
		PanelButtons.Add(Button);
		return Button;
	};
	UHorizontalBox* Primary = WidgetTree->ConstructWidget<UHorizontalBox>();
	AddH(Primary, MakeAction(TEXT("Hotlap"), ActionHotlap, EApexButtonVariant::Primary), FMargin(), VAlign_Fill, 1.0f);
	AddH(Primary, MakeAction(bGuide ? TEXT("Track guide") : TEXT("No guide yet"), ActionGuide,
		bGuide ? EApexButtonVariant::Ghost : EApexButtonVariant::Locked), FMargin(8.0f, 0.0f, 0.0f, 0.0f), VAlign_Fill, 1.0f);
	AddV(Body, Primary, FMargin(0.0f, 22.0f, 0.0f, 8.0f));
	UHorizontalBox* Secondary = WidgetTree->ConstructWidget<UHorizontalBox>();
	AddH(Secondary, MakeAction(TEXT("Watch hotlap"), ActionWatchHotlap, EApexButtonVariant::Ghost), FMargin(), VAlign_Fill, 1.0f);
	AddH(Secondary, MakeAction(TEXT("Leaderboards"), ActionLeaderboard, EApexButtonVariant::Ghost), FMargin(8.0f, 0.0f, 0.0f, 0.0f), VAlign_Fill, 1.0f);
	AddV(Body, Secondary);

	UScrollBox* Scroll = WidgetTree->ConstructWidget<UScrollBox>();
	Scroll->AddChild(MakePanel(*WidgetTree, Body, FMargin(28.0f, 28.0f, 28.0f, 24.0f), MakeBrush(FLinearColor::Transparent)));
	AddV(PanelContent, Scroll, FMargin(), HAlign_Fill, 1.0f);
}

// ---------------------------------------------------------------------------
// Interaction
// ---------------------------------------------------------------------------

void UApexTrackGlobePage::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);

	// The panel slides in over about a third of a second, as the design's.
	const float Target = OpenId.IsEmpty() ? 0.0f : 1.0f;
	if (!FMath::IsNearlyEqual(PanelShown, Target, 0.001f) && PanelBox)
	{
		PanelShown = FMath::FInterpTo(PanelShown, Target, InDeltaTime, 9.0f);
		if (FMath::IsNearlyEqual(PanelShown, Target, 0.01f))
		{
			PanelShown = Target;
		}
		PanelBox->SetWidthOverride(GlobePanelWidth * PanelShown);
		PanelBox->SetVisibility(PanelShown > 0.0f ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);
	}
}

void UApexTrackGlobePage::HandleButtonActivated(UApexButtonWidget* Button)
{
	if (!Button)
	{
		return;
	}
	const FName Id = Button->GetActionId();
	if (Id == GlobePageAction::Row)
	{
		const int32 Index = ApexNav::IndexOf(ListButtons, Button);
		if (ListIds.IsValidIndex(Index))
		{
			Choose(ListIds[Index]);
		}
	}
	else if (Id == GlobePageAction::ZoomIn)  { Globe->StepZoom(1); }
	else if (Id == GlobePageAction::ZoomOut) { Globe->StepZoom(-1); }
	else if (Id == GlobePageAction::Home)
	{
		Globe->ResetView();
		Choose(FString());
	}
	else if (Id == GlobePageAction::Close)
	{
		Choose(FString());
	}
	else
	{
		OnAction.Broadcast(Id);
	}
}

void UApexTrackGlobePage::HandleGlobeSelection(const FString& TrackId)
{
	Choose(TrackId, /*bFly*/ true);
}

// ---------------------------------------------------------------------------
// Navigation
// ---------------------------------------------------------------------------

int32 UApexTrackGlobePage::RegionOf(const UWidget* Widget) const
{
	if (!Widget)
	{
		return INDEX_NONE;
	}
	if (Widget == BackButton.Get() || ApexNav::IndexOf(ListButtons, Widget) != INDEX_NONE)
	{
		return 0;
	}
	if (Widget == Globe.Get())
	{
		return 1;
	}
	if (ApexNav::IndexOf(PanelButtons, Widget) != INDEX_NONE)
	{
		return 2;
	}
	return INDEX_NONE;
}

bool UApexTrackGlobePage::FocusList(int32 Index)
{
	if (ListButtons.Num() == 0)
	{
		return ApexNav::Focus(BackButton);
	}
	if (Index == INDEX_NONE)
	{
		Index = FMath::Max(0, ListIds.IndexOfByKey(OpenId));
	}
	Index = FMath::Clamp(Index, 0, ListButtons.Num() - 1);
	ListScroll->ScrollWidgetIntoView(ListButtons[Index], false);
	return ApexNav::Focus(ListButtons[Index]);
}

bool UApexTrackGlobePage::FocusPanel()
{
	for (UApexButtonWidget* Button : PanelButtons)
	{
		if (ApexNav::Focus(Button))
		{
			return true;
		}
	}
	return false;
}

void UApexTrackGlobePage::FocusRegion(int32 Region)
{
	switch (Region)
	{
	case 1:
		if (Globe && Globe->FocusPin(OpenId))
		{
			return;
		}
		break;
	case 2:
		if (FocusPanel())
		{
			return;
		}
		break;
	default:
		break;
	}
	FocusList(INDEX_NONE);
}

void UApexTrackGlobePage::FocusDefault()
{
	// The globe when it has pins to land on; the list otherwise (offline, or
	// a server whose circuits the catalog cannot place).
	FocusRegion(Globe && Globe->HasPins() ? 1 : 0);
}

bool UApexTrackGlobePage::HandleNavigation(EUINavigation Direction, UWidget* Source)
{
	const int32 Region = RegionOf(Source);
	const bool bPanel = !OpenId.IsEmpty() && PanelButtons.Num() > 0;

	if (ApexNav::IsSequential(Direction))
	{
		// List, globe, panel (when open), round again.
		const int32 Count = bPanel ? 3 : 2;
		const int32 From = Region == INDEX_NONE ? 0 : Region;
		const int32 Step = Direction == EUINavigation::Next ? 1 : Count - 1;
		FocusRegion((From + Step) % Count);
		return true;
	}

	switch (Region)
	{
	case 0:
	{
		const int32 Row = ApexNav::IndexOf(ListButtons, Source);
		if (Direction == EUINavigation::Up)
		{
			if (Row > 0) { FocusList(Row - 1); }
			else         { ApexNav::Focus(BackButton); }
		}
		else if (Direction == EUINavigation::Down)
		{
			if (Row == INDEX_NONE) { FocusList(0); }
			else                   { FocusList(Row + 1); }
		}
		else if (Direction == EUINavigation::Right)
		{
			FocusRegion(1);
		}
		return true;
	}
	case 1:
		// The globe found no pin that way.
		if (Direction == EUINavigation::Left)
		{
			FocusList(INDEX_NONE);
		}
		else if (Direction == EUINavigation::Right && bPanel)
		{
			FocusPanel();
		}
		return true;
	case 2:
	{
		TArray<UWidget*> Candidates;
		for (UApexButtonWidget* Button : PanelButtons)
		{
			if (ApexNav::CanFocus(Button))
			{
				Candidates.Add(Button);
			}
		}
		if (UWidget* Next = ApexNav::NearestToward(Direction, Source, Candidates))
		{
			ApexNav::Focus(Next);
		}
		else if (Direction == EUINavigation::Left)
		{
			FocusRegion(1);
		}
		return true;
	}
	default:
		FocusDefault();
		return true;
	}
}

bool UApexTrackGlobePage::HandleBack()
{
	if (OpenId.IsEmpty())
	{
		// Up a page: the main menu's.
		return false;
	}
	TArray<UWidget*> Panel;
	for (UApexButtonWidget* Button : PanelButtons)
	{
		Panel.Add(Button);
	}
	const bool bWasInPanel = ApexNav::FocusedAmong(Panel) != nullptr;
	Choose(FString());
	if (bWasInPanel)
	{
		FocusRegion(1);
	}
	return true;
}
