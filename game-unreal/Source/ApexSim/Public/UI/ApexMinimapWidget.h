#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"

#include "ApexMinimapWidget.generated.h"

/** One car on the map. Position is in the server's frame, in metres. */
struct FApexMinimapBlip
{
	FVector2D Position = FVector2D::ZeroVector;
	FLinearColor Colour = FLinearColor::White;
	/** The local player's car is drawn larger and last, so it is never hidden. */
	bool bIsLocal = false;
};

/**
 * The circuit outline with a dot per car.
 *
 * Painted rather than composed: a track is a few thousand points, and one
 * widget per segment would be absurd. NativeOnPaint draws the line strip and
 * the blips directly, which also means the whole map costs one draw call each.
 *
 * The shape comes from `TrackConfigSummary.Centerline`, which the protocol
 * codec only parses when `apexsim.net.ParseCenterline` is on — the settings
 * subsystem turns it on exactly when the minimap is being shown.
 */
UCLASS()
class APEXSIM_API UApexMinimapWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	UApexMinimapWidget(const FObjectInitializer& ObjectInitializer);

	/** Replaces the circuit outline and recomputes the fit. Cheap to call rarely. */
	void SetCenterline(const TArray<FVector2D>& Points);

	bool HasCenterline() const { return Centerline.Num() > 1; }

	/** Replaces the car dots. Called every telemetry frame. */
	void SetBlips(TArray<FApexMinimapBlip>&& InBlips);

protected:
	virtual int32 NativePaint(
		const FPaintArgs& Args,
		const FGeometry& AllottedGeometry,
		const FSlateRect& MyCullingRect,
		FSlateWindowElementList& OutDrawElements,
		int32 LayerId,
		const FWidgetStyle& InWidgetStyle,
		bool bParentEnabled) const override;

private:
	/** Server metres to widget-local pixels, preserving aspect and centring. */
	FVector2D ToLocal(const FVector2D& World, const FVector2D& LocalSize) const;

	TArray<FVector2D> Centerline;
	TArray<FApexMinimapBlip> Blips;

	/** Bounding box of the centerline, in server metres. */
	FVector2D WorldMin = FVector2D::ZeroVector;
	FVector2D WorldMax = FVector2D::ZeroVector;
};
