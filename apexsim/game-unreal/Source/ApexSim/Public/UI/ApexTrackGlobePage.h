#pragma once

#include "CoreMinimal.h"
#include "UI/ApexNavigation.h"

#include "ApexTrackGlobePage.generated.h"

class UApexButtonWidget;
class UApexGlobeWidget;
class UScrollBox;
class USizeBox;
class UVerticalBox;
class UWidget;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FApexTrackPageAction, FName, ActionId);

/**
 * Garage > Tracks (docs/game/client.md): the circuits as a list on the left,
 * a globe with a pin per circuit in the middle, and the chosen one's panel
 * sliding in on the right (map, name, place, class, description, length,
 * your best, Hotlap / Track guide / Watch hotlap / Leaderboards).
 *
 * Choosing a circuit, by its row or its pin, makes it the pending track and
 * flies the globe to it. The panel's buttons and Back are the owning main
 * menu's to carry out (OnAction, with the rail's action ids), so they start
 * the same flows the rail did.
 *
 * Keys and pad: Up / Down along the list (Back above it), Right into the
 * globe, whose D-pad hops between pins and hands Left / Right on to the list
 * and the panel; Tab and the shoulders cycle list, globe and panel. Back
 * closes the panel, and with it shut goes up a page (it is not consumed).
 */
UCLASS()
class APEXSIM_API UApexTrackGlobePage : public UApexNavigableWidget
{
	GENERATED_BODY()

public:
	/** The panel's actions, by the main menu's rail action ids. */
	static const FName ActionHotlap;
	static const FName ActionGuide;
	static const FName ActionWatchHotlap;
	static const FName ActionLeaderboard;
	static const FName ActionBack;

	/** Fills the list and the pins from the lobby and the catalog; cheap when nothing changed. */
	void Refresh();

	/** Chooses a circuit as its row or pin would; empty closes the panel. */
	void Choose(const FString& TrackId, bool bFly = true);

	UApexGlobeWidget* GetGlobe() const { return Globe; }
	bool IsPanelOpen() const { return !OpenId.IsEmpty(); }

	UPROPERTY(BlueprintAssignable)
	FApexTrackPageAction OnAction;

	virtual void FocusDefault() override;
	virtual bool HandleNavigation(EUINavigation Direction, UWidget* Source) override;
	virtual bool HandleBack() override;

protected:
	virtual void NativeOnInitialized() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

private:
	void BuildLayout();
	UWidget* BuildStage();
	void RebuildPanel();

	UFUNCTION() void HandleButtonActivated(UApexButtonWidget* Button);
	UFUNCTION() void HandleGlobeSelection(const FString& TrackId);

	bool FocusList(int32 Index);
	bool FocusPanel();
	/** The region a control belongs to: 0 list (and Back), 1 globe, 2 panel; INDEX_NONE for none. */
	int32 RegionOf(const UWidget* Widget) const;
	void FocusRegion(int32 Region);

	UPROPERTY(Transient) TObjectPtr<UApexGlobeWidget> Globe;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> BackButton;
	UPROPERTY(Transient) TObjectPtr<UScrollBox> ListScroll;
	UPROPERTY(Transient) TObjectPtr<UVerticalBox> ListBox;
	UPROPERTY(Transient) TArray<TObjectPtr<UApexButtonWidget>> ListButtons;
	UPROPERTY(Transient) TArray<TObjectPtr<UApexButtonWidget>> StageButtons;
	TArray<FString> ListIds;
	/** What the list was built from, to rebuild only on a change. */
	FString ListKey;

	/** The panel: its width eases between 0 and PanelWidth. */
	UPROPERTY(Transient) TObjectPtr<USizeBox> PanelBox;
	UPROPERTY(Transient) TObjectPtr<UVerticalBox> PanelContent;
	UPROPERTY(Transient) TArray<TObjectPtr<UApexButtonWidget>> PanelButtons;
	FString OpenId;
	float PanelShown = 0.0f;
};
