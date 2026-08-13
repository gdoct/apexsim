#include "ApexTrackSceneReader.h"

#include "ApexTrackEditorModule.h"
#include "ApexTrackSceneData.h"
#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	const TCHAR* kFormatTag = TEXT("apex-ue-scene");
	constexpr int32 kSupportedVersion = 1;

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

	if (Root->TryGetArrayField(TEXT("meshes"), Array))
	{
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
