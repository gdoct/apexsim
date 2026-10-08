#pragma once

#include "CoreMinimal.h"
#include "ApexProtocolTypes.h"
#include "ApexSettingsSubsystem.h"
#include "UI/ApexNavigation.h"

#include "ApexHotlapWidget.generated.h"

class UApexButtonWidget;
class UApexStepperWidget;
class UBorder;
class UEditableTextBox;
class UHorizontalBox;
class UTextBlock;
class UVerticalBox;
class UWidget;
class UWidgetSwitcher;
enum class EApexButtonVariant : uint8;

/** What the hotlap overlay is showing. */
UENUM(BlueprintType)
enum class EApexHotlapView : uint8
{
	/** Not a hotlap session. */
	Hidden,
	/** The car is in the garage: the tuning sheet. */
	Garage,
	/** The car is on the track: the timing sheet beside the HUD. */
	Track,
	/** The record lap is being replayed: a strip with the clock. */
	Replay,
};

/**
 * One compound card in the garage's "Next tyres" column: from the server's
 * setup sheet when it names the car's compounds, else the default five.
 */
struct FApexGarageCompound
{
	/** The `tyre_compound` knob's clicks for this card. */
	int32 Clicks = 0;
	FString Name;
	FString Note;
	FString Figures;
	FLinearColor Colour = FLinearColor::White;
	float Grip = 0.5f;
	float Life = 0.5f;
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
	/** Go out on cold tyres, or at the optimum. */
	ToggleColdTyres,
	/** Every setup knob back to stock. */
	ResetSetup,
	/** Keep the session's laps so far as a replay. */
	SaveReplay,
	/** The scoreboard's camera button: TV, chase, onboard on the watched car. */
	CycleWatchCamera,
	/** Menu mode: the setup editor is done (the main menu's Garage > Manage car setups). */
	CloseEditor,
};

/** The garage's pages, in tab order. */
enum class EApexGarageTab : uint8
{
	Tyres,
	Suspension,
	Engine,
	Save,
	Count,
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FApexOnHotlapAction, EApexHotlapAction, Action);
/** The scoreboard asks to watch a car (its index), or to stop (INDEX_NONE). */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FApexOnHotlapWatch, int32, CarIndex);

/**
 * The hotlap session's own layer over the race view: the garage, and the
 * lap-by-lap timing sheet.
 *
 * **Garage.** While the server holds the car in its garage (the car's
 * `bInGarage` telemetry flag) a full-screen sheet covers the view: the way
 * out, the record lap's replay, the ghost toggle and a reset down the left,
 * and the setup split into four tabs on the right — Tyres (pressures,
 * brakes, the next compound), Suspension (front and rear side by side, with
 * the rake and aero balance they add up to), Engine (engine, gearing, fuel,
 * with top speed per gear and the fuel's range and weight) and Load / Save
 * (named setups per car). Q / E (or the shoulders, or Tab) change tab.
 *
 * Every stepper shows the knob in real units with its change from stock
 * under it. The units come from the server's CarSetupSheet (the car's own
 * stock figures, many of which are the sim's defaults and not in any file);
 * until one arrives, or from a server without it, a knob reads in clicks.
 * The knobs write through UApexSettingsSubsystem, so the root widget still
 * forwards every change to the server as SetCarSetup.
 *
 * **Timing sheet.** On the track, every lap the local car completes, from
 * the server's `LapTiming`: number, time, the delta to the best legal lap,
 * struck laps greyed. Cleared when a hotlap session begins. A legal lap also
 * becomes the loaded saved setup's best while the working setup matches it.
 *
 * **Qualifying** uses the same garage with two differences: the car leaves on
 * cold tyres and drives a whole outlap before its first timed lap, and the
 * garage has a second page, the **scoreboard**, that takes the place of the
 * setup sheet: every car fastest lap first, and a click on a driver puts the
 * view on that car (OnWatchCar; the shell points the race director's
 * spectator camera at it) until the sheet is back or the car goes out.
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

	/** Focus lands on the way out of the garage. */
	virtual void FocusDefault() override;
	/** Previous / Next (shoulders, Tab) change tab; the rest is Slate's. */
	virtual bool HandleNavigation(EUINavigation Direction, UWidget* Source) override;
	/** Nothing to go back to: Escape falls through to the pause key. */
	virtual bool HandleBack() override;

	UPROPERTY(BlueprintAssignable, Category = "ApexSim|UI")
	FApexOnHotlapAction OnAction;

	UPROPERTY(BlueprintAssignable, Category = "ApexSim|UI")
	FApexOnHotlapWatch OnWatchCar;

	/**
	 * The garage as a setup editor outside any session (the main menu's
	 * Garage > Manage car setups): the same tabs and Load / Save for the pending
	 * car, no way out onto a track, no replay rows, and DONE (or Escape / B)
	 * raises CloseEditor. The server's setup sheet is not used (it may belong
	 * to another car), so knobs read in clicks.
	 */
	void SetMenuMode(bool bInMenuMode);
	bool IsMenuMode() const { return bMenuMode; }

	/** The session is a qualifying one: the scoreboard page exists, the hotlap-only rows go. */
	void SetQualifying(bool bInQualifying);
	bool IsQualifying() const { return bQualifying; }

	/** Shows the scoreboard (true) or the setup sheet. Closing it stops watching. */
	void SetBoardOpen(bool bOpen);
	bool IsBoardOpen() const { return bBoardOpen; }

	/** The car being watched from the scoreboard, INDEX_NONE for none. */
	int32 GetWatchedCar() const { return WatchedCar; }
	/** Watches the car on a scoreboard row (0-based); the console's way of clicking one. */
	void WatchRow(int32 Row);
	/** What the camera button says ("TV", "CHASE", "ONBOARD"). */
	void SetWatchCameraLabel(const FString& Label);

	/** A hotlap session began (the sheet is cleared) or ended (everything hides). */
	void SetActive(bool bActive);

	void SetView(EApexHotlapView InView);
	EApexHotlapView GetView() const { return View; }

	/** The local car's lap in progress, from telemetry, for the live line of the sheet. */
	void SetLiveLap(int32 Lap, int32 LapTimeMs, bool bInvalid, int32 BestLapTimeMs);

	/** The replay's clock, for the strip. */
	void SetReplayTime(float ReplayMs, int32 LapTimeMs);

	/** Shows a garage tab (also `apexsim.hotlap.Tab` / `-ApexGarageTab=`). */
	void SetTab(EApexGarageTab Tab);

	/**
	 * The cards for a sheet: one per listed compound, click = reference - index;
	 * the hand-written defaults without a list or for the default five
	 * (what the server sends for a car that files none of its own).
	 */
	static TArray<FApexGarageCompound> CompoundCardsFor(const FApexCarSetupSheet* Sheet);

protected:
	/** Refreshes the scoreboard a few times a second while it is up. */
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

	/** Q / E change tab, unless the setup name box is being typed in. */
	virtual FReply NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent) override;

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

	UFUNCTION()
	void HandleSetupSheet(const FApexCarSetupSheet& Sheet);

	// --- Construction ---------------------------------------------------------

	UWidget* BuildGarage();
	UWidget* BuildGarageHeader();
	UWidget* BuildActionColumn();
	UWidget* BuildTabBar();
	UWidget* BuildTyresPage();
	UWidget* BuildSuspensionPage();
	UWidget* BuildEnginePage();
	UWidget* BuildSavePage();
	UWidget* BuildTimingPanel();
	UWidget* BuildReplayStrip();
	UWidget* BuildBoard();
	/** The garage header's Tune / Scoreboard pair (qualifying only). */
	UWidget* BuildPageToggle(UApexButtonWidget*& OutTune, UApexButtonWidget*& OutBoard);

	/** The board's rows from the roster and the newest telemetry. */
	void RefreshBoard();
	/** Garage card or scoreboard, and the toggles' selection. */
	void ApplyPage();
	void ApplyQualifyingRows();
	void StopWatching();

	/** A stepper for one knob, registered for refreshes. */
	UApexStepperWidget* MakeStepper(int32 Knob);
	/** A section caption and its rows of one knob each. */
	void AddSection(UVerticalBox* Column, const TCHAR* Title, std::initializer_list<int32> Knobs);
	/** One suspension row: the pair's name and note, the front stepper, the rear. */
	UWidget* AddPairRow(UVerticalBox* Table, const TCHAR* Label, const TCHAR* Note, int32 Front, int32 Rear);
	UApexButtonWidget* MakeActionButton(const FString& Label, const FString& Badge, FName ActionId, EApexButtonVariant Variant, float Height, float LabelSize);

	// --- State ----------------------------------------------------------------

	/** Every stepper's value from the settings, and everything derived from the setup. */
	void RefreshSetup();
	/** Rake, balance, gear speeds and fuel: the read-outs worked out from the setup. */
	void RefreshDerived();
	/** The compound cards' selection. */
	void RefreshCompounds();
	/** The saved list and the selected setup's detail. */
	void RefreshSaved();
	/** The replay and ghost rows follow whether a record lap exists and the ghost setting. */
	void RefreshGhostRows();
	/** The header's track and car. */
	void RefreshSubtitle();
	/** Redraw the lap rows and the summary line. */
	void RefreshSheet();
	void ApplyView();
	void ApplyTab();

	UApexSettingsSubsystem* GetSettings() const;
	const FApexCarSetup* GetWorkingSetup() const;
	/** The server's reference card for the car being driven, or null. */
	const FApexCarSetupSheet* GetSheet() const;
	/** The sheet when it names the car's compounds, whatever its knob count; null when the server named none. */
	const FApexCarSetupSheet* GetCompoundSheet() const;
	/** The compound knob's read-out: the sheet's name for the click when it has one, else the default table's. */
	FString CompoundLabel(int32 Clicks) const;
	/** Rebuilds the compound cards in CompoundHost from CompoundCardsFor. */
	void BuildCompoundCards();
	FString GetCarId() const;

	/** A knob at a click count, in the sheet's units ("124.8 N/mm"), or in clicks without one. */
	FString DescribeValue(int32 Knob, int32 Clicks) const;
	/** Its change from stock ("+4.8"), or empty at stock. */
	FString DescribeDelta(int32 Knob, int32 Clicks) const;

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

	bool bQualifying = false;
	bool bBoardOpen = false;
	bool bMenuMode = false;
	int32 WatchedCar = INDEX_NONE;
	float BoardRefreshIn = 0.0f;
	/** The car index each board row shows (INDEX_NONE for an unused row). */
	TArray<int32> BoardCars;
	FString WatchCameraLabel = TEXT("TV");

	EApexHotlapView View = EApexHotlapView::Hidden;
	EApexGarageTab Tab = EApexGarageTab::Tyres;
	bool bActive = false;
	/** Set while the steppers are being written from the settings, so their events are not changes. */
	bool bRefreshing = false;

	int32 LiveLap = 0;
	int32 LiveLapTimeMs = 0;
	bool bLiveInvalid = false;

	/** The saved setup picked in the Load / Save list. */
	FGuid SelectedSaved;
	/** The saved setups of this car, in the list's order (mirrors the rows). */
	TArray<FGuid> SavedRowIds;

	UPROPERTY(Transient) TObjectPtr<UWidget> Garage;
	UPROPERTY(Transient) TObjectPtr<UWidget> BoardLayer;
	UPROPERTY(Transient) TObjectPtr<UWidget> GaragePageToggle;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> GarageCaption;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> SheetTitle;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> GarageNote;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> BoardHint;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> GarageTuneButton;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> GarageBoardButton;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> BoardTuneButton;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> BoardBoardButton;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> BoardCameraButton;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> BoardGoOutButton;
	UPROPERTY(Transient) TArray<TObjectPtr<UApexButtonWidget>> BoardRowButtons;
	UPROPERTY(Transient) TArray<TObjectPtr<UWidget>> BoardRowRoots;
	UPROPERTY(Transient) TArray<TObjectPtr<UTextBlock>> BoardPos;
	UPROPERTY(Transient) TArray<TObjectPtr<UTextBlock>> BoardName;
	UPROPERTY(Transient) TArray<TObjectPtr<UTextBlock>> BoardBest;
	UPROPERTY(Transient) TArray<TObjectPtr<UTextBlock>> BoardGap;
	UPROPERTY(Transient) TArray<TObjectPtr<UTextBlock>> BoardState;
	UPROPERTY(Transient) TObjectPtr<UWidget> TimingPanel;
	UPROPERTY(Transient) TObjectPtr<UWidget> ReplayStrip;
	UPROPERTY(Transient) TObjectPtr<UWidget> TrackHint;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> GarageSubtitle;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> SetupNameText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> SetupChangesText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> SheetLiveText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> SheetLiveLabel;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> SheetBestText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> SheetRecordText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> ReplayText;
	UPROPERTY(Transient) TObjectPtr<UVerticalBox> SheetRows;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> GoOutButton;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> ReplayButton;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> GhostButton;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> SaveReplayButton;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> TyresButton;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> ResetButton;
	UPROPERTY(Transient) TMap<int32, TObjectPtr<UApexStepperWidget>> SetupSteppers;

	// Tabs.
	UPROPERTY(Transient) TArray<TObjectPtr<UApexButtonWidget>> TabButtons;
	UPROPERTY(Transient) TArray<TObjectPtr<UBorder>> TabUnderlines;
	UPROPERTY(Transient) TArray<TObjectPtr<UTextBlock>> TabNumbers;
	UPROPERTY(Transient) TArray<TObjectPtr<UTextBlock>> TabLabels;
	/** Where focus goes on each page when the tab is changed from the keyboard. */
	UPROPERTY(Transient) TArray<TObjectPtr<UWidget>> PageDefaults;
	UPROPERTY(Transient) TObjectPtr<UWidgetSwitcher> Pages;

	// Tyres.
	UPROPERTY(Transient) TArray<TObjectPtr<UApexButtonWidget>> CompoundButtons;
	UPROPERTY(Transient) TObjectPtr<UVerticalBox> CompoundHost;
	/** The cards CompoundButtons were built from, one each, and the sheet list they came from. */
	TArray<FApexGarageCompound> CompoundCards;
	TArray<FString> CompoundCardNames;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> PressureNote;

	// Suspension.
	UPROPERTY(Transient) TObjectPtr<UWidget> CamberRow;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> RakeText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> BalanceText;

	// Engine.
	UPROPERTY(Transient) TObjectPtr<UHorizontalBox> GearBars;
	UPROPERTY(Transient) TObjectPtr<UHorizontalBox> GearLabels;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> FuelRangeText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> FuelWeightText;

	// Load / Save.
	UPROPERTY(Transient) TObjectPtr<UEditableTextBox> SaveNameBox;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> SaveButton;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> SavedCountText;
	UPROPERTY(Transient) TObjectPtr<UVerticalBox> SavedList;
	UPROPERTY(Transient) TArray<TObjectPtr<UApexButtonWidget>> SavedRowButtons;
	UPROPERTY(Transient) TObjectPtr<UWidget> DetailPanel;
	UPROPERTY(Transient) TObjectPtr<UWidget> DetailEmpty;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> DetailName;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> DetailMeta;
	UPROPERTY(Transient) TObjectPtr<UVerticalBox> DetailDiff;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> LoadButton;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> OverwriteButton;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> RenameButton;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> DeleteButton;
};
