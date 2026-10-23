#pragma once

#include "CoreMinimal.h"
#include "UI/ApexNavigation.h"

#include "ApexPauseMenuWidget.generated.h"

class UApexButtonWidget;
class UApexLapTraceWidget;
class UApexStepperWidget;
class USizeBox;
class UBorder;
class UTextBlock;
class UVerticalBox;
class UWidget;

/** What the pause menu is asking the shell to do. */
UENUM(BlueprintType)
enum class EApexPauseAction : uint8
{
	Resume,
	OpenSettings,
	/** Hotlap only: the car back into its garage. */
	ReturnToGarage,
	/** Write the session so far as a replay (UApexReplayRecorder). */
	SaveReplay,
	LeaveSession,
	QuitGame,
	/** A stuck car back onto the track where it is, held for the time cost. */
	RecoverToTrack,
	/** A stuck car towed to its pit box, serviced and driven out. */
	RecoverToPits,
	/** Practice driven alone: the session back ten seconds (`Rewind`). */
	Rewind,
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FApexOnPauseAction, EApexPauseAction, Action);

/**
 * The Escape menu over a running race.
 *
 * It does not act on anything itself — it reports which of the four things the
 * player chose and lets the root widget carry it out, because leaving a session
 * and quitting the game are shell concerns that this widget has no business
 * reaching into.
 *
 * "Paused" is a local state, not a server one: the session keeps running while
 * this is open, because the server is authoritative and eleven other cars are
 * still driving. The status strip says so rather than claiming a hold the
 * protocol cannot deliver.
 */
UCLASS()
class APEXSIM_API UApexPauseMenuWidget : public UApexNavigableWidget
{
	GENERATED_BODY()

public:
	UApexPauseMenuWidget(const FObjectInitializer& ObjectInitializer);

	virtual void NativeOnInitialized() override;
	virtual FReply NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent) override;

	/** Focus lands on Resume, so Accept is always the safe answer. */
	virtual void FocusDefault() override;
	/** Up and Down walk the rows and wrap; nothing lies left or right. */
	virtual bool HandleNavigation(EUINavigation Direction, UWidget* Source) override;
	/** Back resumes, as the footer promises; on the pit plan, back to the menu's rows. */
	virtual bool HandleBack() override;

	UPROPERTY(BlueprintAssignable, Category = "ApexSim|UI")
	FApexOnPauseAction OnAction;

	/** Shows the menu, refreshes the status strip, and takes keyboard focus. */
	UFUNCTION(BlueprintCallable, Category = "ApexSim|UI")
	void Open();

	UFUNCTION(BlueprintCallable, Category = "ApexSim|UI")
	void Close();

	UFUNCTION(BlueprintPure, Category = "ApexSim|UI")
	bool IsOpen() const { return bOpen; }

	/**
	 * Over a race being watched rather than driven: the way out is "stop
	 * watching", and the strip names the race, not the player's car. Read
	 * when the menu opens.
	 */
	void SetWatching(bool bInWatching) { bWatching = bInWatching; }

	/**
	 * The pit plan in place of the menu's rows: the driver's orders for the
	 * next stop and the box-this-lap request (UApexPitPlanSubsystem), sent
	 * as each changes. Back returns to the rows.
	 */
	void OpenPitPlan();
	bool IsPitPlanOpen() const { return bPitPlanOpen; }

	/**
	 * The last lap against the best (UApexLapTraceSubsystem) in place of the
	 * menu's rows, the card widened for the traces. Back returns to the rows.
	 */
	void OpenLapTraces();
	bool IsLapTracesOpen() const { return bLapTracesOpen; }

private:
	void ClosePitPlan();
	void CloseLapTraces();
	/** Every stepper of the plan to the subsystem's plan, its ranges to the car's compounds. */
	void RefreshPitPlan();
	/** The line under the steppers: the plan as the menu's row reads it. */
	void RefreshPitPlanSummary();

	UFUNCTION()
	void HandlePitPlanChanged(UApexStepperWidget* Control, int32 Value);

	/** The lap traces' LAP or AGAINST stepper moved: the graph and legend redrawn. */
	UFUNCTION()
	void HandleLapTracePicked(UApexStepperWidget* Control, int32 Value);
	/** The graph and legend for the steppers' two laps (UApexLapTraceSubsystem::GetLaps). */
	void ShowPickedLapTraces();

	UFUNCTION()
	void HandleButtonActivated(UApexButtonWidget* Button);

	/** One row of the card: label left, consequence right. */
	UApexButtonWidget* AddRow(
		UVerticalBox* Stack,
		const FString& Label,
		const FString& Badge,
		FName ActionId,
		bool bPrimary,
		const FString& KeyCap = FString());

	/** Lap, position, circuit and car — what the blurred scene no longer shows. */
	void RefreshStatusStrip();

	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> StatusText;

	UPROPERTY(Transient)
	TObjectPtr<UBorder> StatusBadge;

	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> StatusBadgeText;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UApexButtonWidget>> Rows;

	/** Shown only in a hotlap with the car out on the track. */
	UPROPERTY(Transient)
	TObjectPtr<UApexButtonWidget> GarageRow;

	/** A stuck car's way out (`RecoverCar`): shown while the local car is out
	 *  on the track in a session that runs; the pits row not in a hotlap or
	 *  qualifying, where the garage row is the way back. */
	UPROPERTY(Transient)
	TObjectPtr<UApexButtonWidget> RecoverTrackRow;

	UPROPERTY(Transient)
	TObjectPtr<UApexButtonWidget> RecoverPitsRow;

	/** Back ten seconds: a practice session the player drives alone (`CanRewind`). */
	UPROPERTY(Transient)
	TObjectPtr<UApexButtonWidget> RewindRow;

	/** Back to the main menu, or out of the race being watched. */
	UPROPERTY(Transient)
	TObjectPtr<UApexButtonWidget> LeaveRow;

	/** Shown while there is a session on record worth a replay. */
	UPROPERTY(Transient)
	TObjectPtr<UApexButtonWidget> SaveReplayRow;

	/** Opens the pit plan: shown to a driver with a car in a race or free practice. */
	UPROPERTY(Transient)
	TObjectPtr<UApexButtonWidget> PitPlanRow;

	/** The heading's big word: PAUSED, or PIT PLAN. */
	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> HeadingText;

	/** The menu's rows and the pit plan, one shown at a time. */
	UPROPERTY(Transient)
	TObjectPtr<UWidget> ActionsPanel;

	UPROPERTY(Transient)
	TObjectPtr<UWidget> PitPlanPanel;

	/** Tyres, fuel, repairs, pads, pad set, box this lap (Index 0..5). */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UApexStepperWidget>> PitPlanSteppers;

	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> PitPlanSummaryText;

	UPROPERTY(Transient)
	TObjectPtr<UApexButtonWidget> PitPlanDoneButton;

	/** Opens the lap traces: shown to a driver once a lap is on record here. */
	UPROPERTY(Transient)
	TObjectPtr<UApexButtonWidget> LapTracesRow;

	UPROPERTY(Transient)
	TObjectPtr<UWidget> LapTracesPanel;

	UPROPERTY(Transient)
	TObjectPtr<UApexLapTraceWidget> LapTraceGraph;

	/** Which held lap is drawn (LAP) and which it is drawn against (AGAINST). */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UApexStepperWidget>> LapTraceSteppers;

	/** The two laps' names and times, and where the files are. */
	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> LapTracesLegend;

	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> LapTracesNote;

	UPROPERTY(Transient)
	TObjectPtr<UApexButtonWidget> LapTracesDoneButton;

	/** The card's width box: wider while the traces are up. */
	UPROPERTY(Transient)
	TObjectPtr<USizeBox> CardSize;

	bool bPitPlanOpen = false;
	bool bLapTracesOpen = false;
	bool bOpen = false;
	bool bWatching = false;
};
