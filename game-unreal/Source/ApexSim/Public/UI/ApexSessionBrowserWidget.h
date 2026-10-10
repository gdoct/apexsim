#pragma once

#include "CoreMinimal.h"
#include "ApexProtocolTypes.h"
#include "UI/ApexScreenWidget.h"

#include "ApexSessionBrowserWidget.generated.h"

class UApexButtonWidget;
class UScrollBox;
class UTextBlock;

/**
 * The live sessions on the server (the latest LobbyState), built in C++ in
 * the menu style: one row a session (the track's preview, its name, host,
 * drivers, kind, length and sky, and its state), Enter on a row joins it in
 * the pending car, Watch spectates a race being driven. The header has Back
 * and Refresh; Change car and Create session sit above the list.
 *
 * The list changes often (counts and states move with every broadcast), so
 * rows are rebuilt only when the set of sessions changes and refreshed in
 * place otherwise, keeping the pad's place.
 */
UCLASS()
class APEXSIM_API UApexSessionBrowserWidget : public UApexScreenWidget
{
	GENERATED_BODY()

public:
	UApexSessionBrowserWidget(const FObjectInitializer& ObjectInitializer);

	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;
	virtual void OnScreenActivated() override;
	virtual void FocusDefault() override;
	virtual bool HandleNavigation(EUINavigation Direction, UWidget* Source) override;

protected:
	virtual void NativeOnInitialized() override;
	virtual FReply NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent) override;

private:
	void BuildLayout();
	void RefreshList();
	/** The row a session's spec says, for a fresh row or one updated in place. */
	void ApplyRow(int32 Index, const FApexSessionSummary& Session);
	void Join(int32 Index);
	void Watch(int32 Index);
	void Refresh();

	UFUNCTION() void HandleButtonActivated(UApexButtonWidget* Button);
	UFUNCTION() void HandleLobbyStateUpdated(const FApexLobbyState& LobbyState);

	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> HeaderBackButton;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> RefreshAction;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> ChangeCarButton;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> CreateButton;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> CountText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> StatusLine;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> CarText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> EmptyText;
	UPROPERTY(Transient) TObjectPtr<UScrollBox> List;

	/** Per session: the row (Enter joins) and its Watch button. */
	UPROPERTY(Transient) TArray<TObjectPtr<UApexButtonWidget>> Rows;
	UPROPERTY(Transient) TArray<TObjectPtr<UApexButtonWidget>> WatchButtons;

	/** The sessions the rows show, in their order. */
	TArray<FApexSessionSummary> Shown;
	int32 LastFocused = 0;
};
