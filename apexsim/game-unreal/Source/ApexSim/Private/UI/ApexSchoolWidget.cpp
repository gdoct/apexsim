#include "UI/ApexSchoolWidget.h"

#include "ApexMenuFlowSubsystem.h"
#include "ApexNetSubsystem.h"
#include "ApexSettingsSave.h"
#include "ApexSettingsSubsystem.h"
#include "ApexSim.h"
#include "Blueprint/WidgetTree.h"
#include "Catalog/ApexCatalogRows.h"
#include "Components/Border.h"
#include "Components/HorizontalBox.h"
#include "Components/SizeBox.h"
#include "Components/ScrollBox.h"
#include "Components/Spacer.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "HAL/IConsoleManager.h"
#include "School/ApexSchool.h"
#include "School/ApexSchoolSubsystem.h"
#include "UI/ApexButtonWidget.h"
#include "UI/ApexNavigation.h"
#include "UI/ApexRootWidget.h"
#include "UI/ApexUIStyle.h"
#include "UObject/UObjectIterator.h"

namespace ApexSchoolScreen
{
	const FName ActionBack(TEXT("School.Back"));
	const FName ActionCar(TEXT("School.Car"));
	const FName ActionTrack(TEXT("School.Track"));
	const FName ActionTest(TEXT("School.Test"));

	constexpr float CardWidth = 1320.0f;

	UApexSchoolWidget* ScreenOf(const UWorld* World)
	{
		for (TObjectIterator<UApexSchoolWidget> It; It; ++It)
		{
			if (It->GetWorld() == World && !It->HasAnyFlags(RF_ClassDefaultObject))
			{
				return *It;
			}
		}
		return nullptr;
	}

	FAutoConsoleCommandWithWorldAndArgs SelectCommand(TEXT("apexsim.school.Select"),
		TEXT("apexsim.school.Select <track stem> [car name]: choose a circuit on the School screen, as its row does (in that car)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			UApexSchoolWidget* Screen = ScreenOf(World);
			const UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
			UApexMenuFlowSubsystem* Flow = GameInstance ? GameInstance->GetSubsystem<UApexMenuFlowSubsystem>() : nullptr;
			const UApexNetSubsystem* Net = GameInstance ? GameInstance->GetSubsystem<UApexNetSubsystem>() : nullptr;
			if (!Screen || !Flow || Args.Num() < 1)
			{
				return;
			}
			if (Net && Args.Num() >= 2)
			{
				const FString Wanted = FString::Join(TArrayView<const FString>(Args).RightChop(1), TEXT(" "));
				for (const FApexCarConfigSummary& Car : Net->GetCachedLobbyState().CarConfigs)
				{
					FApexCarCatalogRow Row;
					if (Flow->GetCarCatalogRow(Car.Id, Row) && Row.DisplayName.Contains(Wanted))
					{
						Flow->SetPendingCar(Car.Id);
						break;
					}
				}
			}
			Screen->SelectTrack(Flow->FindTrackIdByStem(Args[0]));
		}));

	bool IsMetric(const UObject* Context)
	{
		const UGameInstance* GameInstance = Context && Context->GetWorld() ? Context->GetWorld()->GetGameInstance() : nullptr;
		const UApexSettingsSubsystem* Settings = GameInstance ? GameInstance->GetSubsystem<UApexSettingsSubsystem>() : nullptr;
		return !Settings || !Settings->Get() || Settings->Get()->Units == EApexUnits::Metric;
	}
}

UApexSchoolWidget::UApexSchoolWidget(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	SetIsFocusable(true);
}

void UApexSchoolWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	BuildLayout();
}

void UApexSchoolWidget::BuildLayout()
{
	using namespace ApexUI;
	using namespace ApexSchoolScreen;
	auto MakeButton = [this](const FString& Label, FName Action, EApexButtonVariant Variant)
	{
		FApexButtonSpec Spec;
		Spec.Label = Label;
		Spec.Variant = Variant;
		Spec.ActionId = Action;
		Spec.Height = 56.0f;
		Spec.bCentreLabel = true;
		Spec.LabelSize = 17.0f;
		if (Action == ActionBack)
		{
			Spec.Sound = EApexUiSound::Back;
		}
		UApexButtonWidget* Button = WidgetTree->ConstructWidget<UApexButtonWidget>();
		Button->Setup(Spec);
		Button->OnActivated.AddDynamic(this, &UApexSchoolWidget::HandleButtonActivated);
		return Button;
	};

	UVerticalBox* Card = WidgetTree->ConstructWidget<UVerticalBox>();
	AddV(Card, MakeLabel(*WidgetTree, TEXT("Driving school"), Palette::Accent));
	AddV(Card, MakeText(*WidgetTree, TEXT("Licence tests"), Font::Display(42.0f), Palette::TextPrimary), FMargin(0.0f, 10.0f, 0.0f, 4.0f));
	StatusText = MakeText(*WidgetTree, FString(), Font::Mono(12.0f), Palette::TextMuted);
	StatusText->SetAutoWrapText(true);
	AddV(Card, StatusText, FMargin(0.0f, 0.0f, 0.0f, 22.0f));

	UVerticalBox* Left = WidgetTree->ConstructWidget<UVerticalBox>();
	AddV(Left, MakeLabel(*WidgetTree, TEXT("Circuits")), FMargin(0.0f, 0.0f, 0.0f, 10.0f));
	TrackList = WidgetTree->ConstructWidget<UScrollBox>();
	AddV(Left, TrackList, FMargin(), HAlign_Fill, 1.0f);

	UVerticalBox* Right = WidgetTree->ConstructWidget<UVerticalBox>();
	TrackTitle = MakeText(*WidgetTree, FString(), Font::Display(28.0f), Palette::TextPrimary);
	AddV(Right, TrackTitle);
	PrepText = MakeText(*WidgetTree, FString(), Font::Mono(12.0f), Palette::TextMuted);
	PrepText->SetAutoWrapText(true);
	AddV(Right, PrepText, FMargin(0.0f, 4.0f, 0.0f, 14.0f));
	TestBox = WidgetTree->ConstructWidget<UVerticalBox>();
	AddV(Right, TestBox, FMargin(), HAlign_Fill, 1.0f);

	UHorizontalBox* Columns = WidgetTree->ConstructWidget<UHorizontalBox>();
	AddH(Columns, MakeSized(*WidgetTree, Left, 440.0f, -1.0f), FMargin(), VAlign_Fill);
	AddH(Columns, MakeDivider(*WidgetTree, true), FMargin(28.0f, 0.0f), VAlign_Fill);
	AddH(Columns, Right, FMargin(), VAlign_Top, 1.0f);
	AddV(Card, MakeSized(*WidgetTree, Columns, -1.0f, 640.0f));

	UHorizontalBox* Buttons = WidgetTree->ConstructWidget<UHorizontalBox>();
	BackButton = MakeButton(TEXT("Back"), ActionBack, EApexButtonVariant::Ghost);
	CarButton = MakeButton(TEXT("Change car"), ActionCar, EApexButtonVariant::Ghost);
	AddH(Buttons, MakeSized(*WidgetTree, BackButton, 180.0f, -1.0f));
	AddH(Buttons, MakeSized(*WidgetTree, CarButton, 220.0f, -1.0f), FMargin(8.0f, 0.0f, 0.0f, 0.0f));
	AddH(Buttons, WidgetTree->ConstructWidget<USpacer>(), FMargin(), VAlign_Fill, 1.0f);
	AddV(Card, Buttons, FMargin(0.0f, 20.0f, 0.0f, 0.0f));

	UVerticalBox* Centre = WidgetTree->ConstructWidget<UVerticalBox>();
	AddV(Centre, WidgetTree->ConstructWidget<USpacer>(), FMargin(), HAlign_Fill, 1.0f);
	AddV(Centre, MakeSized(*WidgetTree,
		MakePanel(*WidgetTree, Card, FMargin(44.0f, 36.0f), MakeBrush(Palette::Surface, Palette::Border, 1.0f)),
		CardWidth, -1.0f), FMargin(), HAlign_Center);
	AddV(Centre, WidgetTree->ConstructWidget<USpacer>(), FMargin(), HAlign_Fill, 1.2f);
	WidgetTree->RootWidget = MakePanel(*WidgetTree, Centre, FMargin(), MakeBrush(Palette::Background));
}

void UApexSchoolWidget::OnScreenActivated()
{
	Super::OnScreenActivated();
	if (UApexNetSubsystem* Net = GetNet(); Net && !bBound)
	{
		Net->OnLobbyStateUpdated.AddDynamic(this, &UApexSchoolWidget::HandleLobbyState);
		bBound = true;
	}
	if (UApexSchoolSubsystem* School = GetGameInstance()->GetSubsystem<UApexSchoolSubsystem>(); School && !SchoolChangedHandle.IsValid())
	{
		SchoolChangedHandle = School->OnChanged.AddUObject(this, &UApexSchoolWidget::HandleSchoolChanged);
	}
	LoadTracks();
	// The circuit the tests were last made for, else the pending one.
	const UApexSchoolSubsystem* School = GetGameInstance()->GetSubsystem<UApexSchoolSubsystem>();
	const UApexMenuFlowSubsystem* Flow = GetFlow();
	FString Wanted = School && TrackIds.Contains(School->GetTrackId()) ? School->GetTrackId() : FString();
	if (Wanted.IsEmpty() && Flow && TrackIds.Contains(Flow->GetPendingTrackId()))
	{
		Wanted = Flow->GetPendingTrackId();
	}
	RebuildTracks();
	if (!Wanted.IsEmpty())
	{
		SelectTrack(Wanted);
	}
	else
	{
		RebuildTests();
	}
	RefreshStatus();
	FocusDefault();
}

void UApexSchoolWidget::OnScreenDeactivated()
{
	if (UApexSchoolSubsystem* School = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexSchoolSubsystem>() : nullptr)
	{
		School->OnChanged.Remove(SchoolChangedHandle);
	}
	SchoolChangedHandle.Reset();
	Super::OnScreenDeactivated();
}

void UApexSchoolWidget::HandleLobbyState(const FApexLobbyState& LobbyState)
{
	if (IsActiveScreen() && TrackIds.Num() == 0 && LobbyState.TrackConfigs.Num() > 0)
	{
		LoadTracks();
		RebuildTracks();
		FocusDefault();
	}
}

void UApexSchoolWidget::HandleSchoolChanged()
{
	if (!IsActiveScreen())
	{
		return;
	}
	// Keep the pad where it was across the rebuild.
	const int32 FocusedTest = TestButtons.IndexOfByPredicate([](const UApexButtonWidget* B) { return B && B->HasFocusedDescendants(); });
	RebuildTests();
	RebuildTracks();
	RefreshStatus();
	if (TestButtons.IsValidIndex(FocusedTest))
	{
		ApexNav::Focus(TestButtons[FocusedTest]);
	}
}

void UApexSchoolWidget::FocusDefault()
{
	const int32 Selected = TrackIds.IndexOfByKey(SelectedTrackId);
	if (TestButtons.Num() > 0 && ApexNav::Focus(TestButtons[0]))
	{
		return;
	}
	if (TrackButtons.IsValidIndex(Selected) && ApexNav::Focus(TrackButtons[Selected]))
	{
		return;
	}
	if (TrackButtons.Num() > 0 && ApexNav::Focus(TrackButtons[0]))
	{
		return;
	}
	if (!(BackButton && ApexNav::Focus(BackButton)))
	{
		SetKeyboardFocus();
	}
}

bool UApexSchoolWidget::HandleNavigation(EUINavigation Direction, UWidget* Source)
{
	return ApexNav::MoveToward(this, Direction, Source);
}

void UApexSchoolWidget::LoadTracks()
{
	TrackIds.Reset();
	const UApexNetSubsystem* Net = GetNet();
	const UApexMenuFlowSubsystem* Flow = GetFlow();
	if (!Net || !Flow)
	{
		return;
	}
	TArray<TPair<FString, FString>> Tracks;
	for (const FApexTrackConfigSummary& Track : Net->GetCachedLobbyState().TrackConfigs)
	{
		FApexTrackCatalogRow Row;
		if (Flow->GetTrackCatalogRow(Track.Id, Row) && !Row.DisplayName.IsEmpty() && !Row.YamlBaseName.IsEmpty())
		{
			Tracks.Emplace(Row.DisplayName, Track.Id);
		}
	}
	Tracks.Sort([](const TPair<FString, FString>& A, const TPair<FString, FString>& B) { return A.Key.Compare(B.Key, ESearchCase::IgnoreCase) < 0; });
	for (const TPair<FString, FString>& Track : Tracks)
	{
		TrackIds.Add(Track.Value);
	}
}

FString UApexSchoolWidget::MedalSummary(const FString& TrackId) const
{
	const UApexMenuFlowSubsystem* Flow = GetFlow();
	if (!Flow || !Flow->HasPendingCar())
	{
		return FString();
	}
	int32 Counts[4] = { 0, 0, 0, 0 };
	const FString Prefix = ApexSchool::MedalKey(TrackId, Flow->GetPendingCarId(), FString());
	for (const FApexSchoolMedal& Medal : Flow->GetSchoolMedals())
	{
		if (Medal.Key.StartsWith(Prefix, ESearchCase::IgnoreCase))
		{
			++Counts[static_cast<int32>(Medal.Medal)];
		}
	}
	TArray<FString> Parts;
	for (int32 Index = 3; Index >= 1; --Index)
	{
		if (Counts[Index] > 0)
		{
			Parts.Add(FString::Printf(TEXT("%d %s"), Counts[Index], *ApexSchool::MedalName(static_cast<EApexMedal>(Index))));
		}
	}
	return FString::Join(Parts, TEXT(" · "));
}

void UApexSchoolWidget::RefreshStatus()
{
	const UApexMenuFlowSubsystem* Flow = GetFlow();
	if (!Flow || !StatusText)
	{
		return;
	}
	FApexCarCatalogRow Car;
	if (!Flow->HasPendingCar() || !Flow->GetCarCatalogRow(Flow->GetPendingCarId(), Car))
	{
		StatusText->SetText(FText::FromString(TEXT("PICK A CAR: THE TESTS ARE MADE FOR ITS CLASS FROM THE TRACK GUIDE")));
		return;
	}
	int32 Counts[4] = { 0, 0, 0, 0 };
	const FString CarPart = FString::Printf(TEXT("|%s|"), *Flow->GetPendingCarId());
	for (const FApexSchoolMedal& Medal : Flow->GetSchoolMedals())
	{
		if (Medal.Key.Contains(CarPart, ESearchCase::IgnoreCase))
		{
			++Counts[static_cast<int32>(Medal.Medal)];
		}
	}
	StatusText->SetText(FText::FromString(FString::Printf(
		TEXT("%s  ·  %s  ·  %d GOLD, %d SILVER, %d BRONZE IN THIS CAR  ·  MEDALS UNLOCK NOTHING YET"),
		*Car.DisplayName.ToUpper(), *ApexCatalog::DisplayClass(Car.CarClass).ToUpper(), Counts[3], Counts[2], Counts[1])));
}

void UApexSchoolWidget::RebuildTracks()
{
	using namespace ApexSchoolScreen;
	if (!TrackList)
	{
		return;
	}
	const UApexMenuFlowSubsystem* Flow = GetFlow();
	const int32 Focused = TrackButtons.IndexOfByPredicate([](const UApexButtonWidget* B) { return B && B->HasFocusedDescendants(); });
	TrackList->ClearChildren();
	TrackButtons.Reset();
	if (TrackIds.Num() == 0)
	{
		TrackList->AddChild(ApexUI::MakeText(*WidgetTree, TEXT("CONNECT TO A SERVER FIRST: ITS CIRCUITS ARE THE SCHOOL'S"),
			ApexUI::Font::Mono(12.0f), ApexUI::Palette::TextMuted));
		return;
	}
	for (const FString& TrackId : TrackIds)
	{
		FApexTrackCatalogRow Row;
		Flow->GetTrackCatalogRow(TrackId, Row);
		FApexButtonSpec Spec;
		Spec.Label = Row.DisplayName;
		Spec.Badge = MedalSummary(TrackId);
		Spec.BadgeColour = ApexUI::Palette::Accent;
		Spec.ActionId = ActionTrack;
		Spec.Height = 48.0f;
		UApexButtonWidget* Button = WidgetTree->ConstructWidget<UApexButtonWidget>();
		Button->Setup(Spec);
		Button->SetSelected(TrackId == SelectedTrackId);
		Button->OnActivated.AddDynamic(this, &UApexSchoolWidget::HandleButtonActivated);
		TrackList->AddChild(Button);
		TrackButtons.Add(Button);
	}
	if (TrackButtons.IsValidIndex(Focused))
	{
		ApexNav::Focus(TrackButtons[Focused]);
	}
}

void UApexSchoolWidget::RebuildTests()
{
	using namespace ApexUI;
	using namespace ApexSchoolScreen;
	if (!TestBox)
	{
		return;
	}
	TestBox->ClearChildren();
	TestButtons.Reset();
	const UApexMenuFlowSubsystem* Flow = GetFlow();
	const UApexSchoolSubsystem* School = GetGameInstance()->GetSubsystem<UApexSchoolSubsystem>();
	FApexTrackCatalogRow Row;
	if (SelectedTrackId.IsEmpty() || !Flow || !School || !Flow->GetTrackCatalogRow(SelectedTrackId, Row))
	{
		TrackTitle->SetText(FText::FromString(TEXT("Choose a circuit")));
		PrepText->SetText(FText::FromString(TEXT("EACH CIRCUIT HAS A BRAKE TEST, A CORNER EXIT, A CORNER TIME AND A CLEAN LAP, CUT FROM ITS TRACK GUIDE")));
		return;
	}
	TrackTitle->SetText(FText::FromString(Row.DisplayName));
	const bool bOurs = School->GetTrackId() == SelectedTrackId;
	const EApexSchoolPrep Prep = bOurs ? School->GetPrep() : EApexSchoolPrep::None;
	if (Prep != EApexSchoolPrep::Ready)
	{
		PrepText->SetText(FText::FromString((Prep == EApexSchoolPrep::None ? FString(TEXT("Preparing the tests")) : School->GetPrepMessage()).ToUpper()
			+ (Prep == EApexSchoolPrep::Failed ? FString() : FString(TEXT("…")))));
		PrepText->SetColorAndOpacity(Prep == EApexSchoolPrep::Failed ? Palette::Error : Palette::TextMuted);
		return;
	}
	PrepText->SetColorAndOpacity(Palette::TextMuted);
	PrepText->SetText(FText::FromString(TEXT("TARGETS FROM THE TRACK GUIDE CAR OF THE CLASS  ·  ENTER STARTS A TEST")));
	const bool bMetric = IsMetric(this);
	for (const ApexSchool::FTest& Test : School->GetTests())
	{
		if (!Test.IsReady())
		{
			continue;
		}
		const FApexSchoolMedal* Best = School->GetBest(Test.Id);
		const EApexMedal Medal = Best ? Best->Medal : EApexMedal::None;
		FApexButtonSpec Spec;
		Spec.Label = Test.Place.IsEmpty() ? Test.Title : FString::Printf(TEXT("%s  ·  %s"), *Test.Title, *Test.Place);
		Spec.Badge = Medal != EApexMedal::None
			? FString::Printf(TEXT("%s  %s"), *ApexSchool::MedalName(Medal), *ApexSchool::FigureText(Test, Best->Best, bMetric))
			: (Best && Best->Tries > 0 ? FString::Printf(TEXT("%d TRIES"), Best->Tries) : FString(TEXT("NOT TRIED")));
		Spec.BadgeColour = ApexSchool::MedalColour(Medal);
		Spec.ActionId = ActionTest;
		Spec.Height = 52.0f;
		UApexButtonWidget* Button = WidgetTree->ConstructWidget<UApexButtonWidget>();
		Button->Setup(Spec);
		Button->OnActivated.AddDynamic(this, &UApexSchoolWidget::HandleButtonActivated);
		AddV(TestBox, Button, FMargin(0.0f, TestButtons.Num() == 0 ? 0.0f : 14.0f, 0.0f, 0.0f));
		TestButtons.Add(Button);

		UTextBlock* Goal = MakeText(*WidgetTree, ApexSchool::GoalText(Test, bMetric), Font::Body(14.0f), Palette::TextSecondary);
		Goal->SetAutoWrapText(true);
		AddV(TestBox, Goal, FMargin(12.0f, 6.0f, 0.0f, 0.0f));
		const FString Targets = FString::Printf(TEXT("GOLD %s  ·  SILVER %s  ·  BRONZE %s"),
			*ApexSchool::TargetText(Test, EApexMedal::Gold, bMetric).ToUpper(), *ApexSchool::TargetText(Test, EApexMedal::Silver, bMetric).ToUpper(),
			*ApexSchool::TargetText(Test, EApexMedal::Bronze, bMetric).ToUpper());
		AddV(TestBox, MakeText(*WidgetTree, Targets, Font::Mono(11.0f), Palette::TextMuted), FMargin(12.0f, 4.0f, 0.0f, 0.0f));
	}
}

void UApexSchoolWidget::SelectTrack(const FString& TrackId)
{
	if (TrackId.IsEmpty())
	{
		return;
	}
	SelectedTrackId = TrackId;
	for (int32 Index = 0; Index < TrackButtons.Num(); ++Index)
	{
		TrackButtons[Index]->SetSelected(TrackIds.IsValidIndex(Index) && TrackIds[Index] == SelectedTrackId);
	}
	if (UApexSchoolSubsystem* School = GetGameInstance()->GetSubsystem<UApexSchoolSubsystem>())
	{
		School->Prepare(TrackId);
	}
	RebuildTests();
	RefreshStatus();
}

void UApexSchoolWidget::HandleButtonActivated(UApexButtonWidget* Button)
{
	using namespace ApexSchoolScreen;
	if (!Button)
	{
		return;
	}
	if (Button == BackButton)
	{
		GoBack();
		return;
	}
	if (Button == CarButton)
	{
		if (UApexRootWidget* Root = GetRoot())
		{
			Root->ScreenAfterCarSelect = EApexScreen::School;
		}
		ShowScreen(EApexScreen::CarSelect);
		return;
	}
	if (const int32 Track = TrackButtons.IndexOfByKey(Button); Track != INDEX_NONE && TrackIds.IsValidIndex(Track))
	{
		SelectTrack(TrackIds[Track]);
		if (TestButtons.Num() > 0)
		{
			ApexNav::Focus(TestButtons[0]);
		}
		return;
	}
	const int32 Test = TestButtons.IndexOfByKey(Button);
	UApexSchoolSubsystem* School = GetGameInstance()->GetSubsystem<UApexSchoolSubsystem>();
	if (Test == INDEX_NONE || !School)
	{
		return;
	}
	// The buttons list the ready tests in order.
	int32 Seen = 0;
	for (int32 Index = 0; Index < School->GetTests().Num(); ++Index)
	{
		if (!School->GetTests()[Index].IsReady())
		{
			continue;
		}
		if (Seen++ == Test)
		{
			FString Error;
			if (!School->StartTest(Index, Error))
			{
				ShowToast(Error, true);
			}
			return;
		}
	}
}
