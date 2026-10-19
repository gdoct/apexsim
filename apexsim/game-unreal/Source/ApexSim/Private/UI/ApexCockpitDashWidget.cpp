#include "UI/ApexCockpitDashWidget.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "UI/ApexUIStyle.h"

using namespace ApexUI;

namespace
{
	/** Lights, left to right: green through amber to red, then the blue shift lights. */
	FLinearColor LightColour(int32 Index)
	{
		if (Index < 5)  { return Palette::Live; }
		if (Index < 10) { return Palette::Accent; }
		if (Index < 13) { return Palette::Error; }
		return FLinearColor::FromSRGBColor(FColor(0x4A, 0x8C, 0xFF));
	}

	FString FormatLapTime(int32 Ms)
	{
		if (Ms <= 0)
		{
			return TEXT("--:--.-");
		}
		const int32 Minutes = Ms / 60000;
		const int32 Seconds = (Ms / 1000) % 60;
		const int32 Tenths = (Ms / 100) % 10;
		return FString::Printf(TEXT("%d:%02d.%d"), Minutes, Seconds, Tenths);
	}

	FString FormatGear(int32 Gear)
	{
		if (Gear < 0) { return TEXT("R"); }
		if (Gear == 0) { return TEXT("N"); }
		return FString::FromInt(Gear);
	}
}

UApexCockpitDashWidget::UApexCockpitDashWidget(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	SetVisibility(ESlateVisibility::HitTestInvisible);
}

void UApexCockpitDashWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	BuildTree();
}

void UApexCockpitDashWidget::BuildTree()
{
	RpmLights.Reset();

	UVerticalBox* Stack = WidgetTree->ConstructWidget<UVerticalBox>();

	// Light bar across the top: the one thing a driver reads without looking.
	{
		UHorizontalBox* Bar = WidgetTree->ConstructWidget<UHorizontalBox>();
		for (int32 Index = 0; Index < LightCount; ++Index)
		{
			UBorder* Light = MakePanel(*WidgetTree, nullptr, FMargin(), MakeBrush(Palette::Border, FLinearColor::Transparent, 0.0f, 3.0f));
			RpmLights.Add(Light);
			AddH(Bar, MakeSized(*WidgetTree, Light, 26.0f, 18.0f), FMargin(Index == 0 ? 0.0f : 6.0f, 0.0f, 0.0f, 0.0f));
		}
		AddV(Stack, Bar, FMargin(0.0f, 0.0f, 0.0f, 10.0f), HAlign_Center);
	}

	// Middle: gear on the left, speed on the right.
	{
		UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();

		GearText = MakeText(*WidgetTree, TEXT("N"), Font::Display(112.0f, 0), Palette::Accent);
		AddH(Row, MakeSized(*WidgetTree, GearText, 120.0f, -1.0f), FMargin(), VAlign_Center);

		UVerticalBox* Speed = WidgetTree->ConstructWidget<UVerticalBox>();
		SpeedText = MakeText(*WidgetTree, TEXT("0"), Font::Display(84.0f, 0), Palette::TextPrimary);
		AddV(Speed, SpeedText, FMargin(), HAlign_Right);
		SpeedUnitText = MakeLabel(*WidgetTree, TEXT("KM/H"), Palette::TextSecondary);
		AddV(Speed, SpeedUnitText, FMargin(0.0f, -4.0f, 0.0f, 0.0f), HAlign_Right);
		AddH(Row, Speed, FMargin(), VAlign_Center, 1.0f);

		AddV(Stack, Row, FMargin(), HAlign_Fill, 1.0f);
	}

	// Bottom: the lap and its running time.
	{
		UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();
		LapText = MakeText(*WidgetTree, TEXT("LAP -"), Font::Mono(22.0f, 40), Palette::TextSecondary);
		AddH(Row, LapText, FMargin(), VAlign_Center, 1.0f);
		LapTimeText = MakeText(*WidgetTree, TEXT("--:--.-"), Font::Mono(30.0f, 0), Palette::TextPrimary);
		if (UHorizontalBoxSlot* TimeSlot = AddH(Row, LapTimeText, FMargin(), VAlign_Center))
		{
			TimeSlot->SetHorizontalAlignment(HAlign_Right);
		}
		AddV(Stack, Row, FMargin(0.0f, 6.0f, 0.0f, 0.0f));
	}

	UBorder* Face = MakePanel(*WidgetTree, Stack, FMargin(22.0f, 16.0f), MakeBrush(Palette::Background));
	WidgetTree->RootWidget = MakeSized(*WidgetTree, Face, static_cast<float>(DrawWidth), static_cast<float>(DrawHeight));
}

void UApexCockpitDashWidget::SetValues(int32 Gear, float SpeedKph, bool bMetric, float RpmFraction, int32 Lap, int32 LapTimeMs, int32 CountdownMs)
{
	// Every setter here invalidates the widget component's render target, so
	// nothing is written unless the number the driver would read has moved.
	const int32 Lit = FMath::Clamp(FMath::RoundToInt(FMath::Clamp(RpmFraction, 0.0f, 1.0f) * LightCount), 0, LightCount);
	if (Lit != LitLights)
	{
		LitLights = Lit;
		for (int32 Index = 0; Index < RpmLights.Num(); ++Index)
		{
			if (RpmLights[Index])
			{
				RpmLights[Index]->SetBrush(MakeBrush(Index < Lit ? LightColour(Index) : Palette::Border, FLinearColor::Transparent, 0.0f, 3.0f));
			}
		}
	}

	if (Gear != LastGear && GearText)
	{
		LastGear = Gear;
		GearText->SetText(FText::FromString(FormatGear(Gear)));
	}

	const int32 Speed = FMath::RoundToInt(bMetric ? SpeedKph : SpeedKph * 0.621371f);
	if ((Speed != LastSpeed || bMetric != bLastMetric) && SpeedText)
	{
		LastSpeed = Speed;
		bLastMetric = bMetric;
		SpeedText->SetText(FText::FromString(FString::FromInt(FMath::Max(Speed, 0))));
		if (SpeedUnitText)
		{
			SpeedUnitText->SetText(FText::FromString(bMetric ? TEXT("KM/H") : TEXT("MPH")));
		}
	}

	// On the grid the display counts the lights down instead of a lap time,
	// which is what an actual wheel does while the field waits.
	const bool bCountdown = CountdownMs >= 0;
	const int32 Tenths = bCountdown ? -CountdownMs / 100 - 1 : LapTimeMs / 100;
	if (Tenths != LastTimeTenths && LapTimeText)
	{
		LastTimeTenths = Tenths;
		LapTimeText->SetText(FText::FromString(
			bCountdown ? FString::Printf(TEXT("%d.%d"), CountdownMs / 1000, (CountdownMs / 100) % 10) : FormatLapTime(LapTimeMs)));
	}

	if (Lap != LastLap && LapText)
	{
		LastLap = Lap;
		LapText->SetText(FText::FromString(Lap > 0 ? FString::Printf(TEXT("LAP %d"), Lap) : TEXT("LAP -")));
	}
}
