#pragma once

#include "CoreMinimal.h"
#include "ApexProtocolTypes.h"
#include "UI/ApexScreenWidget.h"

#include "ApexSessionResultsWidget.generated.h"

struct FApexCarResult;
struct FApexIncidents;
class UApexButtonWidget;
class UHorizontalBox;
class UTextBlock;
class UVerticalBox;
class UWidget;

/**
 * What just happened: the classification, and the local driver's session.
 *
 * Everything here comes from UApexSessionRecorder, which derives it from the
 * telemetry stream — the protocol has no results message of its own.
 *
 * The screen also opens the moment the local driver takes the flag, while the
 * rest of the field is still racing. Until the session ends it is provisional:
 * the table follows the running order and fills in as the others finish.
 *
 * After qualifying it is the qualifying sheet (ApexBoard::Sheet): the grid in
 * order, gap to pole and to the car ahead, each best lap's sectors with the
 * fastest in purple, and the driver's own grid slot, sectors against pole and
 * ideal lap. Its primary action is the host's: start the race, which grids
 * from this classification.
 *
 * After a race, Incidents turns the table into the race's incident list as
 * the server keeps it for stewards (`RequestIncidents`): each row a contact,
 * a pass under yellow, a track-limits strike or a penalty, and activating
 * one watches this session's own replay from just before it
 * (UApexRootWidget::WatchReplayAt; the session is left).
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

	/** The table on the race's incident list (asked for again) or back on the classification. */
	void ShowIncidents(bool bShow);
	/** Activating incident row Index: the replay from just before it, or a toast why not. */
	void JumpToIncident(int32 Index);
	/** The incident list as it stands, a log line each (`apexsim.incidents`). */
	void LogIncidents() const;

protected:
	virtual void NativeOnInitialized() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

private:
	void BuildLayout();
	UWidget* BuildHeader();
	UWidget* BuildSidePanel();

	void RefreshTable();
	void RefreshSidePanel();
	/** The table's column captions, which differ between a race and qualifying. */
	void RefreshTableHeader(bool bQualifying);
	void FillRaceRows(bool bLive);
	void FillQualifyingRows();
	/** The race's incident list in the table, a row per incident, each a jump into the replay. */
	void FillIncidentRows();
	/** The Incidents button: shown after a race the client is still in, labelled for the other view. */
	void RefreshIncidentsButton();
	void HandleIncidents(const FApexIncidents& Incidents);
	/** The local driver's qualifying: grid slot, gap to pole, sectors, ideal lap. */
	void FillQualifyingSummary(const FApexCarResult& Local);
	/** The primary button: Drive again, or after qualifying the host's Start the race. */
	void RefreshPrimaryAction();

	/** The session recorded is laid out as a timesheet: qualifying, or a weekend's practice. */
	bool IsTimesheet() const;
	bool IsPractice() const;
	/** The header's status: what was run, and whether it is final. */
	FString SessionTitle(bool bLive) const;
	/** The weekend's state while it is in qualifying, for the knockout's cut; null otherwise. */
	const FApexWeekendState* KnockoutRound() const;
	/** The qualifying sheet is a shoot-out's: one flying lap each, in run order (ApexBoard::IsShootoutRound). */
	bool IsShootout() const;
	/** "Start Q2", "Start the race": the weekend's next session; empty when there is none to start. */
	FString NextWeekendSession() const;
	/** This client created the session (the protocol names the host, not their id). */
	bool IsSessionHost() const;

	UFUNCTION() void HandleButtonActivated(UApexButtonWidget* Button);
	UFUNCTION() void HandleWeekendStateChanged();
	/** The Save replay button's state from the replay recorder. */
	void RefreshSaveReplay();

	/** Whether the results are final; drawn from the recorder. */
	bool IsLive() const;

	UPROPERTY(Transient) TObjectPtr<UTextBlock> HeaderStatusText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> HeaderTrackText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> HeaderFormatText;

	UPROPERTY(Transient) TObjectPtr<UHorizontalBox> TableHeaderRow;
	UPROPERTY(Transient) TObjectPtr<UVerticalBox> TableBox;
	UPROPERTY(Transient) TObjectPtr<UVerticalBox> SummaryBox;
	UPROPERTY(Transient) TObjectPtr<UVerticalBox> LapChartBox;

	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> DriveAgainButton;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> BackToLobbyButton;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> SaveReplayButton;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> IncidentsButton;

	/** The table shows the race's incident list instead of the classification. */
	bool bShowingIncidents = false;
	FDelegateHandle IncidentsHandle;

	/** Seconds until the provisional table is redrawn. */
	float LiveRefreshCountdown = 0.0f;
	/** What the last refresh showed, so the switch to final redraws everything. */
	bool bShowingLive = false;
};
