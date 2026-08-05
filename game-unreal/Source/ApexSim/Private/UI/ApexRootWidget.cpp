#include "UI/ApexRootWidget.h"

#include "ApexMenuFlowSubsystem.h"
#include "ApexNetSubsystem.h"
#include "ApexSim.h"
#include "Components/WidgetSwitcher.h"
#include "Engine/GameInstance.h"
#include "HAL/IConsoleManager.h"
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
	}

	Super::NativeDestruct();
}

FReply UApexRootWidget::NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent)
{
	if (InKeyEvent.GetKey() == EKeys::Escape)
	{
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

void UApexRootWidget::HandleSessionJoined(const FString& SessionId, int32 GridPosition)
{
	// Joining always lands in the lobby, whether the session was created or
	// joined from the browser, so the transition belongs here rather than in
	// both screens.
	BackStack.Reset();
	ActivateScreen(EApexScreen::SessionLobby);
}

void UApexRootWidget::HandleSessionLeft()
{
	BackStack.Reset();
	ActivateScreen(EApexScreen::MainMenu);
}
