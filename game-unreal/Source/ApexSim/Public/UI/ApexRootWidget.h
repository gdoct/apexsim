#pragma once

#include "CoreMinimal.h"
#include "ApexMenuFlowSubsystem.h"
#include "ApexProtocolTypes.h"
#include "Blueprint/UserWidget.h"

#include "ApexRootWidget.generated.h"

class UApexScreenWidget;
class UApexStatusBarWidget;
class UApexToastWidget;
class UImage;
class UWidgetSwitcher;

/**
 * The shell's frame: background, screen switcher, status bar, toast.
 *
 * Owns navigation. Screens ask it to move; it keeps a back stack so Escape and
 * every Back button behave the same way without each screen knowing where it
 * was reached from.
 */
UCLASS(Abstract)
class APEXSIM_API UApexRootWidget : public UUserWidget
{
	GENERATED_BODY()

public:
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
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category = "ApexSim|UI")
	TObjectPtr<UWidgetSwitcher> ScreenSwitcher;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category = "ApexSim|UI")
	TObjectPtr<UImage> BackgroundImage;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category = "ApexSim|UI")
	TObjectPtr<UApexStatusBarWidget> StatusBar;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category = "ApexSim|UI")
	TObjectPtr<UApexToastWidget> Toast;

private:
	UFUNCTION()
	void HandleDisconnected(const FString& Reason);

	UFUNCTION()
	void HandleServerError(int32 Code, const FString& Message);

	UFUNCTION()
	void HandleSessionJoined(const FString& SessionId, int32 GridPosition);

	UFUNCTION()
	void HandleSessionLeft();

	/** Notifies the outgoing and incoming screens, then flips the switcher. */
	void ActivateScreen(EApexScreen Screen);
	UApexScreenWidget* GetScreenWidget(EApexScreen Screen) const;

	EApexScreen CurrentScreen = EApexScreen::MainMenu;
	TArray<EApexScreen> BackStack;
};
