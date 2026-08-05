#include "UI/ApexSessionLobbyWidget.h"

#include "ApexMenuFlowSubsystem.h"
#include "ApexNetSubsystem.h"
#include "ApexSim.h"
#include "Components/Button.h"
#include "Components/VerticalBox.h"
#include "Components/TextBlock.h"
#include "UI/ApexPlayerRowWidget.h"
#include "UI/ApexRootWidget.h"
#include "UI/ApexTrackCardWidget.h"

void UApexSessionLobbyWidget::NativeConstruct()
{
	Super::NativeConstruct();

	if (ChangeCarButton) { ChangeCarButton->OnClicked.AddDynamic(this, &UApexSessionLobbyWidget::HandleChangeCarClicked); }
	if (StartRaceButton) { StartRaceButton->OnClicked.AddDynamic(this, &UApexSessionLobbyWidget::HandleStartRaceClicked); }
	if (LeaveButton)     { LeaveButton->OnClicked.AddDynamic(this, &UApexSessionLobbyWidget::HandleLeaveClicked); }

	if (UApexNetSubsystem* Net = GetNet())
	{
		Net->OnLobbyStateUpdated.AddDynamic(this, &UApexSessionLobbyWidget::HandleLobbyStateUpdated);
		Net->OnGameModeChanged.AddDynamic(this, &UApexSessionLobbyWidget::HandleGameModeChanged);
		Net->OnCountdownUpdate.AddDynamic(this, &UApexSessionLobbyWidget::HandleCountdownUpdate);
	}
}

void UApexSessionLobbyWidget::NativeDestruct()
{
	if (UApexNetSubsystem* Net = GetNet())
	{
		Net->OnLobbyStateUpdated.RemoveDynamic(this, &UApexSessionLobbyWidget::HandleLobbyStateUpdated);
		Net->OnGameModeChanged.RemoveDynamic(this, &UApexSessionLobbyWidget::HandleGameModeChanged);
		Net->OnCountdownUpdate.RemoveDynamic(this, &UApexSessionLobbyWidget::HandleCountdownUpdate);
	}
	Super::NativeDestruct();
}

void UApexSessionLobbyWidget::OnScreenActivated()
{
	Super::OnScreenActivated();
	Refresh();
}

void UApexSessionLobbyWidget::HandleLobbyStateUpdated(const FApexLobbyState& LobbyState)
{
	Refresh();
}

FString UApexSessionLobbyWidget::DescribeGameMode(EApexGameMode Mode)
{
	switch (Mode)
	{
	case EApexGameMode::Lobby:         return TEXT("Lobby");
	case EApexGameMode::Sandbox:       return TEXT("Sandbox");
	case EApexGameMode::Countdown:     return TEXT("Countdown");
	case EApexGameMode::DemoLap:       return TEXT("Demo Lap");
	case EApexGameMode::FreePractice:  return TEXT("Free Practice");
	case EApexGameMode::Replay:        return TEXT("Replay");
	case EApexGameMode::Qualification: return TEXT("Qualification");
	case EApexGameMode::Race:          return TEXT("Race");
	default:                           return TEXT("Unknown");
	}
}

void UApexSessionLobbyWidget::HandleGameModeChanged(EApexGameMode NewMode)
{
	if (GameModeText)
	{
		GameModeText->SetText(FText::FromString(FString::Printf(TEXT("Mode: %s"), *DescribeGameMode(NewMode))));
	}
}

void UApexSessionLobbyWidget::HandleCountdownUpdate(int32 SecondsRemaining)
{
	if (StatusText)
	{
		StatusText->SetText(FText::FromString(FString::Printf(TEXT("Starting in %d…"), SecondsRemaining)));
	}
}

void UApexSessionLobbyWidget::Refresh()
{
	const UApexNetSubsystem* Net = GetNet();
	const UApexMenuFlowSubsystem* Flow = GetFlow();
	if (!Net)
	{
		return;
	}

	const FApexLobbyState& Lobby = Net->GetCachedLobbyState();
	const FString& SessionId = Net->GetCurrentSessionId();

	FApexSessionSummary Session;
	const bool bHasSession = Net->FindSessionById(SessionId, Session);

	if (SessionInfoText)
	{
		SessionInfoText->SetText(FText::FromString(bHasSession
			? FString::Printf(TEXT("Track: %s   |   Players: %d/%d"),
				*Session.TrackName, Session.PlayerCount, Session.MaxPlayers)
			: TEXT("Session")));
	}

	// Only the players actually in this session belong in the list; the lobby
	// broadcast carries everyone connected to the server.
	TArray<FApexLobbyPlayer> Participants;
	for (const FApexLobbyPlayer& Player : Lobby.PlayersInLobby)
	{
		if (Player.InSession.Equals(SessionId, ESearchCase::IgnoreCase))
		{
			Participants.Add(Player);
		}
	}

	if (PlayersContainer && PlayerRowClass)
	{
		// Rebuild only when the count changes, so the two-second broadcast does
		// not recreate the whole list under the user.
		if (Participants.Num() != BuiltPlayerRowCount)
		{
			BuiltPlayerRowCount = Participants.Num();
			PlayersContainer->ClearChildren();
			PlayerRows.Reset();

			for (int32 i = 0; i < Participants.Num(); ++i)
			{
				if (UApexPlayerRowWidget* Row = CreateWidget<UApexPlayerRowWidget>(this, PlayerRowClass))
				{
					PlayersContainer->AddChild(Row);
					PlayerRows.Add(Row);
				}
			}
		}

		for (int32 i = 0; i < PlayerRows.Num() && i < Participants.Num(); ++i)
		{
			const bool bIsLocal = Participants[i].Id.Equals(Net->GetPlayerId(), ESearchCase::IgnoreCase);
			PlayerRows[i]->SetPlayer(Participants[i], bIsLocal);
		}
	}

	if (TrackCard && bHasSession)
	{
		// SessionSummary carries a track name but no track ID, so match the
		// catalog by name against the track list to recover the ID.
		for (const FApexTrackConfigSummary& Track : Lobby.TrackConfigs)
		{
			if (Track.Name.Equals(Session.TrackName, ESearchCase::IgnoreCase))
			{
				FApexTrackCatalogRow Row;
				const bool bHasRow = Flow && Flow->GetTrackCatalogRow(Track.Id, Row);
				TrackCard->SetTrack(Track, Row, bHasRow);
				break;
			}
		}
	}

	if (SelectedCarText)
	{
		FApexCarConfigSummary Car;
		const bool bResolved = Flow && Flow->HasPendingCar() && Net->FindCarById(Flow->GetPendingCarId(), Car);
		SelectedCarText->SetText(FText::FromString(bResolved
			? FString::Printf(TEXT("Car: %s"), *Car.Name)
			: TEXT("Car: none selected")));
	}
}

void UApexSessionLobbyWidget::HandleChangeCarClicked()
{
	if (UApexRootWidget* Root = GetRoot())
	{
		Root->ScreenAfterCarSelect = EApexScreen::SessionLobby;
	}
	ShowScreen(EApexScreen::CarSelect);
}

void UApexSessionLobbyWidget::HandleStartRaceClicked()
{
	// Deliberately inert. Sending StartSession (or SetGameMode) would move the
	// server into a driving mode and start broadcasting telemetry to a client
	// with no track view and no car actors to apply it to. Racing is out of
	// scope for the menu shell; the plumbing is already there on
	// UApexNetSubsystem::StartSession for when it lands.
	UE_LOG(LogApexSim, Display,
		TEXT("Start Race pressed — racing is out of scope for the menu shell, no message sent"));
	ShowToast(TEXT("Racing isn't implemented yet — this is the menu shell"), false);
}

void UApexSessionLobbyWidget::HandleLeaveClicked()
{
	if (UApexNetSubsystem* Net = GetNet())
	{
		Net->LeaveSession();
	}
}
