#pragma once

#include "CoreMinimal.h"
#include "ApexProtocolTypes.h"
#include "Blueprint/UserWidget.h"

#include "ApexSessionRowWidget.generated.h"

class UApexSessionRowWidget;
class UButton;
class UTextBlock;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FApexOnSessionJoinClicked, UApexSessionRowWidget*, Row);

/** One session in the browser: track, host, occupancy, kind and state. */
UCLASS(Abstract)
class APEXSIM_API UApexSessionRowWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	virtual void NativeConstruct() override;

	/** bCanJoin is false when the player has not picked a car yet. */
	UFUNCTION(BlueprintCallable, Category = "ApexSim|UI")
	void SetSession(const FApexSessionSummary& Summary, bool bCanJoin);

	UFUNCTION(BlueprintPure, Category = "ApexSim|UI")
	const FString& GetSessionId() const { return SessionId; }

	UPROPERTY(BlueprintAssignable, Category = "ApexSim|UI")
	FApexOnSessionJoinClicked OnJoinClicked;

protected:
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category = "ApexSim|UI")
	TObjectPtr<UTextBlock> TrackNameText;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category = "ApexSim|UI")
	TObjectPtr<UTextBlock> HostAndPlayersText;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category = "ApexSim|UI")
	TObjectPtr<UTextBlock> KindAndStateText;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category = "ApexSim|UI")
	TObjectPtr<UButton> JoinButton;

	// Colour-coded state, matching SessionBrowserDialog.cs:265-275.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ApexSim|UI")
	FLinearColor LobbyColor = FLinearColor(0.30f, 0.85f, 0.40f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ApexSim|UI")
	FLinearColor CountdownColor = FLinearColor(0.95f, 0.85f, 0.25f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ApexSim|UI")
	FLinearColor RacingColor = FLinearColor(0.95f, 0.55f, 0.20f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ApexSim|UI")
	FLinearColor FinishedColor = FLinearColor(0.55f, 0.55f, 0.55f);

private:
	UFUNCTION()
	void HandleJoinClicked();

	static FString DescribeKind(EApexSessionKind Kind);
	static FString DescribeState(EApexSessionState State);
	FLinearColor ColorForState(EApexSessionState State) const;

	FString SessionId;
};
