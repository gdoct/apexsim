#pragma once

#include "Camera/PlayerCameraManager.h"
#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"

#include "ApexSideView.generated.h"

/**
 * The camera of a side viewer (see UApexMultiViewSubsystem): it does not
 * look at anything of its own. Every frame it takes the view the driver's
 * camera manager just worked out — cockpit, chase, broadcast, shot camera,
 * whichever is active, with its post-process — and turns it by the rig's
 * side angle with the off-axis frustum of that panel
 * (ApexMultiView::SideView), so the three views are three windows onto one
 * scene. The engine updates the camera managers in player order with the
 * driver's first, which is what makes "just worked out" true.
 */
UCLASS()
class APEXSIM_API AApexSideViewCameraManager : public APlayerCameraManager
{
	GENERATED_BODY()

public:
	virtual void UpdateViewTarget(FTViewTarget& OutVT, float DeltaTime) override;
};

/**
 * The player controller of a side viewer: a local player whose only purpose
 * is to own one of the two side views. It drives nothing, draws no HUD and
 * takes no input — the keyboard, mouse and the wheel all belong to the
 * first local player — and it is spawned by AApexMenuGameModeBase in place
 * of AApexPlayerController while UApexMultiViewSubsystem says so.
 */
UCLASS()
class APEXSIM_API AApexSideViewController : public APlayerController
{
	GENERATED_BODY()

public:
	AApexSideViewController();
};
