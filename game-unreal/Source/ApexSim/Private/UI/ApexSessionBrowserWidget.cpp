#include "UI/ApexSessionBrowserWidget.h"

#include "ApexMenuFlowSubsystem.h"
#include "ApexNetSubsystem.h"
#include "Components/Button.h"
#include "Components/VerticalBox.h"
#include "Components/TextBlock.h"
#include "UI/ApexRootWidget.h"
#include "UI/ApexSessionRowWidget.h"

void UApexSessionBrowserWidget::NativeConstruct()
{
	Super::NativeConstruct();

	if (RefreshButton)       { RefreshButton->OnClicked.AddDynamic(this, &UApexSessionBrowserWidget::HandleRefreshClicked); }
	if (CarSelectorButton)   { CarSelectorButton->OnClicked.AddDynamic(this, &UApexSessionBrowserWidget::HandleCarSelectorClicked); }
	if (CreateSessionButton) { CreateSessionButton->OnClicked.AddDynamic(this, &UApexSessionBrowserWidget::HandleCreateClicked); }
	if (BackButton)          { BackButton->OnClicked.AddDynamic(this, &UApexSessionBrowserWidget::HandleBackClicked); }

	if (UApexNetSubsystem* Net = GetNet())
	{
		Net->OnLobbyStateUpdated.AddDynamic(this, &UApexSessionBrowserWidget::HandleLobbyStateUpdated);
	}
}

void UApexSessionBrowserWidget::NativeDestruct()
{
	if (UApexNetSubsystem* Net = GetNet())
	{
		Net->OnLobbyStateUpdated.RemoveDynamic(this, &UApexSessionBrowserWidget::HandleLobbyStateUpdated);
	}
	Super::NativeDestruct();
}

void UApexSessionBrowserWidget::OnScreenActivated()
{
	Super::OnScreenActivated();
	RefreshList();
}

void UApexSessionBrowserWidget::HandleLobbyStateUpdated(const FApexLobbyState& LobbyState)
{
	RefreshList();
}

void UApexSessionBrowserWidget::RefreshList()
{
	if (!SessionList || !SessionRowClass)
	{
		return;
	}

	const UApexNetSubsystem* Net = GetNet();
	const UApexMenuFlowSubsystem* Flow = GetFlow();
	if (!Net)
	{
		return;
	}

	const TArray<FApexSessionSummary>& Sessions = Net->GetCachedLobbyState().AvailableSessions;
	const bool bHasCar = Flow && Flow->HasPendingCar();

	TArray<FString> IncomingIds;
	IncomingIds.Reserve(Sessions.Num());
	for (const FApexSessionSummary& Session : Sessions)
	{
		IncomingIds.Add(Session.Id);
	}

	// Only tear down when the membership of the list changes; otherwise reuse
	// the rows and just push new values into them.
	if (IncomingIds != BuiltSessionIds)
	{
		BuiltSessionIds = IncomingIds;
		SessionList->ClearChildren();
		Rows.Reset();

		for (int32 i = 0; i < Sessions.Num(); ++i)
		{
			UApexSessionRowWidget* Row = CreateWidget<UApexSessionRowWidget>(this, SessionRowClass);
			if (!Row)
			{
				continue;
			}
			Row->OnJoinClicked.AddDynamic(this, &UApexSessionBrowserWidget::HandleJoinClicked);
			SessionList->AddChild(Row);
			Rows.Add(Row);
		}
	}

	for (int32 i = 0; i < Rows.Num() && i < Sessions.Num(); ++i)
	{
		Rows[i]->SetSession(Sessions[i], bHasCar);
	}

	if (StatusText)
	{
		if (Sessions.Num() == 0)
		{
			StatusText->SetText(FText::FromString(TEXT("No sessions yet — create one")));
		}
		else if (!bHasCar)
		{
			StatusText->SetText(FText::FromString(
				FString::Printf(TEXT("%d session(s) — pick a car before joining"), Sessions.Num())));
		}
		else
		{
			StatusText->SetText(FText::FromString(
				FString::Printf(TEXT("%d session(s) available"), Sessions.Num())));
		}
	}
}

void UApexSessionBrowserWidget::HandleRefreshClicked()
{
	// RequestLobbyState debounces internally: the server allows 10 control
	// messages a second and charges violations beyond that, and it already
	// broadcasts a fresh snapshot every 2 seconds anyway.
	if (UApexNetSubsystem* Net = GetNet())
	{
		if (!Net->RequestLobbyState())
		{
			ShowToast(TEXT("Refreshing — the server broadcasts every couple of seconds"), false);
		}
	}
}

void UApexSessionBrowserWidget::HandleCarSelectorClicked()
{
	if (UApexRootWidget* Root = GetRoot())
	{
		Root->ScreenAfterCarSelect = EApexScreen::SessionBrowser;
	}
	ShowScreen(EApexScreen::CarSelect);
}

void UApexSessionBrowserWidget::HandleCreateClicked()
{
	ShowScreen(EApexScreen::SessionCreate);
}

void UApexSessionBrowserWidget::HandleBackClicked()
{
	GoBack();
}

void UApexSessionBrowserWidget::HandleJoinClicked(UApexSessionRowWidget* Row)
{
	if (!Row)
	{
		return;
	}

	UApexNetSubsystem* Net = GetNet();
	UApexMenuFlowSubsystem* Flow = GetFlow();
	if (!Net || !Flow)
	{
		return;
	}

	if (!Flow->HasPendingCar())
	{
		ShowToast(TEXT("Select a car first"), true);
		if (UApexRootWidget* Root = GetRoot())
		{
			Root->ScreenAfterCarSelect = EApexScreen::SessionBrowser;
		}
		ShowScreen(EApexScreen::CarSelect);
		return;
	}

	// SelectCar before JoinSession: the server records the car against the
	// player, so joining first would put them on the grid without one.
	Net->SelectCar(Flow->GetPendingCarId());
	Net->JoinSession(Row->GetSessionId());
}
