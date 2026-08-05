#include "UI/ApexConnectDialogWidget.h"

#include "ApexMenuFlowSubsystem.h"
#include "ApexNetSubsystem.h"
#include "Components/Button.h"
#include "Components/EditableTextBox.h"
#include "Components/TextBlock.h"
#include "UI/ApexRootWidget.h"

void UApexConnectDialogWidget::NativeConstruct()
{
	Super::NativeConstruct();

	if (ConnectButton) { ConnectButton->OnClicked.AddDynamic(this, &UApexConnectDialogWidget::HandleConnectClicked); }
	if (BackButton)    { BackButton->OnClicked.AddDynamic(this, &UApexConnectDialogWidget::HandleBackClicked); }

	if (UApexNetSubsystem* Net = GetNet())
	{
		Net->OnConnectionStateChanged.AddDynamic(this, &UApexConnectDialogWidget::HandleConnectionStateChanged);
		Net->OnAuthSucceeded.AddDynamic(this, &UApexConnectDialogWidget::HandleAuthSucceeded);
		Net->OnAuthFailed.AddDynamic(this, &UApexConnectDialogWidget::HandleAuthFailed);
	}
}

void UApexConnectDialogWidget::NativeDestruct()
{
	if (UApexNetSubsystem* Net = GetNet())
	{
		Net->OnConnectionStateChanged.RemoveDynamic(this, &UApexConnectDialogWidget::HandleConnectionStateChanged);
		Net->OnAuthSucceeded.RemoveDynamic(this, &UApexConnectDialogWidget::HandleAuthSucceeded);
		Net->OnAuthFailed.RemoveDynamic(this, &UApexConnectDialogWidget::HandleAuthFailed);
	}
	Super::NativeDestruct();
}

void UApexConnectDialogWidget::OnScreenActivated()
{
	Super::OnScreenActivated();

	// Repopulate from the flow subsystem so a failed attempt comes back with
	// the values the user last typed rather than the defaults.
	if (const UApexMenuFlowSubsystem* Flow = GetFlow())
	{
		if (HostBox)       { HostBox->SetText(FText::FromString(Flow->ServerHost)); }
		if (PortBox)       { PortBox->SetText(FText::AsNumber(Flow->ServerPort)); }
		if (PlayerNameBox) { PlayerNameBox->SetText(FText::FromString(Flow->PlayerName)); }
		if (TokenBox)      { TokenBox->SetText(FText::FromString(Flow->AuthToken)); }
	}

	if (const UApexNetSubsystem* Net = GetNet())
	{
		if (Net->GetConnectionState() == EApexConnectionState::Failed)
		{
			SetStatus(TEXT("Connection failed — check the address and try again"), ErrorColor);
		}
		else if (Net->IsAuthenticated())
		{
			SetStatus(TEXT("Connected"), SuccessColor);
		}
		else
		{
			SetStatus(TEXT(""), PendingColor);
		}
	}
}

void UApexConnectDialogWidget::CommitFields()
{
	UApexMenuFlowSubsystem* Flow = GetFlow();
	if (!Flow)
	{
		return;
	}

	if (HostBox)
	{
		const FString Host = HostBox->GetText().ToString().TrimStartAndEnd();
		if (!Host.IsEmpty())
		{
			Flow->ServerHost = Host;
		}
	}
	if (PortBox)
	{
		const int32 Port = FCString::Atoi(*PortBox->GetText().ToString());
		if (Port > 0 && Port <= 65535)
		{
			Flow->ServerPort = Port;
		}
	}
	if (PlayerNameBox)
	{
		const FString Name = PlayerNameBox->GetText().ToString().TrimStartAndEnd();
		if (!Name.IsEmpty())
		{
			Flow->PlayerName = Name;
		}
	}
	if (TokenBox)
	{
		Flow->AuthToken = TokenBox->GetText().ToString().TrimStartAndEnd();
	}
}

void UApexConnectDialogWidget::SetStatus(const FString& Message, const FLinearColor& Color)
{
	if (StatusText)
	{
		StatusText->SetText(FText::FromString(Message));
		StatusText->SetColorAndOpacity(Color);
	}
}

void UApexConnectDialogWidget::HandleConnectClicked()
{
	CommitFields();

	UApexMenuFlowSubsystem* Flow = GetFlow();
	UApexNetSubsystem* Net = GetNet();
	if (!Flow || !Net)
	{
		return;
	}

	SetStatus(FString::Printf(TEXT("Connecting to %s:%d…"), *Flow->ServerHost, Flow->ServerPort), PendingColor);
	Net->Connect(Flow->ServerHost, Flow->ServerPort, Flow->PlayerName, Flow->AuthToken);
}

void UApexConnectDialogWidget::HandleBackClicked()
{
	GoBack();
}

void UApexConnectDialogWidget::HandleConnectionStateChanged(EApexConnectionState NewState, const FString& Detail)
{
	switch (NewState)
	{
	case EApexConnectionState::Connecting:
	case EApexConnectionState::Authenticating:
		SetStatus(Detail, PendingColor);
		break;
	case EApexConnectionState::Failed:
		SetStatus(Detail, ErrorColor);
		break;
	default:
		break;
	}
}

void UApexConnectDialogWidget::HandleAuthSucceeded(const FString& PlayerId, int32 ServerVersion)
{
	SetStatus(FString::Printf(TEXT("Connected (server version %d)"), ServerVersion), SuccessColor);

	// Only bounce back if this dialog is the screen actually on show — auth can
	// complete while the user has already navigated elsewhere.
	if (GetRoot() && GetRoot()->GetCurrentScreen() == EApexScreen::ConnectDialog)
	{
		ShowScreen(EApexScreen::MainMenu);
	}
}

void UApexConnectDialogWidget::HandleAuthFailed(const FString& Reason)
{
	SetStatus(FString::Printf(TEXT("Authentication failed: %s"), *Reason), ErrorColor);
}
