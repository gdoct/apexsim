#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Styling/SlateBrush.h"
#include "UI/ApexGlobe.h"

#include "ApexGlobeWidget.generated.h"

class UTexture2D;

/** One circuit on the globe. */
struct FApexGlobePin
{
	/** The track id the pin stands for. */
	FString Id;
	/** What its label says: the catalog's display name. */
	FString Name;
	double LonDeg = 0.0;
	double LatDeg = 0.0;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FApexGlobeSelectionChanged, const FString&, TrackId);

/**
 * Garage > Tracks' globe: dotted land, a graticule and the atmosphere's rim,
 * one pin per circuit (docs/game/client.md). Painted, like the minimap, from
 * the maths in ApexGlobe.h.
 *
 * Mouse: drag turns it (and throws it), the wheel zooms, a click on a pin
 * chooses it. Keys and pad, while it has focus: the D-pad or arrows jump to
 * the nearest pin that way (off the last one, the owning surface takes the
 * move), Accept chooses the focused pin, the left stick turns the globe, the
 * triggers or + / - zoom, pad Y or Home resets the view. It turns by itself
 * while idle, and stops under the pointer or with a circuit chosen.
 */
UCLASS()
class APEXSIM_API UApexGlobeWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	UApexGlobeWidget(const FObjectInitializer& ObjectInitializer);

	/** The circuits; a selection or focus whose pin is gone is dropped. */
	void SetPins(const TArray<FApexGlobePin>& InPins);

	/** Marks a circuit chosen and flies to it (an id with no pin only marks nothing chosen). */
	void Select(const FString& TrackId, bool bFly = true);
	/** Nothing chosen; the globe stays where it is and starts turning again. */
	void ClearSelection();
	/** Home view, nothing chosen. */
	void ResetView();
	/** Whether it turns by itself while idle (on by default; off for a still screenshot). */
	void SetIdleSpin(bool bOn) { bIdleSpin = bOn; }
	/** One zoom step in (1) or out (-1). */
	void StepZoom(int32 Direction);

	const FString& GetSelectedId() const { return SelectedId; }
	bool HasPins() const { return Pins.Num() > 0; }
	/** The pin the keys act on; empty when none. */
	FString GetFocusedPinId() const;
	/** Puts the keys' pin on a circuit (when it has one) and focus on the globe. */
	bool FocusPin(const FString& TrackId);

	/** The view, for the console's apexsim.ui.Globe and tests. */
	ApexGlobe::FView& GetView() { return View; }

	/** The player chose on the globe: a pin clicked or accepted (its id), or the view reset (empty). */
	UPROPERTY(BlueprintAssignable)
	FApexGlobeSelectionChanged OnSelectionChanged;

protected:
	virtual void NativeConstruct() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;
	virtual int32 NativePaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
		FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;

	virtual FReply NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;
	virtual FReply NativeOnMouseMove(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;
	virtual FReply NativeOnMouseButtonUp(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;
	virtual FReply NativeOnMouseWheel(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;
	virtual void NativeOnMouseEnter(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;
	virtual void NativeOnMouseLeave(const FPointerEvent& InMouseEvent) override;
	virtual FCursorReply NativeOnCursorQuery(const FGeometry& InGeometry, const FPointerEvent& InCursorEvent) override;
	virtual FReply NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent) override;
	virtual FReply NativeOnAnalogValueChanged(const FGeometry& InGeometry, const FAnalogInputEvent& InAnalogEvent) override;
	virtual FReply NativeOnFocusReceived(const FGeometry& InGeometry, const FFocusEvent& InFocusEvent) override;
	virtual void NativeOnFocusLost(const FFocusEvent& InFocusEvent) override;

private:
	/** Where each pin is this frame, in local units, and what shows. */
	struct FPinState
	{
		FVector2D Base = FVector2D::ZeroVector;
		FVector2D Head = FVector2D::ZeroVector;
		double Depth = 0.0;
		bool bShown = false;
		bool bLabel = false;
		/** Label to the left of the head (near the right edge). */
		bool bLabelLeft = false;
		FVector2D LabelSize = FVector2D::ZeroVector;
	};

	/** Projects every pin and places the labels for Size; fills States. */
	void Layout(const FVector2D& Size) const;
	/** The globe's radius at zoom 1, local units. */
	static double BaseRadius(const FVector2D& Size);
	/** The pin under a local point (head or label), INDEX_NONE if none. */
	int32 HitPin(const FVector2D& Local) const;
	void ChoosePin(int32 Index);
	/** The keys' pin, or the visible pin nearest the middle when there is none. */
	int32 KeysPinOrNearest() const;

	void MakeTextures();

	TArray<FApexGlobePin> Pins;
	mutable TArray<FPinState> States;
	mutable FVector2D LaidOutSize = FVector2D::ZeroVector;

	ApexGlobe::FView View;
	FString SelectedId;
	int32 HoveredPin = INDEX_NONE;
	int32 KeysPin = INDEX_NONE;
	bool bHasKeyFocus = false;
	/** Seconds since construction: the selected pin's pulse. */
	double Clock = 0.0;

	// The pointer: a press becomes a drag once it moves a few pixels.
	bool bPressed = false;
	bool bDragMoved = false;
	int32 PressedPin = INDEX_NONE;
	FVector2D PressAt = FVector2D::ZeroVector;
	FVector2D LastPointer = FVector2D::ZeroVector;
	double LastMoveSeconds = 0.0;
	bool bIdleSpin = true;
	/** The last input was a key or the pad: the keys' pin shows its ring. */
	bool bKeysActive = false;
	double TriggerIn = 0.0;
	double TriggerOut = 0.0;

	UPROPERTY(Transient) TObjectPtr<UTexture2D> OceanTexture;
	UPROPERTY(Transient) TObjectPtr<UTexture2D> AtmosphereTexture;
	FSlateBrush OceanBrush;
	FSlateBrush AtmosphereBrush;
	FSlateBrush CircleBrush;
	FSlateBrush BoxBrush;
};
