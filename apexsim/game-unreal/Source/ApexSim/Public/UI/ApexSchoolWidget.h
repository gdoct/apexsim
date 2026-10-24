#pragma once

#include "CoreMinimal.h"
#include "UI/ApexScreenWidget.h"

#include "ApexSchoolWidget.generated.h"

class UApexButtonWidget;
class UScrollBox;
class UTextBlock;
class UVerticalBox;

/**
 * The driving school's menu (School/ApexSchoolSubsystem.h): the server's
 * circuits on the left, each with the medals the pending car holds there;
 * on the right the chosen circuit's licence tests, made from its track guide
 * of the car's class (made first when it is missing), each with what it asks,
 * what each medal takes and the best so far. A test starts its own hotlap
 * session, where its card waits in the garage. Main menu > Drive > School.
 */
UCLASS()
class APEXSIM_API UApexSchoolWidget : public UApexScreenWidget
{
	GENERATED_BODY()

public:
	UApexSchoolWidget(const FObjectInitializer& ObjectInitializer);

	virtual void OnScreenActivated() override;
	virtual void OnScreenDeactivated() override;
	virtual void FocusDefault() override;
	virtual bool HandleNavigation(EUINavigation Direction, UWidget* Source) override;
	virtual bool WantsLiveBackdrop() const override { return false; }

	/** Choose a circuit, as its row does (`apexsim.school.Select`). */
	void SelectTrack(const FString& TrackId);

protected:
	virtual void NativeOnInitialized() override;

private:
	void BuildLayout();
	/** The server's circuits the catalog can name, by display name. */
	void LoadTracks();
	void RebuildTracks();
	void RebuildTests();
	void RefreshStatus();
	/** "2 GOLD · 1 SILVER" for the pending car on a circuit; empty when it has none. */
	FString MedalSummary(const FString& TrackId) const;
	void HandleSchoolChanged();

	UFUNCTION() void HandleButtonActivated(UApexButtonWidget* Button);
	UFUNCTION() void HandleLobbyState(const FApexLobbyState& LobbyState);

	UPROPERTY(Transient) TObjectPtr<UTextBlock> StatusText;
	UPROPERTY(Transient) TObjectPtr<UScrollBox> TrackList;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> TrackTitle;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> PrepText;
	UPROPERTY(Transient) TObjectPtr<UVerticalBox> TestBox;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> BackButton;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> CarButton;
	UPROPERTY(Transient) TArray<TObjectPtr<UApexButtonWidget>> TrackButtons;
	UPROPERTY(Transient) TArray<TObjectPtr<UApexButtonWidget>> TestButtons;

	TArray<FString> TrackIds;
	FString SelectedTrackId;
	FDelegateHandle SchoolChangedHandle;
	bool bBound = false;
};
