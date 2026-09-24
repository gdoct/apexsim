#pragma once

#include "CoreMinimal.h"
#include "ApexMenuFlowSubsystem.h"
#include "ApexProtocolTypes.h"
#include "Blueprint/UserWidget.h"

#include "ApexRootWidget.generated.h"

class FApexMenuInputProcessor;
class FWeakWidgetPath;
class FWidgetPath;
class SWidget;
struct FFocusEvent;
class UApexHotlapWidget;
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

	UPROPERTY(Transient, BlueprintReadOnly, Category = "ApexSim|UI")
	TObjectPtr<UApexSettingsWidget> SettingsOverlay;

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
};
