#pragma once

#include "CoreMinimal.h"
#include "ApexProtocolTypes.h"
#include "Catalog/ApexCatalogRows.h"
#include "UI/ApexScreenWidget.h"

#include "ApexTrackSelectWidget.generated.h"

class UApexButtonWidget;
class UApexContentCardWidget;
class UEditableTextBox;
class UImage;
class UScrollBox;
class UTextBlock;
class UUniformGridPanel;
class UVerticalBox;
class UWidget;

/**
 * Picks the circuit: a grid of cards on the left, the chosen one described on
 * the right.
 *
 * The track list is whatever the server last announced; everything richer than
 * a name and an id — country, length, category, preview art — comes from
 * DT_TrackCatalog, and personal bests from the local profile. A track with no
 * catalog row still appears, with placeholder art, rather than being hidden.
 */
UCLASS()
class APEXSIM_API UApexTrackSelectWidget : public UApexScreenWidget
{
	GENERATED_BODY()

public:
	UApexTrackSelectWidget(const FObjectInitializer& ObjectInitializer);

	virtual void OnScreenActivated() override;

protected:
	virtual void NativeOnInitialized() override;
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;
	virtual FReply NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent) override;

private:
	void BuildLayout();
	UWidget* BuildHeader();
	UWidget* BuildDetailPanel();

	/** Rebuilds the card grid from the lobby snapshot; cheap no-op if unchanged. */
	void RebuildCards(bool bForce = false);
	/** Applies the search text and the active category filter to the grid. */
	void ApplyFilter();
	void SelectTrack(const FString& TrackId);
	void RefreshDetail();

	/** Category chips are built from whatever categories the catalog actually has. */
	void RebuildFilterChips();

	UFUNCTION() void HandleLobbyStateUpdated(const FApexLobbyState& LobbyState);
	UFUNCTION() void HandleCardActivated(UApexContentCardWidget* Card);
	UFUNCTION() void HandleButtonActivated(UApexButtonWidget* Button);
	UFUNCTION() void HandleSearchChanged(const FText& Text);

	/** Moves focus through the visible cards; Delta of ±GridColumns walks rows. */
	void MoveCardFocus(int32 Delta);
	void FocusCard(int32 Index);

	/** Focus has to wait a tick after the tree changes; see the main menu. */
	void RequestCardFocus();

	/** Cards per row. Fixed rather than wrapped so arrow keys have a grid to walk. */
	static constexpr int32 GridColumns = 3;

	UPROPERTY(Transient) TObjectPtr<UUniformGridPanel> CardGrid;
	UPROPERTY(Transient) TObjectPtr<UScrollBox> CardScroll;
	UPROPERTY(Transient) TObjectPtr<UEditableTextBox> SearchField;
	UPROPERTY(Transient) TObjectPtr<UVerticalBox> FilterChipBox;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> CountText;
	/** Shown in place of the grid while the server has not sent any tracks. */
	UPROPERTY(Transient) TObjectPtr<UTextBlock> EmptyText;

	UPROPERTY(Transient) TObjectPtr<UVerticalBox> DetailBox;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> UseButton;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> DemoButton;

	UPROPERTY(Transient) TArray<TObjectPtr<UApexContentCardWidget>> TrackCards;
	/** The subset currently on screen, in grid order — what the arrows walk. */
	UPROPERTY(Transient) TArray<TObjectPtr<UApexContentCardWidget>> VisibleCards;
	UPROPERTY(Transient) TArray<TObjectPtr<UApexButtonWidget>> FilterChips;

	/** Ids the grid was last built from, to skip rebuilding on every lobby tick. */
	TArray<FString> BuiltTrackIds;

	FString SelectedTrackId;
	/** Empty means "all"; otherwise a category from the catalog, or "__driven". */
	FString ActiveFilter;
	int32 FocusedCardIndex = 0;
};
