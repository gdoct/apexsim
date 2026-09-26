#include "Cars/ApexGlbReader.h"

#include "Dom/JsonObject.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	using FJsonArray = TArray<TSharedPtr<FJsonValue>>;

	constexpr uint32 GlbMagic = 0x46546C67;	  // "glTF"
	constexpr uint32 ChunkJson = 0x4E4F534A;  // "JSON"
	constexpr uint32 ChunkBin = 0x004E4942;	  // "BIN\0"

	constexpr int32 ComponentByte = 5120;
	constexpr int32 ComponentUnsignedByte = 5121;
	constexpr int32 ComponentShort = 5122;
	constexpr int32 ComponentUnsignedShort = 5123;
	constexpr int32 ComponentUnsignedInt = 5125;
	constexpr int32 ComponentFloat = 5126;

	/** Nothing a car needs comes near these; they only stop a damaged file running away. */
	constexpr int32 MaxAccessorCount = 64 * 1024 * 1024;
	constexpr int32 MaxNodeDepth = 64;
	constexpr int32 MaxNodeVisits = 100000;
	constexpr int64 MaxImageSide = 16384;

	uint32 GlbU32(const uint8* Data)
	{
		uint32 Value;
		FMemory::Memcpy(&Value, Data, sizeof(Value));
		return Value;
	}

	const FJsonArray* JsonArrayField(const FJsonObject& Object, const TCHAR* Key)
	{
		const FJsonArray* Array = nullptr;
		return Object.TryGetArrayField(Key, Array) ? Array : nullptr;
	}

	const FJsonObject* JsonObjectField(const FJsonObject& Object, const TCHAR* Key)
	{
		const TSharedPtr<FJsonObject>* Found = nullptr;
		return Object.TryGetObjectField(Key, Found) && Found && Found->IsValid() ? Found->Get() : nullptr;
	}

	const FJsonObject* JsonObjectAt(const FJsonArray* Array, int32 Index)
	{
		if (!Array || !Array->IsValidIndex(Index) || !(*Array)[Index].IsValid())
		{
			return nullptr;
		}
		const TSharedPtr<FJsonObject>* Found = nullptr;
		return (*Array)[Index]->TryGetObject(Found) && Found && Found->IsValid() ? Found->Get() : nullptr;
	}

	double JsonNumber(const FJsonObject& Object, const TCHAR* Key, double Default)
	{
		double Value = Default;
		return Object.TryGetNumberField(Key, Value) ? Value : Default;
	}

	int32 JsonInt(const FJsonObject& Object, const TCHAR* Key, int32 Default)
	{
		double Value = 0.0;
		return Object.TryGetNumberField(Key, Value) ? static_cast<int32>(Value) : Default;
	}

	FString JsonString(const FJsonObject& Object, const TCHAR* Key)
	{
		FString Value;
		Object.TryGetStringField(Key, Value);
		return Value;
	}

	/** `Count` numbers from a JSON array field; false (and `Out` untouched) unless it has exactly that many. */
	bool JsonNumbers(const FJsonObject& Object, const TCHAR* Key, double* Out, int32 Count)
	{
		const FJsonArray* Array = JsonArrayField(Object, Key);
		if (!Array || Array->Num() != Count)
		{
			return false;
		}
		double Values[16];
		check(Count <= 16);
		for (int32 i = 0; i < Count; ++i)
		{
			if (!(*Array)[i].IsValid() || !(*Array)[i]->TryGetNumber(Values[i]))
			{
				return false;
			}
		}
		FMemory::Memcpy(Out, Values, sizeof(double) * Count);
		return true;
	}

	/** `a%20b` -> `a b`: glTF URIs are percent-encoded. */
	FString PercentDecode(const FString& Uri)
	{
		FTCHARToUTF8 Utf8(*Uri);
		const ANSICHAR* Source = reinterpret_cast<const ANSICHAR*>(Utf8.Get());
		const int32 Length = Utf8.Length();
		TArray<ANSICHAR> Bytes;
		for (int32 i = 0; i < Length; ++i)
		{
			const ANSICHAR C = Source[i];
			if (C == '%' && i + 2 < Length && FChar::IsHexDigit(Source[i + 1]) && FChar::IsHexDigit(Source[i + 2]))
			{
				const ANSICHAR Hex[3] = {Source[i + 1], Source[i + 2], 0};
				Bytes.Add(static_cast<ANSICHAR>(FCStringAnsi::Strtoi(Hex, nullptr, 16)));
				i += 2;
			}
			else
			{
				Bytes.Add(C);
			}
		}
		FUTF8ToTCHAR Decoded(Bytes.GetData(), Bytes.Num());
		return FString(Decoded.Length(), Decoded.Get());
	}

	/** A buffer or image URI: a data URI decoded, or a file beside the model read. */
	bool LoadUri(const FString& Uri, const FString& BaseDir, TArray<uint8>& Out, FString& OutError)
	{
		if (Uri.StartsWith(TEXT("data:")))
		{
			int32 Comma = INDEX_NONE;
			if (!Uri.FindChar(TEXT(','), Comma) || !Uri.Left(Comma).EndsWith(TEXT(";base64")))
			{
				OutError = TEXT("a data URI that is not base64");
				return false;
			}
			if (!FBase64::Decode(Uri.Mid(Comma + 1), Out))
			{
				OutError = TEXT("a data URI that does not decode");
				return false;
			}
			return true;
		}
		const FString Path = FPaths::Combine(BaseDir, PercentDecode(Uri));
		if (!FFileHelper::LoadFileToArray(Out, *Path))
		{
			OutError = FString::Printf(TEXT("cannot read %s"), *Path);
			return false;
		}
		return true;
	}

	int32 ComponentSize(int32 Type)
	{
		switch (Type)
		{
		case ComponentByte:
		case ComponentUnsignedByte: return 1;
		case ComponentShort:
		case ComponentUnsignedShort: return 2;
		case ComponentUnsignedInt:
		case ComponentFloat: return 4;
		default: return 0;
		}
	}

	int32 TypeComponents(const FString& Type)
	{
		if (Type == TEXT("SCALAR")) { return 1; }
		if (Type == TEXT("VEC2")) { return 2; }
		if (Type == TEXT("VEC3")) { return 3; }
		if (Type == TEXT("VEC4")) { return 4; }
		return 0;
	}

	double ReadComponent(const uint8* Data, int32 Type, bool bNormalized)
	{
		switch (Type)
		{
		case ComponentFloat:
		{
			float Value;
			FMemory::Memcpy(&Value, Data, sizeof(Value));
			return Value;
		}
		case ComponentUnsignedByte:
			return bNormalized ? *Data / 255.0 : static_cast<double>(*Data);
		case ComponentByte:
		{
			const int8 Value = static_cast<int8>(*Data);
			return bNormalized ? FMath::Max(Value / 127.0, -1.0) : static_cast<double>(Value);
		}
		case ComponentUnsignedShort:
		{
			uint16 Value;
			FMemory::Memcpy(&Value, Data, sizeof(Value));
			return bNormalized ? Value / 65535.0 : static_cast<double>(Value);
		}
		case ComponentShort:
		{
			int16 Value;
			FMemory::Memcpy(&Value, Data, sizeof(Value));
			return bNormalized ? FMath::Max(Value / 32767.0, -1.0) : static_cast<double>(Value);
		}
		case ComponentUnsignedInt:
		{
			uint32 Value;
			FMemory::Memcpy(&Value, Data, sizeof(Value));
			return static_cast<double>(Value);
		}
		default:
			return 0.0;
		}
	}

	/** An accessor resolved to where its elements sit in a buffer. */
	struct FAccessorView
	{
		const uint8* Data = nullptr;	// null: every element is zero (no bufferView)
		int32 Count = 0;
		int32 Stride = 0;
		int32 ComponentType = 0;
		int32 Components = 0;
		bool bNormalized = false;
	};

	/** The parsed document and its buffers, for the duration of one `Parse`. */
	struct FDocument
	{
		TSharedPtr<FJsonObject> Root;
		TArray<TArray<uint8>> Buffers;

		bool View(int32 Index, int32 WantComponents, FAccessorView& Out, FString& OutError) const
		{
			const FJsonObject* Accessor = JsonObjectAt(JsonArrayField(*Root, TEXT("accessors")), Index);
			if (!Accessor)
			{
				OutError = FString::Printf(TEXT("accessor %d does not exist"), Index);
				return false;
			}
			if (JsonObjectField(*Accessor, TEXT("sparse")))
			{
				OutError = FString::Printf(TEXT("accessor %d is sparse, which is not supported"), Index);
				return false;
			}
			Out.ComponentType = JsonInt(*Accessor, TEXT("componentType"), 0);
			Out.Components = TypeComponents(JsonString(*Accessor, TEXT("type")));
			Out.Count = JsonInt(*Accessor, TEXT("count"), -1);
			Out.bNormalized = false;
			Accessor->TryGetBoolField(TEXT("normalized"), Out.bNormalized);
			const int32 Size = ComponentSize(Out.ComponentType);
			if (Size == 0 || Out.Components != WantComponents || Out.Count < 0 || Out.Count > MaxAccessorCount)
			{
				OutError = FString::Printf(TEXT("accessor %d is not %d component(s) of a known type"), Index, WantComponents);
				return false;
			}
			const int32 ElementSize = Size * Out.Components;
			const int32 ViewIndex = JsonInt(*Accessor, TEXT("bufferView"), INDEX_NONE);
			if (ViewIndex == INDEX_NONE)
			{
				Out.Data = nullptr;
				Out.Stride = ElementSize;
				return true;
			}
			const FJsonObject* View = JsonObjectAt(JsonArrayField(*Root, TEXT("bufferViews")), ViewIndex);
			if (!View)
			{
				OutError = FString::Printf(TEXT("accessor %d names buffer view %d, which does not exist"), Index, ViewIndex);
				return false;
			}
			const int32 BufferIndex = JsonInt(*View, TEXT("buffer"), INDEX_NONE);
			if (!Buffers.IsValidIndex(BufferIndex))
			{
				OutError = FString::Printf(TEXT("buffer view %d names buffer %d, which does not exist"), ViewIndex, BufferIndex);
				return false;
			}
			const TArray<uint8>& Buffer = Buffers[BufferIndex];
			const int64 ViewOffset = static_cast<int64>(JsonNumber(*View, TEXT("byteOffset"), 0.0));
			const int64 ViewLength = static_cast<int64>(JsonNumber(*View, TEXT("byteLength"), 0.0));
			const int32 ByteStride = JsonInt(*View, TEXT("byteStride"), 0);
			Out.Stride = ByteStride > 0 ? ByteStride : ElementSize;
			const int64 AccessorOffset = static_cast<int64>(JsonNumber(*Accessor, TEXT("byteOffset"), 0.0));
			const int64 Needed = Out.Count == 0 ? 0 : AccessorOffset + static_cast<int64>(Out.Count - 1) * Out.Stride + ElementSize;
			if (ViewOffset < 0 || ViewLength < 0 || AccessorOffset < 0 || Out.Stride < ElementSize
				|| ViewOffset + ViewLength > Buffer.Num() || Needed > ViewLength)
			{
				OutError = FString::Printf(TEXT("accessor %d runs past its buffer"), Index);
				return false;
			}
			Out.Data = Buffer.GetData() + ViewOffset + AccessorOffset;
			return true;
		}

		/** `Components` numbers per element, normalised integers turned to floats. */
		bool ReadFloats(int32 Index, int32 Components, TArray<double>& Out, FString& OutError) const
		{
			FAccessorView Accessor;
			if (!View(Index, Components, Accessor, OutError))
			{
				return false;
			}
			if (Accessor.ComponentType == ComponentUnsignedInt)
			{
				OutError = FString::Printf(TEXT("accessor %d holds 32-bit integers where floats belong"), Index);
				return false;
			}
			Out.SetNumZeroed(Accessor.Count * Components);
			if (!Accessor.Data)
			{
				return true;
			}
			const int32 Size = ComponentSize(Accessor.ComponentType);
			for (int32 Element = 0; Element < Accessor.Count; ++Element)
			{
				const uint8* At = Accessor.Data + static_cast<int64>(Element) * Accessor.Stride;
				for (int32 C = 0; C < Components; ++C)
				{
					Out[Element * Components + C] = ReadComponent(At + C * Size, Accessor.ComponentType, Accessor.bNormalized);
				}
			}
			return true;
		}

		bool ReadIndices(int32 Index, TArray<uint32>& Out, FString& OutError) const
		{
			FAccessorView Accessor;
			if (!View(Index, 1, Accessor, OutError))
			{
				return false;
			}
			if (Accessor.ComponentType != ComponentUnsignedByte && Accessor.ComponentType != ComponentUnsignedShort
				&& Accessor.ComponentType != ComponentUnsignedInt)
			{
				OutError = FString::Printf(TEXT("index accessor %d is not unsigned"), Index);
				return false;
			}
			Out.SetNumZeroed(Accessor.Count);
			if (!Accessor.Data)
			{
				return true;
			}
			for (int32 i = 0; i < Accessor.Count; ++i)
			{
				Out[i] = static_cast<uint32>(ReadComponent(Accessor.Data + static_cast<int64>(i) * Accessor.Stride, Accessor.ComponentType, false));
			}
			return true;
		}
	};

	/** A node's own transform as a row-vector matrix in glTF space (metres). */
	FMatrix LocalMatrix(const FJsonObject& Node)
	{
		double M[16];
		if (JsonNumbers(Node, TEXT("matrix"), M, 16))
		{
			// glTF stores column-major for column vectors; Unreal's matrices
			// take row vectors, which is the transpose: the same 16 numbers
			// read row by row.
			FMatrix Result;
			for (int32 Row = 0; Row < 4; ++Row)
			{
				for (int32 Col = 0; Col < 4; ++Col)
				{
					Result.M[Row][Col] = M[Row * 4 + Col];
				}
			}
			return Result;
		}
		double T[3] = {0.0, 0.0, 0.0};
		double R[4] = {0.0, 0.0, 0.0, 1.0};
		double S[3] = {1.0, 1.0, 1.0};
		JsonNumbers(Node, TEXT("translation"), T, 3);
		JsonNumbers(Node, TEXT("rotation"), R, 4);
		JsonNumbers(Node, TEXT("scale"), S, 3);
		// Quaternion arithmetic does not care which hand the axes are: the
		// same numbers rotate a glTF vector as glTF means them to.
		FQuat Rotation(R[0], R[1], R[2], R[3]);
		Rotation.Normalize();
		return FTransform(Rotation, FVector(T[0], T[1], T[2]), FVector(S[0], S[1], S[2])).ToMatrixWithScale();
	}

	/** Linear RGBA from a glTF factor; `Count` is 3 or 4. */
	FLinearColor JsonColour(const FJsonObject& Object, const TCHAR* Key, int32 Count, const FLinearColor& Default)
	{
		double V[4] = {0.0, 0.0, 0.0, 1.0};
		if (!JsonNumbers(Object, Key, V, Count))
		{
			return Default;
		}
		return FLinearColor(static_cast<float>(V[0]), static_cast<float>(V[1]), static_cast<float>(V[2]),
			Count == 4 ? static_cast<float>(V[3]) : 1.0f);
	}

	/** Builds the flattened model node by node. */
	class FFlattener
	{
	public:
		FFlattener(const FDocument& InDoc, FApexGlbModel& InOut)
			: Doc(InDoc)
			, Out(InOut)
		{
		}

		bool ReadMaterials(FString& OutError)
		{
			const FJsonArray* Materials = JsonArrayField(*Doc.Root, TEXT("materials"));
			const FJsonArray* Textures = JsonArrayField(*Doc.Root, TEXT("textures"));
			const int32 Count = Materials ? Materials->Num() : 0;
			for (int32 i = 0; i < Count; ++i)
			{
				const FJsonObject* Source = JsonObjectAt(Materials, i);
				FApexGlbMaterial Material;
				if (Source)
				{
					Material.Name = JsonString(*Source, TEXT("name"));
					if (const FJsonObject* Pbr = JsonObjectField(*Source, TEXT("pbrMetallicRoughness")))
					{
						Material.BaseColor = JsonColour(*Pbr, TEXT("baseColorFactor"), 4, FLinearColor::White);
						Material.Metallic = static_cast<float>(JsonNumber(*Pbr, TEXT("metallicFactor"), 1.0));
						Material.Roughness = static_cast<float>(JsonNumber(*Pbr, TEXT("roughnessFactor"), 1.0));
						if (const FJsonObject* Texture = JsonObjectField(*Pbr, TEXT("baseColorTexture")))
						{
							// Only TEXCOORD_0 is read; a texture on another set would be misplaced.
							const FJsonObject* TextureDef = JsonObjectAt(Textures, JsonInt(*Texture, TEXT("index"), INDEX_NONE));
							const int32 Image = TextureDef ? JsonInt(*TextureDef, TEXT("source"), INDEX_NONE) : INDEX_NONE;
							if (Out.Images.IsValidIndex(Image) && JsonInt(*Texture, TEXT("texCoord"), 0) == 0)
							{
								Material.BaseColorImage = Image;
								Out.Images[Image].bUsed = true;
							}
						}
					}
					Material.Emissive = JsonColour(*Source, TEXT("emissiveFactor"), 3, FLinearColor::Black);
					const FString AlphaMode = JsonString(*Source, TEXT("alphaMode"));
					Material.Alpha = AlphaMode == TEXT("MASK") ? EApexGlbAlpha::Mask
						: AlphaMode == TEXT("BLEND")			 ? EApexGlbAlpha::Blend
																 : EApexGlbAlpha::Opaque;
					Material.AlphaCutoff = static_cast<float>(JsonNumber(*Source, TEXT("alphaCutoff"), 0.5));
					Source->TryGetBoolField(TEXT("doubleSided"), Material.bDoubleSided);
					if (const FJsonObject* Extensions = JsonObjectField(*Source, TEXT("extensions")))
					{
						if (const FJsonObject* ClearCoat = JsonObjectField(*Extensions, TEXT("KHR_materials_clearcoat")))
						{
							Material.ClearCoat = static_cast<float>(JsonNumber(*ClearCoat, TEXT("clearcoatFactor"), 0.0));
							Material.ClearCoatRoughness = static_cast<float>(JsonNumber(*ClearCoat, TEXT("clearcoatRoughnessFactor"), 0.0));
						}
						if (const FJsonObject* Strength = JsonObjectField(*Extensions, TEXT("KHR_materials_emissive_strength")))
						{
							Material.EmissiveStrength = static_cast<float>(JsonNumber(*Strength, TEXT("emissiveStrength"), 1.0));
						}
					}
				}
				Material.Name = UniqueName(Material.Name.IsEmpty() ? FString::Printf(TEXT("material_%d"), i) : Material.Name);
				Out.Materials.Add(MoveTemp(Material));
			}
			return true;
		}

		bool Flatten(FString& OutError)
		{
			const FJsonArray* Nodes = JsonArrayField(*Doc.Root, TEXT("nodes"));
			TArray<int32> Roots;
			const FJsonArray* Scenes = JsonArrayField(*Doc.Root, TEXT("scenes"));
			const FJsonObject* Scene = JsonObjectAt(Scenes, JsonInt(*Doc.Root, TEXT("scene"), 0));
			if (Scene)
			{
				if (const FJsonArray* SceneNodes = JsonArrayField(*Scene, TEXT("nodes")))
				{
					for (const TSharedPtr<FJsonValue>& Value : *SceneNodes)
					{
						double Index = 0.0;
						if (Value.IsValid() && Value->TryGetNumber(Index))
						{
							Roots.Add(static_cast<int32>(Index));
						}
					}
				}
			}
			else if (Nodes)
			{
				// No scene: every node nobody names as a child is a root.
				TSet<int32> Children;
				for (int32 i = 0; i < Nodes->Num(); ++i)
				{
					if (const FJsonObject* Node = JsonObjectAt(Nodes, i))
					{
						if (const FJsonArray* Kids = JsonArrayField(*Node, TEXT("children")))
						{
							for (const TSharedPtr<FJsonValue>& Kid : *Kids)
							{
								double Index = 0.0;
								if (Kid.IsValid() && Kid->TryGetNumber(Index))
								{
									Children.Add(static_cast<int32>(Index));
								}
							}
						}
					}
				}
				for (int32 i = 0; i < Nodes->Num(); ++i)
				{
					if (!Children.Contains(i))
					{
						Roots.Add(i);
					}
				}
			}

			struct FPending
			{
				int32 Node;
				FMatrix Parent;
				int32 Depth;
			};
			TArray<FPending> Stack;
			for (int32 i = Roots.Num() - 1; i >= 0; --i)
			{
				Stack.Add({Roots[i], FMatrix::Identity, 0});
			}
			int32 Visits = 0;
			while (Stack.Num() > 0)
			{
				const FPending Pending = Stack.Pop(EAllowShrinking::No);
				const FJsonObject* Node = JsonObjectAt(Nodes, Pending.Node);
				if (!Node)
				{
					OutError = FString::Printf(TEXT("node %d does not exist"), Pending.Node);
					return false;
				}
				if (Pending.Depth > MaxNodeDepth || ++Visits > MaxNodeVisits)
				{
					OutError = TEXT("the node tree is too deep or loops back on itself");
					return false;
				}
				// Row vectors: this node's transform first, then its parent's.
				const FMatrix World = LocalMatrix(*Node) * Pending.Parent;
				const int32 Mesh = JsonInt(*Node, TEXT("mesh"), INDEX_NONE);
				if (Mesh != INDEX_NONE && !AddMesh(Mesh, World, OutError))
				{
					return false;
				}
				if (const FJsonArray* Kids = JsonArrayField(*Node, TEXT("children")))
				{
					for (int32 k = Kids->Num() - 1; k >= 0; --k)
					{
						double Index = 0.0;
						if ((*Kids)[k].IsValid() && (*Kids)[k]->TryGetNumber(Index))
						{
							Stack.Add({static_cast<int32>(Index), World, Pending.Depth + 1});
						}
					}
				}
			}
			return true;
		}

		int32 SkippedPrimitives = 0;

	private:
		FString UniqueName(const FString& Wanted)
		{
			FString Name = Wanted;
			for (int32 Suffix = 1; UsedNames.Contains(Name); ++Suffix)
			{
				Name = FString::Printf(TEXT("%s_%d"), *Wanted, Suffix);
			}
			UsedNames.Add(Name);
			return Name;
		}

		/** The section a material's triangles go into; `Material` INDEX_NONE is glTF's default material. */
		FApexGlbSection& SectionFor(int32 Material)
		{
			if (!Out.Materials.IsValidIndex(Material))
			{
				if (DefaultMaterial == INDEX_NONE)
				{
					// glTF's default: white, fully metallic and rough.
					FApexGlbMaterial Default;
					Default.Name = UniqueName(TEXT("default"));
					DefaultMaterial = Out.Materials.Add(MoveTemp(Default));
				}
				Material = DefaultMaterial;
			}
			if (const int32* Existing = SectionOf.Find(Material))
			{
				return Out.Sections[*Existing];
			}
			const int32 Index = Out.Sections.AddDefaulted();
			Out.Sections[Index].Material = Material;
			SectionOf.Add(Material, Index);
			return Out.Sections[Index];
		}

		bool AddMesh(int32 MeshIndex, const FMatrix& World, FString& OutError)
		{
			const FJsonObject* Mesh = JsonObjectAt(JsonArrayField(*Doc.Root, TEXT("meshes")), MeshIndex);
			const FJsonArray* Primitives = Mesh ? JsonArrayField(*Mesh, TEXT("primitives")) : nullptr;
			if (!Primitives)
			{
				OutError = FString::Printf(TEXT("mesh %d does not exist or has no primitives"), MeshIndex);
				return false;
			}
			// Normals go through the inverse transpose (row vectors: the
			// transpose of the inverse), and a mirroring node turns every
			// triangle over, which is undone by swapping two corners.
			const FMatrix NormalMatrix = World.Inverse().GetTransposed();
			const bool bMirrored = World.Determinant() < 0.0;
			for (int32 p = 0; p < Primitives->Num(); ++p)
			{
				const FJsonObject* Primitive = JsonObjectAt(Primitives, p);
				if (!Primitive)
				{
					continue;
				}
				if (JsonInt(*Primitive, TEXT("mode"), 4) != 4)
				{
					++SkippedPrimitives;
					continue;
				}
				const FJsonObject* Attributes = JsonObjectField(*Primitive, TEXT("attributes"));
				const int32 PositionAccessor = Attributes ? JsonInt(*Attributes, TEXT("POSITION"), INDEX_NONE) : INDEX_NONE;
				if (PositionAccessor == INDEX_NONE)
				{
					++SkippedPrimitives;
					continue;
				}
				TArray<double> Positions;
				if (!Doc.ReadFloats(PositionAccessor, 3, Positions, OutError))
				{
					return false;
				}
				const int32 VertexCount = Positions.Num() / 3;
				TArray<double> Normals;
				const int32 NormalAccessor = JsonInt(*Attributes, TEXT("NORMAL"), INDEX_NONE);
				if (NormalAccessor != INDEX_NONE)
				{
					if (!Doc.ReadFloats(NormalAccessor, 3, Normals, OutError))
					{
						return false;
					}
					if (Normals.Num() != Positions.Num())
					{
						OutError = FString::Printf(TEXT("mesh %d: NORMAL and POSITION differ in count"), MeshIndex);
						return false;
					}
				}
				TArray<double> UVs;
				const int32 UVAccessor = JsonInt(*Attributes, TEXT("TEXCOORD_0"), INDEX_NONE);
				if (UVAccessor != INDEX_NONE)
				{
					if (!Doc.ReadFloats(UVAccessor, 2, UVs, OutError))
					{
						return false;
					}
					if (UVs.Num() / 2 != VertexCount)
					{
						OutError = FString::Printf(TEXT("mesh %d: TEXCOORD_0 and POSITION differ in count"), MeshIndex);
						return false;
					}
				}
				TArray<uint32> Indices;
				const int32 IndexAccessor = JsonInt(*Primitive, TEXT("indices"), INDEX_NONE);
				if (IndexAccessor != INDEX_NONE)
				{
					if (!Doc.ReadIndices(IndexAccessor, Indices, OutError))
					{
						return false;
					}
				}
				else
				{
					Indices.SetNumUninitialized(VertexCount);
					for (int32 i = 0; i < VertexCount; ++i)
					{
						Indices[i] = static_cast<uint32>(i);
					}
				}
				Indices.SetNum(Indices.Num() - Indices.Num() % 3);
				for (const uint32 Index : Indices)
				{
					if (Index >= static_cast<uint32>(VertexCount))
					{
						OutError = FString::Printf(TEXT("mesh %d: an index points past its %d vertices"), MeshIndex, VertexCount);
						return false;
					}
				}
				if (bMirrored)
				{
					for (int32 i = 0; i + 2 < Indices.Num(); i += 3)
					{
						Swap(Indices[i + 1], Indices[i + 2]);
					}
				}

				// Into the model's frame, still glTF's axes.
				TArray<FVector> WorldPositions;
				WorldPositions.SetNumUninitialized(VertexCount);
				for (int32 v = 0; v < VertexCount; ++v)
				{
					WorldPositions[v] = FVector(World.TransformPosition(FVector(Positions[v * 3], Positions[v * 3 + 1], Positions[v * 3 + 2])));
					if (WorldPositions[v].ContainsNaN())
					{
						OutError = FString::Printf(TEXT("mesh %d has a vertex that is not a number"), MeshIndex);
						return false;
					}
				}
				TArray<FVector> WorldNormals;
				WorldNormals.SetNumZeroed(VertexCount);
				if (Normals.Num() > 0)
				{
					for (int32 v = 0; v < VertexCount; ++v)
					{
						WorldNormals[v] = FVector(NormalMatrix.TransformVector(FVector(Normals[v * 3], Normals[v * 3 + 1], Normals[v * 3 + 2])));
					}
				}
				else
				{
					// None given: smooth ones from the faces, counter-clockwise
					// being the front in glTF, weighted by area.
					for (int32 i = 0; i + 2 < Indices.Num(); i += 3)
					{
						const FVector& A = WorldPositions[Indices[i]];
						const FVector Face = FVector::CrossProduct(WorldPositions[Indices[i + 1]] - A, WorldPositions[Indices[i + 2]] - A);
						for (int32 c = 0; c < 3; ++c)
						{
							WorldNormals[Indices[i + c]] += Face;
						}
					}
				}

				const uint32 Base = static_cast<uint32>(Out.Positions.Num());
				for (int32 v = 0; v < VertexCount; ++v)
				{
					const FVector3f Position = ApexGlb::ToUnrealPosition(WorldPositions[v]);
					Out.Positions.Add(Position);
					Out.Bounds += Position;
					FVector Normal = WorldNormals[v];
					if (!Normal.Normalize())
					{
						Normal = FVector(0.0, 1.0, 0.0);
					}
					Out.Normals.Add(ApexGlb::ToUnrealDirection(Normal));
					Out.UVs.Add(UVs.Num() > 0 ? FVector2f(static_cast<float>(UVs[v * 2]), static_cast<float>(UVs[v * 2 + 1]))
											  : FVector2f::ZeroVector);
				}
				FApexGlbSection& Section = SectionFor(JsonInt(*Primitive, TEXT("material"), INDEX_NONE));
				Section.Indices.Reserve(Section.Indices.Num() + Indices.Num());
				for (const uint32 Index : Indices)
				{
					Section.Indices.Add(Base + Index);
				}
			}
			return true;
		}

		const FDocument& Doc;
		FApexGlbModel& Out;
		TSet<FString> UsedNames;
		TMap<int32, int32> SectionOf;
		int32 DefaultMaterial = INDEX_NONE;
	};
}	 // namespace

bool ApexGlb::Parse(TArrayView<const uint8> Bytes, const FString& BaseDir, FApexGlbModel& Out, FString& OutError)
{
	Out = FApexGlbModel();
	FDocument Doc;

	// A binary container, or a plain .gltf JSON file.
	FString JsonText;
	TArray<uint8> BinChunk;
	bool bHasBin = false;
	if (Bytes.Num() >= 12 && GlbU32(Bytes.GetData()) == GlbMagic)
	{
		const uint32 Version = GlbU32(Bytes.GetData() + 4);
		const uint32 Length = GlbU32(Bytes.GetData() + 8);
		if (Version != 2 || Length > static_cast<uint32>(Bytes.Num()))
		{
			OutError = FString::Printf(TEXT("not a glTF 2 binary (version %u, %u of %d bytes)"), Version, Length, Bytes.Num());
			return false;
		}
		int64 Offset = 12;
		bool bHasJson = false;
		while (Offset + 8 <= Length)
		{
			const uint32 ChunkLength = GlbU32(Bytes.GetData() + Offset);
			const uint32 ChunkType = GlbU32(Bytes.GetData() + Offset + 4);
			const int64 Body = Offset + 8;
			if (Body + ChunkLength > Length)
			{
				OutError = TEXT("a chunk runs past the end of the file");
				return false;
			}
			if (ChunkType == ChunkJson && !bHasJson)
			{
				FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Bytes.GetData() + Body), ChunkLength);
				JsonText = FString(Converted.Length(), Converted.Get());
				bHasJson = true;
			}
			else if (ChunkType == ChunkBin && !bHasBin)
			{
				BinChunk.Append(Bytes.GetData() + Body, ChunkLength);
				bHasBin = true;
			}
			// Chunks are padded to four bytes.
			Offset = Body + ((static_cast<int64>(ChunkLength) + 3) & ~3ll);
		}
		if (!bHasJson)
		{
			OutError = TEXT("the file has no JSON chunk");
			return false;
		}
	}
	else
	{
		FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num());
		JsonText = FString(Converted.Length(), Converted.Get());
	}

	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
	if (!FJsonSerializer::Deserialize(Reader, Doc.Root) || !Doc.Root.IsValid())
	{
		OutError = TEXT("the JSON does not parse");
		return false;
	}
	const FJsonObject* Asset = JsonObjectField(*Doc.Root, TEXT("asset"));
	if (!Asset || !JsonString(*Asset, TEXT("version")).StartsWith(TEXT("2")))
	{
		OutError = TEXT("not glTF 2.0");
		return false;
	}
	if (const FJsonArray* Required = JsonArrayField(*Doc.Root, TEXT("extensionsRequired")))
	{
		TArray<FString> Names;
		for (const TSharedPtr<FJsonValue>& Value : *Required)
		{
			FString Name;
			if (Value.IsValid() && Value->TryGetString(Name))
			{
				Names.Add(Name);
			}
		}
		if (Names.Num() > 0)
		{
			OutError = FString::Printf(TEXT("requires %s, which the game does not read"), *FString::Join(Names, TEXT(", ")));
			return false;
		}
	}

	// Buffers: the BIN chunk is buffer 0 when that has no URI.
	if (const FJsonArray* Buffers = JsonArrayField(*Doc.Root, TEXT("buffers")))
	{
		for (int32 i = 0; i < Buffers->Num(); ++i)
		{
			const FJsonObject* Buffer = JsonObjectAt(Buffers, i);
			TArray<uint8>& Data = Doc.Buffers.AddDefaulted_GetRef();
			const FString Uri = Buffer ? JsonString(*Buffer, TEXT("uri")) : FString();
			if (Uri.IsEmpty())
			{
				if (i != 0 || !bHasBin)
				{
					OutError = FString::Printf(TEXT("buffer %d has no data"), i);
					return false;
				}
				Data = MoveTemp(BinChunk);
			}
			else if (!LoadUri(Uri, BaseDir, Data, OutError))
			{
				OutError = FString::Printf(TEXT("buffer %d: %s"), i, *OutError);
				return false;
			}
			const int64 Declared = Buffer ? static_cast<int64>(JsonNumber(*Buffer, TEXT("byteLength"), 0.0)) : 0;
			if (Declared > Data.Num())
			{
				OutError = FString::Printf(TEXT("buffer %d is %d bytes, %lld declared"), i, Data.Num(), Declared);
				return false;
			}
		}
	}

	// Images: kept encoded; DecodeImages turns the used ones into pixels.
	if (const FJsonArray* Images = JsonArrayField(*Doc.Root, TEXT("images")))
	{
		for (int32 i = 0; i < Images->Num(); ++i)
		{
			const FJsonObject* Source = JsonObjectAt(Images, i);
			FApexGlbImage& Image = Out.Images.AddDefaulted_GetRef();
			if (!Source)
			{
				continue;
			}
			Image.Name = JsonString(*Source, TEXT("name"));
			const FString Uri = JsonString(*Source, TEXT("uri"));
			const int32 ViewIndex = JsonInt(*Source, TEXT("bufferView"), INDEX_NONE);
			if (!Uri.IsEmpty())
			{
				FString Error;
				if (!LoadUri(Uri, BaseDir, Image.Encoded, Error))
				{
					// A missing picture is not a missing car: the slot draws untextured.
					Image.Encoded.Reset();
				}
			}
			else if (const FJsonObject* View = JsonObjectAt(JsonArrayField(*Doc.Root, TEXT("bufferViews")), ViewIndex))
			{
				const int32 BufferIndex = JsonInt(*View, TEXT("buffer"), INDEX_NONE);
				const int64 Offset = static_cast<int64>(JsonNumber(*View, TEXT("byteOffset"), 0.0));
				const int64 Length = static_cast<int64>(JsonNumber(*View, TEXT("byteLength"), 0.0));
				if (!Doc.Buffers.IsValidIndex(BufferIndex) || Offset < 0 || Length < 0
					|| Offset + Length > Doc.Buffers[BufferIndex].Num())
				{
					OutError = FString::Printf(TEXT("image %d runs past its buffer"), i);
					return false;
				}
				Image.Encoded.Append(Doc.Buffers[BufferIndex].GetData() + Offset, static_cast<int32>(Length));
			}
		}
	}

	FFlattener Flattener(Doc, Out);
	if (!Flattener.ReadMaterials(OutError) || !Flattener.Flatten(OutError))
	{
		return false;
	}
	if (Out.NumTriangles() == 0)
	{
		OutError = Flattener.SkippedPrimitives > 0
			? FString::Printf(TEXT("no triangles (%d primitive(s) that are not triangle lists were skipped)"), Flattener.SkippedPrimitives)
			: FString(TEXT("no triangles"));
		return false;
	}
	return true;
}

bool ApexGlb::ReadFile(const FString& Path, FApexGlbModel& Out, FString& OutError)
{
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *Path))
	{
		OutError = FString::Printf(TEXT("cannot read %s"), *Path);
		return false;
	}
	return Parse(Bytes, FPaths::GetPath(Path), Out, OutError);
}

void ApexGlb::BuildMips(int32 Width, int32 Height, TArray<TArray<uint8>>& Mips)
{
	if (Mips.Num() != 1 || !FMath::IsPowerOfTwo(Width) || !FMath::IsPowerOfTwo(Height))
	{
		return;
	}
	while (Width > 1 || Height > 1)
	{
		const int32 NextWidth = FMath::Max(1, Width / 2);
		const int32 NextHeight = FMath::Max(1, Height / 2);
		// Indexed afresh each level: adding a mip may move the array.
		TArray<uint8> Next;
		Next.SetNumUninitialized(NextWidth * NextHeight * 4);
		const TArray<uint8>& Source = Mips.Last();
		for (int32 y = 0; y < NextHeight; ++y)
		{
			const int32 Y0 = FMath::Min(y * 2, Height - 1);
			const int32 Y1 = FMath::Min(y * 2 + 1, Height - 1);
			for (int32 x = 0; x < NextWidth; ++x)
			{
				const int32 X0 = FMath::Min(x * 2, Width - 1);
				const int32 X1 = FMath::Min(x * 2 + 1, Width - 1);
				for (int32 c = 0; c < 4; ++c)
				{
					const int32 Sum = Source[(Y0 * Width + X0) * 4 + c] + Source[(Y0 * Width + X1) * 4 + c]
						+ Source[(Y1 * Width + X0) * 4 + c] + Source[(Y1 * Width + X1) * 4 + c];
					Next[(y * NextWidth + x) * 4 + c] = static_cast<uint8>((Sum + 2) / 4);
				}
			}
		}
		Mips.Add(MoveTemp(Next));
		Width = NextWidth;
		Height = NextHeight;
	}
}

bool ApexGlb::DecodeImage(FApexGlbImage& Image, IImageWrapperModule& ImageWrappers)
{
	const EImageFormat Format = Image.Encoded.Num() > 0
		? ImageWrappers.DetectImageFormat(Image.Encoded.GetData(), Image.Encoded.Num())
		: EImageFormat::Invalid;
	const TSharedPtr<IImageWrapper> Wrapper =
		Format != EImageFormat::Invalid ? ImageWrappers.CreateImageWrapper(Format) : nullptr;
	TArray64<uint8> Raw;
	if (!Wrapper.IsValid() || !Wrapper->SetCompressed(Image.Encoded.GetData(), Image.Encoded.Num())
		|| Wrapper->GetWidth() <= 0 || Wrapper->GetHeight() <= 0 || Wrapper->GetWidth() > MaxImageSide
		|| Wrapper->GetHeight() > MaxImageSide || !Wrapper->GetRaw(ERGBFormat::BGRA, 8, Raw))
	{
		return false;
	}
	const int32 Width = static_cast<int32>(Wrapper->GetWidth());
	const int32 Height = static_cast<int32>(Wrapper->GetHeight());
	if (Raw.Num() != static_cast<int64>(Width) * Height * 4)
	{
		return false;
	}
	Image.Width = Width;
	Image.Height = Height;
	Image.Mips.Reset();
	TArray<uint8>& Top = Image.Mips.AddDefaulted_GetRef();
	Top.Append(Raw.GetData(), static_cast<int32>(Raw.Num()));
	BuildMips(Width, Height, Image.Mips);
	// The pixels are what the texture is made from; the file is done with.
	Image.Encoded.Empty();
	return true;
}

bool ApexGlb::DecodeImages(FApexGlbModel& Model, IImageWrapperModule& ImageWrappers, FString& OutError)
{
	TArray<FString> Failed;
	for (int32 i = 0; i < Model.Images.Num(); ++i)
	{
		FApexGlbImage& Image = Model.Images[i];
		if (!Image.bUsed || Image.Mips.Num() > 0)
		{
			continue;
		}
		if (!DecodeImage(Image, ImageWrappers))
		{
			Failed.Add(Image.Name.IsEmpty() ? FString::Printf(TEXT("image %d"), i) : Image.Name);
		}
	}
	if (Failed.Num() > 0)
	{
		OutError = FString::Printf(TEXT("could not decode %s"), *FString::Join(Failed, TEXT(", ")));
		return false;
	}
	return true;
}
