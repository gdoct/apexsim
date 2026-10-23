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
#include "Components/ScrollBox.h"
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

	/** The damage tiles, in EApexDamageLevel order; Visual dents the body and costs nothing. */
	struct FDamageOption
	{
		const TCHAR* Label;
		const TCHAR* Description;
		const TCHAR* Summary;
	};
	const FDamageOption DamageOptions[] = {
		{ TEXT("Off"),     TEXT("None"),             TEXT("NO DAMAGE") },
		{ TEXT("Reduced"), TEXT("Half"),             TEXT("REDUCED DAMAGE") },
		{ TEXT("Full"),    TEXT("Modelled"),         TEXT("FULL DAMAGE") },
		{ TEXT("Visual"),  TEXT("Dents"),            TEXT("VISUAL DAMAGE") },
	};

	struct FModeOption
	{
		EApexGameMode Mode;
		const TCHAR* Label;
		const TCHAR* Description;
		/** A race weekend: practice and knockout qualifying before the race (Mode is Race). */
		bool bWeekend = false;
	};

	// Replay is left out because nothing drives it yet (Qualification times
	// laps and the server keeps the classification for a race to grid from),
	// and Demo lap because the server turns it into a dead end for whoever
	// asks (it drops the human players to spectators); the hotlap took its
	// tile. Sandbox is left out too: the client has no free camera for it, so
	// it was a session of standing cars.
	const FModeOption ModeOptions[] = {
		{ EApexGameMode::FreePractice, TEXT("Free practice"), TEXT("Drive freely") },
		{ EApexGameMode::Hotlap,       TEXT("Hotlap"),        TEXT("Flying laps") },
		{ EApexGameMode::Qualification, TEXT("Qualifying"),   TEXT("Sets the grid") },
		{ EApexGameMode::Race,         TEXT("Race"),          TEXT("Grid start") },
		{ EApexGameMode::Race,         TEXT("Weekend"),       TEXT("Practice to flag"), true },
	};

	/** A weekend's practice choices, minutes; 0 is none. */
	const int32 WeekendPracticeChoices[] = { 0, 10, 20, 30, 60 };
	/** Each knockout round's length choices, minutes. */
	const int32 WeekendRoundChoices[] = { 5, 8, 10, 12, 15, 20 };

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
	 * frame turns counter-clockwise), and what each is called on its chip:
	 * the points of a compass seen from the start line, F for ahead, B for
	 * behind (the caption above the dial spells the pick out in words). */
	constexpr int32 WindFromCount = 8;
	const TCHAR* WindFromChips[WindFromCount] = {
		TEXT("AHEAD"), TEXT("A-LEFT"), TEXT("LEFT"), TEXT("B-LEFT"), TEXT("BEHIND"), TEXT("B-RIGHT"), TEXT("RIGHT"), TEXT("A-RIGHT") };

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
	/** Stored qualifying results shown as chips, newest first (the loaded one always is). */
	constexpr int32 MaxResultsShown = 3;
	/** The start order's chip for an order edited by hand. */
	const TCHAR* const CustomOrderId = TEXT("custom");

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
		Net->OnQualifyingResults.AddDynamic(this, &UApexSessionCreateWidget::HandleQualifyingResults);
	}
}

void UApexSessionCreateWidget::NativeDestruct()
{
	if (UApexNetSubsystem* Net = GetNet())
	{
		Net->OnLobbyStateUpdated.RemoveDynamic(this, &UApexSessionCreateWidget::HandleLobbyStateUpdated);
		Net->OnQualifyingResults.RemoveDynamic(this, &UApexSessionCreateWidget::HandleQualifyingResults);
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

	// ... and -ApexCreateMode=N opens it on a mode (EApexGameMode: 7 is the race).
	int32 ModeValue = 0;
	if (FParse::Value(FCommandLine::Get(), TEXT("ApexCreateMode="), ModeValue))
	{
		if (UApexMenuFlowSubsystem* Flow = GetFlow())
		{
			Flow->CreateStartingMode = static_cast<EApexGameMode>(ModeValue);
		}
	}

	// Redraws the settings and the footer too.
	RefreshContent();
	// Results may have been set since the screen was last open.
	QualifyingRequestedFor.Reset();
	RequestQualifying();
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
		if (Flow && Flow->CreateStartingMode == EApexGameMode::Qualification)
		{
			AddAll(QualifyingFormatButtons);
			AddAll(QualifyingLengthButtons);
			AddAll(QualifyingCutoffButtons);
			if (Flow->CreateQualifyingFormat != EApexQualifyingFormat::Shootout)
			{
				Out.Add(QualifyingAllowanceMinus);
				Out.Add(QualifyingAllowancePlus);
			}
		}
		if (Flow && Flow->PitRuleApplies())
		{
			AddAll(MandatoryStopsButtons);
			AddAll(CompoundChangeButtons);
			AddAll(StartTypeButtons);
			AddAll(PenaltyButtons);
			AddAll(SafetyCarButtons);
			AddAll(ReverseGridButtons);
		}
		if (Flow && Flow->IsCreatingWeekend())
		{
			AddAll(WeekendPracticeButtons);
			AddAll(WeekendRoundsButtons);
			AddAll(WeekendFormatButtons);
			if (!(Flow->CreateWeekendRounds <= 1 && Flow->CreateQualifyingFormat == EApexQualifyingFormat::Shootout))
			{
				AddAll(WeekendRoundLengthButtons);
			}
			AddAll(WeekendCutoffButtons);
			Out.Add(WeekendAllowanceMinus);
			Out.Add(WeekendAllowancePlus);
			if (Flow->CreateWeekendPracticeMinutes > 0)
			{
				AddAll(WeekendPracticeDamageButtons);
			}
			AddAll(WeekendQualifyingDamageButtons);
			for (int32 Day = 0; Day < 2; ++Day)
			{
				if (Day == 0 && Flow->CreateWeekendPracticeMinutes <= 0)
				{
					continue;
				}
				Out.Add(WeekendDayWeatherMinus[Day]);
				Out.Add(WeekendDayWeatherPlus[Day]);
				Out.Add(WeekendDayHourMinus[Day]);
				Out.Add(WeekendDayHourPlus[Day]);
			}
		}
		Out.Add(FieldMinus);
		Out.Add(FieldPlus);
		Out.Add(AiMinus);
		Out.Add(AiPlus);
		Out.Add(AiSkillMinus);
		Out.Add(AiSkillPlus);
		if (const UApexMenuFlowSubsystem* OrderFlow = GetFlow(); OrderFlow && OrderFlow->CreateStartingMode == EApexGameMode::Race && !OrderFlow->IsCreatingWeekend())
		{
			Out.Add(MovingPrev);
			Out.Add(MovingNext);
			Out.Add(YourSlotEarlier);
			Out.Add(YourSlotLater);
			AddAll(StartOrderButtons);
		}
		AddAll(AssistPresetButtons);
		AddAll(AssistButtons);
		AddAll(DamageButtons);
		AddAll(SetupButtons);
	}
	else
	{
		Out.Add(TimeOfDaySlider);
		AddAll(TimePresetButtons);
		AddAll(ClockScaleButtons);
		Out.Add(DateMinus);
		Out.Add(DatePlus);
		AddAll(WeatherButtons);
		AddAll(ChangeableButtons);
		AddAll(TrackRubberButtons);
		Out.Add(ForecastAddButton);
		Out.Add(ForecastRollButton);
		if (const UApexMenuFlowSubsystem* SkyFlow = GetFlow())
		{
			const int32 Steps = SkyFlow->CreateConditions.Clamped().Forecast.Num();
			for (int32 Step = 0; Step < Steps && Step < ForecastStepRows.Num(); ++Step)
			{
				Out.Add(ForecastMinuteMinus[Step]);
				Out.Add(ForecastMinutePlus[Step]);
				Out.Add(ForecastWeatherMinus[Step]);
				Out.Add(ForecastWeatherPlus[Step]);
				Out.Add(ForecastRemoveButtons[Step]);
			}
		}
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
	if (UWidget* Target = ApexNav::NearestToward(Direction, Source, Candidates))
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
		// Visible, not self-hit-test-invisible: it is a scroll box and takes the wheel.
		RaceTab->SetVisibility(ActiveTab == 0 ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
	}
	if (ConditionsTab)
	{
		ConditionsTab->SetVisibility(ActiveTab == 1 ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
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

UWidget* UApexSessionCreateWidget::MakeChipRow(const FString& Label, const TArray<FString>& Chips,
	TArray<TObjectPtr<UApexButtonWidget>>& OutButtons)
{
	// The label in a fixed column, so the rows' chips line up under each other.
	UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();
	ApexUI::AddH(Row, ApexUI::MakeSized(*WidgetTree, ApexUI::MakeLabel(*WidgetTree, Label), 150.0f, -1.0f),
		FMargin(0.0f, 0.0f, 8.0f, 0.0f), VAlign_Center);
	OutButtons.Reset();
	for (int32 Index = 0; Index < Chips.Num(); ++Index)
	{
		FApexButtonSpec Spec;
		Spec.Label = Chips[Index];
		Spec.Variant = EApexButtonVariant::Ghost;
		Spec.bCentreLabel = true;
		Spec.LabelSize = 12.0f;
		Spec.Height = 30.0f;
		UApexButtonWidget* Button = MakeButton(Spec);
		OutButtons.Add(Button);
		ApexUI::AddH(Row, Button, FMargin(Index == 0 ? 0.0f : 4.0f, 0.0f, 0.0f, 0.0f), VAlign_Fill, 1.0f);
	}
	return ApexUI::MakeSized(*WidgetTree, Row, -1.0f, 30.0f);
}

UWidget* UApexSessionCreateWidget::MakeStepperRow(const FString& Label, UApexButtonWidget*& OutMinus, UTextBlock*& OutValue,
	UApexButtonWidget*& OutPlus, UTextBlock** OutLabel)
{
	// As a chip row: the label in the fixed column, the stepper across the rest.
	UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();
	UTextBlock* Caption = ApexUI::MakeLabel(*WidgetTree, Label);
	if (OutLabel)
	{
		*OutLabel = Caption;
	}
	ApexUI::AddH(Row, ApexUI::MakeSized(*WidgetTree, Caption, 150.0f, -1.0f), FMargin(0.0f, 0.0f, 8.0f, 0.0f), VAlign_Center);
	ApexUI::AddH(Row, MakeStepper(OutMinus, OutValue, OutPlus, 30.0f, -1.0f, 14.0f), FMargin(), VAlign_Fill, 1.0f);
	return ApexUI::MakeSized(*WidgetTree, Row, -1.0f, 30.0f);
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
	// The settings take half the screen: at a fixed 760 px they were packed
	// tight beside a preview with room to spare.
	ApexUI::AddH(Columns, BuildRightColumn(), FMargin(), VAlign_Fill, 1.0f);

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
		// The screen's own mouse handlers take the press (see NativeOnMouseButtonDown),
		// so a driver can be dragged to another slot.
		Hit->SetVisibility(ESlateVisibility::HitTestInvisible);

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
	ApexUI::AddH(Legend, ApexUI::MakeText(*WidgetTree, TEXT("DRAG A DRIVER TO MOVE IT · CLICK AN EMPTY SEAT TO SET THE FIELD"),
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
	// The race tab is taller than a 1080p screen leaves it: it scrolls, and
	// follows the focus down, rather than running under the footer.
	UScrollBox* RaceScroll = WidgetTree->ConstructWidget<UScrollBox>();
	RaceScroll->SetScrollWhenFocusChanges(EScrollWhenFocusChanges::InstantScroll);
	RaceScroll->SetScrollbarThickness(FVector2D(4.0f, 4.0f));
	RaceScroll->SetScrollbarPadding(FMargin(8.0f, 0.0f, 0.0f, 0.0f));
	RaceScroll->AddChild(BuildRaceTab());
	RaceTab = RaceScroll;
	// So does the Conditions tab, once a written forecast adds its rows.
	UScrollBox* ConditionsScroll = WidgetTree->ConstructWidget<UScrollBox>();
	ConditionsScroll->SetScrollWhenFocusChanges(EScrollWhenFocusChanges::InstantScroll);
	ConditionsScroll->SetScrollbarThickness(FVector2D(4.0f, 4.0f));
	ConditionsScroll->SetScrollbarPadding(FMargin(8.0f, 0.0f, 0.0f, 0.0f));
	ConditionsScroll->AddChild(BuildConditionsTab());
	ConditionsTab = ConditionsScroll;
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
		Spec.LabelSize = 16.0f;
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
		ApexUI::AddH(Row, MakeStepper(Minus, Value, Plus, 62.0f, -1.0f, 40.0f), FMargin(12.0f, 0.0f, 0.0f, 0.0f), VAlign_Fill, 1.0f);
		LapsMinus = Minus;
		LapsPlus = Plus;
		LapsValueText = Value;

		// The presets get a row of their own: beside the unit and the stepper
		// they had under 50 px each and their names ran into each other.
		UHorizontalBox* Presets = WidgetTree->ConstructWidget<UHorizontalBox>();
		LapPresetButtons.Reset();
		for (int32 Index = 0; Index < UE_ARRAY_COUNT(LapPresets); ++Index)
		{
			FApexButtonSpec Spec;
			Spec.Label = LapPresets[Index].Label;
			Spec.SubLabel = FString::Printf(TEXT("%d laps"), LapPresets[Index].Laps);
			Spec.Variant = EApexButtonVariant::Panel;
			Spec.bCentreLabel = true;
			Spec.LabelSize = 16.0f;
			Spec.Height = 54.0f;
			UApexButtonWidget* Button = MakeButton(Spec);
			LapPresetButtons.Add(Button);
			ApexUI::AddH(Presets, Button, FMargin(Index == 0 ? 0.0f : 6.0f, 0.0f, 0.0f, 0.0f), VAlign_Fill, 1.0f);
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
			Spec.Height = 54.0f;
			UApexButtonWidget* Button = MakeButton(Spec);
			DurationPresetButtons.Add(Button);
			ApexUI::AddH(Presets, Button, FMargin(Index == 0 ? 0.0f : 6.0f, 0.0f, 0.0f, 0.0f), VAlign_Fill, 1.0f);
		}
		ApexUI::AddV(Section, ApexUI::MakeSized(*WidgetTree, Row, -1.0f, 62.0f));
		ApexUI::AddV(Section, ApexUI::MakeSized(*WidgetTree, Presets, -1.0f, 54.0f), FMargin(0.0f, 6.0f, 0.0f, 0.0f));
		LengthSection = Section;
		ApexUI::AddV(Tab, Section, FMargin(0.0f, 0.0f, 0.0f, 22.0f));
	}

	// --- Qualifying length ----------------------------------------------------------
	{
		UVerticalBox* Section = WidgetTree->ConstructWidget<UVerticalBox>();
		UTextBlock* Info = nullptr;
		ApexUI::AddV(Section, MakeCaption(TEXT("Qualifying"), &Info), FMargin(0.0f, 0.0f, 0.0f, 8.0f));
		QualifyingInfoText = Info;
		TArray<FString> Chips;
		for (const int32 Minutes : ApexQualifyingLength::ChoiceMinutes())
		{
			Chips.Add(Minutes > 0 ? ApexRaceLength::Describe(Minutes * 60) : FString(TEXT("Open")));
		}
		ApexUI::AddV(Section, MakeChipRow(TEXT("Format"), { TEXT("Open"), TEXT("Shoot-out") }, QualifyingFormatButtons),
			FMargin(0.0f, 0.0f, 0.0f, 6.0f));
		ApexUI::AddV(Section, MakeChipRow(TEXT("Session"), Chips, QualifyingLengthButtons));
		ApexUI::AddV(Section, MakeChipRow(TEXT("Cut-off"), { TEXT("None"), TEXT("107%") }, QualifyingCutoffButtons),
			FMargin(0.0f, 6.0f, 0.0f, 0.0f));
		UApexButtonWidget* AMinus = nullptr;
		UApexButtonWidget* APlus = nullptr;
		UTextBlock* AValue = nullptr;
		QualifyingAllowanceRow = MakeStepperRow(TEXT("Lap allowance"), AMinus, AValue, APlus);
		QualifyingAllowanceMinus = AMinus;
		QualifyingAllowancePlus = APlus;
		QualifyingAllowanceText = AValue;
		ApexUI::AddV(Section, QualifyingAllowanceRow, FMargin(0.0f, 6.0f, 0.0f, 0.0f));
		QualifyingSection = Section;
		ApexUI::AddV(Tab, Section, FMargin(0.0f, 0.0f, 0.0f, 22.0f));
	}

	// --- Race weekend -----------------------------------------------------------------
	{
		UVerticalBox* Section = WidgetTree->ConstructWidget<UVerticalBox>();
		UTextBlock* Info = nullptr;
		ApexUI::AddV(Section, MakeCaption(TEXT("Race weekend"), &Info), FMargin(0.0f, 0.0f, 0.0f, 8.0f));
		WeekendInfoText = Info;
		TArray<FString> Practice;
		for (const int32 Minutes : WeekendPracticeChoices)
		{
			Practice.Add(Minutes > 0 ? ApexRaceLength::Describe(Minutes * 60) : FString(TEXT("None")));
		}
		ApexUI::AddV(Section, MakeChipRow(TEXT("Practice"), Practice, WeekendPracticeButtons), FMargin(0.0f, 0.0f, 0.0f, 6.0f));
		ApexUI::AddV(Section, MakeChipRow(TEXT("Qualifying"), { TEXT("One round"), TEXT("Q1 · Q2"), TEXT("Q1 · Q2 · Q3") }, WeekendRoundsButtons),
			FMargin(0.0f, 0.0f, 0.0f, 6.0f));
		// The final round open and timed, or a one-lap shoot-out.
		ApexUI::AddV(Section, MakeChipRow(TEXT("Final round"), { TEXT("Open"), TEXT("Shoot-out") }, WeekendFormatButtons),
			FMargin(0.0f, 0.0f, 0.0f, 6.0f));
		TArray<FString> RoundLengths;
		for (const int32 Minutes : WeekendRoundChoices)
		{
			RoundLengths.Add(ApexRaceLength::Describe(Minutes * 60));
		}
		WeekendRoundLengthRow = MakeChipRow(TEXT("Each round"), RoundLengths, WeekendRoundLengthButtons);
		ApexUI::AddV(Section, WeekendRoundLengthRow);
		ApexUI::AddV(Section, MakeChipRow(TEXT("Cut-off (Q1)"), { TEXT("None"), TEXT("107%") }, WeekendCutoffButtons),
			FMargin(0.0f, 6.0f, 0.0f, 0.0f));
		{
			UApexButtonWidget* AMinus = nullptr;
			UApexButtonWidget* APlus = nullptr;
			UTextBlock* AValue = nullptr;
			ApexUI::AddV(Section, MakeStepperRow(TEXT("Lap allowance"), AMinus, AValue, APlus), FMargin(0.0f, 6.0f, 0.0f, 0.0f));
			WeekendAllowanceMinus = AMinus;
			WeekendAllowancePlus = APlus;
			WeekendAllowanceText = AValue;
		}
		// Each phase's own damage rule; the race keeps the Damage row's.
		{
			TArray<FString> DamageChips;
			for (int32 Chip = 0; Chip < ApexCreateSession::PhaseDamageChips; ++Chip)
			{
				DamageChips.Add(ApexCreateSession::PhaseDamageChipLabel(Chip));
			}
			WeekendPracticeDamageRow = MakeChipRow(TEXT("Practice damage"), DamageChips, WeekendPracticeDamageButtons);
			ApexUI::AddV(Section, WeekendPracticeDamageRow, FMargin(0.0f, 6.0f, 0.0f, 0.0f));
			ApexUI::AddV(Section, MakeChipRow(TEXT("Qualifying damage"), DamageChips, WeekendQualifyingDamageButtons),
				FMargin(0.0f, 6.0f, 0.0f, 0.0f));
		}

		// Each day's sky: practice and the qualifying day can run under a
		// weather and an hour of their own (the race day's by default), with
		// the forecast the server will lay out for that day.
		for (int32 Day = 0; Day < 2; ++Day)
		{
			UVerticalBox* DayBox = WidgetTree->ConstructWidget<UVerticalBox>();
			UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();
			ApexUI::AddH(Row, ApexUI::MakeSized(*WidgetTree, ApexUI::MakeLabel(*WidgetTree, Day == 0 ? TEXT("Practice sky") : TEXT("Qualifying sky")), 150.0f, -1.0f),
				FMargin(0.0f, 0.0f, 8.0f, 0.0f), VAlign_Center);
			UApexButtonWidget* WMinus = nullptr;
			UApexButtonWidget* WPlus = nullptr;
			UTextBlock* WValue = nullptr;
			ApexUI::AddH(Row, MakeStepper(WMinus, WValue, WPlus, 30.0f, -1.0f, 14.0f), FMargin(), VAlign_Fill, 1.0f);
			UApexButtonWidget* HMinus = nullptr;
			UApexButtonWidget* HPlus = nullptr;
			UTextBlock* HValue = nullptr;
			ApexUI::AddH(Row, MakeStepper(HMinus, HValue, HPlus, 30.0f, 64.0f, 14.0f), FMargin(8.0f, 0.0f, 0.0f, 0.0f), VAlign_Fill);
			ApexUI::AddV(DayBox, ApexUI::MakeSized(*WidgetTree, Row, -1.0f, 30.0f));
			UTextBlock* Forecast = ApexUI::MakeText(*WidgetTree, FString(), ApexUI::Font::Mono(10.0f, 100), ApexUI::Palette::TextSecondary);
			Forecast->SetAutoWrapText(true);
			ApexUI::AddV(DayBox, Forecast, FMargin(158.0f, 3.0f, 0.0f, 0.0f));
			ApexUI::AddV(Section, DayBox, FMargin(0.0f, 6.0f, 0.0f, 0.0f));
			WeekendDayRows[Day] = DayBox;
			WeekendDayWeatherMinus[Day] = WMinus;
			WeekendDayWeatherPlus[Day] = WPlus;
			WeekendDayWeatherText[Day] = WValue;
			WeekendDayHourMinus[Day] = HMinus;
			WeekendDayHourPlus[Day] = HPlus;
			WeekendDayHourText[Day] = HValue;
			WeekendDayForecastText[Day] = Forecast;
		}
		WeekendSection = Section;
		ApexUI::AddV(Tab, Section, FMargin(0.0f, 0.0f, 0.0f, 22.0f));
	}

	// --- Pit stops -----------------------------------------------------------------
	// The race's pit rule: the stops every car owes and a second dry compound.
	// A car that takes the flag owing either is classified behind every car
	// that met it.
	{
		UVerticalBox* Section = WidgetTree->ConstructWidget<UVerticalBox>();
		UTextBlock* Info = nullptr;
		ApexUI::AddV(Section, MakeCaption(TEXT("Pit stops"), &Info), FMargin(0.0f, 0.0f, 0.0f, 8.0f));
		PitRuleInfoText = Info;
		ApexUI::AddV(Section, MakeChipRow(TEXT("Stops"), { TEXT("None"), TEXT("1"), TEXT("2"), TEXT("3") }, MandatoryStopsButtons));
		ApexUI::AddV(Section, MakeChipRow(TEXT("Two compounds"), { TEXT("Free"), TEXT("Two") }, CompoundChangeButtons),
			FMargin(0.0f, 6.0f, 0.0f, 0.0f));
		PitRuleSection = Section;
		ApexUI::AddV(Tab, Section, FMargin(0.0f, 0.0f, 0.0f, 22.0f));
	}

	// --- Race control ---------------------------------------------------------------
	// How the race starts and what the server penalises: four chip rows, the
	// formation lap folded into the start (a rolling start always has its
	// pace lap) and the three penalties toggles on one row.
	{
		UVerticalBox* Section = WidgetTree->ConstructWidget<UVerticalBox>();
		UTextBlock* Info = nullptr;
		ApexUI::AddV(Section, MakeCaption(TEXT("Race control"), &Info), FMargin(0.0f, 0.0f, 0.0f, 8.0f));
		RaceControlInfoText = Info;
		ApexUI::AddV(Section, MakeChipRow(TEXT("Start"), { TEXT("Standing"), TEXT("Formation lap"), TEXT("Rolling") }, StartTypeButtons));
		ApexUI::AddV(Section, MakeChipRow(TEXT("Penalise"), { TEXT("False starts"), TEXT("Yellow passes"), TEXT("Pit exit light") }, PenaltyButtons),
			FMargin(0.0f, 6.0f, 0.0f, 0.0f));
		ApexUI::AddV(Section, MakeChipRow(TEXT("Safety car"), { TEXT("Off"), TEXT("Virtual"), TEXT("On the road") }, SafetyCarButtons),
			FMargin(0.0f, 6.0f, 0.0f, 0.0f));
		TArray<FString> Reverse;
		for (const int32 Slots : ApexCreateSession::ReverseGridChoices)
		{
			Reverse.Add(Slots == 0 ? FString(TEXT("Off")) : Slots >= FApexRaceRules::ReverseWholeField ? FString(TEXT("All")) : FString::FromInt(Slots));
		}
		ApexUI::AddV(Section, MakeChipRow(TEXT("Reverse grid"), Reverse, ReverseGridButtons), FMargin(0.0f, 6.0f, 0.0f, 0.0f));
		RaceControlSection = Section;
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

	// --- Starting order -----------------------------------------------------------
	// Who sits where on the grid: the seating order (the AI ahead, you behind),
	// a stored qualifying result of this track, or one edited by hand, by
	// dragging the drivers on the grid at the left or stepping your own slot.
	{
		UVerticalBox* Section = WidgetTree->ConstructWidget<UVerticalBox>();
		UTextBlock* Info = nullptr;
		ApexUI::AddV(Section, MakeCaption(TEXT("Starting order"), &Info), FMargin(0.0f, 0.0f, 0.0f, 8.0f));
		StartOrderInfoText = Info;

		// Which driver the slot stepper below moves: you, unless another is picked.
		UHorizontalBox* DriverRow = WidgetTree->ConstructWidget<UHorizontalBox>();
		ApexUI::AddH(DriverRow, ApexUI::MakeText(*WidgetTree, TEXT("Driver"), ApexUI::Font::Display(17.0f), ApexUI::Palette::TextPrimary),
			FMargin(), VAlign_Center, 1.0f);
		UApexButtonWidget* Prev = nullptr;
		UApexButtonWidget* Next = nullptr;
		UTextBlock* Name = nullptr;
		ApexUI::AddH(DriverRow, MakeStepper(Prev, Name, Next, 42.0f, 200.0f, 17.0f));
		MovingPrev = Prev;
		MovingNext = Next;
		MovingNameText = Name;
		ApexUI::AddV(Section, ApexUI::MakePanel(*WidgetTree, DriverRow, FMargin(16.0f, 6.0f, 6.0f, 6.0f),
			ApexUI::MakeBrush(ApexUI::Palette::Surface, ApexUI::Palette::Border, 1.0f)), FMargin(0.0f, 0.0f, 0.0f, 6.0f));

		UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();
		UTextBlock* Label = ApexUI::MakeText(*WidgetTree, TEXT("Grid slot"), ApexUI::Font::Display(17.0f), ApexUI::Palette::TextPrimary);
		ApexUI::AddH(Row, Label, FMargin(), VAlign_Center, 1.0f);
		UApexButtonWidget* Earlier = nullptr;
		UApexButtonWidget* Later = nullptr;
		UTextBlock* Value = nullptr;
		ApexUI::AddH(Row, MakeStepper(Earlier, Value, Later, 42.0f, 48.0f, 24.0f));
		YourSlotEarlier = Earlier;
		YourSlotLater = Later;
		YourSlotValueText = Value;
		ApexUI::AddV(Section, ApexUI::MakePanel(*WidgetTree, Row, FMargin(16.0f, 6.0f, 6.0f, 6.0f),
			ApexUI::MakeBrush(ApexUI::Palette::Surface, ApexUI::Palette::Border, 1.0f)), FMargin(0.0f, 0.0f, 0.0f, 6.0f));

		StartOrderRows = WidgetTree->ConstructWidget<UVerticalBox>();
		ApexUI::AddV(Section, StartOrderRows);
		StartOrderSection = Section;
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
		ApexUI::AddV(Tab, Presets, FMargin(0.0f, 0.0f, 0.0f, 8.0f));

		// Whether the clock runs through the session, and how fast: the sun
		// then moves, dusk comes, the air and the asphalt follow.
		TArray<FString> Labels;
		for (int32 Index = 0; Index < ApexCreateSession::ClockScaleCount; ++Index)
		{
			Labels.Add(ApexCreateSession::ClockScaleLabel(ApexCreateSession::ClockScales[Index]));
		}
		ApexUI::AddV(Tab, MakeChipRow(TEXT("Clock"), Labels, ClockScaleButtons), FMargin(0.0f, 0.0f, 0.0f, 8.0f));

		// The date: how high the sun climbs and how long the day is, over
		// the circuit at its own solar noon.
		UHorizontalBox* DateRow = WidgetTree->ConstructWidget<UHorizontalBox>();
		ApexUI::AddH(DateRow, ApexUI::MakeSized(*WidgetTree, ApexUI::MakeLabel(*WidgetTree, TEXT("Date")), 150.0f, -1.0f),
			FMargin(0.0f, 0.0f, 8.0f, 0.0f), VAlign_Center);
		UApexButtonWidget* Minus = nullptr;
		UApexButtonWidget* Plus = nullptr;
		UTextBlock* Value = nullptr;
		ApexUI::AddH(DateRow, MakeStepper(Minus, Value, Plus, 30.0f, -1.0f, 14.0f), FMargin(), VAlign_Fill, 1.0f);
		ApexUI::AddV(Tab, ApexUI::MakeSized(*WidgetTree, DateRow, -1.0f, 30.0f));
		DateMinus = Minus;
		DatePlus = Plus;
		DateValue = Value;
		DateNote = ApexUI::MakeText(*WidgetTree, FString(), ApexUI::Font::Mono(10.0f, 100), ApexUI::Palette::TextSecondary);
		ApexUI::AddV(Tab, DateNote, FMargin(158.0f, 3.0f, 0.0f, 22.0f));
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
		ApexUI::AddV(Tab, Tiles, FMargin(0.0f, 0.0f, 0.0f, 8.0f));

		// What the sky does from there (a forecast from the pick, worked out
		// by the server), and how much rubber the line starts with.
		TArray<FString> Changes;
		for (int32 Level = 0; Level < ApexCreateSession::ChangeableCount; ++Level)
		{
			Changes.Add(FApexSessionConditions::ChangeableLabel(Level));
		}
		TArray<FString> Rubber;
		for (int32 Index = 0; Index < ApexCreateSession::TrackRubberCount; ++Index)
		{
			Rubber.Add(ApexCreateSession::TrackRubberChipLabel(Index));
		}
		ApexUI::AddV(Tab, MakeChipRow(TEXT("Weather changes"), Changes, ChangeableButtons), FMargin(0.0f, 0.0f, 0.0f, 6.0f));
		ApexUI::AddV(Tab, MakeChipRow(TEXT("Track"), Rubber, TrackRubberButtons), FMargin(0.0f, 0.0f, 0.0f, 10.0f));

		// The forecast the pick lays out (ApexCreateSession::Forecast: the
		// server's own, from the seed the create sends), and another seed.
		FApexButtonSpec RollSpec;
		RollSpec.Label = TEXT("ANOTHER");
		RollSpec.Variant = EApexButtonVariant::Ghost;
		RollSpec.bCentreLabel = true;
		RollSpec.LabelSize = 12.0f;
		RollSpec.Height = 26.0f;
		ForecastRollButton = MakeButton(RollSpec);
		// Or the host writes one: steps of a session minute and a weather.
		FApexButtonSpec AddSpec = RollSpec;
		AddSpec.Label = TEXT("ADD STEP");
		ForecastAddButton = MakeButton(AddSpec);
		UHorizontalBox* ForecastButtons = WidgetTree->ConstructWidget<UHorizontalBox>();
		ApexUI::AddH(ForecastButtons, ApexUI::MakeSized(*WidgetTree, ForecastAddButton, 104.0f, 26.0f));
		ApexUI::AddH(ForecastButtons, ApexUI::MakeSized(*WidgetTree, ForecastRollButton, 96.0f, 26.0f), FMargin(6.0f, 0.0f, 0.0f, 0.0f));
		ApexUI::AddV(Tab, MakeCaption(TEXT("Forecast"), nullptr, ForecastButtons), FMargin(0.0f, 0.0f, 0.0f, 4.0f));
		ForecastText = ApexUI::MakeText(*WidgetTree, FString(), ApexUI::Font::Mono(11.0f, 100), ApexUI::Palette::TextSecondary);
		ForecastText->SetAutoWrapText(true);
		ApexUI::AddV(Tab, ForecastText, FMargin(0.0f, 0.0f, 0.0f, 6.0f));

		ForecastStepRows.Reset();
		ForecastStepLabels.Reset();
		ForecastMinuteMinus.Reset();
		ForecastMinutePlus.Reset();
		ForecastMinuteText.Reset();
		ForecastWeatherMinus.Reset();
		ForecastWeatherPlus.Reset();
		ForecastWeatherText.Reset();
		ForecastRemoveButtons.Reset();
		for (int32 Step = 0; Step < FApexSessionConditions::MaxForecastSteps; ++Step)
		{
			UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();
			UTextBlock* Label = ApexUI::MakeLabel(*WidgetTree, FString::Printf(TEXT("Step %d"), Step + 1));
			ApexUI::AddH(Row, ApexUI::MakeSized(*WidgetTree, Label, 150.0f, -1.0f), FMargin(0.0f, 0.0f, 8.0f, 0.0f), VAlign_Center);
			UApexButtonWidget* MMinus = nullptr;
			UApexButtonWidget* MPlus = nullptr;
			UTextBlock* MValue = nullptr;
			ApexUI::AddH(Row, MakeStepper(MMinus, MValue, MPlus, 30.0f, 156.0f, 13.0f), FMargin(), VAlign_Fill);
			UApexButtonWidget* WMinus = nullptr;
			UApexButtonWidget* WPlus = nullptr;
			UTextBlock* WValue = nullptr;
			ApexUI::AddH(Row, MakeStepper(WMinus, WValue, WPlus, 30.0f, -1.0f, 14.0f), FMargin(8.0f, 0.0f, 0.0f, 0.0f), VAlign_Fill, 1.0f);
			FApexButtonSpec RemoveSpec = RollSpec;
			RemoveSpec.Label = TEXT("REMOVE");
			UApexButtonWidget* Remove = MakeButton(RemoveSpec);
			ApexUI::AddH(Row, ApexUI::MakeSized(*WidgetTree, Remove, 84.0f, 30.0f), FMargin(8.0f, 0.0f, 0.0f, 0.0f), VAlign_Fill);
			UWidget* Sized = ApexUI::MakeSized(*WidgetTree, Row, -1.0f, 30.0f);
			ApexUI::AddV(Tab, Sized, FMargin(0.0f, 0.0f, 0.0f, 6.0f));
			ForecastStepRows.Add(Sized);
			ForecastStepLabels.Add(Label);
			ForecastMinuteMinus.Add(MMinus);
			ForecastMinutePlus.Add(MPlus);
			ForecastMinuteText.Add(MValue);
			ForecastWeatherMinus.Add(WMinus);
			ForecastWeatherPlus.Add(WPlus);
			ForecastWeatherText.Add(WValue);
			ForecastRemoveButtons.Add(Remove);
		}
		ApexUI::AddV(Tab, WidgetTree->ConstructWidget<USpacer>(), FMargin(0.0f, 0.0f, 0.0f, 16.0f));
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
		constexpr float Dial = 232.0f;
		constexpr float ChipW = 56.0f;
		constexpr float ChipH = 28.0f;
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
			Spec.LabelSize = 10.0f;
			Spec.Height = ChipH;
			UApexButtonWidget* Button = MakeButton(Spec);
			// From the left is counter-clockwise from ahead, as in the track's
			// frame: the left of the dial.
			const float Angle = FMath::DegreesToRadians(Index * 45.0f);
			UCanvasPanelSlot* ChipSlot = Canvas->AddChildToCanvas(Button);
			ChipSlot->SetAnchors(FAnchors(0.5f - FMath::Sin(Angle) * 0.36f, 0.5f - FMath::Cos(Angle) * 0.36f));
			ChipSlot->SetAlignment(FVector2D(0.5f, 0.5f));
			ChipSlot->SetSize(FVector2D(ChipW, ChipH));
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
			? (Flow->IsCreatingWeekend() ? FString(TEXT("RACE WEEKEND")) : FString(ModeLabel(Mode)).ToUpper())
			: FString::Printf(TEXT("%s · %s"), *Conditions.ClockText(), *FApexSessionConditions::WeatherLabel(Conditions.Weather).ToUpper());
		TabButtons[Tab]->SetBadge(Summary, ApexUI::Palette::TextSecondary);
	}

	// --- Race tab ----------------------------------------------------------------
	for (int32 Index = 0; Index < ModeButtons.Num(); ++Index)
	{
		if (ModeButtons[Index])
		{
			ModeButtons[Index]->SetSelected(ModeOptions[Index].Mode == Mode
				&& ModeOptions[Index].bWeekend == Flow->IsCreatingWeekend());
		}
	}

	if (WeekendSection)
	{
		WeekendSection->SetVisibility(Flow->IsCreatingWeekend() ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);
	}
	for (int32 Index = 0; Index < WeekendPracticeButtons.Num(); ++Index)
	{
		WeekendPracticeButtons[Index]->SetSelected(WeekendPracticeChoices[Index] == Flow->CreateWeekendPracticeMinutes);
	}
	for (int32 Index = 0; Index < WeekendRoundsButtons.Num(); ++Index)
	{
		WeekendRoundsButtons[Index]->SetSelected(Index + 1 == Flow->CreateWeekendRounds);
	}
	for (int32 Index = 0; Index < WeekendRoundLengthButtons.Num(); ++Index)
	{
		WeekendRoundLengthButtons[Index]->SetSelected(WeekendRoundChoices[Index] == Flow->CreateWeekendRoundMinutes);
	}
	// Each weekend day's sky and its forecast; practice's only when there is practice.
	for (int32 Day = 0; Day < 2; ++Day)
	{
		const bool bPractice = Day == 0;
		if (WeekendDayRows[Day])
		{
			WeekendDayRows[Day]->SetVisibility(bPractice && Flow->CreateWeekendPracticeMinutes <= 0
				? ESlateVisibility::Collapsed : ESlateVisibility::SelfHitTestInvisible);
		}
		const int32 Weather = bPractice ? Flow->CreateWeekendPracticeWeather : Flow->CreateWeekendQualifyingWeather;
		const int32 Hour = bPractice ? Flow->CreateWeekendPracticeHour : Flow->CreateWeekendQualifyingHour;
		if (WeekendDayWeatherText[Day])
		{
			WeekendDayWeatherText[Day]->SetText(FText::FromString(Weather >= 0 && Weather < FApexSessionConditions::WeatherCount
				? FApexSessionConditions::WeatherLabel(static_cast<EApexWeather>(Weather)) : FString(TEXT("Race day's"))));
		}
		if (WeekendDayHourText[Day])
		{
			FApexSessionConditions At;
			At.TimeOfDayMinutes = Hour;
			WeekendDayHourText[Day]->SetText(FText::FromString(Hour >= 0 ? At.ClockText() : FString(TEXT("Race's"))));
		}
		if (WeekendDayWeatherMinus[Day]) { WeekendDayWeatherMinus[Day]->SetIsEnabled(Weather >= 0); }
		if (WeekendDayWeatherPlus[Day])  { WeekendDayWeatherPlus[Day]->SetIsEnabled(Weather < FApexSessionConditions::WeatherCount - 1); }
		if (WeekendDayHourMinus[Day])    { WeekendDayHourMinus[Day]->SetIsEnabled(Hour >= 0); }
		if (WeekendDayHourPlus[Day])     { WeekendDayHourPlus[Day]->SetIsEnabled(Hour < ApexCreateSession::DayHourChoices().Last()); }
		if (WeekendDayForecastText[Day])
		{
			FApexSessionConditions Sky;
			FString Line;
			if (!ApexCreateSession::DaySky(Flow->CreateConditions, Weather, Hour,
					bPractice ? ApexCreateSession::EWeekendDay::Practice : ApexCreateSession::EWeekendDay::Qualifying, Sky))
			{
				Line = TEXT("UNDER THE RACE DAY'S SKY");
			}
			else
			{
				Line = FString::Printf(TEXT("%s FROM %s"), *FApexSessionConditions::WeatherLabel(Sky.Weather).ToUpper(), *Sky.ClockText());
				if (Sky.HasForecast())
				{
					// The race day's written forecast holds on this day too.
					const FString Changes = ApexCreateSession::ForecastLine(Sky, ApexCreateSession::SessionForecast(Sky, Sky.SkySeed), 4);
					Line += FString::Printf(TEXT(" · WRITTEN: %s"), *Changes);
				}
				else if (Sky.WeatherChanges() && Sky.HasSkySeed())
				{
					const FString Changes = ApexCreateSession::ForecastLine(Sky, ApexCreateSession::Forecast(Sky, Sky.SkySeed), 4);
					Line += Changes.IsEmpty() ? FString(TEXT(" · NO CHANGE")) : FString::Printf(TEXT(" · %s"), *Changes);
				}
				else if (!Sky.WeatherChanges())
				{
					Line += TEXT(" · THE SKY HOLDS");
				}
				else
				{
					// A sky from before seeds: the server lays it out from the session's id.
					Line += TEXT(" · CHANGES LAID OUT WHEN THE SESSION IS MADE");
				}
			}
			WeekendDayForecastText[Day]->SetText(FText::FromString(Line));
		}
	}
	for (TArray<TObjectPtr<UApexButtonWidget>>* Cutoff : { &QualifyingCutoffButtons, &WeekendCutoffButtons })
	{
		for (int32 Index = 0; Index < Cutoff->Num(); ++Index)
		{
			if ((*Cutoff)[Index])
			{
				(*Cutoff)[Index]->SetSelected((Index == 1) == Flow->bCreateQualifyingCutoff);
			}
		}
	}
	if (WeekendInfoText)
	{
		WeekendInfoText->SetText(FText::FromString(Flow->DescribeWeekend().ToUpper()));
	}

	// Qualifying's format, its lap allowance and a weekend phase's own damage.
	const bool bShootout = Flow->CreateQualifyingFormat == EApexQualifyingFormat::Shootout;
	for (TArray<TObjectPtr<UApexButtonWidget>>* Format : { &QualifyingFormatButtons, &WeekendFormatButtons })
	{
		for (int32 Index = 0; Index < Format->Num(); ++Index)
		{
			if ((*Format)[Index])
			{
				(*Format)[Index]->SetSelected((Index == 1) == bShootout);
			}
		}
	}
	if (WeekendRoundLengthRow)
	{
		// A shoot-out has no clock: with one round, no round has a length.
		WeekendRoundLengthRow->SetVisibility(bShootout && Flow->CreateWeekendRounds <= 1
			? ESlateVisibility::Collapsed : ESlateVisibility::SelfHitTestInvisible);
	}
	{
		const int32 Allowance = ApexQualifyingLapAllowance::Clamp(Flow->CreateQualifyingLapAllowance);
		const FString Label = ApexCreateSession::LapAllowanceLabel(Allowance);
		if (QualifyingAllowanceRow)
		{
			// A plain shoot-out is one flying lap each: no allowance to set.
			QualifyingAllowanceRow->SetVisibility(bShootout ? ESlateVisibility::Collapsed : ESlateVisibility::SelfHitTestInvisible);
		}
		for (UTextBlock* Text : { QualifyingAllowanceText.Get(), WeekendAllowanceText.Get() })
		{
			if (Text) { Text->SetText(FText::FromString(Label)); }
		}
		for (UApexButtonWidget* Minus : { QualifyingAllowanceMinus.Get(), WeekendAllowanceMinus.Get() })
		{
			if (Minus) { Minus->SetIsEnabled(Allowance > ApexQualifyingLapAllowance::None); }
		}
		for (UApexButtonWidget* Plus : { QualifyingAllowancePlus.Get(), WeekendAllowancePlus.Get() })
		{
			if (Plus) { Plus->SetIsEnabled(Allowance < ApexQualifyingLapAllowance::MaxLaps); }
		}
	}
	if (WeekendPracticeDamageRow)
	{
		WeekendPracticeDamageRow->SetVisibility(Flow->CreateWeekendPracticeMinutes <= 0
			? ESlateVisibility::Collapsed : ESlateVisibility::SelfHitTestInvisible);
	}
	for (int32 Index = 0; Index < WeekendPracticeDamageButtons.Num(); ++Index)
	{
		if (WeekendPracticeDamageButtons[Index])
		{
			WeekendPracticeDamageButtons[Index]->SetSelected(Index == ApexCreateSession::PhaseDamageChip(Flow->CreateWeekendPracticeDamage));
		}
	}
	for (int32 Index = 0; Index < WeekendQualifyingDamageButtons.Num(); ++Index)
	{
		if (WeekendQualifyingDamageButtons[Index])
		{
			WeekendQualifyingDamageButtons[Index]->SetSelected(Index == ApexCreateSession::PhaseDamageChip(Flow->CreateWeekendQualifyingDamage));
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

	const bool bQualifying = Mode == EApexGameMode::Qualification;
	if (QualifyingSection)
	{
		QualifyingSection->SetVisibility(bQualifying ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);
	}
	const TArray<int32>& QualifyingChoices = ApexQualifyingLength::ChoiceMinutes();
	for (int32 Index = 0; Index < QualifyingLengthButtons.Num(); ++Index)
	{
		if (QualifyingLengthButtons[Index] && QualifyingChoices.IsValidIndex(Index))
		{
			QualifyingLengthButtons[Index]->SetSelected(QualifyingChoices[Index] == Flow->CreateQualifyingMinutes);
		}
	}
	if (QualifyingInfoText)
	{
		const bool bTimedQualifying = Flow->EffectiveQualifyingSeconds() > 0;
		const FString Info = bShootout
			? FString(Flow->bCreateQualifyingCutoff ? TEXT("ONE CAR AT A TIME, ONE FLYING LAP · 107% OF POLE TO START")
				: TEXT("ONE CAR AT A TIME, ONE FLYING LAP EACH"))
			: FString(Flow->bCreateQualifyingCutoff
				? (bTimedQualifying ? TEXT("THEN THE FLAG · 107% OF POLE TO START") : TEXT("NO CLOCK · 107% OF POLE TO START"))
				: (bTimedQualifying ? TEXT("THEN THE FLAG · LAPS UNDER WAY COUNT") : TEXT("NO CLOCK · RUNS UNTIL THE HOST STARTS THE RACE")));
		QualifyingInfoText->SetText(FText::FromString(Info));
	}
	for (UApexButtonWidget* LengthChip : QualifyingLengthButtons)
	{
		// A shoot-out has no clock.
		if (LengthChip)
		{
			LengthChip->SetIsEnabled(!bShootout);
		}
	}

	if (PitRuleSection)
	{
		PitRuleSection->SetVisibility(Flow->PitRuleApplies() ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);
	}
	for (int32 Index = 0; Index < MandatoryStopsButtons.Num(); ++Index)
	{
		if (MandatoryStopsButtons[Index])
		{
			MandatoryStopsButtons[Index]->SetSelected(Index == ApexMandatoryStops::Clamp(Flow->CreateMandatoryStops));
		}
	}
	for (int32 Index = 0; Index < CompoundChangeButtons.Num(); ++Index)
	{
		if (CompoundChangeButtons[Index])
		{
			CompoundChangeButtons[Index]->SetSelected((Index == 1) == Flow->bCreateCompoundChange);
		}
	}
	if (PitRuleInfoText)
	{
		const FString Rule = ApexMandatoryStops::Describe(Flow->CreateMandatoryStops, Flow->bCreateCompoundChange);
		PitRuleInfoText->SetText(FText::FromString(Rule.IsEmpty()
			? FString(TEXT("NO RULE · STOP WHEN YOU LIKE"))
			: FString::Printf(TEXT("%s · OWED AT THE FLAG IS A PENALTY"), *Rule.ToUpper())));
	}

	if (RaceControlSection)
	{
		RaceControlSection->SetVisibility(Flow->PitRuleApplies() ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);
	}
	{
		const FApexRaceRules Rules = Flow->CreateRaceRules.Normalised();
		const int32 StartChip = Rules.Start == EApexStartType::Rolling ? 2 : Rules.bFormationLap ? 1 : 0;
		for (int32 Index = 0; Index < StartTypeButtons.Num(); ++Index)
		{
			if (StartTypeButtons[Index])
			{
				StartTypeButtons[Index]->SetSelected(Index == StartChip);
			}
		}
		const bool Penalised[] = { Rules.bJumpStart, Rules.bYellowPenalty, Rules.bPitExitPenalty };
		for (int32 Index = 0; Index < PenaltyButtons.Num() && Index < UE_ARRAY_COUNT(Penalised); ++Index)
		{
			if (PenaltyButtons[Index])
			{
				PenaltyButtons[Index]->SetSelected(Penalised[Index]);
			}
		}
		for (int32 Index = 0; Index < SafetyCarButtons.Num(); ++Index)
		{
			if (SafetyCarButtons[Index])
			{
				SafetyCarButtons[Index]->SetSelected(Index == static_cast<int32>(Rules.SafetyCar));
			}
		}
		const int32 ReverseChip = ApexCreateSession::ReverseGridChip(Rules.ReverseGrid);
		for (int32 Index = 0; Index < ReverseGridButtons.Num(); ++Index)
		{
			if (ReverseGridButtons[Index])
			{
				ReverseGridButtons[Index]->SetSelected(Index == ReverseChip);
			}
		}
		if (RaceControlInfoText)
		{
			// The summary's words, with the start and the penalties said at their defaults too.
			const FApexRaceRules Sent = Flow->RaceRulesToSend();
			FApexRaceRules Rest = Sent;
			Rest.Start = EApexStartType::Standing;
			Rest.bFormationLap = false;
			Rest.bJumpStart = Rest.bYellowPenalty = Rest.bPitExitPenalty = false;
			TArray<FString> Parts;
			Parts.Add(Sent.Start == EApexStartType::Rolling ? TEXT("rolling start")
				: Sent.bFormationLap ? TEXT("formation lap") : TEXT("standing start"));
			const int32 Penalties = Sent.CountPenalties();
			Parts.Add(Penalties == 0 ? FString(TEXT("no penalties"))
				: FString::Printf(TEXT("%d penalt%s"), Penalties, Penalties == 1 ? TEXT("y") : TEXT("ies")));
			if (const FString Others = Rest.Describe(); !Others.IsEmpty())
			{
				Parts.Add(Others);
			}
			RaceControlInfoText->SetText(FText::FromString(FString::Join(Parts, TEXT(" · ")).ToUpper()));
		}
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

	// The sky through the session: the clock's speed, the forecast, the rubber.
	{
		const int32 Clock = ApexCreateSession::ClockScaleIndex(Conditions);
		for (int32 Index = 0; Index < ClockScaleButtons.Num(); ++Index)
		{
			if (ClockScaleButtons[Index]) { ClockScaleButtons[Index]->SetSelected(Index == Clock); }
		}
		const int32 Changes = ApexCreateSession::ChangeableIndex(Conditions);
		for (int32 Index = 0; Index < ChangeableButtons.Num(); ++Index)
		{
			if (ChangeableButtons[Index]) { ChangeableButtons[Index]->SetSelected(Index == Changes); }
		}
		const int32 Rubber = ApexCreateSession::TrackRubberIndex(Conditions);
		for (int32 Index = 0; Index < TrackRubberButtons.Num(); ++Index)
		{
			if (TrackRubberButtons[Index]) { TrackRubberButtons[Index]->SetSelected(Index == Rubber); }
		}
		// The forecast as the server will follow it: the host's written one
		// (kept to its sky, as the server keeps it), else the one laid out
		// from the seed, over the circuit in the date's season.
		float Latitude = ApexSky::DefaultLatitudeDeg;
		const FApexSessionConditions AtSite = ConditionsAtSite(Latitude);
		if (ForecastText)
		{
			FString Line;
			if (AtSite.HasForecast())
			{
				const TArray<FApexForecastStep> Kept = ApexCreateSession::KeptToSky(AtSite, Latitude);
				Line = FString::Printf(TEXT("Written: %s"), *ApexCreateSession::ForecastLine(AtSite, ApexCreateSession::WrittenForecast(Kept),
					FApexSessionConditions::MaxForecastSteps));
				if (Kept != AtSite.Forecast)
				{
					Line += ApexCreateSession::IsWinter(AtSite, Latitude)
						? TEXT("  (a winter sky: its rain falls as snow)") : TEXT("  (no winter sky: its snow falls as rain)");
				}
			}
			else if (!Conditions.WeatherChanges())
			{
				Line = TEXT("The sky holds. Add a step to write a forecast.");
			}
			else if (!Conditions.HasSkySeed())
			{
				// A profile from before seeds: the server lays it out from the session's id.
				Line = TEXT("Laid out when the session is made.");
			}
			else
			{
				Line = ApexCreateSession::ForecastLine(AtSite, ApexCreateSession::Forecast(AtSite, AtSite.SkySeed, Latitude));
				if (Line.IsEmpty())
				{
					Line = TEXT("No change through the day.");
				}
			}
			ForecastText->SetText(FText::FromString(Line));
		}
		if (ForecastRollButton)
		{
			ForecastRollButton->SetIsEnabled(Conditions.WeatherChanges() && !Conditions.HasForecast());
		}
		const TArray<FApexForecastStep>& Steps = Conditions.Forecast;
		if (ForecastAddButton)
		{
			FApexSessionConditions Probe = Conditions;
			ForecastAddButton->SetIsEnabled(ApexCreateSession::AddForecastStep(Probe.Forecast, Conditions.Weather));
		}
		for (int32 Step = 0; Step < ForecastStepRows.Num(); ++Step)
		{
			const bool bShown = Steps.IsValidIndex(Step);
			if (ForecastStepRows[Step])
			{
				ForecastStepRows[Step]->SetVisibility(bShown ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);
			}
			if (!bShown)
			{
				continue;
			}
			int32 Lo = 0;
			int32 Hi = 0;
			ApexCreateSession::StepBounds(Steps, Step, Lo, Hi);
			FString When = ApexCreateSession::StepMinuteLabel(Steps[Step].AtMin);
			if (Conditions.ClockRuns())
			{
				When += TEXT("  ") + ApexCreateSession::ClockAt(Conditions, Steps[Step].AtMin * 60.0f);
			}
			if (ForecastMinuteText[Step]) { ForecastMinuteText[Step]->SetText(FText::FromString(When)); }
			if (ForecastMinuteMinus[Step]) { ForecastMinuteMinus[Step]->SetIsEnabled(Steps[Step].AtMin > Lo); }
			if (ForecastMinutePlus[Step])  { ForecastMinutePlus[Step]->SetIsEnabled(Steps[Step].AtMin < Hi); }
			if (ForecastWeatherText[Step])
			{
				ForecastWeatherText[Step]->SetText(FText::FromString(FApexSessionConditions::WeatherLabel(Steps[Step].Weather)));
			}
		}
	}

	{
		float AirLatitude = ApexSky::DefaultLatitudeDeg;
		const int32 Air = ApexCreateSession::AirTempC(ConditionsAtSite(AirLatitude), AirLatitude);
		if (AirAutoButton) { AirAutoButton->SetSelected(!Conditions.HasAirTemp()); }
		if (AirValueText)  { AirValueText->SetText(FText::FromString(FString::Printf(TEXT("%d °C"), Air))); }
		if (AirNoteText)
		{
			AirNoteText->SetText(FText::FromString(Conditions.HasAirTemp()
				? FString(DescribeAir(Air))
				: Conditions.HasDate() ? FString(TEXT("FROM WEATHER, TIME AND SEASON")) : FString(TEXT("FROM WEATHER AND TIME"))));
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
	RefreshStartOrder();
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
	const int32 YourSlotNumber = CurrentOrder().IndexOfByKey(ApexCreateSession::HostRef()) + 1;

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
			Hint = FString::Printf(TEXT("%d CARS · %d AI · %d OPEN · YOU START P%d"), G.Field, G.Ai, G.Open, YourSlotNumber);
		}
		else
		{
			Hint = G.Ai > 0
				? (YourSlotNumber == G.Ai + 1
					? FString::Printf(TEXT("%d CARS · YOU START P%d, BEHIND THE AI"), G.Field, YourSlotNumber)
					: FString::Printf(TEXT("%d CARS · YOU START P%d"), G.Field, YourSlotNumber))
				: FString(TEXT("1 CAR · JUST YOU"));
		}
		GridHintText->SetText(FText::FromString(Hint));
	}

	const TArray<ApexCreateSession::FGridEntry> Entries = ApexCreateSession::Entries(CurrentOrder(), GridDriverNames());

	for (int32 Index = 0; Index < SlotFaces.Num(); ++Index)
	{
		// The occupied slots come from the order (the AI, you and anyone a
		// stored result names, wherever they sit); the rest are open seats
		// in a multiplayer field and past the grid otherwise.
		const bool bOccupied = Entries.IsValidIndex(Index);
		const ApexCreateSession::ESlot Kind = bOccupied
			? (Entries[Index].bYou ? ApexCreateSession::ESlot::You : ApexCreateSession::ESlot::Ai)
			: (Index < G.Field && bMultiplayer ? ApexCreateSession::ESlot::Open : ApexCreateSession::ESlot::None);
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
			Name = Entries[Index].Label;
			break;
		case ApexCreateSession::ESlot::Ai:
			Fill = ApexUI::Palette::SurfaceHover;
			Name = Entries[Index].Label;
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
		// A driver picked up by a click, dragged, or picked for the steppers
		// stands out; so does the slot a drag would drop it on.
		if (bOccupied && (Index == PickedSlot || Index == DragFrom || (!MovingRef.IsEmpty() && Entries[Index].Ref == MovingRef)))
		{
			Outline = ApexUI::Palette::TextPrimary;
		}
		else if (bOccupied && DragFrom != INDEX_NONE && Index == DragHover)
		{
			Outline = ApexUI::Palette::Accent;
			Fill = ApexUI::Palette::SurfaceHover;
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

FApexSessionConditions UApexSessionCreateWidget::ConditionsAtSite(float& OutLatitudeDeg) const
{
	OutLatitudeDeg = ApexSky::DefaultLatitudeDeg;
	const UApexMenuFlowSubsystem* Flow = GetFlow();
	if (!Flow)
	{
		return FApexSessionConditions();
	}
	FApexSessionConditions C = Flow->CreateConditions.Clamped();
	const FString TrackId = Flow->GetPendingTrackId();
	FApexTrackCatalogRow Row;
	if (Flow->GetTrackCatalogRow(TrackId, Row) && Row.bHasLocation)
	{
		OutLatitudeDeg = Row.LatitudeDeg;
	}
	const UApexNetSubsystem* Net = GetNet();
	const FApexTrackConfigSummary* Site = Net ? Net->GetCachedLobbyState().TrackConfigs.FindByPredicate(
		[&TrackId](const FApexTrackConfigSummary& Track) { return Track.bHasSite && Track.Id.Equals(TrackId, ESearchCase::IgnoreCase); }) : nullptr;
	if (Site)
	{
		OutLatitudeDeg = Site->LatitudeDeg;
		C.SolarNoonMin = ApexSky::SolarNoonMinutes(Site->LatitudeDeg, Site->LongitudeDeg, C.HasDate() ? C.DayOfYear : ApexSky::ModelDayOfYear);
	}
	return C;
}

void UApexSessionCreateWidget::RefreshSkyPreview()
{
	const UApexMenuFlowSubsystem* Flow = GetFlow();
	if (!Flow)
	{
		return;
	}

	float Latitude = ApexSky::DefaultLatitudeDeg;
	const FApexSessionConditions C = ConditionsAtSite(Latitude);
	const ApexCreateSession::FSkyLook Look = ApexCreateSession::Sky(C, Latitude);

	const int32 Day = C.HasDate() ? C.DayOfYear : -1;
	if (DateValue) { DateValue->SetText(FText::FromString(ApexCreateSession::DateLabel(Day))); }
	if (DateMinus) { DateMinus->SetIsEnabled(Day > 0); }
	if (DatePlus)  { DatePlus->SetIsEnabled(Day < ApexCreateSession::DateChoices().Last()); }
	if (DateNote)
	{
		FApexSessionConditions Noon;
		Noon.TimeOfDayMinutes = C.HasSolarNoon() ? C.SolarNoonMin : FMath::RoundToInt(ApexSky::SolarNoonHours * 60.0f);
		DateNote->SetText(FText::FromString(FString::Printf(TEXT("SOLAR NOON %s%s · %s AT %s"),
			*Noon.ClockText(), C.HasSolarNoon() ? TEXT(" HERE") : TEXT(""), Look.Phase, *C.ClockText())));
	}
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
	if (SkyAirText)   { SkyAirText->SetText(FText::FromString(FString::Printf(TEXT("%d °C"), ApexCreateSession::AirTempC(C, Latitude)))); }
	if (SkyTrackText) { SkyTrackText->SetText(FText::FromString(FString::Printf(TEXT("%d °C"), FMath::RoundToInt(ApexCreateSession::TrackTempC(C, Latitude))))); }
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
		Parts.Add(Flow->IsCreatingWeekend() ? FString(TEXT("RACE WEEKEND")) : FString(ModeLabel(Mode)).ToUpper());
		if (Mode == EApexGameMode::Race || Mode == EApexGameMode::Qualification)
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
		if (Flow->QualifyingFormatToSend() == EApexQualifyingFormat::Shootout)
		{
			Parts.Add(Flow->IsCreatingWeekend() && Flow->CreateWeekendRounds > 1 ? TEXT("FINAL ROUND A SHOOT-OUT") : TEXT("SHOOT-OUT"));
		}
		if (const int32 Allowance = Flow->QualifyingLapAllowanceToSend(); Allowance > 0)
		{
			Parts.Add(FString::Printf(TEXT("%s ALLOWED"), *ApexCreateSession::LapAllowanceLabel(Allowance).ToUpper()));
		}
		if (Flow->QualifyingCutoffToSend() != ApexQualifyingCutoff::None)
		{
			Parts.Add(TEXT("107% RULE"));
		}
		if (const FString PitRule = ApexMandatoryStops::Describe(Flow->MandatoryStopsToSend(), Flow->CompoundChangeToSend());
			!PitRule.IsEmpty())
		{
			Parts.Add(PitRule.ToUpper());
		}
		if (const FString Rules = Flow->RaceRulesToSend().Describe(); !Rules.IsEmpty())
		{
			Parts.Add(Rules.ToUpper());
		}
		Parts.Add(DamageOptions[FMath::Clamp(static_cast<int32>(Flow->CreateDamage), 0, static_cast<int32>(UE_ARRAY_COUNT(DamageOptions)) - 1)].Summary);
		{
			// A weekend phase's own rule, when it has one.
			const FApexWeekendFormat Weekend = Flow->WeekendToSend();
			if (Weekend.bHasPracticeDamage)
			{
				Parts.Add(FString::Printf(TEXT("PRACTICE %s"), DamageOptions[static_cast<int32>(Weekend.PracticeDamage)].Summary));
			}
			if (Weekend.bHasQualifyingDamage)
			{
				Parts.Add(FString::Printf(TEXT("QUALIFYING %s"), DamageOptions[static_cast<int32>(Weekend.QualifyingDamage)].Summary));
			}
		}
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
	// The first lobby state means the connection is up: the screen may have
	// opened before it was, with the track's results still to ask for.
	RequestQualifying();
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

// --- The start order -------------------------------------------------------------

int32 UApexSessionCreateWidget::MovingIndex(const TArray<FString>& Order) const
{
	const int32 At = MovingRef.IsEmpty() ? INDEX_NONE : Order.IndexOfByKey(MovingRef);
	return At != INDEX_NONE ? At : Order.IndexOfByKey(ApexCreateSession::HostRef());
}

TArray<FString> UApexSessionCreateWidget::CurrentOrder() const
{
	const UApexMenuFlowSubsystem* Flow = GetFlow();
	if (!Flow)
	{
		return ApexCreateSession::DefaultOrder(0);
	}
	const ApexCreateSession::FGrid G = ApexCreateSession::Grid(
		Flow->CreateSessionKind != EApexSessionKind::Practice, Flow->CreateMaxPlayers, Flow->CreateAiCount, MaxPlayersCeiling);
	return ApexCreateSession::Normalise(Flow->CreateGridOrder, G.Ai, G.Field);
}

void UApexSessionCreateWidget::StoreOrder(const TArray<FString>& Order, const FString& ResultId)
{
	UApexMenuFlowSubsystem* Flow = GetFlow();
	if (!Flow)
	{
		return;
	}
	const ApexCreateSession::FGrid G = ApexCreateSession::Grid(
		Flow->CreateSessionKind != EApexSessionKind::Practice, Flow->CreateMaxPlayers, Flow->CreateAiCount, MaxPlayersCeiling);
	Flow->CreateGridTrackId = Flow->GetPendingTrackId();
	if (ApexCreateSession::IsDefaultOrder(Order, G.Ai, G.Field) && ResultId.IsEmpty())
	{
		// Just the seating order: nothing to send, nothing to remember. A
		// loaded result is remembered even when it comes out as the seating
		// order (an all-AI result always does: its AI map onto this session's
		// in the order they qualified), or its chip would not light up and
		// the click would seem to do nothing. GridOrderToSend still sends
		// nothing for it.
		Flow->CreateGridOrder.Reset();
		Flow->CreateGridResultId.Reset();
		return;
	}
	Flow->CreateGridOrder = ApexCreateSession::Normalise(Order, G.Ai, G.Field);
	Flow->CreateGridResultId = ResultId;
}

bool UApexSessionCreateWidget::IsGridInteractive() const
{
	const UApexMenuFlowSubsystem* Flow = GetFlow();
	return Flow && ActiveTab == 0 && Flow->CreateStartingMode != EApexGameMode::Hotlap
		&& GridSlotsPanel && GridSlotsPanel->GetVisibility() != ESlateVisibility::Collapsed;
}

int32 UApexSessionCreateWidget::SlotUnder(const FVector2D& ScreenPosition) const
{
	if (!IsGridInteractive())
	{
		return INDEX_NONE;
	}
	for (int32 Index = 0; Index < SlotFaces.Num(); ++Index)
	{
		if (SlotFaces[Index] && SlotFaces[Index]->GetCachedGeometry().IsUnderLocation(ScreenPosition))
		{
			return Index;
		}
	}
	return INDEX_NONE;
}

void UApexSessionCreateWidget::ClickSlot(int32 Clicked)
{
	const int32 Occupied = CurrentOrder().Num();
	if (Clicked < Occupied)
	{
		// A driver: pick it up, or drop the one picked up here.
		if (PickedSlot == INDEX_NONE)
		{
			PickedSlot = Clicked;
		}
		else if (PickedSlot == Clicked)
		{
			PickedSlot = INDEX_NONE;
		}
		else
		{
			const int32 From = PickedSlot;
			PickedSlot = INDEX_NONE;
			DropSlot(From, Clicked);
			return;
		}
		RefreshGridPreview();
		return;
	}
	// A seat past the drivers sets the field, as a click on the grid always did.
	PickedSlot = INDEX_NONE;
	PickGridSlot(Clicked + 1);
}

void UApexSessionCreateWidget::DropSlot(int32 From, int32 To)
{
	TArray<FString> Order = CurrentOrder();
	// Dropped on a seat past the drivers: last of them.
	To = FMath::Min(To, Order.Num() - 1);
	if (ApexCreateSession::MoveEntry(Order, From, To))
	{
		StoreOrder(Order, FString());
	}
	PickedSlot = INDEX_NONE;
	Changed();
}

FReply UApexSessionCreateWidget::NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	if (InMouseEvent.GetEffectingButton() == EKeys::LeftMouseButton)
	{
		const int32 Pressed = SlotUnder(InMouseEvent.GetScreenSpacePosition());
		if (Pressed != INDEX_NONE)
		{
			DragFrom = Pressed;
			DragHover = Pressed;
			return FReply::Handled().CaptureMouse(TakeWidget());
		}
	}
	return Super::NativeOnMouseButtonDown(InGeometry, InMouseEvent);
}

FReply UApexSessionCreateWidget::NativeOnMouseMove(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	if (DragFrom != INDEX_NONE)
	{
		const int32 Hover = SlotUnder(InMouseEvent.GetScreenSpacePosition());
		if (Hover != DragHover)
		{
			DragHover = Hover;
			RefreshGridPreview();
		}
		return FReply::Handled();
	}
	return Super::NativeOnMouseMove(InGeometry, InMouseEvent);
}

FReply UApexSessionCreateWidget::NativeOnMouseButtonUp(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	if (DragFrom != INDEX_NONE && InMouseEvent.GetEffectingButton() == EKeys::LeftMouseButton)
	{
		const int32 From = DragFrom;
		const int32 To = SlotUnder(InMouseEvent.GetScreenSpacePosition());
		DragFrom = INDEX_NONE;
		DragHover = INDEX_NONE;
		if (To == From)
		{
			ClickSlot(From);
		}
		else if (To != INDEX_NONE && From < CurrentOrder().Num())
		{
			DropSlot(From, To);
		}
		else
		{
			RefreshGridPreview();
		}
		return FReply::Handled().ReleaseMouseCapture();
	}
	return Super::NativeOnMouseButtonUp(InGeometry, InMouseEvent);
}

void UApexSessionCreateWidget::NativeOnMouseCaptureLost(const FCaptureLostEvent& CaptureLostEvent)
{
	if (DragFrom != INDEX_NONE)
	{
		DragFrom = INDEX_NONE;
		DragHover = INDEX_NONE;
		RefreshGridPreview();
	}
	Super::NativeOnMouseCaptureLost(CaptureLostEvent);
}

void UApexSessionCreateWidget::RequestQualifying()
{
	const UApexMenuFlowSubsystem* Flow = GetFlow();
	UApexNetSubsystem* Net = GetNet();
	if (Flow && Net && Flow->HasPendingTrack() && Net->IsAuthenticated()
		&& !QualifyingRequestedFor.Equals(Flow->GetPendingTrackId(), ESearchCase::IgnoreCase))
	{
		QualifyingRequestedFor = Flow->GetPendingTrackId();
		Net->RequestQualifyingResults(QualifyingRequestedFor);
	}
}

TMap<FString, FString> UApexSessionCreateWidget::GridDriverNames() const
{
	const UApexMenuFlowSubsystem* Flow = GetFlow();
	const UApexNetSubsystem* Net = GetNet();
	return Flow && Net ? ApexCreateSession::DriverNames(Net->GetQualifyingResults(Flow->GetPendingTrackId())) : TMap<FString, FString>();
}

void UApexSessionCreateWidget::LoadResult(const FApexQualifyingResult& Result)
{
	const UApexMenuFlowSubsystem* Flow = GetFlow();
	const UApexNetSubsystem* Net = GetNet();
	if (!Flow || !Net)
	{
		return;
	}
	const ApexCreateSession::FGrid G = ApexCreateSession::Grid(
		Flow->CreateSessionKind != EApexSessionKind::Practice, Flow->CreateMaxPlayers, Flow->CreateAiCount, MaxPlayersCeiling);
	StoreOrder(ApexCreateSession::Normalise(
		ApexCreateSession::OrderFromResult(Result, Net->GetPlayerName(), G.Ai, Net->GetDriverId()), G.Ai, G.Field), Result.Id);
}

void UApexSessionCreateWidget::HandleQualifyingResults(const FString& TrackId)
{
	const UApexMenuFlowSubsystem* Flow = GetFlow();
	const UApexNetSubsystem* Net = GetNet();
	if (Flow && Net && Flow->GetPendingTrackId().Equals(TrackId, ESearchCase::IgnoreCase))
	{
		// A screenshot run can load a stored result: -ApexCreateLoadResult=N is
		// the Nth, newest first (once, when the list first arrives).
		int32 LoadAt = INDEX_NONE;
		const TArray<FApexQualifyingResult>& Results = Net->GetQualifyingResults(TrackId);
		if (!bLoadedFromSwitch && FParse::Value(FCommandLine::Get(), TEXT("ApexCreateLoadResult="), LoadAt) && Results.IsValidIndex(LoadAt))
		{
			bLoadedFromSwitch = true;
			LoadResult(Results[LoadAt]);
		}
		RefreshSettings();
	}
}

void UApexSessionCreateWidget::RefreshStartOrder()
{
	UApexMenuFlowSubsystem* Flow = GetFlow();
	const UApexNetSubsystem* Net = GetNet();
	if (!Flow || !StartOrderRows)
	{
		return;
	}

	// A weekend's grid is its qualifying's.
	const bool bRace = Flow->CreateStartingMode == EApexGameMode::Race && !Flow->IsCreatingWeekend();
	if (StartOrderSection)
	{
		StartOrderSection->SetVisibility(bRace ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);
	}

	// An order is made for one track: another track starts from the seating
	// order again, and this one's stored results are asked for.
	const FString TrackId = Flow->GetPendingTrackId();
	if (!Flow->CreateGridTrackId.Equals(TrackId, ESearchCase::IgnoreCase))
	{
		Flow->CreateGridOrder.Reset();
		Flow->CreateGridResultId.Reset();
		Flow->CreateGridTrackId = TrackId;
		PickedSlot = INDEX_NONE;
		RequestQualifying();
	}

	const bool bMultiplayer = Flow->CreateSessionKind != EApexSessionKind::Practice;
	const ApexCreateSession::FGrid G = ApexCreateSession::Grid(bMultiplayer, Flow->CreateMaxPlayers, Flow->CreateAiCount, MaxPlayersCeiling);
	const TArray<FString> Order = CurrentOrder();
	const bool bDefaultOrder = ApexCreateSession::IsDefaultOrder(Flow->CreateGridOrder, G.Ai, G.Field);
	const TArray<FApexQualifyingResult> Empty;
	const TArray<FApexQualifyingResult>& Results = Net ? Net->GetQualifyingResults(TrackId) : Empty;

	// The newest few, and the loaded one wherever it is in the list.
	int32 Loaded = INDEX_NONE;
	if (!Flow->CreateGridResultId.IsEmpty())
	{
		Loaded = Results.IndexOfByPredicate([&](const FApexQualifyingResult& R) { return R.Id == Flow->CreateGridResultId; });
	}
	const bool bSeating = bDefaultOrder && Loaded == INDEX_NONE;
	const bool bCustom = !bDefaultOrder && Loaded == INDEX_NONE;
	TArray<int32> Shown;
	for (int32 Index = 0; Index < Results.Num() && Shown.Num() < MaxResultsShown; ++Index)
	{
		Shown.Add(Index);
	}
	if (Loaded != INDEX_NONE && !Shown.Contains(Loaded))
	{
		Shown.Last() = Loaded;
	}

	auto When = [](const FString& Stamp)
	{
		FDateTime Utc;
		if (!FDateTime::ParseIso8601(*Stamp, Utc))
		{
			return FString();
		}
		const FDateTime Local = Utc + (FDateTime::Now() - FDateTime::UtcNow());
		static const TCHAR* const Months[] = {
			TEXT("Jan"), TEXT("Feb"), TEXT("Mar"), TEXT("Apr"), TEXT("May"), TEXT("Jun"),
			TEXT("Jul"), TEXT("Aug"), TEXT("Sep"), TEXT("Oct"), TEXT("Nov"), TEXT("Dec"),
		};
		return FString::Printf(TEXT("%02d %s %02d:%02d"), Local.GetDay(), Months[FMath::Clamp(Local.GetMonth() - 1, 0, 11)],
			Local.GetHour(), Local.GetMinute());
	};

	FString Signature = TrackId + (bCustom ? TEXT("|custom") : TEXT(""));
	for (const int32 Index : Shown)
	{
		Signature += FString::Printf(TEXT("|%s:%d"), *Results[Index].Id, Results[Index].Entries.Num());
	}

	if (Signature != StartOrderSignature || StartOrderButtons.IsEmpty())
	{
		StartOrderSignature = Signature;
		const bool bHadFocus = StartOrderButtons.ContainsByPredicate(
			[](const TObjectPtr<UApexButtonWidget>& Button) { return Button && Button->HasKeyboardFocus(); });

		StartOrderRows->ClearChildren();
		StartOrderButtons.Reset();
		StartOrderIds.Reset();

		// The pole sitter goes under the label: a chip is a third of the column,
		// and a name and a time beside the label ran into it.
		auto Add = [this](const FString& Label, const FString& SubLabel, const FString& Id)
		{
			FApexButtonSpec Spec;
			Spec.Label = Label;
			Spec.SubLabel = SubLabel;
			Spec.Variant = EApexButtonVariant::Panel;
			Spec.LabelSize = 16.0f;
			Spec.Height = 58.0f;
			StartOrderButtons.Add(MakeButton(Spec));
			StartOrderIds.Add(Id);
		};
		Add(TEXT("Default"), TEXT("AI ahead"), FString());
		if (bCustom)
		{
			Add(TEXT("Custom"), TEXT("Edited"), CustomOrderId);
		}
		for (const int32 Index : Shown)
		{
			const FApexQualifyingResult& Result = Results[Index];
			const FString Pole = Result.Entries.IsEmpty() ? FString() : Result.Entries[0].Name;
			const FString Stamp = When(Result.RecordedAt);
			Add(Stamp.IsEmpty()
					? FString::Printf(TEXT("Qualifying · %s"), *ApexCatalog::DisplayClass(Result.Class))
					: FString::Printf(TEXT("%s · %s"), *ApexCatalog::DisplayClass(Result.Class), *Stamp),
				Result.Entries.IsEmpty() ? FString(TEXT("Empty"))
					: FString::Printf(TEXT("P1 %s %s"), *Pole.Left(12).ToUpper(), *UApexMenuFlowSubsystem::FormatLapTime(Result.Entries[0].LapTimeMs / 1000.0f)),
				Result.Id);
		}

		// Three to a row, a short last row keeping the thirds.
		UHorizontalBox* Row = nullptr;
		for (int32 Index = 0; Index < StartOrderButtons.Num(); ++Index)
		{
			if (Index % 3 == 0)
			{
				Row = WidgetTree->ConstructWidget<UHorizontalBox>();
				ApexUI::AddV(StartOrderRows, Row, FMargin(0.0f, Index == 0 ? 0.0f : 6.0f, 0.0f, 0.0f));
			}
			ApexUI::AddH(Row, StartOrderButtons[Index], FMargin(Index % 3 == 0 ? 0.0f : 6.0f, 0.0f, 0.0f, 0.0f), VAlign_Fill, 1.0f);
		}
		for (int32 Pad = StartOrderButtons.Num() % 3; Row && Pad != 0 && Pad < 3; ++Pad)
		{
			ApexUI::AddH(Row, WidgetTree->ConstructWidget<USpacer>(), FMargin(6.0f, 0.0f, 0.0f, 0.0f), VAlign_Fill, 1.0f);
		}

		if (bHadFocus)
		{
			// Back onto the chip now in use, wherever the rebuild put it.
			const FString Selected = bSeating ? FString() : bCustom ? FString(CustomOrderId) : Flow->CreateGridResultId;
			const int32 At = StartOrderIds.IndexOfByKey(Selected);
			ApexNav::Focus(StartOrderButtons.IsValidIndex(At) ? StartOrderButtons[At].Get() : StartOrderButtons[0].Get());
		}
	}

	for (int32 Index = 0; Index < StartOrderButtons.Num(); ++Index)
	{
		const FString& Id = StartOrderIds[Index];
		const bool bSelected = Id.IsEmpty() ? bSeating
			: Id == CustomOrderId ? bCustom
			: Loaded != INDEX_NONE && Results[Loaded].Id == Id;
		StartOrderButtons[Index]->SetSelected(bSelected);
	}

	const int32 Mine = MovingIndex(Order);
	if (MovingNameText)
	{
		const TArray<ApexCreateSession::FGridEntry> Drivers = ApexCreateSession::Entries(Order, GridDriverNames());
		MovingNameText->SetText(FText::FromString(Drivers.IsValidIndex(Mine) ? Drivers[Mine].Label : FString(TEXT("YOU"))));
	}
	if (MovingPrev) { MovingPrev->SetIsEnabled(Order.Num() > 1); }
	if (MovingNext) { MovingNext->SetIsEnabled(Order.Num() > 1); }
	if (YourSlotValueText)
	{
		YourSlotValueText->SetText(FText::FromString(FString::Printf(TEXT("P%d"), Mine + 1)));
	}
	if (YourSlotEarlier) { YourSlotEarlier->SetIsEnabled(Mine > 0); }
	if (YourSlotLater)   { YourSlotLater->SetIsEnabled(Mine >= 0 && Mine + 1 < Order.Num()); }

	if (StartOrderInfoText)
	{
		StartOrderInfoText->SetText(FText::FromString(Results.IsEmpty()
			? FString(TEXT("DRAG THE GRID · NO QUALIFYING SET HERE YET"))
			: Results.Num() > Shown.Num()
				? FString::Printf(TEXT("DRAG THE GRID · %d STORED · NEWEST %d SHOWN"), Results.Num(), Shown.Num())
				: FString::Printf(TEXT("DRAG THE GRID · %d STORED FOR THIS TRACK"), Results.Num())));
	}
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
			Flow->IsCreatingWeekend() ? TArray<FString>() : Flow->GridOrderToSend(MaxPlayersCeiling),
			Flow->EffectiveQualifyingSeconds(),
			Flow->WeekendToSend(),
			Flow->QualifyingCutoffToSend(),
			Flow->MandatoryStopsToSend(),
			Flow->CompoundChangeToSend(),
			Flow->QualifyingFormatToSend(),
			Flow->QualifyingLapAllowanceToSend(),
			Flow->RaceRulesToSend());
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
		Flow->bCreateWeekend = ModeOptions[Mode].bWeekend;
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
	if (const int32 Practice = ApexNav::IndexOf(WeekendPracticeButtons, Button); Practice != INDEX_NONE)
	{
		Flow->CreateWeekendPracticeMinutes = WeekendPracticeChoices[Practice];
		Changed();
		return;
	}
	if (const int32 Rounds = ApexNav::IndexOf(WeekendRoundsButtons, Button); Rounds != INDEX_NONE)
	{
		Flow->CreateWeekendRounds = Rounds + 1;
		Changed();
		return;
	}
	if (const int32 RoundLength = ApexNav::IndexOf(WeekendRoundLengthButtons, Button); RoundLength != INDEX_NONE)
	{
		Flow->CreateWeekendRoundMinutes = WeekendRoundChoices[RoundLength];
		Changed();
		return;
	}
	for (int32 Day = 0; Day < 2; ++Day)
	{
		int32& Weather = Day == 0 ? Flow->CreateWeekendPracticeWeather : Flow->CreateWeekendQualifyingWeather;
		int32& Hour = Day == 0 ? Flow->CreateWeekendPracticeHour : Flow->CreateWeekendQualifyingHour;
		if (Button == WeekendDayWeatherMinus[Day] || Button == WeekendDayWeatherPlus[Day])
		{
			// -1 (the race day's), then every weather in order.
			Weather = FMath::Clamp(Weather + (Button == WeekendDayWeatherPlus[Day] ? 1 : -1), -1, FApexSessionConditions::WeatherCount - 1);
			Changed();
			return;
		}
		if (Button == WeekendDayHourMinus[Day] || Button == WeekendDayHourPlus[Day])
		{
			// -1 (the race's hour), then the choices in order.
			const TArray<int32>& Hours = ApexCreateSession::DayHourChoices();
			int32 Index = Hours.IndexOfByKey(Hour);
			Index = FMath::Clamp(Index + (Button == WeekendDayHourPlus[Day] ? 1 : -1), -1, Hours.Num() - 1);
			Hour = Index >= 0 ? Hours[Index] : -1;
			Changed();
			return;
		}
	}
	if (const int32 Stops = ApexNav::IndexOf(MandatoryStopsButtons, Button); Stops != INDEX_NONE)
	{
		Flow->CreateMandatoryStops = ApexMandatoryStops::Clamp(Stops);
		Changed();
		return;
	}
	if (const int32 Compound = ApexNav::IndexOf(CompoundChangeButtons, Button); Compound != INDEX_NONE)
	{
		Flow->bCreateCompoundChange = Compound == 1;
		Changed();
		return;
	}
	if (const int32 Start = ApexNav::IndexOf(StartTypeButtons, Button); Start != INDEX_NONE)
	{
		Flow->CreateRaceRules.Start = Start == 2 ? EApexStartType::Rolling : EApexStartType::Standing;
		Flow->CreateRaceRules.bFormationLap = Start == 1;
		Changed();
		return;
	}
	if (const int32 Penalty = ApexNav::IndexOf(PenaltyButtons, Button); Penalty != INDEX_NONE)
	{
		bool& Rule = Penalty == 0 ? Flow->CreateRaceRules.bJumpStart
			: Penalty == 1 ? Flow->CreateRaceRules.bYellowPenalty : Flow->CreateRaceRules.bPitExitPenalty;
		Rule = !Rule;
		Changed();
		return;
	}
	if (const int32 SafetyCar = ApexNav::IndexOf(SafetyCarButtons, Button); SafetyCar != INDEX_NONE)
	{
		Flow->CreateRaceRules.SafetyCar = static_cast<EApexSafetyCar>(FMath::Clamp(SafetyCar, 0, 2));
		Changed();
		return;
	}
	if (const int32 Reverse = ApexNav::IndexOf(ReverseGridButtons, Button); Reverse != INDEX_NONE)
	{
		Flow->CreateRaceRules.ReverseGrid = static_cast<uint8>(ApexCreateSession::ReverseGridChoices[FMath::Clamp(Reverse, 0, static_cast<int32>(UE_ARRAY_COUNT(ApexCreateSession::ReverseGridChoices)) - 1)]);
		Changed();
		return;
	}
	if (const int32 Format = FMath::Max(ApexNav::IndexOf(QualifyingFormatButtons, Button), ApexNav::IndexOf(WeekendFormatButtons, Button));
		Format != INDEX_NONE)
	{
		Flow->CreateQualifyingFormat = Format == 1 ? EApexQualifyingFormat::Shootout : EApexQualifyingFormat::Open;
		Changed();
		return;
	}
	if (Button == QualifyingAllowanceMinus || Button == QualifyingAllowancePlus
		|| Button == WeekendAllowanceMinus || Button == WeekendAllowancePlus)
	{
		Flow->CreateQualifyingLapAllowance = ApexCreateSession::StepLapAllowance(Flow->CreateQualifyingLapAllowance,
			Button == QualifyingAllowancePlus || Button == WeekendAllowancePlus ? 1 : -1);
		Changed();
		return;
	}
	if (const int32 Chip = ApexNav::IndexOf(WeekendPracticeDamageButtons, Button); Chip != INDEX_NONE)
	{
		Flow->CreateWeekendPracticeDamage = ApexCreateSession::PhaseDamageOfChip(Chip);
		Changed();
		return;
	}
	if (const int32 Chip = ApexNav::IndexOf(WeekendQualifyingDamageButtons, Button); Chip != INDEX_NONE)
	{
		Flow->CreateWeekendQualifyingDamage = ApexCreateSession::PhaseDamageOfChip(Chip);
		Changed();
		return;
	}
	if (const int32 Cutoff = FMath::Max(ApexNav::IndexOf(QualifyingCutoffButtons, Button), ApexNav::IndexOf(WeekendCutoffButtons, Button));
		Cutoff != INDEX_NONE)
	{
		Flow->bCreateQualifyingCutoff = Cutoff == 1;
		Changed();
		return;
	}
	if (const int32 Choice = ApexNav::IndexOf(QualifyingLengthButtons, Button);
		Choice != INDEX_NONE && ApexQualifyingLength::ChoiceMinutes().IsValidIndex(Choice))
	{
		Flow->CreateQualifyingMinutes = ApexQualifyingLength::ChoiceMinutes()[Choice];
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
		// Mouse presses reach the screen through its own handlers (a slot
		// may be dragged); this is only a button activated some other way.
		ClickSlot(Position);
		return;
	}
	if (Button == MovingPrev || Button == MovingNext)
	{
		// Pick the driver one slot up or down the grid, wrapping round.
		const TArray<FString> Order = CurrentOrder();
		if (Order.Num() > 0)
		{
			const int32 At = MovingIndex(Order);
			const int32 To = (At + (Button == MovingNext ? 1 : Order.Num() - 1)) % Order.Num();
			MovingRef = Order[To];
			RefreshStartOrder();
			RefreshGridPreview();
		}
		return;
	}
	if (Button == YourSlotEarlier || Button == YourSlotLater)
	{
		TArray<FString> Order = CurrentOrder();
		const int32 At = MovingIndex(Order);
		if (ApexCreateSession::MoveEntry(Order, At, At + (Button == YourSlotLater ? 1 : -1)))
		{
			StoreOrder(Order, FString());
			Changed();
		}
		return;
	}
	if (const int32 ChipAt = ApexNav::IndexOf(StartOrderButtons, Button); ChipAt != INDEX_NONE)
	{
		const UApexNetSubsystem* Net = GetNet();
		const FString Id = StartOrderIds.IsValidIndex(ChipAt) ? StartOrderIds[ChipAt] : FString();
		if (Id == CustomOrderId)
		{
			// Custom is the working order already.
			return;
		}
		PickedSlot = INDEX_NONE;
		if (Id.IsEmpty())
		{
			StoreOrder(TArray<FString>(), FString());
		}
		else if (Net)
		{
			for (const FApexQualifyingResult& Result : Net->GetQualifyingResults(Flow->GetPendingTrackId()))
			{
				if (Result.Id == Id)
				{
					LoadResult(Result);
					break;
				}
			}
		}
		Changed();
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
		FApexSessionConditions& C = Flow->CreateConditions;
		C.Weather = static_cast<EApexWeather>(Weather);
		// A written forecast stays valid from the new start: a first step
		// that now repeats it goes, as the server would drop it.
		C.Forecast = FApexSessionConditions::ClampedForecast(C.Weather, C.Forecast);
		Changed();
		return;
	}
	if (Button == ForecastAddButton)
	{
		FApexSessionConditions& C = Flow->CreateConditions;
		C.Forecast = FApexSessionConditions::ClampedForecast(C.Weather, C.Forecast);
		if (ApexCreateSession::AddForecastStep(C.Forecast, C.Weather))
		{
			Changed();
			ApexNav::Focus(ForecastMinutePlus.IsValidIndex(C.Forecast.Num() - 1) ? ForecastMinutePlus[C.Forecast.Num() - 1].Get() : nullptr);
		}
		return;
	}
	for (int32 Step = 0; Step < ForecastStepRows.Num(); ++Step)
	{
		FApexSessionConditions& C = Flow->CreateConditions;
		if (Button == ForecastMinuteMinus[Step] || Button == ForecastMinutePlus[Step])
		{
			if (ApexCreateSession::MoveForecastStep(C.Forecast, Step, Button == ForecastMinutePlus[Step] ? ForecastMinuteStep : -ForecastMinuteStep))
			{
				Changed();
			}
			return;
		}
		if (Button == ForecastWeatherMinus[Step] || Button == ForecastWeatherPlus[Step])
		{
			if (ApexCreateSession::CycleForecastWeather(C.Forecast, Step, Button == ForecastWeatherPlus[Step] ? 1 : -1, C.Weather))
			{
				Changed();
			}
			return;
		}
		if (Button == ForecastRemoveButtons[Step])
		{
			if (ApexCreateSession::RemoveForecastStep(C.Forecast, Step, C.Weather))
			{
				Changed();
				// Focus to the row that took its place, else the one before, else Add step.
				const int32 Next = FMath::Min(Step, C.Forecast.Num() - 1);
				ApexNav::Focus(Next >= 0 ? ForecastRemoveButtons[Next].Get() : ForecastAddButton.Get());
			}
			return;
		}
	}
	if (const int32 Clock = ApexNav::IndexOf(ClockScaleButtons, Button); Clock != INDEX_NONE)
	{
		ApexCreateSession::SetClockScale(Flow->CreateConditions, ApexCreateSession::ClockScales[Clock]);
		Changed();
		return;
	}
	if (const int32 Changes = ApexNav::IndexOf(ChangeableButtons, Button); Changes != INDEX_NONE)
	{
		FApexSessionConditions& C = Flow->CreateConditions;
		ApexCreateSession::SetChangeable(C, Changes);
		// A changing sky gets a seed of its own, so its forecast can be shown
		// here; a sky that holds sends none.
		C.bHasSkySeed = Changes > 0;
		if (C.bHasSkySeed && C.SkySeed == 0)
		{
			C.SkySeed = ApexCreateSession::NewSkySeed();
		}
		Changed();
		return;
	}
	if (Button == DateMinus || Button == DatePlus)
	{
		const int32 Day = ApexCreateSession::StepDate(Flow->CreateConditions.HasDate() ? Flow->CreateConditions.DayOfYear : -1,
			Button == DatePlus ? 1 : -1);
		Flow->CreateConditions.DayOfYear = Day > 0 ? Day : FApexSessionConditions::Auto;
		Changed();
		return;
	}
	if (Button == ForecastRollButton)
	{
		FApexSessionConditions& C = Flow->CreateConditions;
		C.SkySeed = ApexCreateSession::NewSkySeed();
		C.bHasSkySeed = C.WeatherChanges();
		Changed();
		return;
	}
	if (const int32 Rubber = ApexNav::IndexOf(TrackRubberButtons, Button); Rubber != INDEX_NONE)
	{
		ApexCreateSession::SetTrackRubber(Flow->CreateConditions, Rubber);
		Changed();
		return;
	}
	if (Button == AirAutoButton)
	{
		// Off auto, the figure starts at what auto would have said.
		FApexSessionConditions& C = Flow->CreateConditions;
		float Latitude = ApexSky::DefaultLatitudeDeg;
		const FApexSessionConditions AtSite = ConditionsAtSite(Latitude);
		C.AirTempC = C.HasAirTemp() ? FApexSessionConditions::AutoAirTemp : ApexCreateSession::AirTempC(AtSite, Latitude);
		Changed();
		return;
	}
	if (Button == AirMinus || Button == AirPlus)
	{
		FApexSessionConditions& C = Flow->CreateConditions;
		float Latitude = ApexSky::DefaultLatitudeDeg;
		const FApexSessionConditions AtSite = ConditionsAtSite(Latitude);
		C.AirTempC = FMath::Clamp(ApexCreateSession::AirTempC(AtSite, Latitude) + (Button == AirPlus ? 1 : -1),
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
