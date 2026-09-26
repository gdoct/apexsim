#include "ApexTestCommon.h"
#include "Cars/ApexCarToml.h"
#include "Cars/ApexGlbReader.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** Little-endian bytes of `Values` appended to `Out`. */
	template <typename T>
	void CarGlbAppend(TArray<uint8>& Out, std::initializer_list<T> Values)
	{
		for (const T Value : Values)
		{
			const int32 At = Out.AddUninitialized(sizeof(T));
			FMemory::Memcpy(Out.GetData() + At, &Value, sizeof(T));
		}
	}

	/** A GLB container around `Json` and `Bin`, chunks padded to four bytes as the spec wants. */
	TArray<uint8> CarGlbContainer(const FString& Json, TArray<uint8> Bin)
	{
		FTCHARToUTF8 Utf8(*Json);
		TArray<uint8> JsonBytes;
		JsonBytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
		while (JsonBytes.Num() % 4 != 0)
		{
			JsonBytes.Add(' ');
		}
		while (Bin.Num() % 4 != 0)
		{
			Bin.Add(0);
		}
		TArray<uint8> Out;
		const uint32 Total = 12 + 8 + JsonBytes.Num() + (Bin.Num() > 0 ? 8 + Bin.Num() : 0);
		CarGlbAppend<uint32>(Out, {0x46546C67u, 2u, Total, static_cast<uint32>(JsonBytes.Num()), 0x4E4F534Au});
		Out.Append(JsonBytes);
		if (Bin.Num() > 0)
		{
			CarGlbAppend<uint32>(Out, {static_cast<uint32>(Bin.Num()), 0x004E4942u});
			Out.Append(Bin);
		}
		return Out;
	}

	/**
	 * One triangle, (0,0,0) (1,0,0) (0,1,0) facing +Z, with UVs and u16
	 * indices 0 1 2, under a node whose TRS is `NodeFields` (JSON, may be
	 * empty), in a clear-coated `car_paint`. `Extra` is spliced into the
	 * top-level object.
	 */
	TArray<uint8> CarGlbTriangle(const FString& NodeFields, const FString& Extra = FString(), int32 Index2 = 2)
	{
		TArray<uint8> Bin;
		CarGlbAppend<float>(Bin, {0, 0, 0, 1, 0, 0, 0, 1, 0});	   // positions, 36 bytes
		CarGlbAppend<float>(Bin, {0, 0, 1, 0, 0, 1, 0, 0, 1});	   // normals, 36
		CarGlbAppend<float>(Bin, {0, 0, 1, 0, 0, 1});			   // uvs, 24
		CarGlbAppend<uint16>(Bin, {0, 1, static_cast<uint16>(Index2)});	 // indices, 6
		const FString Json = FString::Printf(TEXT(
			"{\"asset\":{\"version\":\"2.0\"},%s"
			"\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
			"\"nodes\":[{\"mesh\":0%s}],"
			"\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2},\"indices\":3,\"material\":0}]}],"
			"\"materials\":[{\"name\":\"car_paint\",\"doubleSided\":true,"
			"\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.5,0.25,0.125,1],\"metallicFactor\":0.35,\"roughnessFactor\":0.24},"
			"\"extensions\":{\"KHR_materials_clearcoat\":{\"clearcoatFactor\":1}}}],"
			"\"buffers\":[{\"byteLength\":102}],"
			"\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},{\"buffer\":0,\"byteOffset\":36,\"byteLength\":36},"
			"{\"buffer\":0,\"byteOffset\":72,\"byteLength\":24},{\"buffer\":0,\"byteOffset\":96,\"byteLength\":6}],"
			"\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
			"{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
			"{\"bufferView\":2,\"componentType\":5126,\"count\":3,\"type\":\"VEC2\"},"
			"{\"bufferView\":3,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}]}"),
			*Extra, NodeFields.IsEmpty() ? TEXT("") : *(TEXT(",") + NodeFields));
		return CarGlbContainer(Json, Bin);
	}

	bool CarVectorNear(const FVector3f& A, const FVector3f& B)
	{
		return A.Equals(B, 1.0e-3f);
	}
}	 // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexCarGlbTriangleTest, "ApexSim.Cars.Glb.Triangle", ApexTestFlags)

bool FApexCarGlbTriangleTest::RunTest(const FString& Parameters)
{
	FApexGlbModel Model;
	FString Error;
	TestTrue(TEXT("parses"), ApexGlb::Parse(CarGlbTriangle(TEXT("\"translation\":[1,2,3]")), FString(), Model, Error));
	if (!TestEqual(TEXT("three vertices"), Model.Positions.Num(), 3) || !TestEqual(TEXT("one section"), Model.Sections.Num(), 1))
	{
		AddError(Error);
		return false;
	}
	// glTF (x, y, z) metres lands as Unreal (x, z, y) centimetres, which is where Interchange put it.
	TestTrue(TEXT("origin, translated"), CarVectorNear(Model.Positions[0], FVector3f(100.0f, 300.0f, 200.0f)));
	TestTrue(TEXT("+X stays +X"), CarVectorNear(Model.Positions[1], FVector3f(200.0f, 300.0f, 200.0f)));
	TestTrue(TEXT("glTF up is Unreal up"), CarVectorNear(Model.Positions[2], FVector3f(100.0f, 300.0f, 300.0f)));
	TestTrue(TEXT("glTF +Z is Unreal +Y"), CarVectorNear(Model.Normals[0], FVector3f(0.0f, 1.0f, 0.0f)));
	TestTrue(TEXT("uv as stored"), Model.UVs[1].Equals(FVector2f(1.0f, 0.0f)));
	// The axis swap alone turns glTF's counter-clockwise front into Unreal's
	// clockwise one; the order is kept.
	TestTrue(TEXT("indices in order"), Model.Sections[0].Indices == TArray<uint32>({0, 1, 2}));
	TestTrue(TEXT("bounds"), Model.Bounds.IsValid && CarVectorNear(Model.Bounds.Max, FVector3f(200.0f, 300.0f, 300.0f)));

	const FApexGlbMaterial& Paint = Model.Materials[Model.Sections[0].Material];
	TestEqual(TEXT("slot name"), Paint.Name, FString(TEXT("car_paint")));
	TestTrue(TEXT("base colour"), Paint.BaseColor.Equals(FLinearColor(0.5f, 0.25f, 0.125f, 1.0f)));
	TestEqual(TEXT("metallic"), Paint.Metallic, 0.35f);
	TestEqual(TEXT("roughness"), Paint.Roughness, 0.24f);
	TestEqual(TEXT("clear coat"), Paint.ClearCoat, 1.0f);
	TestTrue(TEXT("opaque"), Paint.Alpha == EApexGlbAlpha::Opaque);
	TestEqual(TEXT("no texture"), Paint.BaseColorImage, static_cast<int32>(INDEX_NONE));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexCarGlbNodesTest, "ApexSim.Cars.Glb.Nodes", ApexTestFlags)

bool FApexCarGlbNodesTest::RunTest(const FString& Parameters)
{
	FApexGlbModel Model;
	FString Error;

	// A quarter turn about glTF +Y: +X goes to -Z, which is Unreal -Y.
	const float S = FMath::Sqrt(0.5f);
	TestTrue(TEXT("rotated parses"), ApexGlb::Parse(CarGlbTriangle(FString::Printf(TEXT("\"rotation\":[0,%f,0,%f]"), S, S)),
		FString(), Model, Error));
	TestTrue(TEXT("rotation"), Model.Positions.Num() == 3 && CarVectorNear(Model.Positions[1], FVector3f(0.0f, -100.0f, 0.0f)));
	TestTrue(TEXT("normal rotated with it"), Model.Normals.Num() == 3 && CarVectorNear(Model.Normals[0], FVector3f(1.0f, 0.0f, 0.0f)));

	// A column-major matrix that scales by 2 and moves 5 along +X.
	TestTrue(TEXT("matrix parses"), ApexGlb::Parse(CarGlbTriangle(TEXT("\"matrix\":[2,0,0,0, 0,2,0,0, 0,0,2,0, 5,0,0,1]")),
		FString(), Model, Error));
	TestTrue(TEXT("matrix"), Model.Positions.Num() == 3 && CarVectorNear(Model.Positions[1], FVector3f(700.0f, 0.0f, 0.0f)));
	TestTrue(TEXT("normal stays unit under scale"), Model.Normals.Num() == 3 && FMath::IsNearlyEqual(Model.Normals[0].Size(), 1.0f, 1.0e-4f));

	// A mirroring node turns every triangle over: two corners swap back.
	TestTrue(TEXT("mirrored parses"), ApexGlb::Parse(CarGlbTriangle(TEXT("\"scale\":[-1,1,1]")), FString(), Model, Error));
	TestTrue(TEXT("mirrored rewound"), Model.Sections.Num() == 1 && Model.Sections[0].Indices == TArray<uint32>({0, 2, 1}));
	TestTrue(TEXT("mirrored normal keeps facing +Z"), Model.Normals.Num() == 3 && CarVectorNear(Model.Normals[0], FVector3f(0.0f, 1.0f, 0.0f)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexCarGlbRejectsTest, "ApexSim.Cars.Glb.Rejects", ApexTestFlags)

bool FApexCarGlbRejectsTest::RunTest(const FString& Parameters)
{
	FApexGlbModel Model;
	FString Error;
	const TArray<uint8> NotGltf = {'n', 'o', 't', ' ', 'g', 'l', 't', 'f'};
	TestFalse(TEXT("not glTF"), ApexGlb::Parse(NotGltf, FString(), Model, Error));
	TestFalse(TEXT("Draco required"), ApexGlb::Parse(CarGlbTriangle(FString(),
		TEXT("\"extensionsRequired\":[\"KHR_draco_mesh_compression\"],")), FString(), Model, Error));
	TestTrue(TEXT("says why"), Error.Contains(TEXT("KHR_draco_mesh_compression")));
	TestFalse(TEXT("index out of range"), ApexGlb::Parse(CarGlbTriangle(FString(), FString(), 7), FString(), Model, Error));

	TArray<uint8> Truncated = CarGlbTriangle(FString());
	Truncated.SetNum(Truncated.Num() - 40);
	TestFalse(TEXT("truncated"), ApexGlb::Parse(Truncated, FString(), Model, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexCarGlbMipsTest, "ApexSim.Cars.Glb.Mips", ApexTestFlags)

bool FApexCarGlbMipsTest::RunTest(const FString& Parameters)
{
	// 4x2 BGRA: left half black, right half white.
	TArray<TArray<uint8>> Mips;
	TArray<uint8>& Top = Mips.AddDefaulted_GetRef();
	for (int32 y = 0; y < 2; ++y)
	{
		for (int32 x = 0; x < 4; ++x)
		{
			const uint8 V = x < 2 ? 0 : 255;
			Top.Append({V, V, V, 255});
		}
	}
	ApexGlb::BuildMips(4, 2, Mips);
	TestEqual(TEXT("4x2, 2x1, 1x1"), Mips.Num(), 3);
	if (Mips.Num() == 3)
	{
		TestEqual(TEXT("2x1 size"), Mips[1].Num(), 2 * 1 * 4);
		TestEqual(TEXT("2x1 keeps the halves"), static_cast<int32>(Mips[1][0]), 0);
		TestEqual(TEXT("2x1 keeps the halves, white"), static_cast<int32>(Mips[1][4]), 255);
		TestEqual(TEXT("1x1 is the average"), static_cast<int32>(Mips[2][0]), 128);
		TestEqual(TEXT("alpha stays opaque"), static_cast<int32>(Mips[2][3]), 255);
	}
	TArray<TArray<uint8>> Odd;
	Odd.AddDefaulted_GetRef().SetNumZeroed(3 * 3 * 4);
	ApexGlb::BuildMips(3, 3, Odd);
	TestEqual(TEXT("not a power of two: one level"), Odd.Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexCarGlbRepoCarsTest, "ApexSim.Cars.Glb.RepoCars", ApexTestFlags)

bool FApexCarGlbRepoCarsTest::RunTest(const FString& Parameters)
{
	// Every car and wheel GLB in the repo reads, and the generated bodies are
	// long along Unreal Y with their paint slot, which is what the car actor,
	// the cockpit and the liveries assume. Nothing to check without the repo.
	const FString Root = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT(".."), TEXT("content")));
	TArray<FString> Files;
	IFileManager::Get().FindFilesRecursive(Files, *FPaths::Combine(Root, TEXT("cars")), TEXT("*.glb"), true, false);
	IFileManager::Get().FindFilesRecursive(Files, *FPaths::Combine(Root, TEXT("wheels")), TEXT("*.glb"), true, false, false);
	if (Files.Num() == 0)
	{
		AddInfo(FString::Printf(TEXT("no GLBs under %s; skipped"), *Root));
		return true;
	}
	for (const FString& File : Files)
	{
		if (File.Contains(TEXT(".orig.")))
		{
			continue;
		}
		FApexGlbModel Model;
		FString Error;
		const bool bRead = ApexGlb::ReadFile(File, Model, Error);
		if (!TestTrue(*FString::Printf(TEXT("%s reads (%s)"), *FPaths::GetCleanFilename(File), *Error), bRead))
		{
			continue;
		}
		TestTrue(*FString::Printf(TEXT("%s has triangles"), *FPaths::GetCleanFilename(File)), Model.NumTriangles() > 0);
		const bool bBody = !File.Contains(TEXT("wheels")) && !File.EndsWith(TEXT("_drs.glb"));
		if (bBody)
		{
			const FVector3f Size = Model.Bounds.GetSize();
			TestTrue(*FString::Printf(TEXT("%s is long along Y (%.0f x %.0f)"), *FPaths::GetCleanFilename(File), Size.X, Size.Y),
				Size.Y > Size.X);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexCarTomlClientTablesTest, "ApexSim.Cars.TomlClientTables", ApexTestFlags)

bool FApexCarTomlClientTablesTest::RunTest(const FString& Parameters)
{
	const FString Text = TEXT(
		"id = \"a1b2\"\n"
		"name = \"Car\"\n"
		"[preview]\n"
		"offset_cm = [1.0, 2.0, 3.0]\n"
		"rotation_deg = [0.0, 90.0, 0.0]   # pitch, yaw, roll\n"
		"scale = 1.5\n"
		"[cockpit]\n"
		"style = \"open\"\n"
		"eye_cm = [10, -20, 95.5]\n");
	FApexCarToml Car;
	FString Error;
	TestTrue(TEXT("parses"), ApexCarToml::Parse(Text, Car, Error));
	TestTrue(TEXT("preview present"), Car.Preview.bPresent);
	TestEqual(TEXT("offset"), Car.Preview.OffsetCm, FVector(1.0, 2.0, 3.0));
	TestEqual(TEXT("yaw"), Car.Preview.Rotation.Yaw, 90.0);
	TestEqual(TEXT("scale"), Car.Preview.Scale, 1.5f);
	TestTrue(TEXT("cockpit present"), Car.bHasCockpit);
	TestTrue(TEXT("open cockpit"), Car.Cockpit.Style == EApexCockpitStyle::OpenWheel);
	TestEqual(TEXT("eye"), Car.Cockpit.Eye, FVector(10.0, -20.0, 95.5));

	FApexCarToml Plain;
	TestTrue(TEXT("no tables parses"), ApexCarToml::Parse(TEXT("id = \"a\"\nname = \"b\"\n"), Plain, Error));
	TestFalse(TEXT("no preview"), Plain.Preview.bPresent);
	TestFalse(TEXT("no cockpit"), Plain.bHasCockpit);

	FApexCarToml Bad;
	TestFalse(TEXT("zero scale is an error"), ApexCarToml::Parse(TEXT("id = \"a\"\nname = \"b\"\n[preview]\nscale = 0\n"), Bad, Error));
	return true;
}

#endif	  // WITH_DEV_AUTOMATION_TESTS
