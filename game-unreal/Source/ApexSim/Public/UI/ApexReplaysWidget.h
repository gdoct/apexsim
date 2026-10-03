#pragma once

#include "CoreMinimal.h"
#include "ApexReplayRecorder.h"
#include "UI/ApexScreenWidget.h"

#include "ApexReplaysWidget.generated.h"

class UApexButtonWidget;
class UScrollBox;
class UTextBlock;
class UVerticalBox;

/**
 * The replays on this machine (UApexReplayRecorder): the ones the player
 * saved, then the recent sessions kept automatically. Enter watches one in
 * the watch view (UApexRootWidget::WatchReplay), K keeps a recent one, Delete
 * removes one. Read again every time the screen opens.
 */
UCLASS()
class APEXSIM_API UApexReplaysWidget : public UApexScreenWidget
{
	GENERATED_BODY()

public:
	UApexReplaysWidget(const FObjectInitializer& ObjectInitializer);

	virtual void OnScreenActivated() override;
	virtual void FocusDefault() override;
	virtual bool HandleNavigation(EUINavigation Direction, UWidget* Source) override;
	/** A replay covers the backdrop anyway; the list reads better on the plain page. */
	virtual bool WantsLiveBackdrop() const override { return false; }

protected:
	virtual void NativeOnInitialized() override;
	virtual FReply NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent) override;

private:
	void BuildLayout();
	/** The list from disk, focus kept on the same row where it can be. */
	void Refresh(int32 FocusIndex = 0);
	/** The row with focus, or INDEX_NONE. */
	int32 FocusedRow() const;
	void KeepRow(int32 Index);
	void DeleteRow(int32 Index);

	UFUNCTION() void HandleButtonActivated(UApexButtonWidget* Button);

	UPROPERTY(Transient) TObjectPtr<UScrollBox> List;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> EmptyText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> CountText;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> BackButton;
	UPROPERTY(Transient) TArray<TObjectPtr<UApexButtonWidget>> Rows;

	TArray<FApexReplayInfo> Replays;
	int32 LastFocused = 0;
};
