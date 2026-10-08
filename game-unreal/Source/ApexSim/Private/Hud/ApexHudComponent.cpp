#include "Hud/ApexHudComponent.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Hud/ApexHudData.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	const TCHAR* const HudComponentFile = TEXT("component.json");

	struct FHudTypeName
	{
		const TCHAR* Name;
		EApexHudElementType Type;
	};

	const FHudTypeName HudTypeNames[] = {
		{TEXT("row"), EApexHudElementType::Row},
		{TEXT("column"), EApexHudElementType::Column},
		{TEXT("stack"), EApexHudElementType::Stack},
		{TEXT("panel"), EApexHudElementType::Panel},
		{TEXT("text"), EApexHudElementType::Text},
		{TEXT("label"), EApexHudElementType::Label},
		{TEXT("rect"), EApexHudElementType::Rect},
		{TEXT("bar"), EApexHudElementType::Bar},
		{TEXT("spacer"), EApexHudElementType::Spacer},
		{TEXT("divider"), EApexHudElementType::Divider},
		{TEXT("keycap"), EApexHudElementType::KeyCap},
		{TEXT("image"), EApexHudElementType::Image},
		{TEXT("minimap"), EApexHudElementType::Minimap},
		{TEXT("mirror"), EApexHudElementType::Mirror},
	};

	/** The keys each element type reads beyond the common ones; anything else draws a warning. */
	TArray<FString> HudKeysFor(EApexHudElementType Type)
	{
		switch (Type)
		{
		case EApexHudElementType::Panel:
			return {TEXT("padding"), TEXT("background"), TEXT("outline"), TEXT("outline_width"), TEXT("radius"), TEXT("direction")};
		case EApexHudElementType::Text:
		case EApexHudElementType::Label:
			return {TEXT("text"), TEXT("font"), TEXT("size"), TEXT("tracking"), TEXT("bold"), TEXT("upper"), TEXT("color"), TEXT("justify")};
		case EApexHudElementType::Rect:
			return {TEXT("color"), TEXT("radius"), TEXT("outline"), TEXT("outline_width")};
		case EApexHudElementType::Bar:
			return {TEXT("value"), TEXT("color"), TEXT("background"), TEXT("direction")};
		case EApexHudElementType::Divider:
			return {TEXT("vertical")};
		case EApexHudElementType::KeyCap:
			return {TEXT("text")};
		case EApexHudElementType::Image:
			return {TEXT("file"), TEXT("color")};
		case EApexHudElementType::Minimap:
			return {TEXT("color")};
		default:
			return {};
		}
	}

	bool HudIsContainer(EApexHudElementType Type)
	{
		return Type == EApexHudElementType::Row || Type == EApexHudElementType::Column
			|| Type == EApexHudElementType::Stack || Type == EApexHudElementType::Panel;
	}

	/** Reads one element; false (with an error) when it cannot be drawn. */
	struct FHudElementParser
	{
		const FString& Folder;
		const FString& ComponentId;
		FApexHudLoadReport& Report;

		bool Error(const FString& Path, const FString& Message)
		{
			Report.Errors.Add(FString::Printf(TEXT("%s: %s: %s"), *ComponentId, *Path, *Message));
			return false;
		}

		void Warn(const FString& Path, const FString& Message)
		{
			Report.Warnings.Add(FString::Printf(TEXT("%s: %s: %s"), *ComponentId, *Path, *Message));
		}

		bool ReadNumber(const FJsonObject& Json, const TCHAR* Key, float& Out, const FString& Path)
		{
			const TSharedPtr<FJsonValue> Value = Json.TryGetField(Key);
			if (!Value.IsValid())
			{
				return true;
			}
			double Number = 0.0;
			if (!Value->TryGetNumber(Number))
			{
				return Error(Path, FString::Printf(TEXT("'%s' must be a number"), Key));
			}
			Out = static_cast<float>(Number);
			return true;
		}

		bool ReadBool(const FJsonObject& Json, const TCHAR* Key, bool& Out, const FString& Path)
		{
			const TSharedPtr<FJsonValue> Value = Json.TryGetField(Key);
			if (!Value.IsValid())
			{
				return true;
			}
			if (!Value->TryGetBool(Out))
			{
				return Error(Path, FString::Printf(TEXT("'%s' must be true or false"), Key));
			}
			return true;
		}

		bool ReadString(const FJsonObject& Json, const TCHAR* Key, FString& Out, const FString& Path)
		{
			const TSharedPtr<FJsonValue> Value = Json.TryGetField(Key);
			if (!Value.IsValid())
			{
				return true;
			}
			if (!Value->TryGetString(Out))
			{
				return Error(Path, FString::Printf(TEXT("'%s' must be text"), Key));
			}
			return true;
		}

		/** A number for all four sides, [horizontal, vertical], or [left, top, right, bottom]. */
		bool ReadMargin(const FJsonObject& Json, const TCHAR* Key, FMargin& Out, const FString& Path)
		{
			const TSharedPtr<FJsonValue> Value = Json.TryGetField(Key);
			if (!Value.IsValid())
			{
				return true;
			}
			double Number = 0.0;
			if (Value->TryGetNumber(Number))
			{
				Out = FMargin(static_cast<float>(Number));
				return true;
			}
			const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
			if (Value->TryGetArray(Items) && (Items->Num() == 2 || Items->Num() == 4))
			{
				TArray<float> Numbers;
				for (const TSharedPtr<FJsonValue>& Item : *Items)
				{
					if (!Item->TryGetNumber(Number))
					{
						break;
					}
					Numbers.Add(static_cast<float>(Number));
				}
				if (Numbers.Num() == 2)
				{
					Out = FMargin(Numbers[0], Numbers[1]);
					return true;
				}
				if (Numbers.Num() == 4)
				{
					Out = FMargin(Numbers[0], Numbers[1], Numbers[2], Numbers[3]);
					return true;
				}
			}
			return Error(Path, FString::Printf(TEXT("'%s' must be a number, [horizontal, vertical] or [left, top, right, bottom]"), Key));
		}

		enum class EPropKind : uint8
		{
			/** `true` / `false`, or `=expression`. */
			Bool,
			/** A number, or `=expression`. */
			Number,
			/** A colour name or #hex, or `=expression`. */
			Colour,
			/** Text with `{expression}` holes, or `=expression`. */
			Template,
		};

		bool ReadProp(const FJsonObject& Json, const TCHAR* Key, EPropKind Kind, FApexHudProp& Out, const FString& Path)
		{
			const TSharedPtr<FJsonValue> Value = Json.TryGetField(Key);
			if (!Value.IsValid())
			{
				return true;
			}
			FString Text;
			bool bBool = false;
			double Number = 0.0;
			if (Value->TryGetBool(bBool) && Value->Type == EJson::Boolean)
			{
				Out.Expr = FApexHudExpr::Constant(FApexHudValue::Of(bBool));
				return true;
			}
			if (Value->Type == EJson::Number && Value->TryGetNumber(Number))
			{
				if (Kind == EPropKind::Template)
				{
					Out.Expr = FApexHudExpr::Constant(FApexHudValue::Of(FApexHudValue::Of(Number).AsString()));
				}
				else
				{
					Out.Expr = FApexHudExpr::Constant(FApexHudValue::Of(Number));
				}
				return true;
			}
			if (!Value->TryGetString(Text))
			{
				return Error(Path, FString::Printf(TEXT("'%s' must be text, a number or true/false"), Key));
			}

			FString CompileError;
			if (Text.StartsWith(TEXT("=")))
			{
				Out.Expr = FApexHudExpr::Compile(Text.Mid(1), CompileError);
			}
			else if (Kind == EPropKind::Template)
			{
				Out.Expr = FApexHudExpr::CompileTemplate(Text, CompileError);
			}
			else if (Kind == EPropKind::Colour)
			{
				FLinearColor Colour;
				if (!ApexHudColour::Parse(Text, Colour))
				{
					return Error(Path, FString::Printf(TEXT("'%s': '%s' is not a colour (a #RRGGBB code, or one of %s; start with '=' for an expression)"),
						Key, *Text, *FString::Join(ApexHudColour::Names(), TEXT(", "))));
				}
				Out.Expr = FApexHudExpr::Constant(FApexHudValue::OfColour(Colour));
				return true;
			}
			else
			{
				return Error(Path, FString::Printf(TEXT("'%s': write an expression with a leading '=': \"=%s\""), Key, *Text));
			}
			if (!Out.Expr.IsValid())
			{
				return Error(Path, FString::Printf(TEXT("'%s': %s"), Key, *CompileError));
			}
			return true;
		}

		bool ReadRepeat(const FJsonObject& Json, FApexHudRepeat& Out, const FString& Path)
		{
			const TSharedPtr<FJsonValue> Value = Json.TryGetField(TEXT("repeat"));
			if (!Value.IsValid())
			{
				return true;
			}
			double Number = 0.0;
			if (Value->Type == EJson::Number && Value->TryGetNumber(Number))
			{
				Out.Count = FMath::RoundToInt(Number);
			}
			else if (const TSharedPtr<FJsonObject>* Object = nullptr; Value->TryGetObject(Object))
			{
				double Count = 0.0;
				double Max = 0.0;
				FString List;
				FString Focus;
				(*Object)->TryGetNumberField(TEXT("count"), Count);
				(*Object)->TryGetNumberField(TEXT("max"), Max);
				(*Object)->TryGetStringField(TEXT("list"), List);
				(*Object)->TryGetStringField(TEXT("focus"), Focus);
				Out.Count = FMath::RoundToInt(Count);
				Out.Max = FMath::RoundToInt(Max);
				Out.List = List.IsEmpty() ? NAME_None : FName(*List);
				Out.Focus = Focus.IsEmpty() ? NAME_None : FName(*Focus);
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : (*Object)->Values)
				{
					if (Field.Key != TEXT("count") && Field.Key != TEXT("max") && Field.Key != TEXT("list") && Field.Key != TEXT("focus"))
					{
						Warn(Path, FString::Printf(TEXT("repeat: unknown key '%s'"), *Field.Key));
					}
				}
			}
			else
			{
				return Error(Path, TEXT("'repeat' must be a count or { \"list\": ..., \"max\": ... }"));
			}

			if (Out.IsList())
			{
				if (!ApexHudData::ListFields().Contains(Out.List))
				{
					TArray<FName> Lists;
					ApexHudData::ListFields().GetKeys(Lists);
					return Error(Path, FString::Printf(TEXT("repeat: there is no list '%s' (lists: %s)"), *Out.List.ToString(),
						*FString::JoinBy(Lists, TEXT(", "), [](const FName& Name) { return Name.ToString(); })));
				}
				if (Out.Max <= 0)
				{
					// Room for every row a list can plausibly hold.
					Out.Max = 32;
				}
				if (!Out.Focus.IsNone() && !ApexHudData::ListFields()[Out.List].Contains(Out.Focus))
				{
					Warn(Path, FString::Printf(TEXT("repeat: '%s' has no field '%s'"), *Out.List.ToString(), *Out.Focus.ToString()));
				}
			}
			if (Out.Slots() <= 0 || Out.Slots() > 256)
			{
				return Error(Path, TEXT("repeat: the count must be between 1 and 256"));
			}
			return true;
		}

		bool Parse(const FJsonObject& Json, const FString& Path, FApexHudElementDef& Out)
		{
			Out.Path = Path;

			FString TypeName;
			if (!Json.TryGetStringField(TEXT("type"), TypeName))
			{
				return Error(Path, TEXT("every element needs a \"type\""));
			}
			const FHudTypeName* Found = nullptr;
			for (const FHudTypeName& Entry : HudTypeNames)
			{
				if (TypeName.Equals(Entry.Name, ESearchCase::IgnoreCase))
				{
					Found = &Entry;
				}
			}
			if (!Found)
			{
				TArray<FString> Names;
				for (const FHudTypeName& Entry : HudTypeNames)
				{
					Names.Add(Entry.Name);
				}
				return Error(Path, FString::Printf(TEXT("unknown type '%s' (types: %s)"), *TypeName, *FString::Join(Names, TEXT(", "))));
			}
			Out.Type = Found->Type;

			// The caption style's defaults; anything given below overrides them.
			if (Out.Type == EApexHudElementType::Label)
			{
				Out.FontFace = TEXT("mono");
				Out.FontSize = 10.0f;
				Out.Tracking = 160;
				Out.bUpper = true;
				Out.Colour.Expr = FApexHudExpr::Constant(FApexHudValue::Of(TEXT("text_muted")));
			}

			TArray<FString> Known = {TEXT("type"), TEXT("children"), TEXT("margin"), TEXT("halign"), TEXT("valign"),
				TEXT("fill"), TEXT("width"), TEXT("height"), TEXT("visible"), TEXT("repeat")};
			Known.Append(HudKeysFor(Out.Type));
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Json.Values)
			{
				if (!Known.Contains(Field.Key))
				{
					Warn(Path, FString::Printf(TEXT("'%s' is not read by a %s"), *Field.Key, Found->Name));
				}
			}

			// `fill` is a number, or an expression sizing the element by the data.
			bool bOk = true;
			if (const TSharedPtr<FJsonValue> FillValue = Json.TryGetField(TEXT("fill")); FillValue.IsValid() && FillValue->Type == EJson::String)
			{
				bOk = ReadProp(Json, TEXT("fill"), EPropKind::Number, Out.FillShare, Path);
				Out.Fill = 1.0f;
			}
			else
			{
				bOk = ReadNumber(Json, TEXT("fill"), Out.Fill, Path);
			}
			bOk = bOk && ReadMargin(Json, TEXT("margin"), Out.Margin, Path)
				&& ReadNumber(Json, TEXT("width"), Out.Width, Path)
				&& ReadNumber(Json, TEXT("height"), Out.Height, Path)
				&& ReadProp(Json, TEXT("visible"), EPropKind::Bool, Out.Visible, Path)
				&& ReadRepeat(Json, Out.Repeat, Path);
			if (!bOk)
			{
				return false;
			}

			FString Align;
			if (Json.TryGetStringField(TEXT("halign"), Align))
			{
				if (Align == TEXT("left")) { Out.HAlign = HAlign_Left; }
				else if (Align == TEXT("center")) { Out.HAlign = HAlign_Center; }
				else if (Align == TEXT("right")) { Out.HAlign = HAlign_Right; }
				else if (Align == TEXT("fill")) { Out.HAlign = HAlign_Fill; }
				else { return Error(Path, TEXT("'halign' is left, center, right or fill")); }
			}
			if (Json.TryGetStringField(TEXT("valign"), Align))
			{
				if (Align == TEXT("top")) { Out.VAlign = VAlign_Top; }
				else if (Align == TEXT("center")) { Out.VAlign = VAlign_Center; }
				else if (Align == TEXT("bottom")) { Out.VAlign = VAlign_Bottom; }
				else if (Align == TEXT("fill")) { Out.VAlign = VAlign_Fill; }
				else { return Error(Path, TEXT("'valign' is top, center, bottom or fill")); }
			}

			switch (Out.Type)
			{
			case EApexHudElementType::Panel:
			{
				FString Direction = TEXT("column");
				bOk = ReadMargin(Json, TEXT("padding"), Out.Padding, Path)
					&& ReadProp(Json, TEXT("background"), EPropKind::Colour, Out.Background, Path)
					&& ReadProp(Json, TEXT("outline"), EPropKind::Colour, Out.Outline, Path)
					&& ReadNumber(Json, TEXT("outline_width"), Out.OutlineWidth, Path)
					&& ReadNumber(Json, TEXT("radius"), Out.Radius, Path)
					&& ReadString(Json, TEXT("direction"), Direction, Path);
				if (bOk)
				{
					if (Direction == TEXT("column")) { Out.Direction = EApexHudElementType::Column; }
					else if (Direction == TEXT("row")) { Out.Direction = EApexHudElementType::Row; }
					else if (Direction == TEXT("stack")) { Out.Direction = EApexHudElementType::Stack; }
					else { return Error(Path, TEXT("'direction' is column, row or stack")); }
				}
				break;
			}
			case EApexHudElementType::Text:
			case EApexHudElementType::Label:
			{
				float Tracking = static_cast<float>(Out.Tracking);
				FString Justify;
				bOk = ReadProp(Json, TEXT("text"), EPropKind::Template, Out.Text, Path)
					&& ReadString(Json, TEXT("font"), Out.FontFace, Path)
					&& ReadNumber(Json, TEXT("size"), Out.FontSize, Path)
					&& ReadNumber(Json, TEXT("tracking"), Tracking, Path)
					&& ReadProp(Json, TEXT("bold"), EPropKind::Bool, Out.Bold, Path)
					&& ReadBool(Json, TEXT("upper"), Out.bUpper, Path)
					&& ReadProp(Json, TEXT("color"), EPropKind::Colour, Out.Colour, Path)
					&& ReadString(Json, TEXT("justify"), Justify, Path);
				Out.Tracking = FMath::RoundToInt(Tracking);
				if (bOk && Out.FontFace != TEXT("body") && Out.FontFace != TEXT("display") && Out.FontFace != TEXT("mono"))
				{
					return Error(Path, TEXT("'font' is body, display or mono"));
				}
				if (bOk && !Justify.IsEmpty())
				{
					if (Justify == TEXT("left")) { Out.Justify = ETextJustify::Left; }
					else if (Justify == TEXT("center")) { Out.Justify = ETextJustify::Center; }
					else if (Justify == TEXT("right")) { Out.Justify = ETextJustify::Right; }
					else { return Error(Path, TEXT("'justify' is left, center or right")); }
				}
				if (bOk && !Out.Text.IsSet())
				{
					Warn(Path, TEXT("a text with no 'text'"));
				}
				break;
			}
			case EApexHudElementType::Rect:
				bOk = ReadProp(Json, TEXT("color"), EPropKind::Colour, Out.Colour, Path)
					&& ReadNumber(Json, TEXT("radius"), Out.Radius, Path)
					&& ReadProp(Json, TEXT("outline"), EPropKind::Colour, Out.Outline, Path)
					&& ReadNumber(Json, TEXT("outline_width"), Out.OutlineWidth, Path);
				if (bOk && (Out.Width < 0.0f || Out.Height < 0.0f) && Out.Fill <= 0.0f)
				{
					Warn(Path, TEXT("a rect with no width or height draws nothing"));
				}
				break;
			case EApexHudElementType::Bar:
				bOk = ReadProp(Json, TEXT("value"), EPropKind::Number, Out.Value, Path)
					&& ReadProp(Json, TEXT("color"), EPropKind::Colour, Out.Colour, Path)
					&& ReadProp(Json, TEXT("background"), EPropKind::Colour, Out.Background, Path)
					&& ReadString(Json, TEXT("direction"), Out.BarDirection, Path);
				if (bOk && Out.BarDirection != TEXT("right") && Out.BarDirection != TEXT("left")
					&& Out.BarDirection != TEXT("up") && Out.BarDirection != TEXT("down"))
				{
					return Error(Path, TEXT("a bar's 'direction' is right, left, up or down"));
				}
				break;
			case EApexHudElementType::Divider:
				bOk = ReadBool(Json, TEXT("vertical"), Out.bVertical, Path);
				break;
			case EApexHudElementType::KeyCap:
				bOk = ReadProp(Json, TEXT("text"), EPropKind::Template, Out.Text, Path);
				if (bOk && Out.Text.IsDynamic())
				{
					return Error(Path, TEXT("a keycap's text is fixed"));
				}
				break;
			case EApexHudElementType::Image:
			{
				FString File;
				bOk = ReadString(Json, TEXT("file"), File, Path) && ReadProp(Json, TEXT("color"), EPropKind::Colour, Out.Colour, Path);
				if (bOk)
				{
					if (File.IsEmpty() || File.Contains(TEXT("..")))
					{
						return Error(Path, TEXT("an image needs a 'file' inside the component's folder"));
					}
					Out.ImageFile = FPaths::Combine(Folder, File);
					if (!FPaths::FileExists(Out.ImageFile))
					{
						Warn(Path, FString::Printf(TEXT("no file '%s'"), *Out.ImageFile));
					}
				}
				break;
			}
			case EApexHudElementType::Minimap:
				bOk = ReadProp(Json, TEXT("color"), EPropKind::Colour, Out.Colour, Path);
				break;
			default:
				break;
			}
			if (!bOk)
			{
				return false;
			}

			const TArray<TSharedPtr<FJsonValue>>* Children = nullptr;
			if (Json.TryGetArrayField(TEXT("children"), Children))
			{
				if (!HudIsContainer(Out.Type))
				{
					return Error(Path, FString::Printf(TEXT("a %s has no children"), Found->Name));
				}
				for (int32 Index = 0; Index < Children->Num(); ++Index)
				{
					const FString ChildPath = FString::Printf(TEXT("%s/children[%d]"), *Path, Index);
					const TSharedPtr<FJsonObject>* ChildJson = nullptr;
					if (!(*Children)[Index]->TryGetObject(ChildJson))
					{
						return Error(ChildPath, TEXT("a child must be an element { \"type\": ... }"));
					}
					FApexHudElementDef& Child = Out.Children.AddDefaulted_GetRef();
					if (!Parse(**ChildJson, ChildPath, Child))
					{
						return false;
					}
				}
			}
			return true;
		}
	};

	/** Walks an element tree checking every name its expressions read. */
	void HudCheckNames(const FApexHudElementDef& Element, FName ListContext, bool bInRepeat, const TSet<FName>& Scalars,
		const FString& ComponentId, TArray<FString>& OutWarnings)
	{
		if (Element.Repeat.IsSet())
		{
			bInRepeat = true;
			if (Element.Repeat.IsList())
			{
				ListContext = Element.Repeat.List;
			}
		}
		const FApexHudProp* Props[] = {&Element.Visible, &Element.Text, &Element.Colour, &Element.Background,
			&Element.Outline, &Element.Value, &Element.Bold, &Element.FillShare};
		for (const FApexHudProp* Prop : Props)
		{
			if (!Prop->IsSet())
			{
				continue;
			}
			TArray<FString> Names;
			Prop->Expr->CollectIdentifiers(Names);
			for (const FString& Name : Names)
			{
				FString Problem;
				if (Name == TEXT("index"))
				{
					if (!bInRepeat)
					{
						Problem = TEXT("'index' outside a repeat");
					}
				}
				else if (Name.StartsWith(TEXT("item.")))
				{
					const FName Field(*Name.Mid(5));
					if (ListContext.IsNone())
					{
						Problem = FString::Printf(TEXT("'%s' outside a repeat over a list"), *Name);
					}
					else if (!ApexHudData::ListFields()[ListContext].Contains(Field))
					{
						Problem = FString::Printf(TEXT("'%s': the %s list has no field '%s'"), *Name, *ListContext.ToString(), *Field.ToString());
					}
				}
				else if (!Scalars.Contains(FName(*Name)))
				{
					Problem = FString::Printf(TEXT("the game publishes no '%s' (apexsim.hud.Data lists every name)"), *Name);
				}
				if (!Problem.IsEmpty())
				{
					OutWarnings.Add(FString::Printf(TEXT("%s: %s: %s, in \"%s\""), *ComponentId, *Element.Path, *Problem, *Prop->Expr->GetSource()));
				}
			}
		}
		for (const FApexHudElementDef& Child : Element.Children)
		{
			HudCheckNames(Child, ListContext, bInRepeat, Scalars, ComponentId, OutWarnings);
		}
	}
}

const TArray<FString>& ApexHud::Regions()
{
	static const TArray<FString> Names = {TEXT("top-left"), TEXT("top"), TEXT("top-right"), TEXT("left"), TEXT("center"),
		TEXT("right"), TEXT("bottom-left"), TEXT("bottom"), TEXT("bottom-right")};
	return Names;
}

const TArray<FString>& ApexHud::Scenes()
{
	static const TArray<FString> Names = {TEXT("hotlap_watch")};
	return Names;
}

TArray<FString> ApexHud::HudDirectories()
{
	TArray<FString> Dirs;
	// In a packaged build ProjectDir is <Release>/Game/ApexSim/, so its parent
	// holds ApexSim.exe and `Hud/`. In the editor the HUD never leaves the repo.
	const FString Default = FPlatformProperties::RequiresCookedData()
		? FPaths::Combine(FPaths::ProjectDir(), TEXT(".."), TEXT("Hud"))
		: FPaths::Combine(FPaths::ProjectDir(), TEXT(".."), TEXT("content"), TEXT("hud"));
	Dirs.Add(FPaths::ConvertRelativePathToFull(Default));

	FString Override;
	if (FParse::Value(FCommandLine::Get(), TEXT("-ApexHudDir="), Override))
	{
		TArray<FString> Parts;
		Override.ParseIntoArray(Parts, TEXT("+"), true);
		for (const FString& Part : Parts)
		{
			Dirs.AddUnique(FPaths::ConvertRelativePathToFull(Part));
		}
	}
	return Dirs;
}

TArray<FString> ApexHud::ComponentFolders(const TArray<FString>& Directories)
{
	TArray<FString> Folders;
	for (const FString& Dir : Directories)
	{
		// The shipped components, then the player's own; a folder with neither
		// (an -ApexHudDir of loose components) holds the components itself.
		TArray<FString> Roots;
		for (const TCHAR* Sub : {TEXT("default"), TEXT("custom")})
		{
			const FString Root = FPaths::Combine(Dir, Sub);
			if (IFileManager::Get().DirectoryExists(*Root))
			{
				Roots.Add(Root);
			}
		}
		if (Roots.IsEmpty())
		{
			Roots.Add(Dir);
		}
		for (const FString& Root : Roots)
		{
			TArray<FString> Names;
			IFileManager::Get().FindFiles(Names, *FPaths::Combine(Root, TEXT("*")), false, true);
			// Sorted, so components with the same order sit the same way on every machine.
			Names.Sort();
			for (const FString& Name : Names)
			{
				const FString Folder = FPaths::Combine(Root, Name);
				if (FPaths::FileExists(FPaths::Combine(Folder, HudComponentFile)))
				{
					Folders.Add(Folder);
				}
			}
		}
	}
	return Folders;
}

FString ApexHud::StripJsonExtras(const FString& Text)
{
	// Comments first, outside strings only.
	FString NoComments;
	NoComments.Reserve(Text.Len());
	const int32 N = Text.Len();
	bool bInString = false;
	for (int32 I = 0; I < N; ++I)
	{
		const TCHAR C = Text[I];
		if (bInString)
		{
			NoComments.AppendChar(C);
			if (C == '\\' && I + 1 < N)
			{
				NoComments.AppendChar(Text[++I]);
			}
			else if (C == '"')
			{
				bInString = false;
			}
			continue;
		}
		if (C == '"')
		{
			bInString = true;
			NoComments.AppendChar(C);
		}
		else if (C == '/' && I + 1 < N && Text[I + 1] == '/')
		{
			while (I < N && Text[I] != '\n')
			{
				++I;
			}
			NoComments.AppendChar('\n');
		}
		else if (C == '/' && I + 1 < N && Text[I + 1] == '*')
		{
			I += 2;
			while (I + 1 < N && !(Text[I] == '*' && Text[I + 1] == '/'))
			{
				// Keep the line count, so the parser's line numbers still match the file.
				if (Text[I] == '\n')
				{
					NoComments.AppendChar('\n');
				}
				++I;
			}
			++I;
		}
		else
		{
			NoComments.AppendChar(C);
		}
	}

	// Then a comma with nothing but white space before a closing bracket.
	FString Out;
	Out.Reserve(NoComments.Len());
	bInString = false;
	const int32 M = NoComments.Len();
	for (int32 I = 0; I < M; ++I)
	{
		const TCHAR C = NoComments[I];
		if (bInString)
		{
			Out.AppendChar(C);
			if (C == '\\' && I + 1 < M)
			{
				Out.AppendChar(NoComments[++I]);
			}
			else if (C == '"')
			{
				bInString = false;
			}
			continue;
		}
		if (C == '"')
		{
			bInString = true;
		}
		else if (C == ',')
		{
			int32 Next = I + 1;
			while (Next < M && FChar::IsWhitespace(NoComments[Next]))
			{
				++Next;
			}
			if (Next < M && (NoComments[Next] == ']' || NoComments[Next] == '}'))
			{
				continue;
			}
		}
		Out.AppendChar(C);
	}
	return Out;
}

bool ApexHud::ParseComponent(const FString& Text, const FString& Folder, FApexHudComponentDef& Out, FApexHudLoadReport& Report)
{
	Out.Folder = Folder;
	Out.Id = FPaths::GetCleanFilename(Folder);

	TSharedPtr<FJsonObject> Json;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(StripJsonExtras(Text));
	if (!FJsonSerializer::Deserialize(Reader, Json) || !Json.IsValid())
	{
		Report.Errors.Add(FString::Printf(TEXT("%s: %s is not valid JSON: %s"), *Out.Id, HudComponentFile, *Reader->GetErrorMessage()));
		return false;
	}

	FHudElementParser Parser{Folder, Out.Id, Report};
	const FString Top = TEXT("component");
	static const TArray<FString> KnownTop = {TEXT("name"), TEXT("description"), TEXT("enabled"), TEXT("default_enabled"), TEXT("region"),
		TEXT("order"), TEXT("margin"), TEXT("float"), TEXT("visible"), TEXT("scene"), TEXT("root")};
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Json->Values)
	{
		if (!KnownTop.Contains(Field.Key))
		{
			Parser.Warn(Top, FString::Printf(TEXT("'%s' is not read by a component"), *Field.Key));
		}
	}

	float Order = 0.0f;
	if (!Parser.ReadString(*Json, TEXT("name"), Out.Name, Top)
		|| !Parser.ReadString(*Json, TEXT("description"), Out.Description, Top)
		|| !Parser.ReadBool(*Json, TEXT("enabled"), Out.bEnabled, Top)
		|| !Parser.ReadBool(*Json, TEXT("default_enabled"), Out.bDefaultEnabled, Top))
	{
		return false;
	}
	if (!Out.bEnabled)
	{
		// A disabled component only has to say so: it is how a custom folder hides a shipped one.
		return true;
	}
	if (!Parser.ReadString(*Json, TEXT("region"), Out.Region, Top)
		|| !Parser.ReadNumber(*Json, TEXT("order"), Order, Top)
		|| !Parser.ReadMargin(*Json, TEXT("margin"), Out.Margin, Top)
		|| !Parser.ReadBool(*Json, TEXT("float"), Out.bFloat, Top)
		|| !Parser.ReadString(*Json, TEXT("scene"), Out.Scene, Top)
		|| !Parser.ReadProp(*Json, TEXT("visible"), FHudElementParser::EPropKind::Bool, Out.Visible, Top))
	{
		return false;
	}
	Out.Order = FMath::RoundToInt(Order);
	if (!Out.Scene.IsEmpty() && !Scenes().Contains(Out.Scene))
	{
		return Parser.Error(Top, FString::Printf(TEXT("'scene' is one of %s"), *FString::Join(Scenes(), TEXT(", "))));
	}
	if (!Regions().Contains(Out.Region))
	{
		return Parser.Error(Top, FString::Printf(TEXT("'region' is one of %s"), *FString::Join(Regions(), TEXT(", "))));
	}

	const TSharedPtr<FJsonObject>* Root = nullptr;
	if (!Json->TryGetObjectField(TEXT("root"), Root))
	{
		return Parser.Error(Top, TEXT("a component needs a \"root\" element"));
	}
	if (!Parser.Parse(**Root, TEXT("root"), Out.Root))
	{
		return false;
	}
	if (Out.Root.Repeat.IsSet())
	{
		return Parser.Error(TEXT("root"), TEXT("the root cannot repeat: put it in a row or column"));
	}
	return true;
}

void ApexHud::LoadComponents(const TArray<FString>& Directories, TArray<FApexHudComponentDef>& Out, FApexHudLoadReport& Report)
{
	TMap<FString, int32> ById;
	TArray<FApexHudComponentDef> All;
	for (const FString& Folder : ComponentFolders(Directories))
	{
		FString Text;
		const FString File = FPaths::Combine(Folder, HudComponentFile);
		if (!FFileHelper::LoadFileToString(Text, *File))
		{
			Report.Errors.Add(FString::Printf(TEXT("%s: cannot read"), *File));
			continue;
		}
		FApexHudComponentDef Component;
		const int32 ErrorsBefore = Report.Errors.Num();
		const bool bParsed = ParseComponent(Text, Folder, Component, Report);
		if (!bParsed)
		{
			if (Report.Errors.Num() == ErrorsBefore)
			{
				Report.Errors.Add(FString::Printf(TEXT("%s: cannot be drawn"), *Component.Id));
			}
			continue;
		}
		// A later folder replaces an earlier one with the same name.
		const FString Id = Component.Id;
		if (const int32* Existing = ById.Find(Id))
		{
			All[*Existing] = MoveTemp(Component);
		}
		else
		{
			const int32 Added = All.Add(MoveTemp(Component));
			ById.Add(Id, Added);
		}
	}
	for (FApexHudComponentDef& Component : All)
	{
		if (Component.bEnabled)
		{
			CheckDataNames(Component, Report.Warnings);
			Out.Add(MoveTemp(Component));
		}
	}
}

void ApexHud::CheckDataNames(const FApexHudComponentDef& Component, TArray<FString>& OutWarnings)
{
	static const TSet<FName> Scalars(ApexHudData::ScalarNames());
	if (Component.Visible.IsSet())
	{
		FApexHudElementDef Holder;
		Holder.Path = TEXT("component");
		Holder.Visible = Component.Visible;
		HudCheckNames(Holder, NAME_None, false, Scalars, Component.Id, OutWarnings);
	}
	HudCheckNames(Component.Root, NAME_None, false, Scalars, Component.Id, OutWarnings);
}
