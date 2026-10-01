#pragma once

#include "CoreMinimal.h"

/**
 * One value the HUD can show: what a data point holds and what an expression
 * in a component evaluates to.
 *
 * Dynamically typed on purpose. A HUD component is a file a player edits, and
 * `"text": "LAP {lap.display}"` should not need a cast to turn a lap number
 * into text; every value converts to every other the way a spreadsheet cell
 * would (see AsBool / AsNumber / AsString / AsColour).
 */
enum class EApexHudValueType : uint8
{
	/** Unknown: a data point the game cannot fill (an older server), or a typo. */
	None,
	Bool,
	Number,
	String,
	Colour,
};

struct APEXSIM_API FApexHudValue
{
	EApexHudValueType Type = EApexHudValueType::None;
	double Number = 0.0;
	FString String;
	FLinearColor Colour = FLinearColor::Transparent;

	static FApexHudValue Of(bool bValue)
	{
		FApexHudValue Out;
		Out.Type = EApexHudValueType::Bool;
		Out.Number = bValue ? 1.0 : 0.0;
		return Out;
	}
	static FApexHudValue Of(double Value)
	{
		FApexHudValue Out;
		Out.Type = EApexHudValueType::Number;
		Out.Number = Value;
		return Out;
	}
	static FApexHudValue Of(float Value) { return Of(static_cast<double>(Value)); }
	static FApexHudValue Of(int32 Value) { return Of(static_cast<double>(Value)); }
	static FApexHudValue Of(const FString& Value)
	{
		FApexHudValue Out;
		Out.Type = EApexHudValueType::String;
		Out.String = Value;
		return Out;
	}
	static FApexHudValue Of(const TCHAR* Value) { return Of(FString(Value)); }
	static FApexHudValue OfColour(const FLinearColor& Value)
	{
		FApexHudValue Out;
		Out.Type = EApexHudValueType::Colour;
		Out.Colour = Value;
		return Out;
	}

	bool IsNone() const { return Type == EApexHudValueType::None; }

	/** False for none, false, 0 and the empty string; true otherwise. */
	bool AsBool() const;
	/** Bools are 0/1, numeric strings parse, everything else is 0. */
	double AsNumber() const;
	/** Whole numbers print without a decimal point; others to at most three places. */
	FString AsString() const;
	/** A colour, or a string naming one (ApexHudColour::Parse). */
	bool AsColour(FLinearColor& Out) const;

	/** Same type and same value (numbers compare exactly, strings case-sensitively); a number and a bool compare by value. */
	bool Equals(const FApexHudValue& Other) const;
};

/** One row of a list data point (a car in the standings, a sector, a tyre). */
using FApexHudRecord = TMap<FName, FApexHudValue>;

namespace ApexHudColour
{
	/**
	 * A colour from its name in the menu palette (`accent`, `surface`,
	 * `text_muted`, …, plus the timing colours `session_best`,
	 * `personal_best` and `tyre_cold`) or a hex code (`#RRGGBB`,
	 * `#RRGGBBAA`, sRGB). Case-insensitive.
	 */
	APEXSIM_API bool Parse(const FString& Text, FLinearColor& Out);

	/** Every name Parse accepts, for the docs and the error messages. */
	APEXSIM_API TArray<FString> Names();
}
