#include "UI/ApexLapTraceWidget.h"

#include "Fonts/SlateFontInfo.h"
#include "Rendering/DrawElements.h"
#include "Styling/CoreStyle.h"
#include "UI/ApexUIStyle.h"

namespace
{
	/** The bands' shares of the height: speed, pedals, delta. */
	constexpr float LapTraceSpeedShare = 0.5f;
	constexpr float LapTracePedalShare = 0.25f;
	constexpr float LapTraceGapPx = 14.0f;
	/** A point per this many pixels of width: plenty for a line, cheap to paint. */
	constexpr float LapTracePixelStep = 2.0f;

	struct FLapTraceBand
	{
		float Top = 0.0f;
		float Height = 0.0f;
		float MinValue = 0.0f;
		float MaxValue = 1.0f;

		float Y(float Value) const
		{
			const float T = (Value - MinValue) / FMath::Max(MaxValue - MinValue, KINDA_SMALL_NUMBER);
			return Top + Height * (1.0f - FMath::Clamp(T, 0.0f, 1.0f));
		}
	};

	void LapTraceLine(FSlateWindowElementList& Out, int32 Layer, const FGeometry& Geometry, const TArray<FVector2D>& Points,
		const FLinearColor& Colour, float Thickness)
	{
		if (Points.Num() > 1)
		{
			FSlateDrawElement::MakeLines(Out, Layer, Geometry.ToPaintGeometry(), Points, ESlateDrawEffect::None, Colour, true, Thickness);
		}
	}

	void LapTraceLabel(FSlateWindowElementList& Out, int32 Layer, const FGeometry& Geometry, const FString& Text,
		const FVector2D& At, const FLinearColor& Colour)
	{
		const FSlateFontInfo Font = ApexUI::Font::Mono(10.0f, 80);
		FSlateDrawElement::MakeText(Out, Layer, Geometry.ToPaintGeometry(FVector2D(400.0f, 16.0f), FSlateLayoutTransform(At)),
			Text, Font, ESlateDrawEffect::None, Colour);
	}
}

void UApexLapTraceWidget::SetTraces(const FApexLapTrace& InReference, const FApexLapTrace& InLap)
{
	Reference = InReference;
	Lap = InLap;
	Delta = ApexLapTrace::DeltaByDistance(Reference, Lap, 10.0f);
	Invalidate(EInvalidateWidgetReason::Paint);
}

int32 UApexLapTraceWidget::NativePaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
	FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	const int32 Base = Super::NativePaint(Args, AllottedGeometry, MyCullingRect, OutDrawElements, LayerId, InWidgetStyle, bParentEnabled);
	const FVector2D Size = AllottedGeometry.GetLocalSize();
	const float Length = FMath::Max(Reference.LengthM(), Lap.LengthM());
	if (Size.X < 10.0f || Size.Y < 10.0f || Length <= 0.0f)
	{
		return Base;
	}
	const int32 GridLayer = Base + 1;
	const int32 RefLayer = Base + 2;
	const int32 LapLayer = Base + 3;
	const FLinearColor Grid = ApexUI::Palette::Border;
	const FLinearColor RefColour = ApexUI::Palette::TextMuted;
	const FLinearColor LapColour = ApexUI::Palette::Accent;
	const FLinearColor Label = ApexUI::Palette::TextSecondary;

	const float Usable = Size.Y - 2.0f * LapTraceGapPx;
	FLapTraceBand Speed{0.0f, Usable * LapTraceSpeedShare, 0.0f, 100.0f};
	FLapTraceBand Pedals{Speed.Height + LapTraceGapPx, Usable * LapTracePedalShare, 0.0f, 1.0f};
	FLapTraceBand Gap{Pedals.Top + Pedals.Height + LapTraceGapPx, Usable - Speed.Height - Pedals.Height, -1.0f, 1.0f};

	// Speed's scale: the faster lap's top speed, rounded up to 50 km/h.
	float Top = 100.0f;
	for (const FApexLapTrace* Trace : {&Reference, &Lap})
	{
		for (const FApexTraceSample& S : Trace->Samples)
		{
			Top = FMath::Max(Top, S.SpeedKph);
		}
	}
	Speed.MaxValue = FMath::CeilToFloat(Top / 50.0f) * 50.0f;
	// The delta's: the largest either way, at least a tenth of a second.
	float Reach = 0.1f;
	for (const FVector2f& D : Delta)
	{
		Reach = FMath::Max(Reach, FMath::Abs(D.Y));
	}
	Gap.MinValue = -Reach;
	Gap.MaxValue = Reach;

	auto X = [&](float DistanceM) { return Size.X * DistanceM / Length; };

	// The bands' frames, the delta's zero, and every 500 m.
	for (const FLapTraceBand* Band : {&Speed, &Pedals, &Gap})
	{
		LapTraceLine(OutDrawElements, GridLayer, AllottedGeometry,
			{FVector2D(0.0, Band->Top + Band->Height), FVector2D(Size.X, Band->Top + Band->Height)}, Grid, 1.0f);
	}
	LapTraceLine(OutDrawElements, GridLayer, AllottedGeometry, {FVector2D(0.0, Gap.Y(0.0f)), FVector2D(Size.X, Gap.Y(0.0f))}, Grid, 1.0f);
	for (float Mark = 500.0f; Mark < Length; Mark += 500.0f)
	{
		LapTraceLine(OutDrawElements, GridLayer, AllottedGeometry, {FVector2D(X(Mark), 0.0), FVector2D(X(Mark), Size.Y)},
			Grid.CopyWithNewOpacity(0.5f), 1.0f);
	}
	LapTraceLabel(OutDrawElements, LapLayer, AllottedGeometry, FString::Printf(TEXT("SPEED  %.0f KM/H"), Speed.MaxValue),
		FVector2D(4.0, Speed.Top + 2.0f), Label);
	LapTraceLabel(OutDrawElements, LapLayer, AllottedGeometry, TEXT("THROTTLE / BRAKE"), FVector2D(4.0, Pedals.Top + 2.0f), Label);
	LapTraceLabel(OutDrawElements, LapLayer, AllottedGeometry, FString::Printf(TEXT("DELTA  +/-%.2f S"), Reach),
		FVector2D(4.0, Gap.Top + 2.0f), Label);

	auto Draw = [&](const FApexLapTrace& Trace, int32 Layer, const FLinearColor& Colour, float Thickness)
	{
		if (Trace.IsEmpty())
		{
			return;
		}
		TArray<FVector2D> SpeedLine, ThrottleLine, BrakeLine;
		for (float Px = 0.0f; Px <= Size.X; Px += LapTracePixelStep)
		{
			const float Distance = Length * Px / Size.X;
			if (Distance > Trace.LengthM())
			{
				break;
			}
			const FApexTraceSample S = Trace.At(Distance);
			SpeedLine.Add(FVector2D(Px, Speed.Y(S.SpeedKph)));
			ThrottleLine.Add(FVector2D(Px, Pedals.Y(S.Throttle)));
			BrakeLine.Add(FVector2D(Px, Pedals.Y(S.Brake)));
		}
		LapTraceLine(OutDrawElements, Layer, AllottedGeometry, SpeedLine, Colour, Thickness);
		LapTraceLine(OutDrawElements, Layer, AllottedGeometry, ThrottleLine, Colour, Thickness);
		LapTraceLine(OutDrawElements, Layer, AllottedGeometry, BrakeLine, Colour.CopyWithNewOpacity(Colour.A * 0.6f), Thickness);
	};
	Draw(Reference, RefLayer, RefColour, 1.5f);
	Draw(Lap, LapLayer, LapColour, 2.0f);

	if (Delta.Num() > 1)
	{
		TArray<FVector2D> GapLine;
		GapLine.Reserve(Delta.Num());
		for (const FVector2f& D : Delta)
		{
			GapLine.Add(FVector2D(X(D.X), Gap.Y(D.Y)));
		}
		LapTraceLine(OutDrawElements, LapLayer, AllottedGeometry, GapLine, LapColour, 2.0f);
	}
	return LapLayer;
}
