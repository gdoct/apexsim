#pragma once

#include "CoreMinimal.h"
#include "ApexProtocolTypes.h"
#include "ApexSettingsSubsystem.h"
#include "UI/ApexNavigation.h"

#include "ApexHotlapWidget.generated.h"

class UApexButtonWidget;
class UApexStepperWidget;
class UBorder;
class UTextBlock;
class UVerticalBox;
class UWidget;

/** What the hotlap overlay is showing. */
UENUM(BlueprintType)
enum class EApexHotlapView : uint8
{
	/** Not a hotlap session. */
	Hidden,
	/** The car is in the garage: the setup card and the timing sheet. */
	Garage,
	/** The car is on the track: the timing sheet beside the HUD. */
	Track,
	/** The record lap is being replayed: a strip with the clock. */
	Replay,
};

/** What the garage card asks the shell to do. */
UENUM(BlueprintType)
enum class EApexHotlapAction : uint8
{
	/** Out onto the run-up before the line. */
	GoOut,
	/** Play the record lap back with the ghost and the replay cameras. */
	ReplayBestLap,
	/** Show or hide the ghost while driving. */
	ToggleGhost,
	/** Every setup knob back to stock. */
	ResetSetup,
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FApexOnHotlapAction, EApexHotlapAction, Action);

/**
 * The hotlap session's own layer over the race view: the garage, and the
 * lap-by-lap timing sheet.
 *
 * **Garage.** While the server holds the car in its garage (the car's
 * `bInGarage` telemetry flag), a card on the left carries the way out, the
 * record lap's replay, the ghost toggle and the car setup — the fourteen
 * clicks-per-knob rows that used to live on a settings tab. Tuning belongs
 * here because it is per car and per circuit: a setup is tried, driven and
 * changed between runs, which is what a hotlap is for. The knobs write
 * through UApexSettingsSubsystem exactly as the tab did, so the root widget
 * still forwards every change to the server as SetCarSetup.
 *
 * **Timing sheet.** Every lap the local car completes, from the server's
 * `LapTiming` messages: number, time, the delta to the best legal lap so
 * far, struck laps greyed. It stays through trips to the garage — the
 * point is to see whether the last change made the car faster — and is
 * cleared when a hotlap session begins.
 *
 * The card acts through OnAction; relocating the car and starting a replay
 * are the shell's and the race director's to carry out.
 */
UCLASS()
class APEXSIM_API UApexHotlapWidget : public UApexNavigableWidget
{
	GENERATED_BODY()

public:
	UApexHotlapWidget(const FObjectInitializer& ObjectInitializer);

	virtual void NativeOnInitialized() override;
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

	/** Focus lands on the way out of the garage. */
	virtual void FocusDefault() override;
	/** Nothing to go back to: Escape falls through to the pause key. */
	virtual bool HandleBack() override { return false; }

	UPROPERTY(BlueprintAssignable, Category = "ApexSim|UI")
	FApexOnHotlapAction OnAction;

	/** A hotlap session began (the sheet is cleared) or ended (everything hides). */
	void SetActive(bool bActive);

	void SetView(EApexHotlapView InView);
	EApexHotlapView GetView() const { return View; }

	/** The local car's lap in progress, from telemetry, for the live line of the sheet. */
	void SetLiveLap(int32 Lap, int32 LapTimeMs, bool bInvalid, int32 BestLapTimeMs);

	/** The replay's clock, for the strip. */
	void SetReplayTime(float ReplayMs, int32 LapTimeMs);

private:
	UFUNCTION()
	void HandleLapTiming(const FApexLapTiming& Timing);

	UFUNCTION()
	void HandleButtonActivated(UApexButtonWidget* Button);

	UFUNCTION()
	void HandleStepperChanged(UApexStepperWidget* Control, int32 Value);

	UFUNCTION()
	void HandleSettingsChanged(EApexSettingsGroup Group);

	UFUNCTION()
	void HandleGhostLap(const FApexGhostLap& Lap);

	UFUNCTION()
	void HandleLapRecord(const FApexLapRecord& Record);

	// --- Construction ---------------------------------------------------------

	UWidget* BuildGarageCard();
	UWidget* BuildTimingPanel();
	UWidget* BuildReplayStrip();
	/** One setup row: name and note on the left, the stepper on the right. */
	void MakeSetupRow(UVerticalBox* Column, int32 Knob, const TCHAR* Label, const TCHAR* Description);
	UApexButtonWidget* MakeCardButton(UVerticalBox* Stack, const FString& Label, const FString& Badge, FName ActionId, bool bPrimary);

	// --- State ----------------------------------------------------------------

	/** Push every knob's value from the settings into its stepper. */
	void RefreshSetup();
	/** The replay and ghost rows follow whether a record lap exists and the ghost setting. */
	void RefreshGhostRows();
	/** Redraw the lap rows and the summary line. */
	void RefreshSheet();
	void ApplyView();

	UApexSettingsSubsystem* GetSettings() const;

	/** One completed lap of the local car. */
	struct FLapEntry
	{
		int32 Lap = 0;
		int32 TimeMs = 0;
		bool bValid = true;
		bool bPersonalBest = false;
		bool bSessionBest = false;
	};
	TArray<FLapEntry> Laps;
	/** The quickest legal lap in the sheet, ms; 0 until one is set. */
	int32 BestMs = 0;

	EApexHotlapView View = EApexHotlapView::Hidden;
	bool bActive = false;
	/** Set while the steppers are being written from the settings, so their events are not changes. */
	bool bRefreshing = false;

	int32 LiveLap = 0;
	int32 LiveLapTimeMs = 0;
	bool bLiveInvalid = false;

	UPROPERTY(Transient) TObjectPtr<UWidget> GarageCard;
	UPROPERTY(Transient) TObjectPtr<UWidget> TimingPanel;
	UPROPERTY(Transient) TObjectPtr<UWidget> ReplayStrip;
	UPROPERTY(Transient) TObjectPtr<UWidget> TrackHint;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> GarageSubtitle;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> SheetLiveText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> SheetLiveLabel;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> SheetBestText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> SheetRecordText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> ReplayText;
	UPROPERTY(Transient) TObjectPtr<UVerticalBox> SheetRows;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> GoOutButton;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> ReplayButton;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> GhostButton;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> ResetButton;
	UPROPERTY(Transient) TMap<int32, TObjectPtr<UApexStepperWidget>> SetupSteppers;
};
