#pragma once

#include "CoreMinimal.h"
#include "ApexSettingsSubsystem.h"
#include "Blueprint/UserWidget.h"
#include "InputCoreTypes.h"

#include "ApexSettingsWidget.generated.h"

class UApexButtonWidget;
class UApexSegmentedWidget;
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

/** The settings overlay's three pages. */
UENUM(BlueprintType)
enum class EApexSettingsTab : uint8
{
	Gameplay,
	Graphics,
	Controls,
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
class APEXSIM_API UApexSettingsWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	UApexSettingsWidget(const FObjectInitializer& ObjectInitializer);

	virtual void NativeOnInitialized() override;
	virtual FReply NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent) override;
	virtual FReply NativeOnAnalogValueChanged(const FGeometry& InGeometry, const FAnalogInputEvent& InAnalogEvent) override;
	virtual FReply NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;

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
	UFUNCTION() void HandleSteeringChanged(float Value);
	UFUNCTION() void HandleDeadzoneChanged(float Value);
	UFUNCTION() void HandleVibrationChanged(float Value);

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
	UWidget* BuildGraphicsPage();
	UWidget* BuildControlsPage();
	UWidget* BuildBindingsGrid();

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
		const FString& PendingNote = FString());

	/** A segmented control registered under an id the single handler knows. */
	UApexSegmentedWidget* MakeSegment(FName ControlId, const TArray<FString>& Options, int32 Selected, float Width = 118.0f);

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
	void FinishListening(const FKey& Key, bool bCancelled);

	/** True for keys that must never become a binding. */
	static bool IsRejectedBindingKey(const FKey& Key);

	UApexSettingsSubsystem* GetSettings() const;

	// --- Widgets --------------------------------------------------------------

	UPROPERTY(Transient) TObjectPtr<UWidgetSwitcher> PageHost;
	UPROPERTY(Transient) TArray<TObjectPtr<UApexButtonWidget>> RailButtons;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> HeaderContextText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> FooterStatusText;

	UPROPERTY(Transient) TMap<FName, TObjectPtr<UApexSegmentedWidget>> Segments;

	UPROPERTY(Transient) TObjectPtr<USlider> AiSkillSlider;
	UPROPERTY(Transient) TObjectPtr<UProgressBar> AiSkillFill;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> AiSkillValue;

	UPROPERTY(Transient) TObjectPtr<USlider> MotionBlurSlider;
	UPROPERTY(Transient) TObjectPtr<UProgressBar> MotionBlurFill;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> MotionBlurValue;

	UPROPERTY(Transient) TObjectPtr<USlider> FovSlider;
	UPROPERTY(Transient) TObjectPtr<UProgressBar> FovFill;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> FovValue;

	UPROPERTY(Transient) TObjectPtr<USlider> SteeringSlider;
	UPROPERTY(Transient) TObjectPtr<UProgressBar> SteeringFill;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> SteeringValue;

	UPROPERTY(Transient) TObjectPtr<USlider> DeadzoneSlider;
	UPROPERTY(Transient) TObjectPtr<UProgressBar> DeadzoneFill;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> DeadzoneValue;

	UPROPERTY(Transient) TObjectPtr<USlider> VibrationSlider;
	UPROPERTY(Transient) TObjectPtr<UProgressBar> VibrationFill;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> VibrationValue;

	UPROPERTY(Transient) TObjectPtr<UComboBoxString> DisplayModeBox;
	UPROPERTY(Transient) TObjectPtr<UComboBoxString> ResolutionBox;
	UPROPERTY(Transient) TObjectPtr<UComboBoxString> FrameLimitBox;
	UPROPERTY(Transient) TObjectPtr<UComboBoxString> ShadowsBox;
	UPROPERTY(Transient) TObjectPtr<UComboBoxString> AntiAliasingBox;
	UPROPERTY(Transient) TObjectPtr<UComboBoxString> TexturesBox;

	UPROPERTY(Transient) TObjectPtr<UTextBlock> DeviceCountText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> GamepadStateText;
	UPROPERTY(Transient) TArray<TObjectPtr<UApexButtonWidget>> BindingChips;

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
};
