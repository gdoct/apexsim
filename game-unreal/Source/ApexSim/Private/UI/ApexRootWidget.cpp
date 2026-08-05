#include "UI/ApexRootWidget.h"

#include "ApexMenuFlowSubsystem.h"
#include "ApexNetSubsystem.h"
#include "ApexSim.h"
#include "Components/Image.h"
#include "Components/WidgetSwitcher.h"
#include "Engine/GameInstance.h"
#include "HAL/IConsoleManager.h"
#include "Race/ApexRaceDirector.h"
#include "UI/ApexScreenWidget.h"
#include "UI/ApexStatusBarWidget.h"
#include "UI/ApexToastWidget.h"

namespace
{
	/**
	 * Jump straight to a screen on startup, by EApexScreen index.
	 *
	 * Screens past the main menu are otherwise only reachable by clicking, which
	 * makes them awkward to inspect in an automated or headless run. -1 keeps the
	 * normal main-menu start. On the command line use -ApexStartScreen=N instead:
	 * -ExecCmds is applied after this widget is built.
	 */
	TAutoConsoleVariable<int32> CVarStartScreen(
		TEXT("apexsim.ui.StartScreen"),
		-1,
		TEXT("Screen index to open on startup (see EApexScreen). -1 = MainMenu."),
		ECVF_Default);
}

void UApexRootWidget::NativeConstruct()
{
	Super::NativeConstruct();

	if (UApexNetSubsystem* Net = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexNetSubsystem>() : nullptr)
	{
		Net->OnDisconnected.AddDynamic(this, &UApexRootWidget::HandleDisconnected);
		Net->OnServerError.AddDynamic(this, &UApexRootWidget::HandleServerError);
		Net->OnSessionJoined.AddDynamic(this, &UApexRootWidget::HandleSessionJoined);
		Net->OnSessionLeft.AddDynamic(this, &UApexRootWidget::HandleSessionLeft);
		Net->OnGameModeChanged.AddDynamic(this, &UApexRootWidget::HandleGameModeChanged);
		Net->OnLobbyStateUpdated.AddDynamic(this, &UApexRootWidget::HandleLobbyStateForAutoRace);
		Net->OnSessionStateChanged.AddDynamic(this, &UApexRootWidget::HandleSessionStateChanged);
	}

	bAutoRaceRequested = FParse::Param(FCommandLine::Get(), TEXT("ApexAutoRace"));
	FParse::Value(FCommandLine::Get(), TEXT("ApexAiCount="), AutoRaceAiCount);
	if (bAutoRaceRequested)
	{
		UE_LOG(LogApexSim, Log, TEXT("-ApexAutoRace: will create and start a session with %d AI once the lobby arrives"),
			AutoRaceAiCount);
	}

	// Escape needs to reach NativeOnKeyDown, which requires focus.
	SetKeyboardFocus();

	// Connect as soon as the shell exists rather than when the main menu is
	// shown: every screen wants lobby data, and the start-screen override below
	// means the main menu is not always the first screen.
	if (UGameInstance* GameInstance = GetGameInstance())
	{
		UApexMenuFlowSubsystem* Flow = GameInstance->GetSubsystem<UApexMenuFlowSubsystem>();
		UApexNetSubsystem* Net = GameInstance->GetSubsystem<UApexNetSubsystem>();
		if (Flow && Net && Flow->ConsumeAutoConnect())
		{
			UE_LOG(LogApexSim, Log, TEXT("Auto-connecting to %s:%d"), *Flow->ServerHost, Flow->ServerPort);
			Net->Connect(Flow->ServerHost, Flow->ServerPort, Flow->PlayerName, Flow->AuthToken);
		}
	}

	CurrentScreen = EApexScreen::MainMenu;
	BackStack.Reset();

	// -ExecCmds runs after the widget is constructed, so the command-line switch
	// is what actually works for launching straight into a screen; the cvar is
	// still honoured for setting it live from the console.
	int32 StartOverride = CVarStartScreen.GetValueOnGameThread();
	int32 CommandLineScreen = -1;
	if (FParse::Value(FCommandLine::Get(), TEXT("ApexStartScreen="), CommandLineScreen))
	{
		StartOverride = CommandLineScreen;
	}

	if (StartOverride >= 0 && ScreenSwitcher && StartOverride < ScreenSwitcher->GetChildrenCount())
	{
		UE_LOG(LogApexSim, Log, TEXT("apexsim.ui.StartScreen=%d — opening that screen instead of the main menu"), StartOverride);
		CurrentScreen = static_cast<EApexScreen>(StartOverride);
	}

	ActivateScreen(CurrentScreen);
}

void UApexRootWidget::NativeDestruct()
{
	if (UApexNetSubsystem* Net = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexNetSubsystem>() : nullptr)
	{
		Net->OnDisconnected.RemoveDynamic(this, &UApexRootWidget::HandleDisconnected);
		Net->OnServerError.RemoveDynamic(this, &UApexRootWidget::HandleServerError);
		Net->OnSessionJoined.RemoveDynamic(this, &UApexRootWidget::HandleSessionJoined);
		Net->OnSessionLeft.RemoveDynamic(this, &UApexRootWidget::HandleSessionLeft);
		Net->OnGameModeChanged.RemoveDynamic(this, &UApexRootWidget::HandleGameModeChanged);
		Net->OnLobbyStateUpdated.RemoveDynamic(this, &UApexRootWidget::HandleLobbyStateForAutoRace);
		Net->OnSessionStateChanged.RemoveDynamic(this, &UApexRootWidget::HandleSessionStateChanged);
	}

	Super::NativeDestruct();
}

FReply UApexRootWidget::NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent)
{
	if (InKeyEvent.GetKey() == EKeys::Escape)
	{
		// While racing there is no menu to go back through, so Escape is the way
		// out of the session entirely.
		if (bRaceViewActive)
		{
			if (UApexNetSubsystem* Net = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexNetSubsystem>() : nullptr)
			{
				Net->LeaveSession();
			}
			return FReply::Handled();
		}

		GoBack();
		return FReply::Handled();
	}
	return Super::NativeOnKeyDown(InGeometry, InKeyEvent);
}

UApexScreenWidget* UApexRootWidget::GetScreenWidget(EApexScreen Screen) const
{
	if (!ScreenSwitcher)
	{
		return nullptr;
	}
	const int32 Index = static_cast<int32>(Screen);
	if (!ScreenSwitcher->GetChildrenCount() || Index >= ScreenSwitcher->GetChildrenCount())
	{
		return nullptr;
	}
	return Cast<UApexScreenWidget>(ScreenSwitcher->GetChildAt(Index));
}

void UApexRootWidget::ActivateScreen(EApexScreen Screen)
{
	if (!ScreenSwitcher)
	{
		UE_LOG(LogApexSim, Warning, TEXT("WBP_Root has no ScreenSwitcher; navigation is inert"));
		return;
	}

	const int32 Index = static_cast<int32>(Screen);
	if (Index >= ScreenSwitcher->GetChildrenCount())
	{
		UE_LOG(LogApexSim, Warning,
			TEXT("Screen %d is not present in the switcher (it has %d children)"),
			Index, ScreenSwitcher->GetChildrenCount());
		return;
	}

	if (UApexScreenWidget* Outgoing = Cast<UApexScreenWidget>(ScreenSwitcher->GetActiveWidget()))
	{
		Outgoing->OnScreenDeactivated();
	}

	ScreenSwitcher->SetActiveWidgetIndex(Index);
	CurrentScreen = Screen;

	if (UApexScreenWidget* Incoming = GetScreenWidget(Screen))
	{
		Incoming->OnScreenActivated();
	}
}

void UApexRootWidget::ShowScreen(EApexScreen Screen)
{
	if (Screen == CurrentScreen)
	{
		return;
	}
	BackStack.Push(CurrentScreen);
	ActivateScreen(Screen);
}

void UApexRootWidget::ReplaceScreen(EApexScreen Screen)
{
	if (Screen == CurrentScreen)
	{
		return;
	}
	ActivateScreen(Screen);
}

void UApexRootWidget::GoBack()
{
	if (BackStack.Num() == 0)
	{
		// Already at the root of the flow; Escape does nothing rather than
		// dumping the user somewhere arbitrary.
		return;
	}
	ActivateScreen(BackStack.Pop());
}

void UApexRootWidget::ShowToast(const FString& Message, bool bIsError)
{
	if (Toast)
	{
		Toast->Show(Message, bIsError);
	}
	else
	{
		UE_LOG(LogApexSim, Log, TEXT("Toast (no widget): %s"), *Message);
	}
}

void UApexRootWidget::HandleDisconnected(const FString& Reason)
{
	BackStack.Reset();
	ActivateScreen(EApexScreen::MainMenu);
	ShowToast(FString::Printf(TEXT("Disconnected: %s"), *Reason), true);
}

void UApexRootWidget::HandleServerError(int32 Code, const FString& Message)
{
	ShowToast(FString::Printf(TEXT("Server error %d: %s"), Code, *Message), true);
}

void UApexRootWidget::HandleLobbyStateForAutoRace(const FApexLobbyState& LobbyState)
{
	TryAutoRace(LobbyState);
}

void UApexRootWidget::TryAutoRace(const FApexLobbyState& LobbyState)
{
	if (!bAutoRaceRequested || bAutoRaceSessionRequested)
	{
		return;
	}
	if (LobbyState.CarConfigs.Num() == 0 || LobbyState.TrackConfigs.Num() == 0)
	{
		return;
	}

	UApexNetSubsystem* Net = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexNetSubsystem>() : nullptr;
	UApexMenuFlowSubsystem* Flow = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexMenuFlowSubsystem>() : nullptr;
	if (!Net || !Flow)
	{
		return;
	}

	bAutoRaceSessionRequested = true;

	const FApexCarConfigSummary& Car = LobbyState.CarConfigs[0];
	const FApexTrackConfigSummary& Track = LobbyState.TrackConfigs[0];
	Flow->SetPendingCar(Car.Id);
	Flow->SetPendingTrack(Track.Id);

	UE_LOG(LogApexSim, Log, TEXT("-ApexAutoRace: creating '%s' on '%s' with %d AI"),
		*Car.Name, *Track.Name, AutoRaceAiCount);

	// SelectCar before CreateSession, same order the UI uses.
	Net->SelectCar(Car.Id);
	Net->CreateSession(Track.Id, 8, AutoRaceAiCount, 5, EApexSessionKind::Practice);
}

void UApexRootWidget::HandleSessionJoined(const FString& SessionId, int32 GridPosition)
{
	// Joining always lands in the lobby, whether the session was created or
	// joined from the browser, so the transition belongs here rather than in
	// both screens.
	BackStack.Reset();
	ActivateScreen(EApexScreen::SessionLobby);

	if (bAutoRaceRequested)
	{
		if (UApexNetSubsystem* Net = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexNetSubsystem>() : nullptr)
		{
			// See the note in ApexSessionLobbyWidget: StartSession alone never
			// leaves Countdown.
			UE_LOG(LogApexSim, Log, TEXT("-ApexAutoRace: session joined, counting into Race"));
			Net->StartCountdown(3, EApexGameMode::Race);
		}
	}
}

void UApexRootWidget::HandleSessionLeft()
{
	SetRaceViewActive(false);
	BackStack.Reset();
	ActivateScreen(EApexScreen::MainMenu);
}

bool UApexRootWidget::IsDrivingMode(EApexGameMode Mode)
{
	// Lobby and Sandbox do not simulate cars, so there is nothing to watch.
	// Everything else has the server ticking physics and sending telemetry.
	switch (Mode)
	{
	case EApexGameMode::Countdown:
	case EApexGameMode::DemoLap:
	case EApexGameMode::FreePractice:
	case EApexGameMode::Qualification:
	case EApexGameMode::Race:
	case EApexGameMode::Replay:
		return true;
	default:
		return false;
	}
}

void UApexRootWidget::HandleGameModeChanged(EApexGameMode NewMode)
{
	// A driving mode is enough on its own — a DemoLap or FreePractice can start
	// without the session state ever leaving Lobby.
	if (IsDrivingMode(NewMode))
	{
		SetRaceViewActive(true);
	}
}

void UApexRootWidget::HandleSessionStateChanged(EApexSessionState NewState)
{
	// This is the signal that actually fires for `StartSession`: the state goes
	// Lobby -> Countdown -> Racing while the game mode stays Lobby throughout.
	SetRaceViewActive(NewState != EApexSessionState::Lobby);
}

void UApexRootWidget::SetRaceViewActive(bool bActive)
{
	if (bRaceViewActive == bActive)
	{
		return;
	}
	bRaceViewActive = bActive;

	// The menu is hit-test invisible rather than removed so the toast and status
	// bar can still be brought back without rebuilding the tree.
	if (ScreenSwitcher)
	{
		ScreenSwitcher->SetVisibility(bActive ? ESlateVisibility::Collapsed : ESlateVisibility::Visible);
	}
	if (BackgroundImage)
	{
		BackgroundImage->SetVisibility(bActive ? ESlateVisibility::Collapsed : ESlateVisibility::HitTestInvisible);
	}

	if (AApexRaceDirector* Director = AApexRaceDirector::Find(this))
	{
		if (bActive)
		{
			Director->BeginRaceView();
		}
		else
		{
			Director->EndRaceView();
		}
	}
	else if (bActive)
	{
		UE_LOG(LogApexSim, Warning,
			TEXT("No AApexRaceDirector in the level; telemetry will arrive but nothing will render"));
	}

	UE_LOG(LogApexSim, Log, TEXT("Race view %s"), bActive ? TEXT("entered") : TEXT("left"));
}
