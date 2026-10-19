#pragma once

#include "CoreMinimal.h"
#include "Engine/GameViewportClient.h"

#include "ApexGameViewportClient.generated.h"

/**
 * The game's viewport client, named in DefaultEngine.ini. It exists for
 * one thing: laying the three views of a triple-monitor rig out across the
 * window as centre, left, right — the driver's view (the first local
 * player) in the middle third and the two side viewers either side of it.
 * The engine's own three-player column layout puts the first player on the
 * left, and its choice of layout is a project setting rather than
 * something a subsystem can ask for.
 */
UCLASS()
class APEXSIM_API UApexGameViewportClient : public UGameViewportClient
{
	GENERATED_BODY()

public:
	UApexGameViewportClient();

	/** On: three local players are drawn centre-left-right. Off: the engine's own rules. */
	void SetTripleLayout(bool bEnable);

	virtual void UpdateActiveSplitscreenType() override;

private:
	bool bTripleLayout = false;
};
