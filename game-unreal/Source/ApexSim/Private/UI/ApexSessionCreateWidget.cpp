#include "UI/ApexSessionCreateWidget.h"

#include "ApexMenuFlowSubsystem.h"
#include "ApexNetSubsystem.h"
#include "Components/Button.h"
#include "Components/ComboBoxString.h"
#include "Components/SpinBox.h"
#include "Components/TextBlock.h"
#include "UI/ApexRootWidget.h"

void UApexSessionCreateWidget::NativeConstruct()
{
	Super::NativeConstruct();

	if (TrackSelectorButton) { TrackSelectorButton->OnClicked.AddDynamic(this, &UApexSessionCreateWidget::HandleTrackSelectorClicked); }
	if (CarSelectorButton)   { CarSelectorButton->OnClicked.AddDynamic(this, &UApexSessionCreateWidget::HandleCarSelectorClicked); }
	if (CreateButton)        { CreateButton->OnClicked.AddDynamic(this, &UApexSessionCreateWidget::HandleCreateClicked); }
	if (BackButton)          { BackButton->OnClicked.AddDynamic(this, &UApexSessionCreateWidget::HandleBackClicked); }

	if (SessionKindCombo)
	{
		SessionKindCombo->ClearOptions();
		SessionKindCombo->AddOption(LabelFromKind(EApexSessionKind::Multiplayer));
		SessionKindCombo->AddOption(LabelFromKind(EApexSessionKind::Practice));
		SessionKindCombo->AddOption(LabelFromKind(EApexSessionKind::Sandbox));
		SessionKindCombo->SetSelectedOption(LabelFromKind(EApexSessionKind::Multiplayer));
		SessionKindCombo->OnSelectionChanged.AddDynamic(this, &UApexSessionCreateWidget::HandleKindChanged);
	}

	if (MaxPlayersSpin)
	{
		MaxPlayersSpin->SetMinValue(1);
		MaxPlayersSpin->SetMaxValue(16);
		MaxPlayersSpin->SetValue(8);
	}
	if (AiCountSpin)
	{
		AiCountSpin->SetMinValue(0);
		AiCountSpin->SetMaxValue(15);
		AiCountSpin->SetValue(0);
	}
	if (LapLimitSpin)
	{
		LapLimitSpin->SetMinValue(1);
		LapLimitSpin->SetMaxValue(50);
		LapLimitSpin->SetValue(5);
	}
}

void UApexSessionCreateWidget::NativeDestruct()
{
	Super::NativeDestruct();
}

void UApexSessionCreateWidget::OnScreenActivated()
{
	Super::OnScreenActivated();
	RefreshSelections();
}

FString UApexSessionCreateWidget::LabelFromKind(EApexSessionKind Kind)
{
	switch (Kind)
	{
	case EApexSessionKind::Practice: return TEXT("Practice");
	case EApexSessionKind::Sandbox:  return TEXT("Sandbox");
	default:                         return TEXT("Multiplayer");
	}
}

EApexSessionKind UApexSessionCreateWidget::KindFromLabel(const FString& Label)
{
	if (Label.Equals(TEXT("Practice"), ESearchCase::IgnoreCase)) { return EApexSessionKind::Practice; }
	if (Label.Equals(TEXT("Sandbox"), ESearchCase::IgnoreCase))  { return EApexSessionKind::Sandbox; }
	return EApexSessionKind::Multiplayer;
}

void UApexSessionCreateWidget::HandleKindChanged(FString SelectedItem, ESelectInfo::Type SelectionType)
{
	if (UApexMenuFlowSubsystem* Flow = GetFlow())
	{
		Flow->CreateSessionKind = KindFromLabel(SelectedItem);
	}
}

void UApexSessionCreateWidget::RefreshSelections()
{
	const UApexMenuFlowSubsystem* Flow = GetFlow();
	const UApexNetSubsystem* Net = GetNet();
	if (!Flow || !Net)
	{
		return;
	}

	if (TrackSelectorLabel)
	{
		FApexTrackConfigSummary Track;
		const bool bResolved = Flow->HasPendingTrack() && Net->FindTrackById(Flow->GetPendingTrackId(), Track);
		TrackSelectorLabel->SetText(FText::FromString(bResolved ? Track.Name : TEXT("Choose a track…")));
	}

	if (CarSelectorLabel)
	{
		FApexCarConfigSummary Car;
		const bool bResolved = Flow->HasPendingCar() && Net->FindCarById(Flow->GetPendingCarId(), Car);
		CarSelectorLabel->SetText(FText::FromString(bResolved ? Car.Name : TEXT("Choose a car…")));
	}

	// Creating without both would be rejected by the server anyway; disabling
	// the button says why up front.
	const bool bReady = Flow->HasPendingTrack() && Flow->HasPendingCar();
	if (CreateButton)
	{
		CreateButton->SetIsEnabled(bReady);
	}
	if (StatusText)
	{
		StatusText->SetText(FText::FromString(bReady
			? TEXT("Ready to create")
			: TEXT("Pick both a track and a car to continue")));
	}
}

void UApexSessionCreateWidget::CommitSpinBoxes()
{
	UApexMenuFlowSubsystem* Flow = GetFlow();
	if (!Flow)
	{
		return;
	}
	if (MaxPlayersSpin) { Flow->CreateMaxPlayers = FMath::RoundToInt(MaxPlayersSpin->GetValue()); }
	if (AiCountSpin)    { Flow->CreateAiCount = FMath::RoundToInt(AiCountSpin->GetValue()); }
	if (LapLimitSpin)   { Flow->CreateLapLimit = FMath::RoundToInt(LapLimitSpin->GetValue()); }
}

void UApexSessionCreateWidget::HandleTrackSelectorClicked()
{
	if (UApexRootWidget* Root = GetRoot())
	{
		Root->ScreenAfterTrackSelect = EApexScreen::SessionCreate;
	}
	ShowScreen(EApexScreen::TrackSelect);
}

void UApexSessionCreateWidget::HandleCarSelectorClicked()
{
	if (UApexRootWidget* Root = GetRoot())
	{
		Root->ScreenAfterCarSelect = EApexScreen::SessionCreate;
	}
	ShowScreen(EApexScreen::CarSelect);
}

void UApexSessionCreateWidget::HandleCreateClicked()
{
	UApexMenuFlowSubsystem* Flow = GetFlow();
	UApexNetSubsystem* Net = GetNet();
	if (!Flow || !Net)
	{
		return;
	}

	if (!Flow->HasPendingTrack() || !Flow->HasPendingCar())
	{
		ShowToast(TEXT("Pick both a track and a car first"), true);
		return;
	}

	CommitSpinBoxes();

	// SelectCar first, then CreateSession — the same order the Godot client
	// used (SessionCreationDialog.cs:279). The server binds the car to the
	// player, so creating first would put the host on the grid without one.
	Net->SelectCar(Flow->GetPendingCarId());
	Net->CreateSession(
		Flow->GetPendingTrackId(),
		Flow->CreateMaxPlayers,
		Flow->CreateAiCount,
		Flow->CreateLapLimit,
		Flow->CreateSessionKind);

	if (StatusText)
	{
		StatusText->SetText(FText::FromString(TEXT("Creating session…")));
	}
}

void UApexSessionCreateWidget::HandleBackClicked()
{
	GoBack();
}
