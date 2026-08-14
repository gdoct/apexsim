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
#include "Race/ApexRaceCoordinate.h"
#include "UI/ApexMinimapWidget.h"
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

	/** A lap is split into three by track position; the protocol has no sectors. */
	constexpr int32 SectorCount = 3;

	/** Purple in every sim: a time nobody has beaten. */
	const FLinearColor PersonalBestColour = FLinearColor::FromSRGBColor(FColor(0xB0, 0x7C, 0xE8));

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

	for (int32 Index = 0; Index < SectorCount; ++Index)
	{
		SectorSplits[Index] = 0.0f;
		ReferenceSplits[Index] = 0.0f;
	}
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

	SetVisibility(bActive && Detail != EApexHudDetail::Hidden
		? ESlateVisibility::HitTestInvisible
		: ESlateVisibility::Collapsed);

	if (bActive)
	{
		// A new race starts with no history: keeping the previous session's
		// reference lap would show a delta against a lap of a different circuit.
		LapSamples.Reset();
		ReferenceLap.Reset();
		LastSeenLap = 0;
		LastLapSeconds = 0.0f;
		BestLapSeconds = 0.0f;
		ObservedMaxRpm = 8000.0f;
		HeaderGameMode = EApexGameMode::Lobby;
		for (int32 Index = 0; Index < SectorCount; ++Index)
		{
			SectorSplits[Index] = 0.0f;
			ReferenceSplits[Index] = 0.0f;
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
	RefreshMinimap();
}

// --- Construction -----------------------------------------------------------

void UApexHudWidget::BuildHud()
{
	const UApexSettingsSubsystem* Settings = GetSettings();
	BuiltDetail = Settings && Settings->Get() ? Settings->Get()->HudDetail : EApexHudDetail::All;
	const bool bFull = BuiltDetail == EApexHudDetail::All;

	// Every cached pointer is about to dangle.
	StandingRows.Reset();
	StandingPlace.Reset();
	StandingName.Reset();
	StandingTime.Reset();
	SectorBars.Reset();
	RpmSegments.Reset();
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

	WidgetTree->RootWidget = RootStack;
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

	return MakeSized(*WidgetTree, Stack, StandingsWidth, -1.0f);
}

UWidget* UApexHudWidget::BuildDeltaPanel()
{
	UVerticalBox* Stack = WidgetTree->ConstructWidget<UVerticalBox>();

	// Sector strip: three bars, one per third of the lap.
	UHorizontalBox* Sectors = WidgetTree->ConstructWidget<UHorizontalBox>();
	for (int32 Index = 0; Index < SectorCount; ++Index)
	{
		UBorder* Bar = MakePanel(*WidgetTree, nullptr, FMargin(), MakeBrush(Palette::Border));
		AddH(Sectors, MakeSized(*WidgetTree, Bar, 62.0f, 5.0f), FMargin(Index == 0 ? 0.0f : 4.0f, 0.0f, 0.0f, 0.0f));
		SectorBars.Add(Bar);
	}
	AddV(Stack, Sectors, FMargin(0.0f, 0.0f, 0.0f, 12.0f), HAlign_Left);

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
		FProgressBarStyle Style = OutBar->WidgetStyle;
		Style.SetBackgroundImage(MakeBrush(Palette::Surface));
		Style.SetFillImage(MakeBrush(Fill));
		OutBar->WidgetStyle = Style;
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

void UApexHudWidget::ComputeStandings(TArray<FStanding>& OutOrder) const
{
	const UApexNetSubsystem* Net = GetNet();
	if (!Net)
	{
		return;
	}

	const int32 LocalIndex = Net->GetLocalCarIndex();
	const FApexSessionRoster& Roster = Net->GetSessionRoster();

	for (const FApexCarTelemetry& Car : Net->GetLatestTelemetry().Cars)
	{
		FStanding Entry;
		Entry.CarIndex = Car.CarIndex;
		// Lap numbers are 1-based, so lap 1 half way round is 0.5 of the race.
		Entry.Progress = FMath::Max(0, Car.CurrentLap - 1) + FMath::Clamp(Car.TrackProgress, 0.0f, 1.0f);
		Entry.SpeedMps = Car.SpeedMps;
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

	// Furthest round the race is leading. Ties break on car index so the order
	// does not flicker between two cars sitting on the grid.
	OutOrder.Sort([](const FStanding& A, const FStanding& B)
	{
		if (!FMath::IsNearlyEqual(A.Progress, B.Progress))
		{
			return A.Progress > B.Progress;
		}
		return A.CarIndex < B.CarIndex;
	});
}

void UApexHudWidget::UpdateDeltaReference(const FApexCarTelemetry& Local)
{
	const float LapSeconds = Local.CurrentLapTimeMs / 1000.0f;
	const float Progress = FMath::Clamp(Local.TrackProgress, 0.0f, 1.0f);

	if (Local.CurrentLap != LastSeenLap)
	{
		// The lap counter moved on. The last sample of the lap that just ended is
		// its time — the protocol never states a lap time, only the running one.
		if (LastSeenLap > 0 && LapSamples.Num() > 1)
		{
			LastLapSeconds = LapSamples.Last().Value;

			if (LastLapSeconds > 0.0f && (BestLapSeconds <= 0.0f || LastLapSeconds < BestLapSeconds))
			{
				BestLapSeconds = LastLapSeconds;
				ReferenceLap = LapSamples;
				ReferenceSplits = SectorSplits;
			}
		}

		LastSeenLap = Local.CurrentLap;
		LapSamples.Reset();
		for (int32 Index = 0; Index < SectorCount; ++Index)
		{
			SectorSplits[Index] = 0.0f;
		}
	}

	// Samples must stay monotonic in progress for the lookup to work; a frame
	// that arrives out of order (or the wrap at the line) is dropped.
	if (LapSamples.Num() == 0 || Progress > LapSamples.Last().Key)
	{
		LapSamples.Emplace(Progress, LapSeconds);
	}

	// Splits are cumulative — the elapsed lap time when the sector boundary went
	// past — and each is written once, the first frame past its boundary.
	for (int32 Index = 0; Index < SectorCount; ++Index)
	{
		const float SectorEnd = static_cast<float>(Index + 1) / SectorCount;
		if (Progress >= SectorEnd && SectorSplits[Index] <= 0.0f)
		{
			SectorSplits[Index] = LapSeconds;
		}
	}
}

float UApexHudWidget::SectorDuration(const TStaticArray<float, 3>& Splits, int32 Index)
{
	if (Index < 0 || Index >= SectorCount || Splits[Index] <= 0.0f)
	{
		return 0.0f;
	}
	const float Previous = Index == 0 ? 0.0f : Splits[Index - 1];
	return Splits[Index] - Previous;
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
	LapText->SetText(FText::FromString(Local ? FString::FromInt(FMath::Max(1, Local->CurrentLap)) : TEXT("-")));

	const UApexMenuFlowSubsystem* Flow = GetFlow();
	const int32 LapLimit = Flow ? Flow->CreateLapLimit : 0;
	LapOfText->SetText(FText::FromString(LapLimit > 0 ? FString::Printf(TEXT("/%d"), LapLimit) : TEXT("")));

	// Gaps are a time, not a distance: how long it would take this car, at its
	// current speed, to cover the ground between them.
	const float TrackLength = [Flow]()
	{
		FApexTrackCatalogRow Row;
		return Flow && Flow->GetTrackCatalogRow(Flow->GetPendingTrackId(), Row) ? Row.LengthM : 0.0f;
	}();

	auto GapTo = [&](int32 OtherPlace) -> float
	{
		if (LocalPlace < 0 || !Order.IsValidIndex(OtherPlace) || TrackLength <= 0.0f)
		{
			return -1.0f;
		}
		const float Speed = FMath::Max(Order[LocalPlace].SpeedMps, 5.0f);
		const float Metres = FMath::Abs(Order[OtherPlace].Progress - Order[LocalPlace].Progress) * TrackLength;
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

	const UApexMenuFlowSubsystem* Flow = GetFlow();
	const float TrackLength = [Flow]()
	{
		FApexTrackCatalogRow Row;
		return Flow && Flow->GetTrackCatalogRow(Flow->GetPendingTrackId(), Row) ? Row.LengthM : 0.0f;
	}();

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
		FString Right;
		if (Place == 0)
		{
			Right = bLocal && BestLapSeconds > 0.0f ? FormatTime(BestLapSeconds) : TEXT("LEADER");
		}
		else if (TrackLength > 0.0f)
		{
			const float Speed = FMath::Max(Entry.SpeedMps, 5.0f);
			Right = FormatGap((Order[0].Progress - Entry.Progress) * TrackLength / Speed);
		}
		else
		{
			Right = TEXT("—");
		}
		StandingTime[Row]->SetText(FText::FromString(Right));
		StandingTime[Row]->SetColorAndOpacity(FSlateColor(bLocal ? Palette::OnAccent : Palette::TextSecondary));
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

	if (ThrottleBar)
	{
		ThrottleBar->SetPercent(FMath::Clamp(Local->Throttle, 0.0f, 1.0f));
	}
	if (BrakeBar)
	{
		BrakeBar->SetPercent(FMath::Clamp(Local->Brake, 0.0f, 1.0f));
	}

	if (LastLapText)
	{
		LastLapText->SetText(FText::FromString(FormatTime(LastLapSeconds)));
	}
	if (BestLapText)
	{
		BestLapText->SetText(FText::FromString(FormatTime(BestLapSeconds)));
	}
	if (LapsLeftText)
	{
		const UApexMenuFlowSubsystem* Flow = GetFlow();
		const int32 LapLimit = Flow ? Flow->CreateLapLimit : 0;
		LapsLeftText->SetText(FText::FromString(
			LapLimit > 0
				? FString::FromInt(FMath::Max(0, LapLimit - FMath::Max(0, Local->CurrentLap - 1)))
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
	const float ReferenceNow = Local ? ReferenceTimeAt(FMath::Clamp(Local->TrackProgress, 0.0f, 1.0f)) : -1.0f;

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

	for (int32 Index = 0; Index < SectorBars.Num(); ++Index)
	{
		const float Mine = SectorDuration(SectorSplits, Index);
		const float ReferenceSector = SectorDuration(ReferenceSplits, Index);

		// Grey until the sector is done; purple when nothing has beaten it, and
		// green or red against the reference lap once there is one.
		FLinearColor Colour = Palette::Border;
		if (Mine > 0.0f)
		{
			Colour = ReferenceSector <= 0.0f || Mine < ReferenceSector
				? PersonalBestColour
				: (Mine > ReferenceSector ? Palette::Error : Palette::Live);
		}
		SectorBars[Index]->SetBrush(MakeBrush(Colour));
	}

	if (SectorCaption && Local)
	{
		const int32 Sector = FMath::Clamp(
			FMath::FloorToInt(FMath::Clamp(Local->TrackProgress, 0.0f, 0.999f) * SectorCount) + 1, 1, SectorCount);
		SectorCaption->SetText(FText::FromString(FString::Printf(TEXT("SECTOR %d"), Sector)));
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
		// seconds and only carries points when the codec is parsing them.
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
