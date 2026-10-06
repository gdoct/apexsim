#include "ApexSideView.h"

#include "ApexMultiViewSubsystem.h"
#include "Engine/GameInstance.h"
#include "Engine/LocalPlayer.h"
#include "Kismet/GameplayStatics.h"

void AApexSideViewCameraManager::UpdateViewTarget(FTViewTarget& OutVT, float DeltaTime)
{
	const APlayerController* Driver = UGameplayStatics::GetPlayerController(this, 0);
	const APlayerCameraManager* Source = Driver && Driver != PCOwner ? Driver->PlayerCameraManager : nullptr;
	const UGameInstance* GameInstance = GetGameInstance();
	const UApexMultiViewSubsystem* MultiView = GameInstance ? GameInstance->GetSubsystem<UApexMultiViewSubsystem>() : nullptr;
	const ULocalPlayer* Player = PCOwner ? PCOwner->GetLocalPlayer() : nullptr;
	const int32 Side = MultiView ? MultiView->SideOf(Player) : 0;
	if (!Source || Side == 0)
	{
		Super::UpdateViewTarget(OutVT, DeltaTime);
		return;
	}

	// The driver's view as it will be drawn this frame: the same place, the
	// same lens, the same grading. The yaw is about the camera's own up, so
	// a cockpit view rolling with the car takes its side panels with it.
	const FMinimalViewInfo& Base = Source->GetCameraCacheView();
	const ApexMultiView::FSideView View = MultiView->SideView(Side < 0, Base.FOV);

	FMinimalViewInfo POV = Base;
	POV.Rotation = (Base.Rotation.Quaternion() * FRotator(0.0f, View.YawDeg, 0.0f).Quaternion()).Rotator();
	POV.FOV = View.FovDeg;
	POV.OffCenterProjectionOffset = FVector2D(View.OffCenterX, 0.0f);
	POV.bConstrainAspectRatio = false;
	POV.ProjectionMode = ECameraProjectionMode::Perspective;
	OutVT.POV = POV;

	// A cut on the driver's view (a new followed car, a broadcast shot) is a
	// cut here too, or the side views smear their temporal history across it.
	if (Source->bGameCameraCutThisFrame)
	{
		bGameCameraCutThisFrame = true;
	}
}

AApexSideViewController::AApexSideViewController()
{
	PlayerCameraManagerClass = AApexSideViewCameraManager::StaticClass();
	bShowMouseCursor = false;
	// Nothing of the driver's input reaches this controller, but a second
	// gamepad would: it is not a second player.
	bBlockInput = true;
}
