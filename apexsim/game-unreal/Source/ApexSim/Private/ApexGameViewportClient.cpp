#include "ApexGameViewportClient.h"

#include "Engine/Engine.h"
#include "Engine/LocalPlayer.h"

UApexGameViewportClient::UApexGameViewportClient()
{
	// The engine fills SplitscreenInfo with its layouts in the base
	// constructor; the three-column one is rewritten so the first player —
	// the driver — is the middle column. Players two and three are the side
	// viewers, added in that order: left first, then right.
	if (SplitscreenInfo.IsValidIndex(ESplitScreenType::ThreePlayer_Vertical))
	{
		constexpr float Third = 1.0f / 3.0f;
		TArray<FPerPlayerSplitscreenData>& Columns = SplitscreenInfo[ESplitScreenType::ThreePlayer_Vertical].PlayerData;
		Columns.Reset();
		Columns.Add(FPerPlayerSplitscreenData(Third, 1.0f, Third, 0.0f));
		Columns.Add(FPerPlayerSplitscreenData(Third, 1.0f, 0.0f, 0.0f));
		Columns.Add(FPerPlayerSplitscreenData(Third, 1.0f, 2.0f * Third, 0.0f));
	}
}

void UApexGameViewportClient::SetTripleLayout(bool bEnable)
{
	bTripleLayout = bEnable;
}

void UApexGameViewportClient::UpdateActiveSplitscreenType()
{
	Super::UpdateActiveSplitscreenType();
	if (!bTripleLayout)
	{
		return;
	}
	int32 Players = 0;
	for (const ULocalPlayer* Player : GEngine->GetGamePlayers(this))
	{
		if (Player && Player->GetViewportClient() == this)
		{
			++Players;
		}
	}
	if (Players == 3)
	{
		ActiveSplitscreenType = ESplitScreenType::ThreePlayer_Vertical;
	}
}
