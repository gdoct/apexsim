#include "UI/ApexMinimapWidget.h"

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
}

void UApexMinimapWidget::SetBlips(TArray<FApexMinimapBlip>&& InBlips)
{
	Blips = MoveTemp(InBlips);
}

FVector2D UApexMinimapWidget::ToLocal(const FVector2D& World, const FVector2D& LocalSize) const
{
	const FVector2D Extent = WorldMax - WorldMin;
	const float Span = FMath::Max(FMath::Max(Extent.X, Extent.Y), 1.0f);

	// One scale for both axes, or a long circuit would be stretched into its box
	// and stop being recognisable.
	const float Usable = FMath::Max(FMath::Min(LocalSize.X, LocalSize.Y) - MapPadding * 2.0f, 1.0f);
	const float Scale = Usable / Span;

	// The circuit is centred in whatever box it is given, so a non-square panel
	// does not push it against one edge.
	const FVector2D Centred = (World - (WorldMin + WorldMax) * 0.5f) * Scale;

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

	const int32 TrackLayer = BaseLayer + 1;
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
