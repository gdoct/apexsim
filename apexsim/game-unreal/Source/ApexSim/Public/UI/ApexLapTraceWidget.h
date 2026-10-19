#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Race/ApexLapTrace.h"

#include "ApexLapTraceWidget.generated.h"

/**
 * Two laps against each other by distance (the pause menu's LAP TRACES
 * page): speed, throttle and brake, and the time the lap loses or gains on
 * the reference, in three bands over the lap. The reference (the best) is
 * drawn muted, the lap (the last) in the accent colour. Painted, like the
 * minimap: a lap is a few thousand points.
 */
UCLASS()
class APEXSIM_API UApexLapTraceWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	/** Copies both; either may be empty (then only the other is drawn, and no delta). */
	void SetTraces(const FApexLapTrace& Reference, const FApexLapTrace& Lap);

protected:
	virtual int32 NativePaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
		FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;

private:
	FApexLapTrace Reference;
	FApexLapTrace Lap;
	TArray<FVector2f> Delta;
};
