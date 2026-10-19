#include "Hud/ApexHudValue.h"

#include "UI/ApexUIStyle.h"

namespace
{
	struct FNamedColour
	{
		const TCHAR* Name;
		FLinearColor Colour;
	};

	/** The palette by name. Built on first use: the palette's own statics must exist first. */
	const TArray<FNamedColour>& NamedColours()
	{
		using namespace ApexUI;
		static const TArray<FNamedColour> Table = {
			{TEXT("background"), Palette::Background},
			{TEXT("surface"), Palette::Surface},
			{TEXT("surface_hover"), Palette::SurfaceHover},
			{TEXT("border"), Palette::Border},
			{TEXT("accent"), Palette::Accent},
			{TEXT("on_accent"), Palette::OnAccent},
			{TEXT("text"), Palette::TextPrimary},
			{TEXT("text_primary"), Palette::TextPrimary},
			{TEXT("text_secondary"), Palette::TextSecondary},
			{TEXT("text_muted"), Palette::TextMuted},
			{TEXT("text_disabled"), Palette::TextDisabled},
			{TEXT("live"), Palette::Live},
			{TEXT("error"), Palette::Error},
			{TEXT("focus"), Palette::Focus},
			// Timing colours: purple for a time nobody in the session has
			// beaten, green for a driver's own best.
			{TEXT("session_best"), FLinearColor::FromSRGBColor(FColor(0xB0, 0x7C, 0xE8))},
			{TEXT("personal_best"), Palette::Live},
			{TEXT("tyre_cold"), FLinearColor::FromSRGBColor(FColor(0x5A, 0x9B, 0xE0))},
			{TEXT("white"), FLinearColor::White},
			{TEXT("black"), FLinearColor::Black},
			{TEXT("transparent"), FLinearColor::Transparent},
		};
		return Table;
	}

	int32 HexDigit(TCHAR C)
	{
		if (C >= '0' && C <= '9')
		{
			return C - '0';
		}
		C = FChar::ToLower(C);
		return C >= 'a' && C <= 'f' ? 10 + (C - 'a') : -1;
	}
}

bool FApexHudValue::AsBool() const
{
	switch (Type)
	{
	case EApexHudValueType::Bool:
	case EApexHudValueType::Number:
		return Number != 0.0;
	case EApexHudValueType::String:
		return !String.IsEmpty();
	case EApexHudValueType::Colour:
		return true;
	default:
		return false;
	}
}

double FApexHudValue::AsNumber() const
{
	switch (Type)
	{
	case EApexHudValueType::Bool:
	case EApexHudValueType::Number:
		return Number;
	case EApexHudValueType::String:
		return String.IsNumeric() ? FCString::Atod(*String) : 0.0;
	default:
		return 0.0;
	}
}

FString FApexHudValue::AsString() const
{
	switch (Type)
	{
	case EApexHudValueType::Bool:
		return Number != 0.0 ? TEXT("true") : TEXT("false");
	case EApexHudValueType::Number:
	{
		if (FMath::IsFinite(Number) && FMath::Abs(Number) < 1e15 && Number == FMath::RoundToDouble(Number))
		{
			return FString::Printf(TEXT("%lld"), static_cast<long long>(Number));
		}
		FString Text = FString::Printf(TEXT("%.3f"), Number);
		while (Text.EndsWith(TEXT("0")))
		{
			Text.LeftChopInline(1);
		}
		return Text;
	}
	case EApexHudValueType::String:
		return String;
	case EApexHudValueType::Colour:
		return TEXT("#") + Colour.ToFColorSRGB().ToHex();
	default:
		return FString();
	}
}

bool FApexHudValue::AsColour(FLinearColor& Out) const
{
	if (Type == EApexHudValueType::Colour)
	{
		Out = Colour;
		return true;
	}
	return Type == EApexHudValueType::String && ApexHudColour::Parse(String, Out);
}

bool FApexHudValue::Equals(const FApexHudValue& Other) const
{
	const bool bNumeric = Type == EApexHudValueType::Number || Type == EApexHudValueType::Bool;
	const bool bOtherNumeric = Other.Type == EApexHudValueType::Number || Other.Type == EApexHudValueType::Bool;
	if (bNumeric && bOtherNumeric)
	{
		return Number == Other.Number;
	}
	if (Type != Other.Type)
	{
		return false;
	}
	switch (Type)
	{
	case EApexHudValueType::String:
		return String.Equals(Other.String, ESearchCase::CaseSensitive);
	case EApexHudValueType::Colour:
		return Colour == Other.Colour;
	default:
		return true;
	}
}

bool ApexHudColour::Parse(const FString& Text, FLinearColor& Out)
{
	const FString Trimmed = Text.TrimStartAndEnd();
	if (Trimmed.StartsWith(TEXT("#")))
	{
		const FString Hex = Trimmed.Mid(1);
		if (Hex.Len() != 6 && Hex.Len() != 8)
		{
			return false;
		}
		uint8 Bytes[4] = {0, 0, 0, 255};
		for (int32 Index = 0; Index < Hex.Len() / 2; ++Index)
		{
			const int32 High = HexDigit(Hex[Index * 2]);
			const int32 Low = HexDigit(Hex[Index * 2 + 1]);
			if (High < 0 || Low < 0)
			{
				return false;
			}
			Bytes[Index] = static_cast<uint8>(High * 16 + Low);
		}
		Out = FLinearColor::FromSRGBColor(FColor(Bytes[0], Bytes[1], Bytes[2], Bytes[3]));
		return true;
	}
	for (const FNamedColour& Named : NamedColours())
	{
		if (Trimmed.Equals(Named.Name, ESearchCase::IgnoreCase))
		{
			Out = Named.Colour;
			return true;
		}
	}
	return false;
}

TArray<FString> ApexHudColour::Names()
{
	TArray<FString> Out;
	for (const FNamedColour& Named : NamedColours())
	{
		Out.Add(Named.Name);
	}
	return Out;
}
