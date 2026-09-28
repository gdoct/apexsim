#include "Race/ApexCarLivery.h"

#include "Cars/ApexCarContentSubsystem.h"
#include "Catalog/ApexCatalogRows.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Materials/MaterialInstanceDynamic.h"

namespace
{
	const FName PaintSlot(TEXT("car_paint"));
	const FName AccentSlot(TEXT("car_accent"));
	const FName LogoSlot(TEXT("car_logo"));

	/**
	 * The component tag that records a slot a texture livery swapped, so the
	 * next livery (or livery 0) knows to put it back: a skin may name any slot
	 * (`EXT_RIM`), and nothing else remembers which.
	 */
	const TCHAR* TexturedTagPrefix = TEXT("ApexLiveryTextured:");

	// The car parents' parameters (ApexCarMaterials.h), which keep the names
	// Interchange's glTF parents gave them.
	const FName BaseColorFactorParam(TEXT("BaseColorFactor"));
	const FName MetallicFactorParam(TEXT("MetallicFactor"));
	const FName BaseColorTextureParam(TEXT("BaseColorTexture"));

	/** A fresh dynamic instance of the slot's authored material, or null when the body has no such slot. */
	UMaterialInstanceDynamic* Instance(UStaticMeshComponent& Mesh, FName Slot)
	{
		const int32 Index = Mesh.GetMaterialIndex(Slot);
		if (Index == INDEX_NONE)
		{
			return nullptr;
		}
		// From the mesh's own material, never from a previous livery's
		// instance, and never the runtime body's shared instance itself.
		return ApexCarContent::OwnMaterialInstance(Mesh, Index, /*bFromMesh*/ true);
	}

	void Restore(UStaticMeshComponent& Mesh, FName Slot)
	{
		const int32 Index = Mesh.GetMaterialIndex(Slot);
		if (Index != INDEX_NONE)
		{
			Mesh.SetMaterial(Index, nullptr);
		}
	}

	/** Every slot an earlier texture livery swapped, back to the model as authored. */
	void RestoreTextured(UStaticMeshComponent& Mesh)
	{
		for (int32 i = Mesh.ComponentTags.Num() - 1; i >= 0; --i)
		{
			const FString Tag = Mesh.ComponentTags[i].ToString();
			if (Tag.StartsWith(TexturedTagPrefix))
			{
				Restore(Mesh, FName(*Tag.RightChop(FCString::Strlen(TexturedTagPrefix))));
				Mesh.ComponentTags.RemoveAt(i);
			}
		}
	}
}

namespace ApexLivery
{
	const FApexCarLivery* Find(const FApexCarCatalogRow& Row, int32 Livery)
	{
		return Livery >= 1 && Livery <= Row.Liveries.Num() ? &Row.Liveries[Livery - 1] : nullptr;
	}

	bool IsSkinSlot(FName Slot)
	{
		const FString Name = Slot.ToString();
		return Name.Equals(TEXT("car_skin"), ESearchCase::IgnoreCase) || Name.StartsWith(TEXT("car_skin_"), ESearchCase::IgnoreCase);
	}

	void Apply(UStaticMeshComponent* Mesh, const FApexCarLivery* Livery)
	{
		if (!Mesh || !Mesh->GetStaticMesh())
		{
			return;
		}
		// Whatever the new livery is, a slot the last one textured starts from
		// the model again; the new one re-textures what it wants below.
		RestoreTextured(*Mesh);
		if (!Livery)
		{
			for (const FName Slot : {PaintSlot, AccentSlot, LogoSlot})
			{
				Restore(*Mesh, Slot);
			}
			return;
		}

		// One instance per slot for the whole livery, so a texture override on
		// `car_paint` lands on the instance that took the paint colour rather
		// than replacing it with a fresh one.
		TMap<FName, UMaterialInstanceDynamic*> Made;
		auto Get = [Mesh, &Made](FName Slot) -> UMaterialInstanceDynamic*
		{
			if (UMaterialInstanceDynamic** Found = Made.Find(Slot))
			{
				return *Found;
			}
			UMaterialInstanceDynamic* Mid = Instance(*Mesh, Slot);
			Made.Add(Slot, Mid);
			return Mid;
		};

		// A skin-only livery names no paint: `car_paint` keeps its colour
		// (and, with no metallic either, its material) rather than turning black.
		if (!Livery->HasPaint() && Livery->PaintMetallic < 0.0f)
		{
			Restore(*Mesh, PaintSlot);
		}
		else if (UMaterialInstanceDynamic* Paint = Get(PaintSlot))
		{
			if (Livery->HasPaint())
			{
				FLinearColor Colour = Livery->Paint;
				Colour.A = 1.0f;
				Paint->SetVectorParameterValue(BaseColorFactorParam, Colour);
			}
			if (Livery->PaintMetallic >= 0.0f)
			{
				Paint->SetScalarParameterValue(MetallicFactorParam, Livery->PaintMetallic);
			}
		}
		if (Livery->Accent.A <= 0.0f)
		{
			Restore(*Mesh, AccentSlot);
		}
		else if (UMaterialInstanceDynamic* Accent = Get(AccentSlot))
		{
			FLinearColor Colour = Livery->Accent;
			Colour.A = 1.0f;
			Accent->SetVectorParameterValue(BaseColorFactorParam, Colour);
		}
		if (UTexture2D* Logo = ApexCarContent::LoadLogo(*Livery))
		{
			if (UMaterialInstanceDynamic* LogoMid = Get(LogoSlot))
			{
				LogoMid->SetTextureParameterValue(BaseColorTextureParam, Logo);
			}
		}
		else
		{
			Restore(*Mesh, LogoSlot);
		}

		// Texture liveries: the skin on every paint slot the importer named
		// `car_skin*`, then the slots the livery names one by one. Only the
		// texture changes; the slot keeps its authored factor, roughness and
		// clear coat, which is what an AC skin swap does too.
		auto Texture = [Mesh, &Get](FName Slot, UTexture2D* Loaded)
		{
			if (UMaterialInstanceDynamic* Mid = Loaded ? Get(Slot) : nullptr)
			{
				Mid->SetTextureParameterValue(BaseColorTextureParam, Loaded);
				Mesh->ComponentTags.AddUnique(FName(*(FString(TexturedTagPrefix) + Slot.ToString())));
			}
		};
		if (UTexture2D* Skin = ApexCarContent::LoadLiveryTexture(Livery->RuntimeSkin))
		{
			for (const FName Slot : Mesh->GetMaterialSlotNames())
			{
				if (IsSkinSlot(Slot))
				{
					Texture(Slot, Skin);
				}
			}
		}
		for (const FApexLiveryTexture& Entry : Livery->RuntimeTextures)
		{
			if (Mesh->GetMaterialIndex(Entry.Slot) != INDEX_NONE)
			{
				Texture(Entry.Slot, ApexCarContent::LoadLiveryTexture(Entry.RuntimeTexture));
			}
		}
	}

	FString DisplayName(const FApexCarCatalogRow& Row, int32 Livery)
	{
		const FApexCarLivery* Found = Find(Row, Livery);
		return Found && !Found->Name.IsEmpty() ? Found->Name : FString(TEXT("Works"));
	}
}
