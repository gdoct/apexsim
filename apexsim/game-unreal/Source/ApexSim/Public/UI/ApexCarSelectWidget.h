#pragma once

#include "CoreMinimal.h"
#include "ApexProtocolTypes.h"
#include "Cars/ApexLiveryEditor.h"
#include "Catalog/ApexCatalogRows.h"
#include "UI/ApexScreenWidget.h"

#include "ApexCarSelectWidget.generated.h"

class UApexButtonWidget;
class UBorder;
class UEditableTextBox;
class UHorizontalBox;
class UImage;
class UProgressBar;
class UScrollBox;
class UTexture2D;
class UTextBlock;
class UVerticalBox;
class UWidget;

/**
 * The garage: every car the server offers, one described at a time.
 *
 * The preview is the shared AApexCarPreviewStage turntable rendered into a
 * render target — one scene capture for the screen rather than one per car.
 *
 * The stat bars are relative, not absolute: a car's power bar is its share of
 * the most powerful car available. Absolute bars would need a scale nothing in
 * the data defines.
 */
UCLASS()
class APEXSIM_API UApexCarSelectWidget : public UApexScreenWidget
{
	GENERATED_BODY()

public:
	UApexCarSelectWidget(const FObjectInitializer& ObjectInitializer);

	virtual void OnScreenActivated() override;
	/** The turntable shares the world with the demo race. */
	virtual bool WantsLiveBackdrop() const override { return false; }
	virtual void OnScreenDeactivated() override;

	// --- Navigation ---------------------------------------------------------
	//
	// Two regions: the class chips across the header and the car list down the
	// left. Up out of the list reaches the chips. Focus on a row describes the
	// car on the right; activating the row drives it.

	virtual void FocusDefault() override;
	virtual bool HandleNavigation(EUINavigation Direction, UWidget* Source) override;
	/** Back closes the livery editor first. */
	virtual bool HandleBack() override;

	/** The livery editor's panel is up (N11 phase 2). */
	bool IsLiveryEditorOpen() const;
	/** Opens it for the selected car (the EDIT button, E or pad Y on a car's row); false for a car with no folder here. */
	bool OpenLiveryEditor();

protected:
	virtual void NativeOnInitialized() override;
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;
	virtual FReply NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent) override;

private:
	void BuildLayout();
	UWidget* BuildHeader();
	UWidget* BuildStage();

	void RebuildList(bool bForce = false);
	void RebuildFilterChips();
	void ApplyFilter();
	void SelectCar(const FString& CarId);
	void RefreshDetail();

	/** Points the shared turntable at the selected car's mesh. */
	void UpdatePreviewStage();
	/** Steps the selected car's livery (wrapping through "Works" and its `Liveries`). */
	void CycleLivery(int32 Step);
	/** The livery button's label and badge, hidden for a car without liveries. */
	void RefreshLiveryButton();
	/** Shows `Livery`'s picture beside its button, or hides the slot (no livery, no picture). */
	void RefreshLiveryPreview(const struct FApexCarLivery* Livery);

	// --- The livery editor: paint, accent, number and name, saved as one of
	// the player's own liveries (Cars/ApexLiveryEditor.h). The turntable
	// shows the draft as it changes.
	UWidget* BuildLiveryEditor();
	void CloseLiveryEditor();
	/** A stepper row (paint, accent, number) one step on. */
	void StepEditor(UApexButtonWidget* Row, int32 Step);
	/** The rows' values, the name, and the draft on the turntable. */
	void RefreshLiveryEditor();
	void SaveLiveryEditor();
	/** The selected car's folder on this machine; empty for one only the table knows. */
	FString SelectedCarDir() const;
	UFUNCTION() void HandleEditorNameChanged(const FText& Text);

	/** Focus has to wait a tick after the tree changes; see the main menu. */
	void RequestRowFocus();

	UFUNCTION() void HandleLobbyStateUpdated(const FApexLobbyState& LobbyState);
	UFUNCTION() void HandleRowActivated(UApexButtonWidget* Row);
	UFUNCTION() void HandleButtonActivated(UApexButtonWidget* Button);
	/** Sends the selected car and livery and leaves for the screen after car select. */
	void ConfirmSelection();

	/** Focuses a visible row, scrolls to it, and describes its car. False if there is no such row. */
	bool FocusRow(int32 Index);
	/** Focuses the active class chip, or the first. */
	bool FocusChip();

	/** Horsepower from the catalog's kW, or 0 when there is no row. */
	static float GetPowerHp(const FApexCarCatalogRow& Row);

	UPROPERTY(Transient) TObjectPtr<UVerticalBox> CarListBox;
	/** Shown in place of the list while the server has not sent any cars. */
	UPROPERTY(Transient) TObjectPtr<UTextBlock> EmptyText;
	UPROPERTY(Transient) TObjectPtr<UScrollBox> CarListScroll;
	UPROPERTY(Transient) TObjectPtr<UHorizontalBox> ChipRow;
	UPROPERTY(Transient) TObjectPtr<UImage> StageImage;

	UPROPERTY(Transient) TObjectPtr<UTextBlock> EyebrowText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> NameText;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> HeaderBackButton;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> LiveryButton;
	/** The mouse's way to choose the car a click selected (a key on a row chooses at once). */
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> ConfirmButton;
	/** The livery's own picture (`preview` in car.toml), beside its button when it has one. */
	UPROPERTY(Transient) TObjectPtr<UImage> LiveryPreview;
	/** Previews read so far, by file: stepping back and forth reads each once. */
	UPROPERTY(Transient) TMap<FString, TObjectPtr<UTexture2D>> LiveryPreviewTextures;

	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> EditLiveryButton;
	UPROPERTY(Transient) TObjectPtr<UBorder> LiveryEditorPanel;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> EditorPaintRow;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> EditorAccentRow;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> EditorNumberRow;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> EditorSaveButton;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> EditorCancelButton;
	UPROPERTY(Transient) TObjectPtr<UEditableTextBox> EditorNameBox;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> EditorNote;
	/** The pad's walk through the editor, top to bottom. */
	UPROPERTY(Transient) TArray<TObjectPtr<UApexButtonWidget>> EditorRows;

	FApexLiveryDraft Draft;
	/** The player typed the name: the paint and number no longer rename the draft. */
	bool bDraftNamed = false;
	/** The number's logo drawn for the turntable, by number and plate, so stepping back finds it. */
	TMap<FString, FString> DraftLogos;

	UPROPERTY(Transient) TObjectPtr<UTextBlock> PowerValue;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> WeightValue;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> RatioValue;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> ClassValue;
	UPROPERTY(Transient) TObjectPtr<UProgressBar> PowerBar;
	UPROPERTY(Transient) TObjectPtr<UProgressBar> WeightBar;
	UPROPERTY(Transient) TObjectPtr<UProgressBar> RatioBar;

	UPROPERTY(Transient) TArray<TObjectPtr<UApexButtonWidget>> CarRows;
	UPROPERTY(Transient) TArray<TObjectPtr<UApexButtonWidget>> VisibleRows;
	UPROPERTY(Transient) TArray<TObjectPtr<UApexButtonWidget>> FilterChips;

	TArray<FString> BuiltCarIds;
	FString SelectedCarId;
	/** 0: the car as authored; N: the catalog row's `Liveries[N - 1]`. */
	int32 SelectedLivery = 0;
	FString ActiveFilter;
	int32 FocusedRowIndex = 0;
};
