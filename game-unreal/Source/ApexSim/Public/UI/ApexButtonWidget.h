#pragma once

#include "CoreMinimal.h"
#include "Audio/ApexUiSound.h"
#include "Blueprint/UserWidget.h"

#include "ApexButtonWidget.generated.h"

class UBorder;
class UHorizontalBox;
class USizeBox;
class UTextBlock;
class UWidget;

/** How a button is drawn. The design has exactly these four weights. */
UENUM(BlueprintType)
enum class EApexButtonVariant : uint8
{
	/** Filled accent. One per screen — the thing the screen exists to do. */
	Primary,
	/** Surface card with a label left and an optional badge right. Rail items, list rows. */
	Panel,
	/** Outline only. Secondary actions sitting next to a Primary. */
	Ghost,
	/** Dim and inert: a feature that is announced but not built yet. */
	Locked,
	/** No fill, no outline — an inline link or a header's back control. */
	Bare,
};

/** Everything that varies between buttons, so Setup stays one readable block. */
USTRUCT(BlueprintType)
struct FApexButtonSpec
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|UI")
	FString Label;

	/** Second line under the label, for rows that carry a spec line. */
	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|UI")
	FString SubLabel;

	/** Right-aligned metadata: "3 LIVE", "HOST", "LOCKED". */
	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|UI")
	FString Badge;

	/** Keyboard shortcut advertised as a chip on the right, e.g. "ENTER". */
	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|UI")
	FString KeyCap;

	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|UI")
	EApexButtonVariant Variant = EApexButtonVariant::Panel;

	/** Identifies the button in the owning screen's single activation handler. */
	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|UI")
	FName ActionId;

	/** Defaults to Metrics::RowHeight; Primary buttons are taller. */
	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|UI")
	float Height = -1.0f;

	/** Ghost/Primary buttons centre their label; rows keep it left. */
	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|UI")
	bool bCentreLabel = false;

	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|UI")
	FLinearColor BadgeColour = FLinearColor(0.43f, 0.43f, 0.46f);

	/** Key cap before the label rather than after — "[ESC] Back". */
	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|UI")
	bool bKeyCapLeading = false;

	/** Overrides the variant's label size. Filter chips and links are smaller. */
	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|UI")
	float LabelSize = -1.0f;

	/** Overrides the variant's label colour. Zero alpha means "use the default". */
	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|UI")
	FLinearColor LabelColour = FLinearColor(0.0f, 0.0f, 0.0f, 0.0f);

	/**
	 * What activating the button sounds like. Accept for nearly everything;
	 * a button whose job is to go back sets Back, so that it sounds the same
	 * as the Escape it advertises.
	 */
	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|UI")
	EApexUiSound Sound = EApexUiSound::Accept;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FApexOnButtonActivated, UApexButtonWidget*, Button);

/**
 * The only clickable thing in the redesigned menu.
 *
 * Deliberately not a UButton: the design needs a focus state (the accent bar on
 * the left of a rail item), and Slate's button style has brushes for hover and
 * press but none for focus. Handling focus, hover and activation here also means
 * keyboard and mouse land on exactly the same path.
 */
UCLASS()
class APEXSIM_API UApexButtonWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	UApexButtonWidget(const FObjectInitializer& ObjectInitializer);

	UPROPERTY(BlueprintAssignable, Category = "ApexSim|UI")
	FApexOnButtonActivated OnActivated;

	/** Applies a spec. Safe to call before or after the widget tree is built. */
	UFUNCTION(BlueprintCallable, Category = "ApexSim|UI")
	void Setup(const FApexButtonSpec& InSpec);

	/** Updates the right-hand badge without rebuilding anything else. */
	UFUNCTION(BlueprintCallable, Category = "ApexSim|UI")
	void SetBadge(const FString& InBadge, const FLinearColor& InColour);

	UFUNCTION(BlueprintCallable, Category = "ApexSim|UI")
	void SetLabel(const FString& InLabel);

	/**
	 * Marks this as the chosen one of a set — an accent outline that survives the
	 * keyboard moving elsewhere. Distinct from focus, which is transient.
	 */
	UFUNCTION(BlueprintCallable, Category = "ApexSim|UI")
	void SetSelected(bool bInSelected);

	UFUNCTION(BlueprintPure, Category = "ApexSim|UI")
	bool IsSelected() const { return bSelected; }

	/** Repoints an existing button at a different action, without rebuilding it. */
	UFUNCTION(BlueprintCallable, Category = "ApexSim|UI")
	void SetActionId(FName InActionId) { Spec.ActionId = InActionId; }

	UFUNCTION(BlueprintPure, Category = "ApexSim|UI")
	FName GetActionId() const { return Spec.ActionId; }

	UFUNCTION(BlueprintPure, Category = "ApexSim|UI")
	bool IsInteractive() const { return Spec.Variant != EApexButtonVariant::Locked; }

protected:
	virtual void NativeOnInitialized() override;
	virtual FReply NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;
	virtual void NativeOnMouseEnter(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;
	virtual void NativeOnMouseLeave(const FPointerEvent& InMouseEvent) override;
	virtual void NativeOnAddedToFocusPath(const FFocusEvent& InFocusEvent) override;
	virtual void NativeOnRemovedFromFocusPath(const FFocusEvent& InFocusEvent) override;
	virtual FReply NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent) override;
	virtual FReply NativeOnAnalogValueChanged(const FGeometry& InGeometry, const FAnalogInputEvent& InAnalogEvent) override;

private:
	void Activate();
	/** Repaints background, accent bar and text for the current hover/focus state. */
	void ApplyState();

	UPROPERTY()
	FApexButtonSpec Spec;

	UPROPERTY(Transient)
	TObjectPtr<USizeBox> SizeBox;

	UPROPERTY(Transient)
	TObjectPtr<UBorder> Background;

	/** The 3px accent stripe down the left edge, shown only when focused. */
	UPROPERTY(Transient)
	TObjectPtr<UWidget> AccentBar;

	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> LabelText;

	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> SubLabelText;

	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> BadgeText;

	UPROPERTY(Transient)
	TObjectPtr<UHorizontalBox> ContentRow;

	bool bHovered = false;
	bool bFocused = false;
	bool bSelected = false;
};
