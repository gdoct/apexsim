#pragma once

#include "CoreMinimal.h"

class IImageWrapperModule;

/** How a glTF material covers what is behind it (`alphaMode`). */
enum class EApexGlbAlpha : uint8
{
	Opaque,
	Mask,
	Blend,
};

/**
 * One glTF material, reduced to what the car parents take
 * (`ApexCarMaterials`): metallic-roughness factors, a base colour texture,
 * emission, alpha and clear coat. Colours are linear, as glTF stores them.
 */
struct FApexGlbMaterial
{
	/** The material slot's name: the glTF name, made unique within the model. */
	FString Name;
	FLinearColor BaseColor = FLinearColor::White;
	/** Index into `FApexGlbModel::Images`, or INDEX_NONE. */
	int32 BaseColorImage = INDEX_NONE;
	float Metallic = 1.0f;
	float Roughness = 1.0f;
	FLinearColor Emissive = FLinearColor::Black;
	/** `KHR_materials_emissive_strength`; kept for the log, not drawn (see ApexCarMaterials). */
	float EmissiveStrength = 1.0f;
	EApexGlbAlpha Alpha = EApexGlbAlpha::Opaque;
	float AlphaCutoff = 0.5f;
	/** `KHR_materials_clearcoat`. */
	float ClearCoat = 0.0f;
	float ClearCoatRoughness = 0.0f;
	bool bDoubleSided = false;
};

/** One embedded image: the file's bytes, and once decoded, BGRA8 mips. */
struct FApexGlbImage
{
	FString Name;
	/** PNG or JPEG, as stored in the GLB. */
	TArray<uint8> Encoded;
	/** Whether a material samples it; only those are decoded. */
	bool bUsed = false;
	int32 Width = 0;
	int32 Height = 0;
	/** BGRA8, largest first; empty until `ApexGlb::DecodeImages`. */
	TArray<TArray<uint8>> Mips;
};

/** The triangles of one material, as indices into the model's vertices. */
struct FApexGlbSection
{
	int32 Material = INDEX_NONE;
	TArray<uint32> Indices;
};

/**
 * A GLB flattened into one mesh the way Interchange's "combine all" import
 * made it: every node's primitives in the model's own frame, one section
 * per material (materials without triangles have none), already in
 * Unreal's frame — glTF `(x, y, z)` metres lands as `(x, z, y)` centimetres,
 * which is where Interchange put it, and the triangles keep glTF's order,
 * which that axis swap turns into Unreal's front face.
 */
struct FApexGlbModel
{
	TArray<FVector3f> Positions;
	TArray<FVector3f> Normals;
	TArray<FVector2f> UVs;
	TArray<FApexGlbSection> Sections;
	TArray<FApexGlbMaterial> Materials;
	TArray<FApexGlbImage> Images;
	FBox3f Bounds = FBox3f(ForceInit);

	int32 NumTriangles() const
	{
		int32 Count = 0;
		for (const FApexGlbSection& Section : Sections)
		{
			Count += Section.Indices.Num() / 3;
		}
		return Count;
	}
};

/**
 * The game's own glTF 2.0 reader, for the cars it builds at runtime
 * (`UApexCarContentSubsystem`). It reads the subset the project's car and
 * wheel GLBs use and refuses what it cannot draw:
 *
 *  - `.glb` (one BIN chunk) or `.gltf` with its buffers beside it or as data URIs;
 *  - the default scene's node tree, `matrix` or TRS, mirrored nodes rewound;
 *  - triangle lists (mode 4) with `POSITION`, `NORMAL` (made smooth when
 *    absent) and `TEXCOORD_0`, float or normalised integers, u8/u16/u32
 *    indices or none;
 *  - metallic-roughness materials with a base colour texture, emission,
 *    `alphaMode`, `KHR_materials_clearcoat`; other textures are ignored;
 *  - PNG or JPEG images, embedded or beside the file.
 *
 * Anything in `extensionsRequired` (Draco, meshopt, quantisation) and sparse
 * accessors are refused. Pure data: `Parse` runs on any thread, and so does
 * `DecodeImages` given a module loaded on the game thread.
 */
namespace ApexGlb
{
	/** Parse a GLB or glTF held in memory; `BaseDir` resolves external buffers and images. */
	APEXSIM_API bool Parse(TArrayView<const uint8> Bytes, const FString& BaseDir, FApexGlbModel& Out, FString& OutError);

	/** Read and parse a file. */
	APEXSIM_API bool ReadFile(const FString& Path, FApexGlbModel& Out, FString& OutError);

	/**
	 * Decode every image a material uses into BGRA8 mips. `ImageWrappers`
	 * must have been loaded on the game thread
	 * (`FModuleManager::LoadModuleChecked<IImageWrapperModule>("ImageWrapper")`).
	 */
	APEXSIM_API bool DecodeImages(FApexGlbModel& Model, IImageWrapperModule& ImageWrappers, FString& OutError);

	/** One image's `Encoded` bytes into BGRA8 mips; the livery logos, which are loose PNGs, come through here too. */
	APEXSIM_API bool DecodeImage(FApexGlbImage& Image, IImageWrapperModule& ImageWrappers);

	/**
	 * A 2x2 box-filtered chain below a BGRA8 image, down to 1x1; only for
	 * power-of-two sizes (anything else keeps the one level). Appends to `Mips`,
	 * whose first entry is the full image.
	 */
	APEXSIM_API void BuildMips(int32 Width, int32 Height, TArray<TArray<uint8>>& Mips);

	/** glTF metres (right-handed, +Y up) -> Unreal centimetres (left-handed, +Z up). */
	inline FVector3f ToUnrealPosition(const FVector& Gltf)
	{
		return FVector3f(static_cast<float>(Gltf.X * 100.0), static_cast<float>(Gltf.Z * 100.0), static_cast<float>(Gltf.Y * 100.0));
	}

	/** The same for a direction (normals): no scale. */
	inline FVector3f ToUnrealDirection(const FVector& Gltf)
	{
		return FVector3f(static_cast<float>(Gltf.X), static_cast<float>(Gltf.Z), static_cast<float>(Gltf.Y));
	}

	/**
	 * A model's triangles dealt out by box, for the bodywork that comes off
	 * in a crash (FApexDamagePartSpec): `OutPieces[0]` holds every triangle
	 * whose centre lies in no box, `OutPieces[i + 1]` those whose centre lies
	 * in `Boxes[i]` (the first box that holds it). Each piece keeps only the
	 * vertices its triangles use and the sections that kept a triangle, with
	 * the materials copied whole; images are not copied, so a material's
	 * `BaseColorImage` still indexes the source's. A piece with no triangle
	 * has no section.
	 */
	APEXSIM_API void SplitByBoxes(const FApexGlbModel& Model, TArrayView<const FBox3f> Boxes, TArray<FApexGlbModel>& OutPieces);
}
