#pragma once

#include "CoreMinimal.h"
#include "ApexProtocolTypes.h"
#include "UI/ApexScreenWidget.h"

#include "ApexMainMenuWidget.generated.h"

class UButton;
class UImage;
class UTextBlock;

/**
 * Landing screen. Auto-connects to the configured server the first time it is
 * shown (mirroring MainMenu.cs:128-145), and gates the buttons that only make
 * sense once authenticated.
 */
UCLASS(Abstract)
class APEXSIM_API UApexMainMenuWidget : public UApexScreenWidget
{
	GENERATED_BODY()

public:
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;
	virtual void OnScreenActivated() override;

protected:
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category = "ApexSim|UI")
	TObjectPtr<UImage> LogoImage;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category = "ApexSim|UI")
	TObjectPtr<UTextBlock> SubtitleText;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category = "ApexSim|UI")
	TObjectPtr<UTextBlock> ConnectionStatusText;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category = "ApexSim|UI")
	TObjectPtr<UButton> ConnectButton;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category = "ApexSim|UI")
	TObjectPtr<UButton> BrowseSessionsButton;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category = "ApexSim|UI")
	TObjectPtr<UButton> CreateSessionButton;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category = "ApexSim|UI")
	TObjectPtr<UButton> CarSelectButton;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category = "ApexSim|UI")
	TObjectPtr<UButton> TrackSelectButton;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category = "ApexSim|UI")
	TObjectPtr<UButton> QuitButton;

	/** Logo width in slate units; height follows the source 4:1 aspect. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ApexSim|UI")
	float LogoWidth = 560.0f;

private:
	UFUNCTION() void HandleConnectClicked();
	UFUNCTION() void HandleBrowseClicked();
	UFUNCTION() void HandleCreateClicked();
	UFUNCTION() void HandleCarSelectClicked();
	UFUNCTION() void HandleTrackSelectClicked();
	UFUNCTION() void HandleQuitClicked();

	UFUNCTION() void HandleConnectionStateChanged(EApexConnectionState NewState, const FString& Detail);
	UFUNCTION() void HandleLobbyStateUpdated(const FApexLobbyState& LobbyState);

	void RefreshButtons();
};
