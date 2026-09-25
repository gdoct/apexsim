#include "UI/ApexHudWidget.h"

#include "ApexMenuFlowSubsystem.h"
#include "ApexNetSubsystem.h"
#include "ApexSettingsSubsystem.h"
#include "ApexSim.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/ProgressBar.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Engine/GameInstance.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Race/ApexRaceCoordinate.h"
#include "Race/ApexRaceDirector.h"
#include "UI/ApexMinimapWidget.h"
#include "UI/ApexMirrorWidget.h"
#include "UI/ApexUIStyle.h"

// The HUD is nothing but style primitives; qualifying every one of them would
// double the length of each layout line without adding any information.
using namespace ApexUI;

namespace
{
	/** Rows the standings panel shows. Five is what fits without crowding. */
	constexpr int32 StandingRowCount = 5;
	constexpr float StandingRowHeight = 43.0f;
	constexpr float StandingsWidth = 366.0f;

	/** Lit blocks in the rev counter. */
	constexpr int32 RpmSegmentCount = 30;
	/** The last few segments are the shift light. */
	constexpr int32 RpmRedSegments = 5;

	constexpr float MinimapSize = 250.0f;
	constexpr float HudEdgeGutter = 30.0f;
	/** The virtual mirror's glass, screen pixels; the same 3.2:1 as the capture. */
	constexpr float VirtualMirrorWidth = 480.0f;
	constexpr float VirtualMirrorHeight = 150.0f;
	/** Clearance under the race-state strip, which is about 80 px tall. */
	constexpr float VirtualMirrorTop = HudEdgeGutter + 96.0f;

	/**
	 * Bars in the sector strip. The server splits a lap into three
	 * (`laps::SECTOR_COUNT`); the strip is built for that many and the board
	 * says how many actually arrived.
	 */
	constexpr int32 SectorCount = 3;

	/** Purple in every sim: a time nobody in the session has beaten. */
	const FLinearColor SessionBestColour = FLinearColor::FromSRGBColor(FColor(0xB0, 0x7C, 0xE8));

	/** A driver's own best, green — the colour a timing screen uses for it. */
	const FLinearColor PersonalBestColour = Palette::Live;

	/** A sector the driver has been quicker through before. */
	const FLinearColor SlowerColour = Palette::Accent;

	/** Milliseconds as "27.431", the way a split is read. */
	FString FormatSplit(int32 Ms)
	{
		return Ms > 0 ? FString::Printf(TEXT("%.3f"), Ms / 1000.0f) : TEXT("--.---");
	}

	/** A car's colour on the map. Distinct enough at four pixels across. */
	FLinearColor BlipColour(int32 CarIndex, bool bIsLocal)
	{
		if (bIsLocal)
		{
			return Palette::Accent;
		}
		// Hue by index rather than a palette lookup: the field size is not known
		// until the roster lands, and every car has to get a colour.
		return FLinearColor::MakeFromHSV8(static_cast<uint8>((CarIndex * 47) % 255), 140, 235);
	}

	/** "1:32.104" — the same shape the results screen uses. */
	FString FormatTime(float Seconds)
	{
		if (Seconds <= 0.0f)
		{
			return TEXT("--:--.---");
		}
		const int32 Minutes = FMath::FloorToInt(Seconds / 60.0f);
		const float Remainder = Seconds - Minutes * 60.0f;
		return FString::Printf(TEXT("%d:%06.3f"), Minutes, Remainder);
	}
}

UApexHudWidget::UApexHudWidget(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// Collapsed until a race starts.
	//
	// Not HitTestInvisible-by-default: the shell only calls SetRaceActive when
	// the race view *changes*, so a HUD that starts visible is never told to go
	// away and sits on top of every menu screen from launch. HitTestInvisible is
	// what it switches to once racing — clicks have to reach the race view.
	SetVisibility(ESlateVisibility::Collapsed);
}

// --- Subsystems -------------------------------------------------------------

UApexNetSubsystem* UApexHudWidget::GetNet() const
{
	const UGameInstance* GameInstance = GetGameInstance();
	return GameInstance ? GameInstance->GetSubsystem<UApexNetSubsystem>() : nullptr;
}

UApexSettingsSubsystem* UApexHudWidget::GetSettings() const
{
	const UGameInstance* GameInstance = GetGameInstance();
	return GameInstance ? GameInstance->GetSubsystem<UApexSettingsSubsystem>() : nullptr;
}

UApexMenuFlowSubsystem* UApexHudWidget::GetFlow() const
{
	const UGameInstance* GameInstance = GetGameInstance();
	return GameInstance ? GameInstance->GetSubsystem<UApexMenuFlowSubsystem>() : nullptr;
}

// --- Lifecycle --------------------------------------------------------------

void UApexHudWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	BuildHud();
}

void UApexHudWidget::NativeConstruct()
{
	Super::NativeConstruct();

	if (UApexSettingsSubsystem* Settings = GetSettings())
	{
		Settings->OnSettingsChanged.AddDynamic(this, &UApexHudWidget::HandleSettingsChanged);
	}
	if (UApexNetSubsystem* Net = GetNet())
	{
		Net->OnTelemetry.AddDynamic(this, &UApexHudWidget::HandleTelemetry);
	}
}

void UApexHudWidget::NativeDestruct()
{
	if (UApexSettingsSubsystem* Settings = GetSettings())
	{
		Settings->OnSettingsChanged.RemoveDynamic(this, &UApexHudWidget::HandleSettingsChanged);
	}
	if (UApexNetSubsystem* Net = GetNet())
	{
		Net->OnTelemetry.RemoveDynamic(this, &UApexHudWidget::HandleTelemetry);
	}

	Super::NativeDestruct();
}

void UApexHudWidget::SetRaceActive(bool bActive)
{
	bRaceActive = bActive;

	const UApexSettingsSubsystem* Settings = GetSettings();
	const EApexHudDetail Detail = Settings && Settings->Get() ? Settings->Get()->HudDetail : EApexHudDetail::All;

	SetVisibility(bActive && bShownWanted && Detail != EApexHudDetail::Hidden
		? ESlateVisibility::HitTestInvisible
		: ESlateVisibility::Collapsed);

	if (bActive)
	{
		// A new race starts with no history: keeping the previous session's
		// reference lap would show a delta against a lap of a different circuit.
		LapSamples.Reset();
		ReferenceLap.Reset();
		LastSeenLap = 0;
		ReferenceLapSeconds = 0.0f;
		ObservedMaxRpm = 8000.0f;
		HeaderGameMode = EApexGameMode::Lobby;

		// The outline is only fetched while the map is empty, and the HUD lives
		// on from one race to the next: without this, a second race on another
		// circuit kept drawing the first one's shape under the blips.
		if (Minimap)
		{
			Minimap->SetCenterline(TArray<FVector2D>());
		}

		RefreshHeader();
	}
}

void UApexHudWidget::HandleSettingsChanged(EApexSettingsGroup Group)
{
	if (Group != EApexSettingsGroup::Gameplay)
	{
		return;
	}

	const UApexSettingsSubsystem* Settings = GetSettings();
	const EApexHudDetail Detail = Settings && Settings->Get() ? Settings->Get()->HudDetail : EApexHudDetail::All;

	if (Detail != BuiltDetail)
	{
		BuildHud();
	}
	// Visibility depends on the detail level, and so does whether the minimap
	// exists at all, so re-apply it either way.
	SetRaceActive(bRaceActive);
}

void UApexHudWidget::HandleTelemetry(const FApexTelemetryFrame& Frame)
{
	if (!bRaceActive)
	{
		return;
	}

	const UApexNetSubsystem* Net = GetNet();
	if (!Net)
	{
		return;
	}

	// Lap bookkeeping is driven off the frame rather than the tick: a lap
	// rolling over is an event on the stream, and sampling it at frame rate
	// would miss the frame the counter moved on a slow client.
	const int32 LocalIndex = Net->GetLocalCarIndex();
	if (const FApexCarTelemetry* Local = Frame.Cars.FindByPredicate(
			[LocalIndex](const FApexCarTelemetry& Car) { return Car.CarIndex == LocalIndex; }))
	{
		UpdateDeltaReference(*Local);
	}
}

void UApexHudWidget::SetShown(bool bShown)
{
	if (bShownWanted == bShown)
	{
		return;
	}
	bShownWanted = bShown;
	const UApexSettingsSubsystem* Settings = GetSettings();
	const EApexHudDetail Detail = Settings && Settings->Get() ? Settings->Get()->HudDetail : EApexHudDetail::All;
	SetVisibility(bRaceActive && bShownWanted && Detail != EApexHudDetail::Hidden
		? ESlateVisibility::HitTestInvisible
		: ESlateVisibility::Collapsed);
}

void UApexHudWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);

	if (!bRaceActive || GetVisibility() == ESlateVisibility::Collapsed)
	{
		return;
	}

	RefreshRaceState();
	RefreshStandings();
	RefreshCarState();
	RefreshDelta();
	RefreshSectors();
	RefreshMinimap();
	RefreshVirtualMirror();
}

// --- Construction -----------------------------------------------------------

void UApexHudWidget::BuildHud()
{
	const UApexSettingsSubsystem* Settings = GetSettings();
	BuiltDetail = Settings && Settings->Get() ? Settings->Get()->HudDetail : EApexHudDetail::All;
	const bool bFull = BuiltDetail == EApexHudDetail::All;

	// Every cached pointer is about to dangle.
	StandingSlots.Reset();
	StandingRows.Reset();
	StandingPlace.Reset();
	StandingName.Reset();
	StandingTime.Reset();
	SectorBars.Reset();
	SectorTimes.Reset();
	RpmSegments.Reset();
	DrsBadge = nullptr;
	DrsText = nullptr;
	LapInvalidText = nullptr;
	FastestLapName = nullptr;
	FastestLapTime = nullptr;
	Minimap = nullptr;
	ThrottleBar = nullptr;
	BrakeBar = nullptr;

	RootStack = WidgetTree->ConstructWidget<UVerticalBox>();

	AddV(RootStack, BuildTopBar(), FMargin(Metrics::PageGutter, HudEdgeGutter, Metrics::PageGutter, 0.0f));

	// The middle of the screen is the driver's sight line into the next corner.
	// Nothing goes here, ever.
	AddV(RootStack, WidgetTree->ConstructWidget<UHorizontalBox>(), FMargin(), HAlign_Fill, 1.0f);

	UHorizontalBox* BottomRow = WidgetTree->ConstructWidget<UHorizontalBox>();

	if (bFull)
	{
		AddH(BottomRow, BuildMinimapPanel(), FMargin(0.0f, 0.0f, 22.0f, 0.0f), VAlign_Bottom);
	}

	AddH(BottomRow, BuildStandingsPanel(), FMargin(), VAlign_Bottom);
	AddH(BottomRow, BuildDeltaPanel(), FMargin(24.0f, 0.0f, 0.0f, 0.0f), VAlign_Bottom);

	// Filler pushes the car's own numbers to the right-hand edge.
	AddH(BottomRow, WidgetTree->ConstructWidget<UHorizontalBox>(), FMargin(), VAlign_Bottom, 1.0f);

	if (bFull)
	{
		AddH(BottomRow, BuildPedalPanel(), FMargin(0.0f, 0.0f, 18.0f, 0.0f), VAlign_Bottom);
	}

	AddH(BottomRow, BuildCarStatePanel(), FMargin(), VAlign_Bottom);

	AddV(RootStack, BottomRow, FMargin(Metrics::PageGutter, 0.0f, Metrics::PageGutter, HudEdgeGutter));

	// The virtual mirror floats over the stack rather than living in it: it
	// comes and goes with a setting and a capture, and the layout underneath
	// must not shift when it does.
	UOverlay* Layers = WidgetTree->ConstructWidget<UOverlay>();
	UOverlaySlot* StackSlot = Layers->AddChildToOverlay(RootStack);
	StackSlot->SetHorizontalAlignment(HAlign_Fill);
	StackSlot->SetVerticalAlignment(VAlign_Fill);

	VirtualMirror = WidgetTree->ConstructWidget<UApexMirrorWidget>();
	VirtualMirror->SetFaceSize(FVector2D(VirtualMirrorWidth, VirtualMirrorHeight));
	VirtualMirror->SetVisibility(ESlateVisibility::Collapsed);
	UOverlaySlot* MirrorSlot = Layers->AddChildToOverlay(VirtualMirror);
	MirrorSlot->SetHorizontalAlignment(HAlign_Center);
	MirrorSlot->SetVerticalAlignment(VAlign_Top);
	MirrorSlot->SetPadding(FMargin(0.0f, VirtualMirrorTop, 0.0f, 0.0f));

	WidgetTree->RootWidget = Layers;
}

void UApexHudWidget::RefreshVirtualMirror()
{
	if (!VirtualMirror)
	{
		return;
	}
	// The director owns the capture; it hands out a texture only while the
	// setting is on and a car is being followed.
	const AApexRaceDirector* Director = AApexRaceDirector::Find(this);
	UTextureRenderTarget2D* Texture = Director ? Director->GetVirtualMirrorTexture() : nullptr;
	VirtualMirror->SetTexture(Texture);
	const ESlateVisibility Wanted = Texture ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed;
	if (VirtualMirror->GetVisibility() != Wanted)
	{
		VirtualMirror->SetVisibility(Wanted);
	}
}

UWidget* UApexHudWidget::BuildTopBar()
{
	UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();

	// Left: which circuit, and what kind of session it is.
	UVerticalBox* Identity = WidgetTree->ConstructWidget<UVerticalBox>();
	TrackNameText = MakeText(*WidgetTree, TEXT("—"), Font::Display(30.0f, 20), Palette::TextPrimary);
	AddV(Identity, TrackNameText);
	SessionLineText = MakeLabel(*WidgetTree, TEXT("—"), Palette::TextSecondary);
	AddV(Identity, SessionLineText, FMargin(0.0f, 8.0f, 0.0f, 0.0f));
	AddH(Row, Identity, FMargin(), VAlign_Top, 1.0f);

	AddH(Row, BuildRaceStateStrip(), FMargin(), VAlign_Top);

	// Right: connection health and the way out.
	UHorizontalBox* Status = WidgetTree->ConstructWidget<UHorizontalBox>();
	UBorder* Dot = nullptr;
	AddH(Status, MakeDot(*WidgetTree, Palette::TextMuted, 8.0f, &Dot));
	PingDot = Dot;
	PingText = MakeText(*WidgetTree, TEXT("-- ms"), Font::Mono(11.0f, 40), Palette::TextSecondary);
	AddH(Status, PingText, FMargin(9.0f, 0.0f, 0.0f, 0.0f));
	CarCountText = MakeText(*WidgetTree, TEXT("0 CARS"), Font::Mono(11.0f, 40), Palette::TextMuted);
	AddH(Status, CarCountText, FMargin(20.0f, 0.0f, 0.0f, 0.0f));
	AddH(Status, MakeKeyCap(*WidgetTree, TEXT("ESC MENU")), FMargin(20.0f, 0.0f, 0.0f, 0.0f));

	// Both outer cells fill equally, so the state strip stays centred on screen
	// however long the circuit's name turns out to be.
	if (UHorizontalBoxSlot* StatusSlot = AddH(Row, Status, FMargin(), VAlign_Top, 1.0f))
	{
		StatusSlot->SetHorizontalAlignment(HAlign_Right);
	}

	return Row;
}

UWidget* UApexHudWidget::BuildRaceStateStrip()
{
	UHorizontalBox* Strip = WidgetTree->ConstructWidget<UHorizontalBox>();

	// Position, on the accent — the one number a driver looks for first.
	{
		UVerticalBox* Box = WidgetTree->ConstructWidget<UVerticalBox>();
		FLinearColor Caption = Palette::OnAccent;
		Caption.A = 0.7f;
		AddV(Box, MakeLabel(*WidgetTree, TEXT("Pos"), Caption), FMargin(), HAlign_Center);

		UHorizontalBox* Value = WidgetTree->ConstructWidget<UHorizontalBox>();
		PositionText = MakeText(*WidgetTree, TEXT("-"), Font::Display(34.0f), Palette::OnAccent);
		AddH(Value, PositionText, FMargin(), VAlign_Bottom);
		PositionOfText = MakeText(*WidgetTree, TEXT("/-"), Font::Mono(13.0f), Caption);
		AddH(Value, PositionOfText, FMargin(2.0f, 0.0f, 0.0f, 4.0f), VAlign_Bottom);
		AddV(Box, Value, FMargin(0.0f, 2.0f, 0.0f, 0.0f), HAlign_Center);

		UBorder* Panel = MakePanel(*WidgetTree, Box, FMargin(20.0f, 9.0f), MakeBrush(Palette::Accent));
		AddH(Strip, MakeSized(*WidgetTree, Panel, -1.0f, 76.0f), FMargin(), VAlign_Fill);
	}

	// Lap.
	{
		UVerticalBox* Box = WidgetTree->ConstructWidget<UVerticalBox>();
		AddV(Box, MakeLabel(*WidgetTree, TEXT("Lap")), FMargin(), HAlign_Center);

		UHorizontalBox* Value = WidgetTree->ConstructWidget<UHorizontalBox>();
		LapText = MakeText(*WidgetTree, TEXT("-"), Font::Display(34.0f), Palette::TextPrimary);
		AddH(Value, LapText, FMargin(), VAlign_Bottom);
		LapOfText = MakeText(*WidgetTree, TEXT("/-"), Font::Mono(13.0f), Palette::TextMuted);
		AddH(Value, LapOfText, FMargin(2.0f, 0.0f, 0.0f, 4.0f), VAlign_Bottom);
		AddV(Box, Value, FMargin(0.0f, 2.0f, 0.0f, 0.0f), HAlign_Center);

		UBorder* Panel = MakePanel(*WidgetTree, Box, FMargin(20.0f, 9.0f), MakeBrush(Palette::Surface));
		AddH(Strip, MakeSized(*WidgetTree, Panel, -1.0f, 76.0f), FMargin(2.0f, 0.0f, 0.0f, 0.0f), VAlign_Fill);
	}

	// Gap to the car in front, then to the one behind. Both name their rival:
	// a bare number does not tell you who you are racing.
	// The out-params are raw pointers, not the TObjectPtr members: a
	// TObjectPtr<T> does not bind to a T*& and assigning after the call is
	// cheaper than a wrapper for every one of these builders.
	auto AddGapTile = [this, Strip](const TCHAR* Caption, UTextBlock*& OutLabel, UTextBlock*& OutValue)
	{
		UVerticalBox* Box = WidgetTree->ConstructWidget<UVerticalBox>();
		OutLabel = MakeLabel(*WidgetTree, Caption);
		AddV(Box, OutLabel, FMargin(), HAlign_Center);
		OutValue = MakeText(*WidgetTree, TEXT("—"), Font::Mono(23.0f), Palette::TextSecondary);
		AddV(Box, OutValue, FMargin(0.0f, 5.0f, 0.0f, 0.0f), HAlign_Center);

		UBorder* Panel = MakePanel(*WidgetTree, Box, FMargin(20.0f, 9.0f), MakeBrush(Palette::Surface));
		AddH(Strip, MakeSized(*WidgetTree, Panel, 200.0f, 76.0f), FMargin(2.0f, 0.0f, 0.0f, 0.0f), VAlign_Fill);
	};

	UTextBlock* AheadCaption = nullptr;
	UTextBlock* AheadNumber = nullptr;
	AddGapTile(TEXT("Ahead"), AheadCaption, AheadNumber);
	AheadLabel = AheadCaption;
	AheadValue = AheadNumber;

	UTextBlock* BehindCaption = nullptr;
	UTextBlock* BehindNumber = nullptr;
	AddGapTile(TEXT("Behind"), BehindCaption, BehindNumber);
	BehindLabel = BehindCaption;
	BehindValue = BehindNumber;

	return Strip;
}

UWidget* UApexHudWidget::BuildStandingsPanel()
{
	UVerticalBox* Stack = WidgetTree->ConstructWidget<UVerticalBox>();

	UBorder* Header = MakePanel(
		*WidgetTree,
		MakeLabel(*WidgetTree, TEXT("Standings")),
		FMargin(16.0f, 9.0f),
		MakeBrush(Palette::SurfaceHover));
	AddV(Stack, Header);

	for (int32 Index = 0; Index < StandingRowCount; ++Index)
	{
		UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();

		UTextBlock* Place = MakeText(*WidgetTree, FString::FromInt(Index + 1), Font::Mono(12.0f), Palette::TextMuted);
		AddH(Row, MakeSized(*WidgetTree, Place, 22.0f, -1.0f));

		UTextBlock* Name = MakeText(*WidgetTree, FString(), Font::Body(14.0f), Palette::TextPrimary);
		AddH(Row, Name, FMargin(14.0f, 0.0f, 0.0f, 0.0f), VAlign_Center, 1.0f);

		UTextBlock* Time = MakeText(*WidgetTree, FString(), Font::Mono(13.0f), Palette::TextSecondary);
		AddH(Row, Time, FMargin(10.0f, 0.0f, 0.0f, 0.0f));

		UBorder* Background = MakePanel(*WidgetTree, Row, FMargin(16.0f, 0.0f), MakeBrush(Palette::Surface));
		Background->SetVerticalAlignment(VAlign_Center);
		USizeBox* Sized = MakeSized(*WidgetTree, Background, -1.0f, StandingRowHeight);
		AddV(Stack, Sized, FMargin(0.0f, 1.0f, 0.0f, 0.0f));

		// The size box, not the border, is what gets hidden: collapsing the
		// border alone would leave its row height behind as a gap.
		StandingSlots.Add(Sized);
		StandingRows.Add(Background);
		StandingPlace.Add(Place);
		StandingName.Add(Name);
		StandingTime.Add(Time);
	}

	// The session's fastest lap, the way a timing screen carries it: who, and
	// what. Purple, because nobody in the session has beaten it.
	UHorizontalBox* Fastest = WidgetTree->ConstructWidget<UHorizontalBox>();
	FastestLapName = MakeText(*WidgetTree, TEXT("FASTEST —"), Font::Body(12.0f), Palette::TextMuted);
	AddH(Fastest, FastestLapName);
	FastestLapTime = MakeText(*WidgetTree, TEXT("--:--.---"), Font::Mono(12.0f), SessionBestColour);
	AddH(Fastest, FastestLapTime, FMargin(10.0f, 0.0f, 0.0f, 0.0f));
	AddV(Stack, Fastest, FMargin(0.0f, 8.0f, 0.0f, 0.0f));

	return MakeSized(*WidgetTree, Stack, StandingsWidth, -1.0f);
}

UWidget* UApexHudWidget::BuildDeltaPanel()
{
	UVerticalBox* Stack = WidgetTree->ConstructWidget<UVerticalBox>();

	// Sector strip: one bar and one split per sector, coloured by the server's
	// own verdict on the time (purple session best, green personal best).
	UHorizontalBox* Sectors = WidgetTree->ConstructWidget<UHorizontalBox>();
	for (int32 Index = 0; Index < SectorCount; ++Index)
	{
		UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>();
		UBorder* Bar = MakePanel(*WidgetTree, nullptr, FMargin(), MakeBrush(Palette::Border));
		AddV(Column, MakeSized(*WidgetTree, Bar, 62.0f, 5.0f));
		UTextBlock* Split = MakeText(*WidgetTree, TEXT("--.---"), Font::Mono(12.0f), Palette::TextMuted);
		AddV(Column, Split, FMargin(0.0f, 4.0f, 0.0f, 0.0f));
		AddH(Sectors, Column, FMargin(Index == 0 ? 0.0f : 4.0f, 0.0f, 0.0f, 0.0f));
		SectorBars.Add(Bar);
		SectorTimes.Add(Split);
	}
	AddV(Stack, Sectors, FMargin(0.0f, 0.0f, 0.0f, 10.0f), HAlign_Left);

	// The lap was struck for leaving the track. Hidden while it is clean, so
	// the panel does not carry an empty row around all race.
	LapInvalidText = MakeText(*WidgetTree, TEXT("LAP INVALID"), Font::Body(14.0f, true), Palette::Error);
	LapInvalidText->SetVisibility(ESlateVisibility::Collapsed);
	AddV(Stack, LapInvalidText, FMargin(0.0f, 0.0f, 0.0f, 8.0f), HAlign_Left);

	UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();
	AddH(Row, MakeLabel(*WidgetTree, TEXT("Delta")));
	DeltaValue = MakeText(*WidgetTree, TEXT("—"), Font::Mono(25.0f), Palette::TextSecondary);
	AddH(Row, DeltaValue, FMargin(16.0f, 0.0f, 0.0f, 0.0f));

	UBorder* Panel = MakePanel(*WidgetTree, Row, FMargin(18.0f, 13.0f), MakeBrush(Palette::Surface));
	Panel->SetHorizontalAlignment(HAlign_Left);
	AddV(Stack, Panel, FMargin(), HAlign_Left);

	return Stack;
}

UWidget* UApexHudWidget::BuildPedalPanel()
{
	UHorizontalBox* Bars = WidgetTree->ConstructWidget<UHorizontalBox>();

	auto AddPedal = [this, Bars](const TCHAR* Caption, const FLinearColor& Fill, UProgressBar*& OutBar)
	{
		OutBar = WidgetTree->ConstructWidget<UProgressBar>();
		FProgressBarStyle Style = OutBar->GetWidgetStyle();
		Style.SetBackgroundImage(MakeBrush(Palette::Surface));
		Style.SetFillImage(MakeBrush(Fill));
		OutBar->SetWidgetStyle(Style);
		// Fills from the bottom, like a pedal travelling.
		OutBar->SetBarFillType(EProgressBarFillType::BottomToTop);
		OutBar->SetPercent(0.0f);

		UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>();
		AddV(Column, MakeSized(*WidgetTree, OutBar, 13.0f, 108.0f), FMargin(), HAlign_Center);
		AddV(Column, MakeLabel(*WidgetTree, Caption), FMargin(0.0f, 7.0f, 0.0f, 0.0f), HAlign_Center);

		AddH(Bars, Column, FMargin(4.0f, 0.0f), VAlign_Bottom);
	};

	UProgressBar* Throttle = nullptr;
	UProgressBar* Brake = nullptr;
	AddPedal(TEXT("Thr"), Palette::Live, Throttle);
	AddPedal(TEXT("Brk"), Palette::Error, Brake);
	ThrottleBar = Throttle;
	BrakeBar = Brake;

	return Bars;
}

UWidget* UApexHudWidget::BuildCarStatePanel()
{
	UVerticalBox* Stack = WidgetTree->ConstructWidget<UVerticalBox>();

	// Rev counter: a caption row, then the segment strip.
	UHorizontalBox* RpmHeader = WidgetTree->ConstructWidget<UHorizontalBox>();
	AddH(RpmHeader, MakeLabel(*WidgetTree, TEXT("Rpm")));
	// The DRS light, as the steering wheel's: dark until the car is in a
	// zone it may use, lit when it may, bright with the flap open.
	DrsText = MakeText(*WidgetTree, TEXT("DRS"), Font::Body(12.0f, true), Palette::TextDisabled);
	DrsBadge = MakePanel(*WidgetTree, DrsText, FMargin(8.0f, 1.0f), MakeBrush(Palette::Border));
	AddH(RpmHeader, DrsBadge, FMargin(12.0f, 0.0f, 0.0f, 0.0f), VAlign_Center);
	AddH(RpmHeader, WidgetTree->ConstructWidget<UHorizontalBox>(), FMargin(), VAlign_Center, 1.0f);
	RpmText = MakeText(*WidgetTree, TEXT("0"), Font::Mono(12.0f, 40), Palette::TextSecondary);
	AddH(RpmHeader, RpmText);
	AddV(Stack, RpmHeader);

	UHorizontalBox* Segments = WidgetTree->ConstructWidget<UHorizontalBox>();
	for (int32 Index = 0; Index < RpmSegmentCount; ++Index)
	{
		UBorder* Segment = MakePanel(*WidgetTree, nullptr, FMargin(), MakeBrush(Palette::Border));
		AddH(Segments, MakeSized(*WidgetTree, Segment, 10.0f, 14.0f), FMargin(0.0f, 0.0f, 2.0f, 0.0f));
		RpmSegments.Add(Segment);
	}
	AddV(Stack, Segments, FMargin(0.0f, 8.0f, 0.0f, 0.0f));

	// Gear on the left, speed on the right — the two numbers read at a glance.
	UHorizontalBox* Numbers = WidgetTree->ConstructWidget<UHorizontalBox>();

	UVerticalBox* GearBox = WidgetTree->ConstructWidget<UVerticalBox>();
	AddV(GearBox, MakeLabel(*WidgetTree, TEXT("Gear")));
	GearText = MakeText(*WidgetTree, TEXT("N"), Font::Display(58.0f), Palette::Accent);
	AddV(GearBox, GearText, FMargin(0.0f, 2.0f, 0.0f, 0.0f));
	AddH(Numbers, GearBox, FMargin(0.0f, 10.0f, 0.0f, 0.0f), VAlign_Bottom);

	AddH(Numbers, WidgetTree->ConstructWidget<UHorizontalBox>(), FMargin(), VAlign_Center, 1.0f);

	SpeedText = MakeText(*WidgetTree, TEXT("0"), Font::Display(70.0f), Palette::TextPrimary);
	AddH(Numbers, SpeedText, FMargin(), VAlign_Bottom);
	SpeedUnitText = MakeText(*WidgetTree, TEXT("KM/H"), Font::Mono(11.0f, 60), Palette::TextMuted);
	AddH(Numbers, SpeedUnitText, FMargin(7.0f, 0.0f, 0.0f, 12.0f), VAlign_Bottom);

	AddV(Stack, Numbers, FMargin(0.0f, 6.0f, 0.0f, 0.0f));

	// Lap times, and how much of the race is left.
	UHorizontalBox* Footer = WidgetTree->ConstructWidget<UHorizontalBox>();

	auto AddFooterCell = [this, Footer](const TCHAR* Caption, UTextBlock*& OutValue, const FLinearColor& Colour, bool bDivider)
	{
		if (bDivider)
		{
			AddH(Footer, MakeSized(*WidgetTree, MakeDivider(*WidgetTree, true), 1.0f, 34.0f), FMargin(18.0f, 0.0f));
		}
		UVerticalBox* Cell = WidgetTree->ConstructWidget<UVerticalBox>();
		AddV(Cell, MakeLabel(*WidgetTree, Caption));
		OutValue = MakeText(*WidgetTree, TEXT("—"), Font::Mono(15.0f), Colour);
		AddV(Cell, OutValue, FMargin(0.0f, 6.0f, 0.0f, 0.0f));
		AddH(Footer, Cell, FMargin(), VAlign_Center);
	};

	UTextBlock* Last = nullptr;
	UTextBlock* Best = nullptr;
	UTextBlock* LapsLeft = nullptr;
	AddFooterCell(TEXT("Last"), Last, Palette::TextPrimary, false);
	AddFooterCell(TEXT("Best"), Best, PersonalBestColour, true);
	// The mockup has a fuel gauge here. Nothing in the protocol carries fuel —
	// the server does not model it — so the cell shows what is actually known
	// about how much race is left.
	AddFooterCell(TEXT("Laps left"), LapsLeft, Palette::TextPrimary, true);
	LastLapText = Last;
	BestLapText = Best;
	LapsLeftText = LapsLeft;

	AddV(Stack, MakeDivider(*WidgetTree), FMargin(0.0f, 14.0f, 0.0f, 12.0f));
	AddV(Stack, Footer);

	UBorder* Panel = MakePanel(*WidgetTree, Stack, FMargin(22.0f, 16.0f), MakeBrush(Palette::Surface));
	return MakeSized(*WidgetTree, Panel, 470.0f, -1.0f);
}

UWidget* UApexHudWidget::BuildMinimapPanel()
{
	Minimap = WidgetTree->ConstructWidget<UApexMinimapWidget>();

	UOverlay* Stack = WidgetTree->ConstructWidget<UOverlay>();
	UOverlaySlot* MapSlot = Stack->AddChildToOverlay(Minimap);
	MapSlot->SetHorizontalAlignment(HAlign_Fill);
	MapSlot->SetVerticalAlignment(VAlign_Fill);

	SectorCaption = MakeLabel(*WidgetTree, TEXT("Sector 1"), Palette::TextMuted);
	UOverlaySlot* CaptionSlot = Stack->AddChildToOverlay(SectorCaption);
	CaptionSlot->SetHorizontalAlignment(HAlign_Left);
	CaptionSlot->SetVerticalAlignment(VAlign_Bottom);
	CaptionSlot->SetPadding(FMargin(14.0f, 0.0f, 0.0f, 12.0f));

	UBorder* Panel = MakePanel(*WidgetTree, Stack, FMargin(0.0f), MakeBrush(Palette::Surface));
	return MakeSized(*WidgetTree, Panel, MinimapSize, MinimapSize);
}

// --- Derived state ----------------------------------------------------------

const FApexCarTelemetry* UApexHudWidget::FindLocalCar() const
{
	const UApexNetSubsystem* Net = GetNet();
	if (!Net)
	{
		return nullptr;
	}
	const int32 LocalIndex = Net->GetLocalCarIndex();
	return Net->GetLatestTelemetry().Cars.FindByPredicate(
		[LocalIndex](const FApexCarTelemetry& Car) { return Car.CarIndex == LocalIndex; });
}

float UApexHudWidget::CatalogTrackLengthM() const
{
	const UApexMenuFlowSubsystem* Flow = GetFlow();
	FApexTrackCatalogRow Row;
	return Flow && Flow->GetTrackCatalogRow(Flow->GetPendingTrackId(), Row) ? Row.LengthM : 0.0f;
}

void UApexHudWidget::ComputeStandings(TArray<FStanding>& OutOrder) const
{
	const UApexNetSubsystem* Net = GetNet();
	if (!Net)
	{
		return;
	}

	const int32 LocalIndex = Net->GetLocalCarIndex();
	const FApexSessionRoster& Roster = Net->GetSessionRoster();

	// Without a catalog length the stations still order cars within a lap; the
	// nominal length only has to dwarf any real station so a completed lap
	// always outranks a partial one. Gaps stay dashed in that case — they need
	// the real length to mean anything.
	const float CatalogLength = CatalogTrackLengthM();
	const float RankLength = CatalogLength > 0.0f ? CatalogLength : 100000.0f;

	for (const FApexCarTelemetry& Car : Net->GetLatestTelemetry().Cars)
	{
		FStanding Entry;
		Entry.CarIndex = Car.CarIndex;
		// The wire's TrackProgress is a station in metres, not a lap fraction;
		// RaceDistanceM also folds in the grid sitting behind the line.
		Entry.Progress = ApexRace::RaceDistanceM(Car.CurrentLap, Car.TrackProgress, RankLength);
		Entry.SpeedMps = Car.SpeedMps;
		Entry.FinishPosition = Car.FinishPosition;
		Entry.bIsLocal = Car.CarIndex == LocalIndex;

		if (const FApexRosterEntry* Row = Roster.Entries.FindByPredicate(
				[&Car](const FApexRosterEntry& Candidate) { return Candidate.CarIndex == Car.CarIndex; }))
		{
			Entry.Name = Row->PlayerName;
		}
		if (Entry.Name.IsEmpty())
		{
			Entry.Name = FString::Printf(TEXT("CAR %d"), Car.CarIndex);
		}

		OutOrder.Add(MoveTemp(Entry));
	}

	// Finishers first, in classified order; then furthest round the race. Ties
	// break on car index so the order does not flicker between two cars sitting
	// on the grid.
	OutOrder.Sort([](const FStanding& A, const FStanding& B)
	{
		if (A.FinishPosition != B.FinishPosition || !FMath::IsNearlyEqual(A.Progress, B.Progress))
		{
			return ApexRace::RanksAhead(A.FinishPosition, A.Progress, B.FinishPosition, B.Progress);
		}
		return A.CarIndex < B.CarIndex;
	});
}

void UApexHudWidget::UpdateDeltaReference(const FApexCarTelemetry& Local)
{
	const float LapSeconds = Local.CurrentLapTimeMs / 1000.0f;

	// Samples are keyed by fraction of the lap so a reference lap can be looked
	// up by position; the wire's TrackProgress is a station in metres.
	const float TrackLength = CatalogTrackLengthM();
	const float Progress = TrackLength > 0.0f
		? FMath::Clamp(Local.TrackProgress / TrackLength, 0.0f, 1.0f)
		: 0.0f;

	if (Local.CurrentLap != LastSeenLap)
	{
		// The lap counter moved on, and the lap it left behind is on the wire
		// as `LastLapTimeMs`. A lap that left the track is no reference:
		// chasing a delta against a lap that cut a chicane would ask the
		// driver to cut it too.
		const float Completed = Local.LastLapTimeMs / 1000.0f;
		if (LastSeenLap > 0 && LapSamples.Num() > 1 && Completed > 0.0f && !Local.bLastLapInvalid)
		{
			if (ReferenceLapSeconds <= 0.0f || Completed < ReferenceLapSeconds)
			{
				ReferenceLapSeconds = Completed;
				ReferenceLap = LapSamples;
			}
		}

		LastSeenLap = Local.CurrentLap;
		LapSamples.Reset();
	}

	// Samples must stay monotonic in progress for the lookup to work; a frame
	// that arrives out of order (or the wrap at the line) is dropped.
	if (LapSamples.Num() == 0 || Progress > LapSamples.Last().Key)
	{
		LapSamples.Emplace(Progress, LapSeconds);
	}

}

float UApexHudWidget::ReferenceTimeAt(float Progress) const
{
	if (ReferenceLap.Num() < 2)
	{
		return -1.0f;
	}

	// Linear scan is fine: a lap holds a few thousand samples at most and this
	// runs once a frame.
	if (Progress <= ReferenceLap[0].Key)
	{
		return ReferenceLap[0].Value;
	}
	for (int32 Index = 1; Index < ReferenceLap.Num(); ++Index)
	{
		if (Progress <= ReferenceLap[Index].Key)
		{
			const TPair<float, float>& Before = ReferenceLap[Index - 1];
			const TPair<float, float>& After = ReferenceLap[Index];
			const float Span = After.Key - Before.Key;
			const float Alpha = Span > KINDA_SMALL_NUMBER ? (Progress - Before.Key) / Span : 0.0f;
			return FMath::Lerp(Before.Value, After.Value, Alpha);
		}
	}
	return ReferenceLap.Last().Value;
}

FString UApexHudWidget::FormatSpeed(float Mps) const
{
	const UApexSettingsSubsystem* Settings = GetSettings();
	const bool bImperial = Settings && Settings->Get() && Settings->Get()->Units == EApexUnits::Imperial;
	const float Value = bImperial ? Mps * 2.236936f : ApexRace::MpsToKph(Mps);
	return FString::FromInt(FMath::RoundToInt(Value));
}

FString UApexHudWidget::FormatGap(float Seconds, bool bSigned)
{
	if (Seconds <= 0.0f || Seconds > 999.0f)
	{
		return TEXT("—");
	}
	return FString::Printf(TEXT("%s%.3f"), bSigned ? TEXT("+") : TEXT(""), Seconds);
}

// --- Per-frame --------------------------------------------------------------

void UApexHudWidget::RefreshHeader()
{
	const UApexNetSubsystem* Net = GetNet();
	const UApexMenuFlowSubsystem* Flow = GetFlow();
	if (!Net || !TrackNameText || !SessionLineText)
	{
		return;
	}

	FString TrackName;
	FApexSessionSummary Session;
	if (Net->FindSessionById(Net->GetCurrentSessionId(), Session) && !Session.TrackName.IsEmpty())
	{
		TrackName = Session.TrackName;
	}
	else if (Flow)
	{
		FApexTrackCatalogRow Row;
		if (Flow->GetTrackCatalogRow(Flow->GetPendingTrackId(), Row))
		{
			TrackName = Row.DisplayName;
		}
	}
	TrackNameText->SetText(FText::FromString(TrackName.IsEmpty() ? TEXT("CIRCUIT") : TrackName.ToUpper()));

	// The mockup's second line carries weather. There is none on the wire, so
	// the line names the session and the car instead — both known, both useful.
	TArray<FString> Parts;
	Parts.Add(UApexMenuFlowSubsystem::GetGameModeName(Net->GetGameMode()));
	if (Flow)
	{
		FApexCarCatalogRow CarRow;
		if (Flow->GetCarCatalogRow(Flow->GetPendingCarId(), CarRow) && !CarRow.DisplayName.IsEmpty())
		{
			Parts.Add(CarRow.DisplayName);
		}
	}
	SessionLineText->SetText(FText::FromString(FString::Join(Parts, TEXT("  ·  ")).ToUpper()));
}

void UApexHudWidget::RefreshRaceState()
{
	const UApexNetSubsystem* Net = GetNet();
	if (!Net || !PositionText)
	{
		return;
	}

	// The header is not per-frame work, but two things it shows arrive after the
	// race view opens: the track name comes with the lobby state, and the mode
	// goes Countdown -> Race a few seconds in. Both would otherwise sit stale on
	// screen for the whole session.
	if (Net->GetGameMode() != HeaderGameMode || (TrackNameText && TrackNameText->GetText().IsEmpty()))
	{
		HeaderGameMode = Net->GetGameMode();
		RefreshHeader();
	}

	TArray<FStanding> Order;
	ComputeStandings(Order);

	const int32 LocalPlace = Order.IndexOfByPredicate([](const FStanding& Entry) { return Entry.bIsLocal; });

	PositionText->SetText(FText::FromString(LocalPlace >= 0 ? FString::FromInt(LocalPlace + 1) : TEXT("-")));
	PositionOfText->SetText(FText::FromString(FString::Printf(TEXT("/%d"), Order.Num())));

	const FApexCarTelemetry* Local = FindLocalCar();
	const UApexMenuFlowSubsystem* Flow = GetFlow();
	// A hotlap has no distance: laps are counted, never counted down.
	const int32 LapLimit = Flow && HeaderGameMode != EApexGameMode::Hotlap ? Flow->CreateLapLimit : 0;

	// The counter keeps stepping on the cool-down lap after the flag.
	LapText->SetText(FText::FromString(Local ? FString::FromInt(ApexRace::DisplayLap(Local->CurrentLap, LapLimit)) : TEXT("-")));
	LapOfText->SetText(FText::FromString(LapLimit > 0 ? FString::Printf(TEXT("/%d"), LapLimit) : TEXT("")));

	// Gaps are a time, not a distance: how long it would take this car, at its
	// current speed, to cover the ground between them. Progress is already in
	// metres, but only when the catalog knows the circuit's length.
	const float TrackLength = CatalogTrackLengthM();

	auto GapTo = [&](int32 OtherPlace) -> float
	{
		if (LocalPlace < 0 || !Order.IsValidIndex(OtherPlace) || TrackLength <= 0.0f)
		{
			return -1.0f;
		}
		const float Speed = FMath::Max(Order[LocalPlace].SpeedMps, 5.0f);
		const float Metres = FMath::Abs(Order[OtherPlace].Progress - Order[LocalPlace].Progress);
		return Metres / Speed;
	};

	auto SetGapTile = [](UTextBlock* Label, UTextBlock* Value, const TCHAR* Caption,
		const TArray<FStanding>& InOrder, int32 Place, float Gap, const FLinearColor& Colour)
	{
		if (!Label || !Value)
		{
			return;
		}
		if (!InOrder.IsValidIndex(Place))
		{
			Label->SetText(FText::FromString(FString(Caption).ToUpper()));
			Value->SetText(FText::FromString(TEXT("—")));
			Value->SetColorAndOpacity(FSlateColor(Palette::TextMuted));
			return;
		}
		Label->SetText(FText::FromString(FString::Printf(TEXT("%s · %s"), Caption, *InOrder[Place].Name).ToUpper()));
		Value->SetText(FText::FromString(FormatGap(Gap)));
		Value->SetColorAndOpacity(FSlateColor(Colour));
	};

	SetGapTile(AheadLabel, AheadValue, TEXT("Ahead"), Order, LocalPlace - 1, GapTo(LocalPlace - 1), Palette::Error);
	SetGapTile(BehindLabel, BehindValue, TEXT("Behind"), Order, LocalPlace + 1, GapTo(LocalPlace + 1), Palette::Live);

	// Connection health. The ping is a heartbeat round trip, refreshed every two
	// seconds, so it is a health light rather than a live latency read.
	const int32 Ping = Net->GetPingMs();
	if (PingText)
	{
		PingText->SetText(FText::FromString(Ping >= 0 ? FString::Printf(TEXT("%d ms"), Ping) : TEXT("-- ms")));
	}
	if (PingDot)
	{
		const FLinearColor Health = Ping < 0 ? Palette::TextMuted
			: Ping < 80 ? Palette::Live
			: Ping < 200 ? Palette::Accent
			: Palette::Error;
		SetDotColour(PingDot, Health, 8.0f);
	}
	if (CarCountText)
	{
		CarCountText->SetText(FText::FromString(FString::Printf(TEXT("%d CARS"), Order.Num())));
	}
}

void UApexHudWidget::RefreshStandings()
{
	if (StandingRows.Num() == 0)
	{
		return;
	}

	const UApexNetSubsystem* Net = GetNet();

	TArray<FStanding> Order;
	ComputeStandings(Order);

	const int32 LocalPlace = Order.IndexOfByPredicate([](const FStanding& Entry) { return Entry.bIsLocal; });

	// The panel shows five rows around the local car rather than the top five:
	// in eleventh place the leaders are not who you are racing.
	int32 First = 0;
	if (LocalPlace >= 0 && Order.Num() > StandingRowCount)
	{
		First = FMath::Clamp(LocalPlace - StandingRowCount / 2, 0, Order.Num() - StandingRowCount);
	}

	const float TrackLength = CatalogTrackLengthM();

	for (int32 Row = 0; Row < StandingRows.Num(); ++Row)
	{
		const int32 Place = First + Row;
		const bool bUsed = Order.IsValidIndex(Place);

		if (StandingSlots.IsValidIndex(Row) && StandingSlots[Row])
		{
			StandingSlots[Row]->SetVisibility(
				bUsed ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
		}
		if (!bUsed)
		{
			continue;
		}

		const FStanding& Entry = Order[Place];
		const bool bLocal = Entry.bIsLocal;

		StandingRows[Row]->SetBrush(MakeBrush(bLocal ? Palette::Accent : Palette::Surface));
		StandingPlace[Row]->SetText(FText::FromString(FString::FromInt(Place + 1)));
		StandingPlace[Row]->SetColorAndOpacity(FSlateColor(bLocal ? Palette::OnAccent : Palette::TextMuted));

		StandingName[Row]->SetText(FText::FromString(bLocal ? TEXT("YOU") : Entry.Name.ToUpper()));
		StandingName[Row]->SetFont(Font::Body(14.0f, bLocal));
		StandingName[Row]->SetColorAndOpacity(FSlateColor(bLocal ? Palette::OnAccent : Palette::TextPrimary));

		// The leader's cell carries a lap time; everyone else's a gap to them.
		// A gap means nothing once a car has taken the flag.
		FString Right;
		if (Entry.FinishPosition > 0)
		{
			Right = TEXT("FINISHED");
		}
		else if (Place == 0)
		{
			const FApexCarTiming* Timing = Net ? Net->GetTimingBoard().Find(Entry.CarIndex) : nullptr;
			Right = Timing && Timing->BestLapMs > 0 ? FormatTime(Timing->BestLapMs / 1000.0f) : TEXT("LEADER");
		}
		else if (TrackLength > 0.0f)
		{
			const float Speed = FMath::Max(Entry.SpeedMps, 5.0f);
			Right = FormatGap((Order[0].Progress - Entry.Progress) / Speed);
		}
		else
		{
			Right = TEXT("—");
		}
		StandingTime[Row]->SetText(FText::FromString(Right));
		StandingTime[Row]->SetColorAndOpacity(FSlateColor(bLocal ? Palette::OnAccent : Palette::TextSecondary));
	}

	// The session's fastest lap. The server names it — only a lap inside track
	// limits can hold it — so the row stays empty until somebody sets one.
	if (FastestLapName && FastestLapTime && Net)
	{
		const FApexTimingBoard& Board = Net->GetTimingBoard();
		const FStanding* Holder = Order.FindByPredicate(
			[&Board](const FStanding& Entry) { return Entry.CarIndex == Board.SessionBestLapCarIndex; });
		const FString Who = Holder
			? (Holder->bIsLocal ? FString(TEXT("YOU")) : Holder->Name.ToUpper())
			: FString(TEXT("—"));
		FastestLapName->SetText(FText::FromString(FString::Printf(TEXT("FASTEST %s"), *Who)));
		FastestLapTime->SetText(FText::FromString(
			Board.SessionBestLapMs > 0 ? FormatTime(Board.SessionBestLapMs / 1000.0f) : TEXT("--:--.---")));
	}
}

void UApexHudWidget::RefreshCarState()
{
	const FApexCarTelemetry* Local = FindLocalCar();
	if (!Local || !SpeedText)
	{
		return;
	}

	SpeedText->SetText(FText::FromString(FormatSpeed(Local->SpeedMps)));
	if (SpeedUnitText)
	{
		const UApexSettingsSubsystem* Settings = GetSettings();
		const bool bImperial = Settings && Settings->Get() && Settings->Get()->Units == EApexUnits::Imperial;
		SpeedUnitText->SetText(FText::FromString(bImperial ? TEXT("MPH") : TEXT("KM/H")));
	}

	if (GearText)
	{
		const FString Gear = Local->Gear < 0 ? TEXT("R") : Local->Gear == 0 ? TEXT("N") : FString::FromInt(Local->Gear);
		GearText->SetText(FText::FromString(Gear));
	}

	// No redline is broadcast, so the scale is the highest reading so far. It
	// only ever grows, which keeps the strip from rescaling under the driver.
	ObservedMaxRpm = FMath::Max(ObservedMaxRpm, Local->EngineRpm);
	const float Fraction = FMath::Clamp(Local->EngineRpm / FMath::Max(ObservedMaxRpm, 1.0f), 0.0f, 1.0f);
	const int32 Lit = FMath::RoundToInt(Fraction * RpmSegmentCount);

	for (int32 Index = 0; Index < RpmSegments.Num(); ++Index)
	{
		const bool bOn = Index < Lit;
		const bool bRed = Index >= RpmSegmentCount - RpmRedSegments;
		const FLinearColor Colour = !bOn ? Palette::Border : (bRed ? Palette::Error : Palette::Accent);
		RpmSegments[Index]->SetBrush(MakeBrush(Colour));
	}
	if (RpmText)
	{
		RpmText->SetText(FText::FromString(FString::FromInt(FMath::RoundToInt(Local->EngineRpm))));
	}
	if (DrsBadge && DrsText)
	{
		const FLinearColor Fill = Local->bDrsOpen ? Palette::Live
			: Local->bDrsAllowed ? Palette::Surface : Palette::Border;
		const FLinearColor Ink = Local->bDrsOpen ? Palette::OnAccent
			: Local->bDrsAllowed ? Palette::Live : Palette::TextDisabled;
		DrsBadge->SetBrush(MakeBrush(Fill));
		DrsText->SetColorAndOpacity(FSlateColor(Ink));
	}

	if (ThrottleBar)
	{
		ThrottleBar->SetPercent(FMath::Clamp(Local->Throttle, 0.0f, 1.0f));
	}
	if (BrakeBar)
	{
		BrakeBar->SetPercent(FMath::Clamp(Local->Brake, 0.0f, 1.0f));
	}

	// Both times are the server's: it starts and stops the clock on the tick
	// the car crosses the line, and it decides whether the lap counted.
	if (LastLapText)
	{
		LastLapText->SetText(FText::FromString(FormatTime(Local->LastLapTimeMs / 1000.0f)));
		LastLapText->SetColorAndOpacity(FSlateColor(
			Local->LastLapTimeMs > 0 && Local->bLastLapInvalid ? Palette::Error : Palette::TextPrimary));
	}
	if (BestLapText)
	{
		BestLapText->SetText(FText::FromString(FormatTime(Local->BestLapTimeMs / 1000.0f)));
	}
	if (LapsLeftText)
	{
		const UApexMenuFlowSubsystem* Flow = GetFlow();
		const int32 LapLimit = Flow && HeaderGameMode != EApexGameMode::Hotlap ? Flow->CreateLapLimit : 0;
		LapsLeftText->SetText(FText::FromString(
			LapLimit > 0
				? FString::FromInt(Local->FinishPosition > 0 ? 0 : FMath::Max(0, LapLimit - FMath::Max(0, Local->CurrentLap - 1)))
				: TEXT("—")));
	}
}

void UApexHudWidget::RefreshDelta()
{
	if (!DeltaValue)
	{
		return;
	}

	const FApexCarTelemetry* Local = FindLocalCar();

	// The reference lap is keyed by fraction of the lap; the wire's
	// TrackProgress is a station in metres.
	const float TrackLength = CatalogTrackLengthM();
	const float Fraction = Local && TrackLength > 0.0f
		? FMath::Clamp(Local->TrackProgress / TrackLength, 0.0f, 1.0f)
		: 0.0f;
	const float ReferenceNow = Local ? ReferenceTimeAt(Fraction) : -1.0f;

	if (!Local || ReferenceNow < 0.0f)
	{
		// Before a lap is in the books there is nothing to be quicker than.
		DeltaValue->SetText(FText::FromString(TEXT("—")));
		DeltaValue->SetColorAndOpacity(FSlateColor(Palette::TextMuted));
	}
	else
	{
		const float Delta = Local->CurrentLapTimeMs / 1000.0f - ReferenceNow;
		DeltaValue->SetText(FText::FromString(FString::Printf(TEXT("%+.3f"), Delta)));
		DeltaValue->SetColorAndOpacity(FSlateColor(Delta <= 0.0f ? Palette::Live : Palette::Error));
	}

}

/**
 * The sector strip and the lap-invalid banner.
 *
 * Every number here was timed by the server and arrived as a `LapTiming`
 * message; the client only decides what colour it is. Purple is the session's
 * best, green the driver's own, amber a sector they have done quicker before,
 * and grey a sector still being driven.
 */
void UApexHudWidget::RefreshSectors()
{
	const UApexNetSubsystem* Net = GetNet();
	const FApexCarTelemetry* Local = FindLocalCar();
	if (!Net)
	{
		return;
	}

	const FApexTimingBoard& Board = Net->GetTimingBoard();
	const FApexCarTiming* Timing = Local ? Board.Find(Local->CarIndex) : nullptr;

	// The lap in progress, or — in the moments after the line, before the
	// first sector of the new lap is done — the lap that just ended, so the
	// driver gets to read their final split.
	const bool bShowLastLap = Timing
		&& !Timing->CurrentSplitsMs.ContainsByPredicate([](int32 Split) { return Split > 0; });
	const TArray<int32>* Splits = Timing
		? (bShowLastLap ? &Timing->LastSplitsMs : &Timing->CurrentSplitsMs)
		: nullptr;

	for (int32 Index = 0; Index < SectorBars.Num(); ++Index)
	{
		const int32 Mine = Splits && Splits->IsValidIndex(Index) ? (*Splits)[Index] : 0;
		const int32 MyBest = Timing && Timing->BestSplitsMs.IsValidIndex(Index)
			? Timing->BestSplitsMs[Index]
			: 0;
		const int32 SessionBest = Board.SessionBestSplitsMs.IsValidIndex(Index)
			? Board.SessionBestSplitsMs[Index]
			: 0;

		FLinearColor Colour = Palette::Border;
		if (Mine > 0)
		{
			Colour = (SessionBest > 0 && Mine <= SessionBest)
				? SessionBestColour
				: ((MyBest <= 0 || Mine <= MyBest) ? PersonalBestColour : SlowerColour);
		}
		SectorBars[Index]->SetBrush(MakeBrush(Colour));
		if (SectorTimes.IsValidIndex(Index))
		{
			SectorTimes[Index]->SetText(FText::FromString(FormatSplit(Mine)));
			SectorTimes[Index]->SetColorAndOpacity(
				FSlateColor(Mine > 0 ? Colour : Palette::TextMuted));
		}
	}

	if (LapInvalidText)
	{
		LapInvalidText->SetVisibility(Local && Local->bLapInvalid
			? ESlateVisibility::HitTestInvisible
			: ESlateVisibility::Collapsed);
	}

	if (SectorCaption)
	{
		// The sector lines are the server's, so the caption reads the station
		// against them rather than splitting the lap into thirds by eye.
		const FApexTrackSectors& Sectors = Net->GetTrackSectors();
		const int32 Sector = (Local && Sectors.IsValid()) ? Sectors.SectorAt(Local->TrackProgress) : 0;
		SectorCaption->SetText(FText::FromString(FString::Printf(TEXT("SECTOR %d"), Sector + 1)));
	}
}

void UApexHudWidget::RefreshMinimap()
{
	const UApexNetSubsystem* Net = GetNet();
	if (!Minimap || !Net)
	{
		return;
	}

	if (!Minimap->HasCenterline())
	{
		// The outline arrives with the lobby state, which is broadcast every two
		// seconds and only carries points when the codec is parsing them. The map
		// is emptied by SetRaceActive, so this also picks up a track change.
		const UApexMenuFlowSubsystem* Flow = GetFlow();
		FApexTrackConfigSummary Track;
		if (Flow && Net->FindTrackById(Flow->GetPendingTrackId(), Track) && Track.Centerline.Num() > 1)
		{
			Minimap->SetCenterline(Track.Centerline);
		}
		else
		{
			return;
		}
	}

	const int32 LocalIndex = Net->GetLocalCarIndex();
	TArray<FApexMinimapBlip> Blips;
	Blips.Reserve(Net->GetLatestTelemetry().Cars.Num());
	for (const FApexCarTelemetry& Car : Net->GetLatestTelemetry().Cars)
	{
		FApexMinimapBlip Blip;
		Blip.Position = FVector2D(Car.Position.X, Car.Position.Y);
		Blip.bIsLocal = Car.CarIndex == LocalIndex;
		Blip.Colour = BlipColour(Car.CarIndex, Blip.bIsLocal);
		Blips.Add(Blip);
	}
	Minimap->SetBlips(MoveTemp(Blips));
}
