#include "UI/ApexTrackGuideWidget.h"

#include "ApexSettingsSave.h"
#include "ApexSettingsSubsystem.h"
#include "Blueprint/WidgetTree.h"
#include "Catalog/ApexCatalogRows.h"
#include "Components/Border.h"
#include "Components/HorizontalBox.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/ScrollBox.h"
#include "Components/SizeBox.h"
#include "Components/Spacer.h"
#include "Components/TextBlock.h"
#include "Components/UniformGridPanel.h"
#include "Components/UniformGridSlot.h"
#include "Components/VerticalBox.h"
#include "Engine/GameInstance.h"
#include "Guide/ApexTrackGuideSubsystem.h"
#include "UI/ApexButtonWidget.h"
#include "UI/ApexUIStyle.h"

using namespace ApexUI;

// Named, not anonymous: this module is unity-built.
namespace ApexGuideUi
{
	constexpr float CardWidth = 460.0f;
	constexpr float CardMaxHeight = 900.0f;
	const FLinearColor CardFill = Palette::Surface.CopyWithNewOpacity(0.86f);

	const FName ActionPrev(TEXT("Guide.Prev"));
	const FName ActionNext(TEXT("Guide.Next"));
	const FName ActionCamera(TEXT("Guide.Camera"));
	const FName ActionPause(TEXT("Guide.Pause"));
	const FName ActionBack(TEXT("Guide.Back"));

	/** A stat tile in a grid of two columns. */
	void AddStat(UWidgetTree& Tree, UUniformGridPanel* Grid, int32& Count, const FString& Label, const FString& Value, float Size = 20.0f)
	{
		if (Value.IsEmpty())
		{
			return;
		}
		UTextBlock* ValueText = nullptr;
		UWidget* Tile = MakeStat(Tree, Label, ValueText, Size);
		ValueText->SetText(FText::FromString(Value));
		UUniformGridSlot* GridSlot = Grid->AddChildToUniformGrid(Tile, Count / 2, Count % 2);
		GridSlot->SetHorizontalAlignment(HAlign_Fill);
		GridSlot->SetVerticalAlignment(VAlign_Fill);
		++Count;
	}

	/** A bulleted line that wraps under its own text. */
	UWidget* Bullet(UWidgetTree& Tree, const FString& Line)
	{
		UHorizontalBox* Row = Tree.ConstructWidget<UHorizontalBox>();
		AddH(Row, MakeText(Tree, TEXT("•"), Font::Body(14.0f, true), Palette::Accent), FMargin(0.0f, 0.0f, 10.0f, 0.0f), VAlign_Top);
		UTextBlock* Text = MakeText(Tree, Line, Font::Body(13.0f), Palette::TextSecondary);
		Text->SetAutoWrapText(true);
		AddH(Row, Text, FMargin(), VAlign_Top, 1.0f);
		return Row;
	}

	UBorder* Chip(UWidgetTree& Tree, const FString& Text, const FLinearColor& Fill, const FLinearColor& Ink, UTextBlock** OutText = nullptr)
	{
		UTextBlock* Label = MakeText(Tree, Text, Font::Mono(11.0f, 140), Ink);
		if (OutText)
		{
			*OutText = Label;
		}
		return MakePanel(Tree, Label, FMargin(12.0f, 6.0f), MakeBrush(Fill));
	}
}

UApexTrackGuideWidget::UApexTrackGuideWidget(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	SetVisibility(ESlateVisibility::Collapsed);
}

UApexTrackGuideSubsystem* UApexTrackGuideWidget::GetGuide() const
{
	const UGameInstance* GameInstance = GetGameInstance();
	return GameInstance ? GameInstance->GetSubsystem<UApexTrackGuideSubsystem>() : nullptr;
}

bool UApexTrackGuideWidget::IsMetric() const
{
	const UGameInstance* GameInstance = GetGameInstance();
	const UApexSettingsSubsystem* Settings = GameInstance ? GameInstance->GetSubsystem<UApexSettingsSubsystem>() : nullptr;
	return !Settings || !Settings->Get() || Settings->Get()->Units == EApexUnits::Metric;
}

void UApexTrackGuideWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	using namespace ApexGuideUi;

	UOverlay* Layers = WidgetTree->ConstructWidget<UOverlay>();

	LoadingPanel = BuildLoading();
	UOverlaySlot* LoadingSlot = Layers->AddChildToOverlay(LoadingPanel);
	LoadingSlot->SetHorizontalAlignment(HAlign_Center);
	LoadingSlot->SetVerticalAlignment(VAlign_Center);

	// The card on the left, clear of the middle where the cars are.
	CardBody = WidgetTree->ConstructWidget<UVerticalBox>();
	UScrollBox* Scroll = WidgetTree->ConstructWidget<UScrollBox>();
	Scroll->SetScrollBarVisibility(ESlateVisibility::Collapsed);
	Scroll->AddChild(CardBody);
	USizeBox* CardSize = MakeSized(*WidgetTree,
		MakePanel(*WidgetTree, Scroll, FMargin(26.0f, 22.0f), MakeBrush(CardFill, Palette::Border, 1.0f)), CardWidth, -1.0f);
	CardSize->SetMaxDesiredHeight(CardMaxHeight);
	Card = CardSize;
	UOverlaySlot* CardSlot = Layers->AddChildToOverlay(Card);
	CardSlot->SetHorizontalAlignment(HAlign_Left);
	CardSlot->SetVerticalAlignment(VAlign_Top);
	CardSlot->SetPadding(FMargin(Metrics::PageGutter, 40.0f, 0.0f, 0.0f));

	TopStrip = BuildTopStrip();
	UOverlaySlot* TopSlot = Layers->AddChildToOverlay(TopStrip);
	TopSlot->SetHorizontalAlignment(HAlign_Center);
	TopSlot->SetVerticalAlignment(VAlign_Top);
	TopSlot->SetPadding(FMargin(CardWidth, 40.0f, 0.0f, 0.0f));

	Controls = BuildControls();
	UOverlaySlot* ControlSlot = Layers->AddChildToOverlay(Controls);
	ControlSlot->SetHorizontalAlignment(HAlign_Center);
	ControlSlot->SetVerticalAlignment(VAlign_Bottom);
	ControlSlot->SetPadding(FMargin(0.0f, 0.0f, 0.0f, 34.0f));

	Layers->SetVisibility(ESlateVisibility::SelfHitTestInvisible);
	WidgetTree->RootWidget = Layers;
}

UWidget* UApexTrackGuideWidget::BuildLoading()
{
	UVerticalBox* Box = WidgetTree->ConstructWidget<UVerticalBox>();
	AddV(Box, MakeLabel(*WidgetTree, TEXT("Track guide"), Palette::Accent), FMargin(), HAlign_Center);
	LoadingTitle = MakeText(*WidgetTree, FString(), Font::Display(40.0f), Palette::TextPrimary);
	AddV(Box, LoadingTitle, FMargin(0.0f, 10.0f, 0.0f, 0.0f), HAlign_Center);
	LoadingText = MakeText(*WidgetTree, TEXT("Building the circuit…"), Font::Mono(11.0f, 120), Palette::TextMuted);
	AddV(Box, LoadingText, FMargin(0.0f, 12.0f, 0.0f, 0.0f), HAlign_Center);
	Box->SetVisibility(ESlateVisibility::HitTestInvisible);
	return Box;
}

UWidget* UApexTrackGuideWidget::BuildTopStrip()
{
	using namespace ApexGuideUi;
	UHorizontalBox* Strip = WidgetTree->ConstructWidget<UHorizontalBox>();
	ProgressText = MakeText(*WidgetTree, FString(), Font::Mono(12.0f, 140), Palette::TextPrimary);
	AddH(Strip, MakePanel(*WidgetTree, ProgressText, FMargin(14.0f, 7.0f), MakeBrush(CardFill)), FMargin(), VAlign_Center);
	UTextBlock* ChipText = nullptr;
	PlaybackChip = Chip(*WidgetTree, FString(), Palette::Accent, Palette::OnAccent, &ChipText);
	PlaybackText = ChipText;
	AddH(Strip, PlaybackChip, FMargin(10.0f, 0.0f, 0.0f, 0.0f), VAlign_Center);
	Strip->SetVisibility(ESlateVisibility::HitTestInvisible);
	return Strip;
}

UApexButtonWidget* UApexTrackGuideWidget::MakeControl(const FString& Label, const FString& Key, FName Action)
{
	FApexButtonSpec Spec;
	Spec.Label = Label;
	Spec.KeyCap = Key;
	Spec.bKeyCapLeading = true;
	Spec.Variant = EApexButtonVariant::Ghost;
	Spec.bCentreLabel = true;
	Spec.LabelSize = 14.0f;
	Spec.Height = 44.0f;
	Spec.ActionId = Action;
	if (Action == ApexGuideUi::ActionBack)
	{
		Spec.Sound = EApexUiSound::Back;
	}
	UApexButtonWidget* Button = WidgetTree->ConstructWidget<UApexButtonWidget>();
	Button->Setup(Spec);
	Button->OnActivated.AddDynamic(this, &UApexTrackGuideWidget::HandleButtonActivated);
	return Button;
}

UWidget* UApexTrackGuideWidget::BuildControls()
{
	using namespace ApexGuideUi;
	UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();
	PrevButton = MakeControl(TEXT("Previous"), TEXT("←"), ActionPrev);
	NextButton = MakeControl(TEXT("Next"), TEXT("→"), ActionNext);
	CameraButton = MakeControl(TEXT("Camera"), TEXT("C"), ActionCamera);
	PauseButton = MakeControl(TEXT("Pause"), TEXT("Space"), ActionPause);
	BackButton = MakeControl(TEXT("Back"), TEXT("Esc"), ActionBack);
	const TPair<UApexButtonWidget*, float> Buttons[] = {
		{ PrevButton, 180.0f }, { NextButton, 300.0f }, { CameraButton, 290.0f }, { PauseButton, 150.0f }, { BackButton, 130.0f } };
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(Buttons); ++Index)
	{
		AddH(Row, MakeSized(*WidgetTree, Buttons[Index].Key, Buttons[Index].Value, 44.0f),
			FMargin(Index == 0 ? 0.0f : 8.0f, 0.0f, 0.0f, 0.0f), VAlign_Center);
	}
	return MakePanel(*WidgetTree, Row, FMargin(10.0f), MakeBrush(CardFill, Palette::Border, 1.0f));
}

void UApexTrackGuideWidget::SetActive(bool bInActive)
{
	bActive = bInActive;
	SetVisibility(bActive ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);
	BuiltCorner = -2;
	bBuiltRunning = false;
	// Not a value any refresh produces, so the first one is applied (an empty chip hides).
	ShownPlayback = TEXT("-");
	ShownProgress.Reset();
	ShownCamera.Reset();
	ShownNext.Reset();
	ShownPause.Reset();
	if (bActive)
	{
		RefreshLive();
	}
}

void UApexTrackGuideWidget::HandleButtonActivated(UApexButtonWidget* Button)
{
	using namespace ApexGuideUi;
	UApexTrackGuideSubsystem* Guide = GetGuide();
	if (!Button || !Guide)
	{
		return;
	}
	const FName Id = Button->GetActionId();
	if (Id == ActionPrev) { Guide->Previous(); }
	else if (Id == ActionNext) { Guide->Next(); }
	else if (Id == ActionCamera) { Guide->CycleCamera(1); }
	else if (Id == ActionPause) { Guide->TogglePause(); }
	else if (Id == ActionBack) { Guide->Close(); }
}

void UApexTrackGuideWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);
	if (bActive)
	{
		RefreshLive();
	}
}

void UApexTrackGuideWidget::RefreshLive()
{
	const UApexTrackGuideSubsystem* Guide = GetGuide();
	if (!Guide || !Guide->IsOpen())
	{
		return;
	}
	const bool bRunning = Guide->GetState() == EApexGuideState::Running;
	const ApexGuide::FPlayer& Player = Guide->GetPlayer();
	const FApexTrackGuide& Data = Guide->GetGuide();

	// Loading: the shell's background covers the world; say what is coming.
	const bool bShowWorld = bRunning && Guide->GetReveal() > 0.0f;
	LoadingPanel->SetVisibility(bShowWorld ? ESlateVisibility::Collapsed : ESlateVisibility::HitTestInvisible);
	LoadingTitle->SetText(FText::FromString(Data.Track.DisplayName.IsEmpty() ? Data.Track.Stem : Data.Track.DisplayName));
	LoadingText->SetText(FText::FromString(Guide->GetState() == EApexGuideState::Loading
		? TEXT("READING THE RECORDING…") : TEXT("BUILDING THE CIRCUIT…")));
	const ESlateVisibility Live = bShowWorld ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed;
	Card->SetVisibility(Live);
	Controls->SetVisibility(Live);
	TopStrip->SetVisibility(bShowWorld ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
	Card->SetRenderOpacity(Guide->GetReveal());
	TopStrip->SetRenderOpacity(Guide->GetReveal());
	Controls->SetRenderOpacity(Guide->GetReveal());
	if (!bRunning)
	{
		return;
	}

	const int32 CornerIndex = Player.GetCorner();
	if (CornerIndex != BuiltCorner || !bBuiltRunning)
	{
		BuiltCorner = CornerIndex;
		bBuiltRunning = true;
		RebuildCard();
	}

	// Along the top: which stop, and how the clock runs.
	const int32 Stops = Data.Corners.Num();
	const FApexGuideCorner* Corner = Guide->GetCorner();
	const FString Progress = Corner
		? FString::Printf(TEXT("%s  ·  %d / %d"), *ApexTrackGuide::TurnLabel(*Corner), CornerIndex + 1, Stops)
		: FString::Printf(TEXT("OVERVIEW  ·  %d STOPS"), Stops);
	if (Progress != ShownProgress)
	{
		ShownProgress = Progress;
		ProgressText->SetText(FText::FromString(Progress));
	}
	FString Playback;
	bool bAccent = true;
	if (Player.IsPaused())
	{
		Playback = TEXT("PAUSED");
	}
	else
	{
		switch (Player.GetPhase())
		{
		case ApexGuide::EPhase::Travel:
			Playback = FString::Printf(TEXT("FAST FORWARD %.0fx"), Player.GetRate());
			break;
		case ApexGuide::EPhase::Slow:
			Playback = FString::Printf(TEXT("SLOW MOTION %gx"), Player.GetRate());
			break;
		case ApexGuide::EPhase::Normal:
			Playback = TEXT("NORMAL SPEED");
			bAccent = false;
			break;
		default:
			break;
		}
	}
	if (Playback != ShownPlayback)
	{
		ShownPlayback = Playback;
		PlaybackChip->SetVisibility(Playback.IsEmpty() ? ESlateVisibility::Collapsed : ESlateVisibility::HitTestInvisible);
		PlaybackChip->SetBrush(MakeBrush(bAccent ? Palette::Accent : ApexGuideUi::CardFill));
		PlaybackText->SetText(FText::FromString(Playback));
		PlaybackText->SetColorAndOpacity(FSlateColor(bAccent ? Palette::OnAccent : Palette::TextSecondary));
	}

	// The buttons say where they go.
	FString Next;
	if (CornerIndex + 1 < Stops)
	{
		Next = FString::Printf(TEXT("Next: %s"), *ApexTrackGuide::TurnLabel(Data.Corners[CornerIndex + 1]));
	}
	else
	{
		Next = TEXT("Next: Overview");
	}
	if (Next != ShownNext)
	{
		ShownNext = Next;
		NextButton->SetLabel(Next);
	}
	const FString Camera = Guide->GetCameraCount() > 0
		? FString::Printf(TEXT("%s  %d/%d"), *Guide->GetCameraName(), Guide->GetCameraNumber(), Guide->GetCameraCount())
		: FString(TEXT("Camera"));
	if (Camera != ShownCamera)
	{
		ShownCamera = Camera;
		CameraButton->SetLabel(Camera);
	}
	const FString Pause = Player.IsPaused() ? TEXT("Play") : TEXT("Pause");
	if (Pause != ShownPause)
	{
		ShownPause = Pause;
		PauseButton->SetLabel(Pause);
	}
	PrevButton->SetIsEnabled(CornerIndex != INDEX_NONE);
}

void UApexTrackGuideWidget::RebuildCard()
{
	CardBody->ClearChildren();
	const UApexTrackGuideSubsystem* Guide = GetGuide();
	if (!Guide)
	{
		return;
	}
	if (Guide->GetCorner())
	{
		BuildCornerCard(CardBody, Guide->GetPlayer().GetCorner());
	}
	else
	{
		BuildTrackCard(CardBody);
	}
}

void UApexTrackGuideWidget::BuildTrackCard(UVerticalBox* Body)
{
	using namespace ApexGuideUi;
	const UApexTrackGuideSubsystem* Guide = GetGuide();
	const FApexTrackGuide& Data = Guide->GetGuide();
	const FApexGuideTrack& T = Data.Track;
	const bool bMetric = IsMetric();

	AddV(Body, MakeLabel(*WidgetTree, TEXT("Track guide"), Palette::Accent));
	UTextBlock* Title = MakeText(*WidgetTree, T.DisplayName.IsEmpty() ? T.Stem : T.DisplayName, Font::Display(34.0f), Palette::TextPrimary);
	Title->SetAutoWrapText(true);
	AddV(Body, Title, FMargin(0.0f, 8.0f, 0.0f, 0.0f));

	TArray<FString> Place;
	if (!T.Country.IsEmpty()) { Place.Add(T.Country.ToUpper()); }
	if (!T.City.IsEmpty()) { Place.Add(T.City.ToUpper()); }
	if (Place.Num() > 0)
	{
		AddV(Body, MakeText(*WidgetTree, FString::Join(Place, TEXT(" · ")), Font::Mono(10.0f, 120), Palette::TextMuted),
			FMargin(0.0f, 6.0f, 0.0f, 0.0f));
	}
	if (!T.Description.IsEmpty())
	{
		UTextBlock* Description = MakeText(*WidgetTree, T.Description, Font::Body(13.0f), Palette::TextSecondary);
		Description->SetAutoWrapText(true);
		AddV(Body, Description, FMargin(0.0f, 10.0f, 0.0f, 0.0f));
	}

	UUniformGridPanel* Grid = WidgetTree->ConstructWidget<UUniformGridPanel>();
	Grid->SetSlotPadding(FMargin(4.0f));
	int32 Count = 0;
	AddStat(*WidgetTree, Grid, Count, TEXT("Length"), T.LengthM > 0.0f ? ApexTrackGuide::DistanceText(T.LengthM, bMetric) : FString());
	FString Turns;
	if (T.Corners > 0)
	{
		Turns = FString::FromInt(T.Corners);
		if (T.Left > 0 || T.Right > 0)
		{
			Turns += FString::Printf(TEXT("  ·  %dL %dR"), T.Left, T.Right);
		}
	}
	AddStat(*WidgetTree, Grid, Count, TEXT("Turns"), Turns);
	if (Data.Corners.Num() > 0 && Data.Corners.Num() != T.Corners)
	{
		AddStat(*WidgetTree, Grid, Count, TEXT("Guide stops"), FString::FromInt(Data.Corners.Num()));
	}
	AddStat(*WidgetTree, Grid, Count, TEXT("Altitude"), T.bHasAltitude ? ApexTrackGuide::HeightText(T.AltitudeM, bMetric, false) : FString());
	const float Range = T.ElevationMaxM - T.ElevationMinM;
	AddStat(*WidgetTree, Grid, Count, TEXT("Elevation range"), Range > 0.5f ? ApexTrackGuide::HeightText(Range, bMetric, false) : FString());
	AddStat(*WidgetTree, Grid, Count, TEXT("Climb per lap"), T.ClimbM > 0.5f ? ApexTrackGuide::HeightText(T.ClimbM, bMetric, false) : FString());
	AddStat(*WidgetTree, Grid, Count, TEXT("Longest straight"), T.LongestStraightM > 0.0f ? ApexTrackGuide::DistanceText(T.LongestStraightM, bMetric) : FString());
	AddStat(*WidgetTree, Grid, Count, TEXT("DRS zones"), T.DrsZones > 0 ? FString::FromInt(T.DrsZones) : FString());
	AddStat(*WidgetTree, Grid, Count, TEXT("Direction"), ApexTrackGuide::TrackDirectionLabel(T.Direction));
	AddStat(*WidgetTree, Grid, Count, TEXT("Built"), T.YearBuilt > 0 ? FString::FromInt(T.YearBuilt) : FString());
	AddV(Body, Grid, FMargin(-4.0f, 16.0f, -4.0f, 0.0f));

	// The guide's own lap: whose figures every corner card quotes.
	TArray<FString> Lap = { TEXT("Guide lap") };
	if (!Data.DisplayClass.IsEmpty()) { Lap.Add(ApexCatalog::DisplayClass(Data.DisplayClass)); }
	const FString CarName = Guide->GetCarName();
	if (!CarName.IsEmpty()) { Lap.Add(CarName); }
	AddV(Body, MakeLabel(*WidgetTree, FString::Join(Lap, TEXT(" · ")), Palette::Accent), FMargin(0.0f, 18.0f, 0.0f, 0.0f));
	UUniformGridPanel* LapGrid = WidgetTree->ConstructWidget<UUniformGridPanel>();
	LapGrid->SetSlotPadding(FMargin(4.0f));
	int32 LapCount = 0;
	AddStat(*WidgetTree, LapGrid, LapCount, TEXT("Lap time"), T.LapTimeS > 0.0f ? ApexTrackGuide::LapTimeText(T.LapTimeS) : FString(), 24.0f);
	AddStat(*WidgetTree, LapGrid, LapCount, TEXT("Top speed"), T.TopSpeedKph > 0.0f ? ApexTrackGuide::SpeedText(T.TopSpeedKph, bMetric) : FString(), 24.0f);
	AddV(Body, LapGrid, FMargin(-4.0f, 6.0f, -4.0f, 0.0f));

	if (T.Notes.Num() > 0)
	{
		AddV(Body, MakeDivider(*WidgetTree), FMargin(0.0f, 16.0f, 0.0f, 12.0f));
		for (const FString& Line : T.Notes)
		{
			AddV(Body, Bullet(*WidgetTree, Line), FMargin(0.0f, 0.0f, 0.0f, 8.0f));
		}
	}

	if (Data.Corners.Num() > 0)
	{
		const FApexGuideCorner& First = Data.Corners[0];
		AddV(Body, MakeDivider(*WidgetTree), FMargin(0.0f, 14.0f, 0.0f, 10.0f));
		AddV(Body, MakeText(*WidgetTree, FString::Printf(TEXT("›  NEXT: %s  ·  %s"), *ApexTrackGuide::TurnLabel(First), *First.Name.ToUpper()),
			Font::Mono(11.0f, 100), Palette::TextSecondary));
	}
}

void UApexTrackGuideWidget::BuildCornerCard(UVerticalBox* Body, int32 CornerIndex)
{
	using namespace ApexGuideUi;
	const UApexTrackGuideSubsystem* Guide = GetGuide();
	const FApexTrackGuide& Data = Guide->GetGuide();
	if (!Data.Corners.IsValidIndex(CornerIndex))
	{
		return;
	}
	const FApexGuideCorner& C = Data.Corners[CornerIndex];
	const bool bMetric = IsMetric();

	UHorizontalBox* Head = WidgetTree->ConstructWidget<UHorizontalBox>();
	AddH(Head, MakeLabel(*WidgetTree, ApexTrackGuide::TurnLabel(C), Palette::Accent), FMargin(), VAlign_Center, 1.0f);
	AddH(Head, MakeText(*WidgetTree, FString::Printf(TEXT("STOP %d / %d"), CornerIndex + 1, Data.Corners.Num()),
		Font::Mono(10.0f, 120), Palette::TextMuted), FMargin(), VAlign_Center);
	AddV(Body, Head);

	UTextBlock* Title = MakeText(*WidgetTree, C.Name, Font::Display(32.0f), Palette::TextPrimary);
	Title->SetAutoWrapText(true);
	AddV(Body, Title, FMargin(0.0f, 8.0f, 0.0f, 0.0f));

	UHorizontalBox* Shape = WidgetTree->ConstructWidget<UHorizontalBox>();
	const FString Direction = ApexTrackGuide::DirectionLabel(C.Direction);
	if (!Direction.IsEmpty())
	{
		const bool bChicane = Direction.Contains(TEXT("-"));
		AddH(Shape, Chip(*WidgetTree, bChicane ? Direction + TEXT(" CHICANE") : Direction, Palette::Accent, Palette::OnAccent),
			FMargin(0.0f, 0.0f, 12.0f, 0.0f), VAlign_Center);
	}
	TArray<FString> Geometry;
	if (C.TurnDeg > 0.0f) { Geometry.Add(FString::Printf(TEXT("%.0f°"), C.TurnDeg)); }
	if (C.MinRadiusM > 0.0f) { Geometry.Add(FString::Printf(TEXT("RADIUS %s"), *ApexTrackGuide::DistanceText(C.MinRadiusM, bMetric).ToUpper())); }
	AddH(Shape, MakeText(*WidgetTree, FString::Join(Geometry, TEXT("  ·  ")), Font::Mono(10.0f, 120), Palette::TextMuted), FMargin(), VAlign_Center);
	AddV(Body, Shape, FMargin(0.0f, 10.0f, 0.0f, 0.0f));

	UUniformGridPanel* Big = WidgetTree->ConstructWidget<UUniformGridPanel>();
	Big->SetSlotPadding(FMargin(4.0f));
	int32 BigCount = 0;
	AddStat(*WidgetTree, Big, BigCount, TEXT("Min speed"), ApexTrackGuide::SpeedText(C.MinSpeedKph, bMetric), 30.0f);
	AddStat(*WidgetTree, Big, BigCount, TEXT("Gear"), C.ApexGear > 0 ? FString::FromInt(C.ApexGear) : FString(TEXT("—")), 30.0f);
	AddV(Body, Big, FMargin(-4.0f, 16.0f, -4.0f, 0.0f));

	UUniformGridPanel* Grid = WidgetTree->ConstructWidget<UUniformGridPanel>();
	Grid->SetSlotPadding(FMargin(4.0f));
	int32 Count = 0;
	AddStat(*WidgetTree, Grid, Count, C.bFlatOut ? TEXT("Entry speed") : TEXT("Braking from"), ApexTrackGuide::SpeedText(C.EntrySpeedKph, bMetric));
	AddStat(*WidgetTree, Grid, Count, TEXT("Exit speed"), ApexTrackGuide::SpeedText(C.ExitSpeedKph, bMetric));
	if (FMath::Abs(C.ElevationChangeM) >= 0.5f)
	{
		AddStat(*WidgetTree, Grid, Count, TEXT("Elevation"), ApexTrackGuide::HeightText(C.ElevationChangeM, bMetric, true));
	}
	if (FMath::Abs(C.BankingDeg) >= 0.5f)
	{
		AddStat(*WidgetTree, Grid, Count, TEXT("Banking"), FString::Printf(TEXT("%+.0f°"), C.BankingDeg));
	}
	AddV(Body, Grid, FMargin(-4.0f, 4.0f, -4.0f, 0.0f));

	const FString Brake = ApexTrackGuide::BrakeText(C);
	if (!Brake.IsEmpty())
	{
		UBorder* BrakeBox = MakePanel(*WidgetTree,
			MakeText(*WidgetTree, Brake.ToUpper(), Font::Display(18.0f), C.bFlatOut ? Palette::Live : Palette::TextPrimary),
			FMargin(16.0f, 10.0f), MakeBrush(Palette::Surface, C.bFlatOut ? Palette::Live : Palette::Error, 1.0f));
		AddV(Body, BrakeBox, FMargin(0.0f, 8.0f, 0.0f, 0.0f));
	}

	const TArray<FString> Lines = ApexTrackGuide::CornerLines(C);
	if (Lines.Num() > 0)
	{
		AddV(Body, MakeDivider(*WidgetTree), FMargin(0.0f, 16.0f, 0.0f, 12.0f));
		AddV(Body, MakeLabel(*WidgetTree, TEXT("Gotchas"), Palette::Accent), FMargin(0.0f, 0.0f, 0.0f, 10.0f));
		for (const FString& Line : Lines)
		{
			AddV(Body, Bullet(*WidgetTree, Line), FMargin(0.0f, 0.0f, 0.0f, 8.0f));
		}
	}
}
