#include "UI/ApexRootWidget.h"

#include "ApexMenuFlowSubsystem.h"
#include "ApexNetSubsystem.h"
#include "ApexPlayerController.h"
#include "ApexSettingsSubsystem.h"
#include "ApexSim.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/WidgetSwitcher.h"
#include "Engine/GameInstance.h"
#include "HAL/IConsoleManager.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetSystemLibrary.h"
#include "TimerManager.h"
#include "UnrealClient.h"
#include "Race/ApexRaceDirector.h"
#include "UI/ApexCarSelectWidget.h"
#include "UI/ApexConnectDialogWidget.h"
#include "UI/ApexHudWidget.h"
#include "UI/ApexMainMenuWidget.h"
#include "UI/ApexPauseMenuWidget.h"
#include "UI/ApexScreenWidget.h"
#include "UI/ApexSessionCreateWidget.h"
#include "UI/ApexSessionLobbyWidget.h"
#include "UI/ApexSessionResultsWidget.h"
#include "UI/ApexSettingsWidget.h"
#include "UI/ApexToastWidget.h"
#include "UI/ApexTrackSelectWidget.h"
#include "UI/ApexUIStyle.h"

namespace
{
	/**
	 * Screens still living in their original widget blueprints.
	 *
	 * The redesign replaces these one at a time; until a screen's C++ class
	 * exists, ResolveScreenClass loads the `.uasset` so the switcher keeps an
	 * entry at every EApexScreen index.
	 */
	const TCHAR* LegacyScreenPath(EApexScreen Screen)
	{
		switch (Screen)
		{
		case EApexScreen::ConnectDialog:  return TEXT("/Game/UI/Screens/WBP_ConnectDialog.WBP_ConnectDialog_C");
		case EApexScreen::SessionBrowser: return TEXT("/Game/UI/Screens/WBP_SessionBrowser.WBP_SessionBrowser_C");
		case EApexScreen::SessionCreate:  return TEXT("/Game/UI/Screens/WBP_SessionCreate.WBP_SessionCreate_C");
		case EApexScreen::CarSelect:      return TEXT("/Game/UI/Screens/WBP_CarSelect.WBP_CarSelect_C");
		case EApexScreen::TrackSelect:    return TEXT("/Game/UI/Screens/WBP_TrackSelect.WBP_TrackSelect_C");
		case EApexScreen::SessionLobby:   return TEXT("/Game/UI/Screens/WBP_SessionLobby.WBP_SessionLobby_C");
		case EApexScreen::Loading:        return TEXT("/Game/UI/Screens/WBP_LoadingScreen.WBP_LoadingScreen_C");
		default:                          return nullptr;
		}
	}

	/** Seconds between a one-click start and the lights going out. */
	constexpr int32 AutoStartCountdownSeconds = 3;

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

UClass* UApexRootWidget::ResolveScreenClass(EApexScreen Screen)
{
	switch (Screen)
	{
	case EApexScreen::MainMenu:
		return UApexMainMenuWidget::StaticClass();

	case EApexScreen::ConnectDialog:
		return UApexConnectDialogWidget::StaticClass();

	case EApexScreen::SessionCreate:
		return UApexSessionCreateWidget::StaticClass();

	case EApexScreen::CarSelect:
		return UApexCarSelectWidget::StaticClass();

	case EApexScreen::TrackSelect:
		return UApexTrackSelectWidget::StaticClass();

	case EApexScreen::SessionLobby:
		return UApexSessionLobbyWidget::StaticClass();

	case EApexScreen::SessionResults:
		return UApexSessionResultsWidget::StaticClass();

	default:
		break;
	}

	if (const TCHAR* Path = LegacyScreenPath(Screen))
	{
		if (UClass* Loaded = LoadClass<UApexScreenWidget>(nullptr, Path))
		{
			return Loaded;
		}
		UE_LOG(LogApexSim, Warning, TEXT("Screen %d has no C++ class and its blueprint (%s) failed to load"),
			static_cast<int32>(Screen), Path);
	}

	// A blank screen still has to occupy the slot: the switcher is indexed by
	// EApexScreen, so a missing entry would shift every screen after it.
	return UApexScreenWidget::StaticClass();
}

void UApexRootWidget::BuildShell()
{
	Background = ApexUI::MakePanel(*WidgetTree, nullptr, FMargin(), ApexUI::MakeBrush(ApexUI::Palette::Background));

	ScreenHost = WidgetTree->ConstructWidget<UWidgetSwitcher>();
	for (int32 Index = 0; Index <= static_cast<int32>(EApexScreen::SessionResults); ++Index)
	{
		const EApexScreen Screen = static_cast<EApexScreen>(Index);
		UClass* ScreenClass = ResolveScreenClass(Screen);

		UApexScreenWidget* Widget = WidgetTree->ConstructWidget<UApexScreenWidget>(ScreenClass);
		ScreenHost->AddChild(Widget);
	}

	ToastPanel = WidgetTree->ConstructWidget<UApexToastWidget>();

	Hud = WidgetTree->ConstructWidget<UApexHudWidget>();
	PauseMenu = WidgetTree->ConstructWidget<UApexPauseMenuWidget>();
	PauseMenu->OnAction.AddDynamic(this, &UApexRootWidget::HandlePauseAction);
	SettingsOverlay = WidgetTree->ConstructWidget<UApexSettingsWidget>();
	SettingsOverlay->OnClosed.AddDynamic(this, &UApexRootWidget::HandleSettingsClosed);

	UOverlay* Frame = WidgetTree->ConstructWidget<UOverlay>();

	UOverlaySlot* BackgroundSlot = Frame->AddChildToOverlay(Background);
	BackgroundSlot->SetHorizontalAlignment(HAlign_Fill);
	BackgroundSlot->SetVerticalAlignment(VAlign_Fill);

	UOverlaySlot* SwitcherSlot = Frame->AddChildToOverlay(ScreenHost);
	SwitcherSlot->SetHorizontalAlignment(HAlign_Fill);
	SwitcherSlot->SetVerticalAlignment(VAlign_Fill);

	// Order is the stack: HUD under the pause menu, pause menu under settings.
	UOverlaySlot* HudSlot = Frame->AddChildToOverlay(Hud);
	HudSlot->SetHorizontalAlignment(HAlign_Fill);
	HudSlot->SetVerticalAlignment(VAlign_Fill);

	UOverlaySlot* PauseSlot = Frame->AddChildToOverlay(PauseMenu);
	PauseSlot->SetHorizontalAlignment(HAlign_Fill);
	PauseSlot->SetVerticalAlignment(VAlign_Fill);

	UOverlaySlot* SettingsSlot = Frame->AddChildToOverlay(SettingsOverlay);
	SettingsSlot->SetHorizontalAlignment(HAlign_Fill);
	SettingsSlot->SetVerticalAlignment(VAlign_Fill);

	UOverlaySlot* ToastSlot = Frame->AddChildToOverlay(ToastPanel);
	ToastSlot->SetHorizontalAlignment(HAlign_Center);
	ToastSlot->SetVerticalAlignment(VAlign_Bottom);
	ToastSlot->SetPadding(FMargin(0.0f, 0.0f, 0.0f, ApexUI::Metrics::HintBarHeight + 16.0f));

	WidgetTree->RootWidget = Frame;
}

void UApexRootWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	BuildShell();
}

void UApexRootWidget::NativeConstruct()
{
	Super::NativeConstruct();

	if (UApexNetSubsystem* Net = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexNetSubsystem>() : nullptr)
	{
		Net->OnUdpReady.AddDynamic(this, &UApexRootWidget::HandleUdpReady);
		Net->OnDisconnected.AddDynamic(this, &UApexRootWidget::HandleDisconnected);
		Net->OnServerError.AddDynamic(this, &UApexRootWidget::HandleServerError);
		Net->OnSessionJoined.AddDynamic(this, &UApexRootWidget::HandleSessionJoined);
		Net->OnSessionLeft.AddDynamic(this, &UApexRootWidget::HandleSessionLeft);
		Net->OnGameModeChanged.AddDynamic(this, &UApexRootWidget::HandleGameModeChanged);
		Net->OnLobbyStateUpdated.AddDynamic(this, &UApexRootWidget::HandleLobbyStateForAutoRace);
		Net->OnSessionStateChanged.AddDynamic(this, &UApexRootWidget::HandleSessionStateChanged);
	}

	// -ApexScreenshotAfter=N grabs the viewport N seconds in. Together with
	// -ApexStartScreen it makes any screen inspectable from a headless run,
	// which is the only way to look at the UI without opening the editor.
	float ScreenshotDelay = 0.0f;
	if (FParse::Value(FCommandLine::Get(), TEXT("ApexScreenshotAfter="), ScreenshotDelay)
		&& ScreenshotDelay > 0.0f
		&& GetWorld())
	{
		FTimerHandle Handle;
		GetWorld()->GetTimerManager().SetTimer(
			Handle,
			FTimerDelegate::CreateWeakLambda(this, []()
			{
				UE_LOG(LogApexSim, Log, TEXT("-ApexScreenshotAfter: requesting a viewport screenshot"));
				// bShowUI, or the grab is of the empty 3D scene behind the menu.
				FScreenshotRequest::RequestScreenshot(true);
			}),
			ScreenshotDelay,
			false);
	}

	// -ApexOpenPause=N / -ApexOpenSettings=N open a race overlay N seconds in.
	// Both are otherwise only reachable with a keypress, which an unattended run
	// cannot make — and the overlays are exactly what a screenshot pass wants to
	// look at. -ApexSettingsTab picks the page (see EApexSettingsTab).
	float OverlayDelay = 0.0f;
	const bool bOpenSettings = FParse::Value(FCommandLine::Get(), TEXT("ApexOpenSettings="), OverlayDelay);
	const bool bOpenPause = !bOpenSettings && FParse::Value(FCommandLine::Get(), TEXT("ApexOpenPause="), OverlayDelay);

	if ((bOpenPause || bOpenSettings) && OverlayDelay > 0.0f && GetWorld())
	{
		int32 TabIndex = 0;
		FParse::Value(FCommandLine::Get(), TEXT("ApexSettingsTab="), TabIndex);

		FTimerHandle Handle;
		GetWorld()->GetTimerManager().SetTimer(
			Handle,
			FTimerDelegate::CreateWeakLambda(this, [this, bOpenSettings, TabIndex]()
			{
				UE_LOG(LogApexSim, Log, TEXT("Opening the %s overlay on request from the command line"),
					bOpenSettings ? TEXT("settings") : TEXT("pause"));
				SetPaused(true);
				if (bOpenSettings && SettingsOverlay)
				{
					SettingsOverlay->Open(static_cast<EApexSettingsTab>(FMath::Clamp(TabIndex, 0, 2)));
				}
			}),
			OverlayDelay,
			false);
	}

	bAutoRaceRequested = FParse::Param(FCommandLine::Get(), TEXT("ApexAutoRace"));
	FParse::Value(FCommandLine::Get(), TEXT("ApexAiCount="), AutoRaceAiCount);
	FParse::Value(FCommandLine::Get(), TEXT("ApexLaps="), AutoRaceLaps);

	// -ApexNoStart stops after creating the session, which is the only way to
	// reach the lobby unattended: a started session hands the view straight to
	// the race director. -ApexMode picks what to count into (see EApexGameMode);
	// a demo lap is the one mode that drives itself to a finish.
	bAutoRaceNoStart = FParse::Param(FCommandLine::Get(), TEXT("ApexNoStart"));
	int32 ModeValue = -1;
	if (FParse::Value(FCommandLine::Get(), TEXT("ApexMode="), ModeValue)
		&& ModeValue >= 0
		&& ModeValue <= static_cast<int32>(EApexGameMode::Race))
	{
		AutoRaceMode = static_cast<EApexGameMode>(ModeValue);
	}

	if (bAutoRaceRequested)
	{
		UE_LOG(LogApexSim, Log,
			TEXT("-ApexAutoRace: will create a session with %d AI over %d lap(s)%s"),
			AutoRaceAiCount, AutoRaceLaps,
			bAutoRaceNoStart ? TEXT(" and stop in the lobby") : TEXT(" and count it in"));
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

	// Push the race layers into their starting state explicitly. SetRaceViewActive
	// only fires on a *change*, so without this the HUD's own default is the only
	// thing deciding whether it is on screen before the first race — and a default
	// that disagrees with the shell leaves it drawn over every menu.
	if (Hud)
	{
		Hud->SetRaceActive(bRaceViewActive);
	}

	// -ExecCmds runs after the widget is constructed, so the command-line switch
	// is what actually works for launching straight into a screen; the cvar is
	// still honoured for setting it live from the console.
	int32 StartOverride = CVarStartScreen.GetValueOnGameThread();
	int32 CommandLineScreen = -1;
	if (FParse::Value(FCommandLine::Get(), TEXT("ApexStartScreen="), CommandLineScreen))
	{
		StartOverride = CommandLineScreen;
	}

	if (StartOverride >= 0 && ScreenHost && StartOverride < ScreenHost->GetChildrenCount())
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
		Net->OnUdpReady.RemoveDynamic(this, &UApexRootWidget::HandleUdpReady);
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
	const FKey Key = InKeyEvent.GetKey();

	// The overlays handle their own keys, including Escape, and they are above
	// this widget in the focus path — so if one is up, nothing here applies.
	if (IsRaceOverlayOpen())
	{
		return Super::NativeOnKeyDown(InGeometry, InKeyEvent);
	}

	if (bRaceViewActive)
	{
		// The pause key is rebindable, so Escape is checked through the settings
		// rather than assumed.
		const UApexSettingsSubsystem* Settings =
			GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexSettingsSubsystem>() : nullptr;
		const bool bPauseKey = Settings ? Settings->IsPauseKey(Key) : Key == EKeys::Escape;

		if (bPauseKey)
		{
			// Escape used to leave the session outright. It now opens the pause
			// menu, which is where leaving lives — one keypress should not end a
			// race that took ten minutes to reach.
			SetPaused(true);
			return FReply::Handled();
		}
		return Super::NativeOnKeyDown(InGeometry, InKeyEvent);
	}

	if (Key == EKeys::Escape)
	{
		GoBack();
		return FReply::Handled();
	}
	return Super::NativeOnKeyDown(InGeometry, InKeyEvent);
}

bool UApexRootWidget::IsRaceOverlayOpen() const
{
	return (PauseMenu && PauseMenu->IsOpen()) || (SettingsOverlay && SettingsOverlay->IsOpen());
}

void UApexRootWidget::SetPaused(bool bPaused)
{
	if (bPauseMenuOpen == bPaused || !PauseMenu)
	{
		return;
	}
	bPauseMenuOpen = bPaused;

	if (bPaused)
	{
		PauseMenu->Open();
	}
	else
	{
		PauseMenu->Close();
	}

	// UI-only input while the menu is up: it zeroes the controls, so the car
	// does not carry on with whatever was held when Escape was pressed.
	if (AApexPlayerController* PlayerController =
			Cast<AApexPlayerController>(UGameplayStatics::GetPlayerController(this, 0)))
	{
		PlayerController->SetDriveInputEnabled(!bPaused && bRaceViewActive);
	}

	if (!bPaused)
	{
		// Focus has to come back here or the next Escape reaches nothing.
		SetKeyboardFocus();
	}
}

void UApexRootWidget::HandlePauseAction(EApexPauseAction Action)
{
	switch (Action)
	{
	case EApexPauseAction::Resume:
		SetPaused(false);
		break;

	case EApexPauseAction::OpenSettings:
		// The pause menu stays open underneath: settings is a step forward from
		// it, and closing settings has to land back on the menu it came from.
		if (SettingsOverlay)
		{
			SettingsOverlay->Open();
		}
		break;

	case EApexPauseAction::LeaveSession:
		SetPaused(false);
		if (UApexNetSubsystem* Net = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexNetSubsystem>() : nullptr)
		{
			// HandleSessionLeft takes the view back and returns to the main menu.
			Net->LeaveSession();
		}
		break;

	case EApexPauseAction::QuitGame:
		// Leave the session first: quitting on an open socket leaves the server
		// waiting out a heartbeat timeout before it frees the slot.
		if (UApexNetSubsystem* Net = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexNetSubsystem>() : nullptr)
		{
			Net->LeaveSession();
			Net->Disconnect();
		}
		UKismetSystemLibrary::QuitGame(this, UGameplayStatics::GetPlayerController(this, 0),
			EQuitPreference::Quit, /*bIgnorePlatformRestrictions*/ false);
		break;
	}
}

void UApexRootWidget::HandleSettingsClosed()
{
	// Back onto the pause menu, which never went away.
	if (bPauseMenuOpen && PauseMenu)
	{
		PauseMenu->Open();
	}
	else
	{
		SetKeyboardFocus();
	}
}

UApexScreenWidget* UApexRootWidget::GetScreenWidget(EApexScreen Screen) const
{
	if (!ScreenHost)
	{
		return nullptr;
	}
	const int32 Index = static_cast<int32>(Screen);
	if (!ScreenHost->GetChildrenCount() || Index >= ScreenHost->GetChildrenCount())
	{
		return nullptr;
	}
	return Cast<UApexScreenWidget>(ScreenHost->GetChildAt(Index));
}

void UApexRootWidget::ActivateScreen(EApexScreen Screen)
{
	if (!ScreenHost)
	{
		UE_LOG(LogApexSim, Warning, TEXT("WBP_Root has no ScreenHost; navigation is inert"));
		return;
	}

	const int32 Index = static_cast<int32>(Screen);
	if (Index >= ScreenHost->GetChildrenCount())
	{
		UE_LOG(LogApexSim, Warning,
			TEXT("Screen %d is not present in the switcher (it has %d children)"),
			Index, ScreenHost->GetChildrenCount());
		return;
	}

	if (UApexScreenWidget* Outgoing = Cast<UApexScreenWidget>(ScreenHost->GetActiveWidget()))
	{
		Outgoing->OnScreenDeactivated();
	}

	ScreenHost->SetActiveWidgetIndex(Index);
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
	if (ToastPanel)
	{
		ToastPanel->Show(Message, bIsError);
	}
	else
	{
		UE_LOG(LogApexSim, Log, TEXT("ToastPanel (no widget): %s"), *Message);
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
	// The player's remembered track when there is one; the server's first
	// otherwise. Le Mans sorts first and is 13 km, which makes for a long wait.
	FApexTrackConfigSummary Track = LobbyState.TrackConfigs[0];
	if (Flow->HasPendingTrack())
	{
		Net->FindTrackById(Flow->GetPendingTrackId(), Track);
	}

	Flow->SetPendingCar(Car.Id);
	Flow->SetPendingTrack(Track.Id);
	Flow->CreateLapLimit = AutoRaceLaps;
	Flow->CreateStartingMode = AutoRaceMode;

	UE_LOG(LogApexSim, Log, TEXT("-ApexAutoRace: creating '%s' on '%s' with %d AI over %d lap(s)"),
		*Car.Name, *Track.Name, AutoRaceAiCount, AutoRaceLaps);

	// SelectCar before CreateSession, same order the UI uses.
	Net->SelectCar(Car.Id);
	Net->CreateSession(Track.Id, 8, AutoRaceAiCount, AutoRaceLaps, EApexSessionKind::Practice);
}

void UApexRootWidget::HandleUdpReady()
{
	if (bStartWhenUdpReady)
	{
		bStartWhenUdpReady = false;
		StartRequestedSession();
	}
}

void UApexRootWidget::StartRequestedSession()
{
	UApexNetSubsystem* Net = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexNetSubsystem>() : nullptr;
	UApexMenuFlowSubsystem* Flow = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexMenuFlowSubsystem>() : nullptr;
	if (!Net || !Flow)
	{
		return;
	}

	// StartCountdown rather than StartSession: StartSession sets a countdown but
	// stores no mode to transition into, so the session sits frozen when it
	// expires.
	UE_LOG(LogApexSim, Log, TEXT("One-click start: counting into mode %d"),
		static_cast<int32>(Flow->AutoStartMode));
	Net->StartCountdown(AutoStartCountdownSeconds, Flow->AutoStartMode);
}

void UApexRootWidget::HandleSessionJoined(const FString& SessionId, int32 GridPosition)
{
	// Joining always lands in the lobby, whether the session was created or
	// joined from the browser, so the transition belongs here rather than in
	// both screens.
	BackStack.Reset();
	ActivateScreen(EApexScreen::SessionLobby);

	if (UApexMenuFlowSubsystem* Flow = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexMenuFlowSubsystem>() : nullptr)
	{
		if (Flow->bAutoStartOnJoin)
		{
			// The main menu promised a session, not a lobby. The countdown has to
			// wait for the telemetry channel, which is usually — but not always —
			// up by the time the join lands.
			Flow->bAutoStartOnJoin = false;

			UApexNetSubsystem* Net = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexNetSubsystem>() : nullptr;
			if (Net && Net->IsUdpReady())
			{
				StartRequestedSession();
			}
			else
			{
				bStartWhenUdpReady = true;
			}
		}
	}

	if (bAutoRaceRequested && !bAutoRaceNoStart)
	{
		if (UApexNetSubsystem* Net = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexNetSubsystem>() : nullptr)
		{
			// See the note in ApexSessionLobbyWidget: StartSession alone never
			// leaves Countdown.
			UE_LOG(LogApexSim, Log, TEXT("-ApexAutoRace: session joined, counting into mode %d"),
				static_cast<int32>(AutoRaceMode));
			Net->StartCountdown(3, AutoRaceMode);
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
	if (NewState == EApexSessionState::Finished)
	{
		// The session recorder has classified it by now — the recorder handles
		// this same transition, and subsystems are notified before widgets.
		SetRaceViewActive(false);
		BackStack.Reset();
		ActivateScreen(EApexScreen::SessionResults);
		return;
	}

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
	if (ScreenHost)
	{
		ScreenHost->SetVisibility(bActive ? ESlateVisibility::Collapsed : ESlateVisibility::Visible);
	}
	if (Background)
	{
		Background->SetVisibility(bActive ? ESlateVisibility::Collapsed : ESlateVisibility::HitTestInvisible);
	}

	if (Hud)
	{
		Hud->SetRaceActive(bActive);
	}

	if (!bActive)
	{
		// A session that ends while paused — a race finishing, a disconnect —
		// must not leave the overlays up over the menu.
		if (SettingsOverlay)
		{
			SettingsOverlay->Close();
		}
		SetPaused(false);
		SetKeyboardFocus();
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
