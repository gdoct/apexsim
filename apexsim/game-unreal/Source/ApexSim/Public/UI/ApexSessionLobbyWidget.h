#pragma once

#include "CoreMinimal.h"
#include "ApexProtocolTypes.h"
#include "UI/ApexScreenWidget.h"

#include "ApexSessionLobbyWidget.generated.h"

class UApexButtonWidget;
class UBorder;
class UTextBlock;
class UVerticalBox;
class UWidget;

/**
 * The waiting room: who is on the grid, and the host's controls for starting.
 *
 * The grid comes from the session roster (car index -> name, human or AI),
 * which is the only message that describes AI drivers at all; the lobby state
 * adds each human's car choice on top of it.
 *
 * There is no ready flag in the protocol, so "ready" here means the driver has
 * picked a car — which is the thing that actually blocks a start.
 */
UCLASS()
class APEXSIM_API UApexSessionLobbyWidget : public UApexScreenWidget
{
	GENERATED_BODY()

public:
	UApexSessionLobbyWidget(const FObjectInitializer& ObjectInitializer);

	virtual void OnScreenActivated() override;

	/** The host's start button; for everyone else, the car link. Never "leave". */
	virtual void FocusDefault() override;
	/**
	 * There is no history to go back to — joining reset it — and one keypress
	 * must not leave a session. Leaving is the button.
	 */
	virtual bool HandleBack() override { return true; }

protected:
	virtual void NativeOnInitialized() override;
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;
	/** A scheduled event's countdown, stepped once a second. */
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

private:
	void BuildLayout();
	UWidget* BuildHeader();
	UWidget* BuildSidePanel();

	void RefreshAll();
	void RefreshHeader();
	void RefreshGrid();
	void RefreshSidePanel();

	/** True when this client created the session and may start it. */
	bool IsHost() const;

	UFUNCTION() void HandleButtonActivated(UApexButtonWidget* Button);
	UFUNCTION() void HandleLobbyStateUpdated(const FApexLobbyState& LobbyState);
	UFUNCTION() void HandleRosterUpdated(const FApexSessionRoster& Roster);
	UFUNCTION() void HandleCountdownUpdate(int32 SecondsRemaining);
	UFUNCTION() void HandleGameModeChanged(EApexGameMode NewMode);

	UPROPERTY(Transient) TObjectPtr<UBorder> StateDot;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> StateText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> TrackText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> FormatText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> SlotsText;

	UPROPERTY(Transient) TObjectPtr<UVerticalBox> GridBox;
	UPROPERTY(Transient) TObjectPtr<UVerticalBox> CarPanelBox;
	/** Rebuilt with the car panel; kept so focus can survive the rebuild. */
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> ChangeCarLink;

	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> ModeButton;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> CountdownButton;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> StartAction;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> NotReadyText;
	UPROPERTY(Transient) TObjectPtr<UVerticalBox> HostControlsBox;
	/** The standings of the championships a race event's session is a round of (ApexChampionshipPanel). */
	UPROPERTY(Transient) TObjectPtr<UVerticalBox> ChampionshipBox;
	FDelegateHandle ChampionshipsHandle;
	void RefreshChampionships();

	/** Countdown length the host picks, cycled through a few sensible values. */
	int32 CountdownSeconds = 10;
	/** Non-zero while the server is counting in. */
	int32 CountdownRemaining = 0;
	/**
	 * The line under the start button for a scheduled event's phase
	 * (`LobbyState.Events`' start time), empty for none; and the second it
	 * was last worked out for.
	 */
	FString ScheduledEventLine() const;
	int64 ShownScheduleSecond = 0;

	/** The session's chat under the grid: the last lines and a line to type in (Enter sends). */
	UPROPERTY(Transient)
	TObjectPtr<class UVerticalBox> ChatList;
	UPROPERTY(Transient)
	TObjectPtr<class UEditableTextBox> ChatInput;
	FDelegateHandle ChatHandle;
	void RefreshChat();
	UFUNCTION()
	void HandleChatCommitted(const FText& Text, ETextCommit::Type CommitMethod);
};
