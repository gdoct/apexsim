#pragma once

#include "CoreMinimal.h"
#include "ApexSettingsSubsystem.h"
#include "Blueprint/UserWidget.h"
#include "Hud/ApexHudComponent.h"
#include "Hud/ApexHudLayout.h"

#include "ApexHudWidget.generated.h"

class UApexHudDataSubsystem;
class UApexMinimapWidget;
class UApexMirrorWidget;
class UCanvasPanel;
class UCanvasPanelSlot;
class UOverlay;
class UPanelWidget;
class UScaleBox;
class UTextBlock;
class UTexture2D;
class UWidget;

/**
 * The race HUD: a host that draws the components under `content/hud`.
 *
 * It knows no panel by name. Each component folder (`standings`, `car_state`,
 * a player's own in `custom/`) says where it sits and what it draws, in
 * elements bound to the data points UApexHudDataSubsystem publishes, and this
 * widget builds the tree once and, every frame, evaluates the bindings and
 * touches only the widgets whose value moved. docs/HUD_MODDING.md is the
 * format; `apexsim.hud.Reload` reads the folders again without a restart.
 *
 * What the panels show is still mostly derived (position, gaps and the delta
 * fall out of each car's lap and station) and the lap timing is the server's;
 * both live in ApexHudData now, not here.
 *
 * Where each panel sits is its component.json's region, unless the player's
 * layout (FApexHudLayout, `custom/layout.json`, written by the HUD editor)
 * pins it somewhere else, scales it or hides it. Every component is wrapped in
 * a scale box; a pinned one sits on a canvas over the regions.
 */
UCLASS()
class APEXSIM_API UApexHudWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	UApexHudWidget(const FObjectInitializer& ObjectInitializer);

	virtual void NativeOnInitialized() override;
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

	/** Shows or hides the whole HUD, and starts or stops its per-frame work. */
	UFUNCTION(BlueprintCallable, Category = "ApexSim|UI")
	void SetRaceActive(bool bActive);

	/**
	 * Hide the HUD without ending the race for it (the hotlap garage card
	 * covers the view and the car is standing still); shown again with the
	 * detail setting honoured.
	 */
	void SetShown(bool bShown);

	/** Read the component folders again and rebuild the tree. */
	void Reload();

	/** What the last load said: errors (components left out) and warnings. */
	const FApexHudLoadReport& GetLoadReport() const { return Report; }
	/** Every component that loaded, shown or not. */
	const TArray<FApexHudComponentDef>& GetComponents() const { return Components; }

	// --- The HUD editor (UApexHudEditorWidget) ------------------------------------

	/**
	 * Show the HUD for laying out, race or not: every shown component is drawn
	 * whatever its own `visible` says, over a made-up race when there is no
	 * real one (UApexHudDataSubsystem::SetPreview).
	 */
	void BeginEditing();
	void EndEditing();
	bool IsEditing() const { return bEditing; }

	const FApexHudLayout& GetLayout() const { return Layout; }
	/** Applies a layout (rebuilding the tree); it is saved by the editor, not here. */
	void SetLayout(const FApexHudLayout& InLayout);
	/** The layout file the HUD was read with, and is saved to. */
	FString GetLayoutFile() const;

	// --- Named layouts (ApexHudLayouts) -------------------------------------------

	/** The layout in use, by name; "Default" is `layout.json`. */
	const FString& GetActiveLayoutName() const { return ActiveLayoutName; }
	/** Names the layout the working one belongs to (and is saved to), without reading anything. */
	void SetActiveLayoutName(const FString& Name);
	/** Reads a named layout and builds the HUD with it. False, and nothing changes, when it will not read. */
	bool LoadLayoutNamed(const FString& Name);
	const ApexHudLayouts::FBindings& GetBindings() const { return Bindings; }
	/** Takes the bindings the editor wrote; the layout for the context is looked up again next frame. */
	void SetBindings(const ApexHudLayouts::FBindings& InBindings);
	/** Where the player is, for choosing a layout. */
	ApexHudLayouts::FContext GetContext() const;

	bool IsComponentShown(int32 Index) const;
	/** A shown component's rectangle in HUD units (1080p pixels, from the top left); false before it is laid out. */
	bool GetComponentRect(int32 Index, FSlateRect& OutRect) const;
	/** The HUD's size in its own units. */
	FVector2D GetHudSize() const;
	FVector2D AbsoluteToHud(const FVector2D& Absolute) const;
	FVector2D HudToAbsolute(const FVector2D& Local) const;

	/**
	 * Pins every shown, laid-out component where it is now, so moving one
	 * leaves the rest where they were rather than closing up its region.
	 */
	void PinAll();

	/**
	 * Moves or scales one component now, without a rebuild while it is already
	 * pinned (a drag calls this every mouse move); the layout takes the
	 * placement either way.
	 */
	void PlaceComponent(int32 Index, const FApexHudPlacement& Placement);

protected:
	UFUNCTION()
	void HandleSettingsChanged(EApexSettingsGroup Group);

private:
	/** One built element: its widgets, its children and what was last applied to it. */
	struct FHudNode
	{
		const FApexHudElementDef* Def = nullptr;
		/** What collapses when the element is hidden: its size box, or the element itself. */
		UWidget* Outer = nullptr;
		UWidget* Widget = nullptr;
		/** Which copy of a repeated element this is; INDEX_NONE when it is not repeated. */
		int32 Slot = INDEX_NONE;
		TArray<int32> Children;

		int8 LastVisible = -1;
		int8 LastBold = -1;
		bool bHasText = false;
		FString LastText;
		bool bHasColour = false;
		FLinearColor LastColour;
		bool bHasBackground = false;
		FLinearColor LastBackground;
		bool bHasOutline = false;
		FLinearColor LastOutline;
		float LastValue = -1.0f;
		/** The share last given to the slot of an element whose `fill` is an expression. */
		float LastFill = -1.0f;
	};

	void BuildHud();
	/** Builds Def (copy Copy of a repeat) and its children; the node's index. */
	int32 BuildElement(const FApexHudElementDef& Def, int32 Copy);
	void AddToContainer(UPanelWidget* Container, const FHudNode& Child);

	void UpdateNode(int32 NodeIndex, const FApexHudScope& Scope);
	void UpdateChildren(int32 NodeIndex, const FApexHudScope& Scope);
	void SetNodeVisible(FHudNode& Node, bool bVisible);
	void ApplyFont(FHudNode& Node, bool bBold);
	void ApplyBrush(FHudNode& Node);
	void ApplyVisibility();
	/** Puts on the layout the bindings give for where the player is now. True when it changed the HUD. */
	bool RefreshContext();

	UApexHudDataSubsystem* GetHudData() const;

	TArray<FApexHudComponentDef> Components;
	FApexHudLoadReport Report;
	TArray<FHudNode> Nodes;
	/** The root node of each component, parallel to Components; INDEX_NONE for one the layout hides. */
	TArray<int32> ComponentRoots;
	/** Each component's scale box, and its canvas slot when pinned; parallel to Components. */
	TArray<UScaleBox*> ComponentWrappers;
	TArray<UCanvasPanelSlot*> ComponentCanvasSlots;

	FApexHudLayout Layout;
	FString LayoutFile;
	FString ActiveLayoutName = TEXT("Default");
	ApexHudLayouts::FBindings Bindings;
	/** The context the layout was last chosen for; empty to choose again. */
	FString LastContextKey;
	/** The widget tree's root for good; each build's layers go inside it. */
	UPROPERTY(Transient) TObjectPtr<UOverlay> HostRoot;
	/** The canvas pinned components sit on, over the regions. */
	UPROPERTY(Transient) TObjectPtr<UCanvasPanel> PinCanvas;
	/** Behind the panels while laying out from the menu, so they are not drawn over the menu's own screens. */
	UPROPERTY(Transient) TObjectPtr<UWidget> EditBackdrop;
	bool bEditing = false;

	/** Widgets with their own per-frame feed. */
	UPROPERTY(Transient) TArray<TObjectPtr<UApexMinimapWidget>> Minimaps;
	UPROPERTY(Transient) TArray<TObjectPtr<UApexMirrorWidget>> Mirrors;
	/** Images read from component folders, kept alive while the tree shows them. */
	UPROPERTY(Transient) TArray<TObjectPtr<UTexture2D>> Images;
	/** Load errors, on screen, so a broken component is not just missing. */
	UPROPERTY(Transient) TObjectPtr<UTextBlock> ErrorText;

	bool bRaceActive = false;
	bool bShownWanted = true;
	/** Whether the minimaps hold an outline, so it is only handed over once per race. */
	bool bMinimapOutlineSet = false;
};
