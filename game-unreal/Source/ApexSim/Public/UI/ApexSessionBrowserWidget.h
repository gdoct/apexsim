#pragma once

#include "CoreMinimal.h"
#include "ApexProtocolTypes.h"
#include "UI/ApexScreenWidget.h"

#include "ApexSessionBrowserWidget.generated.h"

class UApexSessionRowWidget;
class UButton;
class UVerticalBox;
class UTextBlock;

/**
 * Lists the sessions from the latest LobbyState.
 *
 * Unlike the car and track lists, this one genuinely does change often — player
 * counts and states move — so rows are rebuilt whenever the set of session IDs
 * changes and refreshed in place otherwise.
 */
UCLASS(Abstract)
class APEXSIM_API UApexSessionBrowserWidget : public UApexScreenWidget
{
	GENERATED_BODY()

public:
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;
	virtual void OnScreenActivated() override;

protected:
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category = "ApexSim|UI")
	TObjectPtr<UVerticalBox> SessionList;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category = "ApexSim|UI")
	TObjectPtr<UTextBlock> StatusText;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category = "ApexSim|UI")
	TObjectPtr<UButton> RefreshButton;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category = "ApexSim|UI")
	TObjectPtr<UButton> CarSelectorButton;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category = "ApexSim|UI")
	TObjectPtr<UButton> CreateSessionButton;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category = "ApexSim|UI")
	TObjectPtr<UButton> BackButton;

	/** Set to WBP_SessionRow in the WBP defaults. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "ApexSim|UI")
	TSubclassOf<UApexSessionRowWidget> SessionRowClass;

private:
	UFUNCTION() void HandleRefreshClicked();
	UFUNCTION() void HandleCarSelectorClicked();
	UFUNCTION() void HandleCreateClicked();
	UFUNCTION() void HandleBackClicked();
	UFUNCTION() void HandleJoinClicked(UApexSessionRowWidget* Row);
	UFUNCTION() void HandleLobbyStateUpdated(const FApexLobbyState& LobbyState);

	void RefreshList();

	UPROPERTY(Transient)
	TArray<TObjectPtr<UApexSessionRowWidget>> Rows;

	TArray<FString> BuiltSessionIds;
};
