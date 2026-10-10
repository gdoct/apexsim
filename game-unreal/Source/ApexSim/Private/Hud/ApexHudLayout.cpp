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
		TEXT("  // Positions are in 1080p pixels from the anchor point; docs/game/hud-modding.md has the rest.\n")
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

// --- Named layouts and where they apply ------------------------------------------------

const FString ApexHudLayouts::DefaultName(TEXT("Default"));

namespace
{
	bool LayoutSameName(const FString& A, const FString& B)
	{
		return A.Equals(B, ESearchCase::IgnoreCase);
	}

	FString LayoutQuote(const FString& Text)
	{
		return FString::Printf(TEXT("\"%s\""), *Text.ReplaceCharWithEscapedChar());
	}

	FString LayoutCustomDir()
	{
		return FPaths::GetPath(FApexHudLayout::DefaultFile());
	}
}

FString ApexHudLayouts::Directory()
{
	return FPaths::Combine(LayoutCustomDir(), TEXT("layouts"));
}

FString ApexHudLayouts::BindingsFile()
{
	return FPaths::Combine(LayoutCustomDir(), TEXT("layout_bindings.json"));
}

FString ApexHudLayouts::SanitiseName(const FString& Name)
{
	FString Out;
	for (const TCHAR Char : Name)
	{
		// What a file name cannot hold on Windows, and control characters.
		if (Char < 32 || FCString::Strchr(TEXT("\\/:*?\"<>|"), Char))
		{
			continue;
		}
		Out.AppendChar(Char);
	}
	Out.TrimStartAndEndInline();
	if (Out.Len() > 40)
	{
		Out.LeftInline(40);
		Out.TrimEndInline();
	}
	// A name of nothing but dots is a path.
	return Out.Replace(TEXT("."), TEXT("")).IsEmpty() ? FString() : Out;
}

FString ApexHudLayouts::FileFor(const FString& Name)
{
	if (Name.IsEmpty() || LayoutSameName(Name, DefaultName))
	{
		return FApexHudLayout::DefaultFile();
	}
	return FPaths::Combine(Directory(), SanitiseName(Name) + TEXT(".json"));
}

bool ApexHudLayouts::Exists(const FString& Name)
{
	return Name.IsEmpty() || LayoutSameName(Name, DefaultName) || FPaths::FileExists(FileFor(Name));
}

TArray<FString> ApexHudLayouts::List()
{
	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *FPaths::Combine(Directory(), TEXT("*.json")), /*Files*/ true, /*Directories*/ false);
	TArray<FString> Names;
	for (const FString& File : Files)
	{
		const FString Name = FPaths::GetBaseFilename(File);
		if (!Name.IsEmpty() && !LayoutSameName(Name, DefaultName))
		{
			Names.Add(Name);
		}
	}
	Names.Sort([](const FString& A, const FString& B) { return A.Compare(B, ESearchCase::IgnoreCase) < 0; });
	Names.Insert(DefaultName, 0);
	return Names;
}

bool ApexHudLayouts::Rename(const FString& From, const FString& To)
{
	const FString Target = SanitiseName(To);
	if (Target.IsEmpty() || LayoutSameName(From, DefaultName) || LayoutSameName(Target, DefaultName) || !Exists(From))
	{
		return false;
	}
	// A change of case only is the same file, which a move refuses.
	if (!LayoutSameName(From, Target) && Exists(Target))
	{
		return false;
	}
	return IFileManager::Get().Move(*FileFor(Target), *FileFor(From), /*Replace*/ false, /*EvenIfReadOnly*/ false);
}

bool ApexHudLayouts::Delete(const FString& Name)
{
	return !LayoutSameName(Name, DefaultName) && FPaths::FileExists(FileFor(Name)) && IFileManager::Get().Delete(*FileFor(Name));
}

FString ApexHudLayouts::UniqueName(const FString& Base)
{
	FString Stem = SanitiseName(Base);
	if (Stem.IsEmpty())
	{
		Stem = TEXT("Layout");
	}
	FString Name = Stem;
	for (int32 N = 2; Exists(Name) || LayoutSameName(Name, DefaultName); ++N)
	{
		Name = FString::Printf(TEXT("%s %d"), *Stem, N);
	}
	return Name;
}

FString ApexHudLayouts::FBindings::Resolve(const FContext& Context) const
{
	if (Context.bWatching && !Watching.IsEmpty())
	{
		return Watching;
	}
	if (Context.bGarageMode && !Hotlap.IsEmpty())
	{
		return Hotlap;
	}
	if (!Context.CarClass.IsEmpty())
	{
		for (const TPair<FString, FString>& Entry : Classes)
		{
			if (!Entry.Value.IsEmpty() && LayoutSameName(Entry.Key, Context.CarClass))
			{
				return Entry.Value;
			}
		}
	}
	return Everywhere.IsEmpty() ? DefaultName : Everywhere;
}

FString ApexHudLayouts::FBindings::Describe(const FString& Layout) const
{
	TArray<FString> Parts;
	if (LayoutSameName(Everywhere.IsEmpty() ? DefaultName : Everywhere, Layout))
	{
		Parts.Add(TEXT("Everywhere else"));
	}
	if (LayoutSameName(Watching, Layout))
	{
		Parts.Add(TEXT("Watching"));
	}
	if (LayoutSameName(Hotlap, Layout))
	{
		Parts.Add(TEXT("Hotlap and qualifying"));
	}
	TArray<FString> Keys;
	Classes.GetKeys(Keys);
	Keys.Sort();
	for (const FString& Key : Keys)
	{
		if (LayoutSameName(Classes[Key], Layout))
		{
			Parts.Add(Key);
		}
	}
	return FString::Join(Parts, TEXT(", "));
}

void ApexHudLayouts::FBindings::Rename(const FString& From, const FString& To)
{
	auto Swap = [&](FString& Slot)
	{
		if (LayoutSameName(Slot, From))
		{
			Slot = To;
		}
	};
	Swap(Everywhere);
	Swap(Watching);
	Swap(Hotlap);
	for (TPair<FString, FString>& Entry : Classes)
	{
		Swap(Entry.Value);
	}
}

void ApexHudLayouts::FBindings::Forget(const FString& Layout)
{
	Rename(Layout, FString());
	for (auto It = Classes.CreateIterator(); It; ++It)
	{
		if (It.Value().IsEmpty())
		{
			It.RemoveCurrent();
		}
	}
}

FString ApexHudLayouts::FBindings::ToJson() const
{
	TArray<FString> Lines;
	Lines.Add(TEXT("  \"version\": 1"));
	if (!Everywhere.IsEmpty() && !LayoutSameName(Everywhere, DefaultName))
	{
		Lines.Add(FString::Printf(TEXT("  \"everywhere\": %s"), *LayoutQuote(Everywhere)));
	}
	if (!Watching.IsEmpty())
	{
		Lines.Add(FString::Printf(TEXT("  \"watching\": %s"), *LayoutQuote(Watching)));
	}
	if (!Hotlap.IsEmpty())
	{
		Lines.Add(FString::Printf(TEXT("  \"hotlap\": %s"), *LayoutQuote(Hotlap)));
	}
	TArray<FString> Keys;
	Classes.GetKeys(Keys);
	Keys.Sort();
	TArray<FString> ClassLines;
	for (const FString& Key : Keys)
	{
		if (!Classes[Key].IsEmpty())
		{
			ClassLines.Add(FString::Printf(TEXT("    %s: %s"), *LayoutQuote(Key), *LayoutQuote(Classes[Key])));
		}
	}
	if (!ClassLines.IsEmpty())
	{
		Lines.Add(FString::Printf(TEXT("  \"classes\": {\n%s\n  }"), *FString::Join(ClassLines, TEXT(",\n"))));
	}
	return FString::Printf(TEXT("{\n  // Which HUD layout applies where, written by the HUD editor. Names are files in layouts/ (or Default).\n")
		TEXT("  // Watching beats hotlap and qualifying, which beats the car's class, which beats everywhere.\n%s\n}\n"),
		*FString::Join(Lines, TEXT(",\n")));
}

bool ApexHudLayouts::FBindings::FromJson(const FString& Text, FString& OutError)
{
	*this = FBindings();
	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ApexHud::StripJsonExtras(Text));
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		OutError = FString::Printf(TEXT("not valid JSON: %s"), *Reader->GetErrorMessage());
		return false;
	}
	Root->TryGetStringField(TEXT("everywhere"), Everywhere);
	Root->TryGetStringField(TEXT("watching"), Watching);
	Root->TryGetStringField(TEXT("hotlap"), Hotlap);
	const TSharedPtr<FJsonObject>* Entries = nullptr;
	if (Root->TryGetObjectField(TEXT("classes"), Entries))
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : (*Entries)->Values)
		{
			FString Layout;
			if (Field.Value->TryGetString(Layout) && !Layout.IsEmpty())
			{
				Classes.Add(Field.Key, Layout);
			}
		}
	}
	return true;
}

bool ApexHudLayouts::FBindings::Load(FString& OutError)
{
	*this = FBindings();
	FString Text;
	if (!FPaths::FileExists(BindingsFile()))
	{
		return true;
	}
	if (!FFileHelper::LoadFileToString(Text, *BindingsFile()))
	{
		OutError = TEXT("cannot be read");
		return false;
	}
	return FromJson(Text, OutError);
}

bool ApexHudLayouts::FBindings::Save() const
{
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(BindingsFile()), /*Tree*/ true);
	return FFileHelper::SaveStringToFile(ToJson(), *BindingsFile(), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
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
