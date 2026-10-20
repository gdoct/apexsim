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

/** One rain cell on the radar: a disc in the server's frame, metres. */
struct FApexRainCell
{
	FVector2D CentreM = FVector2D::ZeroVector;
	float RadiusM = 0.0f;
	/** 0..1 of heavy rain, the radar colour's scale. */
	float Intensity = 0.0f;
	bool bSnow = false;
};

/**
 * The rain radar's picture (roadmap #26). The server's rain is circuit-wide
 * and follows the forecast in time, with no clouds in space, so the radar
 * draws what is known: a band of cells upwind of the circuit at the
 * distance the wind covers before the forecast's next wetter step, which
 * drifts in at the wind's speed and reaches the circuit as that rain
 * starts; rain falling now covers the circuit, its band's rear edge
 * passing as the forecast dries. The cells' shapes come from the sky's
 * seed, and the drift from the server's own clock (`next_in_s`, the sky's
 * clock), so every client and a replay draw the same picture.
 */
namespace ApexRainRadar
{
	struct FInputs
	{
		/** The sky's seed (`SessionConditions.sky_seed`), or any fixed number. */
		uint64 Seed = 0;
		/** The circuit's middle and half its size, metres. */
		FVector2D TrackCentreM = FVector2D::ZeroVector;
		float TrackRadiusM = 1000.0f;
		/** EApexWeather values: now and the forecast's next (-1 none) in NextInS. */
		int32 Weather = 0;
		int32 RainPct = 0;
		int32 NextWeather = -1;
		int32 NextInS = 0;
		/** The wind: speed, and where it blows toward (degrees CCW from +X). */
		float WindKph = 0.0f;
		float WindToDeg = 0.0f;
		/** The day's clock, seconds: the band's inner drift while the sky holds. */
		int32 ClockS = 0;
	};

	/** The cells to draw; none for a dry sky with no rain ahead. */
	APEXSIM_API TArray<FApexRainCell> Cells(const FInputs& In);

	/** How hard a weather rains, 0..1 (0 for a dry one), and whether it is snow. */
	APEXSIM_API float Intensity(int32 Weather);
	APEXSIM_API bool IsSnow(int32 Weather);

	/** A radar's colour for a cell: light rain green-blue, heavy yellow-red, snow white. */
	APEXSIM_API FLinearColor Colour(const FApexRainCell& Cell);
}

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

	/**
	 * The rain radar's cells (ApexRainRadar::Cells), drawn under the
	 * outline; with any, the map zooms out to take in the band up to
	 * RainZoomMax times the circuit's size. Called every telemetry frame.
	 */
	void SetRain(TArray<FApexRainCell>&& InCells);

	/** How far out the map zooms for the radar, in the circuit's own size. */
	static constexpr float RainZoomMax = 3.0f;

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

	/** The box shown: the circuit's, grown to take in the radar's band. */
	void UpdateView();

	TArray<FVector2D> Centerline;
	TArray<FApexMinimapBlip> Blips;
	TArray<FApexRainCell> Rain;

	/** Bounding box of the centerline, in server metres. */
	FVector2D WorldMin = FVector2D::ZeroVector;
	FVector2D WorldMax = FVector2D::ZeroVector;
	/** What the map shows, in server metres. */
	FVector2D ViewMin = FVector2D::ZeroVector;
	FVector2D ViewMax = FVector2D::ZeroVector;
};
