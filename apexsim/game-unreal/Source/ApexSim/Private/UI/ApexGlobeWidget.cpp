#include "UI/ApexGlobeWidget.h"

#include "Audio/ApexUiAudioSubsystem.h"
#include "Engine/Texture2D.h"
#include "Fonts/FontMeasure.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformTime.h"
#include "Rendering/DrawElements.h"
#include "UI/ApexNavigation.h"
#include "UI/ApexUIStyle.h"

namespace
{
	/** The design's picture is 1300 x 1000 units with a 430-unit globe; sizes below are in those units. */
	constexpr double DesignRadius = 430.0;
	constexpr double DesignAspect = 1.3;

	FLinearColor Srgb(uint8 R, uint8 G, uint8 B, float Alpha = 1.0f)
	{
		FLinearColor Colour = FLinearColor::FromSRGBColor(FColor(R, G, B));
		Colour.A = Alpha;
		return Colour;
	}

	// The globe's own colours, from the design; the pins use the menu palette.
	const FLinearColor GraticuleColour = Srgb(0x9D, 0xB4, 0xE8, 0.14f);
	const FLinearColor LimbColour = Srgb(0x8F, 0xB0, 0xFF, 0.35f);
	const FLinearColor LandNear = Srgb(0xDF, 0xE3, 0xEA, 0.85f);
	const FLinearColor LandMid = Srgb(0xC9, 0xCE, 0xD6, 0.45f);
	const FLinearColor LandFar = Srgb(0xC9, 0xCE, 0xD6, 0.2f);
	const FLinearColor PinStem = Srgb(0xD0, 0xD3, 0xD9);
	const FLinearColor PinDark = Srgb(0x0B, 0x0B, 0x0D);
	const FLinearColor LabelFill = Srgb(0x0A, 0x0A, 0x0C, 0.86f);

	/** Pixels a press moves before it is a drag rather than a click. */
	constexpr double DragThreshold = 4.0;

	FSlateFontInfo LabelFont()
	{
		return ApexUI::Font::Body(12.0f, /*bBold*/ true);
	}

	FColor ToColor(const FLinearColor& Colour)
	{
		return Colour.ToFColor(/*bSRGB*/ true);
	}

	/** A circle as a closed polyline. */
	void CirclePoints(const FVector2D& Centre, double Radius, int32 Segments, TArray<FVector2D>& Out)
	{
		Out.Reset(Segments + 1);
		for (int32 i = 0; i <= Segments; ++i)
		{
			const double A = 2.0 * PI * i / Segments;
			Out.Add(Centre + Radius * FVector2D(FMath::Cos(A), FMath::Sin(A)));
		}
	}
}

UApexGlobeWidget::UApexGlobeWidget(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	SetIsFocusable(true);
}

void UApexGlobeWidget::NativeConstruct()
{
	Super::NativeConstruct();
	MakeTextures();
	CircleBrush = ApexUI::MakeBrush(FLinearColor::White, FLinearColor::Transparent, 0.0f, 0.5f);
	CircleBrush.OutlineSettings.RoundingType = ESlateBrushRoundingType::HalfHeightRadius;
	BoxBrush = ApexUI::MakeBrush(FLinearColor::White);
}

void UApexGlobeWidget::MakeTextures()
{
	if (OceanTexture && AtmosphereTexture)
	{
		return;
	}

	// The ocean: the design's radial gradient, lit from above left (centre at
	// 40% / 36% of its box, 75% of the box out), cut to the disc with a soft
	// texel; the limb's line covers the edge.
	auto Make = [](int32 Size, TFunctionRef<FColor(double U, double V)> Pixel) -> UTexture2D*
	{
		UTexture2D* Texture = UTexture2D::CreateTransient(Size, Size, PF_B8G8R8A8);
		if (!Texture)
		{
			return nullptr;
		}
		Texture->SRGB = true;
		Texture->Filter = TF_Bilinear;
		Texture->AddressX = TA_Clamp;
		Texture->AddressY = TA_Clamp;
		FTexture2DMipMap& Mip = Texture->GetPlatformData()->Mips[0];
		FColor* Pixels = static_cast<FColor*>(Mip.BulkData.Lock(LOCK_READ_WRITE));
		for (int32 Y = 0; Y < Size; ++Y)
		{
			for (int32 X = 0; X < Size; ++X)
			{
				Pixels[Y * Size + X] = Pixel((X + 0.5) / Size, (Y + 0.5) / Size);
			}
		}
		Mip.BulkData.Unlock();
		Texture->UpdateResource();
		return Texture;
	};

	constexpr int32 OceanSize = 512;
	const FLinearColor Stop0 = Srgb(0x1B, 0x20, 0x30);
	const FLinearColor Stop1 = Srgb(0x11, 0x14, 0x1C);
	const FLinearColor Stop2 = Srgb(0x0A, 0x0B, 0x0F);
	OceanTexture = Make(OceanSize, [&](double U, double V)
	{
		const double T = FMath::Clamp(FVector2D(U - 0.4, V - 0.36).Size() / 0.75, 0.0, 1.0);
		FLinearColor Colour = T < 0.6 ? FMath::Lerp(Stop0, Stop1, T / 0.6) : FMath::Lerp(Stop1, Stop2, (T - 0.6) / 0.4);
		const double Edge = (0.5 - FVector2D(U - 0.5, V - 0.5).Size()) * OceanSize;
		Colour.A = FMath::Clamp(Edge + 0.5, 0.0, 1.0);
		return ToColor(Colour);
	});

	// The atmosphere: a rim 1.07 radii out, 0.22 of its blue inside 86% of
	// its own radius and fading to nothing at its edge.
	const FLinearColor Air = Srgb(0x7F, 0xA6, 0xFF);
	AtmosphereTexture = Make(256, [&](double U, double V)
	{
		const double R = FVector2D(U - 0.5, V - 0.5).Size() * 2.0;
		FLinearColor Colour = Air;
		Colour.A = R <= 0.86 ? 0.22f : static_cast<float>(FMath::Clamp(0.22 * (1.0 - (R - 0.86) / 0.14), 0.0, 0.22));
		return ToColor(Colour);
	});

	OceanBrush = FSlateBrush();
	OceanBrush.SetResourceObject(OceanTexture);
	OceanBrush.ImageSize = FVector2D(OceanSize, OceanSize);
	AtmosphereBrush = FSlateBrush();
	AtmosphereBrush.SetResourceObject(AtmosphereTexture);
	AtmosphereBrush.ImageSize = FVector2D(256.0, 256.0);
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

void UApexGlobeWidget::SetPins(const TArray<FApexGlobePin>& InPins)
{
	const FString KeysId = GetFocusedPinId();
	Pins = InPins;
	States.SetNum(Pins.Num());
	LaidOutSize = FVector2D::ZeroVector;
	HoveredPin = INDEX_NONE;
	PressedPin = INDEX_NONE;
	KeysPin = Pins.IndexOfByPredicate([&KeysId](const FApexGlobePin& Pin) { return Pin.Id == KeysId; });
	if (!SelectedId.IsEmpty() && !Pins.ContainsByPredicate([this](const FApexGlobePin& Pin) { return Pin.Id == SelectedId; }))
	{
		ClearSelection();
	}
}

void UApexGlobeWidget::Select(const FString& TrackId, bool bFly)
{
	const int32 Index = Pins.IndexOfByPredicate([&TrackId](const FApexGlobePin& Pin) { return Pin.Id == TrackId; });
	if (Index == INDEX_NONE)
	{
		ClearSelection();
		return;
	}
	SelectedId = TrackId;
	View.bSelected = true;
	if (bFly)
	{
		ApexGlobe::FlyTo(View, Pins[Index].LonDeg, Pins[Index].LatDeg);
	}
}

void UApexGlobeWidget::ClearSelection()
{
	SelectedId.Reset();
	View.bSelected = false;
	View.TargetZoom = ApexGlobe::HomeZoom;
}

void UApexGlobeWidget::ResetView()
{
	SelectedId.Reset();
	ApexGlobe::Reset(View);
}

void UApexGlobeWidget::StepZoom(int32 Direction)
{
	ApexGlobe::ZoomBy(View, Direction > 0 ? ApexGlobe::ZoomStep : 1.0 / ApexGlobe::ZoomStep);
}

FString UApexGlobeWidget::GetFocusedPinId() const
{
	return Pins.IsValidIndex(KeysPin) ? Pins[KeysPin].Id : FString();
}

bool UApexGlobeWidget::FocusPin(const FString& TrackId)
{
	const int32 Index = Pins.IndexOfByPredicate([&TrackId](const FApexGlobePin& Pin) { return Pin.Id == TrackId; });
	if (Index != INDEX_NONE)
	{
		KeysPin = Index;
	}
	bKeysActive = true;
	return ApexNav::Focus(this);
}

void UApexGlobeWidget::ChoosePin(int32 Index)
{
	if (!Pins.IsValidIndex(Index))
	{
		return;
	}
	KeysPin = Index;
	Select(Pins[Index].Id);
	ApexUiAudio::Play(this, EApexUiSound::Accept);
	OnSelectionChanged.Broadcast(Pins[Index].Id);
}

int32 UApexGlobeWidget::KeysPinOrNearest() const
{
	if (States.IsValidIndex(KeysPin) && States[KeysPin].bShown)
	{
		return KeysPin;
	}
	const int32 Selected = Pins.IndexOfByPredicate([this](const FApexGlobePin& Pin) { return Pin.Id == SelectedId; });
	if (States.IsValidIndex(Selected) && States[Selected].bShown)
	{
		return Selected;
	}
	int32 Best = INDEX_NONE;
	double BestDistance = TNumericLimits<double>::Max();
	const FVector2D Middle = LaidOutSize * 0.5;
	for (int32 i = 0; i < States.Num(); ++i)
	{
		const double Distance = FVector2D::DistSquared(States[i].Head, Middle);
		if (States[i].bShown && Distance < BestDistance)
		{
			BestDistance = Distance;
			Best = i;
		}
	}
	return Best;
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

double UApexGlobeWidget::BaseRadius(const FVector2D& Size)
{
	return DesignRadius / 1000.0 * FMath::Min(Size.X / DesignAspect, Size.Y);
}

void UApexGlobeWidget::Layout(const FVector2D& Size) const
{
	States.SetNum(Pins.Num());
	if (!Size.Equals(LaidOutSize, 0.5))
	{
		// The labels' padding scales with the globe.
		for (FPinState& State : States)
		{
			State.LabelSize = FVector2D::ZeroVector;
		}
	}
	LaidOutSize = Size;
	const double R = BaseRadius(Size);
	const double K = R / DesignRadius;
	const FVector2D Centre = Size * 0.5;
	const TSharedRef<FSlateFontMeasure> Measure = FSlateApplication::Get().GetRenderer()->GetFontMeasureService();
	const FSlateFontInfo Font = LabelFont();

	TArray<ApexGlobe::FLabel> Labels;
	TArray<int32> LabelPin;
	for (int32 i = 0; i < Pins.Num(); ++i)
	{
		FPinState& State = States[i];
		const FVector3d Base = ApexGlobe::Project(View, Pins[i].LonDeg, Pins[i].LatDeg, 1.0);
		const FVector3d Head = ApexGlobe::Project(View, Pins[i].LonDeg, Pins[i].LatDeg, 1.0 + ApexGlobe::PinHeight);
		State.Base = Centre + FVector2D(Base.X, -Base.Y) * R;
		State.Head = Centre + FVector2D(Head.X, -Head.Y) * R;
		State.Depth = Base.Z;
		State.bShown = Base.Z >= ApexGlobe::PinMinDepth;
		State.bLabel = false;
		if (!State.bShown)
		{
			continue;
		}
		if (State.LabelSize.IsZero())
		{
			State.LabelSize = Measure->Measure(Pins[i].Name, Font) + FVector2D(18.0, 6.0) * FMath::Max(K, 0.6);
		}
		const double Gap = 12.0 * K;
		State.bLabelLeft = State.Head.X + Gap + State.LabelSize.X > Size.X - 8.0;
		const double X0 = State.bLabelLeft ? State.Head.X - Gap - State.LabelSize.X : State.Head.X + Gap;
		ApexGlobe::FLabel Label;
		Label.Rect = FBox2D(FVector2D(X0, State.Head.Y - State.LabelSize.Y * 0.5), FVector2D(X0 + State.LabelSize.X, State.Head.Y + State.LabelSize.Y * 0.5));
		Label.Depth = State.Depth;
		Label.bAlways = Pins[i].Id == SelectedId || i == HoveredPin || (bHasKeyFocus && bKeysActive && i == KeysPin);
		Labels.Add(Label);
		LabelPin.Add(i);
	}
	const TArray<bool> Shown = ApexGlobe::PlaceLabels(Labels);
	for (int32 j = 0; j < Shown.Num(); ++j)
	{
		States[LabelPin[j]].bLabel = Shown[j];
	}
}

int32 UApexGlobeWidget::HitPin(const FVector2D& Local) const
{
	if (LaidOutSize.IsZero())
	{
		return INDEX_NONE;
	}
	const double K = BaseRadius(LaidOutSize) / DesignRadius;
	const double Reach = FMath::Max(22.0 * K, 14.0);
	int32 Best = INDEX_NONE;
	double BestDepth = -2.0;
	for (int32 i = 0; i < States.Num(); ++i)
	{
		const FPinState& State = States[i];
		if (!State.bShown || State.Depth <= BestDepth)
		{
			continue;
		}
		bool bHit = FVector2D::Distance(State.Head, Local) <= Reach;
		if (!bHit && State.bLabel)
		{
			const double Gap = 12.0 * K;
			const double X0 = State.bLabelLeft ? State.Head.X - Gap - State.LabelSize.X : State.Head.X + Gap;
			bHit = Local.X >= X0 && Local.X <= X0 + State.LabelSize.X
				&& FMath::Abs(Local.Y - State.Head.Y) <= State.LabelSize.Y * 0.5;
		}
		if (bHit)
		{
			Best = i;
			BestDepth = State.Depth;
		}
	}
	return Best;
}

void UApexGlobeWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);
	const double Dt = FMath::Clamp(static_cast<double>(InDeltaTime), 0.0, 0.05);
	Clock += Dt;
	View.ZoomInput = TriggerIn - TriggerOut;
	// Keys and pad on the globe hold it still as the pointer does.
	View.bHovered = IsHovered() || (bHasKeyFocus && bKeysActive) || !bIdleSpin;
	ApexGlobe::Step(View, Dt);
	Layout(MyGeometry.GetLocalSize());
}

// ---------------------------------------------------------------------------
// Paint
// ---------------------------------------------------------------------------

int32 UApexGlobeWidget::NativePaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
	FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	const int32 Base = Super::NativePaint(Args, AllottedGeometry, MyCullingRect, OutDrawElements, LayerId, InWidgetStyle, bParentEnabled);
	const FVector2D Size = AllottedGeometry.GetLocalSize();
	if (Size.X < 20.0 || Size.Y < 20.0)
	{
		return Base;
	}
	if (!Size.Equals(LaidOutSize, 0.5))
	{
		Layout(Size);
	}

	const double R0 = BaseRadius(Size);
	const double R = R0 * View.Zoom;
	const double K = R0 / DesignRadius;
	const FVector2D Centre = Size * 0.5;
	int32 Layer = Base + 1;

	auto Box = [&](const FVector2D& TopLeft, const FVector2D& BoxSize, const FSlateBrush* Brush, const FLinearColor& Tint, int32 OnLayer)
	{
		FSlateDrawElement::MakeBox(OutDrawElements, OnLayer,
			AllottedGeometry.ToPaintGeometry(BoxSize, FSlateLayoutTransform(TopLeft)), Brush, ESlateDrawEffect::None, Tint);
	};
	auto Disc = [&](const FVector2D& At, double Radius, const FLinearColor& Tint, int32 OnLayer)
	{
		Box(At - FVector2D(Radius, Radius), FVector2D(Radius * 2.0, Radius * 2.0), &CircleBrush, Tint, OnLayer);
	};
	auto Lines = [&](const TArray<FVector2D>& Points, const FLinearColor& Tint, double Thickness, int32 OnLayer)
	{
		if (Points.Num() > 1)
		{
			FSlateDrawElement::MakeLines(OutDrawElements, OnLayer, AllottedGeometry.ToPaintGeometry(), Points,
				ESlateDrawEffect::None, Tint, true, static_cast<float>(Thickness));
		}
	};

	// Air, then sea.
	if (AtmosphereTexture)
	{
		const double Air = R * 1.07;
		Box(Centre - FVector2D(Air, Air), FVector2D(Air * 2.0, Air * 2.0), &AtmosphereBrush, FLinearColor::White, Layer);
	}
	++Layer;
	if (OceanTexture)
	{
		Box(Centre - FVector2D(R, R), FVector2D(R * 2.0, R * 2.0), &OceanBrush, FLinearColor::White, Layer);
	}
	++Layer;

	// The graticule: meridians every 30°, parallels at 0, ±30, ±60, drawn
	// only on the near side.
	{
		TArray<FVector2D> Run;
		auto Flush = [&]()
		{
			Lines(Run, GraticuleColour, 1.0, Layer);
			Run.Reset();
		};
		auto Visit = [&](double Lon, double Lat)
		{
			const FVector3d P = ApexGlobe::Project(View, Lon, Lat);
			if (P.Z > 0.0)
			{
				Run.Add(Centre + FVector2D(P.X, -P.Y) * R0);
			}
			else
			{
				Flush();
			}
		};
		for (int32 Lon = -180; Lon < 180; Lon += 30)
		{
			for (int32 Lat = -90; Lat <= 90; Lat += 3)
			{
				Visit(Lon, Lat);
			}
			Flush();
		}
		for (int32 Lat = -60; Lat <= 60; Lat += 30)
		{
			for (int32 Lon = -180; Lon <= 180; Lon += 3)
			{
				Visit(Lon, Lat);
			}
			Flush();
		}
	}
	++Layer;

	// Land, as dots, brighter toward the middle.
	{
		const double P0 = FMath::DegreesToRadians(View.LatDeg);
		const double L0 = FMath::DegreesToRadians(View.LonDeg);
		const double SP0 = FMath::Sin(P0);
		const double CP0 = FMath::Cos(P0);
		const double Dot = FMath::Min(16.0, 6.0 * FMath::Pow(View.Zoom, 0.7)) * K;
		const FSlateRect Cull = MyCullingRect;
		for (const ApexGlobe::FLandDot& Land : ApexGlobe::LandDots())
		{
			const double DL = Land.LonRad - L0;
			const double CD = FMath::Cos(DL);
			const double Z = SP0 * Land.SinLat + CP0 * Land.CosLat * CD;
			if (Z <= 0.02)
			{
				continue;
			}
			const FVector2D At = Centre + R * FVector2D(Land.CosLat * FMath::Sin(DL), -(CP0 * Land.SinLat - SP0 * Land.CosLat * CD));
			if (At.X < -Dot || At.Y < -Dot || At.X > Size.X + Dot || At.Y > Size.Y + Dot)
			{
				continue;
			}
			Disc(At, Dot * 0.5, Z > 0.55 ? LandNear : (Z > 0.22 ? LandMid : LandFar), Layer);
		}
	}
	++Layer;

	{
		TArray<FVector2D> Limb;
		CirclePoints(Centre, R, 160, Limb);
		Lines(Limb, LimbColour, 1.5, Layer);
	}
	++Layer;

	// Pins, back to front.
	const double Scale = FMath::Clamp(FMath::Pow(View.Zoom, 0.4), 0.9, 1.5);
	TArray<int32> Order;
	for (int32 i = 0; i < States.Num(); ++i)
	{
		if (States[i].bShown)
		{
			Order.Add(i);
		}
	}
	Order.Sort([this](int32 A, int32 B) { return States[A].Depth < States[B].Depth; });
	const bool bShowKeys = bHasKeyFocus && bKeysActive;
	const int32 PinLayer = Layer;
	for (const int32 i : Order)
	{
		const FPinState& State = States[i];
		const bool bSelected = Pins[i].Id == SelectedId;
		const float Fade = static_cast<float>(ApexGlobe::PinFade(State.Depth));
		auto Faded = [Fade](FLinearColor Colour) { Colour.A *= Fade; return Colour; };
		const FLinearColor Colour = bSelected ? ApexUI::Palette::Live : ApexUI::Palette::Accent;

		Lines({ State.Base, State.Head }, Faded(PinStem), 2.0 * K, PinLayer);
		Disc(State.Base, 3.1 * K, Faded(PinStem), PinLayer);
		Disc(State.Base, 2.1 * K, Faded(PinDark), PinLayer + 1);
		if (bSelected)
		{
			// The design's pulse: a ring that grows from 0.55 to 2.3 times
			// and fades, every 1.7 s.
			const double T = FMath::Fmod(Clock, 1.7) / 1.7;
			const double Grow = 0.55 + 1.75 * (1.0 - FMath::Square(1.0 - T));
			TArray<FVector2D> Ring;
			CirclePoints(State.Head, 11.0 * Scale * Grow * K, 48, Ring);
			FLinearColor Pulse = Colour;
			Pulse.A = static_cast<float>(0.95 * (1.0 - T)) * Fade;
			Lines(Ring, Pulse, 2.5 * Grow * K, PinLayer + 1);
		}
		const double Head = (bSelected ? 9.5 : 7.5) * Scale * K;
		Disc(State.Head, Head + 1.25 * K, Faded(PinDark), PinLayer + 2);
		Disc(State.Head, Head - 1.25 * K, Faded(Colour), PinLayer + 3);
		if (bShowKeys && i == KeysPin)
		{
			TArray<FVector2D> Ring;
			CirclePoints(State.Head, FMath::Max(18.0 * K, Head + 5.0), 40, Ring);
			Lines(Ring, ApexUI::Palette::Focus, 2.0, PinLayer + 3);
		}
	}
	Layer = PinLayer + 4;

	// Labels over every pin.
	const FSlateFontInfo Font = LabelFont();
	for (const int32 i : Order)
	{
		const FPinState& State = States[i];
		if (!State.bLabel)
		{
			continue;
		}
		const bool bSelected = Pins[i].Id == SelectedId;
		const float Fade = static_cast<float>(ApexGlobe::PinFade(State.Depth));
		const double Gap = 12.0 * K;
		const FVector2D TopLeft(State.bLabelLeft ? State.Head.X - Gap - State.LabelSize.X : State.Head.X + Gap,
			State.Head.Y - State.LabelSize.Y * 0.5);
		FLinearColor Edge = bSelected ? ApexUI::Palette::Live : ApexUI::Palette::Border;
		FLinearColor Fill = LabelFill;
		FLinearColor Text = bSelected ? ApexUI::Palette::Live : ApexUI::Palette::TextPrimary;
		Edge.A *= Fade;
		Fill.A *= Fade;
		Text.A *= Fade;
		Box(TopLeft - FVector2D(1.0, 1.0), State.LabelSize + FVector2D(2.0, 2.0), &BoxBrush, Edge, Layer);
		Box(TopLeft, State.LabelSize, &BoxBrush, Fill, Layer + 1);
		const FVector2D Pad = FVector2D(9.0, 3.0) * FMath::Max(K, 0.6);
		FSlateDrawElement::MakeText(OutDrawElements, Layer + 2,
			AllottedGeometry.ToPaintGeometry(State.LabelSize, FSlateLayoutTransform(TopLeft + Pad)),
			Pins[i].Name, Font, ESlateDrawEffect::None, Text);
		Layer += 3;
	}
	return Layer;
}

// ---------------------------------------------------------------------------
// Pointer
// ---------------------------------------------------------------------------

FReply UApexGlobeWidget::NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	if (InMouseEvent.GetEffectingButton() != EKeys::LeftMouseButton)
	{
		return Super::NativeOnMouseButtonDown(InGeometry, InMouseEvent);
	}
	const FVector2D Local = InGeometry.AbsoluteToLocal(InMouseEvent.GetScreenSpacePosition());
	bPressed = true;
	bDragMoved = false;
	bKeysActive = false;
	PressAt = Local;
	LastPointer = Local;
	LastMoveSeconds = FPlatformTime::Seconds();
	PressedPin = HitPin(Local);
	return FReply::Handled().CaptureMouse(TakeWidget());
}

FReply UApexGlobeWidget::NativeOnMouseMove(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	const FVector2D Local = InGeometry.AbsoluteToLocal(InMouseEvent.GetScreenSpacePosition());
	if (!bPressed)
	{
		HoveredPin = HitPin(Local);
		return Super::NativeOnMouseMove(InGeometry, InMouseEvent);
	}
	if (!bDragMoved && FVector2D::Distance(Local, PressAt) > DragThreshold)
	{
		bDragMoved = true;
		HoveredPin = INDEX_NONE;
		ApexGlobe::BeginDrag(View);
	}
	if (bDragMoved)
	{
		const double Now = FPlatformTime::Seconds();
		ApexGlobe::Drag(View, Local - LastPointer, BaseRadius(InGeometry.GetLocalSize()) * View.Zoom, Now - LastMoveSeconds);
		LastMoveSeconds = Now;
	}
	LastPointer = Local;
	return FReply::Handled();
}

FReply UApexGlobeWidget::NativeOnMouseButtonUp(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	if (!bPressed || InMouseEvent.GetEffectingButton() != EKeys::LeftMouseButton)
	{
		return Super::NativeOnMouseButtonUp(InGeometry, InMouseEvent);
	}
	bPressed = false;
	if (bDragMoved)
	{
		ApexGlobe::EndDrag(View, FPlatformTime::Seconds() - LastMoveSeconds);
	}
	else if (PressedPin != INDEX_NONE && HitPin(InGeometry.AbsoluteToLocal(InMouseEvent.GetScreenSpacePosition())) == PressedPin)
	{
		ChoosePin(PressedPin);
	}
	bDragMoved = false;
	PressedPin = INDEX_NONE;
	return FReply::Handled().ReleaseMouseCapture();
}

FReply UApexGlobeWidget::NativeOnMouseWheel(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	ApexGlobe::ZoomBy(View, FMath::Exp(0.15 * InMouseEvent.GetWheelDelta()));
	return FReply::Handled();
}

void UApexGlobeWidget::NativeOnMouseEnter(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	Super::NativeOnMouseEnter(InGeometry, InMouseEvent);
	bKeysActive = false;
}

void UApexGlobeWidget::NativeOnMouseLeave(const FPointerEvent& InMouseEvent)
{
	Super::NativeOnMouseLeave(InMouseEvent);
	HoveredPin = INDEX_NONE;
}

FCursorReply UApexGlobeWidget::NativeOnCursorQuery(const FGeometry& InGeometry, const FPointerEvent& InCursorEvent)
{
	if (bDragMoved)
	{
		return FCursorReply::Cursor(EMouseCursor::GrabHandClosed);
	}
	return FCursorReply::Cursor(HoveredPin != INDEX_NONE ? EMouseCursor::Hand : EMouseCursor::GrabHand);
}

// ---------------------------------------------------------------------------
// Keys and pad
// ---------------------------------------------------------------------------

FReply UApexGlobeWidget::NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent)
{
	const FKey Key = InKeyEvent.GetKey();
	bKeysActive = true;

	// The stick turns the globe (NativeOnAnalogValueChanged); its button
	// presses are not moves between pins.
	if (Key == EKeys::Gamepad_LeftStick_Up || Key == EKeys::Gamepad_LeftStick_Down
		|| Key == EKeys::Gamepad_LeftStick_Left || Key == EKeys::Gamepad_LeftStick_Right
		|| Key == EKeys::Gamepad_LeftTrigger || Key == EKeys::Gamepad_RightTrigger)
	{
		return FReply::Handled();
	}
	if (Key == EKeys::Gamepad_FaceButton_Top || Key == EKeys::Home)
	{
		ResetView();
		ApexUiAudio::Play(this, EApexUiSound::Back);
		OnSelectionChanged.Broadcast(FString());
		return FReply::Handled();
	}
	if (Key == EKeys::Add || Key == EKeys::Equals || Key == EKeys::PageUp)
	{
		StepZoom(1);
		return FReply::Handled();
	}
	if (Key == EKeys::Subtract || Key == EKeys::Hyphen || Key == EKeys::PageDown)
	{
		StepZoom(-1);
		return FReply::Handled();
	}
	if (!InKeyEvent.IsAltDown() && ApexNav::IsAccept(InKeyEvent))
	{
		const int32 Pin = KeysPinOrNearest();
		if (Pin != INDEX_NONE)
		{
			ChoosePin(Pin);
			return FReply::Handled();
		}
	}

	const EUINavigation Direction = ApexNav::DirectionFromKey(InKeyEvent);
	if (Direction != EUINavigation::Invalid)
	{
		const ENavigationGenesis Genesis = Key.IsGamepadKey() ? ENavigationGenesis::Controller : ENavigationGenesis::Keyboard;
		FVector2D Way = FVector2D::ZeroVector;
		switch (Direction)
		{
		case EUINavigation::Left:  Way = FVector2D(-1.0, 0.0); break;
		case EUINavigation::Right: Way = FVector2D(1.0, 0.0); break;
		case EUINavigation::Up:    Way = FVector2D(0.0, -1.0); break;
		case EUINavigation::Down:  Way = FVector2D(0.0, 1.0); break;
		default: break;
		}
		if (!Way.IsZero())
		{
			const int32 From = KeysPinOrNearest();
			if (From != KeysPin && From != INDEX_NONE)
			{
				// The first press lands on a pin before it moves between them.
				KeysPin = From;
				ApexUiAudio::Play(this, EApexUiSound::Move);
				return FReply::Handled();
			}
			TArray<FVector2D> Heads;
			TArray<bool> Usable;
			for (const FPinState& State : States)
			{
				Heads.Add(State.Head);
				Usable.Add(State.bShown);
			}
			const FVector2D Start = States.IsValidIndex(From) ? States[From].Head : LaidOutSize * 0.5;
			const int32 Next = ApexGlobe::NearestToward(Heads, Usable, From, Start, Way);
			if (Next != INDEX_NONE)
			{
				KeysPin = Next;
				ApexUiAudio::Play(this, EApexUiSound::Move);
				return FReply::Handled();
			}
		}
		// Off the last pin that way: the surface around the globe decides.
		return ApexNav::RouteFromLeaf(this, Direction, Genesis);
	}
	return Super::NativeOnKeyDown(InGeometry, InKeyEvent);
}

FReply UApexGlobeWidget::NativeOnAnalogValueChanged(const FGeometry& InGeometry, const FAnalogInputEvent& InAnalogEvent)
{
	const FKey Key = InAnalogEvent.GetKey();
	const double Value = InAnalogEvent.GetAnalogValue();
	if (Key == EKeys::Gamepad_LeftX)
	{
		View.Stick.X = Value;
	}
	else if (Key == EKeys::Gamepad_LeftY)
	{
		View.Stick.Y = Value;
	}
	else if (Key == EKeys::Gamepad_RightTriggerAxis)
	{
		TriggerIn = Value;
	}
	else if (Key == EKeys::Gamepad_LeftTriggerAxis)
	{
		TriggerOut = Value;
	}
	else
	{
		return Super::NativeOnAnalogValueChanged(InGeometry, InAnalogEvent);
	}
	if (FMath::Abs(Value) > 0.25)
	{
		bKeysActive = true;
	}
	return FReply::Handled();
}

FReply UApexGlobeWidget::NativeOnFocusReceived(const FGeometry& InGeometry, const FFocusEvent& InFocusEvent)
{
	bHasKeyFocus = true;
	if (InFocusEvent.GetCause() != EFocusCause::Mouse)
	{
		bKeysActive = true;
		KeysPin = KeysPinOrNearest();
	}
	return Super::NativeOnFocusReceived(InGeometry, InFocusEvent);
}

void UApexGlobeWidget::NativeOnFocusLost(const FFocusEvent& InFocusEvent)
{
	Super::NativeOnFocusLost(InFocusEvent);
	bHasKeyFocus = false;
	View.Stick = FVector2D::ZeroVector;
	TriggerIn = TriggerOut = 0.0;
}
