#pragma once

#include "CoreMinimal.h"
#include "ApexSettingsSubsystem.h"
#include "InputCoreTypes.h"
#include "UI/ApexNavigation.h"

#include "ApexSettingsWidget.generated.h"

class UApexButtonWidget;
class UApexSegmentedWidget;
class UApexStepperWidget;
class UBorder;
class UComboBoxString;
class UHorizontalBox;
class UProgressBar;
class USlider;
class UTextBlock;
class UVerticalBox;
class UWidget;
class UWidgetSwitcher;

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FApexOnSettingsClosed);

/** The settings overlay's seven pages. */
UENUM(BlueprintType)
enum class EApexSettingsTab : uint8
{
	Gameplay,
	/** The driving aids: what the server runs for this car, and the racing line. */
	Assists,
	Graphics,
	Camera,
	Controls,
	/** Wheels and pedals: the devices, their forces and their own bindings. */
	Wheel,
	Audio,
};

/**
 * The settings overlay, reachable from the pause menu.
 *
 * It holds no state: every control reads its value from UApexSettingsSubsystem
 * on open and writes straight back through it, which is what makes "applies
 * immediately" true rather than a label. The slot is flushed on close.
 *
 * The controls page can be listening for a key, which is a modal state inside a
 * modal screen — while it is on, every key event belongs to the rebind and
 * nothing else on the page may act on one.
 */
UCLASS()
class APEXSIM_API UApexSettingsWidget : public UApexNavigableWidget
{
	GENERATED_BODY()

public:
	UApexSettingsWidget(const FObjectInitializer& ObjectInitializer);

	virtual void NativeOnInitialized() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;
	virtual FReply NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent) override;
	virtual FReply NativeOnAnalogValueChanged(const FGeometry& InGeometry, const FAnalogInputEvent& InAnalogEvent) override;
	virtual FReply NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;

	// --- Navigation ---------------------------------------------------------
	//
	// The rail's tab for the current page is where focus starts; Right from
	// it enters the page, and Slate's geometric search does the rest across
	// the rows. Tab and the shoulders change page from anywhere.

	virtual void FocusDefault() override;
	virtual bool HandleNavigation(EUINavigation Direction, UWidget* Source) override;
	virtual bool HandleBack() override;

	/** Fires when the overlay wants to go away — Escape, Back, or Back to race. */
	UPROPERTY(BlueprintAssignable, Category = "ApexSim|UI")
	FApexOnSettingsClosed OnClosed;

	UFUNCTION(BlueprintCallable, Category = "ApexSim|UI")
	void Open(EApexSettingsTab Tab = EApexSettingsTab::Gameplay);

	UFUNCTION(BlueprintCallable, Category = "ApexSim|UI")
	void Close();

	UFUNCTION(BlueprintPure, Category = "ApexSim|UI")
	bool IsOpen() const { return bOpen; }

protected:
	// Every control needs its own handler: UMG's value-changed delegates are
	// dynamic and carry no sender, so there is nothing to switch on.

	UFUNCTION() void HandleRailActivated(UApexButtonWidget* Button);
	UFUNCTION() void HandleFooterActivated(UApexButtonWidget* Button);
	UFUNCTION() void HandleBindingActivated(UApexButtonWidget* Button);
	UFUNCTION() void HandleSegmentChosen(UApexSegmentedWidget* Control, int32 Index);

	UFUNCTION() void HandleAiSkillChanged(float Value);
	UFUNCTION() void HandleMotionBlurChanged(float Value);
	UFUNCTION() void HandleFovChanged(float Value);
	UFUNCTION() void HandleSeatForwardChanged(float Value);
	UFUNCTION() void HandleSeatHeightChanged(float Value);
	UFUNCTION() void HandleViewPitchChanged(float Value);
	UFUNCTION() void HandleHorizonLockChanged(float Value);
	UFUNCTION() void HandleHeadMotionChanged(float Value);
	UFUNCTION() void HandleLookToApexChanged(float Value);
	UFUNCTION() void HandleSteeringChanged(float Value);
	UFUNCTION() void HandleDeadzoneChanged(float Value);
	UFUNCTION() void HandleVibrationChanged(float Value);
	UFUNCTION() void HandleWheelForceChanged(float Value);
	UFUNCTION() void HandleWheelRoadChanged(float Value);
	UFUNCTION() void HandleWheelDampingChanged(float Value);
	UFUNCTION() void HandleWheelTestActivated(UApexButtonWidget* Button);
	UFUNCTION() void HandleMasterVolumeChanged(float Value);
	UFUNCTION() void HandleUiVolumeChanged(float Value);
	UFUNCTION() void HandleEngineVolumeChanged(float Value);
	UFUNCTION() void HandleRoadVolumeChanged(float Value);
	UFUNCTION() void HandleOtherCarsVolumeChanged(float Value);

	UFUNCTION() void HandleDisplayModeChanged(FString Item, ESelectInfo::Type SelectType);
	UFUNCTION() void HandleResolutionChanged(FString Item, ESelectInfo::Type SelectType);
	UFUNCTION() void HandleFrameLimitChanged(FString Item, ESelectInfo::Type SelectType);
	UFUNCTION() void HandleShadowsChanged(FString Item, ESelectInfo::Type SelectType);
	UFUNCTION() void HandleAntiAliasingChanged(FString Item, ESelectInfo::Type SelectType);
	UFUNCTION() void HandleTexturesChanged(FString Item, ESelectInfo::Type SelectType);

private:
	// --- Construction ---------------------------------------------------------

	void BuildOverlay();
	UWidget* BuildHeader();
	UWidget* BuildRail();
	UWidget* BuildFooter();
	UWidget* BuildGameplayPage();
	UWidget* BuildAssistsPage();
	UWidget* BuildGraphicsPage();
	UWidget* BuildCameraPage();
	UWidget* BuildControlsPage();
	UWidget* BuildWheelPage();
	UWidget* BuildAudioPage();
	UWidget* BuildBindingsGrid();

	/** The wheel column's slots, with a live meter beside each axis. */
	UWidget* BuildWheelBindings();

	/** Redraws the device cards; the attached set changes while the page is open. */
	void RefreshWheelDevices();

	/** Moves the meters to what the wheel is reading right now. */
	void RefreshWheelMeters();

	/** Section caption above a group of rows. */
	UWidget* MakeSectionLabel(const FString& Text);

	/**
	 * One settings row: name and description on the left, a control on the
	 * right. Every row on every page is this shape, which is what keeps the
	 * three pages looking like one screen.
	 *
	 * PendingNote marks a row whose value is stored and saved but has nothing to
	 * act on it yet — the wire protocol carries no driving-aid fields, so
	 * traction control and ABS are choices the server never hears about. Such a
	 * row is dimmed and says so, rather than looking like a working control that
	 * quietly does nothing.
	 */
	UWidget* MakeRow(
		const FString& Label,
		const FString& Description,
		UWidget* Control,
		const FString& PendingNote = FString(),
		float Height = 0.0f,
		UWidget* TitleBadge = nullptr);

	/**
	 * The badge an assists row shows while the session's host has locked that
	 * aid: collapsed until RefreshAssistLocks says otherwise. Registered under
	 * the row's segment id.
	 */
	UWidget* MakeAssistLockBadge(FName ControlId);

	/**
	 * Dims and disables every assists row the current session forbids, and
	 * shows its badge. The server enforces the rule; this keeps the page from
	 * looking like a control that quietly does nothing.
	 */
	void RefreshAssistLocks();

	/** A segmented control registered under an id the single handler knows. */
	UApexSegmentedWidget* MakeSegment(FName ControlId, const TArray<FString>& Options, int32 Selected, float Width = 118.0f);

	/**
	 * A slider track with its value read-out beside it, sized for a row's
	 * control cell. The out-params are the members a handler updates.
	 */
	UWidget* MakeSliderCell(
		TObjectPtr<USlider>& OutSlider,
		TObjectPtr<UProgressBar>& OutFill,
		TObjectPtr<UTextBlock>& OutValue,
		float Width);

	/**
	 * The common tail of every slider handler: the fill and read-out follow
	 * the thumb, the click plays, and the footer's change count moves.
	 */
	void ReflectSlider(UProgressBar* Fill, UTextBlock* Value, float Alpha, const FString& Display);

	/** A binding chip. Its action id encodes the slot it edits. */
	UApexButtonWidget* MakeBindingChip(FName ActionId, int32 Slot);

	// --- State ----------------------------------------------------------------

	void ShowTab(EApexSettingsTab Tab);
	/** Pushes every control's value back from the subsystem. */
	void RefreshFromSettings();
	void RefreshHeaderContext();
	void RefreshFooter();
	void RefreshBindingChips();

	/** Starts listening for the key that will fill a slot. */
	void BeginListening(FName ActionId, int32 Slot);
	/** Stores the captured key, or cancels when it is invalid. */
	void FinishListening(const FKey& Key, bool bCancelled, bool bInvert = false);

	/** True for keys that must never become a binding. */
	static bool IsRejectedBindingKey(const FKey& Key);

	/**
	 * True when the key is for the column being edited: the wheel column takes
	 * DirectInput keys and nothing else, and the other two take everything but.
	 * Binding a wheel button in the gamepad column would put one device in two
	 * columns, where only one of them can be shown.
	 */
	bool IsKeyForListeningSlot(const FKey& Key) const;

	UApexSettingsSubsystem* GetSettings() const;

	// --- Widgets --------------------------------------------------------------

	UPROPERTY(Transient) TObjectPtr<UWidgetSwitcher> PageHost;
	UPROPERTY(Transient) TArray<TObjectPtr<UApexButtonWidget>> RailButtons;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> HeaderContextText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> FooterStatusText;

	UPROPERTY(Transient) TMap<FName, TObjectPtr<UApexSegmentedWidget>> Segments;
	/** Lock badges on the assists page, by the row's segment id. */
	UPROPERTY(Transient) TMap<FName, TObjectPtr<UWidget>> AssistLocks;

	UPROPERTY(Transient) TObjectPtr<USlider> AiSkillSlider;
	UPROPERTY(Transient) TObjectPtr<UProgressBar> AiSkillFill;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> AiSkillValue;

	UPROPERTY(Transient) TObjectPtr<USlider> MotionBlurSlider;
	UPROPERTY(Transient) TObjectPtr<UProgressBar> MotionBlurFill;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> MotionBlurValue;

	UPROPERTY(Transient) TObjectPtr<USlider> FovSlider;
	UPROPERTY(Transient) TObjectPtr<UProgressBar> FovFill;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> FovValue;

	UPROPERTY(Transient) TObjectPtr<USlider> SeatForwardSlider;
	UPROPERTY(Transient) TObjectPtr<UProgressBar> SeatForwardFill;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> SeatForwardValue;

	UPROPERTY(Transient) TObjectPtr<USlider> SeatHeightSlider;
	UPROPERTY(Transient) TObjectPtr<UProgressBar> SeatHeightFill;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> SeatHeightValue;

	UPROPERTY(Transient) TObjectPtr<USlider> ViewPitchSlider;
	UPROPERTY(Transient) TObjectPtr<UProgressBar> ViewPitchFill;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> ViewPitchValue;

	UPROPERTY(Transient) TObjectPtr<USlider> HorizonLockSlider;
	UPROPERTY(Transient) TObjectPtr<UProgressBar> HorizonLockFill;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> HorizonLockValue;

	UPROPERTY(Transient) TObjectPtr<USlider> HeadMotionSlider;
	UPROPERTY(Transient) TObjectPtr<UProgressBar> HeadMotionFill;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> HeadMotionValue;

	UPROPERTY(Transient) TObjectPtr<USlider> LookToApexSlider;
	UPROPERTY(Transient) TObjectPtr<UProgressBar> LookToApexFill;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> LookToApexValue;

	UPROPERTY(Transient) TObjectPtr<USlider> SteeringSlider;
	UPROPERTY(Transient) TObjectPtr<UProgressBar> SteeringFill;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> SteeringValue;

	UPROPERTY(Transient) TObjectPtr<USlider> DeadzoneSlider;
	UPROPERTY(Transient) TObjectPtr<UProgressBar> DeadzoneFill;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> DeadzoneValue;

	UPROPERTY(Transient) TObjectPtr<USlider> VibrationSlider;
	UPROPERTY(Transient) TObjectPtr<UProgressBar> VibrationFill;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> VibrationValue;

	UPROPERTY(Transient) TObjectPtr<USlider> MasterVolumeSlider;
	UPROPERTY(Transient) TObjectPtr<UProgressBar> MasterVolumeFill;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> MasterVolumeValue;

	UPROPERTY(Transient) TObjectPtr<USlider> UiVolumeSlider;
	UPROPERTY(Transient) TObjectPtr<UProgressBar> UiVolumeFill;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> UiVolumeValue;

	UPROPERTY(Transient) TObjectPtr<USlider> EngineVolumeSlider;
	UPROPERTY(Transient) TObjectPtr<UProgressBar> EngineVolumeFill;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> EngineVolumeValue;

	UPROPERTY(Transient) TObjectPtr<USlider> OtherCarsVolumeSlider;
	UPROPERTY(Transient) TObjectPtr<UProgressBar> OtherCarsVolumeFill;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> OtherCarsVolumeValue;

	UPROPERTY(Transient) TObjectPtr<USlider> RoadVolumeSlider;
	UPROPERTY(Transient) TObjectPtr<UProgressBar> RoadVolumeFill;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> RoadVolumeValue;

	UPROPERTY(Transient) TObjectPtr<UComboBoxString> DisplayModeBox;
	UPROPERTY(Transient) TObjectPtr<UComboBoxString> ResolutionBox;
	UPROPERTY(Transient) TObjectPtr<UComboBoxString> FrameLimitBox;
	UPROPERTY(Transient) TObjectPtr<UComboBoxString> ShadowsBox;
	UPROPERTY(Transient) TObjectPtr<UComboBoxString> AntiAliasingBox;
	UPROPERTY(Transient) TObjectPtr<UComboBoxString> TexturesBox;

	UPROPERTY(Transient) TObjectPtr<USlider> WheelForceSlider;
	UPROPERTY(Transient) TObjectPtr<UProgressBar> WheelForceFill;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> WheelForceValue;

	UPROPERTY(Transient) TObjectPtr<USlider> WheelRoadSlider;
	UPROPERTY(Transient) TObjectPtr<UProgressBar> WheelRoadFill;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> WheelRoadValue;

	UPROPERTY(Transient) TObjectPtr<USlider> WheelDampingSlider;
	UPROPERTY(Transient) TObjectPtr<UProgressBar> WheelDampingFill;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> WheelDampingValue;

	UPROPERTY(Transient) TObjectPtr<UTextBlock> DeviceCountText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> GamepadStateText;
	UPROPERTY(Transient) TArray<TObjectPtr<UApexButtonWidget>> BindingChips;

	/** The wheel page's device cards, rebuilt whenever the attached set moves. */
	UPROPERTY(Transient) TObjectPtr<UHorizontalBox> WheelDeviceRow;
	UPROPERTY(Transient) TArray<TObjectPtr<UProgressBar>> WheelMeterBars;
	UPROPERTY(Transient) TArray<TObjectPtr<UTextBlock>> WheelMeterValues;

	/** The "press any key" card. Hidden unless a rebind is in progress. */
	UPROPERTY(Transient) TObjectPtr<UWidget> ListenOverlay;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> ListenTitleText;

	// --- Flags ----------------------------------------------------------------

	EApexSettingsTab CurrentTab = EApexSettingsTab::Gameplay;
	bool bOpen = false;

	/** Suppresses write-back while controls are being filled from the slot. */
	bool bRefreshing = false;

	bool bListening = false;
	FName ListeningAction;
	int32 ListeningSlot = 0;

	/**
	 * Where each axis was when the capture opened.
	 *
	 * An axis is bound by how far it MOVES, not by where it sits: a pedal
	 * rests at one end of its travel and reports that every frame, so a
	 * threshold on the value alone would bind the first pedal it heard from.
	 * The direction of the move is also the answer to which way round the axis
	 * is (FApexKeyBinding::bInvert).
	 */
	TMap<FKey, float> ListenBaselines;

	/** The device list this page was drawn for; a change redraws the cards. */
	uint32 WheelDevicesSerial = 0;
};
