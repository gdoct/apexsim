#pragma once

#include "CoreMinimal.h"
#include "ApexMenuFlowSubsystem.h"
#include "ApexProtocolTypes.h"
#include "Blueprint/UserWidget.h"

#include "ApexRootWidget.generated.h"

class UApexHudWidget;
class UApexPauseMenuWidget;
class UApexScreenWidget;
class UApexSettingsWidget;
class UApexToastWidget;
class UBorder;
class UWidgetSwitcher;
enum class EApexPauseAction : uint8;

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

	/** Switches to a screen and pushes the current one onto the back stack. */
	UFUNCTION(BlueprintCallable, Category = "ApexSim|UI")
	void ShowScreen(EApexScreen Screen);

	/** Switches without recording history — used when returning somewhere. */
	UFUNCTION(BlueprintCallable, Category = "ApexSim|UI")
	void ReplaceScreen(EApexScreen Screen);

	UFUNCTION(BlueprintCallable, Category = "ApexSim|UI")
	void GoBack();

	UFUNCTION(BlueprintCallable, Category = "ApexSim|UI")
	void ShowToast(const FString& Message, bool bIsError = false);

	UFUNCTION(BlueprintPure, Category = "ApexSim|UI")
	EApexScreen GetCurrentScreen() const { return CurrentScreen; }

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

	UFUNCTION()
	void HandleSessionJoined(const FString& SessionId, int32 GridPosition);

	UFUNCTION()
	void HandleSessionLeft();

	UFUNCTION()
	void HandleGameModeChanged(EApexGameMode NewMode);

	UFUNCTION()
	void HandleSessionStateChanged(EApexSessionState NewState);

	/** Hides the menu and hands the view to the race director, or takes it back. */
	void SetRaceViewActive(bool bActive);

	/**
	 * Opens or closes the pause menu.
	 *
	 * Driving input goes with it: while the menu is up the controller is put
	 * back into UI-only input, which both zeroes the controls and stops a
	 * keypress meant for the menu reaching the car. The session itself keeps
	 * running — the server is authoritative and has no pause.
	 */
	void SetPaused(bool bPaused);

	UFUNCTION()
	void HandlePauseAction(EApexPauseAction Action);

	UFUNCTION()
	void HandleSettingsClosed();

	/** True while any race-side overlay owns input. */
	bool IsRaceOverlayOpen() const;

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
	/** A one-click start joined before the telemetry channel was up. */
	bool bStartWhenUdpReady = false;
	bool bAutoRaceRequested = false;
	bool bAutoRaceSessionRequested = false;
	/** -ApexNoStart: create the session but leave it in the lobby. */
	bool bAutoRaceNoStart = false;
	int32 AutoRaceAiCount = 3;
	int32 AutoRaceLaps = 5;
	EApexGameMode AutoRaceMode = EApexGameMode::Race;

	/** Notifies the outgoing and incoming screens, then flips the switcher. */
	void ActivateScreen(EApexScreen Screen);
	UApexScreenWidget* GetScreenWidget(EApexScreen Screen) const;

	EApexScreen CurrentScreen = EApexScreen::MainMenu;
	TArray<EApexScreen> BackStack;
};
