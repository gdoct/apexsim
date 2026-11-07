#pragma once

#include "CoreMinimal.h"
#include "UI/ApexNavigation.h"
#include "Verify/ApexVerifyCard.h"

#include "ApexVerifyWidget.generated.h"

class UApexButtonWidget;
class UBorder;
class UTextBlock;
class UVerticalBox;
class UWidget;

/**
 * The verify card of a sitting block (-ApexVerify=, docs/verifier.md): the
 * block's brief for the first seconds, a clock for its driving time, then
 * one check at a time with its answer words as big buttons, a note typed on
 * an on-screen keyboard (or a real one), a flag, and at the end what comes
 * next: the runner's next block, more time, or stop. Everything is reached
 * with the wheel's menu buttons; nothing needs a mouse.
 *
 * The root opens the card (OpenCard) when the pause key is pressed once the
 * time is up, from the pause menu's VERIFY row at any time, and by itself
 * when WantsCard says so: the time is up and the player is not driving, or a
 * race block's race view has gone (finished, left). The answers file is
 * written on every change; the root quits the game on OnQuit.
 */
UCLASS()
class APEXSIM_API UApexVerifyWidget : public UApexNavigableWidget
{
	GENERATED_BODY()

public:
	UApexVerifyWidget(const FObjectInitializer& ObjectInitializer);

	virtual void NativeOnInitialized() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;
	virtual void FocusDefault() override;
	virtual bool HandleNavigation(EUINavigation Direction, UWidget* Source) override;
	virtual bool HandleBack() override;

	/** A card was given and read. */
	bool IsActive() const { return bActive; }
	bool IsCardOpen() const { return bCardOpen; }
	/** The note's keyboard is up: every key is a letter, the pause key too. */
	bool IsTyping() const { return bCardOpen && bKeyboardOpen; }
	/** The block's driving time is over. */
	bool IsDue() const { return bDue; }
	/** The pause menu's VERIFY row badge: the time left, or the checks answered. */
	FString GetBadge() const;

	/** The root's view of the player, every tick: driving a car, and in the race view at all. */
	void SetDriving(bool bInDriving, bool bInRaceView);
	/** Whether the card should open by itself now (see the class comment). */
	bool WantsCard() const;

	void OpenCard();
	void CloseCard();
	/** `apexsim.verify answer <n>|note|type <text>|flag|skip|back|more|next|stop` on the open card. */
	void HandleCommand(const TArray<FString>& Args);

	/** The card asks to be closed (Drive on, Back from the first check). */
	FSimpleMulticastDelegate OnCloseRequested;
	/** The card is done: the answers are written, the game should quit. */
	FSimpleMulticastDelegate OnQuit;

protected:
	virtual FReply NativeOnPreviewKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent) override;
	virtual FReply NativeOnKeyChar(const FGeometry& InGeometry, const FCharacterEvent& InCharEvent) override;

private:
	void BuildLayout();
	void BuildKeyboard(UVerticalBox* Host);
	void Refresh();
	void RefreshClock();
	void RefreshVisibility();
	bool IsSummary() const { return Page >= Card.Checks.Num(); }
	int32 AnsweredCount() const;
	void Save(EApexVerifyNext Next);
	void ShowPage(int32 InPage);
	void SetKeyboardOpen(bool bOpen);
	void Type(const FString& Text);
	void Finish(EApexVerifyNext Next);

	UFUNCTION() void HandleButtonActivated(UApexButtonWidget* Button);

	FApexVerifyCard Card;
	TArray<FApexVerifyAnswer> Answers;
	bool bActive = false;
	bool bCardOpen = false;
	bool bDue = false;
	bool bClockRunning = false;
	bool bDriving = false;
	bool bInRaceView = false;
	bool bHasDriven = false;
	bool bKeyboardOpen = false;
	bool bShift = false;
	/** Driving time so far, not counting while the card is open. */
	float ElapsedS = 0.0f;
	/** Driving time the card is due at: the block's minutes, plus every "more time". */
	float DueAtS = 0.0f;
	float BriefLeftS = 0.0f;
	int32 Page = 0;

	// The clock, top right, and the brief, left.
	UPROPERTY(Transient) TObjectPtr<UBorder> ClockPanel;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> ClockKicker;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> ClockText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> ClockNote;
	UPROPERTY(Transient) TObjectPtr<UWidget> BriefPanel;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> BriefTitle;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> BriefSetup;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> BriefChecks;

	// The card.
	UPROPERTY(Transient) TObjectPtr<UWidget> CardHost;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> KickerText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> TitleText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> DetailText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> BeforeText;
	UPROPERTY(Transient) TObjectPtr<UWidget> CheckPanel;
	UPROPERTY(Transient) TObjectPtr<UWidget> SummaryPanel;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> SummaryText;
	UPROPERTY(Transient) TArray<TObjectPtr<UApexButtonWidget>> AnswerButtons;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> NoteText;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> NoteButton;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> FlagButton;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> PrevButton;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> SkipButton;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> DriveButton;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> BackToChecksButton;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> MoreTimeButton;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> StopButton;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> NextBlockButton;

	// The on-screen keyboard under the note.
	UPROPERTY(Transient) TObjectPtr<UWidget> KeyboardPanel;
	UPROPERTY(Transient) TArray<TObjectPtr<UApexButtonWidget>> KeyButtons;
	/** KeyButtons' characters, lower case. */
	TArray<FString> KeyChars;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> ShiftKey;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> SpaceKey;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> DeleteKey;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> ClearKey;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> DoneKey;
};
