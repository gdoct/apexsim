#pragma once

#include "CoreMinimal.h"
#include "ApexProtocolTypes.h"
#include "UI/ApexScreenWidget.h"

#include "ApexMainMenuWidget.generated.h"

class UApexButtonWidget;
class UTextBlock;
class UWidget;

/**
 * The landing screen: what you were last doing, and everywhere else you can go.
 *
 * The whole layout is built in C++ (see UI/ApexUIStyle.h) rather than in a
 * widget blueprint, so the design is reviewable as a diff and can be changed
 * without the editor open.
 *
 * The hero half is driven by the local profile — the server has no memory of a
 * player between connections — while the rail's badges come from the lobby
 * snapshot the server broadcasts every couple of seconds.
 */
UCLASS()
class APEXSIM_API UApexMainMenuWidget : public UApexScreenWidget
{
	GENERATED_BODY()

public:
	UApexMainMenuWidget(const FObjectInitializer& ObjectInitializer);

	virtual void OnScreenActivated() override;

	// --- Navigation ---------------------------------------------------------
	//
	// Two columns: the hero's actions run left to right, the rail top to
	// bottom. Left/Right and Up/Down stay inside a column; Tab, the shoulders,
	// Right off the end of the hero and Left off the rail cross over.

	virtual void FocusDefault() override;
	virtual bool HandleNavigation(EUINavigation Direction, UWidget* Source) override;
	/** Back (pad B etc.) closes the Esc menu or steps up a rail page. Escape itself opens the Esc menu, see NativeOnKeyDown. */
	virtual bool HandleBack() override;

protected:
	virtual void NativeOnInitialized() override;
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;
	virtual FReply NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent) override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

private:
	// --- Construction ---------------------------------------------------------

	void BuildLayout();
	UWidget* BuildTopBar();
	UWidget* BuildHero();
	UWidget* BuildRail();

	/** Adds a button to a column and wires its activation to this screen. */
	UApexButtonWidget* AddColumnButton(class UVerticalBox* Column, const struct FApexButtonSpec& Spec, bool bIsRail);

	// --- Data -> widgets ------------------------------------------------------

	void RefreshAll();
	void RefreshHeader();
	void RefreshHero();
	void RefreshRail();
	/** The Watch row's badge: whether a race is on behind the menu to watch. */
	void RefreshWatchBadge();
	/** What the badge last said: -1 never set, 0 none, 1 on now. */
	int32 WatchBadgeState = -1;

	UFUNCTION() void HandleButtonActivated(UApexButtonWidget* Button);
	UFUNCTION() void HandleConnectionStateChanged(EApexConnectionState NewState, const FString& Detail);
	UFUNCTION() void HandleLobbyStateUpdated(const FApexLobbyState& LobbyState);
	UFUNCTION() void HandlePendingCarChanged(const FString& CarId);
	UFUNCTION() void HandlePendingTrackChanged(const FString& TrackId);
	UFUNCTION() void HandleRejoinOfferChanged();

	// --- Rejoin banner ----------------------------------------------------------
	//
	// Over the hero while the server holds a seat for us in a session we lost
	// the connection to (UApexNetSubsystem::HasRejoinOffer). It takes focus
	// when it appears; Up from the hero's actions reaches it, Down leaves it.

	void RefreshRejoin();
	bool IsRejoinShown() const;
	UPROPERTY(Transient) TObjectPtr<UWidget> RejoinBanner;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> RejoinButton;
	/** Focus is on the banner rather than the hero's actions (column 0). */
	bool bOnRejoin = false;

	// --- Keyboard navigation --------------------------------------------------

	/** Moves the rail selection, wrapping and skipping locked rows. */
	void MoveRail(int32 Delta);
	void SwitchColumn();
	/** Focuses the remembered button of the active column. */
	void ApplyFocus();

	/** Starts a session with the remembered setup, or sends the user to pick a track. */
	void StartRememberedSession();

	/** The settings overlay, over this screen, on the controls page. */
	void OpenSettings();

	// --- Rail pages -------------------------------------------------------------
	//
	// The rail is a small tree of pages (docs/game/client.md): Root holds the
	// four main entries, the others hold what is under Garage and Drive.

	enum class EPage : uint8 { Root, Garage, Cars, Tracks, Drive, Create, Count };

	/** Shows one page of the rail and focuses its first usable row. */
	void ShowPage(EPage Page, bool bFocus = true);
	/** One level up; false when already at the root. */
	bool GoUp();
	/** Builds a page's column (heading, rows, a Back row unless Root). */
	class UVerticalBox* BuildPage(EPage Page);

	/** Cycles fullscreen -> borderless -> windowed (Alt+Enter). */
	void CycleDisplayMode();

	/** The Esc menu: Back to main menu / Exit game. */
	UWidget* BuildQuitOverlay();
	void SetQuitOverlayOpen(bool bOpen);
	void QuitGame();

	// The Garage > Garage and Garage > Tracks pages show the cars / circuits as a
	// list in the hero's place: the focused row is committed as the pending car /
	// track (Enter, click, Right or Tab), and the rail acts on it.
	bool IsListPage() const { return CurrentPage == EPage::Cars || CurrentPage == EPage::Tracks; }
	/** Fills the list from the lobby snapshot (cars or circuits). */
	void BuildList(bool bCars);
	/** Rebuilds the list when the lobby's count differs from what is shown. */
	void RefreshList();
	/** Makes the row at Index the pending car / track and marks it. */
	void CommitListItem(int32 Index);

	UPROPERTY(Transient) TObjectPtr<UWidget> HeroHome;
	UPROPERTY(Transient) TObjectPtr<UWidget> HeroList;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> ListTitleText;
	UPROPERTY(Transient) TObjectPtr<class UVerticalBox> ListBox;
	UPROPERTY(Transient) TObjectPtr<class UScrollBox> ListScroll;
	/** The big picture and facts of the focused row. */
	UPROPERTY(Transient) TObjectPtr<class UVerticalBox> ListDetail;
	FString DetailShownId;
	int32 LastListProbe = INDEX_NONE;

	/** Full-screen art behind the page (the focused circuit); fades in when it changes. */
	void SetBackdrop(class UTexture2D* Texture);
	UPROPERTY(Transient) TObjectPtr<UWidget> BackdropLayer;
	UPROPERTY(Transient) TObjectPtr<class UImage> BackdropImage;
	float BackdropAlpha = 0.0f;
	float BackdropTarget = 0.0f;
	void RefreshListDetail(int32 Index);
	UPROPERTY(Transient) TArray<TObjectPtr<UApexButtonWidget>> ListButtons;
	TArray<FString> ListIds;
	int32 ListIndex = 0;
	bool bListShowsCars = false;

	EPage CurrentPage = EPage::Root;
	bool bQuitOpen = false;
	/** Indexed by EPage. */
	UPROPERTY(Transient) TArray<TObjectPtr<class UVerticalBox>> PageBoxes;
	UPROPERTY(Transient) TObjectPtr<class UWidget> QuitOverlay;
	UPROPERTY(Transient) TArray<TObjectPtr<UApexButtonWidget>> QuitButtons;
	/** Every rail row of every page (badges are refreshed on all of them); RailButtons is the visible page's. */
	UPROPERTY(Transient) TArray<TObjectPtr<UApexButtonWidget>> AllRailButtons;

	// --- Widgets --------------------------------------------------------------

	UPROPERTY(Transient) TObjectPtr<class UBorder> ConnectionDot;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> ServerText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> PingText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> DriverText;

	UPROPERTY(Transient) TObjectPtr<UTextBlock> EyebrowText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> TitleHeadText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> TitleTailText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> MetaText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> CarValueText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> SessionValueText;

	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> StartButton;

	/** Column 0: the hero's actions. Column 1: the rail. Tab moves between them. */
	UPROPERTY(Transient) TArray<TObjectPtr<UApexButtonWidget>> HeroButtons;
	UPROPERTY(Transient) TArray<TObjectPtr<UApexButtonWidget>> RailButtons;

	int32 ActiveColumn = 0;
	int32 HeroIndex = 0;
	int32 RailIndex = 0;
};
