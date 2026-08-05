#include "UI/ApexMainMenuWidget.h"

#include "ApexMenuFlowSubsystem.h"
#include "ApexNetSubsystem.h"
#include "ApexSim.h"
#include "Components/Button.h"
#include "Components/Image.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBoxSlot.h"
#include "Kismet/KismetSystemLibrary.h"
#include "UI/ApexRootWidget.h"

void UApexMainMenuWidget::NativeConstruct()
{
	Super::NativeConstruct();

	if (ConnectButton)        { ConnectButton->OnClicked.AddDynamic(this, &UApexMainMenuWidget::HandleConnectClicked); }
	if (BrowseSessionsButton) { BrowseSessionsButton->OnClicked.AddDynamic(this, &UApexMainMenuWidget::HandleBrowseClicked); }
	if (CreateSessionButton)  { CreateSessionButton->OnClicked.AddDynamic(this, &UApexMainMenuWidget::HandleCreateClicked); }
	if (CarSelectButton)      { CarSelectButton->OnClicked.AddDynamic(this, &UApexMainMenuWidget::HandleCarSelectClicked); }
	if (TrackSelectButton)    { TrackSelectButton->OnClicked.AddDynamic(this, &UApexMainMenuWidget::HandleTrackSelectClicked); }
	if (QuitButton)           { QuitButton->OnClicked.AddDynamic(this, &UApexMainMenuWidget::HandleQuitClicked); }

	if (SubtitleText)
	{
		SubtitleText->SetText(FText::FromString(TEXT("Open-source simracing")));
	}

	if (LogoImage)
	{
		// logo.png is 1024x256. Filling the slot stretched it across the whole
		// window, so pin a size at the source aspect and centre it instead.
		LogoImage->SetDesiredSizeOverride(FVector2D(LogoWidth, LogoWidth * 0.25f));
		if (UVerticalBoxSlot* LogoSlot = Cast<UVerticalBoxSlot>(LogoImage->Slot))
		{
			LogoSlot->SetHorizontalAlignment(HAlign_Center);
			LogoSlot->SetPadding(FMargin(0.0f, 24.0f, 0.0f, 16.0f));
		}
	}

	if (UApexNetSubsystem* Net = GetNet())
	{
		Net->OnConnectionStateChanged.AddDynamic(this, &UApexMainMenuWidget::HandleConnectionStateChanged);
		Net->OnLobbyStateUpdated.AddDynamic(this, &UApexMainMenuWidget::HandleLobbyStateUpdated);
	}

	RefreshButtons();
}

void UApexMainMenuWidget::NativeDestruct()
{
	if (UApexNetSubsystem* Net = GetNet())
	{
		Net->OnConnectionStateChanged.RemoveDynamic(this, &UApexMainMenuWidget::HandleConnectionStateChanged);
		Net->OnLobbyStateUpdated.RemoveDynamic(this, &UApexMainMenuWidget::HandleLobbyStateUpdated);
	}
	Super::NativeDestruct();
}

void UApexMainMenuWidget::OnScreenActivated()
{
	Super::OnScreenActivated();

	// Auto-connect lives on the root widget, not here: connecting is a property
	// of the shell starting up, not of this particular screen being shown.
	RefreshButtons();
}

void UApexMainMenuWidget::HandleConnectionStateChanged(EApexConnectionState NewState, const FString& Detail)
{
	if (ConnectionStatusText)
	{
		ConnectionStatusText->SetText(FText::FromString(Detail));
	}

	// A failed auto-connect drops the user straight onto the connect dialog so
	// they can fix the address, rather than leaving them at a dead menu.
	if (NewState == EApexConnectionState::Failed && GetRoot() && GetRoot()->GetCurrentScreen() == EApexScreen::MainMenu)
	{
		ShowScreen(EApexScreen::ConnectDialog);
	}

	RefreshButtons();
}

void UApexMainMenuWidget::HandleLobbyStateUpdated(const FApexLobbyState& LobbyState)
{
	if (UApexMenuFlowSubsystem* Flow = GetFlow())
	{
		Flow->ReportUnmatchedCatalogIds(LobbyState);
	}
	RefreshButtons();
}

void UApexMainMenuWidget::RefreshButtons()
{
	const UApexNetSubsystem* Net = GetNet();
	const bool bAuthenticated = Net && Net->IsAuthenticated();

	// Connect is only offered when there is nothing to connect to; the
	// session buttons only when there is.
	if (ConnectButton)
	{
		ConnectButton->SetVisibility(bAuthenticated ? ESlateVisibility::Collapsed : ESlateVisibility::Visible);
	}

	const ESlateVisibility OnlineVisibility = bAuthenticated ? ESlateVisibility::Visible : ESlateVisibility::Collapsed;
	if (BrowseSessionsButton) { BrowseSessionsButton->SetVisibility(OnlineVisibility); }
	if (CreateSessionButton)  { CreateSessionButton->SetVisibility(OnlineVisibility); }
	if (CarSelectButton)      { CarSelectButton->SetVisibility(OnlineVisibility); }
	if (TrackSelectButton)    { TrackSelectButton->SetVisibility(OnlineVisibility); }

	if (ConnectionStatusText && !bAuthenticated && Net
		&& Net->GetConnectionState() == EApexConnectionState::Disconnected)
	{
		ConnectionStatusText->SetText(FText::FromString(TEXT("Not connected")));
	}
}

void UApexMainMenuWidget::HandleConnectClicked()
{
	ShowScreen(EApexScreen::ConnectDialog);
}

void UApexMainMenuWidget::HandleBrowseClicked()
{
	ShowScreen(EApexScreen::SessionBrowser);
}

void UApexMainMenuWidget::HandleCreateClicked()
{
	ShowScreen(EApexScreen::SessionCreate);
}

void UApexMainMenuWidget::HandleCarSelectClicked()
{
	if (UApexRootWidget* Root = GetRoot())
	{
		Root->ScreenAfterCarSelect = EApexScreen::MainMenu;
	}
	ShowScreen(EApexScreen::CarSelect);
}

void UApexMainMenuWidget::HandleTrackSelectClicked()
{
	if (UApexRootWidget* Root = GetRoot())
	{
		Root->ScreenAfterTrackSelect = EApexScreen::MainMenu;
	}
	ShowScreen(EApexScreen::TrackSelect);
}

void UApexMainMenuWidget::HandleQuitClicked()
{
	if (UApexNetSubsystem* Net = GetNet())
	{
		Net->Disconnect();
	}
	UKismetSystemLibrary::QuitGame(this, nullptr, EQuitPreference::Quit, false);
}
