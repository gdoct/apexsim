#include "UI/ApexStatusBarWidget.h"

#include "ApexNetSubsystem.h"
#include "Components/TextBlock.h"
#include "Engine/GameInstance.h"

void UApexStatusBarWidget::NativeConstruct()
{
	Super::NativeConstruct();

	if (UApexNetSubsystem* Net = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexNetSubsystem>() : nullptr)
	{
		Net->OnConnectionStateChanged.AddDynamic(this, &UApexStatusBarWidget::HandleConnectionStateChanged);
	}
	Refresh();
}

void UApexStatusBarWidget::NativeDestruct()
{
	if (UApexNetSubsystem* Net = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexNetSubsystem>() : nullptr)
	{
		Net->OnConnectionStateChanged.RemoveDynamic(this, &UApexStatusBarWidget::HandleConnectionStateChanged);
	}
	Super::NativeDestruct();
}

void UApexStatusBarWidget::HandleConnectionStateChanged(EApexConnectionState NewState, const FString& Detail)
{
	Refresh();
}

void UApexStatusBarWidget::Refresh()
{
	if (!StatusText)
	{
		return;
	}

	UApexNetSubsystem* Net = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexNetSubsystem>() : nullptr;
	if (!Net)
	{
		StatusText->SetText(FText::FromString(TEXT("Not connected")));
		StatusText->SetColorAndOpacity(IdleColor);
		return;
	}

	switch (Net->GetConnectionState())
	{
	case EApexConnectionState::Authenticated:
	{
		// An 8-character prefix is enough to tell two players apart without
		// putting a full 36-character UUID in the corner of the screen.
		const FString ShortId = Net->GetPlayerId().Left(8);
		StatusText->SetText(FText::FromString(
			FString::Printf(TEXT("Connected  |  %s (%s…)"), *Net->GetPlayerName(), *ShortId)));
		StatusText->SetColorAndOpacity(ConnectedColor);
		break;
	}

	case EApexConnectionState::Connecting:
		StatusText->SetText(FText::FromString(TEXT("Connecting…")));
		StatusText->SetColorAndOpacity(PendingColor);
		break;

	case EApexConnectionState::Authenticating:
		StatusText->SetText(FText::FromString(TEXT("Authenticating…")));
		StatusText->SetColorAndOpacity(PendingColor);
		break;

	case EApexConnectionState::Failed:
		StatusText->SetText(FText::FromString(TEXT("Connection failed")));
		StatusText->SetColorAndOpacity(FailedColor);
		break;

	default:
		StatusText->SetText(FText::FromString(TEXT("Not connected")));
		StatusText->SetColorAndOpacity(IdleColor);
		break;
	}
}
