#pragma once

#include "CoreMinimal.h"
#include "ApexMenuFlowSubsystem.h"
#include "ApexProtocolTypes.h"
#include "Blueprint/UserWidget.h"
#include "Race/ApexSpectatorView.h"

#include "ApexRootWidget.generated.h"

class FApexMenuInputProcessor;
class FWeakWidgetPath;
class FWidgetPath;
class SWidget;
struct FFocusEvent;
class UApexHotlapWidget;
class UApexTrackGuideWidget;
class UApexHudEditorWidget;
class UApexHudWidget;
class UApexPauseMenuWidget;
class UApexScreenWidget;
class UApexSettingsWidget;
class UApexToastWidget;
class UBorder;
class UImage;
class UTexture2D;
class UWidgetSwitcher;
enum class EApexPauseAction : uint8;
enum class EApexHotlapAction : uint8;
enum class EApexSettingsTab : uint8;

/**
 * The shell's frame: background, screen switcher, toast.
 *
 * Owns navigation. Screens ask it to move; it keeps a back stack so Escape and
 * every Back button behave the same way without each screen knowing where it
 * was reached from.
 *
 * The shell and its screens are built in C++ (NativeOnInitialized), so the
 * widget blueprint this is instantiated from contributes nothing but its class.
 * Screens that have not been redesigned yet are still loaded from their old
 * `.uasset` — see ResolveScreenClass.
 */
UCLASS()
class APEXSIM_API UApexRootWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	virtual void NativeOnInitialized() override;
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;
	virtual FReply NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent) override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

	/** Switches to a screen and pushes the current one onto the back stack. */
	UFUNCTION(BlueprintCallable, Category = "ApexSim|UI")
	void ShowScreen(EApexScreen Screen);

	/** Switches without recording history — used when returning somewhere. */
	UFUNCTION(BlueprintCallable, Category = "ApexSim|UI")
	void ReplaceScreen(EApexScreen Screen);

	UFUNCTION(BlueprintCallable, Category = "ApexSim|UI")
	void GoBack();

	/** True when GoBack would move: there is a screen on the back stack. */
	UFUNCTION(BlueprintPure, Category = "ApexSim|UI")
	bool CanGoBack() const { return BackStack.Num() > 0; }

	/** Shows a toast, with a cue: news or bad news. */
	UFUNCTION(BlueprintCallable, Category = "ApexSim|UI")
	void ShowToast(const FString& Message, bool bIsError = false);

	UFUNCTION(BlueprintPure, Category = "ApexSim|UI")
	EApexScreen GetCurrentScreen() const { return CurrentScreen; }

	/** True while Screen is the one on show and no race view covers it. */
	bool IsScreenActive(const UApexScreenWidget* Screen) const;

	/**
	 * Puts keyboard focus on whichever surface is in front: the settings
	 * overlay, the pause menu, or the current screen. Nothing while driving —
	 * the viewport owns input then.
	 */
	void FocusDefault();

	/**
	 * FocusDefault a tick from now. Focus set in the same frame as an input
	 * mode change or a screen switch is overwritten when the deferred Slate
	 * operations run; a tick later it sticks.
	 */
	void RequestFocusDefault();

	UFUNCTION(BlueprintPure, Category = "ApexSim|UI")
	bool IsRaceViewActive() const { return bRaceViewActive; }

	UFUNCTION(BlueprintPure, Category = "ApexSim|UI")
	bool IsPaused() const { return bPauseMenuOpen; }

	/** True while the hotlap garage card is up and owns the keys. */
	UFUNCTION(BlueprintPure, Category = "ApexSim|UI")
	bool IsGarageOpen() const { return bGarageOpen; }

	UFUNCTION(BlueprintPure, Category = "ApexSim|UI")
	bool IsSettingsOpen() const;

	/** True while the settings overlay is capturing a key for a binding. */
	bool IsSettingsListening() const;

	/** True while any race-side overlay owns input. */
	bool IsRaceOverlayOpen() const;

	/** True while the HUD editor is up: it owns the mouse and the keys. */
	bool IsHudEditorOpen() const;

	/**
	 * Lays the HUD out (UApexHudEditorWidget). From the settings overlay it
	 * steps aside and comes back when the editor closes; opened on its own
	 * (`apexsim.hud.Edit`, -ApexOpenHudEditor=N) it just takes over the
	 * screen. Driving input is off while it is up.
	 */
	UFUNCTION()
	void OpenHudEditor();

	/**
	 * Opens or closes the pause menu.
	 *
	 * Driving input goes with it: while the menu is up the controller is put
	 * back into UI-only input, which both zeroes the controls and stops a
	 * keypress meant for the menu reaching the car. The session itself keeps
	 * running — the server is authoritative and has no pause.
	 */
	void SetPaused(bool bPaused);

	/**
	 * Opens the settings overlay on a page. From the pause menu it is a step
	 * forward and closes back onto it; from a menu screen it closes back onto
	 * that screen.
	 */
	void OpenSettings(EApexSettingsTab Tab);

	/** The garage card as a setup editor over the menu (Garage > Manage car setups), outside any session. */
	void OpenSetupEditor();
	void CloseSetupEditor();

	// --- Watching a race -------------------------------------------------------

	/**
	 * Take the race playing behind the menu (a showcase, a local file or a
	 * demo session) full screen, with the spectator's controls and the HUD
	 * on the watched car (docs/game/spectator.md, "Watching a race"). False, with
	 * a toast, when there is nothing to watch. Between the backdrop's races
	 * the view waits for the next one.
	 */
	bool WatchBackdrop();

	/** Spectate a live session from the browser: JoinAsSpectator, then the race view without a car. */
	void WatchSession(const FString& SessionId);

	/**
	 * Play a saved replay (UApexReplayRecorder) in the watch view, with its
	 * transport (pause, seek, speed); leaving it comes back to the Replays
	 * screen. False, with a toast, when it cannot be played.
	 */
	bool WatchReplay(const FString& Path);
	bool IsWatchingReplay() const { return WatchKind == EWatchKind::Backdrop && bWatchingReplay; }

	/** Save the session being recorded as a replay, with a toast either way. */
	void SaveReplay();

	/** Back to the menu (the backdrop) or out of the session (live). */
	void StopWatching();

	bool IsWatching() const { return WatchKind != EWatchKind::None; }
	bool IsWatchingLive() const { return WatchKind == EWatchKind::Live; }

	/**
	 * A key while watching, from the input processor (ApexSpectate::CommandFor):
	 * which car, which camera, the tower, the overlay, the next race, out.
	 * True when the key meant something.
	 */
	bool HandleWatchKey(const FKeyEvent& InKeyEvent);
	/** The same, by action, for the console (`apexsim.watch`). */
	void RunWatchCommand(const ApexSpectate::FCommand& Command);

	// --- Watching a hotlap ------------------------------------------------------

	/**
	 * Main menu > Garage > Tracks > Watch hotlap: one AI car laps a circuit
	 * alone, on its racing line and at the speed its car can manage, worked
	 * out by the server when it is asked for (SessionKind::HotlapWatch). It
	 * starts on the pending circuit and car, under the weather and hour last
	 * watched, and shows a minimal HUD (`scene: hotlap_watch`): the lap and
	 * its sectors, the car's speed, gear and pedals, the corner it is at.
	 * The car (arrows), the weather (W), the hour ([ and ]) and the circuit
	 * (Page Up and Down) can be changed while watching: each change starts
	 * the lap over for the new choice. False, with a toast, when it cannot
	 * be started.
	 */
	bool StartHotlapWatch();

	/** A hotlap is being watched (or asked for). */
	bool IsWatchingHotlap() const { return bHotlapWatchWanted; }

	/** What a key changes about the hotlap being watched. */
	enum class EHotlapChoice : uint8
	{
		Car,
		Track,
		Weather,
		TimeOfDay,
	};

	/** Step one of the hotlap's choices `Direction` places on (wrapping) and, after a moment, start the lap over for it. */
	void ChangeHotlapWatch(EHotlapChoice Choice, int32 Direction);

	// --- Track guide ------------------------------------------------------------

	/**
	 * Open a circuit's track guide (UApexTrackGuideSubsystem, docs/content/track-guide.md)
	 * over the menu: the screens step aside for the world and the guide's
	 * layer; leaving it comes back to the screen it was opened from. False,
	 * with a toast, when the circuit has none or it cannot be played.
	 */
	bool OpenTrackGuide(const FString& Stem);

	/** A guide is open (loading or playing): it owns the screen and the keys. */
	bool IsGuideActive() const { return bGuideLayers; }

	/** A key while the guide is open, from the input processor (ApexGuide::CommandFor). True when it meant something. */
	bool HandleGuideKey(const FKeyEvent& InKeyEvent);

	/** The pad is in use: the HUD's key hints show its buttons. */
	void SetGamepadHints(bool bGamepad);

	/** The screen the car picker should return to once a car is confirmed. */
	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|UI")
	EApexScreen ScreenAfterCarSelect = EApexScreen::MainMenu;

	/** The screen the track picker should return to. */
	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|UI")
	EApexScreen ScreenAfterTrackSelect = EApexScreen::MainMenu;

protected:
	UPROPERTY(Transient, BlueprintReadOnly, Category = "ApexSim|UI")
	TObjectPtr<UWidgetSwitcher> ScreenHost;

	UPROPERTY(Transient, BlueprintReadOnly, Category = "ApexSim|UI")
	TObjectPtr<UBorder> Background;

	UPROPERTY(Transient, BlueprintReadOnly, Category = "ApexSim|UI")
	TObjectPtr<UApexToastWidget> ToastPanel;

	/**
	 * Darkening over the demo race, heaviest on the left where every screen
	 * puts its heading and its first column. Shown only while the demo is.
	 */
	UPROPERTY(Transient, BlueprintReadOnly, Category = "ApexSim|UI")
	TObjectPtr<UImage> BackdropScrim;

	/**
	 * The race layers, stacked above the menu: the HUD, then the pause menu,
	 * then settings.
	 *
	 * They live on the shell rather than in the race director because they are
	 * widgets and the director is an actor — and because leaving a session or
	 * quitting is navigation, which is the shell's job.
	 */
	UPROPERTY(Transient, BlueprintReadOnly, Category = "ApexSim|UI")
	TObjectPtr<UApexHudWidget> Hud;

	UPROPERTY(Transient, BlueprintReadOnly, Category = "ApexSim|UI")
	TObjectPtr<UApexPauseMenuWidget> PauseMenu;

	/** The hotlap layer: garage card, timing sheet, replay strip. Between the HUD and the pause menu. */
	UPROPERTY(Transient, BlueprintReadOnly, Category = "ApexSim|UI")
	TObjectPtr<UApexHotlapWidget> HotlapPanel;

	/** The track guide's cards and controls, over the world while a guide is open. */
	UPROPERTY(Transient, BlueprintReadOnly, Category = "ApexSim|UI")
	TObjectPtr<UApexTrackGuideWidget> GuidePanel;

	UPROPERTY(Transient, BlueprintReadOnly, Category = "ApexSim|UI")
	TObjectPtr<UApexSettingsWidget> SettingsOverlay;

	/** The HUD editor, over everything but the toasts. */
	UPROPERTY(Transient, BlueprintReadOnly, Category = "ApexSim|UI")
	TObjectPtr<UApexHudEditorWidget> HudEditor;

private:
	/** Builds the frame and fills the switcher, one screen per EApexScreen. */
	void BuildShell();

	/**
	 * The widget class for a screen: the redesigned C++ class where one exists,
	 * otherwise the legacy widget blueprint. Every screen has to resolve to
	 * something, because the switcher is indexed by EApexScreen.
	 */
	static UClass* ResolveScreenClass(EApexScreen Screen);

	UFUNCTION()
	void HandleDisconnected(const FString& Reason);

	UFUNCTION()
	void HandleServerError(int32 Code, const FString& Message);

	/** The track or car about to be raced was baked from a different file than the server's. */
	UFUNCTION()
	void HandleContentMismatch(const FString& Message);

	UFUNCTION()
	void HandleSessionJoined(const FString& SessionId, int32 GridPosition);

	UFUNCTION()
	void HandleSettingsChangedForDriverAids(EApexSettingsGroup Group);

	/** Tell the server which aids to run for this player (gearbox, steering, ABS, traction control). */
	void SendDriverAids();

	/** Whether the steering was on a wheel when the aids were last sent, so a rebind or a plug-in re-sends them. */
	bool bAidsSentForWheel = false;

	/** Tell the server the garage setup to simulate this player's car with. */
	void SendCarSetup();

	UFUNCTION()
	void HandleSessionLeft();

	UFUNCTION()
	void HandleGameModeChanged(EApexGameMode NewMode);

	UFUNCTION()
	void HandleSessionStateChanged(EApexSessionState NewState);

	/** Hides the menu and hands the view to the race director, or takes it back. */
	void SetRaceViewActive(bool bActive);

	/**
	 * Watches the race for the chequered flag. When the winner crosses the line
	 * everyone still racing is told; when the local car finishes, its race is
	 * over and the results screen takes over while the others come in. The
	 * session itself only ends once the whole field is in (or out of time).
	 */
	UFUNCTION()
	void HandleTelemetryForFinish(const FApexTelemetryFrame& Frame);

	/** Leaves the race view for the live results, a moment after the flag. */
	void ShowResultsAfterFinish(int32 Position);

	/** Forgets the flags seen, for a race that is about to start. */
	void ResetFinishWatch();

	UFUNCTION()
	void HandlePauseAction(EApexPauseAction Action);

	UFUNCTION()
	void HandleHotlapAction(EApexHotlapAction Action);

	UFUNCTION()
	void HandleHotlapWatch(int32 CarIndex);

	/** The local car's telemetry decides which side of the garage wall the hotlap view is on. */
	UFUNCTION()
	void HandleTelemetryForHotlap(const FApexTelemetryFrame& Frame);

	/** A new record with a trace: fetch it, so the ghost drives the lap just set. */
	UFUNCTION()
	void HandleLapRecordForGhost(const FApexLapRecord& Record);

	/**
	 * The garage card up or down. Up, the HUD hides, driving input stops (the
	 * car is frozen anyway) and the card takes the keys, as the pause menu
	 * does; down, the timing sheet stays beside the HUD.
	 */
	void SetGarageOpen(bool bOpen);

	UFUNCTION()
	void HandleSettingsClosed();

	UFUNCTION()
	void HandleHudEditorClosed();

	/** Drive input on or off for whatever is open now: the one rule SetPaused, the garage and the HUD editor share. */
	void ApplyDriveInput();

	/** True for any mode in which the server is simulating and sending telemetry. */
	static bool IsDrivingMode(EApexGameMode Mode);

	/**
	 * Creates and starts a session unattended, for `-ApexAutoRace`.
	 *
	 * Getting to moving cars otherwise takes half a dozen clicks, which makes
	 * the transport impossible to check in an automated run.
	 */
	void TryAutoRace(const FApexLobbyState& LobbyState);

	UFUNCTION()
	void HandleLobbyStateForAutoRace(const FApexLobbyState& LobbyState);

	UFUNCTION()
	void HandleUdpReady();

	/** Counts a freshly created session into its starting mode. */
	void StartRequestedSession();

	enum class EWatchKind : uint8
	{
		None,
		/** The menu's backdrop race, full screen. */
		Backdrop,
		/** A live session joined as a spectator. */
		Live,
	};
	EWatchKind WatchKind = EWatchKind::None;
	/** A WatchSession is waiting for its SessionJoined. */
	bool bWatchJoinPending = false;
	/** The backdrop being watched is a replay the player chose. */
	bool bWatchingReplay = false;
	ApexSpectate::ETowerMode WatchTower = ApexSpectate::ETowerMode::Interval;
	bool bWatchOverlayHidden = false;
	/** Whether the director's backdrop was up last frame, to notice the next race begin. */
	bool bWatchDemoUp = false;
	/** Seconds the backdrop watch has had no race to show; it gives up eventually. */
	float WatchIdleSeconds = 0.0f;

	/** The hotlap being watched: its circuit, car and sky. Valid while bHotlapWatchWanted. */
	bool bHotlapWatchWanted = false;
	FString HotlapTrackId;
	FString HotlapCarId;
	FApexSessionConditions HotlapConditions;
	/** The weather and hour are remembered from one watch to the next. */
	bool bHotlapConditionsKnown = false;
	/** Seconds on the clock at which the lap starts over for a changed choice; negative when none is waiting. */
	double HotlapRestartAt = -1.0;
	/** When the session was last asked for, to give up on an answer that never comes. */
	double HotlapRequestedAt = -1.0;
	/** The session was asked for and has not been joined. */
	bool bHotlapJoinPending = false;

	/** Ask the server for the hotlap as chosen (select the car, create the session). */
	void SendHotlapWatchRequest();
	/** Put the lap over for a changed choice: the old session goes, the view waits for the new. */
	void RestartHotlapWatch();
	/** Forget the hotlap being watched (its session is left or gone) and give the shell back. */
	void EndHotlapWatch();
	/** Keep a changed choice's restart and the join's answer on the clock. */
	void UpdateHotlapWatch();
	/** `-ApexWatchHotlap[=<Stem>]` and its options, for an unattended run. */
	void ApplyHotlapWatchCommandLine();
	bool bHotlapCommandLineApplied = false;

	/** Keep a watched backdrop on screen and its HUD in step with its races. */
	void UpdateWatch(float DeltaSeconds);
	/** What the HUD data needs of the watch view. */
	void PushWatchState();
	/** The shell's own widgets for watching or not: the menu out of the way, the HUD up. */
	void ApplyWatchLayers();
	/** `-ApexWatch` and its options, for an unattended run. */
	void ApplyWatchCommandLine();
	bool bWatchFromCommandLine = false;
	bool bWatchCommandLineApplied = false;
	bool bWatchSessionFromCommandLine = false;
	bool bWatchReplayFromCommandLine = false;

	bool bRaceViewActive = false;
	bool bPauseMenuOpen = false;
	bool bGarageOpen = false;
	/** The session's mode is Hotlap; the hotlap layer is live. */
	bool bHotlapSession = false;
	/** The winner's flag has been announced for this race. */
	bool bWinnerAnnounced = false;
	/** The local car has taken the flag in this race. */
	bool bLocalFinished = false;
	FTimerHandle ResultsAfterFinishTimer;
	/** A one-click start joined before the telemetry channel was up. */
	bool bStartWhenUdpReady = false;
	bool bAutoRaceRequested = false;
	bool bAutoRaceSessionRequested = false;
	/** -ApexNoStart: create the session but leave it in the lobby. */
	bool bAutoRaceNoStart = false;
	int32 AutoRaceAiCount = 3;
	/** -ApexCountdown=N: seconds of countdown before the auto race goes green. */
	int32 AutoRaceCountdown = 3;
	int32 AutoRaceLaps = 5;
	/** -ApexTrack=<name>: substring-matches a lobby track, overriding the profile. */
	FString AutoRaceTrack;
	/** -ApexCar=<name>: substring-matches a lobby car instead of taking the first. */
	FString AutoRaceCar;
	EApexGameMode AutoRaceMode = EApexGameMode::Race;

	/**
	 * Let the menu's demo race show through: the page backgrounds fade by the
	 * race director's demo opacity, times a gate that closes over screens
	 * that do not want it. The director's world is hidden only once the gate
	 * has closed, so leaving for the car picker fades rather than cuts.
	 */
	void UpdateBackdrop(float DeltaSeconds);

	/** A left-to-right fade of the palette's background, built once in memory. */
	UTexture2D* MakeScrimTexture();

	UPROPERTY(Transient)
	TObjectPtr<UTexture2D> ScrimTexture;

	/** 0 over a screen that refuses the demo, 1 over one that takes it; eased. */
	float BackdropGate = 0.0f;
	/** What was last pushed to the widgets, so an unchanged value costs nothing. */
	float AppliedBackdrop = -1.0f;
	EApexScreen AppliedBackdropScreen = EApexScreen::MainMenu;

	/** Notifies the outgoing and incoming screens, then flips the switcher. */
	void ActivateScreen(EApexScreen Screen);
	UApexScreenWidget* GetScreenWidget(EApexScreen Screen) const;

	EApexScreen CurrentScreen = EApexScreen::MainMenu;
	TArray<EApexScreen> BackStack;

	/** Focus recovery and the pause key; see the class. Registered for the widget's lifetime. */
	TSharedPtr<FApexMenuInputProcessor> InputProcessor;

	/**
	 * Every focus change Slate makes, so a move the player made can be heard.
	 *
	 * Here rather than on each control because the controls are not all ours:
	 * a slider or a dropdown in settings is Slate's own widget, and focus
	 * landing on one is as much a move as focus landing on a button.
	 */
	void HandleFocusChanging(
		const FFocusEvent& Event,
		const FWeakWidgetPath& OldPath,
		const TSharedPtr<SWidget>& OldWidget,
		const FWidgetPath& NewPath,
		const TSharedPtr<SWidget>& NewWidget);

	FDelegateHandle FocusChangingHandle;

	/** The guide opened or closed: the menu out of the way, or back. */
	void HandleGuideActiveChanged(bool bActive);
	void HandleGuideFailed(const FString& Why);
	FDelegateHandle GuideActiveHandle;
	FDelegateHandle GuideFailedHandle;
	/** The guide's layers are up. */
	bool bGuideLayers = false;
};
