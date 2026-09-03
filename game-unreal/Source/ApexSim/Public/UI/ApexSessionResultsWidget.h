#pragma once

#include "CoreMinimal.h"
#include "ApexProtocolTypes.h"
#include "UI/ApexScreenWidget.h"

#include "ApexSessionResultsWidget.generated.h"

class UApexButtonWidget;
class UTextBlock;
class UVerticalBox;
class UWidget;

/**
 * What just happened: the classification, and the local driver's session.
 *
 * Everything here comes from UApexSessionRecorder, which derives it from the
 * telemetry stream — the protocol has no results message of its own.
 */
UCLASS()
class APEXSIM_API UApexSessionResultsWidget : public UApexScreenWidget
{
	GENERATED_BODY()

public:
	UApexSessionResultsWidget(const FObjectInitializer& ObjectInitializer);

	virtual void OnScreenActivated() override;

	/** Drive again, the primary action. */
	virtual void FocusDefault() override;
	/** No history behind a finished session; Back does nothing rather than something arbitrary. */
	virtual bool HandleBack() override { return true; }

protected:
	virtual void NativeOnInitialized() override;

private:
	void BuildLayout();
	UWidget* BuildHeader();
	UWidget* BuildSidePanel();

	void RefreshTable();
	void RefreshSidePanel();

	UFUNCTION() void HandleButtonActivated(UApexButtonWidget* Button);

	UPROPERTY(Transient) TObjectPtr<UTextBlock> HeaderTrackText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> HeaderFormatText;

	UPROPERTY(Transient) TObjectPtr<UVerticalBox> TableBox;
	UPROPERTY(Transient) TObjectPtr<UVerticalBox> SummaryBox;
	UPROPERTY(Transient) TObjectPtr<UVerticalBox> LapChartBox;

	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> DriveAgainButton;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> BackToLobbyButton;
};
