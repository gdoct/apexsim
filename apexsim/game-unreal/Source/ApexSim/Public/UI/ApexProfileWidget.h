#pragma once

#include "CoreMinimal.h"
#include "ApexProfiles.h"
#include "UI/ApexScreenWidget.h"

#include "ApexProfileWidget.generated.h"

class UApexButtonWidget;
class UApexContentCardWidget;
class UEditableTextBox;
class UHorizontalBox;
class UImage;
class UScrollBox;
class UTextBlock;
class UVerticalBox;

/**
 * The driver profiles on this machine (ApexProfiles.h): the list to pick
 * from or add to, and the one in use with its name, picture and stats.
 * Picking another driver switches to them on the spot. Shown at launch,
 * as "Who is driving?", when there are several profiles
 * (UApexMenuFlowSubsystem::ConsumeProfilePrompt), and from the main menu's
 * Profile row and its driver chip.
 */
UCLASS()
class APEXSIM_API UApexProfileWidget : public UApexScreenWidget
{
	GENERATED_BODY()

public:
	UApexProfileWidget(const FObjectInitializer& ObjectInitializer);

	virtual void OnScreenActivated() override;
	virtual void OnScreenDeactivated() override;
	virtual void FocusDefault() override;
	virtual bool HandleNavigation(EUINavigation Direction, UWidget* Source) override;
	virtual bool HandleBack() override;
	virtual bool WantsLiveBackdrop() const override { return false; }

	/** Opened at launch to ask who drives: the heading asks, and Back drives as the one picked. */
	void SetPrompt(bool bInPrompt) { bPrompt = bInPrompt; }

	/** The stats lines as the panel draws them, for the tests: label and value. */
	static TArray<TPair<FString, FString>> StatLines(const FApexDriverStats& Stats);

	/** The PB rows, fastest-named first by track then car: track id, car id, seconds. Pure, for the tests. */
	struct FBestRow
	{
		FString TrackId;
		FString CarId;
		float Seconds = 0.0f;
	};
	static TArray<FBestRow> BestRows(const FApexDriverStats& Stats);

protected:
	virtual void NativeOnInitialized() override;
	virtual FReply NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent) override;

private:
	void BuildLayout();
	void Rebuild();
	void RebuildList();
	void RebuildPortraits();
	void RebuildStats();
	void CommitName();
	/** Leave the launch prompt for the main menu, as the driver picked. */
	void DriveOn();

	UFUNCTION() void HandleButtonActivated(UApexButtonWidget* Button);
	UFUNCTION() void HandlePortraitActivated(UApexContentCardWidget* Card);
	UFUNCTION() void HandleNameCommitted(const FText& Text, ETextCommit::Type CommitMethod);
	UFUNCTION() void HandleProfileChanged();

	UPROPERTY(Transient) TObjectPtr<UTextBlock> HeadingText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> StatusText;
	UPROPERTY(Transient) TObjectPtr<UVerticalBox> ProfileList;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> NewButton;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> DoneButton;
	UPROPERTY(Transient) TObjectPtr<UImage> PortraitImage;
	UPROPERTY(Transient) TObjectPtr<UEditableTextBox> NameField;
	UPROPERTY(Transient) TObjectPtr<UVerticalBox> PortraitGrid;
	UPROPERTY(Transient) TObjectPtr<UHorizontalBox> StatsRow;
	UPROPERTY(Transient) TObjectPtr<UScrollBox> BestList;

	/** The list's rows, each naming its profile by id. */
	UPROPERTY(Transient) TArray<TObjectPtr<UApexButtonWidget>> ProfileButtons;
	TArray<FGuid> ProfileButtonIds;
	UPROPERTY(Transient) TArray<TObjectPtr<UApexContentCardWidget>> PortraitCards;

	/** A profile one Delete press has marked; the second press on it deletes it. */
	FGuid PendingDelete;
	bool bPrompt = false;
	bool bBound = false;
};
