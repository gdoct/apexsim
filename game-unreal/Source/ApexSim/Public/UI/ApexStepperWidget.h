#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"

#include "ApexStepperWidget.generated.h"

class UApexButtonWidget;
class UHorizontalBox;
class UTextBlock;

class UApexStepperWidget;
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FApexOnStepperChanged, UApexStepperWidget*, Control, int32, Value);

/**
 * An integer stepped by a pair of pills: [ − ]  +2  (+8%)  [ + ].
 *
 * The garage setup is made of these — a click count either side of the
 * car's own figure — and a slider is the wrong shape for eleven discrete
 * stops, most of them zero. The pills are ordinary UApexButtonWidgets, so
 * focus, hover and pad activation match every other control; the read-out
 * between them is whatever the owner's formatter says the value means.
 */
UCLASS()
class APEXSIM_API UApexStepperWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	UApexStepperWidget(const FObjectInitializer& ObjectInitializer);

	UPROPERTY(BlueprintAssignable, Category = "ApexSim|UI")
	FApexOnStepperChanged OnChanged;

	/** The read-out for a value; the plain number when unset. */
	TFunction<FString(int32)> Formatter;

	/**
	 * Builds the pills and the read-out. ReadoutWidth is the text cell between
	 * the pills, so a column of steppers lines up whatever their read-outs.
	 */
	UFUNCTION(BlueprintCallable, Category = "ApexSim|UI")
	void Setup(int32 InMin, int32 InMax, int32 InValue, float ReadoutWidth = 150.0f);

	/** Moves the value without firing OnChanged. */
	UFUNCTION(BlueprintCallable, Category = "ApexSim|UI")
	void SetValue(int32 InValue);

	UFUNCTION(BlueprintPure, Category = "ApexSim|UI")
	int32 GetValue() const { return Value; }

	/** Identifies the row in the owning screen's single handler. */
	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|UI")
	FName ControlId;

	/** Which knob this is, for an owner that indexes rather than names. */
	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|UI")
	int32 Index = 0;

protected:
	virtual void NativeOnInitialized() override;

private:
	UFUNCTION()
	void HandlePillActivated(UApexButtonWidget* Button);

	/** Repaints the read-out and dims a pill at the end of its range. */
	void ApplyValue();

	UPROPERTY(Transient)
	TObjectPtr<UHorizontalBox> Row;

	UPROPERTY(Transient)
	TObjectPtr<UApexButtonWidget> MinusPill;

	UPROPERTY(Transient)
	TObjectPtr<UApexButtonWidget> PlusPill;

	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> Readout;

	int32 Min = -5;
	int32 Max = 5;
	int32 Value = 0;
	float TextWidth = 150.0f;
	bool bBuilt = false;
};
