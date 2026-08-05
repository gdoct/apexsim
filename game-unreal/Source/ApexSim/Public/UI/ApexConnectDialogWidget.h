#pragma once

#include "CoreMinimal.h"
#include "ApexProtocolTypes.h"
#include "UI/ApexScreenWidget.h"

#include "ApexConnectDialogWidget.generated.h"

class UButton;
class UEditableTextBox;
class UTextBlock;

/**
 * Host / port / player name / token, and the Connect button.
 *
 * The token field exists because the server supports `[auth] mode = "token"`.
 * In the default dev mode any value is accepted (transport.rs:247).
 */
UCLASS(Abstract)
class APEXSIM_API UApexConnectDialogWidget : public UApexScreenWidget
{
	GENERATED_BODY()

public:
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;
	virtual void OnScreenActivated() override;

protected:
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category = "ApexSim|UI")
	TObjectPtr<UEditableTextBox> HostBox;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category = "ApexSim|UI")
	TObjectPtr<UEditableTextBox> PortBox;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category = "ApexSim|UI")
	TObjectPtr<UEditableTextBox> PlayerNameBox;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category = "ApexSim|UI")
	TObjectPtr<UEditableTextBox> TokenBox;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category = "ApexSim|UI")
	TObjectPtr<UButton> ConnectButton;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category = "ApexSim|UI")
	TObjectPtr<UButton> BackButton;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category = "ApexSim|UI")
	TObjectPtr<UTextBlock> StatusText;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ApexSim|UI")
	FLinearColor PendingColor = FLinearColor(0.95f, 0.80f, 0.25f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ApexSim|UI")
	FLinearColor SuccessColor = FLinearColor(0.25f, 0.85f, 0.35f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ApexSim|UI")
	FLinearColor ErrorColor = FLinearColor(0.90f, 0.30f, 0.30f);

private:
	UFUNCTION() void HandleConnectClicked();
	UFUNCTION() void HandleBackClicked();
	UFUNCTION() void HandleConnectionStateChanged(EApexConnectionState NewState, const FString& Detail);
	UFUNCTION() void HandleAuthSucceeded(const FString& PlayerId, int32 ServerVersion);
	UFUNCTION() void HandleAuthFailed(const FString& Reason);

	/** Copies the current field values back into the flow subsystem. */
	void CommitFields();
	void SetStatus(const FString& Message, const FLinearColor& Color);
};
