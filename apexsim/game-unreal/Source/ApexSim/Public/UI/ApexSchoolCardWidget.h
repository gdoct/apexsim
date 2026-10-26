#pragma once

#include "CoreMinimal.h"
#include "UI/ApexNavigation.h"

#include "ApexSchoolCardWidget.generated.h"

class UApexButtonWidget;
class UBorder;
class UTextBlock;
class UWidget;

/**
 * The driving school's card in its session's garage (School/ApexSchoolSubsystem.h),
 * shown by the root in place of the hotlap garage while the car is parked:
 * before a try the test card (what it asks, what each medal takes, the best
 * so far, why the last try was given up) with START; after one the result
 * card (the medal, the figure, why it failed, a new best) with RETRY. Both
 * have NEXT TEST and BACK TO SCHOOL. While a try is driven it shows the
 * test's time limit counting down at the top of the screen instead.
 */
UCLASS()
class APEXSIM_API UApexSchoolCardWidget : public UApexNavigableWidget
{
	GENERATED_BODY()

public:
	UApexSchoolCardWidget(const FObjectInitializer& ObjectInitializer);

	virtual void NativeOnInitialized() override;
	virtual void NativeDestruct() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;
	virtual void FocusDefault() override;
	virtual bool HandleNavigation(EUINavigation Direction, UWidget* Source) override;
	/** B does nothing here: the card is all there is until the car goes out. */
	virtual bool HandleBack() override { return true; }

	void SetOpen(bool bInOpen);
	bool IsOpen() const { return bOpen; }

private:
	void BuildLayout();
	void Refresh();
	/** The card while open, the countdown while a try is driven, else nothing. */
	void UpdateVisibility();
	void RefreshTimer();

	UFUNCTION() void HandleButtonActivated(UApexButtonWidget* Button);

	UPROPERTY(Transient) TObjectPtr<UTextBlock> KickerText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> TitleText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> GoalText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> MedalText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> FigureText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> WhyText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> TargetsText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> BestText;
	UPROPERTY(Transient) TObjectPtr<UWidget> CardHost;
	UPROPERTY(Transient) TObjectPtr<UBorder> TimerPanel;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> TimerText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> TimerNote;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> PrimaryButton;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> NextButton;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> LeaveButton;

	FDelegateHandle ChangedHandle;
	bool bOpen = false;
};
