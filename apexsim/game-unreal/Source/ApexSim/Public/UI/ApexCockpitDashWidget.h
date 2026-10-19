#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"

#include "ApexCockpitDashWidget.generated.h"

class UBorder;
class UTextBlock;

/**
 * The display on the steering wheel hub: gear, speed, an RPM light bar and
 * the lap in progress.
 *
 * Rendered into the world by a widget component on the cockpit rig rather
 * than onto the screen, so it is the shape of a real wheel display — wide,
 * short, and readable at arm's length — and turns with the rim. Nothing is
 * derived here: the rig pushes the followed car's telemetry each frame.
 */
UCLASS()
class APEXSIM_API UApexCockpitDashWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	UApexCockpitDashWidget(const FObjectInitializer& ObjectInitializer);

	virtual void NativeOnInitialized() override;

	/** Pixel size the widget is laid out for; the rig scales it into centimetres. */
	static constexpr int32 DrawWidth = 512;
	static constexpr int32 DrawHeight = 224;

	/**
	 * One frame of numbers. RpmFraction is rpm over the highest seen, so the
	 * bar always reaches its lights before the shift — the wire never states
	 * a redline.
	 */
	void SetValues(int32 Gear, float SpeedKph, bool bMetric, float RpmFraction, int32 Lap, int32 LapTimeMs, int32 CountdownMs);

private:
	void BuildTree();

	UPROPERTY(Transient) TArray<TObjectPtr<UBorder>> RpmLights;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> GearText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> SpeedText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> SpeedUnitText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> LapText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> LapTimeText;

	int32 LitLights = -1;
	int32 LastGear = INT32_MIN;
	int32 LastSpeed = INT32_MIN;
	bool bLastMetric = true;
	int32 LastLap = INT32_MIN;
	int32 LastTimeTenths = INT32_MIN;

	static constexpr int32 LightCount = 15;
};
