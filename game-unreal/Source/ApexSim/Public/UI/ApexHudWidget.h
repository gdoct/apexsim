#pragma once

#include "CoreMinimal.h"
#include "ApexSettingsSubsystem.h"
#include "Blueprint/UserWidget.h"
#include "Hud/ApexHudComponent.h"

#include "ApexHudWidget.generated.h"

class UApexHudDataSubsystem;
class UApexMinimapWidget;
class UApexMirrorWidget;
class UPanelWidget;
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
	const TArray<FApexHudComponentDef>& GetComponents() const { return Components; }

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

	UApexHudDataSubsystem* GetHudData() const;

	TArray<FApexHudComponentDef> Components;
	FApexHudLoadReport Report;
	TArray<FHudNode> Nodes;
	/** The root node of each component, parallel to Components. */
	TArray<int32> ComponentRoots;

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
