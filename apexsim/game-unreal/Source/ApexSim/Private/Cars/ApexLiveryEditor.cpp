#include "Cars/ApexLiveryEditor.h"

#include "ApexSim.h"
#include "CanvasItem.h"
#include "CanvasTypes.h"
#include "Engine/Canvas.h"
#include "Engine/TextureRenderTarget2D.h"
#include "HAL/FileManager.h"
#include "ImageUtils.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Race/ApexCarLivery.h"
#include "RenderingThread.h"
#include "TextureResource.h"
#include "UI/ApexUIStyle.h"

namespace ApexLiveryEditorPrivate
{
	// Racing colours, invented names; sRGB as a picker shows them.
	const ApexLiveryEditor::FSwatch SwatchTable[] = {
		{TEXT("Racing red"), FColor(200, 16, 32)},
		{TEXT("Snow white"), FColor(240, 240, 236)},
		{TEXT("Graphite"), FColor(44, 46, 50)},
		{TEXT("Midnight blue"), FColor(16, 32, 92)},
		{TEXT("Sky blue"), FColor(64, 156, 224)},
		{TEXT("Forest green"), FColor(18, 84, 46)},
		{TEXT("Lime"), FColor(150, 210, 30)},
		{TEXT("Sun yellow"), FColor(250, 204, 20)},
		{TEXT("Papaya"), FColor(255, 128, 0)},
		{TEXT("Magenta"), FColor(214, 30, 128)},
		{TEXT("Violet"), FColor(96, 48, 168)},
		{TEXT("Silver"), FColor(176, 180, 186)},
		{TEXT("Gold"), FColor(196, 156, 72)},
		{TEXT("Teal"), FColor(0, 128, 128)},
	};

	/** The plate on the logo: a rounded box, as liveries.py draws its plates. */
	constexpr float PlateLeft = 168.0f;
	constexpr float PlateTop = 36.0f;
	constexpr float PlateRight = ApexLiveryEditor::LogoWidth - 168.0f;
	constexpr float PlateBottom = ApexLiveryEditor::LogoHeight - 36.0f;
	constexpr float PlateRadius = 80.0f;

	/** How far inside the plate a pixel centre is, px (negative outside): a rounded box's distance. */
	float PlateInside(float X, float Y)
	{
		const float Cx = (PlateLeft + PlateRight) * 0.5f;
		const float Cy = (PlateTop + PlateBottom) * 0.5f;
		const float Hx = (PlateRight - PlateLeft) * 0.5f - PlateRadius;
		const float Hy = (PlateBottom - PlateTop) * 0.5f - PlateRadius;
		const float Dx = FMath::Abs(X - Cx) - Hx;
		const float Dy = FMath::Abs(Y - Cy) - Hy;
		const float Outside = FVector2f(FMath::Max(Dx, 0.0f), FMath::Max(Dy, 0.0f)).Size() + FMath::Min(FMath::Max(Dx, Dy), 0.0f);
		return PlateRadius - Outside;
	}
}

namespace ApexLiveryEditor
{
	using namespace ApexLiveryEditorPrivate;

	TConstArrayView<FSwatch> Swatches()
	{
		return MakeArrayView(SwatchTable, UE_ARRAY_COUNT(SwatchTable));
	}

	FLinearColor Linear(int32 Swatch)
	{
		const TConstArrayView<FSwatch> All = Swatches();
		const FColor Srgb = All[((Swatch % All.Num()) + All.Num()) % All.Num()].Srgb;
		return FLinearColor::FromSRGBColor(Srgb);
	}

	FString AutoName(const FApexLiveryDraft& Draft)
	{
		const TConstArrayView<FSwatch> All = Swatches();
		const FString Colour = All[((Draft.Paint % All.Num()) + All.Num()) % All.Num()].Name;
		return Draft.Number >= 0 ? FString::Printf(TEXT("%s %d"), *Colour, Draft.Number) : Colour;
	}

	FString CleanName(const FString& Name)
	{
		FString Out;
		for (const TCHAR Char : Name)
		{
			if (Char >= 0x20 && Char != 0x7F && Char != TEXT('"') && Char != TEXT('\\'))
			{
				Out.AppendChar(Char);
			}
		}
		return Out.TrimStartAndEnd().Left(40).TrimEnd();
	}

	FString FolderFor(const FString& Name, const FString& Root)
	{
		FString Stem;
		bool bDash = false;
		for (const TCHAR Char : CleanName(Name).ToLower())
		{
			if ((Char >= TEXT('a') && Char <= TEXT('z')) || (Char >= TEXT('0') && Char <= TEXT('9')))
			{
				if (bDash && !Stem.IsEmpty())
				{
					Stem.AppendChar(TEXT('-'));
				}
				Stem.AppendChar(Char);
				bDash = false;
			}
			else
			{
				bDash = true;
			}
		}
		// `template/` is the template tool's.
		if (Stem.IsEmpty() || Stem == TEXT("template"))
		{
			Stem = TEXT("livery");
		}
		FString Folder = Stem;
		for (int32 Suffix = 2; IFileManager::Get().DirectoryExists(*FPaths::Combine(Root, Folder)); ++Suffix)
		{
			Folder = FString::Printf(TEXT("%s-%d"), *Stem, Suffix);
		}
		return Folder;
	}

	FString LiveryToml(const FApexLiveryDraft& Draft, bool bLogo)
	{
		auto Rgb = [](const FLinearColor& C) { return FString::Printf(TEXT("[%.3f, %.3f, %.3f]"), C.R, C.G, C.B); };
		const FString Name = CleanName(Draft.Name).IsEmpty() ? AutoName(Draft) : CleanName(Draft.Name);
		FString Text = TEXT("# Made in the garage's livery editor.\n[[livery]]\n");
		Text += FString::Printf(TEXT("name = \"%s\"\n"), *Name);
		Text += FString::Printf(TEXT("paint = %s\n"), *Rgb(Linear(Draft.Paint)));
		Text += FString::Printf(TEXT("accent = %s\n"), *Rgb(Linear(Draft.Accent)));
		if (bLogo)
		{
			Text += TEXT("logo = \"logo.png\"\n");
		}
		return Text;
	}

	bool Save(const FString& CarDir, const FApexLiveryDraft& Draft, TArrayView<const uint8> LogoPng, FString& OutFolder,
		FString& OutError)
	{
		if (CarDir.IsEmpty())
		{
			OutError = TEXT("the car has no folder on this machine");
			return false;
		}
		const FString Root = ApexLivery::OwnLiveryRoot(CarDir);
		const FString Name = CleanName(Draft.Name).IsEmpty() ? AutoName(Draft) : CleanName(Draft.Name);
		OutFolder = FPaths::Combine(Root, FolderFor(Name, Root));
		if (!IFileManager::Get().MakeDirectory(*OutFolder, /*Tree*/ true))
		{
			OutError = FString::Printf(TEXT("%s could not be made"), *OutFolder);
			return false;
		}
		const bool bLogo = LogoPng.Num() > 0;
		if (bLogo && !FFileHelper::SaveArrayToFile(LogoPng, *FPaths::Combine(OutFolder, TEXT("logo.png"))))
		{
			OutError = TEXT("the logo could not be written");
			return false;
		}
		FApexLiveryDraft Named = Draft;
		Named.Name = Name;
		if (!FFileHelper::SaveStringToFile(LiveryToml(Named, bLogo), *FPaths::Combine(OutFolder, TEXT("livery.toml")),
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			OutError = TEXT("livery.toml could not be written");
			return false;
		}
		return true;
	}

	TArray<FColor> ComposeLogo(TArrayView<const uint8> Coverage, const FLinearColor& Plate)
	{
		const int32 W = LogoWidth;
		const int32 H = LogoHeight;
		// The number in black on a light plate, white on a dark one.
		const float Luminance = 0.2126f * Plate.R + 0.7152f * Plate.G + 0.0722f * Plate.B;
		const FLinearColor Ink = Luminance > 0.18f ? FLinearColor(0.01f, 0.01f, 0.01f) : FLinearColor(0.95f, 0.95f, 0.95f);
		TArray<FColor> Pixels;
		Pixels.SetNumUninitialized(W * H);
		for (int32 Y = 0; Y < H; ++Y)
		{
			for (int32 X = 0; X < W; ++X)
			{
				const int32 At = Y * W + X;
				const float Inside = FMath::Clamp(PlateInside(X + 0.5f, Y + 0.5f) + 0.5f, 0.0f, 1.0f);
				const float Glyph = Coverage.IsValidIndex(At) ? Coverage[At] / 255.0f : 0.0f;
				FLinearColor Colour = FMath::Lerp(Plate, Ink, Glyph * Inside);
				Colour.A = Inside;
				Pixels[At] = Colour.ToFColor(/*bSRGB*/ true);
			}
		}
		return Pixels;
	}

	bool RenderNumberLogo(int32 Number, const FLinearColor& Plate, TArray<uint8>& OutPng)
	{
		OutPng.Reset();
		if (Number < 0 || !FApp::CanEverRender() || !GIsRHIInitialized)
		{
			return false;
		}
		UTextureRenderTarget2D* Target = NewObject<UTextureRenderTarget2D>(GetTransientPackage());
		Target->RenderTargetFormat = RTF_RGBA8;
		Target->ClearColor = FLinearColor::Black;
		Target->bAutoGenerateMips = false;
		Target->InitAutoFormat(LogoWidth, LogoHeight);
		Target->UpdateResourceImmediate(true);
		FTextureRenderTargetResource* Resource = Target->GameThread_GetRenderTargetResource();
		if (!Resource)
		{
			return false;
		}
		{
			// White glyphs on black: the coverage the plate is composed with.
			FCanvas Canvas(Resource, nullptr, FGameTime(), GMaxRHIFeatureLevel);
			Canvas.Clear(FLinearColor::Black);
			FCanvasTextItem Text(FVector2D(LogoWidth * 0.5f, LogoHeight * 0.5f), FText::AsNumber(Number, &FNumberFormattingOptions::DefaultNoGrouping()),
				ApexUI::Font::Display(250.0f), FLinearColor::White);
			Text.bCentreX = true;
			Text.bCentreY = true;
			Canvas.DrawItem(Text);
			Canvas.Flush_GameThread(true);
		}
		FlushRenderingCommands();
		TArray<FColor> Glyphs;
		if (!Resource->ReadPixels(Glyphs) || Glyphs.Num() != LogoWidth * LogoHeight)
		{
			return false;
		}
		TArray<uint8> Coverage;
		Coverage.SetNumUninitialized(Glyphs.Num());
		for (int32 i = 0; i < Glyphs.Num(); ++i)
		{
			Coverage[i] = Glyphs[i].R;
		}
		const TArray<FColor> Pixels = ComposeLogo(Coverage, Plate);
		TArray64<uint8> Png;
		FImageUtils::PNGCompressImageArray(LogoWidth, LogoHeight, TArrayView64<const FColor>(Pixels.GetData(), Pixels.Num()), Png);
		OutPng.Append(Png.GetData(), static_cast<int32>(Png.Num()));
		Target->MarkAsGarbage();
		return OutPng.Num() > 0;
	}
}
