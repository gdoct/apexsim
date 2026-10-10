#include "UI/ApexRootWidget.h"

#include "ApexDemoModeSubsystem.h"
#include "ApexMenuFlowSubsystem.h"
#include "ApexNetSubsystem.h"
#include "ApexReplayRecorder.h"
#include "ApexPlayerController.h"
#include "ApexSettingsSubsystem.h"
#include "ApexSim.h"
#include "ApexSpectatorSubsystem.h"
#include "Audio/ApexUiAudioSubsystem.h"
#include "Blueprint/WidgetTree.h"
#include "Hud/ApexHudDataSubsystem.h"
#include "UObject/UObjectIterator.h"
#include "Components/Border.h"
#include "Components/Image.h"
#include "Engine/Texture2D.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/WidgetSwitcher.h"
#include "Engine/GameInstance.h"
#include "Guide/ApexGuidePlayer.h"
#include "Guide/ApexTrackGuideSubsystem.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/IConsoleManager.h"
#include "Input/Events.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Layout/WidgetPath.h"
#include "TimerManager.h"
#include "UnrealClient.h"
#include "Race/ApexRaceDirector.h"
#include "Track/ApexTrackContentSubsystem.h"
#include "UI/ApexCarSelectWidget.h"
#include "UI/ApexConnectDialogWidget.h"
#include "UI/ApexHotlapWidget.h"
#include "UI/ApexHudEditorWidget.h"
#include "UI/ApexHudWidget.h"
#include "UI/ApexMainMenuWidget.h"
#include "UI/ApexMenuInputProcessor.h"
#include "UI/ApexPauseMenuWidget.h"
#include "UI/ApexReplaysWidget.h"
#include "UI/ApexSessionBrowserWidget.h"
#include "UI/ApexScreenWidget.h"
#include "UI/ApexSessionCreateWidget.h"
#include "UI/ApexSessionLobbyWidget.h"
#include "UI/ApexSessionResultsWidget.h"
#include "UI/ApexSettingsWidget.h"
#include "UI/ApexToastWidget.h"
#include "UI/ApexTrackGuideWidget.h"
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
	/** Seconds a backdrop watch waits for a race before giving up. */
	constexpr float WatchGiveUpSeconds = 30.0f;

	FAutoConsoleCommandWithWorldAndArgs WatchCommand(
		TEXT("apexsim.watch"),
		TEXT("Watch the race behind the menu, or steer the watch view: ")
		TEXT("apexsim.watch [start|stop|next|prev|car <position>|camera|auto|tower|overlay|race|pause|back|forward|faster|slower] ")
		TEXT("- a watched hotlap: apexsim.watch [hotlap|weather|earlier|later|circuit|prevcircuit] (next / prev step its car)"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			UApexRootWidget* Root = nullptr;
			for (TObjectIterator<UApexRootWidget> It; It; ++It)
			{
				if (It->GetWorld() == World && It->GetCachedWidget().IsValid())
				{
					Root = *It;
					break;
				}
			}
			if (!Root)
			{
				UE_LOG(LogApexSim, Warning, TEXT("apexsim.watch: no shell in this world"));
				return;
			}
			const FString Verb = Args.Num() > 0 ? Args[0].ToLower() : FString(TEXT("start"));
			ApexSpectate::FCommand Command;
			if (Verb == TEXT("start"))
			{
				Root->WatchBackdrop();
				return;
			}
			if (Verb == TEXT("hotlap"))
			{
				Root->StartHotlapWatch();
				return;
			}
			if (Verb == TEXT("stop"))
			{
				Command.Action = ApexSpectate::EAction::Leave;
			}
			else if (Verb == TEXT("next"))
			{
				Command.Action = ApexSpectate::EAction::NextCar;
			}
			else if (Verb == TEXT("prev"))
			{
				Command.Action = ApexSpectate::EAction::PreviousCar;
			}
			else if (Verb == TEXT("car") && Args.Num() > 1)
			{
				Command.Action = ApexSpectate::EAction::Position;
				Command.Position = FCString::Atoi(*Args[1]);
			}
			else if (Verb == TEXT("camera"))
			{
				Command.Action = ApexSpectate::EAction::Camera;
			}
			else if (Verb == TEXT("auto"))
			{
				Command.Action = ApexSpectate::EAction::Auto;
			}
			else if (Verb == TEXT("tower"))
			{
				Command.Action = ApexSpectate::EAction::Tower;
			}
			else if (Verb == TEXT("overlay"))
			{
				Command.Action = ApexSpectate::EAction::Overlay;
			}
			else if (Verb == TEXT("race"))
			{
				Command.Action = ApexSpectate::EAction::NextRace;
			}
			else if (Verb == TEXT("weather"))
			{
				Command.Action = ApexSpectate::EAction::Weather;
			}
			else if (Verb == TEXT("earlier"))
			{
				Command.Action = ApexSpectate::EAction::TimeEarlier;
			}
			else if (Verb == TEXT("later"))
			{
				Command.Action = ApexSpectate::EAction::TimeLater;
			}
			else if (Verb == TEXT("circuit"))
			{
				Command.Action = ApexSpectate::EAction::NextTrack;
			}
			else if (Verb == TEXT("prevcircuit"))
			{
				Command.Action = ApexSpectate::EAction::PreviousTrack;
			}
			else if (Verb == TEXT("pause"))
			{
				Command.Action = ApexSpectate::EAction::PlayPause;
			}
			else if (Verb == TEXT("back"))
			{
				Command.Action = ApexSpectate::EAction::SeekBack;
			}
			else if (Verb == TEXT("forward"))
			{
				Command.Action = ApexSpectate::EAction::SeekForward;
			}
			else if (Verb == TEXT("faster"))
			{
				Command.Action = ApexSpectate::EAction::Faster;
			}
			else if (Verb == TEXT("slower"))
			{
				Command.Action = ApexSpectate::EAction::Slower;
			}
			else
			{
				UE_LOG(LogApexSim, Warning, TEXT("apexsim.watch: unknown '%s'"), *Verb);
				return;
			}
			Root->RunWatchCommand(Command);
		}));

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

	case EApexScreen::Replays:
		return UApexReplaysWidget::StaticClass();

	case EApexScreen::SessionBrowser:
		return UApexSessionBrowserWidget::StaticClass();

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
	for (int32 Index = 0; Index <= static_cast<int32>(EApexScreen::Replays); ++Index)
	{
		const EApexScreen Screen = static_cast<EApexScreen>(Index);
		UClass* ScreenClass = ResolveScreenClass(Screen);

		UApexScreenWidget* Widget = WidgetTree->ConstructWidget<UApexScreenWidget>(ScreenClass);
		ScreenHost->AddChild(Widget);
	}

	ToastPanel = WidgetTree->ConstructWidget<UApexToastWidget>();

	Hud = WidgetTree->ConstructWidget<UApexHudWidget>();
	HotlapPanel = WidgetTree->ConstructWidget<UApexHotlapWidget>();
	HotlapPanel->OnAction.AddDynamic(this, &UApexRootWidget::HandleHotlapAction);
	HotlapPanel->OnWatchCar.AddDynamic(this, &UApexRootWidget::HandleHotlapWatch);
	GuidePanel = WidgetTree->ConstructWidget<UApexTrackGuideWidget>();
	PauseMenu = WidgetTree->ConstructWidget<UApexPauseMenuWidget>();
	PauseMenu->OnAction.AddDynamic(this, &UApexRootWidget::HandlePauseAction);
	SettingsOverlay = WidgetTree->ConstructWidget<UApexSettingsWidget>();
	SettingsOverlay->OnClosed.AddDynamic(this, &UApexRootWidget::HandleSettingsClosed);
	SettingsOverlay->OnEditHudLayout.AddDynamic(this, &UApexRootWidget::OpenHudEditor);
	HudEditor = WidgetTree->ConstructWidget<UApexHudEditorWidget>();
	HudEditor->OnClosed.AddDynamic(this, &UApexRootWidget::HandleHudEditorClosed);

	UOverlay* Frame = WidgetTree->ConstructWidget<UOverlay>();

	UOverlaySlot* BackgroundSlot = Frame->AddChildToOverlay(Background);
	BackgroundSlot->SetHorizontalAlignment(HAlign_Fill);
	BackgroundSlot->SetVerticalAlignment(VAlign_Fill);

	BackdropScrim = WidgetTree->ConstructWidget<UImage>();
	BackdropScrim->SetBrushFromTexture(MakeScrimTexture());
	BackdropScrim->SetVisibility(ESlateVisibility::Collapsed);
	UOverlaySlot* ScrimSlot = Frame->AddChildToOverlay(BackdropScrim);
	ScrimSlot->SetHorizontalAlignment(HAlign_Fill);
	ScrimSlot->SetVerticalAlignment(VAlign_Fill);

	UOverlaySlot* SwitcherSlot = Frame->AddChildToOverlay(ScreenHost);
	SwitcherSlot->SetHorizontalAlignment(HAlign_Fill);
	SwitcherSlot->SetVerticalAlignment(VAlign_Fill);

	// Order is the stack: HUD under the pause menu, pause menu under settings.
	UOverlaySlot* HudSlot = Frame->AddChildToOverlay(Hud);
	HudSlot->SetHorizontalAlignment(HAlign_Fill);
	HudSlot->SetVerticalAlignment(VAlign_Fill);

	UOverlaySlot* HotlapSlot = Frame->AddChildToOverlay(HotlapPanel);
	HotlapSlot->SetHorizontalAlignment(HAlign_Fill);
	HotlapSlot->SetVerticalAlignment(VAlign_Fill);

	UOverlaySlot* GuideSlot = Frame->AddChildToOverlay(GuidePanel);
	GuideSlot->SetHorizontalAlignment(HAlign_Fill);
	GuideSlot->SetVerticalAlignment(VAlign_Fill);

	UOverlaySlot* PauseSlot = Frame->AddChildToOverlay(PauseMenu);
	PauseSlot->SetHorizontalAlignment(HAlign_Fill);
	PauseSlot->SetVerticalAlignment(VAlign_Fill);

	UOverlaySlot* SettingsSlot = Frame->AddChildToOverlay(SettingsOverlay);
	SettingsSlot->SetHorizontalAlignment(HAlign_Fill);
	SettingsSlot->SetVerticalAlignment(VAlign_Fill);

	UOverlaySlot* EditorSlot = Frame->AddChildToOverlay(HudEditor);
	EditorSlot->SetHorizontalAlignment(HAlign_Fill);
	EditorSlot->SetVerticalAlignment(VAlign_Fill);

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
		Net->OnTelemetry.AddDynamic(this, &UApexRootWidget::HandleTelemetryForFinish);
		Net->OnTelemetry.AddDynamic(this, &UApexRootWidget::HandleTelemetryForHotlap);
		Net->OnLapRecord.AddDynamic(this, &UApexRootWidget::HandleLapRecordForGhost);
	}
	if (UApexSettingsSubsystem* Settings = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexSettingsSubsystem>() : nullptr)
	{
		Settings->OnSettingsChanged.AddDynamic(this, &UApexRootWidget::HandleSettingsChangedForDriverAids);
	}
	if (UApexMenuFlowSubsystem* Flow = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexMenuFlowSubsystem>() : nullptr)
	{
		Flow->OnContentMismatch.AddDynamic(this, &UApexRootWidget::HandleContentMismatch);
	}
	if (UApexTrackGuideSubsystem* Guide = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexTrackGuideSubsystem>() : nullptr)
	{
		GuideActiveHandle = Guide->OnActiveChanged.AddUObject(this, &UApexRootWidget::HandleGuideActiveChanged);
		GuideFailedHandle = Guide->OnFailed.AddUObject(this, &UApexRootWidget::HandleGuideFailed);
		if (Guide->IsOpen())
		{
			HandleGuideActiveChanged(true);
		}
	}

	// -ApexScreenshotAfter=N grabs the viewport N seconds in. Together with
	// -ApexStartScreen it makes any screen inspectable from a headless run,
	// which is the only way to look at the UI without opening the editor.
	// A comma list (-ApexScreenshotAfter=20,30,40) takes one at each, for
	// something that moves, like the demo race behind the menu.
	FString ScreenshotDelays;
	if (FParse::Value(FCommandLine::Get(), TEXT("ApexScreenshotAfter="), ScreenshotDelays, /*bShouldStopOnSeparator*/ false)
		&& GetWorld())
	{
		TArray<FString> Delays;
		ScreenshotDelays.ParseIntoArray(Delays, TEXT(","));
		for (const FString& Delay : Delays)
		{
			const float Seconds = FCString::Atof(*Delay);
			if (Seconds <= 0.0f)
			{
				continue;
			}
			FTimerHandle Handle;
			GetWorld()->GetTimerManager().SetTimer(
				Handle,
				FTimerDelegate::CreateWeakLambda(this, [Seconds]()
				{
					UE_LOG(LogApexSim, Log, TEXT("-ApexScreenshotAfter: requesting a viewport screenshot (%.0f s)"), Seconds);
					// bShowUI, or the grab is of the empty 3D scene behind the menu.
					// Each grab gets the next free file name.
					FScreenshotRequest::RequestScreenshot(true);
				}),
				Seconds,
				false);
		}
	}

	// -ApexExecAfter="N=command|N=command" runs a console command N seconds
	// in, for whatever has no switch of its own: e.g. damage arriving
	// mid-race (`25=apexsim.car.DamagePreview 70,55,40,30,90`), so parts fly
	// off on camera rather than being gone from the first frame.
	FString ExecAfter;
	if (FParse::Value(FCommandLine::Get(), TEXT("ApexExecAfter="), ExecAfter, /*bShouldStopOnSeparator*/ false)
		&& GetWorld())
	{
		TArray<FString> Entries;
		ExecAfter.ParseIntoArray(Entries, TEXT("|"));
		for (const FString& Entry : Entries)
		{
			FString When;
			FString Command;
			const float Seconds = Entry.Split(TEXT("="), &When, &Command) ? FCString::Atof(*When) : 0.0f;
			if (Seconds <= 0.0f || Command.TrimStartAndEnd().IsEmpty())
			{
				continue;
			}
			FTimerHandle Handle;
			GetWorld()->GetTimerManager().SetTimer(
				Handle,
				FTimerDelegate::CreateWeakLambda(this, [this, Seconds, Command]()
				{
					UE_LOG(LogApexSim, Log, TEXT("-ApexExecAfter: %s (%.0f s)"), *Command, Seconds);
					if (GEngine)
					{
						GEngine->Exec(GetWorld(), *Command.TrimStartAndEnd());
					}
				}),
				Seconds,
				false);
		}
	}

	// -ApexCameraCycleAfter=N[,N] presses C N seconds in: with a matching
	// -ApexScreenshotAfter list, one unattended run walks the whole camera
	// ladder (cockpit, roof, close, near, far) and grabs each of them.
	FString CameraCycleDelays;
	if (FParse::Value(FCommandLine::Get(), TEXT("ApexCameraCycleAfter="), CameraCycleDelays, /*bShouldStopOnSeparator*/ false)
		&& GetWorld())
	{
		TArray<FString> Delays;
		CameraCycleDelays.ParseIntoArray(Delays, TEXT(","));
		for (const FString& Delay : Delays)
		{
			const float Seconds = FCString::Atof(*Delay);
			if (Seconds <= 0.0f)
			{
				continue;
			}
			FTimerHandle Handle;
			GetWorld()->GetTimerManager().SetTimer(
				Handle,
				FTimerDelegate::CreateWeakLambda(this, [this, Seconds]()
				{
					if (AApexRaceDirector* Director = AApexRaceDirector::Find(this))
					{
						UE_LOG(LogApexSim, Log, TEXT("-ApexCameraCycleAfter: stepping the camera (%.0f s)"), Seconds);
						Director->CycleView();
					}
				}),
				Seconds,
				false);
		}
	}

	// -ApexOpenPause=N / -ApexOpenSettings=N open an overlay N seconds in.
	// Both are otherwise only reachable with a keypress, which an unattended run
	// cannot make — and the overlays are exactly what a screenshot pass wants to
	// look at. Settings opens over the pause menu in a race and over the
	// current screen otherwise. -ApexSettingsTab picks the page (see
	// EApexSettingsTab: 0 gameplay, 1 assists, 2 graphics, 3 camera,
	// 4 controls, 5 wheel, 6 audio).
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
			FTimerDelegate::CreateWeakLambda(this, [this, bOpenSettings, bOpenPause, TabIndex]()
			{
				UE_LOG(LogApexSim, Log, TEXT("Opening the %s overlay on request from the command line"),
					bOpenSettings ? TEXT("settings") : TEXT("pause"));
				if (bOpenPause || bRaceViewActive)
				{
					SetPaused(true);
				}
				if (bOpenSettings)
				{
					OpenSettings(static_cast<EApexSettingsTab>(
						FMath::Clamp(TabIndex, 0, static_cast<int32>(EApexSettingsTab::Audio))));
				}
			}),
			OverlayDelay,
			false);
	}

	// -ApexOpenHudEditor=N opens the HUD editor N seconds in, for a screenshot
	// run; -ApexHudEditorSteps="select standings;move 200 -100;scale 0.2"
	// then drives it (UApexHudEditorWidget::RunStep).
	float HudEditorDelay = 0.0f;
	if (FParse::Value(FCommandLine::Get(), TEXT("ApexOpenHudEditor="), HudEditorDelay) && HudEditorDelay > 0.0f && GetWorld())
	{
		FTimerHandle Handle;
		GetWorld()->GetTimerManager().SetTimer(Handle,
			FTimerDelegate::CreateWeakLambda(this, [this]()
			{
				UE_LOG(LogApexSim, Log, TEXT("Opening the HUD editor on request from the command line"));
				OpenHudEditor();
			}),
			HudEditorDelay, false);
	}

	bAutoRaceRequested = FParse::Param(FCommandLine::Get(), TEXT("ApexAutoRace"));
	FParse::Value(FCommandLine::Get(), TEXT("ApexAiCount="), AutoRaceAiCount);
	FParse::Value(FCommandLine::Get(), TEXT("ApexCountdown="), AutoRaceCountdown);
	FParse::Value(FCommandLine::Get(), TEXT("ApexLaps="), AutoRaceLaps);
	FParse::Value(FCommandLine::Get(), TEXT("ApexTrack="), AutoRaceTrack);
	FParse::Value(FCommandLine::Get(), TEXT("ApexCar="), AutoRaceCar);

	// -ApexNoStart stops after creating the session, which is the only way to
	// reach the lobby unattended: a started session hands the view straight to
	// the race director. -ApexMode picks what to count into (see EApexGameMode);
	// a demo lap is the one mode that drives itself to a finish.
	bAutoRaceNoStart = FParse::Param(FCommandLine::Get(), TEXT("ApexNoStart"));
	int32 ModeValue = -1;
	if (FParse::Value(FCommandLine::Get(), TEXT("ApexMode="), ModeValue)
		&& ModeValue >= 0
		&& ModeValue <= static_cast<int32>(EApexGameMode::Hotlap))
	{
		AutoRaceMode = static_cast<EApexGameMode>(ModeValue);
	}

	// -ApexHotlapOutAfter=N / -ApexHotlapGarageAfter=N / -ApexHotlapReplayAfter=N
	// press the garage card's buttons N seconds in, for a screenshot run of a
	// hotlap (-ApexMode=8) that nobody is at the keyboard for. Each may be a
	// comma list.
	if (GetWorld())
	{
		struct FHotlapSwitch
		{
			const TCHAR* Name;
			EApexHotlapAction Action;
		};
		static const FHotlapSwitch Switches[] = {
			{ TEXT("ApexHotlapOutAfter="), EApexHotlapAction::GoOut },
			{ TEXT("ApexHotlapReplayAfter="), EApexHotlapAction::ReplayBestLap },
		};
		for (const FHotlapSwitch& Switch : Switches)
		{
			FString Delays;
			if (!FParse::Value(FCommandLine::Get(), Switch.Name, Delays, /*bShouldStopOnSeparator*/ false))
			{
				continue;
			}
			TArray<FString> Parts;
			Delays.ParseIntoArray(Parts, TEXT(","));
			for (const FString& Part : Parts)
			{
				const float Seconds = FCString::Atof(*Part);
				if (Seconds <= 0.0f)
				{
					continue;
				}
				const EApexHotlapAction Action = Switch.Action;
				FTimerHandle Handle;
				GetWorld()->GetTimerManager().SetTimer(
					Handle,
					FTimerDelegate::CreateWeakLambda(this, [this, Action, Seconds]()
					{
						UE_LOG(LogApexSim, Log, TEXT("-ApexHotlap*After: garage action %d (%.0f s)"),
							static_cast<int32>(Action), Seconds);
						HandleHotlapAction(Action);
					}),
					Seconds,
					false);
			}
		}
		FString GarageDelays;
		if (FParse::Value(FCommandLine::Get(), TEXT("ApexHotlapGarageAfter="), GarageDelays, false))
		{
			TArray<FString> Parts;
			GarageDelays.ParseIntoArray(Parts, TEXT(","));
			for (const FString& Part : Parts)
			{
				const float Seconds = FCString::Atof(*Part);
				if (Seconds <= 0.0f)
				{
					continue;
				}
				FTimerHandle Handle;
				GetWorld()->GetTimerManager().SetTimer(
					Handle,
					FTimerDelegate::CreateWeakLambda(this, [this]()
					{
						HandlePauseAction(EApexPauseAction::ReturnToGarage);
					}),
					Seconds,
					false);
			}
		}
	}

	if (bAutoRaceRequested)
	{
		UE_LOG(LogApexSim, Log,
			TEXT("-ApexAutoRace: will create a session with %d AI over %d lap(s)%s"),
			AutoRaceAiCount, AutoRaceLaps,
			bAutoRaceNoStart ? TEXT(" and stop in the lobby") : TEXT(" and count it in"));
	}

	// Keys only reach a widget that has focus, or one above it; the processor
	// sees everything, which is what focus recovery and the in-race pause key
	// need. It outlives every input-mode change the shell makes.
	if (FSlateApplication::IsInitialized())
	{
		InputProcessor = MakeShared<FApexMenuInputProcessor>(this);
		FSlateApplication::Get().RegisterInputPreProcessor(InputProcessor);

		FocusChangingHandle = FSlateApplication::Get().OnFocusChanging().AddUObject(
			this, &UApexRootWidget::HandleFocusChanging);
	}

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
	if (FSlateApplication::IsInitialized())
	{
		if (InputProcessor.IsValid())
		{
			FSlateApplication::Get().UnregisterInputPreProcessor(InputProcessor);
		}
		FSlateApplication::Get().OnFocusChanging().Remove(FocusChangingHandle);
	}
	InputProcessor.Reset();
	FocusChangingHandle.Reset();

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
		Net->OnTelemetry.RemoveDynamic(this, &UApexRootWidget::HandleTelemetryForFinish);
	}
	if (UApexMenuFlowSubsystem* Flow = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexMenuFlowSubsystem>() : nullptr)
	{
		Flow->OnContentMismatch.RemoveDynamic(this, &UApexRootWidget::HandleContentMismatch);
	}
	if (UApexTrackGuideSubsystem* Guide = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexTrackGuideSubsystem>() : nullptr)
	{
		Guide->OnActiveChanged.Remove(GuideActiveHandle);
		Guide->OnFailed.Remove(GuideFailedHandle);
	}
	if (GetWorld())
	{
		GetWorld()->GetTimerManager().ClearTimer(ResultsAfterFinishTimer);
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
			ApexUiAudio::Play(this, EApexUiSound::Accept);
			return FReply::Handled();
		}
		return Super::NativeOnKeyDown(InGeometry, InKeyEvent);
	}

	if (Key == EKeys::Escape)
	{
		// Normally the screen has already answered this (UApexScreenWidget::
		// HandleBack); it reaches here when focus sits on the shell itself.
		if (CanGoBack())
		{
			ApexUiAudio::Play(this, EApexUiSound::Back);
			GoBack();
		}
		return FReply::Handled();
	}
	return Super::NativeOnKeyDown(InGeometry, InKeyEvent);
}

void UApexRootWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);
	// -ApexGuide=: opened once the menu world (and its director) is up.
	if (UApexTrackGuideSubsystem* Guide = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexTrackGuideSubsystem>() : nullptr)
	{
		Guide->ApplyCommandLine();
	}
	UpdateWatch(InDeltaTime);
	UpdateHotlapWatch();
	UpdateBackdrop(InDeltaTime);

	// The replay ends itself when the lap is over; the garage card comes back.
	if (HotlapPanel && HotlapPanel->GetView() == EApexHotlapView::Replay)
	{
		AApexRaceDirector* Director = AApexRaceDirector::Find(this);
		if (Director && Director->IsGhostReplayActive())
		{
			HotlapPanel->SetReplayTime(Director->GetGhostReplayTimeMs(), Director->GetGhostLapTimeMs());
		}
		else
		{
			HotlapPanel->SetView(bGarageOpen ? EApexHotlapView::Garage : EApexHotlapView::Track);
			RequestFocusDefault();
		}
	}
}

UTexture2D* UApexRootWidget::MakeScrimTexture()
{
	if (ScrimTexture)
	{
		return ScrimTexture;
	}
	constexpr int32 Width = 256;
	ScrimTexture = UTexture2D::CreateTransient(Width, 1, PF_B8G8R8A8);
	if (!ScrimTexture)
	{
		return nullptr;
	}
	ScrimTexture->SRGB = true;
	ScrimTexture->Filter = TF_Bilinear;
	ScrimTexture->AddressX = TA_Clamp;
	ScrimTexture->AddressY = TA_Clamp;

	const FColor Base = ApexUI::Palette::Background.ToFColorSRGB();
	FTexture2DMipMap& Mip = ScrimTexture->GetPlatformData()->Mips[0];
	FColor* Pixels = static_cast<FColor*>(Mip.BulkData.Lock(LOCK_READ_WRITE));
	for (int32 X = 0; X < Width; ++X)
	{
		// Dense behind the headings on the left, thin over the right half
		// where the race is left to be seen.
		const float U = static_cast<float>(X) / static_cast<float>(Width - 1);
		const float Alpha = FMath::Lerp(0.88f, 0.32f, FMath::SmoothStep(0.12f, 0.8f, U));
		Pixels[X] = FColor(Base.R, Base.G, Base.B, static_cast<uint8>(FMath::RoundToInt(Alpha * 255.0f)));
	}
	Mip.BulkData.Unlock();
	ScrimTexture->UpdateResource();
	return ScrimTexture;
}

void UApexRootWidget::UpdateBackdrop(float DeltaSeconds)
{
	AApexRaceDirector* Director = AApexRaceDirector::Find(this);
	if (bGuideLayers)
	{
		// The guide is the screen: the plain background covers the world
		// until its circuit is built and lit, then lets it through.
		const UApexTrackGuideSubsystem* Guide = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexTrackGuideSubsystem>() : nullptr;
		const float Reveal = Guide ? Guide->GetReveal() : 0.0f;
		if (Background)
		{
			Background->SetVisibility(ESlateVisibility::HitTestInvisible);
			Background->SetBrushColor(FLinearColor(1.0f, 1.0f, 1.0f, 1.0f - Reveal));
		}
		if (BackdropScrim)
		{
			BackdropScrim->SetVisibility(ESlateVisibility::Collapsed);
		}
		AppliedBackdrop = -1.0f;
		return;
	}
	if (WatchKind == EWatchKind::Backdrop)
	{
		// The race is the screen: the plain background covers only what the
		// backdrop has not faded in yet (a race loading, the next one coming).
		const float Watched = Director && Director->IsDemoViewActive() ? Director->GetDemoBackdropOpacity() : 0.0f;
		if (Background)
		{
			Background->SetBrushColor(FLinearColor(1.0f, 1.0f, 1.0f, 1.0f - Watched));
		}
		AppliedBackdrop = -1.0f;
		return;
	}
	const UApexScreenWidget* Screen = GetScreenWidget(CurrentScreen);
	const bool bScreenWants = Screen && Screen->WantsLiveBackdrop();
	BackdropGate = FMath::FInterpConstantTo(BackdropGate, bScreenWants ? 1.0f : 0.0f, DeltaSeconds, 4.0f);

	// In a session, before and after its race (the lobby, the results), the
	// backdrop is the session's own circuit. It goes once faded behind a screen
	// that keeps the world to itself (car select's turntable) and is built
	// again, from the track cache, when the lobby comes back.
	const UApexNetSubsystem* Net = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexNetSubsystem>() : nullptr;
	const bool bSessionBackdrop = Net && Net->IsInSession() && !Net->IsInDemoSession() && !bRaceViewActive;
	if (Director)
	{
		if (bSessionBackdrop && (bScreenWants || BackdropGate > 0.0f))
		{
			Director->BeginLobbyView();
		}
		else if (Director->IsLobbyViewActive())
		{
			Director->EndLobbyView();
		}
	}

	const bool bDemo = Director && Director->IsDemoViewActive() && !bRaceViewActive;
	if (bDemo)
	{
		// Hidden once faded, shown again as soon as a screen wants it.
		Director->SetDemoWorldVisible(bScreenWants || BackdropGate > 0.0f);
	}

	const bool bBackdropView = bDemo || (Director && Director->IsLobbyViewActive() && !bRaceViewActive);
	const float Opacity = bBackdropView ? Director->GetBackdropOpacity() * BackdropGate : 0.0f;
	if (FMath::IsNearlyEqual(Opacity, AppliedBackdrop, 0.002f) && AppliedBackdropScreen == CurrentScreen)
	{
		return;
	}
	if (AppliedBackdropScreen != CurrentScreen)
	{
		// The screen left behind is hidden, but must not come back see-through.
		if (UApexScreenWidget* Previous = GetScreenWidget(AppliedBackdropScreen))
		{
			Previous->SetBackdropOpacity(0.0f);
		}
	}
	AppliedBackdrop = Opacity;
	AppliedBackdropScreen = CurrentScreen;

	if (Background && !bRaceViewActive)
	{
		Background->SetBrushColor(FLinearColor(1.0f, 1.0f, 1.0f, 1.0f - Opacity));
	}
	if (BackdropScrim)
	{
		BackdropScrim->SetVisibility(Opacity > 0.0f && !bRaceViewActive ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
		BackdropScrim->SetRenderOpacity(Opacity);
	}
	if (UApexScreenWidget* Current = GetScreenWidget(CurrentScreen))
	{
		Current->SetBackdropOpacity(Opacity);
	}
}

void UApexRootWidget::HandleFocusChanging(
	const FFocusEvent& Event,
	const FWeakWidgetPath& OldPath,
	const TSharedPtr<SWidget>& OldWidget,
	const FWidgetPath& NewPath,
	const TSharedPtr<SWidget>& NewWidget)
{
	if (!NewWidget.IsValid() || NewWidget == OldWidget)
	{
		return;
	}

	// Slate's own search says Navigation; a host answering a direction with
	// ApexNav::Focus says SetDirectly, which is also what a screen opening
	// says — the navigation scope is what separates those two.
	const EFocusCause Cause = Event.GetCause();
	const bool bMovedByPlayer = Cause == EFocusCause::Navigation
		|| (Cause == EFocusCause::SetDirectly && ApexNav::IsNavigating());
	if (!bMovedByPlayer)
	{
		return;
	}

	// Only focus that lands inside the shell. In the editor this hook hears
	// every panel, and a tab through the details view is not a menu move.
	const TSharedPtr<SWidget> Shell = GetCachedWidget();
	if (!Shell.IsValid() || !NewPath.ContainsWidget(Shell.Get()))
	{
		return;
	}

	ApexUiAudio::Play(this, EApexUiSound::Move);
}

bool UApexRootWidget::IsRaceOverlayOpen() const
{
	return (PauseMenu && PauseMenu->IsOpen()) || (SettingsOverlay && SettingsOverlay->IsOpen()) || bGarageOpen
		|| IsHudEditorOpen();
}

bool UApexRootWidget::IsHudEditorOpen() const
{
	return HudEditor && HudEditor->IsOpen();
}

void UApexRootWidget::OpenHudEditor()
{
	if (!HudEditor || !Hud || HudEditor->IsOpen())
	{
		return;
	}
	// Settings steps aside without closing, so closing the editor lands back
	// on the page it was opened from. So does the pause menu under it: its
	// scrim would otherwise dim the very HUD being laid out.
	if (SettingsOverlay && SettingsOverlay->IsOpen())
	{
		SettingsOverlay->SetVisibility(ESlateVisibility::Collapsed);
	}
	if (PauseMenu && PauseMenu->IsOpen())
	{
		PauseMenu->SetVisibility(ESlateVisibility::Collapsed);
	}
	HudEditor->Open(Hud);
	ApplyDriveInput();
}

void UApexRootWidget::HandleHudEditorClosed()
{
	if (SettingsOverlay && SettingsOverlay->IsOpen())
	{
		SettingsOverlay->SetVisibility(ESlateVisibility::Visible);
	}
	if (PauseMenu && PauseMenu->IsOpen())
	{
		PauseMenu->SetVisibility(ESlateVisibility::Visible);
	}
	ApplyDriveInput();
	RequestFocusDefault();
}

void UApexRootWidget::ApplyDriveInput()
{
	if (AApexPlayerController* PlayerController =
			Cast<AApexPlayerController>(UGameplayStatics::GetPlayerController(this, 0)))
	{
		PlayerController->SetDriveInputEnabled(bRaceViewActive && !IsWatching() && !bPauseMenuOpen && !bGarageOpen && !IsHudEditorOpen());
	}
}

bool UApexRootWidget::IsSettingsOpen() const
{
	return SettingsOverlay && SettingsOverlay->IsOpen();
}

bool UApexRootWidget::IsSettingsListening() const
{
	return SettingsOverlay && SettingsOverlay->IsOpen() && SettingsOverlay->IsListening();
}

bool UApexRootWidget::IsScreenActive(const UApexScreenWidget* Screen) const
{
	return Screen && !bRaceViewActive && GetScreenWidget(CurrentScreen) == Screen;
}

void UApexRootWidget::FocusDefault()
{
	// Front to back: whatever is drawn on top is what the keys should reach.
	if (IsHudEditorOpen())
	{
		HudEditor->FocusDefault();
		return;
	}
	if (SettingsOverlay && SettingsOverlay->IsOpen())
	{
		SettingsOverlay->FocusDefault();
		return;
	}
	if (PauseMenu && PauseMenu->IsOpen())
	{
		PauseMenu->FocusDefault();
		return;
	}
	if ((bGarageOpen || (HotlapPanel && HotlapPanel->IsMenuMode())) && HotlapPanel
		&& HotlapPanel->GetView() == EApexHotlapView::Garage)
	{
		HotlapPanel->FocusDefault();
		return;
	}
	if (bRaceViewActive || IsWatching() || bGuideLayers)
	{
		// Driving: the viewport has focus on purpose, so the car gets the keys.
		// The guide: the input processor hands it every key.
		// Watching: the input processor hands every key to the watch view.
		return;
	}
	if (UApexScreenWidget* Screen = GetScreenWidget(CurrentScreen))
	{
		Screen->FocusDefault();
	}
}

UUserWidget* UApexRootWidget::GetFrontSurface() const
{
	// The same order as FocusDefault.
	if (IsHudEditorOpen())
	{
		return HudEditor;
	}
	if (SettingsOverlay && SettingsOverlay->IsOpen())
	{
		return SettingsOverlay;
	}
	if (PauseMenu && PauseMenu->IsOpen())
	{
		return PauseMenu;
	}
	if ((bGarageOpen || (HotlapPanel && HotlapPanel->IsMenuMode())) && HotlapPanel
		&& HotlapPanel->GetView() == EApexHotlapView::Garage)
	{
		return HotlapPanel;
	}
	if (bRaceViewActive || IsWatching() || bGuideLayers)
	{
		return nullptr;
	}
	return GetScreenWidget(CurrentScreen);
}

void UApexRootWidget::RequestFocusDefault()
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().SetTimerForNextTick(
			FTimerDelegate::CreateWeakLambda(this, [this]() { FocusDefault(); }));
	}
}

void UApexRootWidget::SetPaused(bool bPaused)
{
	// The pause key during a replay stops the replay, as its strip promises.
	if (bPaused && !bPauseMenuOpen)
	{
		if (AApexRaceDirector* Director = AApexRaceDirector::Find(this))
		{
			if (Director->IsGhostReplayActive())
			{
				Director->EndGhostReplay();
				return;
			}
		}
	}
	if (bPauseMenuOpen == bPaused || !PauseMenu)
	{
		return;
	}
	bPauseMenuOpen = bPaused;

	if (bPaused)
	{
		PauseMenu->SetWatching(IsWatching());
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
		PlayerController->SetDriveInputEnabled(!bPaused && bRaceViewActive && !IsWatching() && !bGarageOpen);
	}

	// The input-mode switch above hands focus to the viewport when its deferred
	// operations run, which is after Open() focused the first row. A tick later
	// the menu takes it back for good; on resume there is nothing to focus.
	RequestFocusDefault();
}

void UApexRootWidget::SetGarageOpen(bool bOpen)
{
	if (bGarageOpen == bOpen)
	{
		return;
	}
	bGarageOpen = bOpen;
	if (HotlapPanel && bHotlapSession)
	{
		HotlapPanel->SetView(bOpen ? EApexHotlapView::Garage : EApexHotlapView::Track);
	}
	if (Hud)
	{
		Hud->SetShown(!bOpen);
	}
	if (AApexPlayerController* PlayerController =
			Cast<AApexPlayerController>(UGameplayStatics::GetPlayerController(this, 0)))
	{
		PlayerController->SetDriveInputEnabled(!bOpen && bRaceViewActive && !bPauseMenuOpen);
	}
	if (bOpen)
	{
		ApexUiAudio::Play(this, EApexUiSound::Notice);
	}
	RequestFocusDefault();
}

void UApexRootWidget::HandleHotlapWatch(int32 CarIndex)
{
	// The qualifying scoreboard's click: the race director's spectator camera
	// on that car (it is a live session, the local car in its garage), and
	// back to the driver's own view when the scoreboard lets go.
	AApexRaceDirector* Director = AApexRaceDirector::Find(this);
	if (!Director)
	{
		return;
	}
	if (CarIndex == INDEX_NONE)
	{
		if (Director->IsSpectating())
		{
			Director->SetSpectating(false);
		}
		return;
	}
	if (!Director->IsSpectating())
	{
		Director->SetSpectating(true);
	}
	Director->FocusCar(CarIndex);
	if (HotlapPanel)
	{
		HotlapPanel->SetWatchCameraLabel(ApexSpectate::CameraName(Director->GetSpectatorCamera()));
	}
	ApexUiAudio::Play(this, EApexUiSound::Move);
}

void UApexRootWidget::HandleHotlapAction(EApexHotlapAction Action)
{
	UApexNetSubsystem* Net = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexNetSubsystem>() : nullptr;
	UApexSettingsSubsystem* Settings = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexSettingsSubsystem>() : nullptr;
	switch (Action)
	{
	case EApexHotlapAction::GoOut:
		// The telemetry's garage flag flips the view once the server has moved the car.
		if (Net)
		{
			Net->HotlapRelocate(EApexHotlapDestination::Track,
				Settings && Settings->Get() && Settings->Get()->bHotlapColdTyres);
		}
		break;

	case EApexHotlapAction::CycleWatchCamera:
		if (AApexRaceDirector* Director = AApexRaceDirector::Find(this))
		{
			if (Director->IsSpectating())
			{
				Director->CycleSpectatorCamera();
				if (HotlapPanel)
				{
					HotlapPanel->SetWatchCameraLabel(ApexSpectate::CameraName(Director->GetSpectatorCamera()));
				}
			}
		}
		break;

	case EApexHotlapAction::ReplayBestLap:
		if (AApexRaceDirector* Director = AApexRaceDirector::Find(this))
		{
			if (Director->BeginGhostReplay())
			{
				if (HotlapPanel)
				{
					HotlapPanel->SetView(EApexHotlapView::Replay);
				}
			}
			else
			{
				ShowToast(TEXT("No record lap to replay yet"), false);
			}
		}
		break;

	case EApexHotlapAction::ToggleGhost:
		if (Settings && Settings->Get())
		{
			Settings->SetGhostCar(!Settings->Get()->bGhostCar);
		}
		break;

	case EApexHotlapAction::ToggleColdTyres:
		if (Settings && Settings->Get())
		{
			Settings->SetHotlapColdTyres(!Settings->Get()->bHotlapColdTyres);
		}
		break;

	case EApexHotlapAction::SaveReplay:
		SaveReplay();
		break;

	case EApexHotlapAction::ResetSetup:
		if (Settings)
		{
			Settings->ResetToDefaults(EApexSettingsGroup::CarSetup);
		}
		break;

	case EApexHotlapAction::CloseEditor:
		CloseSetupEditor();
		break;
	}
}

void UApexRootWidget::OpenSetupEditor()
{
	if (!HotlapPanel || HotlapPanel->GetView() != EApexHotlapView::Hidden)
	{
		return;
	}
	HotlapPanel->SetMenuMode(true);
	HotlapPanel->SetView(EApexHotlapView::Garage);
	ApexUiAudio::Play(this, EApexUiSound::Accept);
	RequestFocusDefault();
}

void UApexRootWidget::CloseSetupEditor()
{
	if (!HotlapPanel || !HotlapPanel->IsMenuMode())
	{
		return;
	}
	HotlapPanel->SetView(EApexHotlapView::Hidden);
	HotlapPanel->SetMenuMode(false);
	ApexUiAudio::Play(this, EApexUiSound::Back);
	FocusDefault();
}

void UApexRootWidget::HandleTelemetryForHotlap(const FApexTelemetryFrame& Frame)
{
	const UApexNetSubsystem* Net = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexNetSubsystem>() : nullptr;
	if (!Net || !bRaceViewActive || Net->IsInDemoSession() || !Net->IsInSession())
	{
		return;
	}
	// Hotlap and qualifying share the garage; qualifying adds the scoreboard.
	const bool bHotlap = ApexIsGarageMode(Frame.GameMode);
	const bool bQualifying = Frame.GameMode == EApexGameMode::Qualification;
	if (bHotlap && HotlapPanel && HotlapPanel->IsQualifying() != bQualifying)
	{
		HotlapPanel->SetQualifying(bQualifying);
	}
	if (bHotlap != bHotlapSession)
	{
		bHotlapSession = bHotlap;
		if (HotlapPanel)
		{
			HotlapPanel->SetQualifying(bQualifying);
			HotlapPanel->SetActive(bHotlap);
		}
		if (bHotlap && !bQualifying && Net)
		{
			// The stored record lap's trace, for the ghost and the replay.
			const_cast<UApexNetSubsystem*>(Net)->RequestGhost();
		}
		if (!bHotlap)
		{
			SetGarageOpen(false);
		}
	}
	if (!bHotlap)
	{
		return;
	}
	const int32 LocalIndex = Net->GetLocalCarIndex();
	const FApexCarTelemetry* Local = Frame.Cars.FindByPredicate(
		[LocalIndex](const FApexCarTelemetry& Car) { return Car.CarIndex == LocalIndex; });
	if (!Local)
	{
		return;
	}
	if (HotlapPanel)
	{
		HotlapPanel->SetLiveLap(Local->CurrentLap, Local->CurrentLapTimeMs, Local->bLapInvalid, Local->BestLapTimeMs);
	}
	SetGarageOpen(Local->bInGarage);
}

void UApexRootWidget::HandleLapRecordForGhost(const FApexLapRecord& Record)
{
	UApexNetSubsystem* Net = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexNetSubsystem>() : nullptr;
	if (Net && bHotlapSession && Record.bIsNew && Record.bHasGhost)
	{
		Net->RequestGhost();
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
		OpenSettings(EApexSettingsTab::Gameplay);
		break;

	case EApexPauseAction::SaveReplay:
		SaveReplay();
		break;

	case EApexPauseAction::ReturnToGarage:
		SetPaused(false);
		if (UApexNetSubsystem* Net = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexNetSubsystem>() : nullptr)
		{
			Net->HotlapRelocate(EApexHotlapDestination::Garage);
		}
		break;

	case EApexPauseAction::RecoverToTrack:
	case EApexPauseAction::RecoverToPits:
		SetPaused(false);
		if (UApexNetSubsystem* Net = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexNetSubsystem>() : nullptr)
		{
			Net->RecoverCar(Action == EApexPauseAction::RecoverToPits
				? EApexRecoverDestination::Pits : EApexRecoverDestination::Track);
		}
		break;

	case EApexPauseAction::LeaveSession:
		SetPaused(false);
		if (IsWatching())
		{
			StopWatching();
			break;
		}
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

void UApexRootWidget::OpenSettings(EApexSettingsTab Tab)
{
	if (SettingsOverlay && !SettingsOverlay->IsOpen())
	{
		SettingsOverlay->Open(Tab);
	}
}

void UApexRootWidget::HandleSettingsClosed()
{
	// Back onto the pause menu, which never went away.
	if (bPauseMenuOpen && PauseMenu)
	{
		PauseMenu->Open();
	}
	FocusDefault();
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

	// Not this frame: on the first activation the shell is still being added
	// to the viewport and the game mode's SetInputMode runs straight after,
	// handing focus to the viewport widget. A tick later the tree is live and
	// the focus sticks.
	RequestFocusDefault();
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
	ApexUiAudio::Play(this, bIsError ? EApexUiSound::Error : EApexUiSound::Notice);

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

	// -ApexWatchSession: spectate the first race being driven on the server,
	// for an unattended run of the live watch view.
	if (!bWatchSessionFromCommandLine && !IsWatching() && !bWatchJoinPending
		&& FParse::Param(FCommandLine::Get(), TEXT("ApexWatchSession")))
	{
		if (const FApexSessionSummary* Live = LobbyState.AvailableSessions.FindByPredicate(
				[](const FApexSessionSummary& Session) { return Session.IsWatchable(); }))
		{
			bWatchSessionFromCommandLine = true;
			UE_LOG(LogApexSim, Log, TEXT("-ApexWatchSession: watching %s on %s"), *Live->Id, *Live->TrackName);
			WatchSession(Live->Id);
		}
	}
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

	FApexCarConfigSummary Car = LobbyState.CarConfigs[0];
	if (!AutoRaceCar.IsEmpty())
	{
		if (const FApexCarConfigSummary* Named = LobbyState.CarConfigs.FindByPredicate(
				[this](const FApexCarConfigSummary& Candidate) {
					return Candidate.Name.Contains(AutoRaceCar);
				}))
		{
			Car = *Named;
		}
		else
		{
			UE_LOG(LogApexSim, Warning, TEXT("-ApexCar: no lobby car matches '%s'"), *AutoRaceCar);
		}
	}
	// -ApexTrack wins, then the player's remembered track, then the server's
	// first. Le Mans sorts first and is 13 km, which makes for a long wait.
	// It matches the YAML stem (`Spielberg`) exactly, else a substring of the
	// lobby name: the names are parodies (`Red Pull Ring`) and the stems are
	// what the docs and scripts say.
	FApexTrackConfigSummary Track = LobbyState.TrackConfigs[0];
	bool bTrackForced = false;
	if (!AutoRaceTrack.IsEmpty())
	{
		const FApexTrackConfigSummary* Named = LobbyState.TrackConfigs.FindByPredicate(
			[this, Flow](const FApexTrackConfigSummary& Candidate) {
				FApexTrackCatalogRow Row;
				return Flow && Flow->GetTrackCatalogRow(Candidate.Id, Row)
					&& Row.YamlBaseName.Equals(AutoRaceTrack, ESearchCase::IgnoreCase);
			});
		if (!Named)
		{
			Named = LobbyState.TrackConfigs.FindByPredicate(
				[this](const FApexTrackConfigSummary& Candidate) {
					return Candidate.Name.Contains(AutoRaceTrack);
				});
		}
		if (Named)
		{
			Track = *Named;
			bTrackForced = true;
		}
		else
		{
			UE_LOG(LogApexSim, Warning, TEXT("-ApexTrack: no lobby track matches '%s'"),
				*AutoRaceTrack);
		}
	}
	if (!bTrackForced && Flow->HasPendingTrack())
	{
		Net->FindTrackById(Flow->GetPendingTrackId(), Track);
	}

	Flow->SetPendingCar(Car.Id);
	Flow->SetPendingTrack(Track.Id);
	Flow->CreateLapLimit = AutoRaceLaps;
	Flow->CreateStartingMode = AutoRaceMode;
	// -ApexRaceMinutes=N: a timed race of N minutes instead of -ApexLaps.
	int32 RaceMinutes = 0;
	FParse::Value(FCommandLine::Get(), TEXT("ApexRaceMinutes="), RaceMinutes);
	Flow->bCreateTimedRace = RaceMinutes > 0;
	if (RaceMinutes > 0)
	{
		Flow->CreateRaceMinutes = RaceMinutes;
	}

	UE_LOG(LogApexSim, Log, TEXT("-ApexAutoRace: creating '%s' on '%s' with %d AI over %d lap(s)"),
		*Car.Name, *Track.Name, AutoRaceAiCount, AutoRaceLaps);

	// -ApexLockAssists=abs,tc,gearbox,steering,line,damage forbids those assists in
	// the session, the way the create screen's chips would, so a screenshot
	// run can look at the Assists tab with its locks on.
	FApexAllowedAssists Allowed;
	FString LockList;
	if (FParse::Value(FCommandLine::Get(), TEXT("ApexLockAssists="), LockList, /*bShouldStopOnSeparator*/ false))
	{
		TArray<FString> Locks;
		LockList.ParseIntoArray(Locks, TEXT(","));
		for (const FString& Lock : Locks)
		{
			const FString Key = Lock.TrimStartAndEnd().ToLower();
			if (Key == TEXT("abs"))           { Allowed.bAbs = false; }
			else if (Key == TEXT("tc"))       { Allowed.bTractionControl = false; }
			else if (Key == TEXT("gearbox"))  { Allowed.bAutoGearbox = false; }
			else if (Key == TEXT("steering")) { Allowed.bSteeringAssist = false; }
			else if (Key == TEXT("line"))     { Allowed.bRacingLine = false; }
			else
			{
				UE_LOG(LogApexSim, Warning, TEXT("-ApexLockAssists: unknown assist '%s' (abs, tc, gearbox, steering, line, damage)"), *Lock);
			}
		}
	}

	// -ApexWeather=sunny|cloudy|overcast|lightrain|heavyrain and
	// -ApexTimeOfDay=HH:MM put a screenshot run under any sky.
	FApexSessionConditions Conditions;
	FString WeatherName;
	if (FParse::Value(FCommandLine::Get(), TEXT("ApexWeather="), WeatherName))
	{
		const FString Key = WeatherName.TrimStartAndEnd().ToLower().Replace(TEXT("_"), TEXT("")).Replace(TEXT(" "), TEXT(""));
		if (Key == TEXT("sunny"))          { Conditions.Weather = EApexWeather::Sunny; }
		else if (Key == TEXT("cloudy"))    { Conditions.Weather = EApexWeather::Cloudy; }
		else if (Key == TEXT("overcast"))  { Conditions.Weather = EApexWeather::Overcast; }
		else if (Key == TEXT("lightrain") || Key == TEXT("rain")) { Conditions.Weather = EApexWeather::LightRain; }
		else if (Key == TEXT("heavyrain")) { Conditions.Weather = EApexWeather::HeavyRain; }
		else
		{
			UE_LOG(LogApexSim, Warning, TEXT("-ApexWeather: unknown weather '%s' (sunny, cloudy, overcast, lightrain, heavyrain)"), *WeatherName);
		}
	}
	FString Clock;
	if (FParse::Value(FCommandLine::Get(), TEXT("ApexTimeOfDay="), Clock))
	{
		FString HourText, MinuteText;
		if (!Clock.Split(TEXT(":"), &HourText, &MinuteText))
		{
			HourText = Clock;
			MinuteText = TEXT("0");
		}
		if (HourText.IsNumeric() && MinuteText.IsNumeric())
		{
			Conditions.TimeOfDayMinutes = FCString::Atoi(*HourText) * 60 + FCString::Atoi(*MinuteText);
			Conditions = Conditions.Clamped();
		}
		else
		{
			UE_LOG(LogApexSim, Warning, TEXT("-ApexTimeOfDay: expected HH:MM, got '%s'"), *Clock);
		}
	}
	// -ApexTimeScale=N (the day's clock, 0 frozen to 60), -ApexChangeable=N
	// (0 fixed, 1 settled, 2 changeable, 3 stormy) and -ApexTrackRubber=N (0
	// green to 100 rubbered): a sky that moves through the run.
	int32 SkyValue = 0;
	if (FParse::Value(FCommandLine::Get(), TEXT("ApexTimeScale="), SkyValue))
	{
		Conditions.TimeScale = SkyValue > 0 ? SkyValue : FApexSessionConditions::Auto;
	}
	if (FParse::Value(FCommandLine::Get(), TEXT("ApexChangeable="), SkyValue))
	{
		Conditions.Changeable = SkyValue > 0 ? SkyValue : FApexSessionConditions::Auto;
	}
	if (FParse::Value(FCommandLine::Get(), TEXT("ApexTrackRubber="), SkyValue))
	{
		Conditions.TrackRubberPct = FMath::Max(SkyValue, 0);
	}
	Conditions = Conditions.Clamped();

	// SelectCar before CreateSession, same order the UI uses.
	Net->SelectCar(Car.Id);
	// Room for the AI and the host: the AI are seated first, and a field that
	// fills the session leaves the host refused (Error 500) on the main menu.
	const int32 MaxPlayers = FMath::Clamp(AutoRaceAiCount + 1, 8, 255);
	// -ApexDamage=off|reduced|full: the session's damage rule (full by default).
	EApexDamageLevel Damage = EApexDamageLevel::Full;
	FString DamageText;
	if (FParse::Value(FCommandLine::Get(), TEXT("ApexDamage="), DamageText))
	{
		Damage = DamageText.Equals(TEXT("off"), ESearchCase::IgnoreCase) ? EApexDamageLevel::Off
			: DamageText.Equals(TEXT("reduced"), ESearchCase::IgnoreCase) ? EApexDamageLevel::Reduced
			: EApexDamageLevel::Full;
	}
	// -ApexAiSkill=70..110: the AI field's level (the mixed field by default).
	int32 AiSkill = ApexAiSkill::Mixed;
	if (FParse::Value(FCommandLine::Get(), TEXT("ApexAiSkill="), AiSkill))
	{
		AiSkill = ApexAiSkill::Clamp(AiSkill);
	}
	Net->CreateSession(Track.Id, MaxPlayers, AutoRaceAiCount, AutoRaceLaps, EApexSessionKind::Practice, Allowed, Conditions, Damage,
		AiSkill, Flow->EffectiveRaceSeconds());
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

void UApexRootWidget::SendDriverAids()
{
	UGameInstance* GameInstance = GetGameInstance();
	UApexNetSubsystem* Net = GameInstance ? GameInstance->GetSubsystem<UApexNetSubsystem>() : nullptr;
	const UApexSettingsSubsystem* Settings = GameInstance ? GameInstance->GetSubsystem<UApexSettingsSubsystem>() : nullptr;
	if (!Net || !Net->IsInSession() || !Settings || !Settings->Get())
	{
		return;
	}
	// The player's own choice goes as is; the server applies the session's
	// allowed set on top, so a locked aid is off whatever is sent here.
	const UApexSettingsSave* Values = Settings->Get();
	// The steering aid scales the lock down to what the tyres can hold at
	// this speed: a stick at its stop is a sensible turn. On a wheel it gears
	// the rim down several times over, so the self-aligning torque has to
	// move the rim that much further before the tyres feel it, and a slide
	// cannot be caught. A wheel drives the rack directly; the setting is kept
	// for when the player goes back to a pad.
	bAidsSentForWheel = Settings->IsSteeringOnWheel();
	Net->SetDriverAids(
		Values->bAutoGearbox,
		Values->bSteeringAssist && !bAidsSentForWheel,
		Values->bAbs,
		static_cast<EApexTractionControl>(Values->TractionControl));
}

void UApexRootWidget::SendCarSetup()
{
	UGameInstance* GameInstance = GetGameInstance();
	UApexNetSubsystem* Net = GameInstance ? GameInstance->GetSubsystem<UApexNetSubsystem>() : nullptr;
	const UApexSettingsSubsystem* Settings = GameInstance ? GameInstance->GetSubsystem<UApexSettingsSubsystem>() : nullptr;
	if (!Net || !Net->IsInSession() || !Settings || !Settings->Get())
	{
		return;
	}
	// Clamped again by the server; sent whole so a knob put back to stock is
	// stock there too.
	Net->SetCarSetup(Settings->Get()->CarSetup);
}

void UApexRootWidget::HandleSettingsChangedForDriverAids(EApexSettingsGroup Group)
{
	if (Group == EApexSettingsGroup::Assists)
	{
		SendDriverAids();
	}
	else if (Group == EApexSettingsGroup::Controls || Group == EApexSettingsGroup::Wheel)
	{
		// A rebind, or a wheel plugged in or pulled out, can move the steering
		// between a wheel and a pad; only that changes what the aids are.
		const UApexSettingsSubsystem* Settings =
			GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexSettingsSubsystem>() : nullptr;
		if (Settings && Settings->IsSteeringOnWheel() != bAidsSentForWheel)
		{
			SendDriverAids();
		}
	}
	else if (Group == EApexSettingsGroup::CarSetup)
	{
		SendCarSetup();
	}
}

void UApexRootWidget::HandleContentMismatch(const FString& Message)
{
	ShowToast(Message, true);
}

void UApexRootWidget::HandleSessionJoined(const FString& SessionId, int32 GridPosition)
{
	// Joining always lands in the lobby, whether the session was created or
	// joined from the browser, so the transition belongs here rather than in
	// both screens.
	if (UApexTrackGuideSubsystem* Guide = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexTrackGuideSubsystem>() : nullptr)
	{
		// A session (a one-click hotlap, say) takes the director from the guide.
		Guide->Close();
	}
	BackStack.Reset();
	const UApexNetSubsystem* JoinedNet = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexNetSubsystem>() : nullptr;
	const bool bJoinedHotlap = JoinedNet && JoinedNet->IsHotlapWatch();
	if (!bJoinedHotlap)
	{
		// (A watched hotlap is under way already, and the shell stays out of
		// sight from the moment it is asked for.)
		ActivateScreen(EApexScreen::SessionLobby);
	}

	if (JoinedNet && JoinedNet->IsSessionSpectator())
	{
		// No car: nothing to tune, nothing to start. The race view opens with
		// the session's next telemetry when it is under way, in the lobby
		// until then.
		bWatchJoinPending = false;
		if (bJoinedHotlap)
		{
			bHotlapJoinPending = false;
			bHotlapWatchWanted = true;
		}
		if (WatchKind == EWatchKind::Backdrop)
		{
			StopWatching();
		}
		WatchKind = EWatchKind::Live;
		ApplyWatchLayers();
		PushWatchState();
		UE_LOG(LogApexSim, Log, TEXT("Watching session %s"), *SessionId);
		return;
	}
	SendDriverAids();
	SendCarSetup();

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
			Net->StartCountdown(FMath::Clamp(AutoRaceCountdown, 1, 60), AutoRaceMode);
		}
	}
}

void UApexRootWidget::HandleSessionLeft()
{
	if (bHotlapWatchWanted)
	{
		EndHotlapWatch();
	}
	if (WatchKind == EWatchKind::Live)
	{
		WatchKind = EWatchKind::None;
		ApplyWatchLayers();
		PushWatchState();
	}
	ResetFinishWatch();
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
	case EApexGameMode::Hotlap:
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
	if (NewState == EApexSessionState::Countdown || NewState == EApexSessionState::Lobby)
	{
		ResetFinishWatch();
	}

	if (NewState == EApexSessionState::Finished)
	{
		// The session recorder has classified it by now — the recorder handles
		// this same transition, and subsystems are notified before widgets.
		if (GetWorld())
		{
			GetWorld()->GetTimerManager().ClearTimer(ResultsAfterFinishTimer);
		}
		SetRaceViewActive(false);
		BackStack.Reset();
		ActivateScreen(EApexScreen::SessionResults);
		return;
	}

	SetRaceViewActive(NewState != EApexSessionState::Lobby);
}

void UApexRootWidget::ResetFinishWatch()
{
	bWinnerAnnounced = false;
	bLocalFinished = false;
	if (GetWorld())
	{
		GetWorld()->GetTimerManager().ClearTimer(ResultsAfterFinishTimer);
	}
}

void UApexRootWidget::HandleTelemetryForFinish(const FApexTelemetryFrame& Frame)
{
	const UApexNetSubsystem* Net = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexNetSubsystem>() : nullptr;
	// The menu's demo race finishes too, behind the screens; nobody is in it.
	if (!Net || Net->IsInDemoSession() || !Net->IsInSession()
		|| Frame.SessionState != EApexSessionState::Racing || Frame.GameMode != EApexGameMode::Race)
	{
		return;
	}

	const int32 LocalIndex = Net->GetLocalCarIndex();
	const FApexCarTelemetry* Winner = nullptr;
	const FApexCarTelemetry* Local = nullptr;
	for (const FApexCarTelemetry& Car : Frame.Cars)
	{
		if (Car.FinishPosition == 1)
		{
			Winner = &Car;
		}
		if (Car.CarIndex == LocalIndex)
		{
			Local = &Car;
		}
	}

	// Positions never change once given and every frame repeats them, so a lost
	// datagram only delays this by a frame.
	if (Winner && !bWinnerAnnounced && Net->IsSessionSpectator())
	{
		bWinnerAnnounced = true;
		const FApexRosterEntry* Row = Net->GetSessionRoster().Entries.FindByPredicate(
			[Winner](const FApexRosterEntry& Entry) { return Entry.CarIndex == Winner->CarIndex; });
		ShowToast(Row && !Row->PlayerName.IsEmpty()
			? FString::Printf(TEXT("Chequered flag: %s wins"), *Row->PlayerName)
			: FString(TEXT("Chequered flag: the winner is in")));
		return;
	}
	if (Winner && !bWinnerAnnounced)
	{
		bWinnerAnnounced = true;
		if (Winner->CarIndex != LocalIndex && !(Local && Local->FinishPosition > 0))
		{
			FString Name;
			if (const FApexRosterEntry* Row = Net->GetSessionRoster().Entries.FindByPredicate(
					[Winner](const FApexRosterEntry& Entry) { return Entry.CarIndex == Winner->CarIndex; }))
			{
				Name = Row->PlayerName;
			}
			ShowToast(Name.IsEmpty()
				? TEXT("Chequered flag: the winner is in — finish your race")
				: FString::Printf(TEXT("Chequered flag: %s wins — finish your race"), *Name));
		}
	}

	if (Local && Local->FinishPosition > 0 && !bLocalFinished)
	{
		bLocalFinished = true;
		ShowResultsAfterFinish(Local->FinishPosition);
	}
}

void UApexRootWidget::ShowResultsAfterFinish(int32 Position)
{
	UE_LOG(LogApexSim, Log, TEXT("Local car took the flag in P%d; showing live results"), Position);
	ShowToast(Position == 1
		? FString(TEXT("Chequered flag — you win!"))
		: FString::Printf(TEXT("Chequered flag — you finished P%d"), Position));

	// A beat to see the line go by before the race view gives way. The server
	// drives the car on from here, so nothing is lost by leaving.
	constexpr float FlagToResultsSeconds = 2.5f;
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	World->GetTimerManager().SetTimer(
		ResultsAfterFinishTimer,
		FTimerDelegate::CreateWeakLambda(this, [this]()
		{
			if (!bLocalFinished || !bRaceViewActive)
			{
				return;
			}
			SetRaceViewActive(false);
			BackStack.Reset();
			ActivateScreen(EApexScreen::SessionResults);
		}),
		FlagToResultsSeconds,
		false);
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
	if (bActive && BackdropScrim)
	{
		BackdropScrim->SetVisibility(ESlateVisibility::Collapsed);
	}
	// Re-applied from scratch on the next tick either way.
	AppliedBackdrop = -1.0f;

	if (Hud)
	{
		Hud->SetRaceActive(bActive);
	}

	if (!bActive)
	{
		// A session that ends while paused — a race finishing, a disconnect —
		// must not leave the overlays up over the menu. A layout half made
		// over the race is put back rather than saved behind the player's back.
		if (HudEditor && HudEditor->IsOpen())
		{
			HudEditor->Close(/*bSave*/ false);
		}
		if (SettingsOverlay)
		{
			SettingsOverlay->Close();
		}
		SetPaused(false);
		SetGarageOpen(false);
		bHotlapSession = false;
		if (HotlapPanel)
		{
			HotlapPanel->SetActive(false);
		}
		RequestFocusDefault();
	}

	if (AApexRaceDirector* Director = AApexRaceDirector::Find(this))
	{
		if (bActive)
		{
			Director->BeginRaceView();
			if (WatchKind == EWatchKind::Live)
			{
				Director->SetSpectating(true);
				ApplyDriveInput();
			}
		}
		else
		{
			Director->EndRaceView();
		}
	}
	if (!bActive && WatchKind == EWatchKind::Live)
	{
		// The race is over (the results follow) or the session went.
		WatchKind = EWatchKind::None;
		ApplyWatchLayers();
		PushWatchState();
	}
	else if (bActive)
	{
		UE_LOG(LogApexSim, Warning,
			TEXT("No AApexRaceDirector in the level; telemetry will arrive but nothing will render"));
	}

	UE_LOG(LogApexSim, Log, TEXT("Race view %s"), bActive ? TEXT("entered") : TEXT("left"));
}

// ---------------------------------------------------------------------------
// Watching a race
// ---------------------------------------------------------------------------

bool UApexRootWidget::WatchBackdrop()
{
	if (IsWatching())
	{
		return true;
	}
	AApexRaceDirector* Director = AApexRaceDirector::Find(this);
	const UApexDemoModeSubsystem* Demo = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexDemoModeSubsystem>() : nullptr;
	const bool bRaceUp = Director && Director->IsDemoViewActive();
	if (bRaceViewActive || !Director || !(bRaceUp || (Demo && Demo->IsDemoExpected())))
	{
		ShowToast(TEXT("No race to watch right now: connect to a server or add a showcase"), true);
		return false;
	}
	WatchKind = EWatchKind::Backdrop;
	WatchIdleSeconds = 0.0f;
	bWatchDemoUp = false;
	bWatchOverlayHidden = false;
	WatchTower = ApexSpectate::ETowerMode::Interval;
	ApplyWatchLayers();
	// UpdateWatch hands the race to the spectator's controls as soon as it is up.
	UpdateWatch(0.0f);
	PushWatchState();
	UE_LOG(LogApexSim, Log, TEXT("Watching the backdrop race%s"), bRaceUp ? TEXT("") : TEXT(" (waiting for it)"));
	return true;
}

void UApexRootWidget::WatchSession(const FString& SessionId)
{
	UApexNetSubsystem* Net = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexNetSubsystem>() : nullptr;
	if (!Net || !Net->IsAuthenticated())
	{
		ShowToast(TEXT("Not connected to a server yet"), true);
		return;
	}
	bWatchJoinPending = true;
	WatchTower = ApexSpectate::ETowerMode::Interval;
	bWatchOverlayHidden = false;
	Net->JoinAsSpectator(SessionId);
}

void UApexRootWidget::StopWatching()
{
	const EWatchKind Was = WatchKind;
	if (Was == EWatchKind::None)
	{
		return;
	}
	WatchKind = EWatchKind::None;
	const bool bWasReplay = bWatchingReplay;
	bWatchingReplay = false;
	if (bWasReplay)
	{
		if (UApexDemoModeSubsystem* Demo = GetGameInstance()->GetSubsystem<UApexDemoModeSubsystem>())
		{
			Demo->StopReplay();
		}
	}
	if (AApexRaceDirector* Director = AApexRaceDirector::Find(this))
	{
		Director->SetSpectating(false);
	}
	if (bPauseMenuOpen)
	{
		SetPaused(false);
	}
	if (Was == EWatchKind::Live)
	{
		// HandleSessionLeft takes the view back and returns to the main menu.
		if (UApexNetSubsystem* Net = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexNetSubsystem>() : nullptr)
		{
			Net->LeaveSession();
		}
	}
	ApplyWatchLayers();
	PushWatchState();
	if (bWasReplay && CurrentScreen != EApexScreen::Replays)
	{
		ShowScreen(EApexScreen::Replays);
	}
	ApexUiAudio::Play(this, EApexUiSound::Back);
	RequestFocusDefault();
	UE_LOG(LogApexSim, Log, TEXT("Stopped watching"));
}

void UApexRootWidget::ApplyWatchLayers()
{
	const bool bBackdrop = WatchKind == EWatchKind::Backdrop;
	if (!bRaceViewActive)
	{
		// Over a watched backdrop the menu steps aside; the background stays
		// to cover the gaps between races (UpdateBackdrop). So does it for a
		// hotlap from the moment it is asked for, for the wait while the
		// server starts it and the circuit is built.
		if (ScreenHost)
		{
			ScreenHost->SetVisibility(bBackdrop || bHotlapWatchWanted ? ESlateVisibility::Collapsed : ESlateVisibility::Visible);
		}
		if (BackdropScrim && bBackdrop)
		{
			BackdropScrim->SetVisibility(ESlateVisibility::Collapsed);
		}
		if (Background && !bBackdrop)
		{
			Background->SetVisibility(ESlateVisibility::HitTestInvisible);
		}
		if (Hud)
		{
			const AApexRaceDirector* Director = AApexRaceDirector::Find(this);
			Hud->SetRaceActive(bBackdrop && Director && Director->IsDemoViewActive());
		}
	}
	AppliedBackdrop = -1.0f;
	if (Hud)
	{
		Hud->SetShown(!(IsWatching() && bWatchOverlayHidden) && !bGarageOpen);
	}
	ApplyDriveInput();
}

void UApexRootWidget::PushWatchState()
{
	UApexHudDataSubsystem* HudData = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexHudDataSubsystem>() : nullptr;
	if (!HudData)
	{
		return;
	}
	FString Source;
	if (WatchKind == EWatchKind::Live)
	{
		const UApexNetSubsystem* Net = GetGameInstance()->GetSubsystem<UApexNetSubsystem>();
		Source = Net && Net->IsHotlapWatch() ? TEXT("hotlap") : TEXT("live");
	}
	else if (WatchKind == EWatchKind::Backdrop)
	{
		const UApexDemoModeSubsystem* Demo = GetGameInstance()->GetSubsystem<UApexDemoModeSubsystem>();
		const EApexBackdropSource From = Demo ? Demo->GetSource() : EApexBackdropSource::None;
		Source = From == EApexBackdropSource::Replay ? TEXT("replay")
			: From == EApexBackdropSource::Showcase    ? TEXT("showcase")
			: From == EApexBackdropSource::LocalFile   ? TEXT("file")
													   : TEXT("demo");
	}
	HudData->SetWatchState(Source, ApexSpectate::TowerModeKey(WatchTower));
}

void UApexRootWidget::SetGamepadHints(bool bGamepad)
{
	if (UApexHudDataSubsystem* HudData = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexHudDataSubsystem>() : nullptr)
	{
		HudData->SetGamepadActive(bGamepad);
	}
}

void UApexRootWidget::UpdateWatch(float DeltaSeconds)
{
	ApplyWatchCommandLine();
	if (WatchKind != EWatchKind::Backdrop)
	{
		return;
	}
	AApexRaceDirector* Director = AApexRaceDirector::Find(this);
	const bool bUp = Director && Director->IsDemoViewActive();
	if (bUp != bWatchDemoUp)
	{
		bWatchDemoUp = bUp;
		if (bUp)
		{
			Director->SetDemoWorldVisible(true);
			Director->SetSpectating(true);
		}
		// A new race on, or the last one gone: the HUD starts over with it
		// (its minimap outline, its timing memory) or stands down meanwhile.
		if (Hud)
		{
			Hud->SetRaceActive(false);
			Hud->SetRaceActive(bUp);
		}
	}
	if (bUp)
	{
		Director->SetDemoWorldVisible(true);
		WatchIdleSeconds = 0.0f;
	}
	else
	{
		WatchIdleSeconds += DeltaSeconds;
		const UApexDemoModeSubsystem* Demo = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexDemoModeSubsystem>() : nullptr;
		if (WatchIdleSeconds > WatchGiveUpSeconds || (Demo && !Demo->IsDemoExpected() && WatchIdleSeconds > 3.0f))
		{
			ShowToast(TEXT("No race to watch: back to the menu"), true);
			StopWatching();
			return;
		}
	}
	// The source changes as the backdrop moves from file to showcase.
	PushWatchState();
}

bool UApexRootWidget::HandleWatchKey(const FKeyEvent& InKeyEvent)
{
	const ApexSpectate::FCommand Command = ApexSpectate::CommandFor(InKeyEvent.GetKey());
	if (Command.Action == ApexSpectate::EAction::None)
	{
		return false;
	}
	RunWatchCommand(Command);
	return true;
}

void UApexRootWidget::RunWatchCommand(const ApexSpectate::FCommand& Command)
{
	if (!IsWatching())
	{
		return;
	}
	using ApexSpectate::EAction;
	AApexRaceDirector* Director = AApexRaceDirector::Find(this);
	const bool bLive = Director && Director->IsSpectating();

	// A watched hotlap has one car on the road: the keys that step through a
	// field choose what is lapped instead (the pad's auto, tower and next-race
	// buttons stand in for the weather, the hour and the circuit).
	if (bHotlapWatchWanted)
	{
		switch (Command.Action)
		{
		case EAction::PreviousCar:    ChangeHotlapWatch(EHotlapChoice::Car, -1); return;
		case EAction::NextCar:        ChangeHotlapWatch(EHotlapChoice::Car, 1); return;
		case EAction::Weather:
		case EAction::Auto:           ChangeHotlapWatch(EHotlapChoice::Weather, 1); return;
		case EAction::TimeEarlier:    ChangeHotlapWatch(EHotlapChoice::TimeOfDay, -1); return;
		case EAction::TimeLater:
		case EAction::Tower:          ChangeHotlapWatch(EHotlapChoice::TimeOfDay, 1); return;
		case EAction::PreviousTrack:  ChangeHotlapWatch(EHotlapChoice::Track, -1); return;
		case EAction::NextTrack:
		case EAction::NextRace:       ChangeHotlapWatch(EHotlapChoice::Track, 1); return;
		case EAction::Position:       return;
		default:                      break;
		}
	}

	switch (Command.Action)
	{
	case EAction::PreviousCar:
	case EAction::NextCar:
		if (bLive)
		{
			Director->StepFocus(Command.Action == EAction::NextCar ? 1 : -1);
			ApexUiAudio::Play(this, EApexUiSound::Move);
		}
		break;

	case EAction::Position:
		if (bLive)
		{
			const bool bThere = Director->FocusPosition(Command.Position);
			ApexUiAudio::Play(this, bThere ? EApexUiSound::Move : EApexUiSound::Denied);
		}
		break;

	case EAction::Camera:
		if (bLive)
		{
			Director->CycleSpectatorCamera();
			ApexUiAudio::Play(this, EApexUiSound::Adjust);
		}
		break;

	case EAction::Auto:
		if (bLive)
		{
			Director->SetSpectatorAuto(!Director->IsSpectatorAuto());
			ApexUiAudio::Play(this, EApexUiSound::Adjust);
		}
		break;

	case EAction::Tower:
		WatchTower = ApexSpectate::NextTowerMode(WatchTower);
		PushWatchState();
		ApexUiAudio::Play(this, EApexUiSound::Adjust);
		break;

	case EAction::Overlay:
		bWatchOverlayHidden = !bWatchOverlayHidden;
		ApplyWatchLayers();
		ApexUiAudio::Play(this, EApexUiSound::Adjust);
		break;

	case EAction::PlayPause:
	case EAction::SeekBack:
	case EAction::SeekForward:
	case EAction::Slower:
	case EAction::Faster:
	{
		UApexSpectatorSubsystem* Spectator = GetGameInstance()->GetSubsystem<UApexSpectatorSubsystem>();
		if (!IsWatchingReplay() || !Spectator)
		{
			break;
		}
		if (Command.Action == EAction::PlayPause)
		{
			// At the end, play starts the replay over.
			if (Spectator->IsAtEnd())
			{
				Spectator->SeekTo(0.0);
				Spectator->SetPaused(false);
			}
			else
			{
				Spectator->SetPaused(!Spectator->IsPaused());
			}
		}
		else if (Command.Action == EAction::SeekBack || Command.Action == EAction::SeekForward)
		{
			constexpr double SeekStepSeconds = 10.0;
			Spectator->SeekTo(Spectator->GetPlaybackSeconds() + (Command.Action == EAction::SeekBack ? -SeekStepSeconds : SeekStepSeconds));
		}
		else
		{
			Spectator->SetPlaybackRate(ApexSpectate::StepPlaybackRate(Spectator->GetPlaybackRate(), Command.Action == EAction::Faster ? 1 : -1));
		}
		ApexUiAudio::Play(this, EApexUiSound::Adjust);
		break;
	}

	case EAction::NextRace:
		if (IsWatchingReplay())
		{
			ShowToast(TEXT("A replay: leave it to pick another"), true);
		}
		else if (WatchKind == EWatchKind::Backdrop)
		{
			if (UApexSpectatorSubsystem* Spectator = GetGameInstance()->GetSubsystem<UApexSpectatorSubsystem>())
			{
				Spectator->RequestNext();
			}
			ShowToast(TEXT("Next race coming up"));
		}
		else
		{
			ShowToast(TEXT("A live session: leave it to watch another"), true);
		}
		break;

	case EAction::Leave:
		StopWatching();
		break;

	default:
		break;
	}
}

void UApexRootWidget::ApplyWatchCommandLine()
{
	// -ApexWatchReplay=<file|latest>: play a saved replay as soon as the shell
	// is up, for an unattended run of the replay view.
	FString ReplayArg;
	if (!bWatchReplayFromCommandLine && FParse::Value(FCommandLine::Get(), TEXT("ApexWatchReplay="), ReplayArg)
		&& AApexRaceDirector::Find(this) && !IsWatching())
	{
		bWatchReplayFromCommandLine = true;
		if (ReplayArg.Equals(TEXT("latest"), ESearchCase::IgnoreCase))
		{
			const TArray<FApexReplayInfo> All = UApexReplayRecorder::ListReplays();
			const FApexReplayInfo* Newest = nullptr;
			for (const FApexReplayInfo& Info : All)
			{
				Newest = !Newest || Info.When > Newest->When ? &Info : Newest;
			}
			ReplayArg = Newest ? Newest->Path : FString();
		}
		UE_LOG(LogApexSim, Log, TEXT("-ApexWatchReplay: %s"), ReplayArg.IsEmpty() ? TEXT("no replay on disk") : *ReplayArg);
		if (!ReplayArg.IsEmpty())
		{
			WatchReplay(ReplayArg);
		}
	}

	// -ApexWatch: watch the backdrop as soon as it is up, for a screenshot
	// run (-ApexWatchSession: a live session, HandleLobbyStateForAutoRace);
	// -ApexWatchCamera=tv|chase|onboard, -ApexWatchTower=interval|gap|last|
	// best|tyres and -ApexWatchCar=<position> set the view once a car is on
	// screen, -ApexWatchHideHud the overlay.
	const bool bWatchBackdrop = FParse::Param(FCommandLine::Get(), TEXT("ApexWatch"));
	if (bWatchCommandLineApplied || !(bWatchBackdrop || FParse::Param(FCommandLine::Get(), TEXT("ApexWatchSession"))))
	{
		return;
	}
	AApexRaceDirector* Director = AApexRaceDirector::Find(this);
	if (bWatchBackdrop && !bWatchFromCommandLine)
	{
		if (!Director || !Director->IsDemoViewActive())
		{
			return;
		}
		bWatchFromCommandLine = WatchBackdrop();
		return;
	}
	if (!Director || !Director->IsSpectating() || Director->GetFocusCarIndex() == INDEX_NONE)
	{
		return;
	}
	bWatchCommandLineApplied = true;

	FString Value;
	if (FParse::Value(FCommandLine::Get(), TEXT("ApexWatchTower="), Value))
	{
		for (int32 Mode = 0; Mode < static_cast<int32>(ApexSpectate::ETowerMode::Count); ++Mode)
		{
			if (Value.Equals(ApexSpectate::TowerModeKey(static_cast<ApexSpectate::ETowerMode>(Mode)), ESearchCase::IgnoreCase))
			{
				WatchTower = static_cast<ApexSpectate::ETowerMode>(Mode);
			}
		}
		PushWatchState();
	}
	int32 Position = 0;
	if (FParse::Value(FCommandLine::Get(), TEXT("ApexWatchCar="), Position))
	{
		Director->FocusPosition(Position);
	}
	if (FParse::Value(FCommandLine::Get(), TEXT("ApexWatchCamera="), Value))
	{
		for (int32 Camera = 0; Camera < static_cast<int32>(ApexSpectate::ECamera::Count); ++Camera)
		{
			if (Value.Equals(ApexSpectate::CameraName(static_cast<ApexSpectate::ECamera>(Camera)), ESearchCase::IgnoreCase))
			{
				Director->SetSpectatorCamera(static_cast<ApexSpectate::ECamera>(Camera));
			}
		}
	}
	if (FParse::Param(FCommandLine::Get(), TEXT("ApexWatchHideHud")))
	{
		bWatchOverlayHidden = true;
		ApplyWatchLayers();
	}
}

bool UApexRootWidget::WatchReplay(const FString& Path)
{
	UApexDemoModeSubsystem* Demo = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexDemoModeSubsystem>() : nullptr;
	if (!Demo || IsWatching() || bRaceViewActive)
	{
		return false;
	}
	FString Error;
	if (!Demo->PlayReplay(Path, Error))
	{
		ShowToast(FString::Printf(TEXT("Cannot play this replay: %s"), *Error), true);
		return false;
	}
	if (!WatchBackdrop())
	{
		Demo->StopReplay();
		return false;
	}
	bWatchingReplay = true;
	PushWatchState();
	return true;
}

void UApexRootWidget::SaveReplay()
{
	UApexReplayRecorder* Recorder = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexReplayRecorder>() : nullptr;
	FString Path;
	FString Error;
	if (Recorder && Recorder->SaveReplay(Path, Error))
	{
		ShowToast(FString::Printf(TEXT("Replay saved: %s"), *FPaths::GetBaseFilename(Path)));
	}
	else
	{
		ShowToast(FString::Printf(TEXT("No replay saved: %s"), Recorder ? *Error : TEXT("no recorder")), true);
	}
}

// ---------------------------------------------------------------------------
// Watching a hotlap
// ---------------------------------------------------------------------------

namespace
{
	/** A circuit or a car the hotlap can be watched on: its id and the name on its card. */
	struct FHotlapOption
	{
		FString Id;
		FString Name;
		/** A circuit's YAML base name (its export's stem). */
		FString Stem;
	};

	void SortByName(TArray<FHotlapOption>& Options)
	{
		Options.StableSort([](const FHotlapOption& A, const FHotlapOption& B)
		{
			return A.Name.Compare(B.Name, ESearchCase::IgnoreCase) < 0;
		});
	}

	/** The circuits the server has and this machine can draw (an export is installed), by name. */
	TArray<FHotlapOption> HotlapTracks(const UApexNetSubsystem& Net, const UApexMenuFlowSubsystem& Flow, const UApexTrackContentSubsystem* Content)
	{
		TArray<FHotlapOption> Out;
		for (const FApexTrackConfigSummary& Track : Net.GetCachedLobbyState().TrackConfigs)
		{
			FApexTrackCatalogRow Row;
			if (!Flow.GetTrackCatalogRow(Track.Id, Row) || Row.YamlBaseName.IsEmpty()
				|| (Content && !Content->HasTrack(Row.YamlBaseName)))
			{
				continue;
			}
			Out.Add({Track.Id, Row.DisplayName.IsEmpty() ? Track.Name : Row.DisplayName, Row.YamlBaseName});
		}
		SortByName(Out);
		return Out;
	}

	/** Every car the server has, by name. */
	TArray<FHotlapOption> HotlapCars(const UApexNetSubsystem& Net, const UApexMenuFlowSubsystem& Flow)
	{
		TArray<FHotlapOption> Out;
		for (const FApexCarConfigSummary& Car : Net.GetCachedLobbyState().CarConfigs)
		{
			FApexCarCatalogRow Row;
			const bool bRow = Flow.GetCarCatalogRow(Car.Id, Row);
			Out.Add({Car.Id, bRow && !Row.DisplayName.IsEmpty() ? Row.DisplayName : Car.Name, FString()});
		}
		SortByName(Out);
		return Out;
	}

	/** `Wanted` if the list has it, else the list's first. */
	const FHotlapOption* PickOption(const TArray<FHotlapOption>& Options, const FString& Wanted)
	{
		const FHotlapOption* Found = Options.FindByPredicate([&Wanted](const FHotlapOption& Option) { return Option.Id == Wanted; });
		return Found ? Found : (Options.IsEmpty() ? nullptr : &Options[0]);
	}
}

bool UApexRootWidget::StartHotlapWatch()
{
	UApexNetSubsystem* Net = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexNetSubsystem>() : nullptr;
	UApexMenuFlowSubsystem* Flow = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexMenuFlowSubsystem>() : nullptr;
	if (!Net || !Flow || !Net->IsAuthenticated())
	{
		ShowToast(TEXT("Not connected to a server yet"), true);
		return false;
	}
	if (IsWatching() || bRaceViewActive || bHotlapWatchWanted)
	{
		return false;
	}

	const UApexTrackContentSubsystem* Content = GetGameInstance()->GetSubsystem<UApexTrackContentSubsystem>();
	const TArray<FHotlapOption> Tracks = HotlapTracks(*Net, *Flow, Content);
	const TArray<FHotlapOption> Cars = HotlapCars(*Net, *Flow);
	if (Tracks.IsEmpty() || Cars.IsEmpty())
	{
		ShowToast(Cars.IsEmpty() ? TEXT("The server has no cars yet") : TEXT("No circuit to watch: none has an export installed"), true);
		return false;
	}
	HotlapTrackId = PickOption(Tracks, Flow->GetPendingTrackId())->Id;
	HotlapCarId = PickOption(Cars, Flow->GetPendingCarId())->Id;
	if (!bHotlapConditionsKnown)
	{
		// The first watch is under the sky of the last session set up; after
		// it, the one last watched.
		HotlapConditions = FApexSessionConditions();
		HotlapConditions.Weather = Flow->CreateConditions.Weather;
		HotlapConditions.TimeOfDayMinutes = Flow->CreateConditions.TimeOfDayMinutes;
		bHotlapConditionsKnown = true;
	}

	bHotlapWatchWanted = true;
	WatchTower = ApexSpectate::ETowerMode::Interval;
	bWatchOverlayHidden = false;
	ApplyWatchLayers();
	SendHotlapWatchRequest();
	ApexUiAudio::Play(this, EApexUiSound::Accept);
	return true;
}

void UApexRootWidget::SendHotlapWatchRequest()
{
	UApexNetSubsystem* Net = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexNetSubsystem>() : nullptr;
	const UApexMenuFlowSubsystem* Flow = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexMenuFlowSubsystem>() : nullptr;
	if (!Net || !Flow)
	{
		return;
	}
	FApexTrackCatalogRow Row;
	Flow->GetTrackCatalogRow(HotlapTrackId, Row);
	UE_LOG(LogApexSim, Log, TEXT("Hotlap watch: track '%s', car '%s', %s"),
		*Row.YamlBaseName, *HotlapCarId, *HotlapConditions.Describe());
	// SelectCar before CreateSession, the order the rest of the shell uses.
	Net->SelectCar(HotlapCarId);
	Net->CreateHotlapWatch(HotlapTrackId, Row.DisplayName, Row.YamlBaseName, HotlapConditions);
	bHotlapJoinPending = true;
	HotlapRequestedAt = FPlatformTime::Seconds();
	HotlapRestartAt = -1.0;
}

void UApexRootWidget::ChangeHotlapWatch(EHotlapChoice Choice, int32 Direction)
{
	UApexNetSubsystem* Net = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexNetSubsystem>() : nullptr;
	const UApexMenuFlowSubsystem* Flow = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexMenuFlowSubsystem>() : nullptr;
	if (!bHotlapWatchWanted || !Net || !Flow)
	{
		return;
	}

	FString Message;
	switch (Choice)
	{
	case EHotlapChoice::Car:
	{
		const TArray<FHotlapOption> Cars = HotlapCars(*Net, *Flow);
		if (Cars.Num() < 2)
		{
			ApexUiAudio::Play(this, EApexUiSound::Denied);
			return;
		}
		const int32 At = Cars.IndexOfByPredicate([this](const FHotlapOption& Option) { return Option.Id == HotlapCarId; });
		const FHotlapOption& Next = Cars[ApexSpectate::StepIndex(Cars.Num(), At == INDEX_NONE ? 0 : At, Direction)];
		HotlapCarId = Next.Id;
		Message = FString::Printf(TEXT("Car: %s"), *Next.Name);
		break;
	}
	case EHotlapChoice::Track:
	{
		const TArray<FHotlapOption> Tracks = HotlapTracks(*Net, *Flow, GetGameInstance()->GetSubsystem<UApexTrackContentSubsystem>());
		if (Tracks.Num() < 2)
		{
			ApexUiAudio::Play(this, EApexUiSound::Denied);
			return;
		}
		const int32 At = Tracks.IndexOfByPredicate([this](const FHotlapOption& Option) { return Option.Id == HotlapTrackId; });
		const FHotlapOption& Next = Tracks[ApexSpectate::StepIndex(Tracks.Num(), At == INDEX_NONE ? 0 : At, Direction)];
		HotlapTrackId = Next.Id;
		Message = FString::Printf(TEXT("Circuit: %s"), *Next.Name);
		break;
	}
	case EHotlapChoice::Weather:
		HotlapConditions.Weather = ApexSpectate::StepWeather(HotlapConditions.Weather, Direction);
		Message = FString::Printf(TEXT("Weather: %s"), *FApexSessionConditions::WeatherLabel(HotlapConditions.Weather));
		break;
	case EHotlapChoice::TimeOfDay:
		HotlapConditions.TimeOfDayMinutes = ApexSpectate::StepTimeOfDay(HotlapConditions.TimeOfDayMinutes, Direction);
		Message = FString::Printf(TEXT("Time of day: %s"), *HotlapConditions.ClockText());
		break;
	}
	ShowToast(Message);
	ApexUiAudio::Play(this, EApexUiSound::Adjust);

	// A few presses in a row make one new lap, not one each.
	constexpr double RestartAfterSeconds = 0.8;
	HotlapRestartAt = FPlatformTime::Seconds() + RestartAfterSeconds;
}

void UApexRootWidget::RestartHotlapWatch()
{
	HotlapRestartAt = -1.0;
	// The old lap ends the way a race does (the server replaces its session
	// without a word, so nothing else will say so); the shell stays out of
	// sight until the new one is up (ApplyWatchLayers).
	SetRaceViewActive(false);
	ApplyWatchLayers();
	SendHotlapWatchRequest();
}

void UApexRootWidget::EndHotlapWatch()
{
	bHotlapWatchWanted = false;
	bHotlapJoinPending = false;
	HotlapRestartAt = -1.0;
	// The watch chose its own car; the player's pick is the garage's again.
	UApexNetSubsystem* Net = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexNetSubsystem>() : nullptr;
	const UApexMenuFlowSubsystem* Flow = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexMenuFlowSubsystem>() : nullptr;
	if (Net && Flow && Net->IsAuthenticated() && Flow->HasPendingCar())
	{
		Net->SelectCar(Flow->GetPendingCarId());
	}
	if (!bRaceViewActive)
	{
		ApplyWatchLayers();
	}
}

void UApexRootWidget::UpdateHotlapWatch()
{
	ApplyHotlapWatchCommandLine();
	if (!bHotlapWatchWanted)
	{
		return;
	}
	const double Now = FPlatformTime::Seconds();
	if (HotlapRestartAt >= 0.0 && Now >= HotlapRestartAt)
	{
		RestartHotlapWatch();
		return;
	}
	// An answer that never comes: back to the menu rather than a blank screen.
	constexpr double GiveUpAfterSeconds = 15.0;
	if (bHotlapJoinPending && HotlapRequestedAt >= 0.0 && Now - HotlapRequestedAt > GiveUpAfterSeconds)
	{
		UE_LOG(LogApexSim, Warning, TEXT("Hotlap watch: the server did not start it within %.0f s"), GiveUpAfterSeconds);
		ShowToast(TEXT("The server did not start the hotlap"), true);
		if (UApexNetSubsystem* Net = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexNetSubsystem>() : nullptr)
		{
			Net->LeaveSession();
		}
		SetRaceViewActive(false);
		EndHotlapWatch();
		BackStack.Reset();
		ActivateScreen(EApexScreen::MainMenu);
	}
}

void UApexRootWidget::ApplyHotlapWatchCommandLine()
{
	// -ApexWatchHotlap, with -ApexTrack=<stem|name>, -ApexCar=<name>,
	// -ApexWeather=<sunny|cloudy|overcast|lightrain|heavyrain> and
	// -ApexTimeOfDay=HH:MM: watch a hotlap as soon as the lobby is known, for
	// an unattended run.
	if (bHotlapCommandLineApplied || !FParse::Param(FCommandLine::Get(), TEXT("ApexWatchHotlap")))
	{
		return;
	}
	UApexNetSubsystem* Net = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexNetSubsystem>() : nullptr;
	UApexMenuFlowSubsystem* Flow = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexMenuFlowSubsystem>() : nullptr;
	if (!Net || !Flow || !Net->IsAuthenticated() || !Net->IsUdpReady() || !AApexRaceDirector::Find(this)
		|| Net->GetCachedLobbyState().TrackConfigs.IsEmpty() || Net->GetCachedLobbyState().CarConfigs.IsEmpty())
	{
		return;
	}
	bHotlapCommandLineApplied = true;

	const UApexTrackContentSubsystem* Content = GetGameInstance()->GetSubsystem<UApexTrackContentSubsystem>();
	FString Wanted;
	if (FParse::Value(FCommandLine::Get(), TEXT("ApexTrack="), Wanted))
	{
		for (const FHotlapOption& Track : HotlapTracks(*Net, *Flow, Content))
		{
			if (Track.Stem.Equals(Wanted, ESearchCase::IgnoreCase) || Track.Name.Contains(Wanted))
			{
				Flow->SetPendingTrack(Track.Id);
				break;
			}
		}
	}
	if (FParse::Value(FCommandLine::Get(), TEXT("ApexCar="), Wanted))
	{
		for (const FHotlapOption& Car : HotlapCars(*Net, *Flow))
		{
			if (Car.Name.Contains(Wanted))
			{
				Flow->SetPendingCar(Car.Id);
				break;
			}
		}
	}
	FString Value;
	if (FParse::Value(FCommandLine::Get(), TEXT("ApexWeather="), Value))
	{
		const FString Key = Value.TrimStartAndEnd().ToLower().Replace(TEXT("_"), TEXT("")).Replace(TEXT(" "), TEXT(""));
		Flow->CreateConditions.Weather = Key == TEXT("cloudy") ? EApexWeather::Cloudy
			: Key == TEXT("overcast") ? EApexWeather::Overcast
			: (Key == TEXT("lightrain") || Key == TEXT("rain")) ? EApexWeather::LightRain
			: Key == TEXT("heavyrain") ? EApexWeather::HeavyRain
			: EApexWeather::Sunny;
	}
	if (FParse::Value(FCommandLine::Get(), TEXT("ApexTimeOfDay="), Value))
	{
		FString HourText, MinuteText;
		if (!Value.Split(TEXT(":"), &HourText, &MinuteText))
		{
			HourText = Value;
			MinuteText = TEXT("0");
		}
		if (HourText.IsNumeric() && MinuteText.IsNumeric())
		{
			Flow->CreateConditions.TimeOfDayMinutes =
				(FCString::Atoi(*HourText) * 60 + FCString::Atoi(*MinuteText)) % FApexSessionConditions::MinutesPerDay;
		}
	}
	UE_LOG(LogApexSim, Log, TEXT("-ApexWatchHotlap: starting"));
	StartHotlapWatch();
}

// ---------------------------------------------------------------------------
// Track guide
// ---------------------------------------------------------------------------

bool UApexRootWidget::OpenTrackGuide(const FString& Stem)
{
	UApexTrackGuideSubsystem* Guide = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexTrackGuideSubsystem>() : nullptr;
	if (!Guide)
	{
		return false;
	}
	if (bRaceViewActive || IsWatchingLive())
	{
		ShowToast(TEXT("Leave the session to open a track guide"), true);
		return false;
	}
	FString Error;
	if (!Guide->Open(Stem, FString(), Error))
	{
		ShowToast(FString::Printf(TEXT("No track guide: %s"), *Error), true);
		return false;
	}
	return true;
}

void UApexRootWidget::HandleGuideActiveChanged(bool bActive)
{
	if (bGuideLayers == bActive)
	{
		return;
	}
	bGuideLayers = bActive;
	if (bActive)
	{
		if (IsWatching())
		{
			StopWatching();
		}
		if (SettingsOverlay && SettingsOverlay->IsOpen())
		{
			SettingsOverlay->Close();
		}
		if (bPauseMenuOpen)
		{
			SetPaused(false);
		}
	}
	if (!bRaceViewActive)
	{
		// The screen it was opened from stays current underneath, so closing
		// lands back on it (the track picker, as a rule).
		if (ScreenHost)
		{
			ScreenHost->SetVisibility(bActive ? ESlateVisibility::Collapsed : ESlateVisibility::Visible);
		}
		if (Background)
		{
			Background->SetVisibility(ESlateVisibility::HitTestInvisible);
			Background->SetBrushColor(FLinearColor::White);
		}
		if (BackdropScrim)
		{
			BackdropScrim->SetVisibility(ESlateVisibility::Collapsed);
		}
	}
	if (GuidePanel)
	{
		GuidePanel->SetActive(bActive);
	}
	AppliedBackdrop = -1.0f;
	ApplyDriveInput();
	RequestFocusDefault();
	UE_LOG(LogApexSim, Log, TEXT("Track guide layer %s"), bActive ? TEXT("up") : TEXT("down"));
}

void UApexRootWidget::HandleGuideFailed(const FString& Why)
{
	ShowToast(FString::Printf(TEXT("Track guide: %s"), *Why), true);
}

bool UApexRootWidget::HandleGuideKey(const FKeyEvent& InKeyEvent)
{
	UApexTrackGuideSubsystem* Guide = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexTrackGuideSubsystem>() : nullptr;
	const ApexGuide::FCommand Command = ApexGuide::CommandFor(InKeyEvent.GetKey(), InKeyEvent.IsShiftDown());
	if (!Guide || Command.Action == ApexGuide::EAction::None)
	{
		return false;
	}
	Guide->Run(Command);
	switch (Command.Action)
	{
	case ApexGuide::EAction::Leave:
		ApexUiAudio::Play(this, EApexUiSound::Back);
		break;
	case ApexGuide::EAction::Previous:
	case ApexGuide::EAction::Next:
	case ApexGuide::EAction::Corner:
	case ApexGuide::EAction::Overview:
		ApexUiAudio::Play(this, EApexUiSound::Move);
		break;
	default:
		ApexUiAudio::Play(this, EApexUiSound::Adjust);
		break;
	}
	return true;
}
