#include "ApexTestCommon.h"
#include "Cars/ApexCarContentSubsystem.h"
#include "Cars/ApexCarToml.h"
#include "Cars/ApexGlbReader.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "HAL/FileManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Race/ApexCarLivery.h"

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
	 * empty), in a clear-coated `car_paint` (or `Material`). `Extra` is
	 * spliced into the top-level object.
	 */
	TArray<uint8> CarGlbTriangle(const FString& NodeFields, const FString& Extra = FString(), int32 Index2 = 2,
		const FString& Material = TEXT("car_paint"))
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
			"\"materials\":[{\"name\":\"%s\",\"doubleSided\":true,"
			"\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.5,0.25,0.125,1],\"metallicFactor\":0.35,\"roughnessFactor\":0.24},"
			"\"extensions\":{\"KHR_materials_clearcoat\":{\"clearcoatFactor\":1}}}],"
			"\"buffers\":[{\"byteLength\":102}],"
			"\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},{\"buffer\":0,\"byteOffset\":36,\"byteLength\":36},"
			"{\"buffer\":0,\"byteOffset\":72,\"byteLength\":24},{\"buffer\":0,\"byteOffset\":96,\"byteLength\":6}],"
			"\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
			"{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
			"{\"bufferView\":2,\"componentType\":5126,\"count\":3,\"type\":\"VEC2\"},"
			"{\"bufferView\":3,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}]}"),
			*Extra, NodeFields.IsEmpty() ? TEXT("") : *(TEXT(",") + NodeFields), *Material);
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexCarFoldersTest, "ApexSim.Cars.Folders", ApexTestFlags)

bool FApexCarFoldersTest::RunTest(const FString& Parameters)
{
	// A cars folder is read as default/ then custom/ (the shipped id wins),
	// and as itself when it has neither; the repo's own is split.
	const FString Root = FPaths::Combine(FPaths::AutomationTransientDir(), TEXT("CarFolders"));
	IFileManager::Get().DeleteDirectory(*Root, false, true);
	const FString Split = FPaths::Combine(Root, TEXT("split"));
	const FString Flat = FPaths::Combine(Root, TEXT("flat"));
	IFileManager::Get().MakeDirectory(*FPaths::Combine(Split, TEXT("custom")), true);
	IFileManager::Get().MakeDirectory(*FPaths::Combine(Split, TEXT("default")), true);
	IFileManager::Get().MakeDirectory(*FPaths::Combine(Flat, TEXT("some-car")), true);

	const TArray<FString> SplitFolders = UApexCarContentSubsystem::CarFolders(Split);
	if (TestEqual(TEXT("both halves"), SplitFolders.Num(), 2))
	{
		TestEqual(TEXT("default first"), FPaths::GetCleanFilename(SplitFolders[0]), FString(TEXT("default")));
		TestEqual(TEXT("custom second"), FPaths::GetCleanFilename(SplitFolders[1]), FString(TEXT("custom")));
	}
	const TArray<FString> FlatFolders = UApexCarContentSubsystem::CarFolders(Flat);
	TestEqual(TEXT("a flat folder is itself"), FlatFolders, TArray<FString>{Flat});
	IFileManager::Get().DeleteDirectory(*Root, false, true);

	const FString Repo = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT(".."), TEXT("content"), TEXT("cars")));
	if (IFileManager::Get().DirectoryExists(*Repo))
	{
		TestTrue(TEXT("the repo's cars are under default/"),
			IFileManager::Get().DirectoryExists(*FPaths::Combine(Repo, TEXT("default"))));
	}
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexCarTomlTextureLiveryTest, "ApexSim.Cars.TomlTextureLivery", ApexTestFlags)

bool FApexCarTomlTextureLiveryTest::RunTest(const FString& Parameters)
{
	// An imported car's skins: no paint colour, a skin, slot textures on one
	// line (with a comma inside a quoted path and a trailing comment), a preview.
	const FString Text = TEXT(
		"id = \"ac\"\n"
		"name = \"Imported\"\n"
		"[[livery]]\n"
		"name = \"Gulf\"\n"
		"skin = \"skins/gulf/Skin_00.png\"\n"
		"textures = [\"EXT_RIM=skins/gulf/EXT_RIM.png\", \"Banner = skins/gulf/a,b.jpg\"]   # rims and banner\n"
		"preview = \"skins/gulf/preview.jpg\"\n"
		"[[livery]]\n"
		"name = \"Rims only\"\n"
		"textures = [\"EXT_RIM=skins/x/EXT_RIM.png\"]\n"
		"[[livery]]\n"
		"name = \"Blue\"\n"
		"paint = [0, 0, 1]\n");
	FApexCarToml Car;
	FString Error;
	if (!TestTrue(*FString::Printf(TEXT("parses (%s)"), *Error), ApexCarToml::Parse(Text, Car, Error))
		|| !TestEqual(TEXT("three liveries"), Car.Liveries.Num(), 3))
	{
		return false;
	}
	const FApexCarLiveryToml& Gulf = Car.Liveries[0];
	TestFalse(TEXT("a skin needs no paint"), Gulf.HasPaint());
	TestEqual(TEXT("skin"), Gulf.Skin, FString(TEXT("skins/gulf/Skin_00.png")));
	TestEqual(TEXT("preview"), Gulf.Preview, FString(TEXT("skins/gulf/preview.jpg")));
	if (TestEqual(TEXT("two slot textures"), Gulf.Textures.Num(), 2))
	{
		TestEqual(TEXT("slot"), Gulf.Textures[0].Key, FString(TEXT("EXT_RIM")));
		TestEqual(TEXT("file"), Gulf.Textures[0].Value, FString(TEXT("skins/gulf/EXT_RIM.png")));
		TestEqual(TEXT("slot trimmed"), Gulf.Textures[1].Key, FString(TEXT("Banner")));
		TestEqual(TEXT("comma inside the quotes kept"), Gulf.Textures[1].Value, FString(TEXT("skins/gulf/a,b.jpg")));
	}
	TestEqual(TEXT("textures alone make a livery"), Car.Liveries[1].Textures.Num(), 1);
	TestTrue(TEXT("a colour livery still has its paint"), Car.Liveries[2].HasPaint());
	TestTrue(TEXT("and no skin"), Car.Liveries[2].Skin.IsEmpty() && Car.Liveries[2].Textures.IsEmpty());

	auto Rejects = [this](const TCHAR* What, const TCHAR* Livery)
	{
		FApexCarToml Bad;
		FString Why;
		TestFalse(What, ApexCarToml::Parse(FString(TEXT("id = \"a\"\nname = \"b\"\n[[livery]]\nname = \"c\"\n")) + Livery, Bad, Why));
	};
	Rejects(TEXT("neither paint nor skin"), TEXT("logo = \"l.png\"\n"));
	Rejects(TEXT("textures not an array"), TEXT("skin = \"s.png\"\ntextures = \"EXT_RIM=r.png\"\n"));
	Rejects(TEXT("a texture with no slot"), TEXT("textures = [\"=r.png\"]\n"));
	Rejects(TEXT("a texture with no file"), TEXT("textures = [\"EXT_RIM=\"]\n"));
	Rejects(TEXT("a texture with no ="), TEXT("textures = [\"EXT_RIM\"]\n"));
	Rejects(TEXT("an array over two lines"), TEXT("textures = [\"EXT_RIM=r.png\",\n\"B=b.png\"]\n"));
	Rejects(TEXT("unquoted entries"), TEXT("textures = [EXT_RIM=r.png]\n"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexCarTomlWheelModelsTest, "ApexSim.Cars.TomlWheelModels", ApexTestFlags)

bool FApexCarTomlWheelModelsTest::RunTest(const FString& Parameters)
{
	// Which folder a wheel comes from is decided by its name alone.
	TestFalse(TEXT("a class wheel"), ApexCarToml::IsCarLocalWheel(TEXT("f1")));
	TestTrue(TEXT("a file beside the car.toml"), ApexCarToml::IsCarLocalWheel(TEXT("rim.glb")));
	TestTrue(TEXT("any case"), ApexCarToml::IsCarLocalWheel(TEXT("RIM.GLB")));
	TestTrue(TEXT("a subfolder"), ApexCarToml::IsCarLocalWheel(TEXT("wheels/front")));
	TestTrue(TEXT("a Windows subfolder"), ApexCarToml::IsCarLocalWheel(TEXT("wheels\\front.glb")));

	const FString Axles = TEXT(
		"front_axle_m = 1.6\nrear_axle_m = -1.9\nfront_track_m = 1.6\nrear_track_m = 1.55\n"
		"front_radius_m = 0.33\nrear_radius_m = 0.36\nfront_width_m = 0.3\nrear_width_m = 0.4\n");
	FApexCarToml Car;
	FString Error;
	TestTrue(TEXT("parses"), ApexCarToml::Parse(
		TEXT("id = \"a\"\nname = \"b\"\n[wheels]\nmodel = \"wheels/front.glb\"\nrear_model = \"wheels/rear.glb\"\n") + Axles, Car, Error));
	TestEqual(TEXT("front"), Car.Wheels.Model, FString(TEXT("wheels/front.glb")));
	TestEqual(TEXT("rear"), Car.Wheels.RearModel, FString(TEXT("wheels/rear.glb")));

	FApexCarToml Shared;
	TestTrue(TEXT("a class wheel parses"), ApexCarToml::Parse(TEXT("id = \"a\"\nname = \"b\"\n[wheels]\nmodel = \"f1\"\n") + Axles, Shared, Error));
	TestTrue(TEXT("no rear model: the front's on all four"), Shared.Wheels.RearModel.IsEmpty());
	TestFalse(TEXT("an empty spec has no rear model"), FApexWheelSpec().HasRearModel());
	FApexWheelSpec Spec;
	Spec.RearRuntimeModel = TEXT("C:/cars/x/rear.glb");
	TestTrue(TEXT("a rear path is a rear model"), Spec.HasRearModel());
	TestTrue(TEXT("and changes the spec"), Spec != FApexWheelSpec());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexCarTomlCockpitRigTest, "ApexSim.Cars.TomlCockpitRig", ApexTestFlags)

bool FApexCarTomlCockpitRigTest::RunTest(const FString& Parameters)
{
	FApexCarToml Car;
	FString Error;
	TestTrue(TEXT("parses"), ApexCarToml::Parse(TEXT(
		"id = \"a\"\nname = \"b\"\n[cockpit]\n"
		"steering_wheel_model = \"steer.glb\"\n"
		"rig_wheel = false\n"
		"rig_dash = FALSE   # the interior has a display\n"
		"wheel_rake_deg = -8.5\n"
		"wheel_lock_deg = 270\n"), Car, Error));
	TestEqual(TEXT("steering wheel"), Car.SteeringWheelModel, FString(TEXT("steer.glb")));
	TestFalse(TEXT("no rim"), Car.Cockpit.bRigWheel);
	TestFalse(TEXT("no display"), Car.Cockpit.bRigDash);
	TestEqual(TEXT("rake"), Car.Cockpit.WheelRakeDeg, -8.5f);
	TestEqual(TEXT("lock"), Car.Cockpit.WheelLockDeg, 270.0f);

	// A car.toml without the keys is today's cockpit.
	FApexCarToml Plain;
	TestTrue(TEXT("a table without them parses"), ApexCarToml::Parse(TEXT("id = \"a\"\nname = \"b\"\n[cockpit]\nstyle = \"open\"\n"), Plain, Error));
	TestTrue(TEXT("rim by default"), Plain.Cockpit.bRigWheel);
	TestTrue(TEXT("display by default"), Plain.Cockpit.bRigDash);
	TestTrue(TEXT("no steering wheel by default"), Plain.SteeringWheelModel.IsEmpty());
	TestEqual(TEXT("rake derived"), Plain.Cockpit.WheelRakeDeg, 0.0f);
	TestEqual(TEXT("lock derived"), Plain.Cockpit.WheelLockDeg, 0.0f);

	FApexCarToml Bad;
	TestFalse(TEXT("a switch that is not a bool"), ApexCarToml::Parse(TEXT("id = \"a\"\nname = \"b\"\n[cockpit]\nrig_wheel = no\n"), Bad, Error));
	TestFalse(TEXT("a negative lock"), ApexCarToml::Parse(TEXT("id = \"a\"\nname = \"b\"\n[cockpit]\nwheel_lock_deg = -90\n"), Bad, Error));
	TestFalse(TEXT("a rake past vertical"), ApexCarToml::Parse(TEXT("id = \"a\"\nname = \"b\"\n[cockpit]\nwheel_rake_deg = 120\n"), Bad, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexCarLiverySkinSlotsTest, "ApexSim.Cars.LiverySkinSlots", ApexTestFlags)

bool FApexCarLiverySkinSlotsTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("car_skin"), ApexLivery::IsSkinSlot(TEXT("car_skin")));
	TestTrue(TEXT("car_skin_1"), ApexLivery::IsSkinSlot(TEXT("car_skin_1")));
	TestTrue(TEXT("any case"), ApexLivery::IsSkinSlot(TEXT("Car_Skin_2")));
	TestFalse(TEXT("car_paint"), ApexLivery::IsSkinSlot(TEXT("car_paint")));
	TestFalse(TEXT("a longer word"), ApexLivery::IsSkinSlot(TEXT("car_skinny")));
	TestFalse(TEXT("an AC name"), ApexLivery::IsSkinSlot(TEXT("EXT_RIM")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexCarLiveryTexturesTest, "ApexSim.Cars.LiveryTextures", ApexTestFlags)

bool FApexCarLiveryTexturesTest::RunTest(const FString& Parameters)
{
	// End to end on a built body: a JPEG skin onto `car_skin_1` and a PNG
	// onto a named slot, livery 0 putting both back, and a skin-only livery
	// leaving `car_paint` as authored.
	UApexCarContentSubsystem* Content = UApexCarContentSubsystem::Get();
	if (!Content)
	{
		AddInfo(TEXT("no content subsystem; skipped"));
		return true;
	}
	const FString Root = FPaths::Combine(FPaths::AutomationTransientDir(), TEXT("CarLivery"));
	IFileManager::Get().DeleteDirectory(*Root, false, true);
	IFileManager::Get().MakeDirectory(*Root, true);

	// A 4x4 red image as JPEG and a green one as PNG.
	IImageWrapperModule& Wrappers = FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName(TEXT("ImageWrapper")));
	auto WriteImage = [&Wrappers](EImageFormat Format, const FColor& Colour, const FString& Path) -> bool
	{
		TArray<FColor> Pixels;
		Pixels.Init(Colour, 16);
		const TSharedPtr<IImageWrapper> Wrapper = Wrappers.CreateImageWrapper(Format);
		if (!Wrapper.IsValid() || !Wrapper->SetRaw(Pixels.GetData(), Pixels.Num() * sizeof(FColor), 4, 4, ERGBFormat::BGRA, 8))
		{
			return false;
		}
		const TArray64<uint8> Encoded = Wrapper->GetCompressed(Format == EImageFormat::JPEG ? 90 : 0);
		return Encoded.Num() > 0 && FFileHelper::SaveArrayToFile(Encoded, *Path);
	};
	const FString Jpeg = FPaths::Combine(Root, TEXT("Skin_00.jpg"));
	const FString Png = FPaths::Combine(Root, TEXT("EXT_RIM.png"));
	if (!TestTrue(TEXT("images written"), WriteImage(EImageFormat::JPEG, FColor::Red, Jpeg) && WriteImage(EImageFormat::PNG, FColor::Green, Png)))
	{
		return false;
	}
	UTexture2D* SkinTexture = Content->LoadTexture(Jpeg);
	TestNotNull(TEXT("a JPEG loads"), SkinTexture);
	TestTrue(TEXT("once: the second car in the skin gets the same texture"), Content->LoadTexture(Jpeg) == SkinTexture);

	auto Body = [&](const TCHAR* Material) -> UStaticMesh*
	{
		const FString Path = FPaths::Combine(Root, FString(Material) + TEXT(".glb"));
		FFileHelper::SaveArrayToFile(CarGlbTriangle(FString(), FString(), 2, Material), *Path);
		return Content->LoadModel(Path);
	};
	UStaticMesh* SkinBody = Body(TEXT("car_skin_1"));
	UStaticMesh* RimBody = Body(TEXT("EXT_RIM"));
	UStaticMesh* PaintBody = Body(TEXT("car_paint"));
	if (!TestNotNull(TEXT("skin body"), SkinBody) || !TestNotNull(TEXT("rim body"), RimBody) || !TestNotNull(TEXT("paint body"), PaintBody))
	{
		return false;
	}

	FApexCarLivery Livery;
	Livery.Name = TEXT("Test");
	Livery.Paint = FLinearColor(0.0f, 0.0f, 0.0f, 0.0f);
	Livery.RuntimeSkin = Jpeg;
	FApexLiveryTexture& Rim = Livery.RuntimeTextures.AddDefaulted_GetRef();
	Rim.Slot = TEXT("EXT_RIM");
	Rim.RuntimeTexture = Png;

	// The car parents carry BaseColorTexture; without them (a fresh clone,
	// flat colours) only the instance and its bookkeeping can be checked.
	auto TextureOn = [](UStaticMeshComponent& Component) -> UTexture*
	{
		UTexture* Texture = nullptr;
		UMaterialInterface* Material = Component.GetMaterial(0);
		return Material && Material->GetTextureParameterValue(FHashedMaterialParameterInfo(TEXT("BaseColorTexture")), Texture) ? Texture : nullptr;
	};

	for (UStaticMesh* Mesh : {SkinBody, RimBody})
	{
		UStaticMeshComponent* Component = NewObject<UStaticMeshComponent>(GetTransientPackage());
		Component->SetStaticMesh(Mesh);
		UMaterialInterface* Authored = Component->GetMaterial(0);
		UTexture* Before = TextureOn(*Component);

		ApexLivery::Apply(Component, &Livery);
		UMaterialInstanceDynamic* Mid = Cast<UMaterialInstanceDynamic>(Component->GetMaterial(0));
		TestTrue(*FString::Printf(TEXT("%s: the component's own instance"), *Mesh->GetName()), Mid && Mid->GetOuter() == Component && Mid != Authored);
		TestEqual(*FString::Printf(TEXT("%s: remembered"), *Mesh->GetName()), Component->ComponentTags.Num(), 1);
		UTexture* After = TextureOn(*Component);
		if (After || Before)
		{
			TestTrue(*FString::Printf(TEXT("%s: retextured"), *Mesh->GetName()),
				After == (Mesh == SkinBody ? static_cast<UTexture*>(SkinTexture) : static_cast<UTexture*>(Content->LoadTexture(Png))));
		}

		// Again, as the garage stepping onto the same livery does: one tag, not two.
		ApexLivery::Apply(Component, &Livery);
		TestEqual(*FString::Printf(TEXT("%s: remembered once"), *Mesh->GetName()), Component->ComponentTags.Num(), 1);

		ApexLivery::Apply(Component, nullptr);
		TestTrue(*FString::Printf(TEXT("%s: livery 0 is the model as authored"), *Mesh->GetName()), Component->GetMaterial(0) == Authored);
		TestEqual(*FString::Printf(TEXT("%s: forgotten"), *Mesh->GetName()), Component->ComponentTags.Num(), 0);
	}

	// No paint in the livery: car_paint keeps the model's material.
	UStaticMeshComponent* Painted = NewObject<UStaticMeshComponent>(GetTransientPackage());
	Painted->SetStaticMesh(PaintBody);
	UMaterialInterface* AuthoredPaint = Painted->GetMaterial(0);
	ApexLivery::Apply(Painted, &Livery);
	TestTrue(TEXT("a skin-only livery leaves car_paint alone"), Painted->GetMaterial(0) == AuthoredPaint);
	FApexCarLivery Blue;
	Blue.Paint = FLinearColor(0.0f, 0.0f, 1.0f, 1.0f);
	ApexLivery::Apply(Painted, &Blue);
	TestTrue(TEXT("a colour livery still repaints it"), Painted->GetMaterial(0) != AuthoredPaint);

	IFileManager::Get().DeleteDirectory(*Root, false, true);
	return true;
}

#endif	  // WITH_DEV_AUTOMATION_TESTS
