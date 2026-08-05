#include "UI/ApexPlayerRowWidget.h"

#include "Components/TextBlock.h"

void UApexPlayerRowWidget::SetPlayer(const FApexLobbyPlayer& Player, bool bIsLocalPlayer)
{
	const bool bReady = Player.HasSelectedCar();

	if (PlayerNameText)
	{
		PlayerNameText->SetText(FText::FromString(
			bIsLocalPlayer ? FString::Printf(TEXT("%s  (you)"), *Player.Name) : Player.Name));
		PlayerNameText->SetColorAndOpacity(bReady ? ReadyColor : NotReadyColor);
	}

	if (ReadyGlyphText)
	{
		// A car selection is the only readiness signal the lobby protocol has.
		ReadyGlyphText->SetText(FText::FromString(bReady ? TEXT("✓") : TEXT("○")));
		ReadyGlyphText->SetColorAndOpacity(bReady ? ReadyColor : NotReadyColor);
	}
}
