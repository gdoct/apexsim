#include "ApexSetupShare.h"

namespace
{
	FString SetupShareCleanName(const FString& Name)
	{
		FString Clean;
		for (const TCHAR Char : Name)
		{
			Clean.AppendChar(Char == TEXT(';') || Char == TEXT('=') || Char == TEXT('\n') || Char == TEXT('\r') || Char == TEXT('`')
				? TEXT(' ') : Char);
		}
		Clean.TrimStartAndEndInline();
		return Clean.Left(40);
	}

	/** Where the line ends in a chat message: whitespace other than spaces inside the name, a quote or a backtick. */
	bool SetupShareEndsLine(TCHAR Char)
	{
		return Char == TEXT('\n') || Char == TEXT('\r') || Char == TEXT('`') || Char == TEXT('"') || Char == TEXT('\t');
	}
}

FString ApexSetupShare::Encode(const FShared& Shared)
{
	TArray<FString> Clicks;
	for (int32 Knob = 0; Knob < FApexCarSetup::KnobCount; ++Knob)
	{
		Clicks.Add(FString::FromInt(Shared.Setup.GetClick(Knob)));
	}
	return FString::Printf(TEXT("%scar=%s;crc=%08X;name=%s;c=%s"), Prefix, *Shared.CarId, Shared.CarCrc,
		*SetupShareCleanName(Shared.Name), *FString::Join(Clicks, TEXT(",")));
}

bool ApexSetupShare::Decode(const FString& Text, FShared& Out, FString& OutError)
{
	const int32 Start = Text.Find(Prefix, ESearchCase::IgnoreCase);
	if (Start == INDEX_NONE)
	{
		OutError = TEXT("No setup to paste: copy one made with COPY AS TEXT first");
		return false;
	}
	int32 End = Start;
	while (End < Text.Len() && !SetupShareEndsLine(Text[End]))
	{
		++End;
	}
	const FString Line = Text.Mid(Start + FCString::Strlen(Prefix), End - Start - FCString::Strlen(Prefix));

	FShared Shared;
	bool bHaveClicks = false;
	TArray<FString> Fields;
	Line.ParseIntoArray(Fields, TEXT(";"));
	for (const FString& Field : Fields)
	{
		FString Key, Value;
		if (!Field.Split(TEXT("="), &Key, &Value))
		{
			continue;
		}
		Key.TrimStartAndEndInline();
		Value.TrimStartAndEndInline();
		if (Key == TEXT("car"))
		{
			Shared.CarId = Value;
		}
		else if (Key == TEXT("crc"))
		{
			Shared.CarCrc = static_cast<uint32>(FParse::HexNumber(*Value));
		}
		else if (Key == TEXT("name"))
		{
			Shared.Name = SetupShareCleanName(Value);
		}
		else if (Key == TEXT("c"))
		{
			TArray<FString> Clicks;
			Value.ParseIntoArray(Clicks, TEXT(","));
			for (int32 Knob = 0; Knob < Clicks.Num() && Knob < FApexCarSetup::KnobCount; ++Knob)
			{
				FString Click = Clicks[Knob].TrimStartAndEnd();
				if (!Click.IsNumeric())
				{
					OutError = TEXT("That setup text is damaged: paste the whole line");
					return false;
				}
				Shared.Setup.Clicks[Knob] = FCString::Atoi(*Click);
			}
			bHaveClicks = Clicks.Num() > 0;
		}
	}
	if (Shared.CarId.IsEmpty() || !bHaveClicks)
	{
		OutError = TEXT("That setup text is damaged: paste the whole line");
		return false;
	}
	Shared.Setup.Clamp();
	if (Shared.Name.IsEmpty())
	{
		Shared.Name = TEXT("Pasted setup");
	}
	Out = MoveTemp(Shared);
	return true;
}

FString ApexSetupShare::FileName(const FShared& Shared)
{
	FString Name = FString::Printf(TEXT("%s-%s"), *Shared.CarId, *SetupShareCleanName(Shared.Name));
	FString Safe;
	for (const TCHAR Char : Name)
	{
		Safe.AppendChar(FChar::IsAlnum(Char) || Char == TEXT('-') || Char == TEXT('_') ? Char : TEXT('_'));
	}
	return Safe + TEXT(".txt");
}
