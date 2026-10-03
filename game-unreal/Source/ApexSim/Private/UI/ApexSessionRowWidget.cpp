#include "UI/ApexSessionRowWidget.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Button.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/PanelWidget.h"
#include "Components/TextBlock.h"

void UApexSessionRowWidget::NativeConstruct()
{
	Super::NativeConstruct();

	if (JoinButton)
	{
		JoinButton->OnClicked.AddDynamic(this, &UApexSessionRowWidget::HandleJoinClicked);
	}
	BuildWatchButton();
}

void UApexSessionRowWidget::BuildWatchButton()
{
	if (WatchButton || !JoinButton || !WidgetTree)
	{
		return;
	}
	UPanelWidget* Parent = JoinButton->GetParent();
	if (!Parent)
	{
		return;
	}
	WatchButton = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass(), TEXT("WatchButton"));
	WatchButton->SetStyle(JoinButton->GetStyle());
	UTextBlock* Label = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("WatchLabel"));
	Label->SetText(FText::FromString(TEXT("Watch")));
	if (const UTextBlock* JoinLabel = Cast<UTextBlock>(JoinButton->GetChildAt(0)))
	{
		Label->SetFont(JoinLabel->GetFont());
		Label->SetColorAndOpacity(JoinLabel->GetColorAndOpacity());
	}
	WatchButton->AddChild(Label);
	WatchButton->SetToolTipText(FText::FromString(TEXT("Watch this race without a car")));
	WatchButton->OnClicked.AddDynamic(this, &UApexSessionRowWidget::HandleWatchClicked);
	Parent->InsertChildAt(Parent->GetChildIndex(JoinButton), WatchButton);
	if (UHorizontalBoxSlot* WatchSlot = Cast<UHorizontalBoxSlot>(WatchButton->Slot))
	{
		WatchSlot->SetPadding(FMargin(0.0f, 0.0f, 8.0f, 0.0f));
		WatchSlot->SetVerticalAlignment(VAlign_Center);
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
			TEXT("%s   |   %s   |   %s"), *DescribeKind(Summary.SessionKind), *DescribeState(Summary.State),
			*Summary.Conditions.Describe())));
		KindAndStateText->SetColorAndOpacity(ColorForState(Summary.State));
	}

	if (JoinButton)
	{
		// Joinable means: not over, not full, and the player has a car.
		JoinButton->SetIsEnabled(bCanJoin && Summary.IsJoinable());
	}
	if (WatchButton)
	{
		// Watching needs no car, only a race being driven.
		WatchButton->SetIsEnabled(Summary.IsWatchable());
	}
}

void UApexSessionRowWidget::HandleJoinClicked()
{
	OnJoinClicked.Broadcast(this);
}

void UApexSessionRowWidget::HandleWatchClicked()
{
	OnWatchClicked.Broadcast(this);
}
