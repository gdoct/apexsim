#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"

#include "ApexTrackGuideWidget.generated.h"

class UApexButtonWidget;
class UApexTrackGuideSubsystem;
class UBorder;
class UTextBlock;
class UVerticalBox;
class UWidget;

/**
 * The track guide's layer over the world (docs/TRACK_GUIDE.md): a card on
 * the left with the circuit's facts (the overview) or the corner's (name,
 * direction, minimum speed and gear, braking point, entry and exit speed,
 * elevation, banking, the gotchas), the stop and playback state along the
 * top ("SLOW MOTION 0.25x"), and the controls along the bottom, each a
 * button for the mouse with its key on it.
 *
 * Reads UApexTrackGuideSubsystem every frame and acts on it directly; the
 * keys come from the shell's input processor (UApexRootWidget::
 * HandleGuideKey), so nothing here needs focus. The card is rebuilt only when
 * the stop changes. While the circuit loads it says so over the shell's
 * background.
 */
UCLASS()
class APEXSIM_API UApexTrackGuideWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	UApexTrackGuideWidget(const FObjectInitializer& ObjectInitializer);

	virtual void NativeOnInitialized() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

	/** Shown while a guide is open. */
	void SetActive(bool bActive);

private:
	UFUNCTION()
	void HandleButtonActivated(UApexButtonWidget* Button);

	UApexTrackGuideSubsystem* GetGuide() const;
	bool IsMetric() const;

	UWidget* BuildTopStrip();
	UWidget* BuildControls();
	UWidget* BuildLoading();
	UApexButtonWidget* MakeControl(const FString& Label, const FString& Key, FName Action);

	/** The card for whatever is on screen now. */
	void RebuildCard();
	void BuildTrackCard(UVerticalBox* Body);
	void BuildCornerCard(UVerticalBox* Body, int32 CornerIndex);
	/** Playback chip, progress, camera and the buttons' labels. */
	void RefreshLive();

	UPROPERTY(Transient) TObjectPtr<UWidget> LoadingPanel;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> LoadingTitle;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> LoadingText;
	UPROPERTY(Transient) TObjectPtr<UWidget> Card;
	UPROPERTY(Transient) TObjectPtr<UVerticalBox> CardBody;
	UPROPERTY(Transient) TObjectPtr<UWidget> TopStrip;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> ProgressText;
	UPROPERTY(Transient) TObjectPtr<UBorder> PlaybackChip;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> PlaybackText;
	UPROPERTY(Transient) TObjectPtr<UWidget> Controls;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> PrevButton;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> NextButton;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> CameraButton;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> PauseButton;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> BackButton;

	bool bActive = false;
	/** What the card was built for: the corner index (INDEX_NONE: overview), and whether the guide was running. */
	int32 BuiltCorner = -2;
	bool bBuiltRunning = false;
	/** Labels last pushed, so an unchanged one costs nothing. */
	FString ShownPlayback;
	FString ShownProgress;
	FString ShownCamera;
	FString ShownNext;
	FString ShownPause;
};
