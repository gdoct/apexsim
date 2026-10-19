#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"

#include "ApexMirrorWidget.generated.h"

class UImage;
class UTextureRenderTarget2D;

/**
 * A mirror face: a scene capture's render target inside a thin bezel.
 *
 * The capture looks backwards, which is not what a mirror shows — a mirror
 * shows the rear view reversed left-to-right, so a car on your right stays
 * on your right in the glass. The image is drawn flipped for that reason,
 * and the same widget serves both the rig's glass and the HUD's virtual
 * mirror so the two never disagree about it.
 */
UCLASS()
class APEXSIM_API UApexMirrorWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	UApexMirrorWidget(const FObjectInitializer& ObjectInitializer);

	virtual void NativeOnInitialized() override;

	/** Size the face is laid out at, in widget pixels. Call before the first paint. */
	void SetFaceSize(const FVector2D& Size);

	/** The capture to show; null blanks the glass. */
	void SetTexture(UTextureRenderTarget2D* Texture);

private:
	UPROPERTY(Transient) TObjectPtr<UImage> Glass;
	UPROPERTY(Transient) TObjectPtr<class USizeBox> Frame;
	UPROPERTY(Transient) TObjectPtr<UTextureRenderTarget2D> Shown;
	FVector2D FaceSize = FVector2D(512.0f, 160.0f);
};
