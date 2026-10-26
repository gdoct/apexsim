#include "UI/ApexSchoolCardWidget.h"

#include "ApexMenuFlowSubsystem.h"
#include "ApexSettingsSave.h"
#include "ApexSettingsSubsystem.h"
#include "Blueprint/WidgetTree.h"
#include "Catalog/ApexCatalogRows.h"
#include "Components/Border.h"
#include "Components/HorizontalBox.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/SizeBox.h"
#include "Components/Spacer.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Engine/GameInstance.h"
#include "Guide/ApexTrackGuide.h"
#include "School/ApexSchool.h"
#include "School/ApexSchoolSubsystem.h"
#include "UI/ApexButtonWidget.h"
#include "UI/ApexUIStyle.h"

namespace ApexSchoolCard
{
	constexpr float CardWidth = 820.0f;
	/** Under this many seconds left the countdown turns red. */
	constexpr float HurryS = 10.0f;

	/** "1:23" for a limit, "42.3" or "1:02.3" for a countdown. */
	FString Clock(float Seconds, bool bTenths)
	{
		const float Shown = bTenths ? FMath::FloorToFloat(FMath::Max(0.0f, Seconds) * 10.0f) / 10.0f : FMath::CeilToFloat(FMath::Max(0.0f, Seconds));
		const int32 Minutes = FMath::FloorToInt(Shown / 60.0f);
		const float Rest = Shown - Minutes * 60.0f;
		if (!bTenths)
		{
			return FString::Printf(TEXT("%d:%02d"), Minutes, FMath::RoundToInt(Rest));
		}
		return Minutes > 0 ? FString::Printf(TEXT("%d:%04.1f"), Minutes, Rest) : FString::Printf(TEXT("%.1f"), Rest);
	}
}

UApexSchoolCardWidget::UApexSchoolCardWidget(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	SetIsFocusable(true);
}

void UApexSchoolCardWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	BuildLayout();
	SetVisibility(ESlateVisibility::Collapsed);
	if (UApexSchoolSubsystem* School = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexSchoolSubsystem>() : nullptr)
	{
		ChangedHandle = School->OnChanged.AddUObject(this, &UApexSchoolCardWidget::Refresh);
	}
}

void UApexSchoolCardWidget::NativeDestruct()
{
	if (UApexSchoolSubsystem* School = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexSchoolSubsystem>() : nullptr)
	{
		School->OnChanged.Remove(ChangedHandle);
	}
	ChangedHandle.Reset();
	Super::NativeDestruct();
}

void UApexSchoolCardWidget::BuildLayout()
{
	using namespace ApexUI;
	auto MakeButton = [this](const FString& Label, EApexButtonVariant Variant, EApexUiSound Sound)
	{
		FApexButtonSpec Spec;
		Spec.Label = Label;
		Spec.Variant = Variant;
		Spec.Height = 56.0f;
		Spec.bCentreLabel = true;
		Spec.LabelSize = 17.0f;
		Spec.Sound = Sound;
		UApexButtonWidget* Button = WidgetTree->ConstructWidget<UApexButtonWidget>();
		Button->Setup(Spec);
		Button->OnActivated.AddDynamic(this, &UApexSchoolCardWidget::HandleButtonActivated);
		return Button;
	};

	UVerticalBox* Card = WidgetTree->ConstructWidget<UVerticalBox>();
	KickerText = MakeLabel(*WidgetTree, FString(), Palette::Accent);
	AddV(Card, KickerText);
	TitleText = MakeText(*WidgetTree, FString(), Font::Display(36.0f), Palette::TextPrimary);
	AddV(Card, TitleText, FMargin(0.0f, 8.0f, 0.0f, 6.0f));
	GoalText = MakeText(*WidgetTree, FString(), Font::Body(16.0f), Palette::TextSecondary);
	GoalText->SetAutoWrapText(true);
	AddV(Card, GoalText, FMargin(0.0f, 0.0f, 0.0f, 14.0f));
	MedalText = MakeText(*WidgetTree, FString(), Font::Display(48.0f), Palette::TextPrimary);
	AddV(Card, MedalText);
	FigureText = MakeText(*WidgetTree, FString(), Font::Body(20.0f, true), Palette::TextPrimary);
	AddV(Card, FigureText, FMargin(0.0f, 2.0f, 0.0f, 0.0f));
	WhyText = MakeText(*WidgetTree, FString(), Font::Body(15.0f), Palette::Error);
	WhyText->SetAutoWrapText(true);
	AddV(Card, WhyText, FMargin(0.0f, 6.0f, 0.0f, 0.0f));
	TargetsText = MakeText(*WidgetTree, FString(), Font::Mono(13.0f), Palette::TextSecondary);
	AddV(Card, TargetsText, FMargin(0.0f, 16.0f, 0.0f, 0.0f));
	BestText = MakeText(*WidgetTree, FString(), Font::Mono(12.0f), Palette::TextMuted);
	AddV(Card, BestText, FMargin(0.0f, 10.0f, 0.0f, 0.0f));

	UHorizontalBox* Buttons = WidgetTree->ConstructWidget<UHorizontalBox>();
	LeaveButton = MakeButton(TEXT("Back to school"), EApexButtonVariant::Ghost, EApexUiSound::Back);
	NextButton = MakeButton(TEXT("Next test"), EApexButtonVariant::Ghost, EApexUiSound::Accept);
	PrimaryButton = MakeButton(TEXT("Start"), EApexButtonVariant::Primary, EApexUiSound::Accept);
	AddH(Buttons, MakeSized(*WidgetTree, LeaveButton, 210.0f, -1.0f));
	AddH(Buttons, MakeSized(*WidgetTree, NextButton, 180.0f, -1.0f), FMargin(8.0f, 0.0f, 0.0f, 0.0f));
	AddH(Buttons, WidgetTree->ConstructWidget<USpacer>(), FMargin(), VAlign_Fill, 1.0f);
	AddH(Buttons, MakeSized(*WidgetTree, PrimaryButton, 240.0f, -1.0f));
	AddV(Card, Buttons, FMargin(0.0f, 26.0f, 0.0f, 0.0f));

	FLinearColor Surface = Palette::Surface;
	Surface.A = 0.94f;
	UVerticalBox* Centre = WidgetTree->ConstructWidget<UVerticalBox>();
	AddV(Centre, WidgetTree->ConstructWidget<USpacer>(), FMargin(), HAlign_Fill, 1.0f);
	AddV(Centre, MakeSized(*WidgetTree, MakePanel(*WidgetTree, Card, FMargin(40.0f, 32.0f), MakeBrush(Surface, Palette::Border, 1.0f)),
		ApexSchoolCard::CardWidth, -1.0f), FMargin(), HAlign_Center);
	AddV(Centre, WidgetTree->ConstructWidget<USpacer>(), FMargin(), HAlign_Fill, 1.0f);
	CardHost = Centre;

	// The countdown while a try is driven, top centre.
	UVerticalBox* Timer = WidgetTree->ConstructWidget<UVerticalBox>();
	TimerText = MakeText(*WidgetTree, FString(), Font::Display(40.0f), Palette::TextPrimary);
	TimerText->SetJustification(ETextJustify::Center);
	AddV(Timer, TimerText, FMargin(), HAlign_Center);
	TimerNote = MakeText(*WidgetTree, FString(), Font::Mono(11.0f), Palette::TextMuted);
	TimerNote->SetJustification(ETextJustify::Center);
	AddV(Timer, TimerNote, FMargin(0.0f, 2.0f, 0.0f, 0.0f), HAlign_Center);
	TimerPanel = MakePanel(*WidgetTree, Timer, FMargin(22.0f, 10.0f), MakeBrush(Surface, Palette::Border, 1.0f));
	TimerPanel->SetVisibility(ESlateVisibility::Collapsed);

	UOverlay* Root = WidgetTree->ConstructWidget<UOverlay>();
	UOverlaySlot* CardSlot = Root->AddChildToOverlay(Centre);
	CardSlot->SetHorizontalAlignment(HAlign_Fill);
	CardSlot->SetVerticalAlignment(VAlign_Fill);
	UOverlaySlot* TimerSlot = Root->AddChildToOverlay(TimerPanel);
	TimerSlot->SetHorizontalAlignment(HAlign_Center);
	TimerSlot->SetVerticalAlignment(VAlign_Top);
	TimerSlot->SetPadding(FMargin(0.0f, 36.0f, 0.0f, 0.0f));
	WidgetTree->RootWidget = Root;
}

void UApexSchoolCardWidget::SetOpen(bool bInOpen)
{
	if (bOpen == bInOpen)
	{
		return;
	}
	bOpen = bInOpen;
	if (bOpen)
	{
		Refresh();
	}
	UpdateVisibility();
}

void UApexSchoolCardWidget::UpdateVisibility()
{
	const UGameInstance* GameInstance = GetGameInstance();
	const UApexSchoolSubsystem* School = GameInstance ? GameInstance->GetSubsystem<UApexSchoolSubsystem>() : nullptr;
	const ApexSchool::FTest* Test = School ? School->GetActiveTest() : nullptr;
	const bool bTimer = !bOpen && School && School->IsSchoolSession() && School->GetStage() == EApexSchoolStage::Driving
		&& Test && Test->TimeLimitS > 0.0f;
	if (CardHost)
	{
		CardHost->SetVisibility(bOpen ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);
	}
	if (TimerPanel)
	{
		TimerPanel->SetVisibility(bTimer ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
	}
	SetVisibility(bOpen || bTimer ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);
	if (bTimer)
	{
		RefreshTimer();
	}
}

void UApexSchoolCardWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);
	if (TimerPanel && TimerPanel->GetVisibility() != ESlateVisibility::Collapsed)
	{
		RefreshTimer();
	}
}

void UApexSchoolCardWidget::RefreshTimer()
{
	const UGameInstance* GameInstance = GetGameInstance();
	const UApexSchoolSubsystem* School = GameInstance ? GameInstance->GetSubsystem<UApexSchoolSubsystem>() : nullptr;
	if (!School || !TimerText || !TimerNote)
	{
		return;
	}
	const float Left = School->GetTimeLeftS();
	const bool bRunning = School->IsClockRunning();
	TimerText->SetText(FText::FromString(ApexSchoolCard::Clock(Left, bRunning)));
	TimerText->SetColorAndOpacity(bRunning && Left < ApexSchoolCard::HurryS ? ApexUI::Palette::Error : ApexUI::Palette::TextPrimary);
	TimerNote->SetText(FText::FromString(bRunning ? TEXT("TIME LEFT") : TEXT("TIME LIMIT  ·  STARTS AT THE LINE")));
}

void UApexSchoolCardWidget::Refresh()
{
	const UGameInstance* GameInstance = GetGameInstance();
	const UApexSchoolSubsystem* School = GameInstance ? GameInstance->GetSubsystem<UApexSchoolSubsystem>() : nullptr;
	const UApexMenuFlowSubsystem* Flow = GameInstance ? GameInstance->GetSubsystem<UApexMenuFlowSubsystem>() : nullptr;
	const ApexSchool::FTest* Test = School ? School->GetActiveTest() : nullptr;
	UpdateVisibility();
	if (!bOpen || !Test || !Flow || !TitleText)
	{
		return;
	}
	const UApexSettingsSubsystem* Settings = GameInstance->GetSubsystem<UApexSettingsSubsystem>();
	const bool bMetric = !Settings || !Settings->Get() || Settings->Get()->Units == EApexUnits::Metric;
	const bool bResult = School->GetStage() == EApexSchoolStage::Result;

	FApexTrackCatalogRow Track;
	Flow->GetTrackCatalogRow(School->GetTrackId(), Track);
	FApexCarCatalogRow Car;
	Flow->GetCarCatalogRow(School->GetCarId(), Car);
	KickerText->SetText(FText::FromString(FString::Printf(TEXT("%s  ·  %s  ·  %s"), bResult ? TEXT("Result") : TEXT("Licence test"),
		*Track.DisplayName, *Car.DisplayName).ToUpper()));
	TitleText->SetText(FText::FromString(Test->Place.IsEmpty() ? Test->Title : FString::Printf(TEXT("%s  ·  %s"), *Test->Title, *Test->Place)));
	GoalText->SetText(FText::FromString(ApexSchool::GoalText(*Test, bMetric)));

	const ApexSchool::FResult& Result = School->GetResult();
	if (bResult)
	{
		MedalText->SetText(FText::FromString(Result.bPassed ? ApexSchool::MedalName(Result.Medal) : FString(TEXT("FAILED"))));
		MedalText->SetColorAndOpacity(Result.bPassed ? ApexSchool::MedalColour(Result.Medal) : ApexUI::Palette::Error);
		FString Figure;
		switch (Test->Kind)
		{
		case EApexSchoolKind::Brake:
			Figure = FString::Printf(TEXT("%s  ·  %s at the board"), *ApexSchool::FigureText(*Test, Result.Value, bMetric),
				*ApexTrackGuide::SpeedText(Result.SpeedMps * 3.6f, bMetric));
			break;
		case EApexSchoolKind::CornerExit:
		case EApexSchoolKind::CornerTime:
			Figure = FString::Printf(TEXT("Out at %s  ·  through in %.3f s"), *ApexTrackGuide::SpeedText(Result.SpeedMps * 3.6f, bMetric), Result.Seconds);
			break;
		case EApexSchoolKind::Lap:
		default:
			Figure = FString::Printf(TEXT("Lap %s"), *ApexSchool::FigureText(*Test, Result.Seconds, bMetric));
			break;
		}
		// A try failed before it was measured (off the track, out of time) has no figure.
		const bool bShowFigure = Result.bPassed || Result.SpeedMps > 0.0f || Result.Seconds > 0.0f;
		FigureText->SetText(FText::FromString(bShowFigure ? Figure : FString()));
		WhyText->SetText(FText::FromString(Result.bPassed ? (School->IsNewBest() ? FString(TEXT("New best")) : FString()) : Result.Why));
		WhyText->SetColorAndOpacity(Result.bPassed ? ApexUI::Palette::Live : ApexUI::Palette::Error);
		PrimaryButton->SetLabel(TEXT("Retry"));
	}
	else
	{
		MedalText->SetText(FText::GetEmpty());
		FigureText->SetText(FText::GetEmpty());
		WhyText->SetText(FText::FromString(School->GetNotice().IsEmpty() ? FString()
			: FString::Printf(TEXT("Last try given up: %s"), *School->GetNotice())));
		WhyText->SetColorAndOpacity(ApexUI::Palette::TextMuted);
		PrimaryButton->SetLabel(TEXT("Start"));
	}

	FString Targets = FString::Printf(TEXT("GOLD    %s\nSILVER  %s\nBRONZE  %s"),
		*ApexSchool::TargetText(*Test, EApexMedal::Gold, bMetric), *ApexSchool::TargetText(*Test, EApexMedal::Silver, bMetric),
		*ApexSchool::TargetText(*Test, EApexMedal::Bronze, bMetric));
	if (Test->TimeLimitS > 0.0f)
	{
		Targets += FString::Printf(TEXT("\nTIME    %s from the line"), *ApexSchoolCard::Clock(Test->TimeLimitS, false));
	}
	TargetsText->SetText(FText::FromString(Targets));
	const FApexSchoolMedal* Best = School->GetBest(Test->Id);
	BestText->SetText(FText::FromString(Best && Best->Medal != EApexMedal::None
		? FString::Printf(TEXT("YOUR BEST: %s, %s  ·  %d TRIES"), *ApexSchool::MedalName(Best->Medal),
			*ApexSchool::FigureText(*Test, Best->Best, bMetric), Best->Tries)
		: FString::Printf(TEXT("NO MEDAL YET  ·  %d TRIES"), Best ? Best->Tries : 0)));
	NextButton->SetVisibility(School->GetTests().Num() > 1 ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
}

void UApexSchoolCardWidget::FocusDefault()
{
	if (!(PrimaryButton && ApexNav::Focus(PrimaryButton)))
	{
		SetKeyboardFocus();
	}
}

bool UApexSchoolCardWidget::HandleNavigation(EUINavigation Direction, UWidget* Source)
{
	return ApexNav::MoveToward(this, Direction, Source);
}

void UApexSchoolCardWidget::HandleButtonActivated(UApexButtonWidget* Button)
{
	UApexSchoolSubsystem* School = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexSchoolSubsystem>() : nullptr;
	if (!School || !Button)
	{
		return;
	}
	if (Button == PrimaryButton)
	{
		School->Go();
	}
	else if (Button == NextButton)
	{
		School->NextTest();
		FocusDefault();
	}
	else if (Button == LeaveButton)
	{
		School->LeaveToSchool();
	}
}
