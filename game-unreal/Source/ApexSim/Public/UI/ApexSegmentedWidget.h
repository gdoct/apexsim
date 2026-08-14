#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"

#include "ApexSegmentedWidget.generated.h"

class UApexButtonWidget;
class UHorizontalBox;

class UApexSegmentedWidget;
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FApexOnSegmentChosen, UApexSegmentedWidget*, Control, int32, Index);

/**
 * A row of pills where exactly one is on: OFF / LOW / HIGH.
 *
 * The settings screens are almost entirely made of these, so the selection
 * mechanics live here rather than being rebuilt per row. The pills are ordinary
 * UApexButtonWidgets — Ghost when off, Primary when on — which is what keeps
 * hover, focus and keyboard activation identical to every other control.
 */
UCLASS()
class APEXSIM_API UApexSegmentedWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	UApexSegmentedWidget(const FObjectInitializer& ObjectInitializer);

	UPROPERTY(BlueprintAssignable, Category = "ApexSim|UI")
	FApexOnSegmentChosen OnChosen;

	/**
	 * Builds the pills. Widths are per-control rather than per-pill so a group
	 * of two and a group of three still line up down the right-hand edge.
	 */
	UFUNCTION(BlueprintCallable, Category = "ApexSim|UI")
	void Setup(const TArray<FString>& InOptions, int32 InSelectedIndex, float OptionWidth = 118.0f);

	/** Moves the selection without firing OnChosen. */
	UFUNCTION(BlueprintCallable, Category = "ApexSim|UI")
	void SetSelectedIndex(int32 Index);

	UFUNCTION(BlueprintPure, Category = "ApexSim|UI")
	int32 GetSelectedIndex() const { return SelectedIndex; }

	/** Identifies the row in the owning screen's single handler. */
	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|UI")
	FName ControlId;

protected:
	virtual void NativeOnInitialized() override;

private:
	UFUNCTION()
	void HandlePillActivated(UApexButtonWidget* Button);

	/** Repaints every pill for the current selection. */
	void ApplySelection();

	UPROPERTY(Transient)
	TObjectPtr<UHorizontalBox> Row;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UApexButtonWidget>> Pills;

	TArray<FString> Options;
	int32 SelectedIndex = 0;
	float PillWidth = 118.0f;
};
