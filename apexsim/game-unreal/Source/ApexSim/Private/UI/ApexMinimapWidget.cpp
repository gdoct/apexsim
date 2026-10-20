#include "UI/ApexMinimapWidget.h"

#include "ApexProtocolTypes.h"

#include "Rendering/DrawElements.h"
#include "Styling/CoreStyle.h"
#include "UI/ApexUIStyle.h"

namespace
{
	/** Pixels kept clear around the outline so a blip on the edge is not clipped. */
	constexpr float MapPadding = 18.0f;
	constexpr float TrackLineWidth = 3.0f;
	constexpr float BlipRadius = 3.5f;
	constexpr float LocalBlipRadius = 5.5f;
	/** Rain cells are drawn as this many filled rings, inner ones stronger. */
	constexpr int32 RainRings = 4;
	constexpr int32 RainSegments = 20;

	/** splitmix64 of a seed and a salt: the radar's shapes, the same on every client. */
	uint64 RadarHash(uint64 Seed, uint64 Salt)
	{
		uint64 Z = Seed + 0x9E3779B97F4A7C15ull * (Salt + 1);
		Z = (Z ^ (Z >> 30)) * 0xBF58476D1CE4E5B9ull;
		Z = (Z ^ (Z >> 27)) * 0x94D049BB133111EBull;
		return Z ^ (Z >> 31);
	}

	/** 0..1 from a seed and a salt. */
	float Radar01(uint64 Seed, uint64 Salt)
	{
		return static_cast<float>(RadarHash(Seed, Salt) >> 40) / static_cast<float>(1ull << 24);
	}
}

namespace ApexRainRadar
{
	float Intensity(int32 Weather)
	{
		switch (static_cast<EApexWeather>(Weather))
		{
		case EApexWeather::LightRain: return 0.45f;
		case EApexWeather::HeavyRain: return 1.0f;
		case EApexWeather::Snow: return 0.6f;
		default: return 0.0f;
		}
	}

	bool IsSnow(int32 Weather)
	{
		return static_cast<EApexWeather>(Weather) == EApexWeather::Snow;
	}

	FLinearColor Colour(const FApexRainCell& Cell)
	{
		if (Cell.bSnow)
		{
			return FLinearColor(0.92f, 0.95f, 1.0f, 0.55f);
		}
		// Green-blue for light rain through yellow to red for the heaviest.
		const float T = FMath::Clamp(Cell.Intensity, 0.0f, 1.0f);
		const FLinearColor Light(0.15f, 0.75f, 0.85f);
		const FLinearColor Mid(0.95f, 0.85f, 0.15f);
		const FLinearColor Heavy(0.95f, 0.2f, 0.1f);
		FLinearColor Out = T < 0.6f ? FLinearColor::LerpUsingHSV(Light, Mid, T / 0.6f)
									: FLinearColor::LerpUsingHSV(Mid, Heavy, (T - 0.6f) / 0.4f);
		Out.A = 0.45f;
		return Out;
	}

	TArray<FApexRainCell> Cells(const FInputs& In)
	{
		TArray<FApexRainCell> Out;
		const float NowIntensity = FMath::Clamp(In.RainPct / 100.0f, 0.0f, 1.0f);
		const bool bNextWetter = In.NextWeather >= 0 && In.NextInS > 0 && Intensity(In.NextWeather) > NowIntensity + 0.05f;
		const bool bRaining = NowIntensity > 0.02f;
		if (!bNextWetter && !bRaining)
		{
			return Out;
		}

		// The wind carries the band; in a calm it still drifts, from a seeded way.
		const float WindMps = In.WindKph / 3.6f;
		const float ToRad = WindMps > 0.5f ? FMath::DegreesToRadians(In.WindToDeg)
										   : Radar01(In.Seed, 77) * 2.0f * PI;
		const float SpeedMps = FMath::Max(WindMps, 4.0f);
		const FVector2D Down(FMath::Cos(ToRad), FMath::Sin(ToRad));
		const FVector2D Across(-Down.Y, Down.X);
		const float Size = FMath::Max(In.TrackRadiusM, 200.0f);

		// The band along the wind, as distances downwind of the circuit's
		// middle: [Rear, Front].
		float Rear = 0.0f;
		float Front = 0.0f;
		int32 Weather = In.Weather;
		if (bNextWetter)
		{
			// Coming: its front edge reaches the circuit's upwind side as the
			// step arrives.
			Front = -Size - SpeedMps * In.NextInS;
			Rear = Front - 2.5f * Size;
			Weather = In.NextWeather;
		}
		else
		{
			// Here: it covers the circuit; with a drier step ahead, its rear
			// edge passes the circuit's downwind side as that step arrives.
			const bool bDrying = In.NextWeather >= 0 && In.NextInS > 0;
			Rear = bDrying ? Size - SpeedMps * In.NextInS : -2.5f * Size;
			Front = FMath::Max(Rear + 2.5f * Size, 2.5f * Size);
			Rear = FMath::Min(Rear, Front - 2.5f * Size);
		}
		const float Strength = FMath::Max(Intensity(Weather), NowIntensity);
		if (Strength <= 0.0f)
		{
			return Out;
		}

		// Cells strewn through the band from the seed, drifting through it
		// with the sky's clock while it holds.
		constexpr int32 CellCount = 16;
		const float Depth = Front - Rear;
		const float Drift = FMath::Fmod(In.ClockS * SpeedMps, FMath::Max(Depth, 1.0f));
		for (int32 Index = 0; Index < CellCount; ++Index)
		{
			const uint64 Salt = static_cast<uint64>(Index) * 8;
			float Along = Radar01(In.Seed, Salt) * Depth;
			if (!bNextWetter)
			{
				Along = FMath::Fmod(Along + Drift, FMath::Max(Depth, 1.0f));
			}
			const float Side = (Radar01(In.Seed, Salt + 1) - 0.5f) * 2.4f * Size;
			FApexRainCell Cell;
			Cell.CentreM = In.TrackCentreM + Down * (Rear + Along) + Across * Side;
			Cell.RadiusM = Size * (0.18f + 0.22f * Radar01(In.Seed, Salt + 2));
			Cell.Intensity = Strength * (0.55f + 0.45f * Radar01(In.Seed, Salt + 3));
			Cell.bSnow = IsSnow(Weather);
			Out.Add(Cell);
		}
		return Out;
	}
}

UApexMinimapWidget::UApexMinimapWidget(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// Nothing on the map is clickable, and it sits over the race view.
	SetVisibility(ESlateVisibility::HitTestInvisible);
}

void UApexMinimapWidget::SetCenterline(const TArray<FVector2D>& Points)
{
	Centerline = Points;

	WorldMin = FVector2D::ZeroVector;
	WorldMax = FVector2D::ZeroVector;
	if (Centerline.Num() == 0)
	{
		return;
	}

	WorldMin = Centerline[0];
	WorldMax = Centerline[0];
	for (const FVector2D& Point : Centerline)
	{
		WorldMin.X = FMath::Min(WorldMin.X, Point.X);
		WorldMin.Y = FMath::Min(WorldMin.Y, Point.Y);
		WorldMax.X = FMath::Max(WorldMax.X, Point.X);
		WorldMax.Y = FMath::Max(WorldMax.Y, Point.Y);
	}
	UpdateView();
}

void UApexMinimapWidget::SetBlips(TArray<FApexMinimapBlip>&& InBlips)
{
	Blips = MoveTemp(InBlips);
}

void UApexMinimapWidget::SetRain(TArray<FApexRainCell>&& InCells)
{
	Rain = MoveTemp(InCells);
	UpdateView();
}

void UApexMinimapWidget::UpdateView()
{
	ViewMin = WorldMin;
	ViewMax = WorldMax;
	if (Rain.Num() == 0)
	{
		return;
	}
	// Out to take in the band, no further than RainZoomMax circuits across,
	// round the circuit's own middle so it stays where the driver knows it.
	const FVector2D Centre = (WorldMin + WorldMax) * 0.5f;
	const FVector2D Half = (WorldMax - WorldMin) * 0.5f;
	const float Limit = FMath::Max(Half.X, Half.Y) * RainZoomMax;
	float Reach = FMath::Max(Half.X, Half.Y);
	for (const FApexRainCell& Cell : Rain)
	{
		const FVector2D Off = Cell.CentreM - Centre;
		Reach = FMath::Max(Reach, FMath::Max(FMath::Abs(Off.X), FMath::Abs(Off.Y)) + Cell.RadiusM);
	}
	Reach = FMath::Min(Reach, Limit);
	ViewMin = FVector2D(FMath::Min(WorldMin.X, Centre.X - Reach), FMath::Min(WorldMin.Y, Centre.Y - Reach));
	ViewMax = FVector2D(FMath::Max(WorldMax.X, Centre.X + Reach), FMath::Max(WorldMax.Y, Centre.Y + Reach));
}

FVector2D UApexMinimapWidget::ToLocal(const FVector2D& World, const FVector2D& LocalSize) const
{
	const FVector2D Extent = ViewMax - ViewMin;
	const float Span = FMath::Max(FMath::Max(Extent.X, Extent.Y), 1.0f);

	// One scale for both axes, or a long circuit would be stretched into its box
	// and stop being recognisable.
	const float Usable = FMath::Max(FMath::Min(LocalSize.X, LocalSize.Y) - MapPadding * 2.0f, 1.0f);
	const float Scale = Usable / Span;

	// The circuit is centred in whatever box it is given, so a non-square panel
	// does not push it against one edge.
	const FVector2D Centred = (World - (ViewMin + ViewMax) * 0.5f) * Scale;

	// Server +Y is left of the track and screen +Y is down, so the vertical axis
	// flips here — the same handedness conversion the race coordinates do.
	return FVector2D(LocalSize.X * 0.5f + Centred.X, LocalSize.Y * 0.5f - Centred.Y);
}

int32 UApexMinimapWidget::NativePaint(
	const FPaintArgs& Args,
	const FGeometry& AllottedGeometry,
	const FSlateRect& MyCullingRect,
	FSlateWindowElementList& OutDrawElements,
	int32 LayerId,
	const FWidgetStyle& InWidgetStyle,
	bool bParentEnabled) const
{
	const int32 BaseLayer = Super::NativePaint(
		Args, AllottedGeometry, MyCullingRect, OutDrawElements, LayerId, InWidgetStyle, bParentEnabled);

	const FVector2D LocalSize = AllottedGeometry.GetLocalSize();
	if (Centerline.Num() < 2 || LocalSize.X <= 0.0f || LocalSize.Y <= 0.0f)
	{
		return BaseLayer;
	}

	TArray<FVector2D> Screen;
	Screen.Reserve(Centerline.Num() + 1);
	for (const FVector2D& Point : Centerline)
	{
		Screen.Add(ToLocal(Point, LocalSize));
	}
	// Circuits are closed; the centerline is not, so the last segment is drawn
	// explicitly rather than leaving a gap at the start/finish line. The first
	// point is copied out before the Add: TArray refuses an element that points
	// into the array being added to, reserved capacity or not.
	const FVector2D StartFinish = Screen[0];
	Screen.Add(StartFinish);

	// The radar under the outline: each cell a stack of filled rings, the
	// inner ones stronger, clipped to the map's box.
	int32 RainLayer = BaseLayer;
	if (Rain.Num() > 0)
	{
		RainLayer = BaseLayer + 1;
		const FVector2D Origin = ToLocal(FVector2D::ZeroVector, LocalSize);
		const float PixelsPerMetre = (ToLocal(FVector2D(1000.0f, 0.0f), LocalSize) - Origin).Size() / 1000.0f;
		OutDrawElements.PushClip(FSlateClippingZone(AllottedGeometry));
		TArray<FVector2D> Ring;
		Ring.Reserve(RainSegments + 1);
		for (const FApexRainCell& Cell : Rain)
		{
			const FVector2D Centre = ToLocal(Cell.CentreM, LocalSize);
			const float Radius = Cell.RadiusM * PixelsPerMetre;
			const FLinearColor Colour = ApexRainRadar::Colour(Cell);
			for (int32 K = 0; K < RainRings; ++K)
			{
				// Ring K spans the band (K/N .. (K+1)/N) of the radius.
				const float Mid = Radius * (K + 0.5f) / RainRings;
				Ring.Reset();
				for (int32 Seg = 0; Seg <= RainSegments; ++Seg)
				{
					const float A = 2.0f * PI * Seg / RainSegments;
					Ring.Add(Centre + FVector2D(FMath::Cos(A), FMath::Sin(A)) * Mid);
				}
				FLinearColor Shade = Colour;
				Shade.A *= 1.0f - 0.6f * K / RainRings;
				FSlateDrawElement::MakeLines(OutDrawElements, RainLayer, AllottedGeometry.ToPaintGeometry(), Ring,
					ESlateDrawEffect::None, Shade, /*bAntialias*/ true, FMath::Max(Radius / RainRings, 1.0f));
			}
		}
		OutDrawElements.PopClip();
	}

	const int32 TrackLayer = RainLayer + 1;
	FSlateDrawElement::MakeLines(
		OutDrawElements,
		TrackLayer,
		AllottedGeometry.ToPaintGeometry(),
		Screen,
		ESlateDrawEffect::None,
		ApexUI::Palette::TextMuted,
		/*bAntialias*/ true,
		TrackLineWidth);

	const int32 BlipLayer = TrackLayer + 1;
	const FSlateBrush* Dot = FCoreStyle::Get().GetBrush(TEXT("WhiteBrush"));

	auto DrawBlip = [&](const FApexMinimapBlip& Blip)
	{
		const float Radius = Blip.bIsLocal ? LocalBlipRadius : BlipRadius;
		const FVector2D Position = ToLocal(Blip.Position, LocalSize) - FVector2D(Radius, Radius);
		FSlateDrawElement::MakeBox(
			OutDrawElements,
			BlipLayer,
			AllottedGeometry.ToPaintGeometry(FVector2D(Radius * 2.0f, Radius * 2.0f), FSlateLayoutTransform(Position)),
			Dot,
			ESlateDrawEffect::None,
			Blip.Colour);
	};

	// Rivals first, then the local car, so its own dot is never buried under one
	// of theirs in a tight pack.
	for (const FApexMinimapBlip& Blip : Blips)
	{
		if (!Blip.bIsLocal)
		{
			DrawBlip(Blip);
		}
	}
	for (const FApexMinimapBlip& Blip : Blips)
	{
		if (Blip.bIsLocal)
		{
			DrawBlip(Blip);
		}
	}

	return BlipLayer;
}
