// Copyright ApexSim Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "MainMenuWidget.generated.h"

class UButton;

/**
 * Main Menu Widget with built-in hover effects for buttons
 */
UCLASS()
class APEXSIM_API UMainMenuWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	UMainMenuWidget(const FObjectInitializer& ObjectInitializer);

protected:
	virtual void NativeConstruct() override;

	/** Bind hover events to all buttons in the widget */
	UFUNCTION(BlueprintCallable, Category = "ApexSim|UI")
	void BindButtonHoverEffects();

	/** Called when Play button is hovered */
	UFUNCTION()
	void OnPlayButtonHovered();
	UFUNCTION()
	void OnPlayButtonUnhovered();

	/** Called when Settings button is hovered */
	UFUNCTION()
	void OnSettingsButtonHovered();
	UFUNCTION()
	void OnSettingsButtonUnhovered();

	/** Called when Content button is hovered */
	UFUNCTION()
	void OnContentButtonHovered();
	UFUNCTION()
	void OnContentButtonUnhovered();

	/** Called when Quit button is hovered */
	UFUNCTION()
	void OnQuitButtonHovered();
	UFUNCTION()
	void OnQuitButtonUnhovered();

	/** Apply hover effect to a button */
	UFUNCTION(BlueprintCallable, Category = "ApexSim|UI")
	void ApplyHoverEffect(UButton* Button);

	/** Remove hover effect from a button */
	UFUNCTION(BlueprintCallable, Category = "ApexSim|UI")
	void RemoveHoverEffect(UButton* Button);

protected:
	/** Scale multiplier when button is hovered */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ApexSim|UI|HoverEffects")
	float HoverScaleMultiplier = 1.05f;

	/** Duration of the hover animation */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ApexSim|UI|HoverEffects")
	float HoverAnimationDuration = 0.15f;

	/** Color tint to apply on hover (multiplied with button's existing color) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ApexSim|UI|HoverEffects")
	FLinearColor HoverColorTint = FLinearColor(1.2f, 1.2f, 1.2f, 1.0f);

	/** Whether to play sound on hover */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ApexSim|UI|HoverEffects")
	bool bPlaySoundOnHover = true;

	/** Sound to play when hovering over a button */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ApexSim|UI|HoverEffects")
	TObjectPtr<USoundBase> HoverSound;

	// Button references - these should be bound in the Blueprint
	UPROPERTY(BlueprintReadWrite, meta = (BindWidget))
	TObjectPtr<UButton> PlayButton;

	UPROPERTY(BlueprintReadWrite, meta = (BindWidget))
	TObjectPtr<UButton> SettingsButton;

	UPROPERTY(BlueprintReadWrite, meta = (BindWidget))
	TObjectPtr<UButton> ContentButton;

	UPROPERTY(BlueprintReadWrite, meta = (BindWidget))
	TObjectPtr<UButton> QuitButton;

private:
	/** Store original render transforms for each button */
	TMap<UButton*, FWidgetTransform> OriginalTransforms;

	/** Store original colors for each button */
	TMap<UButton*, FLinearColor> OriginalColors;
};
