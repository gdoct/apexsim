#include "Track/ApexTrackSceneReader.h"

#include "Track/ApexTrackSceneData.h"
#include "Dom/JsonObject.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/Compression.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	const TCHAR* kFormatTag = TEXT("apex-ue-scene");
	constexpr int32 kSupportedVersion = 2;
	const TCHAR* kSceneExtension = TEXT(".uescene.json");
	const TCHAR* kMeshBlobExtension = TEXT(".uemesh");

	/** `APEXMESH`, the blob's first eight bytes. */
	const uint8 kBlobMagic[8] = {'A', 'P', 'E', 'X', 'M', 'E', 'S', 'H'};
	constexpr uint32 kBlobVersion = 1;
	/** Flag bit: the payload is a zlib stream (RFC 1950, with its header). */
	constexpr uint32 kBlobZlib = 1u;
	/** Bytes of payload per vertex (position, normal, uv) and per index. */
	constexpr int64 kBytesPerVertex = 32;
	constexpr int64 kBytesPerIndex = 4;
	/** How much of a manifest the header read looks at. */
	constexpr int64 kHeaderBytes = 64 * 1024;

	/** Sequential little-endian reads over a byte view, failing past its end. */
	struct FBlobCursor
	{
		TConstArrayView<uint8> Data;
		int64 Offset = 0;

		bool Remaining(int64 Count) const
		{
			return Count >= 0 && Offset + Count <= Data.Num();
		}

		bool ReadU32(uint32& Out)
		{
			if (!Remaining(4))
			{
				return false;
			}
			const uint8* P = Data.GetData() + Offset;
			Out = uint32(P[0]) | (uint32(P[1]) << 8) | (uint32(P[2]) << 16) | (uint32(P[3]) << 24);
			Offset += 4;
			return true;
		}

		bool ReadString(FString& Out)
		{
			uint32 Length = 0;
			if (!ReadU32(Length) || !Remaining(Length))
			{
				return false;
			}
			const FUTF8ToTCHAR Converted(
				reinterpret_cast<const ANSICHAR*>(Data.GetData() + Offset), static_cast<int32>(Length));
			Out = FString(Converted.Length(), Converted.Get());
			Offset += Length;
			return true;
		}
	};

	/**
	 * One mesh's buffers out of an uncompressed payload. The layout is the
	 * little-endian one the exporter writes, which is also the memory layout
	 * of `FVector3f`/`FVector2f`/`uint32` on every platform this ships on.
	 */
	bool UnpackPayload(const uint8* Data, uint32 VertexCount, uint32 IndexCount, FApexTrackMesh& Out, FString& OutError)
	{
		static_assert(sizeof(FVector3f) == 12 && sizeof(FVector2f) == 8, "packed float vectors");
		static_assert(PLATFORM_LITTLE_ENDIAN, "the mesh blob is little-endian");
		// The caller has bounded both by the payload size, well inside int32.
		Out.Positions.SetNumUninitialized(static_cast<int32>(VertexCount));
		Out.Normals.SetNumUninitialized(static_cast<int32>(VertexCount));
		Out.UVs.SetNumUninitialized(static_cast<int32>(VertexCount));
		Out.Indices.SetNumUninitialized(static_cast<int32>(IndexCount));
		const int64 Vec3Bytes = int64(VertexCount) * 12;
		const int64 Vec2Bytes = int64(VertexCount) * 8;
		FMemory::Memcpy(Out.Positions.GetData(), Data, Vec3Bytes);
		FMemory::Memcpy(Out.Normals.GetData(), Data + Vec3Bytes, Vec3Bytes);
		FMemory::Memcpy(Out.UVs.GetData(), Data + 2 * Vec3Bytes, Vec2Bytes);
		FMemory::Memcpy(Out.Indices.GetData(), Data + 2 * Vec3Bytes + Vec2Bytes, int64(IndexCount) * 4);
		for (const uint32 Index : Out.Indices)
		{
			if (Index >= VertexCount)
			{
				OutError = FString::Printf(TEXT("mesh %s indexes vertex %u of %u"), *Out.Name, Index, VertexCount);
				return false;
			}
		}
		return true;
	}

	float GetNumber(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, float Default = 0.0f)
	{
		double Value = Default;
		return Object->TryGetNumberField(Field, Value) ? static_cast<float>(Value) : Default;
	}

	FString GetString(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
	{
		FString Value;
		Object->TryGetStringField(Field, Value);
		return Value;
	}

	/**
	 * Read a flat float array. The export flattens vertex buffers
	 * (`[x, y, z, x, y, z, ...]`) because nesting them triples the file size,
	 * and a 6 MB circuit is already a lot of JSON to walk.
	 */
	bool ReadFloatArray(
		const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, TArray<float>& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Object->TryGetArrayField(Field, Values))
		{
			return false;
		}
		Out.Reset(Values->Num());
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			Out.Add(static_cast<float>(Value->AsNumber()));
		}
		return true;
	}

	bool ReadLocation(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, FVector& Out)
	{
		TArray<float> Values;
		if (!ReadFloatArray(Object, Field, Values) || Values.Num() != 3)
		{
			return false;
		}
		Out = FVector(Values[0], Values[1], Values[2]);
		return true;
	}

	bool ReadMesh(const TSharedPtr<FJsonObject>& Object, FApexTrackMesh& Out, FString& OutError)
	{
		Out.Name = GetString(Object, TEXT("name"));
		Out.MaterialKey = GetString(Object, TEXT("material_key"));
		if (Out.Name.IsEmpty())
		{
			OutError = TEXT("a mesh has no name");
			return false;
		}

		TArray<float> Positions;
		TArray<float> Normals;
		TArray<float> UVs;
		if (!ReadFloatArray(Object, TEXT("positions"), Positions)
			|| !ReadFloatArray(Object, TEXT("normals"), Normals)
			|| !ReadFloatArray(Object, TEXT("uvs"), UVs))
		{
			OutError = FString::Printf(TEXT("mesh %s is missing a vertex buffer"), *Out.Name);
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* IndexValues = nullptr;
		if (!Object->TryGetArrayField(TEXT("indices"), IndexValues))
		{
			OutError = FString::Printf(TEXT("mesh %s has no indices"), *Out.Name);
			return false;
		}

		const int32 VertexCount = Positions.Num() / 3;
		if (Positions.Num() % 3 != 0 || Normals.Num() != Positions.Num()
			|| UVs.Num() != VertexCount * 2)
		{
			OutError = FString::Printf(
				TEXT("mesh %s has mismatched buffers (%d position floats, %d normal floats, %d uv "
					 "floats)"),
				*Out.Name, Positions.Num(), Normals.Num(), UVs.Num());
			return false;
		}
		if (IndexValues->Num() % 3 != 0)
		{
			OutError = FString::Printf(
				TEXT("mesh %s has %d indices, which is not a triangle list"), *Out.Name,
				IndexValues->Num());
			return false;
		}

		Out.Positions.Reset(VertexCount);
		Out.Normals.Reset(VertexCount);
		Out.UVs.Reset(VertexCount);
		for (int32 i = 0; i < VertexCount; ++i)
		{
			Out.Positions.Emplace(Positions[i * 3], Positions[i * 3 + 1], Positions[i * 3 + 2]);
			Out.Normals.Emplace(Normals[i * 3], Normals[i * 3 + 1], Normals[i * 3 + 2]);
			Out.UVs.Emplace(UVs[i * 2], UVs[i * 2 + 1]);
		}

		Out.Indices.Reset(IndexValues->Num());
		for (const TSharedPtr<FJsonValue>& Value : *IndexValues)
		{
			const int32 Index = static_cast<int32>(Value->AsNumber());
			if (Index < 0 || Index >= VertexCount)
			{
				OutError = FString::Printf(
					TEXT("mesh %s indexes vertex %d of %d"), *Out.Name, Index, VertexCount);
				return false;
			}
			Out.Indices.Add(static_cast<uint32>(Index));
		}
		return true;
	}
}	 // namespace

const TCHAR* FApexTrackSceneReader::FormatTag()
{
	return kFormatTag;
}

const TCHAR* FApexTrackSceneReader::SceneExtension()
{
	return kSceneExtension;
}

const TCHAR* FApexTrackSceneReader::MeshBlobExtension()
{
	return kMeshBlobExtension;
}

FString FApexTrackSceneReader::StemOf(const FString& ScenePath)
{
	FString Stem = FPaths::GetCleanFilename(ScenePath);
	Stem.RemoveFromEnd(kSceneExtension, ESearchCase::IgnoreCase);
	return Stem;
}

bool FApexTrackSceneReader::ParseMeshBlob(
	TConstArrayView<uint8> Blob, TArray<FApexTrackMesh>& OutMeshes, FString& OutError)
{
	FBlobCursor Cursor{Blob};
	if (!Cursor.Remaining(sizeof(kBlobMagic)) || FMemory::Memcmp(Blob.GetData(), kBlobMagic, sizeof(kBlobMagic)) != 0)
	{
		OutError = TEXT("not a mesh blob (no APEXMESH magic)");
		return false;
	}
	Cursor.Offset = sizeof(kBlobMagic);
	uint32 Version = 0;
	uint32 Count = 0;
	if (!Cursor.ReadU32(Version) || !Cursor.ReadU32(Count))
	{
		OutError = TEXT("mesh blob header is truncated");
		return false;
	}
	if (Version > kBlobVersion)
	{
		OutError = FString::Printf(TEXT("mesh blob is version %u; this build understands up to %u"), Version, kBlobVersion);
		return false;
	}

	TArray<FApexTrackMesh> Meshes;
	// Bounded: a corrupt count must not reserve gigabytes before the truncation check says so.
	Meshes.Reserve(static_cast<int32>(FMath::Min<uint32>(Count, 4096u)));
	TArray<uint8> Inflated;
	for (uint32 i = 0; i < Count; ++i)
	{
		FApexTrackMesh Mesh;
		uint32 VertexCount = 0;
		uint32 IndexCount = 0;
		uint32 Flags = 0;
		uint32 StoredSize = 0;
		if (!Cursor.ReadString(Mesh.Name) || !Cursor.ReadString(Mesh.MaterialKey) || !Cursor.ReadU32(VertexCount)
			|| !Cursor.ReadU32(IndexCount) || !Cursor.ReadU32(Flags) || !Cursor.ReadU32(StoredSize)
			|| !Cursor.Remaining(StoredSize))
		{
			OutError = FString::Printf(TEXT("mesh blob is truncated in mesh %u of %u"), i + 1, Count);
			return false;
		}
		if (IndexCount % 3 != 0)
		{
			OutError = FString::Printf(TEXT("mesh %s has %u indices, which is not a triangle list"), *Mesh.Name, IndexCount);
			return false;
		}
		const int64 RawSize = int64(VertexCount) * kBytesPerVertex + int64(IndexCount) * kBytesPerIndex;
		if (RawSize > int64(MAX_int32))
		{
			OutError = FString::Printf(TEXT("mesh %s is too large (%lld bytes)"), *Mesh.Name, RawSize);
			return false;
		}
		const uint8* Stored = Blob.GetData() + Cursor.Offset;
		const uint8* Raw = Stored;
		if (Flags & kBlobZlib)
		{
			Inflated.SetNumUninitialized(static_cast<int32>(RawSize));
			if (!FCompression::UncompressMemory(NAME_Zlib, Inflated.GetData(), static_cast<int32>(RawSize), Stored,
					static_cast<int32>(StoredSize)))
			{
				OutError = FString::Printf(TEXT("mesh %s does not inflate to its %lld bytes"), *Mesh.Name, RawSize);
				return false;
			}
			Raw = Inflated.GetData();
		}
		else if (int64(StoredSize) != RawSize)
		{
			OutError = FString::Printf(TEXT("mesh %s stores %u bytes for %lld of buffers"), *Mesh.Name, StoredSize, RawSize);
			return false;
		}
		if (!UnpackPayload(Raw, VertexCount, IndexCount, Mesh, OutError))
		{
			return false;
		}
		Cursor.Offset += StoredSize;
		Meshes.Add(MoveTemp(Mesh));
	}
	if (Cursor.Offset != Blob.Num())
	{
		OutError = FString::Printf(TEXT("mesh blob has %lld bytes after its last mesh"), Blob.Num() - Cursor.Offset);
		return false;
	}
	OutMeshes = MoveTemp(Meshes);
	return true;
}

bool FApexTrackSceneReader::LoadHeader(const FString& Path, FApexTrackSceneHeader& OutHeader, FString& OutError)
{
	// Only the head of the file: the manifest writes every scalar the catalog
	// wants ahead of its first array, so the rest (props, centerline, and on
	// a version 1 export the whole geometry) is never read.
	TUniquePtr<IFileHandle> Handle(FPlatformFileManager::Get().GetPlatformFile().OpenRead(*Path));
	if (!Handle)
	{
		OutError = FString::Printf(TEXT("could not read %s"), *Path);
		return false;
	}
	const int64 Size = FMath::Min(Handle->Size(), kHeaderBytes);
	TArray<uint8> Head;
	Head.SetNumUninitialized(static_cast<int32>(Size));
	if (Size <= 0 || !Handle->Read(Head.GetData(), Size))
	{
		OutError = FString::Printf(TEXT("could not read %s"), *Path);
		return false;
	}
	const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Head.GetData()), Head.Num());
	const FString Text(Converted.Length(), Converted.Get());

	FApexTrackSceneHeader Header;
	FString Format;
	const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Text);
	EJsonNotation Notation;
	int32 Depth = 0;
	// The depth of the `metadata` object's own fields while inside it, else
	// -1; an object nested in it (none today) is skipped, not mistaken for it.
	int32 MetadataDepth = -1;
	// A truncated tail makes ReadNext fail, which is the normal way out when
	// the first array lies past the window.
	while (Reader->ReadNext(Notation))
	{
		const FString& Id = Reader->GetIdentifier();
		if (Notation == EJsonNotation::ObjectStart)
		{
			if (Depth == 1 && Id == TEXT("metadata"))
			{
				MetadataDepth = 2;
			}
			++Depth;
			continue;
		}
		if (Notation == EJsonNotation::ObjectEnd)
		{
			--Depth;
			if (Depth < MetadataDepth)
			{
				MetadataDepth = -1;
			}
			if (Depth <= 0)
			{
				break;
			}
			continue;
		}
		if (Notation == EJsonNotation::ArrayStart)
		{
			if (Depth <= 1)
			{
				break;
			}
			continue;
		}
		if (Depth == MetadataDepth && Notation == EJsonNotation::String)
		{
			const FString Value = Reader->GetValueAsString();
			if (Id == TEXT("country"))
			{
				Header.Country = Value;
			}
			else if (Id == TEXT("city"))
			{
				Header.City = Value;
			}
			else if (Id == TEXT("category"))
			{
				Header.Category = Value;
			}
			else if (Id == TEXT("environment_type"))
			{
				Header.EnvironmentType = Value;
			}
			else if (Id == TEXT("description"))
			{
				Header.Description = Value;
			}
			continue;
		}
		if (Depth != 1)
		{
			continue;
		}
		if (Notation == EJsonNotation::String)
		{
			const FString Value = Reader->GetValueAsString();
			if (Id == TEXT("format"))
			{
				Format = Value;
			}
			else if (Id == TEXT("track_id"))
			{
				Header.TrackId = Value;
			}
			else if (Id == TEXT("track_name"))
			{
				Header.TrackName = Value;
			}
			else if (Id == TEXT("track_display_name"))
			{
				Header.DisplayName = Value;
			}
			else if (Id == TEXT("source_track"))
			{
				Header.SourceTrack = Value;
			}
			else if (Id == TEXT("mesh_blob"))
			{
				Header.MeshBlob = Value;
			}
		}
		else if (Notation == EJsonNotation::Number)
		{
			const double Value = Reader->GetValueAsNumber();
			if (Id == TEXT("version"))
			{
				Header.Version = static_cast<int32>(Value);
			}
			else if (Id == TEXT("source_crc"))
			{
				Header.SourceCrc = static_cast<int64>(Value);
			}
			else if (Id == TEXT("length_cm"))
			{
				Header.LengthCm = static_cast<float>(Value);
			}
		}
		else if (Notation == EJsonNotation::Boolean && Id == TEXT("closed_loop"))
		{
			Header.bClosedLoop = Reader->GetValueAsBoolean();
		}
	}

	if (Format != kFormatTag)
	{
		OutError = FString::Printf(TEXT("%s is not a track export (format \"%s\")"), *Path, *Format);
		return false;
	}
	if (Header.Version > kSupportedVersion)
	{
		OutError = FString::Printf(TEXT("%s is format version %d; this build understands up to %d"), *Path,
			Header.Version, kSupportedVersion);
		return false;
	}
	OutHeader = MoveTemp(Header);
	return true;
}

int32 FApexTrackSceneReader::SupportedVersion()
{
	return kSupportedVersion;
}

bool FApexTrackSceneReader::LoadFromFile(
	const FString& Path, FApexTrackScene& OutScene, FString& OutError)
{
	FString Json;
	if (!FFileHelper::LoadFileToString(Json, *Path))
	{
		OutError = FString::Printf(TEXT("could not read %s"), *Path);
		return false;
	}

	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		OutError = FString::Printf(TEXT("%s is not valid JSON"), *Path);
		return false;
	}

	const FString Format = GetString(Root, TEXT("format"));
	if (Format != kFormatTag)
	{
		OutError = FString::Printf(
			TEXT("%s has format \"%s\", expected \"%s\""), *Path, *Format, kFormatTag);
		return false;
	}
	const int32 Version = static_cast<int32>(GetNumber(Root, TEXT("version")));
	if (Version > kSupportedVersion)
	{
		OutError = FString::Printf(
			TEXT("%s is format version %d; this build understands up to %d — re-export with a "
				 "matching track editor"),
			*Path, Version, kSupportedVersion);
		return false;
	}

	FApexTrackScene Scene;
	Scene.TrackId = GetString(Root, TEXT("track_id"));
	Scene.TrackName = GetString(Root, TEXT("track_name"));
	Scene.SourceTrack = GetString(Root, TEXT("source_track"));
	{
		// Not `GetNumber`: a float cannot hold a 32-bit checksum exactly.
		double Crc = 0.0;
		if (Root->TryGetNumberField(TEXT("source_crc"), Crc))
		{
			Scene.SourceCrc = static_cast<int64>(Crc);
		}
	}
	Root->TryGetBoolField(TEXT("closed_loop"), Scene.bClosedLoop);
	Scene.LengthCm = GetNumber(Root, TEXT("length_cm"));

	const TSharedPtr<FJsonObject>* Metadata = nullptr;
	if (Root->TryGetObjectField(TEXT("metadata"), Metadata))
	{
		Scene.Country = GetString(*Metadata, TEXT("country"));
		Scene.City = GetString(*Metadata, TEXT("city"));
		Scene.Category = GetString(*Metadata, TEXT("category"));
		Scene.EnvironmentType = GetString(*Metadata, TEXT("environment_type"));
	}

	const TSharedPtr<FJsonObject>* Dressing = nullptr;
	if (Root->TryGetObjectField(TEXT("dressing"), Dressing))
	{
		const FString Season = GetString(*Dressing, TEXT("season"));
		if (!Season.IsEmpty())
		{
			Scene.Dressing.Season = Season;
		}
		(*Dressing)->TryGetBoolField(TEXT("spectators"), Scene.Dressing.bSpectators);
	}

	const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;

	if (Root->TryGetArrayField(TEXT("materials"), Array))
	{
		for (const TSharedPtr<FJsonValue>& Value : *Array)
		{
			const TSharedPtr<FJsonObject> Object = Value->AsObject();
			if (!Object.IsValid())
			{
				continue;
			}
			FApexTrackMaterial Material;
			Material.Key = GetString(Object, TEXT("key"));
			Material.Family = GetString(Object, TEXT("family"));
			TArray<float> Color;
			if (ReadFloatArray(Object, TEXT("base_color"), Color) && Color.Num() == 4)
			{
				Material.BaseColor = FLinearColor(Color[0], Color[1], Color[2], Color[3]);
			}
			Scene.Materials.Add(MoveTemp(Material));
		}
	}

	const FString MeshBlob = GetString(Root, TEXT("mesh_blob"));
	if (!MeshBlob.IsEmpty())
	{
		// Version 2: the manifest lists the meshes, the blob beside it holds
		// their buffers, in the same order.
		const FString BlobPath = FPaths::Combine(FPaths::GetPath(Path), MeshBlob);
		TArray<uint8> BlobBytes;
		if (!FFileHelper::LoadFileToArray(BlobBytes, *BlobPath))
		{
			OutError = FString::Printf(TEXT("%s names mesh blob %s, which could not be read"), *Path, *BlobPath);
			return false;
		}
		if (!ParseMeshBlob(BlobBytes, Scene.Meshes, OutError))
		{
			OutError = FString::Printf(TEXT("%s: %s"), *BlobPath, *OutError);
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Headers = nullptr;
		if (Root->TryGetArrayField(TEXT("meshes"), Headers))
		{
			if (Headers->Num() != Scene.Meshes.Num())
			{
				OutError = FString::Printf(TEXT("%s lists %d meshes but its blob holds %d"), *Path, Headers->Num(),
					Scene.Meshes.Num());
				return false;
			}
			for (int32 i = 0; i < Headers->Num(); ++i)
			{
				const TSharedPtr<FJsonObject> Object = (*Headers)[i]->AsObject();
				const FApexTrackMesh& Mesh = Scene.Meshes[i];
				if (!Object.IsValid() || GetString(Object, TEXT("name")) != Mesh.Name
					|| GetString(Object, TEXT("material_key")) != Mesh.MaterialKey)
				{
					OutError = FString::Printf(TEXT("%s and its blob disagree about mesh %d (%s); re-export the track"),
						*Path, i, *Mesh.Name);
					return false;
				}
			}
		}
	}
	else if (Root->TryGetArrayField(TEXT("meshes"), Array))
	{
		// Version 1: every buffer inline.
		Scene.Meshes.Reserve(Array->Num());
		for (const TSharedPtr<FJsonValue>& Value : *Array)
		{
			const TSharedPtr<FJsonObject> Object = Value->AsObject();
			if (!Object.IsValid())
			{
				continue;
			}
			FApexTrackMesh Mesh;
			if (!ReadMesh(Object, Mesh, OutError))
			{
				return false;
			}
			Scene.Meshes.Add(MoveTemp(Mesh));
		}
	}

	if (Root->TryGetArrayField(TEXT("props"), Array))
	{
		for (const TSharedPtr<FJsonValue>& Value : *Array)
		{
			const TSharedPtr<FJsonObject> Object = Value->AsObject();
			if (!Object.IsValid())
			{
				continue;
			}
			FApexTrackProp Prop;
			Prop.Kind = GetString(Object, TEXT("kind"));
			Prop.Asset = GetString(Object, TEXT("asset"));
			ReadLocation(Object, TEXT("location"), Prop.Location);
			Prop.YawDeg = GetNumber(Object, TEXT("yaw_deg"));
			Prop.Scale = GetNumber(Object, TEXT("scale"), 1.0f);
			Prop.Text = GetString(Object, TEXT("text"));
			// Layout hints the exporter only writes for the kinds that use them.
			double Hint = 0.0;
			if (Object->TryGetNumberField(TEXT("length_m"), Hint))
			{
				Prop.LengthM = static_cast<float>(Hint);
			}
			if (Object->TryGetNumberField(TEXT("radius_m"), Hint))
			{
				Prop.RadiusM = static_cast<float>(Hint);
			}
			if (Object->TryGetNumberField(TEXT("span_m"), Hint))
			{
				Prop.SpanM = static_cast<float>(Hint);
			}
			Scene.Props.Add(MoveTemp(Prop));
		}
	}

	if (Root->TryGetArrayField(TEXT("grid"), Array))
	{
		for (const TSharedPtr<FJsonValue>& Value : *Array)
		{
			const TSharedPtr<FJsonObject> Object = Value->AsObject();
			if (!Object.IsValid())
			{
				continue;
			}
			FApexTrackGridSlot Slot;
			Slot.Position = static_cast<int32>(GetNumber(Object, TEXT("position")));
			ReadLocation(Object, TEXT("location"), Slot.Location);
			Slot.YawDeg = GetNumber(Object, TEXT("yaw_deg"));
			Scene.Grid.Add(Slot);
		}
	}

	if (Root->TryGetArrayField(TEXT("centerline"), Array))
	{
		Scene.Centerline.Reserve(Array->Num());
		for (const TSharedPtr<FJsonValue>& Value : *Array)
		{
			const TSharedPtr<FJsonObject> Object = Value->AsObject();
			if (!Object.IsValid())
			{
				continue;
			}
			FApexTrackCenterlinePoint Point;
			Point.StationCm = GetNumber(Object, TEXT("s_cm"));
			ReadLocation(Object, TEXT("location"), Point.Location);
			Point.YawDeg = GetNumber(Object, TEXT("yaw_deg"));
			Point.HalfLeftCm = GetNumber(Object, TEXT("half_left_cm"));
			Point.HalfRightCm = GetNumber(Object, TEXT("half_right_cm"));
			Scene.Centerline.Add(Point);
		}
	}

	const TSharedPtr<FJsonObject>* PitObject = nullptr;
	if (Root->TryGetObjectField(TEXT("pit_lane"), PitObject))
	{
		FApexTrackPitLane Pit;
		Pit.WidthCm = GetNumber(*PitObject, TEXT("width_cm"));
		Pit.BoxCount = static_cast<int32>(GetNumber(*PitObject, TEXT("box_count")));
		Pit.SpeedLimitKph = GetNumber(*PitObject, TEXT("speed_limit_kmh"));
		Scene.PitLane = Pit;
	}

	// Optional: older exports have no line of their own and the builder
	// falls back to grid slot 1.
	const TSharedPtr<FJsonObject>* StartObject = nullptr;
	if (Root->TryGetObjectField(TEXT("start_finish"), StartObject))
	{
		FApexTrackStartFinish Start;
		if (ReadLocation(*StartObject, TEXT("location"), Start.Location))
		{
			Start.YawDeg = GetNumber(*StartObject, TEXT("yaw_deg"));
			Start.WidthCm = GetNumber(*StartObject, TEXT("width_m")) * 100.0f;
			Scene.StartFinish = Start;
		}
	}

	// Every mesh must name a material the table declares, or the level would
	// come out with holes of default grey and no clue why.
	for (const FApexTrackMesh& Mesh : Scene.Meshes)
	{
		if (!Scene.FindMaterial(Mesh.MaterialKey))
		{
			OutError = FString::Printf(
				TEXT("mesh %s references material \"%s\", which the export does not declare"),
				*Mesh.Name, *Mesh.MaterialKey);
			return false;
		}
	}

	OutScene = MoveTemp(Scene);
	return true;
}
