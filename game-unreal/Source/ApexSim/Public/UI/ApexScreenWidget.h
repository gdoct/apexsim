#pragma once

#include "CoreMinimal.h"
#include "ApexMenuFlowSubsystem.h"
#include "UI/ApexNavigation.h"

#include "ApexScreenWidget.generated.h"

class UApexMenuFlowSubsystem;
class UApexNetSubsystem;
class UApexRootWidget;

/**
 * Base for every full screen in the shell.
 *
 * Screens live inside the root widget's WidgetSwitcher, so they are constructed
 * once and shown or hidden thereafter. Per-visit work belongs in
 * OnScreenActivated, not NativeConstruct.
 *
 * Concrete rather than abstract so it can stand in for a screen whose widget
 * class could not be loaded, keeping the switcher's indices lined up with
 * EApexScreen.
 *
 * Keyboard and gamepad navigation come from UApexNavigableWidget: the root
 * puts focus on FocusDefault when a screen is shown, and Back (Escape, B)
 * goes back through the root's history unless a screen says otherwise.
 */
UCLASS()
class APEXSIM_API UApexScreenWidget : public UApexNavigableWidget
{
	GENERATED_BODY()

public:
	/** Called by WBP_Root every time this screen becomes the visible one. */
	virtual void OnScreenActivated();
	/** Called when the switcher moves away from this screen. */
	virtual void OnScreenDeactivated();

	/** Back walks the root's history. Screens at the bottom of a flow override this. */
	virtual bool HandleBack() override;

	/** True while this is the screen on show and no race view is covering it. */
	UFUNCTION(BlueprintPure, Category = "ApexSim|UI")
	bool IsActiveScreen() const;

	UFUNCTION(BlueprintPure, Category = "ApexSim|UI")
	UApexNetSubsystem* GetNet() const;

	UFUNCTION(BlueprintPure, Category = "ApexSim|UI")
	UApexMenuFlowSubsystem* GetFlow() const;

	UFUNCTION(BlueprintPure, Category = "ApexSim|UI")
	UApexRootWidget* GetRoot() const;

protected:
	/** Blueprint hook for screens that want to do extra work on activation. */
	UFUNCTION(BlueprintImplementableEvent, Category = "ApexSim|UI", meta = (DisplayName = "On Screen Activated"))
	void BP_OnScreenActivated();

	UFUNCTION(BlueprintImplementableEvent, Category = "ApexSim|UI", meta = (DisplayName = "On Screen Deactivated"))
	void BP_OnScreenDeactivated();

	/** Convenience: navigate via the owning root. */
	UFUNCTION(BlueprintCallable, Category = "ApexSim|UI")
	void ShowScreen(EApexScreen Screen);

	UFUNCTION(BlueprintCallable, Category = "ApexSim|UI")
	void GoBack();

	UFUNCTION(BlueprintCallable, Category = "ApexSim|UI")
	void ShowToast(const FString& Message, bool bIsError = false);
};
