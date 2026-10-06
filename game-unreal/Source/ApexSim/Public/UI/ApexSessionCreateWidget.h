#pragma once

#include "CoreMinimal.h"
#include "ApexProtocolTypes.h"
#include "UI/ApexScreenWidget.h"

#include "ApexSessionCreateWidget.generated.h"

class UApexButtonWidget;
struct FApexButtonSpec;
class UBorder;
class UCanvasPanelSlot;
class UImage;
class UProgressBar;
class USlider;
class UTextBlock;
class UHorizontalBox;
class UApexSettingsSubsystem;
class UVerticalBox;
class UTexture2D;
class UWidget;

/**
 * Sets up a session and creates it: what you drive, where, against whom and
 * under what sky.
 *
 * Two tabs on the right (Race: mode, length, field, assists; Conditions:
 * clock, weather, air, wind), and on the left the track and car over a live
 * preview of the open tab: the starting grid the server will line up, or
 * the sky at that hour in that weather with the air it will resolve.
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
	/** The turntable shares the world with the demo race. */
	virtual bool WantsLiveBackdrop() const override { return false; }

	/** The primary action when it is possible, otherwise the session kind. */
	virtual void FocusDefault() override;
	/** Enter on a slider — anywhere no control took it — creates, as the footer's key cap promises. */
	virtual bool HandleAccept() override;
	/**
	 * Tab and the shoulders switch tabs. Directions go to the nearest control
	 * that way by geometry, whatever it lines up with: the columns do not
	 * share rows, and Slate's own search only looks at what overlaps.
	 */
	virtual bool HandleNavigation(EUINavigation Direction, UWidget* Source) override;

	/** Which tab is open: 0 Race, 1 Conditions. */
	void SetActiveTab(int32 Tab);

protected:
	virtual void NativeOnInitialized() override;
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;

	/**
	 * The starting grid's slots take the mouse here rather than as buttons: a
	 * press on a driver can end on another slot (drag to reorder) or on the
	 * same one (a click picks the driver up, a second click on a slot drops
	 * it there).
	 */
	virtual FReply NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;
	virtual FReply NativeOnMouseMove(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;
	virtual FReply NativeOnMouseButtonUp(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;
	virtual void NativeOnMouseCaptureLost(const FCaptureLostEvent& CaptureLostEvent) override;

private:
	void BuildLayout();
	UWidget* BuildHeaderKinds();
	UWidget* BuildLeftColumn();
	UWidget* BuildGridPreview();
	UWidget* BuildSkyPreview();
	UWidget* BuildRightColumn();
	UWidget* BuildRaceTab();
	UWidget* BuildConditionsTab();
	UWidget* BuildFooter();

	/** One button for the screen's handler. */
	UApexButtonWidget* MakeButton(const FApexButtonSpec& Spec);
	/** A bordered −/+ pair round a value, the field and air rows' control. */
	UWidget* MakeStepper(UApexButtonWidget*& OutMinus, UTextBlock*& OutValue, UApexButtonWidget*& OutPlus,
		float PillSize, float ValueWidth, float ValueSize, UTextBlock** OutNote = nullptr);
	/** A section caption, with an optional read-out at its right end. */
	UWidget* MakeCaption(const FString& Label, UTextBlock** OutRight = nullptr, UWidget* RightWidget = nullptr);

	/** Redraws the track and car summaries from the current pending selection. */
	void RefreshContent();
	/** Pushes every choice into the controls and both previews. */
	void RefreshSettings();
	void RefreshGridPreview();
	/**
	 * The car's saved setups as chips, rebuilt only when the car or the list
	 * changes (a rebuild would drop the keyboard's place), and which one is
	 * the working setup.
	 */
	void RefreshSetups();
	/**
	 * The race's starting order: the chips (the seating order, an edited
	 * one, the stored qualifying results of this track), and your slot. Also
	 * resets the order when the track changed under it and asks the server
	 * for the track's results.
	 */
	void RefreshStartOrder();
	void RequestQualifying();
	void RefreshSkyPreview();
	void RefreshFooter();

	/** Every control focus could land on now: the open tab's, the header's, the footer's. */
	void GatherFocusables(TArray<UWidget*>& Out) const;

	UFUNCTION() void HandleButtonActivated(UApexButtonWidget* Button);
	UFUNCTION() void HandleTimeOfDayChanged(float Value);
	UFUNCTION() void HandleLobbyStateUpdated(const FApexLobbyState& LobbyState);
	UFUNCTION() void HandleQualifyingResults(const FString& TrackId);

	// --- The start order --------------------------------------------------------
	/** The order for the field as it is now, complete: what the grid shows. */
	TArray<FString> CurrentOrder() const;
	/** Keeps an order the host made (a stored result's when ResultId names it). */
	void StoreOrder(const TArray<FString>& Order, const FString& ResultId);
	/** Makes a stored qualifying result the working start order. */
	void LoadResult(const FApexQualifyingResult& Result);
	/** The slot under a desktop-space point, INDEX_NONE for none. */
	int32 SlotUnder(const FVector2D& ScreenPosition) const;
	/** A press and release on one slot: pick the driver up or drop the picked one here. */
	void ClickSlot(int32 Clicked);
	/** A driver dragged from one slot to another: it takes that place and the rest shift. */
	void DropSlot(int32 From, int32 To);
	bool IsGridInteractive() const;

	/** Grid size (multiplayer) or the AI count (single player) from a slot. */
	void PickGridSlot(int32 Position);
	void SetGridSize(int32 Size);
	void SetAiCount(int32 Count);
	UApexSettingsSubsystem* GetSettingsSubsystem() const;

	/** The flow changed: save it and redraw. */
	void Changed();

	/** The art layers, built once: a vertical alpha ramp and a rain hatch. */
	UTexture2D* GradientTexture();
	UTexture2D* RainTexture();

	int32 ActiveTab = 0;

	// --- Header ---------------------------------------------------------------
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> HomeButton;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> KindMultiplayerButton;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> KindSingleButton;

	// --- Left column ----------------------------------------------------------
	UPROPERTY(Transient) TObjectPtr<UHorizontalBox> TrackSummaryBox;
	UPROPERTY(Transient) TObjectPtr<UHorizontalBox> CarSummaryBox;
	/** Rebuilt with the summaries; kept so focus can survive the rebuild. */
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> ChangeTrackLink;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> ChangeCarLink;

	UPROPERTY(Transient) TObjectPtr<UWidget> GridPreview;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> GridHintText;
	UPROPERTY(Transient) TObjectPtr<UWidget> GridSlotsPanel;
	UPROPERTY(Transient) TObjectPtr<UWidget> GridSoloPanel;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> GridSoloTitle;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> GridSoloText;
	UPROPERTY(Transient) TArray<TObjectPtr<UBorder>> SlotFaces;
	UPROPERTY(Transient) TArray<TObjectPtr<UTextBlock>> SlotNumbers;
	UPROPERTY(Transient) TArray<TObjectPtr<UTextBlock>> SlotNames;
	UPROPERTY(Transient) TArray<TObjectPtr<UApexButtonWidget>> SlotButtons;

	UPROPERTY(Transient) TObjectPtr<UWidget> SkyPreview;
	UPROPERTY(Transient) TObjectPtr<UBorder> SkyBase;
	UPROPERTY(Transient) TObjectPtr<UImage> SkyTop;
	UPROPERTY(Transient) TObjectPtr<UBorder> SunDisc;
	UPROPERTY(Transient) TObjectPtr<UBorder> SunGlow;
	UPROPERTY(Transient) TObjectPtr<UCanvasPanelSlot> SunDiscSlot;
	UPROPERTY(Transient) TObjectPtr<UCanvasPanelSlot> SunGlowSlot;
	UPROPERTY(Transient) TObjectPtr<UBorder> CloudVeil;
	UPROPERTY(Transient) TObjectPtr<UImage> RainVeil;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> SkyClockText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> SkyPhaseText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> SkyAirText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> SkyTrackText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> SkyGripText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> SkyWindText;
	UPROPERTY(Transient) TObjectPtr<UWidget> SkyWindArrow;

	// --- Tabs -----------------------------------------------------------------
	UPROPERTY(Transient) TArray<TObjectPtr<UApexButtonWidget>> TabButtons;
	UPROPERTY(Transient) TArray<TObjectPtr<UBorder>> TabLines;
	UPROPERTY(Transient) TObjectPtr<UWidget> RaceTab;
	UPROPERTY(Transient) TObjectPtr<UWidget> ConditionsTab;

	// --- Race tab -------------------------------------------------------------
	UPROPERTY(Transient) TArray<TObjectPtr<UApexButtonWidget>> ModeButtons;
	/** Shown for a race only. */
	UPROPERTY(Transient) TObjectPtr<UWidget> LengthSection;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> LengthInfoText;
	/** Laps or Time: what the stepper and the presets count. */
	UPROPERTY(Transient) TArray<TObjectPtr<UApexButtonWidget>> LengthUnitButtons;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> LapsMinus;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> LapsPlus;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> LapsValueText;
	UPROPERTY(Transient) TArray<TObjectPtr<UApexButtonWidget>> LapPresetButtons;
	/** The timed race's presets, in the same place; one set shows at a time. */
	UPROPERTY(Transient) TArray<TObjectPtr<UApexButtonWidget>> DurationPresetButtons;
	/** Hidden for a hotlap, which has no field. */
	UPROPERTY(Transient) TObjectPtr<UWidget> FieldSection;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> FieldInfoText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> FieldLabelText;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> FieldMinus;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> FieldPlus;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> FieldValueText;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> AiMinus;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> AiPlus;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> AiValueText;
	/** The AI field's level (ApexAiSkill), Mixed below the bottom step; the label names its band. */
	UPROPERTY(Transient) TObjectPtr<UTextBlock> AiSkillLabelText;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> AiSkillMinus;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> AiSkillPlus;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> AiSkillValueText;
	UPROPERTY(Transient) TArray<TObjectPtr<UApexButtonWidget>> AssistPresetButtons;
	/** One toggle per driving aid the session may allow, in EApexAssistChip order. */
	UPROPERTY(Transient) TArray<TObjectPtr<UApexButtonWidget>> AssistButtons;
	/** The session's damage rule, one tile per EApexDamageLevel in enum order. */
	UPROPERTY(Transient) TArray<TObjectPtr<UApexButtonWidget>> DamageButtons;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> SetupInfoText;
	UPROPERTY(Transient) TObjectPtr<UVerticalBox> SetupRows;
	/** Stock, Custom while the working setup is an unsaved one, then the car's saved setups. */
	UPROPERTY(Transient) TArray<TObjectPtr<UApexButtonWidget>> SetupButtons;
	/** Per setup chip: invalid for Stock, the Custom marker, else the saved setup's id. */
	TArray<FGuid> SetupButtonIds;
	/** What the chips were built from; a rebuild only when it changes. */
	FString SetupSignature;
	/** "Stock", the saved setup's name or "Custom", for the footer. */
	FString CurrentSetupLabel;

	// --- Starting order (Race tab, races only) ------------------------------------
	UPROPERTY(Transient) TObjectPtr<UWidget> StartOrderSection;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> StartOrderInfoText;
	UPROPERTY(Transient) TObjectPtr<UVerticalBox> StartOrderRows;
	/** Seating order, Custom while the order is an edited one, then the track's stored results. */
	UPROPERTY(Transient) TArray<TObjectPtr<UApexButtonWidget>> StartOrderButtons;
	/** Per chip: empty for the seating order, `custom`, or a stored result's id. */
	TArray<FString> StartOrderIds;
	FString StartOrderSignature;
	/** The track the stored results were last asked for, so a screen open before the connection is up asks once it is. */
	FString QualifyingRequestedFor;
	/** -ApexCreateLoadResult= is applied once. */
	bool bLoadedFromSwitch = false;
	/** Your slot, earlier and later; the keyboard's and pad's way to move up the grid. */
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> YourSlotEarlier;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> YourSlotLater;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> YourSlotValueText;
	/** The driver a click picked up (0-based slot), INDEX_NONE for none. */
	int32 PickedSlot = INDEX_NONE;
	/** While the mouse is down on a driver: where it started and which slot it is over. */
	int32 DragFrom = INDEX_NONE;
	int32 DragHover = INDEX_NONE;

	// --- Conditions tab -------------------------------------------------------
	UPROPERTY(Transient) TObjectPtr<USlider> TimeOfDaySlider;
	UPROPERTY(Transient) TObjectPtr<UProgressBar> TimeOfDayFill;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> TimeOfDayValue;
	UPROPERTY(Transient) TArray<TObjectPtr<UApexButtonWidget>> TimePresetButtons;
	/** One tile per EApexWeather, in enum order. */
	UPROPERTY(Transient) TArray<TObjectPtr<UApexButtonWidget>> WeatherButtons;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> AirAutoButton;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> AirMinus;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> AirPlus;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> AirValueText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> AirNoteText;
	/** The wind's strengths (WindPresetsKph), then Auto. */
	UPROPERTY(Transient) TArray<TObjectPtr<UApexButtonWidget>> WindButtons;
	/** Where it blows from, eight ways round the start straight. */
	UPROPERTY(Transient) TArray<TObjectPtr<UApexButtonWidget>> WindFromButtons;
	UPROPERTY(Transient) TObjectPtr<UWidget> WindNeedle;
	UPROPERTY(Transient) TObjectPtr<UBorder> WindNeedleShaft;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> WindFromText;

	// --- Footer ---------------------------------------------------------------
	UPROPERTY(Transient) TObjectPtr<UBorder> StatusDot;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> StatusLine;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> SummaryText;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> CancelButton;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> CreateButtonWidget;

	UPROPERTY(Transient) TObjectPtr<UTexture2D> Gradient;
	UPROPERTY(Transient) TObjectPtr<UTexture2D> Rain;

	/** Server caps: 20 on the grid, so 19 AI at most alongside one human. */
	static constexpr int32 MaxPlayersCeiling = 20;
	static constexpr int32 LapsCeiling = 50;
	/** The clock moves in quarter hours: one keypress, one step, 96 to the day. */
	static constexpr int32 TimeOfDayStepMinutes = 15;
	static constexpr int32 TimeOfDaySteps = 24 * 60 / TimeOfDayStepMinutes;
};
