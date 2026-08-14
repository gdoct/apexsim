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

protected:
	virtual void NativeOnInitialized() override;
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;
	virtual FReply NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent) override;

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

	UFUNCTION() void HandleButtonActivated(UApexButtonWidget* Button);
	UFUNCTION() void HandleConnectionStateChanged(EApexConnectionState NewState, const FString& Detail);
	UFUNCTION() void HandleLobbyStateUpdated(const FApexLobbyState& LobbyState);
	UFUNCTION() void HandlePendingCarChanged(const FString& CarId);
	UFUNCTION() void HandlePendingTrackChanged(const FString& TrackId);

	// --- Keyboard navigation --------------------------------------------------

	/** Moves the selection within the active column, wrapping and skipping locked rows. */
	void MoveSelection(int32 Delta);
	void SwitchColumn();
	void ApplyFocus();

	/** Starts a session with the remembered setup, or sends the user to pick a track. */
	void StartRememberedSession();

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
