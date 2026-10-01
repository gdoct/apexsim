#include "Hud/ApexHudLayout.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Hud/ApexHudComponent.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	/** The HUD's gutters, as UApexHudWidget lays the regions out: Metrics::PageGutter and its edge gutter. */
	constexpr float LayoutSideGutter = 56.0f;
	constexpr float LayoutEdgeGutter = 30.0f;

	/** A number as someone editing the file would write it: two places at most, no trailing zeros. */
	FString LayoutNumber(double Value)
	{
		FString Text = FString::Printf(TEXT("%.2f"), FMath::RoundToDouble(Value * 100.0) / 100.0);
		while (Text.EndsWith(TEXT("0")))
		{
			Text.LeftChopInline(1);
		}
		if (Text.EndsWith(TEXT(".")))
		{
			Text.LeftChopInline(1);
		}
		return Text == TEXT("-0") ? FString(TEXT("0")) : Text;
	}

	bool LayoutReadPair(const FJsonObject& Json, const TCHAR* Key, FVector2D& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
		if (!Json.TryGetArrayField(Key, Items) || Items->Num() != 2)
		{
			return false;
		}
		double X = 0.0;
		double Y = 0.0;
		if (!(*Items)[0]->TryGetNumber(X) || !(*Items)[1]->TryGetNumber(Y))
		{
			return false;
		}
		Out = FVector2D(X, Y);
		return true;
	}

	/** The offset that brings the nearest of `Edges` onto the nearest of `Lines`, if within `Distance`. */
	TOptional<TPair<float, float>> LayoutSnapAxis(const float (&Edges)[3], const TArray<float>& Lines, float Distance)
	{
		TOptional<TPair<float, float>> Best;
		for (const float Edge : Edges)
		{
			for (const float Line : Lines)
			{
				const float Offset = Line - Edge;
				if (FMath::Abs(Offset) <= Distance && (!Best.IsSet() || FMath::Abs(Offset) < FMath::Abs(Best->Key)))
				{
					Best = TPair<float, float>(Offset, Line);
				}
			}
		}
		return Best;
	}
}

FString FApexHudLayout::ToJson() const
{
	// Written by hand, one component to a line, so the file reads (and
	// shares, and diffs) as the short table it is; the engine's pretty
	// printer puts every number of every pair on a line of its own.
	TArray<FString> Ids;
	Components.GetKeys(Ids);
	Ids.Sort();
	TArray<FString> Lines;
	for (const FString& Id : Ids)
	{
		const FApexHudPlacement& Placement = Components[Id];
		FString Line = FString::Printf(TEXT("\"enabled\": %s"), Placement.bEnabled ? TEXT("true") : TEXT("false"));
		if (Placement.bPinned)
		{
			Line += FString::Printf(TEXT(", \"anchor\": [%s, %s], \"position\": [%s, %s]"),
				*LayoutNumber(Placement.Anchor.X), *LayoutNumber(Placement.Anchor.Y),
				*LayoutNumber(Placement.Position.X), *LayoutNumber(Placement.Position.Y));
		}
		if (Placement.Scale != 1.0f)
		{
			Line += FString::Printf(TEXT(", \"scale\": %s"), *LayoutNumber(Placement.Scale));
		}
		Lines.Add(FString::Printf(TEXT("    \"%s\": { %s }"), *Id.ReplaceCharWithEscapedChar(), *Line));
	}
	return FString::Printf(TEXT("{\n  // The HUD's layout, written by the HUD editor (Settings > Gameplay > HUD layout).\n")
		TEXT("  // Positions are in 1080p pixels from the anchor point; docs/HUD_MODDING.md has the rest.\n")
		TEXT("  \"version\": 1,\n  \"components\": {\n%s\n  }\n}\n"),
		*FString::Join(Lines, TEXT(",\n")));
}

bool FApexHudLayout::FromJson(const FString& Text, FString& OutError)
{
	Components.Reset();
	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ApexHud::StripJsonExtras(Text));
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		OutError = FString::Printf(TEXT("not valid JSON: %s"), *Reader->GetErrorMessage());
		return false;
	}
	const TSharedPtr<FJsonObject>* Entries = nullptr;
	if (!Root->TryGetObjectField(TEXT("components"), Entries))
	{
		return true;
	}
	TArray<FString> Skipped;
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : (*Entries)->Values)
	{
		const TSharedPtr<FJsonObject>* Entry = nullptr;
		if (!Field.Value->TryGetObject(Entry))
		{
			Skipped.Add(Field.Key);
			continue;
		}
		FApexHudPlacement Placement;
		(*Entry)->TryGetBoolField(TEXT("enabled"), Placement.bEnabled);
		double Scale = 1.0;
		if ((*Entry)->TryGetNumberField(TEXT("scale"), Scale))
		{
			Placement.Scale = ApexHudPlace::ClampScale(static_cast<float>(Scale));
		}
		const bool bAnchor = LayoutReadPair(**Entry, TEXT("anchor"), Placement.Anchor);
		const bool bPosition = LayoutReadPair(**Entry, TEXT("position"), Placement.Position);
		Placement.bPinned = bAnchor && bPosition;
		Placement.Anchor.X = FMath::Clamp(Placement.Anchor.X, 0.0, 1.0);
		Placement.Anchor.Y = FMath::Clamp(Placement.Anchor.Y, 0.0, 1.0);
		if (bAnchor != bPosition)
		{
			Skipped.Add(Field.Key + TEXT(" (anchor and position go together)"));
		}
		Components.Add(Field.Key, Placement);
	}
	if (!Skipped.IsEmpty())
	{
		OutError = FString::Printf(TEXT("entries not understood: %s"), *FString::Join(Skipped, TEXT(", ")));
	}
	return true;
}

FString FApexHudLayout::DefaultFile()
{
	const TArray<FString> Directories = ApexHud::HudDirectories();
	return FPaths::Combine(Directories.IsEmpty() ? FPaths::ProjectSavedDir() : Directories[0], TEXT("custom"), TEXT("layout.json"));
}

bool FApexHudLayout::Load(const FString& File, FString& OutError)
{
	Components.Reset();
	FString Text;
	if (!FPaths::FileExists(File))
	{
		return true;
	}
	if (!FFileHelper::LoadFileToString(Text, *File))
	{
		OutError = TEXT("cannot be read");
		return false;
	}
	return FromJson(Text, OutError);
}

bool FApexHudLayout::Save(const FString& File) const
{
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(File), /*Tree*/ true);
	return FFileHelper::SaveStringToFile(ToJson(), *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

FApexHudPlacement ApexHudPlace::PinRect(const FSlateRect& Rect, const FVector2D& ScreenSize, float Scale, bool bEnabled)
{
	auto Third = [](double Centre, double Extent)
	{
		return Centre < Extent / 3.0 ? 0.0 : Centre > Extent * 2.0 / 3.0 ? 1.0 : 0.5;
	};
	const FVector2D Size = Rect.GetSize();
	const FVector2D Centre = Rect.GetCenter();

	FApexHudPlacement Out;
	Out.bEnabled = bEnabled;
	Out.bPinned = true;
	Out.Scale = Scale;
	Out.Anchor = FVector2D(Third(Centre.X, ScreenSize.X), Third(Centre.Y, ScreenSize.Y));
	// The anchor point of the rectangle, measured from the same point of the screen.
	Out.Position = Rect.GetTopLeft() + Size * Out.Anchor - ScreenSize * Out.Anchor;
	return Out;
}

FVector2D ApexHudPlace::TopLeft(const FApexHudPlacement& Placement, const FVector2D& Size, const FVector2D& ScreenSize)
{
	return ScreenSize * Placement.Anchor + Placement.Position - Size * Placement.Anchor;
}

float ApexHudPlace::ClampScale(float Scale)
{
	return FMath::Clamp(FMath::RoundToFloat(Scale * 20.0f) / 20.0f, FApexHudLayout::MinScale, FApexHudLayout::MaxScale);
}

FSlateRect ApexHudPlace::Snap(const FSlateRect& Rect, const FVector2D& ScreenSize, const TArray<FSlateRect>& Others,
	float Distance, FSnapLines& OutLines)
{
	TArray<float> LinesX = {LayoutSideGutter, static_cast<float>(ScreenSize.X) - LayoutSideGutter, static_cast<float>(ScreenSize.X) * 0.5f};
	TArray<float> LinesY = {LayoutEdgeGutter, static_cast<float>(ScreenSize.Y) - LayoutEdgeGutter, static_cast<float>(ScreenSize.Y) * 0.5f};
	for (const FSlateRect& Other : Others)
	{
		LinesX.Append({Other.Left, Other.Right, (Other.Left + Other.Right) * 0.5f});
		LinesY.Append({Other.Top, Other.Bottom, (Other.Top + Other.Bottom) * 0.5f});
	}

	FSlateRect Out = Rect;
	OutLines = FSnapLines();
	const float EdgesX[3] = {Rect.Left, Rect.Right, (Rect.Left + Rect.Right) * 0.5f};
	if (const TOptional<TPair<float, float>> SnapX = LayoutSnapAxis(EdgesX, LinesX, Distance))
	{
		Out = Out.OffsetBy(FVector2D(SnapX->Key, 0.0f));
		OutLines.X = SnapX->Value;
	}
	const float EdgesY[3] = {Rect.Top, Rect.Bottom, (Rect.Top + Rect.Bottom) * 0.5f};
	if (const TOptional<TPair<float, float>> SnapY = LayoutSnapAxis(EdgesY, LinesY, Distance))
	{
		Out = Out.OffsetBy(FVector2D(0.0f, SnapY->Key));
		OutLines.Y = SnapY->Value;
	}
	return Out;
}
