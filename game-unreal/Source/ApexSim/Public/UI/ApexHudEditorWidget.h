#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Hud/ApexHudLayout.h"
#include "Styling/SlateBrush.h"

#include "ApexHudEditorWidget.generated.h"

class UApexButtonWidget;
class UApexHudEditorWidget;
class UApexHudWidget;
class UTextBlock;
class UVerticalBox;
class UWidget;

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FApexOnHudEditorClosed);

/**
 * Draws what the HUD editor shows over the HUD: a frame round each panel
 * (the selected one in the accent with its resize handle and its name), the
 * guides a drag snaps to, and a faint third-of-the-screen grid, which is
 * what decides the corner a moved panel keeps to. Under the editor's own
 * card, so the frames never cover its buttons. Paint only, no input.
 */
UCLASS()
class APEXSIM_API UApexHudFrameLayer : public UUserWidget
{
	GENERATED_BODY()

public:
	UApexHudFrameLayer(const FObjectInitializer& ObjectInitializer);

	TWeakObjectPtr<UApexHudEditorWidget> Editor;

protected:
	virtual int32 NativePaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
		FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle,
		bool bParentEnabled) const override;

private:
	// Draw elements hold on to their brush until the frame is rendered, so the
	// brushes live here rather than on the stack of NativePaint.
	FSlateBrush FrameBrush;
	FSlateBrush HoverBrush;
	FSlateBrush SelectedBrush;
	FSlateBrush HandleBrush;
	FSlateBrush GuideBrush;
	FSlateBrush GridBrush;
	FSlateBrush TagBrush;
};

/**
 * The HUD editor: move, resize, add and remove the HUD's panels with the
 * mouse, keyboard or pad, over the race or (from the main menu) over a
 * made-up one. Opened from Settings > Gameplay > HUD layout.
 *
 * It edits the HUD widget's FApexHudLayout in place, so what is on screen is
 * the result; Save writes it to `custom/layout.json` and Cancel puts back the
 * layout it opened with. Moving a panel first pins every panel where it is
 * (UApexHudWidget::PinAll), so the rest of its region does not close up
 * behind it.
 *
 * Mouse: drag a panel to move it (it snaps to the gutters, the centre lines
 * and the other panels' edges; Shift to place freely), drag its corner to
 * resize it, click empty space to deselect. Keys: Tab / Shift+Tab (shoulders)
 * pick a panel, arrows (D-pad) nudge it (Shift for 10), [ ] (triggers) size
 * it, Del (Y) shows or hides it, R (X) puts it back where it shipped, H hides
 * this card, Enter (Start) saves, Esc (B) cancels.
 */
UCLASS()
class APEXSIM_API UApexHudEditorWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	UApexHudEditorWidget(const FObjectInitializer& ObjectInitializer);

	UPROPERTY(BlueprintAssignable, Category = "ApexSim|UI")
	FApexOnHudEditorClosed OnClosed;

	void Open(UApexHudWidget* InHud);
	/** Saves the layout or puts back the one it opened with, then closes. */
	void Close(bool bSave);
	bool IsOpen() const { return bOpen; }
	void FocusDefault();

	// --- What the keys, the buttons and the console all go through ----------------

	bool SelectById(const FString& Id);
	void SelectNext(int32 Step);
	/** Moves the selected panel by Delta HUD units. */
	void MoveSelected(const FVector2D& Delta);
	/** Grows or shrinks the selected panel by Step (0.05 is 5%) about its anchor corner. */
	void ScaleSelected(float Step);
	void ToggleComponent(int32 Index);
	/** The selected panel back where its component.json puts it, at 100%. */
	void ResetSelected();
	void ResetAll();
	void SetPanelHidden(bool bHidden);

	/**
	 * One scripted step, for console and unattended runs: `select <id>`,
	 * `next`, `move <dx> <dy>`, `scale <step>`, `toggle <id>`, `reset`,
	 * `resetall`, `panel`, `save`, `cancel`. False when it is not understood.
	 *
	 * Three more drive the real mouse path, as synthesised Slate events (the
	 * way the automation driver does): `grab <id>` presses the button over a
	 * panel's middle, `grab <id> corner` over its resize handle, `dragby <dx>
	 * <dy>` moves the pressed cursor (HUD units), `release` lets go.
	 */
	bool RunStep(const FString& Step);

	// --- Read by the frame layer -------------------------------------------------

	UApexHudWidget* GetHud() const { return Hud.Get(); }
	int32 GetSelected() const { return Selected; }
	int32 GetHovered() const { return Hovered; }
	bool IsDragging() const { return Drag != EDrag::None; }
	const ApexHudPlace::FSnapLines& GetSnapLines() const { return SnapLines; }
	/** The resize handle's size, HUD units. */
	static constexpr float HandleSize = 14.0f;

protected:
	virtual void NativeOnInitialized() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;
	virtual FReply NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;
	virtual FReply NativeOnMouseMove(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;
	virtual FReply NativeOnMouseButtonUp(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;
	virtual FCursorReply NativeOnCursorQuery(const FGeometry& InGeometry, const FPointerEvent& InCursorEvent) override;
	virtual FReply NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent) override;
	/** The left stick moves the selected panel; nothing analog is left to wander focus onto the card. */
	virtual FReply NativeOnAnalogValueChanged(const FGeometry& InGeometry, const FAnalogInputEvent& InAnalogEvent) override;

	UFUNCTION()
	void HandleButton(UApexButtonWidget* Button);

private:
	enum class EDrag : uint8
	{
		None,
		Move,
		Resize,
	};

	void BuildCard();
	/** The component list's rows, made again whenever the set of components changes. */
	void RebuildList();
	/** Labels, badges and the selection, every frame while open. */
	void RefreshCard();

	/** The panel under a HUD point, smallest first where they overlap; INDEX_NONE for none. */
	int32 HitTest(const FVector2D& HudPoint) const;
	bool IsOverHandle(const FVector2D& HudPoint) const;
	bool IsOverCard(const FVector2D& ScreenPosition) const;

	/** The selected component's placement, pinning everything first. False when there is none. */
	bool BeginChange(FApexHudPlacement& OutPlacement, FSlateRect& OutRect);
	void EndDrag();

	TWeakObjectPtr<UApexHudWidget> Hud;
	FApexHudLayout Original;
	bool bOpen = false;
	bool bPanelHidden = false;

	int32 Selected = INDEX_NONE;
	int32 Hovered = INDEX_NONE;

	EDrag Drag = EDrag::None;
	FVector2D DragStartMouse = FVector2D::ZeroVector;
	FSlateRect DragStartRect;
	FApexHudPlacement DragStartPlacement;
	TArray<FSlateRect> DragOthers;
	ApexHudPlace::FSnapLines SnapLines;

	/** The pad's left stick, -1 to 1 on each axis (up is positive Y, as the pad reports it). */
	FVector2D Stick = FVector2D::ZeroVector;

	/** Where a scripted `grab` holds the cursor, screen pixels. */
	FVector2D ScriptCursor = FVector2D::ZeroVector;

	/** Steps from -ApexHudEditorSteps, one every StepInterval seconds. */
	TArray<FString> PendingSteps;
	float StepCountdown = 0.0f;

	UPROPERTY(Transient) TObjectPtr<UApexHudFrameLayer> Frames;
	UPROPERTY(Transient) TObjectPtr<UWidget> Card;
	UPROPERTY(Transient) TObjectPtr<UWidget> ShowCardButton;
	UPROPERTY(Transient) TObjectPtr<UVerticalBox> ListBox;
	UPROPERTY(Transient) TArray<TObjectPtr<UApexButtonWidget>> RowButtons;
	UPROPERTY(Transient) TArray<TObjectPtr<UApexButtonWidget>> ToggleButtons;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> SelectedText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> ScaleText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> StatusText;
	/** How many components the list was made for. */
	int32 ListedCount = -1;
};
