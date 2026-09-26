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
}

namespace ApexLivery
{
	const FApexCarLivery* Find(const FApexCarCatalogRow& Row, int32 Livery)
	{
		return Livery >= 1 && Livery <= Row.Liveries.Num() ? &Row.Liveries[Livery - 1] : nullptr;
	}

	void Apply(UStaticMeshComponent* Mesh, const FApexCarLivery* Livery)
	{
		if (!Mesh || !Mesh->GetStaticMesh())
		{
			return;
		}
		if (!Livery)
		{
			for (const FName Slot : {PaintSlot, AccentSlot, LogoSlot})
			{
				Restore(*Mesh, Slot);
			}
			return;
		}
		if (UMaterialInstanceDynamic* Paint = Instance(*Mesh, PaintSlot))
		{
			FLinearColor Colour = Livery->Paint;
			Colour.A = 1.0f;
			Paint->SetVectorParameterValue(BaseColorFactorParam, Colour);
			if (Livery->PaintMetallic >= 0.0f)
			{
				Paint->SetScalarParameterValue(MetallicFactorParam, Livery->PaintMetallic);
			}
		}
		if (Livery->Accent.A <= 0.0f)
		{
			Restore(*Mesh, AccentSlot);
		}
		else if (UMaterialInstanceDynamic* Accent = Instance(*Mesh, AccentSlot))
		{
			FLinearColor Colour = Livery->Accent;
			Colour.A = 1.0f;
			Accent->SetVectorParameterValue(BaseColorFactorParam, Colour);
		}
		if (UTexture2D* Logo = ApexCarContent::LoadLogo(*Livery))
		{
			if (UMaterialInstanceDynamic* LogoMid = Instance(*Mesh, LogoSlot))
			{
				LogoMid->SetTextureParameterValue(BaseColorTextureParam, Logo);
			}
		}
		else
		{
			Restore(*Mesh, LogoSlot);
		}
	}

	FString DisplayName(const FApexCarCatalogRow& Row, int32 Livery)
	{
		const FApexCarLivery* Found = Find(Row, Livery);
		return Found && !Found->Name.IsEmpty() ? Found->Name : FString(TEXT("Works"));
	}
}
