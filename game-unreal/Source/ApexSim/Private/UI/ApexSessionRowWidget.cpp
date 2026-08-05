#include "UI/ApexSessionRowWidget.h"

#include "Components/Button.h"
#include "Components/TextBlock.h"

void UApexSessionRowWidget::NativeConstruct()
{
	Super::NativeConstruct();

	if (JoinButton)
	{
		JoinButton->OnClicked.AddDynamic(this, &UApexSessionRowWidget::HandleJoinClicked);
	}
}

FString UApexSessionRowWidget::DescribeKind(EApexSessionKind Kind)
{
	switch (Kind)
	{
	case EApexSessionKind::Practice:    return TEXT("Practice");
	case EApexSessionKind::Sandbox:     return TEXT("Sandbox");
	case EApexSessionKind::Multiplayer: return TEXT("Multiplayer");
	default:                            return TEXT("Unknown");
	}
}

FString UApexSessionRowWidget::DescribeState(EApexSessionState State)
{
	switch (State)
	{
	case EApexSessionState::Lobby:     return TEXT("Lobby");
	case EApexSessionState::Countdown: return TEXT("Countdown");
	case EApexSessionState::Racing:    return TEXT("Racing");
	case EApexSessionState::Finished:  return TEXT("Finished");
	default:                           return TEXT("Unknown");
	}
}

FLinearColor UApexSessionRowWidget::ColorForState(EApexSessionState State) const
{
	switch (State)
	{
	case EApexSessionState::Lobby:     return LobbyColor;
	case EApexSessionState::Countdown: return CountdownColor;
	case EApexSessionState::Racing:    return RacingColor;
	default:                           return FinishedColor;
	}
}

void UApexSessionRowWidget::SetSession(const FApexSessionSummary& Summary, bool bCanJoin)
{
	SessionId = Summary.Id;

	if (TrackNameText)
	{
		TrackNameText->SetText(FText::FromString(Summary.TrackName));
	}

	if (HostAndPlayersText)
	{
		HostAndPlayersText->SetText(FText::FromString(FString::Printf(
			TEXT("Host: %s   |   Players: %d/%d"), *Summary.HostName, Summary.PlayerCount, Summary.MaxPlayers)));
	}

	if (KindAndStateText)
	{
		KindAndStateText->SetText(FText::FromString(FString::Printf(
			TEXT("%s   |   %s"), *DescribeKind(Summary.SessionKind), *DescribeState(Summary.State))));
		KindAndStateText->SetColorAndOpacity(ColorForState(Summary.State));
	}

	if (JoinButton)
	{
		// Joinable means: still in lobby, not full, and the player has a car.
		JoinButton->SetIsEnabled(bCanJoin && Summary.IsJoinable());
	}
}

void UApexSessionRowWidget::HandleJoinClicked()
{
	OnJoinClicked.Broadcast(this);
}
