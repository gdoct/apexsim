#include "UI/ApexColourVision.h"

#include "UI/ApexUIStyle.h"

namespace
{
	EApexColourVision GColourVision = EApexColourVision::Standard;

	FLinearColor VisionSrgb(uint8 R, uint8 G, uint8 B)
	{
		return FLinearColor::FromSRGBColor(FColor(R, G, B));
	}

	constexpr int32 RoleCount = static_cast<int32>(EApexColourRole::Count);
	constexpr int32 VisionCount = 3;

	/**
	 * Every role in every palette, built on first use (the menu palette's
	 * statics must exist first). The alternatives were picked against a
	 * simulation of each deficiency (Machado 2009, full severity) so that the
	 * pairs a reader has to tell apart (good from warn from bad, a session best
	 * from a personal one, cold from in the window) stay far apart in CIELAB.
	 */
	const FLinearColor (&Table())[VisionCount][RoleCount]
	{
		static const FLinearColor Colours[VisionCount][RoleCount] = {
			// Standard: the game's own look.
			{
				ApexUI::Palette::Accent,
				ApexUI::Palette::Live,
				ApexUI::Palette::Error,
				ApexUI::Palette::Live,
				VisionSrgb(0xB0, 0x7C, 0xE8),
				VisionSrgb(0x5A, 0x9B, 0xE0),
				VisionSrgb(0x3B, 0xB5, 0x4A),
				VisionSrgb(0xFF, 0xCC, 0x00),
				VisionSrgb(0x1E, 0x6E, 0xFF),
				VisionSrgb(0x14, 0x82, 0x46),
				VisionSrgb(0xDC, 0x28, 0x28),
				VisionSrgb(0x4A, 0x8C, 0xFF),
			},
			// Red-green (deuteranopia, protanopia): blue for good, vermilion for
			// bad, a light yellow between, a pale lilac session best.
			{
				VisionSrgb(0xF0, 0xE4, 0x42),
				VisionSrgb(0x3D, 0x9B, 0xF0),
				VisionSrgb(0xD5, 0x5E, 0x00),
				VisionSrgb(0x3D, 0x9B, 0xF0),
				VisionSrgb(0xF0, 0xD8, 0xFF),
				VisionSrgb(0xE0, 0xE8, 0xF8),
				VisionSrgb(0x3D, 0x9B, 0xF0),
				VisionSrgb(0xFF, 0xCC, 0x00),
				VisionSrgb(0x1E, 0x6E, 0xFF),
				VisionSrgb(0x14, 0x82, 0x46),
				VisionSrgb(0xD5, 0x5E, 0x00),
				// White: the dash's first lights are already the good blue.
				VisionSrgb(0xF2, 0xF2, 0xF2),
			},
			// Blue-yellow (tritanopia): red and green stay; cold, which was a blue
			// that reads as the window's green, goes pale.
			{
				ApexUI::Palette::Accent,
				ApexUI::Palette::Live,
				ApexUI::Palette::Error,
				ApexUI::Palette::Live,
				VisionSrgb(0xB0, 0x7C, 0xE8),
				VisionSrgb(0xE0, 0xE8, 0xF8),
				ApexUI::Palette::Live,
				VisionSrgb(0xFF, 0xCC, 0x00),
				VisionSrgb(0x1E, 0x6E, 0xFF),
				VisionSrgb(0x14, 0x82, 0x46),
				VisionSrgb(0xDC, 0x28, 0x28),
				// White: blue reads as the first lights' green.
				VisionSrgb(0xF2, 0xF2, 0xF2),
			},
		};
		return Colours;
	}
}

FLinearColor ApexColourVision::Role(EApexColourRole Role, EApexColourVision Vision)
{
	const int32 V = FMath::Clamp(static_cast<int32>(Vision), 0, VisionCount - 1);
	const int32 R = FMath::Clamp(static_cast<int32>(Role), 0, RoleCount - 1);
	return Table()[V][R];
}

FLinearColor ApexColourVision::Role(EApexColourRole Role)
{
	return ApexColourVision::Role(Role, GColourVision);
}

EApexColourVision ApexColourVision::Current()
{
	return GColourVision;
}

void ApexColourVision::SetCurrent(EApexColourVision Vision)
{
	GColourVision = static_cast<int32>(Vision) < VisionCount ? Vision : EApexColourVision::Standard;
}

const TCHAR* ApexColourVision::Name(EApexColourVision Vision)
{
	switch (Vision)
	{
	case EApexColourVision::RedGreen: return TEXT("red_green");
	case EApexColourVision::BlueYellow: return TEXT("blue_yellow");
	default: return TEXT("standard");
	}
}

const TCHAR* ApexColourVision::Label(EApexColourVision Vision)
{
	switch (Vision)
	{
	case EApexColourVision::RedGreen: return TEXT("RED-GREEN");
	case EApexColourVision::BlueYellow: return TEXT("BLUE-YELLOW");
	default: return TEXT("STANDARD");
	}
}

bool ApexColourVision::Parse(const FString& Text, EApexColourVision& Out)
{
	const FString Key = Text.TrimStartAndEnd().ToLower().Replace(TEXT("-"), TEXT("_"));
	if (Key == TEXT("standard") || Key == TEXT("0") || Key == TEXT("normal"))
	{
		Out = EApexColourVision::Standard;
		return true;
	}
	if (Key == TEXT("red_green") || Key == TEXT("1") || Key == TEXT("deuteranopia") || Key == TEXT("protanopia"))
	{
		Out = EApexColourVision::RedGreen;
		return true;
	}
	if (Key == TEXT("blue_yellow") || Key == TEXT("2") || Key == TEXT("tritanopia"))
	{
		Out = EApexColourVision::BlueYellow;
		return true;
	}
	return false;
}
