#include "UI/ApexSessionCreateWidget.h"

#include "ApexCarPreviewStage.h"
#include "ApexMenuFlowSubsystem.h"
#include "ApexNetSubsystem.h"
#include "ApexSettingsSave.h"
#include "ApexSettingsSubsystem.h"
#include "ApexSim.h"
#include "Blueprint/WidgetTree.h"
#include "Cars/ApexCarContentSubsystem.h"
#include "Catalog/ApexCatalogRows.h"
#include "Components/Border.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Image.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/ProgressBar.h"
#include "Components/ScaleBox.h"
#include "Components/SizeBox.h"
#include "Components/Slider.h"
#include "Components/Spacer.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Engine/GameInstance.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Track/ApexTrackContentSubsystem.h"
#include "UI/ApexButtonWidget.h"
#include "UI/ApexCreateSessionModel.h"
#include "UI/ApexNavigation.h"
#include "UI/ApexRootWidget.h"
#include "UI/ApexUIStyle.h"

namespace
{
	/** The allowed-assist toggles, in the order they are laid out. */
	enum class EApexAssistChip : int32
	{
		Abs,
		TractionControl,
		AutoGearbox,
		SteeringAssist,
		RacingLine,
		Count,
	};

	/** The flag a chip toggles on the flow's allowed set. */
	bool& AssistFlag(FApexAllowedAssists& Assists, EApexAssistChip Chip)
	{
		switch (Chip)
		{
		case EApexAssistChip::Abs:             return Assists.bAbs;
		case EApexAssistChip::TractionControl: return Assists.bTractionControl;
		case EApexAssistChip::AutoGearbox:     return Assists.bAutoGearbox;
		case EApexAssistChip::SteeringAssist:  return Assists.bSteeringAssist;
		default:                               return Assists.bRacingLine;
		}
	}

	/** The damage tiles, in EApexDamageLevel order. */
	struct FDamageOption
	{
		const TCHAR* Label;
		const TCHAR* Description;
		const TCHAR* Summary;
	};
	const FDamageOption DamageOptions[] = {
		{ TEXT("Off"),     TEXT("No damage"),        TEXT("NO DAMAGE") },
		{ TEXT("Reduced"), TEXT("Half"),             TEXT("REDUCED DAMAGE") },
		{ TEXT("Full"),    TEXT("As modelled"),      TEXT("FULL DAMAGE") },
	};

	struct FModeOption
	{
		EApexGameMode Mode;
		const TCHAR* Label;
		const TCHAR* Description;
	};

	// Replay and Qualification are left out because nothing drives them yet,
	// and Demo lap because the server turns it into a dead end for whoever
	// asks (it drops the human players to spectators); the hotlap took its
	// tile.
	const FModeOption ModeOptions[] = {
		{ EApexGameMode::FreePractice, TEXT("Free practice"), TEXT("Drive freely") },
		{ EApexGameMode::Sandbox,      TEXT("Sandbox"),       TEXT("Free camera") },
		{ EApexGameMode::Hotlap,       TEXT("Hotlap"),        TEXT("Flying laps") },
		{ EApexGameMode::Race,         TEXT("Race"),          TEXT("Grid start") },
	};

	const TCHAR* ModeLabel(EApexGameMode Mode)
	{
		for (const FModeOption& Option : ModeOptions)
		{
			if (Option.Mode == Mode)
			{
				return Option.Label;
			}
		}
		return TEXT("Session");
	}

	struct FLapPreset
	{
		const TCHAR* Label;
		int32 Laps;
	};
	const FLapPreset LapPresets[] = {
		{ TEXT("Sprint"), 3 }, { TEXT("Short"), 5 }, { TEXT("Feature"), 12 }, { TEXT("Endurance"), 30 },
	};

	/** A timed race's quick picks, minutes. */
	struct FDurationPreset
	{
		const TCHAR* Label;
		int32 Minutes;
	};
	const FDurationPreset DurationPresets[] = {
		{ TEXT("Sprint"), 20 }, { TEXT("Feature"), 60 }, { TEXT("Endurance"), 6 * 60 }, { TEXT("Twenty-four"), 24 * 60 },
	};

	/** Quick picks for the clock on the model day: sunrise is just after
	 * five, sunset a little before nine (ApexSky). */
	struct FTimePreset
	{
		const TCHAR* Label;
		int32 Minutes;
	};
	const FTimePreset TimePresets[] = {
		{ TEXT("Dawn"), 6 * 60 + 15 }, { TEXT("Noon"), 13 * 60 }, { TEXT("Dusk"), 20 * 60 + 30 }, { TEXT("Night"), 23 * 60 },
	};

	/** "Pro 93-97" for a level, "Mixed" for the mixed field: what the AI
	 * level card and the footer say about the field. */
	FString AiSkillSummary(int32 Skill)
	{
		const int32 Level = ApexAiSkill::Clamp(Skill);
		if (Level == ApexAiSkill::Mixed)
		{
			return TEXT("Mixed");
		}
		const int32 Low = FMath::Clamp(Level - ApexAiSkill::Spread / 2, ApexAiSkill::Min, ApexAiSkill::Max - ApexAiSkill::Spread);
		return FString::Printf(TEXT("%s %d-%d"), ApexAiSkill::Label(Level), Low, Low + ApexAiSkill::Spread);
	}

	/** The wind's strengths offered, km/h; the last chip is Auto. */
	constexpr int32 WindPresetsKph[] = {0, 12, 22, 35};
	const TCHAR* WindPresetLabels[] = {TEXT("Calm"), TEXT("Light"), TEXT("Breezy"), TEXT("Strong")};
	constexpr int32 WindPresetCount = UE_ARRAY_COUNT(WindPresetsKph);

	/** The dial's eight points, from ahead round to the left (the track
	 * frame turns counter-clockwise), and what each is called on its chip. */
	constexpr int32 WindFromCount = 8;
	const TCHAR* WindFromChips[WindFromCount] = {
		TEXT("AHD"), TEXT("AL"), TEXT("L"), TEXT("BL"), TEXT("BHD"), TEXT("BR"), TEXT("R"), TEXT("AR") };

	/** What the air temperature feels like, under a fixed figure. */
	const TCHAR* DescribeAir(int32 Celsius)
	{
		if (Celsius < 5)  { return TEXT("COLD: SLOW TYRES"); }
		if (Celsius < 15) { return TEXT("COOL"); }
		if (Celsius < 26) { return TEXT("MILD"); }
		if (Celsius < 33) { return TEXT("WARM"); }
		return TEXT("HOT: THIN AIR, HOT TYRES");
	}

	/** The setup chip for a working setup that is no saved one. */
	const FGuid CustomSetupMarker(0x43555354, 0x4F4D, 0x5345, 0x5455);
	/** Saved setups shown at most, newest first (the working one always is). */
	constexpr int32 MaxSetupsShown = 4;

	constexpr float RightColumnWidth = 760.0f;
	constexpr int32 GridSlots = 20;
	constexpr float SlotHeight = 38.0f;
	constexpr float SlotGap = 6.0f;

	/** The near-black of wells and the preview panels. */
	const FLinearColor PanelWell = FLinearColor::FromSRGBColor(FColor(0x0A, 0x0B, 0x0C));

	FLinearColor SrgbColour(const FColor& Colour, float Alpha = 1.0f)
	{
		return FLinearColor::FromSRGBColor(Colour).CopyWithNewOpacity(Alpha);
	}

	UOverlaySlot* AddFill(UOverlay* Overlay, UWidget* Child, const FMargin& Padding = FMargin())
	{
		UOverlaySlot* Slot = Overlay->AddChildToOverlay(Child);
		Slot->SetHorizontalAlignment(HAlign_Fill);
		Slot->SetVerticalAlignment(VAlign_Fill);
		Slot->SetPadding(Padding);
		return Slot;
	}

	/** A fill slot's share of the space. ApexUI::AddV/AddH fill evenly
	 * whatever weight they are given, so a split that is not even is set
	 * here. */
	void SetFillWeight(UVerticalBoxSlot* Slot, float Weight)
	{
		if (Slot)
		{
			FSlateChildSize Size(ESlateSizeRule::Fill);
			Size.Value = Weight;
			Slot->SetSize(Size);
		}
	}

	/** Where a widget was last drawn, in desktop space. */
	FBox2D RectOf(const UWidget* Widget)
	{
		const FGeometry& Geometry = Widget->GetCachedGeometry();
		const FVector2D Min = Geometry.GetAbsolutePosition();
		return FBox2D(Min, Min + Geometry.GetAbsoluteSize());
	}

	/** Space between two intervals; zero where they overlap. */
	float IntervalGap(float AMin, float AMax, float BMin, float BMax)
	{
		return FMath::Max(0.0f, FMath::Max(BMin - AMax, AMin - BMax));
	}

	/**
	 * The control to go to from Source in Direction: wholly on that side of
	 * it, nearest along the way, then nearest across it, so a move that lines
	 * up with nothing still lands on the closest thing that way.
	 */
	UWidget* NearestToward(EUINavigation Direction, const UWidget* Source, const TArray<UWidget*>& Candidates)
	{
		constexpr float Slack = 4.0f;
		const FBox2D From = RectOf(Source);
		const FVector2D FromCentre = From.GetCenter();

		UWidget* Best = nullptr;
		float BestScore = TNumericLimits<float>::Max();
		for (UWidget* Candidate : Candidates)
		{
			if (!Candidate || Candidate == Source || !ApexNav::CanFocus(Candidate))
			{
				continue;
			}
			const FBox2D To = RectOf(Candidate);
			const FVector2D ToCentre = To.GetCenter();
			float Along = 0.0f;
			float Across = 0.0f;
			float CentreAcross = 0.0f;
			switch (Direction)
			{
			case EUINavigation::Right:
				if (To.Min.X < From.Max.X - Slack) { continue; }
				Along = To.Min.X - From.Max.X;
				Across = IntervalGap(From.Min.Y, From.Max.Y, To.Min.Y, To.Max.Y);
				CentreAcross = FMath::Abs(ToCentre.Y - FromCentre.Y);
				break;
			case EUINavigation::Left:
				if (To.Max.X > From.Min.X + Slack) { continue; }
				Along = From.Min.X - To.Max.X;
				Across = IntervalGap(From.Min.Y, From.Max.Y, To.Min.Y, To.Max.Y);
				CentreAcross = FMath::Abs(ToCentre.Y - FromCentre.Y);
				break;
			case EUINavigation::Down:
				if (To.Min.Y < From.Max.Y - Slack) { continue; }
				Along = To.Min.Y - From.Max.Y;
				Across = IntervalGap(From.Min.X, From.Max.X, To.Min.X, To.Max.X);
				CentreAcross = FMath::Abs(ToCentre.X - FromCentre.X);
				break;
			case EUINavigation::Up:
				if (To.Max.Y > From.Min.Y + Slack) { continue; }
				Along = From.Min.Y - To.Max.Y;
				Across = IntervalGap(From.Min.X, From.Max.X, To.Min.X, To.Max.X);
				CentreAcross = FMath::Abs(ToCentre.X - FromCentre.X);
				break;
			default:
				return nullptr;
			}
			const float Score = FMath::Max(0.0f, Along) + 2.0f * Across + 0.05f * CentreAcross;
			if (Score < BestScore)
			{
				BestScore = Score;
				Best = Candidate;
			}
		}
		return Best;
	}
}

UApexSessionCreateWidget::UApexSessionCreateWidget(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	SetIsFocusable(true);
}

void UApexSessionCreateWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	BuildLayout();
}

void UApexSessionCreateWidget::NativeConstruct()
{
	Super::NativeConstruct();

	if (UApexNetSubsystem* Net = GetNet())
	{
		Net->OnLobbyStateUpdated.AddDynamic(this, &UApexSessionCreateWidget::HandleLobbyStateUpdated);
	}
}

void UApexSessionCreateWidget::NativeDestruct()
{
	if (UApexNetSubsystem* Net = GetNet())
	{
		Net->OnLobbyStateUpdated.RemoveDynamic(this, &UApexSessionCreateWidget::HandleLobbyStateUpdated);
	}
	Super::NativeDestruct();
}

void UApexSessionCreateWidget::OnScreenActivated()
{
	Super::OnScreenActivated();

	// A screenshot run can open either tab: -ApexCreateTab=1 is Conditions.
	int32 Tab = ActiveTab;
	if (FParse::Value(FCommandLine::Get(), TEXT("ApexCreateTab="), Tab))
	{
		SetActiveTab(Tab);
	}

	// Redraws the settings and the footer too.
	RefreshContent();
}

void UApexSessionCreateWidget::FocusDefault()
{
	if (!ApexNav::Focus(CreateButtonWidget) && !ApexNav::Focus(KindMultiplayerButton))
	{
		Super::FocusDefault();
	}
}

bool UApexSessionCreateWidget::HandleAccept()
{
	if (CreateButtonWidget && ApexNav::CanFocus(CreateButtonWidget))
	{
		HandleButtonActivated(CreateButtonWidget);
		return true;
	}
	return false;
}

void UApexSessionCreateWidget::GatherFocusables(TArray<UWidget*>& Out) const
{
	auto AddAll = [&Out](const auto& List)
	{
		for (const auto& Widget : List)
		{
			Out.Add(Widget);
		}
	};

	Out.Add(HomeButton);
	Out.Add(KindMultiplayerButton);
	Out.Add(KindSingleButton);
	Out.Add(ChangeTrackLink);
	Out.Add(ChangeCarLink);
	AddAll(TabButtons);

	if (ActiveTab == 0)
	{
		AddAll(ModeButtons);
		AddAll(LengthUnitButtons);
		Out.Add(LapsMinus);
		Out.Add(LapsPlus);
		const UApexMenuFlowSubsystem* Flow = GetFlow();
		AddAll(Flow && Flow->bCreateTimedRace ? DurationPresetButtons : LapPresetButtons);
		Out.Add(FieldMinus);
		Out.Add(FieldPlus);
		Out.Add(AiMinus);
		Out.Add(AiPlus);
		Out.Add(AiSkillMinus);
		Out.Add(AiSkillPlus);
		AddAll(AssistPresetButtons);
		AddAll(AssistButtons);
		AddAll(DamageButtons);
		AddAll(SetupButtons);
	}
	else
	{
		Out.Add(TimeOfDaySlider);
		AddAll(TimePresetButtons);
		AddAll(WeatherButtons);
		Out.Add(AirAutoButton);
		Out.Add(AirMinus);
		Out.Add(AirPlus);
		AddAll(WindButtons);
		AddAll(WindFromButtons);
	}

	Out.Add(CancelButton);
	Out.Add(CreateButtonWidget);
}

bool UApexSessionCreateWidget::HandleNavigation(EUINavigation Direction, UWidget* Source)
{
	if (ApexNav::IsSequential(Direction))
	{
		SetActiveTab(Direction == EUINavigation::Next ? 1 : 0);
		ApexNav::Focus(TabButtons.IsValidIndex(ActiveTab) ? TabButtons[ActiveTab].Get() : nullptr);
		return true;
	}

	if (!Source || Source == this)
	{
		return false;
	}

	TArray<UWidget*> Candidates;
	GatherFocusables(Candidates);
	if (UWidget* Target = NearestToward(Direction, Source, Candidates))
	{
		ApexNav::Focus(Target);
	}
	// Nothing that way: stay put rather than let Slate wander off-screen.
	return true;
}

void UApexSessionCreateWidget::SetActiveTab(int32 Tab)
{
	ActiveTab = FMath::Clamp(Tab, 0, 1);
	if (RaceTab)
	{
		RaceTab->SetVisibility(ActiveTab == 0 ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);
	}
	if (ConditionsTab)
	{
		ConditionsTab->SetVisibility(ActiveTab == 1 ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);
	}
	if (GridPreview)
	{
		GridPreview->SetVisibility(ActiveTab == 0 ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);
	}
	if (SkyPreview)
	{
		SkyPreview->SetVisibility(ActiveTab == 1 ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);
	}
	for (int32 Index = 0; Index < TabLines.Num(); ++Index)
	{
		if (TabLines[Index])
		{
			TabLines[Index]->SetBrush(ApexUI::MakeBrush(Index == ActiveTab ? ApexUI::Palette::Accent : FLinearColor::Transparent));
		}
		if (TabButtons.IsValidIndex(Index) && TabButtons[Index])
		{
			// The open tab's name in full white; RefreshSettings puts the
			// summaries back as badges.
			FApexButtonSpec Spec;
			Spec.Label = Index == 0 ? TEXT("Race") : TEXT("Conditions");
			Spec.Badge = TEXT(" ");
			Spec.Variant = EApexButtonVariant::Bare;
			Spec.LabelSize = 20.0f;
			Spec.Height = 56.0f;
			Spec.LabelColour = Index == ActiveTab ? ApexUI::Palette::TextPrimary : FLinearColor(0.0f, 0.0f, 0.0f, 0.0f);
			TabButtons[Index]->Setup(Spec);
		}
	}
	RefreshSettings();
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

UApexButtonWidget* UApexSessionCreateWidget::MakeButton(const FApexButtonSpec& Spec)
{
	UApexButtonWidget* Button = WidgetTree->ConstructWidget<UApexButtonWidget>();
	Button->Setup(Spec);
	Button->OnActivated.AddDynamic(this, &UApexSessionCreateWidget::HandleButtonActivated);
	return Button;
}

UWidget* UApexSessionCreateWidget::MakeStepper(
	UApexButtonWidget*& OutMinus, UTextBlock*& OutValue, UApexButtonWidget*& OutPlus,
	float PillSize, float ValueWidth, float ValueSize, UTextBlock** OutNote)
{
	auto MakePill = [this, PillSize](const TCHAR* Label)
	{
		FApexButtonSpec Spec;
		Spec.Label = Label;
		Spec.Variant = EApexButtonVariant::Ghost;
		Spec.bCentreLabel = true;
		Spec.LabelSize = PillSize * 0.42f;
		Spec.Height = PillSize;
		return MakeButton(Spec);
	};

	UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();
	OutMinus = MakePill(TEXT("−"));
	ApexUI::AddH(Row, ApexUI::MakeSized(*WidgetTree, OutMinus, PillSize, PillSize), FMargin(), VAlign_Fill);

	UVerticalBox* Lines = WidgetTree->ConstructWidget<UVerticalBox>();
	OutValue = ApexUI::MakeText(*WidgetTree, FString(), ApexUI::Font::Display(ValueSize), ApexUI::Palette::TextPrimary);
	OutValue->SetJustification(ETextJustify::Center);
	ApexUI::AddV(Lines, OutValue, FMargin(), HAlign_Center);
	if (OutNote)
	{
		*OutNote = ApexUI::MakeText(*WidgetTree, FString(), ApexUI::Font::Mono(8.0f, 60), ApexUI::Palette::TextMuted);
		(*OutNote)->SetJustification(ETextJustify::Center);
		ApexUI::AddV(Lines, *OutNote, FMargin(0.0f, 2.0f, 0.0f, 0.0f), HAlign_Center);
	}

	const bool bWell = ValueWidth < 0.0f || OutNote;
	UBorder* Cell = ApexUI::MakePanel(*WidgetTree, Lines, FMargin(4.0f, 0.0f),
		ApexUI::MakeBrush(bWell ? PanelWell : FLinearColor::Transparent));
	Cell->SetHorizontalAlignment(HAlign_Center);
	Cell->SetVerticalAlignment(VAlign_Center);
	if (ValueWidth < 0.0f)
	{
		ApexUI::AddH(Row, Cell, FMargin(6.0f, 0.0f), VAlign_Fill, 1.0f);
	}
	else
	{
		ApexUI::AddH(Row, ApexUI::MakeSized(*WidgetTree, Cell, ValueWidth, PillSize), FMargin(4.0f, 0.0f), VAlign_Fill);
	}

	OutPlus = MakePill(TEXT("+"));
	ApexUI::AddH(Row, ApexUI::MakeSized(*WidgetTree, OutPlus, PillSize, PillSize), FMargin(), VAlign_Fill);
	return Row;
}

UWidget* UApexSessionCreateWidget::MakeCaption(const FString& Label, UTextBlock** OutRight, UWidget* RightWidget)
{
	UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();
	ApexUI::AddH(Row, ApexUI::MakeLabel(*WidgetTree, Label));
	ApexUI::AddH(Row, WidgetTree->ConstructWidget<USpacer>(), FMargin(), VAlign_Center, 1.0f);
	if (OutRight)
	{
		*OutRight = ApexUI::MakeText(*WidgetTree, FString(), ApexUI::Font::Mono(10.0f, 100), ApexUI::Palette::TextSecondary);
		ApexUI::AddH(Row, *OutRight);
	}
	if (RightWidget)
	{
		ApexUI::AddH(Row, RightWidget, FMargin(12.0f, 0.0f, 0.0f, 0.0f));
	}
	return ApexUI::MakeSized(*WidgetTree, Row, -1.0f, 26.0f);
}

void UApexSessionCreateWidget::BuildLayout()
{
	FApexButtonSpec BackSpec;
	BackSpec.Label = TEXT("Home");
	BackSpec.KeyCap = TEXT("Esc");
	BackSpec.bKeyCapLeading = true;
	BackSpec.Sound = EApexUiSound::Back;
	BackSpec.Variant = EApexButtonVariant::Bare;
	BackSpec.LabelSize = 15.0f;
	HomeButton = MakeButton(BackSpec);

	UVerticalBox* Page = WidgetTree->ConstructWidget<UVerticalBox>();
	ApexUI::AddV(Page, ApexUI::MakeScreenHeader(*WidgetTree, HomeButton, TEXT("Create session"), BuildHeaderKinds()));

	UHorizontalBox* Columns = WidgetTree->ConstructWidget<UHorizontalBox>();
	ApexUI::AddH(Columns, BuildLeftColumn(), FMargin(), VAlign_Fill, 1.0f);
	ApexUI::AddH(Columns, ApexUI::MakeDivider(*WidgetTree, true), FMargin(), VAlign_Fill);
	ApexUI::AddH(Columns, ApexUI::MakeSized(*WidgetTree, BuildRightColumn(), RightColumnWidth, -1.0f), FMargin(), VAlign_Fill);

	ApexUI::AddV(Page, Columns, FMargin(), HAlign_Fill, 1.0f);
	ApexUI::AddV(Page, ApexUI::MakeDivider(*WidgetTree));
	ApexUI::AddV(Page, BuildFooter());

	WidgetTree->RootWidget = ApexUI::MakePanel(
		*WidgetTree,
		Page,
		FMargin(),
		ApexUI::MakeBrush(ApexUI::Palette::Background));

	SetActiveTab(0);
}

UWidget* UApexSessionCreateWidget::BuildHeaderKinds()
{
	auto MakeKind = [this](const TCHAR* Label)
	{
		FApexButtonSpec Spec;
		Spec.Label = Label;
		Spec.Variant = EApexButtonVariant::Panel;
		Spec.bCentreLabel = true;
		Spec.LabelSize = 16.0f;
		Spec.Height = 40.0f;
		return MakeButton(Spec);
	};

	KindMultiplayerButton = MakeKind(TEXT("Multiplayer"));
	KindSingleButton = MakeKind(TEXT("Single player"));

	UHorizontalBox* Tray = WidgetTree->ConstructWidget<UHorizontalBox>();
	ApexUI::AddH(Tray, ApexUI::MakeSized(*WidgetTree, KindMultiplayerButton, 170.0f, 40.0f));
	ApexUI::AddH(Tray, ApexUI::MakeSized(*WidgetTree, KindSingleButton, 170.0f, 40.0f), FMargin(3.0f, 0.0f, 0.0f, 0.0f));
	return ApexUI::MakePanel(*WidgetTree, Tray, FMargin(3.0f), ApexUI::MakeBrush(PanelWell, ApexUI::Palette::Border, 1.0f));
}

UWidget* UApexSessionCreateWidget::BuildLeftColumn()
{
	UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>();

	auto MakeCard = [this](UHorizontalBox*& OutBox)
	{
		OutBox = WidgetTree->ConstructWidget<UHorizontalBox>();
		return ApexUI::MakePanel(*WidgetTree, OutBox, FMargin(12.0f),
			ApexUI::MakeBrush(ApexUI::Palette::Surface, ApexUI::Palette::Border, 1.0f));
	};

	UHorizontalBox* Cards = WidgetTree->ConstructWidget<UHorizontalBox>();
	UHorizontalBox* TrackBox = nullptr;
	UHorizontalBox* CarBox = nullptr;
	ApexUI::AddH(Cards, MakeCard(TrackBox), FMargin(), VAlign_Fill, 1.0f);
	ApexUI::AddH(Cards, MakeCard(CarBox), FMargin(12.0f, 0.0f, 0.0f, 0.0f), VAlign_Fill, 1.0f);
	TrackSummaryBox = TrackBox;
	CarSummaryBox = CarBox;
	ApexUI::AddV(Column, Cards, FMargin(0.0f, 0.0f, 0.0f, 18.0f));

	UOverlay* Preview = WidgetTree->ConstructWidget<UOverlay>();
	GridPreview = BuildGridPreview();
	SkyPreview = BuildSkyPreview();
	AddFill(Preview, GridPreview);
	AddFill(Preview, SkyPreview);
	ApexUI::AddV(Column, Preview, FMargin(), HAlign_Fill, 1.0f);

	return ApexUI::MakePanel(
		*WidgetTree,
		Column,
		FMargin(ApexUI::Metrics::PageGutter, 22.0f, 28.0f, 22.0f),
		ApexUI::MakeBrush(FLinearColor::Transparent));
}

UWidget* UApexSessionCreateWidget::BuildGridPreview()
{
	UVerticalBox* Panel = WidgetTree->ConstructWidget<UVerticalBox>();

	UHorizontalBox* Head = WidgetTree->ConstructWidget<UHorizontalBox>();
	ApexUI::AddH(Head, ApexUI::MakeLabel(*WidgetTree, TEXT("Starting grid")));
	ApexUI::AddH(Head, WidgetTree->ConstructWidget<USpacer>(), FMargin(), VAlign_Center, 1.0f);
	GridHintText = ApexUI::MakeText(*WidgetTree, FString(), ApexUI::Font::Mono(10.0f, 80), ApexUI::Palette::TextSecondary);
	ApexUI::AddH(Head, GridHintText);
	ApexUI::AddV(Panel, Head, FMargin(20.0f, 14.0f));
	ApexUI::AddV(Panel, ApexUI::MakeDivider(*WidgetTree));

	UOverlay* Body = WidgetTree->ConstructWidget<UOverlay>();

	// The slots, two staggered columns behind a chequered start line, odd
	// positions on the left as a grid stands.
	UHorizontalBox* Slots = WidgetTree->ConstructWidget<UHorizontalBox>();
	{
		UVerticalBox* Line = WidgetTree->ConstructWidget<UVerticalBox>();
		for (int32 Block = 0; Block < 48; ++Block)
		{
			FLinearColor Colour = ApexUI::Palette::TextPrimary;
			Colour.A = Block % 2 == 0 ? 0.5f : 0.0f;
			ApexUI::AddV(Line, ApexUI::MakeSized(*WidgetTree,
				ApexUI::MakePanel(*WidgetTree, nullptr, FMargin(), ApexUI::MakeBrush(Colour)), 6.0f, 10.0f));
		}
		Line->SetClipping(EWidgetClipping::ClipToBounds);
		ApexUI::AddH(Slots, Line, FMargin(0.0f, 0.0f, 28.0f, 0.0f), VAlign_Fill);
	}

	UVerticalBox* Odd = WidgetTree->ConstructWidget<UVerticalBox>();
	UVerticalBox* Even = WidgetTree->ConstructWidget<UVerticalBox>();
	SlotFaces.Reset();
	SlotNumbers.Reset();
	SlotNames.Reset();
	SlotButtons.Reset();
	for (int32 Index = 0; Index < GridSlots; ++Index)
	{
		UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();
		UTextBlock* Number = ApexUI::MakeText(*WidgetTree, FString::Printf(TEXT("P%d"), Index + 1),
			ApexUI::Font::Mono(10.0f), ApexUI::Palette::TextMuted);
		ApexUI::AddH(Row, ApexUI::MakeSized(*WidgetTree, Number, 34.0f, -1.0f));
		UTextBlock* Name = ApexUI::MakeText(*WidgetTree, FString(), ApexUI::Font::Display(15.0f), ApexUI::Palette::TextSecondary);
		ApexUI::AddH(Row, Name, FMargin(), VAlign_Center, 1.0f);

		UBorder* Face = ApexUI::MakePanel(*WidgetTree, Row, FMargin(12.0f, 0.0f), ApexUI::MakeBrush(FLinearColor::Transparent));
		Face->SetVerticalAlignment(VAlign_Center);

		// A click on a slot sets the field to it. Mouse only: the steppers
		// beside are the keyboard's way, and twenty more stops would bury them.
		FApexButtonSpec HitSpec;
		HitSpec.Variant = EApexButtonVariant::Ghost;
		HitSpec.bOverlay = true;
		HitSpec.Height = SlotHeight;
		HitSpec.Sound = EApexUiSound::Adjust;
		UApexButtonWidget* Hit = MakeButton(HitSpec);
		Hit->SetIsFocusable(false);

		UOverlay* Cell = WidgetTree->ConstructWidget<UOverlay>();
		AddFill(Cell, Face);
		AddFill(Cell, Hit);

		const bool bLeft = Index % 2 == 0;
		ApexUI::AddV(bLeft ? Odd : Even, ApexUI::MakeSized(*WidgetTree, Cell, -1.0f, SlotHeight),
			FMargin(0.0f, 0.0f, 0.0f, SlotGap));

		SlotFaces.Add(Face);
		SlotNumbers.Add(Number);
		SlotNames.Add(Name);
		SlotButtons.Add(Hit);
	}
	ApexUI::AddH(Slots, Odd, FMargin(), VAlign_Top, 1.0f);
	ApexUI::AddH(Slots, Even, FMargin(22.0f, 22.0f, 0.0f, 0.0f), VAlign_Top, 1.0f);
	GridSlotsPanel = Slots;
	AddFill(Body, Slots, FMargin(30.0f, 16.0f, 24.0f, 12.0f));

	UVerticalBox* Solo = WidgetTree->ConstructWidget<UVerticalBox>();
	GridSoloTitle = ApexUI::MakeText(*WidgetTree, FString(), ApexUI::Font::Display(24.0f), ApexUI::Palette::TextPrimary);
	GridSoloTitle->SetJustification(ETextJustify::Center);
	ApexUI::AddV(Solo, GridSoloTitle, FMargin(), HAlign_Center);
	GridSoloText = ApexUI::MakeText(*WidgetTree, FString(), ApexUI::Font::Body(14.0f), ApexUI::Palette::TextSecondary);
	GridSoloText->SetJustification(ETextJustify::Center);
	ApexUI::AddV(Solo, GridSoloText, FMargin(0.0f, 8.0f, 0.0f, 0.0f), HAlign_Center);
	GridSoloPanel = Solo;
	UOverlaySlot* SoloSlot = Body->AddChildToOverlay(Solo);
	SoloSlot->SetHorizontalAlignment(HAlign_Center);
	SoloSlot->SetVerticalAlignment(VAlign_Center);

	Body->SetClipping(EWidgetClipping::ClipToBounds);
	ApexUI::AddV(Panel, Body, FMargin(), HAlign_Fill, 1.0f);
	ApexUI::AddV(Panel, ApexUI::MakeDivider(*WidgetTree));

	// The legend.
	UHorizontalBox* Legend = WidgetTree->ConstructWidget<UHorizontalBox>();
	auto AddKey = [this, Legend](const TCHAR* Label, const FSlateBrush& Swatch)
	{
		ApexUI::AddH(Legend, ApexUI::MakeSized(*WidgetTree, ApexUI::MakePanel(*WidgetTree, nullptr, FMargin(), Swatch), 10.0f, 10.0f));
		ApexUI::AddH(Legend, ApexUI::MakeText(*WidgetTree, Label, ApexUI::Font::Mono(9.0f, 80), ApexUI::Palette::TextSecondary),
			FMargin(8.0f, 0.0f, 22.0f, 0.0f));
	};
	AddKey(TEXT("YOU"), ApexUI::MakeBrush(ApexUI::Palette::Accent));
	AddKey(TEXT("AI"), ApexUI::MakeBrush(ApexUI::Palette::SurfaceHover, ApexUI::Palette::Border, 1.0f));
	AddKey(TEXT("OPEN SEAT"), ApexUI::MakeBrush(FLinearColor::Transparent, ApexUI::Palette::TextMuted, 1.0f));
	ApexUI::AddH(Legend, WidgetTree->ConstructWidget<USpacer>(), FMargin(), VAlign_Center, 1.0f);
	ApexUI::AddH(Legend, ApexUI::MakeText(*WidgetTree, TEXT("CLICK A SLOT TO SET THE FIELD"),
		ApexUI::Font::Mono(9.0f, 80), ApexUI::Palette::TextMuted));
	ApexUI::AddV(Panel, Legend, FMargin(20.0f, 12.0f));

	return ApexUI::MakePanel(*WidgetTree, Panel, FMargin(), ApexUI::MakeBrush(PanelWell, ApexUI::Palette::Border, 1.0f));
}

UWidget* UApexSessionCreateWidget::BuildSkyPreview()
{
	UOverlay* Sky = WidgetTree->ConstructWidget<UOverlay>();
	Sky->SetClipping(EWidgetClipping::ClipToBounds);

	// The sky: the horizon's colour, the zenith's laid over it fading out.
	SkyBase = ApexUI::MakePanel(*WidgetTree, nullptr, FMargin(), ApexUI::MakeBrush(FLinearColor::Black));
	AddFill(Sky, SkyBase);

	FSlateBrush Ramp;
	Ramp.SetResourceObject(GradientTexture());
	Ramp.DrawAs = ESlateBrushDrawType::Image;
	Ramp.ImageSize = FVector2D(1.0f, 64.0f);
	SkyTop = WidgetTree->ConstructWidget<UImage>();
	SkyTop->SetBrush(Ramp);
	AddFill(Sky, SkyTop);

	// The sun, with a glow round it, placed by anchor on a canvas.
	UCanvasPanel* SunCanvas = WidgetTree->ConstructWidget<UCanvasPanel>();
	SunGlow = ApexUI::MakePanel(*WidgetTree, nullptr, FMargin(), ApexUI::MakeBrush(FLinearColor::White, FLinearColor::Transparent, 0.0f, 80.0f));
	SunGlowSlot = SunCanvas->AddChildToCanvas(SunGlow);
	SunGlowSlot->SetAlignment(FVector2D(0.5f, 0.5f));
	SunGlowSlot->SetSize(FVector2D(160.0f, 160.0f));
	SunDisc = ApexUI::MakePanel(*WidgetTree, nullptr, FMargin(), ApexUI::MakeBrush(FLinearColor::White, FLinearColor::Transparent, 0.0f, 30.0f));
	SunDiscSlot = SunCanvas->AddChildToCanvas(SunDisc);
	SunDiscSlot->SetAlignment(FVector2D(0.5f, 0.5f));
	SunDiscSlot->SetSize(FVector2D(60.0f, 60.0f));
	AddFill(Sky, SunCanvas);

	CloudVeil = ApexUI::MakePanel(*WidgetTree, nullptr, FMargin(), ApexUI::MakeBrush(FLinearColor::Transparent));
	AddFill(Sky, CloudVeil);

	FSlateBrush Hatch;
	Hatch.SetResourceObject(RainTexture());
	Hatch.DrawAs = ESlateBrushDrawType::Image;
	Hatch.Tiling = ESlateBrushTileType::Both;
	Hatch.ImageSize = FVector2D(12.0f, 48.0f);
	RainVeil = WidgetTree->ConstructWidget<UImage>();
	RainVeil->SetBrush(Hatch);
	AddFill(Sky, RainVeil);

	// The ground: the bottom quarter, darkening down, under a faint horizon.
	{
		UVerticalBox* Ground = WidgetTree->ConstructWidget<UVerticalBox>();
		SetFillWeight(ApexUI::AddV(Ground, WidgetTree->ConstructWidget<USpacer>(), FMargin(), HAlign_Fill, 1.0f), 0.74f);
		FLinearColor Horizon = ApexUI::Palette::TextPrimary;
		Horizon.A = 0.18f;
		ApexUI::AddV(Ground, ApexUI::MakeSized(*WidgetTree,
			ApexUI::MakePanel(*WidgetTree, nullptr, FMargin(), ApexUI::MakeBrush(Horizon)), -1.0f, 2.0f));

		FSlateBrush Down = Ramp;
		Down.Mirroring = ESlateBrushMirrorType::Vertical;
		UImage* Dark = WidgetTree->ConstructWidget<UImage>();
		Dark->SetBrush(Down);
		Dark->SetColorAndOpacity(PanelWell);
		UBorder* Earth = ApexUI::MakePanel(*WidgetTree, Dark, FMargin(),
			ApexUI::MakeBrush(SrgbColour(FColor(12, 14, 15), 0.7f)));
		SetFillWeight(ApexUI::AddV(Ground, Earth, FMargin(), HAlign_Fill, 1.0f), 0.26f);
		AddFill(Sky, Ground);
	}

	// What it all comes to, over the picture.
	UVerticalBox* Text = WidgetTree->ConstructWidget<UVerticalBox>();
	SkyClockText = ApexUI::MakeText(*WidgetTree, FString(), ApexUI::Font::Display(76.0f), ApexUI::Palette::TextPrimary);
	SkyClockText->SetShadowOffset(FVector2D(0.0f, 2.0f));
	SkyClockText->SetShadowColorAndOpacity(FLinearColor(0.0f, 0.0f, 0.0f, 0.35f));
	ApexUI::AddV(Text, SkyClockText);
	SkyPhaseText = ApexUI::MakeText(*WidgetTree, FString(), ApexUI::Font::Mono(11.0f, 180), ApexUI::Palette::TextPrimary);
	ApexUI::AddV(Text, SkyPhaseText, FMargin(2.0f, 0.0f, 0.0f, 0.0f));
	ApexUI::AddV(Text, WidgetTree->ConstructWidget<USpacer>(), FMargin(), HAlign_Fill, 1.0f);

	UHorizontalBox* Tiles = WidgetTree->ConstructWidget<UHorizontalBox>();
	auto MakeTile = [this](const TCHAR* Label, UTextBlock*& OutValue, UWidget* Extra = nullptr)
	{
		UVerticalBox* Lines = WidgetTree->ConstructWidget<UVerticalBox>();
		ApexUI::AddV(Lines, ApexUI::MakeText(*WidgetTree, Label, ApexUI::Font::Mono(9.0f, 160), ApexUI::Palette::TextSecondary));
		OutValue = ApexUI::MakeText(*WidgetTree, FString(), ApexUI::Font::Display(24.0f), ApexUI::Palette::TextPrimary);
		ApexUI::AddV(Lines, OutValue, FMargin(0.0f, 4.0f, 0.0f, 0.0f));

		UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();
		ApexUI::AddH(Row, Lines, FMargin(), VAlign_Center, 1.0f);
		if (Extra)
		{
			ApexUI::AddH(Row, Extra);
		}
		return ApexUI::MakePanel(*WidgetTree, Row, FMargin(14.0f, 12.0f), ApexUI::MakeBrush(SrgbColour(FColor(10, 11, 12), 0.7f)));
	};

	// The wind tile's arrow: a vane pointing into the wind, like the dial's.
	{
		UVerticalBox* Vane = WidgetTree->ConstructWidget<UVerticalBox>();
		UBorder* Head = ApexUI::MakePanel(*WidgetTree, nullptr, FMargin(), ApexUI::MakeBrush(ApexUI::Palette::TextPrimary));
		UWidget* HeadBox = ApexUI::MakeSized(*WidgetTree, Head, 8.0f, 8.0f);
		HeadBox->SetRenderTransformAngle(45.0f);
		ApexUI::AddV(Vane, HeadBox, FMargin(0.0f, 0.0f, 0.0f, -4.0f), HAlign_Center);
		ApexUI::AddV(Vane, ApexUI::MakeSized(*WidgetTree,
			ApexUI::MakePanel(*WidgetTree, nullptr, FMargin(), ApexUI::MakeBrush(ApexUI::Palette::TextPrimary)), 2.0f, 14.0f),
			FMargin(), HAlign_Center);
		UBorder* Ring = ApexUI::MakePanel(*WidgetTree, Vane, FMargin(),
			ApexUI::MakeBrush(FLinearColor::Transparent, ApexUI::Palette::TextMuted, 1.0f, 17.0f));
		Ring->SetHorizontalAlignment(HAlign_Center);
		Ring->SetVerticalAlignment(VAlign_Center);
		// Turning the ring turns the vane in it; hiding it hides both.
		SkyWindArrow = Ring;
		Ring->SetRenderTransformPivot(FVector2D(0.5f, 0.5f));

		UTextBlock* Air = nullptr;
		UTextBlock* Track = nullptr;
		UTextBlock* Grip = nullptr;
		UTextBlock* Wind = nullptr;
		ApexUI::AddH(Tiles, MakeTile(TEXT("AIR"), Air), FMargin(), VAlign_Fill, 1.0f);
		ApexUI::AddH(Tiles, MakeTile(TEXT("TRACK"), Track), FMargin(2.0f, 0.0f, 0.0f, 0.0f), VAlign_Fill, 1.0f);
		ApexUI::AddH(Tiles, MakeTile(TEXT("GRIP"), Grip), FMargin(2.0f, 0.0f, 0.0f, 0.0f), VAlign_Fill, 1.0f);
		ApexUI::AddH(Tiles, MakeTile(TEXT("WIND"), Wind, ApexUI::MakeSized(*WidgetTree, Ring, 34.0f, 34.0f)),
			FMargin(2.0f, 0.0f, 0.0f, 0.0f), VAlign_Fill, 1.0f);
		SkyAirText = Air;
		SkyTrackText = Track;
		SkyGripText = Grip;
		SkyWindText = Wind;
	}
	ApexUI::AddV(Text, Tiles);
	AddFill(Sky, Text, FMargin(24.0f, 18.0f, 24.0f, 20.0f));

	return ApexUI::MakePanel(*WidgetTree, Sky, FMargin(), ApexUI::MakeBrush(PanelWell, ApexUI::Palette::Border, 1.0f));
}

UWidget* UApexSessionCreateWidget::BuildRightColumn()
{
	UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>();

	UHorizontalBox* Strip = WidgetTree->ConstructWidget<UHorizontalBox>();
	TabButtons.Reset();
	TabLines.Reset();
	for (int32 Tab = 0; Tab < 2; ++Tab)
	{
		FApexButtonSpec Spec;
		Spec.Label = Tab == 0 ? TEXT("Race") : TEXT("Conditions");
		Spec.Badge = TEXT(" ");
		Spec.Variant = EApexButtonVariant::Bare;
		Spec.LabelSize = 20.0f;
		Spec.Height = 56.0f;
		UApexButtonWidget* Button = MakeButton(Spec);

		UBorder* Line = ApexUI::MakePanel(*WidgetTree, nullptr, FMargin(), ApexUI::MakeBrush(FLinearColor::Transparent));
		UVerticalBox* Stack = WidgetTree->ConstructWidget<UVerticalBox>();
		ApexUI::AddV(Stack, Button, FMargin(18.0f, 0.0f));
		ApexUI::AddV(Stack, ApexUI::MakeSized(*WidgetTree, Line, -1.0f, 3.0f));
		ApexUI::AddH(Strip, Stack, FMargin(Tab == 0 ? 0.0f : 6.0f, 0.0f, 0.0f, 0.0f), VAlign_Bottom);

		TabButtons.Add(Button);
		TabLines.Add(Line);
	}
	ApexUI::AddV(Column, ApexUI::MakeSized(*WidgetTree, Strip, -1.0f, 62.0f), FMargin(14.0f, 0.0f, ApexUI::Metrics::PageGutter, 0.0f));
	ApexUI::AddV(Column, ApexUI::MakeDivider(*WidgetTree));

	UOverlay* Body = WidgetTree->ConstructWidget<UOverlay>();
	RaceTab = BuildRaceTab();
	ConditionsTab = BuildConditionsTab();
	AddFill(Body, RaceTab);
	AddFill(Body, ConditionsTab);
	ApexUI::AddV(Column, Body, FMargin(32.0f, 22.0f, ApexUI::Metrics::PageGutter, 22.0f), HAlign_Fill, 1.0f);

	return Column;
}

UWidget* UApexSessionCreateWidget::BuildRaceTab()
{
	UVerticalBox* Tab = WidgetTree->ConstructWidget<UVerticalBox>();

	// --- Mode -------------------------------------------------------------------
	ApexUI::AddV(Tab, MakeCaption(TEXT("Mode")), FMargin(0.0f, 0.0f, 0.0f, 8.0f));
	UHorizontalBox* Modes = WidgetTree->ConstructWidget<UHorizontalBox>();
	ModeButtons.Reset();
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(ModeOptions); ++Index)
	{
		FApexButtonSpec Spec;
		Spec.Label = ModeOptions[Index].Label;
		Spec.SubLabel = ModeOptions[Index].Description;
		Spec.Variant = EApexButtonVariant::Panel;
		Spec.LabelSize = 18.0f;
		Spec.Height = 76.0f;
		UApexButtonWidget* Button = MakeButton(Spec);
		ModeButtons.Add(Button);
		ApexUI::AddH(Modes, Button, FMargin(Index == 0 ? 0.0f : 6.0f, 0.0f, 0.0f, 0.0f), VAlign_Fill, 1.0f);
	}
	ApexUI::AddV(Tab, Modes, FMargin(0.0f, 0.0f, 0.0f, 22.0f));

	// --- Race length --------------------------------------------------------------
	{
		UVerticalBox* Section = WidgetTree->ConstructWidget<UVerticalBox>();
		UTextBlock* Info = nullptr;
		ApexUI::AddV(Section, MakeCaption(TEXT("Race length"), &Info), FMargin(0.0f, 0.0f, 0.0f, 8.0f));
		LengthInfoText = Info;

		UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();

		// Laps or a time: the stepper and the presets follow the pick.
		LengthUnitButtons.Reset();
		for (const TCHAR* Unit : { TEXT("Laps"), TEXT("Time") })
		{
			FApexButtonSpec Spec;
			Spec.Label = Unit;
			Spec.Variant = EApexButtonVariant::Panel;
			Spec.bCentreLabel = true;
			Spec.LabelSize = 16.0f;
			Spec.Height = 62.0f;
			UApexButtonWidget* Button = MakeButton(Spec);
			ApexUI::AddH(Row, ApexUI::MakeSized(*WidgetTree, Button, 84.0f, 62.0f),
				FMargin(LengthUnitButtons.Num() == 0 ? 0.0f : 6.0f, 0.0f, 0.0f, 0.0f), VAlign_Fill);
			LengthUnitButtons.Add(Button);
		}

		UApexButtonWidget* Minus = nullptr;
		UApexButtonWidget* Plus = nullptr;
		UTextBlock* Value = nullptr;
		ApexUI::AddH(Row, MakeStepper(Minus, Value, Plus, 62.0f, 130.0f, 40.0f), FMargin(12.0f, 0.0f, 0.0f, 0.0f), VAlign_Fill);
		LapsMinus = Minus;
		LapsPlus = Plus;
		LapsValueText = Value;

		LapPresetButtons.Reset();
		for (int32 Index = 0; Index < UE_ARRAY_COUNT(LapPresets); ++Index)
		{
			FApexButtonSpec Spec;
			Spec.Label = LapPresets[Index].Label;
			Spec.SubLabel = FString::Printf(TEXT("%d laps"), LapPresets[Index].Laps);
			Spec.Variant = EApexButtonVariant::Panel;
			Spec.bCentreLabel = true;
			Spec.LabelSize = 16.0f;
			Spec.Height = 62.0f;
			UApexButtonWidget* Button = MakeButton(Spec);
			LapPresetButtons.Add(Button);
			ApexUI::AddH(Row, Button, FMargin(Index == 0 ? 12.0f : 6.0f, 0.0f, 0.0f, 0.0f), VAlign_Fill, 1.0f);
		}
		DurationPresetButtons.Reset();
		for (int32 Index = 0; Index < UE_ARRAY_COUNT(DurationPresets); ++Index)
		{
			FApexButtonSpec Spec;
			Spec.Label = DurationPresets[Index].Label;
			Spec.SubLabel = ApexRaceLength::Describe(DurationPresets[Index].Minutes * 60);
			Spec.Variant = EApexButtonVariant::Panel;
			Spec.bCentreLabel = true;
			Spec.LabelSize = 16.0f;
			Spec.Height = 62.0f;
			UApexButtonWidget* Button = MakeButton(Spec);
			DurationPresetButtons.Add(Button);
			ApexUI::AddH(Row, Button, FMargin(Index == 0 ? 12.0f : 6.0f, 0.0f, 0.0f, 0.0f), VAlign_Fill, 1.0f);
		}
		ApexUI::AddV(Section, ApexUI::MakeSized(*WidgetTree, Row, -1.0f, 62.0f));
		LengthSection = Section;
		ApexUI::AddV(Tab, Section, FMargin(0.0f, 0.0f, 0.0f, 22.0f));
	}

	// --- Field --------------------------------------------------------------------
	{
		UVerticalBox* Section = WidgetTree->ConstructWidget<UVerticalBox>();
		UTextBlock* Info = nullptr;
		ApexUI::AddV(Section, MakeCaption(TEXT("Field"), &Info), FMargin(0.0f, 0.0f, 0.0f, 8.0f));
		FieldInfoText = Info;

		auto MakeCard = [this](const TCHAR* Label, UTextBlock*& OutLabel, UApexButtonWidget*& OutMinus,
			UTextBlock*& OutValue, UApexButtonWidget*& OutPlus)
		{
			UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();
			OutLabel = ApexUI::MakeText(*WidgetTree, Label, ApexUI::Font::Display(17.0f), ApexUI::Palette::TextPrimary);
			ApexUI::AddH(Row, OutLabel, FMargin(), VAlign_Center, 1.0f);
			ApexUI::AddH(Row, MakeStepper(OutMinus, OutValue, OutPlus, 42.0f, 48.0f, 24.0f));
			return ApexUI::MakePanel(*WidgetTree, Row, FMargin(16.0f, 6.0f, 6.0f, 6.0f),
				ApexUI::MakeBrush(ApexUI::Palette::Surface, ApexUI::Palette::Border, 1.0f));
		};

		UHorizontalBox* Cards = WidgetTree->ConstructWidget<UHorizontalBox>();
		UTextBlock* FieldLabel = nullptr;
		UTextBlock* AiLabel = nullptr;
		UTextBlock* SkillLabel = nullptr;
		UApexButtonWidget* SMinus = nullptr;
		UApexButtonWidget* SPlus = nullptr;
		UTextBlock* SValue = nullptr;
		UApexButtonWidget* FMinus = nullptr;
		UApexButtonWidget* FPlus = nullptr;
		UTextBlock* FValue = nullptr;
		UApexButtonWidget* AMinus = nullptr;
		UApexButtonWidget* APlus = nullptr;
		UTextBlock* AValue = nullptr;
		ApexUI::AddH(Cards, MakeCard(TEXT("Grid size"), FieldLabel, FMinus, FValue, FPlus), FMargin(), VAlign_Fill, 1.0f);
		ApexUI::AddH(Cards, MakeCard(TEXT("AI drivers"), AiLabel, AMinus, AValue, APlus), FMargin(10.0f, 0.0f, 0.0f, 0.0f), VAlign_Fill, 1.0f);
		FieldLabelText = FieldLabel;
		FieldMinus = FMinus;
		FieldPlus = FPlus;
		FieldValueText = FValue;
		AiMinus = AMinus;
		AiPlus = APlus;
		AiValueText = AValue;
		ApexUI::AddV(Section, Cards);

		// A rule of the session, like the damage: the server seats the
		// whole field within a few points of this level. A row of its own:
		// a third card beside the counts squeezed every label into its
		// stepper.
		ApexUI::AddV(Section, MakeCard(TEXT("AI level"), SkillLabel, SMinus, SValue, SPlus), FMargin(0.0f, 6.0f, 0.0f, 0.0f));
		AiSkillLabelText = SkillLabel;
		AiSkillMinus = SMinus;
		AiSkillPlus = SPlus;
		AiSkillValueText = SValue;
		FieldSection = Section;
		ApexUI::AddV(Tab, Section, FMargin(0.0f, 0.0f, 0.0f, 22.0f));
	}

	// --- Allowed assists ----------------------------------------------------------
	// Which aids the drivers may run. The server forces a disallowed aid off for
	// everyone in the session, so a toggle here is a rule, not a suggestion; the
	// Assists settings page shows the lock to whoever joins.
	{
		UHorizontalBox* Presets = WidgetTree->ConstructWidget<UHorizontalBox>();
		AssistPresetButtons.Reset();
		for (int32 Index = 0; Index < static_cast<int32>(ApexCreateSession::EAssistPreset::Count); ++Index)
		{
			FApexButtonSpec Spec;
			Spec.Label = FString(ApexCreateSession::PresetName(static_cast<ApexCreateSession::EAssistPreset>(Index))).ToUpper();
			Spec.Variant = EApexButtonVariant::Ghost;
			Spec.bCentreLabel = true;
			Spec.LabelSize = 12.0f;
			Spec.Height = 26.0f;
			UApexButtonWidget* Button = MakeButton(Spec);
			AssistPresetButtons.Add(Button);
			ApexUI::AddH(Presets, ApexUI::MakeSized(*WidgetTree, Button, 76.0f, 26.0f), FMargin(Index == 0 ? 0.0f : 4.0f, 0.0f, 0.0f, 0.0f));
		}
		ApexUI::AddV(Tab, MakeCaption(TEXT("Assists allowed"), nullptr, Presets), FMargin(0.0f, 0.0f, 0.0f, 8.0f));

		static const TCHAR* AssistLabels[] = {
			TEXT("ABS"), TEXT("Traction ctrl"), TEXT("Auto gears"), TEXT("Steering aid"), TEXT("Racing line"),
		};
		static_assert(UE_ARRAY_COUNT(AssistLabels) == static_cast<int32>(EApexAssistChip::Count), "one label per chip");

		// Three to a row, the short last row keeping the thirds.
		AssistButtons.Reset();
		UVerticalBox* Rows = WidgetTree->ConstructWidget<UVerticalBox>();
		constexpr int32 ChipCount = static_cast<int32>(EApexAssistChip::Count);
		for (int32 First = 0; First < ChipCount; First += 3)
		{
			UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();
			for (int32 Column = 0; Column < 3; ++Column)
			{
				const FMargin Gap(Column == 0 ? 0.0f : 6.0f, 0.0f, 0.0f, 0.0f);
				if (First + Column >= ChipCount)
				{
					ApexUI::AddH(Row, WidgetTree->ConstructWidget<USpacer>(), Gap, VAlign_Fill, 1.0f);
					continue;
				}
				FApexButtonSpec Spec;
				Spec.Label = AssistLabels[First + Column];
				Spec.Badge = TEXT("On");
				Spec.Variant = EApexButtonVariant::Panel;
				Spec.LabelSize = 16.0f;
				Spec.Height = 46.0f;
				UApexButtonWidget* Button = MakeButton(Spec);
				AssistButtons.Add(Button);
				ApexUI::AddH(Row, Button, Gap, VAlign_Fill, 1.0f);
			}
			ApexUI::AddV(Rows, Row, FMargin(0.0f, First == 0 ? 0.0f : 6.0f, 0.0f, 0.0f));
		}
		ApexUI::AddV(Tab, Rows);
	}

	// --- Damage ---------------------------------------------------------------------
	// A rule of the session, not a driver's: the server scales every hit,
	// overheating second and missed shift by it for every car, AI included.
	{
		UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();
		DamageButtons.Reset();
		for (int32 Index = 0; Index < UE_ARRAY_COUNT(DamageOptions); ++Index)
		{
			FApexButtonSpec Spec;
			Spec.Label = DamageOptions[Index].Label;
			Spec.Badge = DamageOptions[Index].Description;
			Spec.Variant = EApexButtonVariant::Panel;
			Spec.LabelSize = 16.0f;
			Spec.Height = 46.0f;
			UApexButtonWidget* Button = MakeButton(Spec);
			DamageButtons.Add(Button);
			ApexUI::AddH(Row, Button, FMargin(Index == 0 ? 0.0f : 6.0f, 0.0f, 0.0f, 0.0f), VAlign_Fill, 1.0f);
		}
		UTextBlock* Info = nullptr;
		ApexUI::AddV(Tab, MakeCaption(TEXT("Damage"), &Info), FMargin(0.0f, 22.0f, 0.0f, 8.0f));
		Info->SetText(FText::FromString(TEXT("EVERY CAR, AI INCLUDED")));
		ApexUI::AddV(Tab, Row);
	}

	// --- Car setup ------------------------------------------------------------------
	// The setups saved in the hotlap garage for this car. One working setup
	// goes to the server on joining any session, so picking one here loads
	// it, as the garage's Load does; the chips are built in RefreshSetups.
	{
		UTextBlock* Info = nullptr;
		ApexUI::AddV(Tab, MakeCaption(TEXT("Car setup"), &Info), FMargin(0.0f, 22.0f, 0.0f, 8.0f));
		SetupInfoText = Info;
		SetupRows = WidgetTree->ConstructWidget<UVerticalBox>();
		ApexUI::AddV(Tab, SetupRows);
	}

	return Tab;
}

UWidget* UApexSessionCreateWidget::BuildConditionsTab()
{
	UVerticalBox* Tab = WidgetTree->ConstructWidget<UVerticalBox>();

	// --- Time of day ----------------------------------------------------------
	// The clock only moves the sun, but a night race is a night race for the
	// whole field, and the sun warms the asphalt.
	{
		UTextBlock* Clock = nullptr;
		ApexUI::AddV(Tab, MakeCaption(TEXT("Time of day"), &Clock), FMargin(0.0f, 0.0f, 0.0f, 6.0f));
		TimeOfDayValue = Clock;
		TimeOfDayValue->SetColorAndOpacity(FSlateColor(ApexUI::Palette::TextPrimary));

		USlider* Slider = nullptr;
		UProgressBar* Fill = nullptr;
		ApexUI::AddV(Tab, ApexUI::MakeSliderTrack(*WidgetTree, Slider, Fill, 26.0f), FMargin(0.0f, 0.0f, 0.0f, 10.0f));
		TimeOfDaySlider = Slider;
		TimeOfDayFill = Fill;
		TimeOfDaySlider->OnValueChanged.AddDynamic(this, &UApexSessionCreateWidget::HandleTimeOfDayChanged);
		TimeOfDaySlider->SetStepSize(1.0f / TimeOfDaySteps);

		UHorizontalBox* Presets = WidgetTree->ConstructWidget<UHorizontalBox>();
		TimePresetButtons.Reset();
		for (int32 Index = 0; Index < UE_ARRAY_COUNT(TimePresets); ++Index)
		{
			FApexSessionConditions At;
			At.TimeOfDayMinutes = TimePresets[Index].Minutes;
			FApexButtonSpec Spec;
			Spec.Label = TimePresets[Index].Label;
			Spec.Badge = At.ClockText();
			Spec.Variant = EApexButtonVariant::Panel;
			Spec.LabelSize = 16.0f;
			Spec.Height = 42.0f;
			UApexButtonWidget* Button = MakeButton(Spec);
			TimePresetButtons.Add(Button);
			ApexUI::AddH(Presets, Button, FMargin(Index == 0 ? 0.0f : 6.0f, 0.0f, 0.0f, 0.0f), VAlign_Fill, 1.0f);
		}
		ApexUI::AddV(Tab, Presets, FMargin(0.0f, 0.0f, 0.0f, 22.0f));
	}

	// --- Weather ----------------------------------------------------------------
	// A rule like the assists: the server takes the rain's grip off the track
	// for everyone.
	{
		ApexUI::AddV(Tab, MakeCaption(TEXT("Weather")), FMargin(0.0f, 0.0f, 0.0f, 8.0f));

		FSlateBrush Ramp;
		Ramp.SetResourceObject(GradientTexture());
		Ramp.DrawAs = ESlateBrushDrawType::Image;
		Ramp.ImageSize = FVector2D(1.0f, 64.0f);
		FSlateBrush Hatch;
		Hatch.SetResourceObject(RainTexture());
		Hatch.DrawAs = ESlateBrushDrawType::Image;
		Hatch.Tiling = ESlateBrushTileType::Both;
		Hatch.ImageSize = FVector2D(12.0f, 48.0f);
		FSlateBrush Shade = Ramp;
		Shade.Mirroring = ESlateBrushMirrorType::Vertical;

		UHorizontalBox* Tiles = WidgetTree->ConstructWidget<UHorizontalBox>();
		WeatherButtons.Reset();
		for (int32 Index = 0; Index < FApexSessionConditions::WeatherCount; ++Index)
		{
			const EApexWeather Weather = static_cast<EApexWeather>(Index);
			const ApexCreateSession::FWeatherLook Look = ApexCreateSession::WeatherLook(Weather);

			UOverlay* Tile = WidgetTree->ConstructWidget<UOverlay>();
			Tile->SetClipping(EWidgetClipping::ClipToBounds);
			AddFill(Tile, ApexUI::MakePanel(*WidgetTree, nullptr, FMargin(), ApexUI::MakeBrush(SrgbColour(Look.Bottom))));
			UImage* Top = WidgetTree->ConstructWidget<UImage>();
			Top->SetBrush(Ramp);
			Top->SetColorAndOpacity(SrgbColour(Look.Top));
			AddFill(Tile, Top);
			if (Look.Rain > 0.0f)
			{
				UImage* Drops = WidgetTree->ConstructWidget<UImage>();
				Drops->SetBrush(Hatch);
				Drops->SetColorAndOpacity(FLinearColor(1.0f, 1.0f, 1.0f, Look.Rain));
				AddFill(Tile, Drops);
			}

			UVerticalBox* Caption = WidgetTree->ConstructWidget<UVerticalBox>();
			ApexUI::AddV(Caption, ApexUI::MakeText(*WidgetTree, FApexSessionConditions::WeatherLabel(Weather),
				ApexUI::Font::Display(16.0f), ApexUI::Palette::TextPrimary));
			ApexUI::AddV(Caption, ApexUI::MakeText(*WidgetTree, FString::Printf(TEXT("GRIP %d%%"), ApexCreateSession::GripPercent(Weather)),
				ApexUI::Font::Mono(8.0f, 60), ApexUI::Palette::TextSecondary), FMargin(0.0f, 1.0f, 0.0f, 0.0f));
			UOverlay* Strip = WidgetTree->ConstructWidget<UOverlay>();
			UImage* Dim = WidgetTree->ConstructWidget<UImage>();
			Dim->SetBrush(Shade);
			Dim->SetColorAndOpacity(SrgbColour(FColor(10, 11, 12), 0.85f));
			AddFill(Strip, Dim);
			AddFill(Strip, Caption, FMargin(10.0f, 14.0f, 8.0f, 8.0f));
			UVerticalBox* Lower = WidgetTree->ConstructWidget<UVerticalBox>();
			ApexUI::AddV(Lower, WidgetTree->ConstructWidget<USpacer>(), FMargin(), HAlign_Fill, 1.0f);
			ApexUI::AddV(Lower, Strip);
			AddFill(Tile, Lower);

			FApexButtonSpec Spec;
			Spec.Variant = EApexButtonVariant::Ghost;
			Spec.bOverlay = true;
			Spec.Height = 96.0f;
			UApexButtonWidget* Button = MakeButton(Spec);
			AddFill(Tile, Button);
			WeatherButtons.Add(Button);

			UBorder* Frame = ApexUI::MakePanel(*WidgetTree, Tile, FMargin(1.0f),
				ApexUI::MakeBrush(FLinearColor::Transparent, ApexUI::Palette::Border, 1.0f));
			ApexUI::AddH(Tiles, ApexUI::MakeSized(*WidgetTree, Frame, -1.0f, 96.0f),
				FMargin(Index == 0 ? 0.0f : 6.0f, 0.0f, 0.0f, 0.0f), VAlign_Fill, 1.0f);
		}
		ApexUI::AddV(Tab, Tiles, FMargin(0.0f, 0.0f, 0.0f, 22.0f));
	}

	// --- The air: temperature and wind -------------------------------------------
	// Both start as "from the weather", which the server works out and names;
	// the preview shows what it will name.
	UVerticalBox* Air = WidgetTree->ConstructWidget<UVerticalBox>();
	{
		UVerticalBox* Temperature = WidgetTree->ConstructWidget<UVerticalBox>();
		FApexButtonSpec AutoSpec;
		AutoSpec.Label = TEXT("AUTO");
		AutoSpec.Variant = EApexButtonVariant::Ghost;
		AutoSpec.bCentreLabel = true;
		AutoSpec.LabelSize = 12.0f;
		AutoSpec.Height = 26.0f;
		AirAutoButton = MakeButton(AutoSpec);
		ApexUI::AddV(Temperature, MakeCaption(TEXT("Air temperature"), nullptr,
			ApexUI::MakeSized(*WidgetTree, AirAutoButton, 68.0f, 26.0f)), FMargin(0.0f, 0.0f, 0.0f, 8.0f));

		UApexButtonWidget* Minus = nullptr;
		UApexButtonWidget* Plus = nullptr;
		UTextBlock* Value = nullptr;
		UTextBlock* Note = nullptr;
		ApexUI::AddV(Temperature, ApexUI::MakeSized(*WidgetTree,
			MakeStepper(Minus, Value, Plus, 58.0f, -1.0f, 26.0f, &Note), -1.0f, 58.0f));
		AirMinus = Minus;
		AirPlus = Plus;
		AirValueText = Value;
		AirNoteText = Note;
		ApexUI::AddV(Air, Temperature, FMargin(0.0f, 0.0f, 0.0f, 22.0f));
	}
	{
		UVerticalBox* Wind = WidgetTree->ConstructWidget<UVerticalBox>();
		UTextBlock* From = nullptr;
		ApexUI::AddV(Wind, MakeCaption(TEXT("Wind"), &From), FMargin(0.0f, 0.0f, 0.0f, 8.0f));
		WindFromText = From;

		UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();
		UVerticalBox* Strengths = WidgetTree->ConstructWidget<UVerticalBox>();
		WindButtons.Reset();
		for (int32 Index = 0; Index <= WindPresetCount; ++Index)
		{
			const bool bAuto = Index == WindPresetCount;
			FApexButtonSpec Spec;
			Spec.Label = bAuto ? TEXT("Auto") : WindPresetLabels[Index];
			Spec.Badge = bAuto ? FString(TEXT("Weather")) : FString::Printf(TEXT("%d km/h"), WindPresetsKph[Index]);
			Spec.Variant = EApexButtonVariant::Panel;
			Spec.LabelSize = 15.0f;
			Spec.Height = 32.0f;
			UApexButtonWidget* Button = MakeButton(Spec);
			WindButtons.Add(Button);
			ApexUI::AddV(Strengths, Button, FMargin(0.0f, Index == 0 ? 0.0f : 4.0f, 0.0f, 0.0f));
		}
		ApexUI::AddH(Row, Strengths, FMargin(), VAlign_Top, 1.0f);

		// The dial: where the wind comes from, round the start straight
		// (ahead at the top), with a vane pointing into it.
		constexpr float Dial = 184.0f;
		constexpr float Chip = 34.0f;
		UOverlay* Face = WidgetTree->ConstructWidget<UOverlay>();
		AddFill(Face, ApexUI::MakePanel(*WidgetTree, nullptr, FMargin(),
			ApexUI::MakeBrush(PanelWell, ApexUI::Palette::Border, 1.0f, Dial * 0.5f)));
		UCanvasPanel* Canvas = WidgetTree->ConstructWidget<UCanvasPanel>();

		UVerticalBox* Needle = WidgetTree->ConstructWidget<UVerticalBox>();
		UBorder* Tip = ApexUI::MakePanel(*WidgetTree, nullptr, FMargin(), ApexUI::MakeBrush(ApexUI::Palette::Accent));
		UWidget* TipBox = ApexUI::MakeSized(*WidgetTree, Tip, 12.0f, 12.0f);
		TipBox->SetRenderTransformAngle(45.0f);
		ApexUI::AddV(Needle, TipBox, FMargin(0.0f, 0.0f, 0.0f, -6.0f), HAlign_Center);
		WindNeedleShaft = ApexUI::MakePanel(*WidgetTree, nullptr, FMargin(), ApexUI::MakeBrush(ApexUI::Palette::Accent));
		ApexUI::AddV(Needle, ApexUI::MakeSized(*WidgetTree, WindNeedleShaft, 4.0f, 44.0f), FMargin(), HAlign_Center);
		ApexUI::AddV(Needle, WidgetTree->ConstructWidget<USpacer>(), FMargin(), HAlign_Fill, 1.0f);
		Needle->SetRenderTransformPivot(FVector2D(0.5f, 0.5f));
		UCanvasPanelSlot* NeedleSlot = Canvas->AddChildToCanvas(Needle);
		NeedleSlot->SetAnchors(FAnchors(0.5f, 0.5f));
		NeedleSlot->SetAlignment(FVector2D(0.5f, 0.5f));
		NeedleSlot->SetSize(FVector2D(16.0f, 112.0f));
		WindNeedle = Needle;
		Tip->SetVisibility(ESlateVisibility::HitTestInvisible);

		WindFromButtons.Reset();
		for (int32 Index = 0; Index < WindFromCount; ++Index)
		{
			FApexButtonSpec Spec;
			Spec.Label = WindFromChips[Index];
			Spec.Variant = EApexButtonVariant::Ghost;
			Spec.bCentreLabel = true;
			Spec.LabelSize = 11.0f;
			Spec.Height = Chip;
			UApexButtonWidget* Button = MakeButton(Spec);
			// From the left is counter-clockwise from ahead, as in the track's
			// frame: the left of the dial.
			const float Angle = FMath::DegreesToRadians(Index * 45.0f);
			UCanvasPanelSlot* ChipSlot = Canvas->AddChildToCanvas(Button);
			ChipSlot->SetAnchors(FAnchors(0.5f - FMath::Sin(Angle) * 0.39f, 0.5f - FMath::Cos(Angle) * 0.39f));
			ChipSlot->SetAlignment(FVector2D(0.5f, 0.5f));
			ChipSlot->SetSize(FVector2D(Chip, Chip));
			WindFromButtons.Add(Button);
		}
		AddFill(Face, Canvas);
		ApexUI::AddH(Row, ApexUI::MakeSized(*WidgetTree, Face, Dial, Dial), FMargin(14.0f, 0.0f, 0.0f, 0.0f), VAlign_Top);
		ApexUI::AddV(Wind, Row);
		ApexUI::AddV(Air, Wind);
	}
	ApexUI::AddV(Tab, Air);

	return Tab;
}

UWidget* UApexSessionCreateWidget::BuildFooter()
{
	UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();

	UBorder* Dot = nullptr;
	ApexUI::AddH(Row, ApexUI::MakeDot(*WidgetTree, ApexUI::Palette::Live, 8.0f, &Dot));
	StatusDot = Dot;
	StatusLine = ApexUI::MakeText(*WidgetTree, FString(), ApexUI::Font::Mono(10.0f, 100), ApexUI::Palette::Live);
	ApexUI::AddH(Row, StatusLine, FMargin(10.0f, 0.0f, 0.0f, 0.0f));
	SummaryText = ApexUI::MakeText(*WidgetTree, FString(), ApexUI::Font::Mono(10.0f, 60), ApexUI::Palette::TextSecondary);
	SummaryText->SetClipping(EWidgetClipping::ClipToBounds);
	ApexUI::AddH(Row, SummaryText, FMargin(18.0f, 0.0f, 18.0f, 0.0f), VAlign_Center, 1.0f);

	FApexButtonSpec CancelSpec;
	CancelSpec.Label = TEXT("Cancel");
	CancelSpec.Variant = EApexButtonVariant::Ghost;
	CancelSpec.bCentreLabel = true;
	CancelSpec.LabelSize = 17.0f;
	CancelSpec.Height = 52.0f;
	CancelSpec.Sound = EApexUiSound::Back;
	CancelButton = MakeButton(CancelSpec);
	ApexUI::AddH(Row, ApexUI::MakeSized(*WidgetTree, CancelButton, 170.0f, -1.0f), FMargin(0.0f, 0.0f, 12.0f, 0.0f));

	FApexButtonSpec CreateSpec;
	CreateSpec.Label = TEXT("Create session");
	CreateSpec.KeyCap = TEXT("Enter");
	CreateSpec.Variant = EApexButtonVariant::Primary;
	CreateSpec.LabelSize = 21.0f;
	CreateSpec.Height = 52.0f;
	CreateButtonWidget = MakeButton(CreateSpec);
	ApexUI::AddH(Row, ApexUI::MakeSized(*WidgetTree, CreateButtonWidget, 330.0f, -1.0f));

	UBorder* Bar = ApexUI::MakePanel(
		*WidgetTree,
		Row,
		FMargin(ApexUI::Metrics::PageGutter, 0.0f),
		ApexUI::MakeBrush(ApexUI::Palette::Background));
	Bar->SetVerticalAlignment(VAlign_Center);

	return ApexUI::MakeSized(*WidgetTree, Bar, -1.0f, 88.0f);
}

UTexture2D* UApexSessionCreateWidget::GradientTexture()
{
	if (Gradient)
	{
		return Gradient;
	}
	// White, opaque at the top and clear at the bottom: tinted, one colour
	// fading over another.
	constexpr int32 Height = 64;
	Gradient = UTexture2D::CreateTransient(1, Height, PF_B8G8R8A8);
	if (!Gradient)
	{
		return nullptr;
	}
	Gradient->SRGB = true;
	Gradient->Filter = TF_Bilinear;
	Gradient->AddressX = TA_Clamp;
	Gradient->AddressY = TA_Clamp;
	FTexture2DMipMap& Mip = Gradient->GetPlatformData()->Mips[0];
	FColor* Pixels = static_cast<FColor*>(Mip.BulkData.Lock(LOCK_READ_WRITE));
	for (int32 Y = 0; Y < Height; ++Y)
	{
		const float Alpha = 1.0f - static_cast<float>(Y) / static_cast<float>(Height - 1);
		Pixels[Y] = FColor(255, 255, 255, static_cast<uint8>(FMath::RoundToInt(Alpha * 255.0f)));
	}
	Mip.BulkData.Unlock();
	Gradient->UpdateResource();
	return Gradient;
}

UTexture2D* UApexSessionCreateWidget::RainTexture()
{
	if (Rain)
	{
		return Rain;
	}
	// Thin streaks leaning a quarter step per row, tiling: across 48 rows the
	// lean is exactly one 12-pixel period.
	constexpr int32 Width = 12;
	constexpr int32 Height = 48;
	Rain = UTexture2D::CreateTransient(Width, Height, PF_B8G8R8A8);
	if (!Rain)
	{
		return nullptr;
	}
	Rain->SRGB = true;
	Rain->Filter = TF_Bilinear;
	Rain->AddressX = TA_Wrap;
	Rain->AddressY = TA_Wrap;
	FTexture2DMipMap& Mip = Rain->GetPlatformData()->Mips[0];
	FColor* Pixels = static_cast<FColor*>(Mip.BulkData.Lock(LOCK_READ_WRITE));
	for (int32 Y = 0; Y < Height; ++Y)
	{
		for (int32 X = 0; X < Width; ++X)
		{
			const float Phase = FMath::Fmod(static_cast<float>(X) + static_cast<float>(Y) * 0.25f, static_cast<float>(Width));
			const float Distance = FMath::Min(Phase, static_cast<float>(Width) - Phase);
			const float Alpha = FMath::Clamp(1.0f - Distance, 0.0f, 1.0f) * 0.38f;
			Pixels[Y * Width + X] = FColor(225, 235, 245, static_cast<uint8>(FMath::RoundToInt(Alpha * 255.0f)));
		}
	}
	Mip.BulkData.Unlock();
	Rain->UpdateResource();
	return Rain;
}

// ---------------------------------------------------------------------------
// Data
// ---------------------------------------------------------------------------

void UApexSessionCreateWidget::RefreshContent()
{
	UApexMenuFlowSubsystem* Flow = GetFlow();
	const UApexNetSubsystem* Net = GetNet();
	if (!Flow || !TrackSummaryBox || !CarSummaryBox)
	{
		return;
	}

	// Both summaries are rebuilt on every lobby snapshot. A link that had the
	// keyboard would otherwise take the focus down with it.
	const bool bTrackLinkHadFocus = ChangeTrackLink && ChangeTrackLink->HasKeyboardFocus();
	const bool bCarLinkHadFocus = ChangeCarLink && ChangeCarLink->HasKeyboardFocus();

	constexpr float ArtWidth = 150.0f;
	constexpr float ArtHeight = 92.0f;

	auto MakeText = [this](const FString& Name, const FString& Empty, const TArray<FString>& Meta,
		const FString& LinkLabel, UApexButtonWidget*& OutLink)
	{
		UVerticalBox* Text = WidgetTree->ConstructWidget<UVerticalBox>();
		ApexUI::AddV(Text, ApexUI::MakeText(
			*WidgetTree,
			Name.IsEmpty() ? Empty : Name,
			ApexUI::Font::Display(20.0f),
			Name.IsEmpty() ? ApexUI::Palette::TextDisabled : ApexUI::Palette::TextPrimary));
		ApexUI::AddV(
			Text,
			ApexUI::MakeText(*WidgetTree, FString::Join(Meta, TEXT(" · ")), ApexUI::Font::Mono(9.0f, 60), ApexUI::Palette::TextSecondary),
			FMargin(0.0f, 4.0f, 0.0f, 0.0f));
		ApexUI::AddV(Text, WidgetTree->ConstructWidget<USpacer>(), FMargin(), HAlign_Fill, 1.0f);

		FApexButtonSpec LinkSpec;
		LinkSpec.Label = LinkLabel;
		LinkSpec.Variant = EApexButtonVariant::Bare;
		LinkSpec.LabelSize = 15.0f;
		LinkSpec.LabelColour = ApexUI::Palette::Accent;
		LinkSpec.Height = 28.0f;
		OutLink = MakeButton(LinkSpec);
		ApexUI::AddV(Text, OutLink, FMargin(), HAlign_Left);
		return Text;
	};

	// --- Track ---------------------------------------------------------------
	TrackSummaryBox->ClearChildren();
	{
		FApexTrackCatalogRow Row;
		const bool bHasRow = Flow->GetTrackCatalogRow(Flow->GetPendingTrackId(), Row);

		FString Name = bHasRow ? Row.DisplayName : FString();
		if (Name.IsEmpty() && Net)
		{
			FApexTrackConfigSummary Summary;
			if (Net->FindTrackById(Flow->GetPendingTrackId(), Summary))
			{
				Name = Summary.Name;
			}
		}

		TArray<FString> Meta;
		if (bHasRow)
		{
			if (!Row.Country.IsEmpty())  { Meta.Add(Row.Country.ToUpper()); }
			if (Row.LengthM > 0.0f)      { Meta.Add(FString::Printf(TEXT("%.2f KM"), Row.LengthM / 1000.0f)); }
			if (!Row.Category.IsEmpty()) { Meta.Add(ApexCatalog::DisplayClass(Row.Category).ToUpper()); }
		}

		UApexButtonWidget* Link = nullptr;
		UWidget* Text = MakeText(Name, TEXT("No track selected"), Meta,
			Name.IsEmpty() ? TEXT("Select a track") : TEXT("Change track"), Link);
		ChangeTrackLink = Link;

		ApexUI::AddH(
			TrackSummaryBox,
			ApexUI::MakePreview(*WidgetTree, bHasRow ? UApexTrackContentSubsystem::PreviewOf(Row) : nullptr, TEXT("No preview"), ArtWidth, ArtHeight),
			FMargin(0.0f, 0.0f, 14.0f, 0.0f),
			VAlign_Center);
		ApexUI::AddH(TrackSummaryBox, Text, FMargin(), VAlign_Fill, 1.0f);
	}

	// --- Car -----------------------------------------------------------------
	CarSummaryBox->ClearChildren();
	{
		FApexCarCatalogRow Row;
		const bool bHasRow = Flow->GetCarCatalogRow(Flow->GetPendingCarId(), Row);

		FString Name = bHasRow ? Row.DisplayName : FString();
		if (Name.IsEmpty() && Net)
		{
			FApexCarConfigSummary Summary;
			if (Net->FindCarById(Flow->GetPendingCarId(), Summary))
			{
				Name = Summary.Name;
			}
		}

		TArray<FString> Meta;
		if (bHasRow)
		{
			if (!Row.CarClass.IsEmpty()) { Meta.Add(ApexCatalog::DisplayClass(Row.CarClass).ToUpper()); }
			if (Row.MaxPowerKw > 0.0f)   { Meta.Add(FString::Printf(TEXT("%.0f HP"), Row.MaxPowerKw * 1.34102f)); }
			if (Row.MassKg > 0.0f)       { Meta.Add(FString::Printf(TEXT("%.0f KG"), Row.MassKg)); }
		}

		UApexButtonWidget* Link = nullptr;
		UWidget* Text = MakeText(Name, TEXT("No car selected"), Meta,
			Name.IsEmpty() ? TEXT("Select a car") : TEXT("Change car"), Link);
		ChangeCarLink = Link;

		// Cars have meshes rather than preview textures, so the card borrows the
		// garage's turntable: it is idle whenever this screen is up, and a still
		// of the pending car from it beats a captioned placeholder.
		// Only while this screen is in front, though: the content is also
		// rebuilt on every lobby snapshot while the garage is showing, and
		// the garage's own pick would be swapped for the pending car and the
		// turntable stopped under it.
		UWidget* CarArt = nullptr;
		AApexCarPreviewStage* Stage = IsActiveScreen() ? AApexCarPreviewStage::Find(this) : nullptr;
		if (Stage && bHasRow && ApexCarContent::HasBody(Row) && Stage->GetPreviewRenderTarget())
		{
			Stage->SetPreviewTransform(Row.PreviewOffset, Row.PreviewRotation, Row.PreviewScale);
			Stage->SetCarWheels(Row.Wheels);
			Stage->SetCarDrsFlap(Row.DrsFlap);
			Stage->SetCarMesh(ApexCarContent::LoadBody(Row));
			Stage->SetTurntableEnabled(false);
			Stage->ResetTurntable();

			UImage* StageImage = WidgetTree->ConstructWidget<UImage>();
			FSlateBrush Brush = StageImage->GetBrush();
			Brush.SetResourceObject(Stage->GetPreviewRenderTarget());
			Brush.ImageSize = Stage->GetPreviewSize();
			Brush.DrawAs = ESlateBrushDrawType::Image;
			StageImage->SetBrush(Brush);

			UScaleBox* Fit = WidgetTree->ConstructWidget<UScaleBox>();
			Fit->SetStretch(EStretch::ScaleToFit);
			Fit->AddChild(StageImage);
			CarArt = ApexUI::MakePanel(*WidgetTree, Fit, FMargin(), ApexUI::MakeBrush(PanelWell));
		}
		else
		{
			CarArt = ApexUI::MakeArtPlaceholder(*WidgetTree, TEXT("Car"));
		}

		ApexUI::AddH(
			CarSummaryBox,
			ApexUI::MakeSized(*WidgetTree, CarArt, ArtWidth, ArtHeight),
			FMargin(0.0f, 0.0f, 14.0f, 0.0f),
			VAlign_Center);
		ApexUI::AddH(CarSummaryBox, Text, FMargin(), VAlign_Fill, 1.0f);
	}

	if (bTrackLinkHadFocus)
	{
		ApexNav::Focus(ChangeTrackLink);
	}
	else if (bCarLinkHadFocus)
	{
		ApexNav::Focus(ChangeCarLink);
	}

	// The race length reads in kilometres off the track row.
	RefreshSettings();
}

void UApexSessionCreateWidget::RefreshSettings()
{
	UApexMenuFlowSubsystem* Flow = GetFlow();
	if (!Flow)
	{
		return;
	}

	const bool bMultiplayer = Flow->CreateSessionKind != EApexSessionKind::Practice;
	const EApexGameMode Mode = Flow->CreateStartingMode;
	const bool bRace = Mode == EApexGameMode::Race;
	const bool bHotlap = Mode == EApexGameMode::Hotlap;
	const ApexCreateSession::FGrid G = ApexCreateSession::Grid(bMultiplayer, Flow->CreateMaxPlayers, Flow->CreateAiCount, MaxPlayersCeiling);
	const FApexSessionConditions Conditions = Flow->CreateConditions.Clamped();

	if (KindMultiplayerButton && KindSingleButton)
	{
		KindMultiplayerButton->SetSelected(bMultiplayer);
		KindSingleButton->SetSelected(!bMultiplayer);
	}

	// --- Tabs --------------------------------------------------------------------
	for (int32 Tab = 0; Tab < TabButtons.Num(); ++Tab)
	{
		if (!TabButtons[Tab])
		{
			continue;
		}
		const FString Summary = Tab == 0
			? FString(ModeLabel(Mode)).ToUpper()
			: FString::Printf(TEXT("%s · %s"), *Conditions.ClockText(), *FApexSessionConditions::WeatherLabel(Conditions.Weather).ToUpper());
		TabButtons[Tab]->SetBadge(Summary, ApexUI::Palette::TextSecondary);
	}

	// --- Race tab ----------------------------------------------------------------
	for (int32 Index = 0; Index < ModeButtons.Num(); ++Index)
	{
		if (ModeButtons[Index])
		{
			ModeButtons[Index]->SetSelected(ModeOptions[Index].Mode == Mode);
		}
	}

	if (LengthSection)
	{
		LengthSection->SetVisibility(bRace ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);
	}
	const bool bTimed = Flow->bCreateTimedRace;
	const TArray<int32>& Ladder = ApexRaceLength::LadderMinutes();
	for (int32 Index = 0; Index < LengthUnitButtons.Num(); ++Index)
	{
		if (LengthUnitButtons[Index])
		{
			LengthUnitButtons[Index]->SetSelected((Index == 1) == bTimed);
		}
	}
	if (LapsValueText)
	{
		// "1 h 30" does not fit at the size of a lap count.
		LapsValueText->SetFont(ApexUI::Font::Display(bTimed ? 26.0f : 40.0f));
		LapsValueText->SetText(bTimed
			? FText::FromString(ApexRaceLength::Describe(Flow->CreateRaceMinutes * 60))
			: FText::AsNumber(Flow->CreateLapLimit));
	}
	for (int32 Index = 0; Index < LapPresetButtons.Num(); ++Index)
	{
		if (LapPresetButtons[Index])
		{
			LapPresetButtons[Index]->SetVisibility(bTimed ? ESlateVisibility::Collapsed : ESlateVisibility::Visible);
			LapPresetButtons[Index]->SetSelected(LapPresets[Index].Laps == Flow->CreateLapLimit);
		}
	}
	for (int32 Index = 0; Index < DurationPresetButtons.Num(); ++Index)
	{
		if (DurationPresetButtons[Index])
		{
			DurationPresetButtons[Index]->SetVisibility(bTimed ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
			DurationPresetButtons[Index]->SetSelected(DurationPresets[Index].Minutes == Flow->CreateRaceMinutes);
		}
	}
	if (LapsMinus) { LapsMinus->SetIsEnabled(bTimed ? Flow->CreateRaceMinutes > Ladder[0] : Flow->CreateLapLimit > 1); }
	if (LapsPlus)  { LapsPlus->SetIsEnabled(bTimed ? Flow->CreateRaceMinutes < Ladder.Last() : Flow->CreateLapLimit < LapsCeiling); }
	if (LengthInfoText)
	{
		// The distance from the track's length; the time (or a timed race's
		// laps) only from a lap this player has actually driven here. No
		// personal best, no estimate.
		TArray<FString> Parts;
		float BestSeconds = 0.0f;
		const bool bHasBest = Flow->GetBestLapSeconds(Flow->GetPendingTrackId(), BestSeconds) && BestSeconds > 0.0f;
		if (bTimed)
		{
			if (bHasBest)
			{
				// The clock's laps, and the one it runs out on.
				const int32 Laps = FMath::CeilToInt(Flow->CreateRaceMinutes * 60.0f / BestSeconds) + 1;
				Parts.Add(FString::Printf(TEXT("≈ %d LAPS AT YOUR BEST"), Laps));
			}
			Parts.Add(TEXT("THEN THE LEADER'S LAP"));
		}
		else
		{
			if (bHasBest)
			{
				const int32 Minutes = FMath::Max(1, FMath::RoundToInt(BestSeconds * Flow->CreateLapLimit / 60.0f));
				Parts.Add(FString::Printf(TEXT("≈ %d MIN AT YOUR BEST"), Minutes));
			}
			FApexTrackCatalogRow Row;
			if (Flow->GetTrackCatalogRow(Flow->GetPendingTrackId(), Row) && Row.LengthM > 0.0f)
			{
				Parts.Add(FString::Printf(TEXT("%.1f KM"), Row.LengthM * Flow->CreateLapLimit / 1000.0f));
			}
		}
		LengthInfoText->SetText(FText::FromString(FString::Join(Parts, TEXT(" · "))));
	}

	// A hotlap has no field: every driver goes out from their own garage.
	if (FieldSection)
	{
		FieldSection->SetVisibility(bHotlap ? ESlateVisibility::Collapsed : ESlateVisibility::SelfHitTestInvisible);
	}
	if (FieldLabelText) { FieldLabelText->SetText(FText::FromString(bMultiplayer ? TEXT("Grid size") : TEXT("Field"))); }
	if (FieldValueText) { FieldValueText->SetText(FText::AsNumber(G.Field)); }
	if (AiValueText)    { AiValueText->SetText(FText::AsNumber(G.Ai)); }
	const int32 AiSkill = ApexAiSkill::Clamp(Flow->CreateAiSkill);
	if (AiSkillValueText)
	{
		AiSkillValueText->SetText(FText::FromString(AiSkill == ApexAiSkill::Mixed ? FString(TEXT("MIX")) : FString::FromInt(AiSkill)));
	}
	if (AiSkillLabelText)
	{
		AiSkillLabelText->SetText(FText::FromString(AiSkill == ApexAiSkill::Mixed
			? FString(TEXT("AI level  ·  Mixed, novice to ace"))
			: FString::Printf(TEXT("AI level  ·  %s"), *AiSkillSummary(AiSkill))));
		AiSkillLabelText->SetColorAndOpacity(FSlateColor(G.Ai > 0 ? ApexUI::Palette::TextPrimary : ApexUI::Palette::TextMuted));
	}
	if (FieldInfoText)
	{
		FieldInfoText->SetText(FText::FromString(bMultiplayer
			? FString::Printf(TEXT("%d OPEN FOR PLAYERS"), G.Open)
			: FString(TEXT("SINGLE PLAYER · AI ONLY"))));
	}
	if (FieldMinus) { FieldMinus->SetIsEnabled(G.Field > 1); }
	if (FieldPlus)  { FieldPlus->SetIsEnabled(G.Field < MaxPlayersCeiling); }
	if (AiMinus)    { AiMinus->SetIsEnabled(G.Ai > 0); }
	if (AiSkillMinus) { AiSkillMinus->SetIsEnabled(G.Ai > 0 && AiSkill != ApexAiSkill::Mixed); }
	if (AiSkillPlus)  { AiSkillPlus->SetIsEnabled(G.Ai > 0 && AiSkill < ApexAiSkill::Max); }
	if (AiPlus)     { AiPlus->SetIsEnabled(bMultiplayer ? G.Ai < G.Field - 1 : G.Field < MaxPlayersCeiling); }

	const int32 Preset = ApexCreateSession::MatchingPreset(Flow->CreateAllowedAssists);
	for (int32 Index = 0; Index < AssistPresetButtons.Num(); ++Index)
	{
		if (AssistPresetButtons[Index])
		{
			AssistPresetButtons[Index]->SetSelected(Index == Preset);
		}
	}
	for (int32 Index = 0; Index < AssistButtons.Num(); ++Index)
	{
		if (AssistButtons[Index])
		{
			const bool bOn = AssistFlag(Flow->CreateAllowedAssists, static_cast<EApexAssistChip>(Index));
			AssistButtons[Index]->SetBadge(bOn ? TEXT("On") : TEXT("Off"),
				bOn ? ApexUI::Palette::Accent : ApexUI::Palette::TextMuted);
		}
	}

	for (int32 Index = 0; Index < DamageButtons.Num(); ++Index)
	{
		if (DamageButtons[Index])
		{
			DamageButtons[Index]->SetSelected(static_cast<int32>(Flow->CreateDamage) == Index);
		}
	}

	// --- Conditions tab ----------------------------------------------------------
	{
		const float Fraction = static_cast<float>(Conditions.TimeOfDayMinutes) / FApexSessionConditions::MinutesPerDay;
		if (TimeOfDaySlider) { TimeOfDaySlider->SetValue(Fraction); }
		if (TimeOfDayFill)   { TimeOfDayFill->SetPercent(Fraction); }
		if (TimeOfDayValue)  { TimeOfDayValue->SetText(FText::FromString(Conditions.ClockText())); }
		for (int32 Index = 0; Index < TimePresetButtons.Num(); ++Index)
		{
			if (TimePresetButtons[Index])
			{
				TimePresetButtons[Index]->SetSelected(TimePresets[Index].Minutes == Conditions.TimeOfDayMinutes);
			}
		}
	}

	for (int32 Index = 0; Index < WeatherButtons.Num(); ++Index)
	{
		if (WeatherButtons[Index])
		{
			WeatherButtons[Index]->SetSelected(static_cast<int32>(Conditions.Weather) == Index);
		}
	}

	{
		const int32 Air = ApexCreateSession::AirTempC(Conditions);
		if (AirAutoButton) { AirAutoButton->SetSelected(!Conditions.HasAirTemp()); }
		if (AirValueText)  { AirValueText->SetText(FText::FromString(FString::Printf(TEXT("%d °C"), Air))); }
		if (AirNoteText)
		{
			AirNoteText->SetText(FText::FromString(Conditions.HasAirTemp()
				? FString(DescribeAir(Air))
				: FString(TEXT("FROM WEATHER AND TIME"))));
		}
		if (AirMinus) { AirMinus->SetIsEnabled(Air > FApexSessionConditions::MinAirTempC); }
		if (AirPlus)  { AirPlus->SetIsEnabled(Air < FApexSessionConditions::MaxAirTempC); }
	}

	for (int32 Index = 0; Index < WindButtons.Num(); ++Index)
	{
		if (WindButtons[Index])
		{
			const bool bAuto = Index == WindPresetCount;
			WindButtons[Index]->SetSelected(bAuto ? !Conditions.HasWind() : Conditions.WindKph == WindPresetsKph[Index]);
		}
	}
	for (int32 Index = 0; Index < WindFromButtons.Num(); ++Index)
	{
		if (WindFromButtons[Index])
		{
			WindFromButtons[Index]->SetSelected(Conditions.HasWindDirection() && Conditions.WindFromDeg == Index * 45);
		}
	}
	{
		const bool bCalm = ApexCreateSession::WindKph(Conditions) == 0;
		if (WindNeedle)
		{
			WindNeedle->SetVisibility(Conditions.HasWindDirection() ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Hidden);
			WindNeedle->SetRenderTransformAngle(-static_cast<float>(Conditions.WindFromDeg));
			WindNeedle->SetRenderOpacity(bCalm ? 0.35f : 1.0f);
		}
		if (WindFromText)
		{
			WindFromText->SetText(FText::FromString(bCalm ? FString(TEXT("CALM"))
				: Conditions.HasWindDirection()
					? FString::Printf(TEXT("FROM %s"), *FApexSessionConditions::WindFromLabel(Conditions.WindFromDeg).ToUpper())
					: FString(TEXT("DIRECTION AUTO"))));
		}
	}

	RefreshSetups();
	RefreshGridPreview();
	RefreshSkyPreview();
	RefreshFooter();
}

UApexSettingsSubsystem* UApexSessionCreateWidget::GetSettingsSubsystem() const
{
	const UGameInstance* GameInstance = GetGameInstance();
	return GameInstance ? GameInstance->GetSubsystem<UApexSettingsSubsystem>() : nullptr;
}

void UApexSessionCreateWidget::RefreshSetups()
{
	const UApexMenuFlowSubsystem* Flow = GetFlow();
	const UApexSettingsSubsystem* Settings = GetSettingsSubsystem();
	if (!Flow || !Settings || !Settings->Get() || !SetupRows)
	{
		return;
	}

	const FString CarId = Flow->GetPendingCarId();
	const TArray<FApexSavedSetup> Saved = Settings->GetSavedSetups(CarId);
	FApexCarSetup Working = Settings->Get()->CarSetup;
	Working.Clamp();

	// Which saved setup the working one is: the loaded one while it is
	// unchanged, else any with the very same clicks.
	auto Matches = [&Working](const FApexSavedSetup& Entry)
	{
		FApexCarSetup Clicks = Entry.Setup;
		Clicks.Clamp();
		return Clicks == Working;
	};
	const FGuid Loaded = Settings->GetLoadedSetupId();
	int32 Current = Saved.IndexOfByPredicate([&](const FApexSavedSetup& Entry) { return Entry.Id == Loaded && Matches(Entry); });
	if (Current == INDEX_NONE)
	{
		Current = Saved.IndexOfByPredicate(Matches);
	}
	const bool bStock = Working.IsStock();
	const bool bCustom = !bStock && Current == INDEX_NONE;

	// The newest few, and the working one wherever it is in the list.
	TArray<int32> Shown;
	for (int32 Index = 0; Index < Saved.Num() && Shown.Num() < MaxSetupsShown; ++Index)
	{
		Shown.Add(Index);
	}
	if (Current != INDEX_NONE && !Shown.Contains(Current))
	{
		Shown.Last() = Current;
	}

	CurrentSetupLabel = bStock ? FString(TEXT("Stock"))
		: bCustom ? FString(TEXT("Custom"))
		: Saved[Current].Name;

	FString Signature = CarId + (bCustom ? TEXT("|custom") : TEXT(""));
	for (const int32 Index : Shown)
	{
		Signature += FString::Printf(TEXT("|%s:%s:%d"), *Saved[Index].Id.ToString(), *Saved[Index].Name, Saved[Index].BestLapMs);
	}

	if (Signature != SetupSignature || SetupButtons.IsEmpty())
	{
		SetupSignature = Signature;
		const bool bHadFocus = SetupButtons.ContainsByPredicate(
			[](const TObjectPtr<UApexButtonWidget>& Button) { return Button && Button->HasKeyboardFocus(); });

		SetupRows->ClearChildren();
		SetupButtons.Reset();
		SetupButtonIds.Reset();

		auto Add = [this](const FString& Label, const FString& Badge, const FGuid& Id)
		{
			FApexButtonSpec Spec;
			Spec.Label = Label;
			Spec.Badge = Badge;
			Spec.Variant = EApexButtonVariant::Panel;
			Spec.LabelSize = 16.0f;
			Spec.Height = 46.0f;
			SetupButtons.Add(MakeButton(Spec));
			SetupButtonIds.Add(Id);
		};
		Add(TEXT("Stock"), TEXT("As filed"), FGuid());
		if (bCustom)
		{
			Add(TEXT("Custom"), TEXT("Unsaved"), CustomSetupMarker);
		}
		for (const int32 Index : Shown)
		{
			const FApexSavedSetup& Entry = Saved[Index];
			Add(Entry.Name, Entry.BestLapMs > 0
				? UApexMenuFlowSubsystem::FormatLapTime(Entry.BestLapMs / 1000.0f)
				: FString(TEXT("No lap")), Entry.Id);
		}

		// Three to a row, a short last row keeping the thirds.
		UHorizontalBox* Row = nullptr;
		for (int32 Index = 0; Index < SetupButtons.Num(); ++Index)
		{
			if (Index % 3 == 0)
			{
				Row = WidgetTree->ConstructWidget<UHorizontalBox>();
				ApexUI::AddV(SetupRows, Row, FMargin(0.0f, Index == 0 ? 0.0f : 6.0f, 0.0f, 0.0f));
			}
			ApexUI::AddH(Row, SetupButtons[Index], FMargin(Index % 3 == 0 ? 0.0f : 6.0f, 0.0f, 0.0f, 0.0f), VAlign_Fill, 1.0f);
		}
		for (int32 Pad = SetupButtons.Num() % 3; Row && Pad != 0 && Pad < 3; ++Pad)
		{
			ApexUI::AddH(Row, WidgetTree->ConstructWidget<USpacer>(), FMargin(6.0f, 0.0f, 0.0f, 0.0f), VAlign_Fill, 1.0f);
		}

		if (bHadFocus)
		{
			// Back onto the chip now in use, wherever the rebuild put it.
			const FGuid Selected = bStock ? FGuid() : bCustom ? CustomSetupMarker : Saved[Current].Id;
			const int32 At = SetupButtonIds.IndexOfByKey(Selected);
			ApexNav::Focus(SetupButtons.IsValidIndex(At) ? SetupButtons[At].Get() : SetupButtons[0].Get());
		}
	}

	for (int32 Index = 0; Index < SetupButtons.Num(); ++Index)
	{
		const FGuid& Id = SetupButtonIds[Index];
		const bool bSelected = !Id.IsValid() ? bStock
			: Id == CustomSetupMarker ? bCustom
			: Current != INDEX_NONE && Saved[Current].Id == Id;
		SetupButtons[Index]->SetSelected(bSelected);
	}

	if (SetupInfoText)
	{
		SetupInfoText->SetText(FText::FromString(Saved.IsEmpty()
			? FString(TEXT("NONE SAVED · TUNE ONE IN THE HOTLAP GARAGE"))
			: Saved.Num() > Shown.Num()
				? FString::Printf(TEXT("%d SAVED FOR THIS CAR · NEWEST %d SHOWN"), Saved.Num(), Shown.Num())
				: FString::Printf(TEXT("%d SAVED FOR THIS CAR"), Saved.Num())));
	}
}

void UApexSessionCreateWidget::RefreshGridPreview()
{
	const UApexMenuFlowSubsystem* Flow = GetFlow();
	if (!Flow)
	{
		return;
	}

	const bool bMultiplayer = Flow->CreateSessionKind != EApexSessionKind::Practice;
	const bool bHotlap = Flow->CreateStartingMode == EApexGameMode::Hotlap;
	const ApexCreateSession::FGrid G = ApexCreateSession::Grid(bMultiplayer, Flow->CreateMaxPlayers, Flow->CreateAiCount, MaxPlayersCeiling);

	if (GridSlotsPanel) { GridSlotsPanel->SetVisibility(bHotlap ? ESlateVisibility::Collapsed : ESlateVisibility::SelfHitTestInvisible); }
	if (GridSoloPanel)  { GridSoloPanel->SetVisibility(bHotlap ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed); }
	if (GridSoloTitle)  { GridSoloTitle->SetText(FText::FromString(TEXT("Just you and the clock"))); }
	if (GridSoloText)
	{
		GridSoloText->SetText(FText::FromString(
			TEXT("Every driver hotlaps from their own garage: no AI, no grid, no lap limit.")));
	}

	if (GridHintText)
	{
		FString Hint;
		if (bHotlap)
		{
			Hint = TEXT("HOTLAP");
		}
		else if (bMultiplayer)
		{
			Hint = FString::Printf(TEXT("%d CARS · %d AI · %d OPEN · YOU START P%d"), G.Field, G.Ai, G.Open, G.YourSlot);
		}
		else
		{
			Hint = G.Ai > 0
				? FString::Printf(TEXT("%d CARS · YOU START P%d, BEHIND THE AI"), G.Field, G.YourSlot)
				: FString(TEXT("1 CAR · JUST YOU"));
		}
		GridHintText->SetText(FText::FromString(Hint));
	}

	for (int32 Index = 0; Index < SlotFaces.Num(); ++Index)
	{
		const ApexCreateSession::ESlot Kind = ApexCreateSession::SlotAt(G, Index + 1);
		FLinearColor Fill = FLinearColor::Transparent;
		FLinearColor Outline = ApexUI::Palette::Border;
		FLinearColor NumberColour = ApexUI::Palette::TextMuted;
		FLinearColor NameColour = ApexUI::Palette::TextSecondary;
		FString Name;
		switch (Kind)
		{
		case ApexCreateSession::ESlot::You:
			Fill = ApexUI::Palette::Accent;
			Outline = ApexUI::Palette::Accent;
			NumberColour = ApexUI::Palette::OnAccent;
			NameColour = ApexUI::Palette::OnAccent;
			Name = TEXT("YOU");
			break;
		case ApexCreateSession::ESlot::Ai:
			Fill = ApexUI::Palette::SurfaceHover;
			Name = FString::Printf(TEXT("AI %d"), Index + 1);
			break;
		case ApexCreateSession::ESlot::Open:
			Outline = ApexUI::Palette::TextMuted;
			NameColour = ApexUI::Palette::TextMuted;
			Name = TEXT("Open seat");
			break;
		default:
			Outline = ApexUI::Palette::Border * 0.6f;
			Outline.A = 1.0f;
			NumberColour = ApexUI::Palette::TextDisabled;
			break;
		}
		if (SlotFaces[Index])   { SlotFaces[Index]->SetBrush(ApexUI::MakeBrush(Fill, Outline, 1.0f)); }
		if (SlotNumbers[Index]) { SlotNumbers[Index]->SetColorAndOpacity(FSlateColor(NumberColour)); }
		if (SlotNames[Index])
		{
			SlotNames[Index]->SetText(FText::FromString(Name));
			SlotNames[Index]->SetColorAndOpacity(FSlateColor(NameColour));
		}
	}
}

void UApexSessionCreateWidget::RefreshSkyPreview()
{
	const UApexMenuFlowSubsystem* Flow = GetFlow();
	if (!Flow)
	{
		return;
	}

	const FApexSessionConditions C = Flow->CreateConditions.Clamped();
	const ApexCreateSession::FSkyLook Look = ApexCreateSession::Sky(C);
	const ApexCreateSession::FWeatherLook Weather = ApexCreateSession::WeatherLook(C.Weather);

	if (SkyBase) { SkyBase->SetBrush(ApexUI::MakeBrush(SrgbColour(Look.Bottom))); }
	if (SkyTop)  { SkyTop->SetColorAndOpacity(SrgbColour(Look.Top)); }

	// The horizon is a quarter of the way up the panel; the sun climbs the
	// rest by its elevation and crosses it from sunrise on the left.
	const FAnchors SunAt(0.08f + Look.SunX * 0.84f, 1.0f - (0.26f + Look.SunHeight * 0.56f));
	const FLinearColor SunColour = Look.bLowSun ? SrgbColour(FColor(0xFF, 0xB4, 0x6B)) : SrgbColour(FColor(0xFF, 0xF4, 0xD6));
	const FLinearColor GlowColour = Look.bLowSun ? SrgbColour(FColor(0xFF, 0x96, 0x50), 0.3f) : SrgbColour(FColor(0xFF, 0xF0, 0xC8), 0.25f);
	if (SunDiscSlot) { SunDiscSlot->SetAnchors(SunAt); }
	if (SunGlowSlot) { SunGlowSlot->SetAnchors(SunAt); }
	if (SunDisc)
	{
		SunDisc->SetBrush(ApexUI::MakeBrush(SunColour, FLinearColor::Transparent, 0.0f, 30.0f));
		SunDisc->SetRenderOpacity(Look.SunOpacity);
	}
	if (SunGlow)
	{
		SunGlow->SetBrush(ApexUI::MakeBrush(GlowColour, FLinearColor::Transparent, 0.0f, 80.0f));
		SunGlow->SetRenderOpacity(Look.SunOpacity);
	}
	if (CloudVeil) { CloudVeil->SetBrush(ApexUI::MakeBrush(SrgbColour(FColor(150, 158, 166), Weather.Cloud * 0.35f))); }
	if (RainVeil)  { RainVeil->SetColorAndOpacity(FLinearColor(1.0f, 1.0f, 1.0f, Weather.Rain)); }

	if (SkyClockText) { SkyClockText->SetText(FText::FromString(C.ClockText())); }
	if (SkyPhaseText)
	{
		SkyPhaseText->SetText(FText::FromString(FString::Printf(TEXT("%s · %s"),
			Look.Phase, *FApexSessionConditions::WeatherLabel(C.Weather).ToUpper())));
	}

	const int32 Grip = ApexCreateSession::GripPercent(C.Weather);
	if (SkyAirText)   { SkyAirText->SetText(FText::FromString(FString::Printf(TEXT("%d °C"), ApexCreateSession::AirTempC(C)))); }
	if (SkyTrackText) { SkyTrackText->SetText(FText::FromString(FString::Printf(TEXT("%d °C"), FMath::RoundToInt(ApexCreateSession::TrackTempC(C))))); }
	if (SkyGripText)
	{
		SkyGripText->SetText(FText::FromString(FString::Printf(TEXT("%d %%"), Grip)));
		SkyGripText->SetColorAndOpacity(FSlateColor(Grip >= 95 ? ApexUI::Palette::TextPrimary
			: Grip >= 80 ? ApexUI::Palette::Accent
			: ApexUI::Palette::Error));
	}
	if (SkyWindText) { SkyWindText->SetText(FText::FromString(FString::Printf(TEXT("%d km/h"), ApexCreateSession::WindKph(C)))); }
	if (SkyWindArrow)
	{
		SkyWindArrow->SetVisibility(C.HasWindDirection() && ApexCreateSession::WindKph(C) > 0 ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Hidden);
		SkyWindArrow->SetRenderTransformAngle(-static_cast<float>(C.WindFromDeg));
	}
}

void UApexSessionCreateWidget::RefreshFooter()
{
	const UApexMenuFlowSubsystem* Flow = GetFlow();
	const UApexNetSubsystem* Net = GetNet();
	if (!Flow || !StatusLine)
	{
		return;
	}

	const bool bConnected = Net && Net->IsAuthenticated();
	const bool bHasTrack = Flow->HasPendingTrack();
	const bool bHasCar = Flow->HasPendingCar();

	FString Status;
	FLinearColor Colour = ApexUI::Palette::Live;
	if (!bConnected)
	{
		Status = TEXT("NOT CONNECTED");
		Colour = ApexUI::Palette::Error;
	}
	else if (!bHasTrack)
	{
		Status = TEXT("PICK A TRACK");
		Colour = ApexUI::Palette::Error;
	}
	else if (!bHasCar)
	{
		Status = TEXT("NO CAR PICKED");
		Colour = ApexUI::Palette::Accent;
	}
	else
	{
		Status = TEXT("READY");
	}
	StatusLine->SetText(FText::FromString(Status));
	StatusLine->SetColorAndOpacity(FSlateColor(Colour));
	ApexUI::SetDotColour(StatusDot, Colour, 8.0f);

	if (SummaryText)
	{
		const EApexGameMode Mode = Flow->CreateStartingMode;
		const bool bMultiplayer = Flow->CreateSessionKind != EApexSessionKind::Practice;
		const ApexCreateSession::FGrid G = ApexCreateSession::Grid(bMultiplayer, Flow->CreateMaxPlayers, Flow->CreateAiCount, MaxPlayersCeiling);
		const FApexSessionConditions C = Flow->CreateConditions.Clamped();

		TArray<FString> Parts;
		Parts.Add(FString(ModeLabel(Mode)).ToUpper());
		if (Mode == EApexGameMode::Race)
		{
			Parts.Add(Flow->DescribeRaceLength().ToUpper());
		}
		if (Mode != EApexGameMode::Hotlap)
		{
			Parts.Add(FString::Printf(TEXT("%d CARS"), G.Field));
			if (G.Ai > 0)
			{
				Parts.Add(FString::Printf(TEXT("AI %s"), *AiSkillSummary(Flow->CreateAiSkill).ToUpper()));
			}
		}
		Parts.Add(C.ClockText());
		Parts.Add(FApexSessionConditions::WeatherLabel(C.Weather).ToUpper());
		const int32 Preset = ApexCreateSession::MatchingPreset(Flow->CreateAllowedAssists);
		Parts.Add(Preset != INDEX_NONE
			? FString::Printf(TEXT("%s ASSISTS"), *FString(ApexCreateSession::PresetName(static_cast<ApexCreateSession::EAssistPreset>(Preset))).ToUpper())
			: FString::Printf(TEXT("%d/%d ASSISTS"), FApexAllowedAssists::Count - Flow->CreateAllowedAssists.CountLocked(),
				FApexAllowedAssists::Count));
		Parts.Add(DamageOptions[FMath::Clamp(static_cast<int32>(Flow->CreateDamage), 0, 2)].Summary);
		if (!CurrentSetupLabel.IsEmpty())
		{
			Parts.Add(FString::Printf(TEXT("SETUP %s"), *CurrentSetupLabel.ToUpper()));
		}
		if (!bConnected)
		{
			Parts.Insert(TEXT("CONNECT TO A SERVER FIRST"), 0);
		}
		SummaryText->SetText(FText::FromString(FString::Join(Parts, TEXT(" · "))));
	}

	if (CreateButtonWidget)
	{
		CreateButtonWidget->SetIsEnabled(bConnected && bHasTrack);
	}
}

// ---------------------------------------------------------------------------
// Interaction
// ---------------------------------------------------------------------------

void UApexSessionCreateWidget::HandleLobbyStateUpdated(const FApexLobbyState& LobbyState)
{
	RefreshContent();
}

void UApexSessionCreateWidget::Changed()
{
	if (UApexMenuFlowSubsystem* Flow = GetFlow())
	{
		Flow->SaveProfile();
	}
	RefreshSettings();
}

void UApexSessionCreateWidget::SetGridSize(int32 Size)
{
	UApexMenuFlowSubsystem* Flow = GetFlow();
	if (!Flow)
	{
		return;
	}
	Flow->CreateMaxPlayers = FMath::Clamp(Size, 1, MaxPlayersCeiling);
	// AI can never outnumber the grid minus the player.
	Flow->CreateAiCount = FMath::Min(Flow->CreateAiCount, FMath::Max(0, Flow->CreateMaxPlayers - 1));
	Changed();
}

void UApexSessionCreateWidget::SetAiCount(int32 Count)
{
	UApexMenuFlowSubsystem* Flow = GetFlow();
	if (!Flow)
	{
		return;
	}
	if (Flow->CreateSessionKind == EApexSessionKind::Practice)
	{
		// Alone, the field is the AI and you: the grid grows to seat them.
		Flow->CreateAiCount = FMath::Clamp(Count, 0, MaxPlayersCeiling - 1);
		Flow->CreateMaxPlayers = FMath::Max(Flow->CreateMaxPlayers, Flow->CreateAiCount + 1);
	}
	else
	{
		Flow->CreateAiCount = FMath::Clamp(Count, 0, FMath::Max(0, Flow->CreateMaxPlayers - 1));
	}
	Changed();
}

void UApexSessionCreateWidget::PickGridSlot(int32 Position)
{
	const UApexMenuFlowSubsystem* Flow = GetFlow();
	if (!Flow)
	{
		return;
	}
	if (Flow->CreateSessionKind == EApexSessionKind::Practice)
	{
		// You start behind the AI, so the slot you pick is yours.
		SetAiCount(Position - 1);
	}
	else
	{
		SetGridSize(Position);
	}
}

void UApexSessionCreateWidget::HandleTimeOfDayChanged(float Value)
{
	UApexMenuFlowSubsystem* Flow = GetFlow();
	if (!Flow)
	{
		return;
	}

	// Snapped to the quarter hour; the last step wraps onto midnight rather
	// than reading 24:00.
	const int32 Step = FMath::Clamp(FMath::RoundToInt(Value * TimeOfDaySteps), 0, TimeOfDaySteps);
	Flow->CreateConditions.TimeOfDayMinutes = (Step * TimeOfDayStepMinutes) % FApexSessionConditions::MinutesPerDay;
	Changed();
}

void UApexSessionCreateWidget::HandleButtonActivated(UApexButtonWidget* Button)
{
	UApexMenuFlowSubsystem* Flow = GetFlow();
	if (!Button || !Flow)
	{
		return;
	}

	if (Button == HomeButton || Button == CancelButton)
	{
		GoBack();
		return;
	}

	if (Button == ChangeTrackLink)
	{
		if (UApexRootWidget* Root = GetRoot())
		{
			Root->ScreenAfterTrackSelect = EApexScreen::SessionCreate;
		}
		ShowScreen(EApexScreen::TrackSelect);
		return;
	}

	if (Button == ChangeCarLink)
	{
		if (UApexRootWidget* Root = GetRoot())
		{
			Root->ScreenAfterCarSelect = EApexScreen::SessionCreate;
		}
		ShowScreen(EApexScreen::CarSelect);
		return;
	}

	if (Button == KindMultiplayerButton || Button == KindSingleButton)
	{
		// "Single player" is the protocol's Practice kind: the same session, not
		// listed for others to join.
		Flow->CreateSessionKind = Button == KindSingleButton ? EApexSessionKind::Practice : EApexSessionKind::Multiplayer;
		Flow->CreateMaxPlayers = FMath::Max(Flow->CreateMaxPlayers, Flow->CreateAiCount + 1);
		Changed();
		return;
	}

	if (Button == CreateButtonWidget)
	{
		UApexNetSubsystem* Net = GetNet();
		if (!Net || !Net->IsAuthenticated())
		{
			ShowToast(TEXT("Not connected to a server"), true);
			return;
		}
		if (!Flow->HasPendingTrack())
		{
			ShowToast(TEXT("Pick a track first"), true);
			return;
		}

		if (Flow->HasPendingCar())
		{
			Net->SelectCar(Flow->GetPendingCarId());
		}

		// Created sessions land in the lobby: other people may still be joining,
		// and it is the host who decides when to count in.
		Flow->bAutoStartOnJoin = false;
		Flow->SaveProfile();

		UE_LOG(LogApexSim, Log, TEXT("Create session: track '%s', %d players, %d AI at %d, %d laps, %d s, kind %d, %d assists locked, %s, damage %d"),
			*Flow->GetPendingTrackId(), Flow->CreateMaxPlayers, Flow->EffectiveAiCount(), Flow->CreateAiSkill,
			Flow->EffectiveLapLimit(), Flow->EffectiveRaceSeconds(), static_cast<int32>(Flow->CreateSessionKind),
			Flow->CreateAllowedAssists.CountLocked(), *Flow->CreateConditions.Describe(),
			static_cast<int32>(Flow->CreateDamage));

		Net->CreateSession(
			Flow->GetPendingTrackId(),
			Flow->CreateMaxPlayers,
			Flow->EffectiveAiCount(),
			Flow->EffectiveLapLimit(),
			Flow->CreateSessionKind,
			Flow->CreateAllowedAssists,
			Flow->CreateConditions,
			Flow->CreateDamage,
			Flow->CreateAiSkill,
			Flow->EffectiveRaceSeconds());
		return;
	}

	if (const int32 Tab = ApexNav::IndexOf(TabButtons, Button); Tab != INDEX_NONE)
	{
		SetActiveTab(Tab);
		return;
	}

	// --- Race tab ----------------------------------------------------------------
	if (const int32 Mode = ApexNav::IndexOf(ModeButtons, Button); Mode != INDEX_NONE)
	{
		Flow->CreateStartingMode = ModeOptions[Mode].Mode;
		Changed();
		return;
	}
	if (const int32 Unit = ApexNav::IndexOf(LengthUnitButtons, Button); Unit != INDEX_NONE)
	{
		Flow->bCreateTimedRace = Unit == 1;
		Changed();
		return;
	}
	if (Button == LapsMinus || Button == LapsPlus)
	{
		const int32 Step = Button == LapsPlus ? 1 : -1;
		if (Flow->bCreateTimedRace)
		{
			Flow->CreateRaceMinutes = ApexRaceLength::StepMinutes(Flow->CreateRaceMinutes, Step);
		}
		else
		{
			Flow->CreateLapLimit = FMath::Clamp(Flow->CreateLapLimit + Step, 1, LapsCeiling);
		}
		Changed();
		return;
	}
	if (const int32 Laps = ApexNav::IndexOf(LapPresetButtons, Button); Laps != INDEX_NONE)
	{
		Flow->CreateLapLimit = LapPresets[Laps].Laps;
		Changed();
		return;
	}
	if (const int32 Duration = ApexNav::IndexOf(DurationPresetButtons, Button); Duration != INDEX_NONE)
	{
		Flow->CreateRaceMinutes = DurationPresets[Duration].Minutes;
		Changed();
		return;
	}
	if (Button == FieldMinus || Button == FieldPlus)
	{
		const int32 Step = Button == FieldPlus ? 1 : -1;
		const bool bMultiplayer = Flow->CreateSessionKind != EApexSessionKind::Practice;
		const ApexCreateSession::FGrid G = ApexCreateSession::Grid(bMultiplayer, Flow->CreateMaxPlayers, Flow->CreateAiCount, MaxPlayersCeiling);
		if (bMultiplayer)
		{
			SetGridSize(G.Field + Step);
		}
		else
		{
			SetAiCount(G.Ai + Step);
		}
		return;
	}
	if (Button == AiMinus || Button == AiPlus)
	{
		SetAiCount(FMath::Min(Flow->CreateAiCount, FMath::Max(0, Flow->CreateMaxPlayers - 1)) + (Button == AiPlus ? 1 : -1));
		return;
	}
	if (Button == AiSkillMinus || Button == AiSkillPlus)
	{
		Flow->CreateAiSkill = ApexAiSkill::Step(Flow->CreateAiSkill, Button == AiSkillPlus ? 1 : -1);
		Changed();
		return;
	}
	if (const int32 Position = ApexNav::IndexOf(SlotButtons, Button); Position != INDEX_NONE)
	{
		PickGridSlot(Position + 1);
		return;
	}
	if (const int32 Preset = ApexNav::IndexOf(AssistPresetButtons, Button); Preset != INDEX_NONE)
	{
		Flow->CreateAllowedAssists = ApexCreateSession::AssistPreset(static_cast<ApexCreateSession::EAssistPreset>(Preset));
		Changed();
		return;
	}
	if (const int32 SetupAt = ApexNav::IndexOf(SetupButtons, Button); SetupAt != INDEX_NONE)
	{
		UApexSettingsSubsystem* Settings = GetSettingsSubsystem();
		const FGuid Id = SetupButtonIds.IsValidIndex(SetupAt) ? SetupButtonIds[SetupAt] : FGuid();
		if (!Settings || Id == CustomSetupMarker)
		{
			// Custom is the working setup already; nothing to load.
			return;
		}
		if (Id.IsValid())
		{
			Settings->LoadSavedSetup(Id);
		}
		else
		{
			Settings->ResetToDefaults(EApexSettingsGroup::CarSetup);
		}
		RefreshSettings();
		return;
	}
	if (const int32 Damage = ApexNav::IndexOf(DamageButtons, Button); Damage != INDEX_NONE)
	{
		Flow->CreateDamage = static_cast<EApexDamageLevel>(Damage);
		Changed();
		return;
	}
	if (const int32 Assist = ApexNav::IndexOf(AssistButtons, Button); Assist != INDEX_NONE)
	{
		bool& Flag = AssistFlag(Flow->CreateAllowedAssists, static_cast<EApexAssistChip>(Assist));
		Flag = !Flag;
		Changed();
		return;
	}

	// --- Conditions tab ----------------------------------------------------------
	if (const int32 Time = ApexNav::IndexOf(TimePresetButtons, Button); Time != INDEX_NONE)
	{
		Flow->CreateConditions.TimeOfDayMinutes = TimePresets[Time].Minutes;
		Changed();
		return;
	}
	if (const int32 Weather = ApexNav::IndexOf(WeatherButtons, Button); Weather != INDEX_NONE)
	{
		Flow->CreateConditions.Weather = static_cast<EApexWeather>(Weather);
		Changed();
		return;
	}
	if (Button == AirAutoButton)
	{
		// Off auto, the figure starts at what auto would have said.
		FApexSessionConditions& C = Flow->CreateConditions;
		C.AirTempC = C.HasAirTemp() ? FApexSessionConditions::AutoAirTemp : ApexCreateSession::AirTempC(C.Clamped());
		Changed();
		return;
	}
	if (Button == AirMinus || Button == AirPlus)
	{
		FApexSessionConditions& C = Flow->CreateConditions;
		C.AirTempC = FMath::Clamp(ApexCreateSession::AirTempC(C.Clamped()) + (Button == AirPlus ? 1 : -1),
			FApexSessionConditions::MinAirTempC, FApexSessionConditions::MaxAirTempC);
		Changed();
		return;
	}
	if (const int32 Wind = ApexNav::IndexOf(WindButtons, Button); Wind != INDEX_NONE)
	{
		Flow->CreateConditions.WindKph = Wind == WindPresetCount ? FApexSessionConditions::Auto : WindPresetsKph[Wind];
		Changed();
		return;
	}
	if (const int32 From = ApexNav::IndexOf(WindFromButtons, Button); From != INDEX_NONE)
	{
		// The chosen point again hands the direction back to the server.
		FApexSessionConditions& C = Flow->CreateConditions;
		C.WindFromDeg = C.HasWindDirection() && C.WindFromDeg == From * 45 ? FApexSessionConditions::Auto : From * 45;
		Changed();
		return;
	}
}
