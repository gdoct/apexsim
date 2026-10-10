#include "UI/ApexHotlapWidget.h"

#include "ApexMenuFlowSubsystem.h"
#include "ApexNetSubsystem.h"
#include "ApexReplayRecorder.h"
#include "ApexSettingsSave.h"
#include "ApexSettingsSubsystem.h"
#include "ApexSim.h"
#include "Audio/ApexUiAudioSubsystem.h"
#include "Blueprint/WidgetTree.h"
#include "Race/ApexQualifyingBoard.h"
#include "Components/Border.h"
#include "Components/EditableTextBox.h"
#include "Components/HorizontalBox.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/ScrollBox.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/WidgetSwitcher.h"
#include "Engine/GameInstance.h"
#include "HAL/IConsoleManager.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "UObject/UObjectIterator.h"
#include "UI/ApexButtonWidget.h"
#include "UI/ApexStepperWidget.h"
#include "UI/ApexUIStyle.h"

using namespace ApexUI;

namespace
{
	// The garage design is drawn at 1440 x 900; these are its sizes at the
	// 1080-line reference, i.e. 1.2x, with CSS pixels turned into points.
	constexpr float CardInset = 40.0f;
	constexpr float HeaderHeight = 124.0f;
	constexpr float ActionColumnWidth = 320.0f;
	constexpr float TabBarHeight = 68.0f;
	constexpr float FooterHeight = 56.0f;
	constexpr float PagePadX = 38.0f;
	constexpr float PagePadY = 26.0f;
	constexpr float StepperWidth = 270.0f;
	constexpr float StepperReadout = 270.0f - 2.0f * 48.0f - 2.0f * 4.0f;
	constexpr float SidePanelWidth = 440.0f;
	constexpr float SaveDetailWidth = 460.0f;
	constexpr float GearChartHeight = 230.0f;

	constexpr float SheetWidth = 330.0f;
	/** The scoreboard's column and its rows (a grid is twenty cars). */
	constexpr float BoardWidth = 600.0f;
	constexpr int32 BoardRowCount = 20;
	constexpr float BoardRowHeight = 34.0f;
	/** Laps kept on the sheet; older ones scroll off the top. */
	constexpr int32 SheetRowCount = 10;

	const FName ActionGoOut      = TEXT("Hotlap.GoOut");
	const FName ActionReplay     = TEXT("Hotlap.Replay");
	const FName ActionGhost      = TEXT("Hotlap.Ghost");
	const FName ActionTyres      = TEXT("Hotlap.Tyres");
	const FName ActionResetSetup = TEXT("Hotlap.Reset");
	const FName ActionGarageSaveReplay = TEXT("Hotlap.SaveReplay");
	const FName ActionTab        = TEXT("Hotlap.Tab");
	const FName ActionTune       = TEXT("Hotlap.Page.Tune");
	const FName ActionBoard      = TEXT("Hotlap.Page.Board");
	const FName ActionWatch      = TEXT("Hotlap.Watch");
	const FName ActionWatchCamera = TEXT("Hotlap.WatchCamera");
	const FName ActionCompound   = TEXT("Hotlap.Compound");
	const FName ActionSave       = TEXT("Hotlap.Save");
	const FName ActionPickSaved  = TEXT("Hotlap.PickSaved");
	const FName ActionLoad       = TEXT("Hotlap.Load");
	const FName ActionOverwrite  = TEXT("Hotlap.Overwrite");
	const FName ActionDelete     = TEXT("Hotlap.Delete");
	const FName ActionRename     = TEXT("Hotlap.Rename");

	/** The garage design's own tones: a cool near-black card with hairlines of the text colour. */
	const FLinearColor CardFill  = FLinearColor::FromSRGBColor(FColor(16, 19, 21)).CopyWithNewOpacity(0.92f);
	const FLinearColor Ink       = FLinearColor::FromSRGBColor(FColor(0xE8, 0xEA, 0xEA));
	const FLinearColor InkMuted  = FLinearColor::FromSRGBColor(FColor(0x8F, 0x95, 0x99));
	const FLinearColor InkFaint  = FLinearColor::FromSRGBColor(FColor(0x6B, 0x72, 0x76));
	const FLinearColor Hairline  = Ink.CopyWithNewOpacity(0.12f);
	const FLinearColor RowFill   = Ink.CopyWithNewOpacity(0.04f);
	const FLinearColor Outline   = Ink.CopyWithNewOpacity(0.14f);

	/** The timing screen's purple: the session's best. */
	const FLinearColor Purple = FLinearColor::FromSRGBColor(FColor(0xB3, 0x7C, 0xFF));

	struct FTabSpec
	{
		const TCHAR* Label;
	};
	const FTabSpec TabSpecs[] = { { TEXT("TYRES") }, { TEXT("SUSPENSION") }, { TEXT("ENGINE") }, { TEXT("AERO") }, { TEXT("ERS") }, { TEXT("LOAD / SAVE") } };
	static_assert(UE_ARRAY_COUNT(TabSpecs) == static_cast<int32>(EApexGarageTab::Count), "a label per tab");

	/**
	 * Drag scale per click of the knobs that open the car to the air, as
	 * `CarSetup::apply` multiplies them (server/src/car_setup.rs
	 * `*_DRAG_PER_CLICK`): change both sides together. And the deploy map's
	 * bend of Balanced mode's spend curve per click (`ERS_DEPLOY_MAP_PER_CLICK`).
	 */
	const float DeployMapPerClick = 0.1f;
	/**
	 */
	const TPair<int32, float> DragPerClick[] = {
		{ ApexCarSetup::FrontWing, 0.005f }, { ApexCarSetup::RearWing, 0.015f },
		{ ApexCarSetup::BrakeDucts, 0.003f }, { ApexCarSetup::BrakeDuctsRear, 0.003f },
		{ ApexCarSetup::Radiator, 0.003f } };

	/** The compounds, softest slick first, then the treaded tyres: what `tyre_thermal::COMPOUNDS` does to the car's own tyre. */
	struct FCompoundSpec
	{
		int32 Clicks;
		const TCHAR* Name;
		const TCHAR* Note;
		const TCHAR* Figures;
		FLinearColor Colour;
		float Grip;
		float Life;
	};
	const FCompoundSpec Compounds[] = {
		{ 1,  TEXT("SOFT"),   TEXT("Most grip, shortest life."),  TEXT("+3% GRIP · 1.8x WEAR · -6°C"),
			FLinearColor::FromSRGBColor(FColor(0xE0, 0x4B, 0x3C)), 0.94f, 0.30f },
		{ 0,  TEXT("MEDIUM"), TEXT("The car's own tyre."),        TEXT("AS FILED · WET: THE RAIN TYRE"),
			FLinearColor::FromSRGBColor(FColor(0xE8, 0xBA, 0x3A)), 0.80f, 0.55f },
		{ -1, TEXT("HARD"),   TEXT("Slow to warm, lasts longest."), TEXT("-3% GRIP · 0.55x WEAR · +6°C"),
			Ink, 0.66f, 1.00f },
		{ -2, TEXT("INTER"),  TEXT("A damp or drying track."),    TEXT("LIGHT RAIN · -12% DRY · -25°C"),
			FLinearColor::FromSRGBColor(FColor(0x3C, 0xB0, 0x4B)), 0.50f, 0.40f },
		{ -3, TEXT("WET"),    TEXT("Standing water."),            TEXT("HEAVY RAIN · -22% DRY · -35°C"),
			FLinearColor::FromSRGBColor(FColor(0x2F, 0x7F, 0xE0)), 0.35f, 0.30f },
	};

	/** "1:32.104", or dashes for no time. */
	FString FormatMs(int32 Ms)
	{
		if (Ms <= 0)
		{
			return TEXT("--:--.---");
		}
		return FString::Printf(TEXT("%d:%02d.%03d"), Ms / 60000, (Ms / 1000) % 60, Ms % 1000);
	}

	/** "+0.412" / "-1.020" against a reference, in seconds. */
	FString FormatDelta(int32 Ms, int32 ReferenceMs)
	{
		const int32 Delta = Ms - ReferenceMs;
		return FString::Printf(TEXT("%s%d.%03d"), Delta < 0 ? TEXT("-") : TEXT("+"), FMath::Abs(Delta) / 1000, FMath::Abs(Delta) % 1000);
	}

	/** "28 SEP". */
	FString FormatDay(const FDateTime& When)
	{
		static const TCHAR* Months[] = { TEXT("JAN"), TEXT("FEB"), TEXT("MAR"), TEXT("APR"), TEXT("MAY"), TEXT("JUN"),
			TEXT("JUL"), TEXT("AUG"), TEXT("SEP"), TEXT("OCT"), TEXT("NOV"), TEXT("DEC") };
		return FString::Printf(TEXT("%d %s"), When.GetDay(), Months[FMath::Clamp(When.GetMonth() - 1, 0, 11)]);
	}

	FString CompoundName(int32 Clicks)
	{
		return Clicks > 0 ? TEXT("SOFT") : Clicks == 0 ? TEXT("MEDIUM") : Clicks == -1 ? TEXT("HARD")
			: Clicks == -2 ? TEXT("INTER") : TEXT("WET");
	}

	/** A number with a fixed count of decimals. */
	FString Fixed(float Value, int32 Decimals)
	{
		switch (FMath::Clamp(Decimals, 0, 3))
		{
		case 0: return FString::Printf(TEXT("%.0f"), Value);
		case 1: return FString::Printf(TEXT("%.1f"), Value);
		case 2: return FString::Printf(TEXT("%.2f"), Value);
		default: return FString::Printf(TEXT("%.3f"), Value);
		}
	}

	/** A knob's value in the sheet's units at a click count, held to its bounds. */
	float SheetValue(const FApexSetupKnobFigure& Figure, int32 Clicks)
	{
		return FMath::Clamp(Figure.Stock + Clicks * Figure.Step, Figure.Lo, Figure.Hi);
	}

	/** Uppercase mono caption in the design's grey. */
	UTextBlock* Caption(UWidgetTree& Tree, const FString& Text, const FLinearColor& Colour = InkMuted)
	{
		return MakeText(Tree, Text.ToUpper(), Font::Mono(10.0f, 200), Colour);
	}

	/** A 1 px rule across (or down) a container. */
	UWidget* Rule(UWidgetTree& Tree, bool bVertical = false)
	{
		UBorder* Line = MakePanel(Tree, nullptr, FMargin(), MakeBrush(Hairline));
		return MakeSized(Tree, Line, bVertical ? 1.0f : -1.0f, bVertical ? -1.0f : 1.0f);
	}

	/** Content drawn over a button that fills the same slot: the button takes the input, the content is only drawn. */
	UOverlay* ButtonWithContent(UWidgetTree& Tree, UApexButtonWidget* Button, UWidget* Content, const FMargin& ContentPadding)
	{
		UOverlay* Stack = Tree.ConstructWidget<UOverlay>();
		UOverlaySlot* ButtonSlot = Stack->AddChildToOverlay(Button);
		ButtonSlot->SetHorizontalAlignment(HAlign_Fill);
		ButtonSlot->SetVerticalAlignment(VAlign_Fill);
		Content->SetVisibility(ESlateVisibility::HitTestInvisible);
		UOverlaySlot* ContentSlot = Stack->AddChildToOverlay(Content);
		ContentSlot->SetHorizontalAlignment(HAlign_Fill);
		ContentSlot->SetVerticalAlignment(VAlign_Center);
		ContentSlot->SetPadding(ContentPadding);
		return Stack;
	}

	/** A thin horizontal bar filled to Share. */
	UWidget* ShareBar(UWidgetTree& Tree, float Share, const FLinearColor& Fill)
	{
		UHorizontalBox* Track = Tree.ConstructWidget<UHorizontalBox>();
		const float Clamped = FMath::Clamp(Share, 0.01f, 1.0f);
		AddH(Track, MakePanel(Tree, nullptr, FMargin(), MakeBrush(Fill)), FMargin(), VAlign_Fill, Clamped);
		if (Clamped < 1.0f)
		{
			AddH(Track, MakePanel(Tree, nullptr, FMargin(), MakeBrush(Ink.CopyWithNewOpacity(0.12f))), FMargin(), VAlign_Fill, 1.0f - Clamped);
		}
		return MakeSized(Tree, Track, -1.0f, 5.0f);
	}

	UScrollBox* ScrollPage(UWidgetTree& Tree, UWidget* Content)
	{
		UScrollBox* Scroll = Tree.ConstructWidget<UScrollBox>();
		Scroll->SetScrollBarVisibility(ESlateVisibility::Collapsed);
		Scroll->AddChild(Content);
		return Scroll;
	}
}

// `apexsim.hotlap.Tab N` (0-5) shows a garage tab, for screenshots and checks.
static FAutoConsoleCommand GHotlapTabCommand(
	TEXT("apexsim.hotlap.Tab"),
	TEXT("Show a hotlap garage tab: 0 tyres, 1 suspension, 2 engine, 3 aero, 4 ers, 5 load / save."),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		const int32 Index = Args.Num() > 0 ? FCString::Atoi(*Args[0]) : 0;
		for (TObjectIterator<UApexHotlapWidget> It; It; ++It)
		{
			if (It->GetWorld())
			{
				It->SetTab(static_cast<EApexGarageTab>(FMath::Clamp(Index, 0, static_cast<int32>(EApexGarageTab::Count) - 1)));
			}
		}
	}));

// `apexsim.hotlap.Board [0|1]` opens (or closes) the qualifying scoreboard and
// `apexsim.hotlap.Watch N` watches the Nth row's car, for screenshots and checks.
static FAutoConsoleCommand GHotlapBoardCommand(
	TEXT("apexsim.hotlap.Board"),
	TEXT("Open (1, the default) or close (0) the qualifying garage's scoreboard."),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		const bool bOpen = Args.Num() == 0 || FCString::Atoi(*Args[0]) != 0;
		for (TObjectIterator<UApexHotlapWidget> It; It; ++It)
		{
			if (It->GetWorld())
			{
				It->SetBoardOpen(bOpen);
			}
		}
	}));

static FAutoConsoleCommand GHotlapWatchCommand(
	TEXT("apexsim.hotlap.Watch"),
	TEXT("Watch the car on a scoreboard row (0-based), from the qualifying garage."),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		const int32 Row = Args.Num() > 0 ? FCString::Atoi(*Args[0]) : 0;
		for (TObjectIterator<UApexHotlapWidget> It; It; ++It)
		{
			if (It->GetWorld())
			{
				It->WatchRow(Row);
			}
		}
	}));

UApexHotlapWidget::UApexHotlapWidget(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	SetVisibility(ESlateVisibility::Collapsed);
	SetIsFocusable(true);
}

void UApexHotlapWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();

	UOverlay* Layers = WidgetTree->ConstructWidget<UOverlay>();

	Garage = BuildGarage();
	UOverlaySlot* GarageSlot = Layers->AddChildToOverlay(Garage);
	GarageSlot->SetHorizontalAlignment(HAlign_Fill);
	GarageSlot->SetVerticalAlignment(VAlign_Fill);

	// The scoreboard stands in for the garage card in qualifying: a column on
	// the right with the view clear beside it, for the car being watched.
	BoardLayer = BuildBoard();
	UOverlaySlot* BoardSlot = Layers->AddChildToOverlay(BoardLayer);
	BoardSlot->SetHorizontalAlignment(HAlign_Right);
	BoardSlot->SetVerticalAlignment(VAlign_Fill);
	BoardSlot->SetPadding(FMargin(0.0f, CardInset, CardInset, CardInset));

	TimingPanel = BuildTimingPanel();
	UOverlaySlot* SheetSlot = Layers->AddChildToOverlay(TimingPanel);
	SheetSlot->SetHorizontalAlignment(HAlign_Right);
	SheetSlot->SetVerticalAlignment(VAlign_Center);
	SheetSlot->SetPadding(FMargin(0.0f, 0.0f, Metrics::PageGutter, 0.0f));

	ReplayStrip = BuildReplayStrip();
	UOverlaySlot* StripSlot = Layers->AddChildToOverlay(ReplayStrip);
	StripSlot->SetHorizontalAlignment(HAlign_Center);
	StripSlot->SetVerticalAlignment(VAlign_Top);
	StripSlot->SetPadding(FMargin(0.0f, 34.0f, 0.0f, 0.0f));

	// On the track a one-line reminder of the way back in, under the HUD's top bar.
	TrackHint = MakePanel(*WidgetTree,
		MakeText(*WidgetTree, TEXT("ESC  ·  BACK TO GARAGE"), Font::Mono(11.0f, 100), Palette::TextSecondary),
		FMargin(14.0f, 7.0f), MakeBrush(Palette::Surface.CopyWithNewOpacity(0.72f)));
	UOverlaySlot* HintSlot = Layers->AddChildToOverlay(TrackHint);
	HintSlot->SetHorizontalAlignment(HAlign_Center);
	HintSlot->SetVerticalAlignment(VAlign_Top);
	HintSlot->SetPadding(FMargin(0.0f, Metrics::TopBarHeight + 52.0f, 0.0f, 0.0f));

	WidgetTree->RootWidget = Layers;

	int32 StartTab = 0;
	if (FParse::Value(FCommandLine::Get(), TEXT("ApexGarageTab="), StartTab))
	{
		Tab = static_cast<EApexGarageTab>(FMath::Clamp(StartTab, 0, static_cast<int32>(EApexGarageTab::Count) - 1));
	}
	ApplyTab();
	ApplyTabVisibility();
	ApplyView();
}

void UApexHotlapWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);
	if (View == EApexHotlapView::Garage && bQualifying && bBoardOpen)
	{
		BoardRefreshIn -= InDeltaTime;
		if (BoardRefreshIn <= 0.0f)
		{
			BoardRefreshIn = 0.25f;
			RefreshBoard();
		}
	}
}

void UApexHotlapWidget::NativeConstruct()
{
	Super::NativeConstruct();
	UGameInstance* GameInstance = GetGameInstance();
	if (UApexNetSubsystem* Net = GameInstance ? GameInstance->GetSubsystem<UApexNetSubsystem>() : nullptr)
	{
		Net->OnLapTiming.AddDynamic(this, &UApexHotlapWidget::HandleLapTiming);
		Net->OnGhostLap.AddDynamic(this, &UApexHotlapWidget::HandleGhostLap);
		Net->OnLapRecord.AddDynamic(this, &UApexHotlapWidget::HandleLapRecord);
		Net->OnCarSetupSheet.AddDynamic(this, &UApexHotlapWidget::HandleSetupSheet);
	}
	if (UApexSettingsSubsystem* Settings = GetSettings())
	{
		Settings->OnSettingsChanged.AddDynamic(this, &UApexHotlapWidget::HandleSettingsChanged);
	}
}

void UApexHotlapWidget::NativeDestruct()
{
	UGameInstance* GameInstance = GetGameInstance();
	if (UApexNetSubsystem* Net = GameInstance ? GameInstance->GetSubsystem<UApexNetSubsystem>() : nullptr)
	{
		Net->OnLapTiming.RemoveDynamic(this, &UApexHotlapWidget::HandleLapTiming);
		Net->OnGhostLap.RemoveDynamic(this, &UApexHotlapWidget::HandleGhostLap);
		Net->OnLapRecord.RemoveDynamic(this, &UApexHotlapWidget::HandleLapRecord);
		Net->OnCarSetupSheet.RemoveDynamic(this, &UApexHotlapWidget::HandleSetupSheet);
	}
	if (UApexSettingsSubsystem* Settings = GetSettings())
	{
		Settings->OnSettingsChanged.RemoveDynamic(this, &UApexHotlapWidget::HandleSettingsChanged);
	}
	Super::NativeDestruct();
}

UApexSettingsSubsystem* UApexHotlapWidget::GetSettings() const
{
	return GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexSettingsSubsystem>() : nullptr;
}

const FApexCarSetup* UApexHotlapWidget::GetWorkingSetup() const
{
	const UApexSettingsSubsystem* Settings = GetSettings();
	return Settings && Settings->Get() ? &Settings->Get()->CarSetup : nullptr;
}

const FApexCarSetupSheet* UApexHotlapWidget::GetSheet() const
{
	const UGameInstance* GameInstance = GetGameInstance();
	const UApexNetSubsystem* Net = GameInstance ? GameInstance->GetSubsystem<UApexNetSubsystem>() : nullptr;
	if (!Net || bMenuMode)
	{
		return nullptr;
	}
	const FApexCarSetupSheet& Sheet = Net->GetCarSetupSheet();
	return Sheet.Knobs.Num() == FApexCarSetup::KnobCount ? &Sheet : nullptr;
}

FString UApexHotlapWidget::GetCarId() const
{
	const UGameInstance* GameInstance = GetGameInstance();
	const UApexMenuFlowSubsystem* Flow = GameInstance ? GameInstance->GetSubsystem<UApexMenuFlowSubsystem>() : nullptr;
	return Flow ? Flow->GetPendingCarId() : FString();
}

const FApexCarSetupSheet* UApexHotlapWidget::GetCompoundSheet() const
{
	const UGameInstance* GameInstance = GetGameInstance();
	const UApexNetSubsystem* Net = GameInstance ? GameInstance->GetSubsystem<UApexNetSubsystem>() : nullptr;
	if (!Net || bMenuMode)
	{
		return nullptr;
	}
	const FApexCarSetupSheet& Sheet = Net->GetCarSetupSheet();
	return Sheet.HasCompounds() ? &Sheet : nullptr;
}

FString UApexHotlapWidget::CompoundLabel(int32 Clicks) const
{
	if (const FApexCarSetupSheet* Sheet = GetCompoundSheet())
	{
		const FString Name = Sheet->CompoundNameAt(Sheet->CompoundIndexForClicks(Clicks));
		return Name.IsEmpty() ? FString(TEXT("?")) : Name;
	}
	return CompoundName(Clicks);
}

TArray<FApexGarageCompound> UApexHotlapWidget::CompoundCardsFor(const FApexCarSetupSheet* Sheet)
{
	TArray<FApexGarageCompound> Cards;
	// The server sends a list for every car; a car that files none of its own
	// gets the five defaults, whose figures the hand-written cards know.
	static const TCHAR* DefaultNames[] = { TEXT("soft"), TEXT("medium"), TEXT("hard"), TEXT("intermediate"), TEXT("wet") };
	bool bDefaultList = Sheet && Sheet->Compounds.Num() == UE_ARRAY_COUNT(DefaultNames) && Sheet->ReferenceCompound == 1;
	for (int32 Index = 0; bDefaultList && Index < UE_ARRAY_COUNT(DefaultNames); ++Index)
	{
		bDefaultList = Sheet->Compounds[Index].Equals(DefaultNames[Index], ESearchCase::IgnoreCase);
	}
	if (!Sheet || !Sheet->HasCompounds() || bDefaultList)
	{
		for (const FCompoundSpec& Default : Compounds)
		{
			FApexGarageCompound& Card = Cards.AddDefaulted_GetRef();
			Card.Clicks = Default.Clicks;
			Card.Name = Default.Name;
			Card.Note = Default.Note;
			Card.Figures = Default.Figures;
			Card.Colour = Default.Colour;
			Card.Grip = Default.Grip;
			Card.Life = Default.Life;
		}
		return Cards;
	}
	// The car's own list: the sheet names them and says which is the
	// reference (click 0); it carries no grip or wear figures, so the cards
	// say only what they are, and the colour is guessed from the name (the
	// slick palette runs soft to hard down the list).
	const FLinearColor Soft = FLinearColor::FromSRGBColor(FColor(0xE0, 0x4B, 0x3C));
	const FLinearColor Medium = FLinearColor::FromSRGBColor(FColor(0xE8, 0xBA, 0x3A));
	auto IsRain = [](const FString& Name) { return Name.Contains(TEXT("wet")) || Name.Contains(TEXT("inter")); };
	int32 Slicks = 0;
	for (const FString& Name : Sheet->Compounds)
	{
		Slicks += IsRain(Name) ? 0 : 1;
	}
	int32 SlickIndex = 0;
	for (int32 Index = 0; Index < Sheet->Compounds.Num(); ++Index)
	{
		const FString& Raw = Sheet->Compounds[Index];
		FApexGarageCompound& Card = Cards.AddDefaulted_GetRef();
		Card.Clicks = Sheet->ClicksForCompoundIndex(Index);
		Card.Name = Raw.ToUpper();
		const bool bReference = Index == Sheet->ReferenceCompound;
		if (Raw.Contains(TEXT("wet")))
		{
			Card.Note = TEXT("Standing water.");
			Card.Colour = FLinearColor::FromSRGBColor(FColor(0x2F, 0x7F, 0xE0));
			Card.Grip = 0.35f;
			Card.Life = 0.30f;
		}
		else if (Raw.Contains(TEXT("inter")))
		{
			Card.Note = TEXT("A damp or drying track.");
			Card.Colour = FLinearColor::FromSRGBColor(FColor(0x3C, 0xB0, 0x4B));
			Card.Grip = 0.50f;
			Card.Life = 0.40f;
		}
		else
		{
			Card.Note = bReference ? TEXT("The car's own tyre.") : TEXT("A compound of this car.");
			// Softest first: red through amber to the hard's plain ink.
			const float T = Slicks > 1 ? static_cast<float>(SlickIndex) / (Slicks - 1) : 0.5f;
			Card.Colour = T < 0.5f ? FLinearColor::LerpUsingHSV(Soft, Medium, T * 2.0f)
								   : FLinearColor::LerpUsingHSV(Medium, Ink, (T - 0.5f) * 2.0f);
			Card.Grip = FMath::Lerp(0.94f, 0.66f, T);
			Card.Life = FMath::Lerp(0.30f, 1.00f, T);
			++SlickIndex;
		}
		Card.Figures = bReference ? TEXT("AS FILED  ·  THE REFERENCE") : TEXT("ONE OF THIS CAR'S OWN COMPOUNDS");
	}
	return Cards;
}

FString UApexHotlapWidget::DescribeValue(int32 Knob, int32 Clicks) const
{
	if (Knob == ApexCarSetup::TyreCompound)
	{
		return CompoundLabel(Clicks);
	}
	if (Knob == ApexCarSetup::BrakePads || Knob == ApexCarSetup::ErsStartMode || Knob == ApexCarSetup::ErsDeployMap)
	{
		// A choice, not a figure: the sheet's number for it is a dummy.
		return ApexCarSetup::Describe(Knob, Clicks).ToUpper();
	}
	if (const FApexCarSetupSheet* Sheet = GetSheet())
	{
		const FApexSetupKnobFigure& Figure = Sheet->Knobs[Knob];
		const FString Number = Fixed(SheetValue(Figure, Clicks), Figure.Decimals);
		return Figure.Unit.IsEmpty() ? Number : Number + TEXT(" ") + Figure.Unit;
	}
	// No sheet (an older server, or not yet arrived): clicks and their effect.
	return ApexCarSetup::Describe(Knob, Clicks);
}

FString UApexHotlapWidget::DescribeDelta(int32 Knob, int32 Clicks) const
{
	if (Clicks == 0 || Knob == ApexCarSetup::TyreCompound || Knob == ApexCarSetup::BrakePads
		|| Knob == ApexCarSetup::ErsStartMode || Knob == ApexCarSetup::ErsDeployMap)
	{
		return FString();
	}
	if (const FApexCarSetupSheet* Sheet = GetSheet())
	{
		const FApexSetupKnobFigure& Figure = Sheet->Knobs[Knob];
		const float Delta = SheetValue(Figure, Clicks) - SheetValue(Figure, 0);
		const FString Number = Fixed(Delta, Figure.Decimals);
		return Delta >= 0.0f ? TEXT("+") + Number : Number;
	}
	return FString::Printf(TEXT("%+d CLICK%s"), Clicks, FMath::Abs(Clicks) == 1 ? TEXT("") : TEXT("S"));
}

// --- Construction -----------------------------------------------------------

UApexButtonWidget* UApexHotlapWidget::MakeActionButton(
	const FString& Label, const FString& Badge, FName ActionId, EApexButtonVariant Variant, float Height, float LabelSize)
{
	FApexButtonSpec Spec;
	Spec.Label = Label;
	Spec.Badge = Badge;
	Spec.ActionId = ActionId;
	Spec.Variant = Variant;
	Spec.Height = Height;
	Spec.LabelSize = LabelSize;
	Spec.bCentreLabel = Variant == EApexButtonVariant::Ghost;
	UApexButtonWidget* Button = WidgetTree->ConstructWidget<UApexButtonWidget>();
	Button->Setup(Spec);
	Button->OnActivated.AddDynamic(this, &UApexHotlapWidget::HandleButtonActivated);
	return Button;
}

UApexStepperWidget* UApexHotlapWidget::MakeStepper(int32 Knob)
{
	UApexStepperWidget* Stepper = WidgetTree->ConstructWidget<UApexStepperWidget>();
	Stepper->Index = Knob;
	Stepper->ControlId = FName(ApexCarSetup::Knob(Knob).Key);
	Stepper->Formatter = [this, Knob](int32 Value) { return DescribeValue(Knob, Value); };
	Stepper->DeltaFormatter = [this, Knob](int32 Value) { return DescribeDelta(Knob, Value); };
	const ApexCarSetup::FKnob& Spec = ApexCarSetup::Knob(Knob);
	Stepper->Setup(Spec.Min, Spec.Max, 0, StepperReadout);
	Stepper->OnChanged.AddDynamic(this, &UApexHotlapWidget::HandleStepperChanged);
	SetupSteppers.Add(Knob, Stepper);
	return Stepper;
}

UWidget* UApexHotlapWidget::BuildGarage()
{
	UVerticalBox* Card = WidgetTree->ConstructWidget<UVerticalBox>();
	AddV(Card, BuildGarageHeader());
	AddV(Card, Rule(*WidgetTree));

	UHorizontalBox* Body = WidgetTree->ConstructWidget<UHorizontalBox>();
	AddH(Body, MakeSized(*WidgetTree, BuildActionColumn(), ActionColumnWidth, -1.0f), FMargin(), VAlign_Fill);
	AddH(Body, Rule(*WidgetTree, true), FMargin(), VAlign_Fill);

	UVerticalBox* Right = WidgetTree->ConstructWidget<UVerticalBox>();
	AddV(Right, BuildTabBar());
	AddV(Right, Rule(*WidgetTree));
	Pages = WidgetTree->ConstructWidget<UWidgetSwitcher>();
	Pages->AddChild(BuildTyresPage());
	Pages->AddChild(BuildSuspensionPage());
	Pages->AddChild(BuildEnginePage());
	Pages->AddChild(BuildAeroPage());
	Pages->AddChild(BuildErsPage());
	Pages->AddChild(BuildSavePage());
	AddV(Right, MakePanel(*WidgetTree, Pages, FMargin(PagePadX, PagePadY), MakeBrush(FLinearColor::Transparent)),
		FMargin(), HAlign_Fill, 1.0f);
	AddH(Body, Right, FMargin(), VAlign_Fill, 1.0f);
	AddV(Card, Body, FMargin(), HAlign_Fill, 1.0f);

	// Footer: the keys, and what a change means.
	AddV(Card, Rule(*WidgetTree));
	UHorizontalBox* Footer = WidgetTree->ConstructWidget<UHorizontalBox>();
	const TPair<const TCHAR*, const TCHAR*> Hints[] = {
		{ TEXT("↑↓←→"), TEXT("MOVE") }, { TEXT("ENTER"), TEXT("SELECT") }, { TEXT("Q  E"), TEXT("TABS") }, { TEXT("ESC"), TEXT("MENU") } };
	for (const TPair<const TCHAR*, const TCHAR*>& Hint : Hints)
	{
		AddH(Footer, MakeKeyCap(*WidgetTree, Hint.Key, Ink, Ink.CopyWithNewOpacity(0.2f)), FMargin(0.0f, 0.0f, 8.0f, 0.0f));
		AddH(Footer, Caption(*WidgetTree, Hint.Value), FMargin(0.0f, 0.0f, 30.0f, 0.0f));
	}
	AddH(Footer, WidgetTree->ConstructWidget<UHorizontalBox>(), FMargin(), VAlign_Center, 1.0f);
	AddH(Footer, Caption(*WidgetTree, TEXT("Changes apply on the next run")));
	UBorder* FooterPanel = MakePanel(*WidgetTree, Footer, FMargin(PagePadX, 0.0f), MakeBrush(FLinearColor::Transparent));
	FooterPanel->SetVerticalAlignment(VAlign_Center);
	AddV(Card, MakeSized(*WidgetTree, FooterPanel, -1.0f, FooterHeight));

	UBorder* CardPanel = MakePanel(*WidgetTree, Card, FMargin(), MakeBrush(CardFill, Outline, 1.0f));
	UBorder* Inset = MakePanel(*WidgetTree, CardPanel, FMargin(CardInset), MakeBrush(FLinearColor::Transparent));
	return MakeScrim(*WidgetTree, Inset, 10.0f);
}

UWidget* UApexHotlapWidget::BuildGarageHeader()
{
	UHorizontalBox* Header = WidgetTree->ConstructWidget<UHorizontalBox>();

	UVerticalBox* Title = WidgetTree->ConstructWidget<UVerticalBox>();
	GarageCaption = Caption(*WidgetTree, TEXT("Hotlap"), Palette::Accent);
	AddV(Title, GarageCaption);
	UHorizontalBox* TitleLine = WidgetTree->ConstructWidget<UHorizontalBox>();
	AddH(TitleLine, MakeText(*WidgetTree, TEXT("GARAGE"), Font::Display(44.0f, 10), Ink), FMargin(), VAlign_Bottom);
	GarageSubtitle = MakeText(*WidgetTree, FString(), Font::Mono(13.0f, 60), Ink.CopyWithNewOpacity(0.75f));
	AddH(TitleLine, GarageSubtitle, FMargin(22.0f, 0.0f, 0.0f, 8.0f), VAlign_Bottom);
	AddV(Title, TitleLine, FMargin(0.0f, 6.0f, 0.0f, 0.0f));
	AddH(Header, Title, FMargin(), VAlign_Center, 1.0f);
	UApexButtonWidget* Tune = nullptr;
	UApexButtonWidget* Board = nullptr;
	GaragePageToggle = BuildPageToggle(Tune, Board);
	GarageTuneButton = Tune;
	GarageBoardButton = Board;
	GaragePageToggle->SetVisibility(ESlateVisibility::Collapsed);
	AddH(Header, GaragePageToggle, FMargin(0.0f, 0.0f, 12.0f, 0.0f), VAlign_Center);

	UVerticalBox* Setup = WidgetTree->ConstructWidget<UVerticalBox>();
	AddV(Setup, Caption(*WidgetTree, TEXT("Setup")), FMargin(), HAlign_Right);
	UHorizontalBox* SetupLine = WidgetTree->ConstructWidget<UHorizontalBox>();
	SetupNameText = MakeText(*WidgetTree, TEXT("Stock"), Font::Display(24.0f), Ink);
	AddH(SetupLine, SetupNameText, FMargin(), VAlign_Bottom);
	SetupChangesText = MakeText(*WidgetTree, TEXT("STOCK"), Font::Mono(11.0f, 40), Palette::Accent);
	AddH(SetupLine, SetupChangesText, FMargin(14.0f, 0.0f, 0.0f, 4.0f), VAlign_Bottom);
	AddV(Setup, SetupLine, FMargin(0.0f, 5.0f, 0.0f, 0.0f), HAlign_Right);
	AddH(Header, Setup, FMargin(24.0f, 0.0f, 0.0f, 0.0f), VAlign_Center);

	UBorder* Panel = MakePanel(*WidgetTree, Header, FMargin(PagePadX, 0.0f), MakeBrush(FLinearColor::Transparent));
	Panel->SetVerticalAlignment(VAlign_Center);
	return MakeSized(*WidgetTree, Panel, -1.0f, HeaderHeight);
}

UWidget* UApexHotlapWidget::BuildActionColumn()
{
	UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>();
	GoOutButton = MakeActionButton(TEXT("GO OUT ON TRACK"), FString(), ActionGoOut, EApexButtonVariant::Primary, 72.0f, 20.0f);
	AddV(Column, GoOutButton);
	ReplayButton = MakeActionButton(TEXT("REPLAY LAP"), TEXT("No lap yet"), ActionReplay, EApexButtonVariant::Panel, 64.0f, 20.0f);
	AddV(Column, ReplayButton, FMargin(0.0f, 8.0f, 0.0f, 0.0f));
	GhostButton = MakeActionButton(TEXT("GHOST CAR"), TEXT("No lap yet"), ActionGhost, EApexButtonVariant::Panel, 64.0f, 20.0f);
	AddV(Column, GhostButton, FMargin(0.0f, 8.0f, 0.0f, 0.0f));
	// Out of the garage the tyres go on at their optimum, so a hotlap measures
	// the car and not its warm-up; a driver who wants the warm-up asks for
	// cold tyres here (blankets or the air, as from the garage).
	TyresButton = MakeActionButton(TEXT("TYRES OUT"), TEXT("Warm"), ActionTyres, EApexButtonVariant::Panel, 64.0f, 20.0f);
	AddV(Column, TyresButton, FMargin(0.0f, 8.0f, 0.0f, 0.0f));
	// The laps driven so far, kept to watch again (the Replays screen).
	SaveReplayButton = MakeActionButton(TEXT("SAVE REPLAY"), TEXT("None"), ActionGarageSaveReplay, EApexButtonVariant::Panel, 64.0f, 20.0f);
	AddV(Column, SaveReplayButton, FMargin(0.0f, 8.0f, 0.0f, 0.0f));
	AddV(Column, Rule(*WidgetTree), FMargin(0.0f, 18.0f));
	ResetButton = MakeActionButton(TEXT("RESET SETUP"), TEXT("Stock"), ActionResetSetup, EApexButtonVariant::Panel, 64.0f, 20.0f);
	AddV(Column, ResetButton);

	AddV(Column, WidgetTree->ConstructWidget<UVerticalBox>(), FMargin(), HAlign_Fill, 1.0f);
	UTextBlock* Note = MakeText(*WidgetTree,
		TEXT("The car spawns on the run-up before the line, so the first lap is a flying one. Fuel and tyres are put in here, in the garage."),
		Font::Body(14.0f), InkMuted);
	Note->SetAutoWrapText(true);
	GarageNote = Note;
	AddV(Column, Note);

	return MakePanel(*WidgetTree, Column, FMargin(24.0f, 26.0f), MakeBrush(FLinearColor::Transparent));
}

UWidget* UApexHotlapWidget::BuildPageToggle(UApexButtonWidget*& OutTune, UApexButtonWidget*& OutBoard)
{
	UHorizontalBox* Pair = WidgetTree->ConstructWidget<UHorizontalBox>();
	OutTune = MakeActionButton(TEXT("TUNE"), FString(), ActionTune, EApexButtonVariant::Ghost, 42.0f, 14.0f);
	OutBoard = MakeActionButton(TEXT("SCOREBOARD"), FString(), ActionBoard, EApexButtonVariant::Ghost, 42.0f, 14.0f);
	AddH(Pair, MakeSized(*WidgetTree, OutTune, 92.0f, 42.0f), FMargin(), VAlign_Center);
	AddH(Pair, MakeSized(*WidgetTree, OutBoard, 146.0f, 42.0f), FMargin(6.0f, 0.0f, 0.0f, 0.0f), VAlign_Center);
	return Pair;
}

UWidget* UApexHotlapWidget::BuildBoard()
{
	UVerticalBox* Card = WidgetTree->ConstructWidget<UVerticalBox>();

	UHorizontalBox* Head = WidgetTree->ConstructWidget<UHorizontalBox>();
	UVerticalBox* Title = WidgetTree->ConstructWidget<UVerticalBox>();
	AddV(Title, Caption(*WidgetTree, TEXT("Qualifying"), Palette::Accent));
	AddV(Title, MakeText(*WidgetTree, TEXT("SCOREBOARD"), Font::Display(28.0f, 10), Ink), FMargin(0.0f, 4.0f, 0.0f, 0.0f));
	AddH(Head, Title, FMargin(), VAlign_Center, 1.0f);
	UApexButtonWidget* Tune = nullptr;
	UApexButtonWidget* Board = nullptr;
	AddH(Head, BuildPageToggle(Tune, Board), FMargin(), VAlign_Center);
	BoardTuneButton = Tune;
	BoardBoardButton = Board;
	AddV(Card, MakePanel(*WidgetTree, Head, FMargin(24.0f, 18.0f), MakeBrush(FLinearColor::Transparent)));
	AddV(Card, Rule(*WidgetTree));

	// Column captions over the rows.
	UHorizontalBox* Captions = WidgetTree->ConstructWidget<UHorizontalBox>();
	AddH(Captions, MakeSized(*WidgetTree, Caption(*WidgetTree, TEXT("P")), 44.0f, -1.0f));
	AddH(Captions, Caption(*WidgetTree, TEXT("Driver")), FMargin(), VAlign_Center, 1.0f);
	AddH(Captions, MakeSized(*WidgetTree, Caption(*WidgetTree, TEXT("Best")), 120.0f, -1.0f));
	AddH(Captions, MakeSized(*WidgetTree, Caption(*WidgetTree, TEXT("Gap")), 84.0f, -1.0f));
	AddH(Captions, MakeSized(*WidgetTree, Caption(*WidgetTree, TEXT("")), 78.0f, -1.0f));
	AddV(Card, MakePanel(*WidgetTree, Captions, FMargin(24.0f, 12.0f, 24.0f, 6.0f), MakeBrush(FLinearColor::Transparent)));

	UVerticalBox* Rows = WidgetTree->ConstructWidget<UVerticalBox>();
	BoardRowButtons.Reset();
	BoardRowRoots.Reset();
	BoardPos.Reset();
	BoardName.Reset();
	BoardBest.Reset();
	BoardGap.Reset();
	BoardState.Reset();
	BoardCars.Init(INDEX_NONE, BoardRowCount);
	for (int32 Index = 0; Index < BoardRowCount; ++Index)
	{
		UApexButtonWidget* Button = MakeActionButton(FString(), FString(), ActionWatch, EApexButtonVariant::Panel, BoardRowHeight, 14.0f);
		UHorizontalBox* Cells = WidgetTree->ConstructWidget<UHorizontalBox>();
		UTextBlock* Pos = MakeText(*WidgetTree, FString(), Font::Mono(12.0f), InkMuted);
		UTextBlock* Name = MakeText(*WidgetTree, FString(), Font::Display(17.0f), Ink);
		UTextBlock* Best = MakeText(*WidgetTree, FString(), Font::Mono(13.0f), Ink);
		UTextBlock* Gap = MakeText(*WidgetTree, FString(), Font::Mono(12.0f), InkMuted);
		UTextBlock* State = MakeText(*WidgetTree, FString(), Font::Mono(10.0f, 80), InkFaint);
		AddH(Cells, MakeSized(*WidgetTree, Pos, 44.0f, -1.0f), FMargin(), VAlign_Center);
		AddH(Cells, Name, FMargin(), VAlign_Center, 1.0f);
		AddH(Cells, MakeSized(*WidgetTree, Best, 120.0f, -1.0f), FMargin(), VAlign_Center);
		AddH(Cells, MakeSized(*WidgetTree, Gap, 84.0f, -1.0f), FMargin(), VAlign_Center);
		AddH(Cells, MakeSized(*WidgetTree, State, 78.0f, -1.0f), FMargin(), VAlign_Center);
		UOverlay* Row = ButtonWithContent(*WidgetTree, Button, Cells, FMargin(14.0f, 0.0f));
		BoardRowButtons.Add(Button);
		BoardRowRoots.Add(Row);
		BoardPos.Add(Pos);
		BoardName.Add(Name);
		BoardBest.Add(Best);
		BoardGap.Add(Gap);
		BoardState.Add(State);
		AddV(Rows, MakeSized(*WidgetTree, Row, -1.0f, BoardRowHeight), FMargin(0.0f, 0.0f, 0.0f, 2.0f));
	}
	AddV(Card, MakePanel(*WidgetTree, ScrollPage(*WidgetTree, Rows), FMargin(24.0f, 0.0f), MakeBrush(FLinearColor::Transparent)),
		FMargin(), HAlign_Fill, 1.0f);

	AddV(Card, Rule(*WidgetTree));
	UVerticalBox* Foot = WidgetTree->ConstructWidget<UVerticalBox>();
	BoardHint = MakeText(*WidgetTree, TEXT("CLICK A DRIVER TO WATCH THEIR CAR"), Font::Mono(11.0f, 60), InkMuted);
	AddV(Foot, BoardHint, FMargin(0.0f, 0.0f, 0.0f, 10.0f));
	UHorizontalBox* FootButtons = WidgetTree->ConstructWidget<UHorizontalBox>();
	BoardCameraButton = MakeActionButton(TEXT("CAMERA"), TEXT("TV"), ActionWatchCamera, EApexButtonVariant::Panel, 46.0f, 15.0f);
	AddH(FootButtons, BoardCameraButton, FMargin(), VAlign_Fill, 1.0f);
	BoardGoOutButton = MakeActionButton(TEXT("GO OUT FOR A LAP"), FString(), ActionGoOut, EApexButtonVariant::Primary, 46.0f, 15.0f);
	AddH(FootButtons, BoardGoOutButton, FMargin(8.0f, 0.0f, 0.0f, 0.0f), VAlign_Fill, 1.0f);
	AddV(Foot, FootButtons);
	AddV(Card, MakePanel(*WidgetTree, Foot, FMargin(24.0f, 12.0f), MakeBrush(FLinearColor::Transparent)));

	UBorder* Panel = MakePanel(*WidgetTree, Card, FMargin(), MakeBrush(CardFill, Outline, 1.0f));
	return MakeSized(*WidgetTree, Panel, BoardWidth, -1.0f);
}

UWidget* UApexHotlapWidget::BuildTabBar()
{
	UHorizontalBox* Bar = WidgetTree->ConstructWidget<UHorizontalBox>();
	for (int32 Index = 0; Index < static_cast<int32>(EApexGarageTab::Count); ++Index)
	{
		FApexButtonSpec Spec;
		Spec.Variant = EApexButtonVariant::Bare;
		Spec.ActionId = ActionTab;
		Spec.Height = TabBarHeight - 3.0f;
		Spec.Sound = EApexUiSound::Adjust;
		UApexButtonWidget* Button = WidgetTree->ConstructWidget<UApexButtonWidget>();
		Button->Setup(Spec);
		Button->OnActivated.AddDynamic(this, &UApexHotlapWidget::HandleButtonActivated);
		TabButtons.Add(Button);

		UHorizontalBox* Label = WidgetTree->ConstructWidget<UHorizontalBox>();
		UTextBlock* Number = MakeText(*WidgetTree, FString::FromInt(Index + 1), Font::Mono(10.0f), InkMuted);
		AddH(Label, Number, FMargin(0.0f, 0.0f, 12.0f, 0.0f));
		UTextBlock* Name = MakeText(*WidgetTree, TabSpecs[Index].Label, Font::Display(21.0f, 30), InkMuted);
		AddH(Label, Name);
		TabNumbers.Add(Number);
		TabLabels.Add(Name);

		UVerticalBox* Stack = WidgetTree->ConstructWidget<UVerticalBox>();
		AddV(Stack, ButtonWithContent(*WidgetTree, Button, Label, FMargin(26.0f, 0.0f)));
		UBorder* Underline = MakePanel(*WidgetTree, nullptr, FMargin(), MakeBrush(Palette::Accent));
		AddV(Stack, MakeSized(*WidgetTree, Underline, -1.0f, 3.0f));
		TabUnderlines.Add(Underline);
		TabColumns.Add(Stack);
		AddH(Bar, Stack, FMargin(0.0f, 0.0f, 4.0f, 0.0f), VAlign_Bottom);
	}
	AddH(Bar, WidgetTree->ConstructWidget<UHorizontalBox>(), FMargin(), VAlign_Center, 1.0f);
	AddH(Bar, MakeKeyCap(*WidgetTree, TEXT("Q"), InkMuted, Ink.CopyWithNewOpacity(0.2f)), FMargin(0.0f, 0.0f, 6.0f, 0.0f));
	AddH(Bar, MakeKeyCap(*WidgetTree, TEXT("E"), InkMuted, Ink.CopyWithNewOpacity(0.2f)), FMargin(0.0f, 0.0f, 8.0f, 0.0f));
	AddH(Bar, Caption(*WidgetTree, TEXT("Tabs")));
	UBorder* Panel = MakePanel(*WidgetTree, Bar, FMargin(PagePadX, 0.0f, PagePadX, 0.0f), MakeBrush(FLinearColor::Transparent));
	Panel->SetVerticalAlignment(VAlign_Bottom);
	return MakeSized(*WidgetTree, Panel, -1.0f, TabBarHeight);
}

void UApexHotlapWidget::AddSection(UVerticalBox* Column, const TCHAR* Title, std::initializer_list<int32> Knobs)
{
	struct FNote
	{
		int32 Knob;
		const TCHAR* Label;
		const TCHAR* Note;
	};
	static const FNote Notes[] = {
		{ ApexCarSetup::TyrePressureFront, TEXT("Front pressure"), TEXT("Grip falls off either side of the hot target.") },
		{ ApexCarSetup::TyrePressureRear, TEXT("Rear pressure"), TEXT("Same target. Lower rears calm power-on slides.") },
		{ ApexCarSetup::BrakeBias, TEXT("Brake bias"), TEXT("Front share of the braking.") },
		{ ApexCarSetup::BrakeDucts, TEXT("Front brake ducts"), TEXT("Cooler front brakes, a little drag.") },
		{ ApexCarSetup::BrakeDuctsRear, TEXT("Rear brake ducts"), TEXT("Cooler rear brakes, a little drag.") },
		{ ApexCarSetup::BrakePads, TEXT("Brake pads"), TEXT("Endurance pads last; sprint pads bite and wear.") },
		{ ApexCarSetup::Radiator, TEXT("Radiator"), TEXT("More air through it (+): a cooler engine for drag.") },
		{ ApexCarSetup::RevLimiter, TEXT("Rev limiter"), TEXT("Redline. Down only.") },
		{ ApexCarSetup::EngineBraking, TEXT("Engine braking"), TEXT("Drag off throttle.") },
		{ ApexCarSetup::TorqueMap, TEXT("Torque map"), TEXT("Scales the whole curve. Down only.") },
		{ ApexCarSetup::FinalDrive, TEXT("Final drive"), TEXT("Shorter (+) pulls harder, lower top speed.") },
		{ ApexCarSetup::GearSpread, TEXT("Gear spread"), TEXT("Top gear closer (+); first stays put.") },
		{ ApexCarSetup::FuelLoad, TEXT("Fuel load"), TEXT("Laps on the fill. Weight is lap time.") },
		{ ApexCarSetup::ErsStartMode, TEXT("Starting mode"), TEXT("The mode the car leaves the garage or the grid in.") },
		{ ApexCarSetup::ErsRegen, TEXT("Regen level"), TEXT("Less: less charge, more of the braking on the discs. Down only.") },
		{ ApexCarSetup::ErsDeployMap, TEXT("Deploy map"), TEXT("Where Balanced spends the lap's budget: early or late.") },
	};

	UVerticalBox* Section = WidgetTree->ConstructWidget<UVerticalBox>();
	AddV(Section, Caption(*WidgetTree, Title), FMargin(0.0f, 0.0f, 0.0f, 9.0f));
	for (const int32 Knob : Knobs)
	{
		const FNote* Found = nullptr;
		for (const FNote& Entry : Notes)
		{
			if (Entry.Knob == Knob)
			{
				Found = &Entry;
			}
		}
		UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();
		UVerticalBox* Text = WidgetTree->ConstructWidget<UVerticalBox>();
		AddV(Text, MakeText(*WidgetTree, Found ? Found->Label : TEXT(""), Font::Body(19.0f, true), Ink));
		UTextBlock* Note = MakeText(*WidgetTree, Found ? Found->Note : TEXT(""), Font::Body(14.0f), InkMuted);
		Note->SetAutoWrapText(true);
		AddV(Text, Note, FMargin(0.0f, 3.0f, 0.0f, 0.0f));
		if (Knob == ApexCarSetup::TyrePressureFront)
		{
			PressureNote = Note;
		}
		AddH(Row, Text, FMargin(), VAlign_Center, 1.0f);
		UApexStepperWidget* Stepper = MakeStepper(Knob);
		AddH(Row, MakeSized(*WidgetTree, Stepper, StepperWidth, -1.0f), FMargin(24.0f, 0.0f, 0.0f, 0.0f), VAlign_Center);
		AddV(Section, MakePanel(*WidgetTree, Row, FMargin(20.0f, 11.0f, 14.0f, 11.0f), MakeBrush(RowFill)),
			FMargin(0.0f, Section->GetChildrenCount() <= 1 ? 0.0f : 2.0f, 0.0f, 0.0f));
	}
	AddV(Column, Section, FMargin(0.0f, Column->GetChildrenCount() == 0 ? 0.0f : 22.0f, 0.0f, 0.0f));
}

void UApexHotlapWidget::AddPairHeader(UVerticalBox* Table)
{
	UHorizontalBox* Head = WidgetTree->ConstructWidget<UHorizontalBox>();
	AddH(Head, Caption(*WidgetTree, TEXT("Setting")), FMargin(), VAlign_Center, 1.0f);
	UTextBlock* FrontCaption = Caption(*WidgetTree, TEXT("Front"));
	FrontCaption->SetJustification(ETextJustify::Center);
	AddH(Head, MakeSized(*WidgetTree, FrontCaption, StepperWidth, -1.0f), FMargin(30.0f, 0.0f, 0.0f, 0.0f));
	UTextBlock* RearCaption = Caption(*WidgetTree, TEXT("Rear"));
	RearCaption->SetJustification(ETextJustify::Center);
	AddH(Head, MakeSized(*WidgetTree, RearCaption, StepperWidth, -1.0f), FMargin(30.0f, 0.0f, 0.0f, 0.0f));
	AddV(Table, Head, FMargin(20.0f, 0.0f, 14.0f, 6.0f));
}

UWidget* UApexHotlapWidget::AddPairRow(UVerticalBox* Table, const TCHAR* Label, const TCHAR* Note, int32 Front, int32 Rear)
{
	UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();
	UVerticalBox* Text = WidgetTree->ConstructWidget<UVerticalBox>();
	AddV(Text, MakeText(*WidgetTree, Label, Font::Body(19.0f, true), Ink));
	UTextBlock* NoteText = MakeText(*WidgetTree, Note, Font::Body(14.0f), InkMuted);
	NoteText->SetAutoWrapText(true);
	AddV(Text, NoteText, FMargin(0.0f, 3.0f, 0.0f, 0.0f));
	AddH(Row, Text, FMargin(), VAlign_Center, 1.0f);
	AddH(Row, MakeSized(*WidgetTree, MakeStepper(Front), StepperWidth, -1.0f), FMargin(30.0f, 0.0f, 0.0f, 0.0f), VAlign_Center);
	AddH(Row, MakeSized(*WidgetTree, MakeStepper(Rear), StepperWidth, -1.0f), FMargin(30.0f, 0.0f, 0.0f, 0.0f), VAlign_Center);
	UBorder* Panel = MakePanel(*WidgetTree, Row, FMargin(20.0f, 11.0f, 14.0f, 11.0f), MakeBrush(RowFill));
	AddV(Table, Panel, FMargin(0.0f, 2.0f, 0.0f, 0.0f));
	return Panel;
}

UWidget* UApexHotlapWidget::BuildTyresPage()
{
	UHorizontalBox* Page = WidgetTree->ConstructWidget<UHorizontalBox>();

	UVerticalBox* Left = WidgetTree->ConstructWidget<UVerticalBox>();
	AddSection(Left, TEXT("Pressure · hot"), { ApexCarSetup::TyrePressureFront, ApexCarSetup::TyrePressureRear });
	AddSection(Left, TEXT("Brakes"), { ApexCarSetup::BrakeBias, ApexCarSetup::BrakeDucts, ApexCarSetup::BrakeDuctsRear, ApexCarSetup::BrakePads });
	AddH(Page, ScrollPage(*WidgetTree, Left), FMargin(), VAlign_Fill, 1.0f);

	UVerticalBox* Right = WidgetTree->ConstructWidget<UVerticalBox>();
	AddV(Right, Caption(*WidgetTree, TEXT("Next tyres · fitted here or at a stop")), FMargin(0.0f, 0.0f, 0.0f, 12.0f));
	// The cards live in a box of their own: the sheet may rename them after
	// the page is built (a car with compounds of its own).
	CompoundHost = WidgetTree->ConstructWidget<UVerticalBox>();
	AddV(Right, CompoundHost);
	BuildCompoundCards();
	// Five tyres: the column scrolls on a short screen.
	AddH(Page, MakeSized(*WidgetTree, ScrollPage(*WidgetTree, Right), SidePanelWidth, -1.0f), FMargin(36.0f, 0.0f, 0.0f, 0.0f), VAlign_Fill);

	PageDefaults.Add(SetupSteppers.FindRef(ApexCarSetup::TyrePressureFront));
	return Page;
}

void UApexHotlapWidget::BuildCompoundCards()
{
	if (!CompoundHost)
	{
		return;
	}
	const FApexCarSetupSheet* Sheet = GetCompoundSheet();
	CompoundCardNames = Sheet ? Sheet->Compounds : TArray<FString>();
	CompoundCards = CompoundCardsFor(Sheet);
	CompoundHost->ClearChildren();
	CompoundButtons.Reset();
	for (const FApexGarageCompound& Compound : CompoundCards)
	{
		FApexButtonSpec Spec;
		Spec.Variant = EApexButtonVariant::Panel;
		Spec.ActionId = ActionCompound;
		Spec.Height = 104.0f;
		Spec.Sound = EApexUiSound::Adjust;
		UApexButtonWidget* Button = WidgetTree->ConstructWidget<UApexButtonWidget>();
		Button->Setup(Spec);
		Button->OnActivated.AddDynamic(this, &UApexHotlapWidget::HandleButtonActivated);
		CompoundButtons.Add(Button);

		UVerticalBox* Content = WidgetTree->ConstructWidget<UVerticalBox>();
		UHorizontalBox* Head = WidgetTree->ConstructWidget<UHorizontalBox>();
		// The sidewall's colour band as a ring.
		UBorder* Ring = MakePanel(*WidgetTree, nullptr, FMargin(), MakeBrush(FLinearColor::Transparent, Compound.Colour, 3.0f, 8.0f));
		AddH(Head, MakeSized(*WidgetTree, Ring, 16.0f, 16.0f), FMargin(0.0f, 0.0f, 12.0f, 0.0f));
		AddH(Head, MakeText(*WidgetTree, Compound.Name, Font::Display(22.0f, 20), Ink));
		// The note takes what the name leaves, right-aligned and cut short with
		// an ellipsis: a long name of a car's own ran straight into it.
		UTextBlock* NoteText = MakeText(*WidgetTree, Compound.Note, Font::Body(14.0f), InkMuted);
		NoteText->SetJustification(ETextJustify::Right);
		NoteText->SetTextOverflowPolicy(ETextOverflowPolicy::Ellipsis);
		NoteText->SetClipping(EWidgetClipping::ClipToBounds);
		AddH(Head, NoteText, FMargin(14.0f, 0.0f, 0.0f, 0.0f), VAlign_Center, 1.0f);
		AddV(Content, Head);
		UTextBlock* Figures = Caption(*WidgetTree, Compound.Figures, InkFaint);
		Figures->SetTextOverflowPolicy(ETextOverflowPolicy::Ellipsis);
		Figures->SetClipping(EWidgetClipping::ClipToBounds);
		AddV(Content, Figures, FMargin(28.0f, 4.0f, 0.0f, 10.0f));
		const TPair<const TCHAR*, float> Bars[] = { { TEXT("GRIP"), Compound.Grip }, { TEXT("LIFE"), Compound.Life } };
		for (const TPair<const TCHAR*, float>& Bar : Bars)
		{
			UHorizontalBox* Line = WidgetTree->ConstructWidget<UHorizontalBox>();
			AddH(Line, MakeSized(*WidgetTree, Caption(*WidgetTree, Bar.Key), 52.0f, -1.0f));
			AddH(Line, ShareBar(*WidgetTree, Bar.Value, Ink), FMargin(), VAlign_Center, 1.0f);
			AddV(Content, Line, FMargin(0.0f, 3.0f));
		}
		AddV(CompoundHost, ButtonWithContent(*WidgetTree, Button, Content, FMargin(20.0f, 0.0f)), FMargin(0.0f, 0.0f, 0.0f, 8.0f));
	}
	RefreshCompounds();
}

UWidget* UApexHotlapWidget::BuildSuspensionPage()
{
	UVerticalBox* Page = WidgetTree->ConstructWidget<UVerticalBox>();
	AddPairHeader(Page);
	AddPairRow(Page, TEXT("Springs"), TEXT("Stiffer: less dive and roll. Softer rears put power down."),
		ApexCarSetup::SpringFront, ApexCarSetup::SpringRear);
	AddPairRow(Page, TEXT("Dampers"), TEXT("Bump and rebound together."),
		ApexCarSetup::DamperFront, ApexCarSetup::DamperRear);
	AddPairRow(Page, TEXT("Anti-roll bar"), TEXT("Stiffer front: more understeer. Stiffer rear: more oversteer."),
		ApexCarSetup::AntiRollFront, ApexCarSetup::AntiRollRear);
	CamberRow = AddPairRow(Page, TEXT("Camber"), TEXT("More negative: grip in the corner, less under braking."),
		ApexCarSetup::CamberFront, ApexCarSetup::CamberRear);
	AddPairRow(Page, TEXT("Toe"), TEXT("Toe-in (+) steadies the car; toe-out sharpens turn-in."),
		ApexCarSetup::ToeFront, ApexCarSetup::ToeRear);

	PageDefaults.Add(SetupSteppers.FindRef(ApexCarSetup::SpringFront));
	return ScrollPage(*WidgetTree, Page);
}

UWidget* UApexHotlapWidget::BuildEnginePage()
{
	UHorizontalBox* Page = WidgetTree->ConstructWidget<UHorizontalBox>();

	UVerticalBox* Left = WidgetTree->ConstructWidget<UVerticalBox>();
	AddSection(Left, TEXT("Engine"), { ApexCarSetup::RevLimiter, ApexCarSetup::EngineBraking, ApexCarSetup::TorqueMap, ApexCarSetup::Radiator });
	AddSection(Left, TEXT("Transmission"), { ApexCarSetup::FinalDrive, ApexCarSetup::GearSpread });
	AddSection(Left, TEXT("Fuel"), { ApexCarSetup::FuelLoad });
	AddH(Page, ScrollPage(*WidgetTree, Left), FMargin(), VAlign_Fill, 1.0f);

	UVerticalBox* Right = WidgetTree->ConstructWidget<UVerticalBox>();
	AddV(Right, Caption(*WidgetTree, TEXT("Top speed per gear · at redline")), FMargin(0.0f, 0.0f, 0.0f, 12.0f));
	UVerticalBox* Chart = WidgetTree->ConstructWidget<UVerticalBox>();
	GearBars = WidgetTree->ConstructWidget<UHorizontalBox>();
	AddV(Chart, MakeSized(*WidgetTree, GearBars, -1.0f, GearChartHeight));
	AddV(Chart, Rule(*WidgetTree), FMargin(0.0f, 0.0f, 0.0f, 8.0f));
	GearLabels = WidgetTree->ConstructWidget<UHorizontalBox>();
	AddV(Chart, GearLabels);
	AddV(Right, MakePanel(*WidgetTree, Chart, FMargin(20.0f, 20.0f, 20.0f, 14.0f), MakeBrush(FLinearColor::Transparent, Outline, 1.0f)));

	AddV(Right, Caption(*WidgetTree, TEXT("Fuel on board")), FMargin(0.0f, 22.0f, 0.0f, 12.0f));
	UHorizontalBox* Fuel = WidgetTree->ConstructWidget<UHorizontalBox>();
	auto AddFuelCell = [this, Fuel](const TCHAR* Label, TObjectPtr<UTextBlock>& OutValue)
	{
		UVerticalBox* Cell = WidgetTree->ConstructWidget<UVerticalBox>();
		AddV(Cell, Caption(*WidgetTree, Label));
		OutValue = MakeText(*WidgetTree, TEXT("--"), Font::Display(30.0f), Ink);
		AddV(Cell, OutValue, FMargin(0.0f, 4.0f, 0.0f, 0.0f));
		AddH(Fuel, MakePanel(*WidgetTree, Cell, FMargin(20.0f, 16.0f), MakeBrush(FLinearColor::Transparent, Outline, 1.0f)),
			FMargin(), VAlign_Fill, 1.0f);
	};
	AddFuelCell(TEXT("Range"), FuelRangeText);
	AddFuelCell(TEXT("Weight"), FuelWeightText);
	AddV(Right, Fuel);
	AddH(Page, MakeSized(*WidgetTree, Right, SidePanelWidth, -1.0f), FMargin(36.0f, 0.0f, 0.0f, 0.0f), VAlign_Top);

	PageDefaults.Add(SetupSteppers.FindRef(ApexCarSetup::RevLimiter));
	return Page;
}

UWidget* UApexHotlapWidget::BuildAeroPage()
{
	UVerticalBox* Page = WidgetTree->ConstructWidget<UVerticalBox>();
	AddPairHeader(Page);
	AddPairRow(Page, TEXT("Wing"), TEXT("Downforce at 200 km/h. The rear costs three times the drag."),
		ApexCarSetup::FrontWing, ApexCarSetup::RearWing);
	AddPairRow(Page, TEXT("Ride height"), TEXT("Lower: more downforce, until the floor touches. Rake moves the balance forward."),
		ApexCarSetup::RideHeightFront, ApexCarSetup::RideHeightRear);

	// What the knobs add up to, two cells a line.
	UHorizontalBox* Line = nullptr;
	int32 Cells = 0;
	auto AddTotal = [this, Page, &Line, &Cells](const TCHAR* Label, TObjectPtr<UTextBlock>& OutValue)
	{
		if (Cells % 2 == 0)
		{
			Line = WidgetTree->ConstructWidget<UHorizontalBox>();
			AddV(Page, Line, FMargin(0.0f, Cells == 0 ? 16.0f : 2.0f, 0.0f, 0.0f));
		}
		UHorizontalBox* Cell = WidgetTree->ConstructWidget<UHorizontalBox>();
		AddH(Cell, Caption(*WidgetTree, Label), FMargin(), VAlign_Center, 1.0f);
		OutValue = MakeText(*WidgetTree, TEXT("--"), Font::Display(26.0f), Ink);
		AddH(Cell, OutValue);
		AddH(Line, MakePanel(*WidgetTree, Cell, FMargin(20.0f, 14.0f), MakeBrush(FLinearColor::Transparent, Outline, 1.0f)),
			FMargin(Cells % 2 == 0 ? 0.0f : 2.0f, 0.0f, 0.0f, 0.0f), VAlign_Fill, 1.0f);
		++Cells;
	};
	AddTotal(TEXT("Wing downforce · 200 km/h"), DownforceText);
	AddTotal(TEXT("Aero balance · front"), BalanceText);
	AddTotal(TEXT("Rake · rear minus front"), RakeText);
	AddTotal(TEXT("Drag · wings, ducts, radiator"), DragText);

	PageDefaults.Add(SetupSteppers.FindRef(ApexCarSetup::FrontWing));
	return ScrollPage(*WidgetTree, Page);
}

UWidget* UApexHotlapWidget::BuildErsPage()
{
	UHorizontalBox* Page = WidgetTree->ConstructWidget<UHorizontalBox>();

	UVerticalBox* Left = WidgetTree->ConstructWidget<UVerticalBox>();
	AddSection(Left, TEXT("Hybrid"), { ApexCarSetup::ErsStartMode, ApexCarSetup::ErsRegen, ApexCarSetup::ErsDeployMap });
	AddH(Page, ScrollPage(*WidgetTree, Left), FMargin(), VAlign_Fill, 1.0f);

	// The system as filed: what the knobs work with, and the rules they cannot move.
	UVerticalBox* Right = WidgetTree->ConstructWidget<UVerticalBox>();
	AddV(Right, Caption(*WidgetTree, TEXT("The system · as filed")), FMargin(0.0f, 0.0f, 0.0f, 12.0f));
	auto AddCell = [this](UHorizontalBox* Line, const TCHAR* Label, TObjectPtr<UTextBlock>& OutValue, bool bFirst)
	{
		UVerticalBox* Cell = WidgetTree->ConstructWidget<UVerticalBox>();
		AddV(Cell, Caption(*WidgetTree, Label));
		OutValue = MakeText(*WidgetTree, TEXT("--"), Font::Display(30.0f), Ink);
		AddV(Cell, OutValue, FMargin(0.0f, 4.0f, 0.0f, 0.0f));
		AddH(Line, MakePanel(*WidgetTree, Cell, FMargin(20.0f, 16.0f), MakeBrush(FLinearColor::Transparent, Outline, 1.0f)),
			FMargin(bFirst ? 0.0f : 2.0f, 0.0f, 0.0f, 0.0f), VAlign_Fill, 1.0f);
	};
	UHorizontalBox* Top = WidgetTree->ConstructWidget<UHorizontalBox>();
	AddCell(Top, TEXT("Battery"), ErsBatteryText, true);
	AddCell(Top, TEXT("Motor"), ErsMotorText, false);
	AddV(Right, Top);
	UHorizontalBox* Bottom = WidgetTree->ConstructWidget<UHorizontalBox>();
	AddCell(Bottom, TEXT("Lap budget"), ErsBudgetText, true);
	AddCell(Bottom, TEXT("Deploys from"), ErsDeployFromText, false);
	AddV(Right, Bottom, FMargin(0.0f, 2.0f, 0.0f, 0.0f));

	AddV(Right, Caption(*WidgetTree, TEXT("Deploy map · Balanced mode")), FMargin(0.0f, 22.0f, 0.0f, 12.0f));
	UVerticalBox* Map = WidgetTree->ConstructWidget<UVerticalBox>();
	UHorizontalBox* MapLine = WidgetTree->ConstructWidget<UHorizontalBox>();
	AddH(MapLine, Caption(*WidgetTree, TEXT("Budget spent by half distance")), FMargin(), VAlign_Center, 1.0f);
	ErsHalfLapText = MakeText(*WidgetTree, TEXT("--"), Font::Display(26.0f), Ink);
	AddH(MapLine, ErsHalfLapText);
	AddV(Map, MapLine);
	ErsHalfLapNote = MakeText(*WidgetTree, TEXT(""), Font::Body(14.0f), InkMuted);
	ErsHalfLapNote->SetAutoWrapText(true);
	AddV(Map, ErsHalfLapNote, FMargin(0.0f, 6.0f, 0.0f, 0.0f));
	AddV(Right, MakePanel(*WidgetTree, Map, FMargin(20.0f, 14.0f), MakeBrush(FLinearColor::Transparent, Outline, 1.0f)));
	AddH(Page, MakeSized(*WidgetTree, Right, SidePanelWidth, -1.0f), FMargin(36.0f, 0.0f, 0.0f, 0.0f), VAlign_Top);

	PageDefaults.Add(SetupSteppers.FindRef(ApexCarSetup::ErsStartMode));
	return Page;
}

UWidget* UApexHotlapWidget::BuildSavePage()
{
	UHorizontalBox* Page = WidgetTree->ConstructWidget<UHorizontalBox>();

	UVerticalBox* Left = WidgetTree->ConstructWidget<UVerticalBox>();
	AddV(Left, Caption(*WidgetTree, TEXT("Save current setup")), FMargin(0.0f, 0.0f, 0.0f, 12.0f));
	UHorizontalBox* SaveLine = WidgetTree->ConstructWidget<UHorizontalBox>();
	SaveNameBox = MakeSearchBox(*WidgetTree, TEXT("Setup name"));
	{
		FEditableTextBoxStyle Style = SaveNameBox->GetWidgetStyle();
		Style.SetFont(Font::Body(19.0f));
		Style.SetPadding(FMargin(16.0f, 12.0f));
		SaveNameBox->SetWidgetStyle(Style);
	}
	AddH(SaveLine, MakeSized(*WidgetTree, SaveNameBox, -1.0f, 58.0f), FMargin(), VAlign_Center, 1.0f);
	SaveButton = MakeActionButton(TEXT("SAVE AS NEW"), FString(), ActionSave, EApexButtonVariant::Primary, 58.0f, 20.0f);
	AddH(SaveLine, MakeSized(*WidgetTree, SaveButton, 190.0f, -1.0f), FMargin(8.0f, 0.0f, 0.0f, 0.0f), VAlign_Center);
	AddV(Left, SaveLine);

	UHorizontalBox* ListHead = WidgetTree->ConstructWidget<UHorizontalBox>();
	AddH(ListHead, Caption(*WidgetTree, TEXT("Saved · this car")), FMargin(), VAlign_Center, 1.0f);
	SavedCountText = Caption(*WidgetTree, TEXT("0 setups"));
	AddH(ListHead, SavedCountText);
	AddV(Left, ListHead, FMargin(0.0f, 26.0f, 0.0f, 10.0f));

	UHorizontalBox* Columns = WidgetTree->ConstructWidget<UHorizontalBox>();
	AddH(Columns, Caption(*WidgetTree, TEXT("Name"), InkFaint), FMargin(), VAlign_Center, 1.0f);
	AddH(Columns, MakeSized(*WidgetTree, Caption(*WidgetTree, TEXT("Saved"), InkFaint), 100.0f, -1.0f));
	AddH(Columns, MakeSized(*WidgetTree, Caption(*WidgetTree, TEXT("Best"), InkFaint), 120.0f, -1.0f));
	UTextBlock* ChangesCaption = Caption(*WidgetTree, TEXT("Changes"), InkFaint);
	ChangesCaption->SetJustification(ETextJustify::Right);
	AddH(Columns, MakeSized(*WidgetTree, ChangesCaption, 100.0f, -1.0f));
	AddV(Left, Columns, FMargin(20.0f, 0.0f, 20.0f, 6.0f));

	SavedList = WidgetTree->ConstructWidget<UVerticalBox>();
	AddV(Left, ScrollPage(*WidgetTree, SavedList), FMargin(), HAlign_Fill, 1.0f);
	AddH(Page, Left, FMargin(), VAlign_Fill, 1.0f);

	// The selected setup: what it changes, and what can be done with it.
	UOverlay* DetailHost = WidgetTree->ConstructWidget<UOverlay>();
	UVerticalBox* Detail = WidgetTree->ConstructWidget<UVerticalBox>();
	UVerticalBox* DetailHead = WidgetTree->ConstructWidget<UVerticalBox>();
	DetailName = MakeText(*WidgetTree, FString(), Font::Display(28.0f), Ink);
	AddV(DetailHead, DetailName);
	DetailMeta = MakeText(*WidgetTree, FString(), Font::Mono(10.0f, 100), InkMuted);
	AddV(DetailHead, DetailMeta, FMargin(0.0f, 6.0f, 0.0f, 0.0f));
	AddV(Detail, MakePanel(*WidgetTree, DetailHead, FMargin(24.0f, 22.0f), MakeBrush(FLinearColor::Transparent)));
	AddV(Detail, Rule(*WidgetTree));
	AddV(Detail, Caption(*WidgetTree, TEXT("Differences from stock")), FMargin(24.0f, 16.0f, 24.0f, 6.0f));
	DetailDiff = WidgetTree->ConstructWidget<UVerticalBox>();
	AddV(Detail, MakePanel(*WidgetTree, ScrollPage(*WidgetTree, DetailDiff), FMargin(24.0f, 0.0f), MakeBrush(FLinearColor::Transparent)),
		FMargin(), HAlign_Fill, 1.0f);
	AddV(Detail, Rule(*WidgetTree));
	UVerticalBox* DetailActions = WidgetTree->ConstructWidget<UVerticalBox>();
	LoadButton = MakeActionButton(TEXT("LOAD SETUP"), FString(), ActionLoad, EApexButtonVariant::Primary, 60.0f, 22.0f);
	AddV(DetailActions, LoadButton);
	UHorizontalBox* Secondary = WidgetTree->ConstructWidget<UHorizontalBox>();
	OverwriteButton = MakeActionButton(TEXT("OVERWRITE"), FString(), ActionOverwrite, EApexButtonVariant::Ghost, 52.0f, 17.0f);
	AddH(Secondary, OverwriteButton, FMargin(), VAlign_Center, 1.0f);
	// Rename takes the name typed in the box above; with the box empty it
	// puts the setup's name there to edit first.
	RenameButton = MakeActionButton(TEXT("RENAME"), FString(), ActionRename, EApexButtonVariant::Ghost, 52.0f, 17.0f);
	AddH(Secondary, MakeSized(*WidgetTree, RenameButton, 124.0f, -1.0f), FMargin(8.0f, 0.0f, 0.0f, 0.0f), VAlign_Center);
	DeleteButton = MakeActionButton(TEXT("DELETE"), FString(), ActionDelete, EApexButtonVariant::Ghost, 52.0f, 17.0f);
	AddH(Secondary, MakeSized(*WidgetTree, DeleteButton, 124.0f, -1.0f), FMargin(8.0f, 0.0f, 0.0f, 0.0f), VAlign_Center);
	AddV(DetailActions, Secondary, FMargin(0.0f, 8.0f, 0.0f, 0.0f));
	AddV(Detail, MakePanel(*WidgetTree, DetailActions, FMargin(24.0f, 18.0f, 24.0f, 22.0f), MakeBrush(FLinearColor::Transparent)));
	DetailPanel = Detail;
	UOverlaySlot* DetailSlot = DetailHost->AddChildToOverlay(Detail);
	DetailSlot->SetHorizontalAlignment(HAlign_Fill);
	DetailSlot->SetVerticalAlignment(VAlign_Fill);

	UTextBlock* Empty = MakeText(*WidgetTree, TEXT("Select a setup to see its changes."), Font::Body(18.0f), InkMuted);
	DetailEmpty = Empty;
	UOverlaySlot* EmptySlot = DetailHost->AddChildToOverlay(Empty);
	EmptySlot->SetHorizontalAlignment(HAlign_Center);
	EmptySlot->SetVerticalAlignment(VAlign_Center);

	UBorder* DetailFrame = MakePanel(*WidgetTree, DetailHost, FMargin(), MakeBrush(FLinearColor::Transparent, Outline, 1.0f));
	AddH(Page, MakeSized(*WidgetTree, DetailFrame, SaveDetailWidth, -1.0f), FMargin(36.0f, 0.0f, 0.0f, 0.0f), VAlign_Fill);

	PageDefaults.Add(SaveButton);
	return Page;
}

UWidget* UApexHotlapWidget::BuildTimingPanel()
{
	UVerticalBox* Sheet = WidgetTree->ConstructWidget<UVerticalBox>();

	UHorizontalBox* Head = WidgetTree->ConstructWidget<UHorizontalBox>();
	SheetTitle = MakeLabel(*WidgetTree, TEXT("Hotlap"), Palette::Accent);
	AddH(Head, SheetTitle, FMargin(), VAlign_Center, 1.0f);
	SheetLiveLabel = MakeText(*WidgetTree, TEXT("OUT LAP"), Font::Mono(10.0f, 120), Palette::TextMuted);
	AddH(Head, SheetLiveLabel, FMargin(), VAlign_Center);
	AddV(Sheet, Head);

	SheetLiveText = MakeText(*WidgetTree, TEXT("--:--.---"), Font::Mono(30.0f, 20), Palette::TextPrimary);
	AddV(Sheet, SheetLiveText, FMargin(0.0f, 8.0f, 0.0f, 12.0f));
	AddV(Sheet, MakeDivider(*WidgetTree));

	SheetRows = WidgetTree->ConstructWidget<UVerticalBox>();
	AddV(Sheet, SheetRows, FMargin(0.0f, 10.0f, 0.0f, 10.0f));

	AddV(Sheet, MakeDivider(*WidgetTree));
	UHorizontalBox* Best = WidgetTree->ConstructWidget<UHorizontalBox>();
	AddH(Best, MakeText(*WidgetTree, TEXT("BEST"), Font::Mono(10.0f, 120), Palette::TextMuted), FMargin(), VAlign_Center, 1.0f);
	SheetBestText = MakeText(*WidgetTree, TEXT("--:--.---"), Font::Mono(14.0f), Palette::TextPrimary);
	AddH(Best, SheetBestText, FMargin(), VAlign_Center);
	AddV(Sheet, Best, FMargin(0.0f, 10.0f, 0.0f, 0.0f));
	UHorizontalBox* Record = WidgetTree->ConstructWidget<UHorizontalBox>();
	AddH(Record, MakeText(*WidgetTree, TEXT("RECORD"), Font::Mono(10.0f, 120), Palette::TextMuted), FMargin(), VAlign_Center, 1.0f);
	SheetRecordText = MakeText(*WidgetTree, TEXT("--:--.---"), Font::Mono(14.0f), Palette::TextSecondary);
	AddH(Record, SheetRecordText, FMargin(), VAlign_Center);
	AddV(Sheet, Record, FMargin(0.0f, 4.0f, 0.0f, 0.0f));

	UBorder* Panel = MakePanel(*WidgetTree, Sheet, FMargin(20.0f, 16.0f), MakeBrush(Palette::Surface.CopyWithNewOpacity(0.82f)));
	return MakeSized(*WidgetTree, Panel, SheetWidth, -1.0f);
}

UWidget* UApexHotlapWidget::BuildReplayStrip()
{
	UHorizontalBox* Strip = WidgetTree->ConstructWidget<UHorizontalBox>();
	AddH(Strip, MakePanel(*WidgetTree,
		MakeText(*WidgetTree, TEXT("REPLAY"), Font::Mono(10.0f, 120), Palette::OnAccent),
		FMargin(12.0f, 6.0f), MakeBrush(Palette::Accent)), FMargin(), VAlign_Center);
	ReplayText = MakeText(*WidgetTree, FString(), Font::Mono(12.0f, 60), Palette::TextSecondary);
	AddH(Strip, ReplayText, FMargin(18.0f, 0.0f, 0.0f, 0.0f), VAlign_Center);
	AddH(Strip, MakeText(*WidgetTree, TEXT("ESC  ·  STOP"), Font::Mono(11.0f, 100), Palette::TextMuted), FMargin(24.0f, 0.0f, 0.0f, 0.0f), VAlign_Center);
	return MakePanel(*WidgetTree, Strip, FMargin(14.0f, 8.0f), MakeBrush(Palette::Surface.CopyWithNewOpacity(0.82f)));
}

// --- View and tabs ------------------------------------------------------------

void UApexHotlapWidget::SetActive(bool bInActive)
{
	bActive = bInActive;
	Laps.Reset();
	BestMs = 0;
	LiveLap = 0;
	LiveLapTimeMs = 0;
	bLiveInvalid = false;
	if (!bActive)
	{
		SetView(EApexHotlapView::Hidden);
	}
	RefreshSheet();
	RefreshGhostRows();
	RefreshSetup();
}

void UApexHotlapWidget::SetView(EApexHotlapView InView)
{
	if (View == InView)
	{
		return;
	}
	View = InView;
	ApplyView();
	if (View == EApexHotlapView::Garage)
	{
		RefreshSetup();
		RefreshGhostRows();
		RefreshSubtitle();
	}
	RefreshSheet();
}

void UApexHotlapWidget::RefreshSubtitle()
{
	const UGameInstance* GameInstance = GetGameInstance();
	const UApexNetSubsystem* Net = GameInstance ? GameInstance->GetSubsystem<UApexNetSubsystem>() : nullptr;
	const UApexMenuFlowSubsystem* Flow = GameInstance ? GameInstance->GetSubsystem<UApexMenuFlowSubsystem>() : nullptr;
	TArray<FString> Parts;
	FApexSessionSummary Session;
	if (!bMenuMode && Net && Net->FindSessionById(Net->GetCurrentSessionId(), Session) && !Session.TrackName.IsEmpty())
	{
		Parts.Add(Session.TrackName);
	}
	FApexCarCatalogRow CarRow;
	if (Flow && Flow->GetCarCatalogRow(Flow->GetPendingCarId(), CarRow) && !CarRow.DisplayName.IsEmpty())
	{
		Parts.Add(CarRow.DisplayName);
	}
	if (GarageSubtitle)
	{
		GarageSubtitle->SetText(FText::FromString(FString::Join(Parts, TEXT("  ·  ")).ToUpper()));
	}
}

void UApexHotlapWidget::ApplyView()
{
	const bool bShown = View != EApexHotlapView::Hidden;
	SetVisibility(bShown
		? (View == EApexHotlapView::Garage ? ESlateVisibility::Visible : ESlateVisibility::HitTestInvisible)
		: ESlateVisibility::Collapsed);
	ApplyPage();
	if (TimingPanel)
	{
		// The garage covers the screen; the sheet is for the track.
		TimingPanel->SetVisibility(View == EApexHotlapView::Track ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
	}
	if (ReplayStrip)
	{
		ReplayStrip->SetVisibility(View == EApexHotlapView::Replay ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
	}
	if (TrackHint)
	{
		TrackHint->SetVisibility(View == EApexHotlapView::Track ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
	}
}

void UApexHotlapWidget::ApplyPage()
{
	const bool bGarageUp = View == EApexHotlapView::Garage;
	const bool bBoard = bGarageUp && bQualifying && bBoardOpen;
	if (Garage)
	{
		Garage->SetVisibility(bGarageUp && !bBoard ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
	}
	if (BoardLayer)
	{
		BoardLayer->SetVisibility(bBoard ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
	}
	if (GaragePageToggle)
	{
		GaragePageToggle->SetVisibility(bQualifying ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
	}
	for (UApexButtonWidget* Tune : { GarageTuneButton.Get(), BoardTuneButton.Get() })
	{
		if (Tune) { Tune->SetSelected(!bBoardOpen); }
	}
	for (UApexButtonWidget* Board : { GarageBoardButton.Get(), BoardBoardButton.Get() })
	{
		if (Board) { Board->SetSelected(bBoardOpen); }
	}
	if (!bBoard)
	{
		// The sheet is back, or the car is out: the view is the driver's again.
		StopWatching();
	}
	else
	{
		RefreshBoard();
	}
}

void UApexHotlapWidget::StopWatching()
{
	if (WatchedCar != INDEX_NONE)
	{
		WatchedCar = INDEX_NONE;
		OnWatchCar.Broadcast(INDEX_NONE);
	}
}

void UApexHotlapWidget::SetQualifying(bool bInQualifying)
{
	if (bQualifying == bInQualifying)
	{
		return;
	}
	bQualifying = bInQualifying;
	if (!bQualifying)
	{
		bBoardOpen = false;
	}
	ApplyQualifyingRows();
	ApplyPage();
}

void UApexHotlapWidget::ApplyQualifyingRows()
{
	// Qualifying: no record-lap replay or ghost, and the tyres always leave
	// cold (the outlap is the warm-up); the title and the note say so.
	const ESlateVisibility HotlapOnly = (bQualifying || bMenuMode) ? ESlateVisibility::Collapsed : ESlateVisibility::Visible;
	if (SaveReplayButton)
	{
		SaveReplayButton->SetVisibility(bMenuMode ? ESlateVisibility::Collapsed : ESlateVisibility::Visible);
	}
	for (UApexButtonWidget* Button : { ReplayButton.Get(), GhostButton.Get(), TyresButton.Get() })
	{
		if (Button)
		{
			Button->SetVisibility(HotlapOnly);
		}
	}
	if (SheetTitle)
	{
		SheetTitle->SetText(FText::FromString(bQualifying ? TEXT("QUALIFYING") : TEXT("HOTLAP")));
	}
	if (GarageCaption)
	{
		GarageCaption->SetText(FText::FromString(bMenuMode ? TEXT("SETUPS") : bQualifying ? TEXT("QUALIFYING") : TEXT("HOTLAP")));
	}
	if (GarageNote)
	{
		GarageNote->SetText(FText::FromString(bMenuMode
			? TEXT("Tune the pending car and save named setups here. The working setup goes out with you when you join any session. Units show in clicks until a session sends the car's figures.")
			: bQualifying
			? TEXT("The car leaves the pit exit on cold tyres and drives a whole outlap: the first lap over the line is the warm-up, the next one is timed. Tune here, or watch the scoreboard.")
			: TEXT("The car spawns on the run-up before the line, so the first lap is a flying one. Fuel and tyres are put in here, in the garage.")));
	}
	if (GoOutButton)
	{
		GoOutButton->SetLabel(bMenuMode ? TEXT("DONE")
			: bQualifying ? TEXT("GO OUT FOR A LAP") : TEXT("GO OUT ON TRACK"));
	}
}

void UApexHotlapWidget::SetMenuMode(bool bInMenuMode)
{
	if (bMenuMode == bInMenuMode)
	{
		return;
	}
	bMenuMode = bInMenuMode;
	// Rebuilt from the car's own figures (or clicks) whichever way it went.
	CompoundCardNames.Reset();
	ApplyQualifyingRows();
	if (View == EApexHotlapView::Garage)
	{
		RefreshSetup();
		RefreshSubtitle();
	}
}

bool UApexHotlapWidget::HandleBack()
{
	if (bMenuMode && View == EApexHotlapView::Garage)
	{
		OnAction.Broadcast(EApexHotlapAction::CloseEditor);
		return true;
	}
	return false;
}

void UApexHotlapWidget::SetBoardOpen(bool bOpen)
{
	bOpen = bOpen && bQualifying;
	if (bBoardOpen == bOpen)
	{
		return;
	}
	bBoardOpen = bOpen;
	ApplyPage();
	// Focus follows the page: the first driver, or the way out.
	if (bBoardOpen)
	{
		if (BoardRowButtons.IsValidIndex(0))
		{
			ApexNav::Focus(BoardRowButtons[0]);
		}
	}
	else
	{
		FocusDefault();
	}
}

void UApexHotlapWidget::WatchRow(int32 Row)
{
	if (bBoardOpen && BoardCars.IsValidIndex(Row) && BoardCars[Row] != INDEX_NONE)
	{
		WatchedCar = BoardCars[Row];
		OnWatchCar.Broadcast(WatchedCar);
		RefreshBoard();
	}
}

void UApexHotlapWidget::SetWatchCameraLabel(const FString& Label)
{
	WatchCameraLabel = Label;
	if (BoardCameraButton)
	{
		BoardCameraButton->SetBadge(Label, Palette::TextPrimary);
	}
}

void UApexHotlapWidget::RefreshBoard()
{
	const UGameInstance* GameInstance = GetGameInstance();
	const UApexNetSubsystem* Net = GameInstance ? GameInstance->GetSubsystem<UApexNetSubsystem>() : nullptr;
	if (!Net)
	{
		return;
	}
	const TArray<ApexBoard::FRow> Rows = ApexBoard::Build(
		Net->GetSessionRoster().Entries, Net->GetLatestTelemetry().Cars, Net->GetLocalCarIndex());
	FString WatchedName;
	for (int32 Index = 0; Index < BoardRowButtons.Num(); ++Index)
	{
		const bool bUsed = Rows.IsValidIndex(Index);
		BoardRowRoots[Index]->SetVisibility(bUsed ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
		BoardCars[Index] = bUsed ? Rows[Index].CarIndex : INDEX_NONE;
		if (!bUsed)
		{
			continue;
		}
		const ApexBoard::FRow& Row = Rows[Index];
		BoardPos[Index]->SetText(FText::FromString(Row.Position > 0 ? FString::FromInt(Row.Position) : FString(TEXT("-"))));
		BoardName[Index]->SetText(FText::FromString(Row.bYou ? Row.Name + TEXT("  (YOU)") : Row.Name));
		BoardName[Index]->SetColorAndOpacity(Row.bYou ? Palette::Accent : Ink);
		BoardBest[Index]->SetText(FText::FromString(FormatMs(Row.BestMs)));
		BoardBest[Index]->SetColorAndOpacity(Row.Position == 1 ? Purple : Ink);
		BoardGap[Index]->SetText(FText::FromString(Row.Position > 1 ? FormatDelta(Row.GapMs, 0) : FString()));
		BoardState[Index]->SetText(FText::FromString(Row.bInGarage ? TEXT("GARAGE") : TEXT("ON TRACK")));
		BoardRowButtons[Index]->SetSelected(Row.CarIndex == WatchedCar);
		if (Row.CarIndex == WatchedCar)
		{
			WatchedName = Row.Name;
		}
	}
	if (BoardHint)
	{
		BoardHint->SetText(FText::FromString(WatchedCar == INDEX_NONE
			? FString(TEXT("CLICK A DRIVER TO WATCH THEIR CAR"))
			: FString::Printf(TEXT("WATCHING %s · CLICK AGAIN TO STOP"), *WatchedName.ToUpper())));
	}
}

void UApexHotlapWidget::SetTab(EApexGarageTab InTab)
{
	if (InTab == EApexGarageTab::Count || InTab == Tab || !IsTabShown(InTab))
	{
		return;
	}
	Tab = InTab;
	ApplyTab();
	if (Tab == EApexGarageTab::Save)
	{
		RefreshSaved();
	}
}

bool UApexHotlapWidget::IsTabShown(EApexGarageTab InTab) const
{
	if (InTab == EApexGarageTab::Ers)
	{
		const FApexCarSetupSheet* Sheet = GetSheet();
		return !Sheet || Sheet->HasHybrid();
	}
	return InTab != EApexGarageTab::Count;
}

void UApexHotlapWidget::ApplyTabVisibility()
{
	int32 Shown = 0;
	for (int32 Index = 0; Index < TabColumns.Num(); ++Index)
	{
		const bool bShown = IsTabShown(static_cast<EApexGarageTab>(Index));
		if (TabColumns[Index])
		{
			TabColumns[Index]->SetVisibility(bShown ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);
		}
		if (bShown && TabNumbers.IsValidIndex(Index) && TabNumbers[Index])
		{
			TabNumbers[Index]->SetText(FText::AsNumber(++Shown));
		}
	}
	if (!IsTabShown(Tab))
	{
		Tab = EApexGarageTab::Tyres;
		ApplyTab();
	}
}

void UApexHotlapWidget::ApplyTab()
{
	const int32 Active = static_cast<int32>(Tab);
	if (Pages)
	{
		Pages->SetActiveWidgetIndex(Active);
	}
	for (int32 Index = 0; Index < TabButtons.Num(); ++Index)
	{
		const bool bOn = Index == Active;
		if (TabNumbers.IsValidIndex(Index) && TabNumbers[Index])
		{
			TabNumbers[Index]->SetColorAndOpacity(bOn ? Palette::Accent : InkMuted);
		}
		if (TabLabels.IsValidIndex(Index) && TabLabels[Index])
		{
			TabLabels[Index]->SetColorAndOpacity(bOn ? Ink : InkMuted);
		}
		if (TabUnderlines.IsValidIndex(Index) && TabUnderlines[Index])
		{
			TabUnderlines[Index]->SetBrushColor(bOn ? Palette::Accent : FLinearColor::Transparent);
		}
	}
}

void UApexHotlapWidget::FocusDefault()
{
	if (View == EApexHotlapView::Garage && bQualifying && bBoardOpen && BoardRowButtons.IsValidIndex(0))
	{
		ApexNav::Focus(BoardRowButtons[0]);
		return;
	}
	if (View != EApexHotlapView::Garage || !(GoOutButton && ApexNav::Focus(GoOutButton)))
	{
		SetKeyboardFocus();
	}
}

bool UApexHotlapWidget::HandleNavigation(EUINavigation Direction, UWidget* Source)
{
	if (View == EApexHotlapView::Garage && !bBoardOpen && ApexNav::IsSequential(Direction))
	{
		const int32 Count = static_cast<int32>(EApexGarageTab::Count);
		const int32 Step = Direction == EUINavigation::Next ? 1 : -1;
		int32 Next = static_cast<int32>(Tab);
		do
		{
			Next = (Next + Step + Count) % Count;
		}
		while (!IsTabShown(static_cast<EApexGarageTab>(Next)) && Next != static_cast<int32>(Tab));
		SetTab(static_cast<EApexGarageTab>(Next));
		ApexUiAudio::Play(this, EApexUiSound::Adjust);
		if (PageDefaults.IsValidIndex(static_cast<int32>(Tab)))
		{
			// A page opens on a stepper, which holds focus in its pills, not itself.
			UWidget* Default = PageDefaults[static_cast<int32>(Tab)];
			if (!ApexNav::Focus(Default))
			{
				ApexNav::Focus(ApexNav::FindFirstFocusable(Cast<UUserWidget>(Default)));
			}
		}
		return true;
	}
	// The setup pages are rows of steppers in a scroll box: Slate's own search
	// stops at the pills' row and the box's edge, so a pad could not leave
	// the first stepper it reached.
	if (View == EApexHotlapView::Garage && ApexNav::MoveToward(this, Direction, Source))
	{
		return true;
	}
	return Super::HandleNavigation(Direction, Source);
}

FReply UApexHotlapWidget::NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent)
{
	const FKey Key = InKeyEvent.GetKey();
	const bool bTyping = SaveNameBox && SaveNameBox->HasKeyboardFocus();
	if (View == EApexHotlapView::Garage && !bBoardOpen && !bTyping && (Key == EKeys::Q || Key == EKeys::E))
	{
		ApexNav::FNavigationScope Scope;
		HandleNavigation(Key == EKeys::E ? EUINavigation::Next : EUINavigation::Previous, nullptr);
		return FReply::Handled();
	}
	return Super::NativeOnKeyDown(InGeometry, InKeyEvent);
}

// --- Timing sheet -------------------------------------------------------------

void UApexHotlapWidget::SetLiveLap(int32 Lap, int32 LapTimeMs, bool bInvalid, int32 BestLapTimeMs)
{
	LiveLap = Lap;
	LiveLapTimeMs = LapTimeMs;
	bLiveInvalid = bInvalid;
	if (BestLapTimeMs > 0 && (BestMs == 0 || BestLapTimeMs < BestMs))
	{
		BestMs = BestLapTimeMs;
	}
	if (!SheetLiveText || !SheetLiveLabel)
	{
		return;
	}
	if (Lap <= 0)
	{
		SheetLiveLabel->SetText(FText::FromString(TEXT("OUT LAP")));
		SheetLiveLabel->SetColorAndOpacity(Palette::TextSecondary);
		SheetLiveText->SetText(FText::FromString(TEXT("--:--.---")));
		SheetLiveText->SetColorAndOpacity(Palette::TextSecondary);
	}
	else
	{
		SheetLiveLabel->SetText(FText::FromString(bInvalid ? TEXT("LAP INVALID") : FString::Printf(TEXT("LAP %d"), Lap)));
		SheetLiveLabel->SetColorAndOpacity(bInvalid ? Palette::Error : Palette::Live);
		SheetLiveText->SetText(FText::FromString(FormatMs(LapTimeMs)));
		SheetLiveText->SetColorAndOpacity(bInvalid ? Palette::TextMuted : Palette::TextPrimary);
	}
}

void UApexHotlapWidget::SetReplayTime(float ReplayMs, int32 LapTimeMs)
{
	if (ReplayText)
	{
		ReplayText->SetText(FText::FromString(FString::Printf(TEXT("BEST LAP  %s  /  %s"),
			*FormatMs(FMath::Max(0, FMath::RoundToInt(ReplayMs))), *FormatMs(LapTimeMs))));
	}
}

void UApexHotlapWidget::HandleLapTiming(const FApexLapTiming& Timing)
{
	const UGameInstance* GameInstance = GetGameInstance();
	const UApexNetSubsystem* Net = GameInstance ? GameInstance->GetSubsystem<UApexNetSubsystem>() : nullptr;
	if (!bActive || !Net || !Timing.bIsLapEnd || Timing.CarIndex != Net->GetLocalCarIndex())
	{
		return;
	}
	FLapEntry Entry;
	Entry.Lap = Timing.Lap;
	Entry.TimeMs = Timing.LapTimeMs;
	Entry.bValid = Timing.bValid;
	Entry.bPersonalBest = Timing.bPersonalBestLap;
	Entry.bSessionBest = Timing.bSessionBestLap;
	Laps.Add(Entry);
	if (Entry.bValid && Entry.TimeMs > 0 && (BestMs == 0 || Entry.TimeMs < BestMs))
	{
		BestMs = Entry.TimeMs;
	}
	if (Entry.bValid && Entry.TimeMs > 0)
	{
		if (UApexSettingsSubsystem* Settings = GetSettings())
		{
			Settings->RecordSetupLap(GetCarId(), Entry.TimeMs);
		}
	}
	RefreshSheet();
}

void UApexHotlapWidget::RefreshSheet()
{
	if (!SheetRows)
	{
		return;
	}
	SheetRows->ClearChildren();
	const int32 First = FMath::Max(0, Laps.Num() - SheetRowCount);
	if (Laps.Num() == 0)
	{
		AddV(SheetRows, MakeText(*WidgetTree, TEXT("No laps yet"), Font::Body(12.0f), Palette::TextMuted));
	}
	for (int32 Index = First; Index < Laps.Num(); ++Index)
	{
		const FLapEntry& Entry = Laps[Index];
		UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();
		const FLinearColor Colour = !Entry.bValid ? Palette::TextDisabled
			: Entry.bSessionBest ? Purple
			: Entry.bPersonalBest ? Palette::Live
			: Palette::TextPrimary;
		AddH(Row, MakeText(*WidgetTree, FString::Printf(TEXT("L%d"), Entry.Lap), Font::Mono(12.0f), Palette::TextMuted),
			FMargin(), VAlign_Center);
		AddH(Row, MakeText(*WidgetTree, FormatMs(Entry.TimeMs), Font::Mono(14.0f), Colour),
			FMargin(14.0f, 0.0f, 0.0f, 0.0f), VAlign_Center, 1.0f);
		FString Right;
		FLinearColor RightColour = Palette::TextMuted;
		if (!Entry.bValid)
		{
			Right = TEXT("INVALID");
			RightColour = Palette::Error;
		}
		else if (BestMs > 0 && Entry.TimeMs != BestMs)
		{
			Right = FormatDelta(Entry.TimeMs, BestMs);
		}
		else if (BestMs > 0)
		{
			Right = TEXT("BEST");
			RightColour = Colour;
		}
		AddH(Row, MakeText(*WidgetTree, Right, Font::Mono(11.0f, 40), RightColour), FMargin(), VAlign_Center);
		AddV(SheetRows, Row, FMargin(0.0f, Index == First ? 0.0f : 5.0f, 0.0f, 0.0f));
	}
	if (SheetBestText)
	{
		SheetBestText->SetText(FText::FromString(FormatMs(BestMs)));
	}
	if (SheetRecordText)
	{
		const UGameInstance* GameInstance = GetGameInstance();
		const UApexNetSubsystem* Net = GameInstance ? GameInstance->GetSubsystem<UApexNetSubsystem>() : nullptr;
		SheetRecordText->SetText(FText::FromString(FormatMs(Net ? Net->GetLapRecord().LapTimeMs : 0)));
	}
	SetLiveLap(LiveLap, LiveLapTimeMs, bLiveInvalid, 0);
}

// --- Setup --------------------------------------------------------------------

void UApexHotlapWidget::RefreshGhostRows()
{
	const UGameInstance* GameInstance = GetGameInstance();
	const UApexNetSubsystem* Net = GameInstance ? GameInstance->GetSubsystem<UApexNetSubsystem>() : nullptr;
	const UApexSettingsSubsystem* Settings = GetSettings();
	const bool bHasGhost = Net && Net->GetGhostLap().IsValid();
	if (ReplayButton)
	{
		// The button stays; without a lap it says so rather than vanishing,
		// which is what tells a new driver a replay exists at all.
		ReplayButton->SetBadge(bHasGhost
			? FormatMs(Net->GetGhostLap().LapTimeMs)
			: (Net && Net->GetLapRecord().bHasGhost ? TEXT("Loading") : TEXT("No lap yet")),
			bHasGhost ? Palette::TextPrimary : Palette::TextMuted);
	}
	if (SaveReplayButton)
	{
		const UApexReplayRecorder* Recorder = GameInstance ? GameInstance->GetSubsystem<UApexReplayRecorder>() : nullptr;
		const bool bSomething = Recorder && Recorder->HasSomethingToSave();
		const int32 Seconds = Recorder ? FMath::FloorToInt(Recorder->GetRecordedSeconds()) : 0;
		// Short: the column is narrow, and a longer badge ran into the label.
		SaveReplayButton->SetBadge(bSomething ? FString::Printf(TEXT("%d:%02d"), Seconds / 60, Seconds % 60) : TEXT("None"),
			bSomething ? Palette::TextPrimary : Palette::TextMuted);
	}
	if (GhostButton)
	{
		const bool bOn = Settings && Settings->Get() ? Settings->Get()->bGhostCar : true;
		GhostButton->SetBadge(bHasGhost ? (bOn ? TEXT("On") : TEXT("Off")) : TEXT("No lap yet"),
			bOn && bHasGhost ? Palette::Live : Palette::TextMuted);
	}
	if (TyresButton)
	{
		const bool bCold = Settings && Settings->Get() ? Settings->Get()->bHotlapColdTyres : false;
		TyresButton->SetBadge(bCold ? TEXT("Cold") : TEXT("Warm"), bCold ? Palette::TextPrimary : Palette::Live);
	}
}

void UApexHotlapWidget::RefreshSetup()
{
	const FApexCarSetup* Setup = GetWorkingSetup();
	const UApexSettingsSubsystem* Settings = GetSettings();
	if (!Setup || !Settings)
	{
		return;
	}
	bRefreshing = true;
	for (const TPair<int32, TObjectPtr<UApexStepperWidget>>& Entry : SetupSteppers)
	{
		if (Entry.Value)
		{
			Entry.Value->SetValue(Setup->GetClick(Entry.Key));
			// The value may be the same click with a new sheet under it.
			Entry.Value->RefreshReadout();
		}
	}
	bRefreshing = false;

	const int32 Changed = Setup->CountChanged();
	if (ResetButton)
	{
		ResetButton->SetBadge(Changed == 0 ? TEXT("Stock") : FString::Printf(TEXT("%d changed"), Changed),
			Changed == 0 ? Palette::TextMuted : Palette::Accent);
	}
	if (SetupChangesText)
	{
		SetupChangesText->SetText(FText::FromString(Changed == 0 ? TEXT("STOCK")
			: FString::Printf(TEXT("%d CHANGE%s FROM STOCK"), Changed, Changed == 1 ? TEXT("") : TEXT("S"))));
	}
	if (SetupNameText)
	{
		FString Name = Changed == 0 ? TEXT("Stock") : TEXT("Custom");
		const FGuid Loaded = Settings->GetLoadedSetupId();
		for (const FApexSavedSetup& Saved : Settings->GetSavedSetups(GetCarId()))
		{
			if (Saved.Id == Loaded)
			{
				Name = Saved.Name;
			}
		}
		SetupNameText->SetText(FText::FromString(Name));
	}
	if (PressureNote)
	{
		const FApexCarSetupSheet* Sheet = GetSheet();
		PressureNote->SetText(FText::FromString(Sheet && Sheet->TyreOptimalPsi > 0.0f
			? FString::Printf(TEXT("Grip falls off either side of the %.1f psi hot target."), Sheet->TyreOptimalPsi)
			: FString(TEXT("Grip falls off either side of the hot target."))));
	}
	if (CamberRow)
	{
		// Without camber in its car.toml a car's camber changes nothing: no row.
		const FApexCarSetupSheet* Sheet = GetSheet();
		CamberRow->SetVisibility(!Sheet || Sheet->bCamberModelled ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);
	}
	ApplyTabVisibility();
	RefreshCompounds();
	RefreshDerived();
	RefreshSaved();
}

void UApexHotlapWidget::RefreshCompounds()
{
	const FApexCarSetup* Setup = GetWorkingSetup();
	const int32 Clicks = Setup ? Setup->GetClick(ApexCarSetup::TyreCompound) : 0;
	for (int32 Index = 0; Index < CompoundButtons.Num(); ++Index)
	{
		if (CompoundButtons[Index])
		{
			CompoundButtons[Index]->SetSelected(CompoundCards.IsValidIndex(Index) && CompoundCards[Index].Clicks == Clicks);
		}
	}
}

void UApexHotlapWidget::RefreshDerived()
{
	const FApexCarSetup* Setup = GetWorkingSetup();
	const FApexCarSetupSheet* Sheet = GetSheet();
	if (!Setup)
	{
		return;
	}
	auto Value = [Sheet, Setup](int32 Knob) { return SheetValue(Sheet->Knobs[Knob], Setup->GetClick(Knob)); };

	// Downforce, balance, rake and drag.
	if (DownforceText && BalanceText && RakeText && DragText)
	{
		if (Sheet)
		{
			const float Rake = Value(ApexCarSetup::RideHeightRear) - Value(ApexCarSetup::RideHeightFront);
			const float StockRake = SheetValue(Sheet->Knobs[ApexCarSetup::RideHeightRear], 0) - SheetValue(Sheet->Knobs[ApexCarSetup::RideHeightFront], 0);
			const float Front = Value(ApexCarSetup::FrontWing);
			const float Rear = Value(ApexCarSetup::RearWing);
			const float Share = Front + Rear > 0.0f
				? Front / (Front + Rear) + Sheet->RakeBalancePerMm * (Rake - StockRake)
				: 0.0f;
			DownforceText->SetText(FText::FromString(Front + Rear > 0.0f ? FString::Printf(TEXT("%.0f kg"), Front + Rear) : FString(TEXT("--"))));
			RakeText->SetText(FText::FromString(FString::Printf(TEXT("%.0f mm"), Rake)));
			BalanceText->SetText(FText::FromString(Front + Rear > 0.0f ? FString::Printf(TEXT("%.1f %%"), Share * 100.0f) : FString(TEXT("--"))));
		}
		else
		{
			DownforceText->SetText(FText::FromString(TEXT("--")));
			RakeText->SetText(FText::FromString(TEXT("--")));
			BalanceText->SetText(FText::FromString(TEXT("--")));
		}
		// Drag needs no sheet: it is the clicks alone, against the car's own.
		float Drag = 1.0f;
		for (const TPair<int32, float>& Knob : DragPerClick)
		{
			Drag *= 1.0f + Setup->GetClick(Knob.Key) * Knob.Value;
		}
		const float DragPct = (Drag - 1.0f) * 100.0f;
		DragText->SetText(FText::FromString(FMath::Abs(DragPct) < 0.05f
			? FString(TEXT("Stock")) : FString::Printf(TEXT("%+.1f %%"), DragPct)));
	}

	// The hybrid as filed, and where the deploy map has Balanced mode
	// spend the lap budget: the server's `spend_target`, at half distance.
	if (ErsBatteryText && ErsMotorText && ErsBudgetText && ErsDeployFromText && ErsHalfLapText && ErsHalfLapNote)
	{
		const bool bHybrid = Sheet && Sheet->HasHybrid();
		ErsBatteryText->SetText(FText::FromString(bHybrid ? FString::Printf(TEXT("%.1f kWh"), Sheet->HybridBatteryKwh) : FString(TEXT("--"))));
		ErsMotorText->SetText(FText::FromString(bHybrid ? FString::Printf(TEXT("%.0f kW"), Sheet->HybridMotorKw) : FString(TEXT("--"))));
		ErsBudgetText->SetText(FText::FromString(!bHybrid ? FString(TEXT("--"))
			: Sheet->HybridLapBudgetKj > 0.0f ? FString::Printf(TEXT("%.1f MJ"), Sheet->HybridLapBudgetKj / 1000.0f) : FString(TEXT("None"))));
		ErsDeployFromText->SetText(FText::FromString(!bHybrid ? FString(TEXT("--"))
			: Sheet->HybridDeployMinKph > 0.0f ? FString::Printf(TEXT("%.0f km/h"), Sheet->HybridDeployMinKph) : FString(TEXT("Any speed"))));
		const float Early = Setup->GetClick(ApexCarSetup::ErsDeployMap) * DeployMapPerClick;
		const float HalfLap = Early == 0.0f ? 0.5f : FMath::Pow(0.5f, FMath::Max(1.0f - Early, 0.2f));
		ErsHalfLapText->SetText(FText::FromString(FString::Printf(TEXT("%.0f %%"), HalfLap * 100.0f)));
		ErsHalfLapNote->SetText(FText::FromString(bHybrid && Sheet->HybridLapBudgetKj <= 0.0f
			? FString(TEXT("This car has no lap budget: Balanced deploys whenever there is charge, so the map changes nothing."))
			: FString(TEXT("Early spends the budget on the first straights; late saves it for the end of the lap."))));
	}

	// Top speed per gear: the gearing knobs scale the file's ratios as the server's
	// `CarSetup::apply` does, the final drive every gear, the spread the top fully
	// and first not at all.
	if (GearBars && GearLabels)
	{
		GearBars->ClearChildren();
		GearLabels->ClearChildren();
		const int32 Gears = Sheet ? Sheet->GearRatios.Num() : 0;
		if (Gears > 0 && Sheet->WheelRadiusM > 0.0f)
		{
			const float Redline = Value(ApexCarSetup::RevLimiter);
			const float FinalDrive = Value(ApexCarSetup::FinalDrive);
			const float StockTop = SheetValue(Sheet->Knobs[ApexCarSetup::GearSpread], 0);
			const float TopScale = StockTop > 0.0f ? Value(ApexCarSetup::GearSpread) / StockTop : 1.0f;
			TArray<float> Speeds;
			float Fastest = 1.0f;
			for (int32 Gear = 0; Gear < Gears; ++Gear)
			{
				const float T = Gears > 1 ? static_cast<float>(Gear) / (Gears - 1) : 1.0f;
				const float Ratio = Sheet->GearRatios[Gear] * (1.0f + (TopScale - 1.0f) * T);
				const float Kmh = Ratio > 0.0f && FinalDrive > 0.0f
					? Redline / 60.0f * UE_TWO_PI * Sheet->WheelRadiusM / (Ratio * FinalDrive) * 3.6f
					: 0.0f;
				Speeds.Add(Kmh);
				Fastest = FMath::Max(Fastest, Kmh);
			}
			// The scale is fixed a little over the fastest gear, so a longer top gear visibly grows.
			const float Scale = FMath::CeilToFloat(Fastest * 1.08f / 20.0f) * 20.0f;
			const float BarArea = GearChartHeight - 26.0f;
			for (int32 Gear = 0; Gear < Gears; ++Gear)
			{
				UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>();
				AddV(Column, WidgetTree->ConstructWidget<UVerticalBox>(), FMargin(), HAlign_Fill, 1.0f);
				UTextBlock* Kmh = MakeText(*WidgetTree, FString::Printf(TEXT("%.0f"), Speeds[Gear]), Font::Mono(11.0f), Ink.CopyWithNewOpacity(0.8f));
				AddV(Column, Kmh, FMargin(0.0f, 0.0f, 0.0f, 6.0f), HAlign_Center);
				UBorder* Bar = MakePanel(*WidgetTree, nullptr, FMargin(), MakeBrush(Palette::Accent.CopyWithNewOpacity(0.85f)));
				AddV(Column, MakeSized(*WidgetTree, Bar, -1.0f, FMath::Max(2.0f, BarArea * Speeds[Gear] / Scale)));
				AddH(GearBars, Column, FMargin(Gear == 0 ? 0.0f : 5.0f, 0.0f, Gear == Gears - 1 ? 0.0f : 5.0f, 0.0f), VAlign_Fill, 1.0f);

				UTextBlock* Label = MakeText(*WidgetTree, FString::FromInt(Gear + 1), Font::Display(17.0f), InkMuted);
				Label->SetJustification(ETextJustify::Center);
				AddH(GearLabels, Label, FMargin(), VAlign_Center, 1.0f);
			}
		}
		else
		{
			AddH(GearLabels, MakeText(*WidgetTree, TEXT("Waiting for the car's figures from the server."), Font::Body(14.0f), InkMuted));
		}
	}

	// Fuel: what the tank holds for the run, in laps of this track and in kilos.
	if (FuelRangeText && FuelWeightText)
	{
		if (Sheet && Sheet->LapFuelL > 0.0f)
		{
			const float Litres = Value(ApexCarSetup::FuelLoad);
			FuelRangeText->SetText(FText::FromString(FString::Printf(TEXT("%.1f laps"), Litres / Sheet->LapFuelL)));
			FuelWeightText->SetText(FText::FromString(FString::Printf(TEXT("%.0f kg"), Litres * Sheet->FuelKgPerL)));
		}
		else
		{
			const float FillLaps = Sheet ? Value(ApexCarSetup::FuelLoad) : 3.0f + Setup->GetClick(ApexCarSetup::FuelLoad);
			FuelRangeText->SetText(FText::FromString(FString::Printf(TEXT("%.0f laps"), FMath::Max(1.0f, FillLaps))));
			FuelWeightText->SetText(FText::FromString(TEXT("--")));
		}
	}
}

void UApexHotlapWidget::RefreshSaved()
{
	const UApexSettingsSubsystem* Settings = GetSettings();
	if (!Settings || !SavedList)
	{
		return;
	}
	const TArray<FApexSavedSetup> Saved = Settings->GetSavedSetups(GetCarId());

	if (!Saved.ContainsByPredicate([this](const FApexSavedSetup& S) { return S.Id == SelectedSaved; }))
	{
		// Nothing picked yet: the loaded one, if it is this car's.
		SelectedSaved = Settings->GetLoadedSetupId();
		if (!Saved.ContainsByPredicate([this](const FApexSavedSetup& S) { return S.Id == SelectedSaved; }))
		{
			SelectedSaved.Invalidate();
		}
	}

	// Rebuild the rows. Focus on a row being rebuilt would be lost: remember which.
	int32 FocusedRow = INDEX_NONE;
	for (int32 Index = 0; Index < SavedRowButtons.Num(); ++Index)
	{
		if (SavedRowButtons[Index] && SavedRowButtons[Index]->HasKeyboardFocus())
		{
			FocusedRow = Index;
		}
	}
	SavedList->ClearChildren();
	SavedRowButtons.Reset();
	SavedRowIds.Reset();
	for (const FApexSavedSetup& Setup : Saved)
	{
		FApexButtonSpec Spec;
		Spec.Variant = EApexButtonVariant::Panel;
		Spec.ActionId = ActionPickSaved;
		Spec.Height = 62.0f;
		UApexButtonWidget* Button = WidgetTree->ConstructWidget<UApexButtonWidget>();
		Button->Setup(Spec);
		Button->OnActivated.AddDynamic(this, &UApexHotlapWidget::HandleButtonActivated);
		Button->SetSelected(Setup.Id == SelectedSaved);
		SavedRowButtons.Add(Button);
		SavedRowIds.Add(Setup.Id);

		UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();
		UHorizontalBox* NameCell = WidgetTree->ConstructWidget<UHorizontalBox>();
		AddH(NameCell, MakeText(*WidgetTree, Setup.Name, Font::Body(19.0f, true), Ink));
		UTextBlock* Tag = MakeText(*WidgetTree, CompoundLabel(Setup.Setup.GetClick(ApexCarSetup::TyreCompound)), Font::Mono(9.0f, 120), InkMuted);
		AddH(NameCell, MakePanel(*WidgetTree, Tag, FMargin(6.0f, 2.0f), MakeBrush(FLinearColor::Transparent, Ink.CopyWithNewOpacity(0.16f), 1.0f)),
			FMargin(12.0f, 0.0f, 0.0f, 0.0f));
		AddH(Row, NameCell, FMargin(), VAlign_Center, 1.0f);
		AddH(Row, MakeSized(*WidgetTree, MakeText(*WidgetTree, FormatDay(Setup.SavedAt), Font::Mono(12.0f), Ink.CopyWithNewOpacity(0.75f)), 100.0f, -1.0f));
		AddH(Row, MakeSized(*WidgetTree, MakeText(*WidgetTree, Setup.BestLapMs > 0 ? FormatMs(Setup.BestLapMs) : FString(TEXT("—")),
			Font::Mono(12.0f), Ink.CopyWithNewOpacity(0.75f)), 120.0f, -1.0f));
		UTextBlock* Count = MakeText(*WidgetTree, FString::FromInt(Setup.Setup.CountChanged()), Font::Mono(12.0f), Ink.CopyWithNewOpacity(0.75f));
		Count->SetJustification(ETextJustify::Right);
		AddH(Row, MakeSized(*WidgetTree, Count, 100.0f, -1.0f));
		AddV(SavedList, ButtonWithContent(*WidgetTree, Button, Row, FMargin(20.0f, 0.0f)), FMargin(0.0f, 0.0f, 0.0f, 2.0f));
	}
	if (Saved.Num() == 0)
	{
		AddV(SavedList, MakeText(*WidgetTree, TEXT("No saved setups for this car yet. Name the current one above to keep it."),
			Font::Body(16.0f), InkMuted), FMargin(20.0f, 14.0f));
	}
	if (SavedRowButtons.Num() > 0 && FocusedRow != INDEX_NONE)
	{
		ApexNav::Focus(SavedRowButtons[FMath::Min(FocusedRow, SavedRowButtons.Num() - 1)]);
	}
	if (SavedCountText)
	{
		SavedCountText->SetText(FText::FromString(FString::Printf(TEXT("%d SETUP%s"), Saved.Num(), Saved.Num() == 1 ? TEXT("") : TEXT("S"))));
	}

	// The detail of the picked one.
	const FApexSavedSetup* Picked = Saved.FindByPredicate([this](const FApexSavedSetup& S) { return S.Id == SelectedSaved; });
	if (DetailPanel)
	{
		DetailPanel->SetVisibility(Picked ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);
	}
	if (DetailEmpty)
	{
		DetailEmpty->SetVisibility(Picked ? ESlateVisibility::Collapsed : ESlateVisibility::HitTestInvisible);
	}
	if (!Picked)
	{
		return;
	}
	DetailName->SetText(FText::FromString(Picked->Name));
	const int32 Compound = Picked->Setup.GetClick(ApexCarSetup::TyreCompound);
	DetailMeta->SetText(FText::FromString(FString::Printf(TEXT("SAVED %s  ·  BEST %s  ·  %s"),
		*FormatDay(Picked->SavedAt), Picked->BestLapMs > 0 ? *FormatMs(Picked->BestLapMs) : TEXT("NO LAP YET"), *CompoundLabel(Compound))));
	DetailDiff->ClearChildren();
	auto AddDiff = [this](const FString& Label, const FString& Value, const FString& Delta)
	{
		UHorizontalBox* Line = WidgetTree->ConstructWidget<UHorizontalBox>();
		AddH(Line, MakeText(*WidgetTree, Label, Font::Body(16.0f), Ink.CopyWithNewOpacity(0.85f)), FMargin(), VAlign_Center, 1.0f);
		AddH(Line, MakeText(*WidgetTree, Value, Font::Mono(12.0f), Ink));
		UTextBlock* DeltaText = MakeText(*WidgetTree, Delta, Font::Mono(12.0f), Palette::Accent);
		DeltaText->SetJustification(ETextJustify::Right);
		AddH(Line, MakeSized(*WidgetTree, DeltaText, 76.0f, -1.0f), FMargin(10.0f, 0.0f, 0.0f, 0.0f));
		UVerticalBox* Wrap = WidgetTree->ConstructWidget<UVerticalBox>();
		AddV(Wrap, Line, FMargin(0.0f, 8.0f));
		AddV(Wrap, MakeSized(*WidgetTree, MakePanel(*WidgetTree, nullptr, FMargin(), MakeBrush(Ink.CopyWithNewOpacity(0.08f))), -1.0f, 1.0f));
		AddV(DetailDiff, Wrap);
	};
	if (Compound != 0)
	{
		AddDiff(TEXT("Next tyres"), CompoundLabel(Compound), FString());
	}
	static const TCHAR* KnobLabels[] = {
		TEXT("Front pressure"), TEXT("Rear pressure"), TEXT("Rev limiter"), TEXT("Engine braking"), TEXT("Final drive"),
		TEXT("Gear spread"), TEXT("Torque map"), TEXT("Brake bias"), TEXT("Front springs"), TEXT("Rear springs"),
		TEXT("Front dampers"), TEXT("Rear dampers"), TEXT("Front anti-roll bar"), TEXT("Rear anti-roll bar"), TEXT("Fuel load"),
		TEXT("Front wing"), TEXT("Rear wing"), TEXT("Front ride height"), TEXT("Rear ride height"), TEXT("Next tyres"),
		TEXT("Front brake ducts"), TEXT("Front camber"), TEXT("Rear camber"), TEXT("Front toe"), TEXT("Rear toe"),
		TEXT("Rear brake ducts"), TEXT("Brake pads"), TEXT("Radiator"), TEXT("ERS starting mode"),
		TEXT("ERS regen"), TEXT("ERS deploy map"),
	};
	static_assert(UE_ARRAY_COUNT(KnobLabels) == FApexCarSetup::KnobCount, "a label per knob");
	int32 Diffs = Compound != 0 ? 1 : 0;
	for (int32 Knob = 0; Knob < FApexCarSetup::KnobCount; ++Knob)
	{
		const int32 Clicks = Picked->Setup.GetClick(Knob);
		if (Knob == ApexCarSetup::TyreCompound || Clicks == 0)
		{
			continue;
		}
		AddDiff(KnobLabels[Knob], DescribeValue(Knob, Clicks), DescribeDelta(Knob, Clicks));
		++Diffs;
	}
	if (Diffs == 0)
	{
		AddV(DetailDiff, MakeText(*WidgetTree, TEXT("The car as filed."), Font::Body(16.0f), InkMuted), FMargin(0.0f, 8.0f));
	}
}

// --- Actions ------------------------------------------------------------------

void UApexHotlapWidget::HandleButtonActivated(UApexButtonWidget* Button)
{
	if (!Button)
	{
		return;
	}
	UApexSettingsSubsystem* Settings = GetSettings();
	const FName Id = Button->GetActionId();
	if (Id == ActionGoOut)
	{
		OnAction.Broadcast(bMenuMode ? EApexHotlapAction::CloseEditor : EApexHotlapAction::GoOut);
	}
	else if (Id == ActionReplay)
	{
		OnAction.Broadcast(EApexHotlapAction::ReplayBestLap);
	}
	else if (Id == ActionGhost)
	{
		OnAction.Broadcast(EApexHotlapAction::ToggleGhost);
	}
	else if (Id == ActionTyres)
	{
		OnAction.Broadcast(EApexHotlapAction::ToggleColdTyres);
	}
	else if (Id == ActionResetSetup)
	{
		OnAction.Broadcast(EApexHotlapAction::ResetSetup);
	}
	else if (Id == ActionGarageSaveReplay)
	{
		OnAction.Broadcast(EApexHotlapAction::SaveReplay);
	}
	else if (Id == ActionTune)
	{
		SetBoardOpen(false);
	}
	else if (Id == ActionBoard)
	{
		SetBoardOpen(true);
	}
	else if (Id == ActionWatchCamera)
	{
		OnAction.Broadcast(EApexHotlapAction::CycleWatchCamera);
	}
	else if (Id == ActionWatch)
	{
		const int32 Row = ApexNav::IndexOf(BoardRowButtons, Button);
		const int32 Car = BoardCars.IsValidIndex(Row) ? BoardCars[Row] : INDEX_NONE;
		if (Car != INDEX_NONE)
		{
			// A second click on the watched driver gives the view back.
			WatchedCar = Car == WatchedCar ? INDEX_NONE : Car;
			OnWatchCar.Broadcast(WatchedCar);
			RefreshBoard();
		}
	}
	else if (Id == ActionTab)
	{
		SetTab(static_cast<EApexGarageTab>(FMath::Max(0, ApexNav::IndexOf(TabButtons, Button))));
	}
	else if (Id == ActionCompound && Settings)
	{
		const int32 Index = ApexNav::IndexOf(CompoundButtons, Button);
		if (CompoundCards.IsValidIndex(Index))
		{
			Settings->SetCarSetupClick(ApexCarSetup::TyreCompound, CompoundCards[Index].Clicks);
		}
	}
	else if (Id == ActionPickSaved)
	{
		const int32 Index = ApexNav::IndexOf(SavedRowButtons, Button);
		if (SavedRowIds.IsValidIndex(Index))
		{
			SelectedSaved = SavedRowIds[Index];
			RefreshSaved();
		}
	}
	else if (Id == ActionSave && Settings)
	{
		FString Name = SaveNameBox ? SaveNameBox->GetText().ToString().TrimStartAndEnd() : FString();
		if (Name.IsEmpty())
		{
			Name = FString::Printf(TEXT("Setup %d"), Settings->GetSavedSetups(GetCarId()).Num() + 1);
		}
		SelectedSaved = Settings->SaveSetupAs(Name, GetCarId());
		if (SaveNameBox)
		{
			SaveNameBox->SetText(FText::GetEmpty());
		}
	}
	else if (Id == ActionLoad && Settings && SelectedSaved.IsValid())
	{
		Settings->LoadSavedSetup(SelectedSaved);
	}
	else if (Id == ActionOverwrite && Settings && SelectedSaved.IsValid())
	{
		Settings->OverwriteSavedSetup(SelectedSaved);
	}
	else if (Id == ActionRename && Settings && SelectedSaved.IsValid() && SaveNameBox)
	{
		const FString Name = SaveNameBox->GetText().ToString().TrimStartAndEnd();
		if (Name.IsEmpty())
		{
			for (const FApexSavedSetup& Saved : Settings->GetSavedSetups(GetCarId()))
			{
				if (Saved.Id == SelectedSaved)
				{
					SaveNameBox->SetText(FText::FromString(Saved.Name));
				}
			}
			SaveNameBox->SetKeyboardFocus();
		}
		else
		{
			Settings->RenameSavedSetup(SelectedSaved, Name);
			SaveNameBox->SetText(FText::GetEmpty());
		}
	}
	else if (Id == ActionDelete && Settings && SelectedSaved.IsValid())
	{
		Settings->DeleteSavedSetup(SelectedSaved);
		SelectedSaved.Invalidate();
		if (SaveButton)
		{
			ApexNav::Focus(SaveButton);
		}
	}
}

void UApexHotlapWidget::HandleStepperChanged(UApexStepperWidget* Control, int32 Value)
{
	UApexSettingsSubsystem* Settings = GetSettings();
	if (bRefreshing || !Control || !Settings)
	{
		return;
	}
	ApexUiAudio::Play(this, EApexUiSound::Adjust);
	Settings->SetCarSetupClick(Control->Index, Value);
}

void UApexHotlapWidget::HandleSettingsChanged(EApexSettingsGroup Group)
{
	if (Group == EApexSettingsGroup::CarSetup)
	{
		RefreshSetup();
	}
	else if (Group == EApexSettingsGroup::Gameplay)
	{
		RefreshGhostRows();
	}
}

void UApexHotlapWidget::HandleGhostLap(const FApexGhostLap& Lap)
{
	RefreshGhostRows();
}

void UApexHotlapWidget::HandleLapRecord(const FApexLapRecord& Record)
{
	RefreshGhostRows();
	RefreshSheet();
}

void UApexHotlapWidget::HandleSetupSheet(const FApexCarSetupSheet& Sheet)
{
	// A car with compounds of its own renames the cards; the same list
	// again (a rejoin) leaves them, and the focus on them, alone.
	const TArray<FString> Names = Sheet.HasCompounds() ? Sheet.Compounds : TArray<FString>();
	if (Names != CompoundCardNames)
	{
		BuildCompoundCards();
	}
	RefreshSetup();
}
