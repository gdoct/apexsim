#pragma once

#include "CoreMinimal.h"
#include "UI/ApexNavigation.h"

#include "ApexPauseMenuWidget.generated.h"

class UApexButtonWidget;
class UBorder;
class UTextBlock;
class UVerticalBox;
class UWidget;

/** What the pause menu is asking the shell to do. */
UENUM(BlueprintType)
enum class EApexPauseAction : uint8
{
	Resume,
	OpenSettings,
	LeaveSession,
	QuitGame,
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FApexOnPauseAction, EApexPauseAction, Action);

/**
 * The Escape menu over a running race.
 *
 * It does not act on anything itself — it reports which of the four things the
 * player chose and lets the root widget carry it out, because leaving a session
 * and quitting the game are shell concerns that this widget has no business
 * reaching into.
 *
 * "Paused" is a local state, not a server one: the session keeps running while
 * this is open, because the server is authoritative and eleven other cars are
 * still driving. The status strip says so rather than claiming a hold the
 * protocol cannot deliver.
 */
UCLASS()
class APEXSIM_API UApexPauseMenuWidget : public UApexNavigableWidget
{
	GENERATED_BODY()

public:
	UApexPauseMenuWidget(const FObjectInitializer& ObjectInitializer);

	virtual void NativeOnInitialized() override;
	virtual FReply NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent) override;

	/** Focus lands on Resume, so Accept is always the safe answer. */
	virtual void FocusDefault() override;
	/** Up and Down walk the rows and wrap; nothing lies left or right. */
	virtual bool HandleNavigation(EUINavigation Direction, UWidget* Source) override;
	/** Back resumes, as the footer promises. */
	virtual bool HandleBack() override;

	UPROPERTY(BlueprintAssignable, Category = "ApexSim|UI")
	FApexOnPauseAction OnAction;

	/** Shows the menu, refreshes the status strip, and takes keyboard focus. */
	UFUNCTION(BlueprintCallable, Category = "ApexSim|UI")
	void Open();

	UFUNCTION(BlueprintCallable, Category = "ApexSim|UI")
	void Close();

	UFUNCTION(BlueprintPure, Category = "ApexSim|UI")
	bool IsOpen() const { return bOpen; }

private:
	UFUNCTION()
	void HandleButtonActivated(UApexButtonWidget* Button);

	/** One row of the card: label left, consequence right. */
	UApexButtonWidget* AddRow(
		UVerticalBox* Stack,
		const FString& Label,
		const FString& Badge,
		FName ActionId,
		bool bPrimary,
		const FString& KeyCap = FString());

	/** Lap, position, circuit and car — what the blurred scene no longer shows. */
	void RefreshStatusStrip();

	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> StatusText;

	UPROPERTY(Transient)
	TObjectPtr<UBorder> StatusBadge;

	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> StatusBadgeText;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UApexButtonWidget>> Rows;

	bool bOpen = false;
};
