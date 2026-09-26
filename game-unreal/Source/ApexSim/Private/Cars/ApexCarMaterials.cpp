#include "Cars/ApexCarMaterials.h"

#include "Cars/ApexGlbReader.h"
#include "Materials/MaterialInterface.h"
#include "Misc/PackageName.h"

UMaterialInterface* FApexCarParents::For(const FApexGlbMaterial& Material) const
{
	UMaterialInterface* Wanted = nullptr;
	switch (Material.Alpha)
	{
	case EApexGlbAlpha::Blend: Wanted = Translucent; break;
	case EApexGlbAlpha::Mask: Wanted = Masked; break;
	default: Wanted = Material.ClearCoat > 0.0f ? ClearCoat : Opaque; break;
	}
	return Wanted ? Wanted : Opaque.Get();
}

FString ApexCarMaterials::PackageName(const TCHAR* Name)
{
	return FString::Printf(TEXT("%s/%s"), Folder, Name);
}

FString ApexCarMaterials::ObjectPath(const TCHAR* Name)
{
	return FString::Printf(TEXT("%s/%s.%s"), Folder, Name, Name);
}

FApexCarParents ApexCarMaterials::LoadParents()
{
	auto Load = [](const TCHAR* Name) -> UMaterialInterface* {
		// Tested first: a fresh clone has none, which is logged by the caller, not here.
		if (!FPackageName::DoesPackageExist(PackageName(Name)))
		{
			return nullptr;
		}
		return LoadObject<UMaterialInterface>(nullptr, *ObjectPath(Name));
	};
	FApexCarParents Parents;
	Parents.Opaque = Load(OpaqueName);
	Parents.ClearCoat = Load(ClearCoatName);
	Parents.Masked = Load(MaskedName);
	Parents.Translucent = Load(TranslucentName);
	return Parents;
}
