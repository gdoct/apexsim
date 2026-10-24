#include "UI/ApexSeasonWidget.h"

#include "ApexMenuFlowSubsystem.h"
#include "ApexNetSubsystem.h"
#include "ApexSim.h"
#include "Audio/ApexUiAudioSubsystem.h"
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
#include "UI/ApexButtonWidget.h"
#include "UI/ApexNavigation.h"
#include "UI/ApexStepperWidget.h"
#include "UI/ApexUIStyle.h"
#include "UObject/UObjectIterator.h"
#include "HAL/IConsoleManager.h"

namespace
{
	const FName ActionSeasonPrimary(TEXT("Season.Primary"));
	const FName ActionSeasonEnd(TEXT("Season.End"));
	const FName ActionSeasonBack(TEXT("Season.Back"));
	const FName ActionSeasonRound(TEXT("Season.Round"));
	const FName ActionRoundDone(TEXT("Season.RoundDone"));
	const FName ActionRoundReset(TEXT("Season.RoundReset"));

	const FName StepClass(TEXT("Season.Class"));
	const FName StepCar(TEXT("Season.Car"));
	const FName StepRounds(TEXT("Season.Rounds"));
	const FName StepPoints(TEXT("Season.Points"));
	const FName StepLaps(TEXT("Season.Laps"));
	const FName StepAi(TEXT("Season.Ai"));
	const FName StepLevel(TEXT("Season.Level"));
	const FName StepDrops(TEXT("Season.Drops"));
	const FName StepBallast(TEXT("Season.Ballast"));
	const FName StepFastestLap(TEXT("Season.FastestLap"));
	const FName StepFastestLapNeeds(TEXT("Season.FastestLapNeeds"));
	const FName StepRoundWeather(TEXT("Season.RoundWeather"));
	const FName StepRoundStart(TEXT("Season.RoundStart"));
	const FName StepRoundQualifying(TEXT("Season.RoundQualifying"));
	const FName StepRoundLength(TEXT("Season.RoundLength"));

	constexpr float SeasonCardWidth = 1240.0f;
	/** The AI level stepper: 0 the mixed field, then ApexAiSkill::Min upwards by this. */
	constexpr int32 LevelStep = 5;
	constexpr int32 MaxField = 19;

	int32 LevelOf(int32 Index)
	{
		return Index <= 0 ? ApexAiSkill::Mixed : FMath::Min(ApexAiSkill::Max, ApexAiSkill::Min + (Index - 1) * LevelStep);
	}

	int32 IndexOfLevel(int32 Level)
	{
		return Level < 0 ? 0 : 1 + (ApexAiSkill::Clamp(Level) - ApexAiSkill::Min) / LevelStep;
	}

	/** A round's format stepper past 0 (the race alone): ApexQualifyingLength's timed choices, minutes. */
	TArray<int32> QualifyingChoices()
	{
		TArray<int32> Out = ApexQualifyingLength::ChoiceMinutes();
		Out.Remove(0);
		return Out;
	}

	/**
	 * A round's length stepper: 0 the season's laps, 1..LapsSpan the laps
	 * from MinLaps, then ApexRaceLength's ladder, minutes.
	 */
	constexpr int32 MinLaps = 2;
	constexpr int32 MaxLaps = 40;
	constexpr int32 LapsSpan = MaxLaps - MinLaps + 1;

	int32 LengthIndexOf(const FApexSeasonRound& Round)
	{
		if (Round.RaceMinutes > 0)
		{
			const int32 Rung = ApexRaceLength::LadderMinutes().IndexOfByKey(Round.RaceMinutes);
			return 1 + LapsSpan + FMath::Max(0, Rung);
		}
		return Round.Laps > 0 ? 1 + FMath::Clamp(Round.Laps, MinLaps, MaxLaps) - MinLaps : 0;
	}

	void ApplyLengthIndex(FApexSeasonRound& Round, int32 Index)
	{
		Round.Laps = Index >= 1 && Index <= LapsSpan ? MinLaps + Index - 1 : 0;
		const TArray<int32>& Ladder = ApexRaceLength::LadderMinutes();
		Round.RaceMinutes = Index > LapsSpan ? Ladder[FMath::Clamp(Index - 1 - LapsSpan, 0, Ladder.Num() - 1)] : 0;
	}

	/** The Season screen of this world, for the unattended commands. */
	UApexSeasonWidget* SeasonScreenOf(const UWorld* World)
	{
		for (TObjectIterator<UApexSeasonWidget> It; It; ++It)
		{
			if (It->GetWorld() == World && !It->HasAnyFlags(RF_ClassDefaultObject))
			{
				return *It;
			}
		}
		return nullptr;
	}

	FAutoConsoleCommandWithWorldAndArgs SeasonSetupCommand(TEXT("apexsim.season.Setup"),
		TEXT("apexsim.season.Setup <laps> <ai> <rounds> [first track stem, - for the spread's] [ballast 0-2]: the Season screen's setup, for unattended runs."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			if (UApexSeasonWidget* Screen = SeasonScreenOf(World); Screen && Args.Num() >= 3)
			{
				Screen->SetSetup(FCString::Atoi(*Args[0]), FCString::Atoi(*Args[1]), FCString::Atoi(*Args[2]),
					Args.Num() >= 4 && Args[3] != TEXT("-") ? Args[3] : FString(), Args.Num() >= 5 ? FCString::Atoi(*Args[4]) : 0);
			}
		}));

	FAutoConsoleCommandWithWorldAndArgs SeasonFastestLapCommand(TEXT("apexsim.season.FastestLap"),
		TEXT("apexsim.season.FastestLap <points 0-5> [needs a points finish 0/1, default 1]: the Season setup's fastest-lap rule, for unattended runs."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			if (UApexSeasonWidget* Screen = SeasonScreenOf(World); Screen && Args.Num() >= 1)
			{
				Screen->SetFastestLap(FCString::Atoi(*Args[0]), Args.Num() < 2 || FCString::Atoi(*Args[1]) != 0);
			}
		}));

	FAutoConsoleCommandWithWorldAndArgs SeasonRoundCommand(TEXT("apexsim.season.Round"),
		TEXT("apexsim.season.Round <round 1..> <weather, -1 the season's, 0-5> [start hour, -1 the season's, 0-23] [qualifying minutes, 0 none] [laps, 0 the season's] [race minutes, 0 over laps]: a round's own rules in the Season setup, opened on screen."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			if (UApexSeasonWidget* Screen = SeasonScreenOf(World); Screen && Args.Num() >= 2)
			{
				auto Arg = [&Args](int32 Index, int32 Default) { return Args.IsValidIndex(Index) ? FCString::Atoi(*Args[Index]) : Default; };
				Screen->SetRoundRules(Arg(0, 1) - 1, Arg(1, -1), Arg(2, -1), Arg(3, 0), Arg(4, 0), Arg(5, 0));
			}
		}));

	FAutoConsoleCommandWithWorldAndArgs SeasonEditCommand(TEXT("apexsim.season.Edit"),
		TEXT("apexsim.season.Edit <round 1.., 0 for the calendar>: open a round's rules in the Season setup."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			if (UApexSeasonWidget* Screen = SeasonScreenOf(World); Screen && Args.Num() >= 1)
			{
				Screen->EditRound(FCString::Atoi(*Args[0]) - 1);
			}
		}));

	FAutoConsoleCommandWithWorldAndArgs SeasonRaceCommand(TEXT("apexsim.season.Race"),
		TEXT("Press the Season screen's main button: start the season set up, or race its next round."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>&, UWorld* World)
		{
			if (UApexSeasonWidget* Screen = SeasonScreenOf(World))
			{
				Screen->PressPrimary();
			}
		}));
}

void UApexSeasonWidget::SetSetup(int32 InLaps, int32 InAi, int32 InRounds, const FString& FirstStem, int32 InBallast)
{
	BallastIndex = FMath::Clamp(InBallast, 0, 2);
	Laps = FMath::Clamp(InLaps, 2, 40);
	AiCount = FMath::Clamp(InAi, 1, MaxField);
	RoundCount = FMath::Max(1, InRounds);
	const UApexMenuFlowSubsystem* Flow = GetFlow();
	FirstTrackOverride = Flow && !FirstStem.IsEmpty() ? Flow->FindTrackIdByStem(FirstStem) : FString();
	LoadContent();
	Rebuild();
}

void UApexSeasonWidget::PressPrimary()
{
	HandleButtonActivated(PrimaryButton);
}

void UApexSeasonWidget::SetFastestLap(int32 InPoints, bool bInNeedsPoints)
{
	FastestLapPoints = FMath::Clamp(InPoints, 0, 5);
	bFastestLapNeedsPoints = bInNeedsPoints;
	Rebuild();
}

void UApexSeasonWidget::SetRoundRules(int32 Round, int32 InWeather, int32 InStartHour, int32 InQualifyingMinutes, int32 InLaps, int32 InRaceMinutes)
{
	if (Round < 0 || Round >= RoundCount)
	{
		return;
	}
	FApexSeasonRound& Rules = RulesOf(Round);
	Rules.Weather = InWeather < 0 ? -1 : FMath::Min(InWeather, FApexSessionConditions::WeatherCount - 1);
	Rules.StartHour = InStartHour < 0 ? -1 : FMath::Min(InStartHour, 23);
	Rules.QualifyingMinutes = InQualifyingMinutes > 0 ? ApexQualifyingLength::Clamp(InQualifyingMinutes * 60) / 60 : 0;
	Rules.Laps = InLaps > 0 ? FMath::Clamp(InLaps, MinLaps, MaxLaps) : 0;
	Rules.RaceMinutes = InRaceMinutes > 0 ? ApexRaceLength::Clamp(InRaceMinutes * 60) / 60 : 0;
	EditRound(Round);
}

void UApexSeasonWidget::EditRound(int32 Round)
{
	EditingRound = Round >= 0 && Round < RoundCount ? Round : INDEX_NONE;
	Rebuild();
}

FApexSeasonRound& UApexSeasonWidget::RulesOf(int32 Round)
{
	if (RoundRules.Num() <= Round)
	{
		RoundRules.SetNum(Round + 1);
	}
	return RoundRules[Round];
}

bool UApexSeasonWidget::HandleBack()
{
	if (EditingRound != INDEX_NONE)
	{
		const int32 Round = EditingRound;
		EditingRound = INDEX_NONE;
		Rebuild();
		if (RoundButtons.IsValidIndex(Round))
		{
			ApexNav::Focus(RoundButtons[Round]);
		}
		return true;
	}
	return Super::HandleBack();
}

UApexSeasonWidget::UApexSeasonWidget(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	SetIsFocusable(true);
}

void UApexSeasonWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	BuildLayout();
}

void UApexSeasonWidget::BuildLayout()
{
	using namespace ApexUI;
	auto MakeButton = [this](const FString& Label, FName Action, EApexButtonVariant Variant)
	{
		FApexButtonSpec Spec;
		Spec.Label = Label;
		Spec.Variant = Variant;
		Spec.ActionId = Action;
		Spec.Height = 56.0f;
		Spec.bCentreLabel = true;
		Spec.LabelSize = 17.0f;
		if (Action == ActionSeasonBack)
		{
			Spec.Sound = EApexUiSound::Back;
		}
		UApexButtonWidget* Button = WidgetTree->ConstructWidget<UApexButtonWidget>();
		Button->Setup(Spec);
		Button->OnActivated.AddDynamic(this, &UApexSeasonWidget::HandleButtonActivated);
		return Button;
	};

	UVerticalBox* Card = WidgetTree->ConstructWidget<UVerticalBox>();
	AddV(Card, MakeLabel(*WidgetTree, TEXT("Offline season"), Palette::Accent));
	HeadingText = MakeText(*WidgetTree, TEXT("Season"), Font::Display(42.0f), Palette::TextPrimary);
	AddV(Card, HeadingText, FMargin(0.0f, 10.0f, 0.0f, 4.0f));
	StatusText = MakeText(*WidgetTree, FString(), Font::Mono(12.0f), Palette::TextMuted);
	StatusText->SetAutoWrapText(true);
	AddV(Card, StatusText, FMargin(0.0f, 0.0f, 0.0f, 22.0f));

	LeftColumn = WidgetTree->ConstructWidget<UVerticalBox>();
	RightColumn = WidgetTree->ConstructWidget<UVerticalBox>();
	UHorizontalBox* Columns = WidgetTree->ConstructWidget<UHorizontalBox>();
	AddH(Columns, MakeSized(*WidgetTree, LeftColumn, 520.0f, -1.0f), FMargin(), VAlign_Top);
	AddH(Columns, MakeDivider(*WidgetTree, true), FMargin(28.0f, 0.0f), VAlign_Fill);
	AddH(Columns, RightColumn, FMargin(), VAlign_Top, 1.0f);
	AddV(Card, MakeSized(*WidgetTree, Columns, -1.0f, 600.0f));

	UHorizontalBox* Buttons = WidgetTree->ConstructWidget<UHorizontalBox>();
	BackButton = MakeButton(TEXT("Back"), ActionSeasonBack, EApexButtonVariant::Ghost);
	EndButton = MakeButton(TEXT("End season"), ActionSeasonEnd, EApexButtonVariant::Ghost);
	PrimaryButton = MakeButton(TEXT("Start season"), ActionSeasonPrimary, EApexButtonVariant::Primary);
	AddH(Buttons, MakeSized(*WidgetTree, BackButton, 180.0f, -1.0f));
	AddH(Buttons, MakeSized(*WidgetTree, EndButton, 220.0f, -1.0f), FMargin(8.0f, 0.0f, 0.0f, 0.0f));
	AddH(Buttons, WidgetTree->ConstructWidget<USpacer>(), FMargin(), VAlign_Fill, 1.0f);
	AddH(Buttons, MakeSized(*WidgetTree, PrimaryButton, 420.0f, -1.0f));
	AddV(Card, Buttons, FMargin(0.0f, 20.0f, 0.0f, 0.0f));

	UVerticalBox* Centre = WidgetTree->ConstructWidget<UVerticalBox>();
	AddV(Centre, WidgetTree->ConstructWidget<USpacer>(), FMargin(), HAlign_Fill, 1.0f);
	AddV(Centre, MakeSized(*WidgetTree,
		MakePanel(*WidgetTree, Card, FMargin(44.0f, 36.0f), MakeBrush(Palette::Surface, Palette::Border, 1.0f)),
		SeasonCardWidth, -1.0f), FMargin(), HAlign_Center);
	AddV(Centre, WidgetTree->ConstructWidget<USpacer>(), FMargin(), HAlign_Fill, 1.2f);
	WidgetTree->RootWidget = MakePanel(*WidgetTree, Centre, FMargin(), MakeBrush(Palette::Background));
}

void UApexSeasonWidget::OnScreenActivated()
{
	Super::OnScreenActivated();
	if (UApexNetSubsystem* Net = GetNet(); Net && !bBound)
	{
		Net->OnLobbyStateUpdated.AddDynamic(this, &UApexSeasonWidget::HandleLobbyState);
		bBound = true;
	}
	bEndArmed = false;
	EditingRound = INDEX_NONE;
	LoadContent();
	Rebuild();
	FocusDefault();
}

void UApexSeasonWidget::HandleLobbyState(const FApexLobbyState& LobbyState)
{
	if (IsActiveScreen() && TrackIds.Num() == 0 && LobbyState.TrackConfigs.Num() > 0)
	{
		LoadContent();
		Rebuild();
	}
}

void UApexSeasonWidget::FocusDefault()
{
	if (!(PrimaryButton && ApexNav::Focus(PrimaryButton)))
	{
		SetKeyboardFocus();
	}
}

bool UApexSeasonWidget::HandleNavigation(EUINavigation Direction, UWidget* Source)
{
	return ApexNav::MoveToward(this, Direction, Source);
}

void UApexSeasonWidget::LoadContent()
{
	const UApexNetSubsystem* Net = GetNet();
	const UApexMenuFlowSubsystem* Flow = GetFlow();
	Classes.Reset();
	CarsByClass.Reset();
	TrackIds.Reset();
	if (!Net || !Flow)
	{
		return;
	}
	TArray<TPair<FString, FString>> Tracks;
	for (const FApexTrackConfigSummary& Track : Net->GetCachedLobbyState().TrackConfigs)
	{
		FApexTrackCatalogRow Row;
		if (Flow->GetTrackCatalogRow(Track.Id, Row) && !Row.DisplayName.IsEmpty())
		{
			Tracks.Emplace(Row.DisplayName, Track.Id);
		}
	}
	Tracks.Sort([](const TPair<FString, FString>& A, const TPair<FString, FString>& B) { return A.Key.Compare(B.Key, ESearchCase::IgnoreCase) < 0; });
	for (const TPair<FString, FString>& Track : Tracks)
	{
		TrackIds.Add(Track.Value);
	}
	TArray<TPair<FString, FString>> Cars;
	for (const FApexCarConfigSummary& Car : Net->GetCachedLobbyState().CarConfigs)
	{
		FApexCarCatalogRow Row;
		if (Flow->GetCarCatalogRow(Car.Id, Row) && !Row.DisplayName.IsEmpty() && !Row.CarClass.IsEmpty())
		{
			Cars.Emplace(Row.DisplayName, Car.Id);
			CarsByClass.FindOrAdd(Row.CarClass);
		}
	}
	Cars.Sort([](const TPair<FString, FString>& A, const TPair<FString, FString>& B) { return A.Key.Compare(B.Key, ESearchCase::IgnoreCase) < 0; });
	for (const TPair<FString, FString>& Car : Cars)
	{
		FApexCarCatalogRow Row;
		Flow->GetCarCatalogRow(Car.Value, Row);
		CarsByClass.FindOrAdd(Row.CarClass).Add(Car.Value);
	}
	CarsByClass.GetKeys(Classes);
	Classes.Sort([](const FString& A, const FString& B) { return ApexCatalog::DisplayClass(A).Compare(ApexCatalog::DisplayClass(B), ESearchCase::IgnoreCase) < 0; });

	// The setup starts on the garage's car where it can.
	FApexCarCatalogRow Pending;
	if (Flow->GetCarCatalogRow(Flow->GetPendingCarId(), Pending))
	{
		ClassIndex = FMath::Max(0, Classes.IndexOfByKey(Pending.CarClass));
		const TArray<FString>* InClass = CarsByClass.Find(Pending.CarClass);
		CarIndex = InClass ? FMath::Max(0, InClass->IndexOfByPredicate([&Flow](const FString& Id) { return Id.Equals(Flow->GetPendingCarId(), ESearchCase::IgnoreCase); })) : 0;
	}
	ClassIndex = FMath::Clamp(ClassIndex, 0, FMath::Max(0, Classes.Num() - 1));
	RoundCount = FMath::Clamp(RoundCount, FMath::Min(2, TrackIds.Num()), TrackIds.Num());
}

FApexSeason UApexSeasonWidget::SeasonFromSetup() const
{
	FApexSeason Season;
	if (!Classes.IsValidIndex(ClassIndex))
	{
		return Season;
	}
	Season.CarClass = Classes[ClassIndex];
	const TArray<FString>& Cars = CarsByClass.FindChecked(Season.CarClass);
	Season.CarId = Cars.IsValidIndex(CarIndex) ? Cars[CarIndex] : (Cars.Num() > 0 ? Cars[0] : FString());
	Season.Name = FString::Printf(TEXT("%s season"), *ApexCatalog::DisplayClass(Season.CarClass));
	Season.Points = static_cast<EApexPointsPreset>(PointsIndex);
	Season.Laps = Laps;
	Season.AiCount = AiCount;
	Season.AiSkill = AiSkill;
	Season.DropRounds = DropRounds;
	Season.Ballast = static_cast<EApexSuccessBallast>(FMath::Clamp(BallastIndex, 0, 2));
	Season.FastestLapPoints = FastestLapPoints;
	Season.bFastestLapNeedsPoints = FastestLapPoints > 0 && bFastestLapNeedsPoints;
	for (const FString& Track : ApexSeason::SpreadCalendar(TrackIds, RoundCount))
	{
		// The round's own rules over the season's; a round never edited has none.
		const int32 Index = Season.Rounds.Num();
		FApexSeasonRound& Round = Season.Rounds.Add_GetRef(RoundRules.IsValidIndex(Index) ? RoundRules[Index] : FApexSeasonRound());
		Round.TrackId = Track;
		Round.bDone = false;
		Round.Results.Reset();
	}
	if (!FirstTrackOverride.IsEmpty() && Season.Rounds.Num() > 0)
	{
		Season.Rounds[0].TrackId = FirstTrackOverride;
	}
	return Season;
}

void UApexSeasonWidget::Rebuild()
{
	const UApexMenuFlowSubsystem* Flow = GetFlow();
	if (!Flow || !LeftColumn || !RightColumn)
	{
		return;
	}
	LeftColumn->ClearChildren();
	RightColumn->ClearChildren();
	Steppers.Reset();
	RoundButtons.Reset();
	CalendarScroll = nullptr;
	RoundDoneButton = nullptr;
	RoundResetButton = nullptr;
	if (Flow->GetSeason().Rounds.Num() > 0)
	{
		RebuildActive();
	}
	else
	{
		RebuildSetup();
	}
}

void UApexSeasonWidget::RebuildSetup()
{
	using namespace ApexUI;
	const UApexMenuFlowSubsystem* Flow = GetFlow();
	HeadingText->SetText(FText::FromString(TEXT("A new season")));
	const TArray<FApexSeasonSummary>& Past = Flow->GetPastSeasons();
	StatusText->SetText(FText::FromString(Past.Num() == 0
		? FString(TEXT("RACE THE AI ROUND BY ROUND IN ONE CAR. THE SEASON IS SAVED IN YOUR PROFILE"))
		: FString::Printf(TEXT("LAST SEASON: %s, CHAMPION %s, YOU P%d WITH %d POINTS"), *Past.Last().Name.ToUpper(),
			*Past.Last().Champion.ToUpper(), Past.Last().YourPosition, Past.Last().YourPoints)));
	EndButton->SetVisibility(ESlateVisibility::Collapsed);
	PrimaryButton->SetLabel(TEXT("Start season"));
	PrimaryButton->SetIsEnabled(Classes.Num() > 0 && TrackIds.Num() > 0);
	if (Classes.Num() == 0 || TrackIds.Num() == 0)
	{
		AddV(LeftColumn, MakeText(*WidgetTree, TEXT("CONNECT TO A SERVER FIRST: ITS CARS AND CIRCUITS MAKE THE SEASON"), Font::Mono(12.0f), Palette::TextMuted));
		return;
	}

	auto AddStepper = [this](const TCHAR* Label, FName Id, int32 Min, int32 Max, int32 Value, TFunction<FString(int32)> Formatter)
	{
		UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();
		ApexUI::AddH(Row, ApexUI::MakeLabel(*WidgetTree, Label), FMargin(), VAlign_Center, 1.0f);
		UApexStepperWidget* Stepper = WidgetTree->ConstructWidget<UApexStepperWidget>();
		Stepper->ControlId = Id;
		Stepper->Formatter = MoveTemp(Formatter);
		Stepper->Setup(Min, Max, FMath::Clamp(Value, Min, Max), 230.0f);
		Stepper->OnChanged.AddDynamic(this, &UApexSeasonWidget::HandleStepperChanged);
		ApexUI::AddH(Row, Stepper, FMargin(), VAlign_Center);
		ApexUI::AddV(LeftColumn, Row, FMargin(0.0f, Steppers.Num() == 0 ? 0.0f : 6.0f, 0.0f, 0.0f));
		Steppers.Add(Stepper);
	};
	const UApexMenuFlowSubsystem* FlowRef = Flow;
	AddStepper(TEXT("Class"), StepClass, 0, Classes.Num() - 1, ClassIndex,
		[this](int32 V) { return Classes.IsValidIndex(V) ? ApexCatalog::DisplayClass(Classes[V]) : FString(); });
	const TArray<FString>& Cars = CarsByClass.FindChecked(Classes[ClassIndex]);
	AddStepper(TEXT("Your car"), StepCar, 0, FMath::Max(0, Cars.Num() - 1), CarIndex,
		[this, FlowRef](int32 V)
		{
			const TArray<FString>* InClass = Classes.IsValidIndex(ClassIndex) ? CarsByClass.Find(Classes[ClassIndex]) : nullptr;
			FApexCarCatalogRow Row;
			return InClass && InClass->IsValidIndex(V) && FlowRef->GetCarCatalogRow((*InClass)[V], Row) ? Row.DisplayName : FString();
		});
	AddStepper(TEXT("Rounds"), StepRounds, FMath::Min(2, TrackIds.Num()), TrackIds.Num(), RoundCount,
		[](int32 V) { return FString::Printf(TEXT("%d"), V); });
	AddStepper(TEXT("Points"), StepPoints, 0, 2, PointsIndex,
		[](int32 V) { return V == 1 ? FString(TEXT("Classic")) : V == 2 ? FString(TEXT("Deep field")) : FString(TEXT("Modern")); });
	AddStepper(TEXT("Laps a race"), StepLaps, 2, 40, Laps, [](int32 V) { return FString::Printf(TEXT("%d"), V); });
	AddStepper(TEXT("AI drivers"), StepAi, 1, MaxField, AiCount, [](int32 V) { return FString::Printf(TEXT("%d"), V); });
	AddStepper(TEXT("AI level"), StepLevel, 0, IndexOfLevel(ApexAiSkill::Max), IndexOfLevel(AiSkill),
		[](int32 V) { const int32 L = LevelOf(V); return L < 0 ? FString(TEXT("Mixed")) : FString::Printf(TEXT("%s %d"), ApexAiSkill::Label(L), L); });
	AddStepper(TEXT("Dropped rounds"), StepDrops, 0, 3, DropRounds, [](int32 V) { return V == 0 ? FString(TEXT("None")) : FString::Printf(TEXT("Worst %d"), V); });
	AddStepper(TEXT("Success ballast"), StepBallast, 0, 2, BallastIndex,
		[](int32 V) { return V == 1 ? FString(TEXT("Light")) : V == 2 ? FString(TEXT("Heavy")) : FString(TEXT("None")); });

	AddStepper(TEXT("Fastest lap"), StepFastestLap, 0, 5, FastestLapPoints,
		[](int32 V) { return V == 0 ? FString(TEXT("No points")) : FString::Printf(TEXT("+%d point%s"), V, V == 1 ? TEXT("") : TEXT("s")); });
	AddStepper(TEXT("Fastest lap scores"), StepFastestLapNeeds, 0, 1, bFastestLapNeedsPoints ? 1 : 0,
		[](int32 V) { return V == 1 ? FString(TEXT("In the points")) : FString(TEXT("For anyone")); });

	// The calendar the steppers make, or the round being edited.
	const FApexSeason Preview = SeasonFromSetup();
	if (!Preview.Rounds.IsValidIndex(EditingRound))
	{
		EditingRound = INDEX_NONE;
		RebuildCalendar(Preview);
	}
	else
	{
		RebuildRoundEditor(Preview);
	}
}

void UApexSeasonWidget::RebuildCalendar(const FApexSeason& Preview)
{
	using namespace ApexUI;
	const UApexMenuFlowSubsystem* Flow = GetFlow();
	AddV(RightColumn, MakeLabel(*WidgetTree, TEXT("Calendar: pick a round for its own rules")), FMargin(0.0f, 0.0f, 0.0f, 10.0f));
	CalendarScroll = WidgetTree->ConstructWidget<UScrollBox>();
	for (int32 Round = 0; Round < Preview.Rounds.Num(); ++Round)
	{
		FApexTrackCatalogRow Row;
		Flow->GetTrackCatalogRow(Preview.Rounds[Round].TrackId, Row);
		FApexButtonSpec Spec;
		Spec.Label = FString::Printf(TEXT("R%d   %s"), Round + 1, *Row.DisplayName);
		Spec.Badge = ApexSeason::DescribeRound(Preview, Round).ToUpper();
		// A round with rules of its own stands out from the season's.
		Spec.BadgeColour = Preview.Rounds[Round].HasOwnRules() ? Palette::Accent : Palette::TextMuted;
		Spec.ActionId = ActionSeasonRound;
		Spec.Height = 40.0f;
		Spec.LabelSize = 15.0f;
		UApexButtonWidget* Button = WidgetTree->ConstructWidget<UApexButtonWidget>();
		Button->Setup(Spec);
		Button->OnActivated.AddDynamic(this, &UApexSeasonWidget::HandleButtonActivated);
		if (UScrollBoxSlot* RowSlot = Cast<UScrollBoxSlot>(CalendarScroll->AddChild(Button)))
		{
			RowSlot->SetPadding(FMargin(0.0f, 0.0f, 10.0f, 4.0f));
		}
		RoundButtons.Add(Button);
	}
	AddV(RightColumn, MakeSized(*WidgetTree, CalendarScroll, -1.0f, 470.0f));
	AddV(RightColumn, MakeText(*WidgetTree, ApexSeason::PresetName(Preview.Points).ToUpper(), Font::Mono(11.0f), Palette::TextMuted), FMargin(0.0f, 10.0f, 0.0f, 0.0f));
	if (const FString Ballast = ApexSeason::BallastName(Preview.Ballast); !Ballast.IsEmpty())
	{
		AddV(RightColumn, MakeText(*WidgetTree, (Ballast + TEXT(", by the standings into each round")).ToUpper(), Font::Mono(11.0f), Palette::TextMuted), FMargin(0.0f, 4.0f, 0.0f, 0.0f));
	}
	if (const FString FastestLap = ApexSeason::FastestLapRuleName(Preview); !FastestLap.IsEmpty())
	{
		AddV(RightColumn, MakeText(*WidgetTree, FastestLap.ToUpper(), Font::Mono(11.0f), Palette::TextMuted), FMargin(0.0f, 4.0f, 0.0f, 0.0f));
	}
}

void UApexSeasonWidget::RebuildRoundEditor(const FApexSeason& Preview)
{
	using namespace ApexUI;
	const UApexMenuFlowSubsystem* Flow = GetFlow();
	const FApexSeasonRound& Round = Preview.Rounds[EditingRound];
	FApexTrackCatalogRow Track;
	Flow->GetTrackCatalogRow(Round.TrackId, Track);
	AddV(RightColumn, MakeLabel(*WidgetTree, FString::Printf(TEXT("Round %d of %d"), EditingRound + 1, Preview.Rounds.Num())), FMargin(0.0f, 0.0f, 0.0f, 4.0f));
	AddV(RightColumn, MakeText(*WidgetTree, Track.DisplayName, Font::Display(26.0f), Palette::TextPrimary), FMargin(0.0f, 0.0f, 0.0f, 4.0f));
	AddV(RightColumn, MakeText(*WidgetTree, ApexSeason::DescribeRound(Preview, EditingRound).ToUpper(), Font::Mono(11.0f), Palette::Accent), FMargin(0.0f, 0.0f, 0.0f, 16.0f));

	UVerticalBox* Rules = WidgetTree->ConstructWidget<UVerticalBox>();
	AddV(RightColumn, Rules);
	auto AddRule = [this, Rules](const TCHAR* Label, FName Id, int32 Max, int32 Value, TFunction<FString(int32)> Formatter)
	{
		UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();
		ApexUI::AddH(Row, ApexUI::MakeLabel(*WidgetTree, Label), FMargin(), VAlign_Center, 1.0f);
		UApexStepperWidget* Stepper = WidgetTree->ConstructWidget<UApexStepperWidget>();
		Stepper->ControlId = Id;
		Stepper->Formatter = MoveTemp(Formatter);
		Stepper->Setup(0, Max, FMath::Clamp(Value, 0, Max), 270.0f);
		Stepper->OnChanged.AddDynamic(this, &UApexSeasonWidget::HandleStepperChanged);
		ApexUI::AddH(Row, Stepper, FMargin(), VAlign_Center);
		ApexUI::AddV(Rules, Row, FMargin(0.0f, Rules->GetChildrenCount() == 0 ? 0.0f : 8.0f, 0.0f, 0.0f));
		Steppers.Add(Stepper);
	};
	AddRule(TEXT("Weather"), StepRoundWeather, FApexSessionConditions::WeatherCount, Round.Weather + 1,
		[](int32 V) { return V == 0 ? FString(TEXT("Season's sky")) : FApexSessionConditions::WeatherLabel(static_cast<EApexWeather>(V - 1)); });
	AddRule(TEXT("Race start"), StepRoundStart, 24, Round.StartHour + 1,
		[](int32 V) { return V == 0 ? FString(TEXT("Season's time")) : FString::Printf(TEXT("%02d:00"), V - 1); });
	const TArray<int32> Qualifying = QualifyingChoices();
	AddRule(TEXT("Format"), StepRoundQualifying, Qualifying.Num(), Qualifying.IndexOfByKey(Round.QualifyingMinutes) + 1,
		[Qualifying](int32 V) { return Qualifying.IsValidIndex(V - 1) ? FString::Printf(TEXT("%d min qualifying + race"), Qualifying[V - 1]) : FString(TEXT("Race only")); });
	const int32 SeasonLaps = Preview.Laps;
	AddRule(TEXT("Race length"), StepRoundLength, LapsSpan + ApexRaceLength::LadderMinutes().Num(), LengthIndexOf(Round),
		[SeasonLaps](int32 V)
		{
			if (V == 0)
			{
				return FString::Printf(TEXT("Season's %d laps"), SeasonLaps);
			}
			if (V <= LapsSpan)
			{
				return FString::Printf(TEXT("%d laps"), MinLaps + V - 1);
			}
			const TArray<int32>& Ladder = ApexRaceLength::LadderMinutes();
			return ApexRaceLength::Describe(Ladder[FMath::Clamp(V - 1 - LapsSpan, 0, Ladder.Num() - 1)] * 60);
		});

	auto MakeRoundButton = [this](const FString& Label, FName Action, EApexButtonVariant Variant)
	{
		FApexButtonSpec Spec;
		Spec.Label = Label;
		Spec.Variant = Variant;
		Spec.ActionId = Action;
		Spec.Height = 48.0f;
		Spec.bCentreLabel = true;
		Spec.LabelSize = 15.0f;
		UApexButtonWidget* Button = WidgetTree->ConstructWidget<UApexButtonWidget>();
		Button->Setup(Spec);
		Button->OnActivated.AddDynamic(this, &UApexSeasonWidget::HandleButtonActivated);
		return Button;
	};
	RoundResetButton = MakeRoundButton(TEXT("Season's rules"), ActionRoundReset, EApexButtonVariant::Ghost);
	RoundDoneButton = MakeRoundButton(TEXT("Done"), ActionRoundDone, EApexButtonVariant::Panel);
	UHorizontalBox* Buttons = WidgetTree->ConstructWidget<UHorizontalBox>();
	AddH(Buttons, MakeSized(*WidgetTree, RoundResetButton, 220.0f, -1.0f));
	AddH(Buttons, WidgetTree->ConstructWidget<USpacer>(), FMargin(), VAlign_Fill, 1.0f);
	AddH(Buttons, MakeSized(*WidgetTree, RoundDoneButton, 180.0f, -1.0f));
	AddV(RightColumn, Buttons, FMargin(0.0f, 24.0f, 0.0f, 0.0f));
	UTextBlock* Hint = MakeText(*WidgetTree, TEXT("A RULE LEFT ON THE SEASON'S FOLLOWS THE SETUP. AFTER QUALIFYING, START THE RACE FROM ITS RESULTS: THE GRID IS THE CLASSIFICATION"),
		Font::Mono(11.0f), Palette::TextMuted);
	Hint->SetAutoWrapText(true);
	AddV(RightColumn, Hint, FMargin(0.0f, 14.0f, 0.0f, 0.0f));
}

void UApexSeasonWidget::RebuildActive()
{
	using namespace ApexUI;
	const UApexMenuFlowSubsystem* Flow = GetFlow();
	const FApexSeason& Season = Flow->GetSeason();
	const int32 Next = Season.NextRound();
	HeadingText->SetText(FText::FromString(Season.Name));
	FApexCarCatalogRow Car;
	Flow->GetCarCatalogRow(Season.CarId, Car);
	StatusText->SetText(FText::FromString(FString::Printf(TEXT("ROUND %d OF %d  ·  %s  ·  %d AI, %s  ·  %d LAPS  ·  %s%s"),
		FMath::Min(Season.RoundsDone() + 1, Season.Rounds.Num()), Season.Rounds.Num(), *Car.DisplayName.ToUpper(), Season.AiCount,
		*FString(Season.AiSkill < 0 ? TEXT("MIXED") : ApexAiSkill::Label(Season.AiSkill)).ToUpper(), Season.Laps,
		*ApexSeason::PresetName(Season.Points).ToUpper(),
		*(FString(Season.DropRounds > 0 ? *FString::Printf(TEXT("  ·  WORST %d DROPPED"), Season.DropRounds) : TEXT(""))
			+ (Season.Ballast != EApexSuccessBallast::None ? FString(TEXT("  ·  ")) + ApexSeason::BallastName(Season.Ballast).ToUpper() : FString())
			+ (Season.FastestLapPoints > 0 ? FString(TEXT("  ·  ")) + ApexSeason::FastestLapRuleName(Season).ToUpper() : FString())))));
	EndButton->SetVisibility(ESlateVisibility::Visible);
	EndButton->SetLabel(bEndArmed ? TEXT("Press again to end") : (Season.IsFinished() ? TEXT("Close season") : TEXT("End season")));
	PrimaryButton->SetIsEnabled(true);
	if (Next != INDEX_NONE)
	{
		FApexTrackCatalogRow Track;
		Flow->GetTrackCatalogRow(Season.Rounds[Next].TrackId, Track);
		PrimaryButton->SetLabel(FString::Printf(TEXT("Race round %d: %s"), Next + 1, *Track.DisplayName));
	}
	else
	{
		PrimaryButton->SetLabel(TEXT("New season"));
	}

	// The calendar, with each raced round's winner and the player's place.
	AddV(LeftColumn, MakeLabel(*WidgetTree, TEXT("Calendar")), FMargin(0.0f, 0.0f, 0.0f, 10.0f));
	for (int32 Round = 0; Round < Season.Rounds.Num(); ++Round)
	{
		const FApexSeasonRound& R = Season.Rounds[Round];
		FApexTrackCatalogRow Track;
		Flow->GetTrackCatalogRow(R.TrackId, Track);
		// A round still to come names its own rules; NEXT marks the one Race round N makes.
		FString Detail = R.HasOwnRules() ? ApexSeason::DescribeRound(Season, Round).ToUpper() : FString();
		if (Round == Next)
		{
			Detail = Detail.IsEmpty() ? FString(TEXT("NEXT")) : FString(TEXT("NEXT  ·  ")) + Detail;
		}
		if (R.bDone)
		{
			const FApexSeasonEntry* Winner = R.Results.FindByPredicate([](const FApexSeasonEntry& E) { return E.Position == 1; });
			const FApexSeasonEntry* You = R.Results.FindByPredicate([](const FApexSeasonEntry& E) { return E.bYou; });
			Detail = FString::Printf(TEXT("%s  ·  YOU %s"), Winner ? *Winner->Name.ToUpper() : TEXT("-"),
				You && You->Position > 0 ? *FString::Printf(TEXT("P%d"), You->Position) : TEXT("DNF"));
			// The fastest lap's points, on the round that gave them.
			const int32 Scorer = ApexSeason::FastestLapScorer(Season, R);
			if (R.Results.IsValidIndex(Scorer))
			{
				Detail += FString::Printf(TEXT("  ·  FL %s +%d"), R.Results[Scorer].bYou ? TEXT("YOU") : *R.Results[Scorer].Name.ToUpper(), Season.FastestLapPoints);
			}
		}
		const FLinearColor Ink = Round == Next ? Palette::Accent : (R.bDone ? Palette::TextPrimary : Palette::TextMuted);
		UHorizontalBox* Line = WidgetTree->ConstructWidget<UHorizontalBox>();
		AddH(Line, MakeSized(*WidgetTree, MakeText(*WidgetTree, FString::Printf(TEXT("R%d"), Round + 1), Font::Mono(13.0f), Ink), 44.0f, -1.0f), FMargin(), VAlign_Center);
		AddH(Line, MakeText(*WidgetTree, Track.DisplayName, Font::Body(15.0f), Ink), FMargin(), VAlign_Center, 1.0f);
		AddH(Line, MakeText(*WidgetTree, Detail, Font::Mono(11.0f), Round == Next ? Palette::Accent : Palette::TextSecondary), FMargin(8.0f, 0.0f, 0.0f, 0.0f), VAlign_Center);
		AddV(LeftColumn, Line, FMargin(0.0f, 3.0f));
	}

	// The standings.
	// The success ballast the next round hands each driver, by place.
	const TArray<FApexDriverBallast> Ballast = ApexSeason::Ballast(Season);
	const bool bFastestLap = Season.FastestLapPoints > 0;
	auto AddTable = [this, Flow, &Ballast, bFastestLap](const TCHAR* Title, const TArray<ApexSeason::FStanding>& Lines, int32 MaxLines, bool bCars)
	{
		ApexUI::AddV(RightColumn, ApexUI::MakeLabel(*WidgetTree, Title), FMargin(0.0f, bCars ? 18.0f : 0.0f, 0.0f, 8.0f));
		if (Lines.Num() == 0)
		{
			ApexUI::AddV(RightColumn, ApexUI::MakeText(*WidgetTree, TEXT("NO ROUND RACED YET"), ApexUI::Font::Mono(11.0f), ApexUI::Palette::TextMuted));
		}
		for (int32 Index = 0; Index < FMath::Min(Lines.Num(), MaxLines); ++Index)
		{
			const ApexSeason::FStanding& S = Lines[Index];
			FString Name = S.Key;
			if (bCars)
			{
				FApexCarCatalogRow Row;
				Name = Flow->GetCarCatalogRow(S.Key, Row) ? Row.DisplayName : FString(TEXT("Unknown car"));
			}
			const FLinearColor Ink = S.bYou ? ApexUI::Palette::Accent : ApexUI::Palette::TextPrimary;
			UHorizontalBox* Line = WidgetTree->ConstructWidget<UHorizontalBox>();
			ApexUI::AddH(Line, ApexUI::MakeSized(*WidgetTree, ApexUI::MakeText(*WidgetTree, FString::FromInt(Index + 1), ApexUI::Font::Mono(13.0f), ApexUI::Palette::TextMuted), 36.0f, -1.0f), FMargin(), VAlign_Center);
			ApexUI::AddH(Line, ApexUI::MakeText(*WidgetTree, Name.ToUpper(), ApexUI::Font::Body(15.0f), Ink), FMargin(), VAlign_Center, 1.0f);
			if (!bCars && Ballast.IsValidIndex(Index))
			{
				ApexUI::AddH(Line, ApexUI::MakeSized(*WidgetTree, ApexUI::MakeText(*WidgetTree, FString::Printf(TEXT("+%d KG"), Ballast[Index].Kg),
					ApexUI::Font::Mono(11.0f), ApexUI::Palette::TextSecondary), 64.0f, -1.0f), FMargin(), VAlign_Center);
			}
			if (bFastestLap)
			{
				// The fastest laps that scored, each worth the season's bonus.
				ApexUI::AddH(Line, ApexUI::MakeSized(*WidgetTree, ApexUI::MakeText(*WidgetTree, S.FastestLaps > 0 ? FString::Printf(TEXT("%d FL"), S.FastestLaps) : FString(),
					ApexUI::Font::Mono(11.0f), ApexUI::Palette::Accent), 52.0f, -1.0f), FMargin(), VAlign_Center);
			}
			ApexUI::AddH(Line, ApexUI::MakeSized(*WidgetTree, ApexUI::MakeText(*WidgetTree, S.Wins > 0 ? FString::Printf(TEXT("%d WIN%s"), S.Wins, S.Wins == 1 ? TEXT("") : TEXT("S")) : FString(), ApexUI::Font::Mono(11.0f), ApexUI::Palette::TextSecondary), 80.0f, -1.0f), FMargin(), VAlign_Center);
			UTextBlock* Points = ApexUI::MakeText(*WidgetTree, FString::FromInt(S.Points), ApexUI::Font::Mono(15.0f), Ink);
			Points->SetJustification(ETextJustify::Right);
			ApexUI::AddH(Line, ApexUI::MakeSized(*WidgetTree, Points, 60.0f, -1.0f), FMargin(), VAlign_Center);
			ApexUI::AddV(RightColumn, Line, FMargin(0.0f, 2.0f));
		}
	};
	AddTable(TEXT("Drivers"), ApexSeason::DriverStandings(Season), 12, false);
	AddTable(TEXT("Cars"), ApexSeason::CarStandings(Season), 4, true);
}

void UApexSeasonWidget::HandleStepperChanged(UApexStepperWidget* Control, int32 Value)
{
	if (!Control)
	{
		return;
	}
	const FName Id = Control->ControlId;
	if (Id == StepClass)       { ClassIndex = Value; CarIndex = 0; }
	else if (Id == StepCar)    { CarIndex = Value; }
	else if (Id == StepRounds) { RoundCount = Value; }
	else if (Id == StepPoints) { PointsIndex = Value; }
	else if (Id == StepLaps)   { Laps = Value; }
	else if (Id == StepAi)     { AiCount = Value; }
	else if (Id == StepLevel)  { AiSkill = LevelOf(Value); }
	else if (Id == StepDrops)  { DropRounds = Value; }
	else if (Id == StepBallast) { BallastIndex = Value; }
	else if (Id == StepFastestLap) { FastestLapPoints = Value; }
	else if (Id == StepFastestLapNeeds) { bFastestLapNeedsPoints = Value != 0; }
	else if (EditingRound != INDEX_NONE)
	{
		FApexSeasonRound& Rules = RulesOf(EditingRound);
		if (Id == StepRoundWeather)         { Rules.Weather = Value - 1; }
		else if (Id == StepRoundStart)      { Rules.StartHour = Value - 1; }
		else if (Id == StepRoundQualifying) { const TArray<int32> Choices = QualifyingChoices(); Rules.QualifyingMinutes = Choices.IsValidIndex(Value - 1) ? Choices[Value - 1] : 0; }
		else if (Id == StepRoundLength)     { ApplyLengthIndex(Rules, Value); }
	}
	// The class changes the car list, and everything the calendar: build again,
	// keeping the pad on the control it was on.
	const int32 Focused = Steppers.IndexOfByKey(Control);
	Rebuild();
	if (Steppers.IsValidIndex(Focused))
	{
		ApexNav::Focus(ApexNav::FindFirstFocusable(Steppers[Focused]));
	}
}

void UApexSeasonWidget::RaceNextRound()
{
	UApexMenuFlowSubsystem* Flow = GetFlow();
	UApexNetSubsystem* Net = GetNet();
	if (!Flow || !Net)
	{
		return;
	}
	if (!Net->IsAuthenticated())
	{
		ShowToast(TEXT("Not connected to a server yet"), true);
		return;
	}
	if (!Flow->BeginSeasonRound())
	{
		return;
	}
	const FApexSeason& Season = Flow->GetSeason();
	const int32 Round = Flow->GetSeasonRoundRunning();
	Net->SelectCar(Season.CarId);
	// The round's own rules over the season's, and success ballast by the
	// standings so far, handed to the session.
	const ApexSeason::FRoundSession Session = ApexSeason::RoundSession(Season, Round, Flow->CreateConditions);
	// The race (or its qualifying) counts in on join, as the main menu's Start session does.
	Flow->bAutoStartOnJoin = true;
	Flow->AutoStartMode = Session.StartMode;
	UE_LOG(LogApexSim, Log, TEXT("Season round %d: track '%s', %d AI, %s, %d driver(s) ballasted"), Round + 1,
		*Session.TrackId, Season.AiCount, *ApexSeason::DescribeRound(Season, Round), Session.Ballast.Num());
	Net->CreateSessionWithOrder(Session.TrackId, Season.AiCount + 1, Season.AiCount, Session.Laps, EApexSessionKind::Practice,
		Flow->CreateAllowedAssists, Session.Conditions, Flow->CreateDamage, Season.AiSkill, Session.RaceSeconds, TArray<FString>(),
		Session.QualifyingSeconds, FApexWeekendFormat(), 0, 0, false, EApexQualifyingFormat::Open, 0, FApexRaceRules(),
		TArray<FApexClassEntry>(), Session.Ballast);
}

void UApexSeasonWidget::HandleButtonActivated(UApexButtonWidget* Button)
{
	UApexMenuFlowSubsystem* Flow = GetFlow();
	if (!Flow || !Button)
	{
		return;
	}
	if (Button == BackButton)
	{
		if (EditingRound != INDEX_NONE)
		{
			HandleBack();
			return;
		}
		GoBack();
		return;
	}
	if (Button == EndButton)
	{
		if (!bEndArmed)
		{
			bEndArmed = true;
			Rebuild();
			ApexNav::Focus(EndButton);
			return;
		}
		bEndArmed = false;
		Flow->EndSeason();
		Rebuild();
		FocusDefault();
		return;
	}
	bEndArmed = false;
	if (const int32 Round = RoundButtons.IndexOfByKey(Button); Round != INDEX_NONE)
	{
		EditRound(Round);
		if (Steppers.IsValidIndex(Steppers.Num() - 4))
		{
			// The round's first rule, past the setup's own steppers.
			ApexNav::Focus(ApexNav::FindFirstFocusable(Steppers[Steppers.Num() - 4]));
		}
		return;
	}
	if (Button == RoundDoneButton)
	{
		HandleBack();
		return;
	}
	if (Button == RoundResetButton)
	{
		if (EditingRound != INDEX_NONE)
		{
			RulesOf(EditingRound).ClearRules();
			Rebuild();
			ApexNav::Focus(RoundResetButton);
		}
		return;
	}
	if (Button == PrimaryButton)
	{
		const FApexSeason& Season = Flow->GetSeason();
		if (Season.Rounds.Num() == 0)
		{
			Flow->StartSeason(SeasonFromSetup());
			Rebuild();
			FocusDefault();
		}
		else if (Season.IsFinished())
		{
			Flow->EndSeason();
			Rebuild();
			FocusDefault();
		}
		else
		{
			RaceNextRound();
		}
	}
}
