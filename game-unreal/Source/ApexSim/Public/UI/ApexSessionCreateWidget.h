#pragma once

#include "CoreMinimal.h"
#include "ApexProtocolTypes.h"
#include "UI/ApexScreenWidget.h"

#include "ApexSessionCreateWidget.generated.h"

class UApexButtonWidget;
class UProgressBar;
class USlider;
class UTextBlock;
class UVerticalBox;
class UWidget;

/**
 * Sets up a session and creates it: what you drive, where, and against whom.
 *
 * Everything here is also the setup the main menu's one-click start reuses, so
 * changes are written back to the flow subsystem (and the profile) as they are
 * made rather than only on create.
 */
UCLASS()
class APEXSIM_API UApexSessionCreateWidget : public UApexScreenWidget
{
	GENERATED_BODY()

public:
	UApexSessionCreateWidget(const FObjectInitializer& ObjectInitializer);

	virtual void OnScreenActivated() override;

	/** The primary action when it is possible, otherwise the top of the setup column. */
	virtual void FocusDefault() override;
	/** Enter on a slider — anywhere no control took it — creates, as the footer's key cap promises. */
	virtual bool HandleAccept() override;

protected:
	virtual void NativeOnInitialized() override;
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;

private:
	void BuildLayout();
	UWidget* BuildContentColumn();
	UWidget* BuildSettingsColumn();
	UWidget* BuildFooter();

	/** Redraws the track and car summaries from the current pending selection. */
	void RefreshContent();
	/** Pushes slider positions and mode/kind selection into the widgets. */
	void RefreshSettings();
	void RefreshFooter();

	UFUNCTION() void HandleButtonActivated(UApexButtonWidget* Button);
	UFUNCTION() void HandleMaxPlayersChanged(float Value);
	UFUNCTION() void HandleAiCountChanged(float Value);
	UFUNCTION() void HandleLapsChanged(float Value);
	UFUNCTION() void HandleLobbyStateUpdated(const FApexLobbyState& LobbyState);

	UPROPERTY(Transient) TObjectPtr<UVerticalBox> TrackSummaryBox;
	UPROPERTY(Transient) TObjectPtr<UVerticalBox> CarSummaryBox;
	/** Rebuilt with the summaries; kept so focus can survive the rebuild. */
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> ChangeTrackLink;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> ChangeCarLink;

	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> KindMultiplayerButton;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> KindSingleButton;
	UPROPERTY(Transient) TArray<TObjectPtr<UApexButtonWidget>> ModeButtons;

	UPROPERTY(Transient) TObjectPtr<USlider> MaxPlayersSlider;
	UPROPERTY(Transient) TObjectPtr<USlider> AiCountSlider;
	UPROPERTY(Transient) TObjectPtr<USlider> LapsSlider;
	UPROPERTY(Transient) TObjectPtr<UProgressBar> MaxPlayersFill;
	UPROPERTY(Transient) TObjectPtr<UProgressBar> AiCountFill;
	UPROPERTY(Transient) TObjectPtr<UProgressBar> LapsFill;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> MaxPlayersValue;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> AiCountValue;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> LapsValue;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> MaxPlayersSuffix;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> AiCountSuffix;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> LapsSuffix;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> GridSummaryText;

	UPROPERTY(Transient) TObjectPtr<UTextBlock> StatusLine;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> CreateButtonWidget;

	/** Server caps: 20 on the grid, so 19 AI at most alongside one human. */
	static constexpr int32 MaxPlayersCeiling = 20;
	static constexpr int32 LapsCeiling = 50;
};
